#ifndef RECOMP_FAST3D_GLES2_H
#define RECOMP_FAST3D_GLES2_H
#include "fast3d.h"
#include "renderer.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompFast3DGles2 RecompFast3DGles2;
/* Fast3D-to-GLES2 renderer, independent of RT64.
 * Creates an SDL GLES2 context (EGL requested on Linux/X11). All calls belong
 * to the creating host thread. The task adapter owns SP/DP completion events;
 * unsupported graphics commands fail explicitly. */
RecompFast3DGles2 *recomp_fast3d_gles2_create(Runtime *, const char *title);
void recomp_fast3d_gles2_destroy(RecompFast3DGles2 *);
bool recomp_fast3d_gles2_poll(RecompFast3DGles2 *);
void recomp_fast3d_gles2_draw(RecompFast3DGles2 *, uint32_t root);
/* Returns true when a distinct geometry midpoint was presented. */
bool recomp_fast3d_gles2_present(RecompFast3DGles2 *);
void recomp_fast3d_gles2_set_frame_rate(RecompFast3DGles2 *, uint32_t fps);
void recomp_fast3d_gles2_set_vertex_identity(RecompFast3DGles2 *, const RecompVertexIdentity *);
bool recomp_fast3d_gles2_capture(RecompFast3DGles2 *, const char *path);
bool recomp_fast3d_gles2_input(RecompFast3DGles2 *, RecompPad *);
bool recomp_fast3d_gles2_full_sync(RecompFast3DGles2 *);
uint64_t recomp_fast3d_gles2_triangles(RecompFast3DGles2 *);
void recomp_fast3d_gles2_set_framebuffer(RecompFast3DGles2 *, uint32_t physical);
#ifdef __cplusplus
}
#endif
#endif
