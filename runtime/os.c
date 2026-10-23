#include "runtime.h"
#include "runtime_debug.h"

static uint32_t argument(MipsContext *ctx, unsigned index) {
    return (uint32_t)ctx->gpr[4 + index];
}
static int32_t signed_argument(MipsContext *ctx, unsigned index) {
    return (int32_t)mips_i32(ctx->gpr[4 + index]);
}
static void result(MipsContext *ctx, int32_t value) {
    mips_set(ctx, 2, mips_s32((uint32_t)value));
}

void hook_os_disable_int(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, rt->interrupts_disabled ? 0 : 1);
    rt->interrupts_disabled = true;
}
void hook_os_restore_int(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    rt->interrupts_disabled = !(argument(ctx, 0) & 1);
    if (!rt->interrupts_disabled)
        recomp_checkpoint(rt);
}
void hook_os_set_int_mask(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t previous = (rt->interrupt_mask & ~1u) | (rt->interrupts_disabled ? 0u : 1u);
    rt->interrupt_mask = argument(ctx, 0);
    rt->interrupts_disabled = !(rt->interrupt_mask & 1);
    result(ctx, (int32_t)previous);
    if (!rt->interrupts_disabled)
        recomp_checkpoint(rt);
}

/* SDK variant with __OSGlobalIntMask filtering. Its MTC0 sequence preserves
 * Status.IE while replacing only IM bits; RCP bits use the same global mask. */
void hook_os_set_int_mask_global(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    if (!rt->interrupt_global_mask_address)
        recomp_fail("SDK global interrupt mask is not configured", ctx->pc);
    uint32_t global = (uint32_t)recomp_load(rt, rt->interrupt_global_mask_address, 4);
    uint32_t enabled = rt->interrupts_disabled ? 0u : 1u;
    uint32_t previous = (rt->interrupt_mask & UINT32_C(0xff00)) | enabled | (~global & UINT32_C(0xff00));
    uint32_t rcp = rt->interrupt_mask & UINT32_C(0x003f0000);
    if (rcp)
        rcp |= ~global & UINT32_C(0x003f0000);
    rt->interrupt_mask = (argument(ctx, 0) & global & UINT32_C(0x003fff00)) | enabled;
    result(ctx, (int32_t)(previous | rcp));
    if (enabled)
        recomp_checkpoint(rt);
}

void hook_os_virtual_to_physical(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t address = argument(ctx, 0);
    if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000)) {
        mips_set(ctx, 2, address & UINT32_C(0x1fffffff));
        return;
    }
    uint32_t physical;
    if (rt->resolve_address && rt->resolve_address(rt, address, 1, false, &physical)) {
        mips_set(ctx, 2, recomp_ram_address(rt, physical, 1));
        return;
    }
    for (unsigned i = 0; i < 32; i++) {
        const RecompPageMapping *m = &rt->page_mappings[i];
        if (m->installed && address >= m->virtual_base &&
            (uint64_t)address < (uint64_t)m->virtual_base + 2 * m->page_size) {
            mips_set(ctx, 2, recomp_ram_address(rt, address, 1));
            return;
        }
    }
    recomp_fail("osVirtualToPhysical has no native page mapping", address);
}

void hook_os_create_thread(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    /* o32: arguments five and six occupy the caller's outgoing stack area. */
    uint32_t sp = (uint32_t)ctx->gpr[29];
    uint32_t stack = (uint32_t)recomp_load(rt, sp + 16, 4);
    int32_t priority = (int32_t)mips_i32(recomp_load(rt, sp + 20, 4));
    recomp_thread_create(rt, argument(ctx, 0), signed_argument(ctx, 1), argument(ctx, 2), argument(ctx, 3), stack,
                         priority);
}
void hook_os_start_thread(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_thread_start(rt, argument(ctx, 0));
}
void hook_os_stop_thread(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_thread_stop(rt, argument(ctx, 0));
}
void hook_os_destroy_thread(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_thread_destroy(rt, argument(ctx, 0));
}
void hook_os_yield_thread(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)ctx;
    recomp_thread_yield(rt);
}
void hook_os_get_thread_pri(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, recomp_thread_priority(rt, argument(ctx, 0)));
}
void hook_os_get_thread_id(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, recomp_thread_id(rt, argument(ctx, 0)));
}
void hook_os_set_thread_pri(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_thread_set_priority(rt, argument(ctx, 0), signed_argument(ctx, 1));
}
void hook_os_create_mesg_queue(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_queue_create(rt, argument(ctx, 0), argument(ctx, 1), signed_argument(ctx, 2));
}
void hook_os_send_mesg(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, recomp_queue_send(rt, argument(ctx, 0), argument(ctx, 1), signed_argument(ctx, 2), false));
}
void hook_os_jam_mesg(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, recomp_queue_send(rt, argument(ctx, 0), argument(ctx, 1), signed_argument(ctx, 2), true));
}
void hook_os_recv_mesg(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, recomp_queue_recv(rt, argument(ctx, 0), argument(ctx, 1), signed_argument(ctx, 2)));
}
void hook_os_set_event_mesg(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    recomp_event_bind(rt, argument(ctx, 0), argument(ctx, 1), argument(ctx, 2));
}
void hook_os_writeback_dcache_all(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    /* CPU-generated code and native backends share coherent host RAM. */
    (void)rt;
    (void)ctx;
}
void hook_os_cache_range(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    /* Cache instructions operate on cache tags, without reading the addressed
     * RAM. SDK callers also pass ROM aliases and entire virtual ranges.
     * Native CPU and backend snapshots already provide coherent storage. */
    (void)rt;
    (void)ctx;
}
void hook_os_dp_set_status(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    /* Native task submission owns RDP execution; hardware freeze/flush and
     * performance-counter requests do not leave a pending MMIO operation. */
    (void)rt;
    (void)ctx;
}
void hook_os_dp_get_status(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, 0);
}
void hook_os_dp_get_counters(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    for (unsigned i = 0; i < 4; i++)
        recomp_store(rt, argument(ctx, 0) + i * 4, 0, 4);
}
