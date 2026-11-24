#ifndef RECOMP_GLES2_RICE_H
#define RECOMP_GLES2_RICE_H
#include "texture_pack.h"

typedef struct RiceLoad {
    Tile tile;
    unsigned opcode, image_width, image_size, row_bytes;
    uint8_t *bytes;
    size_t size, capacity;
    uint64_t serial;
} RiceLoad;
typedef struct RicePalette {
    uint8_t bytes[512];
    uint64_t loads[256];
} RicePalette;
typedef struct RiceState {
    RiceLoad loads[512];
    RicePalette glide, dma;
} RiceState;
void rice_record_load(RiceState *, uint64_t, const Tile *, uint32_t, uint32_t, unsigned, unsigned,
                      const TextureTransfer *, const uint8_t *, size_t, uint32_t);
bool rice_keys(const RiceState *, const Tile *, unsigned tlut, const Gles2RiceWord *, unsigned width, unsigned height,
               char glide[22], char dma[22]);
#endif
