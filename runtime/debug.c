#define _POSIX_C_SOURCE 200809L
#include "runtime_debug.h"
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_once_t settings_once = PTHREAD_ONCE_INIT;
static bool full;
static struct timespec origin;
static void settings(void) {
    const char *value = getenv("RECOMP_DEBUG_FULL");
    full = value && !strcmp(value, "1");
    (void)clock_gettime(CLOCK_MONOTONIC, &origin);
}
bool recomp_debug_sample(uint64_t count) {
    int saved_errno = errno;
    pthread_once(&settings_once, settings);
    errno = saved_errno;
    return full || count <= 8 || !(count & (count - 1));
}
void recomp_debug_log(const char *category, const char *format, ...) {
    int saved_errno = errno;
    pthread_once(&settings_once, settings);
    struct timespec now = origin;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    double seconds = (double)(now.tv_sec-origin.tv_sec) + (now.tv_nsec-origin.tv_nsec)*1e-9;
    flockfile(stderr);
    fprintf(stderr, "[debug +%.3fs][%s] ", seconds, category);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    funlockfile(stderr);
    errno = saved_errno;
}
void recomp_debug_os(Runtime *rt, const MipsContext *ctx, const char *name, uint64_t count) {
    int32_t id = 0;
    uint32_t object = 0;
    bool guest = recomp_debug_current_thread(rt, &id, &object);
    recomp_debug_log("os", "%s count=%" PRIu64 " caller=%s id=%" PRId32
        " thread=0x%08" PRIx32 " pc=0x%08" PRIx32 " ra=0x%08" PRIx32
        " a0..a3=[0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 "]",
        name, count, guest ? "guest" : "host", id, object, ctx->pc,
        (uint32_t)ctx->gpr[31], (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5],
        (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);
}
