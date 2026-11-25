/* The FPU's transcendental functions, its constant ROM and the packed
 * decimal conversions: Livadix's own code, written from the MC68881/MC68882
 * User's Manual (4.3 computational accuracy and decimal conversions, 4.6
 * the instruction descriptions and their special-value tables, 3.4/3.6 the
 * packed decimal real format, FMOVE's k-factor rules; FMOVECR's offsets
 * from the M68000 PRM).
 *
 * Method: arguments are reduced exactly (trigonometric functions by long
 * division against pi/2 carried to 17,280 fractional bits, computed here by
 * Machin's formula), then evaluated by series in the 128-bit working format
 * (fpb) and rounded once into extended precision. The working format's
 * error stays near 2^-115 relative, so results are the correctly rounded
 * value except within that distance of a rounding boundary: within 1 ulp,
 * nearly always 0.5 ulp. */
#include "fpu_trans.h"

#include <math.h>
#include <string.h>

/* --- constants ------------------------------------------------------------------ */

static fpb K_LN2, K_LN10, K_PI, K_PI2, K_PI4, K_ONE, K_TWO, K_HALF;
static fpb K_POW10[14];         /* 10^(2^i), i = 0..13 */
static int k_ready;

static fpb fb_small(int64_t v) { return fb_from_int(v); }
static fpb fb_recip(int64_t v) { return fb_div(K_ONE, fb_from_int(v)); }

/* atanh(1/n) = sum 1/((2k+1) n^(2k+1)) */
static fpb atanh_inv(int64_t n) {
    fpb term = fb_recip(n), sum = term;
    const fpb n2 = fb_from_int(n * n);
    for (int64_t k = 1; k < 200; k++) {
        term = fb_div(term, n2);
        const fpb t = fb_div(term, fb_from_int(2 * k + 1));
        sum = fb_add(sum, t);
        if (fb_is_zero(t) || t.e < sum.e - 130) break;
    }
    return sum;
}
/* atan(1/n) = sum (-1)^k / ((2k+1) n^(2k+1)) */
static fpb atan_inv(int64_t n) {
    fpb term = fb_recip(n), sum = term;
    const fpb n2 = fb_from_int(n * n);
    for (int64_t k = 1; k < 200; k++) {
        term = fb_div(term, n2);
        const fpb t = fb_div(term, fb_from_int(2 * k + 1));
        sum = (k & 1) ? fb_sub(sum, t) : fb_add(sum, t);
        if (fb_is_zero(t) || t.e < sum.e - 130) break;
    }
    return sum;
}

static void init_consts(void) {
    if (k_ready) return;
    K_ONE = fb_small(1);
    K_TWO = fb_small(2);
    K_HALF = fb_ldexp(K_ONE, -1);
    K_LN2 = fb_ldexp(atanh_inv(3), 1);                          /* ln 2 = 2 atanh(1/3) */
    K_LN10 = fb_add(fb_mul(K_LN2, fb_small(3)), fb_ldexp(atanh_inv(9), 1));   /* + ln 1.25 */
    K_PI = fb_sub(fb_ldexp(atan_inv(5), 4), fb_ldexp(atan_inv(239), 2));     /* Machin */
    K_PI2 = fb_ldexp(K_PI, -1);
    K_PI4 = fb_ldexp(K_PI, -2);
    K_POW10[0] = fb_small(10);
    for (int i = 1; i < 14; i++) K_POW10[i] = fb_mul(K_POW10[i - 1], K_POW10[i - 1]);
    k_ready = 1;
}

/* 10^n in the working format. */
static fpb pow10_fb(int32_t n) {
    init_consts();
    const int neg = n < 0;
    uint32_t u = neg ? (uint32_t)-n : (uint32_t)n;
    fpb r = K_ONE;
    for (int i = 0; u && i < 14; i++, u >>= 1)
        if (u & 1u) r = fb_mul(r, K_POW10[i]);
    if (u) r = fb_ldexp(K_ONE, neg ? -100000 : 100000);         /* far out of range */
    return neg ? fb_div(K_ONE, r) : r;
}

/* --- elementary functions in the working format ---------------------------------- */

static int tiny_vs(fpb t, fpb sum) { return fb_is_zero(t) || t.e < sum.e - 132; }

/* e^r - 1 by its series, for |r| < 1 */
static fpb expm1_series(fpb r) {
    fpb term = r, sum = r;
    for (int n = 2; n < 200; n++) {
        term = fb_div(fb_mul(term, r), fb_small(n));
        sum = fb_add(sum, term);
        if (tiny_vs(term, sum)) break;
    }
    return sum;
}

/* e^x; *scale gets the power of two kept outside (huge arguments). */
static fpb exp_fb(fpb x) {
    init_consts();
    if (fb_is_zero(x)) return K_ONE;
    if (x.e > 15) {                             /* |x| >= 65536: far beyond the range */
        return x.s ? fb_ldexp(K_ONE, -200000) : fb_ldexp(K_ONE, 200000);
    }
    const fpb kf = fb_div(x, K_LN2);
    const int64_t k = fb_floor_int(fb_add(kf, K_HALF));
    const fpb r = fb_sub(x, fb_mul(fb_from_int(k), K_LN2));
    return fb_ldexp(fb_add(K_ONE, expm1_series(r)), (int32_t)k);
}

static fpb expm1_fb(fpb x) {
    init_consts();
    if (fb_is_zero(x)) return x;
    fpb ax = x;
    ax.s = 0;
    if (fb_cmp(ax, K_HALF) < 0) return expm1_series(x);
    return fb_sub(exp_fb(x), K_ONE);
}

/* atanh by its series, for |t| < 1 (fast below 1/2) */
static fpb atanh_series(fpb t) {
    const fpb t2 = fb_mul(t, t);
    fpb term = t, sum = t;
    for (int k = 1; k < 2000; k++) {
        term = fb_mul(term, t2);
        const fpb q = fb_div(term, fb_small(2 * k + 1));
        sum = fb_add(sum, q);
        if (tiny_vs(q, sum)) break;
    }
    return sum;
}

/* ln x, x > 0 */
static fpb log_fb(fpb x) {
    init_consts();
    /* x = m 2^k with m in [sqrt(1/2), sqrt(2)) */
    int32_t k = x.e;
    fpb m = x;
    m.e = 0;                                    /* m in [1, 2) */
    static const uint64_t sqrt2_hi = 0xB504F333F9DE6484ull;
    if (m.h > sqrt2_hi) {
        m.e = -1;
        k++;
    }
    const fpb t = fb_div(fb_sub(m, K_ONE), fb_add(m, K_ONE));
    return fb_add(fb_mul(fb_from_int(k), K_LN2), fb_ldexp(atanh_series(t), 1));
}

static fpb log1p_fb(fpb x) {
    init_consts();
    fpb ax = x;
    ax.s = 0;
    if (fb_cmp(ax, fb_ldexp(K_ONE, -2)) < 0) return fb_ldexp(atanh_series(fb_div(x, fb_add(K_TWO, x))), 1);
    return log_fb(fb_add(K_ONE, x));
}

/* atan, any x */
static fpb atan_fb(fpb x) {
    init_consts();
    if (fb_is_zero(x)) return x;
    const int s = x.s;
    fpb a = x;
    a.s = 0;
    int inv = 0;
    if (fb_cmp(a, K_ONE) > 0) {
        a = fb_div(K_ONE, a);
        inv = 1;
    }
    /* halve the angle until a <= 1/8: atan a = 2 atan(a / (1 + sqrt(1 + a^2))) */
    int halvings = 0;
    const fpb eighth = fb_ldexp(K_ONE, -3);
    while (fb_cmp(a, eighth) > 0) {
        a = fb_div(a, fb_add(K_ONE, fb_sqrt(fb_add(K_ONE, fb_mul(a, a)))));
        halvings++;
    }
    const fpb a2 = fb_mul(a, a);
    fpb term = a, sum = a;
    for (int k = 1; k < 200; k++) {
        term = fb_mul(term, a2);
        const fpb q = fb_div(term, fb_small(2 * k + 1));
        sum = (k & 1) ? fb_sub(sum, q) : fb_add(sum, q);
        if (tiny_vs(q, sum)) break;
    }
    sum = fb_ldexp(sum, halvings);
    if (inv) sum = fb_sub(K_PI2, sum);
    sum.s = s;
    return sum;
}

/* sin and cos of |r| <= pi/4 */
static fpb sin_series(fpb r) {
    const fpb r2 = fb_mul(r, r);
    fpb term = r, sum = r;
    for (int n = 1; n < 100; n++) {
        term = fb_div(fb_mul(term, r2), fb_small((2 * n) * (2 * n + 1)));
        sum = (n & 1) ? fb_sub(sum, term) : fb_add(sum, term);
        if (tiny_vs(term, sum)) break;
    }
    return sum;
}
static fpb cos_series(fpb r) {
    const fpb r2 = fb_mul(r, r);
    fpb term = K_ONE, sum = K_ONE;
    for (int n = 1; n < 100; n++) {
        term = fb_div(fb_mul(term, r2), fb_small((2 * n - 1) * (2 * n)));
        sum = (n & 1) ? fb_sub(sum, term) : fb_add(sum, term);
        if (tiny_vs(term, sum)) break;
    }
    return sum;
}

/* tan x (alt = 0) or tanh x (alt = 1) for |x| < 2^-8 by their series, so the
 * terms after x keep the result on the right side of x for directed
 * rounding: x + x^3/3 + 2x^5/15 + 17x^7/315 + ..., alternating for tanh. */
static fpb tan_series(fpb x, int alt) {
    static const int64_t num[] = {1, 2, 17, 62, 1382, 21844, 929569, 6404582};
    static const int64_t den[] = {3, 15, 315, 2835, 155925, 6081075, 638512875, 10854718875};
    const fpb x2 = fb_mul(x, x);
    fpb p = x, sum = x;
    for (int k = 0; k < 8; k++) {
        p = fb_mul(p, x2);
        const fpb t = fb_div(fb_mul(p, fb_small(num[k])), fb_small(den[k]));
        sum = (alt && !(k & 1)) ? fb_sub(sum, t) : fb_add(sum, t);
    }
    return sum;
}

/* --- exact reduction modulo pi/2 (big fixed-point numbers) ------------------------ */

#define PI_FRAC_WORDS 540u          /* 17,280 fractional bits of pi/2 */
static uint32_t pi2_big[1 + PI_FRAC_WORDS];   /* [0] integer part, then fraction words, most significant first */
static int pi_ready;

/* big = big / d */
static void big_div(uint32_t *b, unsigned n, uint32_t d) {
    uint64_t r = 0;
    for (unsigned i = 0; i < n; i++) {
        const uint64_t cur = (r << 32) | b[i];
        b[i] = (uint32_t)(cur / d);
        r = cur % d;
    }
}
static void big_add(uint32_t *a, const uint32_t *b, unsigned n) {
    uint64_t c = 0;
    for (unsigned i = n; i-- > 0;) {
        const uint64_t s = (uint64_t)a[i] + b[i] + c;
        a[i] = (uint32_t)s;
        c = s >> 32;
    }
}
static void big_sub(uint32_t *a, const uint32_t *b, unsigned n) {
    int64_t borrow = 0;
    for (unsigned i = n; i-- > 0;) {
        int64_t d = (int64_t)a[i] - b[i] - borrow;
        borrow = d < 0;
        if (d < 0) d += 0x100000000ll;
        a[i] = (uint32_t)d;
    }
}
static int big_is_zero(const uint32_t *a, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (a[i]) return 0;
    return 1;
}
static int big_cmp(const uint32_t *a, const uint32_t *b, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

/* atan(1/x) x mult into acc (fixed point, 1 + PI_FRAC_WORDS words). */
static void big_atan_inv(uint32_t *acc, uint32_t x, int mult, int negate) {
    enum { N = 1 + PI_FRAC_WORDS };
    static uint32_t term[N], t[N];
    memset(term, 0, sizeof term);
    term[0] = (uint32_t)mult;
    big_div(term, N, x);
    for (uint32_t k = 0;; k++) {
        memcpy(t, term, sizeof t);
        big_div(t, N, 2 * k + 1);
        if (big_is_zero(t, N)) break;
        if ((k & 1) ^ (unsigned)negate) big_sub(acc, t, N);
        else big_add(acc, t, N);
        big_div(term, N, x);
        big_div(term, N, x);
    }
}

static void init_pi(void) {
    if (pi_ready) return;
    enum { N = 1 + PI_FRAC_WORDS };
    memset(pi2_big, 0, sizeof pi2_big);
    /* pi/2 = 8 atan(1/5) - 2 atan(1/239) */
    big_atan_inv(pi2_big, 5, 8, 0);
    big_atan_inv(pi2_big, 239, 2, 1);
    pi_ready = 1;
    (void)N;
}

/* |x| = m 2^(e-63) reduced: *q (mod 4) and the remainder r in [-pi/4, pi/4]. */
static fpb reduce_pi2(uint64_t m, int32_t e, unsigned *q) {
    init_consts();
    init_pi();
    /* X as fixed point with the same fraction as pi/2: m << (e - 63 + F),
     * F = 32 PI_FRAC_WORDS. Integer words enough for e + 1 bits. */
    const int32_t ibits = e + 2 > 1 ? e + 2 : 1;
    const unsigned iw = (unsigned)(ibits + 31) / 32 + 1;
    const unsigned n = iw + PI_FRAC_WORDS;
    static uint32_t X[1200], T[1200];
    memset(X, 0, n * 4);
    /* place m: bit position (from the binary point) of m's lsb is e - 63 */
    for (int b = 0; b < 64; b++) {
        if (!((m >> b) & 1u)) continue;
        const int32_t pos = e - 63 + b;                 /* weight 2^pos */
        const int64_t bitidx = (int64_t)PI_FRAC_WORDS * 32 + pos;   /* from the lsb of X */
        if (bitidx < 0) continue;
        const unsigned w = n - 1 - (unsigned)(bitidx / 32);
        X[w] |= 1u << (bitidx % 32);
    }
    /* T = pi/2 aligned in the same layout */
    memset(T, 0, n * 4);
    memcpy(T + (iw - 1), pi2_big, sizeof pi2_big);
    /* shift T left as far as it stays <= X (bit lengths) */
    int64_t xbits = 0, tbits = 0;
    for (unsigned i = 0; i < n; i++)
        if (X[i]) { for (int b = 31; b >= 0; b--) if ((X[i] >> b) & 1u) { xbits = (int64_t)(n - 1 - i) * 32 + b + 1; break; } break; }
    for (unsigned i = 0; i < n; i++)
        if (T[i]) { for (int b = 31; b >= 0; b--) if ((T[i] >> b) & 1u) { tbits = (int64_t)(n - 1 - i) * 32 + b + 1; break; } break; }
    int64_t sh = xbits - tbits;
    unsigned quo = 0;
    if (sh >= 0) {
        /* T <<= sh */
        const unsigned ws = (unsigned)(sh / 32), bs = (unsigned)(sh % 32);
        for (unsigned i = 0; i < n; i++) {
            const unsigned src = i + ws;
            uint32_t v = 0;
            if (src < n) v = T[src] << bs;
            if (bs && src + 1 < n) v |= T[src + 1] >> (32 - bs);
            T[i] = v;
        }
        for (int64_t i = sh; i >= 0; i--) {
            quo <<= 1;
            if (big_cmp(X, T, n) >= 0) {
                big_sub(X, T, n);
                quo |= 1;
            }
            /* T >>= 1 */
            for (unsigned k = n; k-- > 0;) T[k] = (T[k] >> 1) | (k ? T[k - 1] << 31 : 0);
        }
    }
    /* r = X (fixed point, in [0, pi/2)); fold to [-pi/4, pi/4] */
    fpb r;
    {
        unsigned first = n;
        for (unsigned i = 0; i < n; i++) if (X[i]) { first = i; break; }
        if (first == n) {
            r = fb_from_int(0);
        } else {
            uint64_t h = 0, l = 0;
            for (unsigned k = 0; k < 4; k++) {
                const uint64_t w = first + k < n ? X[first + k] : 0;
                if (k < 2) h = (h << 32) | w;
                else l = (l << 32) | w;
            }
            /* (h:l) holds words first..first+3; its lsb weight: */
            const int64_t lsbpos = ((int64_t)(n - 1 - (first + 3))) * 32 - (int64_t)PI_FRAC_WORDS * 32;
            r = fb_make(0, (int32_t)(lsbpos + 127), h, l);
        }
    }
    if (fb_cmp(r, K_PI4) > 0) {
        r = fb_sub(r, K_PI2);
        quo++;
    }
    *q = quo & 3u;
    return r;
}

/* --- the operations ----------------------------------------------------------------- */

static fpx result(fpb v, int exact_zero) {
    if (exact_zero && fb_is_zero(v)) return fx_zero(v.s);
    return fb_to_x(v);
}

/* Special-value handling shared by the functions: NaN in -> NaN out (the
 * caller handles NaNs before calling); here only zeros and infinities. */
fpx ft_eval(unsigned op, fpx x, unsigned *exc) {
    init_consts();
    const int s = x.s;
    const int zero = x.k == FX_ZERO, inf = x.k == FX_INF;
    fpb b = fb_from_x(x), ab = b;
    ab.s = 0;
    switch (op) {
    case FT_SINH:
        if (zero || inf) return x;
        if (fb_cmp(ab, K_ONE) < 0) {
            const fpb x2 = fb_mul(b, b);
            fpb term = b, sum = b;
            for (int n = 1; n < 100; n++) {
                term = fb_div(fb_mul(term, x2), fb_small((2 * n) * (2 * n + 1)));
                sum = fb_add(sum, term);
                if (tiny_vs(term, sum)) break;
            }
            return result(sum, 0);
        } else {
            const fpb e = exp_fb(ab);
            fpb r = fb_ldexp(fb_sub(e, fb_div(K_ONE, e)), -1);
            r.s = s;
            return result(r, 0);
        }
    case FT_COSH:
        if (inf) return fx_inf(0);
        if (zero) return fx_from_u64(1);
        {
            const fpb e = exp_fb(ab);
            return result(fb_ldexp(fb_add(e, fb_div(K_ONE, e)), -1), 0);
        }
    case FT_TANH:
        if (zero) return x;
        if (inf || fb_cmp(ab, fb_small(40)) >= 0) {
            /* |x| >= 40: 1 - tanh |x| < 2^-114, far below the last place:
             * just under 1 for the directed modes */
            fpb one = K_ONE;
            if (inf) { fpx r = fx_from_u64(1); r.s = (uint8_t)s; return r; }
            fpb r = fb_sub(one, fb_ldexp(one, -200));
            r.s = s;
            return result(r, 0);
        } else {
            if (ab.e < -8) return result(tan_series(b, 1), 0);
            const fpb em = expm1_fb(fb_ldexp(ab, 1));
            /* tanh = em / (em + 2) = 1 - 2 / (em + 2): the second form keeps
             * the small difference from 1 exact enough for directed rounding */
            fpb r = fb_cmp(ab, K_ONE) < 0 ? fb_div(em, fb_add(em, K_TWO)) : fb_sub(K_ONE, fb_div(K_TWO, fb_add(em, K_TWO)));
            r.s = s;
            return result(r, 0);
        }
    case FT_ATAN:
        if (zero) return x;
        if (inf) { fpb r = K_PI2; r.s = s; return result(r, 0); }
        return result(atan_fb(b), 0);
    case FT_ASIN:
    case FT_ACOS: {
        if (inf || (x.k == FX_NORM && fb_cmp(ab, K_ONE) > 0)) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (op == FT_ASIN) {
            if (zero) return x;
            if (fb_cmp(ab, K_ONE) == 0) { fpb r = K_PI2; r.s = s; return result(r, 0); }
            const fpb d = fb_sqrt(fb_mul(fb_sub(K_ONE, ab), fb_add(K_ONE, ab)));
            fpb r = atan_fb(fb_div(ab, d));
            r.s = s;
            return result(r, 0);
        }
        if (zero) return result(K_PI2, 0);
        if (fb_cmp(b, K_ONE) == 0) return fx_zero(0);
        if (fb_cmp(b, fb_neg(K_ONE)) == 0) return result(K_PI, 0);
        return result(fb_ldexp(atan_fb(fb_sqrt(fb_div(fb_sub(K_ONE, b), fb_add(K_ONE, b)))), 1), 0);
    }
    case FT_ATANH:
        if (zero) return x;
        if (inf || fb_cmp(ab, K_ONE) > 0) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (fb_cmp(ab, K_ONE) == 0) { *exc |= FX_DZ; return fx_inf(s); }
        if (fb_cmp(ab, K_HALF) < 0) return result(atanh_series(b), 0);
        {
            fpb r = fb_ldexp(log_fb(fb_div(fb_add(K_ONE, ab), fb_sub(K_ONE, ab))), -1);
            r.s = s;
            return result(r, 0);
        }
    case FT_SIN:
    case FT_COS:
    case FT_TAN: {
        if (inf) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (zero) return op == FT_COS ? fx_from_u64(1) : x;
        unsigned q;
        const fpb r = reduce_pi2(x.m, x.e, &q);
        if (op == FT_TAN) {
            const fpb sn = sin_series(r), cs = cos_series(r);
            fpb t = (q & 1) ? fb_neg(fb_div(cs, sn)) : (r.e < -8 ? tan_series(r, 0) : fb_div(sn, cs));
            if (s) t = fb_neg(t);
            return result(t, 0);
        }
        if (op == FT_COS) q++;
        fpb v = (q & 1) ? cos_series(r) : sin_series(r);
        if (q & 2) v = fb_neg(v);
        if (op == FT_SIN && s) v = fb_neg(v);
        return result(v, 0);
    }
    case FT_ETOX:
    case FT_TWOTOX:
    case FT_TENTOX:
    case FT_ETOXM1: {
        if (zero) return op == FT_ETOXM1 ? x : fx_from_u64(1);
        if (inf) {
            if (!s) return x;
            if (op == FT_ETOXM1) { fpx r = fx_from_u64(1); r.s = 1; return r; }
            return fx_zero(0);
        }
        if (op == FT_TWOTOX || op == FT_TENTOX) {
            /* exact powers for integer arguments */
            if (x.e < 62 && x.e >= 0 && !(x.m << (x.e + 1))) {
                const int64_t n = (int64_t)(x.m >> (63 - x.e)) * (s ? -1 : 1);
                if (op == FT_TWOTOX && n > -20000 && n < 20000) return fx_scale(fx_from_u64(1), (int32_t)n);
                if (op == FT_TENTOX && n >= 0 && n <= 27) {
                    uint64_t p5 = 1;
                    for (int64_t i = 0; i < n; i++) p5 *= 5;
                    return fx_scale(fx_from_u64(p5), (int32_t)n);
                }
            }
            const fpb arg = fb_mul(b, op == FT_TWOTOX ? K_LN2 : K_LN10);
            return result(exp_fb(arg), 0);
        }
        if (op == FT_ETOXM1) return result(expm1_fb(b), 0);
        return result(exp_fb(b), 0);
    }
    case FT_LOGN:
    case FT_LOG10:
    case FT_LOG2:
        if (zero) { *exc |= FX_DZ; return fx_inf(1); }
        if (s) { *exc |= FX_OPERR; return fx_default_nan(); }
        if (inf) return x;
        if (fb_cmp(b, K_ONE) == 0) return fx_zero(0);
        if (op == FT_LOG2 && x.m == 0x8000000000000000ull && !x.lo) return fx_from_int(x.e);
        if (op == FT_LOG10) {
            /* exact for powers of ten */
            for (int n = 1; n <= 27; n++) {
                uint64_t p5 = 1;
                for (int i = 0; i < n; i++) p5 *= 5;
                const fpx p = fx_scale(fx_from_u64(p5), n);
                if (p.e == x.e && p.m == x.m) return fx_from_int(n);
            }
        }
        {
            const fpb l = log_fb(b);
            if (op == FT_LOGN) return result(l, 0);
            return result(fb_div(l, op == FT_LOG2 ? K_LN2 : K_LN10), 0);
        }
    case FT_LOGNP1:
        if (zero) return x;
        if (inf) {
            if (s) { *exc |= FX_OPERR; return fx_default_nan(); }
            return x;
        }
        if (s && fb_cmp(ab, K_ONE) == 0) { *exc |= FX_DZ; return fx_inf(1); }
        if (s && fb_cmp(ab, K_ONE) > 0) { *exc |= FX_OPERR; return fx_default_nan(); }
        return result(log1p_fb(b), 0);
    default:
        return x;
    }
}

/* --- FMOVECR (M68000 PRM, FMOVECR offsets) ----------------------------------------- */

fpx ft_constant(unsigned offset, int *exact) {
    init_consts();
    *exact = 0;
    static const int exp10[] = {0, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096};
    if (offset == 0x0F) { *exact = 1; return fx_zero(0); }
    if (offset >= 0x32 && offset <= 0x3F) {
        const int n = exp10[offset - 0x32];
        if (n <= 27) {
            uint64_t p5 = 1;
            for (int i = 0; i < n; i++) p5 *= 5;
            *exact = 1;
            return fx_scale(fx_from_u64(p5), n);
        }
        return fb_to_x(pow10_fb(n));
    }
    switch (offset) {
    case 0x00: return fb_to_x(K_PI);
    case 0x0B: return fb_to_x(fb_div(K_LN2, K_LN10));
    case 0x0C: return fb_to_x(exp_fb(K_ONE));
    case 0x0D: return fb_to_x(fb_div(K_ONE, K_LN2));
    case 0x0E: return fb_to_x(fb_div(K_ONE, K_LN10));
    case 0x30: return fb_to_x(K_LN2);
    case 0x31: return fb_to_x(K_LN10);
    default:
        /* offsets the ROM leaves undefined: zero */
        *exact = 1;
        return fx_zero(0);
    }
}

/* --- packed decimal (MC68881 UM 3.4, 3.6 Figure 3-11, Table 3-4) -------------------- */

static uint64_t pow5(int n) { uint64_t p = 1; while (n-- > 0) p *= 5; return p; }

fpx ft_from_packed(const uint32_t w[3], unsigned mode, unsigned *exc) {
    const int sm = (int)(w[0] >> 31), se = (int)((w[0] >> 30) & 1);
    if ((w[0] & 0x7FFF0000u) == 0x7FFF0000u) {
        fpx v = fx_zero(sm);
        if (w[1] | w[2]) {
            v.k = FX_NAN;
            v.m = (uint64_t)w[1] << 32 | w[2];
        } else {
            v.k = FX_INF;
        }
        return v;
    }
    /* 17 digits: the integer digit, then sixteen fraction digits. */
    uint64_t M = w[0] & 15u;
    for (int i = 0; i < 16; i++) {
        const uint32_t word = i < 8 ? w[1] : w[2];
        M = M * 10 + ((word >> (28 - 4 * (i & 7))) & 15u);
    }
    if (M == 0) return fx_zero(sm);
    const int32_t e10 = (int32_t)(((w[0] >> 24) & 15u) * 100 + ((w[0] >> 20) & 15u) * 10 + ((w[0] >> 16) & 15u));
    const int32_t k = (se ? -e10 : e10) - 16;
    fpx v;
    unsigned ex = 0;
    if (k >= 0 && k <= 27) {
        /* exact: M 5^k 2^k as a 128-bit product */
        const uint64_t p = pow5(k);
        const uint64_t a0 = (uint32_t)M, a1 = M >> 32, b0 = (uint32_t)p, b1 = p >> 32;
        const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
        const uint64_t mid = (p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;
        const uint64_t lo = (mid << 32) | (uint32_t)p00;
        const uint64_t hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
        v = fx_scale(fx_from_u128(hi, lo), k);
    } else if (k < 0 && k >= -27 && M % pow5(-k) == 0) {
        v = fx_scale(fx_from_u64(M / pow5(-k)), k);
    } else {
        v = fb_to_x(fb_mul(fb_from_u64(M), pow10_fb(k)));
    }
    v.s = (uint8_t)sm;
    v = fx_round(v, &FMT_X, mode, &ex);
    if (ex & FX_INEX2) *exc |= FX_INEX1;
    return v;
}

/* floor(log10 |x|) for a normalized x, exact. */
static int32_t ilog10(fpx x) {
    double approx = ((double)x.e + log2((double)(x.m >> 11) / 4503599627370496.0)) * 0.30102999566398119521;
    int32_t g = (int32_t)floor(approx);
    fpb ax = fb_from_x(x);
    ax.s = 0;
    for (int i = 0; i < 3; i++) {
        if (fb_cmp(ax, pow10_fb(g)) < 0) g--;
        else if (fb_cmp(ax, pow10_fb(g + 1)) >= 0) g++;
        else break;
    }
    return g;
}

/* |x| x 10^s rounded to an integer in `mode` (the sign of x for directed
 * rounding); *inexact. Exact integer arithmetic where it fits, else the
 * working format with ties detected near integers. */
static uint64_t scale_round(fpx x, int32_t s, unsigned mode, int *inexact) {
    fpx ax = x;
    unsigned ex = 0;
    ax.s = x.s;
    fpx y;
    if (s >= 0 && s <= 27) {
        unsigned e2 = 0;
        y = fx_mul(ax, fx_scale(fx_from_u64(pow5(s)), s), &e2);   /* exact 128-bit product */
    } else if (s < 0 && s >= -27) {
        unsigned e2 = 0;
        y = fx_div(ax, fx_scale(fx_from_u64(pow5(-s)), -s), &e2);  /* quotient with sticky */
    } else {
        /* |s| >= 28: the product cannot be an integer exactly, nor a tie */
        y = fb_to_x(fb_mul(fb_from_x(ax), pow10_fb(s)));
    }
    const fpx r = fx_round_int(y, mode, &ex);
    *inexact = (ex & FX_INEX2) != 0;
    if (r.k != FX_NORM) return 0;
    return r.e >= 63 ? r.m : r.m >> (63 - r.e);
}

void ft_to_packed(fpx x, int k, unsigned mode, unsigned *exc, uint32_t w[3]) {
    const uint32_t sm = x.s ? 0x80000000u : 0;
    w[0] = w[1] = w[2] = 0;
    if (x.k == FX_INF || x.k == FX_NAN) {
        w[0] = sm | 0x7FFF0000u;
        if (x.k == FX_NAN) {
            w[1] = (uint32_t)(x.m >> 32);
            w[2] = (uint32_t)x.m;
        }
        return;
    }
    if (k > 17) {
        *exc |= FX_OPERR;
        k = 17;
    }
    if (x.k == FX_ZERO) {
        w[0] = sm;
        return;
    }
    int32_t ilog = ilog10(x);
    int32_t len = k > 0 ? k : ilog + 1 - k;
    if (len > 17) len = 17;
    if (len < 1) len = 1;
    int inexact;
    uint64_t y = scale_round(x, len - 1 - ilog, mode, &inexact);
    uint64_t lim = 1;
    for (int i = 0; i < len; i++) lim *= 10;
    if (y >= lim) {
        /* rounding carried into a new leading digit */
        ilog++;
        if (k <= 0 && len < 17) len++;
        else y = scale_round(x, len - 1 - ilog, mode, &inexact);
    }
    if (inexact) *exc |= FX_INEX2;
    /* digits: y has len digits; the first is the integer digit */
    uint8_t d[17];
    memset(d, 0, sizeof d);
    for (int i = len - 1; i >= 0; i--) {
        d[i] = (uint8_t)(y % 10);
        y /= 10;
    }
    w[0] = sm | d[0];
    for (int i = 1; i < 17; i++) {
        uint32_t *word = i <= 8 ? &w[1] : &w[2];
        const int pos = (i - 1) & 7;
        *word |= (uint32_t)d[i] << (28 - 4 * pos);
    }
    int32_t ae = ilog < 0 ? -ilog : ilog;
    if (ilog < 0) w[0] |= 0x40000000u;
    if (ae > 999) {
        *exc |= FX_OPERR;
        w[0] |= (uint32_t)((ae / 1000) % 10) << 12;
    }
    w[0] |= (uint32_t)((ae / 100) % 10) << 24 | (uint32_t)((ae / 10) % 10) << 20 | (uint32_t)(ae % 10) << 16;
}
