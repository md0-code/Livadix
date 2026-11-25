#include "ppc_fp_soft.h"
#include <string.h>

/* Original integer arithmetic from MPC601UM 2.5.1/2.5.1.1, 2.5.2,
   2.5.3, 2.5.6 and 5.4.7. No host floating-point operations are used.
   Exact products occupy at most 106 bits; alignment of any two finite
   double operands/products spans less than 4352 bits. The extra low bits
   also hold the 56-bit quotient with its remainder jammed into bit zero. */
enum { FP_BASE = -2208, FP_LIMBS = 136 };
#define FP_SIGN UINT64_C(0x8000000000000000)
#define FP_FRAC UINT64_C(0x000fffffffffffff)
#define FP_UNIT UINT64_C(0x0010000000000000)
#define FP_INF  UINT64_C(0x7ff0000000000000)
#define FP_QUIET UINT64_C(0x0008000000000000)
#define FP_QNAN (FP_INF | FP_QUIET)

typedef struct fp_big { uint32_t limb[FP_LIMBS]; } fp_big;
typedef struct fp_number {
    uint64_t bits, sig;
    int exp;
    bool sign, inf, nan, snan;
} fp_number;

static fp_number fp_decode(uint64_t bits)
{
    fp_number n;
    unsigned exponent = (unsigned)((bits >> 52) & 2047);
    n.bits = bits;
    n.sign = (bits & FP_SIGN) != 0;
    n.sig = bits & FP_FRAC;
    n.inf = exponent == 2047 && n.sig == 0;
    n.nan = exponent == 2047 && n.sig != 0;
    n.snan = n.nan && (bits & FP_QUIET) == 0;
    n.exp = exponent ? (int)exponent - 1023 - 52 : -1074;
    if (exponent != 0 && exponent != 2047) n.sig |= FP_UNIT;
    return n;
}

static unsigned fp_high64(uint64_t value)
{
    unsigned bit = 0;
    while (value >>= 1) ++bit;
    return bit;
}

static void big_put(fp_big *out, uint64_t value, int bit)
{
    unsigned i;
    for (i = 0; value; ++i, value >>= 1) {
        int pos = bit + (int)i;
        if ((value & 1) && pos >= 0 && pos < FP_LIMBS * 32)
            out->limb[(unsigned)pos / 32] |= UINT32_C(1) << ((unsigned)pos % 32);
    }
}

static void big_number(fp_big *out, fp_number n)
{
    memset(out, 0, sizeof(*out));
    big_put(out, n.sig, n.exp - FP_BASE);
}

static void big_product(fp_big *out, fp_number a, fp_number c)
{
    uint32_t product[4] = { 0, 0, 0, 0 };
    uint32_t av[2] = { (uint32_t)a.sig, (uint32_t)(a.sig >> 32) };
    uint32_t cv[2] = { (uint32_t)c.sig, (uint32_t)(c.sig >> 32) };
    unsigned i, j;
    memset(out, 0, sizeof(*out));
    for (i = 0; i < 2; ++i) {
        uint64_t carry = 0;
        for (j = 0; j < 2; ++j) {
            uint64_t term = (uint64_t)av[i] * cv[j] + product[i + j] + carry;
            product[i + j] = (uint32_t)term;
            carry = term >> 32;
        }
        product[i + 2] = (uint32_t)carry;
    }
    for (i = 0; i < 4; ++i)
        big_put(out, product[i], a.exp + c.exp - FP_BASE + (int)i * 32);
}

static int big_compare(const fp_big *a, const fp_big *b)
{
    int i;
    for (i = FP_LIMBS - 1; i >= 0; --i) {
        if (a->limb[i] != b->limb[i])
            return a->limb[i] > b->limb[i] ? 1 : -1;
    }
    return 0;
}

static void big_sum(fp_big *a, const fp_big *b, bool subtract)
{
    uint64_t carry = 0;
    unsigned i;
    for (i = 0; i < FP_LIMBS; ++i) {
        if (subtract) {
            uint64_t rhs = (uint64_t)b->limb[i] + carry;
            uint64_t lhs = a->limb[i];
            a->limb[i] = (uint32_t)(lhs - rhs);
            carry = lhs < rhs;
        } else {
            uint64_t value = (uint64_t)a->limb[i] + b->limb[i] + carry;
            a->limb[i] = (uint32_t)value;
            carry = value >> 32;
        }
    }
}

static bool big_signed_sum(fp_big *a, bool as, fp_big *b, bool bs, unsigned rn)
{
    int compare;
    if (as == bs) {
        big_sum(a, b, false);
        return as;
    }
    compare = big_compare(a, b);
    if (compare >= 0) {
        big_sum(a, b, true);
        return compare == 0 ? rn == 3 : as;
    }
    big_sum(b, a, true);
    *a = *b;
    return bs;
}

static int big_high(const fp_big *a)
{
    int i;
    for (i = FP_LIMBS - 1; i >= 0; --i)
        if (a->limb[i]) return i * 32 + (int)fp_high64(a->limb[i]);
    return -1;
}

static bool big_bit(const fp_big *a, int bit)
{
    if (bit < 0 || bit >= FP_LIMBS * 32) return false;
    return (a->limb[(unsigned)bit / 32] & (UINT32_C(1) << ((unsigned)bit % 32))) != 0;
}

static bool big_below(const fp_big *a, int bit)
{
    unsigned i, count;
    if (bit <= 0) return false;
    if (bit > FP_LIMBS * 32) bit = FP_LIMBS * 32;
    count = (unsigned)bit / 32;
    for (i = 0; i < count; ++i) if (a->limb[i]) return true;
    if ((unsigned)bit % 32 != 0 && count < FP_LIMBS)
        return (a->limb[count] & ((UINT32_C(1) << ((unsigned)bit % 32)) - 1)) != 0;
    return false;
}

static uint64_t big_extract(const fp_big *a, int low, unsigned width)
{
    uint64_t value = 0;
    unsigned i;
    for (i = 0; i < width; ++i)
        if (big_bit(a, low + (int)i)) value |= UINT64_C(1) << i;
    return value;
}

static bool fp_increment(unsigned rn, bool sign, uint64_t q, bool guard, bool sticky)
{
    if (!guard && !sticky) return false;
    if (rn == 0) return guard && (sticky || (q & 1) != 0);
    if (rn == 2) return !sign;
    if (rn == 3) return sign;
    return false;
}

static uint64_t fp_pack(uint64_t sig, int exp, bool sign)
{
    unsigned high;
    int exponent;
    uint64_t bits = sign ? FP_SIGN : 0;
    if (!sig) return bits;
    high = fp_high64(sig);
    exponent = exp + (int)high;
    if (exponent > 1023) return bits | FP_INF; /* Undefined single operands. */
    if (exponent >= -1022) {
        bits |= (uint64_t)(exponent + 1023) << 52;
        bits |= (sig << (52 - high)) & FP_FRAC;
    } else if (exp >= -1074) {
        bits |= sig << (unsigned)(exp + 1074);
    } else if (exp + 1074 > -64) {
        bits |= sig >> (unsigned)(-1074 - exp);
    }
    return bits;
}

static unsigned fp_class(uint64_t bits, int emin)
{
    fp_number n = fp_decode(bits);
    if (n.nan) return 17;
    if (n.inf) return n.sign ? 9 : 5;
    if (!n.sig) return n.sign ? 18 : 2;
    if (n.exp + (int)fp_high64(n.sig) < emin) return n.sign ? 24 : 20;
    return n.sign ? 8 : 4;
}

static ppc_fp_result fp_round(const fp_big *exact, bool sign, bool single, uint32_t fpscr, bool classic)
{
    ppc_fp_result r = { 0, 0, 0, true, true };
    int high = big_high(exact);
    int p = single ? 24 : 53;
    int emin = single ? -126 : -1022;
    int emax = single ? 127 : 1023;
    int adjust = single ? 192 : 1536;
    int exponent, low, unit_exp;
    uint64_t q;
    bool tiny, guard, sticky, inexact, increment;
    if (high < 0) {
        r.bits = sign ? FP_SIGN : 0;
        r.fprf = sign ? 18 : 2;
        return r;
    }
    exponent = high + FP_BASE;
    tiny = exponent < emin;
    low = high - p + 1;
    if (tiny && !(fpscr & PPC_FP_UE)) low = emin - p + 1 - FP_BASE;
    q = big_extract(exact, low, (unsigned)p);
    guard = big_bit(exact, low - 1);
    sticky = big_below(exact, low - 1);
    inexact = guard || sticky;
    increment = fp_increment(fpscr & 3, sign, q, guard, sticky);
    if (increment) ++q;
    unit_exp = low + FP_BASE;
    if (q >= (UINT64_C(1) << (unsigned)p)) {
        q >>= 1;
        ++unit_exp;
    }
    exponent = q ? unit_exp + (int)fp_high64(q) : emin;
    if (inexact) r.raised |= PPC_FP_XX | PPC_FP_FI;
    if (increment) r.raised |= PPC_FP_FR;
    if (exponent > emax) {
        r.raised |= PPC_FP_OX;
        if (fpscr & PPC_FP_OE) {
            unit_exp -= adjust;
        } else {
            bool infinity = (fpscr & 3) == 0 ||
                ((fpscr & 3) == 2 && !sign) || ((fpscr & 3) == 3 && sign);
            /* MPC601UM 5.4.7.4.1 explicitly clears FR/FI on disabled
               overflow; XX is nevertheless set. This is 601-specific. */
            r.raised = (r.raised & ~(PPC_FP_FR | PPC_FP_FI)) | PPC_FP_XX;
            if (classic) r.raised |= PPC_FP_FI; /* PEM 3.7.4: later disabled-overflow FI=1. */
            r.bits = infinity ? (FP_INF | (sign ? FP_SIGN : 0)) :
                fp_pack((UINT64_C(1) << (unsigned)p) - 1, emax - p + 1, sign);
            r.fprf = fp_class(r.bits, emin);
            return r;
        }
    }
    if (tiny) {
        if (fpscr & PPC_FP_UE) {
            r.raised |= PPC_FP_UX;
            unit_exp += adjust;
        } else if (inexact) {
            r.raised |= PPC_FP_UX;
        }
    }
    r.bits = fp_pack(q, unit_exp, sign);
    r.fprf = (tiny && (fpscr & PPC_FP_UE)) ||
        (r.raised & PPC_FP_OX) ? (sign ? 8 : 4) : fp_class(r.bits, emin);
    return r;
}

static void big_quotient(fp_big *out, fp_number a, fp_number b)
{
    uint64_t numerator = a.sig, denominator = b.sig, q = 1, remainder;
    int exp = a.exp - b.exp;
    unsigned i;
    while (numerator < FP_UNIT) { numerator <<= 1; --exp; }
    while (denominator < FP_UNIT) { denominator <<= 1; ++exp; }
    if (numerator < denominator) { numerator <<= 1; --exp; }
    remainder = numerator - denominator;
    for (i = 0; i < 55; ++i) {
        remainder <<= 1;
        q <<= 1;
        if (remainder >= denominator) { ++q; remainder -= denominator; }
    }
    if (remainder) q |= 1;
    memset(out, 0, sizeof(*out));
    big_put(out, q, exp - 55 - FP_BASE);
}

static ppc_fp_result fp_integer(fp_number b, bool toward_zero, uint32_t fpscr)
{
    ppc_fp_result r = { 0, 0, 0, true, false };
    uint64_t mag = b.bits & ~FP_SIGN;
    uint64_t limit = b.sign ? UINT64_C(0x41e0000000000000) : UINT64_C(0x41dfffffffc00000);
    uint64_t q = 0;
    bool guard = false, sticky = false;
    uint32_t integer;
    if (b.nan || b.inf || mag > limit) {
        r.raised = PPC_FP_VXCVI | (b.snan ? PPC_FP_VXSNAN : 0);
        r.write = !(fpscr & PPC_FP_VE);
        /* MPC601UM 3.4.3 and chapter 10 fctiw[z]: saturation, with
           NaN -> most-negative integer (5.4.7.2.1). Upper word is
           the 601-defined QNaN encoding, not host conversion data. */
        integer = b.nan || b.sign ? UINT32_C(0x80000000) : UINT32_C(0x7fffffff);
    } else {
        if (b.exp >= 0) q = b.sig << (unsigned)b.exp;
        else if (b.exp > -64) {
            unsigned shift = (unsigned)-b.exp;
            q = b.sig >> shift;
            guard = ((b.sig >> (shift - 1)) & 1) != 0;
            sticky = shift > 1 && (b.sig & ((UINT64_C(1) << (shift - 1)) - 1)) != 0;
        } else {
            sticky = b.sig != 0;
        }
        if (guard || sticky) r.raised |= PPC_FP_XX | PPC_FP_FI;
        if (fp_increment(toward_zero ? 1 : fpscr & 3, b.sign, q, guard, sticky)) {
            ++q;
            r.raised |= PPC_FP_FR;
        }
        integer = b.sign ? (uint32_t)(UINT64_C(0) - q) : (uint32_t)q;
    }
    r.bits = UINT64_C(0xfff8000000000000) | integer;
    return r;
}

static ppc_fp_result fp_compute(ppc_fp_operation op, bool single,
    uint64_t abits, uint64_t bbits, uint64_t cbits, uint32_t fpscr, bool classic)
{
    fp_number a = fp_decode(abits), b = fp_decode(bbits), c = fp_decode(cbits);
    fp_big x, y;
    ppc_fp_result r = { 0, 0, 0, true, true };
    bool fused = op >= PPC_FP_MADD && op <= PPC_FP_NMSUB;
    bool multiply = op == PPC_FP_MUL || fused;
    bool unary = op >= PPC_FP_ROUND_SINGLE;
    bool subtract = op == PPC_FP_SUB || op == PPC_FP_MSUB || op == PPC_FP_NMSUB;
    bool negate = op == PPC_FP_NMADD || op == PPC_FP_NMSUB;
    bool psign = a.sign != c.sign, bsign = b.sign != subtract;
    uint32_t invalid = 0;
    uint64_t nan = FP_QNAN;
    bool has_nan = false;
    if (op == PPC_FP_TO_INT || op == PPC_FP_TO_INT_ZERO)
        return fp_integer(b, op == PPC_FP_TO_INT_ZERO, fpscr);
    if (unary) {
        if (b.nan) { nan = b.bits | FP_QUIET; has_nan = true; }
        if (b.snan) invalid |= PPC_FP_VXSNAN;
    } else {
        if (a.nan) { nan = a.bits | FP_QUIET; has_nan = true; }
        else if (op != PPC_FP_MUL && b.nan) { nan = b.bits | FP_QUIET; has_nan = true; }
        else if (multiply && c.nan) { nan = c.bits | FP_QUIET; has_nan = true; }
        if (a.snan || (op != PPC_FP_MUL && b.snan) || (multiply && c.snan))
            invalid |= PPC_FP_VXSNAN;
        if (multiply) {
            if ((a.inf && !c.nan && !c.inf && !c.sig) ||
                (c.inf && !a.nan && !a.inf && !a.sig)) invalid |= PPC_FP_VXIMZ;
            if (fused && !a.nan && !c.nan && !b.nan &&
                (a.inf || c.inf) && b.inf && psign != bsign && !(invalid & PPC_FP_VXIMZ))
                invalid |= PPC_FP_VXISI;
        } else if (op == PPC_FP_ADD || op == PPC_FP_SUB) {
            if (a.inf && b.inf && a.sign != bsign) invalid |= PPC_FP_VXISI;
        } else if (op == PPC_FP_DIV) {
            if (a.inf && b.inf) invalid |= PPC_FP_VXIDI;
            if (!a.nan && !a.inf && !a.sig && !b.nan && !b.inf && !b.sig)
                invalid |= PPC_FP_VXZDZ;
        }
    }
    if (has_nan || invalid) {
        r.raised = invalid;
        r.write = !invalid || !(fpscr & PPC_FP_VE);
        r.bits = nan;
        /* PEM appendix D.4.1, MPC601UM 2.5.5: single results retain
           the top 23 payload bits; quieting cannot create infinity. */
        if (single || op == PPC_FP_ROUND_SINGLE) r.bits &= ~UINT64_C(0x1fffffff);
        r.fprf = 17;
        return r;
    }
    if (unary) {
        if (b.inf) r.bits = b.bits;
        else { big_number(&x, b); return fp_round(&x, b.sign, true, fpscr, classic); }
    } else if (op == PPC_FP_ADD || op == PPC_FP_SUB) {
        if (a.inf) r.bits = a.bits;
        else if (b.inf) r.bits = FP_INF | (bsign ? FP_SIGN : 0);
        else {
            big_number(&x, a);
            big_number(&y, b);
            return fp_round(&x, big_signed_sum(&x, a.sign, &y, bsign, fpscr & 3), single, fpscr, classic);
        }
    } else if (multiply) {
        if (a.inf || c.inf) r.bits = FP_INF | (psign ? FP_SIGN : 0);
        else if (fused && b.inf) r.bits = FP_INF | (bsign ? FP_SIGN : 0);
        else {
            big_product(&x, a, c);
            if (fused) {
                big_number(&y, b);
                psign = big_signed_sum(&x, psign, &y, bsign, fpscr & 3);
            }
            r = fp_round(&x, psign, single, fpscr, classic);
            if (negate) {
                r.bits ^= FP_SIGN;
                r.fprf = fp_class(r.bits, single ? -126 : -1022);
                if ((r.raised & PPC_FP_OX) && (fpscr & PPC_FP_OE)) r.fprf = psign ? 4 : 8;
                if ((r.raised & PPC_FP_UX) && (fpscr & PPC_FP_UE)) r.fprf = psign ? 4 : 8;
            }
            return r;
        }
        if (negate) r.bits ^= FP_SIGN;
    } else if (op == PPC_FP_DIV) {
        bool sign = a.sign != b.sign;
        if (!b.sig && !b.inf) {
            r.bits = FP_INF | (sign ? FP_SIGN : 0);
            if (!a.inf) { r.raised = PPC_FP_ZX; r.write = !(fpscr & PPC_FP_ZE); }
        } else if (a.inf) r.bits = FP_INF | (sign ? FP_SIGN : 0);
        else if (b.inf || !a.sig) r.bits = sign ? FP_SIGN : 0;
        else { big_quotient(&x, a, b); return fp_round(&x, sign, single, fpscr, classic); }
    }
    r.fprf = fp_class(r.bits, single ? -126 : -1022);
    return r;
}

ppc_fp_result ppc_fp_compute(ppc_fp_operation op, bool single,
    uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr)
{
    return fp_compute(op, single, a, b, c, fpscr, false);
}

uint64_t ppc_fp_flush(uint64_t bits, bool single)
{
    uint64_t minimum = single ? UINT64_C(0x3810000000000000) : FP_UNIT;
    return (bits & ~FP_SIGN) < minimum ? bits & FP_SIGN : bits;
}

ppc_fp_result ppc_fp_compute_later(ppc_fp_operation op, bool single,
    uint64_t a, uint64_t b, uint64_t c, uint32_t fpscr, bool flush_inputs)
{
    bool precision = single || op == PPC_FP_ROUND_SINGLE;
    ppc_fp_result r;
    /* MPC603eUM 2.3.4.2: denormal results become signed zero. MPC604eUM
       tables 2-12/13 and MPC750UM 2-20/21 also zero denormal operands. */
    if ((fpscr & PPC_FP_NI) && flush_inputs) {
        a = ppc_fp_flush(a, precision); b = ppc_fp_flush(b, precision); c = ppc_fp_flush(c, precision);
    }
    r = fp_compute(op, single, a, b, c, fpscr, true);
    if ((fpscr & PPC_FP_NI) && flush_inputs && r.write && (r.raised & PPC_FP_VXCVI))
        r.bits = (r.bits & UINT64_C(0xffffffff00000000)) | 0x8000u; /* MPC604eUM 2-13, MPC750UM 2-21. */
    if ((fpscr & PPC_FP_NI) && r.classify && r.write) {
        r.bits = ppc_fp_flush(r.bits, precision);
        r.fprf = fp_class(r.bits, precision ? -126 : -1022);
    }
    return r;
}

static uint64_t fp_isqrt(uint64_t n)
{
    uint64_t root = 0, bit = UINT64_C(1) << 62;
    while (bit > n) bit >>= 2;
    while (bit) {
        if (n >= root + bit) { n -= root + bit; root = (root >> 1) + bit; }
        else root >>= 1;
        bit >>= 2;
    }
    return root;
}

ppc_fp_result ppc_fp_estimate(uint64_t bits, uint32_t fpscr, bool square_root, bool flush_inputs)
{
    fp_number b;
    ppc_fp_result r = { 0, 0, 0, true, true };
    if (!square_root) {
        r = ppc_fp_compute_later(PPC_FP_DIV, true, UINT64_C(0x3ff0000000000000), bits, 0, fpscr, flush_inputs);
        /* PEM fres: XX is not affected; FR/FI are implementation-defined. */
        r.raised &= ~(PPC_FP_XX | PPC_FP_FR | PPC_FP_FI);
        return r;
    }
    if ((fpscr & PPC_FP_NI) && flush_inputs) bits = ppc_fp_flush(bits, false);
    b = fp_decode(bits);
    if (b.nan) {
        r.bits = bits | FP_QUIET;
        if (b.snan) r.raised = PPC_FP_VXSNAN;
        r.write = !b.snan || !(fpscr & PPC_FP_VE);
    } else if (!b.inf && !b.sig) {
        r.bits = FP_INF | (b.sign ? FP_SIGN : 0);
        r.raised = PPC_FP_ZX; r.write = !(fpscr & PPC_FP_ZE);
    } else if (b.sign) {
        r.bits = FP_QNAN; r.raised = PPC_FP_VXSQRT; r.write = !(fpscr & PPC_FP_VE);
    } else if (!b.inf) {
        int exponent = b.exp + 52;
        uint64_t root, reciprocal;
        while (b.sig < FP_UNIT) { b.sig <<= 1; --exponent; }
        if (exponent % 2) { b.sig <<= 1; --exponent; }
        /* Q30 radicand, Q31 square root, Q20 reciprocal. Its truncation
           error is below 2^-19, comfortably within PEM 8's 1/4096 bound. */
        root = fp_isqrt((b.sig >> 22) << 32);
        reciprocal = (UINT64_C(1) << 51) / root;
        r.bits = fp_pack(reciprocal, -20 - exponent / 2, false);
    }
    r.fprf = fp_class(r.bits, -1022);
    return r;
}

unsigned ppc_fp_compare(uint64_t abits, uint64_t bbits, bool ordered,
    uint32_t fpscr, uint32_t *raised)
{
    fp_number a = fp_decode(abits), b = fp_decode(bbits);
    uint64_t am = abits & ~FP_SIGN, bm = bbits & ~FP_SIGN;
    *raised = 0;
    if (a.nan || b.nan) {
        bool signaling = a.snan || b.snan;
        if (signaling) *raised |= PPC_FP_VXSNAN;
        /* MPC601UM chapter 10 fcmpo: VE suppresses VXVC for SNaN,
           even if the other operand is a QNaN. FPCC/CR always update. */
        if (ordered && (!signaling || !(fpscr & PPC_FP_VE))) *raised |= PPC_FP_VXVC;
        return 1;
    }
    if (abits == bbits || (!am && !bm)) return 2;
    if (a.sign != b.sign) return a.sign ? 8 : 4;
    if (am < bm) return a.sign ? 4 : 8;
    return a.sign ? 8 : 4;
}

uint32_t ppc_fp_summary(uint32_t fpscr)
{
    fpscr &= ~(PPC_FP_VX | PPC_FP_FEX | PPC_FP_RESERVED);
    if (fpscr & PPC_FP_INVALID) fpscr |= PPC_FP_VX;
    if (((fpscr & PPC_FP_VX) && (fpscr & PPC_FP_VE)) ||
        ((fpscr & PPC_FP_OX) && (fpscr & PPC_FP_OE)) ||
        ((fpscr & PPC_FP_UX) && (fpscr & PPC_FP_UE)) ||
        ((fpscr & PPC_FP_ZX) && (fpscr & PPC_FP_ZE)) ||
        ((fpscr & PPC_FP_XX) && (fpscr & PPC_FP_XE))) fpscr |= PPC_FP_FEX;
    return fpscr;
}

uint64_t ppc_fp_from_single(uint32_t bits)
{
    uint64_t sign = (uint64_t)(bits & UINT32_C(0x80000000)) << 32;
    unsigned exp = (bits >> 23) & 255;
    uint64_t frac = bits & UINT32_C(0x007fffff);
    if (exp == 255) return sign | FP_INF | (frac << 29);
    if (exp) return sign | ((uint64_t)(exp + 896) << 52) | (frac << 29);
    if (!frac) return sign;
    return fp_pack(frac, -149, sign != 0);
}

uint32_t ppc_fp_to_single(uint64_t bits)
{
    unsigned exp = (unsigned)((bits >> 52) & 2047);
    uint32_t sign = (uint32_t)((bits >> 32) & UINT64_C(0x80000000));
    /* MPC601UM 3.5.10.1: literal bit selection, including exceptional
       operands. Out-of-single-range nonzero values are unspecified. */
    if (exp > 896 || (bits & ~FP_SIGN) == 0)
        return (uint32_t)((bits >> 32) & UINT64_C(0xc0000000)) |
            (uint32_t)((bits >> 29) & UINT64_C(0x3fffffff));
    if (exp >= 874) {
        uint64_t frac = (bits & FP_FRAC) | FP_UNIT;
        return sign | (uint32_t)(frac >> (unsigned)(926 - exp));
    }
    return sign;
}
