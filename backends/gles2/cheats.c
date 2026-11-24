#include "cheat_overlay.h"
#include "../cheat_database.h"
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#define CIMGUI_USE_SDL2
#define CIMGUI_USE_OPENGL3
#include <cimgui.h>
#include <cimgui_impl.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5_webgl.h>
#endif

struct Gles2Cheats {
    Runtime *runtime;
    SDL_Window *window;
    char key[32], title[21], *directory;
    CheatCatalog catalog, incoming;
    ImGuiTextFilter *filter;
    SDL_Thread *worker;
    atomic_bool cancel, ready;
    bool refresh, loaded, visible, release_inputs;
};

#ifdef __EMSCRIPTEN__
static atomic_uint toggle_requests;
EMSCRIPTEN_KEEPALIVE void recomp_web_toggle_cheats(void) {
    atomic_fetch_add(&toggle_requests, 1);
}
#endif

static int SDLCALL load(void *opaque) {
    Gles2Cheats *s = opaque;
    cheat_catalog_load(&s->incoming, s->key, s->directory, s->refresh, &s->cancel);
    atomic_store(&s->ready, true);
    return 0;
}
static void start_download(Gles2Cheats *s, bool refresh) {
    if (s->worker)
        return;
    atomic_store(&s->ready, false);
    s->refresh = refresh;
    s->worker = SDL_CreateThread(load, "Cheat catalog", s);
    s->loaded = s->worker != NULL;
    if (!s->worker)
        SDL_strlcpy(s->catalog.status, SDL_GetError(), sizeof(s->catalog.status));
}
static void toggle(Gles2Cheats *s) {
    s->visible = !s->visible;
    s->release_inputs = true;
    if (s->visible) {
        SDL_StartTextInput();
        if (!s->loaded)
            start_download(s, false);
    } else {
        SDL_StopTextInput();
        SDL_CaptureMouse(SDL_FALSE);
        ImGuiIO_ClearInputKeys(igGetIO());
    }
}
Gles2Cheats *gles2_cheats_create(Runtime *rt, SDL_Window *window, const char *data_path) {
    Gles2Cheats *s = calloc(1, sizeof(*s));
    if (!s)
        recomp_fail("cannot allocate cheat menu", 0);
    s->runtime = rt;
    s->window = window;
    atomic_init(&s->cancel, false);
    atomic_init(&s->ready, false);
    const uint8_t *h = rt->rom;
    if (h && rt->rom_size >= 64) {
        snprintf(s->key, sizeof(s->key), "%02X%02X%02X%02X-%02X%02X%02X%02X-C:%02X", h[16], h[17], h[18], h[19], h[20],
                 h[21], h[22], h[23], h[62]);
        memcpy(s->title, h + 32, 20);
        for (int i = 19; i >= 0 && (s->title[i] == ' ' || !s->title[i]); --i)
            s->title[i] = 0;
    }
    char folder[sizeof(s->key)];
    SDL_strlcpy(folder, s->key, sizeof(folder));
    char *colon = strchr(folder, ':');
    if (colon)
        *colon = '-';
    if (SDL_asprintf(&s->directory, "%s/cheats/%s", data_path, folder) < 0)
        recomp_fail("cannot allocate cheat cache path", 0);
    igCreateContext(NULL);
    ImGuiIO *io = igGetIO();
    io->IniFilename = NULL;
    io->LogFilename = NULL;
    io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    s->filter = ImGuiTextFilter_ImGuiTextFilter("");
    if (!s->filter || !ImGui_ImplSDL2_InitForOther(window) || !ImGui_ImplOpenGL3_Init("#version 300 es"))
        recomp_fail("cannot initialize GLES cheat menu", 0);
    return s;
}
void gles2_cheats_destroy(Gles2Cheats *s) {
    if (!s)
        return;
    if (s->visible)
        toggle(s);
    atomic_store(&s->cancel, true);
    if (s->worker)
        SDL_WaitThread(s->worker, NULL);
    cheat_catalog_destroy(&s->incoming);
    cheat_catalog_destroy(&s->catalog);
    ImGuiTextFilter_destroy(s->filter);
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    igDestroyContext(NULL);
    SDL_free(s->directory);
    free(s);
}
bool gles2_cheats_event(Gles2Cheats *s, const SDL_Event *event) {
    if (event->type == SDL_KEYDOWN && !event->key.repeat && event->key.keysym.scancode == SDL_SCANCODE_F12)
        toggle(s);
    else if (s->visible) {
        if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_SCANCODE_ESCAPE)
            toggle(s);
        else
            ImGui_ImplSDL2_ProcessEvent(event);
    }
    return s->visible || s->release_inputs;
}
static void draw_cheat(Gles2Cheats *s, Cheat *c) {
    igBeginDisabled(*c->problem || s->worker);
    if (igCheckbox("##enabled", &c->enabled) && c->enabled)
        c->enabled = recomp_cheats_validate(s->runtime, c->program, c->line_count, c->problem, sizeof(c->problem));
    igEndDisabled();
    igSameLine(0, -1);
    bool details = igTreeNodeEx_StrStr("details", ImGuiTreeNodeFlags_SpanAvailWidth, "%s", c->name);
    if (igIsItemHovered(0) && c->note)
        igSetTooltip("%s", c->note);
    if (!details)
        return;
    igTextDisabled("Source: %s", c->source);
    if (c->note)
        igTextWrapped("%s", c->note);
    if (*c->problem)
        igTextWrapped("Unavailable: %s", c->problem);
    if (c->option_count) {
        igBeginDisabled(c->enabled || *c->problem);
        if (igBeginCombo("Value", c->options[c->option].name, 0)) {
            for (size_t i = 0; i < c->option_count; ++i) {
                igPushID_Int((int)i);
                if (igSelectable_Bool(c->options[i].name, c->option == i, 0, (ImVec2){0})) {
                    c->option = i;
                    cheat_compile(c, s->runtime);
                }
                igPopID();
            }
            igEndCombo();
        }
        igEndDisabled();
    }
    for (size_t i = 0; i < c->line_count; ++i) {
        if (c->program && !*c->problem)
            igText("%08X %04X", c->program[i].address, c->program[i].value);
        else
            igText("%08X %04X", c->lines[i].address, c->lines[i].value);
    }
    igTreePop();
}
static void menu(Gles2Cheats *s) {
    ImVec2 size = igGetIO()->DisplaySize;
    igSetNextWindowPos((ImVec2){size.x / 2, size.y / 2}, ImGuiCond_FirstUseEver, (ImVec2){0.5f, 0.5f});
    igSetNextWindowSize((ImVec2){SDL_min(920.0f, size.x - 24), SDL_min(620.0f, size.y - 24)}, ImGuiCond_FirstUseEver);
    bool show = true;
    if (igBegin("GameShark cheats (F12)", &show, 0)) {
        CheatCatalog *db = &s->catalog;
        igTextUnformatted(s->title, NULL);
        igText("ROM %s", s->key);
        size_t active = 0;
        for (size_t i = 0; i < db->count; ++i)
            active += db->cheats[i].enabled;
        igText("%zu cheats, %zu enabled", db->count, active);
        igSameLine(0, -1);
        if (igButton("Disable all", (ImVec2){0}))
            for (size_t i = 0; i < db->count; ++i)
                db->cheats[i].enabled = false;
        igSameLine(0, -1);
        igBeginDisabled(s->worker || active);
        if (igButton("Refresh downloads", (ImVec2){0}))
            start_download(s, true);
        igEndDisabled();
        if (active)
            igTextDisabled("Disable all cheats before refreshing the code list.");
        if (s->worker)
            igTextUnformatted("Downloading cheat lists in the background...", NULL);
        igTextWrapped("%s", db->status);
        igTextWrapped("F12 / Escape closes. Use mouse or Tab, arrows and Space. Hold F11 in game for GS-button codes.");
        igTextWrapped("Disabling stops writes. Changes already made to game state or saves may need a reload. Codes "
                      "that patch compiled instructions are unsupported.");
        igTextUnformatted("Search", NULL);
        igSameLine(0, -1);
        ImGuiTextFilter_Draw(s->filter, "##search", -1);
        igSeparator();
        if (igBeginChild_Str("cheat list", (ImVec2){0}, ImGuiChildFlags_Border, 0)) {
            for (size_t i = 0; i < db->count; ++i) {
                Cheat *c = &db->cheats[i];
                if (!ImGuiTextFilter_PassFilter(s->filter, c->name, NULL))
                    continue;
                igPushID_Int((int)i);
                draw_cheat(s, c);
                igPopID();
            }
        }
        igEndChild();
    }
    igEnd();
    if (!show)
        toggle(s);
}
bool gles2_cheats_update(Gles2Cheats *s) {
#ifdef __EMSCRIPTEN__
    if (atomic_exchange(&toggle_requests, 0) & 1u)
        toggle(s);
#endif
    if (s->worker && atomic_load(&s->ready)) {
        SDL_WaitThread(s->worker, NULL);
        s->worker = NULL;
        cheat_catalog_destroy(&s->catalog);
        s->catalog = s->incoming;
        s->incoming = (CheatCatalog){0};
        for (size_t i = 0; i < s->catalog.count; ++i)
            cheat_compile(&s->catalog.cheats[i], s->runtime);
    }
    int n = 0;
    const Uint8 *keys = SDL_GetKeyboardState(&n);
    if (s->release_inputs) {
        s->release_inputs = false;
        for (int i = 0; i < n; ++i)
            if (keys[i]) {
                s->release_inputs = true;
                break;
            }
    }
    bool gs = !s->visible && !s->release_inputs && (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) &&
              keys[SDL_SCANCODE_F11];
    if (s->visible) {
        ImGui_ImplSDL2_NewFrame();
#ifdef __EMSCRIPTEN__
        ImGuiIO *io = igGetIO();
        int w, h;
        if (io->DisplaySize.x > 0 && io->DisplaySize.y > 0 &&
            emscripten_webgl_get_drawing_buffer_size(emscripten_webgl_get_current_context(), &w, &h) ==
                EMSCRIPTEN_RESULT_SUCCESS)
            io->DisplayFramebufferScale = (ImVec2){w / io->DisplaySize.x, h / io->DisplaySize.y};
#endif
        ImGui_ImplOpenGL3_NewFrame();
        igNewFrame();
        menu(s);
        igRender();
    }
    for (size_t i = 0; i < s->catalog.count; ++i) {
        Cheat *c = &s->catalog.cheats[i];
        if (c->enabled)
            recomp_cheats_apply(s->runtime, c->program, c->line_count, gs);
    }
    return s->visible || s->release_inputs;
}
void gles2_cheats_draw(Gles2Cheats *s) {
    if (s->visible && igGetDrawData())
        ImGui_ImplOpenGL3_RenderDrawData(igGetDrawData());
}
