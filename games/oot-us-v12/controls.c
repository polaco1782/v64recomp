#include "controls.h"
#include "oot_controls_config.h"
#include "game.h"
#include "fov_control.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool mouse_enabled, gamepad_camera_enabled, fov_enabled, orbit_active;
static bool orbit_seen;
static RecompFov fov;
static uint32_t play, orbit_camera, updating_camera;
static double sensitivity, invert_y, orbit_yaw, orbit_pitch, fov_scale;
static RecompMouseBuffer mouse;
static RecompCameraStick camera_stick;
static uint64_t camera_time;
static void (*poll_live_input)(Runtime *, void *);
static float load_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void store_float(Runtime *rt, uint32_t address, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    recomp_store32(rt, address, bits);
}
static bool valid(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80800000 && !(address & 3) && size <= rt->rdram_size &&
           (address & 0x1fffffff) <= rt->rdram_size - size;
}
static void clear(void) {
    recomp_mouse_clear(&mouse);
    camera_stick = (RecompCameraStick){0};
    camera_time = 0;
    orbit_active = false;
    orbit_camera = 0;
}
static bool live_play(Runtime *rt) {
    return valid(rt, play, OOT_CTL_TRANSITION + 1) &&
           recomp_load32(rt, play + OOT_CTL_MAIN_FIELD) == OOT_CTL_PLAY_MAIN &&
           recomp_load32(rt, play + OOT_CTL_RUNNING) && !recomp_load32(rt, OOT_CTL_GAME_MODE) &&
           recomp_load16(rt, OOT_CTL_HEALTH) && !recomp_load16(rt, play + OOT_CTL_PAUSE) &&
           !recomp_load16(rt, play + OOT_CTL_GAME_OVER) && !recomp_load8(rt, play + OOT_CTL_MESSAGE) &&
           !recomp_load8(rt, play + OOT_CTL_CUTSCENE) && !recomp_load8(rt, play + OOT_CTL_TRANSITION);
}
static uint32_t main_camera(Runtime *rt) {
    if (!live_play(rt) || recomp_load16(rt, play + OOT_CTL_ACTIVE_CAMERA))
        return 0;
    uint32_t camera = recomp_load32(rt, play + OOT_CTL_CAMERA_PTRS);
    return valid(rt, camera, 0x16c) && recomp_load32(rt, camera + OOT_CTL_CAMERA_PLAY) == play &&
                   recomp_load16(rt, camera + OOT_CTL_STATUS) == 7
               ? camera
               : 0;
}
static bool oot_controls_playable(Runtime *rt) {
    return (mouse_enabled || gamepad_camera_enabled) && main_camera(rt);
}
void oot_controls_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    play = updating_camera = 0;
    clear();
    const char *setting = getenv("RECOMP_OOT_MOUSE");
    mouse_enabled = !setting || strcmp(setting, "0");
    setting = getenv("RECOMP_OOT_GAMEPAD_CAMERA");
    gamepad_camera_enabled = setting && !strcmp(setting, "1");
    if (gamepad_camera_enabled)
        mouse_enabled = false;
#ifdef RECOMP_DEBUG
    if (recomp_game_replaying())
        mouse_enabled = gamepad_camera_enabled = false;
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
}
void oot_fov_init(void) {
    recomp_fov_init(&fov, OOT_CTL_NATIVE_FOV, 4.0 / 3.0);
    fov_enabled = false;
}
static void oot_fov_set(double horizontal) {
    recomp_fov_set(&fov, horizontal);
    fov_enabled = recomp_fov_enabled(&fov);
    const double radians = 3.14159265358979323846 / 180;
    fov_scale = tan(fov.horizontal * radians / 2) / ((4.0 / 3.0) * tan(OOT_CTL_NATIVE_FOV * radians / 2));
}
void oot_fov_adjust(int steps) {
    if (recomp_fov_adjust(&fov, steps))
        oot_fov_set(fov.horizontal);
}
static void oot_controls_submit(RecompMouseFrame frame, RecompPad *pad) {
    if (!frame.active)
        clear();
    recomp_mouse_submit(&mouse, frame);
    if (!frame.active)
        return;
    pad->buttons &= 0x100f; // Keep Start and C-buttons; map adventure actions explicitly.
    if (frame.actions & RECOMP_MOUSE_ATTACK)
        pad->buttons |= 0x4000; // B: sword.
    if (frame.actions & (RECOMP_MOUSE_USE | RECOMP_MOUSE_JUMP))
        pad->buttons |= 0x8000; // A: action.
    if (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK)
        pad->buttons |= 0x2000; // Z: target.
    if (frame.actions & RECOMP_MOUSE_DEFEND)
        pad->buttons |= 0x0010; // R: shield.
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
        pad->buttons |= 0x0002; // Q: C-left.
    if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
        pad->buttons |= 0x0004; // R: C-down.
    if (frame.actions & RECOMP_MOUSE_STATUS)
        pad->buttons |= frame.actions & RECOMP_MOUSE_SPEED ? 0x0008 : 0x0001; // Shift+F: C-up.
    int speed = frame.actions & RECOMP_MOUSE_SPEED ? 40 : 80;
    pad->stick_x = (int8_t)(frame.move_x * speed);
    pad->stick_y = (int8_t)(frame.move_y * speed);
}
bool oot_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    bool playable = oot_controls_playable(rt);
    recomp_renderer_keyboard_c_buttons(renderer, mouse_enabled);
    recomp_renderer_mouse_mode(renderer, mouse_enabled, playable);
    recomp_renderer_gamepad_camera_mode(renderer, gamepad_camera_enabled);
    if (!recomp_renderer_input(renderer, pad)) {
        clear();
        return false;
    }
    if (gamepad_camera_enabled) {
        camera_stick = recomp_renderer_camera_stick(renderer);
        if (!playable || !camera_stick.active)
            clear();
    } else {
        oot_controls_submit(recomp_renderer_mouse_frame(renderer), pad);
    }
    return true;
}
void oot_play_update_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    if ((uint32_t)ctx->gpr[31] == OOT_CTL_PLAY_RETURN) {
        play = (uint32_t)ctx->gpr[4];
        if ((mouse_enabled || gamepad_camera_enabled) && poll_live_input)
            recomp_on_host(rt, poll_live_input, NULL);
    }
    original(rt, ctx);
    if (!live_play(rt))
        clear();
}
void oot_camera_update_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    // Vec3s return uses a hidden result pointer in a0; Camera* is in a1.
    uint32_t camera = (uint32_t)ctx->gpr[5], previous = updating_camera;
    bool previous_seen = orbit_seen;
    updating_camera = camera;
    orbit_seen = false;
    original(rt, ctx);
    bool seen = orbit_seen;
    updating_camera = previous;
    orbit_seen = previous_seen;
    if (camera == main_camera(rt) && (!seen || recomp_load16(rt, camera + OOT_CTL_MODE)))
        clear();
    if (!fov_enabled || camera != main_camera(rt))
        return;
    unsigned setting = recomp_load16(rt, camera + OOT_CTL_SETTING);
    if (setting >= sizeof(oot_normal_settings) || !oot_normal_settings[setting])
        return;
    uint32_t field = play + OOT_CTL_VIEW + OOT_CTL_VIEW_FOV;
    float native = load_float(rt, field);
    if (isfinite(native) && native > 0 && native < 179) {
        const double radians = 3.14159265358979323846 / 180;
        store_float(rt, field, (float)(2 * atan(tan(native * radians / 2) * fov_scale) / radians));
    }
}
void oot_normal_camera_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t camera = (uint32_t)ctx->gpr[4], caller = (uint32_t)ctx->gpr[31];
    if (camera == updating_camera)
        orbit_seen = true;
    original(rt, ctx);
    if ((!mouse_enabled && !gamepad_camera_enabled) || camera != updating_camera || camera != main_camera(rt) ||
        caller != OOT_CTL_DISPATCH_RETURN)
        return;
    if (recomp_load16(rt, camera + OOT_CTL_MODE)) {
        clear();
        return;
    }
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if ((!frame.active && !camera_stick.active) || (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK) ||
        (recomp_load16(rt, play + 0x14) & 0x2000) || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        clear();
        return;
    }
    double yaw_delta = frame.dx * sensitivity, pitch_delta = frame.dy * sensitivity * invert_y;
    if (camera_stick.active) {
        uint64_t now = recomp_game_monotonic();
        // A held stick is angular velocity, independent of host polling and
        // RT64 interpolation. Do not accumulate a jump across pauses or stalls.
        double seconds = camera_time ? fmin((now - camera_time) * 1e-9, 0.1) : 0;
        camera_time = now;
        yaw_delta += camera_stick.x * 120 * seconds;
        pitch_delta += camera_stick.y * 90 * seconds;
    }
    double at[3], next[3];
    for (unsigned i = 0; i < 3; i++) {
        at[i] = load_float(rt, camera + OOT_CTL_AT + i * 4);
        next[i] = load_float(rt, camera + OOT_CTL_EYE_NEXT + i * 4) - at[i];
        if (!isfinite(at[i]) || !isfinite(next[i])) {
            clear();
            return;
        }
    }
    double radius = load_float(rt, camera + OOT_CTL_DISTANCE);
    if (!isfinite(radius) || radius < 1 || radius > 10000) {
        clear();
        return;
    }
    if (!orbit_active || orbit_camera != camera) {
        if (!yaw_delta && !pitch_delta)
            return; // Leave native follow intact until camera input.
        orbit_yaw = atan2(next[0], next[2]);
        orbit_pitch = atan2(next[1], hypot(next[0], next[2]));
        orbit_active = true;
        orbit_camera = camera;
    }
    const double radians = 3.14159265358979323846 / 180;
    orbit_yaw = remainder(orbit_yaw - yaw_delta * radians, 2 * 3.14159265358979323846);
    orbit_pitch = fmax(-60 * radians, fmin(70 * radians, orbit_pitch + pitch_delta * radians));
    double xz = radius * cos(orbit_pitch);
    double eye[] = {at[0] + xz * sin(orbit_yaw), at[1] + radius * sin(orbit_pitch), at[2] + xz * cos(orbit_yaw)};
    for (unsigned i = 0; i < 3; i++) {
        store_float(rt, camera + OOT_CTL_EYE_NEXT + i * 4, (float)eye[i]);
        store_float(rt, camera + OOT_CTL_EYE + i * 4, (float)eye[i]);
    }
    // Keep the original background line/floor collision and guest ABI. Scratch
    // registers prevent the extra helper from changing the original return value.
    MipsContext scratch = *ctx;
    scratch.gpr[4] = camera;
    scratch.gpr[5] = camera + OOT_CTL_AT;
    scratch.gpr[6] = camera + OOT_CTL_EYE;
    recomp_lookup(rt, OOT_CTL_BG_CHECK)(rt, &scratch);
    double dx = at[0] - load_float(rt, camera + OOT_CTL_EYE);
    double dy = at[1] - load_float(rt, camera + OOT_CTL_EYE + 4);
    double dz = at[2] - load_float(rt, camera + OOT_CTL_EYE + 8);
    recomp_store16(rt, camera + OOT_CTL_INPUT_DIR,
                   (uint16_t)(int32_t)(atan2(dy, hypot(dx, dz)) * (32768 / 3.14159265358979323846)));
    recomp_store16(rt, camera + OOT_CTL_INPUT_DIR + 2,
                   (uint16_t)(int32_t)(atan2(dx, dz) * (32768 / 3.14159265358979323846)));
    recomp_store16(rt, camera + OOT_CTL_INPUT_DIR + 4, 0);
}
