#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "pager.h"
#include "goldeneye_input.h"
#include "fov.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game_config.h"

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t now_ns, wall_origin;
static GoldenEyePager pager;
static RecompGameOptions options;
static bool reached_graphics;
static uint64_t live_guest_origin;
static uint32_t interrupt_mask = 0x3fff01;
/* Shared verified GoldenEye startup hooks. Include once in each game driver. */
static bool initialized;
static void (*ge_initialize_services)(Runtime *);

void recomp_ge_boot_tlb(Runtime *rt, MipsContext *ctx) {
    /* Original startup installs index 1, 4 MiB even page, invalid odd page. */
    recomp_map_pages(rt, 1, 0x7fe000, 0x70000000, 0, UINT32_MAX);
    mips_set(ctx, 2, 1);
    mips_set(ctx, 3, 31);
    mips_set(ctx, 4, 0x70000000);
    mips_set(ctx, 5, 0x70000000);
    mips_set(ctx, 6, 31);
    mips_set(ctx, 7, 1);
    mips_set(ctx, 8, 0x7fe000);
    mips_set(ctx, 10, 0x70000510);
    recomp_lookup(rt, 0x70000510)(rt, ctx);
}

void recomp_ge_enter_inflate(Runtime *rt, MipsContext *ctx) {
    /* The translated init() already copied this code to 0x70200000. Bind the
     * compiled section only after checking every relocated byte against ROM. */
    for (uint32_t i = 0; i < BOOT_INFLATE_SIZE; i += 4) {
        const uint8_t *p = rt->rom + BOOT_INFLATE_ROM_OFFSET + i;
        uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        recomp_game_require(recomp_load(rt, BOOT_INFLATE_VRAM + i, 4) == word,
                            "GoldenEye inflater relocation differs from ROM", BOOT_INFLATE_VRAM + i);
    }
    rt->section_loaded[BOOT_INFLATE_SECTION] = true;
    mips_set(ctx, 7, 0x7020141c);
    recomp_lookup(rt, 0x7020141c)(rt, ctx);
}

void recomp_ge_os_initialize(Runtime *rt, MipsContext *ctx) {
    recomp_game_require(!initialized, "GoldenEye repeated startup", ctx->pc);
    initialized = true;
    if (ge_initialize_services)
        ge_initialize_services(rt);
    recomp_os_initialize(rt, ctx);
}

void recomp_ge_prepare_context(Runtime *rt, MipsContext *ctx) {
    /* CPU Context/Wired registers select the original exception handler.
     * No CPU exceptions execute in the native runtime. The startup driver
     * installs a native resolver for the 0x7f... game page cache. */
    (void)rt;
    (void)ctx;
}

void recomp_ge_unmap_tlb(Runtime *rt, MipsContext *ctx) {
    uint32_t index = (uint32_t)ctx->gpr[4];
    recomp_game_require(index < 32, "GoldenEye invalid unmap index", index);
    rt->page_mappings[index] = (RecompPageMapping){0};
}

void recomp_ge_pi_raw_dma(Runtime *rt, MipsContext *ctx) {
    uint32_t direction = (uint32_t)ctx->gpr[4], offset = (uint32_t)ctx->gpr[5];
    uint32_t address = (uint32_t)ctx->gpr[6], bytes = (uint32_t)ctx->gpr[7];
    if (offset >= 0x10000000 && offset < 0x20000000)
        offset -= 0x10000000;
    recomp_game_require(direction == 0 && offset <= rt->rom_size && bytes <= rt->rom_size - offset,
                        "GoldenEye unsupported raw PI DMA", offset);
    recomp_copy_to_ram(rt, address, rt->rom + offset, bytes);
    mips_set(ctx, 2, 0);
}

void recomp_ge_pi_status(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    mips_set(ctx, 2, 0); /* Raw ROM copy above completes synchronously. */
}

void recomp_ge_cache(Runtime *rt, MipsContext *ctx) {
    /* SDK cache invalidation accepts entire virtual address ranges as well
     * as loaded pointers. Native RAM/backends share coherent host storage. */
    (void)rt;
    (void)ctx;
}
void recomp_ge_tlb_hi(Runtime *rt, MipsContext *ctx) {
    unsigned index = (unsigned)ctx->gpr[4];
    recomp_game_require(index < 32, "GoldenEye TLB index invalid", index);
    RecompPageMapping *m = &rt->page_mappings[index];
    mips_set(ctx, 2, mips_s32(m->installed ? m->virtual_base : 0x80000000));
}
void recomp_ge_int_mask(Runtime *rt, MipsContext *ctx) {
    uint32_t previous = interrupt_mask;
    interrupt_mask = (uint32_t)ctx->gpr[4];
    rt->interrupts_disabled = !(interrupt_mask & 1);
    mips_set(ctx, 2, mips_s32(previous));
}
void recomp_ge_dp_counters(Runtime *rt, MipsContext *ctx) {
    /* No RDP work precedes the startup graphics boundary. */
    uint32_t address = (uint32_t)ctx->gpr[4];
    for (unsigned i = 0; i < 4; i++)
        recomp_store(rt, address + i * 4, 0, 4);
}
void recomp_ge_dp_status(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    (void)ctx;
    /* The RDP command sink completes synchronously; counter reset, freeze and flush
     * requests do not leave an asynchronous hardware operation outstanding. */
}
void recomp_ge_vi_framebuffer(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, mips_s32(recomp_vi_state(rt).framebuffer));
}
void recomp_ge_vi_repeat_line(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    recomp_game_require((uint8_t)ctx->gpr[4] == 0, "GoldenEye repeated VI line requires native support", ctx->pc);
}
void recomp_ge_set_compare(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    (void)ctx;
    /* Native osSetTimer owns deadlines; CPU Compare is never scheduled. */
}
static void drain(Runtime *rt) {
    /* Retain bounded bootstrap draining. The checked idle-thread object lets
     * the scheduler return without dispatching the empty ROM loop. */
    for (unsigned batch = 0; batch < 256; batch++) {
        RecompRunResult run = recomp_scheduler_run(rt, 16);
        recomp_game_require(run != RECOMP_RUN_COMPLETE, "GoldenEye guest threads ended unexpectedly", BOOT_MAIN_ENTRY);
        if (run == RECOMP_RUN_WAITING && !recomp_task_stats(rt).pending_records)
            return;
    }
}
static uint64_t guest_now(void *opaque) {
    (void)opaque;
    return wall_origin ? live_guest_origin + recomp_game_monotonic() - wall_origin : now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported GoldenEye task", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "GoldenEye task rejected", task->words[4]);
    if (task->words[0] == 1)
        reached_graphics = true;
    return true;
}
static void initialize_services(Runtime *rt) {
    /* SDK globals overlap compressed input until the original inflater runs. */
    RecompClockConfig clock = {
        .ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_now, .timer_sentinel = BOOT_TIMER_SENTINEL};
    recomp_game_require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &ge_scheduler_config) &&
                            recomp_platform_init(rt, &ge_platform_config),
                        "GoldenEye services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options.save};
    recomp_game_require(recomp_io_init(rt, &io), "GoldenEye IO init failed", 0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .microcode = RECOMP_AUDIO_ABI1_LINEAR,
                            .prebuffer_ms = 160,
                            .playback = !options.mute,
                            .task_profile = &goldeneye_audio_task_profile};
    audio = recomp_game_audio_create(rt, &ac);
    recomp_game_require(audio != NULL, "GoldenEye audio init failed", 0);
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_tasks_init(rt, &backend), "GoldenEye task init failed", 0);
}
int main(int argc, char **argv) {
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    goldeneye_fov_init();
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    renderer = recomp_renderer_create_named(&rt, "GoldenEye 007");
    recomp_game_require(renderer != NULL, "GoldenEye renderer init failed", 0);
    recomp_renderer_set_fov_hook(renderer, goldeneye_fov_adjust);
    recomp_renderer_set_aspect_hook(renderer, goldeneye_aspect_update, &rt);
    recomp_renderer_set_framebuffer_sizes(renderer, game_graphics_framebuffers,
                                          sizeof(game_graphics_framebuffers) / sizeof(game_graphics_framebuffers[0]));
    recomp_renderer_set_graphics_microcodes(renderer, game_graphics_microcodes,
                                            sizeof(game_graphics_microcodes) / sizeof(game_graphics_microcodes[0]));
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    goldeneye_pager_install(&rt, &pager, &ge_pager_config);
    rt.prepare_call = goldeneye_pager_prepare_call;
    rt.section_loaded[BOOT_GAME_SECTION] = true;
    goldeneye_input_init(&rt);
    ge_initialize_services = initialize_services;
    MipsContext boot = {.fcr31 = 0x01000800};
    boot.gpr[31] = mips_s32(0xf0000000);
    recomp_lookup(&rt, BOOT_ENTRY)(&rt, &boot);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        if (wall_origin) {
            /* Live work shares the presentation budget with pacing below. A
             * 4096-resume drain here can consume several display intervals. */
            RecompRunResult run = recomp_scheduler_run(&rt, 32);
            recomp_game_require(run != RECOMP_RUN_COMPLETE, "GoldenEye guest threads ended unexpectedly",
                                BOOT_MAIN_ENTRY);
        } else
            drain(&rt);
        if (wall_origin)
            now_ns = guest_now(NULL);
        else {
            now_ns += 16666667;
            if (reached_graphics) {
                live_guest_origin = now_ns;
                wall_origin = recomp_game_monotonic();
            }
        }
        RecompPad pad;
        recomp_renderer_gamepad_camera_mode(renderer, goldeneye_input_gamepad_mode(&rt));
        recomp_renderer_mouse_mode(renderer, goldeneye_input_enabled(),
                                   goldeneye_input_playable(&rt) || goldeneye_input_menu(&rt));
        if (!recomp_renderer_input(renderer, &pad))
            break;
        goldeneye_input_submit(&rt, recomp_renderer_mouse_frame(renderer), recomp_renderer_camera_stick(renderer),
                               &pad);
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "GoldenEye controller update failed", 0);
        recomp_game_require(recomp_game_present(renderer), "GoldenEye presentation failed", 0);
        next_present += 16666667;
        /* VI/AI deadlines and RSP completions must wake guest work between
         * display ticks, including after asynchronous graphics submission. */
        if (wall_origin)
            recomp_game_pace(&rt, next_present);
        else
            recomp_game_sleep(next_present);
    }
    goldeneye_input_destroy();
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
