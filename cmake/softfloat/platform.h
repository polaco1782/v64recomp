/* Portable C11 configuration for Berkeley SoftFloat (not an upstream file). */
#ifndef RECOMP_SOFTFLOAT_PLATFORM_H
#define RECOMP_SOFTFLOAT_PLATFORM_H
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
#define LITTLEENDIAN 1
#endif
#define INLINE static inline
#endif
