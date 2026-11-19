#include "pager.h"

/* VM_AllocRegion appends 4 KiB reservations after the 0x400000 code bank.
 * VM_HandleTLBMiss consumes the SDK faulted-thread context and dispatches
 * the original VM_ReaderThread. Keep allocation, DMA, eviction, mapping and
 * restart in that translated code; only deliver the native memory fault. */
enum { REGIONS = 0x80121378, FAULTED_THREAD = 0x800b8924 };

bool turok2_resolve_address(Runtime *rt, uint32_t address, size_t bytes, bool write, uint32_t *physical) {
    (void)physical;
    if (address < 0x00480000 || address >= 0x10000000)
        return false;
    for (unsigned i = 0; i < 32; i++) {
        const RecompPageMapping *m = &rt->page_mappings[i];
        if (!m->installed || address < m->virtual_base ||
            (uint64_t)address >= (uint64_t)m->virtual_base + 2 * m->page_size)
            continue;
        unsigned half = (address - m->virtual_base) / m->page_size;
        if (m->physical[half] != UINT32_MAX)
            return false;
    }
    uint32_t page = address >> 12, region = REGIONS;
    bool owned = false;
    for (unsigned i = 0; region && i < 1024; i++) {
        uint32_t start = (uint32_t)recomp_load(rt, region + 4, 2);
        uint32_t count = (uint32_t)recomp_load(rt, region + 6, 2);
        if (page >= start && page - start < count) {
            owned = true;
            break;
        }
        region = (uint32_t)recomp_load(rt, region, 4);
    }
    if (!owned)
        recomp_fail("Turok 2 address outside reserved virtual memory", address);
    if (bytes > 0x10000000u - address)
        recomp_fail("Turok 2 virtual span overflows", address);
    RecompThreadSnapshot threads[32];
    size_t count = recomp_thread_snapshots(rt, threads, 32);
    uint32_t object = 0;
    for (size_t i = 0; i < count && i < 32; i++)
        if (threads[i].current)
            object = threads[i].object;
    if (!object)
        recomp_fail("Turok 2 virtual miss outside a guest thread", address);
    recomp_store(rt, FAULTED_THREAD, object, 4);
    recomp_store(rt, object + 0x120, write ? 12 : 8, 4);
    recomp_store(rt, object + 0x124, address, 4);
    recomp_store(rt, object + 0x12, 2, 2);
    if (!recomp_event_post(rt, 12))
        recomp_fail("Turok 2 fault event FIFO full", address);
    recomp_thread_stop(rt, object);
    /* VM_FinishPageJob restarts the thread after the original TLB helper
     * installs the pair. The shared resolver then validates the mapping. */
    for (unsigned i = 0; i < 32; i++) {
        const RecompPageMapping *m = &rt->page_mappings[i];
        if (m->installed && address >= m->virtual_base &&
            (uint64_t)address < (uint64_t)m->virtual_base + 2 * m->page_size &&
            m->physical[(address - m->virtual_base) / m->page_size] != UINT32_MAX)
            return false;
    }
    recomp_fail("Turok 2 pager resumed without mapping the faulting address", address);
}
