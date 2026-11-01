#include "renderer.h"
#include "fast3d_gles2.h"
#include "game.h"
#include "timing.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct RecompRenderer {
    Runtime *rt;
    RecompFast3DGles2 *gpu;
    RecompRendererStats stats;
    char code_hash[65], data_hash[65];
    uint32_t code_bytes, data_bytes;
    uint64_t first_display;
};
static bool authenticate(Runtime *rt, uint32_t address, uint32_t bytes, const char *expected) {
    if (!bytes || bytes > 8192 || strlen(expected) != 64)
        return false;
    recomp_ram_address(rt, address, bytes);
    uint8_t data[8192], digest[32];
    for (uint32_t i = 0; i < bytes; i++)
        data[i] = (uint8_t)recomp_load(rt, address + i, 1);
    unsigned length;
    if (!EVP_Digest(data, bytes, digest, &length, EVP_sha256(), NULL) || length != 32)
        return false;
    char hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return !strcmp(hex, expected);
}
RecompRenderer *recomp_renderer_create_named(Runtime *rt, const char *data, const char *title) {
    (void)data;
    RecompRenderer *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rt = rt;
    s->gpu = recomp_fast3d_gles2_create(rt, title);
    if (!s->gpu) {
        free(s);
        return NULL;
    }
    const char *fps = getenv("RECOMP_GLES2_FPS");
    if (fps && *fps) {
        if (strcmp(fps, "30") && strcmp(fps, "60"))
            recomp_fail("RECOMP_GLES2_FPS must be 30 or 60", 0);
        recomp_renderer_set_frame_rate(s, (uint32_t)strtoul(fps, NULL, 10));
    }
    return s;
}
void recomp_renderer_graphics_identity(RecompRenderer *s, const char *code, uint32_t code_bytes, const char *data,
                                       uint32_t data_bytes) {
    if (s->stats.graphics_tasks || !code || !data || strlen(code) != 64 || strlen(data) != 64)
        recomp_fail("invalid GLES2 graphics identity configuration", 0);
    memcpy(s->code_hash, code, 65);
    memcpy(s->data_hash, data, 65);
    s->code_bytes = code_bytes;
    s->data_bytes = data_bytes;
}
bool recomp_renderer_submit(RecompRenderer *s, const RecompTask *task) {
    uint64_t start = recomp_game_monotonic();
    uint64_t cpu_start = s->rt->timing ? recomp_timing_thread_clock() : 0;
    if (task->resumed || task->words[0] != 1 || task->words[5] != s->code_bytes || task->words[7] != s->data_bytes ||
        !authenticate(s->rt, task->words[4], s->code_bytes, s->code_hash) ||
        !authenticate(s->rt, task->words[6], s->data_bytes, s->data_hash))
        recomp_fail("GLES2 requires the game's verified original Fast3D microcode", task->words[4]);
    recomp_fast3d_gles2_draw(s->gpu, task->words[12]);
    uint64_t elapsed = recomp_game_monotonic() - start;
    s->stats.submit_ns += elapsed;
    recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS, elapsed, 1);
    if (cpu_start) {
        uint64_t cpu_end = recomp_timing_thread_clock();
        if (cpu_end >= cpu_start)
            recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS_CPU, cpu_end - cpu_start, 1);
    }
    s->stats.graphics_tasks++;
    if (!recomp_task_complete(s->rt, task->token, RECOMP_SP_DONE))
        recomp_fail("invalid GLES2 SP completion", task->guest_address);
    s->stats.sp_callbacks++;
    if (recomp_fast3d_gles2_full_sync(s->gpu)) {
        if (!recomp_task_complete(s->rt, task->token, RECOMP_DP_DONE))
            recomp_fail("invalid GLES2 DP completion", task->guest_address);
        s->stats.dp_callbacks++;
    } else {
        if (!recomp_task_finish_without_dp(s->rt, task->token))
            recomp_fail("invalid GLES2 split display list", task->guest_address);
        s->stats.graphics_tasks_without_dp++;
    }
    s->stats.rdp_triangles = recomp_fast3d_gles2_triangles(s->gpu);
    return true;
}
bool recomp_renderer_present(RecompRenderer *s) {
    uint64_t start = recomp_game_monotonic();
    RecompViState vi = recomp_vi_state(s->rt);
    if (vi.framebuffer)
        recomp_fast3d_gles2_set_framebuffer(s->gpu, recomp_ram_address(s->rt, vi.framebuffer, 1));
    if (recomp_fast3d_gles2_present(s->gpu))
        s->stats.interpolated_frames++;
    uint64_t now = recomp_game_monotonic();
    if (!s->first_display)
        s->first_display = now;
    s->stats.presentations++;
    s->stats.display_frames++;
    s->stats.display_elapsed_ns = now - s->first_display;
    s->stats.present_ns += now - start;
    recomp_timing_add(s->rt, RECOMP_TIMING_PRESENT, now - start, 1);
    recomp_timing_add(s->rt, RECOMP_TIMING_DISPLAY, 0, 1);
    return true;
}
bool recomp_renderer_input(RecompRenderer *s, RecompPad *pad) {
    return recomp_fast3d_gles2_input(s->gpu, pad);
}
bool recomp_renderer_capture(RecompRenderer *s, const char *path) {
    return recomp_fast3d_gles2_capture(s->gpu, path);
}
RecompRendererStats recomp_renderer_stats(RecompRenderer *s) {
    return s->stats;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if (s) {
        recomp_fast3d_gles2_destroy(s->gpu);
        free(s);
    }
}
void recomp_renderer_set_frame_rate(RecompRenderer *s, uint32_t fps) {
    if (s->stats.graphics_tasks)
        recomp_fail("configure GLES2 frame rate before submitting tasks", fps);
    recomp_fast3d_gles2_set_frame_rate(s->gpu, fps);
}
void recomp_renderer_set_vertex_identity(RecompRenderer *s, const RecompVertexIdentity *identity) {
    recomp_fast3d_gles2_set_vertex_identity(s->gpu, identity);
}
void recomp_renderer_set_graphics_hook(RecompRenderer *s, RecompGraphicsHook hook) {
    (void)s;
    if (hook)
        recomp_fail("RT64 graphics hook cannot be used with GLES2", 0);
}
void recomp_renderer_set_microcode_hook(RecompRenderer *s, RecompMicrocodeHook hook) {
    (void)s;
    if (hook)
        recomp_fail("RT64 microcode hook cannot be used with GLES2", 0);
}
bool recomp_renderer_submit_rdp(RecompRenderer *s, uint32_t start, uint32_t end, const RecompViRegisters *vi,
                                uint32_t framebuffer) {
    (void)s;
    (void)start;
    (void)end;
    (void)vi;
    (void)framebuffer;
    recomp_fail("raw RDP DMA is not supported by the GLES2 backend", start);
}
void recomp_renderer_mouse_mode(RecompRenderer *s, bool enabled, bool gameplay) {
    (void)s;
    (void)gameplay;
    if (enabled)
        recomp_fail("game-specific mouse capture is not implemented in GLES2", 0);
}
RecompMouseFrame recomp_renderer_mouse_frame(RecompRenderer *s) {
    (void)s;
    return (RecompMouseFrame){0};
}
