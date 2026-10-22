#pragma once
void hook_nightmare_start_thread(Runtime *rt, MipsContext *ctx) {
    /* mainproc starts gameproc here, before it can submit graphics. The IPL
     * osStartThread call is still parked after preemption; retire it before
     * graphics reuses its OSThread/stack storage near the top of 4 MiB RAM.
     * A stopped thread would remain in sync_guest and corrupt that storage. */
    if (recomp_thread_id(rt, 0) == BOOT_MAIN_ID && (uint32_t)ctx->gpr[31] == 0x8006ec04)
        recomp_thread_destroy(rt, 0x803ff000);
    hook_os_start_thread(rt, ctx);
    /* Fallback if IPL's handoff did not preempt. It must never return. */
    if (recomp_thread_id(rt, 0) == -1)
        recomp_thread_destroy(rt, 0);
}
