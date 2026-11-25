/* The translators' encoder on ARM64 hosts (jit_x64.c, ppc_jit.c).
 *
 * Livadix's own code. The translators choose x86-64 operations through the
 * interface of x64emit.h (registers, memory operands [base + index * scale
 * + disp], sizes 1, 2, 4 and 8, the group-1 arithmetic with its flags,
 * condition codes); on an ARM64 host this file lowers each of them to
 * ARM64 instructions that leave the same register, memory and flag state:
 * the x86 semantics from the Intel 64 and IA-32 Architectures Software
 * Developer's Manual, Vol. 1 3.4.3 and Vol. 2 (each instruction's
 * operation and flags), the encodings from the Arm Architecture Reference
 * Manual for A-profile (C4, the A64 instruction set encoding; C6, the
 * instructions used here), the calls by the Procedure Call Standard for
 * the Arm 64-bit Architecture (AAPCS64: X0-X3 the first arguments, X0 the
 * result, X19-X28 kept across a call, SP 16-byte aligned).
 *
 * Registers: the sixteen x86 registers live in fixed ARM registers, the
 * argument registers RCX RDX R8 R9 in X0-X3 (so a call needs no moves),
 * the other volatile ones (RAX R10 R11) in X4-X6, the callee-saved ones
 * (RBX RBP RSI RDI R12-R15) in X19-X26, RSP in SP; X9-X12, X16 and X17 are
 * this file's scratch registers, X18 (the platform register) is never
 * touched. A call (x_call_abs) is BLR, and the result moves to RAX's X4.
 *
 * Flags: ARM's N Z C V hold the x86 SF ZF CF OF after an addition,
 * subtraction, logical operation or compare of any size (8- and 16-bit
 * operations are done in the top bits of a 32-bit one), except that after a
 * subtraction C is the complement of CF (ARM's carry is "no borrow"). The
 * buffer records which x86 flags the ARM flags hold and whether C is
 * complemented, through branches and labels (x_bind); an operation whose
 * x86 flags are not computed here (shifts, multiplies, divides: the
 * translators never test them) leaves them unknown. A condition, SETcc,
 * CMOVcc, ADC or SBB that needs a flag not known marks the buffer bad:
 * the translator then leaves that instruction to the interpreter. */
#ifndef MC_A64EMIT_H
#define MC_A64EMIT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { XAX, XCX, XDX, XBX, XSP, XBP, XSI, XDI, X8, X9, X10, X11, X12, X13, X14, X15 };

/* x86 condition codes (the low nibble of Jcc, SETcc and CMOVcc). */
enum { CC_O, CC_NO, CC_B, CC_AE, CC_E, CC_NE, CC_BE, CC_A, CC_S, CC_NS, CC_P, CC_NP, CC_L, CC_GE, CC_LE, CC_G };
/* x86 group-1 operations and group-2 shifts. */
enum { ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP };
enum { SH_ROL, SH_ROR, SH_RCL, SH_RCR, SH_SHL, SH_SHR, SH_SAL, SH_SAR };

typedef struct xo {
    int8_t reg;     /* >= 0: a register operand */
    int8_t base;    /* memory: base register, -1 none */
    int8_t index;   /* memory: index register, -1 none */
    uint8_t scale;  /* 1, 2, 4, 8 */
    int32_t disp;
} xo;

static inline xo XR(int r) { xo o = {(int8_t)r, -1, -1, 1, 0}; return o; }
static inline xo XM(int base, int32_t disp) { xo o = {-1, (int8_t)base, -1, 1, disp}; return o; }
static inline xo XMI(int base, int index, unsigned scale, int32_t disp) {
    xo o = {-1, (int8_t)base, (int8_t)index, (uint8_t)scale, disp};
    return o;
}

/* The x86 flags the ARM flags hold. */
#define A64_CF 1u
#define A64_ZF 2u
#define A64_SF 4u
#define A64_OF 8u
#define A64_ALL 15u
#define A64_SITES 256u

typedef struct xbuf {
    uint8_t *p, *end;
    int overflow;
    int bad;            /* a flag test met flags not known to hold its x86 meaning */
    uint8_t fv;         /* A64_*: the x86 flags NZCV holds */
    uint8_t inv;        /* C holds the complement of CF */
    uint8_t dead;       /* the last instruction was an unconditional jump */
    unsigned nsite;
    struct { uint8_t *at; uint8_t fv, inv; } site[A64_SITES];   /* the flags at each forward jump */
} xbuf;

/* x86 register -> ARM register (31: SP). */
static const uint8_t a64_map[16] = {4, 0, 1, 19, 31, 20, 21, 22, 2, 3, 5, 6, 23, 24, 25, 26};
#define A64R(x) ((unsigned)a64_map[(x) & 15])
enum { A64_T0 = 16, A64_T1 = 17, A64_T2 = 9, A64_T3 = 10, A64_T4 = 11, A64_T5 = 12, A64_ZR = 31 };
/* ARM conditions. */
enum { A64_EQ, A64_NE, A64_CS, A64_CC, A64_MI, A64_PL, A64_VS, A64_VC, A64_HI, A64_LS, A64_GE, A64_LT, A64_GT, A64_LE, A64_AL };

/* --- raw output -------------------------------------------------------------------- */

static inline void a64_i(xbuf *b, uint32_t ins) {
    if (b->p + 4 <= b->end) {
        memcpy(b->p, &ins, 4);
        b->p += 4;
    } else {
        b->overflow = 1;
    }
    b->dead = 0;
}
/* Data in the instruction stream (literals, patched fields). */
static inline void x8(xbuf *b, unsigned v) {
    if (b->p < b->end) *b->p++ = (uint8_t)v;
    else b->overflow = 1;
}
static inline void x16(xbuf *b, unsigned v) { x8(b, v); x8(b, v >> 8); }
static inline void x32(xbuf *b, uint32_t v) { x8(b, v); x8(b, v >> 8); x8(b, v >> 16); x8(b, v >> 24); }
static inline void x64v(xbuf *b, uint64_t v) { x32(b, (uint32_t)v); x32(b, (uint32_t)(v >> 32)); }

static inline int fits8(int64_t v) { return v >= -128 && v <= 127; }
static inline int fits32(int64_t v) { return v >= INT32_MIN && v <= INT32_MAX; }

static inline unsigned a64_lg(unsigned size) { return size == 8 ? 3u : size == 4 ? 2u : size == 2 ? 1u : 0u; }
#define A64_SF_BIT(sf) ((sf) ? 0x80000000u : 0u)

/* --- data processing (C6: ADD, SUB, ADC, SBC, AND, ORR, EOR, ORN, BFM, SBFM, UBFM, EXTR, the
 * variable shifts, CSEL, CSINC, MADD, MSUB, SMADDL, UMADDL, UDIV, SDIV, REV, REV16, CLZ) ---- */

static inline void a64_addsub_imm(xbuf *b, int sf, int sub, int s, unsigned rd, unsigned rn, unsigned imm12, int lsl12) {
    a64_i(b, A64_SF_BIT(sf) | (sub ? 0x40000000u : 0u) | (s ? 0x20000000u : 0u) | 0x11000000u | (lsl12 ? 0x400000u : 0u) |
             (imm12 & 0xFFFu) << 10 | rn << 5 | rd);
}
/* shift: 0 LSL, 1 LSR, 2 ASR */
static inline void a64_addsub_reg(xbuf *b, int sf, int sub, int s, unsigned rd, unsigned rn, unsigned rm, unsigned shift, unsigned amount) {
    a64_i(b, A64_SF_BIT(sf) | (sub ? 0x40000000u : 0u) | (s ? 0x20000000u : 0u) | 0x0B000000u | shift << 22 | rm << 16 |
             (amount & 63u) << 10 | rn << 5 | rd);
}
static inline void a64_adc(xbuf *b, int sf, int sub, int s, unsigned rd, unsigned rn, unsigned rm) {
    a64_i(b, A64_SF_BIT(sf) | (sub ? 0x40000000u : 0u) | (s ? 0x20000000u : 0u) | 0x1A000000u | rm << 16 | rn << 5 | rd);
}
/* opc: 0 AND, 1 ORR, 2 EOR, 3 ANDS; n: the second operand inverted (BIC, ORN, EON, BICS); shift 0 LSL 1 LSR 2 ASR 3 ROR */
static inline void a64_logic_reg(xbuf *b, int sf, unsigned opc, int n, unsigned rd, unsigned rn, unsigned rm, unsigned shift, unsigned amount) {
    a64_i(b, A64_SF_BIT(sf) | opc << 29 | 0x0A000000u | shift << 22 | (n ? 0x200000u : 0u) | rm << 16 | (amount & 63u) << 10 |
             rn << 5 | rd);
}
static inline void a64_logic_imm(xbuf *b, int sf, unsigned opc, unsigned rd, unsigned rn, uint32_t nimms) {
    a64_i(b, A64_SF_BIT(sf) | opc << 29 | 0x12000000u | (nimms & 0x1FFFu) << 10 | rn << 5 | rd);
}
/* opc: 0 SBFM, 1 BFM, 2 UBFM */
static inline void a64_bfm(xbuf *b, int sf, unsigned opc, unsigned rd, unsigned rn, unsigned immr, unsigned imms) {
    a64_i(b, A64_SF_BIT(sf) | opc << 29 | 0x13000000u | (sf ? 0x400000u : 0u) | (immr & 63u) << 16 | (imms & 63u) << 10 | rn << 5 | rd);
}
static inline void a64_extr(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned rm, unsigned lsb) {
    a64_i(b, A64_SF_BIT(sf) | 0x13800000u | (sf ? 0x400000u : 0u) | rm << 16 | (lsb & 63u) << 10 | rn << 5 | rd);
}
/* type: 0 LSLV, 1 LSRV, 2 ASRV, 3 RORV */
static inline void a64_shiftv(xbuf *b, int sf, unsigned type, unsigned rd, unsigned rn, unsigned rm) {
    a64_i(b, A64_SF_BIT(sf) | 0x1AC02000u | type << 10 | rm << 16 | rn << 5 | rd);
}
static inline void a64_div(xbuf *b, int sf, int sign, unsigned rd, unsigned rn, unsigned rm) {
    a64_i(b, A64_SF_BIT(sf) | (sign ? 0x1AC00C00u : 0x1AC00800u) | rm << 16 | rn << 5 | rd);
}
static inline void a64_msub(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned rm, unsigned ra) {
    a64_i(b, A64_SF_BIT(sf) | 0x1B008000u | rm << 16 | ra << 10 | rn << 5 | rd);
}
/* SMULL / UMULL Xd, Wn, Wm */
static inline void a64_mull(xbuf *b, int sign, unsigned rd, unsigned rn, unsigned rm) {
    a64_i(b, (sign ? 0x9B200000u : 0x9BA00000u) | rm << 16 | A64_ZR << 10 | rn << 5 | rd);
}
/* CSEL (inc 0) / CSINC (inc 1) */
static inline void a64_csel(xbuf *b, int sf, int inc, unsigned rd, unsigned rn, unsigned rm, unsigned cond) {
    a64_i(b, A64_SF_BIT(sf) | 0x1A800000u | (inc ? 0x400u : 0u) | rm << 16 | cond << 12 | rn << 5 | rd);
}
static inline void a64_cset(xbuf *b, unsigned rd, unsigned cond) { a64_csel(b, 0, 1, rd, A64_ZR, A64_ZR, cond ^ 1u); }
static inline void a64_mrs_nzcv(xbuf *b, unsigned rt) { a64_i(b, 0xD53B4200u | rt); }
static inline void a64_msr_nzcv(xbuf *b, unsigned rt) { a64_i(b, 0xD51B4200u | rt); }

/* MOV: ORR from the zero register, or ADD #0 where SP is involved. */
static inline void a64_mov(xbuf *b, int sf, unsigned rd, unsigned rn) {
    if (rd == 31 || rn == 31) a64_addsub_imm(b, sf, 0, 0, rd, rn, 0, 0);
    else a64_logic_reg(b, sf, 1, 0, rd, A64_ZR, rn, 0, 0);
}
static inline void a64_lsl(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned s) {
    const unsigned bits = sf ? 64u : 32u;
    a64_bfm(b, sf, 2, rd, rn, (bits - s) % bits, bits - 1u - s);
}
static inline void a64_lsr(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned s) { a64_bfm(b, sf, 2, rd, rn, s, sf ? 63u : 31u); }
static inline void a64_asr(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned s) { a64_bfm(b, sf, 0, rd, rn, s, sf ? 63u : 31u); }
static inline void a64_ubfx(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_bfm(b, sf, 2, rd, rn, lsb, lsb + width - 1u); }
static inline void a64_sbfx(xbuf *b, int sf, unsigned rd, unsigned rn, unsigned lsb, unsigned width) { a64_bfm(b, sf, 0, rd, rn, lsb, lsb + width - 1u); }
/* The low `width` bits of rn into rd's, its other bits kept (BFI Xd, Xn, #0, #width). */
static inline void a64_bfi0(xbuf *b, unsigned rd, unsigned rn, unsigned width) { a64_bfm(b, 1, 1, rd, rn, 0, width - 1u); }

/* A logical immediate's N:immr:imms (C4.1.4, DecodeBitMasks inverted), or -1. */
static inline int32_t a64_logimm(uint64_t v, int sf) {
    if (!sf) v = (v & 0xFFFFFFFFu) | v << 32;
    if (v == 0 || v == ~(uint64_t)0) return -1;
    unsigned size = 64;
    while (size > 2) {
        const unsigned half = size / 2;
        const uint64_t m = (UINT64_C(1) << half) - 1u;
        if ((v & m) != ((v >> half) & m)) break;
        size = half;
    }
    const uint64_t mask = size == 64 ? ~(uint64_t)0 : (UINT64_C(1) << size) - 1u;
    const uint64_t elt = v & mask;
    unsigned ones = 0;
    for (unsigned i = 0; i < size; i++) ones += (unsigned)(elt >> i) & 1u;
    const uint64_t run = ones == 64 ? ~(uint64_t)0 : (UINT64_C(1) << ones) - 1u;
    for (unsigned r = 0; r < size; r++) {
        /* rotated left by r, the element is the run of ones at the bottom: it is that run rotated right by r */
        const uint64_t rot = r ? ((elt << r) | (elt >> (size - r))) & mask : elt;
        if (rot == run) {
            const unsigned n = size == 64 ? 1u : 0u;
            const unsigned imms = ((~(size - 1u) << 1) | (ones - 1u)) & 0x3Fu;
            return (int32_t)(n << 12 | r << 6 | imms);
        }
    }
    return -1;
}

/* A constant into rd (32-bit: zero-extended): MOVZ or MOVN, then MOVK. */
static inline void a64_movi(xbuf *b, int sf, unsigned rd, uint64_t v) {
    if (!sf) v &= 0xFFFFFFFFu;
    const unsigned n = sf ? 4u : 2u;
    unsigned zeros = 0, ones = 0;
    for (unsigned i = 0; i < n; i++) {
        const unsigned h = (unsigned)(v >> (16u * i)) & 0xFFFFu;
        zeros += h == 0;
        ones += h == 0xFFFFu;
    }
    const uint32_t base = A64_SF_BIT(sf);
    int first = 1;
    if (ones > zeros) {
        for (unsigned i = 0; i < n; i++) {
            const unsigned h = (unsigned)(v >> (16u * i)) & 0xFFFFu;
            if (h == 0xFFFFu) continue;
            if (first) a64_i(b, base | 0x12800000u | i << 21 | (~h & 0xFFFFu) << 5 | rd);      /* MOVN */
            else a64_i(b, base | 0x72800000u | i << 21 | h << 5 | rd);                       /* MOVK */
            first = 0;
        }
        if (first) a64_i(b, base | 0x12800000u | rd);                                        /* MOVN #0: all ones */
    } else {
        for (unsigned i = 0; i < n; i++) {
            const unsigned h = (unsigned)(v >> (16u * i)) & 0xFFFFu;
            if (!h) continue;
            if (first) a64_i(b, base | 0x52800000u | i << 21 | h << 5 | rd);                 /* MOVZ */
            else a64_i(b, base | 0x72800000u | i << 21 | h << 5 | rd);
            first = 0;
        }
        if (first) a64_i(b, base | 0x52800000u | rd);                                        /* MOVZ #0 */
    }
}

/* --- memory (C6: LDR/STR immediate unsigned offset, LDUR/STUR, register offset, literal) ---- */

/* opc: 0 store, 1 load zero-extending, 2 load sign-extending to 64 bits, 3 to 32 bits. */
enum { A64_ST = 0, A64_LD = 1, A64_LDSX = 2, A64_LDSW = 3 };

/* base + index * scale + disp into rd (sf 0: its low 32 bits, zero-extended). */
static inline void a64_addr(xbuf *b, int sf, unsigned rd, xo m) {
    const unsigned sh = m.scale == 8 ? 3u : m.scale == 4 ? 2u : m.scale == 2 ? 1u : 0u;
    int have = 0;
    if (m.base >= 0 && m.index >= 0) {
        unsigned rn = A64R(m.base);
        if (rn == 31) {
            a64_addsub_imm(b, 1, 0, 0, rd, 31, 0, 0);
            rn = rd;
        }
        a64_addsub_reg(b, sf, 0, 0, rd, rn, A64R(m.index), 0, sh);
        have = 1;
    } else if (m.index >= 0) {
        if (sh) a64_lsl(b, sf, rd, A64R(m.index), sh);
        else a64_mov(b, sf, rd, A64R(m.index));
        have = 1;
    }
    const int64_t d = m.disp;
    if (!have) {
        if (m.base < 0) {
            a64_movi(b, sf, rd, (uint64_t)d);
            return;
        }
        const unsigned rn = A64R(m.base);
        if (d >= 0 && d < 4096) a64_addsub_imm(b, sf, 0, 0, rd, rn, (unsigned)d, 0);
        else if (d < 0 && d > -4096) a64_addsub_imm(b, sf, 1, 0, rd, rn, (unsigned)-d, 0);
        else {
            a64_movi(b, 1, A64_T2, (uint64_t)d);
            if (rn == 31) {
                a64_addsub_imm(b, 1, 0, 0, rd, 31, 0, 0);
                a64_addsub_reg(b, sf, 0, 0, rd, rd, A64_T2, 0, 0);
            } else {
                a64_addsub_reg(b, sf, 0, 0, rd, rn, A64_T2, 0, 0);
            }
        }
        return;
    }
    if (d > 0 && d < 4096) a64_addsub_imm(b, sf, 0, 0, rd, rd, (unsigned)d, 0);
    else if (d < 0 && d > -4096) a64_addsub_imm(b, sf, 1, 0, rd, rd, (unsigned)-d, 0);
    else if (d) {
        a64_movi(b, 1, A64_T2, (uint64_t)d);
        a64_addsub_reg(b, sf, 0, 0, rd, rd, A64_T2, 0, 0);
    }
}

/* A load or store of 1 << lg bytes of register rt at m (x86 registers in m). */
static inline void a64_mem(xbuf *b, unsigned opc, unsigned lg, unsigned rt, xo m) {
    const uint32_t sz = lg << 30, op = opc << 22;
    if (m.base >= 0 && m.index < 0) {
        const unsigned rn = A64R(m.base);
        const int64_t d = m.disp;
        if (d >= 0 && !(d & ((1 << lg) - 1)) && (d >> lg) < 4096) {
            a64_i(b, 0x39000000u | sz | op | (uint32_t)(d >> lg) << 10 | rn << 5 | rt);
        } else if (d >= -256 && d < 256) {
            a64_i(b, 0x38000000u | sz | op | ((uint32_t)d & 0x1FFu) << 12 | rn << 5 | rt);
        } else {
            a64_movi(b, 1, A64_T1, (uint64_t)d);
            a64_i(b, 0x38200800u | sz | op | A64_T1 << 16 | 0x6000u | rn << 5 | rt);
        }
        return;
    }
    if (m.base >= 0 && m.disp == 0 && (m.scale == 1 || m.scale == (1u << lg)) && A64R(m.base) != 31) {
        const uint32_t s = (m.scale > 1) ? 0x1000u : 0u;
        a64_i(b, 0x38200800u | sz | op | A64R(m.index) << 16 | 0x6000u | s | A64R(m.base) << 5 | rt);
        return;
    }
    a64_addr(b, 1, A64_T1, m);
    a64_i(b, 0x39000000u | sz | op | A64_T1 << 5 | rt);
}

/* --- the flags --------------------------------------------------------------------------- */

static inline void a64_flags(xbuf *b, unsigned fv, unsigned inv) {
    b->fv = (uint8_t)fv;
    b->inv = (uint8_t)inv;
}

/* The ARM condition for x86 condition cc, from the flags as they stand;
 * -1 when it takes two flags with C as CF (BE, A after an addition: the
 * caller tests a register, a64_be); A64_AL with the buffer marked bad when
 * the flags it needs are not known. */
static inline int a64_cond(xbuf *b, unsigned cc) {
    static const uint8_t need[8] = {A64_OF, A64_CF, A64_ZF, A64_CF | A64_ZF, A64_SF, 0xFF, A64_SF | A64_OF, A64_ZF | A64_SF | A64_OF};
    const unsigned k = (cc >> 1) & 7u, neg = cc & 1u;
    if (need[k] == 0xFF || (b->fv & need[k]) != need[k]) {
        b->bad = 1;
        return A64_AL;
    }
    switch (k) {
    case 0: return (int)(A64_VS ^ neg);
    case 1: return (int)((b->inv ? A64_CC : A64_CS) ^ neg);
    case 2: return (int)(A64_EQ ^ neg);
    case 3: return b->inv ? (int)(A64_LS ^ neg) : -1;
    case 4: return (int)(A64_MI ^ neg);
    case 6: return (int)(A64_LT ^ neg);
    default: return (int)(A64_LE ^ neg);
    }
}
/* rd = CF | ZF (C as CF), the flags kept. */
static inline void a64_be(xbuf *b, unsigned rd) {
    a64_cset(b, rd, A64_CS);
    a64_csel(b, 0, 1, rd, rd, A64_ZR, A64_NE);    /* CSINC rd, rd, WZR, NE: 1 when Z */
}
/* rd = 1 when x86 condition cc holds, else 0 (the flags kept). */
static inline void a64_setbool(xbuf *b, unsigned cc, unsigned rd) {
    const int c = a64_cond(b, cc);
    if (c >= 0) {
        if (c == A64_AL) a64_movi(b, 0, rd, 1);
        else a64_cset(b, rd, (unsigned)c);
        return;
    }
    a64_be(b, rd);
    if (cc & 1u) a64_logic_imm(b, 0, 2, rd, rd, (uint32_t)a64_logimm(1, 0));   /* A: not BE */
}
/* C as CF (want 0) or as its complement (want 1), from a known CF. */
static inline void a64_carry(xbuf *b, int want) {
    if (!(b->fv & A64_CF)) {
        b->bad = 1;
        return;
    }
    if (b->inv == want) return;
    a64_cset(b, A64_T4, b->inv ? A64_CC : A64_CS);              /* A64_T4 = CF */
    if (want) a64_addsub_reg(b, 0, 1, 1, A64_ZR, A64_ZR, A64_T4, 0, 0);    /* CMP WZR, A64_T4: C = !CF */
    else a64_addsub_imm(b, 0, 1, 1, A64_ZR, A64_T4, 1, 0);             /* CMP A64_T4, #1: C = CF */
    b->inv = (uint8_t)want;
}

/* --- moves ------------------------------------------------------------------------------- */

/* MOV reg, r/m: a 32-bit move clears bits 63-32, an 8- or 16-bit one keeps them. */
static inline void x_load(xbuf *b, unsigned size, int dst, xo src) {
    const unsigned rd = A64R(dst);
    if (src.reg >= 0) {
        const unsigned rs = A64R(src.reg);
        if (size == 8) { if (rd != rs) a64_mov(b, 1, rd, rs); }
        else if (size == 4) a64_mov(b, 0, rd, rs);
        else if (rd != rs) a64_bfi0(b, rd, rs, 8u * size);
        return;
    }
    if (size >= 4) {
        a64_mem(b, A64_LD, a64_lg(size), rd, src);
    } else {
        a64_mem(b, A64_LD, a64_lg(size), A64_T0, src);
        a64_bfi0(b, rd, A64_T0, 8u * size);
    }
}
static inline void x_store(xbuf *b, unsigned size, xo dst, int src) {
    if (dst.reg >= 0) x_load(b, size, dst.reg, XR(src));
    else a64_mem(b, A64_ST, a64_lg(size), A64R(src), dst);
}
static inline void x_mov(xbuf *b, unsigned size, xo dst, xo src) {
    if (dst.reg >= 0) x_load(b, size, dst.reg, src);
    else x_store(b, size, dst, src.reg);
}
static inline void x_mov_ri(xbuf *b, int r, uint32_t imm) { a64_movi(b, 0, A64R(r), imm); }
static inline void x_mov_ri64(xbuf *b, int r, uint64_t imm) { a64_movi(b, 1, A64R(r), imm); }
/* MOV r/m, imm (size 8: the immediate sign-extended). */
static inline void x_mov_mi(xbuf *b, unsigned size, xo dst, uint32_t imm) {
    const uint64_t v = size == 8 ? (uint64_t)(int64_t)(int32_t)imm : size == 4 ? imm : imm & ((1u << (8u * size)) - 1u);
    if (dst.reg >= 0) {
        if (size >= 4) {
            a64_movi(b, size == 8, A64R(dst.reg), v);
        } else {
            a64_movi(b, 0, A64_T0, v);
            a64_bfi0(b, A64R(dst.reg), A64_T0, 8u * size);
        }
        return;
    }
    if (v == 0) {
        a64_mem(b, A64_ST, a64_lg(size), A64_ZR, dst);
        return;
    }
    a64_movi(b, size == 8, A64_T0, v);
    a64_mem(b, A64_ST, a64_lg(size), A64_T0, dst);
}
/* MOVZX / MOVSX r32, r/m8 or r/m16. */
static inline void x_movzx(xbuf *b, int dst, xo src, unsigned srcsize) {
    if (src.reg >= 0) a64_ubfx(b, 0, A64R(dst), A64R(src.reg), 0, 8u * srcsize);
    else a64_mem(b, A64_LD, a64_lg(srcsize), A64R(dst), src);
}
static inline void x_movsx(xbuf *b, int dst, xo src, unsigned srcsize) {
    if (src.reg >= 0) a64_sbfx(b, 0, A64R(dst), A64R(src.reg), 0, 8u * srcsize);
    else a64_mem(b, A64_LDSW, a64_lg(srcsize), A64R(dst), src);
}
/* MOVSXD r64, r/m32. */
static inline void x_movsxd(xbuf *b, int dst, xo src) {
    if (src.reg >= 0) a64_sbfx(b, 1, A64R(dst), A64R(src.reg), 0, 32);
    else a64_mem(b, A64_LDSX, 2, A64R(dst), src);
}
/* LEA reg, m (size 4: the address's low 32 bits). */
static inline void x_lea(xbuf *b, unsigned size, int dst, xo m) {
    if (size < 4) {
        b->bad = 1;
        return;
    }
    a64_addr(b, size == 8, A64R(dst), m);
}

/* --- arithmetic -------------------------------------------------------------------------- */

/* The value of an operand in a register: its own, or loaded into `tmp`
 * (zero-extended). */
static inline unsigned a64_operand(xbuf *b, unsigned size, xo o, unsigned tmp) {
    if (o.reg >= 0) return A64R(o.reg);
    a64_mem(b, A64_LD, a64_lg(size), tmp, o);
    return tmp;
}

/* The group-1 operation `op` of `size` bytes: dst op= the source, which is
 * register rs (an ARM register) when has_imm is 0, else the immediate.
 * x86 semantics: the result's flags (CF OF SF ZF), CMP without the write,
 * 32-bit results zero-extended, 8- and 16-bit results into the low bits. */
static inline void a64_alu(xbuf *b, unsigned op, unsigned size, xo dst, int has_imm, unsigned rs, int64_t imm) {
    const int full = size >= 4, sf = size == 8;
    const unsigned dmem = dst.reg < 0;
    unsigned ra = a64_operand(b, size, dst, A64_T0);
    if (full) {
        const unsigned rd = op == ALU_CMP ? A64_ZR : dmem ? A64_T0 : A64R(dst.reg);
        const uint64_t v = sf ? (uint64_t)imm : (uint64_t)(uint32_t)imm;
        unsigned rb = rs;
        if (op == ALU_ADD || op == ALU_SUB || op == ALU_CMP) {
            const int sub = op != ALU_ADD;
            if (has_imm && v < 4096u) a64_addsub_imm(b, sf, sub, 1, rd, ra, (unsigned)v, 0);
            else if (has_imm && !(v & 0xFFFu) && v < (1u << 24)) a64_addsub_imm(b, sf, sub, 1, rd, ra, (unsigned)(v >> 12), 1);
            else {
                if (has_imm) {
                    a64_movi(b, sf, A64_T1, v);
                    rb = A64_T1;
                }
                a64_addsub_reg(b, sf, sub, 1, rd, ra, rb, 0, 0);
            }
            a64_flags(b, A64_ALL, sub);
        } else if (op == ALU_AND || op == ALU_OR || op == ALU_XOR) {
            const unsigned opc = op == ALU_AND ? 3u : op == ALU_OR ? 1u : 2u;
            const int32_t li = has_imm ? a64_logimm(v, sf) : -1;
            if (li >= 0) a64_logic_imm(b, sf, opc, rd, ra, (uint32_t)li);
            else {
                if (has_imm) {
                    a64_movi(b, sf, A64_T1, v);
                    rb = A64_T1;
                }
                a64_logic_reg(b, sf, opc, 0, rd, ra, rb, 0, 0);
            }
            if (op != ALU_AND) a64_logic_reg(b, sf, 3, 0, A64_ZR, rd, rd, 0, 0);    /* TST: N Z, C V clear */
            a64_flags(b, A64_ALL, 0);
        } else {
            /* ADC: C as CF; SBB: C as !CF (SBCS subtracts NOT C). */
            const int sbb = op == ALU_SBB;
            if (has_imm) {
                a64_movi(b, sf, A64_T1, v);
                rb = A64_T1;
            }
            a64_carry(b, sbb);
            a64_adc(b, sf, sbb, 1, rd, ra, rb);
            a64_flags(b, A64_ALL, (unsigned)sbb);
        }
    } else {
        /* In the top bits of a 32-bit operation: the flags are the operand size's. */
        const unsigned bits = 8u * size, sh = 32u - bits;
        const uint32_t lowmask = (1u << sh) - 1u;
        a64_lsl(b, 0, A64_T0, ra, sh);
        if (has_imm) a64_movi(b, 0, A64_T1, ((uint64_t)imm & ((1u << bits) - 1u)) << sh);
        else a64_lsl(b, 0, A64_T1, rs, sh);
        switch (op) {
        case ALU_ADD: a64_addsub_reg(b, 0, 0, 1, A64_T0, A64_T0, A64_T1, 0, 0); a64_flags(b, A64_ALL, 0); break;
        case ALU_SUB: case ALU_CMP: a64_addsub_reg(b, 0, 1, 1, A64_T0, A64_T0, A64_T1, 0, 0); a64_flags(b, A64_ALL, 1); break;
        case ALU_AND: a64_logic_reg(b, 0, 3, 0, A64_T0, A64_T0, A64_T1, 0, 0); a64_flags(b, A64_ALL, 0); break;
        case ALU_OR: case ALU_XOR:
            a64_logic_reg(b, 0, op == ALU_OR ? 1u : 2u, 0, A64_T0, A64_T0, A64_T1, 0, 0);
            a64_logic_reg(b, 0, 3, 0, A64_ZR, A64_T0, A64_T0, 0, 0);
            a64_flags(b, A64_ALL, 0);
            break;
        default: {
            /* ADC: the carry in at bit sh through ones below it in the first
             * operand; SBB: through ones below it in the second. */
            const int sbb = op == ALU_SBB;
            if (!(b->fv & A64_CF)) {
                b->bad = 1;
                break;
            }
            a64_cset(b, A64_T4, b->inv ? A64_CC : A64_CS);                  /* A64_T4 = CF */
            a64_carry(b, sbb);
            a64_addsub_reg(b, 0, 1, 0, A64_T4, A64_ZR, A64_T4, 0, 0);           /* NEG */
            a64_logic_imm(b, 0, 0, A64_T4, A64_T4, (uint32_t)a64_logimm(lowmask, 0));
            if (sbb) a64_logic_reg(b, 0, 1, 0, A64_T1, A64_T1, A64_T4, 0, 0);
            else a64_logic_reg(b, 0, 1, 0, A64_T0, A64_T0, A64_T4, 0, 0);
            a64_adc(b, 0, sbb, 1, A64_T0, A64_T0, A64_T1);
            a64_flags(b, A64_ALL, (unsigned)sbb);
            break;
        }
        }
        if (op == ALU_CMP) return;
        a64_lsr(b, 0, A64_T0, A64_T0, sh);
        if (!dmem) {
            a64_bfi0(b, A64R(dst.reg), A64_T0, bits);
            return;
        }
    }
    if (op == ALU_CMP) return;
    if (dmem) a64_mem(b, A64_ST, a64_lg(size), A64_T0, dst);
}

/* OP r/m, reg */
static inline void x_alu_mr(xbuf *b, unsigned op, unsigned size, xo dst, int src) { a64_alu(b, op, size, dst, 0, A64R(src), 0); }
/* OP reg, r/m */
static inline void x_alu_rm(xbuf *b, unsigned op, unsigned size, int dst, xo src) {
    const unsigned rs = a64_operand(b, size, src, A64_T1);
    a64_alu(b, op, size, XR(dst), 0, rs, 0);
}
static inline void x_alu(xbuf *b, unsigned op, unsigned size, xo dst, xo src) {
    if (src.reg >= 0) x_alu_mr(b, op, size, dst, src.reg);
    else x_alu_rm(b, op, size, dst.reg, src);
}
/* OP r/m, imm (size 8: sign-extended). */
static inline void x_alu_i(xbuf *b, unsigned op, unsigned size, xo dst, int32_t imm) { a64_alu(b, op, size, dst, 1, 0, imm); }
/* OP r/m, imm32 patched later: the immediate is a literal word in the
 * code, read by LDR (literal); returns its position. */
static inline uint8_t *x_alu_i32(xbuf *b, unsigned op, unsigned size, xo dst, int32_t imm) {
    a64_i(b, (size == 8 ? 0x98000000u : 0x18000000u) | 2u << 5 | A64_T3);    /* LDRSW / LDR (literal) A64_T3, the word below */
    a64_i(b, 0x14000002u);                                                /* B over it */
    uint8_t *at = b->p;
    x32(b, (uint32_t)imm);
    a64_alu(b, op, size, dst, 0, A64_T3, 0);
    return at;
}
/* MOV m32, imm32 patched later (as x_alu_i32); returns the word's position. */
static inline uint8_t *x_mov_mi_patch(xbuf *b, xo dst, uint32_t imm) {
    a64_i(b, 0x18000000u | 2u << 5 | A64_T0);
    a64_i(b, 0x14000002u);
    uint8_t *at = b->p;
    x32(b, imm);
    a64_mem(b, A64_ST, 2, A64_T0, dst);
    return at;
}
/* TEST r/m, reg; TEST r/m, imm. */
static inline void a64_test(xbuf *b, unsigned size, xo a, int has_imm, unsigned rs, uint32_t imm) {
    const unsigned ra = a64_operand(b, size, a, A64_T0);
    if (size >= 4) {
        const int sf = size == 8;
        const uint64_t v = sf ? (uint64_t)(int64_t)(int32_t)imm : imm;
        unsigned rb = rs;
        const int32_t li = has_imm ? a64_logimm(v, sf) : -1;
        if (li >= 0) a64_logic_imm(b, sf, 3, A64_ZR, ra, (uint32_t)li);
        else {
            if (has_imm) {
                a64_movi(b, sf, A64_T1, v);
                rb = A64_T1;
            }
            a64_logic_reg(b, sf, 3, 0, A64_ZR, ra, rb, 0, 0);
        }
    } else {
        const unsigned sh = 32u - 8u * size;
        a64_lsl(b, 0, A64_T0, ra, sh);
        if (has_imm) {
            const uint64_t v = (uint64_t)(imm & ((1u << (8u * size)) - 1u)) << sh;
            const int32_t li = a64_logimm(v, 0);
            if (li >= 0) a64_logic_imm(b, 0, 3, A64_ZR, A64_T0, (uint32_t)li);
            else if (!v) a64_logic_reg(b, 0, 3, 0, A64_ZR, A64_T0, A64_ZR, 0, 0);
            else {
                a64_movi(b, 0, A64_T1, v);
                a64_logic_reg(b, 0, 3, 0, A64_ZR, A64_T0, A64_T1, 0, 0);
            }
        } else {
            a64_logic_reg(b, 0, 3, 0, A64_ZR, A64_T0, rs, 0, sh);    /* ANDS WZR, A64_T0, Ws, LSL #sh */
        }
    }
    a64_flags(b, A64_ALL, 0);
}
static inline void x_test(xbuf *b, unsigned size, xo a, int r) { a64_test(b, size, a, 0, A64R(r), 0); }
static inline void x_test_i(xbuf *b, unsigned size, xo a, uint32_t imm) {
    if (!imm) {
        /* nothing tested: ZF set, the others clear */
        a64_logic_reg(b, 0, 3, 0, A64_ZR, A64_ZR, A64_ZR, 0, 0);
        a64_flags(b, A64_ALL, 0);
        return;
    }
    a64_test(b, size, a, 1, 0, imm);
}

/* Group 3: NOT /2, NEG /3, MUL /4, IMUL /5, DIV /6, IDIV /7 (the last four 32-bit:
 * EDX:EAX; their flags are left unknown; DIV and IDIV with the quotient in
 * range, which the translator's tests ensure). */
static inline void x_grp3(xbuf *b, unsigned sub, unsigned size, xo a) {
    const int dmem = a.reg < 0, sf = size == 8;
    if (sub == 2 || sub == 3) {
        const unsigned ra = a64_operand(b, size, a, A64_T0);
        unsigned rd = dmem ? A64_T0 : A64R(a.reg);
        if (size >= 4) {
            if (sub == 2) a64_logic_reg(b, sf, 1, 1, rd, A64_ZR, ra, 0, 0);     /* MVN */
            else {
                a64_addsub_reg(b, sf, 1, 1, rd, A64_ZR, ra, 0, 0);              /* NEGS: C = (a == 0) = !CF */
                a64_flags(b, A64_ALL, 1);
            }
        } else {
            const unsigned bits = 8u * size;
            if (sub == 2) {
                a64_logic_imm(b, 0, 2, A64_T0, ra, (uint32_t)a64_logimm((1u << bits) - 1u, 0));   /* EOR: the low bits inverted */
            } else {
                a64_lsl(b, 0, A64_T0, ra, 32u - bits);
                a64_addsub_reg(b, 0, 1, 1, A64_T0, A64_ZR, A64_T0, 0, 0);
                a64_lsr(b, 0, A64_T0, A64_T0, 32u - bits);
                a64_flags(b, A64_ALL, 1);
            }
            if (!dmem) {
                a64_bfi0(b, A64R(a.reg), A64_T0, bits);
                return;
            }
            rd = A64_T0;
        }
        if (dmem) a64_mem(b, A64_ST, a64_lg(size), rd, a);
        return;
    }
    if (size != 4) {
        b->bad = 1;
        return;
    }
    const unsigned rs = a64_operand(b, 4, a, A64_T1);
    if (sub == 4 || sub == 5) {
        a64_mull(b, sub == 5, A64_T0, A64R(XAX), rs);
        a64_mov(b, 0, A64R(XAX), A64_T0);
        a64_lsr(b, 1, A64R(XDX), A64_T0, 32);
    } else {
        /* the dividend EDX:EAX, the divisor zero- or sign-extended */
        a64_lsl(b, 1, A64_T0, A64R(XDX), 32);
        a64_mov(b, 0, A64_T3, A64R(XAX));
        a64_logic_reg(b, 1, 1, 0, A64_T0, A64_T0, A64_T3, 0, 0);
        if (sub == 7) a64_sbfx(b, 1, A64_T4, rs, 0, 32);
        else a64_mov(b, 0, A64_T4, rs);
        a64_div(b, 1, sub == 7, A64_T3, A64_T0, A64_T4);
        a64_msub(b, 1, A64_T5, A64_T3, A64_T4, A64_T0);
        a64_mov(b, 0, A64R(XAX), A64_T3);
        a64_mov(b, 0, A64R(XDX), A64_T5);
    }
    a64_flags(b, 0, 0);
}
/* IMUL reg, r/m (32-bit): CF and OF when the product does not fit. */
static inline void x_imul(xbuf *b, unsigned size, int dst, xo src) {
    if (size != 4) {
        b->bad = 1;
        return;
    }
    const unsigned rs = a64_operand(b, 4, src, A64_T1);
    a64_mull(b, 1, A64_T0, A64R(dst), rs);
    a64_mov(b, 0, A64R(dst), A64_T0);
    a64_i(b, 0xEB20C000u | A64_T0 << 16 | A64_T0 << 5 | A64_ZR);             /* CMP X16, W16, SXTW */
    a64_cset(b, A64_T4, A64_NE);
    a64_lsl(b, 0, A64_T4, A64_T4, 28);
    a64_logic_reg(b, 0, 1, 0, A64_T4, A64_T4, A64_T4, 0, 1);                 /* bits 29 (C) and 28 (V) */
    a64_msr_nzcv(b, A64_T4);
    a64_flags(b, A64_CF | A64_OF, 0);
}
/* Shifts and rotates by an immediate or by CL: the result as the x86
 * instruction leaves it; the flags a shift sets are left unknown, a rotate
 * keeps SF and ZF. RCL and RCR are not provided. */
static inline void a64_shift(xbuf *b, unsigned sub, unsigned size, xo a, int by_cl, unsigned n) {
    const int dmem = a.reg < 0, sf = size == 8, full = size >= 4;
    const unsigned bits = 8u * size;
    if (sub == SH_RCL || sub == SH_RCR) {
        b->bad = 1;
        return;
    }
    if (sub == SH_SAL) sub = SH_SHL;
    if (!by_cl) {
        n &= sf ? 63u : 31u;
        if (!n) return;
    }
    const unsigned ra = a64_operand(b, size, a, A64_T0);
    const unsigned rd = dmem || !full ? A64_T0 : A64R(a.reg);
    if (full) {
        if (by_cl) {
            if (sub == SH_ROL) {
                a64_addsub_reg(b, 0, 1, 0, A64_T4, A64_ZR, A64R(XCX), 0, 0);   /* -CL: rotate right by it */
                a64_shiftv(b, sf, 3, rd, ra, A64_T4);
            } else {
                a64_shiftv(b, sf, sub == SH_SHL ? 0u : sub == SH_SHR ? 1u : sub == SH_SAR ? 2u : 3u, rd, ra, A64R(XCX));
            }
        } else if (sub == SH_SHL) a64_lsl(b, sf, rd, ra, n);
        else if (sub == SH_SHR) a64_lsr(b, sf, rd, ra, n);
        else if (sub == SH_SAR) a64_asr(b, sf, rd, ra, n);
        else a64_extr(b, sf, rd, ra, ra, sub == SH_ROR ? n : bits - n);
    } else {
        /* 8 and 16 bits: on the 32-bit value, the count masked to 5 bits as x86 masks it */
        unsigned rc = 0;
        if (by_cl) {
            rc = A64_T4;
            a64_logic_imm(b, 0, 0, A64_T4, A64R(XCX), (uint32_t)a64_logimm(31, 0));
        }
        if (sub == SH_SHL) {
            if (by_cl) a64_shiftv(b, 0, 0, A64_T0, ra, rc);
            else a64_lsl(b, 0, A64_T0, ra, n);
        } else if (sub == SH_SHR || sub == SH_SAR) {
            if (sub == SH_SHR) a64_ubfx(b, 0, A64_T0, ra, 0, bits);
            else a64_sbfx(b, 0, A64_T0, ra, 0, bits);
            if (by_cl) a64_shiftv(b, 0, sub == SH_SHR ? 1u : 2u, A64_T0, A64_T0, rc);
            else if (sub == SH_SHR) a64_lsr(b, 0, A64_T0, A64_T0, n);
            else a64_asr(b, 0, A64_T0, A64_T0, n);
        } else {
            if (by_cl) {
                b->bad = 1;
                return;
            }
            const unsigned k = n % bits, left = sub == SH_ROL ? k : (bits - k) % bits;
            if (left) {
                a64_ubfx(b, 0, A64_T1, ra, 0, bits);
                a64_lsl(b, 0, A64_T0, A64_T1, left);
                a64_logic_reg(b, 0, 1, 0, A64_T0, A64_T0, A64_T1, 1, bits - left);   /* ORR A64_T0, A64_T0, A64_T1, LSR #(bits - left) */
            } else if (ra != A64_T0) {
                a64_mov(b, 0, A64_T0, ra);
            }
        }
        if (!dmem) a64_bfi0(b, A64R(a.reg), A64_T0, bits);
    }
    if (dmem) a64_mem(b, A64_ST, a64_lg(size), rd, a);
    if (sub == SH_ROL || sub == SH_ROR) a64_flags(b, b->fv & (A64_SF | A64_ZF), b->inv);
    else a64_flags(b, 0, 0);
}
static inline void x_shift_i(xbuf *b, unsigned sub, unsigned size, xo a, unsigned n) { a64_shift(b, sub, size, a, 0, n); }
static inline void x_shift_cl(xbuf *b, unsigned sub, unsigned size, xo a) { a64_shift(b, sub, size, a, 1, 0); }
/* BT/BTS/BTR/BTC r/m, imm8: CF = the bit (the other flags left unknown). */
static inline void x_bt_i(xbuf *b, unsigned sub, unsigned size, xo a, unsigned bit) {
    const int sf = size == 8;
    bit &= sf ? 63u : size == 4 ? 31u : 15u;
    if (size < 4 && sub >= 5) {
        b->bad = 1;
        return;
    }
    const unsigned ra = a64_operand(b, size, a, A64_T0);
    a64_ubfx(b, 1, A64_T4, ra, bit, 1);
    a64_addsub_imm(b, 1, 1, 1, A64_ZR, A64_T4, 1, 0);                    /* CMP A64_T4, #1: C = the bit */
    if (sub >= 5) {
        /* the bit set, cleared or inverted; a 32-bit register's upper half cleared */
        const unsigned rd = a.reg < 0 ? A64_T0 : A64R(a.reg);
        const uint64_t m = UINT64_C(1) << bit, all = sf ? ~(uint64_t)0 : 0xFFFFFFFFu;
        if (sub == 5) a64_logic_imm(b, sf, 1, rd, ra, (uint32_t)a64_logimm(m, sf));
        else if (sub == 6) a64_logic_imm(b, sf, 0, rd, ra, (uint32_t)a64_logimm(~m & all, sf));
        else a64_logic_imm(b, sf, 2, rd, ra, (uint32_t)a64_logimm(m, sf));
        if (a.reg < 0) a64_mem(b, A64_ST, a64_lg(size), A64_T0, a);
    }
    a64_flags(b, A64_CF, 0);
}
static inline void x_bt_r(xbuf *b, unsigned size, xo a, int r) {
    (void)size; (void)a; (void)r;
    b->bad = 1;
}
/* BSWAP r32 (the upper half cleared) and r64. */
static inline void x_bswap(xbuf *b, int r) { a64_i(b, 0x5AC00800u | A64R(r) << 5 | A64R(r)); }
static inline void x_bswap64(xbuf *b, int r) { a64_i(b, 0xDAC00C00u | A64R(r) << 5 | A64R(r)); }
/* SETcc r/m8: 1 or 0 in the low byte. */
static inline void x_setcc(xbuf *b, unsigned cc, xo a) {
    a64_setbool(b, cc, A64_T0);
    if (a.reg >= 0) a64_bfi0(b, A64R(a.reg), A64_T0, 8);
    else a64_mem(b, A64_ST, 0, A64_T0, a);
}
/* CMOVcc reg, r/m (a 32-bit destination is written, zero-extended, either way). */
static inline void x_cmov(xbuf *b, unsigned cc, unsigned size, int dst, xo src) {
    const int sf = size == 8;
    const unsigned rs = a64_operand(b, size, src, A64_T1), rd = A64R(dst);
    if (size < 4) {
        b->bad = 1;
        return;
    }
    const int c = a64_cond(b, cc);
    if (c >= 0) {
        a64_csel(b, sf, 0, rd, rs, rd, (unsigned)c);
        return;
    }
    /* BE / A with C as CF: through a register, the flags saved around the test */
    a64_be(b, A64_T4);
    a64_mrs_nzcv(b, A64_T5);
    a64_addsub_imm(b, 0, 1, 1, A64_ZR, A64_T4, 0, 0);
    a64_csel(b, sf, 0, rd, rs, rd, (cc & 1u) ? A64_EQ : A64_NE);
    a64_msr_nzcv(b, A64_T5);
}
static inline void x_xchg(xbuf *b, unsigned size, xo a, int r) {
    (void)size; (void)a; (void)r;
    b->bad = 1;
}
/* MOVBE: the load byte-reversed (16 bits into the low half, the rest kept), the store likewise. */
static inline void x_movbe_load(xbuf *b, unsigned size, int dst, xo m) {
    if (size == 2) {
        a64_mem(b, A64_LD, 1, A64_T0, m);
        a64_i(b, 0x5AC00400u | A64_T0 << 5 | A64_T0);
        a64_bfi0(b, A64R(dst), A64_T0, 16);
    } else {
        a64_mem(b, A64_LD, 2, A64R(dst), m);
        a64_i(b, 0x5AC00800u | A64R(dst) << 5 | A64R(dst));
    }
}
static inline void x_movbe_store(xbuf *b, unsigned size, xo m, int src) {
    a64_i(b, (size == 2 ? 0x5AC00400u : 0x5AC00800u) | A64R(src) << 5 | A64_T0);
    a64_mem(b, A64_ST, size == 2 ? 1u : 2u, A64_T0, m);
}
/* STC, CLC (the other flags left unknown), CDQ, BSR (ZF; the destination kept for a zero source). */
static inline void x_stc(xbuf *b) {
    a64_addsub_reg(b, 0, 1, 1, A64_ZR, A64_ZR, A64_ZR, 0, 0);        /* CMP WZR, WZR: C set */
    a64_flags(b, A64_CF, 0);
}
static inline void x_clc(xbuf *b) {
    a64_addsub_reg(b, 0, 0, 1, A64_ZR, A64_ZR, A64_ZR, 0, 0);        /* CMN WZR, WZR: C clear */
    a64_flags(b, A64_CF, 0);
}
static inline void x_cdq(xbuf *b) { a64_asr(b, 0, A64R(XDX), A64R(XAX), 31); }
static inline void x_bsr(xbuf *b, int dst, xo src) {
    const unsigned rs = a64_operand(b, 4, src, A64_T1);
    a64_i(b, 0x5AC01000u | rs << 5 | A64_T0);                /* CLZ */
    a64_movi(b, 0, A64_T4, 31);
    a64_addsub_reg(b, 0, 1, 0, A64_T0, A64_T4, A64_T0, 0, 0);        /* 31 - CLZ */
    a64_addsub_imm(b, 0, 1, 1, A64_ZR, rs, 0, 0);            /* CMP Ws, #0 */
    a64_csel(b, 0, 0, A64R(dst), A64R(dst), A64_T0, A64_EQ);
    a64_flags(b, A64_ZF, 0);
}

/* --- control ----------------------------------------------------------------------------- */

static inline void a64_site(xbuf *b, uint8_t *at) {
    const unsigned i = b->nsite++ % A64_SITES;
    b->site[i].at = at;
    b->site[i].fv = b->fv;
    b->site[i].inv = b->inv;
}
/* Jumps return their position for x_patch / x_bind; unpatched, they go on
 * to the next instruction (x86's rel32 0). */
static inline uint8_t *x_jcc(xbuf *b, unsigned cc) {
    const int c = a64_cond(b, cc);
    uint8_t *at;
    if (c == A64_AL) {
        at = b->p;
        a64_i(b, 0x14000001u);
        b->dead = 1;
    } else if (c >= 0) {
        at = b->p;
        a64_i(b, 0x54000000u | 1u << 5 | (unsigned)c);
    } else {
        a64_be(b, A64_T4);
        at = b->p;
        a64_i(b, ((cc & 1u) ? 0x34000000u : 0x35000000u) | 1u << 5 | A64_T4);  /* A: CBZ; BE: CBNZ */
    }
    a64_site(b, at);
    return at;
}
static inline uint8_t *x_jmp(xbuf *b) {
    uint8_t *at = b->p;
    a64_i(b, 0x14000001u);
    a64_site(b, at);
    b->dead = 1;
    return at;
}
/* Points the jump at `at` (B, BL, B.cond, CBZ, CBNZ) to `target`. */
static inline void x_patch(uint8_t *at, const uint8_t *target) {
    if (!at) return;
    uint32_t ins;
    memcpy(&ins, at, 4);
    const int64_t rel = (target - at) / 4;
    if ((ins & 0x7C000000u) == 0x14000000u) ins = (ins & 0xFC000000u) | ((uint32_t)rel & 0x03FFFFFFu);
    else ins = (ins & 0xFF00001Fu) | ((uint32_t)rel & 0x7FFFFu) << 5;
    memcpy(at, &ins, 4);
}
/* Sends the jump at `at` back to the instruction after it. */
static inline void x_unpatch(uint8_t *at) { x_patch(at, at + 4); }
/* Binds the forward jump at `at` here: the flags here are those every way in holds. */
static inline void x_bind(xbuf *b, uint8_t *at) {
    if (!at) return;
    const int64_t rel = (b->p - at) / 4;
    uint32_t ins;
    memcpy(&ins, at, 4);
    if ((ins & 0x7C000000u) != 0x14000000u && (rel >= (1 << 18) || rel < -(1 << 18))) b->overflow = 1;
    x_patch(at, b->p);
    unsigned fv = 0, inv = 0;
    for (unsigned k = 0; k < A64_SITES && k < b->nsite; k++) {
        const unsigned i = (b->nsite - 1u - k) % A64_SITES;
        if (b->site[i].at == at) {
            fv = b->site[i].fv;
            inv = b->site[i].inv;
            break;
        }
    }
    if (!b->dead) {
        if (inv != b->inv) fv &= ~A64_CF;
        fv &= b->fv;
        inv = b->inv;
    }
    a64_flags(b, fv, inv);
    b->dead = 0;
}
static inline void x_jcc_to(xbuf *b, unsigned cc, const uint8_t *target) { x_patch(x_jcc(b, cc), target); }
static inline void x_jmp_to(xbuf *b, const uint8_t *target) { x_patch(x_jmp(b), target); }
/* JMP r/m64 */
static inline void x_jmp_m(xbuf *b, xo a) {
    if (a.reg >= 0) a64_i(b, 0xD61F0000u | A64R(a.reg) << 5);
    else {
        a64_mem(b, A64_LD, 3, A64_T0, a);
        a64_i(b, 0xD61F0000u | A64_T0 << 5);
    }
    b->dead = 1;
}
/* CALL of a C function: BLR, the result into RAX; the flags unknown after. */
static inline void x_call_m(xbuf *b, xo a) {
    if (a.reg >= 0) a64_i(b, 0xD63F0000u | A64R(a.reg) << 5);
    else {
        a64_mem(b, A64_LD, 3, A64_T0, a);
        a64_i(b, 0xD63F0000u | A64_T0 << 5);
    }
    a64_mov(b, 1, A64R(XAX), 0);
    a64_flags(b, 0, 0);
}
static inline void x_call_abs(xbuf *b, const void *fn) {
    a64_movi(b, 1, A64_T0, (uint64_t)(uintptr_t)fn);
    a64_i(b, 0xD63F0000u | A64_T0 << 5);
    a64_mov(b, 1, A64R(XAX), 0);
    a64_flags(b, 0, 0);
}
/* BL to generated code (a thunk that returns with RET). */
static inline void x_call_rel(xbuf *b, const uint8_t *target) {
    uint8_t *at = b->p;
    a64_i(b, 0x94000000u);
    x_patch(at, target);
    a64_flags(b, 0, 0);
}
/* LEA reg64, [RIP + disp] to an absolute target: ADR, or ADRP and ADD. */
static inline void x_lea_rip(xbuf *b, int dst, const uint8_t *target) {
    const unsigned rd = A64R(dst);
    const int64_t d = target - b->p;
    if (d >= -(1 << 20) && d < (1 << 20)) {
        a64_i(b, 0x10000000u | ((uint32_t)d & 3u) << 29 | ((uint32_t)(d >> 2) & 0x7FFFFu) << 5 | rd);
        return;
    }
    const int64_t pages = (int64_t)((uintptr_t)target >> 12) - (int64_t)((uintptr_t)b->p >> 12);
    a64_i(b, 0x90000000u | ((uint32_t)pages & 3u) << 29 | ((uint32_t)(pages >> 2) & 0x7FFFFu) << 5 | rd);
    a64_addsub_imm(b, 1, 0, 0, rd, rd, (unsigned)((uintptr_t)target & 0xFFFu), 0);
}
/* CBZ RCX forward (JRCXZ's work; the flags kept): bound with x_bind. */
static inline uint8_t *x_jrcxz_fwd(xbuf *b) {
    uint8_t *at = b->p;
    a64_i(b, 0xB4000000u | 1u << 5 | A64R(XCX));
    a64_site(b, at);
    return at;
}
/* A word of data in the code (jumped over); returns its position. */
static inline uint8_t *x_data_slot(xbuf *b, uint32_t v) {
    a64_i(b, 0x14000002u);
    uint8_t *at = b->p;
    x32(b, v);
    return at;
}
static inline void x_push(xbuf *b, int r) { a64_i(b, 0xF8000C00u | (0x1F0u << 12) | 31u << 5 | A64R(r)); }     /* STR Xr, [SP, #-16]! */
static inline void x_pop(xbuf *b, int r) { a64_i(b, 0xF8400400u | (16u << 12) | 31u << 5 | A64R(r)); }         /* LDR Xr, [SP], #16 */
static inline void x_ret(xbuf *b) {
    a64_i(b, 0xD65F03C0u);
    b->dead = 1;
}
/* STP / LDP of two X registers at [SP + 8 * k] (pre: SP -= 8 * -k first; post: SP += 8 * k after). */
static inline void a64_stp_pre(xbuf *b, unsigned r1, unsigned r2, int k) { a64_i(b, 0xA9800000u | ((uint32_t)k & 0x7Fu) << 15 | r2 << 10 | 31u << 5 | r1); }
static inline void a64_ldp_post(xbuf *b, unsigned r1, unsigned r2, int k) { a64_i(b, 0xA8C00000u | ((uint32_t)k & 0x7Fu) << 15 | r2 << 10 | 31u << 5 | r1); }
static inline void a64_stp(xbuf *b, unsigned r1, unsigned r2, int k) { a64_i(b, 0xA9000000u | ((uint32_t)k & 0x7Fu) << 15 | r2 << 10 | 31u << 5 | r1); }
static inline void a64_ldp(xbuf *b, unsigned r1, unsigned r2, int k) { a64_i(b, 0xA9400000u | ((uint32_t)k & 0x7Fu) << 15 | r2 << 10 | 31u << 5 | r1); }

/* The ARM registers the callee-saved x86 ones live in, and the volatile ones (and X29, X30). */
#define A64_SAVED_PAIRS {{19, 20}, {21, 22}, {23, 24}, {25, 26}, {27, 28}}

/* New or changed code made visible to instruction fetch (the data cache
 * cleaned to the point of unification, the instruction cache invalidated). */
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static inline void a64_sync(const void *start, size_t size) { FlushInstructionCache(GetCurrentProcess(), start, size); }
#else
static inline void a64_sync(const void *start, size_t size) { __builtin___clear_cache((char *)start, (char *)start + size); }
#endif

#endif /* MC_A64EMIT_H */
