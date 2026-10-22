#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "game_config.h"
#include "renderer.h"
#include "audio.h"
#include <stdlib.h>
#define require recomp_game_require
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
    require(!task->resumed && (task->words[0] == 1 || task->words[0] == 2), "unsupported SM64 task",
            task->guest_address);
    bool ok = task->words[0] == 1 ? recomp_renderer_submit(renderer, task) : recomp_audio_submit(audio, task);
    require(ok, "SM64 task rejected", task->words[4]);
    return true;
}
static bool waiting(Runtime *rt) {
    if (!rt->section_loaded[1] || recomp_thread_state(rt, GAME_MAIN_THREAD) != RECOMP_WAIT_RECV ||
        (uint32_t)recomp_thread_context(rt, GAME_MAIN_THREAD)->gpr[4] != GAME_GINTRMESGQUEUE)
        return false;
    return recomp_thread_state(rt, GAME_GSOUNDTHREAD) == RECOMP_WAIT_RECV &&
           recomp_thread_state(rt, GAME_GGAMELOOPTHREAD) == RECOMP_WAIT_RECV &&
           (uint32_t)recomp_thread_context(rt, GAME_GGAMELOOPTHREAD)->gpr[4] == GAME_GGAMEVBLANKQUEUE &&
           !recomp_task_stats(rt).pending_records;
}
static void drain(Runtime *rt) {
    for (unsigned n = 0; n < 512; n++) {
        require(recomp_scheduler_run(rt, 16) == RECOMP_RUN_BUDGET, "SM64 scheduler stopped", 0);
        if (waiting(rt))
            break;
    }
}
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    require(
        recomp_runtime_init(&rt, GAME_RAM_SIZE, recomp_functions, recomp_function_count, NULL, recomp_section_count),
        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, GAME_ROM_SIZE, game_sha1);
    require(recomp_boot_load(&rt, game_boot_loads, sizeof(game_boot_loads) / sizeof(game_boot_loads[0])),
            "SM64 boot load failed", 0);
    for (size_t i = 0; i < sizeof(game_parameters) / sizeof(game_parameters[0]); i++)
        recomp_store(&rt, game_parameters[i][0], game_parameters[i][1], 4);
    RecompSchedulerConfig scheduler = {.thread_tail = GAME_THREAD_TAIL,
                                       .run_queue = GAME_RUN_QUEUE,
                                       .active_queue = GAME_ACTIVE_QUEUE,
                                       .running_thread = GAME_RUNNING_THREAD,
                                       .event_table = GAME_EVENT_TABLE};
    require(recomp_scheduler_init(&rt, &scheduler), "SM64 scheduler init failed", 0);
    wall_origin = recomp_game_monotonic();
    RecompClockConfig clock = {
        .ticks_per_second = GAME_COUNTER_HZ, .now_ns = guest_time, .timer_sentinel = GAME_TIMER_SENTINEL};
    require(recomp_clock_init(&rt, &clock) && recomp_platform_init(&rt, &game_platform), "SM64 platform init failed",
            0);
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_tasks_init(&rt, &backend), "SM64 task init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options.save};
    require(recomp_io_init(&rt, &io), "SM64 save/input init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Super Mario 64");
    require(renderer != NULL, "SM64 renderer init failed", 0);
    RecompAudioConfig ac = {
        .playback = !options.mute, .output_enabled = true, .task_profile = &sm64_audio_task_profile};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "SM64 audio init failed", 0);
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
        require(recomp_input_set(&rt, 0, &pad), "SM64 controller update failed", 0);
        drain(&rt);
        require(recomp_renderer_present(renderer), "SM64 presentation failed", 0);
        next_present += 16666667;
        recomp_game_sleep(next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
