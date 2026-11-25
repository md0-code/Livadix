/* The 68000 (and the 68HC000, the same model): its own handlers, sequenced
 * bus cycle by bus cycle.
 *
 * Sources: MC68000 8-/16-/32-Bit Microprocessors User's Manual (UM), ninth
 * edition: section 2 (registers: SR's T S I2-I0 X N Z V C, 24-bit address
 * bus), section 3 (instruction set summary), section 5 (bus operation:
 * function codes, the read-modify-write cycle of TAS, the interrupt
 * acknowledge), section 6 (exception processing: reset, address and bus
 * errors and their 7-word group 0 frame with the access address, the
 * instruction register and R/W, I/N and the function code in its first
 * word; the 3-word frame of every other exception; priorities; trace,
 * illegal and privilege violations, which are not traced), section 8
 * (instruction execution times: the clock periods and read/write cycles of
 * every instruction and effective address, the data-dependent MULU, MULS
 * and shift times, the exception processing times). M68000 Family
 * Programmer's Reference Manual (PRM): sections 2-4 and 6 (operations,
 * condition codes, encodings; the 68000's addressing modes: the brief
 * extension word without scale, no memory indirection; MOVE from SR not
 * privileged; MOVEM to -(An) storing the register's initial value), and
 * section 8 (operation code map).
 *
 * Observation: the order of the bus cycles inside an
 * instruction (which prefetch, read or write comes first, where the
 * internal cycles fall), the program counter a group 0 frame holds, the
 * divide times and the condition codes the manuals leave undefined follow
 * the bus transactions and final states recorded in the SingleStepTests
 * 68000 data (tests/cpu/sst.c, mode --68000), which the manuals'
 * totals bound. Where the data contradicts the manuals, the manuals are
 * followed: ASR by more than the operand size (C and X the sign, the last
 * bit shifted out) and ADDQ.L/SUBQ.L #,An (8 clocks, UM Table 8-6). No
 * other emulator's source was read.
 *
 * The model: at the start of an instruction the opcode (IRD) and the word
 * after it (IRC) are already fetched; `pf` is the address the next
 * prefetch reads. Consuming an extension word takes IRC and refills it
 * (np); the instruction ends with the prefetch that refills the queue for
 * the next one, whose address is then pf - 4. Every bus cycle takes four
 * clock cycles plus the machine's wait states (m68k_bus.wait); internal
 * cycles are added where they fall. A word or long access at an odd
 * address is an address error before its bus cycle; a refused cycle a bus
 * error; both stack the group 0 frame with the PC pf - 4. */
#include "m68k_internal.h"

#define X (cpu->x0)

enum { G0_ADDRESS = 32, G0_BUS, G0_HALT };

void m68k_write_sr_insn(m68k_cpu *cpu, uint32_t sr);

/* --- bus cycles ----------------------------------------------------------- */

static inline void idle(m68k_cpu *cpu, unsigned n) { X.t += n; }

M68K_NORETURN static void group0(m68k_cpu *cpu, int kind, uint32_t a, int read, int program, unsigned fc) {
    X.fa = a;
    X.fstat = (uint16_t)((X.ird & 0xFFE0u) | (read ? 0x10u : 0u) | (program ? 0x08u : 0u) | (fc & 7u));
    m68k_abort(cpu, cpu->in_exception && X.group0 ? G0_HALT : kind);
}

static inline unsigned waits(m68k_cpu *cpu, uint32_t a, unsigned fc, unsigned kind) {
    return cpu->bus.wait ? cpu->bus.wait(cpu->bus.ctx, a & 0xFFFFFFu, fc, kind, cpu->cycles + X.t) : 0;
}

/* One read bus cycle: byte or word. */
static uint32_t bus_rd(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, int program) {
    if (size == 2 && (a & 1u)) group0(cpu, G0_ADDRESS, a, 1, program, fc);
    X.t += waits(cpu, a, fc, 0);
    const uint32_t pa = a & 0xFFFFFFu;
    uint8_t *const *map = cpu->bus.read_map[(fc & 4u) ? 1 : 0];
    const uint8_t *p = map ? map[pa >> M68K_PAGE_SHIFT] : NULL;
    uint32_t v;
    if (p) {
        p += pa & (M68K_PAGE_SIZE - 1);
        v = size == 1 ? p[0] : ld16(p);
    } else if (cpu->bus.read(cpu->bus.ctx, pa, size, fc, &v)) {
        X.t += 4;
        group0(cpu, G0_BUS, a, 1, program, fc);
    }
    X.t += 4;
    return v;
}
static void bus_wr(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    if (size == 2 && (a & 1u)) group0(cpu, G0_ADDRESS, a, 0, 0, fc);
    X.t += waits(cpu, a, fc, 1);
    const uint32_t pa = a & 0xFFFFFFu;
    uint8_t *const *map = cpu->bus.write_map[(fc & 4u) ? 1 : 0];
    uint8_t *p = map ? map[pa >> M68K_PAGE_SHIFT] : NULL;
    if (p) {
        p += pa & (M68K_PAGE_SIZE - 1);
        if (size == 1) p[0] = (uint8_t)v;
        else st16(p, v);
    } else if (cpu->bus.write(cpu->bus.ctx, pa, size, fc, v & sz_mask(size))) {
        X.t += 4;
        group0(cpu, G0_BUS, a, 0, 0, fc);
    }
    X.t += 4;
}
/* A word of the instruction stream without a bus cycle of this
 * instruction (the queue the previous one filled). */
static uint32_t peek16(m68k_cpu *cpu, uint32_t a) {
    const uint32_t pa = a & 0xFFFFFFu;
    uint8_t *const *map = cpu->bus.read_map[cpu->s];
    const uint8_t *p = map ? map[pa >> M68K_PAGE_SHIFT] : NULL;
    uint32_t v = 0;
    if (p) return ld16(p + (pa & (M68K_PAGE_SIZE - 1)));
    if (cpu->bus.read(cpu->bus.ctx, pa & ~1u, 2, cpu->fc_prog, &v)) return 0;
    return v;
}

/* Prefetch: IRC from pf. */
static inline void np(m68k_cpu *cpu) {
    X.irc = (uint16_t)bus_rd(cpu, X.pf, 2, cpu->fc_prog, 1);
    X.pf += 2;
}
/* An extension word: IRC, refilled. */
static inline uint32_t ext16(m68k_cpu *cpu) {
    const uint32_t v = X.irc;
    np(cpu);
    return v;
}
static inline uint32_t ext32(m68k_cpu *cpu) {
    const uint32_t hi = ext16(cpu);
    return (hi << 16) | ext16(cpu);
}
/* The program counter as the instruction sees it: the address of IRC. */
static inline uint32_t pc_irc(m68k_cpu *cpu) { return X.pf - 2; }
/* A jump: the first prefetch at the target (the caller makes the second). */
static inline void jump_to(m68k_cpu *cpu, uint32_t target) {
    X.pf = target;
    np(cpu);
}

static inline uint32_t rdb(m68k_cpu *cpu, uint32_t a) { return bus_rd(cpu, a, 1, cpu->fc_data, 0); }
static inline uint32_t rdw(m68k_cpu *cpu, uint32_t a) { return bus_rd(cpu, a, 2, cpu->fc_data, 0); }
static inline uint32_t rdl(m68k_cpu *cpu, uint32_t a) {
    const uint32_t hi = rdw(cpu, a);
    return (hi << 16) | rdw(cpu, a + 2);
}
static inline void wrb(m68k_cpu *cpu, uint32_t a, uint32_t v) { bus_wr(cpu, a, 1, cpu->fc_data, v & 0xFFu); }
static inline void wrw(m68k_cpu *cpu, uint32_t a, uint32_t v) { bus_wr(cpu, a, 2, cpu->fc_data, v & 0xFFFFu); }
static inline void wrl(m68k_cpu *cpu, uint32_t a, uint32_t v) {
    wrw(cpu, a, v >> 16);
    wrw(cpu, a + 2, v);
}
/* Long word written low word first (MOVE to -(An), the stacking order). */
static inline void wrl_lo(m68k_cpu *cpu, uint32_t a, uint32_t v) {
    wrw(cpu, a + 2, v);
    wrw(cpu, a, v >> 16);
}
static inline uint32_t rd_sz(m68k_cpu *cpu, uint32_t a, unsigned sz) {
    return sz == 1 ? rdb(cpu, a) : sz == 2 ? rdw(cpu, a) : rdl(cpu, a);
}
static inline void wr_sz(m68k_cpu *cpu, uint32_t a, unsigned sz, uint32_t v) {
    if (sz == 1) wrb(cpu, a, v);
    else if (sz == 2) wrw(cpu, a, v);
    else wrl(cpu, a, v);
}

/* --- effective addresses (PRM 2.2; UM Table 8-1) ----------------------------- */

/* The brief extension word: Xn.W or Xn.L plus d8; bits 10-8 ignored. */
static inline uint32_t index_of(m68k_cpu *cpu, uint32_t base, uint32_t ext) {
    uint32_t xn = cpu->r[ext >> 12];
    if (!(ext & 0x0800u)) xn = (uint32_t)(int32_t)(int16_t)xn;
    return base + xn + (uint32_t)(int32_t)(int8_t)ext;
}

/* A memory operand's address, its extension words consumed, the internal
 * cycles of -(An) and the index modes counted. */
static uint32_t ea_calc(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz) {
    switch (mode) {
    case 2: return A(reg);
    case 3: {
        const uint32_t a = A(reg);
        A(reg) = a + ea_step(reg, sz);
        return a;
    }
    case 4:
        idle(cpu, 2);
        A(reg) -= ea_step(reg, sz);
        return A(reg);
    case 5: return A(reg) + (uint32_t)(int32_t)(int16_t)ext16(cpu);
    case 6: idle(cpu, 2); return index_of(cpu, A(reg), ext16(cpu));
    default:
        switch (reg) {
        case 0: return (uint32_t)(int32_t)(int16_t)ext16(cpu);
        case 1: return ext32(cpu);
        case 2: { const uint32_t base = pc_irc(cpu); return base + (uint32_t)(int32_t)(int16_t)ext16(cpu); }
        default: { idle(cpu, 2); const uint32_t base = pc_irc(cpu); return index_of(cpu, base, ext16(cpu)); }
        }
    }
}
/* The function code of an operand access: program-relative operands are
 * read in the data space too, as the SingleStepTests 68000 data record
 * (the Macintosh decodes no function codes). */
static inline unsigned fc_of(m68k_cpu *cpu, unsigned mode, unsigned reg) {
    (void)mode;
    (void)reg;
    return cpu->fc_data;
}
static uint32_t rd_fc(m68k_cpu *cpu, uint32_t a, unsigned sz, unsigned fc) {
    if (sz == 1) return bus_rd(cpu, a, 1, fc, 0);
    if (sz == 2) return bus_rd(cpu, a, 2, fc, 0);
    const uint32_t hi = bus_rd(cpu, a, 2, fc, 0);
    return (hi << 16) | bus_rd(cpu, a + 2, 2, fc, 0);
}
/* A source operand of any mode. */
static uint32_t src_read(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz) {
    if (mode == 0) return D(reg) & sz_mask(sz);
    if (mode == 1) return A(reg) & sz_mask(sz);
    if (mode == 7 && reg == 4) {
        if (sz == 4) return ext32(cpu);
        return ext16(cpu) & sz_mask(sz);
    }
    const uint32_t a = ea_calc(cpu, mode, reg, sz);
    return rd_fc(cpu, a, sz, fc_of(cpu, mode, reg));
}
static inline int is_mem(unsigned mode, unsigned reg) { return mode >= 2 && !(mode == 7 && reg == 4); }

/* --- exceptions (UM section 6) --------------------------------------------- */

static uint16_t sr_now(const m68k_cpu *cpu) { return m68k_sr(cpu); }

static void enter_super(m68k_cpu *cpu, uint16_t old) {
    m68k_set_sr(cpu, (uint16_t)((old | SR_S) & ~SR_T1));
}

/* The vector's handler fetched and its first two words prefetched. */
static void vector_to(m68k_cpu *cpu, unsigned vector) {
    const uint32_t hi = bus_rd(cpu, vector * 4u, 2, M68K_FC_SUPER_DATA, 0);
    const uint32_t handler = (hi << 16) | bus_rd(cpu, vector * 4u + 2u, 2, M68K_FC_SUPER_DATA, 0);
    X.pf = handler;
    np(cpu);
    idle(cpu, 2);
    np(cpu);
}

/* Groups 1 and 2: SR and PC (UM 6.2.5: the short frame). `pre`: the
 * internal cycles before the stacking. */
static void exception(m68k_cpu *cpu, unsigned vector, uint32_t pc, unsigned pre) {
    if (cpu->on_exception) cpu->on_exception(cpu->hook_ctx, cpu, vector, pc);
    const uint16_t old = sr_now(cpu);
    enter_super(cpu, old);
    cpu->in_exception = 1;
    idle(cpu, pre);
    const uint32_t sp = A(7);
    bus_wr(cpu, sp - 2, 2, M68K_FC_SUPER_DATA, pc & 0xFFFFu);
    bus_wr(cpu, sp - 6, 2, M68K_FC_SUPER_DATA, old);
    bus_wr(cpu, sp - 4, 2, M68K_FC_SUPER_DATA, pc >> 16);
    A(7) = sp - 6;
    vector_to(cpu, vector);
    cpu->in_exception = 0;
}

/* Group 0: address and bus errors (UM 6.3.9, 6.3.10): the first word
 * (R/W, I/N, function code over the instruction register's upper bits),
 * the access address, the instruction register, SR and PC. */
static void group0_frame(m68k_cpu *cpu, unsigned vector) {
    const uint32_t pc = X.pf - 4u;
    if (cpu->on_exception) cpu->on_exception(cpu->hook_ctx, cpu, vector, pc);
    const uint16_t old = sr_now(cpu);
    enter_super(cpu, old);
    cpu->in_exception = 1;
    X.group0 = 1;
    idle(cpu, 4);
    const uint32_t sp = A(7);
    bus_wr(cpu, sp - 2, 2, M68K_FC_SUPER_DATA, pc & 0xFFFFu);
    bus_wr(cpu, sp - 6, 2, M68K_FC_SUPER_DATA, old);
    bus_wr(cpu, sp - 4, 2, M68K_FC_SUPER_DATA, pc >> 16);
    bus_wr(cpu, sp - 8, 2, M68K_FC_SUPER_DATA, X.ird);
    bus_wr(cpu, sp - 10, 2, M68K_FC_SUPER_DATA, X.fa & 0xFFFFu);
    bus_wr(cpu, sp - 14, 2, M68K_FC_SUPER_DATA, X.fstat);
    bus_wr(cpu, sp - 12, 2, M68K_FC_SUPER_DATA, X.fa >> 16);
    A(7) = sp - 14;
    vector_to(cpu, vector);
    X.group0 = 0;
    cpu->in_exception = 0;
}

/* Exceptions an instruction raises before it executes: the PC stacked is
 * its own, it is not traced (UM 6.3.3, 6.3.4, 6.3.8). */
static void at_instruction(m68k_cpu *cpu, unsigned vector) {
    X.traced = 0;
    exception(cpu, vector, cpu->ipc, 4);
}
static void illegal(m68k_cpu *cpu, uint32_t op) { (void)op; at_instruction(cpu, VEC_ILLEGAL); }
static void op_line_a(m68k_cpu *cpu, uint32_t op) { (void)op; at_instruction(cpu, VEC_LINE_A); }
static void op_line_f(m68k_cpu *cpu, uint32_t op) { (void)op; at_instruction(cpu, VEC_LINE_F); }
m68k_handler m68k_illegal_handler000(void) { return illegal; }

#define PRIV()                                       \
    do {                                             \
        if (!cpu->s) {                               \
            at_instruction(cpu, VEC_PRIVILEGE);      \
            return;                                  \
        }                                            \
    } while (0)

/* --- the instructions -------------------------------------------------------- */

#define SIZED(fn)                                                         \
    static void fn##_b(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 1); } \
    static void fn##_w(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 2); } \
    static void fn##_l(m68k_cpu *cpu, uint32_t op) { fn(cpu, op, 4); }

enum { K_OR, K_AND, K_EOR, K_ADD, K_SUB, K_CMP };

static inline uint32_t alu(m68k_cpu *cpu, int k, uint32_t s, uint32_t d, unsigned sz) {
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

/* <op> <ea>,Dn: OR, AND, ADD, SUB, CMP. */
static inline void ea_to_dn(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t s = src_read(cpu, mode, reg, sz);
    const unsigned dn = OP_REG9(op);
    np(cpu);
    if (sz == 4) idle(cpu, k == K_CMP ? 2u : is_mem(mode, reg) ? 2u : 4u);
    const uint32_t r = alu(cpu, k, s, D(dn) & sz_mask(sz), sz);
    if (k != K_CMP) set_dn(cpu, dn, r, sz);
}
/* <op> Dn,<ea> (memory) and the immediate forms: read, prefetch, write. */
static inline void rmw_mem(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz, int k, uint32_t s) {
    const uint32_t a = ea_calc(cpu, mode, reg, sz);
    const uint32_t d = rd_sz(cpu, a, sz);
    np(cpu);
    const uint32_t r = alu(cpu, k, s, d, sz);
    if (k == K_CMP) return;
    if (sz == 4) {
        wrw(cpu, a + 2, r);
        wrw(cpu, a, r >> 16);
    } else {
        wr_sz(cpu, a, sz, r);
    }
}
static inline void dn_to_ea(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    rmw_mem(cpu, OP_MODE(op), OP_REG(op), sz, k, D(OP_REG9(op)) & sz_mask(sz));
}
/* EOR Dn,Dn (and the register forms of <op> Dn,<ea> that decode here). */
static inline void eor_op(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    if (OP_MODE(op) == 0) {
        const unsigned r = OP_REG(op);
        np(cpu);
        if (sz == 4) idle(cpu, 4);
        set_dn(cpu, r, alu(cpu, K_EOR, D(OP_REG9(op)) & sz_mask(sz), D(r) & sz_mask(sz), sz), sz);
        return;
    }
    dn_to_ea(cpu, op, sz, K_EOR);
}
#define ARITH(name, k)                                                                         \
    static inline void name##_er(m68k_cpu *cpu, uint32_t op, unsigned sz) { ea_to_dn(cpu, op, sz, k); } \
    static inline void name##_re(m68k_cpu *cpu, uint32_t op, unsigned sz) { dn_to_ea(cpu, op, sz, k); } \
    SIZED(name##_er) SIZED(name##_re)
ARITH(op_or, K_OR)
ARITH(op_and, K_AND)
ARITH(op_sub, K_SUB)
ARITH(op_add, K_ADD)
static inline void op_cmp(m68k_cpu *cpu, uint32_t op, unsigned sz) { ea_to_dn(cpu, op, sz, K_CMP); }
SIZED(op_cmp)
SIZED(eor_op)

/* ADDA, SUBA, CMPA */
static inline void op_adda(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t s = sz_sext(src_read(cpu, mode, reg, sz), sz);
    np(cpu);
    idle(cpu, (sz == 2 || !is_mem(mode, reg)) ? 4u : 2u);
    A(OP_REG9(op)) += s;
}
static inline void op_suba(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t s = sz_sext(src_read(cpu, mode, reg, sz), sz);
    np(cpu);
    idle(cpu, (sz == 2 || !is_mem(mode, reg)) ? 4u : 2u);
    A(OP_REG9(op)) -= s;
}
static inline void op_cmpa(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t s = sz_sext(src_read(cpu, OP_MODE(op), OP_REG(op), sz), sz);
    np(cpu);
    idle(cpu, 2);
    alu_cmp(cpu, s, A(OP_REG9(op)), 4);
}
static void op_adda_w(m68k_cpu *cpu, uint32_t op) { op_adda(cpu, op, 2); }
static void op_adda_l(m68k_cpu *cpu, uint32_t op) { op_adda(cpu, op, 4); }
static void op_suba_w(m68k_cpu *cpu, uint32_t op) { op_suba(cpu, op, 2); }
static void op_suba_l(m68k_cpu *cpu, uint32_t op) { op_suba(cpu, op, 4); }
static void op_cmpa_w(m68k_cpu *cpu, uint32_t op) { op_cmpa(cpu, op, 2); }
static void op_cmpa_l(m68k_cpu *cpu, uint32_t op) { op_cmpa(cpu, op, 4); }

/* ORI, ANDI, SUBI, ADDI, EORI, CMPI #<data>,<ea> */
static inline void imm_op(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    const uint32_t s = sz == 4 ? ext32(cpu) : ext16(cpu) & sz_mask(sz);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        np(cpu);
        if (sz == 4) idle(cpu, k == K_CMP ? 2u : 4u);
        const uint32_t r = alu(cpu, k, s, D(reg) & sz_mask(sz), sz);
        if (k != K_CMP) set_dn(cpu, reg, r, sz);
        return;
    }
    rmw_mem(cpu, mode, reg, sz, k, s);
}
#define IMM_OP(name, k) \
    static inline void name(m68k_cpu *cpu, uint32_t op, unsigned sz) { imm_op(cpu, op, sz, k); } SIZED(name)
IMM_OP(op_ori, K_OR)
IMM_OP(op_andi, K_AND)
IMM_OP(op_subi, K_SUB)
IMM_OP(op_addi, K_ADD)
IMM_OP(op_eori, K_EOR)
IMM_OP(op_cmpi, K_CMP)

/* ORI/ANDI/EORI to CCR and to SR (20 clocks: the immediate word, two
 * internal stretches, the queue refetched). */
static void ccr_sr_op(m68k_cpu *cpu, int to_sr, int kind) {
    if (to_sr && !cpu->s) { at_instruction(cpu, VEC_PRIVILEGE); return; }
    const uint32_t v = ext16(cpu);
    idle(cpu, 8);
    const uint32_t cur = to_sr ? m68k_sr(cpu) : ccr_of(cpu);
    const uint32_t r = kind == K_OR ? cur | v : kind == K_AND ? cur & v : cur ^ v;
    if (to_sr) m68k_write_sr_insn(cpu, r);
    else ccr_set(cpu, r & 0x1Fu);
    X.pf -= 2;
    np(cpu);
    np(cpu);
}
static void op_ori_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 0, K_OR); }
static void op_andi_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 0, K_AND); }
static void op_eori_ccr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 0, K_EOR); }
static void op_ori_sr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 1, K_OR); }
static void op_andi_sr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 1, K_AND); }
static void op_eori_sr(m68k_cpu *cpu, uint32_t op) { (void)op; ccr_sr_op(cpu, 1, K_EOR); }

/* BTST, BCHG, BCLR, BSET: on Dn modulo 32 (UM 8: two more clocks, BCLR
 * four, for bits 16-31), in memory modulo 8. */
static void bit_op(m68k_cpu *cpu, uint32_t op, uint32_t bit, int kind) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        const uint32_t m = 1u << (bit & 31u);
        np(cpu);
        cpu->fz = D(reg) & m;
        if (kind == 0) idle(cpu, 2);
        else idle(cpu, (kind == 2 ? 4u : 2u) + ((bit & 31u) >= 16 ? 2u : 0u));
        if (kind == 1) D(reg) ^= m;
        else if (kind == 2) D(reg) &= ~m;
        else if (kind == 3) D(reg) |= m;
        return;
    }
    const uint32_t m = 1u << (bit & 7u);
    if (kind == 0) {
        cpu->fz = src_read(cpu, mode, reg, 1) & m;
        np(cpu);
        if (mode == 7 && reg == 4) idle(cpu, 2);
        return;
    }
    const uint32_t a = ea_calc(cpu, mode, reg, 1);
    const uint32_t v = rdb(cpu, a);
    cpu->fz = v & m;
    np(cpu);
    wrb(cpu, a, kind == 1 ? v ^ m : kind == 2 ? v & ~m : v | m);
}
static void op_btst_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, ext16(cpu) & 0xFFu, 0); }
static void op_bchg_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, ext16(cpu) & 0xFFu, 1); }
static void op_bclr_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, ext16(cpu) & 0xFFu, 2); }
static void op_bset_i(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, ext16(cpu) & 0xFFu, 3); }
static void op_btst_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 0); }
static void op_bchg_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 1); }
static void op_bclr_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 2); }
static void op_bset_r(m68k_cpu *cpu, uint32_t op) { bit_op(cpu, op, D(OP_REG9(op)), 3); }

/* MOVEP: alternate bytes at (d16,An), high byte first. */
static void op_movep(m68k_cpu *cpu, uint32_t op) {
    const unsigned dn = OP_REG9(op), opmode = (op >> 6) & 7u;
    const uint32_t a = A(OP_REG(op)) + (uint32_t)(int32_t)(int16_t)ext16(cpu);
    switch (opmode) {
    case 4: { const uint32_t hi = rdb(cpu, a); const uint32_t lo = rdb(cpu, a + 2); set_dn(cpu, dn, hi << 8 | lo, 2); break; }
    case 5: {
        const uint32_t b0 = rdb(cpu, a), b1 = rdb(cpu, a + 2), b2 = rdb(cpu, a + 4), b3 = rdb(cpu, a + 6);
        D(dn) = b0 << 24 | b1 << 16 | b2 << 8 | b3;
        break;
    }
    case 6: wrb(cpu, a, D(dn) >> 8); wrb(cpu, a + 2, D(dn)); break;
    default: wrb(cpu, a, D(dn) >> 24); wrb(cpu, a + 2, D(dn) >> 16); wrb(cpu, a + 4, D(dn) >> 8); wrb(cpu, a + 6, D(dn)); break;
    }
    np(cpu);
}

/* MOVE, MOVEA */
static void move_op(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t v = src_read(cpu, OP_MODE(op), OP_REG(op), sz);
    const unsigned dmode = (op >> 6) & 7u, dreg = OP_REG9(op);
    switch (dmode) {
    case 0:
        flags_logic(cpu, v, sz);
        set_dn(cpu, dreg, v, sz);
        np(cpu);
        return;
    case 2: case 3: {
        /* (An)+: the register steps after the write. */
        const uint32_t a = A(dreg);
        flags_logic(cpu, v, sz);
        wr_sz(cpu, a, sz, v);
        if (dmode == 3) A(dreg) = a + ea_step(dreg, sz);
        np(cpu);
        return;
    }
    case 4: {
        np(cpu);
        flags_logic(cpu, v, sz);
        if (sz == 4) {
            A(dreg) -= 2;
            wrw(cpu, A(dreg), v);
            A(dreg) -= 2;
            wrw(cpu, A(dreg), v >> 16);
        } else {
            A(dreg) -= ea_step(dreg, sz);
            wr_sz(cpu, A(dreg), sz, v);
        }
        return;
    }
    case 7:
        if (dreg == 1 && is_mem(OP_MODE(op), OP_REG(op))) {
            /* From memory: the write comes before the second address word
             * is refetched. */
            const uint32_t hi = ext16(cpu);
            const uint32_t a = hi << 16 | X.irc;
            flags_logic(cpu, v, sz);
            wr_sz(cpu, a, sz, v);
            np(cpu);
            np(cpu);
            return;
        }
        /* fallthrough */
    default: {
        const uint32_t a = ea_calc(cpu, dmode, dreg, sz);
        flags_logic(cpu, v, sz);
        wr_sz(cpu, a, sz, v);
        np(cpu);
        return;
    }
    }
}
static void op_move_b(m68k_cpu *cpu, uint32_t op) { move_op(cpu, op, 1); }
static void op_move_w(m68k_cpu *cpu, uint32_t op) { move_op(cpu, op, 2); }
static void op_move_l(m68k_cpu *cpu, uint32_t op) { move_op(cpu, op, 4); }
static void op_movea_w(m68k_cpu *cpu, uint32_t op) {
    A(OP_REG9(op)) = sz_sext(src_read(cpu, OP_MODE(op), OP_REG(op), 2), 2);
    np(cpu);
}
static void op_movea_l(m68k_cpu *cpu, uint32_t op) {
    A(OP_REG9(op)) = src_read(cpu, OP_MODE(op), OP_REG(op), 4);
    np(cpu);
}
static void op_moveq(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = (uint32_t)(int32_t)(int8_t)op;
    D(OP_REG9(op)) = v;
    flags_logic(cpu, v, 4);
    np(cpu);
}

/* NEGX, NEG, NOT, CLR, NBCD, Scc, MOVE from SR, TAS: single operand. The
 * 68000 reads a memory destination before writing it, CLR and Scc too. */
enum { U_NEGX, U_NEG, U_NOT, U_CLR, U_NBCD };
static uint32_t bcd_sub(m68k_cpu *cpu, uint32_t s, uint32_t d);
static uint32_t unary(m68k_cpu *cpu, int u, uint32_t v, unsigned sz) {
    switch (u) {
    case U_NEGX: return alu_subx(cpu, v, 0, sz);
    case U_NEG: return alu_sub(cpu, v, 0, sz);
    case U_NOT: { const uint32_t r = ~v & sz_mask(sz); flags_logic(cpu, r, sz); return r; }
    case U_CLR: flags_logic(cpu, 0, sz); return 0;
    default: return bcd_sub(cpu, v, 0);
    }
}
static void unary_op(m68k_cpu *cpu, uint32_t op, unsigned sz, int u) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        np(cpu);
        if (sz == 4) idle(cpu, 2);
        if (u == U_NBCD) idle(cpu, 2);
        set_dn(cpu, reg, unary(cpu, u, D(reg) & sz_mask(sz), sz), sz);
        return;
    }
    const uint32_t a = ea_calc(cpu, mode, reg, sz);
    const uint32_t v = rd_sz(cpu, a, sz);
    np(cpu);
    const uint32_t r = unary(cpu, u, v, sz);
    if (sz == 4) {
        wrw(cpu, a + 2, r);
        wrw(cpu, a, r >> 16);
    } else {
        wr_sz(cpu, a, sz, r);
    }
}
static inline void op_negx(m68k_cpu *cpu, uint32_t op, unsigned sz) { unary_op(cpu, op, sz, U_NEGX); }
static inline void op_neg(m68k_cpu *cpu, uint32_t op, unsigned sz) { unary_op(cpu, op, sz, U_NEG); }
static inline void op_not(m68k_cpu *cpu, uint32_t op, unsigned sz) { unary_op(cpu, op, sz, U_NOT); }
static inline void op_clr(m68k_cpu *cpu, uint32_t op, unsigned sz) { unary_op(cpu, op, sz, U_CLR); }
SIZED(op_negx)
SIZED(op_neg)
SIZED(op_not)
SIZED(op_clr)
static void op_nbcd(m68k_cpu *cpu, uint32_t op) { unary_op(cpu, op, 1, U_NBCD); }

static void op_scc(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t v = cond(cpu, op >> 8) ? 0xFFu : 0;
    if (mode == 0) {
        np(cpu);
        if (v) idle(cpu, 2);
        set_dn(cpu, reg, v, 1);
        return;
    }
    const uint32_t a = ea_calc(cpu, mode, reg, 1);
    (void)rdb(cpu, a);
    np(cpu);
    wrb(cpu, a, v);
}

static void op_move_from_sr(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t v = m68k_sr(cpu);
    if (mode == 0) {
        np(cpu);
        idle(cpu, 2);
        set_dn(cpu, reg, v, 2);
        return;
    }
    const uint32_t a = ea_calc(cpu, mode, reg, 2);
    (void)rdw(cpu, a);
    np(cpu);
    wrw(cpu, a, v);
}
static void op_move_to_ccr(m68k_cpu *cpu, uint32_t op) {
    const uint32_t v = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    idle(cpu, 4);
    ccr_set(cpu, v & 0x1Fu);
    X.pf -= 2;
    np(cpu);
    np(cpu);
}
static void op_move_to_sr(m68k_cpu *cpu, uint32_t op) {
    PRIV();
    const uint32_t v = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    idle(cpu, 4);
    m68k_write_sr_insn(cpu, v);
    X.pf -= 2;
    np(cpu);
    np(cpu);
}

static void op_tas(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 0) {
        flags_logic(cpu, D(reg) & 0xFFu, 1);
        D(reg) |= 0x80u;
        np(cpu);
        return;
    }
    const uint32_t a = ea_calc(cpu, mode, reg, 1);
    /* One indivisible read-modify-write cycle (UM 5.1.3): ten clocks. */
    X.t += waits(cpu, a, cpu->fc_data, 2);
    const uint32_t pa = a & 0xFFFFFFu;
    uint32_t v = 0;
    uint8_t *const *wmap = cpu->bus.write_map[cpu->s];
    uint8_t *p = wmap ? wmap[pa >> M68K_PAGE_SHIFT] : NULL;
    if (p) {
        p += pa & (M68K_PAGE_SIZE - 1);
        v = *p;
        flags_logic(cpu, v, 1);
        *p = (uint8_t)(v | 0x80u);
    } else {
        if (cpu->bus.read(cpu->bus.ctx, pa, 1, cpu->fc_data, &v)) { X.t += 4; group0(cpu, G0_BUS, a, 1, 0, cpu->fc_data); }
        flags_logic(cpu, v, 1);
        if (cpu->bus.write(cpu->bus.ctx, pa, 1, cpu->fc_data, (v | 0x80u) & 0xFFu)) { X.t += 10; group0(cpu, G0_BUS, a, 0, 0, cpu->fc_data); }
    }
    X.t += 10;
    np(cpu);
}

/* TST */
static inline void op_tst(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    flags_logic(cpu, src_read(cpu, OP_MODE(op), OP_REG(op), sz), sz);
    np(cpu);
}
SIZED(op_tst)

/* CHK <ea>,Dn: trap when Dn > bound (N = Dn < 0) or else Dn < 0 (N set);
 * Z, V, C undefined: Z set for Dn = 0, V and C cleared; N kept without a
 * trap (observed). */
static void op_chk(m68k_cpu *cpu, uint32_t op) {
    const int32_t bound = (int16_t)src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const int32_t v = (int16_t)D(OP_REG9(op));
    np(cpu);
    cpu->fz = (uint32_t)(v & 0xFFFF);
    cpu->fv = 0;
    cpu->fc = 0;
    if (v > bound) {
        cpu->fn = v < 0 ? 0x80000000u : 0;
        exception(cpu, VEC_CHK, X.pf - 4u, 4);
    } else if (v < 0) {
        cpu->fn = 0x80000000u;
        exception(cpu, VEC_CHK, X.pf - 4u, 6);
    } else {
        idle(cpu, 6);
    }
}

static void op_lea(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    uint32_t a;
    if (mode == 6 || (mode == 7 && reg == 3)) {
        idle(cpu, 2);
        const uint32_t base = mode == 6 ? A(reg) : pc_irc(cpu);
        a = index_of(cpu, base, ext16(cpu));
        idle(cpu, 2);
    } else {
        a = ea_calc(cpu, mode, reg, 4);
    }
    A(OP_REG9(op)) = a;
    np(cpu);
}
static void op_pea(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    uint32_t a;
    if (mode == 6 || (mode == 7 && reg == 3)) {
        idle(cpu, 2);
        const uint32_t base = mode == 6 ? A(reg) : pc_irc(cpu);
        a = index_of(cpu, base, ext16(cpu));
        idle(cpu, 2);
    } else {
        a = ea_calc(cpu, mode, reg, 4);
    }
    if (mode == 7 && reg <= 1) {
        A(7) -= 4;
        wrw(cpu, A(7), a >> 16);
        wrw(cpu, A(7) + 2, a);
        np(cpu);
        return;
    }
    np(cpu);
    A(7) -= 4;
    wrw(cpu, A(7), a >> 16);
    wrw(cpu, A(7) + 2, a);
}

/* ABCD, SBCD, NBCD (PRM 4): the decimal carry in C and X, Z cleared when
 * the result is nonzero; N and V, undefined, as the 68000 leaves them
 * (observed): N bit 7 of the result, V set when the decimal correction
 * changed bit 7 the other way than the binary result had it. */
static uint32_t bcd_add(m68k_cpu *cpu, uint32_t s, uint32_t d) {
    const uint32_t x = cpu->fx & 1u;
    const uint32_t bin = s + d + x;
    uint32_t r = bin;
    if ((s & 0x0Fu) + (d & 0x0Fu) + x > 9) r += 0x06;
    const uint32_t carry = r > 0x9F;
    if (carry) r += 0x60;
    cpu->fv = (~bin & r & 0x80u) << 24;
    r &= 0xFFu;
    cpu->fc = cpu->fx = carry;
    cpu->fz |= r;
    cpu->fn = r << 24;
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
    cpu->fv = ((uint32_t)bin & ~res & 0x80u) << 24;
    cpu->fc = cpu->fx = borrow;
    cpu->fz |= res;
    cpu->fn = res << 24;
    return res;
}
/* ABCD/SBCD/ADDX/SUBX -(Ay),-(Ax): the source then the destination,
 * each decremented and read; the result written. */
static void x_mem(m68k_cpu *cpu, uint32_t op, unsigned sz, int kind) {
    const unsigned ry = OP_REG(op), rx = OP_REG9(op);
    idle(cpu, 2);
    uint32_t s, d;
    if (sz == 4) {
        A(ry) -= 2;
        const uint32_t slo = rdw(cpu, A(ry));
        A(ry) -= 2;
        s = rdw(cpu, A(ry)) << 16 | slo;
        A(rx) -= 2;
        const uint32_t dlo = rdw(cpu, A(rx));
        A(rx) -= 2;
        d = rdw(cpu, A(rx)) << 16 | dlo;
    } else {
        A(ry) -= ea_step(ry, sz);
        s = rd_sz(cpu, A(ry), sz);
        A(rx) -= ea_step(rx, sz);
        d = rd_sz(cpu, A(rx), sz);
    }
    uint32_t r;
    switch (kind) {
    case 0: r = alu_addx(cpu, s, d, sz); break;
    case 1: r = alu_subx(cpu, s, d, sz); break;
    case 2: r = bcd_add(cpu, s, d); break;
    default: r = bcd_sub(cpu, s, d); break;
    }
    if (sz == 4) {
        wrw(cpu, A(rx) + 2, r);
        np(cpu);
        wrw(cpu, A(rx), r >> 16);
    } else {
        np(cpu);
        wr_sz(cpu, A(rx), sz, r);
    }
}
static void x_reg(m68k_cpu *cpu, uint32_t op, unsigned sz, int kind) {
    const unsigned ry = OP_REG(op), rx = OP_REG9(op);
    np(cpu);
    if (sz == 4 || kind >= 2) idle(cpu, kind >= 2 ? 2u : 4u);
    const uint32_t s = D(ry) & sz_mask(sz), d = D(rx) & sz_mask(sz);
    uint32_t r;
    switch (kind) {
    case 0: r = alu_addx(cpu, s, d, sz); break;
    case 1: r = alu_subx(cpu, s, d, sz); break;
    case 2: r = bcd_add(cpu, s, d); break;
    default: r = bcd_sub(cpu, s, d); break;
    }
    set_dn(cpu, rx, r, sz);
}
static void op_abcd_r(m68k_cpu *cpu, uint32_t op) { x_reg(cpu, op, 1, 2); }
static void op_abcd_m(m68k_cpu *cpu, uint32_t op) { x_mem(cpu, op, 1, 2); }
static void op_sbcd_r(m68k_cpu *cpu, uint32_t op) { x_reg(cpu, op, 1, 3); }
static void op_sbcd_m(m68k_cpu *cpu, uint32_t op) { x_mem(cpu, op, 1, 3); }
static inline void op_addx_r(m68k_cpu *cpu, uint32_t op, unsigned sz) { x_reg(cpu, op, sz, 0); }
static inline void op_addx_m(m68k_cpu *cpu, uint32_t op, unsigned sz) { x_mem(cpu, op, sz, 0); }
static inline void op_subx_r(m68k_cpu *cpu, uint32_t op, unsigned sz) { x_reg(cpu, op, sz, 1); }
static inline void op_subx_m(m68k_cpu *cpu, uint32_t op, unsigned sz) { x_mem(cpu, op, sz, 1); }
SIZED(op_addx_r) SIZED(op_addx_m) SIZED(op_subx_r) SIZED(op_subx_m)

/* CMPM (Ay)+,(Ax)+ */
static inline void op_cmpm(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned ry = OP_REG(op), rx = OP_REG9(op);
    const uint32_t as = A(ry);
    A(ry) += ea_step(ry, sz);
    const uint32_t s = rd_sz(cpu, as, sz);
    const uint32_t ad = A(rx);
    A(rx) += ea_step(rx, sz);
    const uint32_t d = rd_sz(cpu, ad, sz);
    alu_cmp(cpu, s, d, sz);
    np(cpu);
}
SIZED(op_cmpm)

/* LINK, UNLK, SWAP, EXT, EXG */
static void op_link(m68k_cpu *cpu, uint32_t op) {
    const unsigned reg = OP_REG(op);
    const uint32_t d = (uint32_t)(int32_t)(int16_t)ext16(cpu);
    const uint32_t sp = A(7) - 4;
    const uint32_t v = reg == 7 ? sp : A(reg);
    A(7) = sp;
    wrw(cpu, sp, v >> 16);
    wrw(cpu, sp + 2, v);
    A(reg) = sp;
    A(7) = sp + d;
    np(cpu);
}
static void op_unlk(m68k_cpu *cpu, uint32_t op) {
    const unsigned reg = OP_REG(op);
    const uint32_t a = A(reg);
    A(7) = a;
    const uint32_t v = rdl(cpu, a);
    A(7) = a + 4;
    A(reg) = v;
    np(cpu);
}
static void op_swap(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = (D(r) << 16) | (D(r) >> 16);
    flags_logic(cpu, D(r), 4);
    np(cpu);
}
static void op_ext_w(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    const uint32_t v = (uint32_t)(int32_t)(int8_t)D(r) & 0xFFFFu;
    set_dn(cpu, r, v, 2);
    flags_logic(cpu, v, 2);
    np(cpu);
}
static void op_ext_l(m68k_cpu *cpu, uint32_t op) {
    const unsigned r = OP_REG(op);
    D(r) = (uint32_t)(int32_t)(int16_t)D(r);
    flags_logic(cpu, D(r), 4);
    np(cpu);
}
static void op_exg(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = (op >> 3) & 0x1Fu;
    const unsigned rx = OP_REG9(op) + (mode == 9 ? 8u : 0u), ry = OP_REG(op) + (mode == 8 ? 0u : 8u);
    const uint32_t t = cpu->r[rx];
    cpu->r[rx] = cpu->r[ry];
    cpu->r[ry] = t;
    np(cpu);
    idle(cpu, 2);
}

/* MOVEM: registers to memory (D0 first; to -(An) A7 first, downwards,
 * storing An's initial value: PRM, MOVEM, the 68000 rule) and memory to
 * registers (words sign-extended; one more word read after the last). */
static void movem_rm(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t mask = ext16(cpu);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 4) {
        uint32_t a = A(reg);
        for (unsigned i = 0; i < 16; i++) {
            if (!(mask & (1u << i))) continue;
            const uint32_t v = cpu->r[15u - i];
            if (sz == 4) {
                a -= 2;
                wrw(cpu, a, v);
                a -= 2;
                wrw(cpu, a, v >> 16);
            } else {
                a -= 2;
                wrw(cpu, a, v);
            }
        }
        A(reg) = a;
        np(cpu);
        return;
    }
    uint32_t a = ea_calc(cpu, mode, reg, sz);
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        if (sz == 4) wrl(cpu, a, cpu->r[i]);
        else wrw(cpu, a, cpu->r[i]);
        a += sz;
    }
    np(cpu);
}
static void movem_mr(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const uint32_t mask = ext16(cpu);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const unsigned fc = fc_of(cpu, mode, reg);
    uint32_t a = mode == 3 ? A(reg) : ea_calc(cpu, mode, reg, sz);
    /* An odd address faults at the first word, (An)+ then two past it. */
    if (mode == 3 && (a & 1u) && mask) A(reg) = a + 2;
    for (unsigned i = 0; i < 16; i++) {
        if (!(mask & (1u << i))) continue;
        const uint32_t v = sz == 4 ? rd_fc(cpu, a, 4, fc) : (uint32_t)(int32_t)(int16_t)rd_fc(cpu, a, 2, fc);
        cpu->r[i] = v;
        a += sz;
        if (mode == 3) A(reg) = a;
    }
    (void)rd_fc(cpu, a, 2, fc);
    np(cpu);
}
static void op_movem_rm_w(m68k_cpu *cpu, uint32_t op) { movem_rm(cpu, op, 2); }
static void op_movem_rm_l(m68k_cpu *cpu, uint32_t op) { movem_rm(cpu, op, 4); }
static void op_movem_mr_w(m68k_cpu *cpu, uint32_t op) { movem_mr(cpu, op, 2); }
static void op_movem_mr_l(m68k_cpu *cpu, uint32_t op) { movem_mr(cpu, op, 4); }

/* TRAP, TRAPV, MOVE USP, RESET, NOP, STOP, RTE, RTS, RTR */
static void op_trap(m68k_cpu *cpu, uint32_t op) { exception(cpu, VEC_TRAP + (op & 15u), X.pf - 2u, 4); }
static void op_trapv(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    np(cpu);
    if (cpu->fv >> 31) exception(cpu, VEC_TRAPCC, X.pf - 4u, 0);
}
static void op_move_to_usp(m68k_cpu *cpu, uint32_t op) { PRIV(); cpu->usp = A(OP_REG(op)); np(cpu); }
static void op_move_from_usp(m68k_cpu *cpu, uint32_t op) { PRIV(); A(OP_REG(op)) = cpu->usp; np(cpu); }
static void op_nop(m68k_cpu *cpu, uint32_t op) { (void)op; np(cpu); }
static void op_reset(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIV();
    /* RESET asserted for 124 clocks (UM 6.3.1, Table 8-12: 132 in all). */
    if (cpu->bus.reset) cpu->bus.reset(cpu->bus.ctx);
    idle(cpu, 128);
    np(cpu);
}
static void op_stop(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIV();
    const uint32_t sr = X.irc;
    m68k_write_sr_insn(cpu, sr);
    np(cpu);
    cpu->events |= EV_STOP;
}
static void op_rte(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIV();
    const uint32_t sp = A(7);
    const uint32_t hi = rdw(cpu, sp + 2);
    const uint32_t sr = rdw(cpu, sp);
    const uint32_t lo = rdw(cpu, sp + 4);
    A(7) = sp + 6;
    m68k_write_sr_insn(cpu, sr);
    jump_to(cpu, hi << 16 | lo);
    np(cpu);
}
static void op_rts(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    const uint32_t sp = A(7);
    const uint32_t pc = rdl(cpu, sp);
    A(7) = sp + 4;
    jump_to(cpu, pc);
    np(cpu);
}
static void op_rtr(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    const uint32_t sp = A(7);
    const uint32_t hi = rdw(cpu, sp + 2);
    const uint32_t ccr = rdw(cpu, sp);
    const uint32_t lo = rdw(cpu, sp + 4);
    A(7) = sp + 6;
    ccr_set(cpu, ccr & 0x1Fu);
    jump_to(cpu, hi << 16 | lo);
    np(cpu);
}

/* JMP, JSR: the target's control address; the extension words are not
 * refetched past it. */
static uint32_t jump_ea(m68k_cpu *cpu, unsigned mode, unsigned reg) {
    switch (mode) {
    case 2: return A(reg);
    case 5: idle(cpu, 2); return A(reg) + (uint32_t)(int32_t)(int16_t)X.irc;
    case 6: idle(cpu, 6); return index_of(cpu, A(reg), X.irc);
    default:
        switch (reg) {
        case 0: idle(cpu, 2); return (uint32_t)(int32_t)(int16_t)X.irc;
        case 1: { const uint32_t hi = ext16(cpu); return hi << 16 | X.irc; }
        case 2: idle(cpu, 2); return pc_irc(cpu) + (uint32_t)(int32_t)(int16_t)X.irc;
        default: idle(cpu, 6); return index_of(cpu, pc_irc(cpu), X.irc);
        }
    }
}
/* The return address: past the extension words. */
static uint32_t jump_return(m68k_cpu *cpu, unsigned mode, unsigned reg) {
    const uint32_t words = (mode == 2) ? 0u : (mode == 7 && reg == 1) ? 2u : 1u;
    return cpu->ipc + 2u + 2u * words;
}
static void op_jmp(m68k_cpu *cpu, uint32_t op) {
    const uint32_t t = jump_ea(cpu, OP_MODE(op), OP_REG(op));
    jump_to(cpu, t);
    np(cpu);
}
static void op_jsr(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const uint32_t ret = jump_return(cpu, mode, reg);
    const uint32_t t = jump_ea(cpu, mode, reg);
    jump_to(cpu, t);
    const uint32_t sp = A(7) - 4;
    A(7) = sp;
    wrw(cpu, sp, ret >> 16);
    wrw(cpu, sp + 2, ret);
    np(cpu);
}

/* ADDQ, SUBQ (An: whole register, no flags), Scc, DBcc */
static inline void quick(m68k_cpu *cpu, uint32_t op, unsigned sz, int k) {
    const uint32_t q = ((OP_REG9(op) - 1u) & 7u) + 1u;
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (mode == 1) {
        np(cpu);
        idle(cpu, 4);
        A(reg) = k == K_ADD ? A(reg) + q : A(reg) - q;
        return;
    }
    if (mode == 0) {
        np(cpu);
        if (sz == 4) idle(cpu, 4);
        set_dn(cpu, reg, alu(cpu, k, q, D(reg) & sz_mask(sz), sz), sz);
        return;
    }
    rmw_mem(cpu, mode, reg, sz, k, q);
}
static inline void op_addq(m68k_cpu *cpu, uint32_t op, unsigned sz) { quick(cpu, op, sz, K_ADD); }
static inline void op_subq(m68k_cpu *cpu, uint32_t op, unsigned sz) { quick(cpu, op, sz, K_SUB); }
SIZED(op_addq)
SIZED(op_subq)

static void op_dbcc(m68k_cpu *cpu, uint32_t op) {
    const uint32_t base = pc_irc(cpu);
    const uint32_t disp = (uint32_t)(int32_t)(int16_t)X.irc;
    if (cond(cpu, op >> 8)) {
        idle(cpu, 4);
        np(cpu);
        np(cpu);
        return;
    }
    idle(cpu, 2);
    const unsigned r = OP_REG(op);
    const uint32_t w = (D(r) - 1u) & 0xFFFFu;
    D(r) = (D(r) & 0xFFFF0000u) | w;
    if (disp == 0xFFFFFFFEu && cpu->bus.dbra) {
        cpu->dbra_runs++;
        cpu->bus.dbra(cpu->bus.ctx, 1, w != 0xFFFFu ? 10u : 14u);
    }
    if (w != 0xFFFFu) {
        jump_to(cpu, base + disp);
        np(cpu);
        return;
    }
    /* Expired: the branch target is read and dropped, then the queue refilled past the displacement. */
    {
        const uint32_t save = X.pf;
        X.pf = base + disp;
        np(cpu);
        X.pf = save;
    }
    np(cpu);
    np(cpu);
}

/* Bcc, BRA, BSR: displacement 8 bits, or 16 in the next word when zero
 * ($FF is -1 on the 68000). */
static void op_bcc(m68k_cpu *cpu, uint32_t op) {
    const uint32_t base = pc_irc(cpu);
    uint32_t disp = (uint32_t)(int32_t)(int8_t)op;
    const int word = disp == 0;
    if (word) disp = (uint32_t)(int32_t)(int16_t)X.irc;
    const unsigned cc = (op >> 8) & 15u;
    if (cc == 1) {
        idle(cpu, 2);
        const uint32_t ret = word ? base + 2 : base;
        const uint32_t sp = A(7) - 4;
        A(7) = sp;
        wrw(cpu, sp, ret >> 16);
        wrw(cpu, sp + 2, ret);
        jump_to(cpu, base + disp);
        np(cpu);
        return;
    }
    if (cond(cpu, cc)) {
        idle(cpu, 2);
        jump_to(cpu, base + disp);
        np(cpu);
        return;
    }
    idle(cpu, 4);
    if (word) np(cpu);
    np(cpu);
}

/* MULU, MULS (UM 8: 38 + 2n clocks: n the ones of the source; MULS the
 * 01 and 10 pairs of the source with a zero appended). */
static unsigned ones16(uint32_t v) {
    unsigned n = 0;
    for (v &= 0xFFFFu; v; v &= v - 1) n++;
    return n;
}
static void op_mulu(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    np(cpu);
    idle(cpu, 34 + 2 * ones16(s));
    const unsigned dn = OP_REG9(op);
    const uint32_t r = (D(dn) & 0xFFFFu) * s;
    D(dn) = r;
    flags_logic(cpu, r, 4);
}
static void op_muls(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    np(cpu);
    idle(cpu, 34 + 2 * ones16((s ^ (s << 1)) & 0xFFFFu));
    const unsigned dn = OP_REG9(op);
    const uint32_t r = (uint32_t)((int32_t)(int16_t)D(dn) * (int32_t)(int16_t)s);
    D(dn) = r;
    flags_logic(cpu, r, 4);
}

/* DIVU, DIVS: the time of the 68000's shift-and-subtract (observed; the
 * UM's 140 and 158 clocks are the maxima). Overflow (V set, C cleared, N
 * and Z kept, the register kept) is found before the division: 10 clocks
 * (DIVU), 16 or 18 (DIVS, negative dividend). */
static void divide_by_zero(m68k_cpu *cpu) {
    cpu->fc = 0;
    cpu->fv = 0;
    cpu->fn = 0;
    cpu->fz = 1;
    exception(cpu, VEC_ZERO_DIVIDE, X.pf - 4u, 8);
}
static void divide_overflow(m68k_cpu *cpu, unsigned t) {
    idle(cpu, t - 4);
    np(cpu);
    cpu->fv = 0x80000000u;
    cpu->fc = 0;
}
static void op_divu(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    if (s == 0) { divide_by_zero(cpu); return; }
    const uint32_t d = D(dn);
    if ((d >> 16) >= s) { divide_overflow(cpu, 10); return; }
    /* Per quotient bit: 2 more clocks when the shifted remainder's top bit
     * was clear, 2 fewer of those when the subtraction then succeeded. */
    unsigned t = 76;
    uint32_t rem = d;
    const uint32_t hdiv = s << 16;
    for (int i = 0; i < 15; i++) {
        const uint32_t top = rem & 0x80000000u;
        rem <<= 1;
        if (top) {
            rem -= hdiv;
        } else {
            t += 4;
            if (rem >= hdiv) {
                rem -= hdiv;
                t -= 2;
            }
        }
    }
    idle(cpu, t - 4);
    np(cpu);
    const uint32_t q = d / s, r = d % s;
    D(dn) = (r << 16) | q;
    flags_logic(cpu, q, 2);
}
static void op_divs(m68k_cpu *cpu, uint32_t op) {
    const uint32_t s16 = src_read(cpu, OP_MODE(op), OP_REG(op), 2);
    const unsigned dn = OP_REG9(op);
    const int32_t s = (int16_t)s16;
    if (s == 0) { divide_by_zero(cpu); return; }
    const int32_t d = (int32_t)D(dn);
    const int64_t q = (int64_t)d / s, r = (int64_t)d % s;
    if (q < -32768 || q > 32767) { divide_overflow(cpu, d < 0 ? 18u : 16u); return; }
    /* 120 clocks, 2 more for a negative dividend, 2 more with a positive
     * divisor and a negative dividend, 2 fewer with both positive, and 2
     * per clear bit among the 15 quotient bits below the sign. */
    const uint32_t ad = d < 0 ? 0u - (uint32_t)d : (uint32_t)d, as = s < 0 ? 0u - (uint32_t)s : (uint32_t)s;
    const uint32_t aq = ad / as;
    unsigned t = 12 + 110;
    if (d < 0) t += 2;
    if (s >= 0) t = d >= 0 ? t - 2 : t + 2;
    for (int i = 0; i < 15; i++)
        if (!(aq & (0x8000u >> i))) t += 2;
    idle(cpu, t - 4);
    np(cpu);
    D(dn) = ((uint32_t)r << 16) | ((uint32_t)q & 0xFFFFu);
    flags_logic(cpu, (uint32_t)q & 0xFFFFu, 2);
}

/* Shifts and rotates: on Dn 6 + 2n clocks (8 + 2n long); in memory one
 * bit of a word. */
static uint32_t shift(m68k_cpu *cpu, unsigned type, int left, uint32_t v, unsigned cnt, unsigned sz) {
    const unsigned bits = sz * 8u, sh = sz_shift(sz);
    const uint32_t mask = sz_mask(sz);
    uint32_t r = v, c = 0, ov = 0;
    if (cnt == 0) {
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
static inline void op_shift_r(m68k_cpu *cpu, uint32_t op, unsigned sz) {
    const unsigned cnt = (op & 0x20u) ? (D(OP_REG9(op)) & 63u) : ((OP_REG9(op) - 1u) & 7u) + 1u;
    const unsigned r = OP_REG(op);
    np(cpu);
    idle(cpu, (sz == 4 ? 4u : 2u) + 2u * cnt);
    set_dn(cpu, r, shift(cpu, (op >> 3) & 3u, (op & 0x100u) != 0, D(r) & sz_mask(sz), cnt, sz), sz);
}
SIZED(op_shift_r)
static void op_shift_m(m68k_cpu *cpu, uint32_t op) {
    const uint32_t a = ea_calc(cpu, OP_MODE(op), OP_REG(op), 2);
    const uint32_t v = rdw(cpu, a);
    np(cpu);
    wrw(cpu, a, shift(cpu, (op >> 9) & 3u, (op & 0x100u) != 0, v, 1, 2));
}

/* --- the encoding patterns (PRM section 8; the 68000's addressing
 * categories, UM section 3) ---------------------------------------------------- */

#define P(mask, match, ea, ea2, prio, fn, name) {mask, match, ea, ea2, MM_ALL, prio, fn, name}
#define S3(mask, match, ea, fn, name)                                     \
    P(mask, (match) | 0x0000u, ea, 0, 0, fn##_b, name ".B"),              \
    P(mask, (match) | 0x0040u, ea, 0, 0, fn##_w, name ".W"),              \
    P(mask, (match) | 0x0080u, ea, 0, 0, fn##_l, name ".L")
#define S3B(mask, match, ea, fn, name)                                    \
    P(mask, (match) | 0x0000u, (ea) & ~EA_AN, 0, 0, fn##_b, name ".B"),   \
    P(mask, (match) | 0x0040u, ea, 0, 0, fn##_w, name ".W"),              \
    P(mask, (match) | 0x0080u, ea, 0, 0, fn##_l, name ".L")

const m68k_pattern m68k_patterns000[] = {
    /* line 0 */
    S3(0xFFC0, 0x0000, EA_DALT, op_ori, "ORI"),
    P(0xFFFF, 0x003C, 0, 0, 0, op_ori_ccr, "ORI to CCR"),
    P(0xFFFF, 0x007C, 0, 0, 0, op_ori_sr, "ORI to SR"),
    S3(0xFFC0, 0x0200, EA_DALT, op_andi, "ANDI"),
    P(0xFFFF, 0x023C, 0, 0, 0, op_andi_ccr, "ANDI to CCR"),
    P(0xFFFF, 0x027C, 0, 0, 0, op_andi_sr, "ANDI to SR"),
    S3(0xFFC0, 0x0400, EA_DALT, op_subi, "SUBI"),
    S3(0xFFC0, 0x0600, EA_DALT, op_addi, "ADDI"),
    S3(0xFFC0, 0x0A00, EA_DALT, op_eori, "EORI"),
    P(0xFFFF, 0x0A3C, 0, 0, 0, op_eori_ccr, "EORI to CCR"),
    P(0xFFFF, 0x0A7C, 0, 0, 0, op_eori_sr, "EORI to SR"),
    S3(0xFFC0, 0x0C00, EA_DALT, op_cmpi, "CMPI"),
    P(0xFFC0, 0x0800, EA_DATA & ~EA_IMM, 0, 0, op_btst_i, "BTST"),
    P(0xFFC0, 0x0840, EA_DALT, 0, 0, op_bchg_i, "BCHG"),
    P(0xFFC0, 0x0880, EA_DALT, 0, 0, op_bclr_i, "BCLR"),
    P(0xFFC0, 0x08C0, EA_DALT, 0, 0, op_bset_i, "BSET"),
    P(0xF1C0, 0x0100, EA_DATA, 0, 0, op_btst_r, "BTST"),
    P(0xF1C0, 0x0140, EA_DALT, 0, 0, op_bchg_r, "BCHG"),
    P(0xF1C0, 0x0180, EA_DALT, 0, 0, op_bclr_r, "BCLR"),
    P(0xF1C0, 0x01C0, EA_DALT, 0, 0, op_bset_r, "BSET"),
    P(0xF138, 0x0108, 0, 0, 1, op_movep, "MOVEP"),

    /* lines 1-3 */
    P(0xF000, 0x1000, EA_DATA, EA_DALT, 0, op_move_b, "MOVE.B"),
    P(0xF000, 0x3000, EA_ALL, EA_DALT, 0, op_move_w, "MOVE.W"),
    P(0xF000, 0x2000, EA_ALL, EA_DALT, 0, op_move_l, "MOVE.L"),
    P(0xF1C0, 0x3040, EA_ALL, 0, 0, op_movea_w, "MOVEA.W"),
    P(0xF1C0, 0x2040, EA_ALL, 0, 0, op_movea_l, "MOVEA.L"),

    /* line 4 */
    S3(0xFFC0, 0x4000, EA_DALT, op_negx, "NEGX"),
    P(0xFFC0, 0x40C0, EA_DALT, 0, 0, op_move_from_sr, "MOVE from SR"),
    P(0xF1C0, 0x4180, EA_DATA, 0, 0, op_chk, "CHK.W"),
    P(0xF1C0, 0x41C0, EA_CTRL, 0, 0, op_lea, "LEA"),
    S3(0xFFC0, 0x4200, EA_DALT, op_clr, "CLR"),
    S3(0xFFC0, 0x4400, EA_DALT, op_neg, "NEG"),
    P(0xFFC0, 0x44C0, EA_DATA, 0, 0, op_move_to_ccr, "MOVE to CCR"),
    S3(0xFFC0, 0x4600, EA_DALT, op_not, "NOT"),
    P(0xFFC0, 0x46C0, EA_DATA, 0, 0, op_move_to_sr, "MOVE to SR"),
    P(0xFFC0, 0x4800, EA_DALT, 0, 0, op_nbcd, "NBCD"),
    P(0xFFF8, 0x4840, 0, 0, 0, op_swap, "SWAP"),
    P(0xFFC0, 0x4840, EA_CTRL, 0, 0, op_pea, "PEA"),
    P(0xFFF8, 0x4880, 0, 0, 0, op_ext_w, "EXT.W"),
    P(0xFFF8, 0x48C0, 0, 0, 0, op_ext_l, "EXT.L"),
    P(0xFFC0, 0x4880, EA_CALT | EA_PD, 0, 0, op_movem_rm_w, "MOVEM.W"),
    P(0xFFC0, 0x48C0, EA_CALT | EA_PD, 0, 0, op_movem_rm_l, "MOVEM.L"),
    P(0xFFC0, 0x4C80, EA_CTRL | EA_PI, 0, 0, op_movem_mr_w, "MOVEM.W"),
    P(0xFFC0, 0x4CC0, EA_CTRL | EA_PI, 0, 0, op_movem_mr_l, "MOVEM.L"),
    S3(0xFFC0, 0x4A00, EA_DALT, op_tst, "TST"),
    P(0xFFC0, 0x4AC0, EA_DALT, 0, 0, op_tas, "TAS"),
    P(0xFFFF, 0x4AFC, 0, 0, 0, illegal, "ILLEGAL"),
    P(0xFFF0, 0x4E40, 0, 0, 0, op_trap, "TRAP"),
    P(0xFFF8, 0x4E50, 0, 0, 0, op_link, "LINK"),
    P(0xFFF8, 0x4E58, 0, 0, 0, op_unlk, "UNLK"),
    P(0xFFF8, 0x4E60, 0, 0, 0, op_move_to_usp, "MOVE to USP"),
    P(0xFFF8, 0x4E68, 0, 0, 0, op_move_from_usp, "MOVE from USP"),
    P(0xFFFF, 0x4E70, 0, 0, 0, op_reset, "RESET"),
    P(0xFFFF, 0x4E71, 0, 0, 0, op_nop, "NOP"),
    P(0xFFFF, 0x4E72, 0, 0, 0, op_stop, "STOP"),
    P(0xFFFF, 0x4E73, 0, 0, 0, op_rte, "RTE"),
    P(0xFFFF, 0x4E75, 0, 0, 0, op_rts, "RTS"),
    P(0xFFFF, 0x4E76, 0, 0, 0, op_trapv, "TRAPV"),
    P(0xFFFF, 0x4E77, 0, 0, 0, op_rtr, "RTR"),
    P(0xFFC0, 0x4E80, EA_CTRL, 0, 0, op_jsr, "JSR"),
    P(0xFFC0, 0x4EC0, EA_CTRL, 0, 0, op_jmp, "JMP"),

    /* line 5 */
    S3B(0xF1C0, 0x5000, EA_ALT, op_addq, "ADDQ"),
    S3B(0xF1C0, 0x5100, EA_ALT, op_subq, "SUBQ"),
    P(0xF0C0, 0x50C0, EA_DALT, 0, 0, op_scc, "Scc"),
    P(0xF0F8, 0x50C8, 0, 0, 1, op_dbcc, "DBcc"),

    /* lines 6, 7 */
    P(0xF000, 0x6000, 0, 0, 0, op_bcc, "Bcc"),
    P(0xF100, 0x7000, 0, 0, 0, op_moveq, "MOVEQ"),

    /* line 8 */
    S3(0xF1C0, 0x8000, EA_DATA, op_or_er, "OR"),
    S3(0xF1C0, 0x8100, EA_MALT, op_or_re, "OR"),
    P(0xF1C0, 0x80C0, EA_DATA, 0, 0, op_divu, "DIVU"),
    P(0xF1C0, 0x81C0, EA_DATA, 0, 0, op_divs, "DIVS"),
    P(0xF1F8, 0x8100, 0, 0, 1, op_sbcd_r, "SBCD"),
    P(0xF1F8, 0x8108, 0, 0, 1, op_sbcd_m, "SBCD"),

    /* line 9 */
    S3B(0xF1C0, 0x9000, EA_ALL, op_sub_er, "SUB"),
    S3(0xF1C0, 0x9100, EA_MALT, op_sub_re, "SUB"),
    P(0xF1C0, 0x90C0, EA_ALL, 0, 0, op_suba_w, "SUBA.W"),
    P(0xF1C0, 0x91C0, EA_ALL, 0, 0, op_suba_l, "SUBA.L"),
    P(0xF1F8, 0x9100, 0, 0, 1, op_subx_r_b, "SUBX.B"),
    P(0xF1F8, 0x9140, 0, 0, 1, op_subx_r_w, "SUBX.W"),
    P(0xF1F8, 0x9180, 0, 0, 1, op_subx_r_l, "SUBX.L"),
    P(0xF1F8, 0x9108, 0, 0, 1, op_subx_m_b, "SUBX.B"),
    P(0xF1F8, 0x9148, 0, 0, 1, op_subx_m_w, "SUBX.W"),
    P(0xF1F8, 0x9188, 0, 0, 1, op_subx_m_l, "SUBX.L"),

    /* line A */
    P(0xF000, 0xA000, 0, 0, 0, op_line_a, "line A"),

    /* line B */
    S3B(0xF1C0, 0xB000, EA_ALL, op_cmp, "CMP"),
    P(0xF1C0, 0xB0C0, EA_ALL, 0, 0, op_cmpa_w, "CMPA.W"),
    P(0xF1C0, 0xB1C0, EA_ALL, 0, 0, op_cmpa_l, "CMPA.L"),
    S3(0xF1C0, 0xB100, EA_DALT, eor_op, "EOR"),
    P(0xF1F8, 0xB108, 0, 0, 1, op_cmpm_b, "CMPM.B"),
    P(0xF1F8, 0xB148, 0, 0, 1, op_cmpm_w, "CMPM.W"),
    P(0xF1F8, 0xB188, 0, 0, 1, op_cmpm_l, "CMPM.L"),

    /* line C */
    S3(0xF1C0, 0xC000, EA_DATA, op_and_er, "AND"),
    S3(0xF1C0, 0xC100, EA_MALT, op_and_re, "AND"),
    P(0xF1C0, 0xC0C0, EA_DATA, 0, 0, op_mulu, "MULU"),
    P(0xF1C0, 0xC1C0, EA_DATA, 0, 0, op_muls, "MULS"),
    P(0xF1F8, 0xC100, 0, 0, 1, op_abcd_r, "ABCD"),
    P(0xF1F8, 0xC108, 0, 0, 1, op_abcd_m, "ABCD"),
    P(0xF1F8, 0xC140, 0, 0, 1, op_exg, "EXG"),
    P(0xF1F8, 0xC148, 0, 0, 1, op_exg, "EXG"),
    P(0xF1F8, 0xC188, 0, 0, 1, op_exg, "EXG"),

    /* line D */
    S3B(0xF1C0, 0xD000, EA_ALL, op_add_er, "ADD"),
    S3(0xF1C0, 0xD100, EA_MALT, op_add_re, "ADD"),
    P(0xF1C0, 0xD0C0, EA_ALL, 0, 0, op_adda_w, "ADDA.W"),
    P(0xF1C0, 0xD1C0, EA_ALL, 0, 0, op_adda_l, "ADDA.L"),
    P(0xF1F8, 0xD100, 0, 0, 1, op_addx_r_b, "ADDX.B"),
    P(0xF1F8, 0xD140, 0, 0, 1, op_addx_r_w, "ADDX.W"),
    P(0xF1F8, 0xD180, 0, 0, 1, op_addx_r_l, "ADDX.L"),
    P(0xF1F8, 0xD108, 0, 0, 1, op_addx_m_b, "ADDX.B"),
    P(0xF1F8, 0xD148, 0, 0, 1, op_addx_m_w, "ADDX.W"),
    P(0xF1F8, 0xD188, 0, 0, 1, op_addx_m_l, "ADDX.L"),

    /* line E */
    P(0xF0C0, 0xE000, 0, 0, 0, op_shift_r_b, "shift.B"),
    P(0xF0C0, 0xE040, 0, 0, 0, op_shift_r_w, "shift.W"),
    P(0xF0C0, 0xE080, 0, 0, 0, op_shift_r_l, "shift.L"),
    P(0xF8C0, 0xE0C0, EA_MALT, 0, 0, op_shift_m, "shift mem"),

    /* line F: every opcode */
    P(0xF000, 0xF000, 0, 0, 0, op_line_f, "line F"),
};
const unsigned m68k_pattern_count000 = sizeof m68k_patterns000 / sizeof m68k_patterns000[0];

/* --- the run loop ------------------------------------------------------------- */

/* Trace (UM 6.3.8): after an instruction that began with T set. */
static void take_trace(m68k_cpu *cpu) {
    exception(cpu, VEC_TRACE, X.pf - 4u, 4);
}

static void take_interrupt(m68k_cpu *cpu, unsigned level) {
    /* UM 6.3.2 and Table 8-14 (44 clocks): the acknowledge cycle, then the
     * stacking and the vector. */
    if (cpu->on_exception) cpu->on_exception(cpu->hook_ctx, cpu, VEC_AUTOVECTOR + level, cpu->pc);
    cpu->irqs_taken++;
    X.t = 0;
    X.pf = cpu->pc + 4u;
    const uint16_t old = sr_now(cpu);
    m68k_set_sr(cpu, (uint16_t)(((old | SR_S) & ~(SR_T1 | SR_MASK)) | (level << 8)));
    cpu->events &= ~(uint32_t)EV_STOP;
    cpu->in_exception = 1;
    idle(cpu, 6);
    X.t += cpu->bus.wait ? cpu->bus.wait(cpu->bus.ctx, 0xFFFFF0u | (level << 1), M68K_FC_CPU, 3, cpu->cycles + X.t) : 0;
    const int v = cpu->bus.iack ? cpu->bus.iack(cpu->bus.ctx, level) : M68K_IACK_AUTOVECTOR;
    const unsigned vector = v == M68K_IACK_AUTOVECTOR ? VEC_AUTOVECTOR + level
                            : v == M68K_IACK_SPURIOUS ? VEC_SPURIOUS
                                                      : (unsigned)v & 0xFFu;
    idle(cpu, 4 + 4);
    const uint32_t sp = A(7), pc = cpu->pc;
    bus_wr(cpu, sp - 2, 2, M68K_FC_SUPER_DATA, pc & 0xFFFFu);
    bus_wr(cpu, sp - 6, 2, M68K_FC_SUPER_DATA, old);
    bus_wr(cpu, sp - 4, 2, M68K_FC_SUPER_DATA, pc >> 16);
    A(7) = sp - 6;
    vector_to(cpu, vector);
    cpu->in_exception = 0;
    cpu->pc = X.pf - 4u;
}

static void charge(m68k_cpu *cpu) {
    m68k_charge(cpu, cpu->tm->unit ? 1 : (int64_t)X.t);
    X.t = 0;
}

/* A group 0 fault ended the instruction: its frame, or the halt of a
 * double fault (UM 6.3.9). Returns nonzero when halted. */
static int fault_taken(m68k_cpu *cpu) {
    const int kind = cpu->abort_kind;
    if (kind == G0_HALT || X.group0) {
        X.group0 = 0;
        cpu->in_exception = 0;
        cpu->status = M68K_HALTED;
        charge(cpu);
        cpu->instructions++;
        return 1;
    }
    cpu->in_exception = 0;
    group0_frame(cpu, kind == G0_BUS ? VEC_ACCESS : VEC_ADDRESS);
    cpu->pc = X.pf - 4u;
    charge(cpu);
    cpu->instructions++;
    return 0;
}

/* Instructions while the run has counts left; nonzero to leave. */
static void run_body(m68k_cpu *cpu) {
    for (;;) {
        if (M68K_UNLIKELY(cpu->events)) {
            const uint32_t ev = cpu->events;
            if (ev & EV_EXIT) {
                cpu->events &= ~(uint32_t)EV_EXIT;
                return;
            }
            if (ev & EV_IRQ) {
                const unsigned mask = (cpu->sr_hi >> 8) & 7u;
                unsigned level = 0;
                if (cpu->ipl == 7 && !cpu->nmi_seen) {
                    cpu->nmi_seen = 1;
                    level = 7;
                } else if (cpu->ipl > mask && cpu->ipl < 7) {
                    level = cpu->ipl;
                }
                if (level) {
                    take_interrupt(cpu, level);
                    charge(cpu);
                }
                m68k_update_events(cpu);
            }
            if (cpu->events & EV_STOP) {
                cpu->status = M68K_STOPPED;
                return;
            }
        }
        if (cpu->budget <= 0) return;
        cpu->ipc = cpu->pc;
        X.ird = (uint16_t)peek16(cpu, cpu->pc);
        X.irc = (uint16_t)peek16(cpu, cpu->pc + 2u);
        X.pf = cpu->pc + 4u;
        X.t = 0;
        X.traced = (cpu->sr_hi & SR_T1) != 0;
        cpu->op = X.ird;
        cpu->table[X.ird](cpu, X.ird);
        if (X.traced) take_trace(cpu);
        cpu->pc = X.pf - 4u;
        charge(cpu);
        cpu->instructions++;
    }
}

m68k_status m68k_run000(m68k_cpu *cpu, int64_t counts) {
    if (cpu->status == M68K_HALTED) return M68K_HALTED;
    cpu->status = M68K_OK;
    cpu->budget = counts;
    cpu->abortp = &cpu->abort;
    for (;;) {
        if (setjmp(cpu->abort) != 0) {
            if (fault_taken(cpu)) return M68K_HALTED;
            continue;
        }
        run_body(cpu);
        break;
    }
    return cpu->status;
}
