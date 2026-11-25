/* Livadix — the 65C02 CPU core (see cpu6502.h for sources). Livadix's own code.
 *
 * Cycle counts (R65C02 and W65C02S opcode matrices): immediate 2, zero page
 * 3, zero page indexed 4, absolute 4, absolute indexed 4 (+1 page cross),
 * (zp,X) 6, (zp),Y 5 (+1 page cross), (zp) 5; stores never take the page
 * penalty but absolute indexed and (zp),Y stores take 5 and 6; read-modify-
 * write zero page 5, zero page,X 6, absolute 6, absolute,X 7 for INC and
 * DEC and 6 (+1 page cross) for the shifts and rotates; TSB/TRB 5 and 6;
 * branches 2 (+1 taken, +1 more across a page), BRA 3 (+1 across a page);
 * BBR/BBS 5 (+1 taken, +1 more across a page), RMB/SMB 5; JMP 3, JMP (abs)
 * and JMP (abs,X) 6, JSR, RTS and RTI 6, BRK 7; pushes 3, pulls 4; the
 * implied instructions 2; ADC and SBC one more in decimal mode. */
#include "cpu6502.h"

#include <string.h>

#define RD(a) c->read(c->ctx, (uint16_t)(a))
#define WR(a, v) c->write(c->ctx, (uint16_t)(a), (uint8_t)(v))

static uint8_t fetch(mc_cpu6502 *c) { return RD(c->pc++); }

static uint16_t fetch16(mc_cpu6502 *c) {
    const uint16_t lo = fetch(c);
    return (uint16_t)(lo | (unsigned)fetch(c) << 8);
}

static void push(mc_cpu6502 *c, uint8_t v) { WR(0x0100u | c->s, v); c->s--; }
static uint8_t pull(mc_cpu6502 *c) { c->s++; return RD(0x0100u | c->s); }

/* A 16-bit pointer in zero page (the high byte wraps to $00). */
static uint16_t zp_pointer(mc_cpu6502 *c, uint8_t zp) {
    return (uint16_t)(RD(zp) | (unsigned)RD((uint8_t)(zp + 1u)) << 8);
}

static void nz(mc_cpu6502 *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(MC_6502_N | MC_6502_Z)) | (v & 0x80u) | (v ? 0 : MC_6502_Z));
}

static void flag(mc_cpu6502 *c, uint8_t f, int on) { c->p = (uint8_t)(on ? c->p | f : c->p & ~f); }

static int page_cross(uint16_t a, uint16_t b) { return (a ^ b) > 0xFFu; }

/* Effective addresses. `extra` gets the page-crossing cycle when `penalty`. */
typedef enum { M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY, M_IZP } mode;

static uint16_t ea(mc_cpu6502 *c, mode m, unsigned *extra) {
    uint16_t base, r;
    switch (m) {
    case M_IMM: return c->pc++;
    case M_ZP: return fetch(c);
    case M_ZPX: return (uint8_t)(fetch(c) + c->x);
    case M_ZPY: return (uint8_t)(fetch(c) + c->y);
    case M_ABS: return fetch16(c);
    case M_ABX: base = fetch16(c); r = (uint16_t)(base + c->x); break;
    case M_ABY: base = fetch16(c); r = (uint16_t)(base + c->y); break;
    case M_IZX: return zp_pointer(c, (uint8_t)(fetch(c) + c->x));
    case M_IZY: base = zp_pointer(c, fetch(c)); r = (uint16_t)(base + c->y); break;
    default: return zp_pointer(c, fetch(c));       /* M_IZP */
    }
    if (extra && page_cross(base, r)) (*extra)++;
    return r;
}

static void adc(mc_cpu6502 *c, uint8_t m) {
    const unsigned a = c->a, carry = c->p & MC_6502_C;
    if (!(c->p & MC_6502_D)) {
        const unsigned r = a + m + carry;
        flag(c, MC_6502_V, (~(a ^ m) & (a ^ r) & 0x80u) != 0);
        flag(c, MC_6502_C, r > 0xFFu);
        c->a = (uint8_t)r;
    } else {
        /* BCD (MCS6500 manual, "Decimal mode"): each nibble adjusted by 6. */
        unsigned lo = (a & 0x0Fu) + (m & 0x0Fu) + carry;
        if (lo > 9u) lo += 6u;
        unsigned r = (a & 0xF0u) + (m & 0xF0u) + (lo > 0x0Fu ? 0x10u : 0) + (lo & 0x0Fu);
        flag(c, MC_6502_V, (~(a ^ m) & (a ^ r) & 0x80u) != 0);
        if (r > 0x9Fu) r += 0x60u;
        flag(c, MC_6502_C, r > 0xFFu);
        c->a = (uint8_t)r;
    }
    nz(c, c->a);
}

static void sbc(mc_cpu6502 *c, uint8_t m) {
    const unsigned a = c->a, borrow = (c->p & MC_6502_C) ? 0u : 1u;
    const unsigned r = a - m - borrow;
    flag(c, MC_6502_V, ((a ^ m) & (a ^ r) & 0x80u) != 0);
    flag(c, MC_6502_C, r < 0x100u);
    if (!(c->p & MC_6502_D)) {
        c->a = (uint8_t)r;
    } else {
        const int lo = (int)(a & 0x0Fu) - (int)(m & 0x0Fu) - (int)borrow;
        int d = (int)a - (int)m - (int)borrow;
        if (d < 0) d -= 0x60;
        if (lo < 0) d -= 0x06;
        c->a = (uint8_t)d;
    }
    nz(c, c->a);
}

static void compare(mc_cpu6502 *c, uint8_t reg, uint8_t m) {
    flag(c, MC_6502_C, reg >= m);
    nz(c, (uint8_t)(reg - m));
}

/* Shifts, rotates, INC and DEC on a value. */
static uint8_t rmw(mc_cpu6502 *c, unsigned kind, uint8_t v) {
    unsigned r;
    switch (kind) {
    case 0: flag(c, MC_6502_C, v & 0x80u); r = (uint8_t)(v << 1); break;                         /* ASL */
    case 1: r = (uint8_t)(v << 1 | (c->p & MC_6502_C)); flag(c, MC_6502_C, v & 0x80u); break;   /* ROL */
    case 2: flag(c, MC_6502_C, v & 1u); r = v >> 1; break;                                      /* LSR */
    case 3: r = v >> 1 | ((c->p & MC_6502_C) ? 0x80u : 0); flag(c, MC_6502_C, v & 1u); break;  /* ROR */
    case 6: r = (uint8_t)(v - 1u); break;                                                       /* DEC */
    default: r = (uint8_t)(v + 1u); break;                                                      /* INC */
    }
    nz(c, (uint8_t)r);
    return (uint8_t)r;
}

static unsigned branch(mc_cpu6502 *c, int taken, unsigned base) {
    const int8_t rel = (int8_t)fetch(c);
    if (!taken) return base;
    const uint16_t to = (uint16_t)(c->pc + rel);
    const unsigned n = base + 1u + (page_cross(c->pc, to) ? 1u : 0);
    c->pc = to;
    return n;
}

static unsigned interrupt(mc_cpu6502 *c, uint16_t vector, int brk) {
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p | MC_6502_U) & (brk ? 0xFFu : (unsigned)~MC_6502_B)));
    c->p = (uint8_t)((c->p | MC_6502_I) & ~MC_6502_D);
    c->pc = (uint16_t)(RD(vector) | (unsigned)RD(vector + 1u) << 8);
    return 7;
}

void mc_cpu6502_init(mc_cpu6502 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                     void (*write)(void *, uint16_t, uint8_t), mc_6502_variant variant) {
    memset(c, 0, sizeof *c);
    c->ctx = ctx;
    c->read = read;
    c->write = write;
    c->variant = (uint8_t)variant;
    c->p = MC_6502_U | MC_6502_B | MC_6502_I;
    c->s = 0xFD;
}

unsigned mc_cpu6502_reset(mc_cpu6502 *c) {
    c->s = (uint8_t)(c->s - 3u);
    c->p = (uint8_t)((c->p | MC_6502_I | MC_6502_U | MC_6502_B) & ~MC_6502_D);
    c->pc = (uint16_t)(RD(0xFFFCu) | (unsigned)RD(0xFFFDu) << 8);
    c->waiting = c->stopped = 0;
    c->nmi = 0;
    c->cycles += 7;
    return 7;
}

void mc_cpu6502_set_irq(mc_cpu6502 *c, int asserted) { c->irq = asserted ? 1 : 0; }
void mc_cpu6502_nmi(mc_cpu6502 *c) { c->nmi = 1; }

/* The group of an opcode's read operand: the address modes of columns 1, 5,
 * 9, D (cc = 01) and of (zp) in column 2. */
static mode group1_mode(uint8_t op) {
    static const mode m[8] = {M_IZX, M_ZP, M_IMM, M_ABS, M_IZY, M_ZPX, M_ABY, M_ABX};
    return (op & 0x1Fu) == 0x12u ? M_IZP : m[(op >> 2) & 7u];
}

static unsigned execute(mc_cpu6502 *c, uint8_t op) {
    unsigned n = 0;
    uint16_t a;
    uint8_t v;

    /* ORA AND EOR ADC STA LDA CMP SBC: cc = 01, and (zp) at $x2 (odd rows). */
    if ((op & 3u) == 1u || ((op & 0x1Fu) == 0x12u)) {
        const mode m = group1_mode(op);
        const unsigned kind = op >> 5;
        static const unsigned base[] = {[M_IZX] = 6, [M_ZP] = 3, [M_IMM] = 2, [M_ABS] = 4, [M_IZY] = 5,
                                        [M_ZPX] = 4, [M_ABY] = 4, [M_ABX] = 4, [M_IZP] = 5};
        if (kind == 4) {                                            /* STA */
            if (m == M_IMM) {                                       /* $89: BIT #imm */
                v = fetch(c);
                flag(c, MC_6502_Z, (c->a & v) == 0);
                return 2;
            }
            a = ea(c, m, NULL);
            WR(a, c->a);
            return base[m] + ((m == M_ABX || m == M_ABY || m == M_IZY) ? 1u : 0);
        }
        n = base[m];
        a = ea(c, m, &n);
        v = RD(a);
        switch (kind) {
        case 0: c->a |= v; nz(c, c->a); break;
        case 1: c->a &= v; nz(c, c->a); break;
        case 2: c->a ^= v; nz(c, c->a); break;
        case 3: if (c->p & MC_6502_D) n++; adc(c, v); break;
        case 5: c->a = v; nz(c, v); break;
        case 6: compare(c, c->a, v); break;
        default: if (c->p & MC_6502_D) n++; sbc(c, v); break;
        }
        return n;
    }

    /* RMBn/SMBn ($x7) and BBRn/BBSn ($xF): the Rockwell bit instructions. */
    if ((op & 0x0Fu) == 0x07u) {
        const uint8_t zp = fetch(c), bit = (uint8_t)(1u << ((op >> 4) & 7u));
        v = RD(zp);
        WR(zp, (op & 0x80u) ? (v | bit) : (v & ~bit));
        return 5;
    }
    if ((op & 0x0Fu) == 0x0Fu) {
        const uint8_t zp = fetch(c), bit = (uint8_t)(1u << ((op >> 4) & 7u));
        v = RD(zp);
        return branch(c, (op & 0x80u) ? (v & bit) != 0 : (v & bit) == 0, 5);
    }

    switch (op) {
    /* Read-modify-write: ASL ROL LSR ROR (rows 0-3), DEC (6), INC (7). */
    case 0x06: case 0x26: case 0x46: case 0x66: case 0xC6: case 0xE6:
        a = fetch(c); WR(a, rmw(c, op >> 5, RD(a))); return 5;
    case 0x16: case 0x36: case 0x56: case 0x76: case 0xD6: case 0xF6:
        a = (uint8_t)(fetch(c) + c->x); WR(a, rmw(c, op >> 5, RD(a))); return 6;
    case 0x0E: case 0x2E: case 0x4E: case 0x6E: case 0xCE: case 0xEE:
        a = fetch16(c); WR(a, rmw(c, op >> 5, RD(a))); return 6;
    case 0x1E: case 0x3E: case 0x5E: case 0x7E:
        n = 6; a = ea(c, M_ABX, &n); WR(a, rmw(c, op >> 5, RD(a))); return n;
    case 0xDE: case 0xFE:
        a = ea(c, M_ABX, NULL); WR(a, rmw(c, op >> 5, RD(a))); return 7;
    case 0x0A: case 0x2A: case 0x4A: case 0x6A:
        c->a = rmw(c, op >> 5, c->a); return 2;
    case 0x1A: c->a = rmw(c, 7, c->a); return 2;                    /* INC A */
    case 0x3A: c->a = rmw(c, 6, c->a); return 2;                    /* DEC A */

    /* TSB, TRB. */
    case 0x04: case 0x0C: case 0x14: case 0x1C:
        a = (op & 0x08u) ? fetch16(c) : fetch(c);
        v = RD(a);
        flag(c, MC_6502_Z, (c->a & v) == 0);
        WR(a, (op & 0x10u) ? (v & ~c->a) : (v | c->a));
        return (op & 0x08u) ? 6 : 5;

    /* BIT. */
    case 0x24: case 0x2C: case 0x34: case 0x3C: {
        static const mode m[4] = {M_ZP, M_ABS, M_ZPX, M_ABX};
        static const unsigned b[4] = {3, 4, 4, 4};
        const unsigned i = ((op >> 3) & 1u) | ((op >> 3) & 2u);
        n = b[i];
        v = RD(ea(c, m[i], &n));
        c->p = (uint8_t)((c->p & ~(MC_6502_N | MC_6502_V)) | (v & 0xC0u));
        flag(c, MC_6502_Z, (c->a & v) == 0);
        return n;
    }

    /* Loads, stores and compares of X and Y. */
    case 0xA2: c->x = fetch(c); nz(c, c->x); return 2;
    case 0xA6: c->x = RD(ea(c, M_ZP, NULL)); nz(c, c->x); return 3;
    case 0xB6: c->x = RD(ea(c, M_ZPY, NULL)); nz(c, c->x); return 4;
    case 0xAE: c->x = RD(ea(c, M_ABS, NULL)); nz(c, c->x); return 4;
    case 0xBE: n = 4; c->x = RD(ea(c, M_ABY, &n)); nz(c, c->x); return n;
    case 0xA0: c->y = fetch(c); nz(c, c->y); return 2;
    case 0xA4: c->y = RD(ea(c, M_ZP, NULL)); nz(c, c->y); return 3;
    case 0xB4: c->y = RD(ea(c, M_ZPX, NULL)); nz(c, c->y); return 4;
    case 0xAC: c->y = RD(ea(c, M_ABS, NULL)); nz(c, c->y); return 4;
    case 0xBC: n = 4; c->y = RD(ea(c, M_ABX, &n)); nz(c, c->y); return n;
    case 0x86: WR(ea(c, M_ZP, NULL), c->x); return 3;
    case 0x96: WR(ea(c, M_ZPY, NULL), c->x); return 4;
    case 0x8E: WR(ea(c, M_ABS, NULL), c->x); return 4;
    case 0x84: WR(ea(c, M_ZP, NULL), c->y); return 3;
    case 0x94: WR(ea(c, M_ZPX, NULL), c->y); return 4;
    case 0x8C: WR(ea(c, M_ABS, NULL), c->y); return 4;
    case 0x64: WR(ea(c, M_ZP, NULL), 0); return 3;                  /* STZ */
    case 0x74: WR(ea(c, M_ZPX, NULL), 0); return 4;
    case 0x9C: WR(ea(c, M_ABS, NULL), 0); return 4;
    case 0x9E: WR(ea(c, M_ABX, NULL), 0); return 5;
    case 0xE0: compare(c, c->x, fetch(c)); return 2;
    case 0xE4: compare(c, c->x, RD(ea(c, M_ZP, NULL))); return 3;
    case 0xEC: compare(c, c->x, RD(ea(c, M_ABS, NULL))); return 4;
    case 0xC0: compare(c, c->y, fetch(c)); return 2;
    case 0xC4: compare(c, c->y, RD(ea(c, M_ZP, NULL))); return 3;
    case 0xCC: compare(c, c->y, RD(ea(c, M_ABS, NULL))); return 4;

    /* Branches. */
    case 0x10: return branch(c, !(c->p & MC_6502_N), 2);
    case 0x30: return branch(c, (c->p & MC_6502_N) != 0, 2);
    case 0x50: return branch(c, !(c->p & MC_6502_V), 2);
    case 0x70: return branch(c, (c->p & MC_6502_V) != 0, 2);
    case 0x90: return branch(c, !(c->p & MC_6502_C), 2);
    case 0xB0: return branch(c, (c->p & MC_6502_C) != 0, 2);
    case 0xD0: return branch(c, !(c->p & MC_6502_Z), 2);
    case 0xF0: return branch(c, (c->p & MC_6502_Z) != 0, 2);
    case 0x80: return branch(c, 1, 2);                              /* BRA: 3, 4 across a page */

    /* Jumps, calls, returns, BRK. */
    case 0x4C: c->pc = fetch16(c); return 3;
    case 0x6C: a = fetch16(c); c->pc = (uint16_t)(RD(a) | (unsigned)RD(a + 1u) << 8); return 6;
    case 0x7C: a = (uint16_t)(fetch16(c) + c->x); c->pc = (uint16_t)(RD(a) | (unsigned)RD(a + 1u) << 8); return 6;
    case 0x20:
        a = fetch16(c);
        c->pc--;                                                    /* the return address less one */
        push(c, (uint8_t)(c->pc >> 8));
        push(c, (uint8_t)c->pc);
        c->pc = a;
        return 6;
    case 0x60: a = pull(c); a |= (uint16_t)(pull(c) << 8); c->pc = (uint16_t)(a + 1u); return 6;
    case 0x40:
        c->p = (uint8_t)(pull(c) | MC_6502_U | MC_6502_B);
        a = pull(c);
        c->pc = (uint16_t)(a | (unsigned)pull(c) << 8);
        return 6;
    case 0x00: c->pc++; return interrupt(c, 0xFFFEu, 1);            /* BRK: the signature byte skipped */

    /* Stack. */
    case 0x48: push(c, c->a); return 3;
    case 0xDA: push(c, c->x); return 3;
    case 0x5A: push(c, c->y); return 3;
    case 0x08: push(c, (uint8_t)(c->p | MC_6502_B | MC_6502_U)); return 3;
    case 0x68: c->a = pull(c); nz(c, c->a); return 4;
    case 0xFA: c->x = pull(c); nz(c, c->x); return 4;
    case 0x7A: c->y = pull(c); nz(c, c->y); return 4;
    case 0x28: c->p = (uint8_t)(pull(c) | MC_6502_U | MC_6502_B); return 4;

    /* Implied. */
    case 0x18: c->p &= (uint8_t)~MC_6502_C; return 2;
    case 0x38: c->p |= MC_6502_C; return 2;
    case 0x58: c->p &= (uint8_t)~MC_6502_I; return 2;
    case 0x78: c->p |= MC_6502_I; return 2;
    case 0xB8: c->p &= (uint8_t)~MC_6502_V; return 2;
    case 0xD8: c->p &= (uint8_t)~MC_6502_D; return 2;
    case 0xF8: c->p |= MC_6502_D; return 2;
    case 0xAA: c->x = c->a; nz(c, c->x); return 2;
    case 0x8A: c->a = c->x; nz(c, c->a); return 2;
    case 0xA8: c->y = c->a; nz(c, c->y); return 2;
    case 0x98: c->a = c->y; nz(c, c->a); return 2;
    case 0xBA: c->x = c->s; nz(c, c->x); return 2;
    case 0x9A: c->s = c->x; return 2;
    case 0xE8: c->x++; nz(c, c->x); return 2;
    case 0xC8: c->y++; nz(c, c->y); return 2;
    case 0xCA: c->x--; nz(c, c->x); return 2;
    case 0x88: c->y--; nz(c, c->y); return 2;
    case 0xEA: return 2;

    /* WAI and STP (W65C02S); one-byte NOPs on the R65C02. */
    case 0xCB: if (c->variant == MC_6502_W65C02S) c->waiting = 1; return c->variant == MC_6502_W65C02S ? 3 : 1;
    case 0xDB: if (c->variant == MC_6502_W65C02S) c->stopped = 1; return c->variant == MC_6502_W65C02S ? 3 : 1;

    /* The reserved opcodes: NOPs (W65C02S data sheet, opcode matrix notes). */
    case 0x02: case 0x22: case 0x42: case 0x62: case 0x82: case 0xC2: case 0xE2:
        c->pc++; return 2;
    case 0x44: c->pc++; return 3;
    case 0x54: case 0xD4: case 0xF4: c->pc++; return 4;
    case 0x5C: c->pc = (uint16_t)(c->pc + 2u); return 8;
    case 0xDC: case 0xFC: c->pc = (uint16_t)(c->pc + 2u); return 4;
    default: return 1;                                              /* $x3, $xB: one byte, one cycle */
    }
}

unsigned mc_cpu6502_step(mc_cpu6502 *c) {
    unsigned n;
    if (c->stopped) return 0;
    if (c->nmi) {
        c->nmi = 0;
        c->waiting = 0;
        n = interrupt(c, 0xFFFAu, 0);
    } else if (c->irq && !(c->p & MC_6502_I)) {
        c->waiting = 0;
        n = interrupt(c, 0xFFFEu, 0);
    } else if (c->waiting) {
        if (!c->irq) return 0;
        c->waiting = 0;                                             /* masked /IRQ: resume after WAI */
        n = 1;
    } else {
        const uint8_t op = fetch(c);
        c->used[op >> 5] |= 1u << (op & 31u);
        n = execute(c, op);
    }
    c->cycles += n;
    return n;
}
