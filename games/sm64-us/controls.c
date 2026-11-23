#include "controls.h"
#include "fov_control.h"
#include "game.h"
#include "game_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    AREA_CAMERA = 0x24,
    CAMERA_CUTSCENE = 0x30,
    GEO_CONTEXT_RENDER = 1,
    PERSPECTIVE_FOV = 0x1c,
    C_RIGHT = 0x0001,
    C_LEFT = 0x0002,
    C_DOWN = 0x0004,
    C_UP = 0x0008,
    N64_Z = 0x2000,
    N64_A = 0x8000,
};

static const double degrees = 180.0 / 3.14159265358979323846;
static const double stock_vertical = 45.0, stock_aspect = 4.0 / 3.0;
static RecompFov fov;
static double fov_scale = 1.0;
static double motion_x, motion_y;
static uint32_t last_camera;
static unsigned pulse_frames, release_frames;
static uint16_t camera_button;
static bool mouse_enabled;

static uint32_t active_camera(Runtime *rt) {
    const uint32_t area = recomp_load32(rt, GAME_GCURRENTAREA);
    if (!area)
        return 0;
    const uint32_t camera = recomp_load32(rt, area + AREA_CAMERA);
    return camera && !recomp_load8(rt, camera + CAMERA_CUTSCENE) ? camera : 0;
}

static void adjust_fov(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        fov_scale = tan(fov.horizontal / (2 * degrees)) / (stock_aspect * tan(stock_vertical / (2 * degrees)));
}

void sm64_controls_init(RecompRenderer *renderer) {
#ifdef __EMSCRIPTEN__
    // Keep the browser's published keyboard layout and avoid automatic pointer lock.
    (void)renderer;
    return;
#endif
    recomp_renderer_wasd_stick(renderer, true);
    const char *option = getenv("RECOMP_SM64_MOUSE");
    mouse_enabled = !option || strcmp(option, "0") != 0;
#ifdef RECOMP_DEBUG
    mouse_enabled = mouse_enabled && !recomp_game_replaying();
#endif
    recomp_renderer_separate_mouse_actions(renderer, mouse_enabled);
    recomp_fov_init(&fov, stock_vertical, stock_aspect);
    recomp_renderer_set_fov_hook(renderer, adjust_fov);
}

static uint16_t camera_input(RecompMouseFrame frame) {
    const double step = 18.0;
    motion_x = fmax(-2 * step, fmin(2 * step, motion_x + frame.dx));
    motion_y = fmax(-2 * step, fmin(2 * step, motion_y + frame.dy));
    if (pulse_frames) {
        --pulse_frames;
        return camera_button;
    }
    if (release_frames) {
        --release_frames;
        return 0;
    }
    if (fabs(motion_x) < step && fabs(motion_y) < step)
        return 0;
    if (fabs(motion_x) >= fabs(motion_y)) {
        camera_button = motion_x > 0.0 ? C_RIGHT : C_LEFT;
        motion_x -= copysign(step, motion_x);
    } else {
        camera_button = motion_y > 0.0 ? C_DOWN : C_UP;
        motion_y -= copysign(step, motion_y);
    }
    // Present pressed and released states across a native 30 Hz update.
    pulse_frames = 2;
    release_frames = 3;
    return camera_button;
}

bool sm64_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    const uint32_t camera = mouse_enabled ? active_camera(rt) : 0;
    recomp_renderer_mouse_mode(renderer, mouse_enabled, camera != 0);
    if (!recomp_renderer_input(renderer, pad))
        return false;
    const RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    if (!frame.active || !camera || camera != last_camera) {
        motion_x = motion_y = 0;
        pulse_frames = release_frames = 0;
    }
    last_camera = camera;
    if (frame.active && camera) {
        pad->buttons |= camera_input(frame);
        if (frame.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_ATTACK))
            pad->buttons |= N64_A;
        if (frame.actions & (RECOMP_MOUSE_DEFEND | RECOMP_MOUSE_SECONDARY_ATTACK))
            pad->buttons |= N64_Z;
    }
    return true;
}

void sm64_camera_fov(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    const bool render = (uint32_t)ctx->gpr[4] == GEO_CONTEXT_RENDER;
    const uint32_t node = (uint32_t)ctx->gpr[5];
    original(rt, ctx);
    if (!render || !node || !recomp_fov_enabled(&fov) || !active_camera(rt))
        return;
    const uint32_t previous = recomp_load32(rt, node + PERSPECTIVE_FOV);
    float vertical;
    memcpy(&vertical, &previous, sizeof(vertical));
    if (!isfinite(vertical) || vertical <= 0.0f || vertical >= 180.0f)
        return;
    // The same perspective node feeds projection and the native object culler.
    // Leave the camera's native FOV/zoom state untouched for the next update.
    const float adjusted = (float)(2 * degrees * atan(tan(vertical / (2 * degrees)) * fov_scale));
    uint32_t bits;
    memcpy(&bits, &adjusted, sizeof(bits));
    recomp_store32(rt, node + PERSPECTIVE_FOV, bits);
}
