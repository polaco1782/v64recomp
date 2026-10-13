#include "runtime.h"

#include <execinfo.h>
#include <unwind.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Thread_local RecompFailureHandler failure_handler;
static _Thread_local void *failure_userdata;
static _Thread_local bool reporting_failure, failed_ram_range;
static _Thread_local size_t failed_bytes;
static _Thread_local uintptr_t trace_ceiling;
void recomp_set_trace_ceiling(uintptr_t ceiling) { trace_ceiling=ceiling; }
static _Unwind_Reason_Code trace_frame(struct _Unwind_Context *context,void *opaque) {
    RecompFailureInfo *info=opaque;
    /* Minicoro switches to a separate C stack with a synthetic return address.
     * Stop inside its entry frame rather than unwinding into that address. */
    if(info->native_frame_count==64 || (trace_ceiling && _Unwind_GetCFA(context)>=trace_ceiling))
        return _URC_END_OF_STACK;
    uintptr_t address=(uintptr_t)_Unwind_GetIP(context);
    if(!address)return _URC_END_OF_STACK;
    info->native_frames[info->native_frame_count++]=(void *)address;
    return _URC_NO_REASON;
}
void recomp_set_failure_handler(RecompFailureHandler handler, void *userdata) {
    failure_handler=handler;failure_userdata=userdata;
}
_Noreturn void recomp_fail(const char *message, uint32_t address) {
    fprintf(stderr, "recomp: %s (0x%08x)\n", message, address);
    if (!reporting_failure) {
        reporting_failure=true;
        RecompFailureInfo info={.message=message,.address=address,
            .requested_bytes=failed_bytes,.ram_range=failed_ram_range};
        (void)_Unwind_Backtrace(trace_frame,&info);
        fputs("Native call stack:\n",stderr);
        backtrace_symbols_fd(info.native_frames,(int)info.native_frame_count,2);
        if (failure_handler) failure_handler(&info,failure_userdata);
    }
    abort();
}
_Noreturn void recomp_fellthrough(uint32_t address) {
    recomp_fail("fell off generated function", address);
}
static _Noreturn void fail_ram(const char *message,uint32_t address,size_t bytes) {
    failed_ram_range=true;failed_bytes=bytes;recomp_fail(message,address);
}

static int compare_entries(const void *a, const void *b) {
    const RecompFunctionEntry *x = *(const RecompFunctionEntry *const *)a;
    const RecompFunctionEntry *y = *(const RecompFunctionEntry *const *)b;
    return (x->address > y->address) - (x->address < y->address);
}

bool recomp_runtime_init(Runtime *rt, size_t ram_size,
                         const RecompFunctionEntry *functions, size_t count,
                         const bool *loaded, size_t sections) {
    *rt = (Runtime){0};
    if (!ram_size || ram_size % 4 || ram_size > UINT32_C(0x20000000) || !sections
            || (count && !functions) || count > SIZE_MAX / sizeof(*rt->function_index)) return false;
    rt->rdram = calloc(1, ram_size);
    rt->section_loaded = calloc(sections, sizeof(bool));
    if (!rt->rdram || !rt->section_loaded) {
        recomp_runtime_destroy(rt);
        return false;
    }
    rt->rdram_size = ram_size;
    rt->functions = functions;
    rt->function_count = count;
    rt->section_count = sections;
    if (count) {
        rt->function_index = malloc(count * sizeof(*rt->function_index));
        if (!rt->function_index) { recomp_runtime_destroy(rt); return false; }
        for (size_t i = 0; i < count; i++) rt->function_index[i] = &functions[i];
        qsort(rt->function_index, count, sizeof(*rt->function_index), compare_entries);
    }
    if (loaded) memcpy(rt->section_loaded, loaded, sections * sizeof(bool));
    return true;
}

void recomp_runtime_destroy(Runtime *rt) {
    if (rt->destroy_scheduler) rt->destroy_scheduler(rt);
    if (rt->destroy_tasks) rt->destroy_tasks(rt);
    if (rt->destroy_io) rt->destroy_io(rt);
    if (rt->destroy_platform) rt->destroy_platform(rt);
    if (rt->destroy_clock) rt->destroy_clock(rt);
    free(rt->rdram);
    free(rt->section_loaded);
    free(rt->function_index);
    *rt = (Runtime){0};
}

RecompFunction recomp_lookup(Runtime *rt, uint32_t address) {
    RecompFunction result = NULL;
    size_t first = 0, end = rt->function_count;
    while (first < end) {
        size_t middle = first + (end - first) / 2;
        if (rt->function_index[middle]->address < address) first = middle + 1;
        else end = middle;
    }
    for (size_t i = first; i < rt->function_count && rt->function_index[i]->address == address; i++) {
        const RecompFunctionEntry *entry = rt->function_index[i];
        if (entry->address == address && entry->section < rt->section_count && rt->section_loaded[entry->section]) {
            if (result) recomp_fail("ambiguous loaded function", address);
            result = entry->function;
        }
    }
    if (!result) recomp_fail("unregistered or unloaded function", address);
    if (rt->prepare_call) rt->prepare_call(rt, address);
    return result;
}

static uint32_t ram_address(Runtime *rt, uint32_t address, size_t bytes, bool write) {
    uint32_t result = address;
    if (address >= UINT32_C(0x80000000) && address < UINT32_C(0xc0000000)) result &= UINT32_C(0x1fffffff);
    else if (rt->resolve_address && rt->resolve_address(rt, address, bytes, write, &result)) {
        /* The native loader supplies the backing page; range validation below
         * still applies. It must not alter the semantics of unrelated aliases. */
    } else {
        bool found = false;
        for (unsigned i = 0; i < 32; i++) {
            const RecompPageMapping *m = &rt->page_mappings[i];
            if (!m->installed || address < m->virtual_base
                || (uint64_t)address >= (uint64_t)m->virtual_base + 2 * m->page_size) continue;
            if (found) recomp_fail("overlapping native page mappings", address);
            found = true;
            unsigned page = (address - m->virtual_base) / m->page_size;
            uint32_t offset = (address - m->virtual_base) % m->page_size;
            if (m->physical[page] == UINT32_MAX) recomp_fail("invalid native page", address);
            result = m->physical[page] + offset;
            size_t span = m->page_size - offset;
            if (bytes > span && ((uint64_t)address + span > UINT32_MAX
                || recomp_ram_address(rt, address + (uint32_t)span, bytes - span) != result + span))
                recomp_fail("noncontiguous native page span", address);
        }
    }
    if (result >= rt->rdram_size || bytes > rt->rdram_size - result) fail_ram("RAM address out of range", address,bytes);
    return result;
}
uint32_t recomp_ram_address(Runtime *rt, uint32_t address, size_t bytes) {
    return ram_address(rt, address, bytes, false);
}

void recomp_map_pages(Runtime *rt, unsigned index, uint32_t mask, uint32_t address,
                      uint32_t even, uint32_t odd) {
    const uint32_t masks[] = {0,0x6000,0x1e000,0x7e000,0x1fe000,0x7fe000,0x1ffe000};
    bool valid = false;
    for (unsigned i = 0; i < sizeof(masks)/sizeof(masks[0]); i++) if (mask == masks[i]) valid = true;
    if (index >= 32 || !valid) recomp_fail("invalid native page mapping", address);
    uint32_t size = (mask + 0x2000) / 2;
    uint32_t base = address & ~(2 * size - 1);
    if (base >= 0x80000000 && base < 0xc0000000) recomp_fail("mapping direct RAM alias", address);
    if ((even != UINT32_MAX && (even % size || even >= rt->rdram_size))
        || (odd != UINT32_MAX && (odd % size || odd >= rt->rdram_size)))
        recomp_fail("invalid native physical page", address);
    rt->page_mappings[index] = (RecompPageMapping){base,size,{even,odd},true};
}

void recomp_unmap_pages(Runtime *rt) { memset(rt->page_mappings,0,sizeof(rt->page_mappings)); }
void hook_os_map_tlb(Runtime *rt, MipsContext *ctx) {
    uint32_t stack = (uint32_t)ctx->gpr[29];
    if (recomp_load(rt,stack+20,4) != UINT32_MAX) recomp_fail("ASID page mapping unsupported", ctx->pc);
    recomp_map_pages(rt,(uint32_t)ctx->gpr[4],(uint32_t)ctx->gpr[5],(uint32_t)ctx->gpr[6],
                     (uint32_t)ctx->gpr[7],(uint32_t)recomp_load(rt,stack+16,4));
}
void hook_os_unmap_tlb_all(Runtime *rt, MipsContext *ctx) { (void)ctx; recomp_unmap_pages(rt); }

uint64_t recomp_load(Runtime *rt, uint32_t address, unsigned bytes) {
    if (!(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8)) recomp_fail("invalid load width", address);
    if (address % bytes) fail_ram("unaligned load", address,bytes);
    uint32_t p = recomp_ram_address(rt, address, bytes);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    /* RDRAM is word-swapped: aligned guest words already have native byte
     * order on little-endian hosts. memcpy avoids aliasing/alignment UB. */
    if(bytes==4 && !(p&3)) { uint32_t word;memcpy(&word,rt->rdram+p,4);return word; }
    if(bytes==8 && !(p&3)) {
        uint32_t high,low;memcpy(&high,rt->rdram+p,4);memcpy(&low,rt->rdram+p+4,4);
        return ((uint64_t)high<<32)|low;
    }
    if(bytes==2 && !(p&1)) { uint16_t half;memcpy(&half,rt->rdram+(p^2),2);return half; }
    if(bytes==1)return rt->rdram[p^3];
#endif
    uint64_t value = 0;
    /* Guest big-endian bytes reside in word-swapped storage for RT64. */
    for (unsigned i = 0; i < bytes; i++) value = (value << 8) | rt->rdram[(p + i) ^ 3];
    return value;
}

void recomp_store(Runtime *rt, uint32_t address, uint64_t value, unsigned bytes) {
    if (!(bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8)) recomp_fail("invalid store width", address);
    if (address % bytes) fail_ram("unaligned store", address,bytes);
    uint32_t p = ram_address(rt, address, bytes, true);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if(bytes==4 && !(p&3)) { uint32_t word=(uint32_t)value;memcpy(rt->rdram+p,&word,4);return; }
    if(bytes==8 && !(p&3)) {
        uint32_t high=(uint32_t)(value>>32),low=(uint32_t)value;
        memcpy(rt->rdram+p,&high,4);memcpy(rt->rdram+p+4,&low,4);
        return;
    }
    if(bytes==2 && !(p&1)) { uint16_t half=(uint16_t)value;memcpy(rt->rdram+(p^2),&half,2);return; }
    if(bytes==1){rt->rdram[p^3]=(uint8_t)value;return;}
#endif
    for (unsigned i = 0; i < bytes; i++) rt->rdram[(p + i) ^ 3] = (uint8_t)(value >> (8 * (bytes - i - 1)));
}

void recomp_copy_to_ram(Runtime *rt, uint32_t address, const uint8_t *source, size_t bytes) {
    uint32_t p = ram_address(rt, address, bytes, true);
    for (size_t i = 0; i < bytes; i++) rt->rdram[(p + i) ^ 3] = source[i];
}

uint64_t recomp_load_merge(Runtime *rt, uint32_t address, uint64_t old, unsigned width, bool left) {
    if (width!=4 && width!=8) recomp_fail("invalid merge load width", address);
    unsigned offset=address&(width-1), count=left?width-offset:offset+1;
    uint32_t start=left?address:address-offset;
    uint64_t value=width==4?(uint32_t)old:old;
    for (unsigned i=0;i<count;i++) {
        unsigned byte=left?i:width-count+i, shift=(width-1-byte)*8;
        uint64_t mask=UINT64_C(0xff)<<shift;
        value=(value&~mask)|(recomp_load(rt,start+i,1)<<shift);
    }
    return value;
}

void recomp_store_merge(Runtime *rt, uint32_t address, uint64_t value, unsigned width, bool left) {
    if (width!=4 && width!=8) recomp_fail("invalid merge store width", address);
    unsigned offset=address&(width-1), count=left?width-offset:offset+1;
    uint32_t start=left?address:address-offset;
    recomp_ram_address(rt,start,count); /* Validate all touched bytes before writing. */
    for (unsigned i=0;i<count;i++) {
        unsigned byte=left?i:width-count+i;
        recomp_store(rt,start+i,value>>((width-1-byte)*8),1);
    }
}
