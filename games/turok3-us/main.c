#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include "../turok2-us/acclaim_pager.h"
#include "../turok2-us/acclaim_controls.h"
#include <stdlib.h>
#include "game_config.h"

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t wall_origin;
static bool boot_retired;

static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? recomp_game_monotonic() - wall_origin : 0;
}

static void prepare_call(Runtime *rt, uint32_t address) {
    /* mainproc has begun on its real OSThread. Retire the synthetic boot
     * continuation before the allocator can reuse its reserved RAM. */
    if (!boot_retired && address == BOOT_MAIN_HANDOFF_CALL && recomp_thread_id(rt, 0) == BOOT_MAIN_ID) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        recomp_game_require(ctx == recomp_thread_context(rt, BOOT_MAIN_THREAD) &&
                                (uint32_t)ctx->gpr[31] == BOOT_MAIN_HANDOFF_RETURN,
                            "unexpected Turok 3 boot handoff", address);
        recomp_thread_destroy(rt, BOOT_BOOT_OBJECT);
        boot_retired = true;
    }
}

void recomp_turok3_unmap_page(Runtime *rt, MipsContext *ctx) {
    uint32_t index = (uint32_t)ctx->gpr[4];
    recomp_game_require(index < 32, "Turok 3 invalid page index", index);
    rt->page_mappings[index] = (RecompPageMapping){0};
    if (index == 0)
        rt->section_loaded[BOOT_VIRTUAL_SECTION] = false;
}

void recomp_turok3_map_pages(Runtime *rt, MipsContext *ctx) {
    uint32_t even = (uint32_t)ctx->gpr[7];
    uint32_t odd = (uint32_t)recomp_load(rt, (uint32_t)ctx->gpr[29] + 16, 4);
    uint32_t flags = (uint32_t)recomp_load(rt, (uint32_t)ctx->gpr[29] + 20, 4);
    if (even == UINT32_MAX && odd == UINT32_MAX) {
        recomp_turok3_unmap_page(rt, ctx);
        return;
    }
    recomp_game_require((flags & 7) == 7, "Turok 3 unsupported page permissions", flags);
    recomp_map_pages(rt, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], even, odd);
    if ((uint32_t)ctx->gpr[6] == BOOT_VIRTUAL_ADDRESS) {
        /* VM_Initialize's Expansion Pak path loads the complete executable
         * bank through CLoader before installing this wired mapping. */
        recomp_game_require((uint32_t)ctx->gpr[4] == 0 && (uint32_t)ctx->gpr[5] == 0x1fe000 &&
                                even == (BOOT_VIRTUAL_LOAD_ADDRESS & 0x1fffffff) && odd == UINT32_MAX,
                            "Turok 3 executable bank mapping mismatch", even);
        for (uint32_t i = 0; i < BOOT_VIRTUAL_LOAD_SIZE; i++)
            recomp_game_require(recomp_load(rt, BOOT_VIRTUAL_LOAD_ADDRESS + i, 1) ==
                                    rt->rom[BOOT_VIRTUAL_ROM_OFFSET + i],
                                "Turok 3 executable bank differs from ROM", BOOT_VIRTUAL_ADDRESS + i);
        rt->section_loaded[BOOT_VIRTUAL_SECTION] = true;
    }
}

void recomp_turok3_boot(Runtime *rt, MipsContext *ctx) {
    /* The complete SN64 stub installs one global pair of 1 MiB pages before
     * its indirect call to main. Data retains its direct KSEG aliases. */
    recomp_map_pages(rt, 31, 0x1fe000, 0x00200000, 0, 0x100000);
    mips_set(ctx, 29, mips_s32(0x803fffc0));
    mips_set(ctx, 31, mips_s32(0x80000484));
    recomp_lookup(rt, 0x0020aa50)(rt, ctx);
    recomp_fail("Turok 3 boot returned before the main-thread handoff", 0x0020aa50);
}

static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "unsupported Turok 3 RSP task",
                        task->guest_address);
    bool okay = task->words[0] == 2 ? recomp_audio_submit(audio, task) : recomp_renderer_submit(renderer, task);
    recomp_game_require(okay, "Turok 3 task profile rejected", task->words[4]);
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
    rt.prepare_call = prepare_call;
    rt.resolve_address = acclaim_resolve_address;
    rt.interrupt_global_mask_address = BOOT_INTERRUPT_GLOBAL_MASK;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = BOOT_THREAD_TAIL,
                                       .run_queue = BOOT_RUN_QUEUE,
                                       .active_queue = BOOT_ACTIVE_QUEUE,
                                       .running_thread = BOOT_RUNNING_THREAD,
                                       .event_table = BOOT_EVENT_TABLE,
                                       .idle_thread = BOOT_IDLE_THREAD,
                                       .fpu_fr1 = true};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .vi_clock = BOOT_VI_CLOCK,
                                     .clock_rate = BOOT_CLOCK_RATE,
                                     .final_rom = BOOT_FINAL_ROM,
                                     .current_time = BOOT_CURRENT_TIME,
                                     .base_counter = BOOT_BASE_COUNTER,
                                     .app_nmi_buffer = 0x8000031c};
    wall_origin = recomp_game_monotonic();
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Turok 3 services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options.save}};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Turok 3 IO init failed", 0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Turok 3: Shadow of Oblivion");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    acclaim_controls_init(renderer);
    RecompAudioConfig config = {
        .output_enabled = true, .playback = !options.mute, .task_profile = &turok3_audio_profile};
    audio = recomp_game_audio_create(&rt, &config);
    recomp_game_require(audio != NULL, "Turok 3 audio initialization failed", 0);
    recomp_thread_create(&rt, BOOT_BOOT_OBJECT, -1, BOOT_ENTRY, 0, BOOT_BOOT_STACK, 0);
    recomp_thread_start(&rt, BOOT_BOOT_OBJECT);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!acclaim_controls_poll(&rt, renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "Turok 3 input update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++)
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
        recomp_game_require(recomp_game_present(renderer), "Turok 3 presentation failed", 0);
        next_present = recomp_game_next_deadline(next_present, 16666667);
        recomp_game_pace(&rt, next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
