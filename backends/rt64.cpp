#include "renderer.h"
#include "timing.h"
#include "input.h"
#include "hle/rt64_application.h"
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_present_queue.h"
#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include "rhi/rt64_render_hooks.h"
#include "imgui.h"
#include "textures.h"
#include "texture_editor.h"
#include "cheat_menu.h"
#ifdef RECOMP_DEBUG
#include "debug_overlay.h"
#endif
#ifndef _WIN32
#define Status int
#include <X11/Xutil.h>
#undef Status
#endif
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <stdexcept>

extern "C" void recomp_rt64_set_anamorphic_present(bool enabled);

static std::string texture_dump_directory;
static bool texture_editor_enabled;
static uint64_t texture_editor_paused_ns;

void recomp_renderer_enable_texture_editor(bool enabled) {
    texture_editor_enabled = enabled;
}
uint64_t recomp_renderer_paused_ns(void) {
    return texture_editor_paused_ns;
}

void recomp_renderer_configure_texture_dump(const char *directory) {
    texture_dump_directory = directory ? directory : "";
}

struct RecompRenderer {
    Runtime *rt;
    uint32_t code = 0, data = 0, list = 0;
    RecompGraphicsHook graphics_hook = nullptr;
    RecompMicrocodeHook microcode_hook = nullptr;
    void (*fov_hook)(int) = nullptr;
    bool (*aspect_hook)(void *, float) = nullptr;
    void *aspect_opaque = nullptr;
    bool expanded_aspect = false;
    bool custom_gbi = false;
    RT64::GBI c_gbi{}, c_base_gbi{};
    const RecompMicrocodeConfig *c_gbi_config = nullptr;
    std::array<RecompMicrocodeCommand, UCODE_MAP_SIZE> c_commands{};
    bool raw_rdp = false, raw_vi = false;
    std::thread::id owner = std::this_thread::get_id();
    std::unique_ptr<RT64::Application> app;
    std::unique_ptr<RecompCheatMenu> cheat_menu;
    SDL_EventFilter previous_event_filter = nullptr;
    void *previous_event_userdata = nullptr;
#ifdef RECOMP_DEBUG
    std::unique_ptr<RecompDebugOverlay> debug_overlay;
    RecompDebugGeometryStats debug_geometry;
    uint64_t debug_workload_id = 0;
#endif
    std::array<uint8_t, 64> header{};
    std::array<uint8_t, 4096> dmem{}, imem{};
    uint32_t mi = 0, dp_start = 0, dp_end = 0, dp_current = 0, dp_status = 0, dp_clock = 0, dp_busy = 0, dp_pipe = 0,
             dp_tmem = 0;
    RecompViRegisters vi{};
    RecompRendererStats stats{};
    FILE *frame_trace = nullptr;
    std::atomic<uint64_t> display_frames{0}, first_display_ns{0}, last_display_ns{0};
    uint64_t timing_display_frames = 0;
    RecompTextureFiles textureFiles;
    std::unique_ptr<RecompTextureEditor> texture_editor;
    uint64_t token = 0;
    bool sp_seen = false, dp_seen = false;
    bool ready = false;
    SDL_Window *window = nullptr;
    std::unique_ptr<RecompInput, decltype(&recomp_input_destroy)> input{nullptr, recomp_input_destroy};
    bool mouse_enabled = false, mouse_gameplay = false, mouse_released = false, mouse_captured = false;
    bool separate_mouse_actions = false;
    bool texture_editor_escape = false;
    bool keyboard_c_buttons = false, wasd_stick = false;
    bool gamepad_camera = false;
    RecompCameraStick camera_stick{};
    RecompMouseFrame mouse_frame{};
    explicit RecompRenderer(Runtime *runtime) : rt(runtime) {}
    void check_owner() {
        if (owner != std::this_thread::get_id())
            recomp_fail("RT64 callback on a non-owner thread", 0);
    }
    void wait_workers() {
        app->workloadQueue->waitForWorkloadId(app->state->workloadId);
        app->presentQueue->waitForPresentId(app->state->presentId);
    }
    void wait_texture_reload() {
        wait_workers();
        // A workload ID is signaled after its first rendered frame. Remaining
        // interpolated frames can still hold the old texture snapshot. Match
        // RT64's resource-reload barrier before rebuilding the replacement map.
        app->workloadQueue->waitForIdle();
        app->presentQueue->waitForIdle();
    }
    void sync_vi() {
        if (raw_vi)
            return;
        (void)recomp_vi_registers(rt, &vi);
        // RT64 estimates the color-image base by subtracting a scanline from
        // VI_ORIGIN. Custom modes can start at row zero (Shadow Man does),
        // which otherwise selects unrelated RAM instead of the GPU image.
        // The OS swap state supplies the actual base even for CPU-drawn
        // loading images. Normalize only the renderer's copy when the guest
        // mode starts at that base, preserving modes with a scanline offset.
        const RT64::VI decoded = app->core.decodeVI();
        const RecompViState guest = recomp_vi_state(rt);
        if (decoded.visible() && guest.framebuffer && vi.origin == recomp_ram_address(rt, guest.framebuffer, 1)) {
            const uint32_t rowOffset = vi.origin - decoded.fbAddress();
            if (rowOffset && uint64_t(vi.origin) + rowOffset < rt->rdram_size)
                vi.origin += rowOffset;
        }
    }
};
static RecompRenderer *active_renderer;
static_assert(RECOMP_MICROCODE_MAX_LIGHTS == RSP_MAX_LIGHTS, "C microcode light limit differs from RT64");

struct RecompMicrocodeContext {
    RT64::State *state;
    RT64::DisplayList **list;
};

static RecompRenderer *microcode_owner(RT64::State *state) {
    RecompRenderer *s = active_renderer;
    if (!s || s->app->state.get() != state || !s->c_gbi_config)
        recomp_fail("invalid C microcode callback context", 0);
    s->check_owner();
    return s;
}
static void microcode_reset(RT64::State *state, bool from_task) {
    RecompRenderer *s = microcode_owner(state);
    const auto reset = from_task ? s->c_base_gbi.resetFromTask : s->c_base_gbi.resetFromLoad;
    if (reset)
        reset(state);
    if (s->c_gbi_config->reset) {
        RecompMicrocodeContext context{state, nullptr};
        s->c_gbi_config->reset(&context, from_task, s->c_gbi_config->userdata);
    }
}
static void microcode_reset_task(RT64::State *state) {
    microcode_reset(state, true);
}
static void microcode_reset_load(RT64::State *state) {
    microcode_reset(state, false);
}
static void microcode_command(RT64::State *state, RT64::DisplayList **list) {
    RecompRenderer *s = microcode_owner(state);
    const uint32_t w0 = (*list)->w0, w1 = (*list)->w1;
    const auto command = s->c_commands[w0 >> 24];
    if (!command)
        recomp_fail("missing C microcode command handler", w0);
    RecompMicrocodeContext context{state, list};
    command(&context, w0, w1, s->c_gbi_config->userdata);
}
void recomp_renderer_bind_microcode(void *opaque, uint32_t code, uint32_t data, const RecompMicrocodeConfig *config) {
    RecompRenderer *s = active_renderer;
    if (!s || !config || s->app->interpreter.get() != opaque || (config->binding_count && !config->bindings))
        recomp_fail("invalid C microcode binding", code);
    s->check_owner();
    if (s->c_gbi_config != config) {
        s->c_base_gbi = {};
        RT64::GBI_RDP::setup(&s->c_base_gbi, true);
        switch (config->base) {
            case RECOMP_GBI_F3D:
                s->c_base_gbi.ucode = RT64::GBIUCode::F3D;
                RT64::GBI_F3D::setup(&s->c_base_gbi);
                break;
            case RECOMP_GBI_F3DEX:
                s->c_base_gbi.ucode = RT64::GBIUCode::F3DEX;
                RT64::GBI_F3DEX::setup(&s->c_base_gbi);
                break;
            case RECOMP_GBI_F3DEX2:
                s->c_base_gbi.ucode = RT64::GBIUCode::F3DEX2;
                RT64::GBI_F3DEX2::setup(&s->c_base_gbi);
                break;
            default:
                recomp_fail("unsupported C microcode base", config->base);
        }
        s->c_base_gbi.flags.LowP = config->low_precision;
        s->c_base_gbi.flags.NoN = config->no_near_clip;
        s->c_base_gbi.flags.ReJ = config->reject;
        s->c_gbi = s->c_base_gbi;
        s->c_commands.fill(nullptr);
        for (size_t i = 0; i < config->binding_count; i++) {
            const auto& binding = config->bindings[i];
            if (!binding.command || s->c_commands[binding.opcode])
                recomp_fail("invalid or duplicate C microcode command", binding.opcode);
            s->c_commands[binding.opcode] = binding.command;
            s->c_gbi.map[binding.opcode] = microcode_command;
        }
        s->c_gbi.resetFromTask = microcode_reset_task;
        s->c_gbi.resetFromLoad = microcode_reset_load;
        s->c_gbi_config = config;
    }
    auto *interpreter = s->app->interpreter.get();
    interpreter->hleGBI = &s->c_gbi;
    interpreter->UCode.textAddress = code;
    interpreter->UCode.dataAddress = data;
    interpreter->state->rsp->setGBI(&s->c_gbi);
}
void recomp_microcode_read(RecompMicrocodeContext *context, uint32_t address, bool segmented, void *bytes,
                           size_t size) {
    RecompRenderer *s = microcode_owner(context->state);
    if (segmented)
        address = context->state->rsp->fromSegmentedMasked(address);
    if (address > s->rt->rdram_size || size > s->rt->rdram_size - address)
        recomp_fail("C microcode read outside RDRAM", address);
    auto *output = static_cast<uint8_t *>(bytes);
    for (size_t i = 0; i < size; i++)
        output[i] = context->state->RDRAM[(address + i) ^ 3];
}
void recomp_microcode_default(RecompMicrocodeContext *context) {
    RecompRenderer *s = microcode_owner(context->state);
    if (!context->list)
        recomp_fail("C microcode default command outside display list", 0);
    const uint32_t word = (*context->list)->w0;
    const auto handler = s->c_base_gbi.map[word >> 24];
    if (!handler)
        recomp_fail("C microcode base command is unimplemented", word);
    handler(context->state, context->list);
}
uint32_t recomp_microcode_data_address(RecompMicrocodeContext *context) {
    microcode_owner(context->state);
    return context->state->ext.interpreter->UCode.dataAddress;
}
uint32_t recomp_microcode_geometry_mode(RecompMicrocodeContext *context) {
    microcode_owner(context->state);
    const auto& rsp = *context->state->rsp;
    return rsp.geometryModeStack[rsp.geometryModeStackSize - 1];
}
unsigned recomp_microcode_light_count(RecompMicrocodeContext *context) {
    microcode_owner(context->state);
    const int count = context->state->rsp->lightCount;
    if (count < 0 || count > RECOMP_MICROCODE_MAX_LIGHTS)
        recomp_fail("invalid C microcode light count", count);
    return unsigned(count);
}
void recomp_microcode_light(RecompMicrocodeContext *context, unsigned index, float direction[3], float color[3]) {
    microcode_owner(context->state);
    if (index > RECOMP_MICROCODE_MAX_LIGHTS)
        recomp_fail("invalid C microcode light index", index);
    const auto& light = context->state->rsp->lights[index].dir;
    direction[0] = light.dirx;
    direction[1] = light.diry;
    direction[2] = light.dirz;
    color[0] = light.colr / 255.0f;
    color[1] = light.colg / 255.0f;
    color[2] = light.colb / 255.0f;
}
static unsigned microcode_vertex_index(RecompMicrocodeContext *context, unsigned index) {
    microcode_owner(context->state);
    const auto& rsp = *context->state->rsp;
    if (index >= RSP_MAX_VERTICES)
        recomp_fail("invalid C microcode vertex index", index);
    const auto& draw =
        context->state->ext.workloadQueue->workloads[context->state->ext.workloadQueue->writeCursor].drawData;
    const unsigned result = rsp.indices[index];
    if (result >= draw.vertexCount())
        recomp_fail("C microcode vertex is not loaded", index);
    return result;
}
void recomp_microcode_vertex(RecompMicrocodeContext *context, unsigned index, RecompMicrocodeVertex *vertex) {
    const unsigned global = microcode_vertex_index(context, index);
    const auto& rsp = *context->state->rsp;
    const auto& draw =
        context->state->ext.workloadQueue->workloads[context->state->ext.workloadQueue->writeCursor].drawData;
    const auto& v = rsp.vertices[index];
    if (global >= draw.posTransformed.size() || global >= draw.worldIndices.size() ||
        draw.worldIndices[global] >= draw.worldTransforms.size())
        recomp_fail("C microcode vertex transform is unavailable", index);
    const auto& p = draw.posTransformed[global];
    const auto& world = draw.worldTransforms[draw.worldIndices[global]];
    *vertex = {{float(p.x), float(p.y), float(p.z)},
               {v.normal.x / 127.0f, v.normal.y / 127.0f, v.normal.z / 127.0f},
               {v.color.r / 255.0f, v.color.g / 255.0f, v.color.b / 255.0f},
               {}};
    for (unsigned row = 0; row < 3; row++)
        for (unsigned col = 0; col < 3; col++)
            vertex->world_matrix[row * 3 + col] = float(world[row][col]);
}
void recomp_microcode_vertex_color(RecompMicrocodeContext *context, unsigned index, const float color[3]) {
    const unsigned global = microcode_vertex_index(context, index);
    auto& draw = context->state->ext.workloadQueue->workloads[context->state->ext.workloadQueue->writeCursor].drawData;
    interop::RSPLight result{};
    result.col = {color[0], color[1], color[2]};
    result.colc = result.col;
    draw.lightIndices[global] = uint32_t(draw.rspLights.size());
    draw.lightCounts[global] = 1;
    draw.rspLights.push_back(result);
    context->state->updateDrawStatusAttribute(RT64::DrawAttribute::Lights);
}
static uint64_t monotonic_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static void trace_frame(RecompRenderer *s, const char *event, uint64_t value = 0) {
    if (s->frame_trace &&
        std::fprintf(s->frame_trace, "%llu,%s,%llu\n", static_cast<unsigned long long>(monotonic_ns()), event,
                     static_cast<unsigned long long>(value)) < 0)
        recomp_fail("cannot write renderer frame trace", 0);
}
static void display_draw(plume::RenderCommandList *list, plume::RenderFramebuffer *) {
    RecompRenderer *s = active_renderer;
    uint64_t now = monotonic_ns();
    if (s->display_frames.load() == 0)
        s->first_display_ns = now;
    s->last_display_ns = now;
    s->display_frames++;
    trace_frame(s, "display");
#ifdef RECOMP_DEBUG
    if (s->debug_overlay)
        s->debug_overlay->draw(list, s->display_frames.load(), now);
#endif
    if (s->cheat_menu)
        s->cheat_menu->draw(list);
    if (s->texture_editor)
        s->texture_editor->draw(list);
}
static int menu_event_filter(void *opaque, SDL_Event *event) {
    auto *s = static_cast<RecompRenderer *>(opaque);
    if (s->texture_editor && (s->texture_editor->open() || ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
                                                            event->key.keysym.scancode == SDL_SCANCODE_F8)))
        return 1;
    // Owner shortcuts must reach the app before RT64 consumes them (notably
    // F4, which otherwise opens the replacements inspector instead of recapturing).
    if (s->mouse_enabled && (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) &&
        (event->key.keysym.scancode == SDL_SCANCODE_ESCAPE || event->key.keysym.scancode == SDL_SCANCODE_F4 ||
         event->key.keysym.scancode == SDL_SCANCODE_F10))
        return 1;
    // F12 must reach the shared menu even when RT64's inspector captures keys.
    // While open, route UI events to the owner instead of the inspector.
    if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) && event->key.keysym.scancode == SDL_SCANCODE_F12)
        return 1;
    if (s->cheat_menu && s->cheat_menu->open())
        return 1;
    return s->previous_event_filter ? s->previous_event_filter(s->previous_event_userdata, event) : 1;
}
static void interrupts() {
    RecompRenderer *s = active_renderer;
    if (!s)
        recomp_fail("RT64 interrupt without renderer", 0);
    s->check_owner();
    uint32_t bits = s->mi & 0x21u;
    s->mi &= ~bits;
    if (!bits)
        return;
    if (!s->token && !s->raw_rdp)
        recomp_fail("RT64 interrupt without task token", 0);
    if (bits & 0x20) {
        if (s->dp_seen)
            recomp_fail("invalid RT64 DP receipt", 0);
        s->dp_seen = true;
        s->stats.dp_callbacks++;
#ifdef RECOMP_DEBUG
        // Full sync has flushed every split DL into this submitted workload.
        // The parser owns the ring cursor; these descriptors are read-only to
        // the rendering workers. Ignore repeats while the inspector is paused.
        const auto& workload = s->app->workloadQueue->workloads[s->app->state->lastWorkloadIndex];
        if (workload.workloadId != s->debug_workload_id) {
            s->debug_geometry = recomp_debug_geometry(workload);
            s->debug_workload_id = workload.workloadId;
        }
#endif
    }
    if (bits & 1) {
        if (s->raw_rdp)
            recomp_fail("unexpected SP receipt during raw RDP DMA", 0);
        if (s->sp_seen)
            recomp_fail("invalid RT64 SP receipt", 0);
        s->sp_seen = true;
        s->stats.sp_callbacks++;
    }
}

static bool process_graphics(RecompRenderer *s) {
    uint64_t start = monotonic_ns();
    uint64_t cpu_start = s->rt->timing ? recomp_timing_thread_clock() : 0;
    trace_frame(s, "graphics_begin", s->code);
    bool custom =
        s->microcode_hook && s->microcode_hook(s->app->interpreter.get(), s->rt->rdram_size, s->code, s->data);
    if (!custom && s->custom_gbi) {
        s->app->interpreter->UCode.textAddress = UINT32_MAX;
        s->app->interpreter->UCode.dataAddress = UINT32_MAX;
    }
    if (custom && (!s->app->interpreter->hleGBI || s->app->interpreter->UCode.textAddress != s->code ||
                   s->app->interpreter->UCode.dataAddress != s->data)) {
        recomp_warn("custom graphics microcode binding is incomplete", s->code);
        return false;
    }
    s->custom_gbi = custom;
    // Quake alternates F3DEX.NoN and F3DLP.Rej. Let RT64 select the cached
    // decoder and its variant flags; it prints diagnostics if lookup fails.
    s->app->interpreter->loadUCodeGBI(s->code, s->data, true);
    if (!s->app->interpreter->hleGBI) {
        recomp_warn("RT64 did not recognize the graphics microcode", s->code);
        return false;
    }
    if (s->graphics_hook)
        s->graphics_hook(s->app->interpreter.get(), s->rt->rdram_size, &s->stats.rdp_triangles);
    s->app->processDisplayLists(s->rt->rdram, s->list, 0, true);
    s->textureFiles.collect(*s->app->textureCache);
    uint64_t elapsed = monotonic_ns() - start;
    s->stats.submit_ns += elapsed;
    recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS, elapsed, 1);
    if (s->rt->timing) {
        uint64_t cpu_end = recomp_timing_thread_clock();
        if (cpu_start && cpu_end >= cpu_start)
            recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS_CPU, cpu_end - cpu_start, 1);
    }
    trace_frame(s, "graphics_end", s->dp_seen);
    return true;
}
static void process_present(RecompRenderer *s) {
    uint64_t start = monotonic_ns();
    trace_frame(s, "present_begin", s->vi.origin);
    // Capture live framebuffer bytes before resuming the guest. RT64's render
    // and present queues consume their own storage after this returns.
    s->app->screenApiProfiler.logAndRestart();
    s->app->state->updateScreen(s->app->core.decodeVI(), false);
    s->stats.presentations++;
    uint64_t elapsed = monotonic_ns() - start;
    s->stats.present_ns += elapsed;
    recomp_timing_add(s->rt, RECOMP_TIMING_PRESENT, elapsed, 1);
    if (s->rt->timing) {
        uint64_t displayed = s->display_frames.load();
        recomp_timing_add(s->rt, RECOMP_TIMING_DISPLAY, 0, displayed - s->timing_display_frames);
        s->timing_display_frames = displayed;
    }
    trace_frame(s, "present_end");
}

RecompRenderer *recomp_renderer_create_named(Runtime *rt, const char *data_path, const char *window_title) {
    if (active_renderer || !rt->rom || rt->rdram_size > 8 * 1024 * 1024 || !data_path)
        return nullptr;
#ifndef _WIN32
    if (!XInitThreads())
        return nullptr;
    SDL_SetHint(SDL_HINT_VIDEODRIVER, "x11");
#endif
    SDL_SetMainReady();
    if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "RT64 video initialization failed: %s\n", SDL_GetError());
        return nullptr;
    }
    auto s = std::make_unique<RecompRenderer>(rt);
    const char *deck_setting = std::getenv("RECOMP_STEAM_DECK");
    const bool steam_deck = deck_setting && !std::strcmp(deck_setting, "1");
    s->window = SDL_CreateWindow(window_title ? window_title : "Native N64 recompilation", SDL_WINDOWPOS_CENTERED,
                                 SDL_WINDOWPOS_CENTERED, 1280, steam_deck ? 800 : 960, SDL_WINDOW_RESIZABLE);
    SDL_SysWMinfo wm{};
    SDL_VERSION(&wm.version);
#ifdef _WIN32
    const auto subsystem = SDL_SYSWM_WINDOWS;
#else
    const auto subsystem = SDL_SYSWM_X11;
#endif
    if (!s->window || !SDL_GetWindowWMInfo(s->window, &wm) || wm.subsystem != subsystem) {
        std::fprintf(stderr, "RT64 native window unavailable: %s\n", SDL_GetError());
        if (s->window)
            SDL_DestroyWindow(s->window);
        SDL_Quit();
        return nullptr;
    }
    for (unsigned n = 0; n < 64; n++)
        s->header[n ^ 3] = rt->rom[n];
    RT64::Application::Core core{};
#ifdef _WIN32
    core.window = wm.info.win.window;
#else
    core.window.display = wm.info.x11.display;
    core.window.window = wm.info.x11.window;
#endif
    core.HEADER = s->header.data();
    core.RDRAM = rt->rdram;
    core.DMEM = s->dmem.data();
    core.IMEM = s->imem.data();
    core.MI_INTR_REG = &s->mi;
    core.DPC_START_REG = &s->dp_start;
    core.DPC_END_REG = &s->dp_end;
    core.DPC_CURRENT_REG = &s->dp_current;
    core.DPC_STATUS_REG = &s->dp_status;
    core.DPC_CLOCK_REG = &s->dp_clock;
    core.DPC_BUFBUSY_REG = &s->dp_busy;
    core.DPC_PIPEBUSY_REG = &s->dp_pipe;
    core.DPC_TMEM_REG = &s->dp_tmem;
    core.VI_STATUS_REG = &s->vi.status;
    core.VI_ORIGIN_REG = &s->vi.origin;
    core.VI_WIDTH_REG = &s->vi.width;
    core.VI_INTR_REG = &s->vi.intr;
    core.VI_V_CURRENT_LINE_REG = &s->vi.current_line;
    core.VI_TIMING_REG = &s->vi.burst;
    core.VI_V_SYNC_REG = &s->vi.v_sync;
    core.VI_H_SYNC_REG = &s->vi.h_sync;
    core.VI_LEAP_REG = &s->vi.leap;
    core.VI_H_START_REG = &s->vi.h_start;
    core.VI_V_START_REG = &s->vi.v_start;
    core.VI_V_BURST_REG = &s->vi.v_burst;
    core.VI_X_SCALE_REG = &s->vi.x_scale;
    core.VI_Y_SCALE_REG = &s->vi.y_scale;
    core.checkInterrupts = interrupts;
    RT64::ApplicationConfiguration config;
    config.appId = "n64-native-recomp";
    config.detectDataPath = false;
    config.useConfigurationFile = false;
    config.dataPath = data_path;
    active_renderer = s.get();
    RT64::SetRenderHooks(nullptr, display_draw, nullptr);
    try {
        s->app = std::make_unique<RT64::Application>(core, config);
#ifdef RECOMP_RT64_DEBUG
        s->app->userConfig.developerMode = true;
#else
        s->app->userConfig.developerMode = false;
#endif
        s->app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
        s->app->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
        s->app->userConfig.resolutionMultiplier = 4.0;
        s->app->userConfig.antialiasing = RT64::UserConfiguration::Antialiasing::MSAA4X;
        s->app->userConfig.threePointFiltering = false;
        // Deck packages expand to the built-in 16:10 screen; F7 still toggles it.
        using AspectRatio = RT64::UserConfiguration::AspectRatio;
        s->expanded_aspect = steam_deck;
        s->app->userConfig.aspectRatio = steam_deck ? AspectRatio::Expand : AspectRatio::Original;
        s->app->userConfig.extAspectRatio = s->app->userConfig.aspectRatio;
        s->app->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Manual;
        s->app->userConfig.refreshRateTarget = 60;
        s->app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        s->app->enhancementConfig.textureLOD.scale = true;
#ifdef _WIN32
        const uint32_t window_thread = GetCurrentThreadId();
#else
        const uint32_t window_thread = 0;
#endif
        if (s->app->setup(window_thread) != RT64::Application::SetupResult::Success) {
            std::fprintf(stderr, "RT64 setup failed: %s\n", RT64::GlobalLastError.c_str());
            s->app.reset();
            RT64::SetRenderHooks(nullptr, nullptr, nullptr);
            active_renderer = nullptr;
            SDL_DestroyWindow(s->window);
            SDL_Quit();
            return nullptr;
        }
        s->ready = true;
        if (steam_deck)
            std::fprintf(stderr, "Steam Deck display: 1280x800, expanded aspect ratio\n");
        if (const char *path = std::getenv("RECOMP_FRAME_TRACE"); path && *path) {
            s->frame_trace = std::fopen(path, "wx");
            if (!s->frame_trace)
                recomp_fail("cannot create renderer frame trace", 0);
            std::fprintf(s->frame_trace, "timestamp_ns,event,value\n");
        }
        // The app clock paces this serialized adapter. Waiting again on the
        // compositor can block guest VI/audio work while acquiring an image.
        s->app->swapChain->setVsyncEnabled(false);
        s->app->presentQueue->presentWaitEnabled = false;
        s->app->appWindow->sdlWindow = s->window;
#ifdef RECOMP_DEBUG
        s->debug_overlay = std::make_unique<RecompDebugOverlay>(*s->app);
        s->app->appWindow->sdlCheckFilterInstallation();
#endif
        s->cheat_menu = std::make_unique<RecompCheatMenu>(*s->app, rt, data_path);
        SDL_GetEventFilter(&s->previous_event_filter, &s->previous_event_userdata);
        SDL_SetEventFilter(menu_event_filter, s.get());
        std::string romTitle(reinterpret_cast<const char *>(rt->rom + 32), 20);
        while (!romTitle.empty() && (romTitle.back() == ' ' || romTitle.back() == '\0'))
            romTitle.pop_back();
        if (texture_editor_enabled)
            s->texture_editor = std::make_unique<RecompTextureEditor>(*s->app, s->textureFiles);
        s->textureFiles.configure(*s->app->textureCache, romTitle, rt->rdram_size, texture_dump_directory);
#ifdef RECOMP_DEBUG
        if (const char *path = std::getenv("RECOMP_TEXTURE_RAW_DUMP"); path && *path) {
            std::filesystem::create_directories(path);
            s->app->state->dumpingTexturesDirectory = path;
            std::fprintf(stderr, "Texture raw dump: %s\n", path);
        }
#endif
        s->input.reset(recomp_input_create());
        return s.release();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "RT64 setup: %s\n", e.what());
        if (s->ready) {
            // RT64's end() joins the queues and releases their Vulkan resources
            // before destroying the device.
            recomp_renderer_destroy(s.release());
            return nullptr;
        }
        s->app.reset();
        RT64::SetRenderHooks(nullptr, nullptr, nullptr);
        active_renderer = nullptr;
        if (s->frame_trace)
            std::fclose(s->frame_trace);
        SDL_DestroyWindow(s->window);
        SDL_Quit();
        return nullptr;
    }
}
void recomp_renderer_set_frame_rate(RecompRenderer *s, uint32_t target_fps) {
    s->check_owner();
    if (s->token || s->stats.graphics_tasks || s->stats.presentations ||
        (target_fps && (target_fps < 10 || target_fps > 1000)))
        recomp_fail("configure renderer frame rate before submitting tasks", target_fps);
    s->app->userConfig.refreshRate =
        target_fps ? RT64::UserConfiguration::RefreshRate::Manual : RT64::UserConfiguration::RefreshRate::Original;
    if (target_fps)
        s->app->userConfig.refreshRateTarget = static_cast<int>(target_fps);
    s->app->updateUserConfig(false);
    // Start presenting interpolated frames with the completed display list.
    // Waiting for the guest's buffered VI swap delays the interpolation window.
    s->app->enhancementConfig.presentation.mode =
        target_fps ? RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly
                   : RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
    s->app->updateEnhancementConfig();
}
void recomp_renderer_set_graphics_hook(RecompRenderer *s, RecompGraphicsHook hook) {
    s->check_owner();
    s->graphics_hook = hook;
}
void recomp_renderer_set_microcode_hook(RecompRenderer *s, RecompMicrocodeHook hook) {
    s->check_owner();
    s->microcode_hook = hook;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if (!s)
        return;
    s->check_owner();
    if (s->mouse_captured)
        SDL_SetRelativeMouseMode(SDL_FALSE);
    SDL_SetEventFilter(nullptr, nullptr);
    s->wait_texture_reload();
    if (s->texture_editor) {
        for (auto& scanner : s->app->textureCache->texturePackScanners)
            if (scanner)
                scanner->stop();
        s->texture_editor.reset();
    }
    s->cheat_menu.reset();
#ifdef RECOMP_DEBUG
    s->debug_overlay.reset();
#endif
    if (s->ready) {
        s->wait_workers();
        s->app->end();
    }
    if (std::getenv("RECOMP_AUDIO_TRACE"))
        std::fprintf(stderr,
                     "Renderer timing: graphics_tasks=%llu graphics_job_ms=%.3f presentations=%llu present_job_ms=%.3f "
                     "display_frames=%llu\n",
                     static_cast<unsigned long long>(s->stats.graphics_tasks), s->stats.submit_ns / 1000000.0,
                     static_cast<unsigned long long>(s->stats.presentations), s->stats.present_ns / 1000000.0,
                     static_cast<unsigned long long>(s->display_frames.load()));
#ifdef RECOMP_DEBUG
    std::fprintf(stderr, "Textures: %llu PNGs written, %llu replacements used.\n",
                 static_cast<unsigned long long>(s->textureFiles.pngsWritten),
                 static_cast<unsigned long long>(s->textureFiles.replacedCount()));
#endif
    SDL_Window *window = s->window;
    s->app.reset();
    recomp_rt64_set_anamorphic_present(false);
    RT64::SetRenderHooks(nullptr, nullptr, nullptr);
    active_renderer = nullptr;
    if (s->frame_trace && std::fclose(s->frame_trace))
        recomp_fail("cannot close renderer frame trace", 0);
    s->input.reset();
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
    delete s;
}
bool recomp_renderer_submit(RecompRenderer *s, const RecompTask *task) {
    s->check_owner();
    s->raw_vi = false;
    if (!s->ready || s->token || task->resumed || task->words[0] != 1 ||
        (task->words[1] & ~(RECOMP_TASK_DP_WAIT | RECOMP_TASK_LOADABLE | RECOMP_TASK_USER_FLAGS)) || !task->words[4] ||
        task->words[5] > 0x2000 || !task->words[6] || !task->words[7] || task->words[7] > 0x2000 || !task->words[12] ||
        task->words[12] % 8 || task->words[13] % 8)
        return false;
    s->sync_vi();
    s->code = recomp_ram_address(s->rt, task->words[4], task->words[5] ? task->words[5] : 0x1000);
    s->data = recomp_ram_address(s->rt, task->words[6], task->words[7]);
    s->list = recomp_ram_address(s->rt, task->words[12], task->words[13] ? task->words[13] : 8);
    s->token = task->token;
    s->sp_seen = s->dp_seen = false;
    trace_frame(s, "graphics_submit", s->code);
    /* The task hook parks the guest in recomp_on_host. RT64 captures geometry,
     * textures and readbacks before returning; its GPU queues stay asynchronous. */
    if (!process_graphics(s)) {
        /* A rejected display list did not touch RT64's queues. Publish empty
         * SP and DP completion so the guest can keep submitting tasks. */
        if (!recomp_task_complete(s->rt, task->token, RECOMP_SP_DONE) ||
            !recomp_task_complete(s->rt, task->token, RECOMP_DP_DONE))
            recomp_fail("cannot complete skipped graphics task", task->guest_address);
        s->token = 0;
        s->stats.graphics_tasks++;
        return true;
    }
    trace_frame(s, "graphics_receipt");
    if (!s->sp_seen || !recomp_task_complete(s->rt, task->token, RECOMP_SP_DONE))
        recomp_fail("graphics task missing SP receipt", task->guest_address);
    if (s->dp_seen) {
        if (!recomp_task_complete(s->rt, task->token, RECOMP_DP_DONE))
            recomp_fail("invalid graphics task DP receipt", task->guest_address);
    } else {
        if (!recomp_task_finish_without_dp(s->rt, task->token))
            recomp_fail("invalid graphics task split-list receipt", task->guest_address);
        s->stats.graphics_tasks_without_dp++;
    }
    s->token = 0;
    s->stats.graphics_tasks++;
    return true;
}
bool recomp_renderer_submit_rdp(RecompRenderer *s, uint32_t start, uint32_t end, const RecompViRegisters *registers,
                                uint32_t framebuffer) {
    s->check_owner();
    if (!s->ready || s->token || s->raw_rdp || !registers || end <= start || (start & 7) || (end & 7))
        return false;
    uint32_t physical = recomp_ram_address(s->rt, start, end - start);
    s->vi = *registers;
    s->vi.origin = recomp_ram_address(s->rt, s->vi.origin, 1);
    const RT64::VI decoded = s->app->core.decodeVI();
    if (decoded.visible() && s->vi.origin == recomp_ram_address(s->rt, framebuffer, 1)) {
        const uint32_t rowOffset = s->vi.origin - decoded.fbAddress();
        if (rowOffset && uint64_t(s->vi.origin) + rowOffset < s->rt->rdram_size)
            s->vi.origin += rowOffset;
    }
    s->raw_vi = s->raw_rdp = true;
    s->sp_seen = s->dp_seen = false;
    const uint64_t begin = monotonic_ns();
    const uint64_t cpu_begin = s->rt->timing ? recomp_timing_thread_clock() : 0;
    s->app->processDisplayLists(s->rt->rdram, physical, physical + (end - start), false);
    s->textureFiles.collect(*s->app->textureCache);
    uint64_t elapsed = monotonic_ns() - begin;
    s->stats.submit_ns += elapsed;
    recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS, elapsed, 1);
    if (s->rt->timing) {
        uint64_t cpu_end = recomp_timing_thread_clock();
        if (cpu_begin && cpu_end >= cpu_begin)
            recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS_CPU, cpu_end - cpu_begin, 1);
    }
    s->stats.graphics_tasks++;
    s->raw_rdp = false;
    return s->dp_seen;
}
bool recomp_renderer_present(RecompRenderer *s) {
    s->check_owner();
    s->sync_vi();
    SDL_PumpEvents();
    trace_frame(s, "host_present", s->vi.origin);
#ifdef RECOMP_DEBUG
    s->debug_overlay->update(s->rt, {s->debug_geometry, s->stats.graphics_tasks, s->stats.submit_ns});
    s->app->appWindow->sdlCheckFilterInstallation();
#endif
    process_present(s);
    return true;
}
void recomp_renderer_mouse_mode(RecompRenderer *s, bool enabled, bool gameplay) {
    s->check_owner();
    s->mouse_enabled = enabled;
    s->mouse_gameplay = gameplay;
}
void recomp_renderer_keyboard_c_buttons(RecompRenderer *s, bool enabled) {
    s->check_owner();
    s->keyboard_c_buttons = enabled;
}
void recomp_renderer_wasd_stick(RecompRenderer *s, bool enabled) {
    s->check_owner();
    s->wasd_stick = enabled;
}
void recomp_renderer_separate_mouse_actions(RecompRenderer *s, bool enabled) {
    s->check_owner();
    s->separate_mouse_actions = enabled;
}
void recomp_renderer_gamepad_camera_mode(RecompRenderer *s, bool enabled) {
    s->check_owner();
    s->gamepad_camera = enabled;
}
RecompCameraStick recomp_renderer_camera_stick(RecompRenderer *s) {
    s->check_owner();
    return s->camera_stick;
}
RecompMouseFrame recomp_renderer_mouse_frame(RecompRenderer *s) {
    s->check_owner();
    RecompMouseFrame frame = s->mouse_frame;
    s->mouse_frame.dx = s->mouse_frame.dy = 0;
    s->mouse_frame.wheel = 0;
    return frame;
}
void recomp_renderer_set_fov_hook(RecompRenderer *s, void (*adjust)(int)) {
    s->check_owner();
    s->fov_hook = adjust;
}
float recomp_renderer_projection_expansion(RecompRenderer *s, float source_aspect) {
    s->check_owner();
    using AspectRatio = RT64::UserConfiguration::AspectRatio;
    const auto& config = s->app->userConfig;
    if (config.aspectRatio == AspectRatio::Manual)
        return config.aspectTarget / source_aspect;
    if (config.aspectRatio != AspectRatio::Expand)
        return 1.0f;
    int width, height;
    SDL_GetWindowSize(s->window, &width, &height);
    return height > 0 ? std::max(1.0f, float(width) / height / source_aspect) : 1.0f;
}
static void update_native_aspect(RecompRenderer *s) {
    if (!s->aspect_hook)
        return;
    int width, height;
    SDL_GetWindowSize(s->window, &width, &height);
    float aspect = s->expanded_aspect && height > 0 ? std::max(4.0f / 3.0f, float(width) / height) : 4.0f / 3.0f;
    recomp_rt64_set_anamorphic_present(s->aspect_hook(s->aspect_opaque, aspect));
}
void recomp_renderer_set_aspect_hook(RecompRenderer *s, bool (*update)(void *, float), void *opaque) {
    s->check_owner();
    s->aspect_hook = update;
    s->aspect_opaque = opaque;
    {
        const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
        using AspectRatio = RT64::UserConfiguration::AspectRatio;
        s->app->userConfig.aspectRatio =
            update ? AspectRatio::Original : (s->expanded_aspect ? AspectRatio::Expand : AspectRatio::Original);
        s->app->userConfig.extAspectRatio = s->app->userConfig.aspectRatio;
    }
    s->app->updateUserConfig(true);
    recomp_rt64_set_anamorphic_present(false);
    update_native_aspect(s);
}
static bool run_texture_editor(RecompRenderer *s) {
    const auto start = monotonic_ns();
    recomp_ai_pause_output(s->rt, true);
    s->wait_texture_reload();
    {
        const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
        if (s->app->presentQueue->inspector)
            s->app->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
    }
    SDL_SetRelativeMouseMode(SDL_FALSE);
    s->mouse_captured = false;
    s->mouse_frame = {};
    trace_frame(s, "texture_editor_open");
    s->texture_editor->run();
    s->texture_editor_escape = SDL_GetKeyboardState(nullptr)[SDL_SCANCODE_ESCAPE];
    trace_frame(s, "texture_editor_close");
    texture_editor_paused_ns += monotonic_ns() - start;
    recomp_ai_pause_output(s->rt, false);
    return !s->texture_editor->quitRequested();
}
bool recomp_renderer_input(RecompRenderer *s, RecompPad *pad) {
    s->check_owner();
    s->mouse_frame = {};
    s->camera_stick = {};
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return false;
        if (s->mouse_enabled && event.type == SDL_KEYDOWN && !event.key.repeat) {
            if (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
                s->mouse_released = s->mouse_captured;
            if (event.key.keysym.scancode == SDL_SCANCODE_F4)
                s->mouse_released = false;
            if (event.key.keysym.scancode == SDL_SCANCODE_F10)
                return false;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            s->mouse_frame = {};
            s->mouse_released = true;
        }
        if (event.type == SDL_MOUSEMOTION && s->mouse_captured) {
            s->mouse_frame.dx += event.motion.xrel;
            s->mouse_frame.dy += event.motion.yrel;
        }
        if (event.type == SDL_MOUSEWHEEL && s->mouse_captured) {
            int64_t detents = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                detents = -detents;
            s->mouse_frame.wheel = recomp_mouse_wheel_sum(s->mouse_frame.wheel, detents);
        }
        s->cheat_menu->event(event);
        if (s->cheat_menu->open()) {
            const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
            // RT64 draws its inspector after the external render hook. Close
            // it so it cannot cover the menu or retain keyboard/mouse capture.
            if (s->app->presentQueue->inspector)
                s->app->processDeveloperShortcut(RT64::Application::DeveloperShortcut::Inspector);
        }
        if (s->cheat_menu->captures_input())
            continue;
        if (s->texture_editor && event.type == SDL_KEYDOWN && !event.key.repeat &&
            event.key.keysym.scancode == SDL_SCANCODE_F8) {
            if (!run_texture_editor(s))
                return false;
            continue;
        }
        if (event.type == SDL_KEYDOWN && s->fov_hook && (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) &&
            (event.key.keysym.scancode == SDL_SCANCODE_KP_PLUS || event.key.keysym.scancode == SDL_SCANCODE_KP_MINUS)) {
            bool keyboard_captured = false;
#ifdef RECOMP_DEBUG
            {
                const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
                keyboard_captured = s->app->presentQueue->inspector && ImGui::GetIO().WantCaptureKeyboard;
            }
#endif
            if (!keyboard_captured)
                s->fov_hook(event.key.keysym.scancode == SDL_SCANCODE_KP_PLUS ? 1 : -1);
        }
        if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F5) {
            s->wait_texture_reload();
            s->textureFiles.refreshAndToggle(*s->app->textureCache);
        }
        if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F7) {
            {
                const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
                using AspectRatio = RT64::UserConfiguration::AspectRatio;
                auto& config = s->app->userConfig;
                const bool expand = s->expanded_aspect = !s->expanded_aspect;
                config.aspectRatio = expand && !s->aspect_hook ? AspectRatio::Expand : AspectRatio::Original;
                config.extAspectRatio = config.aspectRatio;
                std::fprintf(stderr, "Aspect ratio: %s\n", expand ? "expanded" : "original");
            }
            s->app->updateUserConfig(true);
        }
#ifdef RECOMP_DEBUG
        if (event.type == SDL_KEYDOWN && !event.key.repeat && event.key.keysym.scancode == SDL_SCANCODE_F6) {
            s->wait_workers();
            if (s->app->device->getCapabilities().wireframe) {
                auto& renderer = *s->app->workloadQueue->framebufferRenderer;
                bool enabled = !renderer.wireframe.load();
                if (enabled)
                    s->app->rasterShaderCache->shaderUber->createWireframePipelines();
                renderer.wireframe = enabled;
                std::fprintf(stderr, "Render mode: %s\n", enabled ? "wireframe" : "textured");
            } else {
                std::fprintf(stderr, "Wireframe rendering is unavailable on this graphics device.\n");
            }
        }
#endif
    }
    update_native_aspect(s);
    s->cheat_menu->update();
    bool capture = s->mouse_enabled && s->mouse_gameplay && !s->mouse_released && !s->cheat_menu->captures_input() &&
                   (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS);
#ifdef RECOMP_DEBUG
    {
        const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
        if (s->app->presentQueue->inspector && (ImGui::GetIO().WantCaptureMouse || ImGui::GetIO().WantCaptureKeyboard))
            capture = false;
    }
#endif
    if (capture != s->mouse_captured) {
        if (SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE) != 0)
            capture = false;
        SDL_GetRelativeMouseState(nullptr, nullptr);
        s->mouse_frame = {};
        trace_frame(s, "mouse_capture", capture);
    }
    s->mouse_captured = capture;
    s->mouse_frame.active = capture;
    if (!capture)
        s->mouse_frame = {};
    *pad = {};
    pad->connected = true;
    if (s->cheat_menu->captures_input())
        return true;
    if (!(SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS))
        return true;
#ifdef RECOMP_DEBUG
    {
        const std::lock_guard lock(s->app->presentQueue->inspectorMutex);
        // SDL's keyboard state includes keys consumed by the ImGui event filter.
        if (s->app->presentQueue->inspector && ImGui::GetIO().WantCaptureKeyboard)
            return true;
    }
#endif
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    s->texture_editor_escape &= keys[SDL_SCANCODE_ESCAPE] != 0;
    if (!s->mouse_enabled && keys[SDL_SCANCODE_ESCAPE] && !s->texture_editor_escape)
        return false;
    if (s->keyboard_c_buttons) {
        if (keys[SDL_SCANCODE_UP])
            pad->buttons |= 0x0008;
        if (keys[SDL_SCANCODE_DOWN])
            pad->buttons |= 0x0004;
        if (keys[SDL_SCANCODE_LEFT])
            pad->buttons |= 0x0002;
        if (keys[SDL_SCANCODE_RIGHT])
            pad->buttons |= 0x0001;
    }
    recomp_input_keyboard_stick(pad, keys, s->keyboard_c_buttons, s->wasd_stick);
    if (s->mouse_enabled) {
        // Menus retain directional navigation; WASD in gameplay travels through
        // the game's adapter rather than becoming a second analog stick.
        if (keys[SDL_SCANCODE_RETURN])
            pad->buttons |= 0x8000;
        if (keys[SDL_SCANCODE_TAB])
            pad->buttons |= 0x1000;
        if (keys[SDL_SCANCODE_E])
            pad->buttons |= 0x4000;
        if (keys[SDL_SCANCODE_Q])
            pad->buttons |= 0x8000;
        if (keys[SDL_SCANCODE_R])
            pad->buttons |= 0x4000;
        if (capture) {
            Uint32 buttons = SDL_GetMouseState(nullptr, nullptr);
            if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))
                pad->buttons |= 0x2000;
            if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))
                pad->buttons |= 0x0010;
            s->mouse_frame.move_x = keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A];
            s->mouse_frame.move_y = keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S];
            if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT))
                s->mouse_frame.actions |= RECOMP_MOUSE_ATTACK;
            if (keys[SDL_SCANCODE_SPACE])
                s->mouse_frame.actions |= RECOMP_MOUSE_JUMP;
            if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT))
                s->mouse_frame.actions |= RECOMP_MOUSE_SECONDARY_ATTACK;
            if (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT])
                s->mouse_frame.actions |= RECOMP_MOUSE_SPEED;
            if (keys[SDL_SCANCODE_Q])
                s->mouse_frame.actions |= RECOMP_MOUSE_NEXT_WEAPON;
            if (keys[SDL_SCANCODE_R])
                s->mouse_frame.actions |= RECOMP_MOUSE_PREVIOUS_WEAPON;
            if (keys[SDL_SCANCODE_E])
                s->mouse_frame.actions |= RECOMP_MOUSE_USE;
            if (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL])
                s->mouse_frame.actions |= RECOMP_MOUSE_DEFEND;
            if (keys[SDL_SCANCODE_F])
                s->mouse_frame.actions |= RECOMP_MOUSE_STATUS;
            if (s->separate_mouse_actions)
                pad->buttons &= 0x1000;
        }
        recomp_input_poll(s->input.get(), pad, s->gamepad_camera ? &s->camera_stick : nullptr);
        return true;
    }
    const struct {
        SDL_Scancode key;
        uint16_t button;
    } bindings[] = {{SDL_SCANCODE_A, 0x8000}, {SDL_SCANCODE_X, 0x4000}, {SDL_SCANCODE_Z, 0x2000},
                    {SDL_SCANCODE_S, 0x1000}, {SDL_SCANCODE_T, 0x0800}, {SDL_SCANCODE_G, 0x0400},
                    {SDL_SCANCODE_F, 0x0200}, {SDL_SCANCODE_H, 0x0100}, {SDL_SCANCODE_C, 0x0020},
                    {SDL_SCANCODE_V, 0x0010}, {SDL_SCANCODE_I, 0x0008}, {SDL_SCANCODE_K, 0x0004},
                    {SDL_SCANCODE_J, 0x0002}, {SDL_SCANCODE_L, 0x0001}};
    if (s->separate_mouse_actions) {
        if (keys[SDL_SCANCODE_RETURN])
            pad->buttons |= 0x8000;
        if (keys[SDL_SCANCODE_TAB])
            pad->buttons |= 0x1000;
        if (keys[SDL_SCANCODE_E])
            pad->buttons |= 0x4000;
    }
    if (s->wasd_stick) {
        if (keys[SDL_SCANCODE_RETURN])
            pad->buttons |= 0x8000;
        if (keys[SDL_SCANCODE_TAB])
            pad->buttons |= 0x1000;
    }
    for (const auto& binding : bindings) {
        if (s->wasd_stick && (binding.key == SDL_SCANCODE_A || binding.key == SDL_SCANCODE_S))
            continue;
        if (keys[binding.key])
            pad->buttons |= binding.button;
    }
    if (!s->wasd_stick && pad->stick_x && pad->stick_y) {
        pad->stick_x = pad->stick_x * 7 / 10;
        pad->stick_y = pad->stick_y * 7 / 10;
    }
    recomp_input_poll(s->input.get(), pad, s->gamepad_camera ? &s->camera_stick : nullptr);
    return true;
}
#ifdef RECOMP_DEBUG
bool recomp_renderer_capture(RecompRenderer *s, const char *path) {
    s->check_owner();
    s->wait_workers();
#ifdef _WIN32
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(s->window, &info) || info.subsystem != SDL_SYSWM_WINDOWS)
        return false;
    HWND window = info.info.win.window;
    RECT rect{};
    if (!GetClientRect(window, &rect) || rect.right <= 0 || rect.bottom <= 0)
        return false;
    BITMAPINFO format{};
    format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    format.bmiHeader.biWidth = rect.right;
    format.bmiHeader.biHeight = -rect.bottom;
    format.bmiHeader.biPlanes = 1;
    format.bmiHeader.biBitCount = 32;
    format.bmiHeader.biCompression = BI_RGB;
    HDC source = GetDC(window);
    if (!source)
        return false;
    HDC copy = CreateCompatibleDC(source);
    void *bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(source, &format, DIB_RGB_COLORS, &bits, NULL, 0);
    bool okay = false;
    if (copy && bitmap && bits) {
        HGDIOBJ previous = SelectObject(copy, bitmap);
        // PW_RENDERFULLCONTENT asks DWM to include the Vulkan swap chain.
        // A window DC BitBlt alone can return a black hardware surface.
        if (PrintWindow(window, copy, PW_CLIENTONLY | 0x00000002)) {
            FILE *file = std::fopen(path, "wb");
            if (file) {
                std::fprintf(file, "P6\n%ld %ld\n255\n", rect.right, rect.bottom);
                const auto *pixels = static_cast<const uint8_t *>(bits);
                for (size_t i = 0; i < size_t(rect.right) * size_t(rect.bottom); i++) {
                    std::fputc(pixels[i * 4 + 2], file);
                    std::fputc(pixels[i * 4 + 1], file);
                    std::fputc(pixels[i * 4], file);
                }
                okay = !std::ferror(file);
                okay = std::fclose(file) == 0 && okay;
            }
        }
        SelectObject(copy, previous);
    }
    if (bitmap)
        DeleteObject(bitmap);
    if (copy)
        DeleteDC(copy);
    ReleaseDC(window, source);
    return okay;
#else
    SDL_SysWMinfo info{};
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(s->app->appWindow->sdlWindow, &info) || info.subsystem != SDL_SYSWM_X11)
        return false;
    XWindowAttributes attrs{};
    Display *display = info.info.x11.display;
    Window window = info.info.x11.window;
    if (!XGetWindowAttributes(display, window, &attrs))
        return false;
    XImage *shot = XGetImage(display, window, 0, 0, attrs.width, attrs.height, AllPlanes, ZPixmap);
    if (!shot)
        return false;
    FILE *file = std::fopen(path, "wb");
    if (!file) {
        XDestroyImage(shot);
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", attrs.width, attrs.height);
    auto component = [](unsigned long pixel, unsigned long mask) {
        unsigned shift = 0;
        while (mask && !(mask & 1)) {
            mask >>= 1;
            shift++;
        }
        return mask ? static_cast<int>(((pixel >> shift) & mask) * 255 / mask) : 0;
    };
    for (int y = 0; y < attrs.height; y++)
        for (int x = 0; x < attrs.width; x++) {
            unsigned long pixel = XGetPixel(shot, x, y);
            std::fputc(component(pixel, shot->red_mask), file);
            std::fputc(component(pixel, shot->green_mask), file);
            std::fputc(component(pixel, shot->blue_mask), file);
        }
    bool okay = !std::ferror(file);
    if (std::fclose(file))
        okay = false;
    XDestroyImage(shot);
    return okay;
#endif
}
#endif
RecompRendererStats recomp_renderer_stats(RecompRenderer *s) {
    s->check_owner();
    s->stats.textures_dumped = s->textureFiles.pngsWritten;
    s->stats.textures_replaced = s->textureFiles.replacedCount();
    s->stats.replacement_files = s->textureFiles.replacementFiles;
    uint64_t first = s->first_display_ns.load(), last = s->last_display_ns.load();
    s->stats.display_frames = s->display_frames.load();
    s->stats.display_elapsed_ns = last >= first ? last - first : 0;
    return s->stats;
}
