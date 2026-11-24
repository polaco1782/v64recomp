#ifndef RECOMP_RDP_GLES2_H
#define RECOMP_RDP_GLES2_H
#include "torch_lights.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompRDPGles2 RecompRDPGles2;
typedef struct RecompGles2InputConfig {
    bool mouse_enabled, mouse_gameplay, keyboard_c_buttons, gamepad_camera;
    bool separate_mouse_actions, wasd_stick;
    void (*fov_adjust)(int);
    bool (*aspect_update)(void *, float);
    void *aspect_userdata;
} RecompGles2InputConfig;
/* RDP renderer for native OpenGL ES 3.0 and browser WebGL 2.
 * Native SDL requests EGL on Linux/X11. All calls belong
 * to the creating host thread. The task adapter owns SP/DP completion events;
 * unsupported graphics commands fail explicitly. */
RecompRDPGles2 *recomp_rdp_gles2_create(Runtime *, const char *title);
#ifdef RECOMP_GLES2_CHEATS
void recomp_rdp_gles2_enable_cheats(RecompRDPGles2 *, const char *data_path);
#endif
void recomp_rdp_gles2_set_torch_source(RecompRDPGles2 *, RecompTorchSource);
void recomp_rdp_gles2_destroy(RecompRDPGles2 *);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
void recomp_rdp_gles2_set_texture_path(RecompRDPGles2 *, const char *data_path);
#endif
bool recomp_rdp_gles2_poll(RecompRDPGles2 *);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
void recomp_rdp_gles2_enable_texture_editor(RecompRDPGles2 *);
uint64_t recomp_rdp_gles2_editor_paused_ns(void);
#endif
/* The caller retains the immutable command buffer until collection. Submission
 * snapshots pages named by texture loads and 8-bit color-image imports. A
 * parked caller can lend live RAM until the mandatory blocking collection. */
void recomp_rdp_gles2_submit(RecompRDPGles2 *, const uint32_t *words, size_t count, bool guest_parked);
bool recomp_rdp_gles2_ready(RecompRDPGles2 *, bool wait);
void recomp_rdp_gles2_collect(RecompRDPGles2 *, uint64_t *elapsed_ns, uint64_t *cpu_ns);
void recomp_rdp_gles2_draw_rdp(RecompRDPGles2 *, uint8_t *rdram, size_t ram_size, const uint32_t *words, size_t count);
void recomp_rdp_gles2_present(RecompRDPGles2 *);
bool recomp_rdp_gles2_capture(RecompRDPGles2 *, const char *path);
bool recomp_rdp_gles2_input(RecompRDPGles2 *, RecompPad *);
void recomp_rdp_gles2_set_input_config(RecompRDPGles2 *, const RecompGles2InputConfig *);
RecompMouseFrame recomp_rdp_gles2_mouse_frame(RecompRDPGles2 *);
RecompCameraStick recomp_rdp_gles2_camera_stick(RecompRDPGles2 *);
bool recomp_rdp_gles2_full_sync(RecompRDPGles2 *);
void recomp_rdp_gles2_set_framebuffer(RecompRDPGles2 *, uint32_t physical, uint32_t vi_status);
void recomp_rdp_gles2_set_ram_scanout(RecompRDPGles2 *);
void recomp_rdp_gles2_set_framebuffer_sizes(RecompRDPGles2 *, const RecompFramebufferSize *, size_t count);
#ifdef __cplusplus
}
#endif
#endif
