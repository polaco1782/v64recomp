#ifndef RECOMP_RENDERER_H
#define RECOMP_RENDERER_H
#include "runtime.h"
#include "mouse_input.h"
#include "input.h"
#ifdef RECOMP_WITH_GLES2
#include "rsp.h"
#endif
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRenderer RecompRenderer;
#ifdef RECOMP_WITH_GLES2
/* ROM-bound task identity and its statically compiled RSP program. */
typedef struct RecompGraphicsMicrocode {
    const char *name;
    uint32_t code_size, data_size;
    const char *code_sha256, *data_sha256;
    const RecompRspProgram *rsp; /* Required checked native RSP catalog. */
} RecompGraphicsMicrocode;
void recomp_renderer_set_graphics_microcodes(RecompRenderer *, const RecompGraphicsMicrocode *, size_t count);
typedef struct RecompFramebufferSize {
    unsigned width, height;
} RecompFramebufferSize;
/* Additional native color-image extents, verified by the game's profile.
 * The default GLES2 adapter accepts 320x240. Bind before submitting tasks. */
void recomp_renderer_set_framebuffer_sizes(RecompRenderer *, const RecompFramebufferSize *, size_t count);
void recomp_gles2_configure_game(RecompRenderer *renderer);
#endif
typedef void (*RecompGraphicsHook)(void *interpreter, size_t ram_size, uint64_t *rdp_triangles);
typedef bool (*RecompMicrocodeHook)(void *interpreter, size_t ram_size, uint32_t code, uint32_t data);
/* Game-owned C microcode extensions can reuse a standard command map without
 * accessing RT64's C++ objects. Contexts are valid only during the callback.
 * Configuration must remain immutable, and it and userdata must outlive the
 * renderer binding. */
typedef struct RecompMicrocodeContext RecompMicrocodeContext;
typedef enum { RECOMP_GBI_F3D, RECOMP_GBI_F3DEX, RECOMP_GBI_F3DEX2 } RecompMicrocodeBase;
typedef void (*RecompMicrocodeCommand)(RecompMicrocodeContext *, uint32_t word0, uint32_t word1, void *userdata);
typedef struct {
    uint8_t opcode;
    RecompMicrocodeCommand command;
} RecompMicrocodeBinding;
typedef struct {
    RecompMicrocodeBase base;
    bool low_precision, no_near_clip, reject;
    const RecompMicrocodeBinding *bindings;
    size_t binding_count;
    void (*reset)(RecompMicrocodeContext *, bool from_task, void *userdata);
    void *userdata;
} RecompMicrocodeConfig;
enum { RECOMP_MICROCODE_MAX_LIGHTS = 7 };
typedef struct {
    float clip_position[3], normal[3], color[3], world_matrix[9];
} RecompMicrocodeVertex;
void recomp_renderer_bind_microcode(void *interpreter, uint32_t code, uint32_t data,
                                    const RecompMicrocodeConfig *config);
void recomp_microcode_read(RecompMicrocodeContext *, uint32_t address, bool segmented, void *bytes, size_t size);
void recomp_microcode_default(RecompMicrocodeContext *);
uint32_t recomp_microcode_data_address(RecompMicrocodeContext *);
uint32_t recomp_microcode_geometry_mode(RecompMicrocodeContext *);
unsigned recomp_microcode_light_count(RecompMicrocodeContext *);
void recomp_microcode_light(RecompMicrocodeContext *, unsigned index, float direction[3], float color[3]);
void recomp_microcode_vertex(RecompMicrocodeContext *, unsigned index, RecompMicrocodeVertex *vertex);
/* Supply a computed shade while retaining source normals for texture generation. */
void recomp_microcode_vertex_color(RecompMicrocodeContext *, unsigned index, const float color[3]);
typedef struct RecompRendererStats {
    uint64_t graphics_tasks, sp_callbacks, dp_callbacks, presentations;
    uint64_t submit_ns, present_ns;
    uint64_t textures_dumped, textures_replaced, replacement_files;
    uint64_t rdp_triangles;
    uint64_t graphics_tasks_without_dp;
    /* Swap-chain draws, including interpolated frames, and time from first to last. */
    uint64_t display_frames, display_elapsed_ns;
} RecompRendererStats;
/* Linux/X11 Vulkan adapter. Public entry points run on the host owner with
 * guest execution parked. Display-list parsing, readbacks and presentation
 * preparation use live RAM; RT64's render/present queues remain asynchronous. */
RecompRenderer *recomp_renderer_create_named(Runtime *, const char *data_path, const char *window_title);
/* Select PNG/JSON texture dumping for subsequently created renderers.
 * Call before renderer creation; NULL disables dumping (the default). */
void recomp_renderer_configure_texture_dump(const char *directory);
void recomp_renderer_enable_texture_editor(bool enabled);
/* Defaults to 60 FPS interpolation. 0 preserves the game's original rate;
 * 10..1000 enables RT64 interpolation at the requested rate.
 * Configure before submitting the first task. Guest VI/CPU timing is unchanged. */
void recomp_renderer_set_frame_rate(RecompRenderer *, uint32_t target_fps);
/* Configure a game-specific microcode extension after RT64 recognizes a task. */
void recomp_renderer_set_graphics_hook(RecompRenderer *, RecompGraphicsHook);
/* A game may bind a verified custom GBI before the normal RT64 lookup. Return
 * false to use RT64's database; true requires a configured interpreter GBI. */
void recomp_renderer_set_microcode_hook(RecompRenderer *, RecompMicrocodeHook);
#ifdef RECOMP_WITH_RT64
/* Horizontal projection expansion applied by RT64 to a native world view. */
float recomp_renderer_projection_expansion(RecompRenderer *, float source_aspect);
/* Call only for the game's checked L3DEX 1.00 task address. This validates the
 * live text/data and binds the constant-depth automap line decoder. */
bool recomp_renderer_bind_l3dex_1_00(void *interpreter, size_t ram_size, uint32_t code, uint32_t data);
#endif
void recomp_renderer_destroy(RecompRenderer *);
bool recomp_renderer_submit(RecompRenderer *, const RecompTask *);
/* Bare-metal RDP DMA, without an RSP microcode task. Registers describe the
 * actual scanout; framebuffer is its base before the VI scanline offset. */
bool recomp_renderer_submit_rdp(RecompRenderer *, uint32_t start, uint32_t end, const RecompViRegisters *,
                                uint32_t framebuffer);
bool recomp_renderer_present(RecompRenderer *);
bool recomp_renderer_input(RecompRenderer *, RecompPad *);
uint64_t recomp_renderer_paused_ns(void);
/* Opt into keypad +/- FOV changes, one degree per keydown (including repeat).
 * Callback runs on the host owner with guest execution parked. */
void recomp_renderer_set_fov_hook(RecompRenderer *, void (*adjust)(int steps));
/* Opt into a game's native anamorphic projection instead of RT64 expansion.
 * Called with the window aspect (4:3 when F7 is off) on the parked owner.
 * Return true when the native world camera supports that aspect; presentation
 * then stretches the completed native framebuffer to the window. */
void recomp_renderer_set_aspect_hook(RecompRenderer *, bool (*update)(void *, float), void *opaque);
/* Opt-in PC controls. Gameplay false releases capture and clears motion.
 * Escape toggles capture; F4 recaptures; F10 quits. Other games are unchanged. */
void recomp_renderer_mouse_mode(RecompRenderer *, bool enabled, bool gameplay);
/* A game handling captured keyboard/mouse actions itself can omit their N64
 * button aliases. Start and physical controller input still reach the pad. */
void recomp_renderer_separate_mouse_actions(RecompRenderer *, bool enabled);
/* Opt into arrow keys as C-buttons and WASD as the analog stick. This mapping
 * remains available when mouse capture ends, including dialog/instrument modes. */
void recomp_renderer_keyboard_c_buttons(RecompRenderer *, bool enabled);
/* Opt into WASD stick input without changing the arrow mapping. In pad-only
 * mode, Return and Tab replace A and S for the N64 A and Start buttons. */
void recomp_renderer_wasd_stick(RecompRenderer *, bool enabled);
/* Native right-stick camera and D-pad C-buttons, independent of mouse capture.
 * The held camera state is neutral while unfocused or a UI captures input. */
void recomp_renderer_gamepad_camera_mode(RecompRenderer *, bool enabled);
RecompCameraStick recomp_renderer_camera_stick(RecompRenderer *);
RecompMouseFrame recomp_renderer_mouse_frame(RecompRenderer *);
bool recomp_renderer_capture(RecompRenderer *, const char *ppm_path);
RecompRendererStats recomp_renderer_stats(RecompRenderer *);
#ifdef __cplusplus
}
#endif
#endif
