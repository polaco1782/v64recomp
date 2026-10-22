#include "run.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "video.h"
#include "re2_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <openssl/evp.h>
#include <string.h>
#include <inttypes.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired;
static bool main_loaded;
static uint32_t loading_address, loading_size, loading_compressed;
#ifdef RECOMP_DEBUG
static unsigned graphics_tasks;
static FILE *input;
static bool next_input;
static unsigned input_frame, previous_input_frame;
static RecompPad replay = {.connected = true}, pending = {.connected = true};
static void read_input(void) {
    unsigned buttons;
    int x, y;
    int fields = fscanf(input, "%u %u %d %d", &input_frame, &buttons, &x, &y);
    recomp_game_require(fields == EOF || fields == 4, "invalid RE2 replay row", previous_input_frame);
    next_input = fields == 4;
    if (!next_input)
        return;
    recomp_game_require(input_frame >= previous_input_frame && buttons <= UINT16_MAX && x >= -128 && x <= 127 &&
                            y >= -128 && y <= 127,
                        "invalid RE2 replay values", input_frame);
    previous_input_frame = input_frame;
    pending = (RecompPad){.connected = true, .buttons = (uint16_t)buttons, .stick_x = (int8_t)x, .stick_y = (int8_t)y};
}
#endif

static void verify_main(Runtime *rt) {
    uint8_t *image = malloc(BOOT_MAIN_SIZE), digest[32];
    unsigned length = 0;
    recomp_game_require(image != NULL, "cannot verify RE2 main image", BOOT_MAIN_ADDRESS);
    for (uint32_t i = 0; i < BOOT_MAIN_SIZE; i++)
        image[i] = (uint8_t)recomp_load(rt, BOOT_MAIN_ADDRESS + i, 1);
    bool ok = EVP_Digest(image, BOOT_MAIN_SIZE, digest, &length, EVP_sha256(), NULL) && length == 32 &&
              !memcmp(digest, main_sha256, 32);
    free(image);
    recomp_game_require(ok, "RE2 main image differs from compiled image", BOOT_MAIN_ADDRESS);
    rt->section_loaded[1] = true;
    main_loaded = true;
}

/* The complete ROM helper writes WatchLo=a0, WatchHi=0. All three enable
 * bits must be clear: native execution has no enabled guest memory watchpoint.
 * Refuse enabled requests instead of silently discarding a debug exception. */
void hook_re2_disable_watch(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    recomp_game_require(((uint32_t)ctx->gpr[4] & 7) == 0, "RE2 enabled CPU watchpoints require native support",
                        ctx->pc);
}
void hook_re2_start_thread(Runtime *rt, MipsContext *ctx) {
    bool handoff = recomp_thread_id(rt, 0) == -1;
    if (handoff)
        recomp_game_require((uint32_t)ctx->gpr[4] == 0x8010b700 && (uint32_t)ctx->gpr[31] == 0x8004d388,
                            "unexpected RE2 IPL thread start", ctx->pc);
    hook_os_start_thread(rt, ctx);
    if (handoff)
        recomp_thread_stop(rt, BOOT_BOOT_OBJECT);
}
void hook_re2_cache_allocate(Runtime *rt, MipsContext *ctx) {
    uint32_t start = (uint32_t)ctx->gpr[4], end = (uint32_t)ctx->gpr[5];
    /* Its unsigned comparison happens after the first increment/CACHE. The
     * audio allocator also calls it with end < start; that still touches just
     * one line and returns start + 16. */
    uint64_t bytes = end > start ? ((uint64_t)end - start + 15) & ~UINT64_C(15) : 16;
    recomp_game_require(bytes <= BOOT_RAM_SIZE && (uint64_t)start + bytes <= UINT32_MAX,
                        "invalid RE2 cache allocation range", start);
    (void)recomp_ram_address(rt, start, bytes);
    /* CACHE 0x0d creates exclusive D-cache lines. Host RAM is already coherent;
     * it neither zeroes bytes nor writes data. Preserve the loop's GPR results. */
    mips_set(ctx, 2, mips_s32(start + (uint32_t)bytes));
    mips_set(ctx, 1, 0);
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}
static void prepare_call(Runtime *rt, uint32_t address) {
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_TRACE_CALLS"))
        fprintf(stderr, "RE2 call 0x%08x\n", address);
#endif
    const MipsContext *call =
        address == 0x800045dc || address == BOOT_INFLATE_CLEANUP ? recomp_thread_context(rt, 0) : NULL;
    if (address == 0x800045dc && call && (uint32_t)call->gpr[31] == 0x8004cc38) {
        recomp_game_require(!loading_compressed, "concurrent RE2 overlay decompression", address);
        loading_address = (uint32_t)call->gpr[6];
        loading_compressed = (uint32_t)call->gpr[7];
        for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++)
            if (re2_overlays[i].address == loading_address && re2_overlays[i].compressed_size == loading_compressed)
                loading_size = re2_overlays[i].size;
        (void)recomp_ram_address(rt, loading_address, loading_size);
        for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++) {
            if ((uint64_t)loading_address < (uint64_t)re2_overlays[i].address + re2_overlays[i].size &&
                (uint64_t)re2_overlays[i].address < (uint64_t)loading_address + loading_size)
                rt->section_loaded[re2_overlays[i].section] = false;
        }
    }
    if (address == BOOT_INFLATE_CLEANUP && call && (uint32_t)call->gpr[31] == 0x8004cc48) {
        uint32_t stream = (uint32_t)call->gpr[4];
        uint32_t produced = (uint32_t)recomp_load(rt, stream + 0x14, 4);
        recomp_game_require(loading_compressed && produced && (!loading_size || produced == loading_size) &&
                                (uint64_t)loading_address + produced <= UINT32_MAX &&
                                recomp_load(rt, stream + 0xc, 4) == loading_address + produced,
                            "RE2 overlay inflater did not produce the complete image", loading_address);
        loading_size = produced;
        bool matched = false;
        for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++) {
            if (re2_overlays[i].address != loading_address || re2_overlays[i].size != loading_size)
                continue;
            uint8_t *image = malloc(loading_size), digest[32];
            unsigned length = 0;
            recomp_game_require(image != NULL, "cannot verify RE2 overlay", loading_address);
            for (uint32_t j = 0; j < loading_size; j++)
                image[j] = (uint8_t)recomp_load(rt, loading_address + j, 1);
            bool ok = EVP_Digest(image, loading_size, digest, &length, EVP_sha256(), NULL) && length == 32 &&
                      !memcmp(digest, re2_overlays[i].sha256, 32);
            free(image);
            if (ok) {
                rt->section_loaded[re2_overlays[i].section] = true;
                matched = true;
                break;
            }
        }
        recomp_game_require(matched, "RE2 decompressed overlay has no verified compiled mapping", loading_address);
        loading_size = loading_compressed = 0;
    }
    /* The startup loader calls inflate cleanup only after producing all bytes.
     * Activate before dispatch validates the first .main target. */
    if (!main_loaded && address == BOOT_INFLATE_CLEANUP) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        recomp_game_require(ctx && (uint32_t)ctx->gpr[31] == 0x80000660, "unexpected RE2 main-image handoff", address);
        verify_main(rt);
    }
    /* Thread creation also looks up its entry while IPL still owns execution.
     * Retire on the initial thread's first actual call instead. */
    if (!boot_retired && address == BOOT_MAIN_THREAD_FIRST_CALL) {
        recomp_game_require(recomp_thread_id(rt, 0) == 7, "unexpected RE2 initial thread", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    if (task->words[0] == 2) {
        recomp_game_require(recomp_audio_submit(audio, task), "RE2 MusyX task rejected", task->words[4]);
        return true;
    }
    if (re2_video_submit(rt, task))
        return true;
    recomp_game_require(task->words[0] == 1, "RE2 RSP video task needs a verified backend", task->guest_address);
    recomp_game_require(recomp_renderer_submit(renderer, task), "RE2 graphics task rejected", task->words[4]);
#ifdef RECOMP_DEBUG
    graphics_tasks++;
#endif
    return true;
}
int re2_run(const RecompGameOptions *options, const char *report) {
    (void)report;
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "RE2 runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c};
    wall_origin = recomp_game_monotonic();
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "RE2 services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options->save}};
    RecompTaskBackend backend = {.submit = submit, .validate_custom_data = re2_video_validate};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "RE2 IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options->data, "Resident Evil 2");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    /* Video writes framebuffer RAM without a graphics full-sync. PresentEarly
     * skips VI-only updates; use RT64's original-rate VI presentation path. */
    recomp_renderer_set_frame_rate(renderer, 0);
    RecompAudioConfig audio_config = {.output_enabled = true,
                                      .playback = !options->mute,
                                      .microcode = RECOMP_AUDIO_MUSYX_V1,
                                      .task_profile = &re2_audio_profile};
#ifdef RECOMP_DEBUG
    audio_config.wav_path = getenv("RECOMP_RE2_WAV");
#endif
    audio = recomp_audio_create(&rt, &audio_config);
    recomp_game_require(audio != NULL, "RE2 audio output initialization failed", 0);
#ifdef RECOMP_DEBUG
    const char *input_path = getenv("RECOMP_RE2_INPUT");
    if (input_path) {
        input = fopen(input_path, "r");
        recomp_game_require(input != NULL, "cannot open RE2 replay", 0);
        read_input();
    }
    const char *capture = getenv("RECOMP_RE2_CAPTURE_EVERY");
    char *capture_end = NULL;
    unsigned long capture_every = capture ? strtoul(capture, &capture_end, 10) : 0;
    recomp_game_require(!capture || (*capture && !*capture_end && capture_every <= UINT32_MAX),
                        "invalid RE2 capture interval", 0);
#endif
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t deadline = recomp_game_monotonic();
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
        recomp_game_require(recomp_input_set(&rt, 0, &pad), "RE2 controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++)
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        recomp_game_require(recomp_renderer_present(renderer), "RE2 presentation failed", 0);
#ifdef RECOMP_DEBUG
        if (capture_every && (frame + 1) % capture_every == 0 && graphics_tasks && report) {
            char path[4096];
            recomp_game_require(snprintf(path, sizeof(path), "%s.frame-%06u.ppm", report, frame + 1) <
                                    (int)sizeof(path),
                                "RE2 capture path too long", 0);
            recomp_game_require(recomp_renderer_capture(renderer, path), "RE2 capture failed", 0);
        }
#endif
        deadline += 16666667;
        recomp_game_pace(&rt, deadline);
    }
#ifdef RECOMP_DEBUG
    if (input)
        fclose(input);
    if (report) {
        FILE *out = fopen(report, "w");
        recomp_game_require(out != NULL, "cannot open RE2 diagnostic report", 0);
        RecompAudioStats stats = recomp_audio_stats(audio);
        fprintf(out,
                "{\"presentations\":%u,\"main_image_verified\":%s,\"boot_retired\":%s,\"graphics_tasks\":%u,\"audio_"
                "frames\":%" PRIu64 ",\"audio_peak\":%u}\n",
                frame, main_loaded ? "true" : "false", boot_retired ? "true" : "false", graphics_tasks, stats.frames,
                stats.peak);
        recomp_game_require(fclose(out) == 0, "cannot write RE2 diagnostic report", 0);
    }
#endif
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
