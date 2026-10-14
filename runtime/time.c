#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <stdlib.h>
#include <time.h>

typedef struct NativeTimer {
    struct NativeTimer *next;
    uint32_t object, key, queue, message;
    uint64_t deadline, interval, order;
    bool active;
} NativeTimer;

struct RecompClock {
    RecompClockConfig config;
    uint64_t origin_ns, last_ns, offset, order;
    NativeTimer *timers;
};

static uint64_t monotonic_ns(void *unused) {
    (void)unused;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) || now.tv_sec < 0
            || (uint64_t)now.tv_sec > UINT64_MAX / UINT64_C(1000000000))
        recomp_fail("monotonic clock unavailable or exhausted", 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

static RecompClock *clock_state(Runtime *rt) {
    if (!rt->clock) recomp_fail("guest clock not initialized", 0);
    return rt->clock;
}

uint64_t recomp_count64(Runtime *rt) {
    RecompClock *c = clock_state(rt);
    uint64_t now = c->config.now_ns(c->config.userdata);
    if (now < c->last_ns) recomp_fail("guest clock source moved backward", 0);
    c->last_ns = now;
    uint64_t elapsed = now - c->origin_ns, seconds = elapsed / UINT64_C(1000000000);
    uint64_t fraction = (elapsed % UINT64_C(1000000000)) * c->config.ticks_per_second / UINT64_C(1000000000);
    if (seconds > (UINT64_MAX - fraction) / c->config.ticks_per_second)
        recomp_fail("guest counter exhausted", 0);
    return seconds * c->config.ticks_per_second + fraction;
}

uint64_t recomp_get_time(Runtime *rt) { return recomp_count64(rt) + clock_state(rt)->offset; }
void recomp_set_time(Runtime *rt, uint64_t time) { clock_state(rt)->offset = time - recomp_count64(rt); }

static bool before(NativeTimer *a, NativeTimer *b) {
    /* libultra inserts a new timer before older timers with the same deadline. */
    return a->deadline < b->deadline || (a->deadline == b->deadline && a->order > b->order);
}

static NativeTimer *next_timer(RecompClock *c, NativeTimer *after) {
    NativeTimer *best = NULL;
    for (NativeTimer *t = c->timers; t; t = t->next)
        if (t->active && (!after || before(after, t)) && (!best || before(t, best))) best = t;
    return best;
}

static void mirror(Runtime *rt, uint64_t now) {
    RecompClock *c = clock_state(rt);
    uint32_t sentinel = c->config.timer_sentinel;
    NativeTimer *first = next_timer(c, NULL), *last = NULL;
    for (NativeTimer *t = first; t;) {
        NativeTimer *next = next_timer(c, t);
        recomp_store(rt, t->object, next ? next->object : sentinel, 4);
        recomp_store(rt, t->object + 4, last ? last->object : sentinel, 4);
        uint64_t base = last ? last->deadline : now;
        recomp_store(rt, t->object + 16, t->deadline > base ? t->deadline - base : 0, 8);
        last = t; t = next;
    }
    if (sentinel) {
        recomp_store(rt, sentinel, first ? first->object : sentinel, 4);
        recomp_store(rt, sentinel + 4, last ? last->object : sentinel, 4);
    }
}

static void poll_clock(Runtime *rt) {
    RecompClock *c = clock_state(rt);
    uint64_t now = recomp_count64(rt);
    /* Missed periodic firings coalesce to one delivery; rearm from poll time,
     * as an OS service, rather than generating unbounded catch-up messages. */
    for (unsigned i = 0; i < 256; i++) {
        NativeTimer *t = next_timer(c, NULL);
        if (!t || t->deadline > now) break;
        uint32_t queue = t->queue, message = t->message;
        if (t->interval) {
            if (t->interval > UINT64_MAX - now) recomp_fail("timer deadline exhausted", t->object);
            if (c->order == UINT64_MAX) recomp_fail("timer order exhausted", t->object);
            t->deadline = now + t->interval;
            t->order = ++c->order;
        } else {
            t->active = false;
            recomp_store(rt, t->object, 0, 4);
            recomp_store(rt, t->object + 4, 0, 4);
        }
        mirror(rt, now);
        if (queue) (void)recomp_queue_send(rt, queue, message, RECOMP_MESG_NOBLOCK, false);
    }
    mirror(rt, now);
}

bool recomp_clock_init(Runtime *rt, const RecompClockConfig *config) {
    if (rt->clock || !config || !config->ticks_per_second || config->ticks_per_second > UINT64_C(1000000000)
            || config->timer_sentinel % 8) return false;
    if (config->timer_sentinel) recomp_ram_address(rt, config->timer_sentinel, 32);
    RecompClock *c = calloc(1, sizeof(*c));
    if (!c) return false;
    c->config = *config;
    if (!c->config.now_ns) c->config.now_ns = monotonic_ns;
    c->origin_ns = c->last_ns = c->config.now_ns(c->config.userdata);
    rt->clock = c; rt->destroy_clock = recomp_clock_destroy; rt->poll_clock = poll_clock;
    if (config->timer_sentinel) {
        for (unsigned i = 0; i < 32; i++) recomp_store(rt, config->timer_sentinel + i, 0, 1);
        mirror(rt, 0);
    }
    return true;
}

void recomp_clock_destroy(Runtime *rt) {
    RecompClock *c = rt->clock;
    if (!c) return;
    for (NativeTimer *t = c->timers; t;) { NativeTimer *next = t->next; free(t); t = next; }
    free(c); rt->clock = NULL; rt->destroy_clock = NULL; rt->poll_clock = NULL;
}

void recomp_timer_set(Runtime *rt, uint32_t object, uint64_t countdown, uint64_t interval,
                      uint32_t queue, uint32_t message) {
    RecompClock *c = clock_state(rt);
    if (!object || object % 8) recomp_fail("invalid guest timer object", object);
    uint32_t key = recomp_ram_address(rt, object, 32);
    if (c->config.timer_sentinel && key == recomp_ram_address(rt, c->config.timer_sentinel, 32))
        recomp_fail("timer object is the list sentinel", object);
    uint64_t now = recomp_count64(rt), delay = countdown ? countdown : interval;
    if (delay > UINT64_MAX - now) recomp_fail("timer deadline exhausted", object);
    NativeTimer *t;
    for (t = c->timers; t && t->key != key; t = t->next) { }
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t) recomp_fail("timer allocation failed", object);
        t->next = c->timers; c->timers = t; t->object = object; t->key = key;
    }
    if (c->order == UINT64_MAX) recomp_fail("timer order exhausted", object);
    t->active = true; t->deadline = now + delay; t->interval = interval;
    t->queue = queue; t->message = message; t->order = ++c->order;
    recomp_store(rt, object + 8, interval, 8);
    recomp_store(rt, object + 24, queue, 4); recomp_store(rt, object + 28, message, 4);
    mirror(rt, now);
}

bool recomp_timer_stop(Runtime *rt, uint32_t object) {
    RecompClock *c = clock_state(rt);
    uint32_t key = recomp_ram_address(rt, object, 32);
    for (NativeTimer *t = c->timers; t; t = t->next) {
        if (t->key != key || !t->active) continue;
        t->active = false;
        recomp_store(rt, t->object, 0, 4); recomp_store(rt, t->object + 4, 0, 4);
        mirror(rt, recomp_count64(rt));
        return true;
    }
    return false;
}

uint64_t recomp_clock_next_deadline(Runtime *rt) {
    NativeTimer *next = next_timer(clock_state(rt), NULL);
    return next ? next->deadline : UINT64_MAX;
}
uint32_t recomp_clock_timer_sentinel(Runtime *rt) { return clock_state(rt)->config.timer_sentinel; }
uint64_t recomp_clock_frequency(Runtime *rt) { return clock_state(rt)->config.ticks_per_second; }

void hook_os_get_count(Runtime *rt, MipsContext *ctx) { mips_set(ctx, 2, mips_s32((uint32_t)recomp_count64(rt))); }
void hook_os_get_time(Runtime *rt, MipsContext *ctx) {
    uint64_t value = recomp_get_time(rt);
    /* o32 returns a 64-bit value in V0 (high word), V1 (low word). */
    mips_set(ctx, 2, mips_s32((uint32_t)(value >> 32))); mips_set(ctx, 3, mips_s32((uint32_t)value));
}
void hook_os_set_time(Runtime *rt, MipsContext *ctx) {
    recomp_set_time(rt, ((uint64_t)(uint32_t)ctx->gpr[4] << 32) | (uint32_t)ctx->gpr[5]);
}
void hook_os_set_timer(Runtime *rt, MipsContext *ctx) {
    /* A0 timer, A1 padding, A2/A3 countdown, stack+16 interval,
     * stack+24 queue, stack+28 message. All guest words remain big-endian. */
    uint32_t sp = (uint32_t)ctx->gpr[29];
    uint64_t countdown = ((uint64_t)(uint32_t)ctx->gpr[6] << 32) | (uint32_t)ctx->gpr[7];
    recomp_timer_set(rt, (uint32_t)ctx->gpr[4], countdown, recomp_load(rt, sp + 16, 8),
                      (uint32_t)recomp_load(rt, sp + 24, 4), (uint32_t)recomp_load(rt, sp + 28, 4));
    mips_set(ctx, 2, 0);
}
void hook_os_stop_timer(Runtime *rt, MipsContext *ctx) {
    bool stopped=recomp_timer_stop(rt,(uint32_t)ctx->gpr[4]);
    mips_set(ctx,2,stopped?0:mips_s32(UINT32_MAX));
}
