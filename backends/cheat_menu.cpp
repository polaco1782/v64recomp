#include "cheat_menu.h"
#include "hle/rt64_present_queue.h"
#include "backends/imgui_impl_vulkan.h"
#include "imgui/imgui_impl_sdl2_custom.h"

namespace {
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
        recomp_fail("cheat menu Vulkan failure", static_cast<uint32_t>(result));
}
}
RecompCheatMenu::RecompCheatMenu(RT64::Application& application, Runtime *rt, const char *data_path)
    : app(application), panel(rt, data_path) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(nullptr);
    context = ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    if (!ImGui_ImplSDL2_InitForOther(app.appWindow->sdlWindow))
        recomp_fail("cannot initialize cheat menu input", 0);
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
        recomp_fail("cannot initialize cheat menu", 0);
}
RecompCheatMenu::~RecompCheatMenu() {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(context);
    vkDestroyRenderPass(device, render_pass, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
}
void RecompCheatMenu::event(const SDL_Event& event) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    panel.event(event);
    if (!panel.open())
        draw_ready = false;
}
void RecompCheatMenu::update() {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    ContextScope restore(context);
    panel.update(app.appWindow->sdlWindow, ImGui_ImplVulkan_NewFrame);
    draw_ready = panel.open();
}
void RecompCheatMenu::draw(plume::RenderCommandList *list) {
    const std::lock_guard lock(app.presentQueue->inspectorMutex);
    if (!draw_ready)
        return;
    ContextScope restore(context);
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), static_cast<plume::VulkanCommandList *>(list)->vk);
}
