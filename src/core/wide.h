/* Livadix: 64 x 64 -> 128-bit products and 128 / 64-bit quotients for a
 * compiler that has neither unsigned __int128 nor _umul128/_udiv128
 * (MSVC for ARM64).
 *
 * The division is Knuth's Algorithm D (The Art of Computer Programming
 * vol. 2, 4.3.1) on two 32-bit digits: the divisor normalised so that its
 * top bit is set, each quotient digit estimated from the top digits and
 * corrected at most twice. */
#ifndef MC_WIDE_H
#define MC_WIDE_H

#include <stdint.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

/* The low half of a x b; the high half in *hi. */
static inline uint64_t mc_mul128(uint64_t a, uint64_t b, uint64_t *hi) {
#if defined(_MSC_VER) && defined(_M_ARM64)
    *hi = __umulh(a, b);
#else
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
    return a * b;
}

static inline int mc_clz64(uint64_t v) {
#if defined(_MSC_VER) && defined(_M_ARM64)
    return (int)_CountLeadingZeros64(v);
#elif defined(__GNUC__)
    return v ? __builtin_clzll(v) : 64;
#else
    int n = 0;
    if (!v) return 64;
    while (!(v >> 63)) { v <<= 1; n++; }
    return n;
#endif
}

/* (hi:lo) / d with hi < d: the quotient, and the remainder in *rem (may be NULL). */
static inline uint64_t mc_div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem) {
    const uint64_t b = UINT64_C(1) << 32;
    const int s = mc_clz64(d);
    uint64_t v, vn1, vn0, un32, un10, un1, un0, un21, q1, q0, rhat;
    v = d << s;
    vn1 = v >> 32;
    vn0 = v & 0xffffffffu;
    un32 = s ? (hi << s) | (lo >> (64 - s)) : hi;
    un10 = lo << s;
    un1 = un10 >> 32;
    un0 = un10 & 0xffffffffu;
    q1 = un32 / vn1;
    rhat = un32 - q1 * vn1;
    while (q1 >= b || q1 * vn0 > b * rhat + un1) {
        q1--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    un21 = un32 * b + un1 - q1 * v;
    q0 = un21 / vn1;
    rhat = un21 - q0 * vn1;
    while (q0 >= b || q0 * vn0 > b * rhat + un0) {
        q0--;
        rhat += vn1;
        if (rhat >= b) break;
    }
    if (rem) *rem = (un21 * b + un0 - q0 * v) >> s;
    return q1 * b + q0;
}

#endif
