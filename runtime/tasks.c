#include "runtime.h"
#include <stdatomic.h>
#include <stdlib.h>

typedef struct TaskRecord {
    struct TaskRecord *next;
    RecompTask task;
    bool yield_requested, sp_done, dp_done, yielded, sp_posted, dp_posted, sp_emitted, dp_emitted;
    bool no_dp_sync;
} TaskRecord;
enum { COMPLETIONS=256 };
struct RecompTasks {
    RecompTaskBackend backend;
    TaskRecord *records, *active;
    RecompTask loaded;
    RecompTask observed;
    bool has_observed;
    bool has_loaded, last_yielded, last_requested, has_yielded_task;
    uint32_t last_key, yielded_key, last_type;
    uint64_t next_token;
    RecompTaskStats stats;
    atomic_flag lock;
    struct { uint64_t token; RecompTaskCompletion kind; } pending[COMPLETIONS];
    unsigned first, count;
};

static RecompTasks *tasks(Runtime *rt) {
    if (!rt->tasks) recomp_fail("task backend not initialized", 0);
    return rt->tasks;
}
static TaskRecord *record(RecompTasks *t, uint64_t token) {
    for (TaskRecord *r=t->records;r;r=r->next) if (r->task.token==token) return r;
    return NULL;
}
static void lock(RecompTasks *t) {
    while (atomic_flag_test_and_set_explicit(&t->lock,memory_order_acquire)) { }
}
bool recomp_task_post_completion(Runtime *rt, uint64_t token, RecompTaskCompletion kind) {
    RecompTasks *t=rt->tasks;
    if (!t || !token || (kind!=RECOMP_SP_DONE && kind!=RECOMP_SP_YIELDED && kind!=RECOMP_DP_DONE)) return false;
    lock(t);
    bool ok=t->count<COMPLETIONS;
    if (ok) {
        unsigned slot=(t->first+t->count)%COMPLETIONS;
        t->pending[slot].token=token; t->pending[slot].kind=kind; t->count++;
    }
    atomic_flag_clear_explicit(&t->lock,memory_order_release);
    return ok;
}

static bool valid(TaskRecord *r, RecompTaskCompletion kind) {
    if (!r) return false;
    if (kind==RECOMP_DP_DONE) return r->task.words[0]==1 && !r->dp_done && !r->yielded && !r->no_dp_sync;
    return !r->sp_done && (kind==RECOMP_SP_DONE
           || (kind==RECOMP_SP_YIELDED && r->yield_requested && r->task.words[0]==1 && !r->dp_done && !r->dp_posted));
}
bool recomp_task_complete(Runtime *rt, uint64_t token, RecompTaskCompletion kind) {
    RecompTasks *t=tasks(rt); TaskRecord *r=record(t,token);
    if (!valid(r,kind) || (kind==RECOMP_DP_DONE?r->dp_posted:r->sp_posted)) return false;
    if (!recomp_task_post_completion(rt,token,kind)) return false;
    if (kind==RECOMP_DP_DONE) r->dp_posted=true; else r->sp_posted=true;
    return true;
}
bool recomp_task_finish_without_dp(Runtime *rt, uint64_t token) {
    TaskRecord *r=record(tasks(rt),token);
    if (!r || r->task.words[0]!=1 || !r->sp_posted || r->dp_posted || r->yielded || r->no_dp_sync)
        return false;
    r->no_dp_sync=true;
    return true;
}

static void poll_tasks(Runtime *rt) {
    RecompTasks *t=tasks(rt);
    for (unsigned i=0;i<COMPLETIONS;i++) {
        lock(t);
        bool present=t->count!=0;
        uint64_t token=0; RecompTaskCompletion kind=RECOMP_SP_DONE;
        if (present) {
            token=t->pending[t->first].token; kind=t->pending[t->first].kind;
            t->first=(t->first+1)%COMPLETIONS; t->count--;
        }
        atomic_flag_clear_explicit(&t->lock,memory_order_release);
        if (!present) break;
        TaskRecord *r=record(t,token);
        if (!valid(r,kind)) { t->stats.rejected_completions++; continue; }
        if (kind==RECOMP_DP_DONE) r->dp_done=true;
        else {
            r->sp_done=true; r->yielded=kind==RECOMP_SP_YIELDED;
            t->last_key=recomp_ram_address(rt,r->task.guest_address,64);
            t->last_type=r->task.words[0];
            t->last_yielded=r->yielded; t->last_requested=r->yield_requested;
            if (r->yielded) { t->yielded_key=t->last_key; t->has_yielded_task=true; }
            if (t->active==r) t->active=NULL;
        }
    }
    TaskRecord **link=&t->records;
    while (*link) {
        TaskRecord *r=*link;
        if (r->sp_done && !r->sp_emitted && recomp_event_post(rt,4)) {
            r->sp_emitted=true; t->stats.sp_events++; if (r->yielded) t->stats.yielded++;
        }
        /* Gate early DP receipts until SP has been queued. SM64's dispatcher
         * consumes SP then DP separately; a full sync never creates an SP event. */
        if (r->sp_emitted && r->dp_done && !r->dp_emitted && recomp_event_post(rt,9)) {
            r->dp_emitted=true; t->stats.dp_events++;
        }
        bool done=r->sp_emitted && (r->task.words[0]==2 || r->yielded || r->no_dp_sync || r->dp_emitted);
        if (done) { *link=r->next; free(r); t->stats.pending_records--; }
        else link=&r->next;
    }
}

bool recomp_tasks_init(Runtime *rt, const RecompTaskBackend *backend) {
    if (rt->tasks || !rt->scheduler || !backend || !backend->submit) return false;
    RecompTasks *t=calloc(1,sizeof(*t));
    if (!t) return false;
    t->backend=*backend; t->lock=(atomic_flag)ATOMIC_FLAG_INIT;
    rt->tasks=t; rt->destroy_tasks=recomp_tasks_destroy; rt->poll_tasks=poll_tasks;
    return true;
}
void recomp_tasks_destroy(Runtime *rt) {
    RecompTasks *t=rt->tasks;
    if (!t) return;
    for (TaskRecord *r=t->records;r;) { TaskRecord *next=r->next; free(r); r=next; }
    free(t); rt->tasks=NULL; rt->destroy_tasks=NULL; rt->poll_tasks=NULL;
}
RecompTaskStats recomp_task_stats(Runtime *rt) { return tasks(rt)->stats; }
bool recomp_task_snapshot(Runtime *rt,RecompTask *out) {
    if (!rt->tasks || !out || !rt->tasks->has_observed) return false;
    *out=rt->tasks->observed;return true;
}

static void buffer(Runtime *rt, uint32_t address, uint32_t bytes, bool required, uint32_t object) {
    if ((!address && bytes) || (required && (!address || !bytes))) recomp_fail("invalid task buffer",object);
    if (address) recomp_ram_address(rt,address,bytes?bytes:1);
}
void hook_os_sp_task_load(Runtime *rt, MipsContext *ctx) {
    RecompTasks *t=tasks(rt);
    uint32_t object=(uint32_t)ctx->gpr[4];
    if (!object || object%8 || t->active) recomp_fail("invalid or busy SP task load",object);
    recomp_ram_address(rt,object,64);
    RecompTask task={.guest_address=object};
    for (unsigned i=0;i<16;i++) task.words[i]=(uint32_t)recomp_load(rt,object+i*4,4);
    t->observed=task;t->has_observed=true;
    if (task.words[0]!=1 && task.words[0]!=2) recomp_fail("unsupported SP task type",object);
    uint32_t flags = RECOMP_TASK_YIELDED | RECOMP_TASK_DP_WAIT | RECOMP_TASK_USER_FLAGS;
    if (task.words[0] == 1) flags |= RECOMP_TASK_LOADABLE;
    if (task.words[1]&~flags) recomp_fail("unsupported SP task flags",object);
    buffer(rt,task.words[2],task.words[3],true,object);
    /* Older SDKs leave ucode_size zero. The RSP bootstrap loads its fixed
     * IMEM program; the native backend recognizes and bounds that program. */
    if(!task.words[4])recomp_fail("invalid task microcode pointer",object);
    buffer(rt,task.words[4],task.words[5],false,object);
    buffer(rt,task.words[6],task.words[7],false,object);
    buffer(rt,task.words[8],task.words[9],false,object);
    buffer(rt,task.words[12],task.words[13],true,object);
    buffer(rt,task.words[14],task.words[15],false,object);
    /* Graphics ucode_size describes the linked source image, including code
     * overlays loaded by the bootstrap. It can exceed the 4 KiB IMEM bank.
     * RT64 recognizes these images with a bounded 8 KiB source range. */
    uint32_t code_limit = task.words[0] == 1 ? 8192 : 4096;
    if (task.words[3]>4096 || task.words[5]>code_limit)
        recomp_fail("SP microcode source exceeds backend bounds",object);
    if (task.words[10]) recomp_ram_address(rt,task.words[10],1);
    if (task.words[11]) recomp_ram_address(rt,task.words[11],8);
    task.resumed=(task.words[1]&1)!=0;
    if (task.resumed) {
        uint32_t key=recomp_ram_address(rt,object,64);
        if (task.words[0]!=1 || !t->has_yielded_task || t->yielded_key!=key || !task.words[14] || !task.words[15])
            recomp_fail("invalid SP task resume",object);
        recomp_store(rt,object+4,task.words[1]&~1u,4);
        task.words[6]=task.words[14]; task.words[7]=task.words[15];
    }
    t->loaded=task; t->has_loaded=true;
}
void hook_os_sp_task_start_go(Runtime *rt, MipsContext *ctx) {
    RecompTasks *t=tasks(rt); uint32_t object=(uint32_t)ctx->gpr[4];
    if (!t->has_loaded || t->active || recomp_ram_address(rt,object,64)!=recomp_ram_address(rt,t->loaded.guest_address,64))
        recomp_fail("SP start without matching loaded task",object);
    if (t->next_token==UINT64_MAX) recomp_fail("SP task token exhausted",object);
    if (t->has_yielded_task && t->loaded.words[0]==1
            && recomp_ram_address(rt,object,64)!=t->yielded_key)
        recomp_fail("multiple suspended graphics tasks require additional backend support",object);
    TaskRecord *r=calloc(1,sizeof(*r));
    if (!r) recomp_fail("SP task allocation failed",object);
    r->task=t->loaded; r->task.token=++t->next_token;
    t->observed=r->task;
    if (t->has_yielded_task && t->yielded_key==recomp_ram_address(rt,object,64)) t->has_yielded_task=false;
    r->next=t->records; t->records=r; t->active=r; t->has_loaded=false;
    t->stats.submissions++; t->stats.pending_records++;
    if (!t->backend.submit(rt,t->backend.userdata,&r->task)) recomp_fail("backend rejected SP task",object);
}
void hook_os_sp_task_yield(Runtime *rt, MipsContext *ctx) {
    RecompTasks *t=tasks(rt);
    if (!t->active) {
        /* A VI notification can reach the game before the queued SP event.
         * Remember its late request so Yielded clears the guest DP-wait flag. */
        if (t->last_type==1) t->last_requested=true;
        return;
    }
    TaskRecord *r=t->active;
    if (r->task.words[0]!=1 || !t->backend.yield) recomp_fail("SP task cannot yield",ctx->pc);
    if (r->yield_requested) return;
    r->yield_requested=true;
    if (r->sp_posted) return; /* The owner's completion is already queued. */
    if (!t->backend.yield(rt,t->backend.userdata,r->task.token)) recomp_fail("backend rejected SP yield",ctx->pc);
}
void hook_os_sp_task_yielded(Runtime *rt, MipsContext *ctx) {
    RecompTasks *t=tasks(rt); uint32_t object=(uint32_t)ctx->gpr[4];
    if (recomp_ram_address(rt,object,64)!=t->last_key) recomp_fail("yield query does not match SP completion",object);
    uint32_t flags=(uint32_t)recomp_load(rt,object+4,4);
    if (t->last_requested) recomp_store(rt,object+4,(flags|(t->last_yielded?1u:0u))&~2u,4);
    mips_set(ctx,2,t->last_yielded?1:0);
}
