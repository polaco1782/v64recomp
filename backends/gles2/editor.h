#ifndef RECOMP_GLES2_EDITOR_H
#define RECOMP_GLES2_EDITOR_H

#include <stdbool.h>
#include <stdint.h>

struct SDL_Window;
typedef struct Gles2TextureEditor Gles2TextureEditor;
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
void gles2_editor_observe(Gles2TextureEditor *, uint64_t key, unsigned width, unsigned height, const uint8_t *rgba);
void gles2_editor_observe_replacement(Gles2TextureEditor *, uint64_t key, unsigned width, unsigned height,
                                      unsigned preview_width, unsigned preview_height, const uint8_t *rgba);
Gles2EditorResult gles2_editor_run(Gles2TextureEditor *, struct SDL_Window *game, unsigned max_texture_size);

#ifdef __cplusplus
}
#endif

#endif
