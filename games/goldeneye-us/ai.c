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
enum { ACT_STAND = 1, ACT_DIE = 4, ACT_DEAD = 5, ACT_PATROL = 14, ACT_GOPOS = 15 };
enum { CHR_INVINCIBLE = 0x10, CHR_NEAR_MISS = 4, PROP_CHR = 3, ATTACK_BOND = 6 };
enum { DECISION_TICKS = 6, EVADE_TICKS = 180, MEMORY_TICKS = 600, REPATH_TICKS = 60 };

typedef struct GuardBrain {
    uint32_t chr, model, decision, evade, routed_at, route_stimulus, threat;
    uint16_t number;
    bool decided, evaded, pursuing, scripted;
} GuardBrain;

static GuardBrain guards[512];
static Runtime *owner;
static uint32_t stage, clock_tick;

static bool enabled(Runtime *rt) {
    const char *setting = getenv("RECOMP_GOLDENEYE_AI");
    return (!setting || strcmp(setting, "0")) && goldeneye_input_playable(rt);
}

static bool valid(Runtime *rt, uint32_t address, uint32_t size) {
    return address >= 0x80000000 && address < 0x80400000 && !(address & 3) &&
           (address & 0x1fffffff) + size <= rt->rdram_size;
}

static GuardBrain *brain(Runtime *rt, uint32_t chr) {
    if (!valid(rt, chr, GE_AI_FLAGS2 + 2))
        return NULL;
    uint32_t model = recomp_load32(rt, chr + GE_AI_MODEL);
    uint32_t prop = recomp_load32(rt, chr + GE_AI_PROP);
    if (!valid(rt, model, 4) || !valid(rt, prop, 0x18))
        return NULL;
    uint32_t now = recomp_load32(rt, GE_AI_CLOCK);
    uint32_t level = recomp_load32(rt, GE_AI_STAGE);
    if (owner != rt || stage != level || now < clock_tick) {
        memset(guards, 0, sizeof(guards));
        owner = rt;
        stage = level;
    }
    clock_tick = now;
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

    uint32_t seen = recomp_load32(rt, chr + GE_AI_LAST_SEEN);
    uint32_t heard = recomp_load32(rt, chr + GE_AI_LAST_HEARD);
    uint32_t stimulus = seen > heard ? seen : heard;
    uint32_t tile = recomp_load32(rt, chr + GE_AI_TARGET_TILE);
    if (!stimulus || now - stimulus > MEMORY_TICKS || !valid(rt, tile, 4)) {
        g->pursuing = false;
        return false;
    }
    if (current == ACT_GOPOS && (g->route_stimulus == stimulus || now - g->routed_at < REPATH_TICKS))
        return true; /* Keep the route instead of restarting it every tick. */
    if (g->pursuing && g->route_stimulus == stimulus)
        return false;
    g->pursuing = true;
    g->route_stimulus = stimulus;
    g->routed_at = now;
    return (uint32_t)invoke(rt, ctx, GE_AI_NAVIGATE, chr, chr + GE_AI_LAST_POSITION, tile, 2).gpr[2] != 0;
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
        uint8_t current = recomp_load8(rt, chr + GE_AI_ACTION);
        uint32_t list = recomp_load32(rt, chr + GE_AI_LIST);
        uint32_t seen = recomp_load32(rt, chr + GE_AI_LAST_SEEN);
        uint32_t heard = recomp_load32(rt, chr + GE_AI_LAST_HEARD);
        bool investigating = (heard && clock_tick - heard <= MEMORY_TICKS) || g->pursuing;
        /* These ordinary guard lists yield while moving. Service their native
         * investigation too, so they can reacquire Bond before reaching a route's
         * end. Alarm raisers, deaf guards and mission-owned movement keep the VM. */
        if (investigating && (list == GE_AI_STANDARD_LIST || list == GE_AI_SIMPLE_LIST)) {
            (void)combat(rt, ctx, g);
        }
        bool attack = list == GE_AI_ATTACK_LIST, chase = list == GE_AI_CHASE_LIST;
        bool persistent = list == GE_AI_PERSISTENT_LIST;
        if (current != ACT_DIE && current != ACT_DEAD && (attack || ((chase || persistent) && (seen || heard)))) {
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
}

void goldeneye_ai_noise_hook(Runtime *rt, MipsContext *ctx, RecompFunction original) {
    uint32_t saved = ctx->fpr[12];
    float noise;
    memcpy(&noise, &saved, sizeof(noise));
    if (enabled(rt) && (uint32_t)ctx->gpr[31] == GE_AI_NOISE_RETURN && isfinite(noise) && noise > 0 &&
        (double)noise * 1.75 <= FLT_MAX) {
        /* Scale the real weapon event. Quiet/silenced weapons stay quieter;
         * silence produces no alert and the game records the sound's location. */
        noise *= 1.75f;
        memcpy(&ctx->fpr[12], &noise, sizeof(noise));
    }
    original(rt, ctx);
    ctx->fpr[12] = saved;
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
