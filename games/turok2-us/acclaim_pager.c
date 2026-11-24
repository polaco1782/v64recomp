#include "acclaim_pager.h"
#include "game_config.h"

static bool page_mapped(const Runtime *rt, uint32_t address) {
    for (unsigned i = 0; i < 32; i++) {
        const RecompPageMapping *m = &rt->page_mappings[i];
        if (!m->installed || address < m->virtual_base ||
            (uint64_t)address >= (uint64_t)m->virtual_base + 2 * m->page_size)
            continue;
        unsigned half = (address - m->virtual_base) / m->page_size;
        if (m->physical[half] != UINT32_MAX)
            return true;
    }
    return false;
}

/* The Acclaim VM reserves 4 KiB pages for assets. Deliver a miss through
 * the guest's OS_EVENT_FAULT so its own reader, DMA and eviction code run. */
bool acclaim_resolve_address(Runtime *rt, uint32_t address, size_t bytes, bool write, uint32_t *physical) {
    (void)physical;
    if (address < BOOT_VM_ASSET_BASE || address >= 0x10000000 || page_mapped(rt, address))
        return false;
    uint32_t page = address >> 12, region = BOOT_VM_REGION_LIST;
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
        recomp_fail("Acclaim address outside reserved virtual memory", address);
    if (bytes > 0x10000000u - address)
        recomp_fail("Acclaim virtual span overflows", address);
    RecompThreadSnapshot threads[32];
    size_t count = recomp_thread_snapshots(rt, threads, 32);
    uint32_t object = 0;
    for (size_t i = 0; i < count && i < 32; i++)
        if (threads[i].current)
            object = threads[i].object;
    if (!object)
        recomp_fail("Acclaim virtual miss outside a guest thread", address);
    recomp_store(rt, BOOT_VM_FAULTED_THREAD, object, 4);
    recomp_store(rt, object + 0x120, write ? 12 : 8, 4);
    recomp_store(rt, object + 0x124, address, 4);
    recomp_store(rt, object + 0x12, 2, 2);
    if (!recomp_event_post(rt, 12))
        recomp_fail("Acclaim fault event FIFO full", address);
    recomp_thread_stop(rt, object);
    if (!page_mapped(rt, address))
        recomp_fail("Acclaim pager resumed without mapping the faulting address", address);
    /* Let the shared runtime translate the address using the new mapping. */
    return false;
}
