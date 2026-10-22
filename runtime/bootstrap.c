#include "runtime.h"
#include "runtime_debug.h"
#include <openssl/evp.h>
#include <string.h>

bool recomp_rom_attach(Runtime *rt, const uint8_t *rom, size_t size, const uint8_t sha1[20]) {
    static const uint8_t magic[] = {0x80, 0x37, 0x12, 0x40};
    if (rt->rom || !rom || !sha1 || size < 64 || size % 4 || memcmp(rom, magic, 4))
        return false;
    uint8_t digest[EVP_MAX_MD_SIZE];
    unsigned length = 0;
    if (!EVP_Digest(rom, size, digest, &length, EVP_sha1(), NULL) || length != 20 || memcmp(digest, sha1, 20))
        return false;
    rt->rom = rom;
    rt->rom_size = size;
    RECOMP_LOG("game", "verified ROM title=%.20s bytes=%zu entry=0x%08" PRIx32 " country=0x%02x revision=%u",
               (const char *)rom + 0x20, size,
               (uint32_t)rom[8] << 24 | (uint32_t)rom[9] << 16 | (uint32_t)rom[10] << 8 | rom[11], rom[0x3e],
               rom[0x3f]);
    return true;
}

static bool ram_range(Runtime *rt, uint32_t address, size_t size, uint32_t *physical) {
    if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000))
        address &= UINT32_C(0x1fffffff);
    *physical = address;
    return address < rt->rdram_size && size <= rt->rdram_size - address;
}

bool recomp_boot_load(Runtime *rt, const RecompBootLoad *loads, size_t count) {
    if (!rt->rom || !loads || !count || rt->scheduler)
        return false;
    for (size_t i = 0; i < count; i++) {
        const RecompBootLoad *a = &loads[i];
        uint32_t pa;
        if (a->section >= rt->section_count || !a->size || a->size % 4 || a->vram % 4 || a->rom_offset % 4 ||
            a->rom_offset > rt->rom_size || a->size > rt->rom_size - a->rom_offset ||
            !ram_range(rt, a->vram, a->size, &pa))
            return false;
        for (size_t j = 0; j < i; j++) {
            uint32_t pb;
            if (!ram_range(rt, loads[j].vram, loads[j].size, &pb) || a->section == loads[j].section ||
                (pa < (uint64_t)pb + loads[j].size && pb < (uint64_t)pa + a->size))
                return false;
        }
    }
    memset(rt->section_loaded, 0, rt->section_count * sizeof(bool));
    for (size_t i = 0; i < count; i++) {
        recomp_copy_to_ram(rt, loads[i].vram, rt->rom + loads[i].rom_offset, loads[i].size);
        rt->section_loaded[loads[i].section] = true;
        RECOMP_LOG("game", "boot section=%" PRIu32 " rom=0x%08" PRIx32 " ram=0x%08" PRIx32 " bytes=%" PRIu32,
                   loads[i].section, loads[i].rom_offset, loads[i].vram, loads[i].size);
    }
    return true;
}
