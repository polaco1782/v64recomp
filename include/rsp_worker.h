#ifndef RECOMP_RSP_WORKER_H
#define RECOMP_RSP_WORKER_H
#include "rsp.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompRspWorker RecompRspWorker;
/* One hardware SP task may be active. Submit snapshots RAM and the descriptor;
 * the owner exclusively holds each completed result until releasing it. No worker API uses
 * Runtime, GL, SDL, guest dispatch or task-completion callbacks. */
RecompRspWorker *recomp_rsp_worker_create(size_t ram_size);
bool recomp_rsp_worker_submit(RecompRspWorker *, const RecompRspProgram *, const uint32_t task[16], const uint8_t *ram);
const RecompRspState *recomp_rsp_worker_result(RecompRspWorker *, bool wait);
void recomp_rsp_worker_release(RecompRspWorker *);
void recomp_rsp_worker_destroy(RecompRspWorker *);
#ifdef __cplusplus
}
#endif
#endif
