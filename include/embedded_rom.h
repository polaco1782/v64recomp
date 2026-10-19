#ifndef RECOMP_EMBEDDED_ROM_H
#define RECOMP_EMBEDDED_ROM_H
#include <stddef.h>
#include <stdint.h>

/* Generated content retains cartridge offsets with verified CPU bytes zeroed. */
extern const uint8_t recomp_embedded_rom[];
extern const size_t recomp_embedded_rom_size;
extern const uint8_t recomp_embedded_original_sha1[20];
extern const uint8_t recomp_embedded_sha1[20];
#endif
