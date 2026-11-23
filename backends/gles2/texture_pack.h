#ifndef RECOMP_GLES2_TEXTURE_PACK_H
#define RECOMP_GLES2_TEXTURE_PACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Gles2TexturePack Gles2TexturePack;
typedef struct Gles2PackImage {
    uint8_t *pixels;
    unsigned width, height;
} Gles2PackImage;
enum { GLES2_TEXTURE_MAX_PNG_BYTES = 16 * 1024 * 1024 };

Gles2TexturePack *gles2_texture_pack_open(const char *zip, const char *overrides, const char *rom_title);
void gles2_texture_pack_close(Gles2TexturePack *pack);
bool gles2_texture_pack_enabled(const Gles2TexturePack *pack);
bool gles2_texture_rt64_id(const uint8_t *tmem, unsigned format, unsigned size, unsigned line, unsigned address,
                           unsigned palette, unsigned tlut, unsigned width, unsigned height, uint64_t *id);
bool gles2_texture_pack_image(Gles2TexturePack *pack, uint64_t id, unsigned width, unsigned height, unsigned limit,
                              Gles2PackImage *image);
void gles2_texture_pack_free_image(Gles2PackImage *image);
bool gles2_texture_decode_png(const uint8_t *bytes, size_t count, unsigned width, unsigned height, unsigned limit,
                             Gles2PackImage *image);

#ifdef __cplusplus
}
#endif
#endif
