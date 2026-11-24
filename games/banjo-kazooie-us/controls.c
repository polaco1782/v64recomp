#include "controls.h"
#include "fov_control.h"
#include "game.h"
#include "game_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const double radians = 0.017453292519943295;
static const double stock_aspect = 1.35185182;
static RecompFov fov;
static RecompMouseBuffer mouse;
static bool enabled, had_dialogue;
static double sensitivity = 0.08, elevation, node_yaw, fov_scale = 1.0, expansion = 1.0;
enum { CAMERA_LOOK = 1, CAMERA_FOLLOW = 0xb, CAMERA_CLIMB = 0x10, CAMERA_NODE = 0x11 };
static unsigned last_camera, last_map;
static uint32_t wide_world_display_list;

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

static bool playing(Runtime *rt) {
    if (!rt->section_loaded[BK_CORE2_SECTION] || recomp_load32(rt, BK_GAME_MODE) != 3)
        return false;
    unsigned level = recomp_load8(rt, BK_LEVEL);
    return level != 0 && level != 0xd && recomp_load32(rt, BK_MAP) != 0x91;
}

static void adjust_fov(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        fov_scale = tan(fov.horizontal * radians / 2) / tan(fov.original * radians / 2);
}

static bool widescreen_playing(Runtime *rt) {
    return playing(rt) && !recomp_load8(rt, BK_TRANSITION) && recomp_load16(rt, BK_DIALOG_TEXT) == 0xffff;
}

static bool update_aspect(void *opaque, float aspect) {
    Runtime *rt = opaque;
    const double native = 4.0 / 3.0;
    expansion = 1.0;
    if (isfinite(aspect) && aspect > 4.0f / 3.0f && aspect <= 21.0f / 9.0f && widescreen_playing(rt)) {
        expansion = (double)aspect / native;
        return true;
    }
    return false;
}

void banjo_controls_init(Runtime *rt, RecompRenderer *renderer) {
    const char *option = getenv("RECOMP_BANJO_MOUSE");
    enabled = !option || strcmp(option, "0");
#ifdef RECOMP_DEBUG
    enabled = enabled && !recomp_game_replaying();
#endif
    const char *value = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (enabled && value && *value) {
        char *end;
        sensitivity = strtod(value, &end);
        recomp_game_require(end != value && !*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    had_dialogue = false;
    wide_world_display_list = 0;
    recomp_fov_init(&fov, 40.0, stock_aspect);
    recomp_renderer_set_fov_hook(renderer, adjust_fov);
    const char *wide = getenv("RECOMP_BANJO_WIDESCREEN");
    if (!wide || strcmp(wide, "0"))
        recomp_renderer_set_aspect_hook(renderer, update_aspect, rt);
}

// bsDroneLook_update reads these angles, applies the stick and updates Banjo's facing.
static void first_person_look(Runtime *rt, RecompMouseFrame frame) {
    if (!frame.dx && !frame.dy)
        return;
    double pitch = remainder(read_float(rt, BK_LOOK_ROTATION), 360.0) - frame.dy * sensitivity;
    pitch = fmax(-55.0, fmin(70.0, pitch));
    double yaw = fmod(read_float(rt, BK_LOOK_ROTATION + 4) - frame.dx * sensitivity, 360.0);
    write_float(rt, BK_LOOK_ROTATION, (float)(pitch < 0 ? pitch + 360.0 : pitch));
    write_float(rt, BK_LOOK_ROTATION + 4, (float)(yaw < 0 ? yaw + 360.0 : yaw));
}

bool banjo_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    bool live = enabled && playing(rt) && !recomp_load8(rt, BK_TRANSITION);
    bool dialogue = live && recomp_load16(rt, BK_DIALOG_TEXT) != 0xffff;
    bool camera_live = live && !dialogue && !had_dialogue;
    had_dialogue = dialogue;
    // Keep the cursor captured and WASD available, but leave A/B dialogue
    // aliases and physical pad buttons intact. Ignore camera deltas through
    // the first poll after closing the dialogue so they cannot spill over.
    recomp_renderer_separate_mouse_actions(renderer, enabled && !dialogue);
    recomp_renderer_mouse_mode(renderer, enabled, live);
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    unsigned camera = 0, map = 0;
    if (frame.active && camera_live) {
        map = recomp_load32(rt, BK_MAP);
        if (recomp_load32(rt, BK_CAMERA_TYPE) == 2) {
            unsigned state = recomp_load8(rt, BK_CAMERA_STATE);
            unsigned look = recomp_load8(rt, BK_FIRST_PERSON_STATE);
            if (look == 2)
                camera = CAMERA_LOOK;
            else if ((look == 0 || look == 4) &&
                     (state == CAMERA_FOLLOW || state == CAMERA_CLIMB || state == CAMERA_NODE))
                camera = state;
        }
    }
    if (camera != last_camera || map != last_map || !camera) {
        recomp_mouse_clear(&mouse);
        elevation = node_yaw = 0;
    }
    last_camera = camera;
    last_map = map;
    if (camera == CAMERA_LOOK)
        first_person_look(rt, frame);
    else if (camera)
        recomp_mouse_submit(&mouse, frame);
    if (!frame.active || !live)
        return true;
    if (frame.move_x || frame.move_y) {
        int speed = (frame.actions & RECOMP_MOUSE_SPEED) ? 40 : 80;
        if (frame.move_x && frame.move_y)
            speed = speed * 7 / 10;
        pad->stick_x = (int8_t)(frame.move_x * speed);
        pad->stick_y = (int8_t)(frame.move_y * speed);
    }
    if (frame.actions & RECOMP_MOUSE_JUMP)
        pad->buttons |= 0x8000;
    if (dialogue)
        return true;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        pad->buttons |= 0x4000;
    if (frame.actions & (RECOMP_MOUSE_SECONDARY_ATTACK | RECOMP_MOUSE_DEFEND))
        pad->buttons |= 0x2000;
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
        pad->buttons |= 0x0002;
    if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
        pad->buttons |= 0x0001;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= 0x0008;
    if (frame.actions & RECOMP_MOUSE_USE)
        pad->buttons |= 0x0004;
    return true;
}

// The guest constructs an eye and focus on its stack. Rotate their difference
// before the camera applies its own smoothing, obstruction checks and final aim.
static void orbit(Runtime *rt, uint32_t goal, uint32_t focus, uint32_t yaw_address) {
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active || !(frame.dx || frame.dy || elevation || node_yaw))
        return;
    double fx = read_float(rt, focus), fy = read_float(rt, focus + 4), fz = read_float(rt, focus + 8);
    double x = read_float(rt, goal) - fx, y = read_float(rt, goal + 4) - fy, z = read_float(rt, goal + 8) - fz;
    double ring = hypot(x, z);
    if (ring <= 0.01)
        return;
    double yaw;
    if (yaw_address) {
        yaw = read_float(rt, yaw_address) - frame.dx * sensitivity;
        yaw = fmod(yaw, 360.0);
        if (frame.dx)
            write_float(rt, yaw_address, (float)(yaw < 0 ? yaw + 360.0 : yaw));
    } else {
        // A map camera node supplies a fresh heading each update.
        node_yaw = remainder(node_yaw - frame.dx * sensitivity, 360.0);
        yaw = atan2(x, z) / radians + node_yaw;
    }
    double native = atan2(y, ring) / radians;
    double pitch = fmin(75.0, fmax(-65.0, native + elevation + frame.dy * sensitivity));
    elevation = pitch - native;
    double radius = hypot(y, ring);
    ring = radius * cos(pitch * radians);
    write_float(rt, goal, (float)(fx + ring * sin(yaw * radians)));
    write_float(rt, goal + 4, (float)(fy + radius * sin(pitch * radians)));
    write_float(rt, goal + 8, (float)(fz + ring * cos(yaw * radians)));
}

void banjo_camera_goal(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t caller = (uint32_t)ctx->gpr[31], goal = (uint32_t)ctx->gpr[4];
    if (caller == BK_FOLLOW_GOAL_RETURN && last_camera == CAMERA_FOLLOW)
        orbit(rt, goal, goal + 12, BK_FOLLOW_YAW);
    else if (caller == BK_CLIMB_GOAL_RETURN && last_camera == CAMERA_CLIMB)
        orbit(rt, goal, goal + 12, BK_CLIMB_YAW);
    else if (caller == BK_NODE_GOAL_RETURN && last_camera == CAMERA_NODE)
        orbit(rt, goal, goal + 0x40, 0);
    original(rt, ctx);
}

void banjo_world_fov(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    wide_world_display_list = 0;
    if (fov_scale != 1.0 && playing(rt) && read_float(rt, BK_VIEWPORT_FOVY) == 40.0f) {
        const uint32_t display_list = (uint32_t)ctx->gpr[4];
        write_float(rt, BK_VIEWPORT_FOVY, (float)(2.0 * atan(tan(20.0 * radians) * fov_scale) / radians));
        original(rt, ctx);
        write_float(rt, BK_VIEWPORT_FOVY, 40.0f);
        wide_world_display_list = display_list;
    } else
        original(rt, ctx);
}

void banjo_world_projection(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    const uint32_t caller = (uint32_t)ctx->gpr[31];
    if (expansion > 1.0 && (caller == BK_WORLD_VIEWPORT_FIRST_RETURN || caller == BK_WORLD_VIEWPORT_SECOND_RETURN) &&
        widescreen_playing(rt)) {
        const float aspect = read_float(rt, BK_VIEWPORT_ASPECT);
        if (aspect > 0.0f && isfinite(aspect)) {
            write_float(rt, BK_VIEWPORT_ASPECT, (float)(aspect * expansion));
            original(rt, ctx);
            write_float(rt, BK_VIEWPORT_ASPECT, aspect);
            return;
        }
    }
    original(rt, ctx);
}

void banjo_zoombox_backdrop(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // The backdrop uses the stock lens to turn screen coordinates into a
    // model position. Reset the display-list lens as well, before the first
    // backdrop; the portrait then restores stock perspective after its ortho draw.
    if (wide_world_display_list && (uint32_t)ctx->gpr[5] == wide_world_display_list &&
        (uint32_t)ctx->gpr[31] == BK_ZOOMBOX_BACKDROP_RETURN) {
        MipsContext viewport = *ctx;
        viewport.gpr[4] = ctx->gpr[5]; // Gfx **
        viewport.gpr[5] = ctx->gpr[6]; // Mtx **
        viewport.gpr[29] -= 16;        // Separate o32 argument home area.
        wide_world_display_list = 0;
        recomp_lookup(rt, BK_LOAD_VIEWPORT_PERSPECTIVE)(rt, &viewport);
    }
    original(rt, ctx);
}

void banjo_view_planes(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    original(rt, ctx);
    if ((fov_scale == 1.0 && expansion == 1.0) || !playing(rt))
        return;
    // Opposing unit planes have equal depth components. Their half-sum
    // isolates that component, including the plane's offset from the origin.
    for (unsigned pair = 0; pair < 2; pair++) {
        double horizontal = 1.0;
        if (pair == 0 && expansion > 1.0 && widescreen_playing(rt))
            horizontal = expansion;
        double scale = fov_scale * horizontal;
        if (scale == 1.0)
            continue;
        uint32_t base = BK_FRUSTUM_PLANES + pair * 32;
        double planes[2][4];
        for (unsigned i = 0; i < 4; i++) {
            double a = read_float(rt, base + 4 * i), b = read_float(rt, base + 16 + 4 * i);
            double depth = (scale - 1.0) * (a + b) * 0.5;
            planes[0][i] = a + depth;
            planes[1][i] = b + depth;
        }
        for (unsigned n = 0; n < 2; n++) {
            double *p = planes[n];
            double length = sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            for (unsigned i = 0; i < 4; i++)
                write_float(rt, base + 16 * n + 4 * i, (float)(p[i] / length));
        }
    }
}
