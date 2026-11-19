#ifndef RECOMP_RDP_GLES2_H
#define RECOMP_RDP_GLES2_H
#include "renderer.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRDPGles2 RecompRDPGles2;
typedef struct RecompGles2InputConfig {
    bool mouse_enabled, mouse_gameplay, keyboard_c_buttons, gamepad_camera;
    void (*fov_adjust)(int);
    bool (*aspect_update)(void *, float);
    void *aspect_userdata;
} RecompGles2InputConfig;
/* RDP-to-GLES2 renderer.
 * Creates an SDL GLES2 context (EGL requested on Linux/X11). All calls belong
 * to the creating host thread. The task adapter owns SP/DP completion events;
 * unsupported graphics commands fail explicitly. */
RecompRDPGles2 *recomp_rdp_gles2_create(Runtime *, const char *title);
void recomp_rdp_gles2_destroy(RecompRDPGles2 *);
bool recomp_rdp_gles2_poll(RecompRDPGles2 *);
/* Host-order RDP words produced by a compiled RSP task. The memory view stays
 * private to this synchronous owner-thread draw and receives native readback. */
void recomp_rdp_gles2_draw_rdp(RecompRDPGles2 *, uint8_t *rdram, size_t ram_size, const uint32_t *words, size_t count);
void recomp_rdp_gles2_readback_range(RecompRDPGles2 *, uint32_t *address, size_t *bytes);
void recomp_rdp_gles2_present(RecompRDPGles2 *);
bool recomp_rdp_gles2_capture(RecompRDPGles2 *, const char *path);
bool recomp_rdp_gles2_input(RecompRDPGles2 *, RecompPad *);
void recomp_rdp_gles2_set_input_config(RecompRDPGles2 *, const RecompGles2InputConfig *);
RecompMouseFrame recomp_rdp_gles2_mouse_frame(RecompRDPGles2 *);
RecompCameraStick recomp_rdp_gles2_camera_stick(RecompRDPGles2 *);
bool recomp_rdp_gles2_full_sync(RecompRDPGles2 *);
uint64_t recomp_rdp_gles2_triangles(RecompRDPGles2 *);
void recomp_rdp_gles2_set_framebuffer(RecompRDPGles2 *, uint32_t physical);
void recomp_rdp_gles2_set_framebuffer_sizes(RecompRDPGles2 *, const RecompFramebufferSize *, size_t count);
#ifdef __cplusplus
}
#endif
#endif
