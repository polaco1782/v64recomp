#include "rsp_worker.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

struct RecompRspWorker {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool requested, running, ready, stop;
    uint32_t task[16];
    RecompRspState state;
};
static void *worker_main(void *opaque) {
    RecompRspWorker *worker = opaque;
    pthread_mutex_lock(&worker->mutex);
    for (;;) {
        while (!worker->requested && !worker->stop)
            pthread_cond_wait(&worker->condition, &worker->mutex);
        if (worker->stop)
            break;
        worker->requested = false;
        worker->running = true;
        pthread_mutex_unlock(&worker->mutex);
        (void)recomp_rsp_start(&worker->state, worker->task);
        pthread_mutex_lock(&worker->mutex);
        worker->running = false;
        worker->ready = true;
        pthread_cond_broadcast(&worker->condition);
    }
    pthread_mutex_unlock(&worker->mutex);
    return NULL;
}
RecompRspWorker *recomp_rsp_worker_create(size_t size) {
    if (!size || size > 16 * 1024 * 1024 || size % 4)
        return NULL;
    RecompRspWorker *worker = calloc(1, sizeof(*worker));
    if (!worker)
        return NULL;
    uint8_t *ram = malloc(size);
    if (!ram) {
        free(worker);
        return NULL;
    }
    recomp_rsp_init(&worker->state, ram, size, NULL);
    if (pthread_mutex_init(&worker->mutex, NULL))
        goto fail;
    if (pthread_cond_init(&worker->condition, NULL)) {
        pthread_mutex_destroy(&worker->mutex);
        goto fail;
    }
    if (pthread_create(&worker->thread, NULL, worker_main, worker)) {
        pthread_cond_destroy(&worker->condition);
        pthread_mutex_destroy(&worker->mutex);
        goto fail;
    }
    return worker;
fail:
    free(ram);
    free(worker);
    return NULL;
}
bool recomp_rsp_worker_submit(RecompRspWorker *worker, const RecompRspProgram *program, const uint32_t task[16],
                              const uint8_t *ram) {
    if (!worker || !program || !task || !ram)
        return false;
    pthread_mutex_lock(&worker->mutex);
    bool available = !worker->requested && !worker->running && !worker->ready && !worker->stop;
    if (available) {
        memcpy(worker->state.ram, ram, worker->state.ram_size);
        memcpy(worker->task, task, sizeof(worker->task));
        worker->state.program = program;
        worker->requested = true;
        pthread_cond_signal(&worker->condition);
    }
    pthread_mutex_unlock(&worker->mutex);
    return available;
}
const RecompRspState *recomp_rsp_worker_result(RecompRspWorker *worker, bool wait) {
    if (!worker)
        return NULL;
    pthread_mutex_lock(&worker->mutex);
    while (wait && !worker->ready && (worker->requested || worker->running))
        pthread_cond_wait(&worker->condition, &worker->mutex);
    const RecompRspState *result = worker->ready ? &worker->state : NULL;
    pthread_mutex_unlock(&worker->mutex);
    return result;
}
void recomp_rsp_worker_release(RecompRspWorker *worker) {
    pthread_mutex_lock(&worker->mutex);
    worker->ready = false;
    pthread_mutex_unlock(&worker->mutex);
}
void recomp_rsp_worker_destroy(RecompRspWorker *worker) {
    if (!worker)
        return;
    pthread_mutex_lock(&worker->mutex);
    worker->stop = true;
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->mutex);
    pthread_join(worker->thread, NULL);
    pthread_cond_destroy(&worker->condition);
    pthread_mutex_destroy(&worker->mutex);
    free(worker->state.ram);
    recomp_rsp_destroy(&worker->state);
    free(worker);
}
