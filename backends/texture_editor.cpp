#include "texture_editor.h"
#include "recomp.h"
#include "texture_editor_paths.h"
#include "texture_decode.h"
#include "textures.h"
#include "backends/imgui_impl_vulkan.h"
#include "gui/rt64_file_dialog.h"
#include "hle/rt64_application.h"
#include "hle/rt64_present_queue.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include <algorithm>
#include <cmath>

using namespace RT64;
using namespace plume;

namespace {
RecompTextureEditor *editor = nullptr;

struct ContextScope {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext *context) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() {
        ImGui::SetCurrentContext(previous);
    }
};

void checkVulkan(VkResult result) {
    if (result != VK_SUCCESS)
        recomp_fail("texture editor Vulkan failure", uint32_t(result));
}

float edge(ImVec2 a, ImVec2 b, ImVec2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

bool pngName(const std::string& name) {
    return ReplacementDatabase::toLower(std::filesystem::u8path(name).extension().u8string()) == ".png";
}
}

RecompTextureEditor::RecompTextureEditor(Application& application, RecompTextureFiles& textureFiles)
    : app(application), files(textureFiles), frozenVI(std::make_unique<VI>()) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(nullptr);
    context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    auto *vkDevice = static_cast<VulkanDevice *>(app.device.get());
    auto *swapChain = static_cast<VulkanSwapChain *>(app.swapChain.get());
    device = vkDevice->vk;
    sampler = static_cast<const VulkanSampler *>(app.shaderLibrary->samplerLibrary.nearest.clampClamp.get())->vk;
    ImGui_ImplVulkan_LoadFunctions(
        [](const char *name, void *instance) {
            return vkGetInstanceProcAddr(*static_cast<VkInstance *>(instance), name);
        },
        &vkDevice->renderInterface->instance);
    renderPass = VulkanGraphicsPipeline::createRenderPass(vkDevice, &swapChain->pickedSurfaceFormat.format, 1,
                                                          VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4};
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = poolSize.descriptorCount;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    checkVulkan(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool));

    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = vkDevice->renderInterface->instance;
    info.PhysicalDevice = vkDevice->physicalDevice;
    info.Device = device;
    info.QueueFamily = swapChain->commandQueue->familyIndex;
    info.Queue = swapChain->commandQueue->queue->vk;
    info.DescriptorPool = descriptorPool;
    info.RenderPass = renderPass;
    info.MinImageCount = 2;
    info.ImageCount = 2;
    info.CheckVkResultFn = checkVulkan;
    if (!ImGui_ImplSDL2_InitForOther(app.appWindow->sdlWindow) || !ImGui_ImplVulkan_Init(&info) ||
        !ImGui_ImplVulkan_CreateFontsTexture())
        recomp_fail("cannot initialize texture editor", 0);

    editor = this;
    RecompTextureHooks::setGeometry(
        [](const auto& params, const auto& call, const auto& instance, const auto& viewport) {
            editor->recordGeometry(params, call, instance, viewport);
        });
    RecompTextureHooks::setPresent(
        [](auto& params, const auto& present, uint32_t address) { editor->recordPresent(params, present, address); },
        [] { return editor->open(); });
    RecompTextureHooks::setRice([](uint64_t hash, const std::string& rice) { editor->recordRice(hash, rice); });
}

RecompTextureEditor::~RecompTextureEditor() {
    RecompTextureHooks::setGeometry(nullptr);
    RecompTextureHooks::setPresent(nullptr, nullptr);
    RecompTextureHooks::setRice(nullptr);
    editor = nullptr;
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(nullptr);
    ImGui::SetCurrentContext(context);
    clearPreview(selectedPreview);
    clearPreview(packPreview);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(context);
    vkDestroyRenderPass(device, renderPass, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
}

void RecompTextureEditor::recordGeometry(const FramebufferRenderer::DrawParams& params, const GameCall& call,
                                         const InstanceDrawCall& instance, const RenderViewport& viewport) {
    const auto type = instance.type;
    if (type != InstanceDrawCall::Type::IndexedTriangles && type != InstanceDrawCall::Type::RawTriangles &&
        type != InstanceDrawCall::Type::RegularRect && type != InstanceDrawCall::Type::FillRect)
        return;
    const auto& workload = *params.curWorkload;
    const auto& pair = workload.fbPairs[params.fbPairIndex];
    if (!params.fbStorage->colorTarget)
        return;
    std::lock_guard lock(geometryMutex);
    auto& frame = geometry[workload.workloadId % geometry.size()];
    if (frame.workload != workload.workloadId) {
        frame.workload = workload.workloadId;
        frame.framebuffers.clear();
    }
    auto& output = frame.framebuffers[pair.colorImage.address];
    // An interpolated workload can build the same calls more than once.
    if (!output.empty() && call.callDesc.callIndex <= output.back().callIndex)
        output.clear();

    Shape shape{};
    shape.callIndex = call.callDesc.callIndex;
    const auto& mode = call.callDesc.otherMode;
    const bool copy = mode.cycleType() == G_CYC_COPY;
    shape.depthCompare = !copy && mode.zCmp();
    shape.depthWrite = !copy && mode.zUpd();
    const auto& draw = workload.drawData;
    const auto& flags = call.shaderDesc.flags;
    shape.clampNear = flags.NoN;
    for (uint32_t t = 0; t < call.callDesc.tileCount; t++) {
        const auto& tile = draw.callTiles[call.callDesc.tileIndex + t];
        const bool used = copy ? t == 0
                               : (flags.usesTexture0 && t == 0) ||
                                     (flags.usesTexture1 && t == (mode.cycleType() == G_CYC_1CYCLE ? 0u : 1u)) ||
                                     (mode.textLOD() == G_TL_LOD && (flags.usesTexture0 || flags.usesTexture1));
        const auto hashEnd = shape.hashes.begin() + shape.hashCount;
        if (used && tile.valid && !tile.tileCopyUsed && hashEnd != shape.hashes.end() &&
            std::find(shape.hashes.begin(), hashEnd, tile.tmemHashOrID) == hashEnd)
            shape.hashes[shape.hashCount++] = tile.tmemHashOrID;
    }
    const float halfWidth = viewport.width / 2.0f, halfHeight = viewport.height / 2.0f;
    const bool fill = type == InstanceDrawCall::Type::FillRect;
    shape.scissor = fill ? instance.clearRect.rect : instance.triangles.scissor;
    if (shape.scissor.isEmpty())
        return;
    if (fill || type == InstanceDrawCall::Type::RegularRect) {
        shape.rectangle = true;
        float x0 = shape.scissor.left, y0 = shape.scissor.top;
        float x1 = shape.scissor.right, y1 = shape.scissor.bottom;
        if (!fill) {
            const auto& tri = instance.triangles;
            const float x = halfWidth * (1 + tri.screenOffset.x), y = halfHeight * (1 - tri.screenOffset.y);
            x0 = x - halfWidth * tri.screenScale.x;
            x1 = x + halfWidth * tri.screenScale.x;
            y0 = y - halfHeight * tri.screenScale.y;
            y1 = y + halfHeight * tri.screenScale.y;
        }
        const float depth =
            (!copy && mode.zSource() == G_ZS_PRIM) ? draw.rdpParams[call.callDesc.callIndex].primDepth.x : 0;
        shape.points = {{{x0, y0, depth}, {x1, y0, depth}, {x1, y1, depth}, {x0, y1, depth}}};
        shape.count = 4;
        output.push_back(shape);
        return;
    }
    const auto& tri = instance.triangles;
    const float nativeWidth = params.targetWidth / params.resolutionScale.x;
    const float nativeHeight = params.targetHeight / params.resolutionScale.y;
    if (nativeWidth <= 0 || nativeHeight <= 0)
        return;
    struct ClipVertex {
        float x, y, depth, w;
    };
    // Use the same screen transform as RasterVS and clip before dividing by W.
    // A triangle crossing the near plane can still cover most of the window.
    for (uint32_t f = 0; f < tri.faceCount; f++) {
        std::array<ClipVertex, 3> vertices;
        bool valid = true;
        for (unsigned n = 0; n < 3; n++) {
            float x, y, depth, w;
            if (type == InstanceDrawCall::Type::IndexedTriangles) {
                const auto index = draw.faceIndices[call.meshDesc.faceIndicesStart + f * 3 + n];
                const auto& pos = draw.posScreen[index];
                x = float(pos.x);
                y = float(pos.y);
                depth = float(pos.z);
                w = float(draw.posTransformed[index].w);
                if (tri.screenScale.x == 1.0f && params.aspectRatioTarget != params.aspectRatioSource) {
                    const auto& nativeViewport = draw.rspViewports[draw.viewProjIndices[index]];
                    const float center = nativeViewport.translate.x;
                    x = center + (x - center) * (params.aspectRatioSource / params.aspectRatioTarget);
                }
            } else {
                const auto offset = (call.meshDesc.rawVertexStart + f * 3 + n) * 4;
                x = draw.triPosFloats[offset];
                y = draw.triPosFloats[offset + 1];
                depth = draw.triPosFloats[offset + 2];
                w = draw.triPosFloats[offset + 3];
            }
            if (!copy && mode.zSource() == G_ZS_PRIM)
                depth = draw.rdpParams[call.callDesc.callIndex].primDepth.x;
            vertices[n] = {
                halfWidth * (1 + (x / nativeWidth * 2 - 1) * tri.screenScale.x + tri.screenOffset.x) * w,
                halfHeight * (1 + (y / nativeHeight * 2 - 1) * tri.screenScale.y - tri.screenOffset.y) * w,
                depth * w,
                w,
            };
            valid &= std::isfinite(x) && std::isfinite(y) && std::isfinite(depth) && std::isfinite(w);
        }
        if (!valid)
            continue;

        constexpr float minW = 0.0001f;
        shape.count = 0;
        auto addPoint = [&](const ClipVertex& v) {
            shape.points[shape.count++] = {v.x / v.w, v.y / v.w, v.depth / v.w};
        };
        for (unsigned n = 0; n < 3; n++) {
            const auto& from = vertices[(n + 2) % 3];
            const auto& to = vertices[n];
            if ((from.w >= minW) != (to.w >= minW)) {
                const float weight = (minW - from.w) / (to.w - from.w);
                addPoint({from.x + (to.x - from.x) * weight, from.y + (to.y - from.y) * weight,
                          from.depth + (to.depth - from.depth) * weight, minW});
            }
            if (to.w >= minW)
                addPoint(to);
        }
        if (shape.count < 3)
            continue;
        if (!copy && flags.culling) {
            const auto& a = shape.points[0];
            const auto& b = shape.points[1];
            const auto& c = shape.points[2];
            if (edge({a.x, a.y}, {b.x, b.y}, {c.x, c.y}) > 0)
                continue;
        }
        output.push_back(shape);
    }
}

void RecompTextureEditor::recordPresent(VIRenderer::RenderParams& params, const Present& present, uint32_t address) {
    if (!present.paused) {
        hasPresentation = params.texture != nullptr;
        if (!hasPresentation)
            return;
        if (!frozen || frozenParams.textureWidth != params.textureWidth ||
            frozenParams.textureHeight != params.textureHeight || frozenParams.textureFormat != params.textureFormat) {
            frozen = app.device->createTexture(
                RenderTextureDesc::Texture2D(params.textureWidth, params.textureHeight, 1, params.textureFormat));
        }
        params.commandList->barriers(RenderBarrierStage::COPY,
                                     RenderTextureBarrier(params.texture, RenderTextureLayout::COPY_SOURCE));
        params.commandList->barriers(RenderBarrierStage::COPY,
                                     RenderTextureBarrier(frozen.get(), RenderTextureLayout::COPY_DEST));
        params.commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(frozen.get()),
                                              RenderTextureCopyLocation::Subresource(params.texture));
        params.commandList->barriers(RenderBarrierStage::GRAPHICS,
                                     RenderTextureBarrier(params.texture, RenderTextureLayout::SHADER_READ));
        frozenParams = params;
        *frozenVI = *params.vi;
        frozenParams.vi = frozenVI.get();
        frozenParams.texture = frozen.get();
        std::lock_guard lock(geometryMutex);
        frozenGeometry.clear();
        const auto& frame = geometry[present.workloadId % geometry.size()];
        if (frame.workload == present.workloadId) {
            const auto it = frame.framebuffers.find(address);
            if (it != frame.framebuffers.end())
                frozenGeometry = it->second;
        }
    }
    if (present.paused && editing && frozen) {
        auto *commandList = params.commandList;
        params = frozenParams;
        params.commandList = commandList;
    }
}

void RecompTextureEditor::recordRice(uint64_t hash, const std::string& rice) {
    if (rice.empty())
        return;
    std::lock_guard lock(riceMutex);
    riceHashes.emplace(hash, rice);
}

void RecompTextureEditor::pick() {
    if (!frozen || ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemActive())
        return;
    RenderViewport viewport;
    RenderRect crop;
    VIRenderer::getViewportAndScissor(app.swapChain.get(), *frozenVI, frozenParams.resolutionScale,
                                      frozenParams.downsamplingScale, frozenParams.removeBlackBorders, viewport, crop);
    const auto fbSize = frozenVI->fbSize();
    const auto& io = ImGui::GetIO();
    const float scaleX =
        viewport.width / (float(fbSize.x) * frozenParams.resolutionScale.x * io.DisplayFramebufferScale.x);
    const float scaleY =
        viewport.height / (float(fbSize.y) * frozenParams.resolutionScale.y * io.DisplayFramebufferScale.y);
    const ImVec2 origin(viewport.x / io.DisplayFramebufferScale.x, viewport.y / io.DisplayFramebufferScale.y);
    const ImVec2 cropMin(crop.left / io.DisplayFramebufferScale.x, crop.top / io.DisplayFramebufferScale.y);
    const ImVec2 cropMax(crop.right / io.DisplayFramebufferScale.x, crop.bottom / io.DisplayFramebufferScale.y);
    if (!(scaleX > 0 && scaleY > 0))
        return;
    const auto mouse = ImGui::GetMousePos();
    if (mouse.x < cropMin.x || mouse.x >= cropMax.x || mouse.y < cropMin.y || mouse.y >= cropMax.y)
        return;
    const ImVec2 point((mouse.x - origin.x) / scaleX, (mouse.y - origin.y) / scaleY);
    const Shape *hit = nullptr;
    float depthBuffer = 1.0f;
    for (const auto& shape : frozenGeometry) {
        if (point.x < shape.scissor.left || point.x >= shape.scissor.right || point.y < shape.scissor.top ||
            point.y >= shape.scissor.bottom)
            continue;
        const auto& a = shape.points[0];
        const auto& c = shape.points[2];
        float depth = a.depth;
        if (shape.rectangle) {
            if (point.x < a.x || point.x >= c.x || point.y < a.y || point.y >= c.y)
                continue;
        } else {
            bool covered = false;
            const ImVec2 va(a.x, a.y);
            for (unsigned i = 1; i + 1 < shape.count; i++) {
                const auto& v1 = shape.points[i];
                const auto& v2 = shape.points[i + 1];
                const ImVec2 vb(v1.x, v1.y), vc(v2.x, v2.y);
                const float area = edge(va, vb, vc);
                if (!std::isfinite(area) || area == 0)
                    continue;
                const float wb = edge(vc, va, point) / area, wc = edge(va, vb, point) / area;
                if (wb < 0 || wc < 0 || wb + wc > 1)
                    continue;
                depth += (v1.depth - a.depth) * wb + (v2.depth - a.depth) * wc;
                covered = true;
                break;
            }
            if (!covered)
                continue;
        }
        if (depth < 0 && shape.clampNear)
            depth = 0;
        if (depth < 0 || depth > 1 || (shape.depthCompare && depth > depthBuffer + 0.00001f))
            continue;
        if (shape.depthWrite)
            depthBuffer = depth;
        if (shape.hashCount || shape.depthWrite)
            hit = shape.hashCount ? &shape : nullptr;
    }
    if (!hit)
        return;

    auto *lines = ImGui::GetBackgroundDrawList();
    const auto screen = [&](float x, float y) { return ImVec2(viewport.x + x * scaleX, viewport.y + y * scaleY); };
    lines->PushClipRect(cropMin, cropMax, true);
    for (const auto& shape : frozenGeometry) {
        if (shape.callIndex != hit->callIndex)
            continue;
        std::array<ImVec2, 4> points;
        for (unsigned i = 0; i < shape.count; i++)
            points[i] = screen(shape.points[i].x, shape.points[i].y);
        lines->PushClipRect(screen(shape.scissor.left, shape.scissor.top),
                            screen(shape.scissor.right, shape.scissor.bottom), true);
        if (&shape == hit)
            lines->AddConvexPolyFilled(points.data(), shape.count, IM_COL32(255, 255, 255, 47));
        lines->AddPolyline(points.data(), shape.count, IM_COL32(110, 228, 255, 225), ImDrawFlags_Closed, 1.4f);
        lines->PopClipRect();
    }
    lines->PopClipRect();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        selection.assign(hit->hashes.begin(), hit->hashes.begin() + hit->hashCount);
        requestTexture = selection.front();
    }
}

void RecompTextureEditor::clearPreview(Preview& preview) {
    if (preview.descriptor)
        ImGui_ImplVulkan_RemoveTexture(preview.descriptor);
    preview = {};
}

void RecompTextureEditor::uploadPreview(Preview& preview, const RecompTextureZip::Image& image,
                                        const std::string& label) {
    clearPreview(preview);
    Texture texture;
    std::unique_ptr<RenderBuffer> upload;
    auto *worker = app.framebufferGraphicsWorker.get();
    {
        RenderWorkerExecution execution(worker);
        TextureCache::setRGBA32(&texture, app.device.get(), worker->commandList.get(), image.pixels.data(),
                                image.pixels.size(), image.width, image.height, image.width * 4, upload);
        worker->commandList->barriers(RenderBarrierStage::GRAPHICS,
                                      RenderTextureBarrier(texture.texture.get(), RenderTextureLayout::SHADER_READ));
    }
    preview.descriptor =
        ImGui_ImplVulkan_AddTexture(sampler, static_cast<VulkanTexture *>(texture.texture.get())->imageView,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    preview.texture = std::move(texture.texture);
    preview.width = image.width;
    preview.height = image.height;
    preview.label = label;
}

void RecompTextureEditor::showPreview(const Preview& preview) {
    if (!preview.descriptor)
        return;
    float scale = std::min(std::min(240.0f, ImGui::GetContentRegionAvail().x) / preview.width, 180.0f / preview.height);
    const ImVec2 size(preview.width * scale, preview.height * scale);
    const auto position = ImGui::GetCursorScreenPos();
    auto *draw = ImGui::GetWindowDrawList();
    constexpr float step = 8.0f;
    for (float y = 0; y < size.y; y += step)
        for (float x = 0; x < size.x; x += step) {
            const int shade = ((int(x / step) + int(y / step)) % 2) ? 84 : 52;
            draw->AddRectFilled({position.x + x, position.y + y},
                                {position.x + std::min(x + step, size.x), position.y + std::min(y + step, size.y)},
                                IM_COL32(shade, shade, shade, 255));
        }
    ImGui::Image(reinterpret_cast<ImTextureID>(preview.descriptor), size);
    ImGui::Text("%d x %d", preview.width, preview.height);
    ImGui::TextWrapped("%s", preview.label.c_str());
}

RecompTextureZip::Image RecompTextureEditor::packImage(const std::string& path) {
    auto& cache = *app.textureCache;
    if (!cache.texturePackScanners.empty()) {
        RecompTextureZip::Image image;
        if (cache.texturePackScanners.front()->loadRGBA(path, image.pixels, image.width, image.height))
            return image;
    }

    const auto& sources = cache.textureMap.replacementMap.fileSystems;
    constexpr size_t maxPngSize = 256 * 1024 * 1024;
    std::vector<uint8_t> bytes;
    if (sources.empty() || sources.front()->getSize(path) > maxPngSize || !sources.front()->load(path, bytes))
        throw std::runtime_error("Could not read texture from the ZIP.");
    return RecompTextureZip::decodePng(bytes);
}

void RecompTextureEditor::selectTexture(uint64_t hash) {
    selectedHash = hash;
    clearPreview(selectedPreview);
    status.clear();
    try {
        auto& cache = *app.textureCache;
        Texture original;
        std::string path;
        {
            const std::lock_guard lock(cache.textureMapMutex);
            const auto& map = cache.textureMap;
            if (map.replacementMapEnabled && !map.replacementMap.fileSystemResolvedPaths.empty()) {
                const auto& paths = map.replacementMap.fileSystemResolvedPaths.front();
                const auto found = paths.find(hash);
                const auto loaded = map.hashMap.find(hash);
                if (found != paths.end() && loaded != map.hashMap.end() && map.textureReplacements[loaded->second])
                    path = found->second.relativePath;
            }
            const auto found = map.hashMap.find(hash);
            if (found != map.hashMap.end()) {
                const auto *texture = map.textures[found->second];
                if (texture && texture->decodeTMEM && texture->bytesTMEM.size() == 4096) {
                    original.width = texture->width;
                    original.height = texture->height;
                    original.bytesTMEM = texture->bytesTMEM;
                    original.loadTile = texture->loadTile;
                    original.tlut = texture->tlut;
                }
            }
        }
        if (!path.empty()) {
            uploadPreview(selectedPreview, packImage(path), path);
        } else if (!original.bytesTMEM.empty()) {
            const auto& tile = original.loadTile;
            RecompTextureZip::Image image;
            image.width = int(original.width);
            image.height = int(original.height);
            image.pixels = RecompTextures::decode(original.bytesTMEM.data(), original.width, original.height, tile.fmt,
                                                  tile.siz, tile.tmem, tile.line, tile.palette, original.tlut);
            uploadPreview(selectedPreview, image, "Original texture");
        } else {
            status = "Preview unavailable for this texture.";
        }
    } catch (const std::exception& error) {
        status = error.what();
    }
}

void RecompTextureEditor::selectPackImage(const std::string& entry) {
    packSelection = entry;
    status.clear();
    clearPreview(packPreview);
    try {
        uploadPreview(packPreview, packImage(entry), entry);
    } catch (const std::exception& error) {
        status = error.what();
    }
}

void RecompTextureEditor::listEntries() {
    entries.clear();
    const auto& sources = app.textureCache->textureMap.replacementMap.fileSystems;
    if (!sources.empty()) {
        for (const auto& entry : *sources.front()) {
            if (pngName(entry))
                entries.push_back(entry);
        }
    }
    std::sort(entries.begin(), entries.end());
}

void RecompTextureEditor::save(const std::string& entry, const std::vector<uint8_t>& png) {
    app.workloadQueue->waitForIdle();
    app.presentQueue->waitForIdle();
    files.editZip(*app.textureCache, entry, png);
    listEntries();
}

void RecompTextureEditor::importTexture() {
    const auto input = FileDialog::getOpenFilename({FileFilter("PNG textures", "png")});
    if (input.empty())
        return;
    const auto png = RecompTextureZip::readPng(input);
    auto image = RecompTextureZip::decodePng(png);
    std::string stem = input.stem().u8string();
    for (char& c : stem)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            c = '_';
    stem = std::string(RecompTexturePaths::Imports) + "/" + stem.substr(0, 120);
    std::string entry = stem + ".png";
    for (unsigned copy = 2; std::binary_search(entries.begin(), entries.end(), entry); copy++)
        entry = stem + "-" + std::to_string(copy) + ".png";

    save(entry, png);
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope use(context);
    packSelection = entry;
    uploadPreview(packPreview, image, entry);
    filter.Clear();
    showBrowser = true;
    status = "Texture imported into the ZIP.";
}

void RecompTextureEditor::replaceTexture() {
    const auto hash = ReplacementDatabase::hashToString(selectedHash);
    std::string rice;
    {
        const std::lock_guard lock(riceMutex);
        const auto found = riceHashes.find(selectedHash);
        if (found != riceHashes.end())
            rice = found->second;
    }
    const auto& title = files.packRomTitle();
    const auto& sources = app.textureCache->textureMap.replacementMap.fileSystems;
    const bool hasRT64Override =
        !sources.empty() && sources.front()->exists(RecompTexturePaths::rt64Prefix() + hash + ".png");
    const bool riceUsable = !hasRT64Override && !rice.empty() && !title.empty() &&
                            title.find_first_of("/\\#") == std::string::npos &&
                            rice.find_first_not_of("0123456789abcdef#") == std::string::npos;
    const std::string entry =
        riceUsable ? std::string(RecompTexturePaths::RiceOverrides) + "/" + title + "#" + rice + "_all.png"
                   : RecompTexturePaths::rt64Prefix() + hash + ".png";
    auto image = packImage(packSelection);
    save(entry, RecompTextureZip::encodePng(image));
    files.enableReplacements(*app.textureCache);
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope use(context);
    uploadPreview(selectedPreview, image, entry);
    showBrowser = false;
    status = "Replacement saved. It applies everywhere this texture is used.";
}

void RecompTextureEditor::window() {
    const auto& io = ImGui::GetIO();
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({std::min(330.0f, io.DisplaySize.x), 0}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints({260, 100},
                                        {std::max(260.0f, io.DisplaySize.x), std::max(100.0f, io.DisplaySize.y - 24)});
    bool show = true;
    if (ImGui::Begin("Texture editor (F8)", &show, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (selection.empty()) {
            ImGui::TextUnformatted("Click a texture in the paused scene.");
        } else {
            if (selection.size() > 1 &&
                ImGui::BeginCombo("Texture", ReplacementDatabase::hashToString(selectedHash).c_str())) {
                for (size_t i = 0; i < selection.size(); i++) {
                    const auto label =
                        "Tile " + std::to_string(i + 1) + "  " + ReplacementDatabase::hashToString(selection[i]);
                    if (ImGui::Selectable(label.c_str(), selection[i] == selectedHash))
                        requestTexture = selection[i];
                }
                ImGui::EndCombo();
            }
            ImGui::Text("Hash %s", ReplacementDatabase::hashToString(selectedHash).c_str());
            showPreview(selectedPreview);
            if (ImGui::Button("Replace..."))
                showBrowser = true;
            ImGui::SameLine();
        }
        if (ImGui::Button("Import PNG..."))
            requestImport = true;
        if (!status.empty())
            ImGui::TextWrapped("%s", status.c_str());
    }
    ImGui::End();
    if (!show)
        editing = false;

    if (showBrowser) {
        ImGui::SetNextWindowPos({io.DisplaySize.x - 12, 12}, ImGuiCond_FirstUseEver, {1, 0});
        ImGui::SetNextWindowSize({std::min(560.0f, io.DisplaySize.x * 0.62f), std::min(650.0f, io.DisplaySize.y - 24)},
                                 ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Textures in the ZIP", &showBrowser)) {
            filter.Draw("Filter");
            if (ImGui::BeginChild("Textures", {0, 150}, ImGuiChildFlags_Border, ImGuiWindowFlags_HorizontalScrollbar)) {
                std::vector<const std::string *> filtered;
                for (const auto& entry : entries)
                    if (filter.PassFilter(entry.c_str()))
                        filtered.push_back(&entry);
                ImGuiListClipper clipper;
                clipper.Begin(int(filtered.size()));
                while (clipper.Step()) {
                    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                        const auto& entry = *filtered[i];
                        ImGui::PushID(i);
                        if (ImGui::Selectable(entry.c_str(), entry == packSelection))
                            requestPackImage = entry;
                        ImGui::PopID();
                    }
                }
            }
            ImGui::EndChild();
            showPreview(packPreview);
            ImGui::BeginDisabled(selection.empty() || !packPreview.descriptor);
            if (ImGui::Button("Use selected texture"))
                requestReplace = true;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Import PNG..."))
                requestImport = true;
        }
        ImGui::End();
    }
}

void RecompTextureEditor::frame() {
    auto& queue = *app.presentQueue;
    {
        const std::lock_guard lock(queue.inspectorMutex);
        ContextScope use(context);
        ImGui_ImplSDL2_NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        window();
        if (editing)
            pick();
        ImGui::Render();
    }

    if (editing && hasPresentation) {
        auto& present = queue.presents[queue.previousWriteCursor()];
        present.presentId = ++app.state->presentId;
        present.paused = true;
        queue.repeatLastPresent();
        queue.waitForPresentId(present.presentId);
        queue.waitForIdle();
    }

    // ImGui draw commands hold the preview descriptor emitted by Image().
    // Replace previews only after the present worker finishes using that frame.
    if (editing && (requestTexture || !requestPackImage.empty())) {
        const std::lock_guard lock(queue.inspectorMutex);
        ContextScope use(context);
        if (requestTexture)
            selectTexture(*requestTexture);
        if (!requestPackImage.empty())
            selectPackImage(requestPackImage);
    }
    requestTexture.reset();
    requestPackImage.clear();

    try {
        if (requestImport)
            importTexture();
        if (requestReplace && packPreview.descriptor)
            replaceTexture();
    } catch (const std::exception& error) {
        status = error.what();
    }
    requestImport = requestReplace = false;
}

void RecompTextureEditor::run() {
    auto& queue = *app.presentQueue;
    if (!hasPresentation)
        return;
    editing = true;
    quit = false;
    selection.clear();
    showBrowser = false;
    packSelection.clear();
    status.clear();
    filter.Clear();
    listEntries();
    SDL_StartTextInput();
    bool closing = false;
    while (editing) {
        {
            const std::lock_guard lock(queue.inspectorMutex);
            ContextScope use(context);
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT ||
                    (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                    quit = true;
                    closing = true;
                }
                if (event.type == SDL_KEYDOWN && !event.key.repeat &&
                    (event.key.keysym.scancode == SDL_SCANCODE_F8 || event.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
                    closing = true;
                ImGui_ImplSDL2_ProcessEvent(&event);
            }
        }
        if (closing)
            break;
        frame();
        SDL_Delay(12);
    }
    editing = false;
    queue.waitForIdle();
    if (hasPresentation)
        queue.presents[queue.previousWriteCursor()].paused = false;
    SDL_StopTextInput();
    SDL_CaptureMouse(SDL_FALSE);
    const std::lock_guard lock(queue.inspectorMutex);
    ContextScope use(context);
    ImGui::GetIO().ClearEventsQueue();
    ImGui::GetIO().ClearInputKeys();
    clearPreview(selectedPreview);
    clearPreview(packPreview);
}

void RecompTextureEditor::draw(RenderCommandList *list) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    if (!editing)
        return;
    ContextScope use(context);
    auto *commands = static_cast<VulkanCommandList *>(list);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commands->vk);
}
