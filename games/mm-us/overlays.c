#include "game.h"
#include "hash.h"
#include "overlays.h"
#include "game_config.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t vram, size, text_size, bss_size, reloc_offset;
    uint8_t sha256[32];
    uint32_t loaded;
} MmOverlay;
#include "mm_overlays.h"

static const size_t overlay_count = sizeof(overlays) / sizeof(overlays[0]);

static void verify_image(Runtime *rt, uint32_t base, uint32_t size, const uint8_t expected[32]) {
    recomp_ram_address(rt, base, size);
    uint8_t *raw = malloc(size), digest[32];
    recomp_game_require(raw != NULL, "Majora's Mask code verification allocation failed", base);
    for (uint32_t i = 0; i < size; i++)
        raw[i] = recomp_load8(rt, base + i);
    bool verified = recomp_sha256(raw, size, digest) && !memcmp(expected, digest, sizeof(digest));
    free(raw);
    recomp_game_require(verified, "Majora's Mask loaded code fingerprint mismatch", base);
}

// Main_Init waits for the code segment's DMA and clears its BSS before returning.
void mm_main_init(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    original(rt, ctx);
    verify_image(rt, MM_CODE_VRAM, MM_CODE_SIZE, mm_code_sha256);
    rt->section_loaded[1] = true;
}

void mm_overlay_relocate(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t ram = ctx->gpr[4], table = ctx->gpr[5], vram = ctx->gpr[6];
    size_t index = 0;
    while (index < overlay_count && overlays[index].vram != vram)
        index++;
    recomp_game_require(index < overlay_count, "unknown Majora's Mask overlay", vram);
    MmOverlay *o = &overlays[index];
    recomp_game_require(table - ram == o->reloc_offset, "invalid Majora's Mask relocation section", table);
    recomp_ram_address(rt, ram, o->size + o->bss_size);
    verify_image(rt, ram, o->size, o->sha256);
    original(rt, ctx);
    for (size_t i = 0; i < overlay_count; i++) {
        MmOverlay *other = &overlays[i];
        if (other->loaded && ram < other->loaded + other->size + other->bss_size &&
            other->loaded < ram + o->size + o->bss_size) {
            other->loaded = 0;
            rt->section_loaded[2 + i] = false;
        }
    }
    o->loaded = ram;
    rt->section_loaded[2 + index] = true;
}

static void deactivate(Runtime *rt, uint32_t ram) {
    if (ram)
        for (size_t i = 0; i < overlay_count; i++)
            if (overlays[i].loaded == ram) {
                overlays[i].loaded = 0;
                rt->section_loaded[2 + i] = false;
                break;
            }
}

void mm_arena_free(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // Both the system heap and ZeldaArena ultimately release through __osFree(arena, ptr).
    deactivate(rt, ctx->gpr[5]);
    original(rt, ctx);
}

void mm_kaleido_clear(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    deactivate(rt, recomp_load32(rt, ctx->gpr[4]));
    original(rt, ctx);
}

void mm_actor_free_overlay(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // Absolute actor overlays can release their slot without freeing its allocation.
    uint32_t slot = (uint32_t)ctx->gpr[4] + 0x10;
    uint32_t base = recomp_load32(rt, slot);
    original(rt, ctx);
    if (base && !recomp_load32(rt, slot))
        deactivate(rt, base);
}

static uint32_t canonical(Runtime *rt, uint32_t pc) {
    (void)rt;
    if (pc >= MM_CODE_VRAM + MM_CODE_SIZE && pc < 0x80800000)
        for (size_t i = 0; i < overlay_count; i++) {
            MmOverlay *o = &overlays[i];
            if (o->loaded && pc - o->loaded < o->text_size)
                return o->vram + pc - o->loaded;
        }
    return pc;
}

static uint32_t relocated(Runtime *rt, uint32_t section, uint32_t address) {
    if (section < 2)
        return address;
    recomp_game_require(section - 2 < overlay_count && rt->section_loaded[section], "Majora's Mask overlay is unloaded",
                        address);
    MmOverlay *o = &overlays[section - 2];
    recomp_game_require(address >= o->vram && address - o->vram < o->size + o->bss_size,
                        "Majora's Mask overlay address out of range", address);
    return o->loaded + address - o->vram;
}

void mm_init_code(Runtime *rt) {
    rt->canonical_function_address = canonical;
    rt->relocated_address = relocated;
}
