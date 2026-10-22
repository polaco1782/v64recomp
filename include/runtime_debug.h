#ifndef RECOMP_RUNTIME_DEBUG_H
#define RECOMP_RUNTIME_DEBUG_H

#include "recomp.h"
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef RECOMP_DEBUG
typedef struct RecompDebugAudioStats {
    uint64_t tasks, synth_ns, underruns;
    double queue_ms;
    bool available, playback, device_open;
} RecompDebugAudioStats;

typedef struct RecompDebugStats {
    uint64_t ops, interrupt_events, vis, rsp_tasks;
    size_t pending_tasks;
    unsigned threads, runnable, waiting;
    RecompDebugAudioStats audio;
} RecompDebugStats;
/* Snapshot only while owning the serialized guest execution token. */
RecompDebugStats recomp_debug_stats(Runtime *);

/* All logging arguments and counters disappear from release builds. Counters
 * belong to each call site and host thread; guest pthreads execute serially. */
void recomp_debug_log(const char *category, const char *format, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
bool recomp_debug_sample(uint64_t count);
bool recomp_debug_watch_queue(uint32_t object);
void recomp_debug_os(Runtime *, const MipsContext *, const char *, uint64_t);
bool recomp_debug_current_thread(Runtime *, int32_t *id, uint32_t *object);
#define RECOMP_LOG(category, ...) recomp_debug_log(category, __VA_ARGS__)
#define RECOMP_WATCH_QUEUE(object) recomp_debug_watch_queue(object)
#define RECOMP_LOG_SAMPLED(category, ...)                                                                              \
    do {                                                                                                               \
        static _Thread_local uint64_t debug_count;                                                                     \
        if (debug_count != UINT64_MAX)                                                                                 \
            ++debug_count;                                                                                             \
        if (recomp_debug_sample(debug_count)) {                                                                        \
            recomp_debug_log(category, __VA_ARGS__);                                                                   \
        }                                                                                                              \
    } while (0)
#define RECOMP_TRACE_OS(rt, ctx)                                                                                       \
    do {                                                                                                               \
        static _Thread_local uint64_t debug_calls;                                                                     \
        if (debug_calls != UINT64_MAX)                                                                                 \
            ++debug_calls;                                                                                             \
        if (recomp_debug_sample(debug_calls))                                                                          \
            recomp_debug_os(rt, ctx, __func__, debug_calls);                                                           \
    } while (0)
#else
#define RECOMP_LOG(...) ((void)0)
#define RECOMP_WATCH_QUEUE(...) false
#define RECOMP_LOG_SAMPLED(...) ((void)0)
#define RECOMP_TRACE_OS(...) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
