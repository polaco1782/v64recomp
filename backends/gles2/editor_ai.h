#ifndef RECOMP_GLES2_EDITOR_AI_H
#define RECOMP_GLES2_EDITOR_AI_H

#include "texture_pack.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { GLES2_AI_SCALE = 8 };

typedef struct Gles2AiJob Gles2AiJob;
Gles2AiJob *gles2_ai_start(const uint8_t *rgba, unsigned width, unsigned height, const char **error);
/* Returns false while Codex is working. On success, returns the decoded image and commits the PNG. */
bool gles2_ai_poll(Gles2AiJob *, const char *zip, uint64_t key, Gles2PackImage *image, const char **error);
void gles2_ai_destroy(Gles2AiJob *);
const char *gles2_editor_remove_loose(const char *dir, uint64_t key);
typedef struct Gles2TileMapping {
    uint64_t stored, key;
} Gles2TileMapping;
const char *gles2_archive_store_mappings(const char *zip, const Gles2TileMapping *mappings, size_t count);
const char *gles2_ai_remove(const char *zip, uint64_t key);

#ifdef __cplusplus
}
#endif
#endif
