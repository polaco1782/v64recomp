#define main recomp_debug_main
#define _GNU_SOURCE
#include "game.h"
#define require recomp_game_require
#include "generated.h"
#include "nightmare_config.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "renderer.h"
#include "audio.h"

/* Set to 1 to trace registered guest calls during ROM bring-up. */
#define LOG_NIGHTMARE_CALLS (getenv("RECOMP_TRACE_CALLS") != NULL)
/* --performance enables per-second timing and queue measurements. */
#define LOG_NIGHTMARE_TIMING 0
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
static FILE *timing_log;
static uint64_t timing_origin;
static FILE *input_script;
static bool next_input;
static unsigned next_input_frame;
static RecompPad replay_pad = {.connected=true}, pending_pad = {.connected=true};
static void read_input(void) {
    unsigned buttons; int x, y;
    if (fscanf(input_script, "%u %u %d %d", &next_input_frame, &buttons, &x, &y) != 4) {
        next_input = false; return;
    }
    pending_pad.buttons = (uint16_t)buttons;
    pending_pad.stick_x = (int8_t)x;
    pending_pad.stick_y = (int8_t)y;
    next_input = true;
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) recomp_fail("Nightmare Creatures host clock failed", 0);
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static void drain(Runtime *rt) {
    for (unsigned batch=0; batch<128; batch++) {
        RecompRunResult run = recomp_scheduler_run(rt, 8);
        if (run != RECOMP_RUN_BUDGET) break;
        RecompThreadSnapshot threads[32];
        size_t count = recomp_thread_snapshots(rt, threads, 32);
        bool work = false;
        for (size_t i=0; i<count; i++)
            if (threads[i].state == RECOMP_READY && threads[i].id != BOOT_MAIN_ID)
                work = true;
        if (!work) break;
    }
}
static void pace(Runtime *rt, uint64_t deadline) {
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) return;
        uint64_t wake = deadline;
        uint64_t event = recomp_platform_next_deadline(rt);
        uint64_t timer = recomp_clock_next_deadline(rt);
        if (timer < event) event = timer;
        if (event != UINT64_MAX) {
            uint64_t ticks = recomp_count64(rt), hz = recomp_clock_frequency(rt);
            if (event <= ticks) wake = now;
            else if (event-ticks < hz) {
                uint64_t ns = ((event-ticks)*UINT64_C(1000000000)+hz-1)/hz;
                if (ns < deadline-now) wake = now+ns;
            }
        }
        /* SI and asynchronous RSP workers also need service between VIs.
         * Keep the owner's idle polling bounded to one millisecond. */
        if (wake-now > UINT64_C(1000000)) wake = now+UINT64_C(1000000);
        if (wake > now) {
            uint64_t delta = wake-now;
            struct timespec ts = {(time_t)(delta/1000000000), (long)(delta%1000000000)};
            nanosleep(&ts, NULL);
        }
        drain(rt);
    }
}

static void check_zero(Runtime *rt, uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a += 4)
        require(recomp_load(rt, a, 4) == 0, "Nightmare Creatures BSS was not cleared", a);
}
static void prepare_call(Runtime *rt, uint32_t address) {
    if (LOG_NIGHTMARE_CALLS) fprintf(stderr, "Nightmare Creatures call 0x%08x\n", address);
    if (address == BOOT_SYSTEM_ENTRY) {
        check_zero(rt, BOOT_BSS_START, BOOT_BSS_END);
        require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5,
                "Nightmare Creatures BSS clear overran", BOOT_BSS_END);
        bss_cleared = true;
    }
    if (!main_verified && rt->scheduler) {
        RecompThreadSnapshot threads[16];
        size_t count = recomp_thread_snapshots(rt, threads, 16);
        for (size_t i = 0; i < count; i++) if (threads[i].object == BOOT_MAIN_OBJECT) {
            initial_main = threads[i];
            require(initial_main.id == BOOT_MAIN_ID && initial_main.priority == BOOT_MAIN_PRIORITY
                    && initial_main.context.pc == BOOT_MAIN_ENTRY
                    && (uint32_t)initial_main.context.gpr[29] == BOOT_MAIN_STACK-16,
                    "Nightmare Creatures initial application thread ABI mismatch", BOOT_MAIN_ENTRY);
            main_verified = true;
        }
    }

}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    if (wall_origin) return monotonic_ns()-wall_origin;
    return now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt; (void)opaque;
    require(recomp_thread_state(rt, 0x803ff000) == RECOMP_DEAD,
            "Nightmare Creatures IPL thread still owns graphics storage", 0x803ff000);
    require(task->words[0] == 1 || task->words[0] == 2, "Nightmare Creatures unsupported RSP task type", task->guest_address);
    if (!reached_task) first_task = *task;
    reached_task = true;
    if (task->words[0] == 2) {
        require(recomp_audio_submit(audio, task), "Nightmare Creatures audio microcode unsupported", task->words[4]);
    } else {
        require(recomp_renderer_submit(renderer, task), "Nightmare Creatures graphics task rejected", task->words[4]);
        graphics_tasks++;
    }
    return true;
}

/* IPL does not return to the entry stub after handing control to libultra. */
#include "hooks.h"

int main(int argc, char **argv) {
    if (argc != 5) { fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT FRAMES RENDERER_DATA\n", argv[0]); return 2; }
    char *end;
    unsigned long frames = strtoul(argv[3], &end, 10);
    require(*argv[3] && !*end && frames <= UINT32_MAX, "Nightmare Creatures invalid frame count", 0);
    const char *interval = getenv("RECOMP_NIGHTMARE_CAPTURE_EVERY");
    unsigned long capture_every = interval ? strtoul(interval, &end, 10) : 0;
    require(!interval || (*interval && !*end && capture_every <= UINT32_MAX), "Nightmare Creatures invalid capture interval", 0);
    report_path = argv[2];
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                               recomp_boot_sections, recomp_section_count), "Nightmare Creatures runtime init failed", 0);
    RecompGameOptions options={.rom=argv[1]};
    uint8_t *rom=recomp_game_rom(&rt,&options,BOOT_ROM_SIZE,boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4) recomp_store(&rt, a, 0xa5a5a5a5, 4);
    for (size_t i = 0; i < sizeof(boot_parameters)/sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second=BOOT_COUNTER_HZ, .now_ns=guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail=BOOT_THREAD_TAIL,
        .run_queue=BOOT_RUN_QUEUE, .active_queue=BOOT_ACTIVE_QUEUE, .running_thread=BOOT_RUNNING_THREAD,
        .idle_thread=BOOT_MAIN_OBJECT,
        .stop_before_entry=BOOT_STARTUP ? 0 : BOOT_MAIN_ENTRY};
    RecompPlatformConfig platform = {.vi_period_ticks=781250,
        .vi_clock=BOOT_VI_CLOCK, .clock_rate=BOOT_CLOCK_RATE,
        .app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "Nightmare Creatures native services init failed", 0);
    /* The priority-15 pad thread continuously reads. A 1 ms serial transfer
     * allows priority-10 game code to run between controller reads. */
    char pak_path[4096];
    require(snprintf(pak_path, sizeof(pak_path), "%s.mpk", argv[1]) < (int)sizeof(pak_path),
            "Controller Pak save path too long", 0);
    RecompIOConfig io = {.connected_mask=1, .pak_mask=1, .pak_paths={pak_path}, .si_transfer_ticks=BOOT_COUNTER_HZ/1000};
    RecompTaskBackend backend = {.submit=submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Nightmare Creatures IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, argv[4], "Nightmare Creatures");
    require(renderer != NULL, "Nightmare Creatures renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled=true, .playback=getenv("RECOMP_AUDIO_MUTE") == NULL,
        .prebuffer_ms=160,
        .microcode=RECOMP_AUDIO_ABI1, .task_profile=&nightmare_audio_task_profile,
        .wav_path=getenv("RECOMP_NIGHTMARE_WAV")};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "Nightmare Creatures audio initialization failed", 0);
    wall_origin = monotonic_ns();
    const char *replay = getenv("RECOMP_NIGHTMARE_INPUT");
    if (replay && *replay) {
        input_script = fopen(replay, "r");
        require(input_script != NULL, "Nightmare Creatures controller replay cannot open", 0);
        read_input();
    }
    /* A synthetic host thread models IPL transfer until osStartThread. */
    recomp_thread_create(&rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(&rt, 0x803ff000);
    uint64_t next_present = monotonic_ns();
    timing_origin = next_present;
    if (LOG_NIGHTMARE_TIMING || getenv("RECOMP_NIGHTMARE_PERFORMANCE")) {
        char path[2048];
        require(snprintf(path, sizeof(path), "%s.timing.jsonl", report_path) < (int)sizeof(path),
                "Nightmare Creatures timing path too long", 0);
        timing_log = fopen(path, "w");
        require(timing_log != NULL, "Nightmare Creatures timing log cannot open", 0);
    }
    while (!frames || presentations < frames) {
        RecompPad pad = {.connected=true};
        if (!recomp_renderer_input(renderer, &pad)) { window_closed = true; break; }
        if (input_script) {
            while (next_input && next_input_frame <= presentations) {
                replay_pad = pending_pad;
                read_input();
            }
            pad = replay_pad;
        }
        require(recomp_input_set(&rt, 0, &pad), "Nightmare Creatures controller update failed", 0);
        drain(&rt);
        require(recomp_renderer_present(renderer), "Nightmare Creatures presentation failed", 0);
        presentations++;
        if (timing_log && presentations%60 == 0) {
            RecompAudioStats a = recomp_audio_stats(audio);
            RecompViState vi = recomp_vi_state(&rt);
            RecompAiState ai = recomp_ai_state(&rt);
            fprintf(timing_log, "{\"elapsed_ns\":%" PRIu64 ",\"presentations\":%u,"
                    "\"graphics_tasks\":%u,\"retraces\":%" PRIu64 ",\"vi_interval\":%u,"
                    "\"audio_tasks\":%" PRIu64 ",\"audio_frames\":%" PRIu64 ",\"audio_hz\":%u,"
                    "\"queued_bytes\":%u,\"underruns\":%" PRIu64 ",\"ai_queued\":%u}\n",
                    monotonic_ns()-timing_origin, presentations, graphics_tasks, vi.retraces,
                    vi.retrace_count, a.tasks, a.frames, a.frequency, a.queued_bytes,
                    a.queue_underruns, ai.queued);
            fflush(timing_log);
        }
        if (capture_every && presentations%capture_every == 0 && graphics_tasks) {
            char capture[2048];
            require(snprintf(capture, sizeof(capture), "%s.frame-%06u.ppm", report_path, presentations) < (int)sizeof(capture),
                    "Nightmare Creatures frame capture path too long", 0);
            require(recomp_renderer_capture(renderer, capture), "Nightmare Creatures frame capture failed", 0);
        }
        next_present += 16666667;
        uint64_t now = monotonic_ns();
        if (now > next_present && now-next_present > UINT64_C(16666667))
            next_present = now;
        pace(&rt, next_present);
    }
    require(graphics_tasks || window_closed, "Nightmare Creatures did not render a graphics task", 0);
    if (graphics_tasks) {
        char capture[2048];
        require(snprintf(capture, sizeof(capture), "%s.window.ppm", report_path) < (int)sizeof(capture),
                "Nightmare Creatures capture path too long", 0);
        require(recomp_renderer_capture(renderer, capture), "Nightmare Creatures capture failed", 0);
    }
    RecompRendererStats rs = recomp_renderer_stats(renderer);
    RecompAudioStats as = recomp_audio_stats(audio);
    RecompIOStats ios = recomp_io_stats(&rt);
    require(bss_cleared,
            "Nightmare Creatures rendered without verified startup", BOOT_MAIN_ENTRY);
    RecompThreadSnapshot threads[16];
    size_t count = recomp_thread_snapshots(&rt, threads, 16);
    const RecompThreadSnapshot *main_thread = NULL;
    for (size_t i = 0; i < count; i++) if (threads[i].object == BOOT_MAIN_OBJECT) main_thread = &threads[i];
    require(main_verified && main_thread != NULL && main_thread->id == BOOT_MAIN_ID,
            "Nightmare Creatures application thread ABI mismatch", BOOT_MAIN_ENTRY);
    FILE *f = fopen(report_path, "w");
    require(f != NULL, "cannot open Nightmare Creatures report", 0);
    int written = fprintf(f, "{\"bss_cleared\":true,\"bss_guard_verified\":true,"
            "\"main_state\":\"%s\",\"main_entry\":%u,\"main_thread\":%u,"
            "\"main_id\":%d,\"main_priority\":%d,\"main_stack\":%u,\"registered_functions\":%zu,"
            "\"first_task_type\":%u,\"first_task_ucode\":%u,\"task_words\":[",
            "rendered_startup",
            BOOT_MAIN_ENTRY, main_thread->object, main_thread->id,
            initial_main.priority, (uint32_t)initial_main.context.gpr[29], recomp_function_count, first_task.words[0], first_task.words[4]);
    for (unsigned i=0; i<16; i++) fprintf(f, "%s%u", i ? "," : "", first_task.words[i]);
    fprintf(f, "],\"graphics_tasks\":%u,\"presentations\":%u,\"sp_callbacks\":%" PRIu64
            ",\"dp_callbacks\":%" PRIu64 ",\"graphics_tasks_without_dp\":%" PRIu64 ",\"window_closed\":%s,\"audio_tasks\":%" PRIu64
            ",\"audio_frames\":%" PRIu64 ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
            ",\"controller_reads\":%" PRIu64 ",\"controller_queries\":%" PRIu64,
            graphics_tasks, presentations, rs.sp_callbacks, rs.dp_callbacks, rs.graphics_tasks_without_dp, window_closed ? "true" : "false",
            as.tasks, as.frames, as.peak, as.queue_underruns, as.queue_resets, ios.reads_completed, ios.queries_completed);
    fputs("}\n", f);
    int closed = fclose(f);
    require(written > 0 && closed == 0, "cannot write Nightmare Creatures report", 0);
    puts(BOOT_STARTUP ? "Nightmare Creatures submitted its original RSP tasks."
                     : "Nightmare Creatures native bootstrap reached its original application thread.");
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    if (input_script) fclose(input_script);
    if (timing_log) fclose(timing_log);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
