#include "pager.h"

/* ROM-derived ABI: func_00106ef0 walks this descriptor list; osGetFaultedThread
 * reads the SDK global below. Event 12 is bound by func_00107120. The translated
 * game allocates, loads, evicts and maps its own 4 KiB pages. */
enum { DESCRIPTORS = 0x8004ca1c, FAULTED_THREAD = 0x800285e4 };

bool shadowman_resolve_address(Runtime *rt, uint32_t address, size_t bytes,
                               bool write, uint32_t *physical) {
    (void)physical;
    if (address < 0x00800000 || address >= 0x10000000) return false;
    /* Installed pairs remain owned by the shared, checked TLB alias service. */
    for (unsigned i = 0; i < 32; i++) {
        const RecompPageMapping *m = &rt->page_mappings[i];
        if (!m->installed || address < m->virtual_base
                || (uint64_t)address >= (uint64_t)m->virtual_base + 2*m->page_size) continue;
        unsigned half = (address-m->virtual_base)/m->page_size;
        if (m->physical[half] != UINT32_MAX) return false;
    }
    uint32_t page = address >> 12, descriptor = DESCRIPTORS;
    bool owned = false;
    for (unsigned i = 0; descriptor && i < 1024; i++) {
        uint32_t start = (uint32_t)recomp_load(rt, descriptor+4, 2);
        uint32_t count = (uint32_t)recomp_load(rt, descriptor+6, 2);
        if (page >= start && page-start < count) { owned = true; break; }
        descriptor = (uint32_t)recomp_load(rt, descriptor, 4);
    }
    if (!owned) return false;
    if (bytes > 0x10000000u-address) recomp_fail("Shadow Man virtual span overflows", address);
    RecompThreadSnapshot threads[32];
    size_t count = recomp_thread_snapshots(rt, threads, 32);
    uint32_t object = 0;
    for (size_t i = 0; i < count; i++) if (threads[i].current) object = threads[i].object;
    if (!object) recomp_fail("Shadow Man virtual miss outside a guest thread", address);
    recomp_store(rt, FAULTED_THREAD, object, 4);
    recomp_store(rt, object+0x120, write ? 4 : 8, 4); /* Native OS fault ABI, not CPU execution. */
    recomp_store(rt, object+0x124, address, 4);
    recomp_store(rt, object+0x12, 2, 2);
    if (!recomp_event_post(rt, 12)) recomp_fail("Shadow Man fault event FIFO full", address);
    recomp_thread_stop(rt, object);
    /* The original completion routine restarts this thread after osMapTLB.
     * Returning false now lets the shared resolver verify the installed pair. */
    return false;
}
