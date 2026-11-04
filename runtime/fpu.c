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
#if defined(RECOMP_FPU_SPECIALIZE) && defined(__clang__)
#define RECOMP_FPU_INLINE __attribute__((always_inline))
#else
#define RECOMP_FPU_INLINE
#endif
#ifdef RECOMP_SOFTFLOAT
#include "softfloat.h"
#define RECOMP_FP_SSE 0
#elif (defined(__x86_64__) && defined(__SSE2__)) || defined(_M_X64)
#define RECOMP_FP_SSE USE_INTRISICS
#elif defined(__aarch64__) && defined(__linux__)
#define RECOMP_FP_SSE 0
#else
#error "Floating-point backend requires x86-64 SSE2 or Linux AArch64"
#endif
#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif
#if defined(__clang__) && !defined(RECOMP_SOFTFLOAT)
#pragma STDC FENV_ACCESS ON
#endif

_Static_assert(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 && DBL_MANT_DIG == 53 &&
                   DBL_MAX_EXP == 1024 && sizeof(float) == 4 && sizeof(double) == 8,
               "floating-point runtime requires IEEE binary32/binary64");

enum { FP_I = 1, FP_U = 2, FP_O = 4, FP_Z = 8, FP_V = 16, FP_E = 32 };
#ifdef RECOMP_SOFTFLOAT
static const uint_fast8_t rounding[] = {softfloat_round_near_even, softfloat_round_minMag, softfloat_round_max,
                                        softfloat_round_min};
typedef struct FpEnvironment {
    uint_fast8_t rounding, flags, tininess;
} FpEnvironment;
static FpEnvironment begin(MipsContext *ctx, int mode, bool sse_only) {
    (void)sse_only;
    FpEnvironment env = {softfloat_roundingMode, softfloat_exceptionFlags, softfloat_detectTininess};
    softfloat_roundingMode = rounding[mode < 0 ? ctx->fcr31 & 3 : (unsigned)mode];
    softfloat_exceptionFlags = 0;
    softfloat_detectTininess = softfloat_tininess_afterRounding;
    return env;
}
static unsigned end(MipsContext *ctx, const FpEnvironment *env) {
    (void)ctx;
    uint_fast8_t flags = softfloat_exceptionFlags;
    softfloat_roundingMode = env->rounding;
    softfloat_exceptionFlags = env->flags;
    softfloat_detectTininess = env->tininess;
    return ((flags & softfloat_flag_inexact) ? FP_I : 0) | ((flags & softfloat_flag_underflow) ? FP_U : 0) |
           ((flags & softfloat_flag_overflow) ? FP_O : 0) | ((flags & softfloat_flag_infinite) ? FP_Z : 0) |
           ((flags & softfloat_flag_invalid) ? FP_V : 0);
}
#else
static const int rounding[] = {FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD};
typedef struct FpEnvironment {
    fenv_t saved;
    int saved_errno;
#if RECOMP_FP_SSE
    unsigned saved_mxcsr;
    bool sse_only;
#endif
} FpEnvironment;

static FpEnvironment begin(MipsContext *ctx, int mode, bool sse_only) {
    FpEnvironment env;
    unsigned rm = mode < 0 ? ctx->fcr31 & 3 : (unsigned)mode;
#if RECOMP_FP_SSE
    env.sse_only = sse_only;
    if (sse_only) {
        static const unsigned mxcsr_rounding[] = {0x0000, 0x6000, 0x4000, 0x2000};
        env.saved_mxcsr = _mm_getcsr();
        /* Clear flags, DAZ/FTZ and rounding, then mask all host SSE traps.
         * Explicit scalar intrinsics keep these operations out of the x87 FPU. */
        _mm_setcsr((env.saved_mxcsr & ~UINT32_C(0xe07f)) | UINT32_C(0x1f80) | mxcsr_rounding[rm]);
        return env;
    }
#else
    (void)sse_only;
#endif
    /* Scalar SSE intrinsics cannot change errno. Only the C/libm fallback
     * needs to save and restore it around host floating-point operations. */
    env.saved_errno = errno;
    if (feholdexcept(&env.saved) || fesetround(rounding[rm]))
        recomp_fail("host floating-point environment unavailable", ctx->pc);
#if defined(__SSE2__) || defined(_M_X64)
    /* Host DAZ/FTZ would corrupt guest subnormals and underflow detection.
     * fesetenv below restores the complete saved MXCSR on our Linux host. */
    _mm_setcsr(_mm_getcsr() & ~UINT32_C(0x8040));
#elif defined(__aarch64__)
    /* libc saves FPCR/FPSR, masks traps and selects rounding. Clear inherited
     * flush/default-NaN/alternative modes as well; fesetenv restores all of
     * them. FIZ/AH are RES0 on Armv8.0 but must be clear on newer hosts. */
    uint64_t control;
    __asm__ volatile("mrs %0, fpcr" : "=r"(control));
    control &= ~UINT64_C(0x07080003);
    __asm__ volatile("msr fpcr, %0" : : "r"(control) : "memory");
#endif
    return env;
}

static unsigned end(MipsContext *ctx, const FpEnvironment *env) {
#if RECOMP_FP_SSE
    if (env->sse_only) {
        unsigned flags = _mm_getcsr();
        _mm_setcsr(env->saved_mxcsr);
        return ((flags & 0x20) ? FP_I : 0) | ((flags & 0x10) ? FP_U : 0) | ((flags & 0x08) ? FP_O : 0) |
               ((flags & 0x04) ? FP_Z : 0) | ((flags & 0x01) ? FP_V : 0);
    }
#endif
    int flags = fetestexcept(FE_ALL_EXCEPT);
    if (fesetenv(&env->saved))
        recomp_fail("cannot restore host floating-point environment", ctx->pc);
    errno = env->saved_errno;
    return ((flags & FE_INEXACT) ? FP_I : 0) | ((flags & FE_UNDERFLOW) ? FP_U : 0) |
           ((flags & FE_OVERFLOW) ? FP_O : 0) | ((flags & FE_DIVBYZERO) ? FP_Z : 0) | ((flags & FE_INVALID) ? FP_V : 0);
}

#endif

static void finish(MipsContext *ctx, unsigned flags) {
    ctx->fcr31 = (ctx->fcr31 & ~UINT32_C(0x3f000)) | (flags << 12);
    if ((flags & FP_E) || (flags & (ctx->fcr31 >> 7) & 31))
        recomp_fail((flags & FP_E) ? "unsupported VR4300 floating-point trap (E)"
                                   : "enabled VR4300 floating-point exception",
                    ctx->pc);
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
    if (fmt == MIPS_FP_D || fmt == MIPS_FP_L)
        mips_set_fpr_pair(ctx, reg, bits);
    else
        ctx->fpr[reg] = (uint32_t)bits;
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
    if (denormal(bits, fmt) || (nan_bits(bits, fmt) && !signaling(bits, fmt)))
        return FP_E;
    return signaling(bits, fmt) ? FP_V : 0;
}
#ifndef RECOMP_SOFTFLOAT
static float as_float(uint32_t bits) {
    float x;
    memcpy(&x, &bits, 4);
    return x;
}
static double as_double(uint64_t bits) {
    double x;
    memcpy(&x, &bits, 8);
    return x;
}
static uint32_t float_bits(float x) {
    uint32_t b;
    memcpy(&b, &x, 4);
    return b;
}
static uint64_t double_bits(double x) {
    uint64_t b;
    memcpy(&b, &x, 8);
    return b;
}

#endif

static uint64_t result(MipsContext *ctx, uint64_t bits, enum MipsFpFormat fmt, unsigned flags) {
    uint64_t minimum = fmt == MIPS_FP_D ? UINT64_C(0x10000000000000) : UINT64_C(0x800000);
    if ((flags & FP_U) && (bits & ~sign_mask(fmt)) == minimum)
        /* A tiny result can round up to minimum normal. Keep that boundary
         * fail-closed until independently classified, but ordinary inexact
         * normal results (no host underflow) need only the Inexact handling. */
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
    if (flags & FP_V)
        bits = quiet_nan(fmt);
    finish(ctx, flags);
    return bits;
}

RECOMP_FPU_INLINE void mips_fp_binary(MipsContext *ctx, unsigned fd, unsigned fs, unsigned ft, enum MipsFpFormat fmt,
                                      enum MipsFpOperation op) {
    uint64_t a = read_bits(ctx, fs, fmt), b = read_bits(ctx, ft, fmt), bits;
    unsigned flags = operand_flags(a, fmt) | operand_flags(b, fmt);
    if (flags) {
        finish(ctx, flags);
        write_bits(ctx, fd, fmt, quiet_nan(fmt));
        return;
    }
    FpEnvironment env = begin(ctx, -1, true);
#ifdef RECOMP_SOFTFLOAT
    if (fmt == MIPS_FP_S) {
        float32_t x = {(uint32_t)a}, y = {(uint32_t)b}, z;
        switch (op) {
            case MIPS_FP_ADD:
                z = f32_add(x, y);
                break;
            case MIPS_FP_SUB:
                z = f32_sub(x, y);
                break;
            case MIPS_FP_MUL:
                z = f32_mul(x, y);
                break;
            case MIPS_FP_DIV:
                z = f32_div(x, y);
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
        bits = z.v;
    } else {
        float64_t x = {a}, y = {b}, z;
        switch (op) {
            case MIPS_FP_ADD:
                z = f64_add(x, y);
                break;
            case MIPS_FP_SUB:
                z = f64_sub(x, y);
                break;
            case MIPS_FP_MUL:
                z = f64_mul(x, y);
                break;
            case MIPS_FP_DIV:
                z = f64_div(x, y);
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
        bits = z.v;
    }
#else
    if (fmt == MIPS_FP_S) {
        volatile float x = as_float((uint32_t)a), y = as_float((uint32_t)b), z;
#if RECOMP_FP_SSE
        __m128 vx = _mm_set_ss(x), vy = _mm_set_ss(y);
        switch (op) {
            case MIPS_FP_ADD:
                z = _mm_cvtss_f32(_mm_add_ss(vx, vy));
                break;
            case MIPS_FP_SUB:
                z = _mm_cvtss_f32(_mm_sub_ss(vx, vy));
                break;
            case MIPS_FP_MUL:
                z = _mm_cvtss_f32(_mm_mul_ss(vx, vy));
                break;
            case MIPS_FP_DIV:
                z = _mm_cvtss_f32(_mm_div_ss(vx, vy));
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#else
        switch (op) {
            case MIPS_FP_ADD:
                z = x + y;
                break;
            case MIPS_FP_SUB:
                z = x - y;
                break;
            case MIPS_FP_MUL:
                z = x * y;
                break;
            case MIPS_FP_DIV:
                z = x / y;
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#endif
        bits = float_bits(z);
    } else {
        volatile double x = as_double(a), y = as_double(b), z;
#if RECOMP_FP_SSE
        __m128d vx = _mm_set_sd(x), vy = _mm_set_sd(y);
        switch (op) {
            case MIPS_FP_ADD:
                z = _mm_cvtsd_f64(_mm_add_sd(vx, vy));
                break;
            case MIPS_FP_SUB:
                z = _mm_cvtsd_f64(_mm_sub_sd(vx, vy));
                break;
            case MIPS_FP_MUL:
                z = _mm_cvtsd_f64(_mm_mul_sd(vx, vy));
                break;
            case MIPS_FP_DIV:
                z = _mm_cvtsd_f64(_mm_div_sd(vx, vy));
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#else
        switch (op) {
            case MIPS_FP_ADD:
                z = x + y;
                break;
            case MIPS_FP_SUB:
                z = x - y;
                break;
            case MIPS_FP_MUL:
                z = x * y;
                break;
            case MIPS_FP_DIV:
                z = x / y;
                break;
            default:
                recomp_fail("invalid native FP binary operation", ctx->pc);
        }
#endif
        bits = double_bits(z);
    }
#endif
    flags = end(ctx, &env);
    write_bits(ctx, fd, fmt, result(ctx, bits, fmt, flags));
}

void mips_fp_unary(MipsContext *ctx, unsigned fd, unsigned fs, enum MipsFpFormat fmt, enum MipsFpOperation op) {
    uint64_t bits = read_bits(ctx, fs, fmt);
    if (op == MIPS_FP_MOV) {
        write_bits(ctx, fd, fmt, bits);
        return;
    }
    unsigned flags = operand_flags(bits, fmt);
    if (flags) {
        finish(ctx, flags);
        write_bits(ctx, fd, fmt, quiet_nan(fmt));
        return;
    }
    if (op == MIPS_FP_SQRT) {
        FpEnvironment env = begin(ctx, -1, true);
#ifdef RECOMP_SOFTFLOAT
        bits = fmt == MIPS_FP_S ? f32_sqrt((float32_t){(uint32_t)bits}).v : f64_sqrt((float64_t){bits}).v;
#elif RECOMP_FP_SSE
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
        if (fmt == MIPS_FP_S) {
            volatile float x = as_float((uint32_t)bits), y = sqrtf(x);
            bits = float_bits(y);
        } else {
            volatile double x = as_double(bits), y = sqrt(x);
            bits = double_bits(y);
        }
#endif
        flags = end(ctx, &env);
    } else if (op == MIPS_FP_ABS)
        bits &= ~sign_mask(fmt);
    else if (op == MIPS_FP_NEG)
        bits ^= sign_mask(fmt);
    else
        recomp_fail("invalid native FP unary operation", ctx->pc);
    write_bits(ctx, fd, fmt, result(ctx, bits, fmt, flags));
}

void mips_fp_convert(MipsContext *ctx, unsigned fd, unsigned fs, enum MipsFpFormat from, enum MipsFpFormat to,
                     int mode) {
    uint64_t bits = read_bits(ctx, fs, from);
    unsigned flags = (from == MIPS_FP_W || from == MIPS_FP_L) ? 0 : operand_flags(bits, from);
    if (flags) {
        /* Nonfinite integer conversion still needs an exception replacement. */
        if (to == MIPS_FP_W || to == MIPS_FP_L)
            finish(ctx, FP_E);
        finish(ctx, flags);
        write_bits(ctx, fd, to, quiet_nan(to));
        return;
    }
#ifdef RECOMP_SOFTFLOAT
    FpEnvironment env = begin(ctx, mode, false);
    if (from == MIPS_FP_W || from == MIPS_FP_L) {
        uint64_t raw = from == MIPS_FP_W ? mips_s32(bits) : bits;
        int64_t integer = (raw >> 63) ? -(int64_t)(~raw) - 1 : (int64_t)raw;
        if (to == MIPS_FP_S)
            bits = i64_to_f32(integer).v;
        else if (to == MIPS_FP_D)
            bits = i64_to_f64(integer).v;
        else
            recomp_fail("invalid integer FP conversion", ctx->pc);
    } else if (to == MIPS_FP_W) {
        bits = (uint32_t)(from == MIPS_FP_S ? f32_to_i32((float32_t){(uint32_t)bits}, softfloat_roundingMode, true)
                                            : f64_to_i32((float64_t){bits}, softfloat_roundingMode, true));
    } else if (to == MIPS_FP_L) {
        bits = (uint64_t)(from == MIPS_FP_S ? f32_to_i64((float32_t){(uint32_t)bits}, softfloat_roundingMode, true)
                                            : f64_to_i64((float64_t){bits}, softfloat_roundingMode, true));
    } else if (to == MIPS_FP_S)
        bits = f64_to_f32((float64_t){bits}).v;
    else
        bits = f32_to_f64((float32_t){(uint32_t)bits}).v;
    flags = end(ctx, &env);
    if (to == MIPS_FP_W || to == MIPS_FP_L) {
        if (flags & FP_V)
            finish(ctx, FP_E);
        finish(ctx, flags);
    } else
        bits = result(ctx, bits, to, flags);
#elif RECOMP_FP_SSE
    FpEnvironment env = begin(ctx, mode, true);
    if (from == MIPS_FP_W || from == MIPS_FP_L) {
        uint64_t signed_bits = from == MIPS_FP_W ? mips_s32(bits) : bits;
        volatile int64_t integer =
            (signed_bits & UINT64_C(0x8000000000000000)) ? -(int64_t)(~signed_bits) - 1 : (int64_t)signed_bits;
        /* Convert directly to the destination precision to avoid double rounding. */
        if (to == MIPS_FP_S) {
            volatile float value = _mm_cvtss_f32(_mm_cvtsi64_ss(_mm_setzero_ps(), integer));
            bits = float_bits(value);
        } else if (to == MIPS_FP_D) {
            volatile double value = _mm_cvtsd_f64(_mm_cvtsi64_sd(_mm_setzero_pd(), integer));
            bits = double_bits(value);
        } else
            recomp_fail("invalid integer FP conversion", ctx->pc);
    } else if (to == MIPS_FP_W || to == MIPS_FP_L) {
        if (from == MIPS_FP_S) {
            volatile float source = as_float((uint32_t)bits);
            volatile int64_t integer =
                to == MIPS_FP_W ? _mm_cvtss_si32(_mm_set_ss(source)) : _mm_cvtss_si64(_mm_set_ss(source));
            bits = to == MIPS_FP_W ? (uint32_t)integer : (uint64_t)integer;
        } else {
            volatile double source = as_double(bits);
            volatile int64_t integer =
                to == MIPS_FP_W ? _mm_cvtsd_si32(_mm_set_sd(source)) : _mm_cvtsd_si64(_mm_set_sd(source));
            bits = to == MIPS_FP_W ? (uint32_t)integer : (uint64_t)integer;
        }
    } else if (to == MIPS_FP_S) {
        volatile double source = as_double(bits);
        volatile float value = _mm_cvtss_f32(_mm_cvtsd_ss(_mm_setzero_ps(), _mm_set_sd(source)));
        bits = float_bits(value);
    } else {
        volatile float source = as_float((uint32_t)bits);
        volatile double value = _mm_cvtsd_f64(_mm_cvtss_sd(_mm_setzero_pd(), _mm_set_ss(source)));
        bits = double_bits(value);
    }
    flags = end(ctx, &env);
    if (to == MIPS_FP_W || to == MIPS_FP_L) {
        /* Invalid conversion returns INT_MIN on SSE, which is also a valid
         * result. Use the exception flag to distinguish a guest range trap. */
        if (flags & FP_V)
            finish(ctx, FP_E);
        finish(ctx, flags);
    } else
        bits = result(ctx, bits, to, flags);
#else
    /* The C fallback can call libm and requires complete host fenv isolation. */
    FpEnvironment env = begin(ctx, mode, false);
    if (from == MIPS_FP_L) {
        /* Decode two's complement without out-of-range signed casts. Convert
         * directly to the requested precision: an intermediate double would
         * double-round some 64-bit integers when the destination is single. */
        volatile int64_t integer = (bits & UINT64_C(0x8000000000000000)) ? -(int64_t)(~bits) - 1 : (int64_t)bits;
        if (to == MIPS_FP_S) {
            volatile float value = (float)integer;
            bits = float_bits(value);
        } else if (to == MIPS_FP_D) {
            volatile double value = (double)integer;
            bits = double_bits(value);
        } else
            recomp_fail("invalid long-integer FP conversion", ctx->pc);
        flags = end(ctx, &env);
        write_bits(ctx, fd, to, result(ctx, bits, to, flags));
        return;
    }
    double source;
    if (from == MIPS_FP_W) {
        /* Defined two's-complement decoding, without out-of-range casts. */
        int64_t integer =
            (int64_t)(bits & UINT32_C(0x7fffffff)) - ((bits & UINT32_C(0x80000000)) ? INT64_C(2147483648) : 0);
        source = (double)integer;
    } else
        source = from == MIPS_FP_S ? (double)as_float((uint32_t)bits) : as_double(bits);
    if (to == MIPS_FP_W || to == MIPS_FP_L) {
        volatile double x = source, rounded = rint(x);
        flags = end(ctx, &env);
        double limit = to == MIPS_FP_L ? 0x1p63 : 2147483648.0;
        if (!isfinite(rounded) || rounded < -limit || rounded >= limit)
            finish(ctx, FP_E); /* VR4300 out-of-range integer conversions trap even with enables clear. */
        bits = to == MIPS_FP_L ? (uint64_t)(int64_t)rounded : (uint32_t)(int32_t)rounded;
        finish(ctx, flags);
    } else {
        if (to == MIPS_FP_S) {
            volatile double x = source;
            volatile float y = (float)x;
            bits = float_bits(y);
        } else {
            volatile double y = source;
            bits = double_bits(y);
        }
        flags = end(ctx, &env);
        bits = result(ctx, bits, to, flags);
    }
#endif
    write_bits(ctx, fd, to, bits);
}

void mips_fp_compare(MipsContext *ctx, unsigned fs, unsigned ft, enum MipsFpFormat fmt, unsigned predicate) {
    uint64_t a = read_bits(ctx, fs, fmt), b = read_bits(ctx, ft, fmt);
    bool unordered = nan_bits(a, fmt) || nan_bits(b, fmt), equal = false, less = false;
    unsigned flags = unordered && ((predicate & 8) || signaling(a, fmt) || signaling(b, fmt)) ? FP_V : 0;
    if (!unordered) {
#if USE_INTRISICS || defined(RECOMP_SOFTFLOAT)
        /* Non-NaN encodings are ordered by magnitude, reversed for negatives.
         * Treat both signed zeros as equal. Integer comparisons also preserve
         * denormals without changing host rounding, exception flags or DAZ. */
        uint64_t sign = sign_mask(fmt);
        equal = a == b || ((a | b) & ~sign) == 0;
        less = !equal && (((a ^ b) & sign) ? (a & sign) != 0 : (a & sign) ? a > b : a < b);
#else
        FpEnvironment env = begin(ctx, -1, true);
        if (fmt == MIPS_FP_S) {
            volatile float x = as_float((uint32_t)a), y = as_float((uint32_t)b);
            equal = x == y;
            less = x < y;
        } else {
            volatile double x = as_double(a), y = as_double(b);
            equal = x == y;
            less = x < y;
        }
        (void)end(ctx, &env);
#endif
    }
    finish(ctx, flags);
    bool condition = ((predicate & 1) && unordered) || ((predicate & 2) && equal) || ((predicate & 4) && less);
    ctx->fcr31 = (ctx->fcr31 & ~UINT32_C(0x800000)) | (condition ? UINT32_C(0x800000) : 0);
}
