/* Extended-precision arithmetic for the FPU: Livadix's own code, written from
 * the MC68881/MC68882 User's Manual (3.2 data types, 3.5 format conversions,
 * 3.6 data format details, 4.5.3 operation tables, 4.5.5.2 underflow, round,
 * overflow, 6.1.3-6.1.7 the exception conditions and their default
 * results). See fpu_soft.h. */
#include "fpu_soft.h"

#include <math.h>
#include <string.h>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#elif defined(_MSC_VER)
#include "../wide.h"
#endif

const fpfmt FMT_X = {64, -16383, 16383};
const fpfmt FMT_D = {53, -1022, 1023};
const fpfmt FMT_S = {24, -126, 127};

#define TOP 0x8000000000000000ull

/* --- 64- and 128-bit helpers ------------------------------------------------ */

static int clz64(uint64_t v) {
#if defined(_MSC_VER) && defined(_M_X64)
    unsigned long i;
    return _BitScanReverse64(&i, v) ? 63 - (int)i : 64;
#elif defined(_MSC_VER)
    return mc_clz64(v);
#elif defined(__GNUC__)
    return v ? __builtin_clzll(v) : 64;
#else
    int n = 0;
    if (!v) return 64;
    if (!(v >> 32)) { n += 32; v <<= 32; }
    if (!(v >> 48)) { n += 16; v <<= 16; }
    if (!(v >> 56)) { n += 8; v <<= 8; }
    if (!(v >> 60)) { n += 4; v <<= 4; }
    if (!(v >> 62)) { n += 2; v <<= 2; }
    if (!(v >> 63)) n += 1;
    return n;
#endif
}

static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo) {
#if defined(_MSC_VER) && defined(_M_X64)
    *lo = _umul128(a, b, hi);
    return;
#elif defined(_MSC_VER)
    *lo = mc_mul128(a, b, hi);
    return;
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 p = (unsigned __int128)a * b;
    *hi = (uint64_t)(p >> 64);
    *lo = (uint64_t)p;
    return;
#else
    const uint64_t a0 = (uint32_t)a, a1 = a >> 32, b0 = (uint32_t)b, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
    *lo = (mid << 32) | (uint32_t)p00;
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
#endif
}

/* (hi:lo) / d with hi < d: the 64-bit quotient and the remainder. */
static uint64_t div128(uint64_t hi, uint64_t lo, uint64_t d, uint64_t *rem) {
#if defined(_MSC_VER) && defined(_M_X64)
    return _udiv128(hi, lo, d, rem);
#elif defined(_MSC_VER)
    return mc_div128(hi, lo, d, rem);
#elif defined(__SIZEOF_INT128__)
    const unsigned __int128 n = (unsigned __int128)hi << 64 | lo;
    *rem = (uint64_t)(n % d);
    return (uint64_t)(n / d);
#else
    uint64_t q = 0;
    for (int i = 0; i < 64; i++) {
        const uint64_t c = hi >> 63;
        hi = (hi << 1) | (lo >> 63);
        lo <<= 1;
        q <<= 1;
        if (c || hi >= d) { hi -= d; q |= 1; }
    }
    *rem = hi;
    return q;
#endif
}

/* (m:lo) >> n with every bit shifted out ORed into bit 0 of lo. */
static void shr_sticky(uint64_t *m, uint64_t *lo, uint32_t n) {
    if (n == 0) return;
    if (n < 64) {
        const uint64_t out = *lo << (64 - n);
        *lo = (*m << (64 - n)) | (*lo >> n) | (out != 0);
        *m >>= n;
    } else if (n < 128) {
        const uint32_t k = n - 64;
        const uint64_t out = (k ? *m << (64 - k) : 0) | *lo;
        *lo = (*m >> k) | (out != 0);
        *m = 0;
    } else {
        *lo = (*m | *lo) != 0;
        *m = 0;
    }
}

/* Shifts (m:lo) left until bit 63 of m is set; returns the count. */
static int normalize(uint64_t *m, uint64_t *lo) {
    if (!*m) {
        if (!*lo) return 0;
        *m = *lo;
        *lo = 0;
        const int n = clz64(*m);
        *m <<= n;
        return 64 + n;
    }
    const int n = clz64(*m);
    if (n) {
        *m = (*m << n) | (*lo >> (64 - n));
        *lo <<= n;
    }
    return n;
}

/* --- classes ------------------------------------------------------------------ */

fpx fx_zero(int sign) { fpx v; memset(&v, 0, sizeof v); v.s = (uint8_t)sign; v.k = FX_ZERO; return v; }
fpx fx_inf(int sign) { fpx v = fx_zero(sign); v.k = FX_INF; return v; }
fpx fx_default_nan(void) { fpx v = fx_zero(0); v.k = FX_NAN; v.m = 0xFFFFFFFFFFFFFFFFull; return v; }
int fx_is_snan(fpx v) { return v.k == FX_NAN && !(v.m & 0x4000000000000000ull); }
fpx fx_quiet(fpx v) { v.m |= 0x4000000000000000ull; return v; }

static fpx norm(int s, int32_t e, uint64_t m, uint64_t lo) {
    fpx v;
    v.s = (uint8_t)s;
    if (!m && !lo) return fx_zero(s);
    const int n = normalize(&m, &lo);
    v.m = m;
    v.lo = lo;
    v.e = e - n;
    v.k = FX_NORM;
    return v;
}

/* --- formats ------------------------------------------------------------------- */

fpx fx_from_x(m68k_fx80 r) {
    const int s = r.se >> 15;
    const int32_t be = r.se & 0x7FFF;
    if (be == 0x7FFF) {
        fpx v = fx_zero(s);
        if (r.m & 0x7FFFFFFFFFFFFFFFull) {
            v.k = FX_NAN;
            v.m = r.m;
        } else {
            v.k = FX_INF;
        }
        return v;
    }
    return norm(s, be - 16383, r.m, 0);
}

fpx fx_from_x_e15(m68k_fx80 r, int e15) {
    if (!e15) return fx_from_x(r);
    /* The 68040's frame exponent with its bit 15 (ETE15/FPTE15) set: a
     * negative biased exponent, two's complement in 16 bits. */
    const int32_t be = (int32_t)(int16_t)(uint16_t)(0x8000u | (r.se & 0x7FFFu));
    return norm(r.se >> 15, be - 16383, r.m, 0);
}

m68k_fx80 fx_to_x(fpx v) {
    m68k_fx80 r;
    const uint16_t s = (uint16_t)(v.s ? 0x8000u : 0);
    switch (v.k) {
    case FX_ZERO: r.se = s; r.m = 0; return r;
    case FX_INF: r.se = s | 0x7FFF; r.m = 0; return r;
    case FX_NAN: r.se = s | 0x7FFF; r.m = v.m; return r;
    default: break;
    }
    if (v.e >= -16383) {
        r.se = (uint16_t)(s | (uint16_t)(v.e + 16383));
        r.m = v.m;
    } else {
        const int32_t n = -16383 - v.e;
        r.se = s;
        r.m = n >= 64 ? 0 : v.m >> n;
    }
    return r;
}

fpx fx_from_s(uint32_t b) {
    const int s = (int)(b >> 31);
    const uint32_t ex = (b >> 23) & 0xFF, f = b & 0x7FFFFF;
    if (ex == 0xFF) {
        fpx v = fx_zero(s);
        if (f) { v.k = FX_NAN; v.m = TOP | ((uint64_t)f << 40); }
        else v.k = FX_INF;
        return v;
    }
    if (ex == 0) return norm(s, -126, (uint64_t)f << 40, 0);
    return norm(s, (int32_t)ex - 127, TOP | ((uint64_t)f << 40), 0);
}

fpx fx_from_d(uint64_t b) {
    const int s = (int)(b >> 63);
    const uint32_t ex = (uint32_t)(b >> 52) & 0x7FF;
    const uint64_t f = b & 0xFFFFFFFFFFFFFull;
    if (ex == 0x7FF) {
        fpx v = fx_zero(s);
        if (f) { v.k = FX_NAN; v.m = TOP | (f << 11); }
        else v.k = FX_INF;
        return v;
    }
    if (ex == 0) return norm(s, -1022, f << 11, 0);
    return norm(s, (int32_t)ex - 1023, TOP | (f << 11), 0);
}

uint32_t fx_to_s(fpx v) {
    const uint32_t s = v.s ? 0x80000000u : 0;
    switch (v.k) {
    case FX_ZERO: return s;
    case FX_INF: return s | 0x7F800000u;
    case FX_NAN: {
        uint32_t f = (uint32_t)(v.m >> 40) & 0x7FFFFF;
        if (!f) f = 0x400000;       /* a NaN whose high bits are zero stays a NaN */
        return s | 0x7F800000u | f;
    }
    default: break;
    }
    if (v.e < -126) {
        const int32_t n = -126 - v.e;
        return s | (uint32_t)(n + 40 >= 64 ? 0 : v.m >> (40 + n));
    }
    return s | (uint32_t)(v.e + 127) << 23 | ((uint32_t)(v.m >> 40) & 0x7FFFFF);
}

uint64_t fx_to_d(fpx v) {
    const uint64_t s = v.s ? 0x8000000000000000ull : 0;
    switch (v.k) {
    case FX_ZERO: return s;
    case FX_INF: return s | 0x7FF0000000000000ull;
    case FX_NAN: {
        uint64_t f = (v.m >> 11) & 0xFFFFFFFFFFFFFull;
        if (!f) f = 0x8000000000000ull;
        return s | 0x7FF0000000000000ull | f;
    }
    default: break;
    }
    if (v.e < -1022) {
        const int32_t n = -1022 - v.e;
        return s | (n + 11 >= 64 ? 0 : v.m >> (11 + n));
    }
    return s | (uint64_t)(v.e + 1023) << 52 | ((v.m >> 11) & 0xFFFFFFFFFFFFFull);
}

fpx fx_from_u64(uint64_t u) { return norm(0, 63, u, 0); }
fpx fx_from_u128(uint64_t hi, uint64_t lo) { return norm(0, 127, hi, lo); }
fpx fx_from_int(int64_t i) {
    fpx v = fx_from_u64(i < 0 ? 0 - (uint64_t)i : (uint64_t)i);
    v.s = i < 0;
    return v;
}

/* --- rounding (MC68881 UM 4.5.5.2, Figure 6-3; 6.1.4, 6.1.5) --------------------- */

fpx fx_round(fpx v, const fpfmt *f, unsigned mode, unsigned *exc) {
    if (v.k != FX_NORM) return v;
    const int p = f->bits;
    uint64_t m = v.m, lo = v.lo;
    int32_t e = v.e;
    const int tiny = e < f->emin;
    if (tiny) {
        const int64_t d = (int64_t)f->emin - e;
        shr_sticky(&m, &lo, d > 200 ? 200u : (uint32_t)d);
        e = f->emin;
    }
    int inexact, gt, eq, lsb;
    uint64_t unit;
    if (p == 64) {
        inexact = lo != 0;
        gt = lo > TOP;
        eq = lo == TOP;
        lsb = (int)(m & 1u);
        unit = 1;
    } else {
        const uint64_t mask = (1ull << (64 - p)) - 1, half = 1ull << (63 - p), r = m & mask;
        inexact = r != 0 || lo != 0;
        gt = r > half || (r == half && lo != 0);
        eq = r == half && lo == 0;
        lsb = (int)((m >> (64 - p)) & 1u);
        unit = 1ull << (64 - p);
        m &= ~mask;
    }
    int up;
    switch (mode & 3u) {
    case RND_N: up = gt || (eq && lsb); break;
    case RND_Z: up = 0; break;
    case RND_M: up = inexact && v.s; break;
    default: up = inexact && !v.s; break;
    }
    if (up) {
        m += unit;
        if (m == 0) {
            m = TOP;
            e++;
        }
    }
    if (tiny) *exc |= FX_UNFL;
    if (inexact) *exc |= FX_INEX2;
    if (m == 0) return fx_zero(v.s);
    fpx r = norm(v.s, e, m, 0);
    if (r.e > f->emax) {
        *exc |= FX_OVFL;
        const unsigned md = mode & 3u;
        if (md == RND_N || (md == RND_P && !v.s) || (md == RND_M && v.s)) return fx_inf(v.s);
        r.m = p == 64 ? ~0ull : ~((1ull << (64 - p)) - 1);
        r.e = f->emax;
        r.lo = 0;
    }
    return r;
}

fpx fx_round_int(fpx v, unsigned mode, unsigned *exc) {
    if (v.k != FX_NORM || v.e >= 63) return v;
    if (v.e < 0) {
        int up;
        const int above_half = v.e == -1 && (v.m != TOP || v.lo != 0);
        switch (mode & 3u) {
        case RND_N: up = above_half; break;
        case RND_Z: up = 0; break;
        case RND_M: up = v.s; break;
        default: up = !v.s; break;
        }
        *exc |= FX_INEX2;
        if (!up) return fx_zero(v.s);
        fpx one = fx_from_u64(1);
        one.s = v.s;
        return one;
    }
    fpfmt f = {v.e + 1, -0x40000000, 0x40000000};
    return fx_round(v, &f, mode, exc);
}

int32_t fx_to_int(fpx v, int bits, unsigned mode, unsigned *exc) {
    const int64_t maxv = (1ll << (bits - 1)) - 1, minv = -(1ll << (bits - 1));
    if (v.k == FX_ZERO) return 0;
    if (v.k == FX_INF) {
        *exc |= FX_OPERR;
        return (int32_t)(v.s ? minv : maxv);
    }
    fpx r = fx_round_int(v, mode, exc);
    if (r.k == FX_ZERO) return 0;
    if (r.e >= bits) {
        *exc |= FX_OPERR;
        return (int32_t)(r.s ? minv : maxv);
    }
    const uint64_t mag = r.m >> (63 - r.e);
    if (!r.s && mag > (uint64_t)maxv) { *exc |= FX_OPERR; return (int32_t)maxv; }
    if (r.s && mag > (uint64_t)maxv + 1) { *exc |= FX_OPERR; return (int32_t)minv; }
    return (int32_t)(r.s ? -(int64_t)mag : (int64_t)mag);
}

/* --- arithmetic (operation tables of MC68881 UM 4.6) -------------------------- */

int fx_cmp_mag(fpx a, fpx b) {
    if (a.e != b.e) return a.e < b.e ? -1 : 1;
    if (a.m != b.m) return a.m < b.m ? -1 : 1;
    if (a.lo != b.lo) return a.lo < b.lo ? -1 : 1;
    return 0;
}

fpx fx_add(fpx a, fpx b, unsigned mode, unsigned *exc) {
    if (a.k == FX_INF || b.k == FX_INF) {
        if (a.k == FX_INF && b.k == FX_INF && a.s != b.s) {
            *exc |= FX_OPERR;
            return fx_default_nan();
        }
        return a.k == FX_INF ? a : b;
    }
    if (a.k == FX_ZERO && b.k == FX_ZERO) return fx_zero(a.s == b.s ? a.s : (mode & 3u) == RND_M);
    if (a.k == FX_ZERO) return b;
    if (b.k == FX_ZERO) return a;
    if (fx_cmp_mag(a, b) < 0) {
        const fpx t = a;
        a = b;
        b = t;
    }
    uint64_t bm = b.m, bl = b.lo;
    const int64_t d = (int64_t)a.e - b.e;
    shr_sticky(&bm, &bl, d > 200 ? 200u : (uint32_t)d);
    uint64_t m = a.m, lo = a.lo;
    int32_t e = a.e;
    if (a.s == b.s) {
        const uint64_t l2 = lo + bl;
        const uint64_t c1 = l2 < lo;
        const uint64_t t = m + bm;
        int carry = t < m;
        const uint64_t m2 = t + c1;
        carry |= m2 < t;
        lo = l2;
        m = m2;
        if (carry) {
            shr_sticky(&m, &lo, 1);
            m |= TOP;
            e++;
        }
        fpx r;
        r.m = m; r.lo = lo; r.e = e; r.s = a.s; r.k = FX_NORM;
        return r;
    }
    const uint64_t borrow = lo < bl;
    lo -= bl;
    m = m - bm - borrow;
    if (!m && !lo) return fx_zero((mode & 3u) == RND_M);
    return norm(a.s, e, m, lo);
}

fpx fx_mul(fpx a, fpx b, unsigned *exc) {
    const int s = a.s ^ b.s;
    if ((a.k == FX_INF && b.k == FX_ZERO) || (a.k == FX_ZERO && b.k == FX_INF)) {
        *exc |= FX_OPERR;
        return fx_default_nan();
    }
    if (a.k == FX_INF || b.k == FX_INF) return fx_inf(s);
    if (a.k == FX_ZERO || b.k == FX_ZERO) return fx_zero(s);
    uint64_t hi, lo;
    mul64(a.m, b.m, &hi, &lo);
    /* The operands' own low bits (exact intermediates): their cross terms
     * only add sticky information below the 128-bit product. */
    if (a.lo || b.lo) lo |= 1;
    return norm(s, a.e + b.e + 1, hi, lo);
}

fpx fx_div(fpx a, fpx b, unsigned *exc) {
    const int s = a.s ^ b.s;
    if ((a.k == FX_ZERO && b.k == FX_ZERO) || (a.k == FX_INF && b.k == FX_INF)) {
        *exc |= FX_OPERR;
        return fx_default_nan();
    }
    if (a.k == FX_INF) return fx_inf(s);
    if (b.k == FX_INF) return fx_zero(s);
    if (b.k == FX_ZERO) {
        *exc |= FX_DZ;
        return fx_inf(s);
    }
    if (a.k == FX_ZERO) return fx_zero(s);
    /* The 128 quotient bits floor(a.m x 2^127 / b.m), the remainder
     * sticky: the top bit is a.m >= b.m, the rest two 64-bit divisions of
     * what is left (each below b.m) shifted down one. */
    const uint64_t top = a.m >= b.m;
    const uint64_t r0 = top ? a.m - b.m : a.m;
    uint64_t r1, r2;
    const uint64_t q1 = div128(r0, 0, b.m, &r1);
    const uint64_t q0 = div128(r1, 0, b.m, &r2);
    const uint64_t qh = top << 63 | q1 >> 1;
    const uint64_t ql = (q1 << 63) | (q0 >> 1) | ((q0 & 1u) | (r2 != 0));
    return norm(s, a.e - b.e, qh, ql);
}

/* 192-bit unsigned helpers for the square root. */
typedef struct u192 { uint64_t w[3]; } u192;   /* w[0] most significant */
static int u192_ge(const u192 *a, const u192 *b) {
    for (int i = 0; i < 3; i++)
        if (a->w[i] != b->w[i]) return a->w[i] > b->w[i];
    return 1;
}
static void u192_sub(u192 *a, const u192 *b) {
    uint64_t borrow = 0;
    for (int i = 2; i >= 0; i--) {
        const uint64_t x = a->w[i], y = b->w[i];
        const uint64_t d = x - y - borrow;
        borrow = (x < y) || (x == y && borrow);
        a->w[i] = d;
    }
}
static void u192_add(u192 *a, const u192 *b) {
    uint64_t c = 0;
    for (int i = 2; i >= 0; i--) {
        const uint64_t s = a->w[i] + b->w[i] + c;
        c = (s < a->w[i]) || (s == a->w[i] && (b->w[i] | c));
        a->w[i] = s;
    }
}
static void u192_shr(u192 *a, unsigned n) {
    while (n--) {
        a->w[2] = (a->w[2] >> 1) | (a->w[1] << 63);
        a->w[1] = (a->w[1] >> 1) | (a->w[0] << 63);
        a->w[0] >>= 1;
    }
}

fpx fx_sqrt(fpx a, unsigned *exc) {
    if (a.k == FX_ZERO) return a;
    if (a.s) {
        *exc |= FX_OPERR;
        return fx_default_nan();
    }
    if (a.k == FX_INF) return a;
    /* sqrt(m x 2^E), E = e - 63: radicand N = (m:lo) x 2^(64 - j) < 2^192
     * with E - 128 + j even; isqrt(N) has 95 or 96 bits, the remainder is
     * sticky. */
    const int64_t E = (int64_t)a.e - 63;
    const int j = (int)(E & 1);
    u192 n = {{0, 0, 0}};
    if (!j) {
        n.w[0] = a.m;
        n.w[1] = a.lo;
    } else {
        n.w[0] = a.m >> 1;
        n.w[1] = (a.m << 63) | (a.lo >> 1);
        n.w[2] = a.lo << 63;
    }
    u192 res = {{0, 0, 0}}, bit = {{0, 0, 0}};
    bit.w[0] = 1ull << 62;          /* the highest power of 4 below 2^192 */
    while (!u192_ge(&n, &bit) && (bit.w[0] | bit.w[1] | bit.w[2])) u192_shr(&bit, 2);
    while (bit.w[0] | bit.w[1] | bit.w[2]) {
        u192 t = res;
        u192_add(&t, &bit);
        if (u192_ge(&n, &t)) {
            u192_sub(&n, &t);
            u192_shr(&res, 1);
            u192_add(&res, &bit);
        } else {
            u192_shr(&res, 1);
        }
        u192_shr(&bit, 2);
    }
    const int sticky = (n.w[0] | n.w[1] | n.w[2]) != 0;
    /* res < 2^96 sits in w[1] (high) and w[2]. */
    fpx r = norm(0, (int32_t)((E - 128 + j) / 2) + 127, res.w[1], res.w[2]);
    if (sticky) r.lo |= 1;
    return r;
}

fpx fx_scale(fpx a, int32_t n) {
    if (a.k == FX_NORM) a.e += n;
    return a;
}

fpx fx_rem(fpx a, fpx b, int nearest, unsigned *exc, unsigned *qbyte) {
    const unsigned qs = (unsigned)(a.s ^ b.s) << 7;
    *qbyte = qs;
    if (a.k == FX_INF || b.k == FX_ZERO) {
        *exc |= FX_OPERR;
        return fx_default_nan();
    }
    if (b.k == FX_INF || a.k == FX_ZERO) return a;
    /* |a| = ma 2^(ea-63), |b| = mb 2^(eb-63), n = ea - eb. */
    const int64_t n = (int64_t)a.e - b.e;
    if (n < 0) {
        /* |a| < |b|: FMOD keeps a; FREM takes q = 1 when |a| > |b|/2, which
         * needs n = -1 and ma > mb (a tie keeps the even q = 0). */
        if (!nearest || n < -1 || a.m <= b.m) return a;
        fpx bb = b, aa = a;
        bb.s = 0;
        aa.s = 1;
        unsigned e2 = 0;
        fpx d = fx_add(bb, aa, RND_N, &e2);
        d.s = (uint8_t)(a.s ^ 1);
        *qbyte = qs | 1u;
        return d;
    }
    uint64_t r = a.m, q = 0;
    int rc = 0;
    for (int64_t i = 0; i <= n; i++) {
        q <<= 1;
        if (rc || r >= b.m) {
            r -= b.m;
            q |= 1;
        }
        if (i < n) {
            rc = (int)(r >> 63);
            r <<= 1;
        }
    }
    int s = a.s;
    if (nearest && r) {
        /* 2r against mb: above, or equal with q odd, rounds q up. */
        const int above = (r >> 63) || (r << 1) > b.m;
        const int tie = !(r >> 63) && (r << 1) == b.m;
        if (above || (tie && (q & 1))) {
            r = b.m - r;
            q++;
            s ^= 1;
        }
    }
    *qbyte = qs | (unsigned)(q & 0x7F);
    if (r == 0) return fx_zero(a.s);
    return norm(s, b.e, r, 0);
}

/* --- the 128-bit working format ------------------------------------------------ */

static fpb fb_norm(int s, int32_t e, uint64_t h, uint64_t l) {
    fpb r;
    r.s = s;
    if (!h && !l) { r.h = r.l = 0; r.e = 0; return r; }
    const int n = normalize(&h, &l);
    r.h = h;
    r.l = l;
    r.e = e - n;
    return r;
}
fpb fb_from_x(fpx v) {
    fpb r;
    r.s = v.s;
    if (v.k != FX_NORM) { r.h = r.l = 0; r.e = 0; return r; }
    r.h = v.m;
    r.l = v.lo;
    r.e = v.e;
    return r;
}
fpx fb_to_x(fpb v) {
    if (!v.h) return fx_zero(v.s);
    fpx r;
    r.m = v.h;
    r.lo = v.l | 1;                 /* the working result is never exact */
    r.e = v.e;
    r.s = (uint8_t)v.s;
    r.k = FX_NORM;
    return r;
}
fpb fb_from_u64(uint64_t u) { return fb_norm(0, 127, 0, u); }
fpb fb_make(int s, int32_t e, uint64_t h, uint64_t l) { return fb_norm(s, e, h, l); }
fpb fb_from_int(int64_t i) {
    fpb r = fb_from_u64(i < 0 ? 0 - (uint64_t)i : (uint64_t)i);
    r.s = i < 0;
    return r;
}
int fb_is_zero(fpb a) { return a.h == 0; }
fpb fb_neg(fpb a) { a.s ^= 1; return a; }
fpb fb_ldexp(fpb a, int32_t n) { if (a.h) a.e += n; return a; }

static int fb_cmp_mag(fpb a, fpb b) {
    if (!a.h || !b.h) return a.h ? 1 : b.h ? -1 : 0;
    if (a.e != b.e) return a.e < b.e ? -1 : 1;
    if (a.h != b.h) return a.h < b.h ? -1 : 1;
    if (a.l != b.l) return a.l < b.l ? -1 : 1;
    return 0;
}
int fb_cmp(fpb a, fpb b) {
    const int az = !a.h, bz = !b.h;
    if (az && bz) return 0;
    if (az) return b.s ? 1 : -1;
    if (bz) return a.s ? -1 : 1;
    if (a.s != b.s) return a.s ? -1 : 1;
    const int c = fb_cmp_mag(a, b);
    return a.s ? -c : c;
}

fpb fb_add(fpb a, fpb b) {
    if (!a.h) return b;
    if (!b.h) return a;
    if (fb_cmp_mag(a, b) < 0) { const fpb t = a; a = b; b = t; }
    const int64_t d = (int64_t)a.e - b.e;
    uint64_t bh = b.h, bl = b.l;
    int lost;                                   /* bits of b shifted out */
    if (d >= 128) { lost = 1; bh = bl = 0; }
    else if (d >= 64) { lost = bl != 0 || (d > 64 && (bh << (128 - d)) != 0); bl = bh >> (d - 64); bh = 0; }
    else if (d > 0) { lost = (bl << (64 - d)) != 0; bl = (bl >> d) | (bh << (64 - d)); bh >>= d; }
    else lost = 0;
    uint64_t h = a.h, l = a.l;
    /* Lost bits make b a little larger than bl:bh: a sum gets a sticky low
     * bit, a difference one unit less and the sticky bit, so the working
     * result stays on the right side of the true value for directed
     * rounding. */
    if (lost && a.s != b.s) {
        bl += 1;
        if (bl == 0) bh++;
    }
    if (a.s == b.s) {
        if (lost) l |= 1;
        const uint64_t l2 = l + bl, c = l2 < l;
        const uint64_t h2 = h + bh + c;
        const int carry = h2 < h || (h2 == h && (bh | c));
        if (carry) return fb_norm(a.s, a.e + 1, (h2 >> 1) | 0x8000000000000000ull, (l2 >> 1) | (h2 << 63));
        return fb_norm(a.s, a.e, h2, l2);
    }
    const uint64_t borrow = l < bl;
    fpb r = fb_norm(a.s, a.e, h - bh - borrow, l - bl);
    if (lost) r.l |= 1;
    return r;
}
fpb fb_sub(fpb a, fpb b) { return fb_add(a, fb_neg(b)); }

static void add_at(uint64_t *p, int i, uint64_t v) {
    while (v && i >= 0) {
        const uint64_t t = p[i] + v;
        v = t < v;
        p[i] = t;
        i--;
    }
}

fpb fb_mul(fpb a, fpb b) {
    if (!a.h || !b.h) { fpb z = {0, 0, 0, a.s ^ b.s}; return z; }
    uint64_t p[4] = {0, 0, 0, 0}, hi, lo;   /* p[0] most significant */
    mul64(a.l, b.l, &hi, &lo);
    add_at(p, 3, lo); add_at(p, 2, hi);
    mul64(a.h, b.l, &hi, &lo);
    add_at(p, 2, lo); add_at(p, 1, hi);
    mul64(a.l, b.h, &hi, &lo);
    add_at(p, 2, lo); add_at(p, 1, hi);
    mul64(a.h, b.h, &hi, &lo);
    add_at(p, 1, lo); add_at(p, 0, hi);
    return fb_norm(a.s ^ b.s, a.e + b.e + 1, p[0], p[1] | ((p[2] | p[3]) != 0));
}

fpb fb_div(fpb a, fpb b) {
    if (!a.h || !b.h) { fpb z = {0, 0, 0, a.s ^ b.s}; return z; }
    uint64_t rh = a.h, rl = a.l, qh = 0, ql = 0;
    int rc = 0;
    for (int i = 0; i < 128; i++) {
        qh = (qh << 1) | (ql >> 63);
        ql <<= 1;
        if (rc || rh > b.h || (rh == b.h && rl >= b.l)) {
            const uint64_t borrow = rl < b.l;
            rl -= b.l;
            rh = rh - b.h - borrow;
            ql |= 1;
        }
        rc = (int)(rh >> 63);
        rh = (rh << 1) | (rl >> 63);
        rl <<= 1;
    }
    return fb_norm(a.s ^ b.s, a.e - b.e, qh, ql);
}

fpb fb_sqrt(fpb a) {
    if (!a.h || a.s) { fpb z = {0, 0, 0, 0}; return z; }
    /* A double first guess, then Newton: x = (x + a/x) / 2. */
    const double mant = (double)(a.h >> 11) * (1.0 / 9007199254740992.0);   /* [0.5, 1) */
    const int32_t e = a.e + 1;     /* a = mant x 2^e */
    const double g = sqrt(mant * ((e & 1) ? 2.0 : 1.0));
    const int32_t ge = (e - (e & 1)) / 2;
    fpb x = fb_from_u64((uint64_t)(g * 9007199254740992.0));
    x = fb_ldexp(x, ge - 53);
    for (int i = 0; i < 5; i++) x = fb_ldexp(fb_add(x, fb_div(a, x)), -1);
    return x;
}

int64_t fb_floor_int(fpb a) {
    if (!a.h || a.e < 0) return a.h && a.s ? -1 : 0;
    const uint64_t mag = a.e >= 63 ? a.h : a.h >> (63 - a.e);
    int frac = 0;
    if (a.e < 63) frac = (a.h << (a.e + 1)) != 0 || a.l != 0;
    else if (a.e < 127) frac = (a.l << (a.e - 63)) != 0;
    return a.s ? -(int64_t)mag - frac : (int64_t)mag;
}

fpb fb_frac(fpb a) {
    if (!a.h || a.e < 0) return a;
    if (a.e >= 127) { fpb z = {0, 0, 0, a.s}; return z; }
    uint64_t h = a.h, l = a.l;
    const int keep = 127 - a.e;     /* fraction bits */
    if (keep < 64) { h = 0; l &= (1ull << keep) - 1; }
    else h &= (keep >= 128) ? ~0ull : (1ull << (keep - 64)) - 1;
    return fb_norm(a.s, a.e, h, l);
}
