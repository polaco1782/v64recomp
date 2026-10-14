#define _POSIX_C_SOURCE 200809L
#include "runtime.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"threading check failed: %s:%d: %s\n",__FILE__,__LINE__,#x);abort(); } } while(0)
enum { HIGH=0x100, LOW=0x400, VICTIM=0x700, QUEUE=0xa00, MESSAGES=0xa40, DEST=0xa50 };
static pthread_t host,high_native,low_native;
static int sequence[16],cursor,host_calls,victims;
static void mark(int value) { sequence[cursor++]=value; }
static void host_callback(Runtime *rt,void *opaque) {
    (void)rt;CHECK(pthread_equal(pthread_self(),host));CHECK(*(int *)opaque==123);host_calls++;
}
static void high(Runtime *rt,MipsContext *ctx) {
    (void)ctx;high_native=pthread_self();CHECK(!pthread_equal(high_native,host));
    int saved=123;mark(1);
    recomp_on_host(rt,host_callback,&saved);
    CHECK(!recomp_queue_recv(rt,QUEUE,DEST,RECOMP_MESG_BLOCK));
    CHECK(recomp_load(rt,DEST,4)==0x12345678 && saved==123);mark(3);
    recomp_thread_stop(rt,0);
    CHECK(saved==123);mark(5);
}
static void victim(Runtime *rt,MipsContext *ctx) { (void)rt;(void)ctx;victims++; }
static void low(Runtime *rt,MipsContext *ctx) {
    (void)ctx;low_native=pthread_self();CHECK(!pthread_equal(low_native,host));mark(2);
    CHECK(!recomp_queue_send(rt,QUEUE,0x12345678,RECOMP_MESG_BLOCK,false));mark(4);
    CHECK(recomp_thread_state(rt,HIGH)==RECOMP_STOPPED);
    recomp_thread_start(rt,HIGH);mark(6);
    /* Destroy/reuse an OSThread while its pthread is parked before entry. */
    recomp_thread_create(rt,VICTIM,3,0x3000,0,0xf000,5);
    recomp_thread_destroy(rt,VICTIM);
    recomp_thread_create(rt,VICTIM,4,0x3000,0,0xf000,5);
    recomp_thread_start(rt,VICTIM);
    recomp_thread_destroy(rt,0);
    CHECK(0);
}
static const RecompFunctionEntry entries[]={
    {0x1000,4,0,high,"high"},{0x2000,4,0,low,"low"},{0x3000,4,0,victim,"victim"}
};
static void init(Runtime *rt,const RecompFunctionEntry *functions,size_t count) {
    bool loaded=true;
    CHECK(recomp_runtime_init(rt,65536,functions,count,&loaded,1));
    RecompSchedulerConfig config={.thread_tail=0xb00,.run_queue=0xb10,.active_queue=0xb14,.running_thread=0xb18};
    CHECK(recomp_scheduler_init(rt,&config));
}
static void scheduling(void) {
    Runtime rt;init(&rt,entries,3);cursor=0;
    recomp_queue_create(&rt,QUEUE,MESSAGES,1);
    recomp_thread_create(&rt,HIGH,1,0x1000,0,0xe000,20);
    recomp_thread_create(&rt,LOW,2,0x2000,0,0xd000,10);
    recomp_thread_start(&rt,LOW);recomp_thread_start(&rt,HIGH);
    CHECK(recomp_scheduler_run(&rt,1)==RECOMP_RUN_BUDGET);
    CHECK(cursor==1 && recomp_thread_state(&rt,HIGH)==RECOMP_WAIT_RECV);
    CHECK(recomp_scheduler_run(&rt,32)==RECOMP_RUN_COMPLETE);
    for(int i=0;i<6;i++)CHECK(sequence[i]==i+1);
    CHECK(cursor==6 && victims==1 && host_calls==1);
    CHECK(!pthread_equal(high_native,low_native));
    recomp_runtime_destroy(&rt);
    /* Teardown must join threads parked in a blocking OS call. */
    init(&rt,entries,3);recomp_queue_create(&rt,QUEUE,MESSAGES,1);
    recomp_thread_create(&rt,HIGH,1,0x1000,0,0xe000,20);recomp_thread_start(&rt,HIGH);
    CHECK(recomp_scheduler_run(&rt,8)==RECOMP_RUN_WAITING);recomp_runtime_destroy(&rt);
}
typedef struct Job {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool started,release;
    unsigned result;
} Job;
static void work(void *opaque) {
    Job *job=opaque;CHECK(!pthread_equal(pthread_self(),host));
    pthread_mutex_lock(&job->mutex);job->started=true;pthread_cond_signal(&job->condition);
    while(!job->release)pthread_cond_wait(&job->condition,&job->mutex);
    pthread_mutex_unlock(&job->mutex);job->result++;
}
static void workers(void) {
    Job job={.mutex=PTHREAD_MUTEX_INITIALIZER,.condition=PTHREAD_COND_INITIALIZER};
    RecompWorker *w=recomp_worker_create("test-worker");CHECK(w);
    CHECK(recomp_worker_submit(w,work,&job));
    pthread_mutex_lock(&job.mutex);
    while(!job.started)pthread_cond_wait(&job.condition,&job.mutex);
    CHECK(!recomp_worker_ready(w));CHECK(!recomp_worker_submit(w,work,&job));
    job.release=true;pthread_cond_signal(&job.condition);pthread_mutex_unlock(&job.mutex);
    recomp_worker_finish(w);CHECK(job.result==1);
    /* Repeated immediate finish/resubmit catches lost worker wakeups. */
    for(unsigned i=0;i<1000;i++) { CHECK(recomp_worker_submit(w,work,&job));recomp_worker_finish(w); }
    CHECK(job.result==1001);recomp_worker_destroy(w);
    pthread_mutex_destroy(&job.mutex);pthread_cond_destroy(&job.condition);
    Runtime rt;init(&rt,NULL,0);
    uint8_t *before=calloc(1,rt.rdram_size),*after=calloc(1,rt.rdram_size);CHECK(before && after);
    after[40]=12;rt.rdram[80]=99;
    recomp_worker_merge(&rt,before,after);
    CHECK(rt.rdram[40]==12 && rt.rdram[80]==99);
    free(before);free(after);recomp_runtime_destroy(&rt);
}
static void observe_failure(const RecompFailureInfo *info,void *opaque) {
    const char *expected=opaque;
    CHECK(pthread_equal(pthread_self(),host));CHECK(info->native_frame_count>0);
    CHECK(!strcmp(info->message,expected));_Exit(0);
}
static void fail_guest(Runtime *rt,MipsContext *ctx) { (void)rt;(void)ctx;recomp_fail("guest failure fixture",0x1234); }
static void fail_worker(void *opaque) { (void)opaque;recomp_fail("worker failure fixture",0x5678); }
static void failures(int kind) {
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        struct rlimit no_core={0,0};setrlimit(RLIMIT_CORE,&no_core);
        if(kind==0) {
            Runtime rt;RecompFunctionEntry entry={0x1000,4,0,fail_guest,"fail"};init(&rt,&entry,1);
            recomp_set_failure_handler(observe_failure,"guest failure fixture");
            recomp_thread_create(&rt,HIGH,1,0x1000,0,0xe000,20);recomp_thread_start(&rt,HIGH);
            recomp_scheduler_run(&rt,8);
        } else if(kind==1) {
            recomp_set_failure_handler(observe_failure,"worker failure fixture");
            RecompWorker *w=recomp_worker_create("test-failure");CHECK(w);
            CHECK(recomp_worker_submit(w,fail_worker,NULL));recomp_worker_finish(w);
        } else {
            Runtime rt;init(&rt,NULL,0);uint8_t *before=calloc(1,rt.rdram_size),*after=calloc(1,rt.rdram_size);
            recomp_set_failure_handler(observe_failure,"guest wrote an in-flight task output");
            after[40]=12;rt.rdram[40]=13;recomp_worker_merge(&rt,before,after);
        }
        _Exit(1);
    }
    int status;CHECK(waitpid(child,&status,0)==child);CHECK(WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(void) {
    host=pthread_self();scheduling();workers();for(int kind=0;kind<3;kind++)failures(kind);
    puts("pthread scheduling, worker handoffs, RAM merge and failure forwarding passed");
    return 0;
}
