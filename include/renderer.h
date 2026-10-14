#ifndef RECOMP_RENDERER_H
#define RECOMP_RENDERER_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRenderer RecompRenderer;
typedef void (*RecompGraphicsHook)(void *interpreter, size_t ram_size, uint64_t *rdp_triangles);
typedef struct RecompRendererStats {
    uint64_t graphics_tasks, sp_callbacks, dp_callbacks, presentations;
    uint64_t submit_ns, present_ns;
    uint64_t textures_dumped, textures_replaced, replacement_files;
    uint64_t rdp_triangles;
    uint64_t graphics_tasks_without_dp;
} RecompRendererStats;
/* Linux/X11 Vulkan adapter. All entry points and callbacks run on the owner thread. */
RecompRenderer *recomp_renderer_create_named(Runtime *, const char *data_path, const char *window_title);
/* Configure a game-specific microcode extension after RT64 recognizes a task. */
void recomp_renderer_set_graphics_hook(RecompRenderer *, RecompGraphicsHook);
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
