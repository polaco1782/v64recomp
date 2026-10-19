#ifndef RECOMP_H
#define RECOMP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define RECOMP_LITTLE_ENDIAN_HOST 1
#else
#define RECOMP_LITTLE_ENDIAN_HOST 0
#endif

/* Set to 0 to disable optional intrinsics; compiler -D overrides this default. */
#ifndef USE_INTRISICS
#define USE_INTRISICS 1
#endif

/* Sign-extend to the 64-bit GPR bit pattern; each argument is evaluated once. */
#define mips_s32(x) ((uint64_t)(int64_t)(int32_t)(x))
#define mips_s16(x) ((uint64_t)(int64_t)(int16_t)(x))
#define mips_s8(x) ((uint64_t)(int64_t)(int8_t)(x))

#define mips_set(ctx, reg, value) ctx->gpr[reg] = (value)

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MipsContext {
    uint64_t gpr[32];
    uint64_t hi, lo;
    uint32_t fpr[32], fcr31;
    uint32_t pc; /* Debug instruction/call location; thread entry in Release. Not execution state. */
    uint32_t fpr_high[32]; /* Independent upper words when Status.FR is one. */
    bool fpu_fr1;
} MipsContext;

/* Generated sources are shared between build modes. Release must not evaluate
 * either argument or emit any diagnostic PC stores, even at -O0. */
#ifdef RECOMP_DEBUG
#define RECOMP_TRACK_PC(context, address) ((context)->pc = (address))
#else
#define RECOMP_TRACK_PC(context, address) ((void)0)
#endif

typedef struct Runtime Runtime;
typedef struct RecompScheduler RecompScheduler;
typedef struct RecompClock RecompClock;
typedef struct RecompPlatform RecompPlatform;
typedef struct RecompTasks RecompTasks;
typedef struct RecompIO RecompIO;
typedef void (*RecompFunction)(Runtime *, MipsContext *);
/* Optional native loader for game-specific virtual memory. Return false for
 * addresses it does not own; handled spans must resolve to contiguous RDRAM. */
typedef bool (*RecompAddressResolver)(Runtime *, uint32_t, size_t, bool write, uint32_t *);

typedef struct RecompFunctionEntry {
    uint32_t address, size, section;
    RecompFunction function;
    const char *name;
} RecompFunctionEntry;

/* Native global page aliases installed through libultra, not CPU TLB opcodes. */
typedef struct RecompPageMapping {
    uint32_t virtual_base, page_size, physical[2];
    bool installed;
} RecompPageMapping;

struct Runtime {
    /* Keep frequently accessed fields first for compact native member offsets. */
    uint8_t *rdram;
    size_t rdram_size;
    const RecompFunctionEntry *functions;
    size_t function_count;
    bool *section_loaded;
    size_t section_count;
    RecompScheduler *scheduler;
    void (*destroy_scheduler)(Runtime *);
    void (*checkpoint)(Runtime *, bool);
    RecompClock *clock;
    RecompPlatform *platform;
    void (*destroy_clock)(Runtime *);
    void (*destroy_platform)(Runtime *);
    void (*poll_clock)(Runtime *);
    void (*poll_platform)(Runtime *);
    const uint8_t *rom; /* Verified canonical bytes, owned by the caller. */
    size_t rom_size;
    bool platform_initialized;
    RecompTasks *tasks;
    void (*destroy_tasks)(Runtime *);
    void (*poll_tasks)(Runtime *);
    RecompIO *io;
    void (*destroy_io)(Runtime *);
    void (*poll_io)(Runtime *);
    const RecompFunctionEntry **function_index; /* Sorted once at init; original registry order is retained. */
    size_t *function_lookup; /* Hash slots hold the first sorted index + 1 for each guest address. */
    size_t function_lookup_mask;
    RecompAddressResolver resolve_address;
    void (*prepare_call)(Runtime *, uint32_t);
    void *address_userdata;
    /* Optional game loader: map relocated entries to their compiled addresses. */
    uint32_t (*canonical_function_address)(Runtime *, uint32_t);
    uint32_t (*relocated_address)(Runtime *, uint32_t section, uint32_t address);
    bool interrupts_disabled; /* Native OS critical section; serialized guest scheduling. */
    uint32_t interrupt_mask; /* Logical SDK CPU/RCP interrupt mask. */
    RecompPageMapping page_mappings[32];
#ifdef RECOMP_DEBUG
    /* Shared crash history; guest execution is serialized by the scheduler. */
    uint32_t recent_calls[128];
    unsigned recent_call_next, recent_call_count;
#endif
};

bool recomp_runtime_init(Runtime *, size_t, const RecompFunctionEntry *, size_t,
                         const bool *, size_t);
void recomp_runtime_destroy(Runtime *);
static inline void recomp_record_call(Runtime *rt, uint32_t address) {
#ifdef RECOMP_DEBUG
    rt->recent_calls[rt->recent_call_next] = address;
    rt->recent_call_next = (rt->recent_call_next + 1) % 128;
    if (rt->recent_call_count < 128) rt->recent_call_count++;
#else
    (void)rt; (void)address;
#endif
}
RecompFunction recomp_lookup(Runtime *, uint32_t);
static inline uint32_t recomp_relocated_address(Runtime *rt, uint32_t section, uint32_t address) {
    return rt->relocated_address ? rt->relocated_address(rt, section, address) : address;
}
static inline uint32_t recomp_canonical_function_address(Runtime *rt, uint32_t address) {
    return rt->canonical_function_address ? rt->canonical_function_address(rt, address) : address;
}
typedef struct RecompFailureInfo {
    const char *message;
    uint32_t address;
    size_t requested_bytes;
    bool ram_range;
    void *native_frames[64];
    size_t native_frame_count;
} RecompFailureInfo;
typedef void (*RecompFailureHandler)(const RecompFailureInfo *, void *);
/* Owner-thread observer for controlled runtime failures, before abort(). */
void recomp_set_failure_handler(RecompFailureHandler, void *);
/* Forward a captured worker failure to the current thread's observer. */
void recomp_dispatch_failure(const RecompFailureInfo *);
#ifdef __cplusplus
[[noreturn]] void recomp_fail(const char *, uint32_t);
#else
_Noreturn void recomp_fail(const char *, uint32_t);
#endif

/* Known targets still require loaded-section validation and pager preparation. */
static inline void recomp_prepare_direct_call(Runtime *rt, uint32_t address, uint32_t section) {
    recomp_record_call(rt, address);
    if (section >= rt->section_count || !rt->section_loaded[section])
        recomp_fail("unregistered or unloaded function", address);
    if (rt->prepare_call) rt->prepare_call(rt, address);
}

/* Fixed generated calls bind a unique entry in the compiled registry. Shared
 * dispatch avoids searching it and retains mapped/Debug lookup behavior. */
void recomp_call_generated(Runtime *, MipsContext *, const RecompFunctionEntry *);

/* Floating-point helpers. Each operation restores the host environment
 * before returning, including when guest rounding differs between threads. */
enum MipsFpFormat { MIPS_FP_S, MIPS_FP_D, MIPS_FP_W, MIPS_FP_L };
enum MipsFpOperation { MIPS_FP_ADD, MIPS_FP_SUB, MIPS_FP_MUL, MIPS_FP_DIV,
                       MIPS_FP_SQRT, MIPS_FP_ABS, MIPS_FP_NEG, MIPS_FP_MOV };
void mips_fp_binary(MipsContext *, unsigned, unsigned, unsigned,
                    enum MipsFpFormat, enum MipsFpOperation);
void mips_fp_unary(MipsContext *, unsigned, unsigned,
                   enum MipsFpFormat, enum MipsFpOperation);
void mips_fp_convert(MipsContext *, unsigned, unsigned,
                     enum MipsFpFormat, enum MipsFpFormat, int);
void mips_fp_compare(MipsContext *, unsigned, unsigned, enum MipsFpFormat, unsigned);
void mips_fp_set_control(MipsContext *, uint32_t);

/* Shared reader for ROM/MMIO, pagers, and Debug diagnostics. Fixed-width
 * generated loads use the helpers below. */
uint64_t recomp_load(Runtime *, uint32_t, unsigned);
/* Shared writer for mapped addresses, unusual backing alignment, and Debug
 * diagnostics. Existing callers with a runtime-selected width also use it. */
void recomp_store(Runtime *, uint32_t, uint64_t, unsigned);
uint32_t recomp_ram_address_write_slow(Runtime *, uint32_t, size_t);

static inline bool recomp_direct_address(uint32_t address) {
    return (address & UINT32_C(0xc0000000)) == UINT32_C(0x80000000);
}

/* Only the first 8 MiB of KSEG0/KSEG1 is ordinary N64 RAM. Other direct
 * segment addresses still need ROM/MMIO dispatch. As with stores, Release
 * assumes aligned, in-range guest accesses on this RAM path. */
static inline bool recomp_direct_ram_read(uint32_t address) {
    return (address & UINT32_C(0xdf800000)) == UINT32_C(0x80000000);
}

uint8_t recomp_load8_shared(Runtime *, uint32_t);
uint16_t recomp_load16_shared(Runtime *, uint32_t);
uint32_t recomp_load32_shared(Runtime *, uint32_t);
uint64_t recomp_load64_shared(Runtime *, uint32_t);

/* Keep ordinary RAM loads inline even when their address is computed at runtime.
 * ROM/MMIO and mapped addresses retain the shared fallback. Debug always uses
 * the checked reader, including for constant addresses. */
static inline uint8_t recomp_load8(Runtime *rt, uint32_t address) {
#ifndef RECOMP_DEBUG
    if (recomp_direct_ram_read(address))
        return rt->rdram[(address & UINT32_C(0x1fffffff)) ^ 3];
#endif
    return recomp_load8_shared(rt, address);
}

static inline uint16_t recomp_load16(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint16_t value;
        memcpy(&value, rt->rdram + ((address & UINT32_C(0x1fffffff)) ^ 2), 2);
        return value;
    }
#endif
    return recomp_load16_shared(rt, address);
}

static inline uint32_t recomp_load32(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint32_t value;
        memcpy(&value, rt->rdram + (address & UINT32_C(0x1fffffff)), 4);
        return value;
    }
#endif
    return recomp_load32_shared(rt, address);
}

static inline uint64_t recomp_load64(Runtime *rt, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_ram_read(address)) {
        uint64_t value;
        memcpy(&value, rt->rdram + (address & UINT32_C(0x1fffffff)), 8);
        return (value << 32) | (value >> 32);
    }
#endif
    return recomp_load64_shared(rt, address);
}

static inline uint32_t recomp_ram_address_write(Runtime *rt, uint32_t address, size_t bytes) {
#ifdef RECOMP_DEBUG
    return recomp_ram_address_write_slow(rt, address, bytes);
#else
    if (recomp_direct_address(address))
        return address & UINT32_C(0x1fffffff);
    return recomp_ram_address_write_slow(rt, address, bytes);
#endif
}

/* Keep only direct RAM stores inline. Duplicating the mapped-address and
 * bytewise fallback paths at every guest store increases generated .text.
 * Release assumes aligned, in-range guest accesses on this direct path.
 * Debug uses the shared writer so alignment/range failures retain their span. */
static inline void recomp_store8(Runtime *rt, uint32_t address, uint8_t value) {
#ifndef RECOMP_DEBUG
    if (recomp_direct_address(address)) {
        uint32_t p = recomp_ram_address_write(rt, address, 1);
        rt->rdram[p ^ 3] = value;
        return;
    }
#endif
    recomp_store(rt, address, value, 1);
}

static inline void recomp_store16(Runtime *rt, uint32_t address, uint16_t value) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_address(address)) {
        uint32_t p = recomp_ram_address_write(rt, address, 2);
        memcpy(rt->rdram + (p ^ 2), &value, 2);
        return;
    }
#endif
    recomp_store(rt, address, value, 2);
}

static inline void recomp_store32(Runtime *rt, uint32_t address, uint32_t value) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_address(address)) {
        uint32_t p = recomp_ram_address_write(rt, address, 4);
        memcpy(rt->rdram + p, &value, 4);
        return;
    }
#endif
    recomp_store(rt, address, value, 4);
}

static inline void recomp_store64(Runtime *rt, uint32_t address, uint64_t value) {
#if RECOMP_LITTLE_ENDIAN_HOST && !defined(RECOMP_DEBUG)
    if (recomp_direct_address(address)) {
        uint32_t p = recomp_ram_address_write(rt, address, 8);
        uint64_t swapped = (value << 32) | (value >> 32);
        memcpy(rt->rdram + p, &swapped, 8);
        return;
    }
#endif
    recomp_store(rt, address, value, 8);
}

void recomp_copy_to_ram(Runtime *, uint32_t, const uint8_t *, size_t);
uint32_t recomp_ram_address(Runtime *, uint32_t, size_t);
/* Hardware/RSP bus addresses bypass CPU virtual page aliases. */
uint32_t recomp_physical_address(Runtime *, uint32_t, size_t);
uint64_t recomp_load_merge(Runtime *, uint32_t, uint64_t, unsigned, bool);
void recomp_store_merge(Runtime *, uint32_t, uint64_t, unsigned, bool);

static inline void recomp_checkpoint(Runtime *rt) {
    if (rt->checkpoint) rt->checkpoint(rt, false);
}
static inline void recomp_yield_checkpoint(Runtime *rt) {
    if (rt->checkpoint) rt->checkpoint(rt, true);
}

static inline bool mips_signed_less(uint64_t x, uint64_t y) {
    return (x ^ UINT64_C(0x8000000000000000)) < (y ^ UINT64_C(0x8000000000000000));
}
static inline uint64_t mips_sra32(uint64_t x, unsigned count) {
    uint32_t value = (uint32_t)x;
    count &= 31;
    if (count) value = (value >> count) | ((value & UINT32_C(0x80000000)) ? UINT32_MAX << (32 - count) : 0);
    return mips_s32(value);
}
static inline uint64_t mips_sra64(uint64_t value, unsigned count) {
    count &= 63;
    if (count) value = (value >> count) | ((value & UINT64_C(0x8000000000000000)) ? UINT64_MAX << (64 - count) : 0);
    return value;
}
static inline uint64_t mips_add32(uint64_t x, uint64_t y) {
#if USE_INTRISICS && (defined(__GNUC__) || defined(__clang__))
    int32_t result;
    if (__builtin_add_overflow((int32_t)x, (int32_t)y, &result))
        recomp_fail("signed ADD overflow", 0);
#else
    uint32_t a = (uint32_t)x, b = (uint32_t)y, result = a + b;
    if (((a ^ result) & (b ^ result)) & UINT32_C(0x80000000)) recomp_fail("signed ADD overflow", 0);
#endif
    return mips_s32(result);
}
static inline uint64_t mips_sub32(uint64_t x, uint64_t y) {
#if USE_INTRISICS && (defined(__GNUC__) || defined(__clang__))
    int32_t result;
    if (__builtin_sub_overflow((int32_t)x, (int32_t)y, &result))
        recomp_fail("signed SUB overflow", 0);
#else
    uint32_t a = (uint32_t)x, b = (uint32_t)y, result = a - b;
    if (((a ^ b) & (a ^ result)) & UINT32_C(0x80000000)) recomp_fail("signed SUB overflow", 0);
#endif
    return mips_s32(result);
}
static inline int64_t mips_i32(uint64_t x) {
    return (int64_t)(uint32_t)x - ((x & UINT64_C(0x80000000)) ? INT64_C(4294967296) : 0);
}
static inline void mips_mult(MipsContext *ctx, uint64_t x, uint64_t y, bool is_signed) {
    uint64_t result = is_signed ? (uint64_t)(mips_i32(x) * mips_i32(y)) : (uint64_t)(uint32_t)x * (uint32_t)y;
    ctx->lo = mips_s32((uint32_t)result);
    ctx->hi = mips_s32((uint32_t)(result >> 32));
}
static inline void mips_div(MipsContext *ctx, uint64_t x, uint64_t y, bool is_signed) {
    int64_t a = mips_i32(x), b = mips_i32(y);
    if (!(uint32_t)y) {
        ctx->lo = mips_s32(is_signed && a < 0 ? 1 : UINT32_MAX);
        ctx->hi = mips_s32((uint32_t)x);
    } else if (is_signed) {
        /* int64 intermediates also make INT32_MIN / -1 well-defined. */
        ctx->lo = mips_s32((uint32_t)(a / b));
        ctx->hi = mips_s32((uint32_t)(a % b));
    } else {
        ctx->lo = mips_s32((uint32_t)x / (uint32_t)y);
        ctx->hi = mips_s32((uint32_t)x % (uint32_t)y);
    }
}

static inline void mips_dmult(MipsContext *ctx, uint64_t x, uint64_t y, bool is_signed) {
    /* Four 32-bit products preserve all 128 bits without compiler extensions. */
    uint64_t x0=(uint32_t)x, x1=x>>32, y0=(uint32_t)y, y1=y>>32;
    uint64_t low=x0*y0, middle=x1*y0+(low>>32), carry=middle>>32;
    middle=(uint32_t)middle+x0*y1;
    ctx->lo=(middle<<32)|(uint32_t)low;
    ctx->hi=x1*y1+carry+(middle>>32);
    if (is_signed) {
        if (x>>63) ctx->hi-=y;
        if (y>>63) ctx->hi-=x;
    }
}
static inline void mips_ddiv(MipsContext *ctx, uint64_t x, uint64_t y, bool is_signed) {
    bool negative_x=is_signed && (x>>63), negative_y=is_signed && (y>>63);
    if (!y) { ctx->lo=negative_x?1:UINT64_MAX; ctx->hi=x; return; }
    uint64_t a=negative_x?UINT64_C(0)-x:x, b=negative_y?UINT64_C(0)-y:y;
    uint64_t q=a/b, rem=a%b;
    ctx->lo=negative_x!=negative_y?UINT64_C(0)-q:q;
    ctx->hi=negative_x?UINT64_C(0)-rem:rem;
}

/* FR=0 pairs adjacent words; FR=1 has an independent 64-bit register at every index. */
static inline uint64_t mips_fpr_pair(const MipsContext *ctx, unsigned index) {
    if (index >= 32) recomp_fail("invalid FPU register", ctx->pc);
    if (ctx->fpu_fr1) return ((uint64_t)ctx->fpr_high[index]<<32)|ctx->fpr[index];
    if (index >= 31 || (index & 1)) recomp_fail("FR=0 double register must be even", ctx->pc);
    return ((uint64_t)ctx->fpr[index+1]<<32)|ctx->fpr[index];
}

static inline void mips_set_fpr_pair(MipsContext *ctx, unsigned index, uint64_t value) {
    if (index >= 32) recomp_fail("invalid FPU register", ctx->pc);
    if (ctx->fpu_fr1) ctx->fpr_high[index]=(uint32_t)(value>>32);
    else {
        if (index >= 31 || (index & 1)) recomp_fail("FR=0 double register must be even", ctx->pc);
        ctx->fpr[index+1]=(uint32_t)(value>>32);
    }
    ctx->fpr[index]=(uint32_t)value;
}

#ifdef __cplusplus
}
#endif
#endif
