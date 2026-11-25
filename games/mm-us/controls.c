#include "controls.h"
#include "fov_control.h"
#include "game.h"
#include "mm_controls_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t play;
static uint32_t wide_aspect_bits;
static bool mouse_enabled;
static RecompMouseBuffer mouse;
static RecompFov fov;
static double sensitivity = 0.08;
static double yaw, pitch;
static bool orbiting, normal_view;
static const double radians = 0.017453292519943295;

static bool playing(Runtime *rt) {
    return play && recomp_load32(rt, play + 4) == MM_CTL_PLAY_MAIN && recomp_load8(rt, play + 0x9b) &&
           !recomp_load32(rt, MM_CTL_SAVE_CONTEXT + 0x3ca8) && !recomp_load16(rt, play + 0x16f1c) &&
           !recomp_load16(rt, play + 0x17000) && !recomp_load8(rt, play + 0x1682a) &&
           !recomp_load8(rt, play + 0x1f2c) && !recomp_load8(rt, play + 0x18875) &&
           recomp_load16(rt, MM_CTL_CUTSCENE_ID) == 0xffff;
}

static bool player_camera(Runtime *rt, uint32_t camera) {
    return playing(rt) && !recomp_load16(rt, play + 0x810) && camera == play + 0x220 &&
           recomp_load32(rt, play + 0x800) == camera && recomp_load32(rt, camera + 0x8c) == play &&
           recomp_load16(rt, camera + 0x140) == 7 && !recomp_load16(rt, camera + 0x144);
}

static float read_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void write_float(Runtime *rt, uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    recomp_store32(rt, address, bits);
}

static void adjust_fov(int steps) {
    recomp_fov_adjust(&fov, steps);
}

static bool update_aspect(void *opaque, float aspect) {
    Runtime *rt = opaque;
    wide_aspect_bits = 0;
    if (!(aspect > 4.0f / 3 && aspect <= 21.0f / 9) || !normal_view || !player_camera(rt, play + 0x220))
        return false;
    memcpy(&wide_aspect_bits, &aspect, sizeof(aspect));
    return true;
}

void mm_world_perspective(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // Only the main PlayState view passes its perspective normalizer in a1.
    if (wide_aspect_bits && (uint32_t)ctx->gpr[31] == MM_CTL_PERSPECTIVE_RETURN &&
        (uint32_t)ctx->gpr[5] == play + 0xb8 + MM_CTL_VIEW_NORM && normal_view && player_camera(rt, play + 0x220))
        ctx->gpr[7] = wide_aspect_bits;
    original(rt, ctx);
}

void mm_controls_init(Runtime *rt, RecompRenderer *renderer) {
    const char *option = getenv("RECOMP_MM_MOUSE");
    mouse_enabled = !option || strcmp(option, "0");
#ifdef RECOMP_DEBUG
    mouse_enabled = mouse_enabled && !recomp_game_replaying();
#endif
    const char *value = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (mouse_enabled && value && *value) {
        char *end;
        sensitivity = strtod(value, &end);
        recomp_game_require(end != value && !*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    recomp_fov_init(&fov, 60.0, 4.0 / 3.0);
    recomp_renderer_set_fov_hook(renderer, adjust_fov);
    recomp_renderer_set_aspect_hook(renderer, update_aspect, rt);
    recomp_renderer_separate_mouse_actions(renderer, mouse_enabled);
}

bool mm_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    bool live = mouse_enabled && playing(rt);
    recomp_renderer_mouse_mode(renderer, mouse_enabled, live);
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    recomp_mouse_submit(&mouse, frame);
    if (!frame.active) {
        orbiting = false;
        return true;
    }
    int speed = (frame.actions & RECOMP_MOUSE_SPEED) ? 40 : 80;
    if (frame.move_x || frame.move_y) {
        // Use the N64 stick's circular range on diagonals.
        if (frame.move_x && frame.move_y)
            speed = speed * 7 / 10;
        pad->stick_x = (int8_t)(frame.move_x * speed);
        pad->stick_y = (int8_t)(frame.move_y * speed);
    }
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        pad->buttons |= 0x4000;
    if (frame.actions & (RECOMP_MOUSE_USE | RECOMP_MOUSE_JUMP))
        pad->buttons |= 0x8000;
    if (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK)
        pad->buttons |= 0x2000;
    if (frame.actions & RECOMP_MOUSE_DEFEND)
        pad->buttons |= 0x0010;
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
        pad->buttons |= 0x0002;
    if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
        pad->buttons |= 0x0004;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= (frame.actions & RECOMP_MOUSE_SPEED) ? 0x0008 : 0x0001;
    return true;
}

void mm_play_frame(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t state = (uint32_t)ctx->gpr[4];
    if (state != play) {
        orbiting = false;
        recomp_mouse_clear(&mouse);
    }
    play = state;
    original(rt, ctx);
    if (!recomp_load8(rt, play + 0x9b)) {
        play = 0;
        orbiting = false;
        recomp_mouse_clear(&mouse);
    }
}

void mm_normal_camera(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t camera = (uint32_t)ctx->gpr[4];
    bool selected = (uint32_t)ctx->gpr[31] == MM_CTL_DISPATCH_RETURN && player_camera(rt, camera);
    original(rt, ctx);
    if (!selected)
        return;
    normal_view = true;
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active || (recomp_load16(rt, play + 0x14) & 0x2000)) {
        orbiting = false;
        return;
    }
    if (!orbiting && frame.dx == 0 && frame.dy == 0)
        return;
    double at[3], offset[3];
    for (unsigned i = 0; i < 3; i++) {
        at[i] = read_float(rt, camera + 0x50 + i * 4);
        offset[i] = read_float(rt, camera + 0x74 + i * 4) - at[i];
    }
    double horizontal = hypot(offset[0], offset[2]);
    double radius = hypot(offset[1], horizontal);
    if (!(radius > 1.0 && isfinite(radius))) {
        orbiting = false;
        return;
    }
    if (!orbiting) {
        yaw = atan2(offset[0], offset[2]);
        pitch = atan2(offset[1], horizontal);
        orbiting = true;
    }
    yaw = remainder(yaw - frame.dx * sensitivity * radians, 2 * 3.141592653589793);
    pitch = fmin(70 * radians, fmax(-60 * radians, pitch + frame.dy * sensitivity * radians));
    double ring = radius * cos(pitch);
    double eye[3] = {at[0] + ring * sin(yaw), at[1] + radius * sin(pitch), at[2] + ring * cos(yaw)};
    for (unsigned i = 0; i < 3; i++) {
        write_float(rt, camera + 0x74 + i * 4, (float)eye[i]);
        write_float(rt, camera + 0x5c + i * 4, (float)eye[i]);
    }

    // Use MM's line test to clip the eye before Camera_Update submits its view.
    MipsContext scratch = *ctx;
    scratch.gpr[4] = camera;
    scratch.gpr[5] = camera + 0x50;
    scratch.gpr[6] = camera + 0x5c;
    recomp_lookup(rt, MM_CTL_BG_CHECK)(rt, &scratch);
    for (unsigned i = 0; i < 3; i++)
        offset[i] = at[i] - read_float(rt, camera + 0x5c + i * 4);
    double angle_unit = 32768.0 / 3.141592653589793;
    recomp_store16(rt, camera + 0x134, (uint16_t)(int32_t)(atan2(offset[1], hypot(offset[0], offset[2])) * angle_unit));
    recomp_store16(rt, camera + 0x136, (uint16_t)(int32_t)(atan2(offset[0], offset[2]) * angle_unit));
}

void mm_camera_update(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // Camera_Update returns a Vec3s through a0, so the Camera pointer is a1.
    uint32_t camera = (uint32_t)ctx->gpr[5];
    normal_view = false;
    original(rt, ctx);
    if (!normal_view) {
        if (play && camera == play + 0x220) {
            orbiting = false;
            recomp_mouse_clear(&mouse);
        }
        return;
    }
    if (recomp_fov_enabled(&fov) && player_camera(rt, camera)) {
        uint32_t field = play + 0xd0;
        double native = read_float(rt, field);
        if (native > 0.0 && native < 179.0) {
            double scale = tan(fov.horizontal * radians / 2) / tan(fov.original * radians / 2);
            write_float(rt, field, (float)(2 * atan(tan(native * radians / 2) * scale) / radians));
        }
    }
}
