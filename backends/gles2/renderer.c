#include "renderer.h"
#include "rdp_gles2.h"
#include "game.h"
#include "timing.h"
#include "rsp.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct RecompRenderer {
    Runtime *rt;
    RecompRDPGles2 *gpu;
    RecompRendererStats stats;
    const RecompGraphicsMicrocode *microcodes;
    size_t microcode_count;
    RecompGles2InputConfig input_config;
    uint64_t first_display;
    RecompRspState rsp;
    RecompTask pending;
    bool registered;
    bool raw_vi;
};
#ifdef RECOMP_GLES2_TEXTURE_PACKS
static bool texture_editor_enabled;

void recomp_renderer_enable_texture_editor(bool enabled) {
    texture_editor_enabled = enabled;
}
#endif
uint64_t recomp_renderer_paused_ns(void) {
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    return recomp_rdp_gles2_editor_paused_ns();
#else
    return 0;
#endif
}
static bool authenticate(Runtime *rt, uint32_t address, uint32_t bytes, const char *expected) {
    if (!bytes || bytes > 8192 || strlen(expected) != 64)
        return false;
    uint32_t physical = address;
    if ((address & 0xe0000000u) == 0x80000000u || (address & 0xe0000000u) == 0xa0000000u)
        physical &= 0x1fffffffu;
    if (physical >= rt->rdram_size || bytes > rt->rdram_size - physical)
        return false;
    address = 0x80000000u | physical;
    uint8_t data[8192], digest[32];
    for (uint32_t i = 0; i < bytes; i++)
        data[i] = (uint8_t)recomp_load(rt, address + i, 1);
    if (!recomp_sha256(data, bytes, digest))
        return false;
    char hex[65];
    for (unsigned i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return !strcmp(hex, expected);
}
RecompRenderer *recomp_renderer_create_named(Runtime *rt, const char *data_path, const char *title) {
#if !defined(RECOMP_GLES2_TEXTURE_PACKS) && !defined(RECOMP_GLES2_CHEATS)
    (void)data_path;
#endif
    RecompRenderer *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rt = rt;
    s->gpu = recomp_rdp_gles2_create(rt, title);
    if (!s->gpu) {
        free(s);
        return NULL;
    }
#ifdef RECOMP_GLES2_CHEATS
    recomp_rdp_gles2_enable_cheats(s->gpu, data_path);
#endif
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    recomp_rdp_gles2_set_texture_path(s->gpu, data_path);
    const char *texture_editor = getenv("RECOMP_GLES2_TEXTURE_EDITOR");
    if (texture_editor_enabled || !texture_editor || strcmp(texture_editor, "0"))
        recomp_rdp_gles2_enable_texture_editor(s->gpu);
#endif
#ifdef RECOMP_HAS_RSP_PROGRAM
    recomp_gles2_configure_game(s);
#endif
    return s;
}
void recomp_renderer_set_graphics_microcodes(RecompRenderer *s, const RecompGraphicsMicrocode *microcodes,
                                             size_t count) {
    if (s->stats.graphics_tasks || !microcodes || !count || count > 32)
        recomp_fail("invalid GLES2 microcode profile", 0);
    for (size_t i = 0; i < count; i++) {
        const RecompGraphicsMicrocode *m = &microcodes[i];
        if (!m->name || !m->rsp || !m->code_size || m->code_size > 8192 || !m->data_size || m->data_size > 8192 ||
            !m->data_fingerprint_size || m->data_fingerprint_size > m->data_size || !m->code_sha256 ||
            !m->data_sha256 || strlen(m->code_sha256) != 64 || strlen(m->data_sha256) != 64)
            recomp_fail("invalid GLES2 microcode identity", (uint32_t)i);
    }
    s->microcodes = microcodes;
    s->microcode_count = count;
}
static const RecompGraphicsMicrocode *identify(RecompRenderer *s, uint32_t code, uint32_t data, uint32_t code_size,
                                               uint32_t data_size) {
    for (size_t i = 0; i < s->microcode_count; i++) {
        const RecompGraphicsMicrocode *m = &s->microcodes[i];
        if (code_size == m->code_size && (!data_size || data_size == m->data_size) &&
            authenticate(s->rt, code, m->code_size, m->code_sha256) &&
            authenticate(s->rt, data, m->data_fingerprint_size, m->data_sha256))
            return m;
    }
    recomp_fail("GLES2 graphics microcode is not in the game's verified profile", code);
}
static void finish_draw(Runtime *rt, void *opaque) {
    RecompRenderer *s = opaque;
    uint64_t elapsed, cpu;
    recomp_rdp_gles2_collect(s->gpu, &elapsed, &cpu);
    s->stats.submit_ns += elapsed;
    recomp_timing_add(rt, RECOMP_TIMING_GRAPHICS, elapsed, 1);
    if (cpu)
        recomp_timing_add(rt, RECOMP_TIMING_GRAPHICS_CPU, cpu, 1);
    if (recomp_rdp_gles2_full_sync(s->gpu)) {
        if (!recomp_task_complete(rt, s->pending.token, RECOMP_DP_DONE))
            recomp_fail("invalid native RDP completion", s->pending.guest_address);
        s->stats.dp_callbacks++;
    } else {
        if (!recomp_task_finish_without_dp(rt, s->pending.token))
            recomp_fail("invalid native RDP task without DP sync", s->pending.guest_address);
        s->stats.graphics_tasks_without_dp++;
    }
    s->stats.rdp_triangles = recomp_rdp_gles2_triangles(s->gpu);
    s->pending.token = 0;
}
static void render_service(Runtime *rt, void *opaque, bool wait) {
    RecompRenderer *s = opaque;
    if (s->pending.token && recomp_rdp_gles2_ready(s->gpu, wait))
        recomp_on_host(rt, finish_draw, s);
}
static void wait_draw(RecompRenderer *s) {
    if (s->pending.token)
        render_service(s->rt, s, true);
}
bool recomp_renderer_submit(RecompRenderer *s, const RecompTask *task) {
    if (task->resumed || task->words[0] != 1)
        recomp_fail("unsupported GLES2 graphics task", task->guest_address);
    /* Some SDK task builders leave ucode_size zero; the boot loader still
     * transfers the standard 4 KiB IMEM span. Authenticate that entire span. */
    uint32_t code_size = task->words[5] ? task->words[5] : 0x1000;
    const RecompGraphicsMicrocode *microcode = identify(s, task->words[4], task->words[6], code_size, task->words[7]);
    wait_draw(s);
    if (!s->registered) {
        if (!recomp_task_service_add(s->rt, render_service, s))
            recomp_fail("GLES render service initialization failed", task->guest_address);
        s->registered = true;
        recomp_rsp_init(&s->rsp, s->rt->rdram, s->rt->rdram_size, microcode->rsp);
    }
    s->rsp.program = microcode->rsp;
    s->raw_vi = false;
    /* The OS loads the task bootstrap by DMA from its physical address. */
    uint32_t words[16];
    memcpy(words, task->words, sizeof(words));
    words[2] = recomp_ram_address(s->rt, words[2], words[3]);
    s->stats.graphics_tasks++;
    if (!recomp_rsp_start(&s->rsp, words)) {
        fprintf(stderr, "RSP pc=%03x instruction=%08x after %llu instructions\n", s->rsp.instruction_pc,
                s->rsp.error_word, (unsigned long long)s->rsp.instructions);
        recomp_fail(s->rsp.error, s->rsp.error_address);
    }
    recomp_rdp_gles2_submit(s->gpu, s->rsp.rdp, s->rsp.rdp_count, false);
    s->pending = *task;
    if (!recomp_task_complete(s->rt, task->token, RECOMP_SP_DONE))
        recomp_fail("invalid native RSP completion", task->guest_address);
    s->stats.sp_callbacks++;
#ifdef RECOMP_DEBUG
    if (s->stats.graphics_tasks < 4 || !(s->stats.graphics_tasks & 255))
        fprintf(stderr, "RSP native task=%llu instructions=%llu RDP words=%zu DMA ranges=%zu\n",
                (unsigned long long)task->token, (unsigned long long)s->rsp.instructions, s->rsp.rdp_count,
                s->rsp.write_count);
#endif
    return true;
}

bool recomp_renderer_present(RecompRenderer *s) {
    wait_draw(s);
    uint64_t start = recomp_game_monotonic();
    if (!s->raw_vi) {
        RecompViState vi = recomp_vi_state(s->rt);
        RecompViRegisters registers;
        if (vi.framebuffer && recomp_vi_registers(s->rt, &registers) && (registers.status & 3) >= 2)
            recomp_rdp_gles2_set_framebuffer(s->gpu, recomp_ram_address(s->rt, vi.framebuffer, 1), registers.status);
    }
    recomp_rdp_gles2_present(s->gpu);
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
    wait_draw(s);
    return recomp_rdp_gles2_input(s->gpu, pad);
}
bool recomp_renderer_capture(RecompRenderer *s, const char *path) {
    wait_draw(s);
    return recomp_rdp_gles2_capture(s->gpu, path);
}
RecompRendererStats recomp_renderer_stats(RecompRenderer *s) {
    return s->stats;
}
void recomp_renderer_destroy(RecompRenderer *s) {
    if (s) {
        wait_draw(s);
        if (s->registered)
            recomp_task_service_remove(s->rt, s);
        recomp_rsp_destroy(&s->rsp);
        recomp_rdp_gles2_destroy(s->gpu);
        free(s);
    }
}
void recomp_renderer_set_ram_scanout(RecompRenderer *s) {
    wait_draw(s);
    recomp_rdp_gles2_set_ram_scanout(s->gpu);
}
void recomp_renderer_set_framebuffer_sizes(RecompRenderer *s, const RecompFramebufferSize *sizes, size_t count) {
    wait_draw(s);
    recomp_rdp_gles2_set_framebuffer_sizes(s->gpu, sizes, count);
}
bool recomp_renderer_submit_rdp(RecompRenderer *s, uint32_t start, uint32_t end, const RecompViRegisters *vi,
                                uint32_t framebuffer) {
    if (!vi || (start | end) & 7 || end < start || (vi->status & 3) < 2 || !vi->width)
        recomp_fail("invalid raw RDP submission", start);
    uint32_t bytes = end - start;
    start = recomp_ram_address(s->rt, start, bytes);
    end = start + bytes;
    wait_draw(s);
    size_t count = (end - start) / 4;
    uint32_t *words = count ? malloc(count * sizeof(*words)) : NULL;
    if (count && !words)
        recomp_fail("raw RDP command allocation failed", start);
    for (size_t i = 0; i < count; i++)
        words[i] = recomp_load32(s->rt, 0x80000000u | (start + (uint32_t)i * 4));
    recomp_rdp_gles2_submit(s->gpu, words, count, true);
    /* The raw DPC caller consumes FULLSYNC and the color image on return. */
    uint64_t elapsed, cpu;
    recomp_rdp_gles2_collect(s->gpu, &elapsed, &cpu);
    recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS, elapsed, 1);
    if (cpu)
        recomp_timing_add(s->rt, RECOMP_TIMING_GRAPHICS_CPU, cpu, 1);
    free(words);
    recomp_rdp_gles2_set_framebuffer(s->gpu, recomp_ram_address(s->rt, framebuffer, 1), vi->status);
    s->raw_vi = true;
    s->stats.graphics_tasks++;
    s->stats.rdp_triangles = recomp_rdp_gles2_triangles(s->gpu);
    return recomp_rdp_gles2_full_sync(s->gpu);
}
void recomp_renderer_mouse_mode(RecompRenderer *s, bool enabled, bool gameplay) {
    wait_draw(s);
    s->input_config.mouse_enabled = enabled;
    s->input_config.mouse_gameplay = gameplay;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
RecompMouseFrame recomp_renderer_mouse_frame(RecompRenderer *s) {
    wait_draw(s);
    return recomp_rdp_gles2_mouse_frame(s->gpu);
}
void recomp_renderer_separate_mouse_actions(RecompRenderer *s, bool enabled) {
    wait_draw(s);
    s->input_config.separate_mouse_actions = enabled;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
void recomp_renderer_keyboard_c_buttons(RecompRenderer *s, bool enabled) {
    wait_draw(s);
    s->input_config.keyboard_c_buttons = enabled;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
void recomp_renderer_wasd_stick(RecompRenderer *s, bool enabled) {
    wait_draw(s);
    s->input_config.wasd_stick = enabled;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
void recomp_renderer_gamepad_camera_mode(RecompRenderer *s, bool enabled) {
    wait_draw(s);
    s->input_config.gamepad_camera = enabled;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
RecompCameraStick recomp_renderer_camera_stick(RecompRenderer *s) {
    wait_draw(s);
    return recomp_rdp_gles2_camera_stick(s->gpu);
}
void recomp_renderer_set_fov_hook(RecompRenderer *s, void (*adjust)(int)) {
    wait_draw(s);
    s->input_config.fov_adjust = adjust;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
void recomp_renderer_set_aspect_hook(RecompRenderer *s, bool (*update)(void *, float), void *userdata) {
    wait_draw(s);
    s->input_config.aspect_update = update;
    s->input_config.aspect_userdata = userdata;
    recomp_rdp_gles2_set_input_config(s->gpu, &s->input_config);
}
