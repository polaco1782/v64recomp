#define main recomp_debug_main
#define _GNU_SOURCE
#include "game.h"
#define require recomp_game_require
#include "generated.h"
#include "duke_config.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "renderer.h"
#include "audio.h"

/* Set to 1 to trace registered guest calls during ROM bring-up. */
#define LOG_DUKE_CALLS (getenv("RECOMP_TRACE_CALLS") != NULL)
static bool bss_cleared, main_verified;
static RecompThreadSnapshot initial_main;
static const char *report_path;
static uint64_t now_ns;
static bool reached_task;
static RecompTask first_task;
static RecompRenderer *renderer;
static RecompAudio *audio;
static unsigned graphics_tasks, presentations;
static bool window_closed;
static uint64_t wall_origin;
static bool performance;
static uint64_t run_start, cpu_start, scheduler_ns, sleep_ns, frame_waits;
static FILE *performance_log;
static FILE *input_script;
static bool next_input;
static unsigned next_input_frame;
static RecompPad replay_pad = {.connected = true}, pending_pad = {.connected = true};
static void read_input(void) {
    unsigned buttons;
    int x, y;
    if (fscanf(input_script, "%u %u %d %d", &next_input_frame, &buttons, &x, &y) != 4) {
        next_input = false;
        return;
    }
    pending_pad.buttons = (uint16_t)buttons;
    pending_pad.stick_x = (int8_t)x;
    pending_pad.stick_y = (int8_t)y;
    next_input = true;
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts))
        recomp_fail("Duke Nukem 64 host clock failed", 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
static void drain(Runtime *rt) {
    uint64_t batch_start = performance ? monotonic_ns() : 0;
    for (unsigned batch = 0; batch < 128; batch++) {
        RecompRunResult run = recomp_scheduler_run(rt, 8);
        if (run != RECOMP_RUN_BUDGET) {
            /* Bootstrap against exact hardware deadlines until the original
             * two-retrace calibration finishes. Wall-clock scheduling jitter
             * here would otherwise slow every subsequent game frame. */
            if (!wall_origin && run == RECOMP_RUN_WAITING) {
                uint64_t event = recomp_platform_next_deadline(rt);
                uint64_t timer = recomp_clock_next_deadline(rt);
                if (timer < event)
                    event = timer;
                uint64_t ticks = recomp_count64(rt), hz = recomp_clock_frequency(rt);
                if (event != UINT64_MAX && event > ticks && event - ticks <= hz) {
                    now_ns += ((event - ticks) * UINT64_C(1000000000) + hz - 1) / hz;
                    continue;
                }
            }
            break;
        }
        RecompThreadSnapshot threads[32];
        size_t count = recomp_thread_snapshots(rt, threads, 32);
        bool work = false;
        for (size_t i = 0; i < count; i++) {
            if (threads[i].state != RECOMP_READY || threads[i].id == BOOT_MAIN_ID)
                continue;
            /* The main game thread spins on osGetTime between frames.
             * Once it yields there, let the host pace presentations and
             * keep servicing VI/audio deadlines instead of burning the
             * entire resume budget repeatedly reading the same clock. */
            /* The guest link register identifies this call independently of
             * diagnostic PC tracking and remains set while the loop polls. */
            if (threads[i].id == BOOT_GAME_THREAD_ID &&
                (uint32_t)threads[i].context.gpr[31] == BOOT_FRAME_POLL_CALL + 8) {
                if (performance)
                    frame_waits++;
                continue;
            }
            work = true;
        }
        if (!work)
            break;
    }
    if (performance)
        scheduler_ns += monotonic_ns() - batch_start;
}
static void pace(Runtime *rt, uint64_t deadline) {
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline)
            return;
        uint64_t wake = deadline;
        uint64_t event = recomp_platform_next_deadline(rt);
        uint64_t timer = recomp_clock_next_deadline(rt);
        if (timer < event)
            event = timer;
        if (event != UINT64_MAX) {
            uint64_t ticks = recomp_count64(rt), hz = recomp_clock_frequency(rt);
            if (event <= ticks)
                wake = now;
            else if (event - ticks < hz) {
                uint64_t ns = ((event - ticks) * UINT64_C(1000000000) + hz - 1) / hz;
                if (ns < deadline - now)
                    wake = now + ns;
            }
        }
        if (wake > now) {
            uint64_t delta = wake - now;
            struct timespec ts = {(time_t)(delta / 1000000000), (long)(delta % 1000000000)};
            uint64_t sleep_start = performance ? monotonic_ns() : 0;
            nanosleep(&ts, NULL);
            if (performance)
                sleep_ns += monotonic_ns() - sleep_start;
        }
        if (wake == deadline)
            continue;
        /* Audio DMA and VI deadlines need service between presentations.
         * Resume useful work, then sleep again when only clock polling remains. */
        drain(rt);
    }
}
static uint64_t thread_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts))
        recomp_fail("Duke Nukem 64 CPU clock failed", 0);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void check_zero(Runtime *rt, uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a += 4)
        require(recomp_load(rt, a, 4) == 0, "Duke Nukem 64 BSS was not cleared", a);
}
static void prepare_call(Runtime *rt, uint32_t address) {
    if (LOG_DUKE_CALLS)
        fprintf(stderr, "Duke Nukem 64 call 0x%08x\n", address);
    if (!wall_origin && address == BOOT_FRAME_CALIBRATION_END) {
        require(recomp_load(rt, BOOT_FRAME_VI_PERIOD, 8) == 781250, "Duke Nukem 64 retrace calibration mismatch",
                BOOT_FRAME_VI_PERIOD);
        wall_origin = monotonic_ns();
    }
    if (address == BOOT_SYSTEM_ENTRY) {
        check_zero(rt, BOOT_BSS_START, BOOT_BSS_END);
        require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5, "Duke Nukem 64 BSS clear overran", BOOT_BSS_END);
        bss_cleared = true;
    }
    if (!main_verified && rt->scheduler) {
        RecompThreadSnapshot threads[16];
        size_t count = recomp_thread_snapshots(rt, threads, 16);
        for (size_t i = 0; i < count; i++)
            if (threads[i].object == BOOT_MAIN_OBJECT) {
                initial_main = threads[i];
                require(initial_main.id == BOOT_MAIN_ID && initial_main.priority == BOOT_MAIN_PRIORITY &&
                            initial_main.context.pc == BOOT_MAIN_ENTRY &&
                            (uint32_t)initial_main.context.gpr[29] == BOOT_MAIN_STACK - 16,
                        "Duke Nukem 64 initial application thread ABI mismatch", BOOT_MAIN_ENTRY);
                main_verified = true;
            }
    }
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    if (wall_origin)
        return now_ns + monotonic_ns() - wall_origin;
    return now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "Duke Nukem 64 unsupported RSP task type", task->guest_address);
    if (!reached_task)
        first_task = *task;
    reached_task = true;
    if (task->words[0] == 2) {
        require(recomp_audio_submit(audio, task), "Duke Nukem 64 audio microcode unsupported", task->words[4]);
    } else {
        require(recomp_renderer_submit(renderer, task), "Duke Nukem 64 graphics task rejected", task->words[4]);
        graphics_tasks++;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT FRAMES RENDERER_DATA\n", argv[0]);
        return 2;
    }
    char *end;
    unsigned long frames = strtoul(argv[3], &end, 10);
    require(*argv[3] && !*end && frames <= UINT32_MAX, "Duke Nukem 64 invalid frame count", 0);
    const char *interval = getenv("RECOMP_DUKE_CAPTURE_EVERY");
    unsigned long capture_every = interval ? strtoul(interval, &end, 10) : 0;
    require(!interval || (*interval && !*end && capture_every <= UINT32_MAX), "Duke Nukem 64 invalid capture interval",
            0);
    report_path = argv[2];
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count, recomp_boot_sections,
                                recomp_section_count),
            "Duke Nukem 64 runtime init failed", 0);
    RecompGameOptions options = {.rom = argv[1]};
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4)
        recomp_store(&rt, a, 0xa5a5a5a5, 4);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       /* Original mainproc lowers itself to priority zero and loops at
         * 0x8004b530 after creating the application threads. */
                                       .idle_thread = BOOT_MAIN_OBJECT,
                                       .stop_before_entry = BOOT_STARTUP ? 0 : BOOT_MAIN_ENTRY};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                recomp_platform_init(&rt, &platform),
            "Duke Nukem 64 native services init failed", 0);
    char pak_path[4096];
    require(snprintf(pak_path, sizeof(pak_path), "%s.mpk", argv[1]) < (int)sizeof(pak_path),
            "Controller Pak save path too long", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {pak_path}};
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Duke Nukem 64 IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, argv[4], "Duke Nukem 64");
    require(renderer != NULL, "Duke Nukem 64 renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = getenv("RECOMP_AUDIO_MUTE") == NULL,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &duke_audio_task_profile,
                            .wav_path = getenv("RECOMP_DUKE_WAV")};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "Duke Nukem 64 audio initialization failed", 0);
    performance = getenv("RECOMP_DUKE_PERFORMANCE") != NULL;
    const char *replay = getenv("RECOMP_DUKE_INPUT");
    if (replay && *replay) {
        input_script = fopen(replay, "r");
        require(input_script != NULL, "Duke Nukem 64 controller replay cannot open", 0);
        read_input();
    }
    /* A synthetic host thread models IPL transfer until osStartThread. */
    recomp_thread_create(&rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(&rt, 0x803ff000);
    uint64_t next_present = monotonic_ns();
    if (performance) {
        run_start = next_present;
        cpu_start = thread_cpu_ns();
    }
    if (performance) {
        char path[2048];
        require(snprintf(path, sizeof(path), "%s.performance.jsonl", report_path) < (int)sizeof(path),
                "Duke Nukem 64 performance path too long", 0);
        performance_log = fopen(path, "w");
        require(performance_log != NULL, "Duke Nukem 64 performance log cannot open", 0);
    }
    while (!frames || presentations < frames) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad)) {
            window_closed = true;
            break;
        }
        if (input_script) {
            while (next_input && next_input_frame <= presentations) {
                replay_pad = pending_pad;
                read_input();
            }
            pad = replay_pad;
        }
        require(recomp_input_set(&rt, 0, &pad), "Duke Nukem 64 controller update failed", 0);
        drain(&rt);
        require(recomp_renderer_present(renderer), "Duke Nukem 64 presentation failed", 0);
        presentations++;
        if (performance && presentations % 60 == 0) {
            fprintf(performance_log,
                    "{\"elapsed_ns\":%" PRIu64 ",\"main_cpu_ns\":%" PRIu64
                    ",\"graphics_tasks\":%u,\"presentations\":%u,\"retraces\":%" PRIu64 ",\"frame_vi_ticks\":%" PRIu64
                    ",\"game_state\":%u}\n",
                    monotonic_ns() - run_start, thread_cpu_ns() - cpu_start, graphics_tasks, presentations,
                    recomp_vi_state(&rt).retraces, recomp_load(&rt, BOOT_FRAME_VI_PERIOD, 8),
                    (unsigned)recomp_load(&rt, BOOT_GAME_STATE, 1));
            fflush(performance_log);
        }
        if (capture_every && presentations % capture_every == 0 && graphics_tasks) {
            char capture[2048];
            require(snprintf(capture, sizeof(capture), "%s.frame-%06u.ppm", report_path, presentations) <
                        (int)sizeof(capture),
                    "Duke Nukem 64 frame capture path too long", 0);
            require(recomp_renderer_capture(renderer, capture), "Duke Nukem 64 frame capture failed", 0);
        }
        next_present += 16666667;
        uint64_t sleep_start = monotonic_ns();
        /* A slow startup/frame must not leave an unbounded backlog of
         * presentations that run without sleeping to catch an old deadline. */
        if (sleep_start > next_present && sleep_start - next_present > 16666667)
            next_present = sleep_start;
        pace(&rt, next_present);
    }
    require(graphics_tasks || window_closed, "Duke Nukem 64 did not render a graphics task", 0);
    if (graphics_tasks) {
        char capture[2048];
        require(snprintf(capture, sizeof(capture), "%s.window.ppm", report_path) < (int)sizeof(capture),
                "Duke Nukem 64 capture path too long", 0);
        require(recomp_renderer_capture(renderer, capture), "Duke Nukem 64 capture failed", 0);
    }
    RecompRendererStats rs = recomp_renderer_stats(renderer);
    RecompAudioStats as = recomp_audio_stats(audio);
    RecompIOStats ios = recomp_io_stats(&rt);
    uint64_t elapsed_ns = performance ? monotonic_ns() - run_start : 0;
    uint64_t cpu_ns = performance ? thread_cpu_ns() - cpu_start : 0;
    require(bss_cleared, "Duke Nukem 64 rendered without verified startup", BOOT_MAIN_ENTRY);
    RecompThreadSnapshot threads[16];
    size_t count = recomp_thread_snapshots(&rt, threads, 16);
    const RecompThreadSnapshot *main_thread = NULL;
    for (size_t i = 0; i < count; i++)
        if (threads[i].object == BOOT_MAIN_OBJECT)
            main_thread = &threads[i];
    require(main_verified && main_thread != NULL && main_thread->id == BOOT_MAIN_ID,
            "Duke Nukem 64 application thread ABI mismatch", BOOT_MAIN_ENTRY);
    FILE *f = fopen(report_path, "w");
    require(f != NULL, "cannot open Duke Nukem 64 report", 0);
    int written = fprintf(f,
                          "{\"bss_cleared\":true,\"bss_guard_verified\":true,"
                          "\"main_state\":\"%s\",\"main_entry\":%u,\"main_thread\":%u,"
                          "\"main_id\":%d,\"main_priority\":%d,\"main_stack\":%u,\"registered_functions\":%zu,"
                          "\"first_task_type\":%u,\"first_task_ucode\":%u,\"task_words\":[",
                          "rendered_startup", BOOT_MAIN_ENTRY, main_thread->object, main_thread->id,
                          initial_main.priority, (uint32_t)initial_main.context.gpr[29], recomp_function_count,
                          first_task.words[0], first_task.words[4]);
    for (unsigned i = 0; i < 16; i++)
        fprintf(f, "%s%u", i ? "," : "", first_task.words[i]);
    fprintf(f,
            "],\"graphics_tasks\":%u,\"presentations\":%u,\"sp_callbacks\":%" PRIu64 ",\"dp_callbacks\":%" PRIu64
            ",\"graphics_tasks_without_dp\":%" PRIu64 ",\"window_closed\":%s,\"audio_tasks\":%" PRIu64
            ",\"audio_frames\":%" PRIu64 ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
            ",\"controller_reads\":%" PRIu64 ",\"controller_queries\":%" PRIu64,
            graphics_tasks, presentations, rs.sp_callbacks, rs.dp_callbacks, rs.graphics_tasks_without_dp,
            window_closed ? "true" : "false", as.tasks, as.frames, as.peak, as.queue_underruns, as.queue_resets,
            ios.reads_completed, ios.queries_completed);
    if (performance)
        fprintf(f,
                ",\"performance\":{\"elapsed_ns\":%" PRIu64 ",\"main_cpu_ns\":%" PRIu64 ",\"scheduler_ns\":%" PRIu64
                ",\"graphics_submit_ns\":%" PRIu64 ",\"present_ns\":%" PRIu64 ",\"sleep_ns\":%" PRIu64
                ",\"frame_waits\":%" PRIu64 ",\"frame_vi_ticks\":%" PRIu64 "}",
                elapsed_ns, cpu_ns, scheduler_ns, rs.submit_ns, rs.present_ns, sleep_ns, frame_waits,
                recomp_load(&rt, BOOT_FRAME_VI_PERIOD, 8));
    fputs("}\n", f);
    int closed = fclose(f);
    require(written > 0 && closed == 0, "cannot write Duke Nukem 64 report", 0);
    puts(BOOT_STARTUP ? "Duke Nukem 64 submitted its original RSP tasks."
                      : "Duke Nukem 64 native bootstrap reached its original application thread.");
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    if (input_script)
        fclose(input_script);
    if (performance_log)
        fclose(performance_log);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
