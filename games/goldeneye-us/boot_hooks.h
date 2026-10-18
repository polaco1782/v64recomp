/* Shared verified GoldenEye startup hooks. Include once in each game driver. */
static bool initialized;
#ifdef RECOMP_DEBUG
#include "boot_capture.h"
#endif
static void (*ge_initialize_services)(Runtime *);

void hook_ge_boot_tlb(Runtime *rt, MipsContext *ctx) {
    /* Original startup installs index 1, 4 MiB even page, invalid odd page. */
    recomp_map_pages(rt, 1, 0x7fe000, 0x70000000, 0, UINT32_MAX);
    mips_set(ctx, 2, 1); mips_set(ctx, 3, 31);
    mips_set(ctx, 4, 0x70000000); mips_set(ctx, 5, 0x70000000);
    mips_set(ctx, 6, 31); mips_set(ctx, 7, 1); mips_set(ctx, 8, 0x7fe000);
    mips_set(ctx, 10, 0x70000510);
    recomp_lookup(rt, 0x70000510)(rt, ctx);
}

void hook_ge_enter_inflate(Runtime *rt, MipsContext *ctx) {
    /* The translated init() already copied this code to 0x70200000. Bind the
     * compiled section only after checking every relocated byte against ROM. */
    for (uint32_t i = 0; i < BOOT_INFLATE_SIZE; i += 4) {
        const uint8_t *p = rt->rom + BOOT_INFLATE_ROM_OFFSET + i;
        uint32_t word = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        require(recomp_load(rt, BOOT_INFLATE_VRAM + i, 4) == word,
                "GoldenEye inflater relocation differs from ROM", BOOT_INFLATE_VRAM + i);
    }
    rt->section_loaded[BOOT_INFLATE_SECTION] = true;
    mips_set(ctx, 7, 0x7020141c);
    recomp_lookup(rt, 0x7020141c)(rt, ctx);
}

void hook_ge_os_initialize(Runtime *rt, MipsContext *ctx) {
    require(!initialized,"GoldenEye repeated startup",ctx->pc);
    initialized=true;
#ifdef RECOMP_DEBUG
    debug_capture_startup(rt);
#endif
    if (ge_initialize_services) ge_initialize_services(rt);
    hook_os_initialize(rt, ctx);
}

void hook_ge_prepare_context(Runtime *rt, MipsContext *ctx) {
    /* CPU Context/Wired registers select the original exception handler.
     * No CPU exceptions execute in the native runtime. The startup driver
     * installs a native resolver for the 0x7f... game page cache. */
    (void)rt; (void)ctx;
}

void hook_ge_unmap_tlb(Runtime *rt, MipsContext *ctx) {
    uint32_t index = (uint32_t)ctx->gpr[4];
    require(index < 32, "GoldenEye invalid unmap index", index);
    rt->page_mappings[index] = (RecompPageMapping){0};
}

void hook_ge_pi_raw_dma(Runtime *rt, MipsContext *ctx) {
    uint32_t direction = (uint32_t)ctx->gpr[4], offset = (uint32_t)ctx->gpr[5];
    uint32_t address = (uint32_t)ctx->gpr[6], bytes = (uint32_t)ctx->gpr[7];
    if (offset >= 0x10000000 && offset < 0x20000000) offset -= 0x10000000;
    require(direction == 0 && offset <= rt->rom_size && bytes <= rt->rom_size - offset,
            "GoldenEye unsupported raw PI DMA", offset);
    recomp_copy_to_ram(rt, address, rt->rom + offset, bytes);
    mips_set(ctx, 2, 0);
}

void hook_ge_pi_status(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    mips_set(ctx, 2, 0); /* Raw ROM copy above completes synchronously. */
}

void hook_ge_cache(Runtime *rt, MipsContext *ctx) {
    /* SDK cache invalidation accepts entire virtual address ranges as well
     * as loaded pointers. Native RAM/backends share coherent host storage. */
    (void)rt; (void)ctx;
}
