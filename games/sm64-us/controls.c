#include "controls.h"
#include "fov_control.h"
#include "game.h"
#include "game_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum {
    AREA_CAMERA = 0x24,
    CAMERA_YAW = 0x02,
    CAMERA_C_UP = 6,
    CAMERA_INSIDE_CANNON = 10,
    CAMERA_CUTSCENE = 0x30,
    GEO_CONTEXT_RENDER = 1,
    PERSPECTIVE_FOV = 0x1c,
    NODE_CAMERA = 0x18,
    NODE_EYE = 0x1c,
    NODE_FOCUS = 0x28,
    LAKITU_EYE = 0x8c,
    MARIO_INTENDED_MAG = 0x20,
    MARIO_INTENDED_YAW = 0x24,
    MARIO_POS = 0x3c,
    MARIO_OBJECT = 0x88,
    MARIO_AREA = 0x90,
    C_RIGHT = 0x0001,
    C_LEFT = 0x0002,
    C_DOWN = 0x0004,
    C_UP = 0x0008,
    N64_R = 0x0010,
    N64_Z = 0x2000,
    N64_B = 0x4000,
    N64_A = 0x8000,
};

static const double degrees = 180.0 / 3.14159265358979323846;
static const double stock_vertical = 45.0, stock_aspect = 4.0 / 3.0;
static RecompFov fov;
static double fov_scale = 1.0;
static double sensitivity = 0.08;
static bool mouse_enabled;

// Unit vector from Mario to the rendered eye. Lakitu keeps running the stock camera.
typedef struct {
    double x, y, z;
} Vec3;
static struct {
    uint32_t camera;
    double yaw, pitch, radius;
    Vec3 direction;
} orbit;

static uint32_t float_bits(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float read_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static Vec3 read_vec(Runtime *rt, uint32_t address) {
    return (Vec3){read_float(rt, address), read_float(rt, address + 4), read_float(rt, address + 8)};
}

static void write_vec(Runtime *rt, uint32_t address, Vec3 v) {
    recomp_store32(rt, address, float_bits((float)v.x));
    recomp_store32(rt, address + 4, float_bits((float)v.y));
    recomp_store32(rt, address + 8, float_bits((float)v.z));
}

static uint32_t active_camera(Runtime *rt) {
    const uint32_t area = recomp_load32(rt, GAME_GCURRENTAREA);
    if (!area)
        return 0;
    const uint32_t camera = recomp_load32(rt, area + AREA_CAMERA);
    return camera && !recomp_load8(rt, camera + CAMERA_CUTSCENE) ? camera : 0;
}

static uint32_t player_camera(Runtime *rt) {
    if (!mouse_enabled || recomp_load16(rt, GAME_SCURRPLAYMODE) || recomp_load32(rt, GAME_GCURRDEMOINPUT))
        return 0;
    uint32_t mario = recomp_load32(rt, GAME_GMARIOSTATE);
    if (!mario || !recomp_load32(rt, mario + MARIO_OBJECT) ||
        recomp_load32(rt, mario + MARIO_AREA) != recomp_load32(rt, GAME_GCURRENTAREA))
        return 0;
    uint32_t camera = active_camera(rt);
    return camera == recomp_load32(rt, GAME_GCAMERA) ? camera : 0;
}

static bool orbit_active(Runtime *rt) {
    if (!orbit.camera)
        return false;
    if (orbit.camera == player_camera(rt)) {
        uint8_t mode = recomp_load8(rt, orbit.camera);
        if (mode != CAMERA_C_UP && mode != CAMERA_INSIDE_CANNON)
            return true;
    }
    orbit.camera = 0;
    return false;
}

static Vec3 orbit_focus(Runtime *rt) {
    Vec3 focus = read_vec(rt, recomp_load32(rt, GAME_GMARIOSTATE) + MARIO_POS);
    focus.y += 125.0;
    return focus;
}

static void move_orbit(Runtime *rt, uint32_t camera, RecompMouseFrame frame) {
    if (!orbit.camera) {
        Vec3 focus = orbit_focus(rt), eye = read_vec(rt, GAME_GLAKITUSTATE + LAKITU_EYE);
        double x = eye.x - focus.x, y = eye.y - focus.y, z = eye.z - focus.z;
        double horizontal = hypot(x, z);
        orbit.radius = hypot(horizontal, y);
        if (!isfinite(orbit.radius) || orbit.radius < 1.0)
            return;
        orbit.yaw = atan2(x, z);
        orbit.pitch = atan2(y, horizontal);
        orbit.camera = camera;
    }
    orbit.yaw = remainder(orbit.yaw - frame.dx * sensitivity / degrees, 360.0 / degrees);
    orbit.pitch = fmax(-75.0 / degrees, fmin(75.0 / degrees, orbit.pitch + frame.dy * sensitivity / degrees));
    orbit.direction = (Vec3){sin(orbit.yaw) * cos(orbit.pitch), sin(orbit.pitch), cos(orbit.yaw) * cos(orbit.pitch)};
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
    const char *value = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (mouse_enabled && value && *value) {
        char *end;
        sensitivity = strtod(value, &end);
        recomp_game_require(end != value && !*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    recomp_renderer_separate_mouse_actions(renderer, mouse_enabled);
    recomp_fov_init(&fov, stock_vertical, stock_aspect);
    recomp_renderer_set_fov_hook(renderer, adjust_fov);
}

bool sm64_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    uint32_t camera = player_camera(rt);
    recomp_renderer_mouse_mode(renderer, mouse_enabled, camera != 0);
    if (!recomp_renderer_input(renderer, pad))
        return false;
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    if (!frame.active || !camera) {
        orbit.camera = 0;
        return true;
    }

    if (frame.move_x || frame.move_y) {
        int speed = (frame.actions & RECOMP_MOUSE_SPEED) ? 40 : 80;
        if (frame.move_x && frame.move_y)
            speed = speed * 7 / 10;
        pad->stick_x = (int8_t)(frame.move_x * speed);
        pad->stick_y = (int8_t)(frame.move_y * speed);
    }
    if (frame.actions & RECOMP_MOUSE_JUMP)
        pad->buttons |= N64_A;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        pad->buttons |= N64_B;
    if (frame.actions & (RECOMP_MOUSE_DEFEND | RECOMP_MOUSE_SECONDARY_ATTACK))
        pad->buttons |= N64_Z;
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
        pad->buttons |= C_LEFT;
    if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
        pad->buttons |= C_RIGHT;
    if (frame.actions & RECOMP_MOUSE_USE)
        pad->buttons |= C_DOWN;
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= C_UP;

    uint8_t mode = recomp_load8(rt, camera);
    if (orbit.camera != camera)
        orbit.camera = 0;
    if (mode == CAMERA_C_UP || mode == CAMERA_INSIDE_CANNON ||
        (pad->buttons & (C_LEFT | C_RIGHT | C_UP | C_DOWN | N64_R)))
        orbit.camera = 0;
    else if (frame.dx || frame.dy)
        move_orbit(rt, camera, frame);
    return true;
}

// Clip the focus-to-eye segment against the same live static and moving
// triangles used by SM64. The original camera's collision checks face Lakitu.
static Vec3 surface_vertex(Runtime *rt, uint32_t address) {
    return (Vec3){(int16_t)recomp_load16(rt, address), (int16_t)recomp_load16(rt, address + 2),
                  (int16_t)recomp_load16(rt, address + 4)};
}

static Vec3 difference(Vec3 a, Vec3 b) {
    return (Vec3){a.x - b.x, a.y - b.y, a.z - b.z};
}

static Vec3 cross(Vec3 a, Vec3 b) {
    return (Vec3){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static double dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double visible_radius(Runtime *rt, Vec3 focus) {
    double radius = orbit.radius;
    uint32_t pool = recomp_load32(rt, GAME_SSURFACEPOOL), count = recomp_load32(rt, GAME_GSURFACESALLOCATED);
    // alloc_surface_pools at 0x80383340 allocates 2300 surfaces.
    if (!pool || count > 2300)
        return radius;
    double end_y = focus.y + orbit.direction.y * radius;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t surface = pool + i * 0x30;
        if ((int16_t)recomp_load16(rt, surface + 6) > fmax(focus.y, end_y) ||
            (int16_t)recomp_load16(rt, surface + 8) < fmin(focus.y, end_y) || (recomp_load8(rt, surface + 4) & 2) ||
            recomp_load16(rt, surface) == 0x12)
            continue;
        Vec3 a = surface_vertex(rt, surface + 0xa);
        Vec3 ab = difference(surface_vertex(rt, surface + 0x10), a);
        Vec3 ac = difference(surface_vertex(rt, surface + 0x16), a);
        Vec3 p = cross(orbit.direction, ac);
        double det = dot(ab, p);
        if (det <= 1e-6)
            continue;
        Vec3 from_a = difference(focus, a);
        double u = dot(from_a, p);
        if (u < 0 || u > det)
            continue;
        Vec3 q = cross(from_a, ab);
        double v = dot(orbit.direction, q);
        if (v < 0 || u + v > det)
            continue;
        double distance = dot(ac, q) / det;
        if (distance > 0 && distance < radius + 20.0)
            radius = fmax(1.0, distance - 20.0);
    }
    return radius;
}

void sm64_render_camera(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t node = (uint32_t)ctx->gpr[4];
    original(rt, ctx);
    if (!orbit_active(rt) || recomp_load32(rt, node + NODE_CAMERA) != orbit.camera)
        return;
    Vec3 focus = orbit_focus(rt);
    double radius = visible_radius(rt, focus);
    Vec3 eye = {focus.x + radius * orbit.direction.x, focus.y + radius * orbit.direction.y,
                focus.z + radius * orbit.direction.z};
    write_vec(rt, node + NODE_FOCUS, focus);
    write_vec(rt, node + NODE_EYE, eye);
}

void sm64_skybox_camera(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (orbit_active(rt)) {
        // The skybox reads only focus minus eye. Its o32 arguments are posX
        // in a3, posY/Z at sp+0x10, and focus at sp+0x18.
        uint32_t sp = (uint32_t)ctx->gpr[29];
        ctx->gpr[7] = mips_s32(float_bits((float)orbit.direction.x));
        recomp_store32(rt, sp + 0x10, float_bits((float)orbit.direction.y));
        recomp_store32(rt, sp + 0x14, float_bits((float)orbit.direction.z));
        write_vec(rt, sp + 0x18, (Vec3){0});
    }
    original(rt, ctx);
}

void sm64_movement_yaw(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t mario = (uint32_t)ctx->gpr[4];
    original(rt, ctx);
    if (!orbit_active(rt) || mario != recomp_load32(rt, GAME_GMARIOSTATE) ||
        read_float(rt, mario + MARIO_INTENDED_MAG) <= 0)
        return;
    uint16_t intended = recomp_load16(rt, mario + MARIO_INTENDED_YAW);
    uint16_t native_yaw = recomp_load16(rt, orbit.camera + CAMERA_YAW);
    int view_yaw = (int)lround(orbit.yaw * degrees * (65536.0 / 360.0));
    recomp_store16(rt, mario + MARIO_INTENDED_YAW, (uint16_t)(intended - native_yaw + view_yaw));
}

void sm64_camera_fov(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    const bool render = (uint32_t)ctx->gpr[4] == GEO_CONTEXT_RENDER;
    const uint32_t node = (uint32_t)ctx->gpr[5];
    original(rt, ctx);
    if (!render || !node || !recomp_fov_enabled(&fov) || !active_camera(rt))
        return;
    const float vertical = read_float(rt, node + PERSPECTIVE_FOV);
    if (!isfinite(vertical) || vertical <= 0.0f || vertical >= 180.0f)
        return;
    // The same perspective node feeds projection and the native object culler.
    // Leave the camera's native FOV/zoom state untouched for the next update.
    const float adjusted = (float)(2 * degrees * atan(tan(vertical / (2 * degrees)) * fov_scale));
    recomp_store32(rt, node + PERSPECTIVE_FOV, float_bits(adjusted));
}
