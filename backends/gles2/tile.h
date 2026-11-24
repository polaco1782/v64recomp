#ifndef RECOMP_GLES2_TILE_H
#define RECOMP_GLES2_TILE_H
#include <stdint.h>

typedef struct Tile {
    unsigned fmt, siz, line, tmem, palette, cs, ct, ms, mt, ss, st, sl, tl, sh, th;
} Tile;

/* Bit offsets preserve the first nibble of odd 4-bit rows. */
typedef struct TextureTransfer {
    uint64_t first_bit;
    unsigned stride_bits, row_bits, rows, width;
} TextureTransfer;

#endif
