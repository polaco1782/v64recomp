#include "flash.h"
#include "game.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_BYTES = 128, SECTOR_PAGES = 128, FLASH_PAGES = 1024 };
static uint8_t image[FLASH_PAGES * PAGE_BYTES];
static uint8_t pending[sizeof(image)];
static const char *save_path;

void mm_flash_open(const char *path) {
    save_path = path;
    memset(image, 0xff, sizeof(image));
    FILE *file = fopen(path, "rb");
    if (!file) {
        recomp_game_require(errno == ENOENT, "cannot open MM FlashRAM save", 0);
        return;
    }
    bool valid = fread(image, 1, sizeof(image), file) == sizeof(image) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    recomp_game_require(valid, "invalid MM FlashRAM save size", sizeof(image));
}

void mm_flash_init(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    (void)original;
    // SysFlashrom_InitFlash's queue, initialized flag and recognized MX vendor.
    recomp_queue_create(rt, 0x801fbe10, 0x801fbe28, 1);
    recomp_store32(rt, 0x801fbe2c, 1);
    recomp_store32(rt, 0x801fbe30, 0x00c2001e);
    mips_set(ctx, 2, 0);
}

void mm_flash_read(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    (void)original;
    uint32_t page = (uint32_t)ctx->gpr[5], pages = (uint32_t)ctx->gpr[6];
    if (!recomp_load32(rt, 0x801fbe2c) || page >= FLASH_PAGES || pages > FLASH_PAGES - page) {
        mips_set(ctx, 2, mips_s32(-1));
        return;
    }
    if (pages)
        recomp_copy_to_ram(rt, (uint32_t)ctx->gpr[4], image + page * PAGE_BYTES, pages * PAGE_BYTES);
    mips_set(ctx, 2, 0);
}

void mm_flash_program(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    (void)original;
    uint32_t address = (uint32_t)ctx->gpr[4], page = (uint32_t)ctx->gpr[5], pages = (uint32_t)ctx->gpr[6];
    if (!recomp_load32(rt, 0x801fbe2c) || page >= FLASH_PAGES || page % SECTOR_PAGES || pages > FLASH_PAGES - page) {
        mips_set(ctx, 2, mips_s32(-1));
        return;
    }
    // The retail routine erases one sector before programming its pages.
    // Publish both changes together so a failed host write retains the previous save.
    memcpy(pending, image, sizeof(image));
    uint8_t *sector = pending + page * PAGE_BYTES;
    memset(sector, 0xff, SECTOR_PAGES * PAGE_BYTES);
    if (pages) {
        recomp_ram_address(rt, address, pages * PAGE_BYTES);
        for (uint32_t i = 0; i < pages * PAGE_BYTES; i++)
            sector[i] &= recomp_load8(rt, address + i);
    }
    if (!recomp_save_image(save_path, pending, sizeof(pending))) {
        mips_set(ctx, 2, mips_s32(-1));
        return;
    }
    memcpy(image, pending, sizeof(image));
    mips_set(ctx, 2, 0);
}
