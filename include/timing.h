#ifndef RECOMP_TIMING_H
#define RECOMP_TIMING_H
#include "recomp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional wall-time/counter measurements, shared by all games and both modes.
 * Access only with the guest execution token; rendering workers never write it.
 * Categories can nest: their durations must not be added into a frame total. */
typedef enum RecompTimingPhase {
    RECOMP_TIMING_DISPATCH,
    RECOMP_TIMING_CHECKPOINT,
    RECOMP_TIMING_CONTEXT_SYNC,
    RECOMP_TIMING_GUEST_WAIT,
    RECOMP_TIMING_HOST_CALLBACK,
    RECOMP_TIMING_GRAPHICS,
    RECOMP_TIMING_PRESENT,
    RECOMP_TIMING_AUDIO,
    RECOMP_TIMING_SLEEP,
    RECOMP_TIMING_DISPLAY,
    RECOMP_TIMING_GRAPHICS_CPU,
    RECOMP_TIMING_PHASES
} RecompTimingPhase;

bool recomp_timing_init(Runtime *);
void recomp_timing_destroy(Runtime *);
uint64_t recomp_timing_clock(void);
/* Calling-thread CPU time; zero when unavailable. Never includes GPU/workers. */
uint64_t recomp_timing_thread_clock(void);
void recomp_timing_record(Runtime *, RecompTimingPhase, uint64_t ns, uint64_t count);
void recomp_timing_report(Runtime *, bool final);

static inline uint64_t recomp_timing_begin(Runtime *rt) {
    return rt->timing ? recomp_timing_clock() : 0;
}
static inline void recomp_timing_add(Runtime *rt, RecompTimingPhase phase, uint64_t ns, uint64_t count) {
    if (rt->timing)
        recomp_timing_record(rt, phase, ns, count);
}
static inline void recomp_timing_end(Runtime *rt, RecompTimingPhase phase, uint64_t start) {
    if (rt->timing)
        recomp_timing_record(rt, phase, recomp_timing_clock() - start, 1);
}
static inline void recomp_timing_poll(Runtime *rt) {
    if (rt->timing)
        recomp_timing_report(rt, false);
}

#ifdef __cplusplus
}
#endif
#endif
