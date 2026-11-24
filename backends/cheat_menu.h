#pragma once
#include "cheat_panel.h"
#include "hle/rt64_application.h"
#include "imgui.h"
#include "plume_vulkan.h"

/* UI and Runtime access stay on the SDL owner. The present hook renders only
 * completed ImGui draw data. RT64's inspector mutex guards every context switch
 * and prevents the owner rebuilding draw data while it is being submitted. */
class RecompCheatMenu {
public:
    RecompCheatMenu(RT64::Application&, Runtime *, const char *data_path);
    ~RecompCheatMenu();
    void event(const SDL_Event&);
    void update();
    void draw(plume::RenderCommandList *);
    bool open() const {
        return panel.open();
    }
    bool captures_input() const {
        return panel.captures_input();
    }

private:
    RT64::Application& app;
    CheatPanel panel;
    ImGuiContext *context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    bool draw_ready = false;
};
