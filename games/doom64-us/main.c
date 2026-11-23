#define _POSIX_C_SOURCE 200809L
#include "audio.h"
#include "controls.h"
#include "fov.h"
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "game_config.h"
#include <math.h>
#include <stdlib.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;

static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}

void doom64_refresh_brightness(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if ((uint32_t)ctx->gpr[31] == BOOT_DISPLAY_RESET_RETURN)
        recomp_store32(rt, BOOT_BRIGHTNESS_ADDRESS, BOOT_BRIGHTNESS_DEFAULT);
    original(rt, ctx);
}

void doom64_set_light_factor(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    int32_t factor = (int32_t)ctx->gpr[4];
    original(rt, ctx);
    uint32_t brightness = recomp_load32(rt, BOOT_BRIGHTNESS_ADDRESS);
    if (factor <= 0 || brightness == 0)
        return;

    // The guest rebuilds this palette from the map's original colors on each
    // refresh. Lift its darker channels while keeping black and white fixed.
    uint32_t light = recomp_load32(rt, BOOT_LIGHT_TABLE);
    int32_t count = (int32_t)recomp_load32(rt, BOOT_LIGHT_COUNT);
    for (int32_t i = 0; i < count; i++, light += 8) {
        uint32_t rgba = recomp_load32(rt, light);
        uint32_t adjusted = rgba & 255;
        for (unsigned shift = 8; shift <= 24; shift += 8) {
            uint32_t channel = (rgba >> shift) & 255;
            uint32_t lifted = (uint32_t)(sqrtf(channel * 255.0f) + 0.5f);
            adjusted |= (channel + (lifted - channel) * brightness / 100) << shift;
        }
        recomp_store32(rt, light, adjusted);
    }
}

static void retire_boot(Runtime *rt, uint32_t address) {
    // I_IdleGameThread's first call follows the osStartThread handoff. Its
    // descendants can now reuse upper RDRAM for the original framebuffers.
    if (address == 0x8003a9e0 && recomp_thread_id(rt, 0) == 1) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        recomp_game_require((uint32_t)ctx->gpr[31] == 0x800056a4, "unexpected Doom 64 IPL handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        rt->prepare_call = NULL;
    }
}

#ifdef RECOMP_WITH_RT64
static bool bind_microcode(void *interpreter, size_t ram_size, uint32_t code, uint32_t data) {
    return code == 0x4b470 && recomp_renderer_bind_l3dex_1_00(interpreter, ram_size, code, data);
}
#endif

static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported Doom 64 RSP task",
                        task->guest_address);
    return task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
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
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    recomp_store32(&rt, BOOT_BRIGHTNESS_ADDRESS, BOOT_BRIGHTNESS_DEFAULT);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store32(&rt, boot_parameters[i][0], boot_parameters[i][1]);
    rt.prepare_call = retire_boot;
    rt.interrupt_global_mask_address = BOOT_INTERRUPT_GLOBAL_MASK;
    wall_origin = recomp_game_monotonic();
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .idle_thread = BOOT_IDLE_OBJECT};
    RecompPlatformConfig platform = {.vi_period_ticks = BOOT_COUNTER_HZ / 60,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c};
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options.save}};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform) && recomp_io_init(&rt, &io) &&
                            recomp_tasks_init(&rt, &backend),
                        "Doom 64 services init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Doom 64");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    doom64_controls_init(renderer);
    doom64_fov_init(renderer);
#ifdef RECOMP_WITH_RT64
    recomp_renderer_set_microcode_hook(renderer, bind_microcode);
#endif
    RecompAudioConfig ac = {
        .output_enabled = true, .playback = !options.mute, .prebuffer_ms = 160, .task_profile = &doom64_audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!doom64_controls_poll(&rt, renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++)
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        recomp_game_require(recomp_game_present(renderer), "presentation failed", 0);
        next_present = recomp_game_next_deadline(next_present, 16666667);
        recomp_game_pace(&rt, next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
