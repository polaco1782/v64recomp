#define _GNU_SOURCE
#include "profiler.h"
#include <stdio.h>

#if defined(__linux__) && defined(__x86_64__)
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

/* Available when the game links libstdc++; keep the shared runtime C-only. */
extern char *__cxa_demangle(const char *, char *, size_t *, int *) __attribute__((weak));

/* A CPU timer delivers to its own target thread. The handler only copies the
 * interrupted registers/stack into a bounded single-producer ring. In particular
 * it never calls malloc, dladdr, stdio, locks, or an unwinder. Reading a stale or
 * invalid frame through the kernel returns an error instead of crashing the game.
 * Slots are intentionally kept until process exit: deleted timers may still
 * have a queued signal containing their slot pointer. */
enum {
    DEPTH = 32,
    RING = 256,
    MAX_THREADS = 1024,
    STACK_BUCKETS = 16384,
    MAX_STACKS = 65536,
    SYMBOL_BUCKETS = 8192,
    MAX_SYMBOLS = 65536
};
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal handler requires lock-free counters");
typedef struct Sample {
    unsigned depth, missed;
    uintptr_t pc[DEPTH];
} Sample;
typedef struct ThreadStat {
    uint64_t start_ticks, user, system;
    char name[64];
} ThreadStat;
typedef struct Thread {
    struct Thread *next;
    int tid, timer_error;
    uint64_t start_ticks, user_base, system_base, user, system;
    char name[64];
    bool seen, timer_live;
    timer_t timer;
    _Atomic unsigned head, tail, dropped;
    uint64_t samples, overruns;
    Sample ring[RING];
} Thread;
typedef struct Symbol {
    struct Symbol *next;
    uintptr_t address, module_base;
    uint64_t self, inclusive;
    char *label, *module;
} Symbol;
typedef struct Stack {
    struct Stack *next;
    Thread *thread;
    uint64_t count;
    unsigned depth;
    Symbol *symbols[DEPTH];
} Stack;
typedef struct Module {
    struct Module *next;
    uintptr_t base;
    char *path;
    void *mapping;
    size_t bytes, count;
    Elf64_Sym *functions;
    const char *strings;
    size_t string_bytes;
    bool relative;
} Module;
static struct {
    pthread_t collector;
    _Atomic bool stopping;
    bool started, sampling, report_error;
    int signal, mem_fd, collector_tid;
    unsigned hz, delay, duration, thread_count, stack_count, symbol_count;
    uint64_t start_ns, elapsed_ns, discarded_stacks, allocation_failures, shallow_samples, depth_limit_samples,
        thread_limit_hits;
    long ticks;
    char *directory;
    Thread *threads;
    Module *modules;
    Stack *stacks[STACK_BUCKETS];
    Symbol *symbols[SYMBOL_BUCKETS];
    struct rusage usage;
    double user_seconds, system_seconds;
} profile;

static uint64_t monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
static void sample_signal(int signo, siginfo_t *info, void *context) {
    int saved_errno = errno;
    if (signo != profile.signal || info->si_code != SI_TIMER)
        goto done;
    Thread *t = info->si_value.sival_ptr;
    unsigned head = atomic_load_explicit(&t->head, memory_order_relaxed);
    if (head - atomic_load_explicit(&t->tail, memory_order_acquire) >= RING) {
        atomic_fetch_add_explicit(&t->dropped, 1, memory_order_relaxed);
        goto done;
    }
    Sample *s = &t->ring[head % RING];
    ucontext_t *uc = context;
    uintptr_t sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
    s->depth = 1;
    s->pc[0] = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    s->missed = info->si_overrun > 0 ? (unsigned)info->si_overrun : 0;
    while (s->depth < DEPTH && fp >= sp && fp - sp < 8 * 1024 * 1024 && !(fp % sizeof(uintptr_t))) {
        uintptr_t frame[2];
        if (syscall(SYS_pread64, profile.mem_fd, frame, sizeof(frame), (off_t)fp) != (long)sizeof(frame))
            break;
        if (!frame[1])
            break;
        /* Return addresses point after the call, possibly at a symbol boundary. */
        s->pc[s->depth++] = frame[1] - 1;
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    atomic_store_explicit(&t->head, head + 1, memory_order_release);
done:
    errno = saved_errno;
}
static FILE *output(const char *name) {
    char path[4096];
    if (snprintf(path, sizeof(path), "%s/%s", profile.directory, name) >= (int)sizeof(path))
        return NULL;
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Profiler: cannot write %s: %s\n", path, strerror(errno));
        profile.report_error = true;
    }
    return f;
}
static void finish_file(FILE *f) {
    bool bad = ferror(f) != 0;
    if (fclose(f))
        bad = true;
    if (bad) {
        profile.report_error = true;
        fprintf(stderr, "Profiler: report write failed\n");
    }
}
static void quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            fputc('\\', f);
        if (*p < 32)
            fprintf(f, "\\u%04x", *p);
        else
            fputc(*p, f);
    }
    fputc('"', f);
}
static void copy_proc(const char *source, const char *name) {
    FILE *in = fopen(source, "r");
    if (!in)
        return;
    FILE *out = output(name);
    if (out) {
        char data[4096];
        size_t n;
        while ((n = fread(data, 1, sizeof(data), in)))
            if (fwrite(data, 1, n, out) != n)
                break;
        finish_file(out);
    }
    fclose(in);
}
static bool thread_stat(int tid, ThreadStat *row) {
    char path[128], buffer[4096];
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool okay = fgets(buffer, sizeof(buffer), f) != NULL;
    fclose(f);
    if (!okay)
        return false;
    char *begin = strchr(buffer, '('), *end = strrchr(buffer, ')');
    if (!begin || !end || end <= begin)
        return false;
    snprintf(row->name, sizeof(row->name), "%.*s", (int)(end - begin - 1), begin + 1);
    char *save = NULL, *token = strtok_r(end + 2, " ", &save);
    for (unsigned field = 3; token; field++, token = strtok_r(NULL, " ", &save)) {
        if (field == 14)
            row->user = strtoull(token, NULL, 10);
        if (field == 15)
            row->system = strtoull(token, NULL, 10);
        if (field == 22) {
            row->start_ticks = strtoull(token, NULL, 10);
            return true;
        }
    }
    return false;
}
static int elf_address(const void *a, const void *b) {
    const Elf64_Sym *x = a, *y = b;
    return x->st_value < y->st_value ? -1 : (x->st_value > y->st_value ? 1 : 0);
}
static bool file_range(size_t bytes, uint64_t offset, uint64_t length) {
    return offset <= bytes && length <= bytes - offset;
}
/* dladdr only sees dynamic exports. Read native ELF function symbols as well,
 * so static runtime helpers and RT64's hidden C++ functions retain their names.
 * This runs exclusively on the collector, never in a sampling signal. */
static const char *elf_function(const Dl_info *info, uintptr_t pc, uintptr_t *local_address) {
    if (!info->dli_fname)
        return NULL;
    uintptr_t base = (uintptr_t)info->dli_fbase;
    Module *m = profile.modules;
    while (m && !(m->base == base && !strcmp(m->path, info->dli_fname)))
        m = m->next;
    if (!m) {
        m = calloc(1, sizeof(*m));
        if (!m)
            return NULL;
        m->path = strdup(info->dli_fname);
        if (!m->path) {
            free(m);
            return NULL;
        }
        m->base = base;
        m->next = profile.modules;
        profile.modules = m;
        int fd = open(m->path, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0)
            return NULL;
        if (fstat(fd, &st) || st.st_size < (off_t)sizeof(Elf64_Ehdr)) {
            close(fd);
            return NULL;
        }
        m->bytes = (size_t)st.st_size;
        m->mapping = mmap(NULL, m->bytes, PROT_READ, MAP_PRIVATE, fd, 0);
        close(fd);
        if (m->mapping == MAP_FAILED) {
            m->mapping = NULL;
            return NULL;
        }
        const Elf64_Ehdr *e = m->mapping;
        if (memcmp(e->e_ident, ELFMAG, SELFMAG) || e->e_ident[EI_CLASS] != ELFCLASS64 ||
            e->e_ident[EI_DATA] != ELFDATA2LSB || e->e_machine != EM_X86_64 ||
            (e->e_type != ET_DYN && e->e_type != ET_EXEC) || e->e_shentsize != sizeof(Elf64_Shdr) ||
            !file_range(m->bytes, e->e_shoff, (uint64_t)e->e_shnum * sizeof(Elf64_Shdr)))
            return NULL;
        m->relative = e->e_type == ET_DYN;
        const Elf64_Shdr *sections = (const void *)((const char *)m->mapping + e->e_shoff);
        for (unsigned i = 0; i < e->e_shnum; i++) {
            const Elf64_Shdr *s = &sections[i];
            if (s->sh_type != SHT_SYMTAB || s->sh_entsize != sizeof(Elf64_Sym) || s->sh_link >= e->e_shnum ||
                !file_range(m->bytes, s->sh_offset, s->sh_size))
                continue;
            const Elf64_Shdr *strings = &sections[s->sh_link];
            if (strings->sh_type != SHT_STRTAB || !file_range(m->bytes, strings->sh_offset, strings->sh_size))
                continue;
            const Elf64_Sym *symbols = (const void *)((const char *)m->mapping + s->sh_offset);
            size_t count = s->sh_size / sizeof(*symbols);
            m->functions = calloc(count, sizeof(*m->functions));
            if (!m->functions)
                return NULL;
            m->strings = (const char *)m->mapping + strings->sh_offset;
            m->string_bytes = strings->sh_size;
            for (size_t j = 0; j < count; j++) {
                unsigned type = ELF64_ST_TYPE(symbols[j].st_info);
                if ((type == STT_FUNC || type == STT_GNU_IFUNC) && symbols[j].st_shndx != SHN_UNDEF &&
                    symbols[j].st_size && symbols[j].st_name < m->string_bytes)
                    m->functions[m->count++] = symbols[j];
            }
            qsort(m->functions, m->count, sizeof(*m->functions), elf_address);
            break;
        }
    }
    uintptr_t address = m->relative ? pc - base : pc;
    size_t low = 0, high = m->count;
    while (low < high) {
        size_t mid = low + (high - low) / 2;
        if (m->functions[mid].st_value <= address)
            low = mid + 1;
        else
            high = mid;
    }
    if (!low)
        return NULL;
    const Elf64_Sym *s = &m->functions[low - 1];
    if (address - s->st_value >= s->st_size || !memchr(m->strings + s->st_name, 0, m->string_bytes - s->st_name))
        return NULL;
    if (ELF64_ST_BIND(s->st_info) == STB_LOCAL)
        *local_address = s->st_value;
    return m->strings + s->st_name;
}
static Symbol *symbol(uintptr_t address) {
    unsigned bucket = (unsigned)(address ^ (address >> 16)) % SYMBOL_BUCKETS;
    for (Symbol *s = profile.symbols[bucket]; s; s = s->next)
        if (s->address == address)
            return s;
    if (profile.symbol_count >= MAX_SYMBOLS)
        return NULL;
    Symbol *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    Dl_info d = {0};
    dladdr((void *)address, &d);
    uintptr_t local_address = 0;
    const char *name = d.dli_sname ? d.dli_sname : elf_function(&d, address, &local_address);
    char label[1024];
    const char *module = d.dli_fname ? strrchr(d.dli_fname, '/') : NULL;
    module = module ? module + 1 : (d.dli_fname ? d.dli_fname : "unknown");
    char *demangled = NULL;
    if (name && __cxa_demangle && !strncmp(name, "_Z", 2))
        demangled = __cxa_demangle(name, NULL, NULL, NULL);
    if (name && local_address)
        snprintf(label, sizeof(label), "%s!%s@0x%" PRIxPTR, module, demangled ? demangled : name, local_address);
    else if (name)
        snprintf(label, sizeof(label), "%s!%s", module, demangled ? demangled : name);
    else
        snprintf(label, sizeof(label), "%s+0x%" PRIxPTR, module, address - (uintptr_t)d.dli_fbase);
    free(demangled);
    /* Folded-stack separators and line breaks must not come from ELF names. */
    for (char *p = label; *p; p++)
        if (*p == ';' || *p == '\n' || *p == '\r')
            *p = '_';
    s->label = strdup(label);
    s->module = strdup(d.dli_fname ? d.dli_fname : "");
    if (!s->label || !s->module) {
        free(s->label);
        free(s->module);
        free(s);
        return NULL;
    }
    s->module_base = (uintptr_t)d.dli_fbase;
    s->address = address;
    s->next = profile.symbols[bucket];
    profile.symbols[bucket] = s;
    profile.symbol_count++;
    return s;
}
static void consume(Thread *t) {
    unsigned tail = atomic_load_explicit(&t->tail, memory_order_relaxed);
    unsigned head = atomic_load_explicit(&t->head, memory_order_acquire);
    while (tail != head) {
        Sample *s = &t->ring[tail % RING];
        Symbol *frames[DEPTH];
        unsigned depth = 0;
        uint64_t hash = (unsigned)t->tid;
        for (unsigned i = 0; i < s->depth; i++) {
            Symbol *entry = symbol(s->pc[i]);
            if (!entry) {
                profile.allocation_failures++;
                break;
            }
            frames[depth++] = entry;
            hash = (hash ^ entry->address) * UINT64_C(1099511628211);
        }
        t->samples++;
        t->overruns += s->missed;
        if (s->depth == 1)
            profile.shallow_samples++;
        if (s->depth == DEPTH)
            profile.depth_limit_samples++;
        if (depth) {
            frames[0]->self++;
            for (unsigned i = 0; i < depth; i++) {
                bool duplicate = false;
                for (unsigned j = 0; j < i; j++)
                    if (!strcmp(frames[j]->label, frames[i]->label))
                        duplicate = true;
                if (!duplicate)
                    frames[i]->inclusive++;
            }
            unsigned bucket = hash % STACK_BUCKETS;
            Stack *stack = profile.stacks[bucket];
            while (stack && !(stack->thread == t && stack->depth == depth &&
                              !memcmp(stack->symbols, frames, depth * sizeof(*frames))))
                stack = stack->next;
            if (!stack && profile.stack_count < MAX_STACKS) {
                stack = calloc(1, sizeof(*stack));
                if (stack) {
                    stack->thread = t;
                    stack->depth = depth;
                    memcpy(stack->symbols, frames, depth * sizeof(*frames));
                    stack->next = profile.stacks[bucket];
                    profile.stacks[bucket] = stack;
                    profile.stack_count++;
                }
            }
            if (stack)
                stack->count++;
            else
                profile.discarded_stacks++;
        }
        atomic_store_explicit(&t->tail, ++tail, memory_order_release);
    }
}
static void arm(Thread *t) {
    if (t->tid == profile.collector_tid)
        return;
    struct sigevent event = {
        .sigev_notify = SIGEV_THREAD_ID, .sigev_signo = profile.signal, .sigev_value.sival_ptr = t};
    event._sigev_un._tid = t->tid;
    /* Linux make_thread_cpuclock(tid, CPUCLOCK_SCHED): per-thread = 4,
     * scheduler CPU clock = 2. Unsigned shifting avoids signed-shift UB. */
    clockid_t clock = (clockid_t)(((~(unsigned)t->tid) << 3) | 6u);
    if (timer_create(clock, &event, &t->timer)) {
        t->timer_error = errno;
        return;
    }
    t->timer_live = true;
    uint64_t period = 1000000000 / profile.hz;
    struct itimerspec interval = {.it_interval = {0, (long)period}, .it_value = {0, (long)period}};
    /* hz=1 has a full-second interval, rather than an invalid tv_nsec. */
    if (period == 1000000000)
        interval.it_interval = interval.it_value = (struct timespec){1, 0};
    if (timer_settime(t->timer, 0, &interval, NULL)) {
        t->timer_error = errno;
        timer_delete(t->timer);
        t->timer_live = false;
    }
}
static void scan(bool baseline) {
    DIR *dir = opendir("/proc/self/task");
    if (!dir)
        return;
    for (Thread *t = profile.threads; t; t = t->next)
        t->seen = false;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long tid = strtol(entry->d_name, &end, 10);
        if (*end || tid <= 0)
            continue;
        ThreadStat row = {0};
        if (!thread_stat((int)tid, &row))
            continue;
        Thread *t = profile.threads;
        while (t && !(t->tid == tid && t->start_ticks == row.start_ticks))
            t = t->next;
        if (!t) {
            if (profile.thread_count >= MAX_THREADS) {
                profile.thread_limit_hits++;
                continue;
            }
            t = calloc(1, sizeof(*t));
            if (!t) {
                profile.allocation_failures++;
                continue;
            }
            t->tid = (int)tid;
            t->start_ticks = row.start_ticks;
            if (baseline) {
                t->user_base = row.user;
                t->system_base = row.system;
            }
            t->next = profile.threads;
            profile.threads = t;
            profile.thread_count++;
            if (profile.sampling)
                arm(t);
        }
        t->user = row.user;
        t->system = row.system;
        t->seen = true;
        memcpy(t->name, row.name, sizeof(t->name));
    }
    closedir(dir);
    for (Thread *t = profile.threads; t; t = t->next) {
        if (!t->seen && t->timer_live) {
            timer_delete(t->timer);
            t->timer_live = false;
        }
        consume(t);
    }
}
static double timeval_seconds(struct timeval value) {
    return value.tv_sec + value.tv_usec / 1e6;
}
static int by_cpu(const void *a, const void *b) {
    const Thread *x = *(Thread *const *)a, *y = *(Thread *const *)b;
    uint64_t xc = x->user - x->user_base + x->system - x->system_base;
    uint64_t yc = y->user - y->user_base + y->system - y->system_base;
    return xc < yc ? 1 : (xc > yc ? -1 : 0);
}
static int by_self(const void *a, const void *b) {
    const Symbol *x = *(Symbol *const *)a, *y = *(Symbol *const *)b;
    return x->self < y->self ? 1 : (x->self > y->self ? -1 : strcmp(x->label, y->label));
}
static int by_label(const void *a, const void *b) {
    const Symbol *x = *(Symbol *const *)a, *y = *(Symbol *const *)b;
    return strcmp(x->label, y->label);
}
typedef struct ThreadHotspot {
    Symbol *symbol;
    uint64_t count;
} ThreadHotspot;
static int hotspot_label(const void *a, const void *b) {
    const ThreadHotspot *x = a, *y = b;
    return strcmp(x->symbol->label, y->symbol->label);
}
static int hotspot_count(const void *a, const void *b) {
    const ThreadHotspot *x = a, *y = b;
    return x->count < y->count ? 1 : (x->count > y->count ? -1 : hotspot_label(a, b));
}
static void thread_hotspots(Thread **threads, unsigned count) {
    ThreadHotspot *rows = calloc(profile.stack_count ? profile.stack_count : 1, sizeof(*rows));
    if (!rows) {
        profile.report_error = true;
        return;
    }
    FILE *f = output("thread-hotspots.txt");
    if (!f) {
        free(rows);
        return;
    }
    for (unsigned i = 0; i < count; i++) {
        Thread *t = threads[i];
        if (!t->samples)
            continue;
        unsigned n = 0;
        for (unsigned b = 0; b < STACK_BUCKETS; b++)
            for (Stack *s = profile.stacks[b]; s; s = s->next)
                if (s->thread == t)
                    rows[n++] = (ThreadHotspot){s->symbols[0], s->count};
        qsort(rows, n, sizeof(*rows), hotspot_label);
        unsigned group = 0;
        for (unsigned j = 1; j < n; j++) {
            if (strcmp(rows[group].symbol->label, rows[j].symbol->label)) {
                group = j;
                continue;
            }
            rows[group].count += rows[j].count;
            rows[j].count = 0;
        }
        qsort(rows, n, sizeof(*rows), hotspot_count);
        fprintf(f, "\nTID %d (%s), %" PRIu64 " samples; self %% within this thread\n", t->tid, t->name, t->samples);
        for (unsigned j = 0; j < n && j < 30; j++)
            if (rows[j].count)
                fprintf(f, "%7.2f%% %10" PRIu64 "  %s\n", 100.0 * rows[j].count / t->samples, rows[j].count,
                        rows[j].symbol->label);
    }
    finish_file(f);
    free(rows);
}
static void report(void) {
    double elapsed = profile.elapsed_ns / 1e9;
    if (elapsed <= 0)
        elapsed = 1e-9;
    Thread *threads[MAX_THREADS];
    unsigned count = 0;
    uint64_t samples = 0, dropped = 0, overruns = 0;
    double observed = 0;
    for (Thread *t = profile.threads; t; t = t->next) {
        threads[count++] = t;
        samples += t->samples;
        overruns += t->overruns;
        dropped += atomic_load_explicit(&t->dropped, memory_order_relaxed);
        observed += (t->user - t->user_base + t->system - t->system_base) / (double)profile.ticks;
    }
    qsort(threads, count, sizeof(*threads), by_cpu);
    FILE *summary = output("summary.json"), *csv = output("threads.csv"), *text = output("hotspots.txt");
    if (summary)
        fprintf(summary,
                "{\"format_version\":1,\"backend\":\"linux-thread-cpu-timers-fp\","
                "\"elapsed_seconds\":%.6f,\"frequency_hz\":%u,\"user_seconds\":%.6f,\"system_seconds\":%.6f,"
                "\"cpu_percent\":%.3f,\"observed_thread_seconds\":%.6f,\"thread_accounting_gap_seconds\":%.6f,"
                "\"samples\":%" PRIu64 ",\"ring_drops\":%" PRIu64 ",\"timer_overruns\":%" PRIu64 ","
                "\"discarded_stacks\":%" PRIu64 ",\"allocation_failures\":%" PRIu64 ","
                "\"shallow_samples\":%" PRIu64 ",\"depth_limit_samples\":%" PRIu64 ","
                "\"thread_limit_hits\":%" PRIu64 ",\"threads\":[",
                elapsed, profile.hz, profile.user_seconds, profile.system_seconds,
                100 * (profile.user_seconds + profile.system_seconds) / elapsed, observed,
                profile.user_seconds + profile.system_seconds - observed, samples, dropped, overruns,
                profile.discarded_stacks, profile.allocation_failures, profile.shallow_samples,
                profile.depth_limit_samples, profile.thread_limit_hits);
    if (csv)
        fputs("tid,name,cpu_percent,user_seconds,system_seconds,samples,timer_errno\n", csv);
    if (text)
        fprintf(text,
                "Wall time: %.3fs; process CPU: %.1f%% (one logical CPU = 100%%)\n"
                "User %.3fs; kernel %.3fs; samples %" PRIu64 "; ring drops %" PRIu64 "; timer overruns %" PRIu64 "\n"
                "Process minus observed thread CPU %.3fs (signed gap: exited threads and counter granularity).\n"
                "CPU samples include kernel time, attributed to the interrupted userspace PC.\n"
                "Stacks use frame pointers, at most %d frames; optimized libraries can truncate them.\n\n"
                "     TID    CPU %%    user s  kernel s   samples  name\n",
                elapsed, 100 * (profile.user_seconds + profile.system_seconds) / elapsed, profile.user_seconds,
                profile.system_seconds, samples, dropped, overruns,
                profile.user_seconds + profile.system_seconds - observed, DEPTH);
    for (unsigned i = 0; i < count; i++) {
        Thread *t = threads[i];
        double user = (t->user - t->user_base) / (double)profile.ticks;
        double system = (t->system - t->system_base) / (double)profile.ticks, percent = 100 * (user + system) / elapsed;
        if (summary) {
            fprintf(summary, "%s{\"tid\":%d,\"start_ticks\":%" PRIu64 ",\"name\":", i ? "," : "", t->tid,
                    t->start_ticks);
            quoted(summary, t->name);
            fprintf(summary,
                    ",\"cpu_percent\":%.3f,\"user_seconds\":%.6f,\"system_seconds\":%.6f,"
                    "\"samples\":%" PRIu64 ",\"timer_errno\":%d}",
                    percent, user, system, t->samples, t->timer_error);
        }
        if (csv) {
            fprintf(csv, "%d,\"", t->tid);
            for (const char *p = t->name; *p; p++) {
                if (*p == '"')
                    fputc('"', csv);
                fputc(*p, csv);
            }
            fprintf(csv, "\",%.3f,%.6f,%.6f,%" PRIu64 ",%d\n", percent, user, system, t->samples, t->timer_error);
        }
        if (text)
            fprintf(text, "%8d %8.1f %9.3f %9.3f %9" PRIu64 "  %s%s\n", t->tid, percent, user, system, t->samples,
                    t->name, t->timer_error ? " [timer unavailable; see summary.json]" : "");
    }
    if (summary) {
        fputs("]}\n", summary);
        finish_file(summary);
    }
    if (csv)
        finish_file(csv);
    thread_hotspots(threads, count);
    /* Combine instruction addresses from the same resolved symbol for a useful
     * function table. Raw addresses remain in samples.jsonl for offline lookup. */
    Symbol **sorted = calloc(profile.symbol_count ? profile.symbol_count : 1, sizeof(*sorted));
    if (sorted) {
        unsigned n = 0;
        for (unsigned b = 0; b < SYMBOL_BUCKETS; b++)
            for (Symbol *s = profile.symbols[b]; s; s = s->next)
                sorted[n++] = s;
        qsort(sorted, n, sizeof(*sorted), by_label);
        unsigned group = 0;
        for (unsigned i = 1; i < n; i++) {
            if (strcmp(sorted[i]->label, sorted[group]->label)) {
                group = i;
                continue;
            }
            sorted[group]->self += sorted[i]->self;
            sorted[group]->inclusive += sorted[i]->inclusive;
            sorted[i]->self = sorted[i]->inclusive = 0;
        }
        qsort(sorted, n, sizeof(*sorted), by_self);
        FILE *functions = output("functions.csv");
        if (functions) {
            fputs("self_samples,inclusive_samples,self_percent,function\n", functions);
            for (unsigned i = 0; i < n; i++)
                if (sorted[i]->self || sorted[i]->inclusive) {
                    fprintf(functions, "%" PRIu64 ",%" PRIu64 ",%.3f,\"", sorted[i]->self, sorted[i]->inclusive,
                            samples ? 100.0 * sorted[i]->self / samples : 0);
                    for (const char *p = sorted[i]->label; *p; p++) {
                        if (*p == '"')
                            fputc('"', functions);
                        fputc(*p, functions);
                    }
                    fputs("\"\n", functions);
                }
            finish_file(functions);
        }
        if (text) {
            fputs("\n self %   self samples    inclusive   function\n", text);
            for (unsigned i = 0; i < n && i < 100; i++)
                if (sorted[i]->self)
                    fprintf(text, "%7.2f %14" PRIu64 " %12" PRIu64 "   %s\n",
                            samples ? 100.0 * sorted[i]->self / samples : 0, sorted[i]->self, sorted[i]->inclusive,
                            sorted[i]->label);
            fputs("\nInclusive counts count a function once per stack, including recursive stacks.\n"
                  "All functions: functions.csv; call paths: stacks.folded; raw PCs: samples.jsonl.\n",
                  text);
        }
        free(sorted);
    }
    if (text)
        finish_file(text);
    FILE *folded = output("stacks.folded"), *raw = output("samples.jsonl");
    for (unsigned b = 0; b < STACK_BUCKETS; b++)
        for (Stack *s = profile.stacks[b]; s; s = s->next) {
            if (folded) {
                fprintf(folded, "tid-%d", s->thread->tid);
                for (unsigned i = s->depth; i > 0; i--)
                    fprintf(folded, ";%s", s->symbols[i - 1]->label);
                fprintf(folded, " %" PRIu64 "\n", s->count);
            }
            if (raw) {
                fprintf(raw, "{\"tid\":%d,\"count\":%" PRIu64 ",\"frames\":[", s->thread->tid, s->count);
                for (unsigned i = 0; i < s->depth; i++) {
                    fprintf(raw, "%s{\"pc\":\"0x%" PRIxPTR "\",\"symbol\":", i ? "," : "", s->symbols[i]->address);
                    quoted(raw, s->symbols[i]->label);
                    fputs(",\"module\":", raw);
                    quoted(raw, s->symbols[i]->module);
                    fprintf(raw, ",\"module_base\":\"0x%" PRIxPTR "\",\"module_offset\":\"0x%" PRIxPTR "\"}",
                            s->symbols[i]->module_base, s->symbols[i]->address - s->symbols[i]->module_base);
                }
                fputs("]}\n", raw);
            }
        }
    if (folded)
        finish_file(folded);
    if (raw)
        finish_file(raw);
    fprintf(stderr, "Profiler: %s (%" PRIu64 " samples, CPU %.1f%%)%s\n", profile.directory, samples,
            100 * (profile.user_seconds + profile.system_seconds) / elapsed,
            profile.report_error ? " [report incomplete]" : "");
}
static void *collect(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "n64-profiler");
    profile.collector_tid = (int)syscall(SYS_gettid);
    uint64_t ready = monotonic() + (uint64_t)profile.delay * 1000000000;
    while (!atomic_load_explicit(&profile.stopping, memory_order_relaxed) && monotonic() < ready) {
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
    }
    if (atomic_load_explicit(&profile.stopping, memory_order_relaxed)) {
        fprintf(stderr, "Profiler: game exited before capture delay elapsed\n");
        return NULL;
    }
    profile.start_ns = monotonic();
    getrusage(RUSAGE_SELF, &profile.usage);
    profile.sampling = true;
    scan(true);
    copy_proc("/proc/self/maps", "maps.txt");
    copy_proc("/proc/self/status", "process-status.txt");
    while (!atomic_load_explicit(&profile.stopping, memory_order_relaxed)) {
        struct timespec pause = {0, 100000000};
        nanosleep(&pause, NULL);
        scan(false);
        if (profile.duration && monotonic() - profile.start_ns >= (uint64_t)profile.duration * 1000000000)
            break;
    }
    profile.sampling = false;
    for (Thread *t = profile.threads; t; t = t->next)
        if (t->timer_live) {
            timer_delete(t->timer);
            t->timer_live = false;
        }
    scan(false);
    profile.elapsed_ns = monotonic() - profile.start_ns;
    struct rusage end;
    getrusage(RUSAGE_SELF, &end);
    profile.user_seconds = timeval_seconds(end.ru_utime) - timeval_seconds(profile.usage.ru_utime);
    profile.system_seconds = timeval_seconds(end.ru_stime) - timeval_seconds(profile.usage.ru_stime);
    copy_proc("/proc/self/maps", "maps-end.txt");
    report();
    return NULL;
}
void recomp_profile_stop(void) {
    if (!profile.started)
        return;
    atomic_store_explicit(&profile.stopping, true, memory_order_relaxed);
    pthread_join(profile.collector, NULL);
    profile.started = false;
    /* Keep the handler and slot storage alive for already queued timer signals. */
}
bool recomp_profile_start(const char *directory, unsigned hz, unsigned delay, unsigned duration) {
    if (profile.directory || !directory || !*directory || !hz || hz > 1000)
        return false;
    if (strlen(directory) > 3900) {
        fprintf(stderr, "Profiler: directory path too long\n");
        return false;
    }
    profile.signal = SIGRTMIN + 4;
    struct sigaction previous;
    if (sigaction(profile.signal, NULL, &previous) || previous.sa_handler != SIG_DFL) {
        fprintf(stderr, "Profiler: reserved realtime signal is already in use\n");
        return false;
    }
    /* Never overwrite an earlier capture. Parent must exist, like perf output. */
    if (mkdir(directory, 0755)) {
        fprintf(stderr, "Profiler: cannot create new directory %s: %s\n", directory, strerror(errno));
        return false;
    }
    profile.directory = strdup(directory);
    if (!profile.directory)
        return false;
    profile.hz = hz;
    profile.delay = delay;
    profile.duration = duration;
    profile.ticks = sysconf(_SC_CLK_TCK);
    /* pread is async-signal-safe and an unmapped frame returns an error. Keep
     * this descriptor open until process exit, including queued final signals. */
    profile.mem_fd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
    unsigned probe = 0;
    if (profile.mem_fd < 0 ||
        pread(profile.mem_fd, &probe, sizeof(probe), (off_t)(uintptr_t)&hz) != (ssize_t)sizeof(probe)) {
        fprintf(stderr, "Profiler: cannot safely read this process's stacks: %s\n", strerror(errno));
        return false;
    }
    struct sigaction action = {.sa_sigaction = sample_signal, .sa_flags = SA_SIGINFO | SA_RESTART};
    sigemptyset(&action.sa_mask);
    if (sigaction(profile.signal, &action, NULL))
        return false;
    int error = pthread_create(&profile.collector, NULL, collect, NULL);
    if (error) {
        fprintf(stderr, "Profiler: collector failed: %s\n", strerror(error));
        return false;
    }
    profile.started = true;
    atexit(recomp_profile_stop);
    fprintf(stderr, "Profiler: %u Hz CPU samples, delay %us, duration %us (0 = until exit), output %s\n", hz, delay,
            duration, directory);
    return true;
}
#else
bool recomp_profile_start(const char *directory, unsigned hz, unsigned delay, unsigned duration) {
    (void)directory;
    (void)hz;
    (void)delay;
    (void)duration;
    fprintf(stderr, "Profiler: built-in sampling requires x86-64 Linux\n");
    return false;
}
void recomp_profile_stop(void) {}
#endif
