#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "game_config.h"
#include "quake2_input.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired;
static bool running;
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}
static void prepare_call(Runtime *rt, uint32_t address) {
    /* Retire IPL storage on mainproc's first SDK call,
     * before any game allocation can reuse the synthetic OSThread storage. */
    if (!boot_retired && address == 0x8005a270 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        recomp_game_require((uint32_t)ctx->gpr[31] == 0x800004d4, "unexpected IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "Quake II RSP task rejected", task->words[4]);
    return true;
}
static bool poll_input(Runtime *rt, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, quake2_input_enabled(), quake2_input_playable(rt));
    if (!recomp_renderer_input(renderer, pad)) {
        running = false;
        quake2_input_submit((RecompMouseFrame){0});
        return false;
    }
    quake2_input_submit(recomp_renderer_mouse_frame(renderer));
    return true;
}
static void poll_command_input(Runtime *rt, void *opaque) {
    (void)opaque;
    if (!running)
        return;
    RecompPad pad = {.connected = true};
    if (poll_input(rt, &pad))
        recomp_game_require(recomp_game_input(rt, 0, &pad), "controller update failed", 0);
}
static void drain(Runtime *rt) {
    for (unsigned batch = 0; running && batch < 32; batch++) {
        if (recomp_scheduler_run(rt, 32) != RECOMP_RUN_BUDGET)
            break;
    }
}
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .idle_thread = BOOT_IDLE_THREAD};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c};
    wall_origin = recomp_game_monotonic();
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Quake II services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options.save}};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Quake II IO/task init failed",
                        0);
    renderer = recomp_renderer_create_named(&rt, "Quake II");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    recomp_renderer_set_graphics_microcodes(renderer, game_graphics_microcodes,
                                            sizeof(game_graphics_microcodes) / sizeof(game_graphics_microcodes[0]));
    running = true;
    quake2_input_init(poll_command_input);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options.mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_NAUDIO,
                            .task_profile = &quake2_audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t next_present = recomp_game_monotonic();
    uint32_t frame = 0;
    for (; running && (!options.frames || frame < options.frames); frame++) {
        RecompPad pad = {.connected = true};
        if (!poll_input(&rt, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        drain(&rt);
        if (!running)
            break;
        recomp_game_require(recomp_game_present(renderer), "presentation failed", 0);
        next_present += 16666667;
        recomp_game_pace(&rt, next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
