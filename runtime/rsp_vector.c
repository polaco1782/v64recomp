#include "rsp.h"
#include <limits.h>
#include <pthread.h>
#include <string.h>

static const uint8_t selectors[16][8] = {
    {0, 1, 2, 3, 4, 5, 6, 7}, {0, 1, 2, 3, 4, 5, 6, 7}, {0, 0, 2, 2, 4, 4, 6, 6}, {1, 1, 3, 3, 5, 5, 7, 7},
    {0, 0, 0, 0, 4, 4, 4, 4}, {1, 1, 1, 1, 5, 5, 5, 5}, {2, 2, 2, 2, 6, 6, 6, 6}, {3, 3, 3, 3, 7, 7, 7, 7},
    {0, 0, 0, 0, 0, 0, 0, 0}, {1, 1, 1, 1, 1, 1, 1, 1}, {2, 2, 2, 2, 2, 2, 2, 2}, {3, 3, 3, 3, 3, 3, 3, 3},
    {4, 4, 4, 4, 4, 4, 4, 4}, {5, 5, 5, 5, 5, 5, 5, 5}, {6, 6, 6, 6, 6, 6, 6, 6}, {7, 7, 7, 7, 7, 7, 7, 7}};
static int64_t wrap48(int64_t value) {
    uint64_t bits = (uint64_t)value & UINT64_C(0xffffffffffff);
    return (int64_t)bits - ((bits & UINT64_C(0x800000000000)) ? INT64_C(0x1000000000000) : 0);
}
static uint16_t saturate(int64_t value) {
    return (uint16_t)(value < -32768 ? -32768 : value > 32767 ? 32767 : value);
}
static uint16_t low_saturate(int64_t value) {
    return value < INT32_MIN ? 0 : value > INT32_MAX ? 65535 : (uint16_t)value;
}
static uint16_t unsigned_fraction(int64_t value) {
    return value < 0 ? 0 : value > INT32_MAX ? 65535 : (uint16_t)(value >> 16);
}
static void set_low(RecompRspState *s, unsigned lane, uint16_t value) {
    s->accumulator[lane] = wrap48((int64_t)(((uint64_t)s->accumulator[lane] & UINT64_C(0xffffffff0000)) | value));
}
static void operands(RecompRspState *s, unsigned vs, unsigned vt, unsigned e, uint16_t a[8], uint16_t b[8]) {
    for (unsigned i = 0; i < 8; i++) {
        a[i] = s->v[vs][i];
        b[i] = s->v[vt][selectors[e][i]];
    }
}
static uint8_t vector_byte(const RecompRspState *s, unsigned reg, unsigned byte) {
    return (uint8_t)(s->v[reg][(byte & 15) / 2] >> ((byte & 1) ? 0 : 8));
}
static void vector_store(RecompRspState *s, unsigned reg, unsigned byte, uint8_t value) {
    unsigned lane = (byte & 15) / 2, shift = (byte & 1) ? 0 : 8;
    s->v[reg][lane] = (uint16_t)((s->v[reg][lane] & ~(255u << shift)) | ((unsigned)value << shift));
}
uint32_t recomp_rsp_mfc2(RecompRspState *s, unsigned reg, unsigned element) {
    return (uint32_t)(int32_t)(int16_t)((unsigned)vector_byte(s, reg, element) * 256 +
                                        vector_byte(s, reg, element + 1));
}
void recomp_rsp_mtc2(RecompRspState *s, unsigned reg, unsigned element, uint32_t value) {
    vector_store(s, reg, element, (uint8_t)(value >> 8));
    if (element != 15)
        vector_store(s, reg, element + 1, (uint8_t)value);
}
uint32_t recomp_rsp_cfc2(RecompRspState *s, unsigned reg) {
    uint16_t value = (reg & 3) == 0 ? s->vco : (reg & 3) == 1 ? s->vcc : s->vce;
    return (uint32_t)(int32_t)(int16_t)value;
}
void recomp_rsp_ctc2(RecompRspState *s, unsigned reg, uint32_t value) {
    if ((reg & 3) == 0)
        s->vco = (uint16_t)value;
    else if ((reg & 3) == 1)
        s->vcc = (uint16_t)value;
    else
        s->vce = (uint8_t)value;
}

/* The two input vectors are snapshotted before writing VD, including when a
 * broadcast source aliases the destination. All accumulator writes wrap at 48 bits. */
#define MUL(name, product, accumulate, result)                                                                         \
    void recomp_rsp_##name(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {                     \
        uint16_t a[8], b[8];                                                                                           \
        operands(s, vs, vt, e, a, b);                                                                                  \
        for (unsigned i = 0; i < 8; i++) {                                                                             \
            int64_t p = (product);                                                                                     \
            s->accumulator[i] = wrap48((accumulate) ? s->accumulator[i] + p : p);                                      \
            s->v[vd][i] = (result);                                                                                    \
        }                                                                                                              \
    }
MUL(vmulf, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 2 + 32768, false, saturate(s->accumulator[i] >> 16))
MUL(vmulu, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 2 + 32768, false, unsigned_fraction(s->accumulator[i]))
MUL(vmudl, ((uint32_t)a[i] * b[i]) >> 16, false, (uint16_t)s->accumulator[i])
MUL(vmudm, (int64_t)(int16_t)a[i] * b[i], false, (uint16_t)(s->accumulator[i] >> 16))
MUL(vmudn, (int64_t)a[i] * (int16_t)b[i], false, (uint16_t)s->accumulator[i])
MUL(vmudh, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 65536, false, saturate(s->accumulator[i] >> 16))
MUL(vmacf, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 2, true, saturate(s->accumulator[i] >> 16))
MUL(vmacu, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 2, true, unsigned_fraction(s->accumulator[i]))
MUL(vmadl, ((uint32_t)a[i] * b[i]) >> 16, true, low_saturate(s->accumulator[i]))
MUL(vmadm, (int64_t)(int16_t)a[i] * b[i], true, saturate(s->accumulator[i] >> 16))
MUL(vmadn, (int64_t)a[i] * (int16_t)b[i], true, low_saturate(s->accumulator[i]))
MUL(vmadh, (int64_t)(int16_t)a[i] * (int16_t)b[i] * 65536, true, saturate(s->accumulator[i] >> 16))
#undef MUL

static inline void round_accumulator(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e,
                                     bool positive) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++) {
        int64_t value = (int16_t)b[i];
        if (vs & 1)
            value *= 65536;
        if ((s->accumulator[i] >= 0) == positive)
            s->accumulator[i] = wrap48(s->accumulator[i] + value);
        s->v[vd][i] = saturate(s->accumulator[i] >> 16);
    }
}
void recomp_rsp_vrndp(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    round_accumulator(s, d, a, b, e, true);
}
void recomp_rsp_vrndn(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    round_accumulator(s, d, a, b, e, false);
}
void recomp_rsp_vmulq(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++) {
        int64_t p = (int64_t)(int16_t)a[i] * (int16_t)b[i];
        if (p < 0)
            p += 31;
        s->accumulator[i] = p * 65536;
        s->v[vd][i] = saturate(p >> 1) & 0xfff0;
    }
}
void recomp_rsp_vmacq(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    (void)vs;
    (void)vt;
    (void)e;
    for (unsigned i = 0; i < 8; i++) {
        int64_t p = s->accumulator[i] >> 16;
        if (!(p & 32))
            p += p < 0 ? 32 : p >= 32 ? -32 : 0;
        s->accumulator[i] = wrap48(p * 65536 + (uint16_t)s->accumulator[i]);
        s->v[vd][i] = saturate(p >> 1) & 0xfff0;
    }
}
/* Specialize mode flags in each C11 handler: no wrapper-to-helper call or
 * per-lane mode dispatch remains, including in builds that outline inline hints. */
#define ADD_SUB(name, subtract, carry)                                                                                 \
    void recomp_rsp_##name(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {                     \
        uint16_t a[8], b[8];                                                                                           \
        operands(s, vs, vt, e, a, b);                                                                                  \
        uint16_t flags = 0;                                                                                            \
        for (unsigned i = 0; i < 8; i++) {                                                                             \
            int32_t left = carry ? a[i] : (int16_t)a[i], right = carry ? b[i] : (int16_t)b[i];                         \
            int32_t value = subtract ? left - right : left + right;                                                    \
            if (!carry)                                                                                                \
                value += subtract ? -(int32_t)((s->vco >> i) & 1) : (int32_t)((s->vco >> i) & 1);                      \
            set_low(s, i, (uint16_t)value);                                                                            \
            s->v[vd][i] = carry ? (uint16_t)value : saturate(value);                                                   \
            if (carry && (subtract ? value < 0 : value > 65535))                                                       \
                flags |= (uint16_t)(1u << i);                                                                          \
            if (carry && subtract && value)                                                                            \
                flags |= (uint16_t)(1u << (8 + i));                                                                    \
        }                                                                                                              \
        s->vco = flags;                                                                                                \
    }
ADD_SUB(vadd, false, false)
ADD_SUB(vsub, true, false)
ADD_SUB(vaddc, false, true)
ADD_SUB(vsubc, true, true)
#undef ADD_SUB

void recomp_rsp_vabs(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++) {
        int value = (int16_t)a[i] < 0 ? -(int16_t)b[i] : a[i] ? (int16_t)b[i] : 0;
        set_low(s, i, (uint16_t)value);
        s->v[vd][i] = saturate(value);
    }
}
void recomp_rsp_vsar(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    (void)vs;
    (void)vt;
    for (unsigned i = 0; i < 8; i++)
        s->v[vd][i] = e >= 8 && e <= 10 ? (uint16_t)((uint64_t)s->accumulator[i] >> ((10 - e) * 16)) : 0;
}
#define LOGIC(name, expression)                                                                                        \
    void recomp_rsp_##name(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {                     \
        uint16_t a[8], b[8];                                                                                           \
        operands(s, vs, vt, e, a, b);                                                                                  \
        for (unsigned i = 0; i < 8; i++) {                                                                             \
            uint16_t value = (uint16_t)(expression);                                                                   \
            set_low(s, i, value);                                                                                      \
            s->v[vd][i] = value;                                                                                       \
        }                                                                                                              \
    }
LOGIC(vand, a[i] & b[i])
LOGIC(vnand, ~(a[i] & b[i]))
LOGIC(vor, a[i] | b[i])
LOGIC(vnor, ~(a[i] | b[i]))
LOGIC(vxor, a[i] ^ b[i])
LOGIC(vnxor, ~(a[i] ^ b[i]))
#undef LOGIC
#define COMPARE(name, operation)                                                                                       \
    void recomp_rsp_##name(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {                     \
        uint16_t a[8], b[8];                                                                                           \
        operands(s, vs, vt, e, a, b);                                                                                  \
        uint16_t flags = 0;                                                                                            \
        for (unsigned i = 0; i < 8; i++) {                                                                             \
            bool equal = a[i] == b[i], low = ((s->vco >> i) & 1) != 0, high = ((s->vco >> (i + 8)) & 1) != 0;          \
            bool choose;                                                                                               \
            switch (operation) {                                                                                       \
                case 0:                                                                                                \
                    choose = (int16_t)a[i] < (int16_t)b[i] || (equal && low && high);                                  \
                    break;                                                                                             \
                case 1:                                                                                                \
                    choose = equal && !high;                                                                           \
                    break;                                                                                             \
                case 2:                                                                                                \
                    choose = !equal || high;                                                                           \
                    break;                                                                                             \
                default:                                                                                               \
                    choose = (int16_t)a[i] > (int16_t)b[i] || (equal && !(low && high));                               \
                    break;                                                                                             \
            }                                                                                                          \
            if (choose)                                                                                                \
                flags |= (uint16_t)(1u << i);                                                                          \
            uint16_t value = choose ? a[i] : b[i];                                                                     \
            set_low(s, i, value);                                                                                      \
            s->v[vd][i] = value;                                                                                       \
        }                                                                                                              \
        s->vcc = flags;                                                                                                \
        s->vco = 0;                                                                                                    \
    }
COMPARE(vlt, 0)
COMPARE(veq, 1)
COMPARE(vne, 2)
COMPARE(vge, 3)
#undef COMPARE

void recomp_rsp_vmrg(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++) {
        uint16_t v = (s->vcc & (1u << i)) ? a[i] : b[i];
        set_low(s, i, v);
        s->v[vd][i] = v;
    }
    s->vco = 0;
}
void recomp_rsp_vch(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    s->vco = s->vcc = 0;
    s->vce = 0;
    for (unsigned i = 0; i < 8; i++) {
        int left = (int16_t)a[i], right = (int16_t)b[i];
        bool opposite = (left < 0) != (right < 0);
        int16_t delta = (int16_t)(opposite ? left + right : left - right);
        bool lo = opposite ? delta <= 0 : right < 0, hi = opposite ? right < 0 : delta >= 0;
        if (lo)
            s->vcc |= (uint16_t)(1u << i);
        if (hi)
            s->vcc |= (uint16_t)(1u << (i + 8));
        if (opposite)
            s->vco |= (uint16_t)(1u << i);
        if (delta && a[i] != (uint16_t)~b[i])
            s->vco |= (uint16_t)(1u << (i + 8));
        if (opposite && delta == -1)
            s->vce |= (uint8_t)(1u << i);
        uint16_t v = opposite ? (lo ? (uint16_t)-b[i] : a[i]) : (hi ? b[i] : a[i]);
        set_low(s, i, v);
        s->v[vd][i] = v;
    }
}
void recomp_rsp_vcl(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++) {
        bool lo = (s->vcc & (1u << i)) != 0, hi = (s->vcc & (1u << (i + 8))) != 0;
        bool sign = (s->vco & (1u << i)) != 0, unequal = (s->vco & (1u << (i + 8))) != 0;
        if (!unequal) {
            if (sign) {
                unsigned sum = (unsigned)a[i] + b[i];
                bool zero = (uint16_t)sum == 0, carry = sum > 65535;
                lo = (s->vce & (1u << i)) ? (zero || !carry) : (zero && !carry);
                s->vcc = (uint16_t)((s->vcc & ~(1u << i)) | ((unsigned)lo << i));
            } else {
                hi = a[i] >= b[i];
                s->vcc = (uint16_t)((s->vcc & ~(1u << (i + 8))) | ((unsigned)hi << (i + 8)));
            }
        }
        uint16_t v = sign ? (lo ? (uint16_t)-b[i] : a[i]) : (hi ? b[i] : a[i]);
        set_low(s, i, v);
        s->v[vd][i] = v;
    }
    s->vco = 0;
    s->vce = 0;
}
void recomp_rsp_vcr(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    s->vcc = s->vco = 0;
    s->vce = 0;
    for (unsigned i = 0; i < 8; i++) {
        int left = (int16_t)a[i], right = (int16_t)b[i];
        bool opposite = (left < 0) != (right < 0);
        bool lo = opposite ? left + right + 1 <= 0 : right < 0, hi = opposite ? right < 0 : left - right >= 0;
        if (lo)
            s->vcc |= (uint16_t)(1u << i);
        if (hi)
            s->vcc |= (uint16_t)(1u << (i + 8));
        uint16_t v = opposite ? (lo ? (uint16_t)~b[i] : a[i]) : (hi ? b[i] : a[i]);
        set_low(s, i, v);
        s->v[vd][i] = v;
    }
}

static uint16_t reciprocals[512], square_roots[512];
static pthread_once_t tables_once = PTHREAD_ONCE_INIT;
static void make_tables(void) {
    reciprocals[0] = 65535;
    for (unsigned i = 1; i < 512; i++)
        reciprocals[i] = (uint16_t)((((UINT64_C(1) << 34) / (i + 512)) + 1) >> 8);
    for (unsigned i = 0; i < 512; i++) {
        uint64_t divisor = (i + 512) >> ((i & 1) ? 1 : 0), lo = UINT64_C(1) << 17, hi = UINT64_C(1) << 18;
        while (lo + 1 < hi) {
            uint64_t mid = (lo + hi) / 2;
            if (divisor * mid * mid < (UINT64_C(1) << 44))
                lo = mid;
            else
                hi = mid;
        }
        square_roots[i] = (uint16_t)(lo >> 1);
    }
}
void recomp_rsp_vector_tables_init(void) {
    (void)pthread_once(&tables_once, make_tables);
}
static inline void divide_vector(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e, bool root,
                                 bool low, bool high) {
    uint16_t source = s->v[vt][e & 7], a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++)
        set_low(s, i, b[i]);
    if (high) {
        s->v[vd][vs & 7] = (uint16_t)s->div_output;
        s->div_input = source;
        s->div_pending = true;
        return;
    }
    uint32_t bits = low && s->div_pending ? (s->div_input << 16) | source : (uint32_t)(int32_t)(int16_t)source;
    int64_t signed_input = (int64_t)bits - ((bits & 0x80000000u) ? INT64_C(0x100000000) : 0);
    uint32_t mask = signed_input < 0 ? UINT32_MAX : 0, absolute = bits ^ mask;
    if (signed_input > -32768)
        absolute -= mask;
    uint32_t result;
    if (!absolute)
        result = 0x7fffffff;
    else if (signed_input == -32768)
        result = 0xffff0000;
    else {
        unsigned shift = 0;
        for (uint32_t n = absolute; !(n & 0x80000000u); n <<= 1)
            shift++;
        unsigned index = (unsigned)(((uint64_t)absolute << shift) & 0x7fc00000u) >> 22;
        uint32_t table = root ? square_roots[(index & 510) | (shift & 1)] : reciprocals[index];
        result = ((0x10000u | table) << 14) >> (root ? ((31 - shift) / 2) : (31 - shift));
        result ^= mask;
    }
    s->div_pending = false;
    s->div_output = result >> 16;
    s->v[vd][vs & 7] = (uint16_t)result;
}
void recomp_rsp_vrcp(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, false, false, false);
}
void recomp_rsp_vrcpl(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, false, true, false);
}
void recomp_rsp_vrcph(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, false, false, true);
}
void recomp_rsp_vrsq(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, true, false, false);
}
void recomp_rsp_vrsql(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, true, true, false);
}
void recomp_rsp_vrsqh(RecompRspState *s, unsigned d, unsigned a, unsigned b, unsigned e) {
    divide_vector(s, d, a, b, e, true, false, true);
}
void recomp_rsp_vmov(RecompRspState *s, unsigned vd, unsigned vs, unsigned vt, unsigned e) {
    uint16_t a[8], b[8];
    operands(s, vs, vt, e, a, b);
    for (unsigned i = 0; i < 8; i++)
        set_low(s, i, b[i]);
    s->v[vd][vs & 7] = b[vs & 7];
}

static inline void load_bytes(RecompRspState *s, unsigned reg, unsigned element, uint32_t address, unsigned count) {
    if (count > 16 - element)
        count = 16 - element;
    for (unsigned i = 0; i < count; i++)
        vector_store(s, reg, element + i, s->dmem[(address + i) & 4095]);
}
static inline void store_bytes(RecompRspState *s, unsigned reg, unsigned element, uint32_t address, unsigned count) {
    for (unsigned i = 0; i < count; i++)
        s->dmem[(address + i) & 4095] = vector_byte(s, reg, element + i);
}
#define BYTES(name, count)                                                                                             \
    void recomp_rsp_l##name(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {                                   \
        load_bytes(s, r, e, a, count);                                                                                 \
    }                                                                                                                  \
    void recomp_rsp_s##name(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {                                   \
        store_bytes(s, r, e, a, count);                                                                                \
    }
BYTES(bv, 1)
BYTES(sv, 2)
BYTES(lv, 4)
BYTES(dv, 8)
#undef BYTES
void recomp_rsp_lqv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    load_bytes(s, r, e, a, 16 - (a & 15));
}
void recomp_rsp_sqv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    store_bytes(s, r, e, a, 16 - (a & 15));
}
void recomp_rsp_lrv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    unsigned count = (a & 15) > e ? (a & 15) - e : 0;
    load_bytes(s, r, 16 - count, a & ~15u, count);
}
void recomp_rsp_srv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    unsigned count = a & 15;
    store_bytes(s, r, e + 16 - count, a & ~15u, count);
}
static inline void load_packed(RecompRspState *s, unsigned r, unsigned e, uint32_t a, unsigned shift, unsigned step) {
    uint32_t base = a & ~7u, offset = (a & 7) - e;
    for (unsigned i = 0; i < 8; i++)
        s->v[r][i] = (uint16_t)((unsigned)s->dmem[(base + ((offset + i * step) & 15)) & 4095] << shift);
}
void recomp_rsp_lpv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    load_packed(s, r, e, a, 8, 1);
}
void recomp_rsp_luv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    load_packed(s, r, e, a, 7, 1);
}
void recomp_rsp_lhv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    load_packed(s, r, e, a, 7, 2);
}
void recomp_rsp_spv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    for (unsigned i = 0; i < 8; i++) {
        unsigned index = e + i;
        unsigned shift = (index & 15) < 8 ? 8 : 7;
        s->dmem[(a + i) & 4095] = (uint8_t)(s->v[r][index & 7] >> shift);
    }
}
void recomp_rsp_suv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    for (unsigned i = 0; i < 8; i++) {
        unsigned index = e + i;
        unsigned shift = (index & 15) < 8 ? 7 : 8;
        s->dmem[(a + i) & 4095] = (uint8_t)(s->v[r][index & 7] >> shift);
    }
}
void recomp_rsp_shv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    uint32_t base = a & ~7u, offset = a & 7;
    for (unsigned i = 0; i < 8; i++) {
        unsigned byte = e + i * 2, value = (unsigned)vector_byte(s, r, byte) * 2 + (vector_byte(s, r, byte + 1) >> 7);
        s->dmem[(base + ((offset + i * 2) & 15)) & 4095] = (uint8_t)value;
    }
}
void recomp_rsp_lfv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    uint16_t temporary[8];
    uint32_t base = a & ~7u, offset = (a & 7) - e;
    for (unsigned i = 0; i < 8; i++)
        temporary[i] =
            (uint16_t)((unsigned)s->dmem[(base + ((offset + (i & 3) * 4 + (i >= 4 ? 8 : 0)) & 15)) & 4095] << 7);
    unsigned count = e + 8 > 16 ? 16 - e : 8;
    for (unsigned i = 0; i < count; i++) {
        unsigned byte = e + i;
        vector_store(s, r, byte, (uint8_t)(temporary[byte / 2] >> ((byte & 1) ? 0 : 8)));
    }
}
void recomp_rsp_sfv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    static const int first[16] = {0, 6, -1, -1, 1, 7, -1, -1, 4, -1, -1, 3, 5, -1, -1, 0};
    uint32_t base = a & ~7u, offset = a & 7;
    for (unsigned i = 0; i < 4; i++) {
        unsigned lane = first[e] < 0 ? 0 : ((unsigned)first[e] & 4) | (((unsigned)first[e] + i) & 3);
        s->dmem[(base + ((offset + i * 4) & 15)) & 4095] = first[e] < 0 ? 0 : (uint8_t)(s->v[r][lane] >> 7);
    }
}
void recomp_rsp_lwv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    for (unsigned i = 0; i < e * 2; i++)
        vector_store(s, r, 16 - e + i, s->dmem[(a + i * 4) & 4095]);
}
void recomp_rsp_swv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    uint32_t base = a & ~7u, offset = a & 7;
    for (unsigned i = 0; i < 16; i++)
        s->dmem[(base + ((offset + i) & 15)) & 4095] = vector_byte(s, r, e + i);
}
void recomp_rsp_ltv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    uint32_t base = a & ~7u, offset = (e + (a & 8)) & 15;
    for (unsigned i = 0; i < 8; i++)
        for (unsigned byte = 0; byte < 2; byte++) {
            unsigned reg = (r & ~7u) + ((e / 2 + i) & 7);
            vector_store(s, reg, i * 2 + byte, s->dmem[(base + ((offset + i * 2 + byte) & 15)) & 4095]);
        }
}
void recomp_rsp_stv(RecompRspState *s, unsigned r, unsigned e, uint32_t a) {
    uint32_t base = a & ~7u, offset = (a & 7) - (e & ~1u);
    unsigned element = 16 - (e & ~1u);
    for (unsigned i = 0; i < 8; i++)
        for (unsigned byte = 0; byte < 2; byte++)
            s->dmem[(base + ((offset + i * 2 + byte) & 15)) & 4095] =
                vector_byte(s, (r & ~7u) + i, element + i * 2 + byte);
}
