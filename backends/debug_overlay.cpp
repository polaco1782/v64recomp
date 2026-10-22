#include "debug_overlay.h"
#include "hle/rt64_present_queue.h"
#include "backends/imgui_impl_vulkan.h"
#include <cstdio>

namespace {
// Every context switch happens under RT64's inspector mutex. Leave its context
// current for the inspector and input filter, including when it is closed.
struct ContextScope {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext *context) {
        ImGui::SetCurrentContext(context);
    }
    ~ContextScope() {
        ImGui::SetCurrentContext(previous);
    }
};
void check_vulkan(VkResult result) {
    if (result != VK_SUCCESS)
        recomp_fail("debug overlay Vulkan failure", static_cast<uint32_t>(result));
}
}

RecompDebugOverlay::RecompDebugOverlay(RT64::Application& application) : app(application) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(nullptr);
    context = ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    auto *vk_device = static_cast<plume::VulkanDevice *>(app.device.get());
    auto *swap_chain = static_cast<plume::VulkanSwapChain *>(app.swapChain.get());
    device = vk_device->vk;
    ImGui_ImplVulkan_LoadFunctions(
        [](const char *name, void *instance) {
            return vkGetInstanceProcAddr(*static_cast<VkInstance *>(instance), name);
        },
        &vk_device->renderInterface->instance);
    render_pass = plume::VulkanGraphicsPipeline::createRenderPass(vk_device, &swap_chain->pickedSurfaceFormat.format, 1,
                                                                  VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    descriptor_pool = plume::VulkanDescriptorSet::createDescriptorPool(
        vk_device, {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}}, false);
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = vk_device->renderInterface->instance;
    info.PhysicalDevice = vk_device->physicalDevice;
    info.Device = device;
    info.QueueFamily = swap_chain->commandQueue->familyIndex;
    info.Queue = swap_chain->commandQueue->queue->vk;
    info.DescriptorPool = descriptor_pool;
    info.RenderPass = render_pass;
    info.MinImageCount = 2;
    info.ImageCount = 2;
    info.CheckVkResultFn = check_vulkan;
    if (!ImGui_ImplVulkan_Init(&info) || !ImGui_ImplVulkan_CreateFontsTexture())
        recomp_fail("cannot initialize debug overlay", 0);
}

RecompDebugOverlay::~RecompDebugOverlay() {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    ImGui_ImplVulkan_Shutdown();
    ImGui::DestroyContext(context);
    vkDestroyRenderPass(device, render_pass, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
}

void RecompDebugOverlay::update(Runtime *runtime, const RecompDebugGraphicsStats& snapshot_graphics) {
    RecompDebugStats snapshot = recomp_debug_stats(runtime);
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    stats = snapshot;
    graphics = snapshot_graphics;
}

void RecompDebugOverlay::draw(plume::RenderCommandList *list, uint64_t frames, uint64_t now) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    if (!sample_ns) {
        sample_ns = now;
        sample_frames = frames;
        sample_ops = stats.ops;
        sample_graphics = graphics;
        sample_audio = stats.audio;
    }
    if (now - sample_ns >= 500000000) {
        double seconds = (now - sample_ns) / 1e9;
        fps = (frames - sample_frames) / seconds;
        ops_per_second = (stats.ops - sample_ops) / seconds;
        uint64_t dl = graphics.tasks - sample_graphics.tasks;
        uint64_t al = stats.audio.tasks - sample_audio.tasks;
        dl_per_second = dl / seconds;
        al_per_second = al / seconds;
        graphics_ms = dl ? (graphics.submit_ns - sample_graphics.submit_ns) / (dl * 1e6) : 0;
        audio_ms = al ? (stats.audio.synth_ns - sample_audio.synth_ns) / (al * 1e6) : 0;
        sample_ns = now;
        sample_frames = frames;
        sample_ops = stats.ops;
        sample_graphics = graphics;
        sample_audio = stats.audio;
    }
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(app.swapChain->getWidth(), app.swapChain->getHeight());
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    char queue[48];
    if (!stats.audio.available)
        std::snprintf(queue, sizeof(queue), "n/a");
    else if (!stats.audio.playback)
        std::snprintf(queue, sizeof(queue), "off");
    else if (!stats.audio.device_open)
        std::snprintf(queue, sizeof(queue), "starting");
    else
        std::snprintf(queue, sizeof(queue), "%.1fms", stats.audio.queue_ms);
    char text[512];
    std::snprintf(
        text, sizeof(text),
        "FPS %5.1f  OP/s %.2fM\nOPs %llu  RSP %llu\nIRQ events %llu  VIs %llu\nThreads %u  Run %u  Wait %u\n"
        "Tris/frame %llu  Draws/frame %llu\nDL/s %.1f  AL/s %.1f  Pending %zu\nGFX/DL %.2fms  Audio/AL %.2fms\n"
        "Audio Q %s  Underruns %llu",
        fps, ops_per_second / 1e6, static_cast<unsigned long long>(stats.ops),
        static_cast<unsigned long long>(stats.rsp_tasks), static_cast<unsigned long long>(stats.interrupt_events),
        static_cast<unsigned long long>(stats.vis), stats.threads, stats.runnable, stats.waiting,
        static_cast<unsigned long long>(graphics.geometry.triangles),
        static_cast<unsigned long long>(graphics.geometry.draw_calls), dl_per_second, al_per_second,
        stats.pending_tasks, graphics_ms, audio_ms, queue, static_cast<unsigned long long>(stats.audio.underruns));
    constexpr float font_size = 12.0f;
    ImFont *font = ImGui::GetFont();
    ImVec2 size = font->CalcTextSizeA(font_size, 10000.0f, 0, text);
    ImVec2 position(io.DisplaySize.x - size.x - 8.0f, 8.0f);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddText(font, font_size, ImVec2(position.x + 1, position.y + 1), IM_COL32(0, 0, 0, 200), text);
    draw->AddText(font, font_size, position, IM_COL32(80, 255, 100, 255), text);
    ImGui::Render();
    auto *vk_list = static_cast<plume::VulkanCommandList *>(list);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), vk_list->vk);
}
