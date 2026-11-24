#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "controls.h"
#include "recomp_jpeg.h"
#include "flash.h"
#include "overlays.h"
#include "game_config.h"
#include <stdlib.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;

static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return recomp_game_monotonic() - wall_origin;
}

static void retire_boot(Runtime *rt, uint32_t address) {
    if (address == 0x8008038c && recomp_thread_id(rt, 0) == 1) {
        recomp_game_require((uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x80080528,
                            "unexpected Majora's Mask IPL handoff", address);
        recomp_thread_destroy(rt, 0x807ff000);
        rt->prepare_call = NULL;
    }
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    if (task->words[0] == 4) {
        recomp_game_require(recomp_jpeg_submit(rt, task), "Majora's Mask JPEG task rejected", task->words[4]);
        return recomp_task_complete(rt, task->token, RECOMP_SP_DONE);
    }
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported Majora's Mask RSP task",
                        task->guest_address);
    return task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
}

int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, MM_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, MM_ROM_SIZE, mm_sha1);
    recomp_copy_to_ram(&rt, MM_INITIAL_LOAD_ADDRESS, rom + MM_INITIAL_ROM_OFFSET, MM_INITIAL_LOAD_SIZE);
    mm_init_code(&rt);
    mm_flash_open(options.save);
    for (size_t i = 0; i < sizeof(mm_boot_parameters) / sizeof(mm_boot_parameters[0]); i++)
        recomp_store32(&rt, mm_boot_parameters[i][0], mm_boot_parameters[i][1]);
    rt.prepare_call = retire_boot;
    rt.interrupt_global_mask_address = MM_INTERRUPT_GLOBAL_MASK;
    wall_origin = recomp_game_monotonic();
    RecompClockConfig clock = {.ticks_per_second = MM_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = MM_THREAD_TAIL,
                                       .run_queue = MM_RUN_QUEUE,
                                       .active_queue = MM_ACTIVE_QUEUE,
                                       .running_thread = MM_RUNNING_THREAD,
                                       .idle_thread = MM_IDLE_OBJECT};
    RecompPlatformConfig platform = {.vi_period_ticks = MM_COUNTER_HZ / 60,
                                     .vi_clock = MM_VI_CLOCK,
                                     .clock_rate = MM_CLOCK_RATE,
                                     .final_rom = MM_FINAL_ROM,
                                     .cart_handle = MM_CART_HANDLE,
                                     .app_nmi_buffer = 0x8000031c};
    RecompIOConfig io = {.connected_mask = 1, .si_transfer_ticks = MM_COUNTER_HZ / 1000};
    RecompTaskBackend backend = {.submit = submit, .additional_task_types = 1u << 4};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform) && recomp_io_init(&rt, &io) &&
                            recomp_tasks_init(&rt, &backend),
                        "Majora's Mask services init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "The Legend of Zelda: Majora's Mask");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    mm_controls_init(renderer);
    RecompAudioConfig ac = {
        .output_enabled = true, .playback = !options.mute, .prebuffer_ms = 160, .task_profile = &mm_audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, 0x807ff000, -1, MM_ENTRY, 0, 0x807fd000, 0);
    recomp_thread_start(&rt, 0x807ff000);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!mm_controls_poll(&rt, renderer, &pad))
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
