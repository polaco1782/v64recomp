#ifndef RECOMP_RENDERER_H
#define RECOMP_RENDERER_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRenderer RecompRenderer;
typedef void (*RecompGraphicsHook)(void *interpreter, size_t ram_size, uint64_t *rdp_triangles);
typedef bool (*RecompMicrocodeHook)(void *interpreter, size_t ram_size, uint32_t code, uint32_t data);
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
/* Defaults to 60 FPS interpolation. 0 preserves the game's original rate;
 * 10..1000 enables RT64 interpolation at the requested rate.
 * Configure before submitting the first task. Guest VI/CPU timing is unchanged. */
void recomp_renderer_set_frame_rate(RecompRenderer *, uint32_t target_fps);
/* Configure a game-specific microcode extension after RT64 recognizes a task. */
void recomp_renderer_set_graphics_hook(RecompRenderer *, RecompGraphicsHook);
/* A game may bind a verified custom GBI before the normal RT64 lookup. Return
 * false to use RT64's database; true requires a configured interpreter GBI. */
void recomp_renderer_set_microcode_hook(RecompRenderer *, RecompMicrocodeHook);
void recomp_renderer_destroy(RecompRenderer *);
bool recomp_renderer_submit(RecompRenderer *, const RecompTask *);
bool recomp_renderer_present(RecompRenderer *);
bool recomp_renderer_input(RecompRenderer *, RecompPad *);
bool recomp_renderer_capture(RecompRenderer *, const char *ppm_path);
RecompRendererStats recomp_renderer_stats(RecompRenderer *);
#ifdef __cplusplus
}
#endif
#endif
