#pragma once
void hook_ge_tlb_hi(Runtime *rt, MipsContext *ctx) {
    unsigned index = (unsigned)ctx->gpr[4];
    require(index < 32, "GoldenEye TLB index invalid", index);
    RecompPageMapping *m = &rt->page_mappings[index];
    mips_set(ctx, 2, mips_s32(m->installed ? m->virtual_base : 0x80000000));
}
void hook_ge_int_mask(Runtime *rt, MipsContext *ctx) {
    uint32_t previous = interrupt_mask;
    interrupt_mask = (uint32_t)ctx->gpr[4];
    rt->interrupts_disabled = !(interrupt_mask & 1);
    mips_set(ctx, 2, mips_s32(previous));
}
void hook_ge_dp_counters(Runtime *rt, MipsContext *ctx) {
    /* No RDP work precedes the startup graphics boundary. */
    uint32_t address = (uint32_t)ctx->gpr[4];
    for (unsigned i = 0; i < 4; i++)
        recomp_store(rt, address + i * 4, 0, 4);
}
void hook_ge_dp_status(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    (void)ctx;
    /* RT64 executes RDP tasks synchronously; counter reset, freeze and flush
     * requests do not leave an asynchronous hardware operation outstanding. */
}
void hook_ge_vi_framebuffer(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, mips_s32(recomp_vi_state(rt).framebuffer));
}
void hook_ge_vi_repeat_line(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    require((uint8_t)ctx->gpr[4] == 0, "GoldenEye repeated VI line requires native support", ctx->pc);
}
void hook_ge_set_compare(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    (void)ctx;
    /* Native osSetTimer owns deadlines; CPU Compare is never scheduled. */
}
