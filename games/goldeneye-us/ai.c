#include "game.h"
#include "generated.h"
#include "goldeneye_ai_config.h"
#include "goldeneye_input.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Replace shared combat decisions, while the original VM owns mission scripts.
 * Addresses, script pointers and field operands are checked by generate.py.
 * Guest helpers keep animation, weapons, navigation and collision authoritative.
 */
enum {
    ACT_INIT = 0,
    ACT_STAND = 1,
    ACT_ANIM = 3,
    ACT_DIE = 4,
    ACT_DEAD = 5,
    ACT_ARGH = 6,
    ACT_PREARGH = 7,
    ACT_ATTACK = 8,
    ACT_PATROL = 14,
    ACT_GOPOS = 15
};
enum { CHR_INVINCIBLE = 0x10, CHR_NEAR_MISS = 4, PROP_CHR = 3, ATTACK_BOND = 6 };
enum { DECISION_TICKS = 6, EVADE_TICKS = 180, MEMORY_TICKS = 600, REPATH_TICKS = 60 };
enum { SCENERY_TARGET = 0x8000, SCENERY_BURST_TICKS = 45 };

typedef struct GuardBrain {
    uint32_t chr, model, decision, evade, routed_at, route_stimulus, threat;
    uint32_t noise_serial, scenery, scenery_last, scenery_object, scenery_tick, next_scenery, random;
    uint32_t sound_serial, sound_tick, sound_tile, sound_position[3], routed_sound;
    uint16_t number;
    bool decided, evaded, pursuing, scripted, sound_done, scientist_hostile, scientist_friendly;
} GuardBrain;

typedef struct Noise {
    uint32_t serial, tick, tile, source;
    float position[3], radius;
} Noise;

static GuardBrain guards[512];
static Noise noises[16];
static Runtime *owner;
static uint32_t stage, clock_tick, noise_serial, firing_guard;
static bool positional_noise_scan;

static bool enabled(Runtime *rt) {
    const char *setting = getenv("RECOMP_GOLDENEYE_AI");
    return (!setting || strcmp(setting, "0")) && goldeneye_input_playable(rt);
}

static bool valid(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80400000 && !(address & 3) &&
           (address & 0x1fffffff) + size <= rt->rdram_size;
}

static bool bigger_explosions(void) {
    const char *setting = getenv("RECOMP_GOLDENEYE_EXPLOSIONS");
    return !setting || strcmp(setting, "0");
}

static void configure_explosions(Runtime *rt) {
    /* Accept the checked native or already adjusted value. This also handles
     * stage changes and a fresh runtime without ever multiplying twice. */
    for (size_t i = 0; i < sizeof(ge_explosion_changes) / sizeof(ge_explosion_changes[0]); i++) {
        const uint32_t *change = ge_explosion_changes[i];
        recomp_game_require(valid(rt, change[0], 4), "invalid GoldenEye explosion definition", change[0]);
        uint32_t value = recomp_load32(rt, change[0]);
        recomp_game_require(value == change[1] || value == change[2], "GoldenEye explosion definition changed",
                            change[0]);
    }
    for (size_t i = 0; i < sizeof(ge_explosion_changes) / sizeof(ge_explosion_changes[0]); i++) {
        const uint32_t *change = ge_explosion_changes[i];
        if (recomp_load32(rt, change[0]) != change[2])
            recomp_store32(rt, change[0], change[2]);
    }
}

static void synchronize(Runtime *rt) {
    uint32_t now = recomp_load32(rt, GE_AI_CLOCK), level = recomp_load32(rt, GE_AI_STAGE);
    if (owner != rt || stage != level || now < clock_tick) {
        memset(guards, 0, sizeof(guards));
        memset(noises, 0, sizeof(noises));
        noise_serial = 0;
        owner = rt;
        stage = level;
    }
    clock_tick = now;
}

static float load_float(Runtime *rt, uint32_t address) {
    uint32_t bits = recomp_load32(rt, address);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t random_next(GuardBrain *g) {
    uint32_t value = g->random;
    if (!value)
        value = g->chr ^ g->model ^ ((uint32_t)g->number << 16) ^ UINT32_C(0x9e3779b9);
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    return g->random = value;
}

static bool ordinary_list(uint32_t list) {
    return list == GE_AI_STANDARD_LIST || list == GE_AI_SIMPLE_LIST || list == GE_AI_DEAF_LIST;
}

static bool adapted_list(uint32_t list) {
    return ordinary_list(list) || list == GE_AI_ATTACK_LIST || list == GE_AI_CHASE_LIST ||
           list == GE_AI_PERSISTENT_LIST;
}

static bool noise_at(Runtime *rt, uint32_t position, uint32_t tile, float radius) {
    if (!valid(rt, position, 12) || !valid(rt, tile, 4))
        return false;
    synchronize(rt);
    Noise event = {.tick = clock_tick, .tile = tile, .source = firing_guard, .radius = radius};
    for (unsigned axis = 0; axis < 3; axis++) {
        event.position[axis] = load_float(rt, position + axis * 4);
        if (!isfinite(event.position[axis]))
            return false;
    }
    event.serial = ++noise_serial;
    noises[(event.serial - 1) % (sizeof(noises) / sizeof(noises[0]))] = event;
    return true;
}

static GuardBrain *brain(Runtime *rt, uint32_t chr) {
    if (!valid(rt, chr, GE_AI_WEAPONS + 8))
        return NULL;
    uint32_t model = recomp_load32(rt, chr + GE_AI_MODEL);
    uint32_t prop = recomp_load32(rt, chr + GE_AI_PROP);
    if (!valid(rt, model, 4) || !valid(rt, prop, 0x18))
        return NULL;
    synchronize(rt);
    uint32_t now = clock_tick;
    uint16_t number = recomp_load16(rt, chr + GE_AI_NUMBER);
    GuardBrain *empty = NULL;
    for (size_t i = 0; i < sizeof(guards) / sizeof(guards[0]); i++) {
        GuardBrain *g = &guards[i];
        if (g->chr == chr) {
            if (g->model != model || g->number != number)
                *g = (GuardBrain){.chr = chr, .model = model, .number = number};
            g->scripted |= (recomp_load32(rt, chr + GE_AI_FLAGS) & CHR_INVINCIBLE) != 0;
            if (recomp_load32(rt, chr + GE_AI_FLAGS) & CHR_NEAR_MISS)
                g->threat = now;
            return g;
        }
        if (!g->chr && !empty)
            empty = g;
    }
    /* Optional adaptation must leave the original available if the pool fills. */
    if (empty) {
        *empty = (GuardBrain){.chr = chr, .model = model, .number = number};
        empty->scripted = (recomp_load32(rt, chr + GE_AI_FLAGS) & CHR_INVINCIBLE) != 0;
        if (recomp_load32(rt, chr + GE_AI_FLAGS) & CHR_NEAR_MISS)
            empty->threat = now;
    }
    return empty;
}

static MipsContext invoke(Runtime *rt, const MipsContext *ctx, uint32_t target, uint32_t a0, uint32_t a1, uint32_t a2,
                          uint32_t a3) {
    /* Extra guest calls use the caller's o32 outgoing area and preserve its
     * registers, including the VM's live list/offset and floating-point state. */
    MipsContext call = *ctx;
    call.gpr[4] = mips_s32(a0);
    call.gpr[5] = mips_s32(a1);
    call.gpr[6] = mips_s32(a2);
    call.gpr[7] = mips_s32(a3);
    recomp_lookup(rt, target)(rt, &call);
    return call;
}

static bool action(Runtime *rt, const MipsContext *ctx, uint32_t target, uint32_t chr) {
    return (uint32_t)invoke(rt, ctx, target, chr, 1, 0, 0).gpr[2] != 0;
}

static void scientist_reaction(Runtime *rt, const MipsContext *ctx, GuardBrain *g) {
    if (g->scientist_hostile || g->scientist_friendly)
        return;
    uint8_t body = recomp_load8(rt, g->chr + GE_AI_BODY);
    if (body != GE_AI_SCIENTIST_FEMALE_BODY && body != GE_AI_SCIENTIST_MALE_BODY)
        return;
    uint32_t list = recomp_load32(rt, g->chr + GE_AI_LIST);
    /* Preserve Doak before and after his script assigns character number 79. */
    if (stage == GE_AI_FACILITY_STAGE &&
        (g->number == 79 || list == (uint32_t)invoke(rt, ctx, GE_AI_FIND_LIST, 0x425, 0, 0, 0).gpr[2])) {
        g->scientist_friendly = true;
        return;
    }
    if (list == GE_AI_DRAW_LIST) {
        g->scientist_hostile = true;
        return;
    }
    uint8_t current = recomp_load8(rt, g->chr + GE_AI_ACTION);
    if (current == ACT_INIT || current == ACT_DIE || current == ACT_DEAD || current == ACT_ARGH ||
        current == ACT_PREARGH)
        return;
    MipsContext call = invoke(rt, ctx, GE_AI_DISTANCE, g->chr, 0, 0, 0);
    float distance;
    memcpy(&distance, &call.fpr[0], sizeof(distance));
    if (!isfinite(distance) || distance < 0 || distance > 600 || !action(rt, ctx, GE_AI_CAN_SEE, g->chr))
        return;
    bool armed = (uint32_t)invoke(rt, ctx, GE_AI_ARMED_PROP, g->chr, 0, 0, 0).gpr[2] != 0 ||
                 (uint32_t)invoke(rt, ctx, GE_AI_ARMED_PROP, g->chr, 1, 0, 0).gpr[2] != 0;
    (void)action(rt, ctx, GE_AI_STOP, g->chr);
    g->sound_serial = g->scenery = 0;
    g->decided = g->pursuing = false;
    g->scientist_hostile = true;
    /* The checked shared list owns turning, drawing and giving the TT33.
     * An already armed scientist can enter the normal attack selector directly. */
    recomp_store32(rt, g->chr + GE_AI_LIST, armed ? GE_AI_ATTACK_LIST : GE_AI_DRAW_LIST);
    recomp_store16(rt, g->chr + GE_AI_OFFSET, 0);
    recomp_store16(rt, g->chr + GE_AI_RETURN_LIST, 2);
}

static void sound_destination(Runtime *rt, const GuardBrain *g) {
    /* Keep a private snapshot: native hearing and sight share LAST_POSITION. */
    for (unsigned axis = 0; axis < 3; axis++)
        recomp_store32(rt, g->chr + GE_AI_LAST_POSITION + axis * 4, g->sound_position[axis]);
    recomp_store32(rt, g->chr + GE_AI_TARGET_TILE, g->sound_tile);
}

static bool hear_noise(Runtime *rt, const MipsContext *ctx, GuardBrain *g) {
    float scale = load_float(rt, g->chr + GE_AI_HEARING);
    if (!isfinite(scale) || scale <= 0)
        return false;
    uint32_t prop = recomp_load32(rt, g->chr + GE_AI_PROP);
    const Noise *heard = NULL;
    for (size_t i = 0; i < sizeof(noises) / sizeof(noises[0]); i++) {
        const Noise *n = &noises[i];
        if (!n->serial || n->serial <= g->noise_serial || n->source == g->chr || clock_tick - n->tick > MEMORY_TICKS)
            continue;
        double distance = 0;
        for (unsigned axis = 0; axis < 3; axis++) {
            double delta = load_float(rt, prop + GE_AI_PROP_POSITION + axis * 4) - n->position[axis];
            distance += delta * delta;
        }
        double radius = n->radius * (double)scale;
        if (distance <= radius * radius && (!heard || n->serial > heard->serial))
            heard = n;
    }
    /* A guard must be in range when the event arrives, not walk into an old
     * sound later. A fresh event can interrupt idle activity immediately. */
    g->noise_serial = noise_serial;
    if (!heard)
        return false;
    memcpy(g->sound_position, heard->position, sizeof(g->sound_position));
    g->sound_serial = heard->serial;
    g->sound_tick = heard->tick;
    g->sound_tile = heard->tile;
    g->sound_done = false;
    /* Do not raise the legacy "heard Bond" flag: its scripts run to Bond's
     * current position, rather than to the coordinates of this sound. */
    sound_destination(rt, g);
    uint8_t current = recomp_load8(rt, g->chr + GE_AI_ACTION);
    if (g->scenery || current == ACT_STAND || current == ACT_ANIM || current == ACT_PATROL)
        (void)action(rt, ctx, GE_AI_STOP, g->chr);
    g->scenery = 0;
    g->decided = false;
    g->pursuing = false;
    return true;
}

static bool scenery_valid(Runtime *rt, uint32_t prop, uint32_t object) {
    if (!valid(rt, prop, GE_AI_PROP_PREV + 4) || recomp_load8(rt, prop + GE_AI_PROP_TYPE) != 1 ||
        !(recomp_load8(rt, prop + GE_AI_PROP_FLAGS) & 4) || recomp_load32(rt, prop + GE_AI_PROP_PARENT) ||
        !valid(rt, object, GE_AI_OBJECT_RUNTIME_FLAGS + 4) || recomp_load32(rt, prop + GE_AI_PROP_DATA) != object ||
        recomp_load8(rt, object + GE_AI_OBJECT_TYPE) != 3 || recomp_load8(rt, object + GE_AI_OBJECT_STATE) & 0x80 ||
        recomp_load32(rt, object + GE_AI_OBJECT_PROP) != prop || !recomp_load32(rt, object + GE_AI_OBJECT_MODEL) ||
        recomp_load32(rt, object + GE_AI_OBJECT_RUNTIME_FLAGS) & 0xd4 || /* tagged, removed, embedded, projectile */
        recomp_load32(rt, object + GE_AI_OBJECT_FLAGS) & 0x2002c000 ||   /* protected/assigned/contained/moving */
        recomp_load32(rt, object + GE_AI_OBJECT_FLAGS2) & 0x2018c400)    /* no AI, safe-linked or gunfire-immune */
        return false;
    uint16_t model = recomp_load16(rt, object + GE_AI_OBJECT_MODEL_ID);
    for (size_t i = 0; i < sizeof(ge_ai_scenery_models) / sizeof(ge_ai_scenery_models[0]); i++)
        if (model == ge_ai_scenery_models[i])
            return true;
    return false;
}

static void scenery_shot(Runtime *rt, const MipsContext *ctx, GuardBrain *g) {
    uint32_t now = clock_tick;
    if (!g->next_scenery) {
        g->next_scenery = now + 180 + random_next(g) % 240;
        return;
    }
    if ((int32_t)(now - g->next_scenery) < 0 || !action(rt, ctx, GE_AI_READY, g->chr))
        return;
    g->next_scenery = now + 360 + random_next(g) % 360;
    /* Ambient bursts use ordinary bullet weapons, never rockets or grenades. */
    bool armed = false;
    for (unsigned hand = 0; hand < 2; hand++) {
        uint32_t weapon = recomp_load32(rt, g->chr + GE_AI_WEAPONS + hand * 4);
        if (!weapon)
            continue;
        if (!valid(rt, weapon, GE_AI_PROP_DATA + 4))
            return;
        uint32_t object = recomp_load32(rt, weapon + GE_AI_PROP_DATA);
        if (!valid(rt, object, GE_AI_WEAPON_ITEM + 1))
            return;
        uint8_t item = recomp_load8(rt, object + GE_AI_WEAPON_ITEM);
        if (item < 4 || item > 21)
            return;
        armed = true;
    }
    if (!armed)
        return;
    uint32_t self = recomp_load32(rt, g->chr + GE_AI_PROP);
    uint32_t candidate = 0, candidate_object = 0, count = 0;
    uint32_t prop = (uint32_t)invoke(rt, ctx, GE_AI_ACTIVE_PROPS, 0, 0, 0, 0).gpr[2];
    for (unsigned scanned = 0; prop && scanned < 600; scanned++) {
        if (!valid(rt, prop, GE_AI_PROP_PREV + 4))
            break;
        uint32_t object = recomp_load32(rt, prop + GE_AI_PROP_DATA);
        if (scenery_valid(rt, prop, object)) {
            double distance = 0;
            for (unsigned axis = 0; axis < 3; axis++) {
                double delta = load_float(rt, prop + GE_AI_PROP_POSITION + axis * 4) -
                               load_float(rt, self + GE_AI_PROP_POSITION + axis * 4);
                distance += delta * delta;
            }
            uint32_t tile = recomp_load32(rt, prop + GE_AI_PROP_TILE);
            if (distance >= 40000 && distance <= 1440000 && valid(rt, tile, 4)) {
                bool clear =
                    (uint32_t)invoke(rt, ctx, GE_AI_VISIBLE_POSITION, g->chr, prop + GE_AI_PROP_POSITION, tile, 0)
                        .gpr[2] != 0;
                /* The visibility ray may stop on the chosen box itself. Any
                 * earlier wall, door or other prop rejects it. */
                if ((clear || recomp_load32(rt, GE_AI_LAST_HIT_PROP) == prop) && random_next(g) % ++count == 0) {
                    candidate = prop;
                    candidate_object = object;
                }
            }
        }
        prop = recomp_load32(rt, prop + GE_AI_PROP_PREV);
    }
    if (candidate) {
        g->scenery = candidate;
        g->scenery_last = candidate;
        g->scenery_object = candidate_object;
        g->scenery_tick = now;
        if (!(uint32_t)invoke(rt, ctx, GE_AI_STAND_FIRE, g->chr, SCENERY_TARGET, candidate, 0).gpr[2])
            g->scenery = 0;
    }
}

/* False means the last known position was reached, expired or is unreachable.
 * Decisions use the 60 Hz simulation clock, independently of presentation FPS. */
static bool combat(Runtime *rt, const MipsContext *ctx, GuardBrain *g) {
    uint32_t chr = g->chr, now = clock_tick;
    uint8_t current = recomp_load8(rt, chr + GE_AI_ACTION);
    if (current != ACT_GOPOS && !action(rt, ctx, GE_AI_READY, chr))
        return true; /* Finish firing, injuries and animation transitions. */
    if (g->decided && now - g->decision < DECISION_TICKS)
        return true;
    g->decision = now;
    g->decided = true;

    if (action(rt, ctx, GE_AI_CAN_SEE, chr)) {
        (void)invoke(rt, ctx, GE_AI_REMEMBER, chr, 0, 0, 0);
        g->sound_serial = 0; /* Actual sight can replace an audible clue. */
        g->pursuing = false;
        MipsContext call = invoke(rt, ctx, GE_AI_DISTANCE, chr, 0, 0, 0);
        float distance;
        memcpy(&distance, &call.fpr[0], sizeof(distance));
        recomp_game_require(isfinite(distance) && distance >= 0, "invalid GoldenEye combat distance", chr);
        if (distance <= 3000) {
            if (g->threat && now - g->threat <= 120 && distance < 1600 &&
                (!g->evaded || now - g->evade >= EVADE_TICKS)) {
                g->evade = now;
                g->evaded = true;
                if (action(rt, ctx, GE_AI_STEP, chr))
                    return true;
            }
            if (distance > 1100 && action(rt, ctx, GE_AI_RUN_FIRE, chr))
                return true;
            if (distance > 1100 && action(rt, ctx, GE_AI_WALK_FIRE, chr))
                return true;
            if (distance > 600 && g->number % 3 == 0 && action(rt, ctx, GE_AI_KNEEL_FIRE, chr))
                return true;
            return action(rt, ctx, GE_AI_STAND_FIRE, chr);
        }
    }

    bool sound = g->sound_serial != 0;
    if (sound && g->sound_done)
        return false; /* Wait here for another sound or a visible target. */
    uint32_t seen = recomp_load32(rt, chr + GE_AI_LAST_SEEN);
    uint32_t heard = recomp_load32(rt, chr + GE_AI_LAST_HEARD);
    uint32_t stimulus = sound ? g->sound_tick : (seen > heard ? seen : heard);
    uint32_t tile = sound ? g->sound_tile : recomp_load32(rt, chr + GE_AI_TARGET_TILE);
    if ((!sound && !stimulus) || now - stimulus > MEMORY_TICKS || !valid(rt, tile, 4)) {
        g->pursuing = false;
        return false;
    }
    bool same_route = sound ? g->routed_sound == g->sound_serial : (!g->routed_sound && g->route_stimulus == stimulus);
    if (current == ACT_GOPOS && (same_route || now - g->routed_at < REPATH_TICKS))
        return true; /* Keep the route instead of restarting it every tick. */
    if (g->pursuing && same_route) {
        if (sound) {
            g->sound_done = true;
            g->pursuing = false;
        }
        return false;
    }
    if (sound)
        sound_destination(rt, g);
    g->pursuing = true;
    g->route_stimulus = stimulus;
    g->routed_sound = sound ? g->sound_serial : 0;
    g->routed_at = now;
    bool running = (uint32_t)invoke(rt, ctx, GE_AI_NAVIGATE, chr, chr + GE_AI_LAST_POSITION, tile, 2).gpr[2] != 0;
    if (sound && !running) {
        g->sound_done = true;
        g->pursuing = false;
    }
    return running;
}

static uint32_t return_list(Runtime *rt, const MipsContext *ctx, uint32_t chr) {
    int32_t id = (int16_t)recomp_load16(rt, chr + GE_AI_RETURN_LIST);
    if (id < 0 || id == ATTACK_BOND || id == 8 || id == 13)
        return 0; /* Leave unusual/self-returning scripts with the original VM. */
    return (uint32_t)invoke(rt, ctx, GE_AI_FIND_LIST, (uint32_t)id, 0, 0, 0).gpr[2];
}

void goldeneye_ai_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t chr = (uint32_t)ctx->gpr[4];
    GuardBrain *g = enabled(rt) && (uint32_t)ctx->gpr[5] == PROP_CHR ? brain(rt, chr) : NULL;
    if (g && !g->scripted) {
        scientist_reaction(rt, ctx, g);
        uint32_t list = recomp_load32(rt, chr + GE_AI_LIST);
        if (adapted_list(list))
            (void)hear_noise(rt, ctx, g);
        else {
            g->noise_serial = noise_serial;
            if (g->sound_serial) {
                g->sound_serial = 0;
                g->pursuing = false; /* Hand mission-owned movement back intact. */
            }
        }
        if (g->sound_serial && clock_tick - g->sound_tick > MEMORY_TICKS) {
            if (g->pursuing && g->routed_sound == g->sound_serial && recomp_load8(rt, chr + GE_AI_ACTION) == ACT_GOPOS)
                (void)action(rt, ctx, GE_AI_STOP, chr);
            g->sound_serial = 0;
            g->pursuing = false;
            g->decided = false;
        }
        if (g->scenery && (clock_tick - g->scenery_tick >= SCENERY_BURST_TICKS ||
                           !scenery_valid(rt, g->scenery, g->scenery_object))) {
            (void)action(rt, ctx, GE_AI_STOP, chr);
            g->scenery = 0;
        }
        uint8_t current = recomp_load8(rt, chr + GE_AI_ACTION);
        uint32_t seen = recomp_load32(rt, chr + GE_AI_LAST_SEEN);
        uint32_t heard = recomp_load32(rt, chr + GE_AI_LAST_HEARD);
        bool investigating = g->sound_serial || (heard && clock_tick - heard <= MEMORY_TICKS) || g->pursuing;
        /* Own the whole sound investigation, including waiting on arrival.
         * Running the ordinary VM afterward can replace the route with RunToBond.
         * Mission-owned movement and alarm scripts keep their interpreter. */
        if (current != ACT_DIE && current != ACT_DEAD && investigating && ordinary_list(list)) {
            bool sound = g->sound_serial != 0;
            bool running = combat(rt, ctx, g);
            if (sound || running)
                return;
        }
        bool attack = list == GE_AI_ATTACK_LIST, chase = list == GE_AI_CHASE_LIST;
        bool persistent = list == GE_AI_PERSISTENT_LIST;
        if (current != ACT_DIE && current != ACT_DEAD &&
            (attack || ((chase || persistent) && (seen || heard || g->sound_serial)))) {
            uint32_t resume = persistent ? 0 : return_list(rt, ctx, chr);
            if (persistent || resume) {
                bool running = combat(rt, ctx, g);
                if (persistent || (chase && running))
                    return;
                recomp_store32(rt, chr + GE_AI_LIST, resume);
                recomp_store16(rt, chr + GE_AI_OFFSET, 0);
                if (attack)
                    recomp_store8(rt, chr + GE_AI_FLAGS2, recomp_load8(rt, chr + GE_AI_FLAGS2) | 1);
            }
        }
    }
    original(rt, ctx);
    if (g && !g->scripted && ordinary_list(recomp_load32(rt, chr + GE_AI_LIST))) {
        uint32_t seen = recomp_load32(rt, chr + GE_AI_LAST_SEEN);
        uint32_t heard = recomp_load32(rt, chr + GE_AI_LAST_HEARD);
        if ((!seen || clock_tick - seen > MEMORY_TICKS) && (!heard || clock_tick - heard > MEMORY_TICKS) &&
            !g->pursuing && !g->scenery && !g->sound_serial)
            scenery_shot(rt, ctx, g);
    }
}

void goldeneye_ai_alert_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    /* The original weapon scan calls this helper to copy Bond's current
     * coordinates into every listener. Our event already captured the shot's
     * position. Keep legacy alerts only for actors outside the replacement. */
    if (positional_noise_scan && (uint32_t)ctx->gpr[31] == GE_AI_NOISE_ALERT_RETURN && enabled(rt)) {
        uint32_t chr = (uint32_t)ctx->gpr[4];
        GuardBrain *g = brain(rt, chr);
        if (g && !g->scripted && adapted_list(recomp_load32(rt, chr + GE_AI_LIST)))
            return;
    }
    original(rt, ctx);
}

void goldeneye_ai_noise_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t saved = ctx->fpr[12];
    bool previous_scan = positional_noise_scan;
    positional_noise_scan = false;
    float noise;
    memcpy(&noise, &saved, sizeof(noise));
    if (enabled(rt) && (uint32_t)ctx->gpr[31] == GE_AI_NOISE_RETURN && isfinite(noise) && noise > 0 &&
        (double)noise * 2.5 + 8 <= FLT_MAX / 100) {
        /* The old 1.75x multiplier left a suppressed PP7 audible at only about
         * two metres. Add a nearby-shot radius while retaining relative loudness. */
        noise = 8 + noise * 2.5f;
        memcpy(&ctx->fpr[12], &noise, sizeof(noise));
        uint32_t player = (uint32_t)invoke(rt, ctx, GE_AI_PLAYER_PROP, 0, 0, 0, 0).gpr[2];
        if (valid(rt, player, GE_AI_PROP_TILE + 4))
            positional_noise_scan =
                noise_at(rt, player + GE_AI_PROP_POSITION, recomp_load32(rt, player + GE_AI_PROP_TILE), noise * 100);
    }
    original(rt, ctx);
    positional_noise_scan = previous_scan;
    ctx->fpr[12] = saved;
}

void goldeneye_ai_impact_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t prop = (uint32_t)ctx->gpr[5];
    if (enabled(rt) && valid(rt, prop, GE_AI_PROP_TILE + 4))
        noise_at(rt, prop + GE_AI_PROP_POSITION, recomp_load32(rt, prop + GE_AI_PROP_TILE), 1400);
    original(rt, ctx);
}

void goldeneye_ai_explosion_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    bool larger = bigger_explosions();
    if (larger)
        configure_explosions(rt);
    if (enabled(rt)) {
        uint32_t type = (uint32_t)ctx->gpr[7];
        bool blast = type >= 2 && type <= 20 && type != 15 && type != 16;
        float radius = type == 1 || type == 16 ? 1400 : (larger && blast ? 13500 : 4500);
        noise_at(rt, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], radius);
    }
    original(rt, ctx);
}

void goldeneye_ai_target_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    /* An assigned target must keep resolving during watch/pause transitions,
     * until the firing adapter cancels that burst. */
    if ((uint32_t)ctx->gpr[5] == SCENERY_TARGET) {
        uint32_t chr = (uint32_t)ctx->gpr[4], prop = (uint32_t)ctx->gpr[6];
        uint32_t tile_out = (uint32_t)ctx->gpr[7];
        GuardBrain *g = brain(rt, chr);
        if (g && g->scenery_last == prop && valid(rt, prop, GE_AI_PROP_TILE + 4) && valid(rt, tile_out, 4)) {
            recomp_store32(rt, tile_out, recomp_load32(rt, prop + GE_AI_PROP_TILE));
            ctx->gpr[2] = mips_s32(prop + GE_AI_PROP_POSITION);
            return;
        }
        recomp_fail("GoldenEye scenery target lost its owner", chr);
    }
    original(rt, ctx);
}

void goldeneye_ai_fire_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t previous = firing_guard;
    firing_guard = (uint32_t)ctx->gpr[4];
    GuardBrain *g = brain(rt, firing_guard);
    bool scenery = g && recomp_load8(rt, g->chr + GE_AI_ACTION) == ACT_ATTACK &&
                   recomp_load32(rt, g->chr + GE_AI_ATTACK_TARGET_TYPE) == SCENERY_TARGET;
    if (scenery && (!enabled(rt) || !g->scenery || !scenery_valid(rt, g->scenery, g->scenery_object) ||
                    clock_tick - g->scenery_tick >= SCENERY_BURST_TICKS)) {
        (void)action(rt, ctx, GE_AI_STOP, g->chr);
        g->scenery = 0;
    } else {
        original(rt, ctx);
    }
    firing_guard = previous;
}

void goldeneye_ai_list_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    /* A list can change inside one VM invocation, before any yield. Intercept
     * only SetChrAiList(SELF, ATTACK_BOND), never lookups used to spawn another
     * guard, switch another character, run an aircraft or install a mission list.
     * The checked caller keeps ChrRecord in s7 and resets its offset to zero. */
    if ((uint32_t)ctx->gpr[31] == GE_AI_SET_SELF_RETURN && (uint32_t)ctx->gpr[4] == ATTACK_BOND && enabled(rt)) {
        uint32_t chr = (uint32_t)ctx->gpr[23];
        GuardBrain *g = brain(rt, chr);
        uint32_t resume = g && !g->scripted ? return_list(rt, ctx, chr) : 0;
        if (resume) {
            (void)combat(rt, ctx, g);
            /* Equivalent to the shared attack list's Return, including its
             * awareness flag, without changing the stored return-list ID. */
            recomp_store8(rt, chr + GE_AI_FLAGS2, recomp_load8(rt, chr + GE_AI_FLAGS2) | 1);
            ctx->gpr[2] = mips_s32(resume);
            return;
        }
    }
    original(rt, ctx);
}
