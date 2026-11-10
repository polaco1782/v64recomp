#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "graphics.h"
#include "controls.h"
#include "game_config.h"
#include <inttypes.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired, program_verified;
static const uint8_t *cartridge;
static bool running = true;
static void poll_command_input(Runtime *rt, void *opaque) {
    (void)opaque;
    RecompPad pad = {.connected = true};
    if (!turok_controls_poll(rt, renderer, &pad))
        running = false;
    recomp_game_require(recomp_game_input(rt, 0, &pad), "controller update failed", 0);
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
        recomp_game_require(raw != NULL, "program verification allocation failed", BOOT_DECODED_ADDRESS);
        for (uint32_t i = 0; i < BOOT_DECODED_SIZE; i++)
            raw[i] = (uint8_t)recomp_load(rt, BOOT_DECODED_ADDRESS + i, 1);
        SHA256(raw, BOOT_DECODED_SIZE, digest);
        free(raw);
        recomp_game_require(!memcmp(digest, decoded_sha256, 32), "Turok guest decoded image mismatch",
                            BOOT_DECODED_ADDRESS);
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
    if (!boot_retired && address == 0x80001d30 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        recomp_game_require((uint32_t)ctx->gpr[31] == 0x8000133c, "unexpected IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}
/* Original 0x80000450 waits for PI, then copies size/4 cartridge words. */
void recomp_turok_rom_read(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = (uint32_t)ctx->gpr[4], destination = (uint32_t)ctx->gpr[5];
    uint32_t size = (uint32_t)ctx->gpr[6] & ~3u;
    recomp_game_require(offset <= BOOT_ROM_SIZE && size <= BOOT_ROM_SIZE - offset,
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
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "Turok RSP task rejected", task->words[4]);
    return true;
}
static void drain(Runtime *rt) {
    for (unsigned batch = 0; batch < 32; batch++) {
        if (recomp_scheduler_run(rt, 32) != RECOMP_RUN_BUDGET)
            break;
    }
}
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = prepare_call;
    cartridge = rom;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
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
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Turok services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options.save}};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Turok IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Turok");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    recomp_renderer_set_microcode_hook(renderer, turok_bind_microcode);
    turok_controls_init(poll_command_input);
    recomp_renderer_set_fov_hook(renderer, turok_fov_adjust);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options.mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &turok_audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t next_present = recomp_game_monotonic();
    uint32_t frame = 0;
    for (; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!running || !turok_controls_poll(&rt, renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        drain(&rt);
        recomp_game_require(recomp_game_present(renderer), "presentation failed", 0);
        next_present += 16666667;
        recomp_game_pace(&rt, next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
