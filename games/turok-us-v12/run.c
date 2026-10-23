#define _POSIX_C_SOURCE 200809L
#include "run.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "graphics.h"
#include "turok_config.h"
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define require recomp_game_require
static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired, program_verified;
static const uint8_t *cartridge;
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
/* Keep horizontal stick input for analog turning. Swap only vertical live
 * input: arrows/left stick move forward/back, while I/K/right stick look
 * up/down. J/L/right stick retain C-button strafing. Debug replays remain
 * original N64 inputs, independent of the host bindings. */
static void map_controls(RecompPad *pad) {
    const uint16_t look = pad->buttons & 0x000c;
    const int y = pad->stick_y;
    pad->buttons &= ~0x000c;
    if (y > 20)
        pad->buttons |= 0x0008;
    if (y < -20)
        pad->buttons |= 0x0004;
    pad->stick_y = ((look & 0x0008) != 0) * 80 - ((look & 0x0004) != 0) * 80;
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}
/* IPL has no OS services yet. Do not publish live scheduler globals into
 * the program while its original RNC routine is still writing that image.
 * A checked boot continuation activates the image before its first SDK call. */
static void activate_program(Runtime *rt) {
    if (!program_verified) {
        uint8_t *raw = malloc(BOOT_DECODED_SIZE), digest[32];
        require(raw != NULL, "program verification allocation failed", BOOT_DECODED_ADDRESS);
        for (uint32_t i = 0; i < BOOT_DECODED_SIZE; i++)
            raw[i] = (uint8_t)recomp_load(rt, BOOT_DECODED_ADDRESS + i, 1);
        SHA256(raw, BOOT_DECODED_SIZE, digest);
        free(raw);
        require(!memcmp(digest, decoded_sha256, 32), "Turok guest decoded image mismatch", BOOT_DECODED_ADDRESS);
        rt->section_loaded[BOOT_MAIN_SECTION] = true;
        program_verified = true;
        rt->interrupts_disabled = false;
    }
}
static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == 0x800004c4)
        rt->interrupts_disabled = true;
    if (address == 0x800005dc)
        activate_program(rt);
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_TRACE_CALLS"))
        fprintf(stderr, "Turok call 0x%08x\n", address);
#endif
#ifdef RECOMP_DEBUG
    if (address == BOOT_SYSTEM_ENTRY) {
        for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
            require(recomp_load(rt, a, 4) == 0, "Turok BSS clear mismatch", a);
        require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5, "Turok BSS overrun", BOOT_BSS_END);
        bss_verified = true;
    }
#endif
    if (!boot_retired && address == 0x80001d30 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        require((uint32_t)ctx->gpr[31] == 0x8000133c, "unexpected IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
/* Original 0x80000450 waits for PI, then copies size/4 cartridge words. */
void hook_turok_rom_read(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = (uint32_t)ctx->gpr[4], destination = (uint32_t)ctx->gpr[5];
    uint32_t size = (uint32_t)ctx->gpr[6] & ~3u;
    require(offset <= BOOT_ROM_SIZE && size <= BOOT_ROM_SIZE - offset,
            "Turok boot cartridge read outside ROM", offset);
    if (size)
        recomp_copy_to_ram(rt, destination, cartridge + offset, size);
    mips_set(ctx, 2, mips_s32((0xb0000000u | offset) + size));
    mips_set(ctx, 3, mips_s32(destination + size));
    mips_set(ctx, 6, UINT64_MAX);
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
#ifdef RECOMP_DEBUG
    static bool seen[3];
    if (!seen[task->words[0]]) {
        fprintf(stderr, "Turok first task type %u:", task->words[0]);
        for (unsigned i = 0; i < 16; i++)
            fprintf(stderr, " %08x", task->words[i]);
        fputc('\n', stderr);
        seen[task->words[0]] = true;
    }
    if (task->words[0] == 1)
        graphics_tasks++;
#endif
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    require(ok, "Turok RSP task rejected", task->words[4]);
    return true;
}
static void drain(Runtime *rt) {
    for (unsigned batch = 0; batch < 32; batch++) {
        if (recomp_scheduler_run(rt, 32) != RECOMP_RUN_BUDGET)
            break;
    }
}
int turok_run(const RecompGameOptions *options, const char *report) {
#ifndef RECOMP_DEBUG
    (void)report;
#endif
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count, recomp_boot_sections,
                                recomp_section_count),
            "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    cartridge = rom;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
#ifdef RECOMP_DEBUG
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4)
        recomp_store(&rt, a, 0xa5a5a5a5, 4);
    const char *replay_path = getenv("RECOMP_TUROK_INPUT");
    if (replay_path) {
        input = fopen(replay_path, "r");
        require(input != NULL, "cannot open replay", 0);
        read_input();
    }
    const char *capture = getenv("RECOMP_TUROK_CAPTURE_EVERY");
    unsigned capture_every = capture ? (unsigned)strtoul(capture, NULL, 10) : 0;
    const char *capture_begin = getenv("RECOMP_TUROK_CAPTURE_FROM");
    unsigned capture_from = capture_begin ? (unsigned)strtoul(capture_begin, NULL, 10) : 0;
#endif
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .idle_thread = BOOT_IDLE_THREAD};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = 0,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c};
    wall_origin = recomp_game_monotonic();
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                recomp_platform_init(&rt, &platform),
            "Turok services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options->save}};
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Turok IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options->data, "Turok");
    require(renderer != NULL, "renderer initialization failed", 0);
    recomp_renderer_set_microcode_hook(renderer, turok_bind_microcode);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options->mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &turok_audio_profile};
#ifdef RECOMP_DEBUG
    ac.wav_path = getenv("RECOMP_TUROK_WAV");
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
        map_controls(&pad);
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
        if (capture_every && frame + 1 >= capture_from && (frame + 1) % capture_every == 0 && graphics_tasks && report) {
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
    RecompViState vi = recomp_vi_state(&rt);
    if (report) {
        FILE *f = fopen(report, "w");
        require(f != NULL, "cannot open diagnostic report", 0);
        fprintf(f,
                "{\"ram_bytes\":%zu,\"vi_width\":%u,\"bss_verified\":%s,\"boot_retired\":%s,\"presentations\":%u,"
                "\"graphics_tasks\":%" PRIu64 ",\"audio_tasks\":%" PRIu64 ",\"audio_frames\":%" PRIu64
                ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
                ",\"controller_reads\":%" PRIu64 "}\n",
                rt.rdram_size, vi.mode ? (unsigned)recomp_load(&rt, vi.mode + 8, 4) : 0,
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
