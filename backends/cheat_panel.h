#pragma once
#include "cheat_database.h"
#include "imgui.h"
#include <SDL.h>
#include <future>

// The backend installs its ImGui context before forwarding input and frame updates.
class CheatPanel {
public:
    CheatPanel(Runtime *, const char *data_path);
    ~CheatPanel();
    void toggle();
    void event(const SDL_Event&);
    void update(SDL_Window *, void (*renderer_frame)());
    bool open() const {
        return visible;
    }
    bool captures_input() const {
        return visible || release_inputs;
    }

private:
    void start_download(bool refresh);
    void window();
    Runtime *runtime;
    std::atomic<bool> visible{false}, cancel{false};
    bool release_inputs = false, loaded = false;
    std::string key, title;
    std::filesystem::path cache;
    RecompCheats::Catalog catalog;
    std::future<RecompCheats::Catalog> download;
    ImGuiTextFilter filter;
};
