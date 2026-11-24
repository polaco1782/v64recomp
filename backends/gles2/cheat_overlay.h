#pragma once
#include <stdbool.h>
#include <SDL.h>
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct Gles2Cheats Gles2Cheats;
Gles2Cheats *gles2_cheats_create(Runtime *, SDL_Window *, const char *data_path);
void gles2_cheats_destroy(Gles2Cheats *);
bool gles2_cheats_event(Gles2Cheats *, const SDL_Event *);
bool gles2_cheats_update(Gles2Cheats *);
void gles2_cheats_draw(Gles2Cheats *);
#ifdef __cplusplus
}
#endif
