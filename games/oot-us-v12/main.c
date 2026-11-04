#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "jpeg.h"
#include "controls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game_config.h"
#include "overlays.h"

static RecompRenderer *renderer;
static bool window_closed;
static RecompAudio *audio;
static uint64_t now_ns, wall_origin;
static void poll_controls(Runtime *rt, void *opaque) {
    (void)opaque;
    RecompPad pad = {.connected = true};
    if (!oot_controls_poll(rt, renderer, &pad))
        window_closed = true;
    else
        recomp_game_require(recomp_game_input(rt, 0, &pad), "OoT controller update failed", 0);
}
static void oot_prepare_call(Runtime *rt, uint32_t address) {
#if BOOT_STARTUP
    /* mainproc's first call is osCreateViManager. osStartThread preempts IPL,
     * so its handoff hook may not return until graphics is already running.
     * Retire the parked IPL thread here, before OOT reuses its OSThread and
     * stack storage for the framebuffer at 0x803da800..0x80400000. */
    if (address == 0x80005f20 && recomp_thread_id(rt, 0) == BOOT_MAIN_ID &&
        (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x80000650)
        recomp_thread_destroy(rt, 0x803ff000);
    if (address == 0x80003040 && !rt->section_loaded[2] &&
        (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800adacc) {
        uint8_t *image = malloc(OOT_DD_SIZE), digest[32];
        unsigned length = 0;
        recomp_game_require(image != NULL, "OOT DD verification allocation failed", 0);
        for (uint32_t i = 0; i < OOT_DD_SIZE; i++)
            image[i] = (uint8_t)recomp_load(rt, 0x801c7740 + i, 1);
        bool valid = EVP_Digest(image, OOT_DD_SIZE, digest, &length, EVP_sha256(), NULL) && length == 32 &&
                     !memcmp(digest, oot_dd_sha256, 32);
        free(image);
        recomp_game_require(valid, "OOT DD code fingerprint mismatch", 0x801c7740);
        rt->section_loaded[2] = true;
    }
    if (address == 0x800adaf8 && !recomp_load(rt, 0x80121ae2, 1))
        rt->section_loaded[2] = false;
    /* The pinned main-thread loader has completed its synchronous code DMA
     * before this osGetTime call. Activate only after verifying the whole image. */
    if (address == 0x80004ad0 && !rt->section_loaded[1] &&
        (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800005ec) {
        uint8_t *image = malloc(OOT_CODE_SIZE), digest[32];
        unsigned length = 0;
        recomp_game_require(image != NULL, "OOT code verification allocation failed", 0);
        for (uint32_t i = 0; i < OOT_CODE_SIZE; i++)
            image[i] = (uint8_t)recomp_load(rt, 0x800116e0 + i, 1);
        bool valid = EVP_Digest(image, OOT_CODE_SIZE, digest, &length, EVP_sha256(), NULL) && length == 32 &&
                     !memcmp(digest, oot_code_sha256, 32);
        free(image);
        recomp_game_require(valid, "OOT decompressed code fingerprint mismatch", 0x800116e0);
        rt->section_loaded[1] = true;
    }
#endif
}
void recomp_oot_cic_boot_task(Runtime *rt, MipsContext *ctx) {
    recomp_store(rt, 0x80008ef8, 0, 4);
    recomp_store(rt, 0x80008efc, 0, 4);
    mips_set(ctx, 2, 0);
}
void recomp_oot_start_thread(Runtime *rt, MipsContext *ctx) {
    recomp_os_start_thread(rt, ctx);
    /* Fallback if the handoff did not preempt. The normal path retires IPL
     * from mainproc's first call above; IPL must never resume guest code. */
    if (recomp_thread_id(rt, 0) == -1)
        recomp_thread_destroy(rt, 0);
}

static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? recomp_game_monotonic() - wall_origin : now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    if (task->words[0] == 4) {
        recomp_game_require(recomp_jpeg_decode_ps(rt, task, &oot_jpeg_profile), "OOT JPEG task rejected",
                            task->words[4]);
        return recomp_task_complete(rt, task->token, RECOMP_SP_DONE);
    }
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported RSP task type", task->guest_address);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(ok, "game task rejected", task->words[4]);
    return true;
}
int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    oot_fov_init();
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = oot_prepare_call;
    rt.canonical_function_address = oot_canonical;
    rt.relocated_address = oot_relocated;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .idle_thread = BOOT_MAIN_OBJECT,
                                       .stop_before_entry = 0};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .sram_size = 32768,
                                     .sram_path = options.save,
                                     .cart_handle = BOOT_CART_HANDLE,
                                     .drive_handle = BOOT_DRIVE_HANDLE,
                                     .app_nmi_buffer = 0x8000031c};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Ocarina of Time native services init failed", 0);
    /* The priority-15 pad thread continuously reads. A 1 ms serial transfer
     * allows priority-10 game code to run between controller reads. */
    RecompIOConfig io = {.connected_mask = 1, .si_transfer_ticks = BOOT_COUNTER_HZ / 1000};
    RecompTaskBackend backend = {.submit = submit, .additional_task_types = 1u << 4};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend),
                        "Ocarina of Time IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "The Legend of Zelda: Ocarina of Time");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    recomp_renderer_set_fov_hook(renderer, oot_fov_adjust);
    oot_controls_init(poll_controls);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options.mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_NEAD_OOT,
                            .task_profile = &oot_audio_task_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    wall_origin = recomp_game_monotonic();
    recomp_thread_create(&rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(&rt, 0x803ff000);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !window_closed && (!options.frames || frame < options.frames); frame++) {
        RecompPad pad = {.connected = true};
        if (!oot_controls_poll(&rt, renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++) {
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
            RecompThreadSnapshot threads[32];
            size_t count = recomp_thread_snapshots(&rt, threads, 32);
            bool work = false;
            for (size_t i = 0; i < count && i < 32; i++)
                if (threads[i].state == RECOMP_READY && threads[i].id != BOOT_MAIN_ID)
                    work = true;
            if (!work)
                break;
        }
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
