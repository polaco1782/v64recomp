#include "acclaim_controls.h"
#include "acclaim_controls_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double radians = 3.14159265358979323846 / 180.0;
static RecompMouseBuffer mouse;
static RecompFov fov;
static RecompRenderer *video;
static double sensitivity = 0.08, invert_y = -1.0;
enum PlayerControl { NO_CONTROL, ON_FOOT, CLIMBING };
static enum PlayerControl control;
static bool mouse_enabled, pitch_held, camera_active;
static bool climb_yaw_held;
static RecompMouseFrame climb_mouse;
static uint64_t last_control;
static uint32_t look_object, look_limits;
static int pending_steps;
static float camera_scale = 1.0f, display_expansion = 1.0f;

static float from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t to_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static bool playing(Runtime *rt) {
    if (recomp_load32(rt, AC_MODE) != AC_PLAY_MODE || recomp_load32(rt, AC_NEXT_MODE) != AC_PLAY_MODE)
        return false;
    uint32_t cinema = recomp_load32(rt, AC_CINEMA);
    return cinema && !recomp_load32(rt, cinema + AC_CINEMA_ACTIVE) && recomp_load32(rt, AC_PLAYER);
}

static bool local_control(Runtime *rt) {
    return control != NO_CONTROL && recomp_vi_state(rt).retraces - last_control <= 8 && playing(rt);
}

static void player_control(Runtime *rt, enum PlayerControl next) {
    if (control != next || !local_control(rt)) {
        pitch_held = false;
        climb_yaw_held = false;
        climb_mouse = (RecompMouseFrame){0};
        recomp_mouse_clear(&mouse);
    }
    control = next;
    last_control = recomp_vi_state(rt).retraces;
}

static void adjust(int steps) {
    if (fov.original > 0.0)
        recomp_fov_adjust(&fov, steps);
    else
        pending_steps = (int)fmax(0, fmin(140, pending_steps + (double)steps));
}

void acclaim_controls_init(RecompRenderer *renderer) {
    video = renderer;
    recomp_renderer_set_fov_hook(renderer, adjust);
    const char *mode = getenv("RECOMP_TUROK_MOUSE");
    mouse_enabled = !mode || strcmp(mode, "0");
#ifdef RECOMP_DEBUG
    mouse_enabled = mouse_enabled && !recomp_game_replaying();
#endif
    if (!mouse_enabled)
        return;
    const char *setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(end != setting && !*end && isfinite(sensitivity) && sensitivity >= 0.001 &&
                                sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    invert_y = setting && !strcmp(setting, "1") ? 1.0 : -1.0;
    recomp_renderer_separate_mouse_actions(renderer, true);
}

bool acclaim_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    bool active = mouse_enabled && local_control(rt);
    recomp_renderer_mouse_mode(renderer, active, active);
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    recomp_mouse_submit(&mouse, frame);
    if (!frame.active) {
        pitch_held = false;
        climb_yaw_held = false;
        climb_mouse = (RecompMouseFrame){0};
        return true;
    }
    // The guest's controller mapping still handles movement, firing and weapon changes.
    // The renderer already merged the physical controller into pad.
    if (frame.move_y > 0)
        pad->buttons |= 0x0008;
    if (frame.move_y < 0)
        pad->buttons |= 0x0004;
    if (frame.move_x < 0)
        pad->buttons |= 0x0002;
    if (frame.move_x > 0)
        pad->buttons |= 0x0001;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        pad->buttons |= 0x2000;
    if (frame.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_SECONDARY_ATTACK))
        pad->buttons |= 0x0010;
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
        pad->buttons |= 0x8000;
    if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
        pad->buttons |= 0x4000;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= 0x0020;
    return true;
}

void acclaim_player_look(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (playing(rt) && (uint32_t)ctx->gpr[4] == recomp_load32(rt, AC_PLAYER)) {
        look_object = ctx->gpr[5];
        look_limits = ctx->gpr[6];
    }
    original(rt, ctx);
    look_object = look_limits = 0;
}

// The climb handler steers relative to the wall. Feed its own interpolation
// targets so movement, facing on dismount and camera limits stay native.
static void climb_axis(Runtime *rt, MipsContext *ctx, bool yaw) {
    uint32_t destination = (uint32_t)ctx->gpr[4];
    uint32_t player = recomp_load32(rt, AC_PLAYER);
    if (!playing(rt) || destination != player + (yaw ? AC_CLIMB_YAW : AC_CLIMB_PITCH))
        return;
    if (yaw) {
        player_control(rt, CLIMBING);
        climb_mouse = recomp_mouse_consume(&mouse);
    }
    if (!climb_mouse.active)
        return;
    double delta = yaw ? climb_mouse.dx : climb_mouse.dy * invert_y;
    if (!yaw)
        climb_mouse.active = false;
    bool *held = yaw ? &climb_yaw_held : &pitch_held;
    // Turok 2 also angles the camera toward the ends of the ladder with the stick centered.
    float stick = from_bits(yaw ? (uint32_t)ctx->gpr[5] : recomp_load32(rt, player + AC_CLIMB_PITCH_STICK));
    if (stick != 0) {
        *held = false; // A physical stick resumes the game's interpolation.
        return;
    }
    if (!isfinite(delta))
        return;
    if (delta != 0)
        *held = true;
    if (*held) {
        double angle = from_bits(recomp_load32(rt, destination)) + delta * sensitivity * radians;
        double limit = yaw ? from_bits(recomp_load32(rt, AC_CLIMB_YAW_LIMIT)) : 90.0 * radians;
        float result = (float)fmax(-limit, fmin(limit, angle));
        recomp_store32(rt, destination, to_bits(result));
        mips_set(ctx, 5, mips_s32(to_bits(result)));
    }
}

#ifdef AC_CLIMB_IDLE_RETURN
// Turok 2's idle animation recenters yaw a second time when motion stops on a ladder.
void acclaim_player_idle(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t player = (uint32_t)ctx->gpr[4];
    bool hold =
        (uint32_t)ctx->gpr[31] == AC_CLIMB_IDLE_RETURN && climb_yaw_held && player == recomp_load32(rt, AC_PLAYER);
    uint32_t angle = hold ? recomp_load32(rt, player + AC_CLIMB_YAW) : 0;
    original(rt, ctx);
    if (hold)
        recomp_store32(rt, player + AC_CLIMB_YAW, angle);
}
#endif

// Both retail players call the same interpolation helper for yaw and pitch.
// The pitch call follows the native stick yaw and supplies the current pitch bounds.
void acclaim_player_axis(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (look_object && (uint32_t)ctx->gpr[31] == AC_PITCH_RETURN) {
        player_control(rt, ON_FOOT);
        RecompMouseFrame frame = recomp_mouse_consume(&mouse);
        if (frame.active) {
            uint32_t pitch = ctx->gpr[4];
            if (isfinite(frame.dx) && frame.dx != 0) {
                float yaw = from_bits(recomp_load32(rt, look_object + AC_YAW));
                recomp_store32(rt, look_object + AC_YAW, to_bits((float)(yaw + frame.dx * sensitivity * radians)));
            }
            // A physical stick selecting an angle takes back native pitch control.
            if (from_bits((uint32_t)ctx->gpr[5]) != 0)
                pitch_held = false;
            if (isfinite(frame.dy) && frame.dy != 0)
                pitch_held = true;
            if (pitch_held) {
                float angle = from_bits(recomp_load32(rt, pitch));
                float lo = from_bits(recomp_load32(rt, look_limits + AC_PITCH_LOW));
                float hi = from_bits(recomp_load32(rt, look_limits + AC_PITCH_HIGH));
                angle = (float)fmax(lo, fmin(hi, angle + frame.dy * invert_y * sensitivity * radians));
                recomp_store32(rt, pitch, to_bits(angle));
                mips_set(ctx, 5, mips_s32(to_bits(angle)));
            }
        }
    } else if ((uint32_t)ctx->gpr[31] == AC_CLIMB_YAW_RETURN || (uint32_t)ctx->gpr[31] == AC_CLIMB_PITCH_RETURN) {
        climb_axis(rt, ctx, (uint32_t)ctx->gpr[31] == AC_CLIMB_YAW_RETURN);
    }
    original(rt, ctx);
}

void acclaim_world_camera(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    camera_active =
        control == ON_FOOT && local_control(rt) && (uint32_t)ctx->gpr[4] == recomp_load32(rt, AC_MAIN_CAMERA);
    camera_scale = display_expansion = 1.0f;
    original(rt, ctx);
    camera_active = false;
    camera_scale = display_expansion = 1.0f;
}

void acclaim_perspective(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (camera_active && (uint32_t)ctx->gpr[31] == AC_PROJECTION_RETURN) {
        double vertical = from_bits((uint32_t)ctx->gpr[6]);
        double aspect = from_bits((uint32_t)ctx->gpr[7]);
        if (vertical > 0 && vertical < 179 && aspect > 0 && isfinite(aspect)) {
            if (fov.original == 0) {
                // Retail zoom and area FOV are dynamic. Calibrate from the first
                // local on-foot projection; later zooms retain their focal ratio.
                recomp_fov_init(&fov, vertical, aspect);
                if (pending_steps) {
                    recomp_fov_adjust(&fov, pending_steps);
                    pending_steps = 0;
                }
            }
            if (recomp_fov_enabled(&fov)) {
                double stock_tangent = tan(vertical * radians / 2);
                double wanted = tan(fov.horizontal * radians / 2) / tan(fov.original * radians / 2);
                double maximum = tan(140 * radians / 2) / (stock_tangent * aspect);
                camera_scale = (float)fmax(1.0, fmin(wanted, maximum));
                float new_vertical = (float)(2 * atan(stock_tangent * camera_scale) / radians);
                mips_set(ctx, 6, mips_s32(to_bits(new_vertical)));
            }
#ifdef RECOMP_WITH_RT64
            display_expansion = fmaxf(1.0f, recomp_renderer_projection_expansion(video, 4.0f / 3));
#endif
        }
    }
    original(rt, ctx);
}

void acclaim_fixed_matrix(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t matrix = ctx->gpr[4];
    bool widen = camera_active && display_expansion != 1 && (uint32_t)ctx->gpr[31] == AC_FIXED_RETURN;
    original(rt, ctx);
    if (widen) {
        // RT64 expands the fixed matrix at presentation. The engine's object
        // tests use its float copy, with clip X in column zero.
        for (unsigned row = 0; row < 4; row++) {
            uint32_t x = matrix + 16 * row;
            recomp_store32(rt, x, to_bits(from_bits(recomp_load32(rt, x)) / display_expansion));
        }
    }
}

void acclaim_frustum_points(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (camera_active && (uint32_t)ctx->gpr[31] == AC_CORNERS_RETURN && (camera_scale != 1 || display_expansion != 1)) {
        uint32_t points = ctx->gpr[5];
        recomp_game_require((uint32_t)ctx->gpr[7] == 5, "unexpected Acclaim camera frustum", points);
        for (unsigned i = 1; i < 5; i++) {
            uint32_t point = points + 12 * i;
            float x = from_bits(recomp_load32(rt, point)), y = from_bits(recomp_load32(rt, point + 4));
            float nx = x * camera_scale * display_expansion, ny = y * camera_scale;
            recomp_store32(rt, point, to_bits(nx));
            recomp_store32(rt, point + 4, to_bits(ny));
            if (i == 1) {
                double z = from_bits(recomp_load32(rt, point + 8));
                double radius = hypot(hypot(x, y), z);
                if (radius > 0)
                    ctx->fpr[20] = to_bits((float)(from_bits(ctx->fpr[20]) * hypot(hypot(nx, ny), z) / radius));
            }
        }
    }
    original(rt, ctx);
}
