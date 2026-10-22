#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "graphics.h"
#include "pager.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define require recomp_game_require
#define monotonic_ns recomp_game_monotonic
static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t now_ns, wall_origin;
#include "goldeneye_boot_config.h"
static GoldenEyePager pager;
static RecompGameOptions options;
static bool reached_graphics;
static uint64_t live_guest_origin;
static uint32_t interrupt_mask = 0x3fff01;
#include "boot_hooks.h"
#include "services.h"
static void drain(Runtime *rt) {
    /* Retain bounded bootstrap draining. The checked idle-thread object lets
     * the scheduler return without dispatching the empty ROM loop. */
    for (unsigned batch = 0; batch < 256; batch++) {
        RecompRunResult run = recomp_scheduler_run(rt, 16);
        require(run != RECOMP_RUN_COMPLETE, "GoldenEye guest threads ended unexpectedly", BOOT_MAIN_ENTRY);
        if (run == RECOMP_RUN_WAITING && !recomp_task_stats(rt).pending_records)
            return;
    }
}
static uint64_t guest_now(void *opaque) {
    (void)opaque;
    return wall_origin ? live_guest_origin + monotonic_ns() - wall_origin : now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "unsupported GoldenEye task", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    require(ok, "GoldenEye task rejected", task->words[4]);
    if (task->words[0] == 1)
        reached_graphics = true;
    return true;
}
static void initialize_services(Runtime *rt) {
    /* SDK globals overlap compressed input until the original inflater runs. */
    RecompClockConfig clock = {
        .ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_now, .timer_sentinel = BOOT_TIMER_SENTINEL};
    require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &ge_scheduler_config) &&
                recomp_platform_init(rt, &ge_platform_config),
            "GoldenEye services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options.save};
    require(recomp_io_init(rt, &io), "GoldenEye IO init failed", 0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .microcode = RECOMP_AUDIO_ABI1_LINEAR,
                            .prebuffer_ms = 160,
                            .playback = !options.mute,
                            .task_profile = &goldeneye_audio_task_profile};
    audio = recomp_audio_create(rt, &ac);
    require(audio != NULL, "GoldenEye audio init failed", 0);
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_tasks_init(rt, &backend), "GoldenEye task init failed", 0);
}
int main(int argc, char **argv) {
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count, recomp_boot_sections,
                                recomp_section_count),
            "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    renderer = recomp_renderer_create_named(&rt, options.data, "GoldenEye 007");
    require(renderer != NULL, "GoldenEye renderer init failed", 0);
    recomp_renderer_set_graphics_hook(renderer, goldeneye_configure_graphics);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    goldeneye_pager_install(&rt, &pager, &ge_pager_config);
    rt.prepare_call = goldeneye_pager_prepare_call;
    rt.section_loaded[BOOT_GAME_SECTION] = true;
    ge_initialize_services = initialize_services;
    MipsContext boot = {.fcr31 = 0x01000800};
    boot.gpr[31] = mips_s32(0xf0000000);
    recomp_lookup(&rt, BOOT_ENTRY)(&rt, &boot);
    uint64_t next_present = monotonic_ns();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        if (wall_origin) {
            /* Live work shares the presentation budget with pacing below. A
             * 4096-resume drain here can consume several display intervals. */
            RecompRunResult run = recomp_scheduler_run(&rt, 32);
            require(run != RECOMP_RUN_COMPLETE, "GoldenEye guest threads ended unexpectedly", BOOT_MAIN_ENTRY);
        } else
            drain(&rt);
        if (wall_origin)
            now_ns = guest_now(NULL);
        else {
            now_ns += 16666667;
            if (reached_graphics) {
                live_guest_origin = now_ns;
                wall_origin = monotonic_ns();
            }
        }
        RecompPad pad;
        if (!recomp_renderer_input(renderer, &pad))
            break;
        require(recomp_input_set(&rt, 0, &pad), "GoldenEye controller update failed", 0);
        require(recomp_renderer_present(renderer), "GoldenEye presentation failed", 0);
        next_present += 16666667;
        /* VI/AI deadlines and RSP completions must wake guest work between
         * display ticks, including after asynchronous graphics submission. */
        if (wall_origin)
            recomp_game_pace(&rt, next_present);
        else
            recomp_game_sleep(next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
