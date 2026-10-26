#include "runtime.h"
#include "runtime_debug.h"

#include <execinfo.h>
#include <unwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opt in when diagnosing native memory-copy calls. */
#define LOG_NATIVE_MEMORY_CALLS 0

static _Thread_local RecompFailureHandler failure_handler;
static _Thread_local void *failure_userdata;
static _Thread_local bool reporting_failure, failed_ram_range;
static _Thread_local size_t failed_bytes;
typedef struct RecompWarningSite {
    uint32_t address;
    uint64_t message_hash;
} RecompWarningSite;
static _Thread_local RecompWarningSite warning_sites[32];
static _Thread_local unsigned warning_site_count;
static _Thread_local bool warning_limit_reported;
static _Thread_local uintptr_t trace_ceiling;
void recomp_set_trace_ceiling(uintptr_t ceiling) {
    trace_ceiling = ceiling;
}
static _Unwind_Reason_Code trace_frame(struct _Unwind_Context *context, void *opaque) {
    RecompFailureInfo *info = opaque;
    /* An optional stack boundary can limit diagnostic unwinding. */
    if (info->native_frame_count == 64 || (trace_ceiling && _Unwind_GetCFA(context) >= trace_ceiling))
        return _URC_END_OF_STACK;
    uintptr_t address = (uintptr_t)_Unwind_GetIP(context);
    if (!address)
        return _URC_END_OF_STACK;
    info->native_frames[info->native_frame_count++] = (void *)address;
    return _URC_NO_REASON;
}

/* Failure paths are cold and never return. Keeping them out of line keeps
   the store fast paths small. */
__attribute__((noreturn, cold)) void recomp_fail(const char *msg, uint32_t address);

void recomp_set_failure_handler(RecompFailureHandler handler, void *userdata) {
    failure_handler = handler;
    failure_userdata = userdata;
}
void recomp_dispatch_failure(const RecompFailureInfo *info) {
    if (!reporting_failure) {
        reporting_failure = true;
        if (failure_handler)
            failure_handler(info, failure_userdata);
    }
    abort();
}
void recomp_dispatch_warning(const RecompFailureInfo *info) {
    if (reporting_failure)
        return;
    reporting_failure = true;
    if (failure_handler)
        failure_handler(info, failure_userdata);
    reporting_failure = false;
}
void recomp_warn(const char *message, uint32_t address) {
    if (reporting_failure)
        return;
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *p = (const unsigned char *)message; *p; p++)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    for (unsigned i = 0; i < warning_site_count; i++)
        if (warning_sites[i].address == address && warning_sites[i].message_hash == hash)
            return;
    if (warning_site_count == sizeof(warning_sites) / sizeof(warning_sites[0])) {
        if (!warning_limit_reported) {
            fputs("recomp warning: diagnostic report limit reached\n", stderr);
            warning_limit_reported = true;
        }
        return;
    }
    warning_sites[warning_site_count++] = (RecompWarningSite){.address = address, .message_hash = hash};
    fprintf(stderr, "recomp warning: %s (0x%08x)\n", message, address);
    RecompFailureInfo info = {.message = message, .address = address, .warning = true};
    (void)_Unwind_Backtrace(trace_frame, &info);
    fputs("Native call stack:\n", stderr);
    backtrace_symbols_fd(info.native_frames, (int)info.native_frame_count, 2);
    reporting_failure = true;
    if (failure_handler)
        failure_handler(&info, failure_userdata);
    reporting_failure = false;
}
_Noreturn void recomp_fail(const char *message, uint32_t address) {
    fprintf(stderr, "recomp: %s (0x%08x)\n", message, address);
    if (!reporting_failure) {
        reporting_failure = true;
        RecompFailureInfo info = {
            .message = message, .address = address, .requested_bytes = failed_bytes, .ram_range = failed_ram_range};
        (void)_Unwind_Backtrace(trace_frame, &info);
        fputs("Native call stack:\n", stderr);
        backtrace_symbols_fd(info.native_frames, (int)info.native_frame_count, 2);
        if (failure_handler)
            failure_handler(&info, failure_userdata);
    }
    abort();
}
static _Noreturn void fail_ram(const char *message, uint32_t address, size_t bytes) {
    failed_ram_range = true;
    failed_bytes = bytes;
    recomp_fail(message, address);
}

static int compare_entries(const void *a, const void *b) {
    const RecompFunctionEntry *x = *(const RecompFunctionEntry *const *)a;
    const RecompFunctionEntry *y = *(const RecompFunctionEntry *const *)b;
    return (x->address > y->address) - (x->address < y->address);
}

static size_t function_hash(uint32_t address) {
    /* Mix the whole address: overlays and ROM mappings often share low bits. */
    uint32_t key = address >> 2;
    key = (key ^ (key >> 16)) * UINT32_C(0x7feb352d);
    key = (key ^ (key >> 15)) * UINT32_C(0x846ca68b);
    return key ^ (key >> 16);
}

static bool index_functions(Runtime *rt) {
    size_t count = rt->function_count;
    if (!count)
        return true;
    rt->function_index = malloc(count * sizeof(*rt->function_index));
    if (!rt->function_index)
        return false;
    for (size_t i = 0; i < count; i++)
        rt->function_index[i] = &rt->functions[i];
    qsort(rt->function_index, count, sizeof(*rt->function_index), compare_entries);

    /* Keep the table at most half full, with a zero slot ending every probe. */
    if (count > SIZE_MAX / 2)
        return false;
    size_t slots = 1;
    while (slots < count * 2) {
        if (slots > SIZE_MAX / 2)
            return false;
        slots *= 2;
    }
    if (slots > SIZE_MAX / sizeof(*rt->function_lookup))
        return false;
    rt->function_lookup = calloc(slots, sizeof(*rt->function_lookup));
    if (!rt->function_lookup)
        return false;
    rt->function_lookup_mask = slots - 1;
    for (size_t i = 0; i < count; i++) {
        uint32_t address = rt->function_index[i]->address;
        if (i && rt->function_index[i - 1]->address == address)
            continue;
        size_t slot = function_hash(address) & rt->function_lookup_mask;
        while (rt->function_lookup[slot])
            slot = (slot + 1) & rt->function_lookup_mask;
        rt->function_lookup[slot] = i + 1;
    }
    return true;
}

bool recomp_runtime_init(Runtime *rt, size_t ram_size, const RecompFunctionEntry *functions, size_t count,
                         const bool *loaded, size_t sections) {
    *rt = (Runtime){.interrupt_mask = UINT32_C(0x003fff01)};
    if (!ram_size || ram_size % 4 || ram_size > UINT32_C(0x20000000) || !sections || (count && !functions) ||
        count > SIZE_MAX / sizeof(*rt->function_index))
        return false;
    /* RT64 addresses an 8 MiB physical window, including its RAM debugger dump.
     * Preserve that backing for 4 MiB games now that it uses live RAM. Guest
     * bounds and Expansion Pak detection still use the requested rdram_size. */
    size_t backing_size = ram_size == 4 * 1024 * 1024 ? 8 * 1024 * 1024 : ram_size;
    rt->rdram = calloc(1, backing_size);
    rt->section_loaded = calloc(sections, sizeof(bool));
    if (!rt->rdram || !rt->section_loaded) {
        recomp_runtime_destroy(rt);
        return false;
    }
    rt->rdram_size = ram_size;
    rt->functions = functions;
    rt->function_count = count;
    rt->section_count = sections;
    if (!index_functions(rt)) {
        recomp_runtime_destroy(rt);
        return false;
    }
    if (loaded)
        memcpy(rt->section_loaded, loaded, sections * sizeof(bool));
    RECOMP_LOG("game", "runtime initialized: RAM=%zu bytes functions=%zu sections=%zu", ram_size, count, sections);
    return true;
}

void recomp_runtime_destroy(Runtime *rt) {
    RECOMP_LOG("game", "runtime shutdown: RAM=%zu bytes", rt->rdram_size);
    if (rt->destroy_scheduler)
        rt->destroy_scheduler(rt);
    if (rt->destroy_tasks)
        rt->destroy_tasks(rt);
    if (rt->destroy_io)
        rt->destroy_io(rt);
    if (rt->destroy_platform)
        rt->destroy_platform(rt);
    if (rt->destroy_clock)
        rt->destroy_clock(rt);
    free(rt->rdram);
    free(rt->section_loaded);
    free(rt->function_index);
    free(rt->function_lookup);
    if (failure_userdata == rt)
        recomp_set_failure_handler(NULL, NULL);
    *rt = (Runtime){0};
}

RecompFunction recomp_lookup(Runtime *rt, uint32_t address) {
    recomp_record_call(rt, address);
    address = recomp_canonical_function_address(rt, address);
    RecompFunction result = NULL;
    size_t first = rt->function_count;
    if (rt->function_lookup) {
        size_t slot = function_hash(address) & rt->function_lookup_mask;
        while (rt->function_lookup[slot]) {
            size_t index = rt->function_lookup[slot] - 1;
            if (rt->function_index[index]->address == address) {
                first = index;
                break;
            }
            slot = (slot + 1) & rt->function_lookup_mask;
        }
    }
    for (size_t i = first; i < rt->function_count && rt->function_index[i]->address == address; i++) {
        const RecompFunctionEntry *entry = rt->function_index[i];
        if (entry->address == address && entry->section < rt->section_count && rt->section_loaded[entry->section]) {
            if (result)
                recomp_fail("ambiguous loaded function", address);
            result = entry->function;
        }
    }
    if (!result)
        recomp_fail("unregistered or unloaded function", address);
    if (rt->prepare_call)
        rt->prepare_call(rt, address);
    return result;
}

static uint32_t ram_address(Runtime *rt, uint32_t address, size_t bytes, bool write) {
    uint32_t result = address;
    if (recomp_direct_address(address))
        result &= UINT32_C(0x1fffffff);
    else if (rt->resolve_address && rt->resolve_address(rt, address, bytes, write, &result)) {
        /* The native loader supplies the backing page; range validation below
         * still applies. It must not alter the semantics of unrelated aliases. */
    } else {
        bool found = false;
        for (unsigned i = 0; i < 32; i++) {
            const RecompPageMapping *m = &rt->page_mappings[i];
            if (!m->installed || address < m->virtual_base ||
                (uint64_t)address >= (uint64_t)m->virtual_base + 2 * m->page_size)
                continue;
            if (found)
                recomp_fail("overlapping native page mappings", address);
            found = true;
            unsigned page = (address - m->virtual_base) / m->page_size;
            uint32_t offset = (address - m->virtual_base) % m->page_size;
            if (m->physical[page] == UINT32_MAX)
                recomp_fail("invalid native page", address);
            result = m->physical[page] + offset;
            size_t span = m->page_size - offset;
            if (bytes > span && ((uint64_t)address + span > UINT32_MAX ||
                                 ram_address(rt, address + (uint32_t)span, bytes - span, write) != result + span))
                recomp_fail("noncontiguous native page span", address);
        }
    }
    if (result >= rt->rdram_size || bytes > rt->rdram_size - result)
        fail_ram("RAM address out of range", address, bytes);
    return result;
}

uint32_t recomp_ram_address(Runtime *rt, uint32_t address, size_t bytes) {
    return ram_address(rt, address, bytes, false);
}
uint32_t recomp_ram_address_write_slow(Runtime *rt, uint32_t address, size_t bytes) {
    return ram_address(rt, address, bytes, true);
}
uint32_t recomp_physical_address(Runtime *rt, uint32_t address, size_t bytes) {
    if (address >= rt->rdram_size || bytes > rt->rdram_size - address)
        fail_ram("physical RAM address out of range", address, bytes);
    return address;
}

void recomp_map_pages(Runtime *rt, unsigned index, uint32_t mask, uint32_t address, uint32_t even, uint32_t odd) {
    const uint32_t masks[] = {0, 0x6000, 0x1e000, 0x7e000, 0x1fe000, 0x7fe000, 0x1ffe000};
    bool valid = false;
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++)
        if (mask == masks[i])
            valid = true;
    if (index >= 32 || !valid)
        recomp_fail("invalid native page mapping", address);
    uint32_t size = (mask + 0x2000) / 2;
    uint32_t base = address & ~(2 * size - 1);
    if (base >= 0x80000000 && base < 0xc0000000)
        recomp_fail("mapping direct RAM alias", address);
    if ((even != UINT32_MAX && (even % size || even >= rt->rdram_size)) ||
        (odd != UINT32_MAX && (odd % size || odd >= rt->rdram_size)))
        recomp_fail("invalid native physical page", address);
    rt->page_mappings[index] = (RecompPageMapping){base, size, {even, odd}, true};
}

void recomp_unmap_pages(Runtime *rt) {
    memset(rt->page_mappings, 0, sizeof(rt->page_mappings));
}
void hook_os_map_tlb(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t stack = (uint32_t)ctx->gpr[29];
    if (recomp_load(rt, stack + 20, 4) != UINT32_MAX)
        recomp_fail("ASID page mapping unsupported", ctx->pc);
    recomp_map_pages(rt, (uint32_t)ctx->gpr[4], (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], (uint32_t)ctx->gpr[7],
                     (uint32_t)recomp_load(rt, stack + 16, 4));
}
void hook_os_unmap_tlb_all(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)ctx;
    recomp_unmap_pages(rt);
}

uint64_t recomp_load(Runtime *rt, uint32_t address, unsigned bytes) {
#ifdef RECOMP_DEBUG
    if (!(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8))
        recomp_fail("invalid load width", address);
    if (address % bytes)
        fail_ram("unaligned load", address, bytes);
#endif
    /* Some audio managers inline osAiGetLength. Use the same native DMA
     * state as the SDK hook for this read-only register, including polling
     * completed buffers. Other hardware accesses still fail below. */
    if (bytes == 4 &&
        (address == UINT32_C(0xa4500004) || address == UINT32_C(0x84500004) || address == UINT32_C(0x04500004)))
        return recomp_ai_state(rt).length;
    uint32_t bus = address;
    if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000))
        bus &= UINT32_C(0x1fffffff);
    /* Native PI copies run atomically between guest dispatches. Queued requests
     * are software messages; no PI bus transfer remains active in guest code. */
    if (bytes == 4 && bus == UINT32_C(0x04600010) && rt->platform)
        return 0;
    /* Some games read cartridge integrity words directly instead of calling
     * osPiRawReadIo. Use the attached, verified canonical ROM, never RDRAM. */
    if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000) && bus >= UINT32_C(0x10000000) &&
        bus < UINT32_C(0x1fc00000) && rt->rom) {
        size_t offset = bus - UINT32_C(0x10000000);
        if (offset > rt->rom_size || bytes > rt->rom_size - offset)
            recomp_fail("cartridge read outside attached ROM", address);
        uint64_t value = 0;
        for (unsigned i = 0; i < bytes; i++)
            value = (value << 8) | rt->rom[offset + i];
        return value;
    }
    uint32_t p = recomp_ram_address(rt, address, bytes);
#if RECOMP_LITTLE_ENDIAN_HOST
    /* RDRAM is word-swapped: aligned guest words already have native byte
     * order on little-endian hosts. memcpy avoids aliasing/alignment UB. */
    if (bytes == 4 && !(p & 3)) {
        uint32_t word;
        memcpy(&word, rt->rdram + p, 4);
        return word;
    }
    if (bytes == 8 && !(p & 3)) {
        uint32_t high, low;
        memcpy(&high, rt->rdram + p, 4);
        memcpy(&low, rt->rdram + p + 4, 4);
        return ((uint64_t)high << 32) | low;
    }
    if (bytes == 2 && !(p & 1)) {
        uint16_t half;
        memcpy(&half, rt->rdram + (p ^ 2), 2);
        return half;
    }
    if (bytes == 1)
        return rt->rdram[p ^ 3];
#endif
    uint64_t value = 0;
    /* Guest big-endian bytes reside in word-swapped storage for RT64. */
    for (unsigned i = 0; i < bytes; i++)
        value = (value << 8) | rt->rdram[(p + i) ^ 3];
    return value;
}

uint8_t recomp_load8_shared(Runtime *rt, uint32_t address) {
#ifndef RECOMP_DEBUG
    if (recomp_direct_ram_read(address))
        return rt->rdram[(address & UINT32_C(0x1fffffff)) ^ 3];
#endif
    return (uint8_t)recomp_load(rt, address, 1);
}

uint16_t recomp_load16_shared(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint16_t value;
        memcpy(&value, rt->rdram + ((address & UINT32_C(0x1fffffff)) ^ 2), 2);
        return value;
    }
#endif
    return (uint16_t)recomp_load(rt, address, 2);
}

uint32_t recomp_load32_shared(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint32_t value;
        memcpy(&value, rt->rdram + (address & UINT32_C(0x1fffffff)), 4);
        return value;
    }
#endif
    return (uint32_t)recomp_load(rt, address, 4);
}

uint64_t recomp_load64_shared(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint64_t value;
        memcpy(&value, rt->rdram + (address & UINT32_C(0x1fffffff)), 8);
        return (value << 32) | (value >> 32);
    }
#endif
    return recomp_load(rt, address, 8);
}

void recomp_store(Runtime *rt, uint32_t address, uint64_t value, unsigned bytes) {
#ifdef RECOMP_DEBUG
    if (!(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8))
        recomp_fail("invalid store width", address);
    if (address % bytes)
        fail_ram("unaligned store", address, bytes);
#endif
    uint32_t p = recomp_ram_address_write_slow(rt, address, bytes);
#if RECOMP_LITTLE_ENDIAN_HOST
    /* A resolver may return backing with a different alignment from the guest
     * address. Keep that bytewise fallback here, once for the whole program. */
    if (bytes == 4 && !(p & 3)) {
        uint32_t word = (uint32_t)value;
        memcpy(rt->rdram + p, &word, 4);
        return;
    }
    if (bytes == 8 && !(p & 3)) {
        uint64_t swapped = (value << 32) | (value >> 32);
        memcpy(rt->rdram + p, &swapped, 8);
        return;
    }
    if (bytes == 2 && !(p & 1)) {
        uint16_t half = (uint16_t)value;
        memcpy(rt->rdram + (p ^ 2), &half, 2);
        return;
    }
#endif
    for (unsigned i = 0; i < bytes; i++)
        rt->rdram[(p + i) ^ 3] = (uint8_t)(value >> (8 * (bytes - i - 1)));
}

void recomp_copy_to_ram(Runtime *rt, uint32_t address, const uint8_t *source, size_t bytes) {
    uint32_t p = ram_address(rt, address, bytes, true);
    for (size_t i = 0; i < bytes; i++)
        rt->rdram[(p + i) ^ 3] = source[i];
}

/* A custom pager can yield or evict a previously resolved source when resolving
 * the destination. Keep its reads/writes separate, like generated guest code.
 * Fixed mappings can use blocks, bounded by the smallest libultra page (4 KiB). */
static bool paged_address(Runtime *rt, uint32_t address) {
    return rt->resolve_address && !recomp_direct_address(address);
}

static size_t memory_span(uint32_t address, size_t bytes, bool backward) {
    size_t span = recomp_direct_address(address) ? bytes
                  : backward                     ? (address & 0xfff) + 1
                                                 : 0x1000 - (address & 0xfff);
    return span < bytes ? span : bytes;
}

static void copy_guest(Runtime *rt, uint32_t dest, uint32_t src, uint32_t bytes, bool backward) {
    if (!bytes)
        return;
    if ((uint64_t)dest + bytes > UINT64_C(0x100000000) || (uint64_t)src + bytes > UINT64_C(0x100000000))
        fail_ram("memory copy span overflows", dest, bytes);
    if (paged_address(rt, dest) || paged_address(rt, src)) {
        for (uint32_t n = 0; n < bytes; n++) {
            uint32_t i = backward ? bytes - 1 - n : n;
            uint64_t value = recomp_load(rt, src + i, 1);
            recomp_store(rt, dest + i, value, 1);
        }
        return;
    }
    while (bytes) {
        uint32_t d = backward ? dest + bytes - 1 : dest;
        uint32_t s = backward ? src + bytes - 1 : src;
        size_t span = memory_span(d, bytes, backward);
        size_t source_span = memory_span(s, bytes, backward);
        if (span > source_span)
            span = source_span;
        if (backward) {
            d -= (uint32_t)span - 1;
            s -= (uint32_t)span - 1;
        }
        uint32_t p = ram_address(rt, d, span, true), q = ram_address(rt, s, span, false);
        /* Whole words have identical storage order only when both ends align.
         * Copy the head/tail as guest bytes; libc handles the aligned middle. */
        if (!(p & 3) && !(q & 3) && !(span & 3)) {
            memmove(rt->rdram + p, rt->rdram + q, span);
        } else if ((p & 3) == (q & 3)) {
            size_t head = (4 - (p & 3)) & 3;
            if (head > span)
                head = span;
            size_t middle = (span - head) & ~(size_t)3;
            if (backward) {
                for (size_t i = span; i > head + middle;) {
                    i--;
                    rt->rdram[(p + i) ^ 3] = rt->rdram[(q + i) ^ 3];
                }
                memmove(rt->rdram + p + head, rt->rdram + q + head, middle);
                for (size_t i = head; i;) {
                    i--;
                    rt->rdram[(p + i) ^ 3] = rt->rdram[(q + i) ^ 3];
                }
            } else {
                for (size_t i = 0; i < head; i++)
                    rt->rdram[(p + i) ^ 3] = rt->rdram[(q + i) ^ 3];
                memmove(rt->rdram + p + head, rt->rdram + q + head, middle);
                for (size_t i = head + middle; i < span; i++)
                    rt->rdram[(p + i) ^ 3] = rt->rdram[(q + i) ^ 3];
            }
        } else {
            for (size_t n = 0; n < span; n++) {
                size_t i = backward ? span - 1 - n : n;
                rt->rdram[(p + i) ^ 3] = rt->rdram[(q + i) ^ 3];
            }
        }
        bytes -= (uint32_t)span;
        if (!backward) {
            dest += (uint32_t)span;
            src += (uint32_t)span;
        }
    }
}

static void fill_guest(Runtime *rt, uint32_t dest, uint8_t value, uint32_t bytes) {
    if ((uint64_t)dest + bytes > UINT64_C(0x100000000))
        fail_ram("memory fill span overflows", dest, bytes);
    while (bytes) {
        size_t span = memory_span(dest, bytes, false);
        uint32_t p = ram_address(rt, dest, span, true);
        size_t head = (4 - (p & 3)) & 3;
        if (head > span)
            head = span;
        size_t middle = (span - head) & ~(size_t)3;
        for (size_t i = 0; i < head; i++)
            rt->rdram[(p + i) ^ 3] = value;
        memset(rt->rdram + p + head, value, middle);
        for (size_t i = head + middle; i < span; i++)
            rt->rdram[(p + i) ^ 3] = value;
        dest += (uint32_t)span;
        bytes -= (uint32_t)span;
    }
}

void hook_os_memcpy(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t dest = (uint32_t)ctx->gpr[4];
    copy_guest(rt, dest, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], false);
    mips_set(ctx, 2, mips_s32(dest));

    if (LOG_NATIVE_MEMORY_CALLS)
        fprintf(stderr, "hook_os_memcpy called: dest=0x%08x, src=0x%08x, bytes=%u\n", dest, (uint32_t)ctx->gpr[5],
                (uint32_t)ctx->gpr[6]);
}
void hook_os_bcopy(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t src = (uint32_t)ctx->gpr[4], dest = (uint32_t)ctx->gpr[5], bytes = (uint32_t)ctx->gpr[6];
    /* Verified SDK variants return the original destination in v0, even though
     * the public bcopy interface is void. Their overlap check uses signed GPRs. */
    if (src != dest)
        copy_guest(rt, dest, src, bytes, mips_i32(dest) >= mips_i32(src) && mips_i32(dest) < mips_i32(src) + bytes);
    mips_set(ctx, 2, mips_s32(dest));
}
void hook_os_memset(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t dest = (uint32_t)ctx->gpr[4];
    fill_guest(rt, dest, (uint8_t)ctx->gpr[5], (uint32_t)ctx->gpr[6]);
    mips_set(ctx, 2, mips_s32(dest));
}
void hook_os_bzero(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    if (mips_i32(ctx->gpr[5]) > 0)
        fill_guest(rt, (uint32_t)ctx->gpr[4], 0, (uint32_t)ctx->gpr[5]);
}
void hook_os_bcmp(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t a = (uint32_t)ctx->gpr[4], b = (uint32_t)ctx->gpr[5];
    for (int64_t i = 0, bytes = mips_i32(ctx->gpr[6]); i < bytes; i++) {
        uint64_t value = recomp_load(rt, a + (uint32_t)i, 1);
        if (value != recomp_load(rt, b + (uint32_t)i, 1)) {
            mips_set(ctx, 2, 1);
            return;
        }
    }
    mips_set(ctx, 2, 0);
}
void hook_os_strlen(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t start = (uint32_t)ctx->gpr[4], address = start;
    while (recomp_load(rt, address, 1))
        address++;
    mips_set(ctx, 2, mips_s32(address - start));
}
void hook_os_strchr(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t address = (uint32_t)ctx->gpr[4];
    uint8_t wanted = (uint8_t)ctx->gpr[5];
    for (;;) {
        uint8_t value = (uint8_t)recomp_load(rt, address, 1);
        if (value == wanted) {
            mips_set(ctx, 2, mips_s32(address));
            return;
        }
        if (!value) {
            mips_set(ctx, 2, 0);
            return;
        }
        address++;
    }
}

uint64_t recomp_load_merge(Runtime *rt, uint32_t address, uint64_t old, unsigned width, bool left) {
    if (width != 4 && width != 8)
        recomp_fail("invalid merge load width", address);
    unsigned offset = address & (width - 1), count = left ? width - offset : offset + 1;
    uint32_t start = left ? address : address - offset;
    uint64_t value = width == 4 ? (uint32_t)old : old;
    for (unsigned i = 0; i < count; i++) {
        unsigned byte = left ? i : width - count + i, shift = (width - 1 - byte) * 8;
        uint64_t mask = UINT64_C(0xff) << shift;
        value = (value & ~mask) | (recomp_load(rt, start + i, 1) << shift);
    }
    return value;
}

void recomp_store_merge(Runtime *rt, uint32_t address, uint64_t value, unsigned width, bool left) {
#ifdef RECOMP_DEBUG
    if (width != 4 && width != 8)
        recomp_fail("invalid merge store width", address);
#endif
    unsigned offset = address & (width - 1), count = left ? width - offset : offset + 1;
    uint32_t start = left ? address : address - offset;
    uint32_t p = recomp_ram_address_write(rt, start, count);
    for (unsigned i = 0; i < count; i++) {
        unsigned byte = left ? i : width - count + i;
        rt->rdram[(p + i) ^ 3] = (uint8_t)(value >> ((width - 1 - byte) * 8));
    }
}
