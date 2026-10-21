#pragma once
#include "cheat_database.h"
#include "hle/rt64_application.h"
#include "imgui.h"
#include "plume_vulkan.h"
#include <future>

/* UI and Runtime access stay on the SDL owner. The present hook renders only
 * completed ImGui draw data. RT64's inspector mutex guards every context switch
 * and prevents the owner rebuilding draw data while it is being submitted. */
class RecompCheatMenu {
public:
    RecompCheatMenu(RT64::Application &, Runtime *, const char *data_path);
    ~RecompCheatMenu();
    void event(const SDL_Event &);
    void update();
    void draw(plume::RenderCommandList *);
    bool open() const { return visible.load(); }
    bool captures_input() const { return visible || release_inputs; }
private:
    void start_download(bool refresh);
    void window();
    RT64::Application &app;
    Runtime *runtime;
    ImGuiContext *context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    std::atomic<bool> visible{false}, cancel{false};
    bool release_inputs = false, loaded = false, draw_ready = false;
    std::string key, title;
    std::filesystem::path cache;
    RecompCheats::Catalog catalog;
    std::future<RecompCheats::Catalog> download;
    ImGuiTextFilter filter;
};
