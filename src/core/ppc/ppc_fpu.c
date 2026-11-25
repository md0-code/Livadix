#include "ppc.h"
#include "ppc_fp_soft.h"

/* MPC601UM chapter 10 instruction encodings; 5.4.7 program causes.
   The CPU dispatch owns the MSR[FP] availability check and FP memory ops. */
static bool fp_illegal(ppc_cpu *cpu)
{
    ppc_exception(cpu, PPC_EX_PROGRAM, UINT32_C(0x00080000), cpu->last_pc);
    return true;
}

static void fp_cr_field(ppc_cpu *cpu, unsigned field, unsigned value)
{
    unsigned shift = 28 - field * 4;
    cpu->cr = (cpu->cr & ~(UINT32_C(15) << shift)) | ((uint32_t)value << shift);
}

static void fp_status(ppc_cpu *cpu, uint32_t before, bool explicit_fx)
{
    /* MPC601UM 2.2.3 and chapter 10 mtfsf/mtfsfi: implicit FX is
       a zero-to-one transition detector, not an OR of sticky flags.
       A write selecting field zero instead explicitly supplies FX. */
    if (!explicit_fx && (cpu->fpscr & ~before & PPC_FP_STICKY)) cpu->fpscr |= PPC_FP_FX;
    cpu->fpscr = ppc_fp_summary(cpu->fpscr) | (cpu->model == PPC_MODEL_601 ? 0u : cpu->fpscr & PPC_FP_NI);
}

static bool fp_finish(ppc_cpu *cpu, uint32_t opcode, bool record, bool may_except)
{
    if (record && (opcode & 1)) fp_cr_field(cpu, 1, cpu->fpscr >> 28);
    /* MPC601UM 5.4.7.1/table 5-18: either FE bit selects precise
       mode on the 601. FPSCR enables govern writeback even in ignore
       mode; an enabled exception is taken after that writeback. */
    /* Figure 5-1: not every FP instruction can cause an enabled exception.
       Register moves, mffs and mcrfs only observe existing status. */
    if (may_except && (cpu->fpscr & PPC_FP_FEX) && (cpu->msr & (PPC_MSR_FE0 | PPC_MSR_FE1)))
        ppc_exception(cpu, PPC_EX_PROGRAM, UINT32_C(0x00100000), cpu->last_pc);
    return true;
}

/* MPC604EUM table 2-12, MPC750UM table 2-20: in non-IEEE mode the 604,
   604e and 750 zero every denormalized operand A, B and C of a
   floating-point operation, register moves and fsel included (their
   selected or moved operand, and fsel's compared A). The 603e zeroes
   only denormalized results (MPC603EUM 2.3.4.2), the 601 has no NI. */
static uint64_t operand(const ppc_cpu *cpu, uint32_t fpscr, unsigned reg)
{
    uint64_t bits = cpu->fpr[reg];
    return cpu->model >= PPC_MODEL_604 && (fpscr & PPC_FP_NI) ? ppc_fp_flush(bits, false) : bits;
}

bool ppc_fpu(ppc_cpu *cpu, uint32_t opcode)
{
    unsigned primary = opcode >> 26;
    unsigned d = (opcode >> 21) & 31, a = (opcode >> 16) & 31;
    unsigned b = (opcode >> 11) & 31, c = (opcode >> 6) & 31;
    unsigned xo = (opcode >> 1) & 1023, axo = xo & 31;
    uint32_t before = cpu->fpscr;
    ppc_fp_operation operation;
    ppc_fp_result result;
    bool single = primary == 59;
    if (primary != 59 && primary != 63) return false;
    if (primary == 63) {
        switch (xo) {
        case 0: case 32: {
            uint32_t raised;
            unsigned relation;
            uint64_t av = operand(cpu, before, a), bv = operand(cpu, before, b);
            if ((d & 3) || (opcode & 1)) return fp_illegal(cpu);
            relation = ppc_fp_compare(av, bv, xo == 32,
                before, &raised);
            cpu->fpscr = (before & ~PPC_FP_FPCC) | (relation << 12) | raised;
            fp_status(cpu, before, false);
            fp_cr_field(cpu, d >> 2, relation);
            return fp_finish(cpu, opcode, false, true);
        }
        case 64: {
            unsigned shift, value;
            if ((d & 3) || (a & 3) || b || (opcode & 1)) return fp_illegal(cpu);
            shift = 28 - (a >> 2) * 4;
            value = (before >> shift) & 15;
            cpu->fpscr &= ~((PPC_FP_STICKY | PPC_FP_FX) & (UINT32_C(15) << shift));
            fp_status(cpu, before, true);
            fp_cr_field(cpu, d >> 2, value);
            return fp_finish(cpu, opcode, false, false);
        }
        case 583:
            if (a || b) return fp_illegal(cpu);
            /* MPC601UM chapter 10 mffs (10-123), not the conflicting
               FFFFFFFF in the chapter 3 summary table. */
            cpu->fpr[d] = UINT64_C(0xfff8000000000000) | before;
            return fp_finish(cpu, opcode, true, false);
        case 38: case 70: {
            uint32_t bit = UINT32_C(0x80000000) >> d;
            if (a || b) return fp_illegal(cpu);
            bit &= ~(PPC_FP_FEX | PPC_FP_VX | (PPC_FP_RESERVED & (cpu->model == PPC_MODEL_601 ? UINT32_MAX : ~PPC_FP_NI)));
            if (xo == 38) cpu->fpscr |= bit;
            else cpu->fpscr &= ~bit;
            fp_status(cpu, before, d == 0);
            return fp_finish(cpu, opcode, true, true);
        }
        case 134: {
            unsigned shift;
            if ((d & 3) || a || (b & 1)) return fp_illegal(cpu);
            shift = 28 - (d >> 2) * 4;
            cpu->fpscr = (before & ~(UINT32_C(15) << shift)) | ((uint32_t)(b >> 1) << shift);
            fp_status(cpu, before, d == 0);
            return fp_finish(cpu, opcode, true, true);
        }
        case 711: {
            unsigned mask = (opcode >> 17) & 255, field;
            uint32_t bits = 0;
            if (opcode & UINT32_C(0x02010000)) return fp_illegal(cpu);
            for (field = 0; field < 8; ++field)
                if (mask & (128u >> field)) bits |= UINT32_C(15) << (28 - field * 4);
            cpu->fpscr = (before & ~bits) | ((uint32_t)cpu->fpr[b] & bits);
            fp_status(cpu, before, (mask & 128) != 0);
            return fp_finish(cpu, opcode, true, true);
        }
        case 40: case 72: case 136: case 264:
            if (a) return fp_illegal(cpu);
            result.bits = operand(cpu, before, b);
            if (xo == 40) result.bits ^= UINT64_C(0x8000000000000000);
            if (xo == 136) result.bits |= UINT64_C(0x8000000000000000);
            if (xo == 264) result.bits &= UINT64_C(0x7fffffffffffffff);
            cpu->fpr[d] = result.bits;
            return fp_finish(cpu, opcode, true, false);
        case 12: case 14: case 15:
            if (a) return fp_illegal(cpu);
            operation = xo == 12 ? PPC_FP_ROUND_SINGLE :
                xo == 14 ? PPC_FP_TO_INT : PPC_FP_TO_INT_ZERO;
            goto compute;
        default:
            break;
        }
    }
    if (cpu->model != PPC_MODEL_601) {
        if (primary == 63 && axo == 23) {
            uint64_t select = operand(cpu, before, a), magnitude = select & UINT64_C(0x7fffffffffffffff);
            bool nonnegative = magnitude == 0 || (!(select >> 63) && magnitude <= UINT64_C(0x7ff0000000000000));
            /* PEM fsel: even SNaNs select B without changing FPSCR. */
            cpu->fpr[d] = operand(cpu, before, nonnegative ? c : b);
            return fp_finish(cpu, opcode, true, false);
        }
        if ((primary == 59 && axo == 24) || (primary == 63 && axo == 26)) {
            if (a || c) return fp_illegal(cpu);
            result = ppc_fp_estimate(cpu->fpr[b], before, axo == 26, cpu->model >= PPC_MODEL_604);
            goto apply;
        }
    }
    /* MPC601UM 10.3/table 10-6: these architectural optional
       operations are illegal on the 601, not host approximations. */
    if (axo == 22 || (primary == 59 && axo == 24) ||
        (primary == 63 && (axo == 23 || axo == 26))) return fp_illegal(cpu);
    switch (axo) {
    case 18: operation = PPC_FP_DIV; if (c) return fp_illegal(cpu); break;
    case 20: operation = PPC_FP_SUB; if (c) return fp_illegal(cpu); break;
    case 21: operation = PPC_FP_ADD; if (c) return fp_illegal(cpu); break;
    case 25: operation = PPC_FP_MUL; if (b) return fp_illegal(cpu); break;
    case 28: operation = PPC_FP_MSUB; break;
    case 29: operation = PPC_FP_MADD; break;
    case 30: operation = PPC_FP_NMSUB; break;
    case 31: operation = PPC_FP_NMADD; break;
    default: return false;
    }
compute:
    result = cpu->model == PPC_MODEL_601
        ? ppc_fp_compute(operation, single, cpu->fpr[a], cpu->fpr[b], cpu->fpr[c], before)
        : ppc_fp_compute_later(operation, single, cpu->fpr[a], cpu->fpr[b], cpu->fpr[c], before, cpu->model >= PPC_MODEL_604);
apply:
    cpu->fpscr = (before & ~(PPC_FP_FR | PPC_FP_FI)) | result.raised;
    if (result.write) {
        cpu->fpr[d] = result.bits;
        if (result.classify)
            cpu->fpscr = (cpu->fpscr & ~PPC_FP_FPRF) | ((uint32_t)result.fprf << 12);
    }
    fp_status(cpu, before, false);
    return fp_finish(cpu, opcode, true, true);
}

/* --- the host-arithmetic path -------------------------------------------------------------
 *
 * ppc_fpu_fast makes the same register, FPSCR and CR changes as ppc_fpu for
 * the ordinary case of the arithmetic, rounding, conversion and compare
 * instructions, on the host's IEEE 754 doubles, and declines everything
 * else (false, nothing changed), which stays ppc_fpu's: operands that are
 * NaNs, infinities or denormalized, exponents near the ends of the range,
 * a rounding mode other than nearest, non-IEEE mode, a result that is not a normal number
 * or zero, an exception that would be taken, reserved fields.
 *
 * The result is the host's correctly rounded one (IEEE 754-2019 4.3.1,
 * roundTiesToEven: the PowerPC's RN = 0, MPC601UM 2.5.6, PEM 3.3.5). FI and
 * FR need the sign of what rounding discarded, which error-free
 * transformations give exactly in this exponent range: Knuth's TwoSum for a
 * sum (The Art of Computer Programming vol. 2, 4.2.2, Theorem B), Dekker's
 * product with Veltkamp's splitting for a product (T. J. Dekker, "A
 * floating-point technique for extending the available precision",
 * Numerische Mathematik 18, 1971), the exactly representable remainder
 * x - q*y for a quotient, and for the fused forms the three-term sum of the
 * product's two parts and the addend. A single-precision result is the
 * double result rounded again, declined in the one case where two
 * roundings differ from one: a double result halfway between two singles
 * that was itself inexact. Host: SSE2 scalar arithmetic in the default
 * rounding mode, no contraction (the pragmas below); on ARM64 the same IEEE 754
 * double arithmetic of its FPU in its default round-to-nearest mode. */
#if defined(_M_X64) || defined(__x86_64__)
#include <emmintrin.h>
#endif
#include <math.h>
#include <string.h>

#ifdef _MSC_VER
#pragma float_control(precise, on)
#pragma fp_contract(off)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#endif   /* GCC has no such pragma: src/CMakeLists.txt compiles this file with -ffp-contract=off */

static double fast_value(uint64_t bits) { double d; memcpy(&d, &bits, sizeof d); return d; }
static uint64_t fast_bits(double d) { uint64_t bits; memcpy(&bits, &d, sizeof bits); return bits; }
static int fast_sign(double d) { return d > 0 ? 1 : d < 0 ? -1 : 0; }

/* Zero, or a normal number whose biased exponent is in [low, high]. */
static bool fast_plain(uint64_t bits, unsigned low, unsigned high)
{
    const unsigned e = (unsigned)(bits >> 52) & 2047u;
    return !(bits << 1) || (e >= low && e <= high);
}

/* Neither a NaN nor denormalized: what the compares order as they stand. */
static bool fast_ordered(uint64_t bits)
{
    const unsigned e = (unsigned)(bits >> 52) & 2047u;
    const uint64_t fraction = bits & UINT64_C(0x000fffffffffffff);
    return e == 0 ? !fraction : e != 2047u || !fraction;
}

static void two_sum(double a, double b, double *sum, double *error)
{
    const double s = a + b, bb = s - a;
    *sum = s;
    *error = (a - (s - bb)) + (b - bb);
}

static void two_product(double a, double b, double *product, double *error)
{
    const double split = 134217729.0;                   /* 2^27 + 1 */
    const double ca = split * a, ah = ca - (ca - a), al = a - ah;
    const double cb = split * b, bh = cb - (cb - b), bl = b - bh;
    const double p = a * b;
    *product = p;
    *error = ((ah * bh - p) + ah * bl + al * bh) + al * bl;
}

bool ppc_fpu_fast(ppc_cpu *cpu, uint32_t opcode)
{
    const unsigned primary = opcode >> 26;
    const unsigned d = (opcode >> 21) & 31, a = (opcode >> 16) & 31;
    const unsigned b = (opcode >> 11) & 31, c = (opcode >> 6) & 31;
    const unsigned xo = (opcode >> 1) & 1023, axo = xo & 31;
    const uint32_t before = cpu->fpscr;
    const uint64_t ab = cpu->fpr[a], bb = cpu->fpr[b], cb = cpu->fpr[c];
    const bool single = primary == 59;
    double x, y, z, r = 0, error = 0;
    int discarded = 0;          /* the sign of (exact result - r) */
    bool negate = false, inexact, rounded_up, classify = true, to_single = single;
    uint64_t bits;
    uint32_t now;
    if ((primary != 59 && primary != 63) || !(cpu->msr & PPC_MSR_FP)) return false;
    /* Non-IEEE mode zeroes denormalized operands and results by the instruction's
       precision (ppc_fp_compute_later): the reference path's. */
    if (cpu->model != PPC_MODEL_601 && (before & PPC_FP_NI)) return false;
    if (primary == 63 && (xo == 0 || xo == 32)) {
        const uint64_t am = ab & ~UINT64_C(0x8000000000000000), bm = bb & ~UINT64_C(0x8000000000000000);
        unsigned relation;
        if ((d & 3) || (opcode & 1)) return false;
        if (!fast_ordered(ab) || !fast_ordered(bb)) return false;
        if (ab == bb || (!am && !bm)) relation = 2;
        else if ((ab >> 63) != (bb >> 63)) relation = ab >> 63 ? 8 : 4;
        else if (am < bm) relation = ab >> 63 ? 4 : 8;
        else relation = ab >> 63 ? 8 : 4;
        now = (before & ~PPC_FP_FPCC) | (relation << 12);
        now = ppc_fp_summary(now) | (cpu->model == PPC_MODEL_601 ? 0u : now & PPC_FP_NI);
        if ((now & PPC_FP_FEX) && (cpu->msr & (PPC_MSR_FE0 | PPC_MSR_FE1))) return false;
        cpu->fpscr = now;
        fp_cr_field(cpu, d >> 2, relation);
        return true;
    }
    if (primary == 63 && (xo == 14 || xo == 15)) {
        const uint64_t magnitude = bb & ~UINT64_C(0x8000000000000000);
        int integer;
        if (a || (xo == 14 && (before & 3))) return false;
        if (!fast_plain(bb, 1, 2046) ||
            magnitude > (bb >> 63 ? UINT64_C(0x41e0000000000000) : UINT64_C(0x41dfffffffc00000))) return false;
        y = fast_value(bb);
#if defined(_M_X64) || defined(__x86_64__)
        integer = xo == 14 ? _mm_cvtsd_si32(_mm_set_sd(y)) : _mm_cvttsd_si32(_mm_set_sd(y));
#else
        /* the default rounding mode is to nearest, ties to even: nearbyint's */
        integer = xo == 14 ? (int)nearbyint(y) : (int)y;
#endif
        inexact = (double)integer != y;
        rounded_up = inexact && fabs((double)integer) > fabs(y);
        bits = UINT64_C(0xfff8000000000000) | (uint32_t)integer;
        classify = false;
        goto finish;
    }
    if (before & 3) return false;
    if (primary == 63 && xo == 12) {
        if (a || !fast_plain(bb, 1, 2046)) return false;
        r = fast_value(bb);
        to_single = true;
    } else switch (axo) {
    case 20: case 21:
        /* below 2^1022, so that no step of TwoSum overflows */
        if (c || !fast_plain(ab, 1, 2044) || !fast_plain(bb, 1, 2044)) return false;
        x = fast_value(ab); y = fast_value(bb);
        two_sum(x, axo == 20 ? -y : y, &r, &error);
        discarded = fast_sign(error);
        break;
    case 25:
        if (b || !fast_plain(ab, 573, 1473) || !fast_plain(cb, 573, 1473)) return false;
        two_product(fast_value(ab), fast_value(cb), &r, &error);
        discarded = fast_sign(error);
        break;
    case 18:
        if (c || !fast_plain(ab, 573, 1473) || !fast_plain(bb, 573, 1473) || !(bb << 1)) return false;
        x = fast_value(ab); y = fast_value(bb);
        r = x / y;
        if (x != 0) {
            double high, low;
            two_product(r, y, &high, &low);
            discarded = fast_sign((x - high) - low) * (y < 0 ? -1 : 1);
        }
        break;
    case 28: case 29: case 30: case 31: {
        double high, low, sh, sl, vh, vl, th, tl, wh, wl;
        if (!fast_plain(ab, 573, 1473) || !fast_plain(cb, 573, 1473) || !fast_plain(bb, 123, 1923)) return false;
        x = fast_value(ab); z = fast_value(cb); y = fast_value(bb);
        if (axo == 28 || axo == 30) y = -y;
        negate = axo >= 30;
        r = fma(x, z, y);
        /* exact = high + low + y = th + tl + vl; r and th are within one unit of it */
        two_product(x, z, &high, &low);
        two_sum(high, y, &sh, &sl);
        two_sum(sl, low, &vh, &vl);
        two_sum(sh, vh, &th, &tl);
        two_sum(th - r, tl, &wh, &wl);
        discarded = wh != 0 ? fast_sign(wh) : fast_sign(vl);
        (void)wl;
        if (r == 0 && discarded) return false;
        break;
    }
    default: return false;
    }
    if (to_single) {
        const float f = (float)r;
        const double back = (double)f, step = r - back;
        const unsigned e = (unsigned)(fast_bits(back) >> 52) & 2047u;
        if (back != 0 ? e < 898 || e > 1150 : r != 0) return false;
        if (step != 0) {
            if (discarded && (fast_bits(r) & UINT64_C(0x1fffffff)) == UINT64_C(0x10000000)) return false;
            discarded = fast_sign(step);
        }
        r = back;
    } else {
        const unsigned e = (unsigned)(fast_bits(r) >> 52) & 2047u;
        if (r != 0 ? e < 2 || e > 2046 : discarded != 0) return false;
    }
    inexact = discarded != 0;
    rounded_up = inexact && (discarded < 0) != (r < 0);
    if (negate) r = -r;
    bits = fast_bits(r);
finish:
    now = (before & ~(PPC_FP_FR | PPC_FP_FI)) | (inexact ? PPC_FP_XX | PPC_FP_FI : 0u) | (rounded_up ? PPC_FP_FR : 0u);
    if (classify)
        now = (now & ~PPC_FP_FPRF) | (uint32_t)(bits << 1 ? (bits >> 63 ? 8u : 4u) : (bits >> 63 ? 18u : 2u)) << 12;
    if (now & ~before & PPC_FP_STICKY) now |= PPC_FP_FX;
    now = ppc_fp_summary(now) | (cpu->model == PPC_MODEL_601 ? 0u : now & PPC_FP_NI);
    if ((now & PPC_FP_FEX) && (cpu->msr & (PPC_MSR_FE0 | PPC_MSR_FE1))) return false;
    cpu->fpr[d] = bits;
    cpu->fpscr = now;
    if (opcode & 1) fp_cr_field(cpu, 1, now >> 28);
    return true;
}
