#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "game_config.h"
#include "pager.h"

static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t now_ns, wall_origin;
static bool tlb_installed, application_started, system_initialized;
static uint32_t interrupt_mask = 0x3fff01;
static void shadowman_prepare_call(Runtime *rt, uint32_t address) {
    if (address >= BOOT_GAME_VRAM && address < BOOT_GAME_VRAM + BOOT_GAME_SIZE) {
        for (unsigned i = 0; i < 16; i++) {
            uint8_t expected = rt->rom[BOOT_GAME_ROM_OFFSET + address - BOOT_GAME_VRAM + i];
            recomp_game_require(recomp_load(rt, address + i, 1) == expected,
                                "Shadow Man executable page differs from translated ROM", address + i);
        }
    }
}
void hook_shadowman_cache(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    (void)ctx;
}
void hook_shadowman_int_mask(Runtime *rt, MipsContext *ctx) {
    uint32_t previous = interrupt_mask;
    interrupt_mask = (uint32_t)ctx->gpr[4];
    rt->interrupts_disabled = !(interrupt_mask & 1);
    mips_set(ctx, 2, mips_s32(previous));
}
void hook_shadowman_ai_busy(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, recomp_ai_state(rt).queued == 2 ? 1 : 0);
}
void hook_shadowman_map_tlb(Runtime *rt, MipsContext *ctx) {
    uint32_t index = (uint32_t)ctx->gpr[4], sp = (uint32_t)ctx->gpr[29];
    uint32_t even = (uint32_t)ctx->gpr[7], odd = (uint32_t)recomp_load(rt, sp + 16, 4);
    recomp_game_require(index < 32 && recomp_load(rt, sp + 20, 4) == 7, "Shadow Man TLB requires a global SDK mapping",
                        ctx->pc);
    if (even == UINT32_MAX && odd == UINT32_MAX)
        rt->page_mappings[index] = (RecompPageMapping){0};
    else
        recomp_map_pages(rt, index, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], even, odd);
}
void hook_shadowman_unmap_tlb(Runtime *rt, MipsContext *ctx) {
    unsigned index = (unsigned)ctx->gpr[4];
    recomp_game_require(index < 32, "Shadow Man invalid TLB index", index);
    rt->page_mappings[index] = (RecompPageMapping){0};
}
void hook_shadowman_motor_init(Runtime *rt, MipsContext *ctx) {
    hook_os_motor_init(rt, ctx);
}
void hook_shadowman_motor_stop(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    mips_set(ctx, 2, 5); /* PFS_ERR_INVALID: osMotorInit could not find an accessory. */
}
void hook_shadowman_pi_raw_dma(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = (uint32_t)ctx->gpr[5], address = (uint32_t)ctx->gpr[6], bytes = (uint32_t)ctx->gpr[7];
    if (offset >= 0xb0000000 && offset < 0xb2000000)
        offset -= 0xb0000000;
    if (offset >= 0x10000000 && offset < 0x12000000)
        offset -= 0x10000000;
    recomp_game_require(ctx->gpr[4] == 0 && offset <= rt->rom_size && bytes <= rt->rom_size - offset,
                        "Shadow Man unsupported raw PI DMA", offset);
    recomp_copy_to_ram(rt, address, rt->rom + offset, bytes);
    if (offset == BOOT_GAME_ROM_OFFSET && bytes >= ((BOOT_GAME_SIZE - 1) & ~0xfffu))
        rt->section_loaded[BOOT_GAME_SECTION] = true;
    recomp_game_require(recomp_event_post(rt, 8), "Shadow Man PI completion FIFO full", address);
    mips_set(ctx, 2, 0);
}
void hook_shadowman_boot_tlb(Runtime *rt, MipsContext *ctx) {
    recomp_game_require(!tlb_installed && ctx->gpr[4] == 30, "Shadow Man boot TLB ABI mismatch", ctx->pc);
    /* Original inline unmap loop followed by a global 1 MiB odd-page mapping.
     * EntryLo global bits make the SDK's ASID argument immaterial here. */
    recomp_unmap_pages(rt);
    recomp_map_pages(rt, 31, 0x1fe000, 0, UINT32_MAX, 0);
    recomp_game_require(recomp_ram_address(rt, BOOT_SYSTEM_ENTRY, 4) == BOOT_SYSTEM_ENTRY - 0x100000,
                        "Shadow Man low virtual mapping mismatch", BOOT_SYSTEM_ENTRY);
    tlb_installed = true;
    mips_set(ctx, 31, mips_s32(0x800004ac));
    recomp_lookup(rt, BOOT_SYSTEM_ENTRY)(rt, ctx);
    recomp_fail("Shadow Man system startup returned", BOOT_SYSTEM_ENTRY);
}
void hook_shadowman_get_sr(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, 0x2400ff00 | (rt->interrupts_disabled ? 0 : 1));
}
void hook_shadowman_start_thread(Runtime *rt, MipsContext *ctx) {
    hook_os_start_thread(rt, ctx);
    if (!application_started) {
        application_started = true;
        /* IPL has no schedulable thread on hardware. Remove our host-only
         * transfer thread when the original first thread is dispatched. */
        recomp_thread_stop(rt, 0);
        recomp_fail("Shadow Man IPL thread resumed", ctx->pc);
    }
}
void hook_shadowman_initialize(Runtime *rt, MipsContext *ctx) {
    recomp_game_require(tlb_installed && !system_initialized, "Shadow Man repeated OS initialization", ctx->pc);
    system_initialized = true;
    hook_os_initialize(rt, ctx);
    recomp_store(rt, 0x800285c0, 48681812, 4); /* Original NTSC osViClock. */
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? recomp_game_monotonic() - wall_origin : now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    (void)opaque;
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
    Runtime rt;
    recomp_game_require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "runtime init failed", 0);
    uint8_t *rom = recomp_game_rom(&rt, &options, BOOT_ROM_SIZE, boot_sha1);
    rt.prepare_call = shadowman_prepare_call;
    rt.resolve_address = shadowman_resolve_address;
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail = 0x800285d0,
                                       .run_queue = 0x800285d8,
                                       .active_queue = 0x800285dc,
                                       .running_thread = 0x800285e0,
                                       .event_table = 0x800503b0,
                                       .stop_before_entry = 0,
                                       .fpu_fr1 = true};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250, .vi_clock = 0x800285c0, .app_nmi_buffer = 0x8000031c};
    recomp_game_require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler) &&
                            recomp_platform_init(&rt, &platform),
                        "Shadow Man native services init failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options.save}};
    RecompTaskBackend backend = {.submit = submit};
    recomp_game_require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Shadow Man IO/task init failed",
                        0);
    renderer = recomp_renderer_create_named(&rt, options.data, "Shadow Man");
    recomp_game_require(renderer != NULL, "renderer initialization failed", 0);
    /* Preserve the game's VI-driven timing for its buffered startup frames. */
    recomp_renderer_set_frame_rate(renderer, 0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options.mute,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &shadowman_audio_task_profile};
    audio = recomp_game_audio_create(&rt, &ac);
    recomp_game_require(audio != NULL, "audio initialization failed", 0);
    wall_origin = recomp_game_monotonic();
    recomp_thread_create(&rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(&rt, 0x803ff000);
    uint64_t next_present = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(renderer, &pad))
            break;
        recomp_game_require(recomp_game_input(&rt, 0, &pad), "controller update failed", 0);
        for (unsigned batch = 0; batch < 32; batch++) {
            if (recomp_scheduler_run(&rt, 32) != RECOMP_RUN_BUDGET)
                break;
            RecompThreadSnapshot threads[32];
            size_t count = recomp_thread_snapshots(&rt, threads, 32);
            bool work = false;
            for (size_t i = 0; i < count && i < 32; i++)
                if (threads[i].state == RECOMP_READY && threads[i].id != 0)
                    work = true;
            if (!work)
                break;
        }
        recomp_game_require(recomp_game_present(renderer), "presentation failed", 0);
        next_present += 16666667;
        recomp_game_sleep(next_present);
    }
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
