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
bool recomp_debug_trace_calls;
static bool full;
static uint32_t watched_queue;
static struct timespec origin;
static void settings(void) {
    const char *value = getenv("RECOMP_DEBUG_FULL");
    full = value && !strcmp(value, "1");
    value = getenv("RECOMP_DEBUG_QUEUE");
    if (value)
        watched_queue = (uint32_t)strtoul(value, NULL, 0);
    (void)clock_gettime(CLOCK_MONOTONIC, &origin);
}
bool recomp_debug_watch_queue(uint32_t object) {
    int saved_errno = errno;
    pthread_once(&settings_once, settings);
    errno = saved_errno;
    return watched_queue && ((watched_queue ^ object) & 0x1fffffff) == 0;
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
    double seconds = (double)(now.tv_sec - origin.tv_sec) + (now.tv_nsec - origin.tv_nsec) * 1e-9;
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
void recomp_debug_trace_call(uint32_t address) {
    recomp_debug_log("call", "0x%08x", address);
}
void recomp_debug_os(Runtime *rt, const MipsContext *ctx, const char *name, uint64_t count) {
    int32_t id = 0;
    uint32_t object = 0;
    bool guest = recomp_debug_current_thread(rt, &id, &object);
    recomp_debug_log("os",
                     "%s count=%" PRIu64 " caller=%s id=%" PRId32 " thread=0x%08" PRIx32 " pc=0x%08" PRIx32
                     " ra=0x%08" PRIx32 " a0..a3=[0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 ",0x%08" PRIx32 "]",
                     name, count, guest ? "guest" : "host", id, object, ctx->pc, (uint32_t)ctx->gpr[31],
                     (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7]);
}

/* Game diagnostics run on the serialized guest execution thread. Keep live
 * module Runtime layout stable. Flush each distinct edge before dispatch can
 * block or fail, so an interrupted gameplay run still yields a usable trace. */
typedef struct TraceEdge {
    struct TraceEdge *next;
    uint32_t section, pc, target_section, target;
    enum RecompEdgeKind kind;
} TraceEdge;

Runtime *recomp_edge_owner;
static FILE *edge_file;
static TraceEdge **edge_buckets;
enum { EDGE_BUCKETS = 4096 };
static const RecompCodeRegion *edge_sections;

void recomp_debug_open_edges(Runtime *rt, const char *path, const uint8_t sha1[20], const char *layout,
                             const RecompCodeRegion *sections, size_t count) {
    if (recomp_edge_owner || count != rt->section_count)
        recomp_fail("invalid transfer trace session", 0);
    edge_file = fopen(path, "wx");
    if (!edge_file)
        recomp_fail("cannot create new transfer trace file", 0);
    edge_buckets = calloc(EDGE_BUCKETS, sizeof(*edge_buckets));
    if (!edge_buckets)
        recomp_fail("cannot allocate transfer trace index", 0);
    edge_sections = sections;
    fputs("# v64recomp-edges-v1 ", edge_file);
    for (unsigned i = 0; i < 20; ++i)
        fprintf(edge_file, "%02x", sha1[i]);
    fprintf(edge_file, " %s\nsection,pc,kind,target_section,target\n", layout);
    if (fflush(edge_file) || ferror(edge_file))
        recomp_fail("cannot write transfer trace header", 0);
    recomp_edge_owner = rt;
}

void recomp_debug_edge(Runtime *rt, uint32_t section, uint32_t pc, uint32_t destination, enum RecompEdgeKind kind) {
    if (rt != recomp_edge_owner)
        return;
    uint32_t target = destination;
    uint32_t target_section = UINT32_MAX;
    for (size_t i = 0; i < rt->section_count; ++i) {
        if (!rt->section_loaded[i])
            continue;
        /* The canonical lookup hook can activate a new overlay. Observing a
         * transfer must never run it. Inspect only already loaded mappings. */
        uint32_t base = recomp_relocated_address(rt, (uint32_t)i, edge_sections[i].vram);
        uint32_t offset = destination - base;
        if (offset >= edge_sections[i].size)
            offset = destination - edge_sections[i].vram;
        if (offset < edge_sections[i].size) {
            if (target_section != UINT32_MAX) {
                target_section = UINT32_MAX;
                target = destination;
                break;
            }
            target_section = (uint32_t)i;
            target = edge_sections[i].vram + offset;
        }
    }
    size_t slot = ((pc >> 2) * 31u + (target >> 2) * 17u + section * 11u + target_section + kind) % EDGE_BUCKETS;
    for (TraceEdge *e = edge_buckets[slot]; e; e = e->next)
        if (e->section == section && e->pc == pc && e->target_section == target_section && e->target == target &&
            e->kind == kind)
            return;
    TraceEdge *entry = malloc(sizeof(*entry));
    if (!entry)
        recomp_fail("cannot allocate transfer trace edge", pc);
    *entry = (TraceEdge){edge_buckets[slot], section, pc, target_section, target, kind};
    edge_buckets[slot] = entry;
    static const char *const names[] = {"call", "jump", "tail", "return", "relocated-call"};
    fprintf(edge_file, "%u,0x%08x,%s,", section, pc, names[kind]);
    if (target_section != UINT32_MAX)
        fprintf(edge_file, "%u", target_section);
    if (fprintf(edge_file, ",0x%08x\n", target) < 0 || fflush(edge_file) || ferror(edge_file))
        recomp_fail("cannot write transfer trace edge", pc);
}

void recomp_debug_close_edges(Runtime *rt) {
    if (rt != recomp_edge_owner)
        return;
    recomp_edge_owner = NULL;
    for (size_t i = 0; i < EDGE_BUCKETS; ++i) {
        TraceEdge *e = edge_buckets[i];
        while (e) {
            TraceEdge *next = e->next;
            free(e);
            e = next;
        }
    }
    free(edge_buckets);
    if (fclose(edge_file))
        recomp_fail("cannot close transfer trace", 0);
}
