#pragma once
void hook_nightmare_start_thread(Runtime *rt, MipsContext *ctx) {
    hook_os_start_thread(rt, ctx);
    if (recomp_thread_id(rt, 0) == -1) recomp_thread_stop(rt, 0);
}
