#define _POSIX_C_SOURCE 200809L
#include "run.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "rayman2_config.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define require recomp_game_require
static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired;
#ifdef RECOMP_DEBUG
static bool bss_verified;
static uint64_t graphics_tasks;
static FILE *input;
static bool next_input;
static unsigned input_frame;
static RecompPad replay = {.connected = true}, pending = {.connected = true};
static void read_input(void) {
    unsigned buttons;
    int x, y;
    next_input = fscanf(input, "%u %u %d %d", &input_frame, &buttons, &x, &y) == 4;
    if (next_input) {
        require(buttons <= UINT16_MAX && x >= -128 && x <= 127 && y >= -128 && y <= 127, "invalid controller replay",
                input_frame);
        pending =
            (RecompPad){.connected = true, .buttons = (uint16_t)buttons, .stick_x = (int8_t)x, .stick_y = (int8_t)y};
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
        fprintf(stderr, "Rayman 2 call 0x%08x\n", address);
    if (address == BOOT_SYSTEM_ENTRY) {
        for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
            require(recomp_load(rt, a, 4) == 0, "Rayman 2 BSS clear mismatch", a);
        require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5, "Rayman 2 BSS overrun", BOOT_BSS_END);
        bss_verified = true;
    }
#endif
    /* Retire IPL storage on mainproc's first SDK call,
     * before any game allocation can reuse the synthetic OSThread storage. */
    if (!boot_retired && address == 0x80002690 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        require((uint32_t)ctx->gpr[31] == 0x800004d8, "unexpected IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
#ifdef RECOMP_DEBUG
    static bool seen[3];
    if (!seen[task->words[0]]) {
        fprintf(stderr, "Rayman 2 first task type %u:", task->words[0]);
        for (unsigned i = 0; i < 16; i++)
            fprintf(stderr, " %08x", task->words[i]);
        fputc('\n', stderr);
        seen[task->words[0]] = true;
    }
    if (task->words[0] == 1)
        graphics_tasks++;
#endif
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    require(ok, "Rayman 2 RSP task rejected", task->words[4]);
    return true;
}
static void drain(Runtime *rt) {
    for (unsigned batch = 0; batch < 32; batch++) {
        if (recomp_scheduler_run(rt, 32) != RECOMP_RUN_BUDGET)
            break;
    }
}
int rayman2_run(const RecompGameOptions *options, const char *report) {
#ifndef RECOMP_DEBUG
    (void)report;
#endif
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count, recomp_boot_sections,
                                recomp_section_count),
            "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
#ifdef RECOMP_DEBUG
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4)
        recomp_store(&rt, a, 0xa5a5a5a5, 4);
    const char *replay_path = getenv("RECOMP_RAYMAN2_INPUT");
    if (replay_path) {
        input = fopen(replay_path, "r");
        require(input != NULL, "cannot open replay", 0);
        read_input();
    }
    const char *capture = getenv("RECOMP_RAYMAN2_CAPTURE_EVERY");
    unsigned capture_every = capture ? (unsigned)strtoul(capture, NULL, 10) : 0;
#endif
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    static const RecompBootLoad loads[] = {
        {.section = 1, .vram = 0x80025c50, .rom_offset = 0x1dcc0, .size = BOOT_MAIN_TEXT_SIZE},
        {.section = 2, .vram = 0x800f64a0, .rom_offset = 0xc5bf0, .size = 0xae30},
    };
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c,
                                     .code_loads = loads,
                                     .code_load_count = 2};
    wall_origin = recomp_game_monotonic();
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                recomp_platform_init(&rt, &platform),
            "Rayman 2 services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options->save}};
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Rayman 2 IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options->data, "Rayman 2: The Great Escape");
    require(renderer != NULL, "renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options->mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &rayman2_audio_profile};
#ifdef RECOMP_DEBUG
    ac.wav_path = getenv("RECOMP_RAYMAN2_WAV");
#endif
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t next_present = recomp_game_monotonic();
    uint32_t frame = 0;
    for (; !options->frames || frame < options->frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
#ifdef RECOMP_DEBUG
        if (input) {
            while (next_input && input_frame <= frame) {
                replay = pending;
                read_input();
            }
            pad = replay;
        }
#endif
        require(recomp_input_set(&rt, 0, &pad), "controller update failed", 0);
        drain(&rt);
        require(recomp_renderer_present(renderer), "presentation failed", 0);
#ifdef RECOMP_DEBUG
        if (capture_every && (frame + 1) % capture_every == 0 && graphics_tasks && report) {
            char path[4096];
            require(snprintf(path, sizeof(path), "%s.frame-%06u.ppm", report, frame + 1) < (int)sizeof(path),
                    "capture path too long", 0);
            require(recomp_renderer_capture(renderer, path), "capture failed", 0);
        }
#endif
        next_present += 16666667;
        recomp_game_pace(&rt, next_present);
    }
#ifdef RECOMP_DEBUG
    RecompAudioStats as = recomp_audio_stats(audio);
    RecompIOStats ios = recomp_io_stats(&rt);
    if (report) {
        FILE *f = fopen(report, "w");
        require(f != NULL, "cannot open diagnostic report", 0);
        fprintf(f,
                "{\"bss_verified\":%s,\"boot_retired\":%s,\"presentations\":%u,\"graphics_tasks\":%" PRIu64
                ",\"audio_tasks\":%" PRIu64 ",\"audio_frames\":%" PRIu64
                ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
                ",\"controller_reads\":%" PRIu64 "}\n",
                bss_verified ? "true" : "false", boot_retired ? "true" : "false", frame, graphics_tasks, as.tasks,
                as.frames, as.peak, as.queue_underruns, as.queue_resets, ios.reads_completed);
        require(fclose(f) == 0, "cannot write diagnostic report", 0);
    }
    if (input)
        fclose(input);
#endif
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
