#include "controls.h"
#include "quake_controls_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled, projection_valid, wide_render;
static double sensitivity, invert_y, matrix_scale;
static RecompFov fov;
static RecompMouseBuffer mouse;
static uint32_t native_projection[16];
static void (*poll_live_input)(Runtime *, void *);
#define radians (3.14159265358979323846 / 180)

static float from_bits(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}
static float read_float(Runtime *rt, uint32_t address) {
    return from_bits(recomp_load32(rt, address));
}
static void write_float(Runtime *rt, uint32_t address, float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    recomp_store32(rt, address, bits);
}
static bool valid(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80400000 && !(address & 3) && size <= rt->rdram_size &&
           (address & 0x1fffffff) <= rt->rdram_size - size;
}
static uint32_t live_entity(Runtime *rt) {
    if (recomp_load32(rt, Q_CTL_RUNNING) != 1 || recomp_load32(rt, Q_CTL_PHASE) ||
        recomp_load32(rt, Q_CTL_PLAYERS) != 1 || recomp_load32(rt, Q_CTL_INTERMISSION) ||
        recomp_load32(rt, Q_CTL_CLIENTS + Q_CTL_CONTROLLER))
        return 0;
    uint32_t entity = recomp_load32(rt, Q_CTL_CLIENTS + Q_CTL_ENTITY_POINTER);
    if (!valid(rt, entity, Q_CTL_MOVE_TYPE + 4) || !recomp_load16(rt, entity + Q_CTL_MOVE_TYPE))
        return 0;
    float health = read_float(rt, entity + Q_CTL_HEALTH);
    return isfinite(health) && health > 0 ? entity : 0;
}
void quake_controls_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    projection_valid = wide_render = false;
    matrix_scale = 1;
    recomp_mouse_clear(&mouse);
    const char *setting = getenv("RECOMP_QUAKE_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_QUAKE_INPUT"))
        enabled = false;
#endif
    sensitivity = 0.08;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(end != setting && !*end && isfinite(sensitivity) && sensitivity >= 0.001 &&
                                sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    invert_y = setting && !strcmp(setting, "1") ? -1 : 1;
    double near_plane = from_bits(Q_CTL_NEAR_BITS), top = from_bits(Q_CTL_TOP_BITS);
    recomp_fov_init(&fov, 2 * atan(top / near_plane) / radians, near_plane / top);
}
void quake_fov_adjust(int steps) {
    recomp_fov_adjust(&fov, steps);
}
bool quake_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_mouse_mode(renderer, enabled, enabled && live_entity(rt));
    if (!recomp_renderer_input(renderer, pad)) {
        recomp_mouse_clear(&mouse);
        return false;
    }
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    recomp_mouse_submit(&mouse, frame);
    if (frame.active) {
        // Native Start/pause remains available; the guest command owns actions.
        pad->buttons &= 0x1000;
        pad->stick_x = pad->stick_y = 0;
    }
    return true;
}
void quake_command_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t entity = live_entity(rt);
    if (!enabled || !entity || (uint32_t)ctx->gpr[31] != Q_CTL_COMMAND_RETURN ||
        recomp_load32(rt, Q_CTL_CURRENT_CLIENT) != Q_CTL_CLIENTS || recomp_load32(rt, Q_CTL_CURRENT_ENTITY) != entity) {
        recomp_mouse_clear(&mouse);
        original(rt, ctx);
        return;
    }
    if (poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    uint32_t command = Q_CTL_CLIENTS + Q_CTL_COMMAND;
    bool fixed = recomp_load16(rt, entity + Q_CTL_FIX_ANGLE) != 0;
    float pitch = read_float(rt, entity + (fixed ? Q_CTL_MODEL_PITCH : Q_CTL_PITCH));
    float yaw = read_float(rt, entity + (fixed ? Q_CTL_MODEL_YAW : Q_CTL_YAW));
    uint32_t previous = recomp_load32(rt, command + Q_CTL_PREVIOUS);
    original(rt, ctx);
    if (!frame.active || live_entity(rt) != entity || !isfinite(pitch) || !isfinite(yaw) || !isfinite(frame.dx) ||
        !isfinite(frame.dy)) {
        recomp_mouse_clear(&mouse);
        return;
    }
    float next_yaw = (float)fmod(yaw - frame.dx * sensitivity, 360.0);
    if (next_yaw < 0)
        next_yaw += 360;
    float next_pitch =
        (float)fmax(Q_CTL_PITCH_MIN, fmin(from_bits(Q_CTL_PITCH_MAX_BITS), pitch + frame.dy * sensitivity * invert_y));
    write_float(rt, entity + Q_CTL_YAW, next_yaw);
    write_float(rt, entity + Q_CTL_PITCH, next_pitch);
    write_float(rt, command + Q_CTL_DELTA_YAW, (float)(-frame.dx * sensitivity));
    write_float(rt, command + Q_CTL_DELTA_PITCH, next_pitch - pitch);
    bool run = (recomp_load32(rt, Q_CTL_AUTORUN) != 0) != ((frame.actions & RECOMP_MOUSE_SPEED) != 0);
    write_float(rt, command + Q_CTL_FORWARD, (float)(frame.move_y * Q_CTL_FORWARD_SPEED * (run ? 2 : 1)));
    write_float(rt, command + Q_CTL_SIDE, (float)(frame.move_x * Q_CTL_SIDE_SPEED * (run ? 2 : 1)));
    write_float(rt, command + Q_CTL_UP, 0);
    uint32_t flags = run ? 0x20 : 0;
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        flags |= 1;
    if (frame.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_SECONDARY_ATTACK))
        flags |= 4;
    bool next = (frame.actions & RECOMP_MOUSE_NEXT_WEAPON) != 0;
    bool back = (frame.actions & (RECOMP_MOUSE_PREVIOUS_WEAPON | RECOMP_MOUSE_USE)) != 0;
    if (frame.wheel) {
        next = frame.wheel < 0;
        back = frame.wheel > 0;
        previous &= ~0x300u;
    }
    if (next)
        flags |= 0x100 | ((previous & 0x100) ? 0 : 8);
    if (back)
        flags |= 0x200 | ((previous & 0x200) ? 0 : 16);
    recomp_store32(rt, command + Q_CTL_FLAGS, flags);
    recomp_store32(rt, command + Q_CTL_PREVIOUS, flags);
    recomp_store32(rt, entity + Q_CTL_ENTITY_BUTTONS, flags & 0x1f);
}
void quake_frustum_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool world = (uint32_t)ctx->gpr[31] == Q_CTL_FRUSTUM_RETURN && (uint32_t)ctx->gpr[4] == Q_CTL_PROJECTION_MATRIX;
    original(rt, ctx);
    if (world) {
        for (unsigned i = 0; i < 16; i++)
            native_projection[i] = recomp_load32(rt, Q_CTL_PROJECTION_MATRIX + i * 4);
        projection_valid = true;
        matrix_scale = 1;
    }
}
static void projection(Runtime *rt, double scale) {
    if (!projection_valid || scale == matrix_scale)
        return;
    uint32_t matrix = Q_CTL_PROJECTION_MATRIX;
    for (unsigned i = 0; i < 16; i++)
        recomp_store32(rt, matrix + i * 4, native_projection[i]);
    if (scale != 1) {
        const unsigned diagonal[] = {0, 5};
        for (unsigned i = 0; i < 2; i++) {
            unsigned offset = diagonal[i] * 2;
            int32_t fixed = (int32_t)((int16_t)recomp_load16(rt, matrix + offset) * 65536.0 +
                                      recomp_load16(rt, matrix + 32 + offset));
            int32_t adjusted = (int32_t)(fixed / scale);
            recomp_store16(rt, matrix + offset, (uint16_t)((uint32_t)adjusted >> 16));
            recomp_store16(rt, matrix + 32 + offset, (uint16_t)adjusted);
        }
    }
    matrix_scale = scale;
}
void quake_render_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool previous = wide_render;
    wide_render = projection_valid && recomp_fov_enabled(&fov) && live_entity(rt) &&
                  recomp_load32(rt, Q_CTL_CURRENT_CLIENT) == Q_CTL_CLIENTS;
    projection(rt, wide_render ? tan(fov.horizontal * radians / 2) : 1);
    original(rt, ctx);
    wide_render = previous;
    // Keep the selected matrix in RAM: the RSP task reads it after this returns.
}
void quake_planes_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if (!wide_render || (uint32_t)ctx->gpr[31] != Q_CTL_PLANES_RETURN) {
        original(rt, ctx);
        return;
    }
    uint32_t saved[6];
    double view = tan(fov.horizontal * radians / 2);
    double horizontal = tan((fov.horizontal / 2 + 1) * radians);
    double vertical = fmax(1, tan(atan(view * from_bits(Q_CTL_TOP_BITS) / from_bits(Q_CTL_NEAR_BITS)) + radians));
    for (unsigned i = 0; i < 6; i++) {
        uint32_t address = (i < 3 ? Q_CTL_RIGHT : Q_CTL_UP_VECTOR) + (i % 3) * 4;
        saved[i] = recomp_load32(rt, address);
        write_float(rt, address, (float)(from_bits(saved[i]) / (i < 3 ? horizontal : vertical)));
    }
    // Original plane normals, distances, types and sign bits remain guest code.
    original(rt, ctx);
    for (unsigned i = 0; i < 6; i++)
        recomp_store32(rt, (i < 3 ? Q_CTL_RIGHT : Q_CTL_UP_VECTOR) + (i % 3) * 4, saved[i]);
}
