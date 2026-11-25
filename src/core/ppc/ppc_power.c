/* Original implementation from MPC601UM, chapters 3 and 10, and PEM32
 * appendix B.26, table B-2. The supplied 601.txt stops at chapter 10;
 * its advertised appendix B is absent. No other implementation was used.
 *
 * All 28 POWER-only mnemonics listed in MPC601UM chapter 10 are decoded
 * here. Ordinary PowerPC instructions and SPR access belong to the parent.
 * Undefined results are deterministic: zero for division overflow/invalid
 * clcs selectors, zero in unused loaded bytes, unchanged CR0 when undefined.
 */
#include "ppc.h"

/* MPC601UM 10.1: instruction fields; 10-51 and 10-154: primary opcodes. */
enum { POWER_DOZI = 9, POWER_RLMI = 22, POWER_X = 31 };
/* MPC601UM 10-7, 10-45/46, 10-50, 10-140, 10-145: XO-form opcodes. */
enum {
    POWER_ABS = 360, POWER_NABS = 488, POWER_DOZ = 264,
    POWER_MUL = 107, POWER_DIV = 331, POWER_DIVS = 363,
    POWER_OE = 512
};
/* MPC601UM 10-25, 10-107/108, 10-117/118, 10-158: X-form opcodes. */
enum {
    POWER_CLCS = 531, POWER_LSCBX = 277,
    POWER_MASKG = 29, POWER_MASKIR = 541, POWER_RRIB = 537
};
/* MPC601UM 10-161 through 10-178: all fifteen MQ shift opcodes. */
enum {
    POWER_SLE = 153, POWER_SLEQ = 217, POWER_SLIQ = 184,
    POWER_SLLIQ = 248, POWER_SLLQ = 216, POWER_SLQ = 152,
    POWER_SRAIQ = 952, POWER_SRAQ = 920, POWER_SRE = 665,
    POWER_SREA = 921, POWER_SREQ = 729, POWER_SRIQ = 696,
    POWER_SRLIQ = 760, POWER_SRLQ = 728, POWER_SRQ = 664
};
/* MPC601UM 2.3.2.2, 2.3.3.3, 5.4.3 table 5-10, 5.4.7 table 5-16. */
#define POWER_SIGN UINT32_C(0x80000000)
#define POWER_COUNT UINT32_C(0x0000007f)
#define POWER_IO_DSI UINT32_C(0x04000000)
#define POWER_ILLEGAL UINT32_C(0x00080000)

static int64_t power_signed(uint32_t word)
{
    return (int64_t)word - ((word & POWER_SIGN) ? INT64_C(0x100000000) : 0);
}

static uint32_t power_rotl(uint32_t word, unsigned n)
{
    n &= 31u;
    return n ? (word << n) | (word >> (32u - n)) : word;
}

/* MPC601UM 3.3.4.1, table 3-8 resolves the typo in 10-117:
 * mstart == mstop + 1 produces all ones, not mstart == mstop == 1. */
static uint32_t power_mask(unsigned first, unsigned last)
{
    uint32_t left = UINT32_MAX >> first;
    uint32_t right = UINT32_MAX << (31u - last);
    return first <= last ? left & right : left | right;
}

static uint32_t power_merge(uint32_t source, uint32_t old, uint32_t mask)
{
    return (source & mask) | (old & ~mask);
}

static bool power_illegal(ppc_cpu *cpu)
{
    ppc_exception(cpu, PPC_EX_PROGRAM, POWER_ILLEGAL, cpu->last_pc);
    return true;
}

/* Unsigned magnitudes also cover the signed 64-bit minimum divided by -1
 * without overflowing a C signed value. MPC601UM 10-45/46, table 3-1. */
static void power_divide(ppc_cpu *cpu, unsigned d, uint32_t a, uint32_t b,
                         bool wide, bool oe, bool record)
{
    bool negative = (a & POWER_SIGN) != 0;
    bool divisor_negative = (b & POWER_SIGN) != 0;
    uint64_t dividend = wide ? ((uint64_t)a << 32) | cpu->mq : (uint64_t)a;
    if (negative)
        dividend = wide ? UINT64_C(0) - dividend : (uint64_t)(UINT32_C(0) - a);
    uint32_t divisor = divisor_negative ? UINT32_C(0) - b : b;
    uint64_t quotient = divisor ? dividend / divisor : 0;
    uint32_t remainder = divisor ? (uint32_t)(dividend % divisor) : 0;
    bool quotient_negative = negative != divisor_negative;
    bool overflow = divisor == 0 || quotient >
        (quotient_negative ? UINT64_C(0x80000000) : UINT64_C(0x7fffffff));
    uint32_t result;
    if (overflow) {
        /* The -2^31/-1 case is specified even though its quotient overflows. */
        result = negative && dividend == UINT64_C(0x80000000) &&
                 b == UINT32_MAX ? POWER_SIGN : 0;
        remainder = 0;
    } else {
        result = (uint32_t)quotient;
        if (quotient_negative)
            result = UINT32_C(0) - result;
        if (negative)
            remainder = UINT32_C(0) - remainder;
    }
    cpu->gpr[d] = result;
    cpu->mq = remainder;
    if (oe)
        ppc_overflow(cpu, overflow);
    if (record)
        ppc_record(cpu, remainder);
}

static bool power_shift(ppc_cpu *cpu, uint32_t opcode, unsigned xo)
{
    bool immediate, right, arithmetic = false;
    switch (xo) {
    case POWER_SLIQ: case POWER_SLLIQ:
        immediate = true; right = false; break;
    case POWER_SRIQ: case POWER_SRLIQ:
        immediate = true; right = true; break;
    case POWER_SRAIQ:
        immediate = true; right = true; arithmetic = true; break;
    case POWER_SLQ: case POWER_SLLQ: case POWER_SLE: case POWER_SLEQ:
        immediate = false; right = false; break;
    case POWER_SRQ: case POWER_SRLQ: case POWER_SRE: case POWER_SREQ:
        immediate = false; right = true; break;
    case POWER_SRAQ: case POWER_SREA:
        immediate = false; right = true; arithmetic = true; break;
    default:
        return false;
    }
    unsigned s = (opcode >> 21) & 31u;
    unsigned a = (opcode >> 16) & 31u;
    unsigned b = (opcode >> 11) & 31u;
    uint32_t source = cpu->gpr[s], old_mq = cpu->mq;
    uint32_t shift = immediate ? b : cpu->gpr[b];
    unsigned n = shift & 31u;
    uint32_t rotated = power_rotl(source, right ? (32u - n) : n);
    uint32_t mask = right ? UINT32_MAX >> n : UINT32_MAX << n;
    uint32_t result;
    bool large = (shift & 32u) != 0;
    bool write_mq = true;
    switch (xo) {
    case POWER_SLLQ: case POWER_SRLQ:
        /* Table 3-9 (3-27) corrects 10-165: for shifts below 32 use
         * the rotated source, not zeros; above 31 complement the mask.
         * MQ is already aligned by an earlier MQ-producing instruction. */
        result = large ? old_mq & mask : power_merge(rotated, old_mq, mask);
        write_mq = false;
        break;
    case POWER_SLLIQ: case POWER_SRLIQ: case POWER_SLEQ: case POWER_SREQ:
        result = power_merge(rotated, old_mq, mask);
        break;
    default:
        if (large && (xo == POWER_SLQ || xo == POWER_SRQ || xo == POWER_SRAQ))
            mask = 0;
        result = arithmetic ? power_merge(rotated,
                 (source & POWER_SIGN) ? UINT32_MAX : 0, mask) : rotated & mask;
        break;
    }
    if (arithmetic) {
        bool carry = (source & POWER_SIGN) != 0 && (rotated & ~mask) != 0;
        cpu->xer = (cpu->xer & ~PPC_XER_CA) | (carry ? PPC_XER_CA : 0);
    }
    cpu->gpr[a] = result;
    if (write_mq)
        cpu->mq = rotated;
    if (opcode & 1u)
        ppc_record(cpu, result);
    return true;
}

/* MPC601UM 5.4.6 table 5-13: alignment DSISR encodes instruction fields. */
static void power_alignment(ppc_cpu *cpu, uint32_t opcode, uint32_t ea)
{
    cpu->dar = ea;
    cpu->dsisr = (((opcode >> 1) & 3u) << 15) |
                 (((opcode >> 6) & 1u) << 14) |
                 (((opcode >> 7) & 15u) << 10) |
                 (((opcode >> 21) & 31u) << 5) | ((opcode >> 16) & 31u);
    ppc_exception(cpu, PPC_EX_ALIGNMENT, 0, cpu->last_pc);
}

static void power_lscbx(ppc_cpu *cpu, uint32_t opcode)
{
    unsigned d = (opcode >> 21) & 31u;
    unsigned a = (opcode >> 16) & 31u;
    unsigned b = (opcode >> 11) & 31u;
    unsigned count = cpu->xer & POWER_COUNT;
    uint32_t ea = (a ? cpu->gpr[a] : 0) + cpu->gpr[b];
    /* MPC601UM 5.4.6.3: string operations cannot run in little-endian mode. */
    if (cpu->hid[0] & 8u) {
        power_alignment(cpu, opcode, ea);
        return;
    }
    /* MPC601UM 3.5.6 (3-53): zero count causes no memory or GPR access. */
    if (count == 0)
        return;
    uint32_t sr = cpu->sr[ea >> 28];
    bool io = (sr & POWER_SIGN) != 0;
    bool memory_forced = io && ((sr >> 20) & 511u) == 127u;
    if (io && !memory_forced) {
        /* MPC601UM 5.4.3 table 5-10: lscbx disallows ordinary I/O segments,
         * including when DR is clear. This is not a generic byte load. */
        cpu->dar = ea;
        cpu->dsisr = POWER_IO_DSI;
        ppc_exception(cpu, PPC_EX_DSI, 0, cpu->last_pc);
        return;
    }
    uint32_t end = ea + count - 1u;
    /* MPC601UM 5.4.6.1.1/3/4: preflight the entire requested range, even
     * if a comparison would match early; translated 4KiB rule includes BATs.
     * Memory-forced I/O lscbx uses page/block rules, irrespective of DR. */
    if (((ea ^ end) & UINT32_C(0xf0000000)) != 0 ||
        (((cpu->msr & PPC_MSR_DR) || memory_forced) &&
         ((ea ^ end) & UINT32_C(0xfffff000)) != 0)) {
        power_alignment(cpu, opcode, ea);
        return;
    }
    unsigned compare = (cpu->xer >> 8) & 255u;
    bool match = false;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t byte;
        if (!ppc_load(cpu, ea + i, 1, &byte))
            return;
        unsigned target = (d + i / 4u) & 31u;
        /* MPC601UM 10-107 and 3.5.6: address-register overlap is VALID
         * on the 601. Consume/compare its bytes but inhibit its writeback;
         * r0 is protected only if used as rB, never merely as rA=0. */
        if (target != b && (a == 0 || target != a)) {
            if ((i & 3u) == 0)
                cpu->gpr[target] = 0;
            cpu->gpr[target] |= (byte & 255u) << (24u - 8u * (i & 3u));
        }
        if ((byte & 255u) == compare) {
            match = true;
            cpu->xer = (cpu->xer & ~POWER_COUNT) | (i + 1u);
            break;
        }
    }
    if (opcode & 1u) {
        /* MPC601UM 10-108: CR0=00||match||XER[SO], not signed data. */
        cpu->cr = (cpu->cr & UINT32_C(0x0fffffff)) |
                  (match ? UINT32_C(0x20000000) : 0) |
                  ((cpu->xer & PPC_XER_SO) ? UINT32_C(0x10000000) : 0);
    }
}

bool ppc_power(ppc_cpu *cpu, uint32_t opcode)
{
    unsigned primary = opcode >> 26;
    unsigned d = (opcode >> 21) & 31u;
    unsigned a = (opcode >> 16) & 31u;
    unsigned b = (opcode >> 11) & 31u;
    uint32_t av = cpu->gpr[a], bv = cpu->gpr[b];
    if (primary == POWER_DOZI) {
        uint32_t imm = opcode & UINT32_C(0xffff);
        int64_t signed_imm = (int64_t)imm - ((imm & 32768u) ? 65536 : 0);
        cpu->gpr[d] = power_signed(av) > signed_imm ? 0 :
                      (uint32_t)(signed_imm - power_signed(av));
        return true;
    }
    if (primary == POWER_RLMI) {
        unsigned first = (opcode >> 6) & 31u;
        unsigned last = (opcode >> 1) & 31u;
        uint32_t result = power_merge(power_rotl(cpu->gpr[d], bv & 31u), av,
                                      power_mask(first, last));
        cpu->gpr[a] = result;
        if (opcode & 1u)
            ppc_record(cpu, result);
        return true;
    }
    if (primary != POWER_X)
        return false;
    unsigned xo = (opcode >> 1) & 1023u;
    bool record = (opcode & 1u) != 0;
    bool oe = (xo & POWER_OE) != 0;
    uint32_t result;
    bool overflow = false;
    switch (xo) {
    case POWER_ABS: case POWER_ABS + POWER_OE:
    case POWER_NABS: case POWER_NABS + POWER_OE:
        if (b != 0)
            return power_illegal(cpu);
        if ((xo & ~POWER_OE) == POWER_ABS) {
            result = (av & POWER_SIGN) ? UINT32_C(0) - av : av;
            overflow = av == POWER_SIGN;
        } else {
            result = (av & POWER_SIGN) ? av : UINT32_C(0) - av;
        }
        break;
    case POWER_DOZ: case POWER_DOZ + POWER_OE: {
        int64_t difference = power_signed(bv) - power_signed(av);
        result = difference < 0 ? 0 : (uint32_t)difference;
        overflow = difference > INT64_C(0x7fffffff);
        break;
    }
    case POWER_MUL: case POWER_MUL + POWER_OE: {
        int64_t product = power_signed(av) * power_signed(bv);
        uint64_t bits = (uint64_t)product;
        cpu->gpr[d] = (uint32_t)(bits >> 32);
        cpu->mq = (uint32_t)bits;
        overflow = product < -INT64_C(0x80000000) || product > INT64_C(0x7fffffff);
        if (oe)
            ppc_overflow(cpu, overflow);
        if (record)
            ppc_record(cpu, cpu->mq);
        return true;
    }
    case POWER_DIV: case POWER_DIV + POWER_OE:
    case POWER_DIVS: case POWER_DIVS + POWER_OE:
        power_divide(cpu, d, av, bv, (xo & ~POWER_OE) == POWER_DIV, oe, record);
        return true;
    case POWER_MASKG:
        result = power_mask(cpu->gpr[d] & 31u, bv & 31u);
        cpu->gpr[a] = result;
        if (record)
            ppc_record(cpu, result);
        return true;
    case POWER_MASKIR:
        result = power_merge(cpu->gpr[d], av, bv);
        cpu->gpr[a] = result;
        if (record)
            ppc_record(cpu, result);
        return true;
    case POWER_RRIB: {
        uint32_t bit = POWER_SIGN >> (bv & 31u);
        result = (av & ~bit) | ((cpu->gpr[d] & POWER_SIGN) ? bit : 0);
        cpu->gpr[a] = result;
        if (record)
            ppc_record(cpu, result);
        return true;
    }
    case POWER_CLCS:
        if (b != 0)
            return power_illegal(cpu);
        /* MPC601UM 3.8 table 3-41 and 4.8.1: A is an encoded selector,
         * not the contents of GPR[A]. Unified cache line size is 64 bytes. */
        cpu->gpr[d] = a >= 12u && a <= 15u ? 64u : 0;
        return true;
    case POWER_LSCBX:
        power_lscbx(cpu, opcode);
        return true;
    default:
        return power_shift(cpu, opcode, xo);
    }
    cpu->gpr[d] = result;
    if (oe)
        ppc_overflow(cpu, overflow);
    if (record)
        ppc_record(cpu, result);
    return true;
}
