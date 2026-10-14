/* VR4300 arithmetic for an IEEE binary32/binary64 host.
 * Build with -frounding-math -ffp-contract=off, without fast-math.
 * Guest traps fail at the original instruction; there is no exception emulator.
 * See docs/fpu.md for the supported operations and host requirements. */
#include "recomp.h"
#include <fenv.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <string.h>
#if !defined(__linux__) || !defined(__x86_64__) || !defined(__SSE2__)
#error "This floating-point backend currently requires Linux x86-64 SSE2"
#endif
#if defined(__SSE2__)
#include <emmintrin.h>
#endif
#if defined(__clang__)
#pragma STDC FENV_ACCESS ON
#endif

_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128
               && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024
               && sizeof(float) == 4 && sizeof(double) == 8,
               "floating-point runtime requires IEEE binary32/binary64");

enum { FP_I = 1, FP_U = 2, FP_O = 4, FP_Z = 8, FP_V = 16, FP_E = 32 };
static const int rounding[] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
typedef struct FpEnvironment {
    fenv_t saved;
    int saved_errno;
#if USE_INTRISICS
    unsigned saved_mxcsr;
    bool sse_only;
#endif
} FpEnvironment;

static FpEnvironment begin(MipsContext *ctx, int mode, bool sse_only) {
    FpEnvironment env;
    env.saved_errno = errno;
    unsigned rm = mode < 0 ? ctx->fcr31 & 3 : (unsigned)mode;
#if USE_INTRISICS
    env.sse_only = sse_only;
    if (sse_only) {
        static const unsigned mxcsr_rounding[] = { 0x0000, 0x6000, 0x4000, 0x2000 };
        env.saved_mxcsr = _mm_getcsr();
        /* Clear flags, DAZ/FTZ and rounding, then mask all host SSE traps.
         * Explicit scalar intrinsics keep these operations out of the x87 FPU. */
        _mm_setcsr((env.saved_mxcsr & ~UINT32_C(0xe07f))
                   | UINT32_C(0x1f80) | mxcsr_rounding[rm]);
        return env;
    }
#else
    (void)sse_only;
#endif
    if (feholdexcept(&env.saved) || fesetround(rounding[rm]))
        recomp_fail("host floating-point environment unavailable", ctx->pc);
#if defined(__SSE2__)
    /* Host DAZ/FTZ would corrupt guest subnormals and underflow detection.
     * fesetenv below restores the complete saved MXCSR on our Linux host. */
    _mm_setcsr(_mm_getcsr() & ~UINT32_C(0x8040));
#endif
    return env;
}

static unsigned end(MipsContext *ctx, const FpEnvironment *env) {
#if USE_INTRISICS
    if (env->sse_only) {
        unsigned flags = _mm_getcsr();
        _mm_setcsr(env->saved_mxcsr);
        errno = env->saved_errno;
        return ((flags & 0x20) ? FP_I : 0) | ((flags & 0x10) ? FP_U : 0)
             | ((flags & 0x08) ? FP_O : 0) | ((flags & 0x04) ? FP_Z : 0)
             | ((flags & 0x01) ? FP_V : 0);
    }
#endif
    int flags = fetestexcept(FE_ALL_EXCEPT);
    if (fesetenv(&env->saved)) recomp_fail("cannot restore host floating-point environment", ctx->pc);
    errno = env->saved_errno;
    return ((flags & FE_INEXACT) ? FP_I : 0) | ((flags & FE_UNDERFLOW) ? FP_U : 0)
         | ((flags & FE_OVERFLOW) ? FP_O : 0) | ((flags & FE_DIVBYZERO) ? FP_Z : 0)
         | ((flags & FE_INVALID) ? FP_V : 0);
}

static void finish(MipsContext *ctx, unsigned flags) {
    ctx->fcr31 = (ctx->fcr31 & ~UINT32_C(0x3f000)) | (flags << 12);
    if ((flags & FP_E) || (flags & (ctx->fcr31 >> 7) & 31))
        recomp_fail((flags & FP_E) ? "unsupported VR4300 floating-point trap (E)"
                                  : "enabled VR4300 floating-point exception", ctx->pc);
    ctx->fcr31 |= flags << 2;
}

void mips_fp_set_control(MipsContext *ctx, uint32_t value) {
    ctx->fcr31 = value & UINT32_C(0x0183ffff);
    if ((ctx->fcr31 & UINT32_C(0x20000)) || ((ctx->fcr31 >> 12) & (ctx->fcr31 >> 7) & 31))
        recomp_fail("VR4300 floating-point trap on CTC1", ctx->pc);
}

static uint64_t read_bits(MipsContext *ctx, unsigned reg, enum MipsFpFormat fmt) {
    return (fmt == MIPS_FP_D || fmt == MIPS_FP_L) ? mips_fpr_pair(ctx, reg) : ctx->fpr[reg];
}
static void write_bits(MipsContext *ctx, unsigned reg, enum MipsFpFormat fmt, uint64_t bits) {
    if (fmt == MIPS_FP_D || fmt == MIPS_FP_L) mips_set_fpr_pair(ctx, reg, bits);
    else ctx->fpr[reg] = (uint32_t)bits;
}
static uint64_t sign_mask(enum MipsFpFormat fmt) {
    return fmt == MIPS_FP_D ? UINT64_C(0x8000000000000000) : UINT64_C(0x80000000);
}
static uint64_t exponent_mask(enum MipsFpFormat fmt) {
    return fmt == MIPS_FP_D ? UINT64_C(0x7ff0000000000000) : UINT64_C(0x7f800000);
}
static uint64_t fraction_mask(enum MipsFpFormat fmt) {
    return fmt == MIPS_FP_D ? UINT64_C(0xfffffffffffff) : UINT64_C(0x7fffff);
}
static bool nan_bits(uint64_t bits, enum MipsFpFormat fmt) {
    return (bits & exponent_mask(fmt)) == exponent_mask(fmt) && (bits & fraction_mask(fmt));
}
static bool signaling(uint64_t bits, enum MipsFpFormat fmt) {
    return nan_bits(bits, fmt) && (bits & (fmt == MIPS_FP_D ? UINT64_C(0x8000000000000) : UINT64_C(0x400000)));
}
static bool denormal(uint64_t bits, enum MipsFpFormat fmt) {
    return !(bits & exponent_mask(fmt)) && (bits & fraction_mask(fmt));
}
static uint64_t quiet_nan(enum MipsFpFormat fmt) {
    /* Legacy MIPS quiet/signaling bit is opposite to the host convention. */
    return fmt == MIPS_FP_D ? UINT64_C(0x7ff7ffffffffffff) : UINT64_C(0x7fbfffff);
}
static unsigned operand_flags(uint64_t bits, enum MipsFpFormat fmt) {
    if (denormal(bits, fmt) || (nan_bits(bits, fmt) && !signaling(bits, fmt))) return FP_E;
    return signaling(bits, fmt) ? FP_V : 0;
}
static float as_float(uint32_t bits) { float x; memcpy(&x, &bits, 4); return x; }
static double as_double(uint64_t bits) { double x; memcpy(&x, &bits, 8); return x; }
static uint32_t float_bits(float x) { uint32_t b; memcpy(&b, &x, 4); return b; }
static uint64_t double_bits(double x) { uint64_t b; memcpy(&b, &x, 8); return b; }

static uint64_t result(MipsContext *ctx, uint64_t bits, enum MipsFpFormat fmt, unsigned flags) {
    uint64_t minimum = fmt == MIPS_FP_D ? UINT64_C(0x10000000000000) : UINT64_C(0x800000);
    if ((flags & FP_I) && (bits & ~sign_mask(fmt)) == minimum)
        /* Host and VR4300 tininess detection can disagree at this boundary.
         * Require explicit support instead of silently copying host behavior. */
        finish(ctx, FP_E);
    if ((flags & FP_U) || denormal(bits, fmt)) {
        if (!(ctx->fcr31 & UINT32_C(0x1000000)) || (ctx->fcr31 & UINT32_C(0x180))) {
            finish(ctx, FP_E);
        }
        /* VR4300 flushes even exact denormal results and records U + I. */
        flags |= FP_U | FP_I;
        bool negative = (bits & sign_mask(fmt)) != 0;
        unsigned mode = ctx->fcr31 & 3;
        bits &= sign_mask(fmt);
        if ((mode == 2 && !negative) || (mode == 3 && negative))
            bits |= minimum;
    }
    if (flags & FP_V) bits = quiet_nan(fmt);
    finish(ctx, flags);
    return bits;
}

void mips_fp_binary(MipsContext *ctx, unsigned fd, unsigned fs, unsigned ft,
                    enum MipsFpFormat fmt, enum MipsFpOperation op) {
    uint64_t a = read_bits(ctx, fs, fmt), b = read_bits(ctx, ft, fmt), bits;
    unsigned flags = operand_flags(a, fmt) | operand_flags(b, fmt);
    if (flags) { finish(ctx, flags); write_bits(ctx, fd, fmt, quiet_nan(fmt)); return; }
    FpEnvironment env = begin(ctx, -1, true);
    if (fmt == MIPS_FP_S) {
        volatile float x = as_float((uint32_t)a), y = as_float((uint32_t)b), z;
#if USE_INTRISICS
        __m128 vx = _mm_set_ss(x), vy = _mm_set_ss(y);
        switch (op) {
        case MIPS_FP_ADD: z = _mm_cvtss_f32(_mm_add_ss(vx, vy)); break;
        case MIPS_FP_SUB: z = _mm_cvtss_f32(_mm_sub_ss(vx, vy)); break;
        case MIPS_FP_MUL: z = _mm_cvtss_f32(_mm_mul_ss(vx, vy)); break;
        case MIPS_FP_DIV: z = _mm_cvtss_f32(_mm_div_ss(vx, vy)); break;
        default: recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#else
        switch (op) {
        case MIPS_FP_ADD: z = x + y; break;
        case MIPS_FP_SUB: z = x - y; break;
        case MIPS_FP_MUL: z = x * y; break;
        case MIPS_FP_DIV: z = x / y; break;
        default: recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#endif
        bits = float_bits(z);
    } else {
        volatile double x = as_double(a), y = as_double(b), z;
#if USE_INTRISICS
        __m128d vx = _mm_set_sd(x), vy = _mm_set_sd(y);
        switch (op) {
        case MIPS_FP_ADD: z = _mm_cvtsd_f64(_mm_add_sd(vx, vy)); break;
        case MIPS_FP_SUB: z = _mm_cvtsd_f64(_mm_sub_sd(vx, vy)); break;
        case MIPS_FP_MUL: z = _mm_cvtsd_f64(_mm_mul_sd(vx, vy)); break;
        case MIPS_FP_DIV: z = _mm_cvtsd_f64(_mm_div_sd(vx, vy)); break;
        default: recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#else
        switch (op) {
        case MIPS_FP_ADD: z = x + y; break;
        case MIPS_FP_SUB: z = x - y; break;
        case MIPS_FP_MUL: z = x * y; break;
        case MIPS_FP_DIV: z = x / y; break;
        default: recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#endif
        bits = double_bits(z);
    }
    flags = end(ctx, &env);
    write_bits(ctx, fd, fmt, result(ctx, bits, fmt, flags));
}

void mips_fp_unary(MipsContext *ctx, unsigned fd, unsigned fs,
                   enum MipsFpFormat fmt, enum MipsFpOperation op) {
    uint64_t bits = read_bits(ctx, fs, fmt);
    if (op == MIPS_FP_MOV) { write_bits(ctx, fd, fmt, bits); return; }
    unsigned flags = operand_flags(bits, fmt);
    if (flags) { finish(ctx, flags); write_bits(ctx, fd, fmt, quiet_nan(fmt)); return; }
    if (op == MIPS_FP_SQRT) {
        FpEnvironment env = begin(ctx, -1, true);
#if USE_INTRISICS
        if (fmt == MIPS_FP_S) {
            volatile float x = as_float((uint32_t)bits);
            volatile float y = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
            bits = float_bits(y);
        } else {
            volatile double x = as_double(bits);
            volatile double y = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)));
            bits = double_bits(y);
        }
#else
        if (fmt == MIPS_FP_S) { volatile float x = as_float((uint32_t)bits), y = sqrtf(x); bits = float_bits(y); }
        else { volatile double x = as_double(bits), y = sqrt(x); bits = double_bits(y); }
#endif
        flags = end(ctx, &env);
    } else if (op == MIPS_FP_ABS) bits &= ~sign_mask(fmt);
    else if (op == MIPS_FP_NEG) bits ^= sign_mask(fmt);
    else recomp_fail("invalid native FP unary operation", ctx->pc);
    write_bits(ctx, fd, fmt, result(ctx, bits, fmt, flags));
}

void mips_fp_convert(MipsContext *ctx, unsigned fd, unsigned fs,
                     enum MipsFpFormat from, enum MipsFpFormat to, int mode) {
    uint64_t bits = read_bits(ctx, fs, from);
    unsigned flags = (from == MIPS_FP_W || from == MIPS_FP_L) ? 0 : operand_flags(bits, from);
    if (flags) {
        /* Nonfinite integer conversion still needs an exception replacement. */
        if (to == MIPS_FP_W || to == MIPS_FP_L) finish(ctx, FP_E);
        finish(ctx, flags); write_bits(ctx, fd, to, quiet_nan(to)); return;
    }
    /* Conversions can call libm, so retain complete host fenv isolation. */
    FpEnvironment env = begin(ctx, mode, false);
    if (from == MIPS_FP_L) {
        /* Decode two's complement without out-of-range signed casts. Convert
         * directly to the requested precision: an intermediate double would
         * double-round some 64-bit integers when the destination is single. */
        volatile int64_t integer = (bits & UINT64_C(0x8000000000000000))
            ? -(int64_t)(~bits) - 1 : (int64_t)bits;
        if (to == MIPS_FP_S) { volatile float value = (float)integer; bits = float_bits(value); }
        else if (to == MIPS_FP_D) { volatile double value = (double)integer; bits = double_bits(value); }
        else recomp_fail("invalid long-integer FP conversion", ctx->pc);
        flags = end(ctx, &env);
        write_bits(ctx, fd, to, result(ctx, bits, to, flags));
        return;
    }
    double source;
    if (from == MIPS_FP_W) {
        /* Defined two's-complement decoding, without out-of-range casts. */
        source = (double)(bits & UINT32_C(0x7fffffff)) - ((bits & UINT32_C(0x80000000)) ? 2147483648.0 : 0.0);
    } else source = from == MIPS_FP_S ? (double)as_float((uint32_t)bits) : as_double(bits);
    if (to == MIPS_FP_W || to == MIPS_FP_L) {
        volatile double x = source, rounded = rint(x);
        flags = end(ctx, &env);
        double limit = to == MIPS_FP_L ? 0x1p63 : 2147483648.0;
        if (!isfinite(rounded) || rounded < -limit || rounded >= limit)
            finish(ctx, FP_E); /* VR4300 out-of-range integer conversions trap even with enables clear. */
        bits = to == MIPS_FP_L ? (uint64_t)(int64_t)rounded : (uint32_t)(int32_t)rounded;
        finish(ctx, flags);
    } else {
        if (to == MIPS_FP_S) { volatile double x = source; volatile float y = (float)x; bits = float_bits(y); }
        else { volatile double y = source; bits = double_bits(y); }
        flags = end(ctx, &env);
        bits = result(ctx, bits, to, flags);
    }
    write_bits(ctx, fd, to, bits);
}

void mips_fp_compare(MipsContext *ctx, unsigned fs, unsigned ft, enum MipsFpFormat fmt, unsigned predicate) {
    uint64_t a = read_bits(ctx, fs, fmt), b = read_bits(ctx, ft, fmt);
    bool unordered = nan_bits(a, fmt) || nan_bits(b, fmt), equal = false, less = false;
    unsigned flags = unordered && ((predicate & 8) || signaling(a, fmt) || signaling(b, fmt)) ? FP_V : 0;
    if (!unordered) {
        FpEnvironment env = begin(ctx, -1, true); /* Comparisons support denormals, independent of host DAZ. */
#if USE_INTRISICS
        if (fmt == MIPS_FP_S) {
            volatile float x = as_float((uint32_t)a), y = as_float((uint32_t)b);
            __m128 vx = _mm_set_ss(x), vy = _mm_set_ss(y);
            equal = _mm_ucomieq_ss(vx, vy);
            less = _mm_ucomilt_ss(vx, vy);
        } else {
            volatile double x = as_double(a), y = as_double(b);
            __m128d vx = _mm_set_sd(x), vy = _mm_set_sd(y);
            equal = _mm_ucomieq_sd(vx, vy);
            less = _mm_ucomilt_sd(vx, vy);
        }
#else
        if (fmt == MIPS_FP_S) { volatile float x = as_float((uint32_t)a), y = as_float((uint32_t)b); equal = x == y; less = x < y; }
        else { volatile double x = as_double(a), y = as_double(b); equal = x == y; less = x < y; }
#endif
        (void)end(ctx, &env);
    }
    finish(ctx, flags);
    bool condition = ((predicate & 1) && unordered) || ((predicate & 2) && equal) || ((predicate & 4) && less);
    ctx->fcr31 = (ctx->fcr31 & ~UINT32_C(0x800000)) | (condition ? UINT32_C(0x800000) : 0);
}
