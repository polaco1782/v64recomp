#ifndef RECOMP_GLES2_TEXTURE_PACK_H
#define RECOMP_GLES2_TEXTURE_PACK_H
#include "tile.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Gles2TexturePack Gles2TexturePack;
typedef struct Gles2RiceWord {
    uint64_t load;
    uint16_t nibbles;
} Gles2RiceWord;
typedef struct Gles2PackImage {
    uint8_t *pixels;
    unsigned width, height;
} Gles2PackImage;
enum { GLES2_TEXTURE_MAX_SIDE = 2048, GLES2_TEXTURE_MAX_PNG_BYTES = 16 * 1024 * 1024 };

#define GLES2_AI_PREFIX "v64recomp/ai/"
#define GLES2_AI_ENTRY GLES2_AI_PREFIX "%016" PRIx64 ".gles.png"
bool gles2_texture_pack_ai(Gles2TexturePack *pack, uint64_t hash, size_t *source);
Gles2TexturePack *gles2_texture_pack_open(const char *zip, const char *overrides, const char *rom_title);
void gles2_texture_pack_close(Gles2TexturePack *pack);
bool gles2_texture_pack_enabled(const Gles2TexturePack *pack);
bool gles2_texture_pack_has_rice(const Gles2TexturePack *pack);
void gles2_texture_pack_record_load(Gles2TexturePack *pack, uint64_t serial, const Tile *tile, uint32_t w0, uint32_t w1,
                                    unsigned image_width, unsigned image_size, const TextureTransfer *dma,
                                    const uint8_t *ram, size_t ram_size, uint32_t address);
typedef struct Gles2PackTile {
    uint64_t hash;
    unsigned width, height;
} Gles2PackTile;
typedef enum Gles2PackStatus { GLES2_PACK_MISS, GLES2_PACK_INCOMPLETE, GLES2_PACK_FOUND } Gles2PackStatus;
/* Source indices identify Rice images, loose overrides and AI entries until reload. */
bool gles2_texture_pack_known(const Gles2TexturePack *pack, uint64_t hash, size_t *source);
void gles2_texture_pack_remember(Gles2TexturePack *pack, uint64_t hash, size_t source);
bool gles2_texture_pack_local(const Gles2TexturePack *pack, uint64_t hash, size_t *source);
Gles2PackStatus gles2_texture_pack_rice(Gles2TexturePack *pack, Gles2PackTile key, const Tile *tile, unsigned tlut,
                                        const Gles2RiceWord words[512], size_t *source);
bool gles2_texture_pack_decode(Gles2TexturePack *pack, size_t source, Gles2PackTile key, unsigned limit,
                               Gles2PackImage *image);
void gles2_texture_pack_free_image(Gles2PackImage *image);
bool gles2_texture_decode_png(const uint8_t *bytes, size_t count, unsigned width, unsigned height, unsigned limit,
                              Gles2PackImage *image);

#ifdef __cplusplus
}
#endif
#endif
