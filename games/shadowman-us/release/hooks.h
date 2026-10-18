#pragma once
static bool tlb_installed, application_started, system_initialized;
static uint32_t interrupt_mask=0x3fff01;
static void shadowman_prepare_call(Runtime *rt, uint32_t address) {
    if (address >= BOOT_GAME_VRAM && address < BOOT_GAME_VRAM + BOOT_GAME_SIZE) {
        for (unsigned i = 0; i < 16; i++) {
            uint8_t expected = rt->rom[BOOT_GAME_ROM_OFFSET + address - BOOT_GAME_VRAM + i];
            require(recomp_load(rt, address+i, 1) == expected,
                    "Shadow Man executable page differs from translated ROM", address+i);
        }
    }
}
void hook_shadowman_cache(Runtime *rt, MipsContext *ctx) { (void)rt; (void)ctx; }
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
    uint32_t even = (uint32_t)ctx->gpr[7], odd = (uint32_t)recomp_load(rt, sp+16, 4);
    require(index < 32 && recomp_load(rt, sp+20, 4) == 7,
            "Shadow Man TLB requires a global SDK mapping", ctx->pc);
    if (even == UINT32_MAX && odd == UINT32_MAX) rt->page_mappings[index] = (RecompPageMapping){0};
    else recomp_map_pages(rt, index, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], even, odd);
}
void hook_shadowman_unmap_tlb(Runtime *rt, MipsContext *ctx) {
    unsigned index = (unsigned)ctx->gpr[4];
    require(index < 32, "Shadow Man invalid TLB index", index);
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
    if (offset >= 0xb0000000 && offset < 0xb2000000) offset -= 0xb0000000;
    if (offset >= 0x10000000 && offset < 0x12000000) offset -= 0x10000000;
    require(ctx->gpr[4] == 0 && offset <= rt->rom_size && bytes <= rt->rom_size-offset,
            "Shadow Man unsupported raw PI DMA", offset);
    recomp_copy_to_ram(rt, address, rt->rom+offset, bytes);
    if (offset == BOOT_GAME_ROM_OFFSET && bytes >= ((BOOT_GAME_SIZE-1) & ~0xfffu))
        rt->section_loaded[BOOT_GAME_SECTION] = true;
    require(recomp_event_post(rt, 8), "Shadow Man PI completion FIFO full", address);
    mips_set(ctx, 2, 0);
}
void hook_shadowman_boot_tlb(Runtime *rt, MipsContext *ctx) {
    require(!tlb_installed && ctx->gpr[4] == 30, "Shadow Man boot TLB ABI mismatch", ctx->pc);
#ifdef RECOMP_DEBUG
    debug_boot_bss(rt);
#endif
    /* Original inline unmap loop followed by a global 1 MiB odd-page mapping.
     * EntryLo global bits make the SDK's ASID argument immaterial here. */
    recomp_unmap_pages(rt);
    recomp_map_pages(rt, 31, 0x1fe000, 0, UINT32_MAX, 0);
    require(recomp_ram_address(rt, BOOT_SYSTEM_ENTRY, 4) == BOOT_SYSTEM_ENTRY - 0x100000,
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
    require(tlb_installed && !system_initialized, "Shadow Man repeated OS initialization", ctx->pc);
    system_initialized = true;
#ifdef RECOMP_DEBUG
    debug_system_bss(rt);
#endif
    hook_os_initialize(rt, ctx);
    recomp_store(rt, 0x800285c0, 48681812, 4); /* Original NTSC osViClock. */
}
