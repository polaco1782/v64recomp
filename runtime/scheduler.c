#define _GNU_SOURCE 1
#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "runtime.h"
#include "runtime_debug.h"
#include "timing.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct NativeQueue NativeQueue;
typedef struct NativeThread {
    struct NativeThread *next;
    Runtime *runtime;
    pthread_t native;
    pthread_cond_t resume;
    bool native_live, running, exiting;
    MipsContext context;
    RecompFunction entry;
    uint32_t object, key;
    int32_t id, priority;
    RecompThreadState state, wait_kind;
    NativeQueue *wait_queue;
    uint64_t order;
    unsigned checkpoints;
    bool interrupts_disabled;
    uint32_t interrupt_mask;
#if RECOMP_LITTLE_ENDIAN_HOST
    /* Serialized context words, excluding the OSThread header. Running threads
     * refresh this cache; parked threads retain their last saved registers. */
    uint32_t saved_context[(0x230 - 0x20) / 4];
    bool context_cached;
#endif
} NativeThread;

struct NativeQueue {
    NativeQueue *next;
    uint32_t object, key;
    bool wait_lists_dirty;
};

enum { PENDING_EVENTS = 256 };
struct RecompScheduler {
    RecompSchedulerConfig config;
    NativeThread *threads, *current;
    /* Reused by loop checkpoints while runnable state is unchanged. Access
     * follows the same execution-token ownership as the thread list. */
    int32_t ready_priority;
    bool ready_priority_valid;
    NativeQueue *queues;
    uint64_t order;
    struct {
        NativeQueue *queue;
        uint32_t message;
    } events[RECOMP_OS_EVENTS];
    atomic_flag pending_lock;
    uint32_t pending[PENDING_EVENTS];
    unsigned pending_first, pending_count;
    bool polling;
    pthread_t owner;
    pthread_mutex_t mutex;
    pthread_cond_t returned;
    void (*host_callback)(Runtime *, void *);
    void *host_userdata;
#ifdef RECOMP_DEBUG
    uint64_t debug_next_status;
    uint64_t debug_interrupt_events;
#endif
};

static _Thread_local NativeThread *guest_thread;

#ifdef RECOMP_DEBUG
RecompDebugStats recomp_debug_stats(Runtime *rt) {
    RecompDebugStats stats = {.ops = rt->debug_ops};
    if (rt->platform)
        stats.vis = recomp_vi_state(rt).retraces;
    if (rt->tasks) {
        RecompTaskStats tasks = recomp_task_stats(rt);
        stats.rsp_tasks = tasks.submissions;
        stats.pending_tasks = tasks.pending_records;
    }
    if (rt->debug_audio_snapshot)
        rt->debug_audio_snapshot(rt->debug_audio_userdata, &stats.audio);
    if (rt->scheduler) {
        stats.interrupt_events = rt->scheduler->debug_interrupt_events;
        for (NativeThread *t = rt->scheduler->threads; t; t = t->next) {
            if (t->state == RECOMP_DEAD)
                continue;
            stats.threads++;
            if (t->state == RECOMP_READY || t->state == RECOMP_RUNNING)
                stats.runnable++;
            if (t->state == RECOMP_WAIT_SEND || t->state == RECOMP_WAIT_RECV)
                stats.waiting++;
        }
    }
    return stats;
}

static const char *debug_state_name(RecompThreadState state) {
    static const char *const names[] = {"stopped", "ready", "running", "wait_send", "wait_recv", "dead"};
    return names[state];
}
bool recomp_debug_current_thread(Runtime *rt, int32_t *id, uint32_t *object) {
    if (!guest_thread || guest_thread->runtime != rt)
        return false;
    *id = guest_thread->id;
    *object = guest_thread->object;
    return true;
}
static void debug_status(Runtime *rt) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        return;
    RecompScheduler *s = rt->scheduler;
    uint64_t seconds = (uint64_t)now.tv_sec;
    if (seconds < s->debug_next_status)
        return;
    s->debug_next_status = seconds + 1;
    unsigned states[RECOMP_DEAD + 1] = {0};
    for (NativeThread *t = s->threads; t; t = t->next)
        states[t->state]++;
    RECOMP_LOG("status", "threads ready=%u running=%u stopped=%u wait_send=%u wait_recv=%u dead=%u",
               states[RECOMP_READY], states[RECOMP_RUNNING], states[RECOMP_STOPPED], states[RECOMP_WAIT_SEND],
               states[RECOMP_WAIT_RECV], states[RECOMP_DEAD]);
    if (rt->platform) {
        RecompViState vi = recomp_vi_state(rt);
        RECOMP_LOG("status", "VI retraces=%" PRIu64 " framebuffer=0x%08" PRIx32 " black=%s", vi.retraces,
                   vi.current_framebuffer, vi.black ? "yes" : "no");
    }
    if (rt->io) {
        RecompIOStats io = recomp_io_stats(rt);
        RECOMP_LOG("status",
                   "input reads=%" PRIu64 " EEPROM reads=%" PRIu64 " writes=%" PRIu64 " save_failures=%" PRIu64,
                   io.reads_completed, io.eeprom_reads, io.eeprom_writes, io.persistence_failures);
    }
    if (rt->tasks) {
        RecompTaskStats tasks = recomp_task_stats(rt);
        RECOMP_LOG("status", "RSP tasks=%" PRIu64 " pending=%zu SP events=%" PRIu64 " DP events=%" PRIu64,
                   tasks.submissions, tasks.pending_records, tasks.sp_events, tasks.dp_events);
    }
}
#endif

static RecompScheduler *scheduler(Runtime *rt) {
    if (!rt->scheduler)
        recomp_fail("scheduler not initialized", 0);
    return rt->scheduler;
}
void recomp_on_host(Runtime *rt, void (*callback)(Runtime *, void *), void *userdata) {
    RecompScheduler *s = rt->scheduler;
    if (!s || pthread_equal(pthread_self(), s->owner)) {
        uint64_t timing = recomp_timing_begin(rt);
        callback(rt, userdata);
        recomp_timing_end(rt, RECOMP_TIMING_HOST_CALLBACK, timing);
        return;
    }
    if (!guest_thread || guest_thread != s->current)
        recomp_fail("host callback outside guest/owner thread", 0);
    /* The execution token owns mutex throughout translated code. The host
     * acquires it only while this thread is parked on its condition variable. */
    s->host_callback = callback;
    s->host_userdata = userdata;
    pthread_cond_signal(&s->returned);
    while (s->host_callback)
        pthread_cond_wait(&guest_thread->resume, &s->mutex);
}

static void report_guest_failure(Runtime *rt, void *opaque) {
    (void)rt;
    const RecompFailureInfo *info = opaque;
    if (info->warning)
        recomp_dispatch_warning(info);
    else
        recomp_dispatch_failure(info);
}
static void guest_failure(const RecompFailureInfo *info, void *opaque) {
    recomp_on_host(opaque, report_guest_failure, (void *)info);
}

static void join_thread(RecompScheduler *s, NativeThread *t) {
    if (!t->native_live)
        return;
    t->exiting = true;
    pthread_cond_signal(&t->resume);
    pthread_mutex_unlock(&s->mutex);
    if (pthread_join(t->native, NULL))
        recomp_fail("pthread join failed", t->object);
    pthread_mutex_lock(&s->mutex);
    t->native_live = false;
}

size_t recomp_thread_snapshots(Runtime *rt, RecompThreadSnapshot *out, size_t capacity) {
    if (!rt->scheduler || !out)
        return 0;
    size_t count = 0;
    for (NativeThread *t = rt->scheduler->threads; t && count < capacity; t = t->next)
        out[count++] = (RecompThreadSnapshot){.object = t->object,
                                              .id = t->id,
                                              .priority = t->priority,
                                              .state = t->state,
                                              .current = t == rt->scheduler->current,
                                              .context = t->context};
    return count;
}

static uint64_t ticket(RecompScheduler *s) {
    if (s->order == UINT64_MAX)
        recomp_fail("scheduler order exhausted", 0);
    return ++s->order;
}

static NativeThread *thread(Runtime *rt, uint32_t object) {
    RecompScheduler *s = scheduler(rt);
    if (!object) {
        if (!s->current)
            recomp_fail("no current guest thread", 0);
        return s->current;
    }
    uint32_t key = recomp_ram_address(rt, object, s->config.fpu_fr1 ? 0x230 : RECOMP_THREAD_BYTES);
    for (NativeThread *t = s->threads; t; t = t->next)
        if (t->key == key)
            return t;
    recomp_fail("unknown guest thread", object);
}

static NativeQueue *queue(Runtime *rt, uint32_t object) {
    uint32_t key = recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
    for (NativeQueue *q = scheduler(rt)->queues; q; q = q->next)
        if (q->key == key)
            return q;
    recomp_fail("unknown message queue", object);
}

static bool before(const NativeThread *a, const NativeThread *b) {
    return a->priority > b->priority || (a->priority == b->priority && a->order < b->order);
}

static NativeThread *best(RecompScheduler *s, RecompThreadState state, NativeQueue *q, NativeThread *after) {
    NativeThread *result = NULL;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state != state || (q && t->wait_queue != q) || (after && !before(after, t)))
            continue;
        if (!result || before(t, result))
            result = t;
    }
    return result;
}

static bool host_idle(RecompScheduler *s, const NativeThread *t) {
    return t && s->config.idle_thread && t->object == s->config.idle_thread && t->priority == 0;
}

static void save_context_checked(Runtime *rt, NativeThread *t) {
    static const unsigned registers[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                         16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31};
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
        recomp_store(rt, t->object + 0x20 + i * 8, t->context.gpr[registers[i]], 8);
    recomp_store(rt, t->object + 0x108, t->context.lo, 8);
    recomp_store(rt, t->object + 0x110, t->context.hi, 8);
    recomp_store(rt, t->object + 0x11c, t->context.pc, 4);
    recomp_store(rt, t->object + 0x12c, t->context.fcr31, 4);
    if (t->context.fpu_fr1) {
        for (unsigned i = 0; i < 32; i++)
            recomp_store(rt, t->object + 0x130 + i * 8, mips_fpr_pair(&t->context, i), 8);
    } else {
        for (unsigned i = 0; i < 32; i++)
            recomp_store(rt, t->object + 0x130 + (i ^ 1) * 4, t->context.fpr[i], 4);
    }
}

#if RECOMP_LITTLE_ENDIAN_HOST
static void context_word_pair(uint32_t *words, uint64_t value) {
    /* Guest RAM stores each big-endian 64-bit register as two native words. */
    words[0] = (uint32_t)(value >> 32);
    words[1] = (uint32_t)value;
}

static void cache_context(NativeThread *t) {
    static const unsigned registers[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                         16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 28, 29, 30, 31};
    for (unsigned i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
        context_word_pair(t->saved_context + i * 2, t->context.gpr[registers[i]]);
    context_word_pair(t->saved_context + (0x108 - 0x20) / 4, t->context.lo);
    context_word_pair(t->saved_context + (0x110 - 0x20) / 4, t->context.hi);
    t->saved_context[(0x11c - 0x20) / 4] = t->context.pc;
    t->saved_context[(0x12c - 0x20) / 4] = t->context.fcr31;
    if (t->context.fpu_fr1) {
        for (unsigned i = 0; i < 32; i++)
            context_word_pair(t->saved_context + (0x130 - 0x20) / 4 + i * 2, mips_fpr_pair(&t->context, i));
    } else {
        for (unsigned i = 0; i < 32; i++)
            t->saved_context[(0x130 - 0x20) / 4 + (i ^ 1)] = t->context.fpr[i];
    }
    t->context_cached = true;
}

static void sync_context_range(uint8_t *ram, const uint8_t *saved, size_t bytes) {
    /* Guest/native code can modify a parked thread's saved context. Compare
     * live RAM rather than trusting the cache as evidence that RAM is current. */
    if (memcmp(ram, saved, bytes))
        memcpy(ram, saved, bytes);
}
#endif

static void save_context(Runtime *rt, NativeThread *t) {
#if RECOMP_LITTLE_ENDIAN_HOST
    size_t bytes = t->context.fpu_fr1 ? 0x230 : RECOMP_THREAD_BYTES;
    uint32_t physical = t->object & UINT32_C(0x1fffffff);
    if (recomp_direct_address(t->object) && physical < rt->rdram_size && bytes <= rt->rdram_size - physical) {
        if (!t->context_cached || t->running)
            cache_context(t);
        /* Validate the complete direct-RAM span in Debug and Release. Mapped
         * objects keep per-field resolution and the original write ordering. */
        physical = recomp_ram_address_write_slow(rt, t->object + 0x20, bytes - 0x20);
        uint8_t *ram = rt->rdram + physical;
        const uint8_t *saved = (const uint8_t *)t->saved_context;
        sync_context_range(ram, saved, 0x118 - 0x20);
        sync_context_range(ram + 0x11c - 0x20, saved + 0x11c - 0x20, 4);
        sync_context_range(ram + 0x12c - 0x20, saved + 0x12c - 0x20, bytes - 0x12c);
        return;
    }
#endif
    save_context_checked(rt, t);
}

static void sync_list(Runtime *rt, RecompThreadState state, NativeQueue *q, uint32_t head) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *first = best(s, state, q, NULL);
    if (head)
        recomp_store(rt, head, first ? first->object : s->config.thread_tail, 4);
    for (NativeThread *t = first; t;) {
        NativeThread *next = best(s, state, q, t);
        recomp_store(rt, t->object, next ? next->object : s->config.thread_tail, 4);
        recomp_store(rt, t->object + 8, head, 4);
        t = next;
    }
}

static void sync_guest(Runtime *rt) {
    uint64_t timing = recomp_timing_begin(rt);
    RecompScheduler *s = scheduler(rt);
    NativeThread *active = NULL, *previous = NULL;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state == RECOMP_DEAD)
            continue;
        unsigned state = t->state == RECOMP_RUNNING                                     ? RECOMP_OS_RUNNING
                         : t->state == RECOMP_READY                                     ? RECOMP_OS_RUNNABLE
                         : t->state == RECOMP_WAIT_SEND || t->state == RECOMP_WAIT_RECV ? RECOMP_OS_WAITING
                                                                                        : RECOMP_OS_STOPPED;
        recomp_store(rt, t->object + 4, (uint32_t)t->priority, 4);
        recomp_store(rt, t->object + 0x10, state, 2);
        /* Waiting-thread next links belong to their queue. Preserve them
         * unless that queue's waiter list is being updated below. */
        if (t->state != RECOMP_WAIT_SEND && t->state != RECOMP_WAIT_RECV)
            recomp_store(rt, t->object, 0, 4);
        uint32_t queue_head = t->wait_queue ? t->wait_queue->object + (t->wait_kind == RECOMP_WAIT_SEND ? 4 : 0)
                              : t->state == RECOMP_RUNNING ? s->config.run_queue
                                                           : 0;
        recomp_store(rt, t->object + 8, queue_head, 4);
        recomp_store(rt, t->object + 0x0c, 0, 4);
        save_context(rt, t);
        if (!active)
            active = t;
        if (previous)
            recomp_store(rt, previous->object + 0x0c, t->object, 4);
        previous = t;
    }
    if (s->config.active_queue)
        recomp_store(rt, s->config.active_queue, active ? active->object : 0, 4);
    if (s->config.running_thread)
        recomp_store(rt, s->config.running_thread,
                     s->current && s->current->state == RECOMP_RUNNING ? s->current->object : 0, 4);
    sync_list(rt, RECOMP_READY, NULL, s->config.run_queue);
    for (NativeQueue *q = s->queues; q; q = q->next) {
        /* Overlay storage can be reused after a queue is no longer needed.
         * A cached empty queue must not rewrite memory now owned by assets. */
        if (!q->wait_lists_dirty)
            continue;
        sync_list(rt, RECOMP_WAIT_RECV, q, q->object);
        sync_list(rt, RECOMP_WAIT_SEND, q, q->object + 4);
        q->wait_lists_dirty = false;
    }
    recomp_timing_end(rt, RECOMP_TIMING_CONTEXT_SYNC, timing);
}

static void ready(RecompScheduler *s, NativeThread *t) {
    RECOMP_LOG_SAMPLED("thread", "ready id=%" PRId32 " thread=0x%08" PRIx32 " priority=%" PRId32, t->id, t->object,
                       t->priority);
    if (t->wait_queue)
        t->wait_queue->wait_lists_dirty = true;
    s->ready_priority_valid = false;
    t->state = RECOMP_READY;
    t->wait_queue = NULL;
    t->order = ticket(s);
}

static void suspend(Runtime *rt, RecompThreadState state) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, 0);
    RECOMP_LOG_SAMPLED(
        "thread", "suspend id=%" PRId32 " thread=0x%08" PRIx32 " state=%s queue=0x%08" PRIx32 " pc=0x%08" PRIx32, t->id,
        t->object, debug_state_name(state), t->wait_queue ? t->wait_queue->object : 0, t->context.pc);
    if (t->wait_queue)
        t->wait_queue->wait_lists_dirty = true;
    s->ready_priority_valid = false;
    t->state = state;
    t->order = ticket(s);
    sync_guest(rt);
    t->running = false;
    pthread_cond_signal(&s->returned);
    while (!t->running && !t->exiting)
        pthread_cond_wait(&t->resume, &s->mutex);
    if (t->exiting) {
        pthread_mutex_unlock(&s->mutex);
        pthread_exit(NULL);
    }
}

static bool preemption_due(RecompScheduler *s) {
    if (!guest_thread || s->polling || !s->current)
        return false;
    if (!s->ready_priority_valid) {
        NativeThread *candidate = best(s, RECOMP_READY, NULL, NULL);
        s->ready_priority = candidate ? candidate->priority : -1;
        s->ready_priority_valid = true;
    }
    return s->ready_priority > s->current->priority;
}

static void preempt(Runtime *rt) {
    if (preemption_due(scheduler(rt)))
        suspend(rt, RECOMP_READY);
}

static void wake(Runtime *rt, NativeQueue *q, RecompThreadState state) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = best(s, state, q, NULL);
    if (t)
        ready(s, t);
}

static void retire(NativeThread *t) {
    RECOMP_LOG("thread", "retired id=%" PRId32 " thread=0x%08" PRIx32, t->id, t->object);
    scheduler(t->runtime)->ready_priority_valid = false;
    t->state = RECOMP_DEAD;
    if (t->wait_queue)
        t->wait_queue->wait_lists_dirty = true;
    t->wait_queue = NULL;
    save_context(t->runtime, t);
    recomp_store(t->runtime, t->object, 0, 4);
    recomp_store(t->runtime, t->object + 8, 0, 4);
    recomp_store(t->runtime, t->object + 0x0c, 0, 4);
    recomp_store(t->runtime, t->object + 0x10, RECOMP_OS_STOPPED, 2);
}

static void *enter(void *opaque) {
    NativeThread *t = opaque;
    RecompScheduler *s = scheduler(t->runtime);
    guest_thread = t;
    recomp_set_failure_handler(guest_failure, t->runtime);
    char name[16];
    snprintf(name, sizeof(name), "n64-%d", t->id);
    for (size_t i = 0; i < s->config.thread_name_count; i++) {
        if (s->config.thread_names[i].entry == t->context.pc) {
            snprintf(name, sizeof(name), "%s", s->config.thread_names[i].name);
            break;
        }
    }
#ifdef _WIN32
    wchar_t wide_name[16] = {0};
    for (size_t i = 0; i < strlen(name); i++)
        wide_name[i] = (unsigned char)name[i];
    SetThreadDescription(GetCurrentThread(), wide_name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
    pthread_mutex_lock(&s->mutex);
    RECOMP_LOG("thread", "entered pthread id=%" PRId32 " entry=0x%08" PRIx32, t->id, t->context.pc);
    while (!t->running && !t->exiting)
        pthread_cond_wait(&t->resume, &s->mutex);
    if (!t->exiting) {
        const RecompSchedulerConfig *config = &s->config;
        bool stop = config->stop_before_entry == t->context.pc;
        for (size_t i = 0; i < config->stop_before_count; i++)
            if (config->stop_before_entries[i] == t->context.pc)
                stop = true;
        if (stop)
            suspend(t->runtime, RECOMP_STOPPED);
        t->entry(t->runtime, &t->context);
        retire(t);
    }
    t->running = false;
    pthread_cond_signal(&s->returned);
    pthread_mutex_unlock(&s->mutex);
    return NULL;
}

static void lock_pending(RecompScheduler *s) {
    while (atomic_flag_test_and_set_explicit(&s->pending_lock, memory_order_acquire)) {
    }
}

static void deliver_events(Runtime *rt) {
    RecompScheduler *s = scheduler(rt);
    /* A bounded drain lets the guest run even if workers keep posting. */
    for (unsigned i = 0; i < PENDING_EVENTS; i++) {
        lock_pending(s);
        bool present = s->pending_count != 0;
        uint32_t event = 0;
        if (present) {
            event = s->pending[s->pending_first];
            s->pending_first = (s->pending_first + 1) % PENDING_EVENTS;
            s->pending_count--;
        }
        atomic_flag_clear_explicit(&s->pending_lock, memory_order_release);
        if (!present)
            break;
#ifdef RECOMP_DEBUG
        s->debug_interrupt_events++;
#endif
        if (s->events[event].queue)
            (void)recomp_queue_send(rt, s->events[event].queue->object, s->events[event].message, RECOMP_MESG_NOBLOCK,
                                    false);
    }
}

static void poll_on_host(Runtime *rt, void *unused) {
    (void)unused;
    RecompScheduler *s = scheduler(rt);
    if (s->polling)
        return;
    /* A timer/VI/DMA service finishes updating its records before guest code
     * can run. Otherwise queue-send preemption could reenter that service. */
    s->polling = true;
    if (!rt->platform || rt->platform_initialized) {
        /* Consume available backend receipts before generating elapsed timer
         * and VI notifications. A completed SP task must not be queued behind
         * a later VI: games can wait for its flag inside the VI handler with
         * interrupts disabled, preventing them from receiving the SP message. */
        if (rt->poll_tasks)
            rt->poll_tasks(rt);
        /* VI delivery writes its queue directly; finish the SP/DP event FIFO
         * first rather than leaving those receipts until the final drain. */
        deliver_events(rt);
        if (rt->poll_clock)
            rt->poll_clock(rt);
        if (rt->poll_platform)
            rt->poll_platform(rt);
        if (rt->poll_io)
            rt->poll_io(rt);
    }
    deliver_events(rt);
    s->polling = false;
}

static void poll(Runtime *rt) {
    recomp_on_host(rt, poll_on_host, NULL);
}

static bool checkpoint_due(Runtime *rt, bool force_yield) {
    RecompScheduler *s = scheduler(rt);
    if (!guest_thread)
        return true;
    /* The host polls between dispatches. Avoid a kernel rendezvous at every
     * backedge; the configured interval bounds service/preemption latency. */
    unsigned interval = s->config.checkpoint_interval ? s->config.checkpoint_interval : 64;
    if (force_yield || ++s->current->checkpoints >= interval) {
        s->current->checkpoints = 0;
        return true;
    }
    return preemption_due(s);
}

static void checkpoint(Runtime *rt) {
    /* The decision and this action execute under the same guest execution
     * token. Generated code flushes its registers between the two calls. */
    recomp_timing_add(rt, RECOMP_TIMING_CHECKPOINT, 0, 1);
    if (!guest_thread)
        poll(rt);
    else
        suspend(rt, RECOMP_READY);
}

bool recomp_scheduler_init(Runtime *rt, const RecompSchedulerConfig *config) {
    if (rt->scheduler || !config || !config->thread_tail || config->thread_tail % 4)
        return false;
    if (config->thread_name_count && !config->thread_names)
        return false;
    for (size_t i = 0; i < config->thread_name_count; i++) {
        const RecompThreadName *name = &config->thread_names[i];
        if (!name->entry || name->entry % 4 || !name->name || !name->name[0] || strlen(name->name) >= 16)
            return false;
        for (size_t j = 0; j < i; j++)
            if (config->thread_names[j].entry == name->entry)
                return false;
    }
    if (config->stop_before_count && !config->stop_before_entries)
        return false;
    recomp_ram_address(rt, config->thread_tail, 8);
    if (config->idle_thread) {
        if (config->idle_thread % 4)
            return false;
        recomp_ram_address(rt, config->idle_thread, config->fpu_fr1 ? 0x230 : RECOMP_THREAD_BYTES);
    }
    uint32_t globals[] = {config->run_queue, config->active_queue, config->running_thread, config->event_table};
    for (unsigned i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
        if (globals[i] % 4)
            return false;
        if (globals[i])
            recomp_ram_address(rt, globals[i], i == 3 ? RECOMP_OS_EVENTS * 8 : 4);
    }
    RecompScheduler *s = calloc(1, sizeof(*s));
    if (!s)
        return false;
    s->owner = pthread_self();
    if (pthread_mutex_init(&s->mutex, NULL)) {
        free(s);
        return false;
    }
    if (pthread_cond_init(&s->returned, NULL)) {
        pthread_mutex_destroy(&s->mutex);
        free(s);
        return false;
    }
    s->config = *config;
    s->pending_lock = (atomic_flag)ATOMIC_FLAG_INIT;
    rt->scheduler = s;
    rt->destroy_scheduler = recomp_scheduler_destroy;
    rt->checkpoint_due = checkpoint_due;
    rt->checkpoint = checkpoint;
    recomp_store(rt, config->thread_tail, 0, 4);
    recomp_store(rt, config->thread_tail + 4, UINT32_MAX, 4);
    if (config->event_table)
        for (unsigned i = 0; i < RECOMP_OS_EVENTS * 8; i++)
            recomp_store(rt, config->event_table + i, 0, 1);
    sync_guest(rt);
    return true;
}

void recomp_scheduler_destroy(Runtime *rt) {
    RecompScheduler *s = rt->scheduler;
    if (!s)
        return;
    if (s->current)
        recomp_fail("cannot destroy scheduler inside a guest thread", s->current->object);
    pthread_mutex_lock(&s->mutex);
    for (NativeThread *t = s->threads; t;) {
        NativeThread *next = t->next;
        join_thread(s, t);
        pthread_cond_destroy(&t->resume);
        free(t);
        t = next;
    }
    for (NativeQueue *q = s->queues; q;) {
        NativeQueue *next = q->next;
        free(q);
        q = next;
    }
    pthread_mutex_unlock(&s->mutex);
    pthread_cond_destroy(&s->returned);
    pthread_mutex_destroy(&s->mutex);
    free(s);
    rt->scheduler = NULL;
    rt->destroy_scheduler = NULL;
    rt->checkpoint_due = NULL;
    rt->checkpoint = NULL;
}

RecompRunResult recomp_scheduler_run(Runtime *rt, size_t max_resumes) {
    RecompScheduler *s = scheduler(rt);
    if (!pthread_equal(pthread_self(), s->owner) || s->current || !max_resumes)
        recomp_fail("invalid or nested scheduler run", 0);
    pthread_mutex_lock(&s->mutex);
    bool owner_interrupts_disabled = rt->interrupts_disabled;
    uint32_t owner_interrupt_mask = rt->interrupt_mask;
#ifdef RECOMP_DEBUG
    debug_status(rt);
#endif
    for (size_t count = 0; count < max_resumes; count++) {
        poll(rt);
        NativeThread *t = best(s, RECOMP_READY, NULL, NULL);
        /* Poll first so timers and backend receipts can wake real work even
         * when the only runnable guest was the idle thread on entry. */
        if (!t || host_idle(s, t))
            break;
        s->current = t;
        s->ready_priority_valid = false;
        t->state = RECOMP_RUNNING;
        RECOMP_LOG_SAMPLED("thread",
                           "dispatch id=%" PRId32 " thread=0x%08" PRIx32 " priority=%" PRId32 " pc=0x%08" PRIx32, t->id,
                           t->object, t->priority, t->context.pc);
        sync_guest(rt);
        /* __osDispatch restores each thread's CPU IE and interrupt masks.
         * A thread may block in a critical section; its disabled state must
         * not suppress checkpoints and device service in the next thread. */
        rt->interrupts_disabled = t->interrupts_disabled;
        rt->interrupt_mask = t->interrupt_mask;
        t->running = true;
        recomp_timing_add(rt, RECOMP_TIMING_DISPATCH, 0, 1);
        pthread_cond_signal(&t->resume);
        while (t->running) {
            uint64_t timing = recomp_timing_begin(rt);
            pthread_cond_wait(&s->returned, &s->mutex);
            recomp_timing_end(rt, RECOMP_TIMING_GUEST_WAIT, timing);
            if (s->host_callback) {
                timing = recomp_timing_begin(rt);
                s->host_callback(rt, s->host_userdata);
                recomp_timing_end(rt, RECOMP_TIMING_HOST_CALLBACK, timing);
                s->host_callback = NULL;
                pthread_cond_signal(&t->resume);
            }
        }
        t->interrupts_disabled = rt->interrupts_disabled;
        t->interrupt_mask = rt->interrupt_mask;
        rt->interrupts_disabled = owner_interrupts_disabled;
        rt->interrupt_mask = owner_interrupt_mask;
        s->current = NULL;
        if (t->state == RECOMP_RUNNING)
            recomp_fail("pthread returned without a state transition", t->object);
        if (t->state == RECOMP_DEAD)
            join_thread(s, t);
        sync_guest(rt);
    }
    NativeThread *next = best(s, RECOMP_READY, NULL, NULL);
    recomp_timing_poll(rt);
    pthread_mutex_unlock(&s->mutex);
    if (next)
        return host_idle(s, next) ? RECOMP_RUN_WAITING : RECOMP_RUN_BUDGET;
    bool stopped = false;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state == RECOMP_WAIT_SEND || t->state == RECOMP_WAIT_RECV)
            return RECOMP_RUN_WAITING;
        if (t->state == RECOMP_STOPPED)
            stopped = true;
    }
    return stopped ? RECOMP_RUN_STOPPED : RECOMP_RUN_COMPLETE;
}

void recomp_thread_create(Runtime *rt, uint32_t object, int32_t id, uint32_t entry, uint32_t argument, uint32_t stack,
                          int32_t priority) {
    RecompScheduler *s = scheduler(rt);
    if (!object || object % 8 || stack % 8 || priority < 0 || priority > 255)
        recomp_fail("invalid thread object/stack/priority", object);
    unsigned bytes = s->config.fpu_fr1 ? 0x230 : RECOMP_THREAD_BYTES;
    uint32_t key = recomp_ram_address(rt, object, bytes);
    recomp_ram_address(rt, stack - 16, 16);
    NativeThread **link = &s->threads;
    while (*link) {
        NativeThread *t = *link;
        if (t->key == key) {
            /* Some games initialize a new entry in an existing stopped
             * loader thread. Discard its suspended native call stack before
             * resetting the guest context. Runnable/waiting threads still
             * own their context and cannot be overwritten. */
            if (t->state != RECOMP_DEAD && (t->state != RECOMP_STOPPED || t->wait_queue || t == s->current))
                recomp_fail("thread object is already active", object);
            if (t->state != RECOMP_DEAD)
                retire(t);
            *link = t->next;
            if (!guest_thread)
                pthread_mutex_lock(&s->mutex);
            join_thread(s, t);
            if (!guest_thread)
                pthread_mutex_unlock(&s->mutex);
            pthread_cond_destroy(&t->resume);
            free(t);
            break;
        }
        link = &t->next;
    }
    NativeThread *t = calloc(1, sizeof(*t));
    if (!t)
        recomp_fail("thread allocation failed", object);
    t->runtime = rt;
    t->object = object;
    t->key = key;
    t->id = id;
    t->priority = priority;
    /* SDK osCreateThread starts with SR=0xff03 and RCP mask=0x3f. */
    t->interrupt_mask = UINT32_C(0x003fff01);
    t->state = RECOMP_STOPPED;
    t->entry = recomp_lookup(rt, entry);
    t->context.pc = entry;
    t->context.gpr[4] = mips_s32(argument);
    t->context.gpr[29] = mips_s32(stack - 16);
    t->context.gpr[31] = mips_s32(UINT32_C(0xf0000000));
    t->context.fcr31 = UINT32_C(0x01000800);
    t->context.fpu_fr1 = s->config.fpu_fr1;
    if (pthread_cond_init(&t->resume, NULL)) {
        free(t);
        recomp_fail("pthread condition allocation failed", object);
    }
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes))
        recomp_fail("pthread attribute allocation failed", object);
    if (s->config.native_stack_size && pthread_attr_setstacksize(&attributes, s->config.native_stack_size))
        recomp_fail("invalid pthread stack size", object);
    int created = pthread_create(&t->native, &attributes, enter, t);
    pthread_attr_destroy(&attributes);
    if (created) {
        pthread_cond_destroy(&t->resume);
        free(t);
        recomp_fail("pthread allocation failed", object);
    }
    t->native_live = true;
    t->next = s->threads;
    s->threads = t;
    for (unsigned i = 0; i < bytes; i++)
        recomp_store(rt, object + i, 0, 1);
    recomp_store(rt, object + 0x14, (uint32_t)id, 4);
    recomp_store(rt, object + 0x118, s->config.fpu_fr1 ? 0x0400ff03 : 0xff03, 4);
    recomp_store(rt, object + 0x128, 0x3f, 4);
    sync_guest(rt);
    RECOMP_LOG("thread",
               "created pthread id=%" PRId32 " thread=0x%08" PRIx32 " entry=0x%08" PRIx32 " arg=0x%08" PRIx32
               " stack=0x%08" PRIx32 " priority=%" PRId32,
               id, object, entry, argument, stack, priority);
}

void recomp_thread_start(Runtime *rt, uint32_t object) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, object);
    RECOMP_LOG("thread", "start id=%" PRId32 " thread=0x%08" PRIx32, t->id, t->object);
    if (t->state == RECOMP_DEAD)
        recomp_fail("cannot restart a destroyed thread", t->object);
    if (t->state == RECOMP_STOPPED && t->wait_queue) {
        t->wait_queue->wait_lists_dirty = true;
        t->state = t->wait_kind;
        t->order = ticket(s);
        wake(rt, t->wait_queue, t->wait_kind);
    } else if (t->state != RECOMP_READY && t->state != RECOMP_RUNNING) {
        ready(s, t);
    }
    sync_guest(rt);
    preempt(rt);
}

void recomp_thread_stop(Runtime *rt, uint32_t object) {
    NativeThread *t = thread(rt, object);
    RECOMP_LOG("thread", "stop id=%" PRId32 " thread=0x%08" PRIx32, t->id, t->object);
    if (t->state == RECOMP_DEAD)
        recomp_fail("cannot stop a destroyed thread", t->object);
    if (t->wait_queue)
        t->wait_queue->wait_lists_dirty = true;
    if (t == scheduler(rt)->current)
        suspend(rt, RECOMP_STOPPED);
    else {
        scheduler(rt)->ready_priority_valid = false;
        t->state = RECOMP_STOPPED;
        sync_guest(rt);
    }
}

void recomp_thread_destroy(Runtime *rt, uint32_t object) {
    NativeThread *t = thread(rt, object);
    if (t->state == RECOMP_DEAD)
        return;
    retire(t);
    if (t == scheduler(rt)->current) {
        suspend(rt, RECOMP_DEAD);
        recomp_fail("destroyed thread was resumed", t->object);
    } else {
        RecompScheduler *s = scheduler(rt);
        if (!guest_thread)
            pthread_mutex_lock(&s->mutex);
        join_thread(s, t);
        if (!guest_thread)
            pthread_mutex_unlock(&s->mutex);
        sync_guest(rt);
    }
}

void recomp_thread_yield(Runtime *rt) {
    suspend(rt, RECOMP_READY);
}
int32_t recomp_thread_priority(Runtime *rt, uint32_t object) {
    return thread(rt, object)->priority;
}
int32_t recomp_thread_id(Runtime *rt, uint32_t object) {
    return thread(rt, object)->id;
}
RecompThreadState recomp_thread_state(Runtime *rt, uint32_t object) {
    return thread(rt, object)->state;
}
const MipsContext *recomp_thread_context(Runtime *rt, uint32_t object) {
    return &thread(rt, object)->context;
}

void recomp_thread_set_priority(Runtime *rt, uint32_t object, int32_t priority) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, object);
    if (priority < 0 || priority > 255 || t->state == RECOMP_DEAD)
        recomp_fail("invalid thread priority update", object);
    if (t->priority != priority) {
        RECOMP_LOG("thread", "priority id=%" PRId32 " thread=0x%08" PRIx32 " old=%" PRId32 " new=%" PRId32, t->id,
                   t->object, t->priority, priority);
        if (t->wait_queue)
            t->wait_queue->wait_lists_dirty = true;
        s->ready_priority_valid = false;
        t->priority = priority;
        if (t->state != RECOMP_RUNNING && t->state != RECOMP_STOPPED)
            t->order = ticket(s);
        sync_guest(rt);
        preempt(rt);
    }
}

void recomp_queue_create(Runtime *rt, uint32_t object, uint32_t messages, int32_t count) {
    RECOMP_LOG("queue", "create queue=0x%08" PRIx32 " buffer=0x%08" PRIx32 " capacity=%" PRId32, object, messages,
               count);
    RecompScheduler *s = scheduler(rt);
    if (!object || object % 4 || messages % 4 || count <= 0 || (size_t)count > rt->rdram_size / 4)
        recomp_fail("invalid message queue/buffer/capacity", object);
    uint32_t key = recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
    recomp_ram_address(rt, messages, (size_t)count * 4);
    NativeQueue *q;
    for (q = s->queues; q && q->key != key; q = q->next) {
    }
    if (!q) {
        q = calloc(1, sizeof(*q));
        if (!q)
            recomp_fail("message queue allocation failed", object);
        q->object = object;
        q->key = key;
        q->next = s->queues;
        s->queues = q;
    } else {
        for (NativeThread *t = s->threads; t; t = t->next)
            if (t->wait_queue == q)
                recomp_fail("cannot reset a queue with suspended waiters", object);
    }
    recomp_store(rt, q->object + 8, 0, 4);
    recomp_store(rt, q->object + 12, 0, 4);
    recomp_store(rt, q->object + 16, (uint32_t)count, 4);
    recomp_store(rt, q->object + 20, messages, 4);
    q->wait_lists_dirty = true;
    sync_guest(rt);
}

typedef struct QueueFields {
    uint32_t valid, first, count, messages;
} QueueFields;
static QueueFields fields(Runtime *rt, NativeQueue *q) {
    QueueFields f = {(uint32_t)recomp_load(rt, q->object + 8, 4), (uint32_t)recomp_load(rt, q->object + 12, 4),
                     (uint32_t)recomp_load(rt, q->object + 16, 4), (uint32_t)recomp_load(rt, q->object + 20, 4)};
    if (!f.count || f.count > rt->rdram_size / 4 || f.valid > f.count || f.first >= f.count || f.messages % 4)
        recomp_fail("corrupt guest message queue", q->object);
    recomp_ram_address(rt, f.messages, (size_t)f.count * 4);
    return f;
}

int32_t recomp_queue_send(Runtime *rt, uint32_t object, uint32_t message, int32_t flag, bool jam) {
    /* libultra's nonblocking send rejects a zero-capacity queue before it
     * touches wait lists or the message buffer. A BSS queue can receive early
     * timer deliveries before osCreateMesgQueue initializes it. */
    if (object && flag == RECOMP_MESG_NOBLOCK) {
        recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
        if (recomp_load(rt, object + 8, 4) == 0 && recomp_load(rt, object + 16, 4) == 0)
            return -1;
    }
    NativeQueue *q = queue(rt, object);
    QueueFields f = fields(rt, q);
    while (f.valid == f.count) {
        if (flag != RECOMP_MESG_BLOCK) {
            RECOMP_LOG_SAMPLED("queue", "send full queue=0x%08" PRIx32 " message=0x%08" PRIx32, object, message);
            return -1;
        }
        NativeThread *t = thread(rt, 0);
        t->wait_queue = q;
        t->wait_kind = RECOMP_WAIT_SEND;
        suspend(rt, RECOMP_WAIT_SEND);
        f = fields(rt, q);
    }
    uint32_t index = (f.first + f.valid) % f.count;
    if (jam) {
        f.first = (f.first + f.count - 1) % f.count;
        index = f.first;
    }
    recomp_store(rt, f.messages + index * 4, message, 4);
    recomp_store(rt, q->object + 12, f.first, 4);
    recomp_store(rt, q->object + 8, f.valid + 1, 4);
    if (RECOMP_WATCH_QUEUE(object))
        RECOMP_LOG("queue-watch", "send queue=%08" PRIx32 " message=%08" PRIx32 " valid=%u id=%d pc=%08" PRIx32, object,
                   message, f.valid + 1, scheduler(rt)->current ? scheduler(rt)->current->id : 0,
                   scheduler(rt)->current ? scheduler(rt)->current->context.pc : 0);
    wake(rt, q, RECOMP_WAIT_RECV);
    sync_guest(rt);
    preempt(rt);
    return 0;
}

int32_t recomp_queue_recv(Runtime *rt, uint32_t object, uint32_t destination, int32_t flag) {
    /* libultra checks validCount before touching the ring or thread lists.
     * Zelda polls a zero-initialized queue for its unused disk audio medium. */
    if (object && !flag) {
        recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
        if (!recomp_load(rt, object + 8, 4))
            return -1;
    }
    NativeQueue *q = queue(rt, object);
    if (destination) {
        if (destination % 4)
            recomp_fail("unaligned message destination", destination);
        recomp_ram_address(rt, destination, 4);
    }
    QueueFields f = fields(rt, q);
    while (!f.valid) {
        if (!flag)
            return -1;
        if (RECOMP_WATCH_QUEUE(object))
            RECOMP_LOG("queue-watch", "wait queue=%08" PRIx32 " id=%d pc=%08" PRIx32, object, thread(rt, 0)->id,
                       thread(rt, 0)->context.pc);
        NativeThread *t = thread(rt, 0);
        t->wait_queue = q;
        t->wait_kind = RECOMP_WAIT_RECV;
        suspend(rt, RECOMP_WAIT_RECV);
        f = fields(rt, q);
    }
    if (destination)
        recomp_store(rt, destination, recomp_load(rt, f.messages + f.first * 4, 4), 4);
    if (RECOMP_WATCH_QUEUE(object))
        RECOMP_LOG("queue-watch", "recv queue=%08" PRIx32 " message=%08" PRIx32 " valid=%u id=%d pc=%08" PRIx32, object,
                   (uint32_t)recomp_load(rt, f.messages + f.first * 4, 4), f.valid - 1,
                   scheduler(rt)->current ? scheduler(rt)->current->id : 0,
                   scheduler(rt)->current ? scheduler(rt)->current->context.pc : 0);
    recomp_store(rt, q->object + 12, (f.first + 1) % f.count, 4);
    recomp_store(rt, q->object + 8, f.valid - 1, 4);
    wake(rt, q, RECOMP_WAIT_SEND);
    sync_guest(rt);
    preempt(rt);
    return 0;
}

int32_t recomp_queue_peek(Runtime *rt, uint32_t object, uint32_t *message) {
    QueueFields f = fields(rt, queue(rt, object));
    if (!f.valid)
        return -1;
    if (!message)
        recomp_fail("missing queue peek destination", object);
    *message = (uint32_t)recomp_load(rt, f.messages + f.first * 4, 4);
    return 0;
}

void recomp_event_bind(Runtime *rt, uint32_t event, uint32_t object, uint32_t message) {
    RECOMP_LOG("queue", "bind event=%" PRIu32 " queue=0x%08" PRIx32 " message=0x%08" PRIx32, event, object, message);
    RecompScheduler *s = scheduler(rt);
    if (event >= RECOMP_OS_EVENTS)
        recomp_fail("invalid OS event", event);
    s->events[event].queue = object ? queue(rt, object) : NULL;
    s->events[event].message = message;
    if (s->config.event_table) {
        recomp_store(rt, s->config.event_table + event * 8, object, 4);
        recomp_store(rt, s->config.event_table + event * 8 + 4, message, 4);
    }
}

bool recomp_event_post(Runtime *rt, uint32_t event) {
    RecompScheduler *s = rt->scheduler;
    if (!s || event >= RECOMP_OS_EVENTS)
        return false;
    lock_pending(s);
    bool accepted = s->pending_count < PENDING_EVENTS;
    if (accepted) {
        s->pending[(s->pending_first + s->pending_count) % PENDING_EVENTS] = event;
        s->pending_count++;
    }
    atomic_flag_clear_explicit(&s->pending_lock, memory_order_release);
    return accepted;
}
