#define main recomp_debug_main
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#define _POSIX_C_SOURCE 200809L
#include "generated.h"
#include "game.h"
#include "game_config.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define AUDIO_SYNTHESIZED "true"
#include "recomp_audio.h"
#include "audio.h"
static RecompAudio *native_audio;
static bool audio_playback;
#include "renderer.h"
#include <time.h>
static RecompRenderer *renderer;
static unsigned rendered_tasks, presentations;
#define CHECK(x) recomp_game_require((x), "SM64 check failed: " #x, 0)
static uint64_t ns;
static uint64_t scheduler_ns, wall_origin, clock_origin;
static uint64_t playback_origin;
static bool live_clock;
static bool live_startup, capture_output;
static uint64_t monotonic_ns(void) {
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void pace(uint64_t deadline) {
    uint64_t current = monotonic_ns();
    if (deadline > current) {
        uint64_t delta = deadline - current;
        struct timespec t = {(time_t)(delta / 1000000000), (long)(delta % 1000000000)};
        nanosleep(&t, NULL);
    }
}
static unsigned graphics, audio, face_frames, menu_frames, world_frames, play_frames, goal;
static bool menu_started, world_started, ready;
static const char *output, *mode;
static FILE *trace;
static RecompPad input;
static float start_pos[3], last_pos[3], max_height, min_height;
static uint64_t now_ns(void *unused) {
    (void)unused;
    return live_clock ? clock_origin + monotonic_ns() - wall_origin : ns;
}
static float guest_float(Runtime *rt, uint32_t a) {
    uint32_t bits = (uint32_t)recomp_load(rt, a, 4);
    float f;
    memcpy(&f, &bits, 4);
    CHECK(isfinite(f));
    return f;
}
#include "validation_input.h"
static FILE *artifact(const char *name, const char *access) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", output, name);
    CHECK(n > 0 && (size_t)n < sizeof(path));
    FILE *f = fopen(path, access);
    CHECK(f);
    return f;
}
static void capture_renderer(void) {
    RecompRendererStats rs = recomp_renderer_stats(renderer);
    CHECK(rs.graphics_tasks == rendered_tasks && rs.sp_callbacks == rendered_tasks &&
          rs.dp_callbacks == rendered_tasks && rs.presentations == presentations);
    char path[4096];
    CHECK(snprintf(path, sizeof(path), "%s/window.ppm", output) > 0);
    CHECK(recomp_renderer_capture(renderer, path));
    FILE *f = artifact("renderer_results.json", "w");
    fprintf(f, "{\"display_frames\":%" PRIu64 ",\"display_elapsed_ns\":%" PRIu64 ",", rs.display_frames,
            rs.display_elapsed_ns);
    fprintf(f,
            "\"graphics_tasks\":%u,\"sp_callbacks\":%" PRIu64 ",\"dp_callbacks\":%" PRIu64
            ",\"presentations\":%u,\"startup_tasks_captured\":%u,\"audio_synthesized\":%s,\"submit_ns\":%" PRIu64
            ",\"present_ns\":%" PRIu64 ",\"textures_dumped\":%" PRIu64 ",\"textures_replaced\":%" PRIu64
            ",\"replacement_files\":%" PRIu64 "}\n",
            rendered_tasks, rs.sp_callbacks, rs.dp_callbacks, presentations, graphics - rendered_tasks,
            AUDIO_SYNTHESIZED, rs.submit_ns, rs.present_ns, rs.textures_dumped, rs.textures_replaced,
            rs.replacement_files);
    CHECK(!fclose(f));
}
static void finish_audio(void) {
    RecompAudioStats as = recomp_audio_stats(native_audio);
    FILE *f = artifact("audio_results.json", "w");
    fprintf(f,
            "{\"tasks\":%" PRIu64 ",\"commands\":%" PRIu64 ",\"ai_buffers\":%" PRIu64 ",\"frames\":%" PRIu64
            ",\"nonzero_samples\":%" PRIu64 ",\"clipped_samples\":%" PRIu64
            ",\"frequency\":%u,\"peak\":%u,\"synth_ns\":%" PRIu64 ",\"playback_frames\":%" PRIu64
            ",\"queue_resets\":%" PRIu64 ",\"queued_bytes\":%u,\"device_open\":%s,\"driver\":\"%s\",\"opcodes\":[",
            as.tasks, as.commands, as.ai_buffers, as.frames, as.nonzero_samples, as.clipped_samples, as.frequency,
            as.peak, as.synth_ns, as.playback_frames, as.queue_resets, as.queued_bytes,
            as.device_open ? "true" : "false", as.driver);
    for (unsigned i = 0; i < 16; i++) {
        fprintf(f, "%s%" PRIu64, i ? "," : "", as.opcodes[i]);
    }
    fprintf(f,
            "],\"queue_underruns\":%" PRIu64
            ",\"min_queued_bytes\":%u,\"max_queued_bytes\":%u,\"prebuffer_frames\":%u}\n",
            as.queue_underruns, as.min_queued_bytes, as.max_queued_bytes, as.prebuffer_frames);
    CHECK(!fclose(f));
    recomp_audio_destroy(native_audio);
    native_audio = NULL;
    printf("Audio: %" PRIu64 " PCM frames at %u Hz; peak %u; SDL playback %s; %" PRIu64 " underruns, %" PRIu64
           " queue resets.\n",
           as.frames, as.frequency, as.peak, as.device_open ? "yes" : "no", as.queue_underruns, as.queue_resets);
}
static void capture(Runtime *rt, const RecompTask *task, const char *prefix) {
    if (!capture_output)
        return;
    char name[128];
    CHECK(snprintf(name, sizeof(name), "%s_rdram.bin", prefix) > 0);
    FILE *f = artifact(name, "wb");
    CHECK(fwrite(rt->rdram, 1, rt->rdram_size, f) == rt->rdram_size && !fclose(f));
    CHECK(snprintf(name, sizeof(name), "%s_commands.bin", prefix) > 0);
    f = artifact(name, "wb");
    for (unsigned n = 0; n < task->words[13]; n++)
        CHECK(fputc((int)recomp_load(rt, task->words[12] + n, 1), f) != EOF);
    CHECK(!fclose(f));
}
static bool submit(Runtime *rt, void *unused, const RecompTask *task) {
    (void)unused;
    CHECK(!task->resumed && (task->words[0] == 1 || task->words[0] == 2));
    bool world = false;
    if (task->words[0] == 1) {
        CHECK(route_mode() || !goal || play_frames < goal);
        CHECK(task->words[1] == 0 && task->words[13] % 8 == 0);
        CHECK(task->words[4] == 0x8032b330 && task->words[5] == 0x1000 && task->words[6] == 0x80339ac0);
        uint32_t cmd = (uint32_t)recomp_load(rt, GAME_SLEVELSCRIPTCOMMAND, 4);
        if (!world_started && cmd && recomp_load(rt, cmd, 1) == 0x12 && recomp_load(rt, cmd + 4, 4) == 0x801768a0)
            menu_started = true;
        if (menu_started && !world_started)
            menu_frames++;
        if (!menu_started && recomp_load(rt, GAME_GGDCTRL + 0xe8, 4))
            face_frames++;
        uint32_t mario = (uint32_t)recomp_load(rt, GAME_GMARIOSTATE, 4);
        uint32_t action = mario ? (uint32_t)recomp_load(rt, mario + 0xc, 4) : 0;
        unsigned level = (unsigned)recomp_load(rt, GAME_GCURRLEVELNUM, 2);
        world = level > 0 && recomp_load(rt, GAME_GCURRENTAREA, 4) && mario && action;
        if (world) {
            world_started = true;
            world_frames++;
            bool controllable = (action & 0x1c0) != 0x100;
            for (unsigned n = 0; n < 3; n++)
                last_pos[n] = guest_float(rt, mario + 0x3c + n * 4);
            if (controllable && !ready) {
                ready = true;
                memcpy(start_pos, last_pos, sizeof(start_pos));
                max_height = min_height = last_pos[1];
                printf("Mario controllable in level %u after %u world frames.\n", level, world_frames);
                fflush(stdout);
                char data[4096];
                CHECK(snprintf(data, sizeof(data), "%s/rt64-data", output) > 0);
                if (!renderer) {
                    renderer = recomp_renderer_create_named(rt, data, "Super Mario 64");
                    CHECK(renderer);
                }
                if (!live_startup) {
                    recomp_audio_set_output_enabled(native_audio, true);
                    playback_origin = monotonic_ns();
                    clock_origin = ns;
                }
                if (!goal && !live_clock) {
                    wall_origin = monotonic_ns();
                    clock_origin = ns;
                    live_clock = true;
                }
            }
            if (ready) {
                if (!play_frames)
                    capture(rt, task, "first_playable");
                play_frames++;
                max_height = fmaxf(max_height, last_pos[1]);
                min_height = fminf(min_height, last_pos[1]);
                if (route_mode()) {
                    route_observe(level);
                    if (route_level_frames == 30) {
                        char prefix[80];
                        CHECK(snprintf(prefix, sizeof(prefix), "level_%u", level) > 0);
                        capture(rt, task, prefix);
                        fprintf(trace,
                                "{\"type\":\"level_snapshot\",\"level\":%u,\"graphics_task\":%u,\"play_frame\":%u,"
                                "\"segments\":[",
                                level, graphics + 1, play_frames);
                        for (unsigned s = 0; s < 16; s++)
                            fprintf(trace, "%s%u", s ? "," : "",
                                    (unsigned)recomp_load(rt, GAME_SSEGMENTTABLE + s * 4, 4));
                        fputs("]}\n", trace);
                    }
                }
                if (goal && (route_mode() ? route_done : play_frames == goal))
                    capture(rt, task, "last_playable");
            }
            if (world_frames == 1)
                capture(rt, task, "first_world");
            if (world_frames % 60 == 0) {
                printf("Game frames: %u; playable: %u; action: %08x.\n", world_frames, play_frames, action);
                fflush(stdout);
            }
        }
        graphics++;
    } else
        audio++;
    fprintf(trace,
            "{\"type\":\"%s_task\",\"token\":%" PRIu64
            ",\"graphics_task\":%u,\"world\":%s,\"play_frame\":%u,\"descriptor\":[",
            task->words[0] == 1 ? "graphics" : "audio", task->token, graphics, world ? "true" : "false", play_frames);
    for (unsigned n = 0; n < 16; n++) {
        fprintf(trace, "%s%u", n ? "," : "", task->words[n]);
    }
    fputs("]}\n", trace);
    if (world) {
        uint32_t m = (uint32_t)recomp_load(rt, GAME_GMARIOSTATE, 4), c = (uint32_t)recomp_load(rt, GAME_GCAMERA, 4);
        fprintf(trace,
                "{\"type\":\"castle_state\",\"graphics_task\":%u,\"world_frame\":%u,\"play_frame\":%u,\"level\":%u,"
                "\"area\":%u,\"action\":%u,\"position\":[%.9g,%.9g,%.9g],\"floor_height\":%.9g,\"floor\":%u,\"health\":"
                "%u,\"camera\":%u,\"camera_position\":[%.9g,%.9g,%.9g],\"cutscene\":%u,\"buttons\":%u,\"stick_x\":%d,"
                "\"stick_y\":%d}\n",
                graphics, world_frames, play_frames, (unsigned)recomp_load(rt, GAME_GCURRLEVELNUM, 2),
                (unsigned)recomp_load(rt, (uint32_t)recomp_load(rt, GAME_GCURRENTAREA, 4), 1),
                (unsigned)recomp_load(rt, m + 0xc, 4), last_pos[0], last_pos[1], last_pos[2], guest_float(rt, m + 0x70),
                (unsigned)recomp_load(rt, m + 0x68, 4), (unsigned)recomp_load(rt, m + 0xae, 2), c,
                c ? guest_float(rt, c + 0x10) : 0, c ? guest_float(rt, c + 0x14) : 0, c ? guest_float(rt, c + 0x18) : 0,
                c ? (unsigned)recomp_load(rt, c + 0x30, 1) : 0, input.buttons, input.stick_x, input.stick_y);
    }
    CHECK(!ferror(trace));
    if (task->words[0] == 1 && renderer) {
        rendered_tasks++;
        return recomp_renderer_submit(renderer, task);
    }
    if (task->words[0] == 2)
        return recomp_audio_submit(native_audio, task);
    CHECK(recomp_task_complete(rt, task->token, RECOMP_SP_DONE));
    if (task->words[0] == 1)
        CHECK(recomp_task_complete(rt, task->token, RECOMP_DP_DONE));
    return true;
}
static bool yield_task(Runtime *rt, void *unused, uint64_t token) {
    (void)rt;
    (void)unused;
    (void)token;
    CHECK(false);
    return false;
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
static bool drain(Runtime *rt) {
    uint64_t start = monotonic_ns();
    bool done = false;
    /* Check for sleeping game/sound threads before spending a full batch on
     * the guest idle loop. Keep the same overall 8192-resume work limit. */
    for (unsigned n = 0; n < 512; n++) {
        CHECK(recomp_scheduler_run(rt, 16) == RECOMP_RUN_BUDGET);
        if (waiting(rt)) {
            done = true;
            break;
        }
    }
    scheduler_ns += monotonic_ns() - start;
    return done;
}
static void set_input(Runtime *rt) {
    input = (RecompPad){.connected = true};
    if (!strcmp(mode, "live")) {
        CHECK(recomp_input_set(rt, 0, &input));
        return;
    }
    if (!menu_started && face_frames >= 30 && face_frames < 33)
        input.buttons = 0x1000;
    if (menu_started && !world_started && menu_frames >= 25 && menu_frames < 28)
        input.buttons = 0x8000;
    /* Acknowledge the original opening dialog, using controller input only. */
    if (world_started && !ready && world_frames % 16 < 3)
        input.buttons = 0x8000;
    if (ready) {
        if (route_mode())
            route_input(rt);
        if (!strcmp(mode, "move") && play_frames >= 30 && play_frames < 90)
            input.stick_y = 80;
        if (!strcmp(mode, "jump") && play_frames >= 30 && play_frames < 33)
            input.buttons = 0x8000;
    }
    CHECK(recomp_input_set(rt, 0, &input));
}
int main(int argc, char **argv) {
    CHECK(argc == 6 || (argc == 7 && !strcmp(argv[6], "--cheat-menu")));
    bool cheat_menu = argc == 7;
    char *end;
    goal = (unsigned)strtoul(argv[4], &end, 10);
    CHECK(!*end && (!goal || (goal >= 60 && goal <= 600)));
    output = argv[2];
    mode = argv[5];
    CHECK(!strcmp(mode, "idle") || !strcmp(mode, "move") || !strcmp(mode, "jump") || !strcmp(mode, "live") ||
          route_mode());
    CHECK(!route_mode() || goal);
    CHECK(!cheat_menu || (!goal && !strcmp(mode, "live")));
    capture_output = goal || (getenv("RECOMP_CAPTURE") && !strcmp(getenv("RECOMP_CAPTURE"), "1"));
    trace = capture_output ? artifact("task_trace.jsonl", "w") : fopen(RECOMP_NULL_DEVICE, "w");
    CHECK(trace);
    Runtime rt;
    CHECK(recomp_runtime_init(&rt, GAME_RAM_SIZE, recomp_functions, recomp_function_count, NULL, recomp_section_count));
    RecompGameOptions options = {.rom = argv[1]};
    uint8_t *rom = recomp_game_rom(&rt, &options, GAME_ROM_SIZE, game_sha1);
    CHECK(recomp_boot_load(&rt, game_boot_loads, sizeof(game_boot_loads) / sizeof(game_boot_loads[0])));
    /* This signed-byte flag belongs to the loaded main data, outside the BSS
     * cleared by guest startup. The original title script selects the menu. */
    if (cheat_menu) {
        recomp_store(&rt, GAME_GDEBUGLEVELSELECT, 1, 1);
        puts("Cheat menu enabled: press S at the title screen; A/X select a stage, S starts it.");
    }
    for (size_t n = 0; n < sizeof(game_parameters) / sizeof(game_parameters[0]); n++)
        recomp_store(&rt, game_parameters[n][0], game_parameters[n][1], 4);
    RecompSchedulerConfig sched = {.thread_tail = GAME_THREAD_TAIL,
                                   .run_queue = GAME_RUN_QUEUE,
                                   .active_queue = GAME_ACTIVE_QUEUE,
                                   .running_thread = GAME_RUNNING_THREAD,
                                   .event_table = GAME_EVENT_TABLE};
    CHECK(recomp_scheduler_init(&rt, &sched));
    RecompClockConfig clock = {
        .ticks_per_second = GAME_COUNTER_HZ, .now_ns = now_ns, .timer_sentinel = GAME_TIMER_SENTINEL};
    CHECK(recomp_clock_init(&rt, &clock) && recomp_platform_init(&rt, &game_platform));
    RecompTaskBackend backend = {.submit = submit, .yield = yield_task};
    CHECK(recomp_tasks_init(&rt, &backend));
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = argv[3]};
    CHECK(recomp_io_init(&rt, &io));
    live_startup = !goal && !strcmp(mode, "live");
    if (live_startup) {
        char data[4096];
        CHECK(snprintf(data, sizeof(data), "%s/rt64-data", output) > 0);
        renderer = recomp_renderer_create_named(&rt, data, "Super Mario 64");
        CHECK(renderer);
        wall_origin = monotonic_ns();
        clock_origin = ns;
        live_clock = true;
    }
    char wav_path[4096];
    CHECK(snprintf(wav_path, sizeof(wav_path), "%s/audio.wav", output) > 0);
    audio_playback = !getenv("RECOMP_AUDIO_MUTE") || strcmp(getenv("RECOMP_AUDIO_MUTE"), "1");
    RecompAudioConfig audio_config = {.wav_path = capture_output ? wav_path : NULL,
                                      .playback = audio_playback,
                                      .output_enabled = live_startup,
                                      .task_profile = &sm64_audio_task_profile};
    native_audio = recomp_audio_create(&rt, &audio_config);
    CHECK(native_audio);
    MipsContext ctx = {0};
    ctx.gpr[31] = mips_s32(0xf0000000);
    recomp_lookup(&rt, GAME_ENTRY_POINT)(&rt, &ctx);
    unsigned batches = 0;
    while (!drain(&rt) && !live_startup)
        CHECK(++batches < 32);
    unsigned vi_count = 0;
    uint64_t next_present = 0;
    while (!goal || (route_mode() ? !route_done : play_frames < goal) || !waiting(&rt)) {
        set_input(&rt);
        if (renderer) {
            RecompPad pad;
            if (!recomp_renderer_input(renderer, &pad))
                goto cancelled;
            if (!strcmp(mode, "live")) {
                input = pad;
                CHECK(recomp_input_set(&rt, 0, &input));
            }
        }
        /* Interactive title/debug menus may remain open indefinitely. */
        ++vi_count;
        if (goal || (!ready && !live_startup))
            CHECK(vi_count < 16384);
        if (!live_clock)
            ns = UINT64_C(16666667) * vi_count;
        (void)drain(&rt);
        if (renderer) {
            if (!next_present)
                next_present = monotonic_ns();
            CHECK(recomp_renderer_present(renderer));
            presentations++;
            /* Rendering and guest work consume part of the VI interval. A
             * fixed extra sleep delays audio service and loses wall-clock VIs.
             * Absolute deadlines also prevent sleep overshoot accumulating. */
            next_present += UINT64_C(16666667);
            pace(next_present);
        }
    }
    CHECK(ready && (route_mode() ? route_target_frames == goal : play_frames == goal) && world_frames >= goal);
    CHECK(recomp_load(&rt, GAME_GCURRLEVELNUM, 2) == (route_mode() ? 6 : 16) &&
          recomp_load(&rt, GAME_GCURRSAVEFILENUM, 2) == 1);
    RecompTaskStats ts = recomp_task_stats(&rt);
    RecompIOStats is = recomp_io_stats(&rt);
    CHECK(ts.sp_events == graphics + audio && ts.dp_events == graphics && !ts.pending_records && !ts.yielded &&
          !ts.rejected_completions && !is.persistence_failures);
    CHECK(is.reads_completed == graphics - 1 && is.si_events == graphics - 1);
    float distance = hypotf(last_pos[0] - start_pos[0], last_pos[2] - start_pos[2]);
    if (!strcmp(mode, "move"))
        CHECK(distance > 100);
    if (!strcmp(mode, "jump"))
        CHECK(max_height - start_pos[1] > 100 && fabsf(last_pos[1] - start_pos[1]) < 2);
    fprintf(trace,
            "{\"type\":\"%s\",\"mode\":\"%s\",\"graphics_tasks\":%u,\"audio_tasks\":%u,\"world_frames\":%u,\"play_"
            "frames\":%u,\"target_frames\":%u,\"vblanks\":%u,\"controller_reads\":%" PRIu64 ",\"sp_events\":%" PRIu64
            ",\"dp_events\":%" PRIu64 ",\"start_position\":[%.9g,%.9g,%.9g],\"last_position\":[%.9g,%.9g,%.9g],"
                                      "\"distance\":%.9g,\"max_height\":%.9g,\"min_height\":%.9g}\n",
            route_mode() ? "interior_summary" : "castle_summary", mode, graphics, audio, world_frames, play_frames,
            route_target_frames, vi_count, is.reads_completed, ts.sp_events, ts.dp_events, start_pos[0], start_pos[1],
            start_pos[2], last_pos[0], last_pos[1], last_pos[2], distance, max_height, min_height);
    fprintf(trace, "{\"type\":\"performance\",\"scheduler_ns\":%" PRIu64 "}\n", scheduler_ns);
    RecompAudioStats as = recomp_audio_stats(native_audio);
    CHECK(as.tasks == audio && as.ai_buffers && as.nonzero_samples);
    finish_audio();
    CHECK(!fclose(trace));
    printf("PASS: %s: %u world / %u playable frames, displacement %.3f, peak height %.3f.\n",
           route_mode() ? "Castle lobby" : "Castle Grounds", world_frames, play_frames, distance, max_height);
    capture_renderer();
    RecompViState vi = recomp_vi_state(&rt);
    CHECK(!vi.black && vi.framebuffer);
    FILE *f = artifact("native_framebuffer.ppm", "wb");
    fputs("P6\n320 240\n255\n", f);
    uint32_t fb = recomp_ram_address(&rt, vi.framebuffer, 320 * 240 * 2);
    for (unsigned n = 0; n < 320 * 240; n++) {
        unsigned c = (unsigned)recomp_load(&rt, fb + n * 2, 2);
        CHECK(fputc((int)((c >> 11) & 31) * 255 / 31, f) != EOF);
        CHECK(fputc((int)((c >> 6) & 31) * 255 / 31, f) != EOF);
        CHECK(fputc((int)((c >> 1) & 31) * 255 / 31, f) != EOF);
    }
    CHECK(!fclose(f));
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
cancelled:
    if (capture_output)
        capture_renderer();
    fprintf(trace,
            "{\"type\":\"performance\",\"scheduler_ns\":%" PRIu64
            ",\"live_clock\":%s,\"play_frames\":%u,\"elapsed_live_ns\":%" PRIu64 "}\n",
            scheduler_ns, live_clock ? "true" : "false", play_frames, live_clock ? monotonic_ns() - wall_origin : 0);
    fputs("{\"type\":\"cancelled\"}\n", trace);
    CHECK(!fclose(trace));
    finish_audio();
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    puts("SM64 window closed.");
    return 0;
}
