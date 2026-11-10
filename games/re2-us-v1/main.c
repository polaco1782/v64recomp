#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "video.h"
#include "game_config.h"
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
static unsigned loading_source;
static unsigned pending_patch = UINT32_MAX;

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
void recomp_re2_disable_watch(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    recomp_game_require(((uint32_t)ctx->gpr[4] & 7) == 0, "RE2 enabled CPU watchpoints require native support",
                        ctx->pc);
}
void recomp_re2_start_thread(Runtime *rt, MipsContext *ctx) {
    bool handoff = recomp_thread_id(rt, 0) == -1;
    if (handoff)
        recomp_game_require((uint32_t)ctx->gpr[4] == 0x8010b700 && (uint32_t)ctx->gpr[31] == 0x8004d388,
                            "unexpected RE2 IPL thread start", ctx->pc);
    recomp_os_start_thread(rt, ctx);
    if (handoff)
        recomp_thread_stop(rt, BOOT_BOOT_OBJECT);
}
void recomp_re2_cache_allocate(Runtime *rt, MipsContext *ctx) {
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
static bool image_matches(Runtime *rt, uint32_t address, uint32_t size, const uint8_t expected[32]) {
    uint8_t *image = malloc(size), digest[32];
    unsigned length = 0;
    recomp_game_require(image != NULL, "cannot verify RE2 overlay", address);
    (void)recomp_ram_address(rt, address, size);
    for (uint32_t i = 0; i < size; i++)
        image[i] = (uint8_t)recomp_load(rt, address + i, 1);
    bool ok =
        EVP_Digest(image, size, digest, &length, EVP_sha256(), NULL) && length == 32 && !memcmp(digest, expected, 32);
    free(image);
    return ok;
}
static void prepare_call(Runtime *rt, uint32_t address) {
    /* Lookup checks loaded sections before prepare_call. The actor dispatcher
     * runs after the original loader's DMA and patch loop, before its first
     * enemy lookup. Verify once here; doing so in high-priority SDK callbacks
     * while the loader is blocked would delay its PI/VI progress. */
    if (pending_patch != UINT32_MAX && address == 0x80065220) {
        recomp_game_require(image_matches(rt, re2_overlays[pending_patch].address, re2_overlays[pending_patch].size,
                                          re2_overlays[pending_patch].sha256),
                            "RE2 patched overlay differs from compiled image", re2_overlays[pending_patch].address);
        rt->section_loaded[re2_overlays[pending_patch].section] = true;
        pending_patch = UINT32_MAX;
    }
    const MipsContext *call =
        address == 0x800045dc || address == BOOT_INFLATE_CLEANUP ? recomp_thread_context(rt, 0) : NULL;
    if (address == 0x800045dc && call &&
        ((uint32_t)call->gpr[31] == 0x8004cc38 || (uint32_t)call->gpr[31] == 0x80032840)) {
        recomp_game_require(!loading_compressed, "concurrent RE2 overlay decompression", address);
        loading_address = (uint32_t)call->gpr[6];
        loading_compressed = (uint32_t)call->gpr[7];
        loading_size = 0;
        loading_source = UINT32_MAX;
        for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++)
            if (!re2_overlays[i].patched && re2_overlays[i].cartridge == (uint32_t)call->gpr[5] &&
                re2_overlays[i].compressed_size == loading_compressed) {
                loading_size = re2_overlays[i].size;
                loading_source = i;
            }
        (void)recomp_ram_address(rt, loading_address, loading_size);
        for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++) {
            if ((uint64_t)loading_address < (uint64_t)re2_overlays[i].address + re2_overlays[i].size &&
                (uint64_t)re2_overlays[i].address < (uint64_t)loading_address + loading_size)
                rt->section_loaded[re2_overlays[i].section] = false;
        }
    }
    if (address == BOOT_INFLATE_CLEANUP && call &&
        ((uint32_t)call->gpr[31] == 0x8004cc48 || (uint32_t)call->gpr[31] == 0x80032850)) {
        uint32_t stream = (uint32_t)call->gpr[4];
        uint32_t produced = (uint32_t)recomp_load(rt, stream + 0x14, 4);
        recomp_game_require(loading_compressed && produced && (!loading_size || produced == loading_size) &&
                                (uint64_t)loading_address + produced <= UINT32_MAX &&
                                recomp_load(rt, stream + 0xc, 4) == loading_address + produced,
                            "RE2 overlay inflater did not produce the complete image", loading_address);
        loading_size = produced;
        recomp_game_require(loading_source != UINT32_MAX &&
                                image_matches(rt, loading_address, loading_size, re2_overlays[loading_source].sha256),
                            "RE2 decompressed overlay has no verified compiled mapping", loading_address);
        if (re2_overlays[loading_source].address == loading_address)
            rt->section_loaded[re2_overlays[loading_source].section] = true;
        else {
            pending_patch = UINT32_MAX;
            for (unsigned i = 0; i < sizeof(re2_overlays) / sizeof(re2_overlays[0]); i++)
                if (re2_overlays[i].patched && re2_overlays[i].address == loading_address &&
                    re2_overlays[i].size == loading_size)
                    pending_patch = i;
            recomp_game_require(pending_patch != UINT32_MAX, "RE2 enemy slot has no compiled patch variant",
                                loading_address);
        }
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
                        "RE2 runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
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
                                     .app_nmi_buffer = 0x8000031c,
                                     .sram_size = 32768,
                                     .sram_path = options.save};
    wall_origin = recomp_game_monotonic();
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "RE2 services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1};
    RecompTaskBackend backend = {.submit = submit, .validate_custom_data = re2_video_validate};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "RE2 IO/task init failed", 0);
    renderer = recomp_renderer_create_named(&rt, "Resident Evil 2");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    /* Video writes framebuffer RAM without a graphics full-sync; VI-only
     * updates are presented directly from guest RAM. */
    RecompAudioConfig audio_config = {.output_enabled = true,
                                      .playback = !options.mute,
                                      .microcode = RECOMP_AUDIO_MUSYX_V1,
                                      .task_profile = &re2_audio_profile};
    audio = recomp_game_audio_create(&rt, &audio_config);
    recomp_game_require(audio != NULL, "RE2 audio output initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t deadline = recomp_game_monotonic();
    uint32_t frame = 0;
    for (; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "RE2 controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++)
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        recomp_game_require(recomp_game_present(renderer), "RE2 presentation failed", 0);
        deadline += 16666667;
        recomp_game_pace(&rt, deadline);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
