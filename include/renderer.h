#ifndef RECOMP_RENDERER_H
#define RECOMP_RENDERER_H
#include "runtime.h"
#include "mouse_input.h"
#include "input.h"
#include "rsp.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRenderer RecompRenderer;
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
typedef struct RecompRendererStats {
    uint64_t graphics_tasks, sp_callbacks, dp_callbacks, presentations;
    uint64_t submit_ns, present_ns;
    uint64_t rdp_triangles;
    uint64_t graphics_tasks_without_dp;
    /* Swap-chain draws and time from first to last. */
    uint64_t display_frames, display_elapsed_ns;
} RecompRendererStats;
/* GLES2 entry points run on the creating host thread. The RSP worker owns
 * private task state; drawing, readback and task receipts stay on the owner. */
RecompRenderer *recomp_renderer_create_named(Runtime *, const char *window_title);
void recomp_renderer_destroy(RecompRenderer *);
bool recomp_renderer_submit(RecompRenderer *, const RecompTask *);
/* Bare-metal RDP DMA, without an RSP microcode task. Start/end accept physical
 * RDRAM addresses or checked KSEG aliases. Registers describe the
 * actual scanout; framebuffer is its base before the VI scanline offset. */
bool recomp_renderer_submit_rdp(RecompRenderer *, uint32_t start, uint32_t end, const RecompViRegisters *,
                                uint32_t framebuffer);
bool recomp_renderer_present(RecompRenderer *);
bool recomp_renderer_input(RecompRenderer *, RecompPad *);
/* Opt into keypad +/- FOV changes, one degree per keydown (including repeat).
 * Callback runs on the host owner with guest execution parked. */
void recomp_renderer_set_fov_hook(RecompRenderer *, void (*adjust)(int steps));
/* Opt into a game's native anamorphic projection.
 * Called with the window aspect (4:3 when F7 is off) on the parked owner.
 * Return true when the native world camera supports that aspect; presentation
 * then stretches the completed native framebuffer to the window. */
void recomp_renderer_set_aspect_hook(RecompRenderer *, bool (*update)(void *, float), void *opaque);
/* Opt-in PC controls. Gameplay false releases capture and clears motion.
 * Escape toggles capture; F4 recaptures; F10 quits. Other games are unchanged. */
void recomp_renderer_mouse_mode(RecompRenderer *, bool enabled, bool gameplay);
/* Opt into arrow keys as C-buttons and WASD as the analog stick. This mapping
 * remains available when mouse capture ends, including dialog/instrument modes. */
void recomp_renderer_keyboard_c_buttons(RecompRenderer *, bool enabled);
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
