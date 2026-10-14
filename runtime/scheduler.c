#define _DEFAULT_SOURCE 1
#define MINICORO_IMPL
#define MCO_USE_VMEM_ALLOCATOR
#define MCO_NO_DEBUG
#include "minicoro.h"

#include "runtime.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct NativeQueue NativeQueue;
typedef struct NativeThread {
    struct NativeThread *next;
    Runtime *runtime;
    mco_coro *coroutine;
    MipsContext context;
    RecompFunction entry;
    uint32_t object, key;
    int32_t id, priority;
    RecompThreadState state, wait_kind;
    NativeQueue *wait_queue;
    uint64_t order;
    unsigned checkpoints;
    uintptr_t trace_ceiling;
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
    NativeQueue *queues;
    uint64_t order;
    struct { NativeQueue *queue; uint32_t message; } events[RECOMP_OS_EVENTS];
    atomic_flag pending_lock;
    uint32_t pending[PENDING_EVENTS];
    unsigned pending_first, pending_count;
    bool polling;
};

static RecompScheduler *scheduler(Runtime *rt) {
    if (!rt->scheduler) recomp_fail("scheduler not initialized", 0);
    return rt->scheduler;
}
size_t recomp_thread_snapshots(Runtime *rt,RecompThreadSnapshot *out,size_t capacity) {
    if (!rt->scheduler || !out) return 0;
    size_t count=0;
    for (NativeThread *t=rt->scheduler->threads;t && count<capacity;t=t->next)
        out[count++]=(RecompThreadSnapshot){.object=t->object,.id=t->id,.priority=t->priority,
            .state=t->state,.current=t==rt->scheduler->current,.context=t->context};
    return count;
}

static uint64_t ticket(RecompScheduler *s) {
    if (s->order == UINT64_MAX) recomp_fail("scheduler order exhausted", 0);
    return ++s->order;
}

static NativeThread *thread(Runtime *rt, uint32_t object) {
    RecompScheduler *s = scheduler(rt);
    if (!object) {
        if (!s->current) recomp_fail("no current guest thread", 0);
        return s->current;
    }
    uint32_t key = recomp_ram_address(rt, object, s->config.fpu_fr1 ? 0x230 : RECOMP_THREAD_BYTES);
    for (NativeThread *t = s->threads; t; t = t->next)
        if (t->key == key) return t;
    recomp_fail("unknown guest thread", object);
}

static NativeQueue *queue(Runtime *rt, uint32_t object) {
    uint32_t key = recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
    for (NativeQueue *q = scheduler(rt)->queues; q; q = q->next)
        if (q->key == key) return q;
    recomp_fail("unknown message queue", object);
}

static bool before(const NativeThread *a, const NativeThread *b) {
    return a->priority > b->priority || (a->priority == b->priority && a->order < b->order);
}

static NativeThread *best(RecompScheduler *s, RecompThreadState state, NativeQueue *q,
                          NativeThread *after) {
    NativeThread *result = NULL;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state != state || (q && t->wait_queue != q) || (after && !before(after, t))) continue;
        if (!result || before(t, result)) result = t;
    }
    return result;
}

static void save_context(Runtime *rt, NativeThread *t) {
    static const unsigned registers[] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31};
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

static void sync_list(Runtime *rt, RecompThreadState state, NativeQueue *q, uint32_t head) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *first = best(s, state, q, NULL);
    if (head) recomp_store(rt, head, first ? first->object : s->config.thread_tail, 4);
    for (NativeThread *t = first; t;) {
        NativeThread *next = best(s, state, q, t);
        recomp_store(rt, t->object, next ? next->object : s->config.thread_tail, 4);
        recomp_store(rt, t->object + 8, head, 4);
        t = next;
    }
}

static void sync_guest(Runtime *rt) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *active = NULL, *previous = NULL;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state == RECOMP_DEAD) continue;
        unsigned state = t->state == RECOMP_RUNNING ? RECOMP_OS_RUNNING
                       : t->state == RECOMP_READY ? RECOMP_OS_RUNNABLE
                       : t->state == RECOMP_WAIT_SEND || t->state == RECOMP_WAIT_RECV ? RECOMP_OS_WAITING
                       : RECOMP_OS_STOPPED;
        recomp_store(rt, t->object + 4, (uint32_t)t->priority, 4);
        recomp_store(rt, t->object + 0x10, state, 2);
        /* Waiting-thread next links belong to their queue. Preserve them
         * unless that queue's waiter list is being updated below. */
        if(t->state!=RECOMP_WAIT_SEND && t->state!=RECOMP_WAIT_RECV)
            recomp_store(rt, t->object, 0, 4);
        uint32_t queue_head = t->wait_queue ? t->wait_queue->object + (t->wait_kind == RECOMP_WAIT_SEND ? 4 : 0)
                            : t->state == RECOMP_RUNNING ? s->config.run_queue : 0;
        recomp_store(rt, t->object + 8, queue_head, 4);
        recomp_store(rt, t->object + 0x0c, 0, 4);
        save_context(rt, t);
        if (!active) active = t;
        if (previous) recomp_store(rt, previous->object + 0x0c, t->object, 4);
        previous = t;
    }
    if (s->config.active_queue) recomp_store(rt, s->config.active_queue, active ? active->object : 0, 4);
    if (s->config.running_thread) recomp_store(rt, s->config.running_thread,
        s->current && s->current->state == RECOMP_RUNNING ? s->current->object : 0, 4);
    sync_list(rt, RECOMP_READY, NULL, s->config.run_queue);
    for (NativeQueue *q = s->queues; q; q = q->next) {
        /* Overlay storage can be reused after a queue is no longer needed.
         * A cached empty queue must not rewrite memory now owned by assets. */
        if (!q->wait_lists_dirty) continue;
        sync_list(rt, RECOMP_WAIT_RECV, q, q->object);
        sync_list(rt, RECOMP_WAIT_SEND, q, q->object + 4);
        q->wait_lists_dirty=false;
    }
}

static void ready(RecompScheduler *s, NativeThread *t) {
    if (t->wait_queue) t->wait_queue->wait_lists_dirty=true;
    t->state = RECOMP_READY;
    t->wait_queue = NULL;
    t->order = ticket(s);
}

static void suspend(Runtime *rt, RecompThreadState state) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, 0);
    if (t->wait_queue) t->wait_queue->wait_lists_dirty=true;
    t->state = state;
    t->order = ticket(s);
    sync_guest(rt);
    if (mco_yield(t->coroutine) != MCO_SUCCESS) recomp_fail("coroutine yield failed", t->object);
}

static void preempt(Runtime *rt) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *candidate = best(s, RECOMP_READY, NULL, NULL);
    if (!s->polling && s->current && candidate && candidate->priority > s->current->priority)
        suspend(rt, RECOMP_READY);
}

static void wake(Runtime *rt, NativeQueue *q, RecompThreadState state) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = best(s, state, q, NULL);
    if (t) ready(s, t);
}

static void retire(NativeThread *t) {
    t->state = RECOMP_DEAD;
    if (t->wait_queue) t->wait_queue->wait_lists_dirty=true;
    t->wait_queue = NULL;
    save_context(t->runtime, t);
    recomp_store(t->runtime, t->object, 0, 4);
    recomp_store(t->runtime, t->object + 8, 0, 4);
    recomp_store(t->runtime, t->object + 0x0c, 0, 4);
    recomp_store(t->runtime, t->object + 0x10, RECOMP_OS_STOPPED, 2);
}

static void enter(mco_coro *coroutine) {
    unsigned char trace_boundary=0;
    NativeThread *t = mco_get_user_data(coroutine);
    t->trace_ceiling=(uintptr_t)&trace_boundary;
    recomp_set_trace_ceiling(t->trace_ceiling);
    const RecompSchedulerConfig *config = &scheduler(t->runtime)->config;
    bool stop = config->stop_before_entry == t->context.pc;
    for (size_t i = 0; i < config->stop_before_count; i++)
        if (config->stop_before_entries[i] == t->context.pc) stop = true;
    if (stop)
        suspend(t->runtime, RECOMP_STOPPED);
    t->entry(t->runtime, &t->context);
    retire(t);
}

static void lock_pending(RecompScheduler *s) {
    while (atomic_flag_test_and_set_explicit(&s->pending_lock, memory_order_acquire)) { }
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
        if (!present) break;
        if (s->events[event].queue)
            (void)recomp_queue_send(rt, s->events[event].queue->object, s->events[event].message, RECOMP_MESG_NOBLOCK, false);
    }
}

static void poll(Runtime *rt) {
    RecompScheduler *s = scheduler(rt);
    if (s->polling) return;
    /* A timer/VI/DMA service finishes updating its records before guest code
     * can run. Otherwise queue-send preemption could reenter that service. */
    s->polling = true;
    if (!rt->platform || rt->platform_initialized) {
        if (rt->poll_clock) rt->poll_clock(rt);
        if (rt->poll_platform) rt->poll_platform(rt);
        if (rt->poll_tasks) rt->poll_tasks(rt);
        if (rt->poll_io) rt->poll_io(rt);
    }
    deliver_events(rt);
    s->polling = false;
}

static void checkpoint(Runtime *rt, bool force_yield) {
    if (rt->interrupts_disabled) return;
    RecompScheduler *s = scheduler(rt);
    poll(rt);
    if (!s->current) return;
    unsigned interval = s->config.checkpoint_interval ? s->config.checkpoint_interval : 64;
    if (force_yield || ++s->current->checkpoints >= interval) {
        s->current->checkpoints = 0;
        suspend(rt, RECOMP_READY);
    } else {
        preempt(rt);
    }
}

bool recomp_scheduler_init(Runtime *rt, const RecompSchedulerConfig *config) {
    if (rt->scheduler || !config || !config->thread_tail || config->thread_tail % 4) return false;
    if (config->stop_before_count && !config->stop_before_entries) return false;
    recomp_ram_address(rt, config->thread_tail, 8);
    uint32_t globals[] = {config->run_queue, config->active_queue, config->running_thread, config->event_table};
    for (unsigned i = 0; i < sizeof(globals) / sizeof(globals[0]); i++) {
        if (globals[i] % 4) return false;
        if (globals[i]) recomp_ram_address(rt, globals[i], i == 3 ? RECOMP_OS_EVENTS * 8 : 4);
    }
    RecompScheduler *s = calloc(1, sizeof(*s));
    if (!s) return false;
    s->config = *config;
    s->pending_lock = (atomic_flag)ATOMIC_FLAG_INIT;
    rt->scheduler = s;
    rt->destroy_scheduler = recomp_scheduler_destroy;
    rt->checkpoint = checkpoint;
    recomp_store(rt, config->thread_tail, 0, 4);
    recomp_store(rt, config->thread_tail + 4, UINT32_MAX, 4);
    if (config->event_table)
        for (unsigned i = 0; i < RECOMP_OS_EVENTS * 8; i++) recomp_store(rt, config->event_table + i, 0, 1);
    sync_guest(rt);
    return true;
}

void recomp_scheduler_destroy(Runtime *rt) {
    RecompScheduler *s = rt->scheduler;
    if (!s) return;
    if (s->current) recomp_fail("cannot destroy scheduler inside a guest coroutine", s->current->object);
    for (NativeThread *t = s->threads; t;) {
        NativeThread *next = t->next;
        if (t->coroutine && mco_destroy(t->coroutine) != MCO_SUCCESS) recomp_fail("coroutine destruction failed", t->object);
        free(t);
        t = next;
    }
    for (NativeQueue *q = s->queues; q;) {
        NativeQueue *next = q->next;
        free(q);
        q = next;
    }
    free(s);
    rt->scheduler = NULL;
    rt->destroy_scheduler = NULL;
    rt->checkpoint = NULL;
}

RecompRunResult recomp_scheduler_run(Runtime *rt, size_t max_resumes) {
    RecompScheduler *s = scheduler(rt);
    if (s->current || !max_resumes) recomp_fail("invalid or nested scheduler run", 0);
    for (size_t count = 0; count < max_resumes; count++) {
        poll(rt);
        NativeThread *t = best(s, RECOMP_READY, NULL, NULL);
        if (!t) break;
        s->current = t;
        t->state = RECOMP_RUNNING;
        sync_guest(rt);
        recomp_set_trace_ceiling(t->trace_ceiling);
        mco_result resumed=mco_resume(t->coroutine);
        recomp_set_trace_ceiling(0);
        if (resumed != MCO_SUCCESS) recomp_fail("coroutine resume failed", t->object);
        s->current = NULL;
        if (t->state == RECOMP_RUNNING) recomp_fail("coroutine returned without a state transition", t->object);
        if (t->state == RECOMP_DEAD && t->coroutine) {
            if (mco_destroy(t->coroutine) != MCO_SUCCESS) recomp_fail("coroutine destruction failed", t->object);
            t->coroutine = NULL;
        }
        sync_guest(rt);
    }
    if (best(s, RECOMP_READY, NULL, NULL)) return RECOMP_RUN_BUDGET;
    bool stopped = false;
    for (NativeThread *t = s->threads; t; t = t->next) {
        if (t->state == RECOMP_WAIT_SEND || t->state == RECOMP_WAIT_RECV) return RECOMP_RUN_WAITING;
        if (t->state == RECOMP_STOPPED) stopped = true;
    }
    return stopped ? RECOMP_RUN_STOPPED : RECOMP_RUN_COMPLETE;
}

void recomp_thread_create(Runtime *rt, uint32_t object, int32_t id, uint32_t entry,
                          uint32_t argument, uint32_t stack, int32_t priority) {
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
            if (t->state != RECOMP_DEAD) recomp_fail("thread object is already active", object);
            *link = t->next;
            if (t->coroutine && mco_destroy(t->coroutine) != MCO_SUCCESS)
                recomp_fail("coroutine destruction failed", t->object);
            free(t);
            break;
        }
        link = &t->next;
    }
    NativeThread *t = calloc(1, sizeof(*t));
    if (!t) recomp_fail("thread allocation failed", object);
    t->runtime = rt; t->object = object; t->key = key; t->id = id; t->priority = priority;
    t->state = RECOMP_STOPPED;
    t->entry = recomp_lookup(rt, entry);
    t->context.pc = entry;
    t->context.gpr[4] = mips_s32(argument);
    t->context.gpr[29] = mips_s32(stack - 16);
    t->context.gpr[31] = mips_s32(UINT32_C(0xf0000000));
    t->context.fcr31 = UINT32_C(0x01000800);
    t->context.fpu_fr1 = s->config.fpu_fr1;
    mco_desc description = mco_desc_init(enter, s->config.native_stack_size);
    description.user_data = t;
    if (mco_create(&t->coroutine, &description) != MCO_SUCCESS) {
        free(t); recomp_fail("coroutine allocation failed", object);
    }
    t->next = s->threads; s->threads = t;
    for (unsigned i = 0; i < bytes; i++) recomp_store(rt, object + i, 0, 1);
    recomp_store(rt, object + 0x14, (uint32_t)id, 4);
    recomp_store(rt, object + 0x118, s->config.fpu_fr1 ? 0x0400ff03 : 0xff03, 4);
    recomp_store(rt, object + 0x128, 0x3f, 4);
    sync_guest(rt);
}

void recomp_thread_start(Runtime *rt, uint32_t object) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, object);
    if (t->state == RECOMP_DEAD) recomp_fail("cannot restart a destroyed thread", t->object);
    if (t->state == RECOMP_STOPPED && t->wait_queue) {
        t->wait_queue->wait_lists_dirty=true;
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
    if (t->state == RECOMP_DEAD) recomp_fail("cannot stop a destroyed thread", t->object);
    if (t->wait_queue) t->wait_queue->wait_lists_dirty=true;
    if (t == scheduler(rt)->current) suspend(rt, RECOMP_STOPPED);
    else { t->state = RECOMP_STOPPED; sync_guest(rt); }
}

void recomp_thread_destroy(Runtime *rt, uint32_t object) {
    NativeThread *t = thread(rt, object);
    if (t->state == RECOMP_DEAD) return;
    retire(t);
    if (t == scheduler(rt)->current) {
        suspend(rt, RECOMP_DEAD);
        recomp_fail("destroyed thread was resumed", t->object);
    } else {
        if (t->coroutine) {
            if (mco_destroy(t->coroutine) != MCO_SUCCESS) recomp_fail("coroutine destruction failed", t->object);
            t->coroutine = NULL;
        }
        sync_guest(rt);
    }
}

void recomp_thread_yield(Runtime *rt) { suspend(rt, RECOMP_READY); }
int32_t recomp_thread_priority(Runtime *rt, uint32_t object) { return thread(rt, object)->priority; }
int32_t recomp_thread_id(Runtime *rt, uint32_t object) { return thread(rt, object)->id; }
RecompThreadState recomp_thread_state(Runtime *rt, uint32_t object) { return thread(rt, object)->state; }
const MipsContext *recomp_thread_context(Runtime *rt, uint32_t object) { return &thread(rt, object)->context; }

void recomp_thread_set_priority(Runtime *rt, uint32_t object, int32_t priority) {
    RecompScheduler *s = scheduler(rt);
    NativeThread *t = thread(rt, object);
    if (priority < 0 || priority > 255 || t->state == RECOMP_DEAD) recomp_fail("invalid thread priority update", object);
    if (t->priority != priority) {
        if (t->wait_queue) t->wait_queue->wait_lists_dirty=true;
        t->priority = priority;
        if (t->state != RECOMP_RUNNING && t->state != RECOMP_STOPPED) t->order = ticket(s);
        sync_guest(rt);
        preempt(rt);
    }
}

void recomp_queue_create(Runtime *rt, uint32_t object, uint32_t messages, int32_t count) {
    RecompScheduler *s = scheduler(rt);
    if (!object || object % 4 || messages % 4 || count <= 0 || (size_t)count > rt->rdram_size / 4)
        recomp_fail("invalid message queue/buffer/capacity", object);
    uint32_t key = recomp_ram_address(rt, object, RECOMP_QUEUE_BYTES);
    recomp_ram_address(rt, messages, (size_t)count * 4);
    NativeQueue *q;
    for (q = s->queues; q && q->key != key; q = q->next) { }
    if (!q) {
        q = calloc(1, sizeof(*q));
        if (!q) recomp_fail("message queue allocation failed", object);
        q->object = object; q->key = key; q->next = s->queues; s->queues = q;
    } else {
        for (NativeThread *t = s->threads; t; t = t->next)
            if (t->wait_queue == q) recomp_fail("cannot reset a queue with suspended waiters", object);
    }
    recomp_store(rt, q->object + 8, 0, 4);
    recomp_store(rt, q->object + 12, 0, 4);
    recomp_store(rt, q->object + 16, (uint32_t)count, 4);
    recomp_store(rt, q->object + 20, messages, 4);
    q->wait_lists_dirty=true;
    sync_guest(rt);
}

typedef struct QueueFields { uint32_t valid, first, count, messages; } QueueFields;
static QueueFields fields(Runtime *rt, NativeQueue *q) {
    QueueFields f = {(uint32_t)recomp_load(rt, q->object + 8, 4), (uint32_t)recomp_load(rt, q->object + 12, 4),
                     (uint32_t)recomp_load(rt, q->object + 16, 4), (uint32_t)recomp_load(rt, q->object + 20, 4)};
    if (!f.count || f.count > rt->rdram_size / 4 || f.valid > f.count || f.first >= f.count || f.messages % 4)
        recomp_fail("corrupt guest message queue", q->object);
    recomp_ram_address(rt, f.messages, (size_t)f.count * 4);
    return f;
}

int32_t recomp_queue_send(Runtime *rt, uint32_t object, uint32_t message, int32_t flag, bool jam) {
    NativeQueue *q = queue(rt, object);
    QueueFields f = fields(rt, q);
    while (f.valid == f.count) {
        if (flag != RECOMP_MESG_BLOCK) return -1;
        NativeThread *t = thread(rt, 0);
        t->wait_queue = q; t->wait_kind = RECOMP_WAIT_SEND;
        suspend(rt, RECOMP_WAIT_SEND);
        f = fields(rt, q);
    }
    uint32_t index = (f.first + f.valid) % f.count;
    if (jam) { f.first = (f.first + f.count - 1) % f.count; index = f.first; }
    recomp_store(rt, f.messages + index * 4, message, 4);
    recomp_store(rt, q->object + 12, f.first, 4);
    recomp_store(rt, q->object + 8, f.valid + 1, 4);
    wake(rt, q, RECOMP_WAIT_RECV);
    sync_guest(rt);
    preempt(rt);
    return 0;
}

int32_t recomp_queue_recv(Runtime *rt, uint32_t object, uint32_t destination, int32_t flag) {
    NativeQueue *q = queue(rt, object);
    if (destination) {
        if (destination % 4) recomp_fail("unaligned message destination", destination);
        recomp_ram_address(rt, destination, 4);
    }
    QueueFields f = fields(rt, q);
    while (!f.valid) {
        if (!flag) return -1;
        NativeThread *t = thread(rt, 0);
        t->wait_queue = q; t->wait_kind = RECOMP_WAIT_RECV;
        suspend(rt, RECOMP_WAIT_RECV);
        f = fields(rt, q);
    }
    if (destination) recomp_store(rt, destination, recomp_load(rt, f.messages + f.first * 4, 4), 4);
    recomp_store(rt, q->object + 12, (f.first + 1) % f.count, 4);
    recomp_store(rt, q->object + 8, f.valid - 1, 4);
    wake(rt, q, RECOMP_WAIT_SEND);
    sync_guest(rt);
    preempt(rt);
    return 0;
}

int32_t recomp_queue_peek(Runtime *rt, uint32_t object, uint32_t *message) {
    QueueFields f = fields(rt, queue(rt, object));
    if (!f.valid) return -1;
    if (!message) recomp_fail("missing queue peek destination", object);
    *message = (uint32_t)recomp_load(rt, f.messages + f.first * 4, 4);
    return 0;
}

void recomp_event_bind(Runtime *rt, uint32_t event, uint32_t object, uint32_t message) {
    RecompScheduler *s = scheduler(rt);
    if (event >= RECOMP_OS_EVENTS) recomp_fail("invalid OS event", event);
    s->events[event].queue = object ? queue(rt, object) : NULL;
    s->events[event].message = message;
    if (s->config.event_table) {
        recomp_store(rt, s->config.event_table + event * 8, object, 4);
        recomp_store(rt, s->config.event_table + event * 8 + 4, message, 4);
    }
}

bool recomp_event_post(Runtime *rt, uint32_t event) {
    RecompScheduler *s = rt->scheduler;
    if (!s || event >= RECOMP_OS_EVENTS) return false;
    lock_pending(s);
    bool accepted = s->pending_count < PENDING_EVENTS;
    if (accepted) {
        s->pending[(s->pending_first + s->pending_count) % PENDING_EVENTS] = event;
        s->pending_count++;
    }
    atomic_flag_clear_explicit(&s->pending_lock, memory_order_release);
    return accepted;
}
