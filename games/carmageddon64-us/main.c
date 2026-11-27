#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "game_config.h"
#include <stdlib.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static bool ipl_retired;

/* The title repeats libultra's IPL-complete command before creating any SI queues. */
static bool device_read(Runtime *rt, uint32_t bus, unsigned bytes, uint64_t *value) {
    (void)rt;
    if (!ipl_retired && bytes == 4 && (bus == 0x04800018 || bus == 0x1fc007fc)) {
        *value = 0;
        return true;
    }
    return false;
}

static bool device_write(Runtime *rt, uint32_t bus, unsigned bytes, uint64_t value) {
    (void)rt;
    return !ipl_retired && bus == 0x1fc007fc && bytes == 4 && value == 8;
}

static void prepare_call(Runtime *rt, uint32_t address) {
    if (!ipl_retired && address == 0x8008cde4 && recomp_thread_id(rt, 0) == 0x66) {
        recomp_game_require((uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x8008ce78, "unexpected IPL handoff",
                            address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        ipl_retired = true;
    }
}

static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "Carmageddon 64 RSP task rejected", task->words[4]);
    return true;
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
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    rt.prepare_call = prepare_call;
    rt.device_read = device_read;
    rt.device_write = device_write;
    rt.interrupt_global_mask_address = BOOT_GLOBAL_INT_MASK;
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .idle_thread = BOOT_IDLE_THREAD};
    RecompPlatformConfig platform = {.vi_period_ticks = BOOT_COUNTER_HZ / 60,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .app_nmi_buffer = 0x8000031c,
                                     .base_counter = BOOT_BASE_COUNTER,
                                     .current_time = BOOT_CURRENT_TIME,
                                     .cart_handle = BOOT_CART_HANDLE,
                                     .drive_handle = BOOT_DRIVE_HANDLE};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Carmageddon 64 services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1,
                         .pak_mask = 1,
                         .pak_paths = {options.save},
                         .cont_init_queue = 0x80228920,
                         .cont_init_messages = 0x80228938,
                         .cont_init_queue_capacity = 1};
    RecompTaskBackend tasks = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &tasks), "IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Carmageddon 64");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled = true, .playback = !options.mute, .task_profile = &audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t deadline = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        deadline = recomp_game_next_deadline(deadline, 16666667);
        recomp_game_pace(&rt, deadline);
        recomp_game_require(recomp_game_present(renderer), "presentation failed", 0);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
