#include "game.h"
#include "audio.h"
#include "generated.h"
#include "renderer.h"
#include "game_config.h"
#include <openssl/sha.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static RecompRenderer *renderer;
static RecompAudio *audio;
static bool core_services, idle_configured;
static uint64_t wall_origin;

void recomp_banjo_get_sr(Runtime *rt, MipsContext *ctx) {
    /* CPU interrupt-mask bits come from the current native scheduler thread.
     * The guest executes with CU1 enabled, outside EXL/ERL. */
    ctx->gpr[2] = UINT32_C(0x20000000) | (rt->interrupt_mask & 0xff00u) | (rt->interrupts_disabled ? 0u : 1u);
}

void recomp_banjo_ucode_load(Runtime *rt, MipsContext *ctx) {
    recomp_game_require(recomp_task_stats(rt).submissions == 0, "Banjo IPL3 RSP state requested after task execution",
                        0x80255c30);
    /* IPL3 leaves DMEM[0] = 0xffffffff and IMEM[0] = 0x000017d7.
     * Preserve the game's xor results and normal PI DMA/queue path rather
     * than bypassing the cartridge's boot microcode load. */
    recomp_store(rt, 0x80283380, 0, 4);
    recomp_store(rt, 0x80283384, 0, 4);
    recomp_store(rt, 0x80283388, 0, 4);
    ctx->gpr[4] = (int32_t)0x80283280;
    ctx->gpr[5] = (int32_t)0xb0000b70;
    ctx->gpr[6] = 0x100;
    recomp_lookup(rt, 0x802405f0)(rt, ctx);
}

static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? recomp_game_monotonic() - wall_origin : 0;
}

static bool matches_image(Runtime *rt, size_t n) {
    uint8_t *image = malloc(banjo_images[n].size);
    uint8_t digest[SHA256_DIGEST_LENGTH];
    recomp_game_require(image != NULL, "Banjo image verification allocation failed", banjo_images[n].address);
    for (uint32_t i = 0; i < banjo_images[n].size; i++)
        image[i] = (uint8_t)recomp_load(rt, banjo_images[n].address + i, 1);
    SHA256(image, banjo_images[n].size, digest);
    free(image);
    return !memcmp(digest, banjo_images[n].sha256, sizeof(digest));
}

static void services(Runtime *rt, bool core) {
    RecompClockConfig clock = {
        .ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time, .timer_sentinel = core ? 0x80285d00 : 0};
    RecompSchedulerConfig scheduler = {.thread_tail = core ? 0x80277370 : 0x80005120,
                                       .run_queue = core ? 0x80277378 : 0x80005128,
                                       .active_queue = core ? 0x8027737c : 0x8000512c,
                                       .running_thread = core ? 0x80277380 : 0x80005130};
    RecompPlatformConfig platform = {.vi_period_ticks = BOOT_VI_PERIOD_TICKS,
                                     .clock_rate = core ? 0x80277120 : 0x800050e0,
                                     .vi_clock = core ? 0x80277128 : 0x800050e8,
                                     .final_rom = core ? 0x80283440 : 0x800072b0,
                                     .cart_handle = core ? 0x80285be0 : 0x80009740,
                                     .timer_list = core ? 0x80277410 : 0,
                                     .current_time = core ? 0x80285d20 : 0,
                                     .base_counter = core ? 0x80285d28 : 0,
                                     .vi_intr_count = core ? 0x80285d2c : 0,
                                     .app_nmi_buffer = 0x8000031c};
    recomp_game_require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &scheduler) &&
                            recomp_platform_init(rt, &platform),
                        "Banjo native services initialization failed", 0);
    rt->interrupt_global_mask_address = core ? 0x80277130 : 0x800050f0;
}

static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == 0x8023df9c && !idle_configured) {
        /* initThread_entry starts at priority zero but first creates the PI
         * manager and main thread. Only after osStartThread dispatches main
         * (call 0x80240c50) does it reach its endless branch at 0x80240c58.
         * Keep that initialization and let the host wait instead of spinning. */
        recomp_scheduler_set_idle_thread(rt, 0x8027ed10);
        idle_configured = true;
    }
    if (address == 0x80263c10 && !core_services) {
        /* The original core1 entry has now cleared its BSS. No guest thread
         * exists yet: switch from the boot SDK's globals to the core1 instance. */
        recomp_platform_destroy(rt);
        recomp_scheduler_destroy(rt);
        recomp_clock_destroy(rt);
        services(rt, true);
        core_services = true;
    }
    if (address == 0x80253050) {
        /* Keep the resident cores; the shared level slot must never retain
         * executable targets from the previous image during a DMA/decode. */
        for (size_t n = 2; n < sizeof(banjo_images) / sizeof(banjo_images[0]); n++)
            rt->section_loaded[banjo_images[n].section] = false;
    }
}

static uint32_t activate_image(Runtime *rt, uint32_t address) {
    bool needs_image = false;
    for (size_t n = 0; n < sizeof(banjo_images) / sizeof(banjo_images[0]); n++) {
        if (address < banjo_images[n].address || address - banjo_images[n].address >= banjo_images[n].size)
            continue;
        if (rt->section_loaded[banjo_images[n].section])
            return address;
        needs_image = true;
        if (matches_image(rt, n)) {
            rt->section_loaded[banjo_images[n].section] = true;
            return address;
        }
    }
    recomp_game_require(!needs_image, "Banjo loaded image differs from compiled overlay", address);
    return address;
}

static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "Banjo unsupported RSP task", task->words[0]);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "Banjo RSP task rejected", task->words[4]);
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
                        "Banjo runtime initialization failed", 0);
    wall_origin = 0;
    core_services = false;
    idle_configured = false;
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t n = 0; n < sizeof(boot_parameters) / sizeof(boot_parameters[0]); n++)
        recomp_store(&rt, boot_parameters[n][0], boot_parameters[n][1], 4);
    services(&rt, false);
    RecompIOConfig io = {.connected_mask = 1, .eeprom_size = 512, .eeprom_path = options.save};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend),
                        "Banjo IO/task initialization failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Banjo-Kazooie");
    recomp_game_require(renderer != NULL, "Banjo renderer initialization failed", 0);
    /* Preserve the game's steady 30 Hz output. Automatic 60 Hz matching
     * presents the logo's interpolated frames in uneven pairs. */
    recomp_renderer_set_frame_rate(renderer, 0);
    rt.prepare_call = prepare_call;
    rt.canonical_function_address = activate_image;
    wall_origin = recomp_game_monotonic();
    MipsContext ctx = {0};
    recomp_lookup(&rt, BOOT_ENTRY)(&rt, &ctx);
    recomp_game_require(core_services, "Banjo boot did not enter core1", 0x8023da20);
    /* Boot replaces the platform services with core1's SDK instance. Attach
     * the AI output sink afterward, before dispatching the initial thread;
     * destroying the boot platform would otherwise discard the sink. */
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options.mute,
                            .microcode = RECOMP_AUDIO_NAUDIO,
                            .task_profile = &banjo_audio_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "Banjo audio initialization failed", 0);
    uint64_t deadline = recomp_game_monotonic();
    for (unsigned frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "Banjo controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++) {
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        }
        recomp_game_require(recomp_game_present(renderer), "Banjo presentation failed", 0);
        deadline += 16666667;
        recomp_game_pace(&rt, deadline);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
