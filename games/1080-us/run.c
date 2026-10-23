#include "run.h"
#include "audio.h"
#include "generated.h"
#include "renderer.h"
#include "snowboarding_config.h"
#include <openssl/sha.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define require recomp_game_require

static bool bootup_verified;
static RecompAudio *audio;
static RecompRenderer *renderer;
static uint64_t wall_origin;
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? recomp_game_monotonic() - wall_origin : 0;
}
#ifdef RECOMP_DEBUG
static bool bss_verified;
#endif

static uint32_t activate_module(Runtime *rt, uint32_t address) {
    for (size_t n = 0; n < sizeof(uso_import_calls) / sizeof(uso_import_calls[0]); n++) {
        if (uso_import_calls[n].target == address)
            require(recomp_load(rt, uso_import_calls[n].pc, 4) == uso_import_calls[n].word,
                    "1080 USO import is not bound to compiled target", uso_import_calls[n].pc);
    }
    /* Recheck small scene modules at every entry, including after a guest
     * unload/reload. Only the measured initial allocation is compiled. */
    for (size_t n = 0; n < sizeof(uso_modules) / sizeof(uso_modules[0]); n++) {
        if (address < uso_modules[n].address || address - uso_modules[n].address >= uso_modules[n].size)
            continue;
        uint8_t *image = malloc(uso_modules[n].size);
        uint8_t digest[SHA256_DIGEST_LENGTH];
        require(image != NULL, "1080 scene verification allocation failed", address);
        for (uint32_t i = 0; i < uso_modules[n].size; i++)
            image[i] = (uint8_t)recomp_load(rt, uso_modules[n].address + i, 1);
        SHA256(image, uso_modules[n].size, digest);
        free(image);
        rt->section_loaded[uso_modules[n].section] = !memcmp(digest, uso_modules[n].sha256, sizeof(digest));
        require(rt->section_loaded[uso_modules[n].section], "1080 loaded scene differs from compiled USO", address);
    }
    return address;
}

static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == 0x80001088 && !bootup_verified) {
        uint8_t *image = malloc(BOOT_BOOTUP_SIZE);
        uint8_t digest[SHA256_DIGEST_LENGTH];
        require(image != NULL, "1080 bootup text verification allocation failed", BOOT_BOOTUP_ADDRESS);
        for (uint32_t i = 0; i < BOOT_BOOTUP_SIZE; i++)
            image[i] = (uint8_t)recomp_load(rt, BOOT_BOOTUP_ADDRESS + i, 1);
        SHA256(image, BOOT_BOOTUP_SIZE, digest);
        free(image);
        require(!memcmp(digest, bootup_sha256, sizeof(digest)),
                "1080 guest bootup USO differs from compiled image", BOOT_BOOTUP_ADDRESS);
        rt->section_loaded[BOOT_BOOTUP_SECTION] = true;
        rt->interrupt_global_mask_address = BOOT_GLOBAL_INTERRUPT_MASK;
        bootup_verified = true;
    }
#ifdef RECOMP_DEBUG
    if (address != BOOT_KERNEL_MAIN)
        return;
    for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
        require(recomp_load(rt, a, 4) == 0, "1080 kernel BSS clear mismatch", a);
    require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5,
            "1080 kernel BSS overrun", BOOT_BSS_END);
    bss_verified = true;
#endif
}

static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "1080 unsupported RSP task type", task->words[0]);
    bool ok = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    require(ok, "1080 RSP task rejected", task->words[4]);
    return true;
}

/* Run the original kernel, checked USO modules, and shared device services. */
int snowboarding_run(const RecompGameOptions *options) {
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                recomp_boot_sections, recomp_section_count),
            "1080 runtime initialization failed", 0);
    bootup_verified = false;
    wall_origin = 0;
#ifdef RECOMP_DEBUG
    bss_verified = false;
#endif
    uint8_t *rom = recomp_game_rom(&rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_DIAGNOSTIC_THREAD_TAIL};
    RecompPlatformConfig platform = {.vi_period_ticks = BOOT_VI_PERIOD_TICKS,
                                     .clock_rate = BOOT_CLOCK_RATE, .vi_clock = BOOT_VI_CLOCK,
                                     .final_rom = BOOT_FINAL_ROM, .app_nmi_buffer = 0x8000031c,
                                     .sram_size = BOOT_SRAM_SIZE, .sram_path = options->save};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                recomp_platform_init(&rt, &platform),
            "1080 native services initialization failed", 0);
#ifdef RECOMP_DEBUG
    for (uint32_t a = BOOT_BSS_START; a < BOOT_BSS_END; a += 4)
        recomp_store(&rt, a, 0xa5a5a5a5, 4);
    recomp_store(&rt, BOOT_BSS_END, 0xa5a5a5a5, 4);
#endif
    RecompIOConfig io = {.connected_mask = 1};
    RecompTaskBackend backend = {.submit = submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "1080 IO/task initialization failed", 0);
    renderer = recomp_renderer_create_named(&rt, options->data, "1080 Snowboarding");
    require(renderer != NULL, "1080 renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled = true, .playback = !options->mute,
                            .microcode = RECOMP_AUDIO_NEAD_1080, .task_profile = &snowboarding_audio_profile};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "1080 audio initialization failed", 0);
    rt.prepare_call = prepare_call;
    rt.canonical_function_address = activate_module;
    wall_origin = recomp_game_monotonic();
    MipsContext ctx = {0};
    recomp_lookup(&rt, BOOT_ENTRY)(&rt, &ctx);
#ifdef RECOMP_DEBUG
    require(bss_verified, "1080 kernel did not reach checked BSS handoff", BOOT_KERNEL_MAIN);
#endif
    uint64_t deadline = recomp_game_monotonic();
    for (unsigned frame = 0; !options->frames || frame < options->frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
        require(recomp_input_set(&rt, 0, &pad), "1080 controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++) {
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        }
        require(recomp_renderer_present(renderer), "1080 presentation failed", 0);
        deadline += 16666667;
        recomp_game_pace(&rt, deadline);
    }
#ifdef RECOMP_DEBUG
    RecompRendererStats stats = recomp_renderer_stats(renderer);
    fprintf(stderr, "1080: graphics tasks=%llu presentations=%llu\n",
            (unsigned long long)stats.graphics_tasks, (unsigned long long)stats.presentations);
#endif
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
