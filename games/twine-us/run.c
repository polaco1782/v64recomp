#include "run.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "twine_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#define require recomp_game_require
static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired;
#ifdef RECOMP_DEBUG
static bool bss_verified;
static FILE *input;
static unsigned input_frame;
static bool next_input;
static RecompPad replay = {.connected = true}, pending = {.connected = true};
static void read_input(void) {
    unsigned buttons;
    int x, y;
    next_input = fscanf(input, "%u %u %d %d", &input_frame, &buttons, &x, &y) == 4;
    if (next_input) {
        require(buttons <= UINT16_MAX && x >= -128 && x <= 127 && y >= -128 && y <= 127,
                "invalid TWINE replay input", input_frame);
        pending = (RecompPad){.connected = true, .buttons = (uint16_t)buttons,
                             .stick_x = (int8_t)x, .stick_y = (int8_t)y};
    }
}
#endif
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}
static void prepare_call(Runtime *rt, uint32_t address) {
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_TRACE_CALLS"))
        fprintf(stderr, "TWINE call 0x%08x\n", address);
    if (address == BOOT_SYSTEM_ENTRY) {
        for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
            require(recomp_load(rt, a, 4) == 0, "TWINE BSS clear mismatch", a);
        bss_verified = true;
    }
#endif
    /* mainproc's first call follows osStartThread's preemption of IPL. */
    if (!boot_retired && address == 0x800a2d90 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        require((uint32_t)ctx->gpr[31] == 0x800009c0, "unexpected TWINE IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
#ifdef RECOMP_DEBUG
    const char *break_task = getenv("RECOMP_TWINE_BREAK_TASK");
    if (break_task && strtoul(break_task, NULL, 0) == task->words[0])
        recomp_fail("TWINE requested RSP task inspection", task->guest_address);
#endif
    require(task->words[0] == 1 || task->words[0] == 2, "unsupported TWINE RSP task type", task->guest_address);
    bool okay = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    require(okay, "TWINE RSP task rejected", task->words[4]);
    return true;
}
int twine_run(const RecompGameOptions *options, const char *report) {
#ifndef RECOMP_DEBUG
    (void)report;
#endif
    Runtime rt;
    boot_retired = false;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                recomp_boot_sections, recomp_section_count), "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
        .run_queue = BOOT_RUN_QUEUE, .active_queue = BOOT_ACTIVE_QUEUE,
        .running_thread = BOOT_RUNNING_THREAD, .event_table = BOOT_EVENT_TABLE,
        .idle_thread = BOOT_MAIN_OBJECT};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250, .vi_clock = BOOT_VI_CLOCK,
        .clock_rate = BOOT_CLOCK_RATE, .final_rom = BOOT_FINAL_ROM, .app_nmi_buffer = 0x8000031c};
    wall_origin = recomp_game_monotonic();
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
            recomp_platform_init(&rt, &platform), "TWINE services init failed", 0);
    char pak_path[4096];
    require(snprintf(pak_path, sizeof(pak_path), "%s.pak", options->save) < (int)sizeof(pak_path),
            "TWINE Controller Pak path too long", 0);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options->save,
                        .pak_mask = 1, .pak_paths = {pak_path}};
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "TWINE IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options->data, "007: The World Is Not Enough");
    require(renderer != NULL, "renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled = true, .playback = !options->mute,
        .prebuffer_ms = 160, .microcode = RECOMP_AUDIO_MUSYX_V1, .task_profile = &twine_audio_profile};
#ifdef RECOMP_DEBUG
    ac.wav_path = getenv("RECOMP_TWINE_WAV");
#endif
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "audio initialization failed", 0);
#ifdef RECOMP_DEBUG
    const char *replay_path = getenv("RECOMP_TWINE_INPUT");
    if (replay_path) {
        input = fopen(replay_path, "r");
        require(input != NULL, "cannot open TWINE replay", 0);
        read_input();
    }
    const char *capture = getenv("RECOMP_TWINE_CAPTURE_EVERY");
    unsigned capture_every = capture ? (unsigned)strtoul(capture, NULL, 10) : 0;
#endif
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t deadline = recomp_game_monotonic();
    uint32_t frame = 0;
    for (; !options->frames || frame < options->frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad)) break;
#ifdef RECOMP_DEBUG
        if (input) {
            while (next_input && input_frame <= frame) { replay = pending; read_input(); }
            pad = replay;
        }
#endif
        require(recomp_input_set(&rt, 0, &pad), "controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++)
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET) break;
        require(recomp_renderer_present(renderer), "presentation failed", 0);
#ifdef RECOMP_DEBUG
        if (capture_every && report && (frame + 1) % capture_every == 0 &&
            recomp_renderer_stats(renderer).graphics_tasks) {
            char path[4096];
            require(snprintf(path, sizeof(path), "%s.frame-%06u.ppm", report, frame + 1) < (int)sizeof(path),
                    "TWINE capture path too long", 0);
            require(recomp_renderer_capture(renderer, path), "TWINE capture failed", 0);
        }
#endif
        deadline += 16666667;
        recomp_game_pace(&rt, deadline);
    }
#ifdef RECOMP_DEBUG
    if (report) {
        FILE *file = fopen(report, "w");
        require(file != NULL, "cannot open TWINE report", 0);
        RecompRendererStats graphics = recomp_renderer_stats(renderer);
        RecompAudioStats sound = recomp_audio_stats(audio);
        RecompIOStats io_stats = recomp_io_stats(&rt);
        RecompThreadSnapshot threads[32];
        size_t count = recomp_thread_snapshots(&rt, threads, 32);
        fprintf(file, "{\"presentations\":%u,\"bss_verified\":%s,\"boot_retired\":%s,"
                "\"graphics_tasks\":%" PRIu64 ",\"audio_tasks\":%" PRIu64 ",\"audio_frames\":%" PRIu64
                ",\"audio_peak\":%u,\"controller_reads\":%" PRIu64 ",\"threads\":[",
                frame, bss_verified ? "true" : "false", boot_retired ? "true" : "false",
                graphics.graphics_tasks, sound.tasks, sound.frames, sound.peak, io_stats.reads_completed);
        for (size_t i = 0; i < count && i < 32; i++)
            fprintf(file, "%s{\"id\":%d,\"state\":%d,\"pc\":%u,\"ra\":%u,\"queue\":%u}",
                    i ? "," : "", threads[i].id, threads[i].state, threads[i].context.pc,
                    (uint32_t)threads[i].context.gpr[31], (uint32_t)recomp_load(&rt, threads[i].object + 8, 4));
        fputs("]}\n", file);
        require(fclose(file) == 0, "cannot write TWINE report", 0);
    }
    if (input) { fclose(input); input = NULL; }
#endif
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
