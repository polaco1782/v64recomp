#define _GNU_SOURCE 1
#include "runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct RecompWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    void (*job)(void *);
    void *userdata;
    bool busy, done, stopping, failed, joined;
    RecompFailureInfo failure;
    char failure_message[512];
    char name[16];
};
static void failed(const RecompFailureInfo *info, void *opaque) {
    RecompWorker *w=opaque;
    pthread_mutex_lock(&w->mutex);
    w->failure=*info;
    snprintf(w->failure_message,sizeof(w->failure_message),"%s",info->message);
    w->failure.message=w->failure_message;w->failed=true;w->done=true;
    pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->mutex);
    pthread_exit(NULL);
}
static void *run(void *opaque) {
    RecompWorker *w=opaque;
    pthread_setname_np(pthread_self(),w->name);
    recomp_set_failure_handler(failed,w);
    pthread_mutex_lock(&w->mutex);
    for (;;) {
        while (!w->job && !w->stopping) pthread_cond_wait(&w->changed,&w->mutex);
        if (w->stopping) break;
        void (*job)(void *)=w->job;void *userdata=w->userdata;
        w->job=NULL;
        pthread_mutex_unlock(&w->mutex);
        job(userdata);
        pthread_mutex_lock(&w->mutex);
        w->done=true;
        pthread_cond_broadcast(&w->changed);
    }
    pthread_mutex_unlock(&w->mutex);
    return NULL;
}
RecompWorker *recomp_worker_create(const char *name) {
    RecompWorker *w=calloc(1,sizeof(*w));if(!w)return NULL;
    snprintf(w->name,sizeof(w->name),"%s",name?name:"n64-worker");
    if(pthread_mutex_init(&w->mutex,NULL)) { free(w);return NULL; }
    if(pthread_cond_init(&w->changed,NULL)) { pthread_mutex_destroy(&w->mutex);free(w);return NULL; }
    if(pthread_create(&w->thread,NULL,run,w)) {
        pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->mutex);free(w);return NULL;
    }
    return w;
}
bool recomp_worker_submit(RecompWorker *w,void (*job)(void *),void *userdata) {
    if(!job)return false;
    pthread_mutex_lock(&w->mutex);
    bool okay=!w->busy && !w->stopping && !w->joined;
    if(okay) { w->job=job;w->userdata=userdata;w->busy=true;w->done=false;pthread_cond_broadcast(&w->changed); }
    pthread_mutex_unlock(&w->mutex);return okay;
}
bool recomp_worker_ready(RecompWorker *w) {
    pthread_mutex_lock(&w->mutex);bool ready=w->busy && w->done;
    pthread_mutex_unlock(&w->mutex);return ready;
}
void recomp_worker_finish(RecompWorker *w) {
    pthread_mutex_lock(&w->mutex);
    while(w->busy && !w->done) pthread_cond_wait(&w->changed,&w->mutex);
    bool has_failed=w->failed;RecompFailureInfo info=w->failure;
    w->failed=false;
    w->busy=false;pthread_cond_broadcast(&w->changed);
    pthread_mutex_unlock(&w->mutex);
    if(has_failed) {
        /* pthread_exit may still be unwinding C++ job frames. Join before
         * allowing diagnostic observers to inspect their shared job data. */
        if(pthread_join(w->thread,NULL))recomp_fail("failed worker join failed",0);
        w->joined=true;
        recomp_dispatch_failure(&info);
    }
}
void recomp_worker_destroy(RecompWorker *w) {
    if(!w)return;
    recomp_worker_finish(w);
    pthread_mutex_lock(&w->mutex);w->stopping=true;pthread_cond_broadcast(&w->changed);pthread_mutex_unlock(&w->mutex);
    if(!w->joined && pthread_join(w->thread,NULL))recomp_fail("processing worker join failed",0);
    pthread_cond_destroy(&w->changed);pthread_mutex_destroy(&w->mutex);free(w);
}
void recomp_worker_merge(Runtime *rt,const uint8_t *before,const uint8_t *after) {
    /* Compare words first: most of the RAM snapshot is read-only input. */
    for(size_t i=0;i<rt->rdram_size;i+=4) {
        if(!memcmp(before+i,after+i,4))continue;
        for(size_t j=i;j<i+4;j++)if(before[j]!=after[j]) {
            if(rt->rdram[j]!=before[j] && rt->rdram[j]!=after[j])
                recomp_fail("guest wrote an in-flight task output",(uint32_t)(j^3));
            rt->rdram[j]=after[j];
        }
    }
}
