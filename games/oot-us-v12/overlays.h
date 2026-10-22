#pragma once
#include <openssl/evp.h>
/* Canonical addresses describe compiled code. RAM addresses belong to the
 * game's original allocator and are assigned by its translated overlay loader. */
typedef struct OotOverlay {
    uint32_t section, vram, size, text_size, bss_size, reloc_offset;
    uint8_t sha256[32];
    uint32_t loaded;
} OotOverlay;
#include "oot_overlays.h"

static uint32_t oot_canonical(Runtime *rt, uint32_t address) {
    (void)rt;
    if (address < 0x80115480 || address >= 0x80800000)
        return address;
    for (size_t i = 0; i < sizeof(oot_overlays) / sizeof(oot_overlays[0]); i++) {
        OotOverlay *o = &oot_overlays[i];
        if (o->loaded && address >= o->loaded && address - o->loaded < o->text_size)
            return o->vram + address - o->loaded;
    }
    return address;
}
static uint32_t oot_relocated(Runtime *rt, uint32_t section, uint32_t address) {
    if (section >= 3 && section - 3 < sizeof(oot_overlays) / sizeof(oot_overlays[0])) {
        OotOverlay *o = &oot_overlays[section - 3];
        if (!o->loaded || !rt->section_loaded[section] || address < o->vram ||
            address - o->vram >= o->size + o->bss_size)
            recomp_fail("OOT unloaded overlay address", address);
        return o->loaded + address - o->vram;
    }
    return address;
}

void hook_oot_overlay_relocate(Runtime *rt, MipsContext *ctx) {
    uint32_t base = (uint32_t)ctx->gpr[4], table = (uint32_t)ctx->gpr[5], vram = (uint32_t)ctx->gpr[6];
    OotOverlay *o = NULL;
    for (size_t i = 0; i < sizeof(oot_overlays) / sizeof(oot_overlays[0]); i++)
        if (oot_overlays[i].vram == vram)
            o = &oot_overlays[i];
    if (!o || table - base != o->reloc_offset)
        recomp_fail("OOT unknown overlay relocation", vram);
    recomp_ram_address(rt, base, o->size + o->bss_size);
    uint8_t *raw = malloc(o->size), digest[32];
    unsigned length = 0;
    if (!raw)
        recomp_fail("OOT overlay verification allocation failed", vram);
    for (uint32_t i = 0; i < o->size; i++)
        raw[i] = (uint8_t)recomp_load(rt, base + i, 1);
    bool valid =
        EVP_Digest(raw, o->size, digest, &length, EVP_sha256(), NULL) && length == 32 && !memcmp(digest, o->sha256, 32);
    free(raw);
    if (!valid)
        recomp_fail("OOT loaded overlay fingerprint mismatch", vram);
    uint32_t text = (uint32_t)recomp_load(rt, table, 4), data = (uint32_t)recomp_load(rt, table + 4, 4);
    uint32_t sections[4] = {0, base, base + text, base + text + data};
    uint32_t count = (uint32_t)recomp_load(rt, table + 16, 4), hi_pc[32] = {0}, hi_word[32] = {0};
    if (count > (o->size - o->reloc_offset - 20) / 4)
        recomp_fail("OOT invalid overlay relocation count", table);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t entry = (uint32_t)recomp_load(rt, table + 20 + i * 4, 4), section = entry >> 30;
        uint32_t offset = entry & 0xffffff, type = entry >> 24 & 63;
        if (!section || sections[section] - base + offset > o->reloc_offset - 4)
            recomp_fail("OOT invalid overlay relocation site", entry);
        uint32_t pc = sections[section] + offset, word = (uint32_t)recomp_load(rt, pc, 4), patched = word;
        if (type == 2) {
            if (!(word & 0x0f000000))
                patched = word - vram + base;
        } else if (type == 4) {
            uint32_t target = 0x80000000 | ((word & 0x03ffffff) << 2);
            patched = (word & 0xfc000000) | (((target - vram + base) & 0x0fffffff) >> 2);
        } else if (type == 5) {
            unsigned reg = word >> 16 & 31;
            hi_pc[reg] = pc;
            hi_word[reg] = word;
        } else if (type == 6) {
            unsigned reg = word >> 21 & 31;
            if (!hi_pc[reg])
                recomp_fail("OOT unmatched LO16 relocation", pc);
            uint32_t target = (hi_word[reg] << 16) + (int16_t)word;
            if (!(target & 0x0f000000)) {
                target = target - vram + base;
                uint32_t hi = (hi_word[reg] & 0xffff0000) | (((target + 0x8000) >> 16) & 0xffff);
                recomp_store(rt, hi_pc[reg], hi, 4);
                patched = (word & 0xffff0000) | (target & 0xffff);
            }
        } else
            recomp_fail("OOT unsupported overlay relocation type", type);
        recomp_store(rt, pc, patched, 4);
    }
    for (size_t i = 0; i < sizeof(oot_overlays) / sizeof(oot_overlays[0]); i++) {
        OotOverlay *other = &oot_overlays[i];
        if (other->loaded && (uint64_t)base < (uint64_t)other->loaded + other->size + other->bss_size &&
            (uint64_t)other->loaded < (uint64_t)base + o->size + o->bss_size) {
            other->loaded = 0;
            rt->section_loaded[other->section] = false;
        }
    }
    o->loaded = base;
    rt->section_loaded[o->section] = true;
    fprintf(stderr, "OOT overlay 0x%08x loaded at 0x%08x\n", vram, base);
}
