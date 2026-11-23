#include "controls.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Native camera/VI fields and caller PCs for the pinned US 1.1 image only. */
enum {
    PD_VARS = 0x80099fc0u,
    PD_PLAYERS = 0x64,
    PD_CURRENT_PLAYER = 0x284,
    PD_CAMERA_MODE = 0x0000,
    PD_IS_DEAD = 0x00d8,
    PD_YAW = 0x0144,
    PD_PITCH = 0x0154,
    PD_PAUSE_MODE = 0x1a24,
    PD_TELEPORT_STATE = 0x1bfc,
    PD_SET_PLAYER_FOV_RETURN = 0x7f0bab60,
    PD_SET_VI_FOV_RETURN = 0x7f0bab74,
    PD_WALK_LOOK_RETURN = 0x7f0c787c,
    PD_C_LEFT = 0x0002,
    PD_C_RIGHT = 0x0001,
};

static const double radians_per_degree = 3.14159265358979323846 / 180;
static const double stock_vertical = 60;
static const double reference_aspect = 4.0 / 3.0;
static RecompFov fov;
static double tangent_scale;
static bool desktop;
static RecompMouseBuffer mouse;

static uint32_t ordinary_camera(Runtime *rt) {
    uint32_t player = recomp_load32(rt, PD_VARS + PD_CURRENT_PLAYER);
    if ((player & 3) || player < 0x80000000u || player - 0x80000000u >= rt->rdram_size ||
        rt->rdram_size - (player - 0x80000000u) <= PD_TELEPORT_STATE ||
        recomp_load32(rt, PD_VARS + PD_PLAYERS) != player)
        return 0;

    for (unsigned i = 1; i < 4; i++) {
        if (recomp_load32(rt, PD_VARS + PD_PLAYERS + i * 4))
            return 0;
    }
    if (recomp_load32(rt, player + PD_CAMERA_MODE) || recomp_load8(rt, player + PD_TELEPORT_STATE) ||
        recomp_load32(rt, player + PD_IS_DEAD) || recomp_load32(rt, player + PD_PAUSE_MODE))
        return 0;
    return player;
}

static void adjust_fov(int steps) {
    if (recomp_fov_adjust(&fov, steps)) {
        tangent_scale = tan(fov.horizontal * radians_per_degree / 2) /
                        (reference_aspect * tan(stock_vertical * radians_per_degree / 2));
    }
}

void perfectdark_controls_init(RecompRenderer *renderer) {
    const char *setting = getenv("RECOMP_PERFECTDARK_WASD");
    bool replaying = false;
#ifdef RECOMP_DEBUG
    replaying = recomp_game_replaying();
#endif
    desktop = !replaying && setting && strcmp(setting, "1") == 0;
    recomp_renderer_keyboard_c_buttons(renderer, desktop);
    recomp_fov_init(&fov, stock_vertical, reference_aspect);
    tangent_scale = 1;
    recomp_mouse_clear(&mouse);
    if (!replaying)
        recomp_renderer_set_fov_hook(renderer, adjust_fov);
}

bool perfectdark_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, desktop, desktop && ordinary_camera(rt));
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    recomp_mouse_submit(&mouse, frame);
    if (frame.active && frame.move_x) {
        /* Native 1.1 uses the left/right C-buttons for strafing while the
         * stick's horizontal axis turns. Leave physical input alone when
         * the keyboard is not supplying this axis. */
        if (pad->stick_x >= -80 && pad->stick_x <= 80)
            pad->stick_x = 0;
        pad->buttons |= frame.move_x < 0 ? PD_C_LEFT : PD_C_RIGHT;
    }
    return true;
}

void perfectdark_fov_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t caller = (uint32_t)ctx->gpr[31];
    if (recomp_fov_enabled(&fov) && (caller == PD_SET_PLAYER_FOV_RETURN || caller == PD_SET_VI_FOV_RETURN) &&
        ordinary_camera(rt)) {
        float native;
        memcpy(&native, &ctx->fpr[12], sizeof(native));
        if (isfinite(native) && native > 0 && native < 180) {
            float adjusted =
                (float)(2 * atan(tan(native * radians_per_degree / 2) * tangent_scale) / radians_per_degree);
            memcpy(&ctx->fpr[12], &adjusted, sizeof(adjusted));
        }
    }
    /* Preserve native zoom state: both consumers receive the same transformed
     * angle and the next update continues to supply a fresh native value. */
    original(rt, ctx);
}

void perfectdark_look_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if ((uint32_t)ctx->gpr[31] == PD_WALK_LOOK_RETURN) {
        RecompMouseFrame frame = recomp_mouse_consume(&mouse);
        uint32_t player = frame.active ? ordinary_camera(rt) : 0;
        if (player && (frame.dx || frame.dy)) {
            uint32_t yaw_bits = recomp_load32(rt, player + PD_YAW);
            uint32_t pitch_bits = recomp_load32(rt, player + PD_PITCH);
            float yaw, pitch;
            memcpy(&yaw, &yaw_bits, sizeof(yaw));
            memcpy(&pitch, &pitch_bits, sizeof(pitch));
            if (isfinite(yaw) && isfinite(pitch) && isfinite(frame.dx) && isfinite(frame.dy)) {
                const double degrees_per_count = 0.14;
                double dx = fmax(-180, fmin(180, frame.dx * degrees_per_count));
                double dy = fmax(-180, fmin(180, frame.dy * degrees_per_count));
                yaw = (float)fmod(yaw - dx, 360);
                if (yaw < 0)
                    yaw += 360;
                pitch = (float)fmax(-90, fmin(90, pitch + dy));
                memcpy(&yaw_bits, &yaw, sizeof(yaw_bits));
                memcpy(&pitch_bits, &pitch, sizeof(pitch_bits));
                recomp_store32(rt, player + PD_YAW, yaw_bits);
                recomp_store32(rt, player + PD_PITCH, pitch_bits);
            }
        }
    }
    /* The guest recalculates the basis, model heading and look vector from
     * the angles before it updates movement. All other callers are stock. */
    original(rt, ctx);
}
