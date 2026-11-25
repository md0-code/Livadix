/* Livadix — the 68HC05 CPU core (see cpu6805.h for sources). Livadix's own code.
 *
 * Flags (M68HC05 instruction set): ADD/ADC set H, N, Z, C; SUB, SBC, CMP,
 * CPX set N, Z, C (C = borrow); AND, ORA, EOR, BIT, LDA, LDX, STA, STX set
 * N, Z; NEG sets N, Z and C = (result != 0); COM sets N, Z and C = 1; LSR
 * clears N; the shifts and rotates set C from the bit shifted out; INC,
 * DEC, TST set N, Z; CLR clears N and sets Z; MUL clears H and C;
 * BRSET/BRCLR put the tested bit in C.
 *
 * HC05 cycle counts: BRSET/BRCLR 5, BSET/BCLR 5, branches 3, BSR 6;
 * read-modify-write direct 5, inherent 3, indexed 8-bit offset 6, indexed
 * 5 (TST 4, 3, 5, 4); MUL 11; RTI 9, RTS 6, SWI 10, the other inherent
 * control instructions 2; register/memory immediate 2, direct 3, extended
 * 4, indexed 16-bit offset 5, 8-bit offset 4, no offset 3 (stores one more
 * except immediate; JMP 2, 3, 4, 3, 2; JSR 5, 6, 7, 6, 5). */
#include "cpu6805.h"

#include <string.h>

#define RD(a) c->read(c->ctx, (uint16_t)(a))
#define WR(a, v) c->write(c->ctx, (uint16_t)(a), (uint8_t)(v))

static uint16_t sp_address(const mc_cpu6805 *c) { return (uint16_t)(0x00C0u | (c->sp & 0x3Fu)); }

static void push(mc_cpu6805 *c, uint8_t v) {
    WR(sp_address(c), v);
    c->sp = (uint8_t)(0xC0u | ((c->sp - 1u) & 0x3Fu));
}

static uint8_t pull(mc_cpu6805 *c) {
    c->sp = (uint8_t)(0xC0u | ((c->sp + 1u) & 0x3Fu));
    return RD(sp_address(c));
}

static uint8_t fetch(mc_cpu6805 *c) {
    const uint8_t v = RD(c->pc);
    c->pc = (uint16_t)((c->pc + 1u) & c->pc_mask);
    return v;
}

static void nz(mc_cpu6805 *c, uint8_t v) {
    c->cc = (uint8_t)(c->cc & ~(MC_6805_N | MC_6805_Z));
    if (v & 0x80u) c->cc |= MC_6805_N;
    if (!v) c->cc |= MC_6805_Z;
}

static void set_c(mc_cpu6805 *c, int on) { c->cc = (uint8_t)(on ? c->cc | MC_6805_C : c->cc & ~MC_6805_C); }

static void branch(mc_cpu6805 *c, int taken, uint8_t rel) {
    if (taken) c->pc = (uint16_t)((c->pc + (uint16_t)(int8_t)rel) & c->pc_mask);
}

static int condition(const mc_cpu6805 *c, unsigned n) {
    const unsigned cc = c->cc;
    int r;
    switch (n >> 1) {
    case 0: r = 1; break;                                               /* BRA / BRN */
    case 1: r = !(cc & (MC_6805_C | MC_6805_Z)); break;                 /* BHI / BLS */
    case 2: r = !(cc & MC_6805_C); break;                               /* BCC / BCS */
    case 3: r = !(cc & MC_6805_Z); break;                               /* BNE / BEQ */
    case 4: r = !(cc & MC_6805_H); break;                               /* BHCC / BHCS */
    case 5: r = !(cc & MC_6805_N); break;                               /* BPL / BMI */
    case 6: r = !(cc & MC_6805_I); break;                               /* BMC / BMS */
    default: r = !c->irq_pin; break;                                    /* BIL / BIH */
    }
    return (n & 1u) ? !r : r;
}

/* Read-modify-write operations (low nibble of $3x-$7x); returns -1 for an
 * undefined one. */
static int rmw(mc_cpu6805 *c, unsigned op, uint8_t v, uint8_t *out) {
    unsigned r;
    switch (op) {
    case 0x0: r = (uint8_t)(0u - v); set_c(c, r != 0); break;          /* NEG */
    case 0x3: r = (uint8_t)~v; set_c(c, 1); break;                      /* COM */
    case 0x4: set_c(c, v & 1u); r = v >> 1; break;                      /* LSR */
    case 0x6: r = (v >> 1) | ((c->cc & MC_6805_C) ? 0x80u : 0); set_c(c, v & 1u); break;   /* ROR */
    case 0x7: set_c(c, v & 1u); r = (v >> 1) | (v & 0x80u); break;      /* ASR */
    case 0x8: set_c(c, v & 0x80u); r = (uint8_t)(v << 1); break;        /* LSL */
    case 0x9: r = (uint8_t)(v << 1 | ((c->cc & MC_6805_C) ? 1u : 0)); set_c(c, v & 0x80u); break; /* ROL */
    case 0xA: r = (uint8_t)(v - 1u); break;                             /* DEC */
    case 0xC: r = (uint8_t)(v + 1u); break;                             /* INC */
    case 0xD: r = v; break;                                             /* TST */
    case 0xF: r = 0; break;                                             /* CLR */
    default: return -1;
    }
    nz(c, (uint8_t)r);
    *out = (uint8_t)r;
    return 0;
}

static uint8_t add(mc_cpu6805 *c, uint8_t a, uint8_t b, unsigned carry) {
    const unsigned r = (unsigned)a + b + carry;
    const unsigned h = ((a & 0xFu) + (b & 0xFu) + carry) > 0xFu;
    c->cc = (uint8_t)((c->cc & ~(MC_6805_H | MC_6805_C)) | (h ? MC_6805_H : 0) | (r > 0xFFu ? MC_6805_C : 0));
    nz(c, (uint8_t)r);
    return (uint8_t)r;
}

static uint8_t sub(mc_cpu6805 *c, uint8_t a, uint8_t b, unsigned borrow) {
    const unsigned r = (unsigned)a - b - borrow;
    set_c(c, r > 0xFFu);
    nz(c, (uint8_t)r);
    return (uint8_t)r;
}

static void interrupt_entry(mc_cpu6805 *c, uint16_t vector) {
    push(c, (uint8_t)c->pc);
    push(c, (uint8_t)(c->pc >> 8));
    push(c, c->x);
    push(c, c->a);
    push(c, c->cc);
    c->cc |= MC_6805_I;
    c->pc = (uint16_t)(((unsigned)RD(vector) << 8 | RD(vector + 1u)) & c->pc_mask);
}

unsigned mc_cpu6805_interrupt(mc_cpu6805 *c, uint16_t vector) {
    if (c->cc & MC_6805_I) return 0;
    c->stopped = c->waiting = 0;
    interrupt_entry(c, vector);
    c->cycles += 10;
    return 10;
}

/* Cycle tables of the register/memory group by mode (IMM, DIR, EXT, IX2, IX1, IX). */
static const uint8_t cyc_rm[6] = {2, 3, 4, 5, 4, 3};
static const uint8_t cyc_st[6] = {0, 4, 5, 6, 5, 4};
static const uint8_t cyc_jmp[6] = {0, 2, 3, 4, 3, 2};
static const uint8_t cyc_jsr[6] = {6, 5, 6, 7, 6, 5};    /* [0] is BSR */

unsigned mc_cpu6805_step(mc_cpu6805 *c) {
    if (c->stopped || c->waiting) return 0;
    const uint16_t start = c->pc;
    const uint8_t op = fetch(c);
    const unsigned hi = op >> 4, lo = op & 15u;
    unsigned n = 2;
    switch (hi) {
    case 0x0: {                                         /* BRSET/BRCLR n,dd,rr */
        const uint8_t d = fetch(c), rel = fetch(c);
        const unsigned bit = (RD(d) >> (lo >> 1)) & 1u;
        set_c(c, bit);
        branch(c, (lo & 1u) ? !bit : bit, rel);
        n = 5;
        break;
    }
    case 0x1: {                                         /* BSET/BCLR n,dd */
        const uint8_t d = fetch(c);
        const uint8_t m = (uint8_t)(1u << (lo >> 1));
        const uint8_t v = RD(d);
        WR(d, (lo & 1u) ? v & ~m : v | m);
        n = 5;
        break;
    }
    case 0x2: {                                         /* Bcc rr */
        const uint8_t rel = fetch(c);
        branch(c, condition(c, lo), rel);
        n = 3;
        break;
    }
    case 0x3: case 0x4: case 0x5: case 0x6: case 0x7: {
        if (op == 0x42) {                               /* MUL */
            const unsigned r = (unsigned)c->a * c->x;
            c->x = (uint8_t)(r >> 8);
            c->a = (uint8_t)r;
            c->cc = (uint8_t)(c->cc & ~(MC_6805_H | MC_6805_C));
            n = 11;
            break;
        }
        uint16_t ea = 0;
        uint8_t v, r;
        if (hi == 0x3) ea = fetch(c);
        else if (hi == 0x6) ea = (uint16_t)(fetch(c) + c->x);
        else if (hi == 0x7) ea = c->x;
        v = hi == 0x4 ? c->a : hi == 0x5 ? c->x : RD(ea);
        if (rmw(c, lo, v, &r) < 0) {
            c->pc = (uint16_t)((start + 1u) & c->pc_mask);
            c->illegal++;
            n = 2;
            break;
        }
        if (hi == 0x4) c->a = r;
        else if (hi == 0x5) c->x = r;
        else if (lo != 0xD) WR(ea, r);
        static const uint8_t cyc[5] = {5, 3, 3, 6, 5}, cyc_tst[5] = {4, 3, 3, 5, 4};
        n = (lo == 0xD ? cyc_tst : cyc)[hi - 3];
        break;
    }
    case 0x8: case 0x9:
        switch (op) {
        case 0x80:                                      /* RTI */
            c->cc = (uint8_t)(pull(c) | 0xE0u);
            c->a = pull(c);
            c->x = pull(c);
            c->pc = (uint16_t)(pull(c) << 8);
            c->pc = (uint16_t)((c->pc | pull(c)) & c->pc_mask);
            n = 9;
            break;
        case 0x81:                                      /* RTS */
            c->pc = (uint16_t)(pull(c) << 8);
            c->pc = (uint16_t)((c->pc | pull(c)) & c->pc_mask);
            n = 6;
            break;
        case 0x83:                                      /* SWI: not masked by I */
            interrupt_entry(c, (uint16_t)(c->vector_top - 2u));
            n = 10;
            break;
        case 0x8E: c->cc &= (uint8_t)~MC_6805_I; c->stopped = 1; break;    /* STOP */
        case 0x8F: c->cc &= (uint8_t)~MC_6805_I; c->waiting = 1; break;    /* WAIT */
        case 0x97: c->x = c->a; break;                  /* TAX */
        case 0x98: c->cc &= (uint8_t)~MC_6805_C; break; /* CLC */
        case 0x99: c->cc |= MC_6805_C; break;           /* SEC */
        case 0x9A: c->cc &= (uint8_t)~MC_6805_I; break; /* CLI */
        case 0x9B: c->cc |= MC_6805_I; break;           /* SEI */
        case 0x9C: c->sp = 0xFF; break;                 /* RSP */
        case 0x9D: break;                               /* NOP */
        case 0x9F: c->a = c->x; break;                  /* TXA */
        default: c->illegal++; break;
        }
        break;
    default: {                                          /* $A0-$FF register/memory */
        const unsigned mode = hi - 0xAu;                /* 0 IMM, 1 DIR, 2 EXT, 3 IX2, 4 IX1, 5 IX */
        uint16_t ea = 0;
        if (mode == 0) {
            if (lo == 0x7 || lo == 0xC || lo == 0xF) {
                c->illegal++;
                break;
            }
            if (lo == 0xD) {                            /* BSR */
                const uint8_t rel = fetch(c);
                push(c, (uint8_t)c->pc);
                push(c, (uint8_t)(c->pc >> 8));
                branch(c, 1, rel);
                n = 6;
                break;
            }
            ea = c->pc;
            c->pc = (uint16_t)((c->pc + 1u) & c->pc_mask);
        } else if (mode == 1) ea = fetch(c);
        else if (mode == 2) {
            ea = (uint16_t)(fetch(c) << 8);
            ea |= fetch(c);
        } else if (mode == 3) {
            ea = (uint16_t)(fetch(c) << 8);
            ea = (uint16_t)((ea | fetch(c)) + c->x);
        } else if (mode == 4) ea = (uint16_t)(fetch(c) + c->x);
        else ea = c->x;
        n = cyc_rm[mode];
        switch (lo) {
        case 0x0: c->a = sub(c, c->a, RD(ea), 0); break;                                 /* SUB */
        case 0x1: (void)sub(c, c->a, RD(ea), 0); break;                                  /* CMP */
        case 0x2: c->a = sub(c, c->a, RD(ea), (c->cc & MC_6805_C) ? 1u : 0); break;      /* SBC */
        case 0x3: (void)sub(c, c->x, RD(ea), 0); break;                                  /* CPX */
        case 0x4: c->a &= RD(ea); nz(c, c->a); break;                                    /* AND */
        case 0x5: nz(c, (uint8_t)(c->a & RD(ea))); break;                                /* BIT */
        case 0x6: c->a = RD(ea); nz(c, c->a); break;                                     /* LDA */
        case 0x7: WR(ea, c->a); nz(c, c->a); n = cyc_st[mode]; break;                    /* STA */
        case 0x8: c->a ^= RD(ea); nz(c, c->a); break;                                    /* EOR */
        case 0x9: c->a = add(c, c->a, RD(ea), (c->cc & MC_6805_C) ? 1u : 0); break;      /* ADC */
        case 0xA: c->a |= RD(ea); nz(c, c->a); break;                                    /* ORA */
        case 0xB: c->a = add(c, c->a, RD(ea), 0); break;                                 /* ADD */
        case 0xC: c->pc = (uint16_t)(ea & c->pc_mask); n = cyc_jmp[mode]; break;         /* JMP */
        case 0xD:                                                                        /* JSR */
            push(c, (uint8_t)c->pc);
            push(c, (uint8_t)(c->pc >> 8));
            c->pc = (uint16_t)(ea & c->pc_mask);
            n = cyc_jsr[mode];
            break;
        case 0xE: c->x = RD(ea); nz(c, c->x); break;                                     /* LDX */
        default: WR(ea, c->x); nz(c, c->x); n = cyc_st[mode]; break;                     /* STX */
        }
        break;
    }
    }
    c->cycles += n;
    return n;
}

void mc_cpu6805_reset(mc_cpu6805 *c) {
    c->sp = 0xFF;
    c->cc = (uint8_t)(0xE0u | MC_6805_I);
    c->stopped = c->waiting = 0;
    c->pc = (uint16_t)(((unsigned)RD(c->vector_top) << 8 | RD(c->vector_top + 1u)) & c->pc_mask);
}

void mc_cpu6805_init(mc_cpu6805 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                     void (*write)(void *, uint16_t, uint8_t), uint16_t vector_top) {
    memset(c, 0, sizeof *c);
    c->ctx = ctx;
    c->read = read;
    c->write = write;
    c->vector_top = vector_top;
    c->pc_mask = 0x1FFFu;
    c->irq_pin = 1;
    c->cc = 0xE0u | MC_6805_I;
    c->sp = 0xFF;
}
