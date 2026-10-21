#include "cheat_menu.h"
#include "hle/rt64_present_queue.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui/imgui_impl_sdl2_custom.h"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace {
struct ContextScope {
    ImGuiContext *previous = ImGui::GetCurrentContext();
    explicit ContextScope(ImGuiContext *context) { ImGui::SetCurrentContext(context); }
    ~ContextScope() { ImGui::SetCurrentContext(previous); }
};
void check_vulkan(VkResult result) {
    if (result != VK_SUCCESS) recomp_fail("cheat menu Vulkan failure", static_cast<uint32_t>(result));
}
}
RecompCheatMenu::RecompCheatMenu(RT64::Application &application, Runtime *rt, const char *data_path)
    : app(application), runtime(rt), key(RecompCheats::rom_key(rt)),
      cache(std::filesystem::path(data_path) / "cheats" / key) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(nullptr);
    context = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    title.assign(reinterpret_cast<const char *>(rt->rom + 32), 20);
    while (!title.empty() && (title.back() == ' ' || title.back() == '\0')) title.pop_back();
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForOther(app.appWindow->sdlWindow)) recomp_fail("cannot initialize cheat menu input", 0);
    auto *vk_device = static_cast<plume::VulkanDevice *>(app.device.get());
    auto *swap_chain = static_cast<plume::VulkanSwapChain *>(app.swapChain.get());
    device = vk_device->vk;
    ImGui_ImplVulkan_LoadFunctions([](const char *name, void *instance) {
        return vkGetInstanceProcAddr(*static_cast<VkInstance *>(instance), name);
    }, &vk_device->renderInterface->instance);
    render_pass = plume::VulkanGraphicsPipeline::createRenderPass(vk_device,
        &swap_chain->pickedSurfaceFormat.format, 1, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
    descriptor_pool = plume::VulkanDescriptorSet::createDescriptorPool(vk_device,
        {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}}, false);
    ImGui_ImplVulkan_InitInfo info{};
    info.Instance = vk_device->renderInterface->instance;
    info.PhysicalDevice = vk_device->physicalDevice; info.Device = device;
    info.QueueFamily = swap_chain->commandQueue->familyIndex; info.Queue = swap_chain->commandQueue->queue->vk;
    info.DescriptorPool = descriptor_pool; info.RenderPass = render_pass;
    info.MinImageCount = 2; info.ImageCount = 2; info.CheckVkResultFn = check_vulkan;
    if (!ImGui_ImplVulkan_Init(&info) || !ImGui_ImplVulkan_CreateFontsTexture())
        recomp_fail("cannot initialize cheat menu", 0);
}
RecompCheatMenu::~RecompCheatMenu() {
    cancel = true;
    if (download.valid()) download.wait();
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(context);
    vkDestroyRenderPass(device, render_pass, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
}
void RecompCheatMenu::start_download(bool refresh) {
    if (download.valid()) return;
    loaded = true;
    try {
        download = std::async(std::launch::async, [this, refresh] {
            return RecompCheats::load_catalog(key, cache, refresh, cancel);
        });
    } catch (const std::exception &e) { catalog.status = e.what(); }
}
void RecompCheatMenu::event(const SDL_Event &event) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F12) {
        visible = !visible;
        release_inputs = true;
        if (visible) { SDL_StartTextInput(); if (!loaded) start_download(false); }
        else { SDL_StopTextInput(); SDL_CaptureMouse(SDL_FALSE); ImGui::GetIO().ClearInputKeys(); }
        return;
    }
    if (!visible) return;
    if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        visible = false; release_inputs = true;
        SDL_StopTextInput(); SDL_CaptureMouse(SDL_FALSE); ImGui::GetIO().ClearInputKeys();
        return;
    }
    ImGui_ImplSDL2_ProcessEvent(&event);
}
void RecompCheatMenu::update() {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    if (download.valid() && download.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            catalog = download.get();
            for (auto &cheat : catalog.cheats) cheat.compile(runtime);
        } catch (const std::exception &e) { catalog.status = e.what(); }
    }
    int key_count = 0;
    const Uint8 *keys = SDL_GetKeyboardState(&key_count);
    if (release_inputs && std::none_of(keys, keys + key_count, [](Uint8 key) { return key != 0; })) release_inputs = false;
    bool gs_button = !captures_input() && (SDL_GetWindowFlags(app.appWindow->sdlWindow) & SDL_WINDOW_INPUT_FOCUS) && keys[SDL_SCANCODE_F11];
    draw_ready = false;
    if (visible) {
        ImGui_ImplSDL2_NewFrame();
        ImGui_ImplVulkan_NewFrame();
        ImGui::NewFrame();
        window();
        ImGui::Render();
        draw_ready = true;
    }
    // The owner is between guest dispatches and renderer worker receipts have
    // been merged before input. The present thread never reads or writes RAM.
    for (const auto &cheat : catalog.cheats) if (cheat.enabled)
        recomp_cheats_apply(runtime, cheat.program.data(), cheat.program.size(), gs_button);
}
void RecompCheatMenu::window() {
    const auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(920.0f, io.DisplaySize.x - 24), std::min(620.0f, io.DisplaySize.y - 24)), ImGuiCond_FirstUseEver);
    bool show = visible;
    if (ImGui::Begin("GameShark cheats (F12)", &show)) {
        ImGui::TextUnformatted(title.c_str());
        ImGui::Text("ROM %s", key.c_str());
        size_t active = std::count_if(catalog.cheats.begin(), catalog.cheats.end(), [](const auto &c) { return c.enabled; });
        ImGui::Text("%zu cheats, %zu enabled", catalog.cheats.size(), active);
        ImGui::SameLine();
        if (ImGui::Button("Disable all")) for (auto &cheat : catalog.cheats) cheat.enabled = false;
        ImGui::SameLine();
        ImGui::BeginDisabled(download.valid() || active != 0);
        if (ImGui::Button("Refresh downloads")) start_download(true);
        ImGui::EndDisabled();
        if (active) ImGui::TextDisabled("Disable all cheats before refreshing the code list.");
        if (download.valid()) ImGui::TextUnformatted("Downloading cheat lists in the background...");
        ImGui::TextWrapped("%s", catalog.status.c_str());
        ImGui::TextWrapped("F12 / Escape closes. Use mouse or Tab, arrows and Space. Hold F11 in game for GS-button codes.");
        ImGui::TextWrapped("Disabling stops writes. Changes already made to game state or saves may need a reload. Codes that patch compiled instructions are unsupported.");
        filter.Draw("Search", -1);
        ImGui::Separator();
        ImGui::BeginChild("cheat list", ImVec2(0, 0), true);
        for (size_t i = 0; i < catalog.cheats.size(); i++) {
            auto &cheat = catalog.cheats[i];
            if (!filter.PassFilter(cheat.name.c_str())) continue;
            ImGui::PushID(static_cast<int>(i));
            ImGui::BeginDisabled(!cheat.problem.empty() || download.valid());
            if (ImGui::Checkbox("##enabled", &cheat.enabled) && cheat.enabled) {
                // Revalidate at activation, including all writes in a repeat.
                char reason[192];
                if (!recomp_cheats_validate(runtime, cheat.program.data(), cheat.program.size(), reason, sizeof(reason))) {
                    cheat.enabled = false; cheat.problem = reason;
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            bool details = ImGui::TreeNodeEx("details", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", cheat.name.c_str());
            if (ImGui::IsItemHovered() && !cheat.note.empty()) ImGui::SetTooltip("%s", cheat.note.c_str());
            if (details) {
                ImGui::TextDisabled("Source: %s", cheat.source.c_str());
                if (!cheat.note.empty()) ImGui::TextWrapped("%s", cheat.note.c_str());
                if (!cheat.problem.empty()) ImGui::TextWrapped("Unavailable: %s", cheat.problem.c_str());
                if (!cheat.options.empty()) {
                    ImGui::BeginDisabled(cheat.enabled || !cheat.problem.empty());
                    if (ImGui::BeginCombo("Value", cheat.options[cheat.option].name.c_str())) {
                        for (size_t n = 0; n < cheat.options.size(); n++) {
                            ImGui::PushID(static_cast<int>(n));
                            if (ImGui::Selectable(cheat.options[n].name.c_str(), cheat.option == static_cast<int>(n))) {
                                cheat.option = static_cast<int>(n); cheat.compile(runtime);
                            }
                            ImGui::PopID();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::EndDisabled();
                }
                for (const auto &line : cheat.program) ImGui::Text("%08X %04X", line.address, line.value);
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
    if (!show) {
        visible = false; release_inputs = true;
        SDL_StopTextInput(); SDL_CaptureMouse(SDL_FALSE); ImGui::GetIO().ClearInputKeys();
    }
}
void RecompCheatMenu::draw(plume::RenderCommandList *list) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    if (!visible || !draw_ready) return;
    ContextScope restore(context);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<plume::VulkanCommandList *>(list)->vk);
}
