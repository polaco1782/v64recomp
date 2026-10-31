#define _POSIX_C_SOURCE 200809L
#include "timing.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

struct RecompTiming {
    FILE *file;
    uint64_t origin, previous_wall, previous_cpu;
    uint64_t ns[RECOMP_TIMING_PHASES], count[RECOMP_TIMING_PHASES];
    unsigned logical_cpus;
    bool cpu_valid;
};

static const char *phase_names[RECOMP_TIMING_PHASES] = {
    "dispatch", "checkpoint", "context_sync", "guest_wait", "host_callback",
    "graphics", "present", "audio", "sleep", "display",
    "graphics_cpu"
};

uint64_t recomp_timing_clock(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now))
        recomp_fail("timing clock failed", 0);
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}

uint64_t recomp_timing_thread_clock(void) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        return 0;
    uint64_t ticks = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ticks += ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    return ticks * 100;
#elif defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec cpu;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu))
        return 0;
    return (uint64_t)cpu.tv_sec * UINT64_C(1000000000) + (uint64_t)cpu.tv_nsec;
#else
    return 0;
#endif
}

static bool process_cpu(uint64_t *ns) {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
        return false;
    uint64_t ticks = ((uint64_t)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
    ticks += ((uint64_t)user.dwHighDateTime << 32) | user.dwLowDateTime;
    *ns = ticks * 100;
#else
    struct timespec cpu;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu))
        return false;
    *ns = (uint64_t)cpu.tv_sec * UINT64_C(1000000000) + (uint64_t)cpu.tv_nsec;
#endif
    return true;
}

bool recomp_timing_init(Runtime *rt) {
    const char *path = getenv("RECOMP_TIMING");
    if (!path || !*path)
        return true;
    struct RecompTiming *t = calloc(1, sizeof(*t));
    if (!t)
        return false;
    /* Existing reports are never replaced. Choose a fresh path per run. */
    t->file = fopen(path, "wx");
    if (!t->file) {
        fprintf(stderr, "Timing: cannot create %s; use a new file in an existing directory\n", path);
        free(t);
        return false;
    }
#ifdef _WIN32
    t->logical_cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#else
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    t->logical_cpus = cpus > 0 ? (unsigned)cpus : 0;
#endif
    t->origin = t->previous_wall = recomp_timing_clock();
    t->cpu_valid = process_cpu(&t->previous_cpu);
    fputs("elapsed_s,window_ms,logical_cpus,cpu_cores", t->file);
    for (unsigned i = 0; i < RECOMP_TIMING_PHASES; i++)
        fprintf(t->file, ",%s_count,%s_ms", phase_names[i], phase_names[i]);
    fputc('\n', t->file);
    if (fflush(t->file) || ferror(t->file)) {
        fclose(t->file);
        free(t);
        return false;
    }
    rt->timing = t;
    fprintf(stderr, "Timing: writing one-second windows to %s (cpu_cores=1 means one busy logical CPU)\n", path);
    return true;
}

void recomp_timing_record(Runtime *rt, RecompTimingPhase phase, uint64_t ns, uint64_t count) {
    rt->timing->ns[phase] += ns;
    rt->timing->count[phase] += count;
}

void recomp_timing_report(Runtime *rt, bool final) {
    struct RecompTiming *t = rt->timing;
    if (!t || !t->file)
        return;
    uint64_t now = recomp_timing_clock(), elapsed = now - t->previous_wall;
    if (!elapsed || (!final && elapsed < UINT64_C(1000000000)))
        return;
    uint64_t cpu = 0;
    bool valid = process_cpu(&cpu);
    double cores = valid && t->cpu_valid && cpu >= t->previous_cpu
                       ? (double)(cpu - t->previous_cpu) / (double)elapsed : -1;
    fprintf(t->file, "%.6f,%.3f,%u,%.4f", (now - t->origin) / 1e9, elapsed / 1e6,
            t->logical_cpus, cores);
    for (unsigned i = 0; i < RECOMP_TIMING_PHASES; i++) {
        fprintf(t->file, ",%" PRIu64 ",%.3f", t->count[i], t->ns[i] / 1e6);
        t->ns[i] = t->count[i] = 0;
    }
    fputc('\n', t->file);
    if (fflush(t->file) || ferror(t->file)) {
        fputs("Timing: report write failed; recording stopped\n", stderr);
        fclose(t->file);
        t->file = NULL;
    }
    t->previous_wall = now;
    t->previous_cpu = cpu;
    t->cpu_valid = valid;
}

void recomp_timing_destroy(Runtime *rt) {
    if (!rt->timing)
        return;
    recomp_timing_report(rt, true);
    if (rt->timing->file && fclose(rt->timing->file))
        fputs("Timing: report close failed\n", stderr);
    free(rt->timing);
    rt->timing = NULL;
}
