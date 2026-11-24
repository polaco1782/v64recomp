#include "cheat_overlay.h"
#include "../cheat_panel.h"
#include "backends/imgui_impl_opengl3.h"
#include "backends/imgui_impl_sdl2.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5_webgl.h>
#endif

struct Gles2Cheats {
    CheatPanel panel;
    SDL_Window *window;

    Gles2Cheats(Runtime *rt, SDL_Window *window, const char *data_path) : panel(rt, data_path), window(window) {}
};

#ifdef __EMSCRIPTEN__
// The page runs on the browser thread. SDL and the menu belong to the game thread.
static std::atomic<unsigned> toggle_requests{0};
extern "C" EMSCRIPTEN_KEEPALIVE void recomp_web_toggle_cheats(void) {
    ++toggle_requests;
}
#endif

Gles2Cheats *gles2_cheats_create(Runtime *rt, SDL_Window *window, const char *data_path) {
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    if (!ImGui_ImplSDL2_InitForOther(window) || !ImGui_ImplOpenGL3_Init("#version 300 es"))
        recomp_fail("cannot initialize GLES cheat menu", 0);
    return new Gles2Cheats(rt, window, data_path);
}

void gles2_cheats_destroy(Gles2Cheats *menu) {
    if (!menu)
        return;
    if (menu->panel.open())
        menu->panel.toggle();
    delete menu;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

bool gles2_cheats_event(Gles2Cheats *menu, const SDL_Event *event) {
    menu->panel.event(*event);
    return menu->panel.captures_input();
}

static void renderer_frame() {
#ifdef __EMSCRIPTEN__
    // The proxied WebGL context is created outside SDL. Use its actual backing size.
    auto& io = ImGui::GetIO();
    int width, height;
    if (io.DisplaySize.x > 0 && io.DisplaySize.y > 0 &&
        emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &width, &height) ==
            EMSCRIPTEN_RESULT_SUCCESS)
        io.DisplayFramebufferScale = ImVec2(width / io.DisplaySize.x, height / io.DisplaySize.y);
#endif
    ImGui_ImplOpenGL3_NewFrame();
}

bool gles2_cheats_update(Gles2Cheats *menu) {
#ifdef __EMSCRIPTEN__
    if (toggle_requests.exchange(0) & 1u)
        menu->panel.toggle();
#endif
    menu->panel.update(menu->window, renderer_frame);
    return menu->panel.captures_input();
}

void gles2_cheats_draw(Gles2Cheats *menu) {
    if (menu->panel.open() && ImGui::GetDrawData())
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}
