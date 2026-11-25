/* Livadix — the MELPS 740 CPU core (see cpu740.h for sources). Livadix's own code:
 * one table built from the Software manual's Appendix 2 and 3 gives each
 * opcode its mnemonic, addressing mode and phi cycle count, and the
 * executor works from the table. */
#include "cpu740.h"

#include <stdio.h>
#include <string.h>

enum {
    M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_IZX, M_IZY,
    M_IND,      /* JMP ($hhll) */
    M_ZIND,     /* JMP/JSR ($zz) */
    M_SP,       /* JSR \$FFzz */
    M_REL, M_BBA, M_BBZ, M_LDM
};

enum {
    O_BAD,
    O_ORA, O_AND, O_EOR, O_ADC, O_STA, O_LDA, O_CMP, O_SBC,
    O_ASL, O_ROL, O_LSR, O_ROR, O_INC, O_DEC,
    O_LDX, O_LDY, O_STX, O_STY, O_CPX, O_CPY,
    O_BIT, O_TST, O_COM, O_LDM, O_RRF,
    O_BBS, O_BBC, O_SEB, O_CLB,
    O_BPL, O_BMI, O_BVC, O_BVS, O_BCC, O_BCS, O_BNE, O_BEQ, O_BRA,
    O_JMP, O_JSR, O_RTS, O_RTI, O_BRK,
    O_PHA, O_PHP, O_PLA, O_PLP,
    O_TAX, O_TXA, O_TAY, O_TYA, O_TSX, O_TXS,
    O_INX, O_DEX, O_INY, O_DEY,
    O_CLC, O_SEC, O_CLI, O_SEI, O_CLV, O_CLD, O_SED, O_CLT, O_SET,
    O_NOP, O_WIT, O_STP, O_MUL, O_DIV,
    O_COUNT
};

static const char *const NAMES[O_COUNT] = {
    "???",
    "ORA", "AND", "EOR", "ADC", "STA", "LDA", "CMP", "SBC",
    "ASL", "ROL", "LSR", "ROR", "INC", "DEC",
    "LDX", "LDY", "STX", "STY", "CPX", "CPY",
    "BIT", "TST", "COM", "LDM", "RRF",
    "BBS", "BBC", "SEB", "CLB",
    "BPL", "BMI", "BVC", "BVS", "BCC", "BCS", "BNE", "BEQ", "BRA",
    "JMP", "JSR", "RTS", "RTI", "BRK",
    "PHA", "PHP", "PLA", "PLP",
    "TAX", "TXA", "TAY", "TYA", "TSX", "TXS",
    "INX", "DEX", "INY", "DEY",
    "CLC", "SEC", "CLI", "SEI", "CLV", "CLD", "SED", "CLT", "SET",
    "NOP", "WIT", "STP", "MUL", "DIV",
};

typedef struct op740 {
    uint8_t op, mode, cycles;
} op740;

static op740 TABLE[256];
static int table_ready;

static void def(unsigned code, unsigned op, unsigned mode, unsigned cycles) {
    TABLE[code].op = (uint8_t)op;
    TABLE[code].mode = (uint8_t)mode;
    TABLE[code].cycles = (uint8_t)cycles;
}

/* Appendix 2 (instruction table) and Appendix 3 (code table). */
static void build_table(void) {
    for (unsigned i = 0; i < 256; i++) def(i, O_BAD, M_IMP, 2);
    /* The eight accumulator operations: aaa bbb 01, bbb = (zz,X), zz, #nn,
     * hhll, (zz),Y, zz,X, hhll,Y, hhll,X. */
    static const uint8_t alu[8] = {O_ORA, O_AND, O_EOR, O_ADC, O_STA, O_LDA, O_CMP, O_SBC};
    static const uint8_t modes[8] = {M_IZX, M_ZP, M_IMM, M_ABS, M_IZY, M_ZPX, M_ABY, M_ABX};
    static const uint8_t load_cycles[8] = {6, 3, 2, 4, 6, 4, 5, 5};
    static const uint8_t store_cycles[8] = {7, 4, 0, 5, 7, 5, 6, 6};
    for (unsigned a = 0; a < 8; a++)
        for (unsigned b = 0; b < 8; b++) {
            if (alu[a] == O_STA && modes[b] == M_IMM) continue;
            def(a << 5 | b << 2 | 1, alu[a], modes[b], alu[a] == O_STA ? store_cycles[b] : load_cycles[b]);
        }
    /* Shifts and rotates: zz, A, hhll, zz,X, hhll,X. */
    static const uint8_t shifts[4] = {O_ASL, O_ROL, O_LSR, O_ROR};
    for (unsigned a = 0; a < 4; a++) {
        def(a << 5 | 0x06, shifts[a], M_ZP, 5);
        def(a << 5 | 0x0A, shifts[a], M_ACC, 2);
        def(a << 5 | 0x0E, shifts[a], M_ABS, 6);
        def(a << 5 | 0x16, shifts[a], M_ZPX, 6);
        def(a << 5 | 0x1E, shifts[a], M_ABX, 7);
    }
    def(0xC6, O_DEC, M_ZP, 5); def(0xD6, O_DEC, M_ZPX, 6); def(0xCE, O_DEC, M_ABS, 6); def(0xDE, O_DEC, M_ABX, 7);
    def(0xE6, O_INC, M_ZP, 5); def(0xF6, O_INC, M_ZPX, 6); def(0xEE, O_INC, M_ABS, 6); def(0xFE, O_INC, M_ABX, 7);
    def(0x1A, O_DEC, M_ACC, 2); def(0x3A, O_INC, M_ACC, 2);
    def(0xA2, O_LDX, M_IMM, 2); def(0xA6, O_LDX, M_ZP, 3); def(0xB6, O_LDX, M_ZPY, 4);
    def(0xAE, O_LDX, M_ABS, 4); def(0xBE, O_LDX, M_ABY, 5);
    def(0xA0, O_LDY, M_IMM, 2); def(0xA4, O_LDY, M_ZP, 3); def(0xB4, O_LDY, M_ZPX, 4);
    def(0xAC, O_LDY, M_ABS, 4); def(0xBC, O_LDY, M_ABX, 5);
    def(0x86, O_STX, M_ZP, 4); def(0x96, O_STX, M_ZPY, 5); def(0x8E, O_STX, M_ABS, 5);
    def(0x84, O_STY, M_ZP, 4); def(0x94, O_STY, M_ZPX, 5); def(0x8C, O_STY, M_ABS, 6);
    def(0xE0, O_CPX, M_IMM, 2); def(0xE4, O_CPX, M_ZP, 3); def(0xEC, O_CPX, M_ABS, 4);
    def(0xC0, O_CPY, M_IMM, 2); def(0xC4, O_CPY, M_ZP, 3); def(0xCC, O_CPY, M_ABS, 4);
    def(0x24, O_BIT, M_ZP, 3); def(0x2C, O_BIT, M_ABS, 4);
    def(0x64, O_TST, M_ZP, 3);
    def(0x44, O_COM, M_ZP, 5);
    def(0x3C, O_LDM, M_LDM, 4);
    def(0x82, O_RRF, M_ZP, 8);
    /* Bit management: i = bits 7-5 (Appendix 2: (2i)x10+3, (2i+1)x10+3...). */
    for (unsigned i = 0; i < 8; i++) {
        def(i << 5 | 0x03, O_BBS, M_BBA, 4); def(i << 5 | 0x13, O_BBC, M_BBA, 4);
        def(i << 5 | 0x07, O_BBS, M_BBZ, 5); def(i << 5 | 0x17, O_BBC, M_BBZ, 5);
        def(i << 5 | 0x0B, O_SEB, M_ACC, 2); def(i << 5 | 0x1B, O_CLB, M_ACC, 2);
        def(i << 5 | 0x0F, O_SEB, M_ZP, 5); def(i << 5 | 0x1F, O_CLB, M_ZP, 5);
    }
    def(0x10, O_BPL, M_REL, 2); def(0x30, O_BMI, M_REL, 2); def(0x50, O_BVC, M_REL, 2); def(0x70, O_BVS, M_REL, 2);
    def(0x90, O_BCC, M_REL, 2); def(0xB0, O_BCS, M_REL, 2); def(0xD0, O_BNE, M_REL, 2); def(0xF0, O_BEQ, M_REL, 2);
    def(0x80, O_BRA, M_REL, 4);
    def(0x4C, O_JMP, M_ABS, 3); def(0x6C, O_JMP, M_IND, 5); def(0xB2, O_JMP, M_ZIND, 4);
    def(0x20, O_JSR, M_ABS, 6); def(0x02, O_JSR, M_ZIND, 7); def(0x22, O_JSR, M_SP, 5);
    def(0x60, O_RTS, M_IMP, 6); def(0x40, O_RTI, M_IMP, 6); def(0x00, O_BRK, M_IMP, 7);
    def(0x48, O_PHA, M_IMP, 3); def(0x08, O_PHP, M_IMP, 3); def(0x68, O_PLA, M_IMP, 4); def(0x28, O_PLP, M_IMP, 4);
    def(0xAA, O_TAX, M_IMP, 2); def(0x8A, O_TXA, M_IMP, 2); def(0xA8, O_TAY, M_IMP, 2);
    def(0x98, O_TYA, M_IMP, 2); def(0xBA, O_TSX, M_IMP, 2); def(0x9A, O_TXS, M_IMP, 2);
    def(0xE8, O_INX, M_IMP, 2); def(0xCA, O_DEX, M_IMP, 2); def(0xC8, O_INY, M_IMP, 2); def(0x88, O_DEY, M_IMP, 2);
    def(0x18, O_CLC, M_IMP, 2); def(0x38, O_SEC, M_IMP, 2); def(0x58, O_CLI, M_IMP, 2); def(0x78, O_SEI, M_IMP, 2);
    def(0xB8, O_CLV, M_IMP, 2); def(0xD8, O_CLD, M_IMP, 2); def(0xF8, O_SED, M_IMP, 2);
    def(0x12, O_CLT, M_IMP, 2); def(0x32, O_SET, M_IMP, 2);
    def(0xEA, O_NOP, M_IMP, 2); def(0xC2, O_WIT, M_IMP, 2); def(0x42, O_STP, M_IMP, 2);
    def(0x62, O_MUL, M_ZPX, 15); def(0xE2, O_DIV, M_ZPX, 16);
    table_ready = 1;
}

/* --- memory --------------------------------------------------------------------- */

static uint8_t rd(mc_cpu740 *c, uint16_t a) { return c->read(c->ctx, a); }
static void wr(mc_cpu740 *c, uint16_t a, uint8_t v) { c->write(c->ctx, a, v); }
static uint8_t fetch(mc_cpu740 *c) { return rd(c, c->pc++); }
static void push(mc_cpu740 *c, uint8_t v) {
    wr(c, (uint16_t)(c->stack_page | c->s), v);
    c->s--;
}
static uint8_t pull(mc_cpu740 *c) {
    c->s++;
    return rd(c, (uint16_t)(c->stack_page | c->s));
}

static void nz(mc_cpu740 *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(MC_740_N | MC_740_Z)) | (v & 0x80u) | (v ? 0 : MC_740_Z));
}
static void set_flag(mc_cpu740 *c, uint8_t flag, int on) {
    c->p = (uint8_t)(on ? (c->p | flag) : (c->p & ~flag));
}

/* --- arithmetic (Software manual 3.3: ADC, SBC, CMP) ------------------------------- */

static uint8_t adc(mc_cpu740 *c, uint8_t a, uint8_t m) {
    const unsigned carry = c->p & MC_740_C;
    const unsigned sum = (unsigned)a + m + carry;
    set_flag(c, MC_740_V, (~(a ^ m) & (a ^ sum) & 0x80u) != 0);
    nz(c, (uint8_t)sum);
    if (!(c->p & MC_740_D)) {
        set_flag(c, MC_740_C, sum > 0xFFu);
        return (uint8_t)sum;
    }
    unsigned lo = (a & 0x0Fu) + (m & 0x0Fu) + carry;
    unsigned hi = (unsigned)(a >> 4) + (m >> 4);
    if (lo > 9) { lo += 6; hi++; }
    if (hi > 9) hi += 6;
    set_flag(c, MC_740_C, hi > 0x0Fu);
    return (uint8_t)(hi << 4 | (lo & 0x0Fu));
}

static uint8_t sbc(mc_cpu740 *c, uint8_t a, uint8_t m) {
    const unsigned borrow = (c->p & MC_740_C) ? 0u : 1u;
    const unsigned diff = (unsigned)a - m - borrow;
    set_flag(c, MC_740_V, ((a ^ m) & (a ^ diff) & 0x80u) != 0);
    nz(c, (uint8_t)diff);
    set_flag(c, MC_740_C, diff < 0x100u);
    if (!(c->p & MC_740_D)) return (uint8_t)diff;
    int lo = (int)(a & 0x0Fu) - (int)(m & 0x0Fu) - (int)borrow;
    int hi = (int)(a >> 4) - (int)(m >> 4);
    if (lo < 0) { lo -= 6; hi--; }
    if (hi < 0) hi -= 6;
    return (uint8_t)((unsigned)hi << 4 | ((unsigned)lo & 0x0Fu));
}

static void compare(mc_cpu740 *c, uint8_t r, uint8_t m) {
    nz(c, (uint8_t)(r - m));
    set_flag(c, MC_740_C, r >= m);
}

/* --- execution ------------------------------------------------------------------- */

void mc_cpu740_init(mc_cpu740 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                    void (*write)(void *, uint16_t, uint8_t)) {
    if (!table_ready) build_table();
    memset(c, 0, sizeof *c);
    c->ctx = ctx;
    c->read = read;
    c->write = write;
}

void mc_cpu740_reset(mc_cpu740 *c) {
    c->p |= MC_740_I;
    c->waiting = c->stopped = 0;
    c->pc = (uint16_t)(rd(c, 0xFFFE) | rd(c, 0xFFFF) << 8);
}

unsigned mc_cpu740_interrupt(mc_cpu740 *c, uint16_t vector) {
    if (c->p & MC_740_I) return 0;
    c->waiting = c->stopped = 0;
    push(c, (uint8_t)(c->pc >> 8));
    push(c, (uint8_t)c->pc);
    c->p &= (uint8_t)~MC_740_B;
    push(c, c->p);
    c->p |= MC_740_I;
    c->pc = (uint16_t)(rd(c, vector) | rd(c, (uint16_t)(vector + 1)) << 8);
    c->cycles += 7;
    return 7;
}

/* The effective address of the operand (modes with a memory operand). */
static uint16_t address(mc_cpu740 *c, unsigned mode) {
    uint16_t a;
    uint8_t z;
    switch (mode) {
    case M_ZP: return fetch(c);
    case M_ZPX: return (uint8_t)(fetch(c) + c->x);
    case M_ZPY: return (uint8_t)(fetch(c) + c->y);
    case M_ABS: a = fetch(c); return (uint16_t)(a | fetch(c) << 8);
    case M_ABX: a = fetch(c); a = (uint16_t)(a | fetch(c) << 8); return (uint16_t)(a + c->x);
    case M_ABY: a = fetch(c); a = (uint16_t)(a | fetch(c) << 8); return (uint16_t)(a + c->y);
    case M_IZX:
        z = (uint8_t)(fetch(c) + c->x);
        return (uint16_t)(rd(c, z) | rd(c, (uint8_t)(z + 1)) << 8);
    case M_IZY:
        z = fetch(c);
        a = (uint16_t)(rd(c, z) | rd(c, (uint8_t)(z + 1)) << 8);
        return (uint16_t)(a + c->y);
    default: return 0;
    }
}

static void branch(mc_cpu740 *c, int8_t rel, int taken, unsigned *cycles) {
    if (!taken) return;
    c->pc = (uint16_t)(c->pc + rel);
    *cycles += 2;
}

unsigned mc_cpu740_step(mc_cpu740 *c) {
    if (c->waiting || c->stopped) return 0;
    const uint8_t code = fetch(c);
    const op740 *o = &TABLE[code];
    unsigned cycles = o->cycles;
    const unsigned mode = o->mode;
    uint16_t ea = 0;
    uint8_t m = 0, v;

    switch (o->op) {
    /* The accumulator operations, with the T flag's M(X) form. */
    case O_ORA: case O_AND: case O_EOR: case O_ADC: case O_LDA: case O_CMP: case O_SBC: {
        m = mode == M_IMM ? fetch(c) : rd(c, address(c, mode));
        const int t = (c->p & MC_740_T) != 0;
        const uint8_t r = t ? rd(c, c->x) : c->a;
        uint8_t out = r;
        switch (o->op) {
        case O_ORA: out = r | m; nz(c, out); break;
        case O_AND: out = r & m; nz(c, out); break;
        case O_EOR: out = r ^ m; nz(c, out); break;
        case O_ADC: out = adc(c, r, m); break;
        case O_SBC: out = sbc(c, r, m); break;
        case O_LDA: out = m; nz(c, out); break;
        default: compare(c, r, m); break;
        }
        if (o->op != O_CMP) {
            if (t) wr(c, c->x, out);
            else c->a = out;
        }
        if (t) cycles += o->op == O_LDA ? 2u : o->op == O_CMP ? 1u : 3u;
        break;
    }
    case O_STA: wr(c, address(c, mode), c->a); break;
    case O_STX: wr(c, address(c, mode), c->x); break;
    case O_STY: wr(c, address(c, mode), c->y); break;
    case O_LDX: c->x = mode == M_IMM ? fetch(c) : rd(c, address(c, mode)); nz(c, c->x); break;
    case O_LDY: c->y = mode == M_IMM ? fetch(c) : rd(c, address(c, mode)); nz(c, c->y); break;
    case O_CPX: compare(c, c->x, mode == M_IMM ? fetch(c) : rd(c, address(c, mode))); break;
    case O_CPY: compare(c, c->y, mode == M_IMM ? fetch(c) : rd(c, address(c, mode))); break;

    /* Read-modify-write. */
    case O_ASL: case O_ROL: case O_LSR: case O_ROR: case O_INC: case O_DEC: {
        if (mode == M_ACC) v = c->a;
        else { ea = address(c, mode); v = rd(c, ea); }
        const unsigned carry = c->p & MC_740_C;
        switch (o->op) {
        case O_ASL: set_flag(c, MC_740_C, v & 0x80u); v = (uint8_t)(v << 1); break;
        case O_ROL: set_flag(c, MC_740_C, v & 0x80u); v = (uint8_t)(v << 1 | carry); break;
        case O_LSR: set_flag(c, MC_740_C, v & 1u); v = (uint8_t)(v >> 1); break;
        case O_ROR: set_flag(c, MC_740_C, v & 1u); v = (uint8_t)(v >> 1 | carry << 7); break;
        case O_INC: v++; break;
        default: v--; break;
        }
        nz(c, v);
        if (mode == M_ACC) c->a = v;
        else wr(c, ea, v);
        break;
    }
    case O_BIT:
        m = rd(c, address(c, mode));
        c->p = (uint8_t)((c->p & ~(MC_740_N | MC_740_V | MC_740_Z)) | (m & 0xC0u) | ((c->a & m) ? 0 : MC_740_Z));
        break;
    case O_TST: nz(c, rd(c, address(c, mode))); break;
    case O_COM: ea = address(c, mode); v = (uint8_t)~rd(c, ea); nz(c, v); wr(c, ea, v); break;
    case O_LDM: v = fetch(c); wr(c, fetch(c), v); break;
    case O_RRF: ea = address(c, mode); v = rd(c, ea); wr(c, ea, (uint8_t)(v << 4 | v >> 4)); break;

    /* Bit management. */
    case O_SEB: case O_CLB: {
        const uint8_t bit = (uint8_t)(1u << (code >> 5));
        if (mode == M_ACC) c->a = o->op == O_SEB ? (uint8_t)(c->a | bit) : (uint8_t)(c->a & ~bit);
        else {
            ea = fetch(c);
            v = rd(c, ea);
            wr(c, ea, o->op == O_SEB ? (uint8_t)(v | bit) : (uint8_t)(v & ~bit));
        }
        break;
    }
    case O_BBS: case O_BBC: {
        const uint8_t bit = (uint8_t)(1u << (code >> 5));
        v = mode == M_BBA ? c->a : rd(c, fetch(c));
        const int8_t rel = (int8_t)fetch(c);
        branch(c, rel, ((v & bit) != 0) == (o->op == O_BBS), &cycles);
        break;
    }

    /* Branches and jumps. */
    case O_BPL: branch(c, (int8_t)fetch(c), !(c->p & MC_740_N), &cycles); break;
    case O_BMI: branch(c, (int8_t)fetch(c), (c->p & MC_740_N) != 0, &cycles); break;
    case O_BVC: branch(c, (int8_t)fetch(c), !(c->p & MC_740_V), &cycles); break;
    case O_BVS: branch(c, (int8_t)fetch(c), (c->p & MC_740_V) != 0, &cycles); break;
    case O_BCC: branch(c, (int8_t)fetch(c), !(c->p & MC_740_C), &cycles); break;
    case O_BCS: branch(c, (int8_t)fetch(c), (c->p & MC_740_C) != 0, &cycles); break;
    case O_BNE: branch(c, (int8_t)fetch(c), !(c->p & MC_740_Z), &cycles); break;
    case O_BEQ: branch(c, (int8_t)fetch(c), (c->p & MC_740_Z) != 0, &cycles); break;
    case O_BRA: { const int8_t rel = (int8_t)fetch(c); c->pc = (uint16_t)(c->pc + rel); break; }
    case O_JMP:
    case O_JSR: {
        uint16_t target;
        if (mode == M_ABS) target = address(c, M_ABS);
        else if (mode == M_IND) {
            ea = address(c, M_ABS);
            target = (uint16_t)(rd(c, ea) | rd(c, (uint16_t)(ea + 1)) << 8);
        } else if (mode == M_ZIND) {
            const uint8_t z = fetch(c);
            target = (uint16_t)(rd(c, z) | rd(c, (uint8_t)(z + 1)) << 8);
        } else target = (uint16_t)(0xFF00u | fetch(c));
        if (o->op == O_JSR) {
            /* The address of the instruction's last byte (RTS adds 1). */
            const uint16_t ret = (uint16_t)(c->pc - 1);
            push(c, (uint8_t)(ret >> 8));
            push(c, (uint8_t)ret);
        }
        c->pc = target;
        break;
    }
    case O_RTS: {
        const uint8_t lo = pull(c);
        c->pc = (uint16_t)((lo | pull(c) << 8) + 1);
        break;
    }
    case O_RTI: {
        c->p = pull(c);
        const uint8_t lo = pull(c);
        c->pc = (uint16_t)(lo | pull(c) << 8);
        break;
    }
    case O_BRK: {
        const uint16_t ret = (uint16_t)(c->pc + 1);
        push(c, (uint8_t)(ret >> 8));
        push(c, (uint8_t)ret);
        c->p |= MC_740_B;
        push(c, c->p);
        c->p |= MC_740_I;
        c->pc = (uint16_t)(rd(c, 0xFFF4) | rd(c, 0xFFF5) << 8);
        break;
    }

    /* Stack, transfers, flags. */
    case O_PHA: push(c, c->a); break;
    case O_PHP: push(c, c->p); break;
    case O_PLA: c->a = pull(c); nz(c, c->a); break;
    case O_PLP: c->p = pull(c); break;
    case O_TAX: c->x = c->a; nz(c, c->x); break;
    case O_TXA: c->a = c->x; nz(c, c->a); break;
    case O_TAY: c->y = c->a; nz(c, c->y); break;
    case O_TYA: c->a = c->y; nz(c, c->a); break;
    case O_TSX: c->x = c->s; nz(c, c->x); break;
    case O_TXS: c->s = c->x; break;
    case O_INX: c->x++; nz(c, c->x); break;
    case O_DEX: c->x--; nz(c, c->x); break;
    case O_INY: c->y++; nz(c, c->y); break;
    case O_DEY: c->y--; nz(c, c->y); break;
    case O_CLC: c->p &= (uint8_t)~MC_740_C; break;
    case O_SEC: c->p |= MC_740_C; break;
    case O_CLI: c->p &= (uint8_t)~MC_740_I; break;
    case O_SEI: c->p |= MC_740_I; break;
    case O_CLV: c->p &= (uint8_t)~MC_740_V; break;
    case O_CLD: c->p &= (uint8_t)~MC_740_D; break;
    case O_SED: c->p |= MC_740_D; break;
    case O_CLT: c->p &= (uint8_t)~MC_740_T; break;
    case O_SET: c->p |= MC_740_T; break;
    case O_NOP: break;
    case O_WIT: c->waiting = 1; break;
    case O_STP: c->stopped = 1; break;

    /* Appendix 2: MUL M(S),A <- A x M(zz+X), S - 1; DIV A <- (M(zz+X+1),
     * M(zz+X)) / A, M(S) <- one's complement of the remainder, S - 1. */
    case O_MUL:
    case O_DIV:
        if (!c->muldiv) {
            c->pc++;
            c->illegal++;
            cycles = 2;
            break;
        }
        ea = fetch(c);
        if (o->op == O_MUL) {
            const unsigned product = (unsigned)c->a * rd(c, (uint8_t)(ea + c->x));
            wr(c, (uint16_t)(c->stack_page | c->s), (uint8_t)(product >> 8));
            c->a = (uint8_t)product;
        } else {
            const unsigned dividend = rd(c, (uint8_t)(ea + c->x)) | (unsigned)rd(c, (uint8_t)(ea + c->x + 1)) << 8;
            const unsigned divisor = c->a ? c->a : 1u;   /* division by zero: undocumented† */
            wr(c, (uint16_t)(c->stack_page | c->s), (uint8_t)~(dividend % divisor));
            c->a = (uint8_t)(dividend / divisor);
        }
        c->s--;
        break;

    default:
        c->illegal++;
        break;
    }
    c->cycles += cycles;
    return cycles;
}

/* --- disassembly ------------------------------------------------------------------- */

unsigned mc_cpu740_disasm(const mc_cpu740 *c, uint16_t pc, char *out, unsigned capacity) {
    if (!table_ready) build_table();
    mc_cpu740 *mc = (mc_cpu740 *)c;
    const uint8_t code = mc->read(mc->ctx, pc);
    const op740 *o = &TABLE[code];
    uint8_t b[3] = {code, mc->read(mc->ctx, (uint16_t)(pc + 1)), mc->read(mc->ctx, (uint16_t)(pc + 2))};
    const char *n = NAMES[o->op];
    const unsigned bit = code >> 5;
    const uint16_t abs = (uint16_t)(b[1] | b[2] << 8);
    unsigned len = 1;
    switch (o->mode) {
    case M_IMP: snprintf(out, capacity, "%s", n); break;
    case M_ACC:
        if (o->op == O_SEB || o->op == O_CLB) snprintf(out, capacity, "%s %u,A", n, bit);
        else snprintf(out, capacity, "%s A", n);
        break;
    case M_IMM: snprintf(out, capacity, "%s #$%02X", n, b[1]); len = 2; break;
    case M_ZP:
        if (o->op == O_SEB || o->op == O_CLB) snprintf(out, capacity, "%s %u,$%02X", n, bit, b[1]);
        else snprintf(out, capacity, "%s $%02X", n, b[1]);
        len = 2;
        break;
    case M_ZPX: snprintf(out, capacity, "%s $%02X,X", n, b[1]); len = 2; break;
    case M_ZPY: snprintf(out, capacity, "%s $%02X,Y", n, b[1]); len = 2; break;
    case M_ABS: snprintf(out, capacity, "%s $%04X", n, abs); len = 3; break;
    case M_ABX: snprintf(out, capacity, "%s $%04X,X", n, abs); len = 3; break;
    case M_ABY: snprintf(out, capacity, "%s $%04X,Y", n, abs); len = 3; break;
    case M_IZX: snprintf(out, capacity, "%s ($%02X,X)", n, b[1]); len = 2; break;
    case M_IZY: snprintf(out, capacity, "%s ($%02X),Y", n, b[1]); len = 2; break;
    case M_IND: snprintf(out, capacity, "%s ($%04X)", n, abs); len = 3; break;
    case M_ZIND: snprintf(out, capacity, "%s ($%02X)", n, b[1]); len = 2; break;
    case M_SP: snprintf(out, capacity, "%s \\$FF%02X", n, b[1]); len = 2; break;
    case M_REL: snprintf(out, capacity, "%s $%04X", n, (uint16_t)(pc + 2 + (int8_t)b[1])); len = 2; break;
    case M_BBA: snprintf(out, capacity, "%s %u,A,$%04X", n, bit, (uint16_t)(pc + 2 + (int8_t)b[1])); len = 2; break;
    case M_BBZ: snprintf(out, capacity, "%s %u,$%02X,$%04X", n, bit, b[1], (uint16_t)(pc + 3 + (int8_t)b[2])); len = 3; break;
    case M_LDM: snprintf(out, capacity, "%s #$%02X,$%02X", n, b[1], b[2]); len = 3; break;
    default: snprintf(out, capacity, "???"); break;
    }
    if (o->op == O_MUL || o->op == O_DIV) len = 2;
    return len;
}
