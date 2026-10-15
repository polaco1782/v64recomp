/* Native equivalent of GoldenEye's ROM page cache and TLB miss handling. */
#include "pager.h"
#include <string.h>

enum { PAGE_BYTES = 0x2000, MANAGEMENT_BYTES = 16 };
static void checked(bool ok, const char *message, uint32_t address) {
    if (!ok) recomp_fail(message, address);
}

void goldeneye_pager_install(Runtime *rt, GoldenEyePager *pager, const GoldenEyePagerConfig *c) {
    checked(rt && pager && c && !rt->resolve_address && !rt->prepare_call && rt->rom,
            "GoldenEye pager installation conflict", 0);
    checked(c->virtual_base % PAGE_BYTES == 0 && c->virtual_pages && c->virtual_pages <= 128
            && c->slots && c->slots <= 90 && (uint64_t)c->virtual_base + c->virtual_pages * PAGE_BYTES <= UINT32_MAX
            && c->rom_offset <= rt->rom_size && c->virtual_pages * PAGE_BYTES <= rt->rom_size - c->rom_offset,
            "GoldenEye pager configuration invalid", c->virtual_base);
    recomp_ram_address(rt, c->management_table, c->virtual_pages * MANAGEMENT_BYTES);
    recomp_ram_address(rt, c->slot_table, c->slots * 2);
    recomp_ram_address(rt, c->buffer_pointer, 4);
    recomp_ram_address(rt, c->random_state, 8);
    recomp_ram_address(rt, c->miss_count, 4);
    recomp_ram_address(rt, c->current_slot, 4);
    *pager = (GoldenEyePager){.config=*c};
    rt->address_userdata = pager;
    rt->resolve_address = goldeneye_pager_resolve;
    rt->prepare_call = goldeneye_pager_prepare_call;
}

static unsigned next_slot(Runtime *rt, const GoldenEyePagerConfig *c) {
    /* Exact integer shifts/XORs of original tlbRandomGetNext (0x70001b10). */
    uint64_t state = recomp_load(rt, c->random_state, 8);
    uint64_t next = ((state << 63) >> 31) | ((state << 31) >> 32);
    next ^= (state << 44) >> 32;
    next ^= (next >> 20) & 0xfff;
    recomp_store(rt, c->random_state, next, 8);
    return (uint32_t)next % c->slots;
}

static void forget_tlb(Runtime *rt, uint32_t virtual_address) {
    for (unsigned i=2; i<32; i++)
        if (rt->page_mappings[i].installed && rt->page_mappings[i].virtual_base == virtual_address)
            rt->page_mappings[i] = (RecompPageMapping){0};
}

static uint32_t page(Runtime *rt, GoldenEyePager *pager, unsigned index) {
    const GoldenEyePagerConfig *c = &pager->config;
    uint32_t base = (uint32_t)recomp_load(rt, c->buffer_pointer, 4);
    checked(base >= 0x80000000 && base < 0xa0000000 && base % PAGE_BYTES == 0,
            "GoldenEye page cache used before initialization", base);
    recomp_ram_address(rt, base, c->slots * PAGE_BYTES);
    uint32_t entry = c->management_table + index * MANAGEMENT_BYTES;
    uint32_t context = (uint32_t)recomp_load(rt, entry, 4);
    unsigned slot;
    if (context & 2) {
        slot = (unsigned)recomp_load(rt, entry + 4, 4);
        checked(slot < c->slots && recomp_load(rt, c->slot_table + slot * 2, 1) == 0
                && recomp_load(rt, c->slot_table + slot * 2 + 1, 1) == index,
                "GoldenEye page cache tables disagree", entry);
        pager->hits++;
    } else {
        forget_tlb(rt, c->virtual_base + index * PAGE_BYTES);
        slot = next_slot(rt, c);
        uint32_t owner = c->slot_table + slot * 2;
        if (recomp_load(rt, owner, 1) == 0) {
            unsigned old = (unsigned)recomp_load(rt, owner + 1, 1);
            checked(old < c->virtual_pages, "GoldenEye cache slot owner invalid", owner);
            recomp_store(rt, c->management_table + old * MANAGEMENT_BYTES, 1, 4);
            forget_tlb(rt, c->virtual_base + old * PAGE_BYTES);
            pager->evictions++;
        }
        recomp_copy_to_ram(rt, base + slot * PAGE_BYTES,
                           rt->rom + c->rom_offset + index * PAGE_BYTES, PAGE_BYTES);
        uint32_t physical = (base & 0x1fffffff) + slot * PAGE_BYTES;
        recomp_store(rt, entry + 4, slot, 4);
        recomp_store(rt, entry, ((physical >> 12) << 6) | 0x1f, 4);
        recomp_store(rt, owner, 0, 1);
        recomp_store(rt, owner + 1, index, 1);
        recomp_store(rt, c->miss_count, (uint32_t)recomp_load(rt, c->miss_count, 4) + 1, 4);
        recomp_store(rt, c->current_slot, slot, 4);
        pager->loads++;
    }
    uint32_t physical = (base & 0x1fffffff) + slot * PAGE_BYTES;
    checked(context == 1 || !(context & 2) || ((context >> 6) << 12) == physical,
            "GoldenEye cache physical address mismatch", entry);
    uint32_t virtual_address = c->virtual_base + index * PAGE_BYTES;
    /* Resident cache pages outlive CPU TLB entries. Reinstall on a native
     * address/call miss, preserving the wired library alias at index 1. */
    unsigned cached=pager->tlb_index[index];
    const RecompPageMapping *mapping=&rt->page_mappings[cached];
    bool installed=cached>=2 && mapping->installed && mapping->virtual_base==virtual_address
        && mapping->page_size==PAGE_BYTES/2 && mapping->physical[0]==physical
        && mapping->physical[1]==physical+PAGE_BYTES/2;
    if(!installed) {
        for(unsigned i=2;i<32;i++) {
            mapping=&rt->page_mappings[i];
            if(mapping->installed && mapping->virtual_base==virtual_address
                    && mapping->page_size==PAGE_BYTES/2 && mapping->physical[0]==physical
                    && mapping->physical[1]==physical+PAGE_BYTES/2) {
                pager->tlb_index[index]=(uint8_t)i;installed=true;break;
            }
        }
    }
    if (!installed) {
        unsigned tlb = 2 + pager->next_tlb++ % 30;
        recomp_map_pages(rt, tlb, 0, virtual_address, physical, physical + PAGE_BYTES / 2);
        pager->tlb_index[index]=(uint8_t)tlb;
    }
    return physical;
}

bool goldeneye_pager_resolve(Runtime *rt, uint32_t address, size_t bytes, bool write, uint32_t *physical) {
    GoldenEyePager *pager = rt->address_userdata;
    const GoldenEyePagerConfig *c = &pager->config;
    if (address < c->virtual_base || (uint64_t)address >= (uint64_t)c->virtual_base + c->virtual_pages * PAGE_BYTES)
        return false;
    checked(!write, "GoldenEye write to ROM code page requires explicit support", address);
    uint32_t offset = address - c->virtual_base;
    checked(bytes <= c->virtual_pages * PAGE_BYTES - offset,
            "GoldenEye virtual code span out of range", address);
    unsigned index = offset / PAGE_BYTES;
    uint32_t first = page(rt, pager, index);
    *physical = first + offset % PAGE_BYTES;
    /* Pointer/range APIs require contiguity. Aligned MIPS loads and merge
     * helpers resolve individual spans and work across nonadjacent slots. */
    size_t span = PAGE_BYTES - offset % PAGE_BYTES;
    for (unsigned next=index+1; span<bytes; next++, span+=PAGE_BYTES)
        checked(page(rt, pager, next) == first + (next-index) * PAGE_BYTES,
                "GoldenEye noncontiguous cached virtual span", address);
    return true;
}

void goldeneye_pager_prepare_call(Runtime *rt, uint32_t address) {
    uint32_t physical;
    if (goldeneye_pager_resolve(rt, address, 4, false, &physical)) {
        GoldenEyePager *pager = rt->address_userdata;
        pager->calls++;
    }
}
