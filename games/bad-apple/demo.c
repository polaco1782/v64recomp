#include "demo.h"
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "recomp_audio.h"
#include "bad_apple_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Demo {
    RecompRenderer *renderer;
    RecompAudio *audio;
    RecompViRegisters vi;
    uint32_t pi_ram, pi_cart, ai_ram, dp_start, dp_end;
    uint32_t frames, limit, dac_rate;
    uint64_t next_frame;
    bool running;
} Demo;

static void short_wait(Runtime *rt, void *opaque) {
    (void)rt;
    (void)opaque;
    recomp_game_sleep(recomp_game_monotonic() + 1000000);
}

static void present(Runtime *rt, void *opaque) {
    Demo *demo = opaque;
    recomp_game_require(recomp_renderer_submit_rdp(demo->renderer, demo->dp_start, demo->dp_end,
                                                  &demo->vi, demo->vi.origin),
                        "Bad Apple RDP DMA failed", demo->dp_start);
    recomp_game_require(recomp_renderer_present(demo->renderer), "Bad Apple presentation failed", 0);
    demo->frames++;
    RecompPad pad = {0};
    demo->running = recomp_renderer_input(demo->renderer, &pad) &&
                    (!demo->limit || demo->frames < demo->limit);
    (void)rt;
}

static void wait_scanline(Runtime *rt, void *opaque) {
    Demo *demo = opaque;
    /* The only VI poll in this pinned program waits for line 480 once per
     * movie frame. Pace the 30 FPS stream at this hardware boundary. */
    recomp_game_sleep(demo->next_frame);
    uint64_t now = recomp_game_monotonic();
    demo->next_frame += 1000000000 / 30;
    if (demo->next_frame < now)
        demo->next_frame = now;
    (void)rt;
}

static bool device_read(Runtime *rt, uint32_t bus, unsigned bytes, uint64_t *value) {
    Demo *demo = rt->device_userdata;
    if (bus == 0x04400010 && bytes == 4) {
        recomp_on_host(rt, wait_scanline, demo);
        *value = 480;
        return true;
    }
    if (bus == 0x0450000c && bytes == 1) {
        RecompAiState ai = recomp_ai_state(rt);
        if (ai.queued) {
            recomp_on_host(rt, short_wait, NULL);
            ai = recomp_ai_state(rt);
        }
        /* LB reads the high byte of the big-endian AI status register. */
        *value = (ai.queued ? 0x40 : 0) | (ai.queued == 2 ? 0x80 : 0);
        return true;
    }
    return false;
}

static bool device_write(Runtime *rt, uint32_t bus, unsigned bytes, uint64_t value) {
    if (bytes != 4)
        return false;
    Demo *demo = rt->device_userdata;
    uint32_t word = (uint32_t)value;
    MipsContext call = {0};
    if (bus == 0x1fc007fc) {
        recomp_game_require(word == 8, "unsupported PIF boot command", bus);
        return true; /* IPL handshake: native bootstrap has already loaded RAM. */
    }
    if (bus >= 0x04400000 && bus <= 0x04400034) {
        /* Explicit members avoid assuming C struct layout matches the bus. */
        uint32_t *registers[] = {&demo->vi.status, &demo->vi.origin, &demo->vi.width,
            &demo->vi.intr, &demo->vi.current_line, &demo->vi.burst, &demo->vi.v_sync,
            &demo->vi.h_sync, &demo->vi.leap, &demo->vi.h_start, &demo->vi.v_start,
            &demo->vi.v_burst, &demo->vi.x_scale, &demo->vi.y_scale};
        *registers[(bus - 0x04400000) / 4] = word;
        return true;
    }
    switch (bus) {
    case 0x04500000: demo->ai_ram = word & 0x00fffff8; return true;
    case 0x04500004:
        call.gpr[4] = demo->ai_ram;
        call.gpr[5] = word & 0x3fff8;
        hook_os_ai_set_next_buffer(rt, &call);
        recomp_game_require(call.gpr[2] == 0, "Bad Apple AI FIFO full", bus);
        return true;
    case 0x04500008:
        recomp_game_require(word == 1, "unsupported Bad Apple AI control", bus);
        return true;
    case 0x04500010:
        demo->dac_rate = (word & 0x3fff) + 1;
        call.gpr[4] = 48681812 / demo->dac_rate;
        hook_os_ai_set_frequency(rt, &call);
        recomp_game_require(recomp_ai_state(rt).dac_rate == demo->dac_rate,
                            "Bad Apple AI divisor mismatch", bus);
        return true;
    case 0x04500014:
        recomp_game_require(word == 15, "unsupported Bad Apple AI sample width", bus);
        return true;
    case 0x04600000: demo->pi_ram = word & 0x00fffffe; return true;
    case 0x04600004: demo->pi_cart = word & 0xfffffffe; return true;
    case 0x0460000c: {
        uint32_t count = (word & 0x00ffffff) + 1;
        recomp_game_require(demo->pi_cart >= 0x10000000, "invalid Bad Apple PI source", demo->pi_cart);
        uint32_t offset = demo->pi_cart - 0x10000000;
        recomp_game_require(offset <= rt->rom_size && count <= rt->rom_size - offset,
                            "Bad Apple PI DMA outside ROM", demo->pi_cart);
        recomp_copy_to_ram(rt, demo->pi_ram, rt->rom + offset, count);
        return true;
    }
    case 0x04100000: demo->dp_start = word; return true;
    case 0x04100004:
        demo->dp_end = word;
        recomp_on_host(rt, present, demo);
        if (!demo->running)
            recomp_thread_stop(rt, 0);
        return true;
    default: return false;
    }
}

int bad_apple_main(int argc, char **argv, BadAppleDiagnostics diagnostics, const char *wav) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "Bad Apple runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL, .checkpoint_interval = 4096};
    RecompPlatformConfig platform = {.vi_period_ticks = BOOT_COUNTER_HZ / 60};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                        recomp_platform_init(&rt, &platform), "Bad Apple native services init failed", 0);
    /* The bare-metal ROM does not call libultra. Initialize the shared native
     * AI service with NTSC timing before forwarding the verified AI accesses. */
    recomp_store(&rt, 0x80000300, 1, 4);
    MipsContext init = {0};
    hook_os_initialize(&rt, &init);
    Demo demo = {.limit = options.frames, .running = true};
    rt.device_userdata = &demo;
    rt.device_read = device_read;
    rt.device_write = device_write;
    demo.renderer = recomp_renderer_create_named(&rt, options.data, "Bad Apple!! - v64recomp");
    recomp_game_require(demo.renderer != NULL, "Bad Apple renderer init failed", 0);
    recomp_renderer_set_frame_rate(demo.renderer, 0);
    RecompAudioConfig audio_config = {.playback = !options.mute, .output_enabled = true, .wav_path = wav};
    demo.audio = recomp_audio_create(&rt, &audio_config);
    recomp_game_require(demo.audio != NULL, "Bad Apple audio init failed", 0);
    demo.next_frame = recomp_game_monotonic();
    recomp_thread_create(&rt, BOOT_THREAD_OBJECT, 1, BOOT_ENTRY, 0, BOOT_THREAD_STACK, 10);
    recomp_thread_start(&rt, BOOT_THREAD_OBJECT);
    while (demo.running) {
        RecompRunResult result = recomp_scheduler_run(&rt, 64);
        recomp_game_require(!demo.running || result == RECOMP_RUN_BUDGET,
                            "Bad Apple guest stopped unexpectedly", BOOT_ENTRY);
    }
    if (diagnostics)
        diagnostics(&rt, demo.renderer, demo.audio, demo.frames);
    recomp_audio_destroy(demo.audio);
    recomp_renderer_destroy(demo.renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
