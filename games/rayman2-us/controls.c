#include "controls.h"
#include "rayman2_controls_config.h"
#include "fov_control.h"
#include "game.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define radians (3.14159265358979323846 / 180)
static RecompFov fov;
static RecompMouseBuffer mouse;
static bool enabled;
static double sensitivity, invert_x, invert_y;
static void (*poll_live_input)(Runtime *, void *);

static float from_bits(uint32_t bits) {
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
    return address >= 0x80000000 && !(address & 3) && size <= rt->rdram_size &&
           (uint64_t)address + size <= UINT64_C(0x80000000) + rt->rdram_size;
}
static uint32_t world_camera(Runtime *rt) {
    if (!rt->section_loaded[1])
        return 0;
    uint32_t records = recomp_load32(rt, R2_CTL_CAMERA_RECORDS);
    if (!valid(rt, records, 32) || !recomp_load32(rt, records) || !recomp_load8(rt, records + R2_CTL_RECORD_ACTIVE))
        return 0;
    uint32_t camera = recomp_load32(rt, records + R2_CTL_RECORD_CAMERA);
    return valid(rt, camera, 0xdc) && recomp_load32(rt, camera + R2_CTL_CAMERA_TYPE) == R2_CTL_PERSPECTIVE_TYPE ? camera
                                                                                                                : 0;
}
static bool live_input(Runtime *rt) {
    return world_camera(rt) && recomp_load32(rt, R2_CTL_SCENE_PHASE) == R2_CTL_PLAY_PHASE &&
           !recomp_load32(rt, R2_CTL_INPUT_MODE);
}
static void clear(void) {
    recomp_mouse_clear(&mouse);
}
void rayman2_controls_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    clear();
    const char *setting = getenv("RECOMP_RAYMAN2_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (recomp_game_replaying())
        enabled = false;
#endif
    sensitivity = 0.08;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(end != setting && !*end && isfinite(sensitivity) && sensitivity >= 0.001 &&
                                sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_X");
    invert_x = setting && !strcmp(setting, "1") ? -1 : 1;
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    invert_y = setting && !strcmp(setting, "1") ? -1 : 1;
    double horizontal = from_bits(R2_CTL_NATIVE_HORIZONTAL_BITS);
    double aspect = 1.0 / from_bits(R2_CTL_NATIVE_RATIO_BITS);
    recomp_fov_init(&fov, 2 * atan(tan(horizontal / 2) / aspect) / radians, aspect);
}
void rayman2_fov_adjust(int steps) {
    recomp_fov_adjust(&fov, steps);
}
bool rayman2_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad) {
    recomp_renderer_keyboard_c_buttons(renderer, enabled);
    recomp_renderer_mouse_mode(renderer, enabled, enabled && live_input(rt));
    if (!recomp_renderer_input(renderer, pad)) {
        clear();
        return false;
    }
    RecompMouseFrame frame = recomp_renderer_mouse_frame(renderer);
    if (!frame.active || ((frame.actions ^ mouse.pending.actions) & RECOMP_MOUSE_SECONDARY_ATTACK))
        clear();
    recomp_mouse_submit(&mouse, frame);
    if (frame.active) {
        // The shared mouse defaults target shooters. Select Rayman's actions.
        pad->buttons &= 0x100f;
        if (frame.actions & RECOMP_MOUSE_ATTACK)
            pad->buttons |= 0x4000; // B: shoot/charge.
        if (frame.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_USE))
            pad->buttons |= 0x8000; // A: jump/swim/confirm.
        if (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK)
            pad->buttons |= 0x0008; // C-up: native look mode.
        if (frame.actions & RECOMP_MOUSE_DEFEND)
            pad->buttons |= 0x2000; // Z: strafe.
        if (frame.actions & RECOMP_MOUSE_STATUS)
            pad->buttons |= 0x0004; // C-down: recenter.
        if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON)
            pad->buttons |= 0x0002;
        if (frame.actions & RECOMP_MOUSE_PREVIOUS_WEAPON)
            pad->buttons |= 0x0001;
        int speed = frame.actions & RECOMP_MOUSE_SPEED ? 40 : 80;
        pad->stick_x = (int8_t)(frame.move_x * speed);
        pad->stick_y = (int8_t)(frame.move_y * speed);
    }
    return true;
}
void rayman2_pad_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool port_zero = (int16_t)ctx->gpr[4] == 0;
    if (enabled && port_zero && poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    original(rt, ctx);
    if (!enabled || !port_zero)
        return;
    uint32_t pad = (uint32_t)ctx->gpr[2];
    if (!live_input(rt) || !valid(rt, pad, 6)) {
        clear();
        return;
    }
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        clear();
        return;
    }
    bool look = (frame.actions & RECOMP_MOUSE_SECONDARY_ATTACK) != 0;
    if (look) {
        // C-up uses the original analog look path, including its pitch limits.
        // Each delta is consumed at the guest input update, not presentation rate.
        double x = fmax(-80, fmin(80, frame.dx * sensitivity * 50 * invert_x));
        double y = fmax(-80, fmin(80, -frame.dy * sensitivity * 50 * invert_y));
        recomp_store8(rt, pad + 2, (uint8_t)(int8_t)lround(x));
        recomp_store8(rt, pad + 3, (uint8_t)(int8_t)lround(y));
    } else {
        // Native third-person rotation is digital. Even the smallest mouse
        // motion must reach this update; a sensitivity threshold drops slow
        // motion and a queued hold continues turning after the mouse stops.
        double dx = frame.dx * invert_x;
        uint16_t buttons = recomp_load16(rt, pad);
        if (dx > 0)
            buttons |= 0x0001;
        else if (dx < 0)
            buttons |= 0x0002;
        recomp_store16(rt, pad, buttons);
    }
}
void rayman2_projection_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t camera = (uint32_t)ctx->gpr[6];
    if (!recomp_fov_enabled(&fov) || !camera || camera != world_camera(rt)) {
        original(rt, ctx);
        return;
    }
    uint32_t horizontal = recomp_load32(rt, camera + R2_CTL_HORIZONTAL);
    uint32_t vertical = recomp_load32(rt, camera + R2_CTL_VERTICAL);
    double native = from_bits(horizontal);
    if (!isfinite(native) || native <= 0 || native >= 3.13) {
        original(rt, ctx);
        return;
    }
    double scale = tan(fov.horizontal * radians / 2) / tan(from_bits(R2_CTL_NATIVE_HORIZONTAL_BITS) / 2);
    store_float(rt, camera + R2_CTL_HORIZONTAL, (float)(2 * atan(tan(native / 2) * scale)));
    // One original call rebuilds focal lengths, CPU planes and the GPU matrix.
    original(rt, ctx);
    // Scripts and zoom continue to own the native source angles. Derived
    // projection/visibility values stay widened until the next camera setup.
    recomp_store32(rt, camera + R2_CTL_HORIZONTAL, horizontal);
    recomp_store32(rt, camera + R2_CTL_VERTICAL, vertical);
}
