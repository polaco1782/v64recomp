#ifndef RECOMP_GLES2_EDITOR_H
#define RECOMP_GLES2_EDITOR_H

#include "texture_pack.h"
#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;
typedef struct Gles2TextureEditor Gles2TextureEditor;
typedef struct Gles2EditorPreview Gles2EditorPreview;
typedef enum Gles2EditorResult {
    GLES2_EDITOR_CLOSED = 0,
    GLES2_EDITOR_CHANGED = 1,
    GLES2_EDITOR_QUIT = 2,
    GLES2_EDITOR_OPEN_FAILED = 4
} Gles2EditorResult;

#ifdef __cplusplus
extern "C" {
#endif

Gles2TextureEditor *gles2_editor_create(const char *overrides_dir);
void gles2_editor_destroy(Gles2TextureEditor *);
bool gles2_editor_touch(Gles2TextureEditor *, uint64_t key, bool replaced);
void gles2_editor_presented(Gles2TextureEditor *);
void gles2_editor_observe(Gles2TextureEditor *, uint64_t key, unsigned width, unsigned height, const uint8_t *rgba);
Gles2EditorPreview *gles2_editor_make_preview(unsigned width, unsigned height, const uint8_t *rgba);
/* If newly allocated, fill the returned buffer with the decoded native tile. */
uint8_t *gles2_editor_observe_replacement(Gles2TextureEditor *, uint64_t key, unsigned width, unsigned height,
                                          const Gles2EditorPreview *preview);
struct RecompRDPGles2;
Gles2EditorResult gles2_editor_run(struct RecompRDPGles2 *gpu);

#ifdef __cplusplus
}
#endif

#endif
