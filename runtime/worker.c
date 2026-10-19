#define _GNU_SOURCE 1
#include "runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#if USE_INTRISICS && defined(__SSE2__)
#include <emmintrin.h>
#endif

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
void recomp_worker_snapshot(Runtime *rt,uint8_t *before,uint8_t *after) {
    size_t i=0;
#if USE_INTRISICS && defined(__SSE2__)
    /* Load each guest byte once and feed both private buffers. Guest execution
     * is parked on the owner while the snapshot is taken. */
    for(;rt->rdram_size-i>=16;i+=16) {
        __m128i bytes=_mm_loadu_si128((const __m128i *)(rt->rdram+i));
        _mm_storeu_si128((__m128i *)(before+i),bytes);
        _mm_storeu_si128((__m128i *)(after+i),bytes);
    }
#endif
    memcpy(before+i,rt->rdram+i,rt->rdram_size-i);
    memcpy(after+i,before+i,rt->rdram_size-i);
}
static bool block_equal(const uint8_t *before,const uint8_t *after) {
#if USE_INTRISICS && defined(__SSE2__)
    __m128i equal=_mm_set1_epi8(-1);
    for(size_t i=0;i<64;i+=16) {
        __m128i a=_mm_loadu_si128((const __m128i *)(before+i));
        __m128i b=_mm_loadu_si128((const __m128i *)(after+i));
        equal=_mm_and_si128(equal,_mm_cmpeq_epi8(a,b));
    }
    return _mm_movemask_epi8(equal)==0xffff;
#else
    return !memcmp(before,after,64);
#endif
}
void recomp_worker_merge(Runtime *rt,const uint8_t *before,const uint8_t *after) {
    /* Most input RAM is unchanged. Skip whole cache lines, then retain the
     * original byte-level conflict checks and ascending write/failure order. */
    for(size_t i=0;i<rt->rdram_size;) {
        size_t count=rt->rdram_size-i;
        if(count>=64) {
            count=64;
            if(block_equal(before+i,after+i)) { i+=64;continue; }
        }
        for(size_t j=i;j<i+count;j++)if(before[j]!=after[j]) {
            if(rt->rdram[j]!=before[j] && rt->rdram[j]!=after[j])
                recomp_fail("guest wrote an in-flight task output",(uint32_t)(j^3));
            rt->rdram[j]=after[j];
        }
        i+=count;
    }
}
