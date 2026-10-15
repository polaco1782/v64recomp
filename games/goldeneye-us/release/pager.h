#ifndef GOLDENEYE_PAGER_H
#define GOLDENEYE_PAGER_H
#include "runtime.h"

typedef struct GoldenEyePagerConfig {
    uint32_t virtual_base, rom_offset, virtual_pages, slots;
    uint32_t management_table, slot_table, buffer_pointer, random_state;
    uint32_t miss_count, current_slot;
} GoldenEyePagerConfig;
typedef struct GoldenEyePager {
    GoldenEyePagerConfig config;
    uint64_t loads, evictions, hits, calls;
    unsigned next_tlb;
    uint8_t tlb_index[128];
} GoldenEyePager;
void goldeneye_pager_install(Runtime *, GoldenEyePager *, const GoldenEyePagerConfig *);
bool goldeneye_pager_resolve(Runtime *, uint32_t, size_t, bool, uint32_t *);
void goldeneye_pager_prepare_call(Runtime *, uint32_t);
#endif
