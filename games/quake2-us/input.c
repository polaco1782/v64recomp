#include "quake2_input.h"
#include "game.h"
#include "generated.h"
#include "quake2_input_config.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static bool enabled;
static uint32_t current_client;
static double sensitivity, vertical;
static RecompMouseBuffer mouse;
static void (*poll_live_input)(Runtime *, void *);

static bool valid_span(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80800000 && !(address & 3) &&
           (address & 0x1fffffff) + size <= rt->rdram_size;
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
bool quake2_input_enabled(void) { return enabled; }
bool quake2_input_playable(Runtime *rt) {
    if (!enabled || recomp_load32(rt, Q2_INPUT_PHASE) != 1 ||
        recomp_load32(rt, Q2_INPUT_MODE) == 2 || recomp_load32(rt, Q2_INPUT_PLAYER_COUNT) != 1 ||
        recomp_load32(rt, Q2_INPUT_INTERMISSION)) return false;
    uint32_t client = recomp_load32(rt, Q2_INPUT_CLIENTS);
    uint32_t entities = recomp_load32(rt, Q2_INPUT_ENTITIES);
    if (!valid_span(rt, client, Q2_INPUT_CLIENT_SIZE) ||
        !valid_span(rt, entities, Q2_INPUT_ENTITY_SIZE * 2)) return false;
    uint32_t entity = entities + Q2_INPUT_ENTITY_SIZE; // Entity zero is the world.
    uint32_t flags = recomp_load32(rt, entity + Q2_INPUT_ENTITY_FLAGS);
    return recomp_load32(rt, entity + Q2_INPUT_ENTITY_CLIENT) == client &&
           (flags & 1) && !(flags & 0x40) &&
           !(recomp_load32(rt, entity + Q2_INPUT_ENTITY_MOVE_FLAGS) & 0x2000);
}
void quake2_input_init(void (*poll)(Runtime *, void *)) {
    poll_live_input = poll;
    const char *setting = getenv("RECOMP_QUAKE2_MOUSE");
    enabled = !setting || strcmp(setting, "0");
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_QUAKE2_INPUT")) enabled = false;
#endif
    sensitivity = 0.08;
    setting = getenv("RECOMP_MOUSE_SENSITIVITY");
    if (setting && *setting) {
        char *end;
        sensitivity = strtod(setting, &end);
        recomp_game_require(!*end && isfinite(sensitivity) && sensitivity >= 0.001 && sensitivity <= 10,
                            "mouse sensitivity must be 0.001..10 degrees per count", 0);
    }
    setting = getenv("RECOMP_MOUSE_INVERT_Y");
    vertical = setting && !strcmp(setting, "1") ? -1 : 1;
    current_client = 0;
    recomp_mouse_clear(&mouse);
}
void quake2_input_submit(RecompMouseFrame frame) { recomp_mouse_submit(&mouse, frame); }

void quake2_input_dispatch_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t previous = current_client;
    current_client = 0;
    if ((uint32_t)ctx->gpr[31] == Q2_INPUT_DISPATCH_RETURN && quake2_input_playable(rt) &&
        (uint32_t)ctx->gpr[4] == recomp_load32(rt, Q2_INPUT_CLIENTS))
        current_client = (uint32_t)ctx->gpr[4];
    else recomp_mouse_clear(&mouse);
    original(rt, ctx);
    // Start/pause can bypass command construction. Never carry motion through it.
    if (!quake2_input_playable(rt)) recomp_mouse_clear(&mouse);
    current_client = previous;
}
void quake2_input_command_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t command = (uint32_t)ctx->gpr[4];
    if (!current_client || command != current_client + Q2_INPUT_COMMAND ||
        (uint32_t)ctx->gpr[31] != Q2_INPUT_COMMAND_RETURN || !quake2_input_playable(rt)) {
        recomp_mouse_clear(&mouse);
        original(rt, ctx);
        return;
    }
    /* Guest commands can run during both drain() and frame pacing. Sampling
     * only at presentation boundaries delivers one lump of motion followed
     * by empty commands when a slower frame spans several guest updates. */
    if (poll_live_input)
        recomp_on_host(rt, poll_live_input, NULL);
    RecompMouseFrame frame = recomp_mouse_consume(&mouse);
    if (!frame.active) { original(rt, ctx); return; }
    float pitch = read_float(rt, command + Q2_INPUT_PITCH);
    float yaw = read_float(rt, command + Q2_INPUT_YAW);
    uint32_t previous = recomp_load32(rt, command + Q2_INPUT_HELD);
    if (!isfinite(pitch) || !isfinite(yaw) || !isfinite(frame.dx) || !isfinite(frame.dy)) {
        recomp_mouse_clear(&mouse);
        original(rt, ctx);
        return;
    }
    // Preserve the original function's ABI/side effects, then replace its command
    // before the caller enters guest movement, collision and weapon processing.
    original(rt, ctx);
    yaw = (float)fmod(yaw - frame.dx * sensitivity, 360.0);
    if (yaw < 0) yaw += 360;
    pitch = (float)fmax(-89, fmin(89, pitch + frame.dy * sensitivity * vertical));
    write_float(rt, command + Q2_INPUT_YAW, yaw);
    write_float(rt, command + Q2_INPUT_PITCH, pitch);
    write_float(rt, command + Q2_INPUT_CENTER_RATE, 0);
    write_float(rt, command + Q2_INPUT_CENTER_TIMER, 0);

    bool speed = (frame.actions & RECOMP_MOUSE_SPEED) != 0;
    bool autorun = recomp_load32(rt, Q2_INPUT_AUTORUN) != 0;
    int32_t magnitude = speed != autorun ? 400 : 200;
    recomp_store32(rt, command + Q2_INPUT_FORWARD, (uint32_t)(frame.move_y * magnitude));
    recomp_store32(rt, command + Q2_INPUT_SIDE, (uint32_t)(frame.move_x * magnitude));
    bool jump = (frame.actions & (RECOMP_MOUSE_JUMP | RECOMP_MOUSE_SECONDARY_ATTACK)) != 0;
    recomp_store32(rt, command + Q2_INPUT_UP, jump ? magnitude : 0);
    uint32_t flags = 0;
    if (frame.actions & RECOMP_MOUSE_ATTACK) flags |= 1;
    if (frame.actions & RECOMP_MOUSE_NEXT_WEAPON) flags |= 4;
    if (frame.actions & (RECOMP_MOUSE_PREVIOUS_WEAPON | RECOMP_MOUSE_USE)) flags |= 8;
    if (frame.actions & RECOMP_MOUSE_STATUS) flags |= 0x10;
    if (speed) flags |= 0x40;
    if (frame.wheel) {
        // One fresh edge per queued detent, even on adjacent guest updates.
        flags = (flags & ~0xcu) | (frame.wheel > 0 ? 8 : 4);
        previous &= ~0xcu;
    }
    // The dispatcher also marks any digital input with bit 31 (0x80025100).
    if (flags || frame.move_x || frame.move_y || jump)
        flags |= UINT32_C(0x80000000);
    recomp_store32(rt, command + Q2_INPUT_HELD, flags);
    recomp_store32(rt, command + Q2_INPUT_PREVIOUS, previous);
    recomp_store32(rt, command + Q2_INPUT_PRESSED, flags & ~previous);
}
