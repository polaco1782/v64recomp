#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "game_config.h"
#include "renderer.h"
#include "audio.h"
#include "graphics.h"
#include <stdlib.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(!task->resumed && (task->words[0] == 1 || task->words[0] == 2), "unsupported SM64 task",
                        task->guest_address);
    bool ok = task->words[0] == 1 ? recomp_renderer_submit(renderer, task) : recomp_audio_submit(audio, task);
    recomp_game_require(ok, "SM64 task rejected", task->words[4]);
    return true;
}
static void drain(Runtime *rt) {
    for (unsigned n = 0; n < 512; n++) {
        RecompRunResult run = recomp_scheduler_run(rt, 16);
        recomp_game_require(run != RECOMP_RUN_COMPLETE, "SM64 scheduler stopped", 0);
        if (run == RECOMP_RUN_WAITING)
            break;
    }
}
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    recomp_game_require(
        recomp_runtime_init(&rt, GAME_RAM_SIZE, recomp_functions, recomp_function_count, NULL, recomp_section_count),
        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, GAME_ROM_SIZE, game_sha1);
    recomp_game_require(recomp_boot_load(&rt, game_boot_loads, sizeof(game_boot_loads) / sizeof(game_boot_loads[0])),
                        "SM64 boot load failed", 0);
    for (size_t i = 0; i < sizeof(game_parameters) / sizeof(game_parameters[0]); i++)
        recomp_store(&rt, game_parameters[i][0], game_parameters[i][1], 4);
    RecompSchedulerConfig scheduler = {.thread_tail = GAME_THREAD_TAIL,
                                       .run_queue = GAME_RUN_QUEUE,
                                       .active_queue = GAME_ACTIVE_QUEUE,
                                       .running_thread = GAME_RUNNING_THREAD,
                                       .event_table = GAME_EVENT_TABLE,
                                       .idle_thread = GAME_IDLE_THREAD};
    recomp_game_require(recomp_scheduler_init(&rt, &scheduler), "SM64 scheduler init failed", 0);
    wall_origin = recomp_game_monotonic();
    RecompClockConfig clock = {
        .ticks_per_second = GAME_COUNTER_HZ, .now_ns = guest_time, .timer_sentinel = GAME_TIMER_SENTINEL};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_platform_init(&rt, &game_platform),
                        "SM64 platform init failed", 0);
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_tasks_init(&rt, &backend), "SM64 task init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options.save};
    recomp_game_require(recomp_io_init(&rt, &io), "SM64 save/input init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Super Mario 64");
    recomp_game_require(renderer != NULL, "SM64 renderer init failed", 0);
    sm64_graphics_bind(renderer);
    RecompAudioConfig ac = {
        .playback = !options.mute, .output_enabled = true, .task_profile = &sm64_audio_task_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "SM64 audio init failed", 0);
    /* Renderer setup is host work; start the guest wall clock afterwards. */
    wall_origin = recomp_game_monotonic();
    MipsContext ctx = {0};
    ctx.gpr[31] = mips_s32(0xf0000000);
    recomp_lookup(&rt, GAME_ENTRY_POINT)(&rt, &ctx);
    drain(&rt);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad;
        if (!recomp_renderer_input(renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "SM64 controller update failed", 0);
        drain(&rt);
        recomp_game_require(recomp_game_present(renderer), "SM64 presentation failed", 0);
        next_present += 16666667;
        /* Service VI/AI/timers and task receipts between display ticks so a
         * completed frame is not held until the following 60 Hz host tick. */
        recomp_game_pace(&rt, next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
