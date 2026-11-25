/* The instruction handlers and the encoding patterns they are decoded from.
 *
 * Sources: M68000 Family Programmer's Reference Manual (PRM): section 4
 * (integer instructions: operation, condition codes, encodings), section 6
 * (supervisor instructions: ANDI/EORI/ORI to SR, MOVE to/from SR, MOVE USP,
 * MOVEC, MOVES, RESET, RTE, STOP, CINV, CPUSH, PFLUSH, PTEST), section 8
 * (instruction format summary and the operation code map); MC68040 User's
 * Manual section 5 (MOVE16) and section 8 (which exceptions use which frame).
 * Each handler's comment names the instruction; its PRM page gives the
 * operation and the condition codes written here.
 *
 * Commit order (restartable faults, m68k.h): handlers read every operand
 * before they change a data register, the SR or a control register; address
 * registers stepped by (An)+/-(An) are undone by the run loop. Writes never
 * abort, so results may be written to memory at any point. */
#include "m68k_internal.h"

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* SIZED and ARITH make every size, whether or not the table uses it */
#endif

void m68k_op_rte(m68k_cpu *cpu, uint32_t op);
void m68k_op_stop(m68k_cpu *cpu, uint32_t op);
void m68k_op_reset(m68k_cpu *cpu, uint32_t op);
void m68k_op_movec(m68k_cpu *cpu, uint32_t op);
void m68k_write_sr_insn(m68k_cpu *cpu, uint32_t sr);

#define SIZED(fn)                                                         \
    static void fn##_b(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 1); } \
    static void fn##_w(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 2); } \
    static void fn##_l(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 4); }
#define SIZED_WL(fn)                                                      \
    static void fn##_w(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 2); } \
    static void fn##_l(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 4); }

enum { K_OR, K_AND, K_EOR, K_ADD, K_SUB, K_CMP };

M68K_INLINE uint32_t alu(m68k_cpu *cpu, int k, uint32_t s, uint32_t d, unsigned sz) {
    uint32_t r;
    switch (k) {
    case K_OR: r = (s | d) & sz_mask(sz); flags_logic(cpu, r, sz); return r;
    case K_AND: r = (s & d) & sz_mask(sz); flags_logic(cpu, r, sz); return r;
    case K_EOR: r = (s ^ d) & sz_mask(sz); flags_logic(cpu, r, sz); return r;
    case K_ADD: return alu_add(cpu, s, d, sz);
    case K_SUB: return alu_sub(cpu, s, d, sz);
    default: alu_cmp(cpu, s, d, sz); return d;
    }
}

/* Read-modify-write of a data-alterable destination. */
M68K_INLINE void rmw(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz, int k, uint32_t s) {
    if (mode == 0) {
        set_dn(cpu, reg, alu(cpu, k, s, D(reg) & sz_mask(sz), sz), sz);
    } else {
        const uint32_t a = ea_addr(cpu, mode, reg, sz);
        const uint32_t r = alu(cpu, k, s, rd(cpu, a, sz), sz);
        wr(cpu, a, sz, r);
    }
}

static void illegal(m68k_cpu *cpu, uint32_t op) { (void)op; m68k_exception_illegal(cpu); }

/* ===== line 0: immediate, bit, MOVEP, CAS, CAS2, CMP2/CHK2, MOVES ========= */

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI #<data>,<ea> */
M68K_INLINE void imm_op(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    const uint32_t s = imm(cpu, sz);
    if (k == K_CMP) alu_cmp(cpu, s, ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz);
    else rmw(cpu, OP_MODE(op), OP_REG(op), sz, k, s);
}
#define IMM_OP(name, k) \
    M68K_INLINE void name(m68k_cpu *cpu, uint32_t op, unsigned sz) { imm_op(cpu, op, sz, k); } SIZED(name)
IMM_OP(op_ori, K_OR)
IMM_OP(op_andi, K_AND)
IMM_OP(op_subi, K_SUB)
IMM_OP(op_addi, K_ADD)
IMM_OP(op_eori, K_EOR)
IMM_OP(op_cmpi, K_CMP)

/* ORI/ANDI/EORI to CCR, to SR (privileged) */
static void op_ori_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_set(cpu, ccr_of(cpu) | (fetch16(cpu) & 0x1Fu)); }
static void op_andi_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_set(cpu, ccr_of(cpu) & fetch16(cpu)); }
static void op_eori_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_set(cpu, (ccr_of(cpu) ^ fetch16(cpu)) & 0x1Fu); }
static void op_ori_sr(m68k_cpu *cpu, uint32_t op) { (void)op; PRIVILEGED(); m68k_write_sr_insn(cpu, m68k_sr(cpu) | fetch16(cpu)); }
static void op_andi_sr(m68k_cpu *cpu, uint32_t op) { (void)op; PRIVILEGED(); m68k_write_sr_insn(cpu, m68k_sr(cpu) & fetch16(cpu)); }
static void op_eori_sr(m68k_cpu *cpu, uint32_t op) { (void)op; PRIVILEGED(); m68k_write_sr_insn(cpu, m68k_sr(cpu) ^ fetch16(cpu)); }

/* BTST, BCHG, BCLR, BSET: on Dn the bit number is modulo 32 (long), in
 * memory modulo 8 (byte). Z = the bit's complement. */
M68K_INLINE void bit_op(m68k_cpu *cpu, uint32_t op, uint32_t bit, int kind) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        const uint32_t m = 1u << (bit & 31u);
        cpu->fz = D(reg) & m;
        if (kind == 1) D(reg) ^= m;
        else if (kind == 2) D(reg) &= ~m;
        else if (kind == 3) D(reg) |= m;
        return;
    }
    const uint32_t m = 1u << (bit & 7u);
    if (kind == 0) {
        cpu->fz = ea_read(cpu, mode, reg, 1) & m;
        return;
    }
    const uint32_t a = ea_addr(cpu, mode, reg, 1);
    const uint32_t v = rd8(cpu, a);
    cpu->fz = v & m;
    wr8(cpu, a, kind == 1 ? v ^ m : kind == 2 ? v & ~m : v | m);
}
static void op_btst_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, fetch16(cpu) & 0xFFu, 0); }
static void op_bchg_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, fetch16(cpu) & 0xFFu, 1); }
static void op_bclr_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, fetch16(cpu) & 0xFFu, 2); }
static void op_bset_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, fetch16(cpu) & 0xFFu, 3); }
static void op_btst_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 0); }
static void op_bchg_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 1); }
static void op_bclr_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 2); }
static void op_bset_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 3); }

/* MOVEP: alternate bytes at (d16,An), high byte first. */
static void op_movep(m68k_cpu *cpu, uint32_t op) {
    const unsigned dn = OP_REG9(op), opmode = (op >> 6) & 7u;
    const uint32_t a = A(OP_REG(op)) + (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    cpu->restart_writes = 1;
    switch (opmode) {
    case 4: { const uint32_t v = (rd8(cpu, a) << 8) | rd8(cpu, a + 2); set_dn(cpu, dn, v, 2); break; }
    case 5: {
        const uint32_t v = (rd8(cpu, a) << 24) | (rd8(cpu, a + 2) << 16) | (rd8(cpu, a + 4) << 8) | rd8(cpu, a + 6);
        D(dn) = v;
        break;
    }
    case 6: wr8(cpu, a, D(dn) >> 8); wr8(cpu, a + 2, D(dn)); break;
    default:
        wr8(cpu, a, D(dn) >> 24); wr8(cpu, a + 2, D(dn) >> 16); wr8(cpu, a + 4, D(dn) >> 8); wr8(cpu, a + 6, D(dn));
        break;
    }
    cpu->restart_writes = 0;
}

/* CAS Dc,Du,<ea>: compare the operand with Dc; equal: Du to the operand,
 * else the operand to Dc. Size in bits 10-9: 01 byte, 10 word, 11 long. */
static void op_cas(m68k_cpu *cpu, uint32_t op) {
    const unsigned sz = ((op >> 9) & 3u) == 1 ? 1u : ((op >> 9) & 3u) == 2 ? 2u : 4u;
    const uint32_t ext = fetch16(cpu);
    const unsigned dc = ext & 7u, du = (ext >> 6) & 7u;
    const uint32_t a = ea_addr(cpu, OP_MODE(op), OP_REG(op), sz);
    const uint32_t v = rd_locked(cpu, a, sz);
    alu_cmp(cpu, D(dc) & sz_mask(sz), v, sz);
    if (cpu->fz == 0) wr(cpu, a, sz, D(du));
    else set_dn(cpu, dc, v, sz);
}

/* CAS2 Dc1:Dc2,Du1:Du2,(Rn1):(Rn2) */
static void op_cas2(m68k_cpu *cpu, uint32_t op) {
    const unsigned sz = (op & 0x0200u) ? 4u : 2u;
    const uint32_t e1 = fetch16(cpu), e2 = fetch16(cpu);
    const uint32_t a1 = cpu->r[e1 >> 12], a2 = cpu->r[e2 >> 12];
    const unsigned dc1 = e1 & 7u, du1 = (e1 >> 6) & 7u, dc2 = e2 & 7u, du2 = (e2 >> 6) & 7u;
    const uint32_t m1 = rd_locked(cpu, a1, sz), m2 = rd_locked(cpu, a2, sz);
    alu_cmp(cpu, D(dc1) & sz_mask(sz), m1, sz);
    if (cpu->fz == 0) {
        alu_cmp(cpu, D(dc2) & sz_mask(sz), m2, sz);
        if (cpu->fz == 0) {
            wr(cpu, a1, sz, D(du1));
            wr(cpu, a2, sz, D(du2));
            return;
        }
    }
    cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_FAIL);
    set_dn(cpu, dc1, m1, sz);
    set_dn(cpu, dc2, m2, sz);
}

/* CMP2/CHK2 <ea>,Rn: bounds pair at <ea> (lower, upper). For An the bounds
 * are sign-extended and compared with all 32 bits; for Dn at the operand
 * size. Z: Rn equals a bound; C: Rn out of bounds, where a lower bound above
 * the upper one (unsigned) describes a range wrapping through zero, which is
 * how signed bounds compare (PRM, CMP2: "the arithmetically smaller value
 * should be used as the lower bound" for signed comparisons). N, V
 * undefined: left unchanged. CHK2 traps (vector 6, format $2) when C. */
static void op_cmp2(m68k_cpu *cpu, uint32_t op) {
    const unsigned code = (op >> 9) & 3u;
    const unsigned sz = code == 0 ? 1u : code == 1 ? 2u : 4u;
    const uint32_t ext = fetch16(cpu);
    const uint32_t a = ea_addr(cpu, OP_MODE(op), OP_REG(op), sz);
    uint32_t lo = rd(cpu, a, sz), hi = rd(cpu, a + sz, sz), v;
    if (ext & 0x8000u) {
        lo = sz_sext(lo, sz);
        hi = sz_sext(hi, sz);
        v = cpu->r[ext >> 12];
    } else {
        v = cpu->r[ext >> 12] & sz_mask(sz);
    }
    const int out = lo <= hi ? (v < lo || v > hi) : (v < lo && v > hi);
    cpu->fz = !(v == lo || v == hi);
    cpu->fc = (uint32_t)out;
    const int trap = out && (ext & 0x0800u);
    const unsigned flags = ((ext & 0x0800u) ? M68K_TQ_CHK2 : 0u) | ((ext & 0x8000u) ? M68K_TQ_AN : 0u) |
                           (lo > hi ? M68K_TQ_WRAP : 0u) | (trap ? M68K_TQ_TRAP : 0u);
    if (flags) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, flags);
    if (trap) m68k_exception_fmt2(cpu, VEC_CHK, cpu->pc, cpu->ipc);
}

/* MOVES Rn,<ea> / <ea>,Rn (privileged): the access uses DFC / SFC, through
 * the MMU as that function code is translated (m68k_moves_read). */
static void op_moves(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    const unsigned sz = ((op >> 6) & 3u) == 0 ? 1u : ((op >> 6) & 3u) == 1 ? 2u : 4u;
    const uint32_t ext = fetch16(cpu);
    const unsigned rn = ext >> 12;
    cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, ((ext & 0x0800u) ? M68K_TQ_WRITE : 0u) | (rn >= 8 ? M68K_TQ_AN : 0u));
    if (ext & 0x0800u) {
        const uint32_t v = cpu->r[rn];
        const uint32_t a = ea_addr(cpu, OP_MODE(op), OP_REG(op), sz);
        m68k_moves_write(cpu, a, sz, cpu->dfc, v & sz_mask(sz));
    } else {
        const uint32_t a = ea_addr(cpu, OP_MODE(op), OP_REG(op), sz);
        const uint32_t v = m68k_moves_read(cpu, a, sz, cpu->sfc);
        if (rn >= 8) cpu->r[rn] = sz_sext(v, sz);
        else set_dn(cpu, rn, v, sz);
    }
}

/* CALLM, RTM (68020 only; PRM section 4, MC68020 UM appendix D.1-D.2):
 * module calls through a module descriptor (Opt, Type, access level; entry
 * word pointer; data area pointer; stack pointer) and the 24-byte module
 * stack frame: Opt/Type/saved access level, CCR, argument count, reserved,
 * the descriptor pointer, the saved PC, the saved module data area pointer
 * and the saved stack pointer. Opt 000 (arguments on the stack below the
 * frame) and 100 (through the saved stack pointer) and Type $00 are
 * executed; any other Opt or Type is a format error (D.1.1, 6.3.6). Type
 * $01 asks the access-level hardware (CPU space, A19-A16 = 0001) for the
 * change: with no MC68851 the cycle bus-errors, and Livadix's 68851 has the
 * access-level mechanism disabled (AC = 0, so the access status reads $00,
 * MC68851 UM 6.1.7.1): either way a format error, nothing changed (D.2.1).
 * The frame's saved access level is 0 for Type $00 (no hardware is asked)
 * and its saved stack pointer the SP at the start of the CALLM. */
#define MODULE_FRAME 24u
static int module_ok(uint32_t control) {
    const unsigned opt = control >> 13, type = (control >> 8) & 0x1Fu;
    return (opt == 0 || opt == 4) && type == 0;
}
static void op_callm(m68k_cpu *cpu, uint32_t op) {
    const uint32_t args = fetch16(cpu) & 0xFFu;
    const uint32_t d = ea_addr(cpu, OP_MODE(op), OP_REG(op), 4);
    const uint32_t control = rd32(cpu, d) >> 16;
    if (!module_ok(control)) {
        m68k_exception_restart(cpu, VEC_FORMAT);
        return;
    }
    const uint32_t entry = rd32(cpu, d + 4), data = rd32(cpu, d + 8);
    /* The entry word names the register for the data area pointer; the
     * module's first instruction follows it (D.1.1, Figure D-2). */
    const uint8_t *p = cpu->pmap[entry >> M68K_PAGE_SHIFT];
    const uint32_t word = p && (entry & (M68K_PAGE_SIZE - 1)) != M68K_PAGE_SIZE - 1
                              ? ld16(p + (entry & (M68K_PAGE_SIZE - 1)))
                              : m68k_slow_read(cpu, entry, 2, cpu->fc_prog);
    const unsigned rn = word >> 12;
    const uint32_t sp = A(7), frame = sp - MODULE_FRAME;
    cpu->restart_writes = 1;
    wr16(cpu, frame, control & 0xFF00u);
    wr16(cpu, frame + 2, ccr_of(cpu));
    wr16(cpu, frame + 4, args);
    wr16(cpu, frame + 6, 0);
    wr32(cpu, frame + 8, d);
    wr32(cpu, frame + 12, cpu->pc);
    wr32(cpu, frame + 16, cpu->r[rn]);
    wr32(cpu, frame + 20, sp);
    cpu->restart_writes = 0;
    cpu->r[rn] = data;
    A(7) = frame;           /* A7 named by the entry word ends as the stack pointer */
    jump(cpu, entry + 2);
}
static void op_rtm(m68k_cpu *cpu, uint32_t op) {
    const unsigned rn = op & 15u;
    const uint32_t sp = A(7);
    const uint32_t control = rd16(cpu, sp);
    if (!module_ok(control)) {
        m68k_exception_restart(cpu, VEC_FORMAT);
        return;
    }
    const uint32_t ccr = rd16(cpu, sp + 2), args = rd16(cpu, sp + 4) & 0xFFu;
    const uint32_t pc = rd32(cpu, sp + 12), data = rd32(cpu, sp + 16);
    ccr_set(cpu, ccr & 0x1Fu);
    cpu->r[rn] = data;
    A(7) = sp + MODULE_FRAME + args;
    jump(cpu, pc);
}

/* ===== lines 1-3: MOVE, MOVEA ============================================ */

/* The destination's address (and any extension word or pointer it needs)
 * comes before the condition codes change, so a fault there finds them as
 * they were. */
M68K_INLINE void op_move(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t v = ea_read(cpu, OP_MODE(op), OP_REG(op), sz);
    const unsigned dmode = (op >> 6) & 7u;
    if (dmode == 0) {
        flags_logic(cpu, v, sz);
        set_dn(cpu, OP_REG9(op), v, sz);
        return;
    }
    const uint32_t a = ea_addr(cpu, dmode, OP_REG9(op), sz);
    flags_logic(cpu, v, sz);
    wr(cpu, a, sz, v);
}
SIZED(op_move)
M68K_INLINE void op_movea(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    A(OP_REG9(op)) = sz_sext(ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz);
}
SIZED_WL(op_movea)

/* Specialised forms of the commonest moves. */
static void op_move_l_dd(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = D(OP_REG(op));
    flags_logic(cpu, v, 4);
    D(OP_REG9(op)) = v;
}
static void op_move_l_ad(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = A(OP_REG(op));
    flags_logic(cpu, v, 4);
    D(OP_REG9(op)) = v;
}
static void op_move_l_aid(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = rd32(cpu, A(OP_REG(op)));
    flags_logic(cpu, v, 4);
    D(OP_REG9(op)) = v;
}
static void op_movea_l_d(m68k_cpu *cpu, uint32_t op) { A(OP_REG9(op)) = cpu->r[op & 15u]; }

/* ===== line 4 ============================================================= */

/* NEGX, CLR, NEG, NOT */
M68K_INLINE void op_negx(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) { set_dn(cpu, reg, alu_subx(cpu, D(reg) & sz_mask(sz), 0, sz), sz); return; }
    const uint32_t a = ea_addr(cpu, mode, reg, sz);
    wr(cpu, a, sz, alu_subx(cpu, rd(cpu, a, sz), 0, sz));
}
SIZED(op_negx)
M68K_INLINE void op_neg(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) { set_dn(cpu, reg, alu_sub(cpu, D(reg) & sz_mask(sz), 0, sz), sz); return; }
    const uint32_t a = ea_addr(cpu, mode, reg, sz);
    wr(cpu, a, sz, alu_sub(cpu, rd(cpu, a, sz), 0, sz));
}
SIZED(op_neg)
M68K_INLINE void op_not(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        const uint32_t r = ~D(reg) & sz_mask(sz);
        flags_logic(cpu, r, sz);
        set_dn(cpu, reg, r, sz);
        return;
    }
    const uint32_t a = ea_addr(cpu, mode, reg, sz);
    const uint32_t r = ~rd(cpu, a, sz) & sz_mask(sz);
    flags_logic(cpu, r, sz);
    wr(cpu, a, sz, r);
}
SIZED(op_not)
/* CLR writes without reading on the 68020 and later. */
M68K_INLINE void op_clr(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    if (OP_MODE(op) == 0) {
        set_dn(cpu, OP_REG(op), 0, sz);
    } else {
        wr(cpu, ea_addr(cpu, OP_MODE(op), OP_REG(op), sz), sz, 0);
    }
    flags_logic(cpu, 0, sz);
}
SIZED(op_clr)

/* MOVE from SR (privileged on the 68010 and later), from CCR, to CCR, to SR */
static void op_move_from_sr(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    ea_write(cpu, OP_MODE(op), OP_REG(op), 2, m68k_sr(cpu));
}
static void op_move_from_ccr(m68k_cpu *cpu, uint32_t op) { ea_write(cpu, OP_MODE(op), OP_REG(op), 2, ccr_of(cpu)); }
static void op_move_to_ccr(m68k_cpu *cpu, uint32_t op) { ccr_set(cpu, ea_read(cpu, OP_MODE(op), OP_REG(op), 2) & 0x1Fu); }
static void op_move_to_sr(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    m68k_write_sr_insn(cpu, ea_read(cpu, OP_MODE(op), OP_REG(op), 2));
}

/* CHK <ea>,Dn: trap (vector 6, format $2) when Dn < 0 (N set) or Dn >
 * bound (N cleared), signed at the operand size. Z, V, C undefined: Z
 * follows Dn, V and C are cleared. */
M68K_INLINE void op_chk(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const int32_t bound = (int32_t)sz_sext(ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz);
    const int32_t v = (int32_t)sz_sext(D(OP_REG9(op)), sz);
    cpu->fz = (uint32_t)v;
    cpu->fv = 0;
    cpu->fc = 0;
    if (v < 0 || v > bound) {
        cpu->fn = v < 0 ? 0x80000000u : 0;
        cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_TRAP);
        m68k_exception_fmt2(cpu, VEC_CHK, cpu->pc, cpu->ipc);
    }
}
SIZED_WL(op_chk)

static void op_lea(m68k_cpu *cpu, uint32_t op) { A(OP_REG9(op)) = ea_addr(cpu, OP_MODE(op), OP_REG(op), 4); }
static void op_pea(m68k_cpu *cpu, uint32_t op) { push32(cpu, ea_addr(cpu, OP_MODE(op), OP_REG(op), 4)); }

/* ABCD, SBCD, NBCD: packed decimal with X (PRM 4, ABCD/SBCD/NBCD): C and X
 * the decimal carry or borrow, Z cleared when the result is nonzero and
 * otherwise unchanged; N and V undefined: N follows bit 7 of the result, V
 * is cleared. Digits outside 0-9 have no defined result; these adjust each
 * digit as decimal arithmetic does (add or subtract 6 when the digit
 * overflows or borrows), which for invalid digits gives the results and
 * carries the SingleStepTests 68000 data records. */
static uint32_t bcd_add(m68k_cpu *cpu, uint32_t s, uint32_t d) {
    const uint32_t x = cpu->fx & 1u;
    uint32_t r = s + d + x;
    if ((s & 0x0Fu) + (d & 0x0Fu) + x > 9) r += 0x06;
    const uint32_t carry = r > 0x9F;
    if (carry) r += 0x60;
    r &= 0xFFu;
    cpu->fc = cpu->fx = carry;
    cpu->fz |= r;
    cpu->fn = r << 24;
    cpu->fv = 0;
    return r;
}
static uint32_t bcd_sub(m68k_cpu *cpu, uint32_t s, uint32_t d) {
    const uint32_t x = cpu->fx & 1u;
    const int32_t bin = (int32_t)d - (int32_t)s - (int32_t)x;
    int32_t r = bin;
    if ((int32_t)(d & 0x0Fu) - (int32_t)(s & 0x0Fu) - (int32_t)x < 0) r -= 0x06;
    const uint32_t borrow = r < 0;
    if (bin < 0) r -= 0x60;
    const uint32_t res = (uint32_t)r & 0xFFu;
    cpu->fc = cpu->fx = borrow;
    cpu->fz |= res;
    cpu->fn = res << 24;
    cpu->fv = 0;
    return res;
}
static void op_nbcd(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) { set_dn(cpu, reg, bcd_sub(cpu, D(reg) & 0xFFu, 0), 1); return; }
    const uint32_t a = ea_addr(cpu, mode, reg, 1);
    wr8(cpu, a, bcd_sub(cpu, rd8(cpu, a), 0));
}
static void op_abcd_r(m68k_cpu *cpu, uint32_t op) {
    const unsigned rx = OP_REG(op), ry = OP_REG9(op);
    set_dn(cpu, ry, bcd_add(cpu, D(rx) & 0xFFu, D(ry) & 0xFFu), 1);
}
static void op_abcd_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = rd8(cpu, ea_addr(cpu, 4, OP_REG(op), 1));
    const uint32_t a = ea_addr(cpu, 4, OP_REG9(op), 1);
    wr8(cpu, a, bcd_add(cpu, s, rd8(cpu, a)));
}
static void op_sbcd_r(m68k_cpu *cpu, uint32_t op) {
    const unsigned rx = OP_REG(op), ry = OP_REG9(op);
    set_dn(cpu, ry, bcd_sub(cpu, D(rx) & 0xFFu, D(ry) & 0xFFu), 1);
}
static void op_sbcd_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = rd8(cpu, ea_addr(cpu, 4, OP_REG(op), 1));
    const uint32_t a = ea_addr(cpu, 4, OP_REG9(op), 1);
    wr8(cpu, a, bcd_sub(cpu, s, rd8(cpu, a)));
}

/* PACK/UNPK (68020+): no condition codes. */
static void op_pack_r(m68k_cpu *cpu, uint32_t op) {
    const uint32_t adj = fetch16(cpu);
    const uint32_t v = (D(OP_REG(op)) + adj) & 0xFFFFu;
    set_dn(cpu, OP_REG9(op), ((v >> 4) & 0xF0u) | (v & 0x0Fu), 1);
}
static void op_pack_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t adj = fetch16(cpu);
    const uint32_t lo = rd8(cpu, ea_addr(cpu, 4, OP_REG(op), 1));
    const uint32_t hi = rd8(cpu, ea_addr(cpu, 4, OP_REG(op), 1));
    const uint32_t v = ((hi << 8 | lo) + adj) & 0xFFFFu;
    wr8(cpu, ea_addr(cpu, 4, OP_REG9(op), 1), ((v >> 4) & 0xF0u) | (v & 0x0Fu));
}
static void op_unpk_r(m68k_cpu *cpu, uint32_t op) {
    const uint32_t adj = fetch16(cpu);
    const uint32_t s = D(OP_REG(op));
    set_dn(cpu, OP_REG9(op), ((((s & 0xF0u) << 4) | (s & 0x0Fu)) + adj) & 0xFFFFu, 2);
}
static void op_unpk_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t adj = fetch16(cpu);
    const uint32_t s = rd8(cpu, ea_addr(cpu, 4, OP_REG(op), 1));
    const uint32_t v = ((((s & 0xF0u) << 4) | (s & 0x0Fu)) + adj) & 0xFFFFu;
    wr8(cpu, ea_addr(cpu, 4, OP_REG9(op), 1), v);
    wr8(cpu, ea_addr(cpu, 4, OP_REG9(op), 1), v >> 8);
}

/* LINK An,#d: SP - 4 -> SP; An -> (SP); SP -> An; SP + d -> SP. */
static void link(m68k_cpu *cpu, unsigned reg, uint32_t d) {
    const uint32_t sp = A(7) - 4;
    wr32(cpu, sp, reg == 7 ? sp : A(reg));
    A(reg) = sp;
    A(7) = sp + d;
}
static void op_link_w(m68k_cpu *cpu, uint32_t op) { link(cpu, OP_REG(op), (uint32_t)(int32_t)(int16_t)fetch16(cpu)); }
static void op_link_l(m68k_cpu *cpu, uint32_t op) { link(cpu, OP_REG(op), fetch32(cpu)); }
/* UNLK An: An -> SP; (SP) -> An; SP + 4 -> SP. */
static void op_unlk(m68k_cpu *cpu, uint32_t op) {
    const unsigned reg = OP_REG(op);
    const uint32_t a = A(reg);
    const uint32_t v = rd32(cpu, a);
    A(7) = a + 4;
    A(reg) = v;
}

static void op_swap(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = (D(r) << 16) | (D(r) >> 16);
    flags_logic(cpu, D(r), 4);
}
static void op_ext_w(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    const uint32_t v = (uint32_t)(int32_t)(int8_t)D(r) & 0xFFFFu;
    set_dn(cpu, r, v, 2);
    flags_logic(cpu, v, 2);
}
static void op_ext_l(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = (uint32_t)(int32_t)(int16_t)D(r);
    flags_logic(cpu, D(r), 4);
}
static void op_extb_l(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = (uint32_t)(int32_t)(int8_t)D(r);
    flags_logic(cpu, D(r), 4);
}

/* MOVEM <list>,<ea>: ascending addresses from D0 to A7; -(An) takes the
 * list reversed (bit 0 = A7) and stores downwards, and when An is in the
 * list the value stored is An's initial value minus the operand size
 * (PRM, MOVEM: the 68020, 68030 and 68040 rule). */
M68K_INLINE void op_movem_rm(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t mask = fetch16(cpu);
    cpu->cyc_extra += m68k_timing_movem(cpu->tm, (uint16_t)op, mask);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    cpu->restart_writes = 1;
    if (mode == 4) {
        const uint32_t initial = A(reg);
        uint32_t a = initial;
        for (unsigned i = 0; i < 16; i++) {
            if (!(mask & (1u << i))) continue;
            const unsigned r = 15u - i;
            a -= sz;
            wr(cpu, a, sz, r == 8u + reg ? initial - sz : cpu->r[r]);
        }
        A(reg) = a;
        cpu->restart_writes = 0;
        return;
    }
    uint32_t a = ea_addr(cpu, mode, reg, sz);
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        wr(cpu, a, sz, cpu->r[i]);
        a += sz;
    }
    cpu->restart_writes = 0;
}
SIZED_WL(op_movem_rm)
/* MOVEM <ea>,<list>: words are sign-extended into whole registers; with
 * (An)+ the register ends at the address after the last operand, even when
 * it is in the list. Values are gathered first so a fault restarts cleanly. */
M68K_INLINE void op_movem_mr(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t mask = fetch16(cpu);
    cpu->cyc_extra += m68k_timing_movem(cpu->tm, (uint16_t)op, mask);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    uint32_t a = mode == 3 ? A(reg) : ea_addr(cpu, mode, reg, sz);
    uint32_t v[16] = {0};
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        v[i] = sz == 2 ? (uint32_t)(int32_t)(int16_t)rd16(cpu, a) : rd32(cpu, a);
        a += sz;
    }
    for (unsigned i = 0; i < 16; i++)
        if (mask & (1u << i)) cpu->r[i] = v[i];
    if (mode == 3) A(reg) = a;
}
SIZED_WL(op_movem_mr)

M68K_INLINE void op_tst(m68k_cpu *cpu, uint32_t op, unsigned sz) { flags_logic(cpu, ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz); }
SIZED(op_tst)
static void op_tas(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        flags_logic(cpu, D(reg) & 0xFFu, 1);
        D(reg) |= 0x80u;
        return;
    }
    const uint32_t a = ea_addr(cpu, mode, reg, 1);
    const uint32_t v = rd_locked(cpu, a, 1);
    flags_logic(cpu, v, 1);
    wr8(cpu, a, v | 0x80u);
}

/* MULU.L/MULS.L <ea>,Dl or Dh:Dl (68020+). 32-bit result: V on overflow. */
static void op_mull(m68k_cpu *cpu, uint32_t op) {
    const uint32_t ext = fetch16(cpu);
    if (ext & 0x0800u) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_SIGNED);
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), 4);
    const unsigned dl = (ext >> 12) & 7u, dh = ext & 7u;
    uint64_t p;
    int overflow;
    if (ext & 0x0800u) {
        const int64_t sp = (int64_t)(int32_t)s * (int64_t)(int32_t)D(dl);
        p = (uint64_t)sp;
        overflow = sp != (int64_t)(int32_t)sp;
    } else {
        p = (uint64_t)s * D(dl);
        overflow = (p >> 32) != 0;
    }
    cpu->fc = 0;
    if (ext & 0x0400u) {
        D(dl) = (uint32_t)p;
        D(dh) = (uint32_t)(p >> 32);
        cpu->fn = (uint32_t)(p >> 32);
        cpu->fz = (uint32_t)p | (uint32_t)(p >> 32);
        cpu->fv = 0;
    } else {
        D(dl) = (uint32_t)p;
        cpu->fn = (uint32_t)p;
        cpu->fz = (uint32_t)p;
        cpu->fv = overflow ? 0x80000000u : 0;
    }
}

/* Divide by zero: vector 5, format $2; C cleared, N Z V undefined (kept). */
static void divide_by_zero(m68k_cpu *cpu) {
    cpu->fc = 0;
    cpu->cyc_extra += m68k_timing_query(cpu, cpu->op, M68K_TQ_TRAP);
    m68k_exception_fmt2(cpu, VEC_ZERO_DIVIDE, cpu->pc, cpu->ipc);
}
/* Overflow: V set, C cleared, N and Z undefined (kept), destination kept. */
static void divide_overflow(m68k_cpu *cpu) {
    cpu->fv = 0x80000000u;
    cpu->fc = 0;
}

/* DIVU.L/DIVS.L <ea>,Dq / Dr:Dq (68020+): 64-bit dividend when bit 10 is
 * set; with a 32-bit dividend and Dr = Dq only the quotient is kept. */
static void op_divl(m68k_cpu *cpu, uint32_t op) {
    const uint32_t ext = fetch16(cpu);
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), 4);
    const unsigned dq = (ext >> 12) & 7u, dr = ext & 7u;
    if (s == 0) { divide_by_zero(cpu); return; }
    if (ext & 0x0800u) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_SIGNED);
    uint32_t q, r;
    if (ext & 0x0800u) {
        const int64_t divisor = (int32_t)s;
        int64_t dividend;
        if (ext & 0x0400u) dividend = (int64_t)(((uint64_t)D(dr) << 32) | D(dq));
        else dividend = (int32_t)D(dq);
        if (divisor == -1 && dividend == INT64_MIN) { divide_overflow(cpu); return; }
        const int64_t qq = dividend / divisor, rr = dividend % divisor;
        if (qq != (int64_t)(int32_t)qq) { divide_overflow(cpu); return; }
        q = (uint32_t)qq;
        r = (uint32_t)rr;
    } else {
        uint64_t dividend = (ext & 0x0400u) ? ((uint64_t)D(dr) << 32) | D(dq) : D(dq);
        const uint64_t qq = dividend / s, rr = dividend % s;
        if (qq >> 32) { divide_overflow(cpu); return; }
        q = (uint32_t)qq;
        r = (uint32_t)rr;
    }
    if (dr != dq) D(dr) = r;
    D(dq) = q;
    flags_logic(cpu, q, 4);
}

static void op_trap(m68k_cpu *cpu, uint32_t op) { m68k_exception(cpu, VEC_TRAP + (op & 15u), cpu->pc); }
static void op_move_to_usp(m68k_cpu *cpu, uint32_t op) { PRIVILEGED(); cpu->usp = A(OP_REG(op)); }
static void op_move_from_usp(m68k_cpu *cpu, uint32_t op) { PRIVILEGED(); A(OP_REG(op)) = cpu->usp; }
static void op_nop(m68k_cpu *cpu, uint32_t op) { (void)cpu; (void)op; }
static void op_rts(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    const uint32_t pc = rd32(cpu, A(7));
    A(7) += 4;
    jump(cpu, pc);
}
static void op_rtd(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    const uint32_t d = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    const uint32_t pc = rd32(cpu, A(7));
    A(7) += 4 + d;
    jump(cpu, pc);
}
static void op_rtr(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    const uint32_t ccr = rd16(cpu, A(7));
    const uint32_t pc = rd32(cpu, A(7) + 2);
    A(7) += 6;
    ccr_set(cpu, ccr & 0x1Fu);
    jump(cpu, pc);
}
static void op_trapv(m68k_cpu *cpu, uint32_t op) {
    if (cpu->fv >> 31) {
        cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_TRAP);
        m68k_exception_fmt2(cpu, VEC_TRAPCC, cpu->pc, cpu->ipc);
    }
}
static void op_jsr(m68k_cpu *cpu, uint32_t op) {
    const uint32_t target = ea_addr(cpu, OP_MODE(op), OP_REG(op), 4);
    push32(cpu, cpu->pc);
    jump(cpu, target);
}
static void op_jmp(m68k_cpu *cpu, uint32_t op) { jump(cpu, ea_addr(cpu, OP_MODE(op), OP_REG(op), 4)); }
/* BKPT: no breakpoint hardware answers the acknowledge cycle on a
 * Macintosh, so the processor takes the illegal-instruction exception
 * (PRM, BKPT). */
static void op_bkpt(m68k_cpu *cpu, uint32_t op) { illegal(cpu, op); }

/* ===== line 5: ADDQ, SUBQ, Scc, DBcc, TRAPcc ============================= */

M68K_INLINE void op_addq(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t q = ((OP_REG9(op) - 1u) & 7u) + 1u;
    if (OP_MODE(op) == 1) { A(OP_REG(op)) += q; return; }   /* whole register, no flags */
    rmw(cpu, OP_MODE(op), OP_REG(op), sz, K_ADD, q);
}
SIZED(op_addq)
M68K_INLINE void op_subq(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t q = ((OP_REG9(op) - 1u) & 7u) + 1u;
    if (OP_MODE(op) == 1) { A(OP_REG(op)) -= q; return; }
    rmw(cpu, OP_MODE(op), OP_REG(op), sz, K_SUB, q);
}
SIZED(op_subq)
static void op_addq_l_d(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = alu_add(cpu, ((OP_REG9(op) - 1u) & 7u) + 1u, D(r), 4);
}
static void op_subq_l_d(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = alu_sub(cpu, ((OP_REG9(op) - 1u) & 7u) + 1u, D(r), 4);
}

static void op_scc(m68k_cpu *cpu, uint32_t op) {
    ea_write(cpu, OP_MODE(op), OP_REG(op), 1, cond(cpu, op >> 8) ? 0xFFu : 0);
}
static void op_dbcc(m68k_cpu *cpu, uint32_t op) {
    const uint32_t base = cpu->pc;
    const uint32_t disp = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    if (cond(cpu, op >> 8)) return;
    const unsigned r = OP_REG(op);
    const uint32_t w = (D(r) - 1u) & 0xFFFFu;
    D(r) = (D(r) & 0xFFFF0000u) | w;
    /* cc false: the table holds cc true; branching or the counter's end */
    const unsigned t = w != 0xFFFFu ? cpu->tm->dbcc_branch : cpu->tm->dbcc_expired;
    cpu->cyc_extra += (int32_t)t - (int32_t)cpu->ctab[op];
    /* A delay loop (DBcc to itself): its time is the machine's. */
    if (disp == 0xFFFFFFFEu && cpu->bus.dbra) {
        cpu->dbra_runs++;
        cpu->bus.dbra(cpu->bus.ctx, 1, t);
    }
    if (w != 0xFFFFu) jump(cpu, base + disp);
}
static void op_trapcc(m68k_cpu *cpu, uint32_t op) {
    if ((op & 7u) == 2) (void)fetch16(cpu);
    else if ((op & 7u) == 3) (void)fetch32(cpu);
    if (cond(cpu, op >> 8)) {
        cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_TRAP);
        m68k_exception_fmt2(cpu, VEC_TRAPCC, cpu->pc, cpu->ipc);
    }
}

/* ===== line 6: Bcc, BRA, BSR ============================================== */

static void op_bcc(m68k_cpu *cpu, uint32_t op) {
    const uint32_t base = cpu->pc;
    uint32_t disp = (uint32_t)(int32_t)(int8_t)op;
    if (disp == 0) disp = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    else if (disp == 0xFFFFFFFFu) disp = fetch32(cpu);
    const unsigned cc = (op >> 8) & 15u;
    if (cc == 1) {
        push32(cpu, cpu->pc);
        jump(cpu, base + disp);
    } else if (cond(cpu, cc)) {
        jump(cpu, base + disp);
        /* taken: the table holds Bcc not taken (BRA its own) */
        if (cc) cpu->cyc_extra += (int32_t)cpu->tm->bcc_taken - (int32_t)cpu->ctab[op];
    } else if ((disp & 1u) && cpu->model >= M68K_68040) {
        /* The 68040 prefetches both paths: an odd offset takes the address
         * error even when the branch is not taken (MC68040 UM 8.2.2). */
        cpu->pc = base + disp;
    }
}
/* Bcc.B: the common short form, without the extension checks. */
static void op_bcc_s(m68k_cpu *cpu, uint32_t op) {
    if (cond(cpu, op >> 8)) {
        jump(cpu, cpu->pc + (uint32_t)(int32_t)(int8_t)op);
        cpu->cyc_extra += (int32_t)cpu->tm->bcc_taken - (int32_t)cpu->ctab[op];
    } else if (M68K_UNLIKELY(op & 1u) && cpu->model >= M68K_68040) cpu->pc += (uint32_t)(int32_t)(int8_t)op;
}
static void op_bra_s(m68k_cpu *cpu, uint32_t op) { jump(cpu, cpu->pc + (uint32_t)(int32_t)(int8_t)op); }

/* ===== line 7: MOVEQ ====================================================== */

static void op_moveq(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = (uint32_t)(int32_t)(int8_t)op;
    D(OP_REG9(op)) = v;
    flags_logic(cpu, v, 4);
}

/* ===== lines 8, 9, B, C, D: OR, SUB, CMP, EOR, AND, ADD and friends ======= */

/* <op> <ea>,Dn */
M68K_INLINE void ea_to_dn(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), sz);
    const unsigned dn = OP_REG9(op);
    const uint32_t r = alu(cpu, k, s, D(dn) & sz_mask(sz), sz);
    if (k != K_CMP) set_dn(cpu, dn, r, sz);
}
/* <op> Dn,<ea> */
M68K_INLINE void dn_to_ea(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    rmw(cpu, OP_MODE(op), OP_REG(op), sz, k, D(OP_REG9(op)) & sz_mask(sz));
}
#define ARITH(name, k)                                                                      \
    M68K_INLINE void name##_er(m68k_cpu *cpu, uint32_t op, unsigned sz) { ea_to_dn(cpu, op, sz, k); } \
    M68K_INLINE void name##_re(m68k_cpu *cpu, uint32_t op, unsigned sz) { dn_to_ea(cpu, op, sz, k); } \
    SIZED(name##_er) SIZED(name##_re)
ARITH(op_or, K_OR)
ARITH(op_and, K_AND)
ARITH(op_sub, K_SUB)
ARITH(op_add, K_ADD)
ARITH(op_eor, K_EOR)
M68K_INLINE void op_cmp(m68k_cpu *cpu, uint32_t op, unsigned sz) { ea_to_dn(cpu, op, sz, K_CMP); }
SIZED(op_cmp)

/* Register-to-register long forms of the commonest operations. */
static void op_add_l_dd(m68k_cpu *cpu, uint32_t op) { D(OP_REG9(op)) = alu_add(cpu, D(OP_REG(op)), D(OP_REG9(op)), 4); }
static void op_sub_l_dd(m68k_cpu *cpu, uint32_t op) { D(OP_REG9(op)) = alu_sub(cpu, D(OP_REG(op)), D(OP_REG9(op)), 4); }
static void op_cmp_l_dd(m68k_cpu *cpu, uint32_t op) { alu_cmp(cpu, D(OP_REG(op)), D(OP_REG9(op)), 4); }
static void op_and_l_dd(m68k_cpu *cpu, uint32_t op) { const uint32_t r = D(OP_REG9(op)) & D(OP_REG(op)); D(OP_REG9(op)) = r; flags_logic(cpu, r, 4); }
static void op_or_l_dd(m68k_cpu *cpu, uint32_t op) { const uint32_t r = D(OP_REG9(op)) | D(OP_REG(op)); D(OP_REG9(op)) = r; flags_logic(cpu, r, 4); }

/* ADDA, SUBA, CMPA: the source sign-extended to 32 bits; ADDA/SUBA leave
 * the condition codes alone, CMPA compares all 32 bits. */
M68K_INLINE void op_adda(m68k_cpu *cpu, uint32_t op, unsigned sz) { A(OP_REG9(op)) += sz_sext(ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz); }
M68K_INLINE void op_suba(m68k_cpu *cpu, uint32_t op, unsigned sz) { A(OP_REG9(op)) -= sz_sext(ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz); }
M68K_INLINE void op_cmpa(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    alu_cmp(cpu, sz_sext(ea_read(cpu, OP_MODE(op), OP_REG(op), sz), sz), A(OP_REG9(op)), 4);
}
SIZED_WL(op_adda) SIZED_WL(op_suba) SIZED_WL(op_cmpa)

/* ADDX, SUBX: Dy,Dx and -(Ay),-(Ax) */
M68K_INLINE void op_addx_r(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned rx = OP_REG9(op);
    set_dn(cpu, rx, alu_addx(cpu, D(OP_REG(op)) & sz_mask(sz), D(rx) & sz_mask(sz), sz), sz);
}
M68K_INLINE void op_addx_m(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t s = rd(cpu, ea_addr(cpu, 4, OP_REG(op), sz), sz);
    const uint32_t a = ea_addr(cpu, 4, OP_REG9(op), sz);
    wr(cpu, a, sz, alu_addx(cpu, s, rd(cpu, a, sz), sz));
}
M68K_INLINE void op_subx_r(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned rx = OP_REG9(op);
    set_dn(cpu, rx, alu_subx(cpu, D(OP_REG(op)) & sz_mask(sz), D(rx) & sz_mask(sz), sz), sz);
}
M68K_INLINE void op_subx_m(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t s = rd(cpu, ea_addr(cpu, 4, OP_REG(op), sz), sz);
    const uint32_t a = ea_addr(cpu, 4, OP_REG9(op), sz);
    wr(cpu, a, sz, alu_subx(cpu, s, rd(cpu, a, sz), sz));
}
SIZED(op_addx_r) SIZED(op_addx_m) SIZED(op_subx_r) SIZED(op_subx_m)

/* CMPM (Ay)+,(Ax)+ */
M68K_INLINE void op_cmpm(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t s = rd(cpu, ea_addr(cpu, 3, OP_REG(op), sz), sz);
    const uint32_t d = rd(cpu, ea_addr(cpu, 3, OP_REG9(op), sz), sz);
    alu_cmp(cpu, s, d, sz);
}
SIZED(op_cmpm)

/* MULU.W, MULS.W: 16 x 16 -> 32 */
static void op_mulu_w(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    const uint32_t r = (D(dn) & 0xFFFFu) * s;
    D(dn) = r;
    flags_logic(cpu, r, 4);
}
static void op_muls_w(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    const uint32_t r = (uint32_t)((int32_t)(int16_t)D(dn) * (int32_t)(int16_t)s);
    D(dn) = r;
    flags_logic(cpu, r, 4);
}
/* DIVU.W, DIVS.W: 32 / 16 -> 16-bit remainder : 16-bit quotient */
static void op_divu_w(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = ea_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    if (s == 0) { divide_by_zero(cpu); return; }
    const uint32_t q = D(dn) / s, r = D(dn) % s;
    if (q > 0xFFFFu) { divide_overflow(cpu); return; }
    D(dn) = (r << 16) | q;
    flags_logic(cpu, q, 2);
}
static void op_divs_w(m68k_cpu *cpu, uint32_t op) {
    const int32_t s = (int16_t)ea_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    if (s == 0) { divide_by_zero(cpu); return; }
    const int32_t d = (int32_t)D(dn);
    if (d == INT32_MIN && s == -1) { divide_overflow(cpu); return; }
    const int32_t q = d / s, r = d % s;
    if (q < -32768 || q > 32767) { divide_overflow(cpu); return; }
    D(dn) = ((uint32_t)r << 16) | ((uint32_t)q & 0xFFFFu);
    flags_logic(cpu, (uint32_t)q & 0xFFFFu, 2);
}

/* EXG */
static void op_exg_dd(m68k_cpu *cpu, uint32_t op) { const uint32_t t = D(OP_REG9(op)); D(OP_REG9(op)) = D(OP_REG(op)); D(OP_REG(op)) = t; }
static void op_exg_aa(m68k_cpu *cpu, uint32_t op) { const uint32_t t = A(OP_REG9(op)); A(OP_REG9(op)) = A(OP_REG(op)); A(OP_REG(op)) = t; }
static void op_exg_da(m68k_cpu *cpu, uint32_t op) { const uint32_t t = D(OP_REG9(op)); D(OP_REG9(op)) = A(OP_REG(op)); A(OP_REG(op)) = t; }

/* ===== line E: shifts, rotates, bit fields ================================= */

/* AS, LS, ROX, RO (type 0-3) by `cnt` (PRM 4: ASL/ASR, LSL/LSR, ROXL/ROXR,
 * ROL/ROR). A zero count clears C (ROX: C = X) and leaves X. ASL sets V if
 * the most significant bit changed at any time during the shift. */
M68K_INLINE uint32_t shift(m68k_cpu *cpu, unsigned type, int left, uint32_t v, unsigned cnt, unsigned sz) {
    const unsigned bits = sz * 8u, sh = sz_shift(sz);
    const uint32_t mask = sz_mask(sz);
    uint32_t r, c, ov = 0;
    if (cnt == 0) {
        r = v;
        c = type == 2 ? (cpu->fx & 1u) : 0;
    } else {
        switch (type) {
        case 0:
            if (left) {
                if (cnt >= bits) {
                    r = 0;
                    c = cnt == bits ? v & 1u : 0;
                    ov = v != 0;
                } else {
                    r = (v << cnt) & mask;
                    c = (v >> (bits - cnt)) & 1u;
                    const uint32_t top = v >> (bits - 1u - cnt);
                    const uint32_t ones = (uint32_t)((1ull << (cnt + 1u)) - 1u);
                    ov = top != 0 && top != ones;
                }
            } else {
                const uint32_t sign = (v >> (bits - 1u)) & 1u;
                if (cnt >= bits) {
                    r = sign ? mask : 0;
                    c = sign;
                } else {
                    r = (uint32_t)((int32_t)(v << sh) >> (cnt + sh)) & mask;
                    c = (v >> (cnt - 1u)) & 1u;
                }
            }
            cpu->fx = c;
            break;
        case 1:
            if (cnt > bits) {
                r = 0;
                c = 0;
            } else if (left) {
                r = cnt == bits ? 0 : (v << cnt) & mask;
                c = (v >> (bits - cnt)) & 1u;
            } else {
                r = cnt == bits ? 0 : v >> cnt;
                c = (v >> (cnt - 1u)) & 1u;
            }
            cpu->fx = c;
            break;
        case 2: {
            const unsigned w = bits + 1u, n = cnt % w;
            const uint64_t wm = (1ull << w) - 1u;
            uint64_t t = ((uint64_t)(cpu->fx & 1u) << bits) | v;
            if (n) t = left ? ((t << n) | (t >> (w - n))) & wm : ((t >> n) | (t << (w - n))) & wm;
            r = (uint32_t)t & mask;
            c = (uint32_t)(t >> bits) & 1u;
            cpu->fx = c;
            break;
        }
        default: {
            const unsigned n = cnt & (bits - 1u);
            r = n ? (left ? ((v << n) | (v >> (bits - n))) : ((v >> n) | (v << (bits - n)))) & mask : v;
            c = left ? r & 1u : (r >> (bits - 1u)) & 1u;
            break;
        }
        }
    }
    cpu->fn = r << sh;
    cpu->fz = r;
    cpu->fv = ov << 31;
    cpu->fc = c;
    return r;
}
M68K_INLINE void op_shift_r(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned cnt = (op & 0x20u) ? (D(OP_REG9(op)) & 63u) : ((OP_REG9(op) - 1u) & 7u) + 1u;
    const unsigned r = OP_REG(op);
    /* A count above the operand's size takes longer on the 68030 (11.6.12). */
    if (M68K_UNLIKELY(cnt > 8u * sz)) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_BIG);
    set_dn(cpu, r, shift(cpu, (op >> 3) & 3u, (op & 0x100u) != 0, D(r) & sz_mask(sz), cnt, sz), sz);
}
SIZED(op_shift_r)
static void op_shift_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t a = ea_addr(cpu, OP_MODE(op), OP_REG(op), 2);
    wr16(cpu, a, shift(cpu, (op >> 9) & 3u, (op & 0x100u) != 0, rd16(cpu, a), 1, 2));
}

static uint32_t rol32(uint32_t v, unsigned n) { n &= 31u; return n ? (v << n) | (v >> (32u - n)) : v; }
static uint32_t ror32(uint32_t v, unsigned n) { n &= 31u; return n ? (v >> n) | (v << (32u - n)) : v; }

/* A bit field's bytes (1 to 5) as at most two operand accesses: long and
 * byte for five, word and byte for three; so a refused write leaves no more
 * pending writes than the writeback slots hold. */
static uint64_t bf_read(m68k_cpu *cpu, uint32_t a, unsigned n) {
    switch (n) {
    case 1: return rd8(cpu, a);
    case 2: return rd16(cpu, a);
    case 3: return (uint64_t)rd16(cpu, a) << 8 | rd8(cpu, a + 2);
    case 4: return rd32(cpu, a);
    default: return (uint64_t)rd32(cpu, a) << 8 | rd8(cpu, a + 4);
    }
}
static void bf_write(m68k_cpu *cpu, uint32_t a, unsigned n, uint64_t v) {
    switch (n) {
    case 1: wr8(cpu, a, (uint32_t)v & 0xFFu); break;
    case 2: wr16(cpu, a, (uint32_t)v & 0xFFFFu); break;
    case 3: wr16(cpu, a, (uint32_t)(v >> 8) & 0xFFFFu); wr8(cpu, a + 2, (uint32_t)v & 0xFFu); break;
    case 4: wr32(cpu, a, (uint32_t)v); break;
    default: wr32(cpu, a, (uint32_t)(v >> 8)); wr8(cpu, a + 4, (uint32_t)v & 0xFFu); break;
    }
}

/* BFTST, BFEXTU, BFCHG, BFEXTS, BFCLR, BFFFO, BFSET, BFINS (68020+).
 * Offset: immediate 0-31 or Dn (signed); width 1-32 (0 means 32). In Dn
 * the field wraps around bit 0; in memory it starts at <ea> + offset/8
 * (floor) bit offset%8 and spans up to five bytes. N = the field's msb,
 * Z = field zero, V and C cleared, X kept. */
static void op_bitfield(m68k_cpu *cpu, uint32_t op) {
    const uint32_t ext = fetch16(cpu);
    const unsigned kind = (op >> 8) & 7u;
    const int32_t offset = (ext & 0x0800u) ? (int32_t)D((ext >> 6) & 7u) : (int32_t)((ext >> 6) & 31u);
    unsigned width = (ext & 0x0020u) ? (D(ext & 7u) & 31u) : (ext & 31u);
    if (width == 0) width = 32;
    const unsigned dn = (ext >> 12) & 7u;
    const uint32_t ones = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
    const unsigned mode = OP_MODE(op);
    uint32_t field;
    uint64_t data = 0;
    uint32_t addr = 0;
    unsigned nbytes = 0, bitoff = 0;
    if (mode == 0) {
        field = rol32(D(OP_REG(op)), (unsigned)offset) >> (32u - width);
    } else {
        addr = ea_addr(cpu, mode, OP_REG(op), 1) + (uint32_t)(offset >> 3);
        bitoff = (unsigned)offset & 7u;
        nbytes = (bitoff + width + 7u) / 8u;
        data = bf_read(cpu, addr, nbytes) << (64u - 8u * nbytes);
        field = (uint32_t)((data << bitoff) >> (64u - width));
    }
    /* Offset or width in a register; a field across a long word (68040)
     * or five bytes long (68020/030) (m68k_timing.c). */
    {
        const int span = mode != 0 && (cpu->model >= M68K_68040 ? (addr & 3u) * 8u + bitoff + width > 32u : nbytes == 5);
        const unsigned flags = ((ext & 0x0820u) ? M68K_TQ_REGARG : 0u) | (span ? M68K_TQ_SPAN : 0u);
        if (flags) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, flags);
    }
    uint32_t newfield = field;
    uint32_t flagval = field;
    switch (kind) {
    case 1: D(dn) = field; break;                                                       /* BFEXTU */
    case 2: newfield = ~field & ones; break;                                            /* BFCHG */
    case 3: D(dn) = width == 32 ? field : (uint32_t)((int32_t)(field << (32u - width)) >> (32u - width)); break;   /* BFEXTS */
    case 4: newfield = 0; break;                                                        /* BFCLR */
    case 5: {                                                                           /* BFFFO */
        unsigned i = 0;
        while (i < width && !((field >> (width - 1u - i)) & 1u)) i++;
        D(dn) = (uint32_t)offset + i;
        break;
    }
    case 6: newfield = ones; break;                                                     /* BFSET */
    case 7: newfield = D(dn) & ones; flagval = newfield; break;                         /* BFINS */
    default: break;                                                                     /* BFTST */
    }
    cpu->fn = flagval << (32u - width);
    cpu->fz = flagval;
    cpu->fv = 0;
    cpu->fc = 0;
    if (kind == 2 || kind == 4 || kind == 6 || kind == 7) {
        if (mode == 0) {
            const unsigned r = OP_REG(op);
            uint32_t t = rol32(D(r), (unsigned)offset);
            const uint32_t top = width == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> width);
            t = (t & ~top) | (width == 32 ? newfield : newfield << (32u - width));
            D(r) = ror32(t, (unsigned)offset);
        } else {
            const unsigned shiftpos = 64u - bitoff - width;
            const uint64_t fm = (uint64_t)ones << shiftpos;
            data = (data & ~fm) | ((uint64_t)newfield << shiftpos);
            bf_write(cpu, addr, nbytes, data >> (64u - 8u * nbytes));
        }
    }
}

/* ===== line A and line F =================================================== */

static void op_line_a(m68k_cpu *cpu, uint32_t op) { (void)op; m68k_exception_line_a(cpu); }
static void op_line_f(m68k_cpu *cpu, uint32_t op) { (void)op; m68k_exception_line_f(cpu); }

/* CINV, CPUSH (68040, privileged; PRM 6, MC68040 UM 4.4): caches in bits
 * 7-6 (01 data, 10 instruction, 11 both), scope in bits 4-3 (01 line, 10
 * page, 11 all) at the physical address in An. Memory is always coherent
 * here, so invalidating or pushing has no data effect; the
 * operation is passed to the cache hook for decoded-code caches. Scope 00
 * is illegal (PRM, CINV/CPUSH: "causes illegal instruction trap"). */
static void op_cinv(m68k_cpu *cpu, uint32_t op) {
    const unsigned scope = (op >> 3) & 3u, caches = (op >> 6) & 3u;
    if (scope == 0) {
        illegal(cpu, op);
        return;
    }
    PRIVILEGED();
    if (caches && cpu->on_cache) cpu->on_cache(cpu->cache_ctx, caches == 1 ? 1u : caches == 2 ? 2u : 3u, scope, A(OP_REG(op)));
}
/* PFLUSH (68040, privileged; PRM 6): opmode in bits 4-3. */
static void op_pflush(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    m68k_mmu040_pflush(cpu, (op >> 3) & 3u, A(OP_REG(op)));
}
/* PTESTR/PTESTW (An) (68040, privileged; PRM 6): bit 5 set for read. */
static void op_ptest(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    m68k_mmu040_ptest(cpu, A(OP_REG(op)), !(op & 0x20u));
}

/* MOVE16 (68040; MC68040 UM section 5 and PRM): one 16-byte line, both
 * addresses aligned down to 16; post-incremented registers step by 16. */
static void move16(m68k_cpu *cpu, uint32_t src, uint32_t dst) {
    src &= ~15u;
    dst &= ~15u;
    uint32_t v[4];
    for (unsigned i = 0; i < 4; i++) v[i] = rd32(cpu, src + 4u * i);
    cpu->restart_writes = 1;
    for (unsigned i = 0; i < 4; i++) wr32(cpu, dst + 4u * i, v[i]);
    cpu->restart_writes = 0;
}
static void op_move16_pp(m68k_cpu *cpu, uint32_t op) {
    const uint32_t ext = fetch16(cpu);
    const unsigned ax = OP_REG(op), ay = (ext >> 12) & 7u;
    move16(cpu, A(ax), A(ay));
    A(ax) += 16;
    A(ay) += 16;
}
static void op_move16_abs(m68k_cpu *cpu, uint32_t op) {
    const uint32_t abs = fetch32(cpu);
    const unsigned ay = OP_REG(op);
    switch ((op >> 3) & 3u) {
    case 0: move16(cpu, A(ay), abs); A(ay) += 16; break;   /* (Ay)+,(xxx).L */
    case 1: move16(cpu, abs, A(ay)); A(ay) += 16; break;   /* (xxx).L,(Ay)+ */
    case 2: move16(cpu, A(ay), abs); break;                /* (Ay),(xxx).L */
    default: move16(cpu, abs, A(ay)); break;               /* (xxx).L,(Ay) */
    }
}

/* ===== the encoding patterns (PRM section 8, operation code map) =========== */

#define P(mask, match, ea, ea2, models, prio, fn, name) {mask, match, ea, ea2, models, prio, fn, name}
#define S3(mask, match, ea, models, fn, name)                                         \
    P(mask, (match) | 0x0000u, ea, 0, models, 0, fn##_b, name ".B"),                  \
    P(mask, (match) | 0x0040u, ea, 0, models, 0, fn##_w, name ".W"),                  \
    P(mask, (match) | 0x0080u, ea, 0, models, 0, fn##_l, name ".L")
/* Sized forms where a byte operand may not come from An. */
#define S3B(mask, match, ea, models, fn, name)                                        \
    P(mask, (match) | 0x0000u, (ea) & ~EA_AN, 0, models, 0, fn##_b, name ".B"),       \
    P(mask, (match) | 0x0040u, ea, 0, models, 0, fn##_w, name ".W"),                  \
    P(mask, (match) | 0x0080u, ea, 0, models, 0, fn##_l, name ".L")

const m68k_pattern m68k_patterns[] = {
    /* line 0 */
    S3(0xFFC0, 0x0000, EA_DALT, MM_ALL, op_ori, "ORI"),
    P(0xFFFF, 0x003C, 0, 0, MM_ALL, 0, op_ori_ccr, "ORI to CCR"),
    P(0xFFFF, 0x007C, 0, 0, MM_ALL, 0, op_ori_sr, "ORI to SR"),
    S3(0xFFC0, 0x0200, EA_DALT, MM_ALL, op_andi, "ANDI"),
    P(0xFFFF, 0x023C, 0, 0, MM_ALL, 0, op_andi_ccr, "ANDI to CCR"),
    P(0xFFFF, 0x027C, 0, 0, MM_ALL, 0, op_andi_sr, "ANDI to SR"),
    S3(0xFFC0, 0x0400, EA_DALT, MM_ALL, op_subi, "SUBI"),
    S3(0xFFC0, 0x0600, EA_DALT, MM_ALL, op_addi, "ADDI"),
    S3(0xFFC0, 0x0A00, EA_DALT, MM_ALL, op_eori, "EORI"),
    P(0xFFFF, 0x0A3C, 0, 0, MM_ALL, 0, op_eori_ccr, "EORI to CCR"),
    P(0xFFFF, 0x0A7C, 0, 0, MM_ALL, 0, op_eori_sr, "EORI to SR"),
    S3(0xFFC0, 0x0C00, EA_DATA & ~EA_IMM, MM_ALL, op_cmpi, "CMPI"),
    P(0xFFC0, 0x0800, EA_DATA & ~EA_IMM, 0, MM_ALL, 0, op_btst_i, "BTST"),
    P(0xFFC0, 0x0840, EA_DALT, 0, MM_ALL, 0, op_bchg_i, "BCHG"),
    P(0xFFC0, 0x0880, EA_DALT, 0, MM_ALL, 0, op_bclr_i, "BCLR"),
    P(0xFFC0, 0x08C0, EA_DALT, 0, MM_ALL, 0, op_bset_i, "BSET"),
    P(0xF1C0, 0x0100, EA_DATA, 0, MM_ALL, 0, op_btst_r, "BTST"),
    P(0xF1C0, 0x0140, EA_DALT, 0, MM_ALL, 0, op_bchg_r, "BCHG"),
    P(0xF1C0, 0x0180, EA_DALT, 0, MM_ALL, 0, op_bclr_r, "BCLR"),
    P(0xF1C0, 0x01C0, EA_DALT, 0, MM_ALL, 0, op_bset_r, "BSET"),
    P(0xF138, 0x0108, 0, 0, MM_ALL, 0, op_movep, "MOVEP"),
    P(0xFFC0, 0x0AC0, EA_MALT, 0, MM_ALL, 0, op_cas, "CAS.B"),
    P(0xFFC0, 0x0CC0, EA_MALT, 0, MM_ALL, 0, op_cas, "CAS.W"),
    P(0xFFC0, 0x0EC0, EA_MALT, 0, MM_ALL, 0, op_cas, "CAS.L"),
    P(0xFFFF, 0x0CFC, 0, 0, MM_ALL, 0, op_cas2, "CAS2.W"),
    P(0xFFFF, 0x0EFC, 0, 0, MM_ALL, 0, op_cas2, "CAS2.L"),
    P(0xFFC0, 0x00C0, EA_CTRL, 0, MM_ALL, 0, op_cmp2, "CMP2/CHK2.B"),
    P(0xFFC0, 0x02C0, EA_CTRL, 0, MM_ALL, 0, op_cmp2, "CMP2/CHK2.W"),
    P(0xFFC0, 0x04C0, EA_CTRL, 0, MM_ALL, 0, op_cmp2, "CMP2/CHK2.L"),
    P(0xFFC0, 0x0E00, EA_MALT, 0, MM_ALL, 0, op_moves, "MOVES.B"),
    P(0xFFC0, 0x0E40, EA_MALT, 0, MM_ALL, 0, op_moves, "MOVES.W"),
    P(0xFFC0, 0x0E80, EA_MALT, 0, MM_ALL, 0, op_moves, "MOVES.L"),
    P(0xFFF0, 0x06C0, 0, 0, MM_020, 0, op_rtm, "RTM"),
    P(0xFFC0, 0x06C0, EA_CTRL, 0, MM_020, 0, op_callm, "CALLM"),

    /* lines 1-3 */
    P(0xF000, 0x1000, EA_DATA, EA_DALT, MM_ALL, 0, op_move_b, "MOVE.B"),
    P(0xF000, 0x3000, EA_ALL, EA_DALT, MM_ALL, 0, op_move_w, "MOVE.W"),
    P(0xF000, 0x2000, EA_ALL, EA_DALT, MM_ALL, 0, op_move_l, "MOVE.L"),
    P(0xF1C0, 0x3040, EA_ALL, 0, MM_ALL, 0, op_movea_w, "MOVEA.W"),
    P(0xF1C0, 0x2040, EA_ALL, 0, MM_ALL, 0, op_movea_l, "MOVEA.L"),
    P(0xF1F8, 0x2000, 0, 0, MM_ALL, 1, op_move_l_dd, "MOVE.L"),
    P(0xF1F8, 0x2008, 0, 0, MM_ALL, 1, op_move_l_ad, "MOVE.L"),
    P(0xF1F8, 0x2010, 0, 0, MM_ALL, 1, op_move_l_aid, "MOVE.L"),
    P(0xF1F0, 0x2040, 0, 0, MM_ALL, 1, op_movea_l_d, "MOVEA.L"),

    /* line 4 */
    S3(0xFFC0, 0x4000, EA_DALT, MM_ALL, op_negx, "NEGX"),
    P(0xFFC0, 0x40C0, EA_DALT, 0, MM_ALL, 0, op_move_from_sr, "MOVE from SR"),
    P(0xF1C0, 0x4180, EA_DATA, 0, MM_ALL, 0, op_chk_w, "CHK.W"),
    P(0xF1C0, 0x4100, EA_DATA, 0, MM_ALL, 0, op_chk_l, "CHK.L"),
    P(0xF1C0, 0x41C0, EA_CTRL, 0, MM_ALL, 0, op_lea, "LEA"),
    S3(0xFFC0, 0x4200, EA_DALT, MM_ALL, op_clr, "CLR"),
    P(0xFFC0, 0x42C0, EA_DALT, 0, MM_ALL, 0, op_move_from_ccr, "MOVE from CCR"),
    S3(0xFFC0, 0x4400, EA_DALT, MM_ALL, op_neg, "NEG"),
    P(0xFFC0, 0x44C0, EA_DATA, 0, MM_ALL, 0, op_move_to_ccr, "MOVE to CCR"),
    S3(0xFFC0, 0x4600, EA_DALT, MM_ALL, op_not, "NOT"),
    P(0xFFC0, 0x46C0, EA_DATA, 0, MM_ALL, 0, op_move_to_sr, "MOVE to SR"),
    P(0xFFC0, 0x4800, EA_DALT, 0, MM_ALL, 0, op_nbcd, "NBCD"),
    P(0xFFF8, 0x4808, 0, 0, MM_ALL, 0, op_link_l, "LINK.L"),
    P(0xFFF8, 0x4840, 0, 0, MM_ALL, 0, op_swap, "SWAP"),
    P(0xFFF8, 0x4848, 0, 0, MM_ALL, 0, op_bkpt, "BKPT"),
    P(0xFFC0, 0x4840, EA_CTRL, 0, MM_ALL, 0, op_pea, "PEA"),
    P(0xFFF8, 0x4880, 0, 0, MM_ALL, 0, op_ext_w, "EXT.W"),
    P(0xFFF8, 0x48C0, 0, 0, MM_ALL, 0, op_ext_l, "EXT.L"),
    P(0xFFF8, 0x49C0, 0, 0, MM_ALL, 0, op_extb_l, "EXTB.L"),
    P(0xFFC0, 0x4880, EA_CALT | EA_PD, 0, MM_ALL, 0, op_movem_rm_w, "MOVEM.W"),
    P(0xFFC0, 0x48C0, EA_CALT | EA_PD, 0, MM_ALL, 0, op_movem_rm_l, "MOVEM.L"),
    P(0xFFC0, 0x4C80, EA_CTRL | EA_PI, 0, MM_ALL, 0, op_movem_mr_w, "MOVEM.W"),
    P(0xFFC0, 0x4CC0, EA_CTRL | EA_PI, 0, MM_ALL, 0, op_movem_mr_l, "MOVEM.L"),
    S3B(0xFFC0, 0x4A00, EA_ALL, MM_ALL, op_tst, "TST"),
    P(0xFFC0, 0x4AC0, EA_DALT, 0, MM_ALL, 0, op_tas, "TAS"),
    P(0xFFFF, 0x4AFC, 0, 0, MM_ALL, 0, illegal, "ILLEGAL"),
    P(0xFFC0, 0x4C00, EA_DATA, 0, MM_ALL, 0, op_mull, "MULx.L"),
    P(0xFFC0, 0x4C40, EA_DATA, 0, MM_ALL, 0, op_divl, "DIVx.L"),
    P(0xFFF0, 0x4E40, 0, 0, MM_ALL, 0, op_trap, "TRAP"),
    P(0xFFF8, 0x4E50, 0, 0, MM_ALL, 0, op_link_w, "LINK.W"),
    P(0xFFF8, 0x4E58, 0, 0, MM_ALL, 0, op_unlk, "UNLK"),
    P(0xFFF8, 0x4E60, 0, 0, MM_ALL, 0, op_move_to_usp, "MOVE to USP"),
    P(0xFFF8, 0x4E68, 0, 0, MM_ALL, 0, op_move_from_usp, "MOVE from USP"),
    P(0xFFFF, 0x4E70, 0, 0, MM_ALL, 0, m68k_op_reset, "RESET"),
    P(0xFFFF, 0x4E71, 0, 0, MM_ALL, 0, op_nop, "NOP"),
    P(0xFFFF, 0x4E72, 0, 0, MM_ALL, 0, m68k_op_stop, "STOP"),
    P(0xFFFF, 0x4E73, 0, 0, MM_ALL, 0, m68k_op_rte, "RTE"),
    P(0xFFFF, 0x4E74, 0, 0, MM_ALL, 0, op_rtd, "RTD"),
    P(0xFFFF, 0x4E75, 0, 0, MM_ALL, 0, op_rts, "RTS"),
    P(0xFFFF, 0x4E76, 0, 0, MM_ALL, 0, op_trapv, "TRAPV"),
    P(0xFFFF, 0x4E77, 0, 0, MM_ALL, 0, op_rtr, "RTR"),
    P(0xFFFE, 0x4E7A, 0, 0, MM_ALL, 0, m68k_op_movec, "MOVEC"),
    P(0xFFC0, 0x4E80, EA_CTRL, 0, MM_ALL, 0, op_jsr, "JSR"),
    P(0xFFC0, 0x4EC0, EA_CTRL, 0, MM_ALL, 0, op_jmp, "JMP"),

    /* line 5 */
    S3B(0xF1C0, 0x5000, EA_ALT, MM_ALL, op_addq, "ADDQ"),
    S3B(0xF1C0, 0x5100, EA_ALT, MM_ALL, op_subq, "SUBQ"),
    P(0xF1F8, 0x5080, 0, 0, MM_ALL, 1, op_addq_l_d, "ADDQ.L"),
    P(0xF1F8, 0x5180, 0, 0, MM_ALL, 1, op_subq_l_d, "SUBQ.L"),
    P(0xF0C0, 0x50C0, EA_DALT, 0, MM_ALL, 0, op_scc, "Scc"),
    P(0xF0F8, 0x50C8, 0, 0, MM_ALL, 0, op_dbcc, "DBcc"),
    P(0xF0FF, 0x50FA, 0, 0, MM_ALL, 0, op_trapcc, "TRAPcc.W"),
    P(0xF0FF, 0x50FB, 0, 0, MM_ALL, 0, op_trapcc, "TRAPcc.L"),
    P(0xF0FF, 0x50FC, 0, 0, MM_ALL, 0, op_trapcc, "TRAPcc"),

    /* line 6 */
    P(0xF000, 0x6000, 0, 0, MM_ALL, 0, op_bcc, "Bcc"),

    /* line 7 */
    P(0xF100, 0x7000, 0, 0, MM_ALL, 0, op_moveq, "MOVEQ"),

    /* line 8 */
    S3(0xF1C0, 0x8000, EA_DATA, MM_ALL, op_or_er, "OR"),
    S3(0xF1C0, 0x8100, EA_MALT, MM_ALL, op_or_re, "OR"),
    P(0xF1C0, 0x80C0, EA_DATA, 0, MM_ALL, 0, op_divu_w, "DIVU.W"),
    P(0xF1C0, 0x81C0, EA_DATA, 0, MM_ALL, 0, op_divs_w, "DIVS.W"),
    P(0xF1F8, 0x8100, 0, 0, MM_ALL, 0, op_sbcd_r, "SBCD"),
    P(0xF1F8, 0x8108, 0, 0, MM_ALL, 0, op_sbcd_m, "SBCD"),
    P(0xF1F8, 0x8140, 0, 0, MM_ALL, 0, op_pack_r, "PACK"),
    P(0xF1F8, 0x8148, 0, 0, MM_ALL, 0, op_pack_m, "PACK"),
    P(0xF1F8, 0x8180, 0, 0, MM_ALL, 0, op_unpk_r, "UNPK"),
    P(0xF1F8, 0x8188, 0, 0, MM_ALL, 0, op_unpk_m, "UNPK"),
    P(0xF1F8, 0x8080, 0, 0, MM_ALL, 1, op_or_l_dd, "OR.L"),

    /* line 9 */
    S3B(0xF1C0, 0x9000, EA_ALL, MM_ALL, op_sub_er, "SUB"),
    S3(0xF1C0, 0x9100, EA_MALT, MM_ALL, op_sub_re, "SUB"),
    P(0xF1C0, 0x90C0, EA_ALL, 0, MM_ALL, 0, op_suba_w, "SUBA.W"),
    P(0xF1C0, 0x91C0, EA_ALL, 0, MM_ALL, 0, op_suba_l, "SUBA.L"),
    P(0xF1F8, 0x9100, 0, 0, MM_ALL, 0, op_subx_r_b, "SUBX.B"),
    P(0xF1F8, 0x9140, 0, 0, MM_ALL, 0, op_subx_r_w, "SUBX.W"),
    P(0xF1F8, 0x9180, 0, 0, MM_ALL, 0, op_subx_r_l, "SUBX.L"),
    P(0xF1F8, 0x9108, 0, 0, MM_ALL, 0, op_subx_m_b, "SUBX.B"),
    P(0xF1F8, 0x9148, 0, 0, MM_ALL, 0, op_subx_m_w, "SUBX.W"),
    P(0xF1F8, 0x9188, 0, 0, MM_ALL, 0, op_subx_m_l, "SUBX.L"),
    P(0xF1F8, 0x9080, 0, 0, MM_ALL, 1, op_sub_l_dd, "SUB.L"),

    /* line A */
    P(0xF000, 0xA000, 0, 0, MM_ALL, 0, op_line_a, "line A"),

    /* line B */
    S3B(0xF1C0, 0xB000, EA_ALL, MM_ALL, op_cmp, "CMP"),
    P(0xF1C0, 0xB0C0, EA_ALL, 0, MM_ALL, 0, op_cmpa_w, "CMPA.W"),
    P(0xF1C0, 0xB1C0, EA_ALL, 0, MM_ALL, 0, op_cmpa_l, "CMPA.L"),
    S3(0xF1C0, 0xB100, EA_DALT, MM_ALL, op_eor_re, "EOR"),
    P(0xF1F8, 0xB108, 0, 0, MM_ALL, 0, op_cmpm_b, "CMPM.B"),
    P(0xF1F8, 0xB148, 0, 0, MM_ALL, 0, op_cmpm_w, "CMPM.W"),
    P(0xF1F8, 0xB188, 0, 0, MM_ALL, 0, op_cmpm_l, "CMPM.L"),
    P(0xF1F8, 0xB080, 0, 0, MM_ALL, 1, op_cmp_l_dd, "CMP.L"),

    /* line C */
    S3(0xF1C0, 0xC000, EA_DATA, MM_ALL, op_and_er, "AND"),
    S3(0xF1C0, 0xC100, EA_MALT, MM_ALL, op_and_re, "AND"),
    P(0xF1C0, 0xC0C0, EA_DATA, 0, MM_ALL, 0, op_mulu_w, "MULU.W"),
    P(0xF1C0, 0xC1C0, EA_DATA, 0, MM_ALL, 0, op_muls_w, "MULS.W"),
    P(0xF1F8, 0xC100, 0, 0, MM_ALL, 0, op_abcd_r, "ABCD"),
    P(0xF1F8, 0xC108, 0, 0, MM_ALL, 0, op_abcd_m, "ABCD"),
    P(0xF1F8, 0xC140, 0, 0, MM_ALL, 0, op_exg_dd, "EXG"),
    P(0xF1F8, 0xC148, 0, 0, MM_ALL, 0, op_exg_aa, "EXG"),
    P(0xF1F8, 0xC188, 0, 0, MM_ALL, 0, op_exg_da, "EXG"),
    P(0xF1F8, 0xC080, 0, 0, MM_ALL, 1, op_and_l_dd, "AND.L"),

    /* line D */
    S3B(0xF1C0, 0xD000, EA_ALL, MM_ALL, op_add_er, "ADD"),
    S3(0xF1C0, 0xD100, EA_MALT, MM_ALL, op_add_re, "ADD"),
    P(0xF1C0, 0xD0C0, EA_ALL, 0, MM_ALL, 0, op_adda_w, "ADDA.W"),
    P(0xF1C0, 0xD1C0, EA_ALL, 0, MM_ALL, 0, op_adda_l, "ADDA.L"),
    P(0xF1F8, 0xD100, 0, 0, MM_ALL, 0, op_addx_r_b, "ADDX.B"),
    P(0xF1F8, 0xD140, 0, 0, MM_ALL, 0, op_addx_r_w, "ADDX.W"),
    P(0xF1F8, 0xD180, 0, 0, MM_ALL, 0, op_addx_r_l, "ADDX.L"),
    P(0xF1F8, 0xD108, 0, 0, MM_ALL, 0, op_addx_m_b, "ADDX.B"),
    P(0xF1F8, 0xD148, 0, 0, MM_ALL, 0, op_addx_m_w, "ADDX.W"),
    P(0xF1F8, 0xD188, 0, 0, MM_ALL, 0, op_addx_m_l, "ADDX.L"),
    P(0xF1F8, 0xD080, 0, 0, MM_ALL, 1, op_add_l_dd, "ADD.L"),

    /* line E */
    P(0xF0C0, 0xE000, 0, 0, MM_ALL, 0, op_shift_r_b, "shift.B"),
    P(0xF0C0, 0xE040, 0, 0, MM_ALL, 0, op_shift_r_w, "shift.W"),
    P(0xF0C0, 0xE080, 0, 0, MM_ALL, 0, op_shift_r_l, "shift.L"),
    P(0xF8C0, 0xE0C0, EA_MALT, 0, MM_ALL, 0, op_shift_m, "shift mem"),
    P(0xFFC0, 0xE8C0, EA_DN | EA_CTRL, 0, MM_ALL, 0, op_bitfield, "BFTST"),
    P(0xFFC0, 0xE9C0, EA_DN | EA_CTRL, 0, MM_ALL, 0, op_bitfield, "BFEXTU"),
    P(0xFFC0, 0xEAC0, EA_DN | EA_CALT, 0, MM_ALL, 0, op_bitfield, "BFCHG"),
    P(0xFFC0, 0xEBC0, EA_DN | EA_CTRL, 0, MM_ALL, 0, op_bitfield, "BFEXTS"),
    P(0xFFC0, 0xECC0, EA_DN | EA_CALT, 0, MM_ALL, 0, op_bitfield, "BFCLR"),
    P(0xFFC0, 0xEDC0, EA_DN | EA_CTRL, 0, MM_ALL, 0, op_bitfield, "BFFFO"),
    P(0xFFC0, 0xEEC0, EA_DN | EA_CALT, 0, MM_ALL, 0, op_bitfield, "BFSET"),
    P(0xFFC0, 0xEFC0, EA_DN | EA_CALT, 0, MM_ALL, 0, op_bitfield, "BFINS"),

    /* line F: everything not listed is line F (illegal on every model) */
    P(0xF000, 0xF000, 0, 0, MM_ALL, 0, op_line_f, "line F"),
    /* coprocessor 0 (68030 on-chip MMU: PMOVE, PFLUSH, PLOAD, PTEST; the
     * extension word decides, mmu030.c) */
    P(0xFFC0, 0xF000, EA_ALL, 0, MM_030, 1, m68k_op_pmmu030, "PMMU"),
    /* coprocessor 0 beside the 68020: the MC68851 when fitted (general,
     * PScc/PDBcc/PTRAPcc, PBcc, PSAVE, PRESTORE; mmu851.c), else line F */
    P(0xFE00, 0xF000, 0, 0, MM_020, 1, m68k_op_pmmu851, "PMMU"),
    /* coprocessor 1 (FPU): general, conditional and branch types (F200-
     * F2FF), FSAVE and FRESTORE (F300-F37F); fpu.c decodes them, takes line
     * F where no FPU answers and the 68LC040's format $4 frame. */
    P(0xFF00, 0xF200, 0, 0, MM_ALL, 1, m68k_op_fpu, "FPU"),
    P(0xFF80, 0xF300, 0, 0, MM_ALL, 1, m68k_op_fpu, "FSAVE/FRESTORE"),
    /* 68040 */
    P(0xFF20, 0xF400, 0, 0, MM_040S, 1, op_cinv, "CINV"),
    P(0xFF20, 0xF420, 0, 0, MM_040S, 1, op_cinv, "CPUSH"),
    P(0xFFE0, 0xF500, 0, 0, MM_040S, 1, op_pflush, "PFLUSH"),
    P(0xFFD8, 0xF548, 0, 0, MM_040S, 1, op_ptest, "PTEST"),
    P(0xFFF8, 0xF620, 0, 0, MM_040S, 1, op_move16_pp, "MOVE16"),
    P(0xFFE0, 0xF600, 0, 0, MM_040S, 1, op_move16_abs, "MOVE16"),
};
const unsigned m68k_pattern_count = sizeof m68k_patterns / sizeof m68k_patterns[0];

m68k_handler m68k_illegal_handler(void) { return illegal; }

/* Handlers chosen by more than a mask: Bcc and BRA with an 8-bit
 * displacement (neither $00 nor $FF) skip the extension checks. */
m68k_handler m68k_specialise(uint32_t op, m68k_handler h) {
    if (h == op_bcc && (op & 0xFFu) != 0 && (op & 0xFFu) != 0xFFu) {
        const unsigned cc = (op >> 8) & 15u;
        if (cc == 0) return op_bra_s;
        if (cc != 1) return op_bcc_s;
    }
    return h;
}
