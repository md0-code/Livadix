/* Internals shared by the CPU's files: memory access with the fast path,
 * effective-address calculation, flags and the exception entry points.
 *
 * Sources: M68000 Family Programmer's Reference Manual (PRM) section 2
 * ("Addressing Capabilities": 2.1 instruction format and extension words,
 * 2.2 effective addressing modes), section 3 (condition-code computation and
 * the conditional tests) and the per-instruction condition codes of section
 * 4; MC68040 User's Manual
 * section 7 (misaligned operands are split into aligned bus cycles) and
 * section 8 (exception processing). */
#ifndef MC_M68K_INTERNAL_H
#define MC_M68K_INTERNAL_H

#include "m68k.h"

#include <string.h>

#if defined(_MSC_VER)
#include <stdlib.h>
#define M68K_INLINE static __forceinline
#define M68K_NOINLINE __declspec(noinline)
#define M68K_NORETURN __declspec(noreturn)
#define M68K_LIKELY(x) (x)
#define M68K_UNLIKELY(x) (x)
#define m68k_bswap16(x) _byteswap_ushort(x)
#define m68k_bswap32(x) _byteswap_ulong(x)
#else
#define M68K_INLINE static inline __attribute__((always_inline))
#define M68K_NOINLINE __attribute__((noinline))
#define M68K_NORETURN __attribute__((noreturn))
#define M68K_LIKELY(x) __builtin_expect(!!(x), 1)
#define M68K_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define m68k_bswap16(x) __builtin_bswap16(x)
#define m68k_bswap32(x) __builtin_bswap32(x)
#endif

#define D(n) cpu->r[(n)]
#define A(n) cpu->r[8 + (n)]

/* SR bits (PRM 1.3.1, "Status Register"). */
#define SR_T1 0x8000u
#define SR_T0 0x4000u
#define SR_S 0x2000u
#define SR_M 0x1000u
#define SR_MASK 0x0700u

/* Exception vectors (PRM appendix B, "Exception Vector Assignments";
 * MC68040 UM section 8). */
enum {
    VEC_RESET_SSP = 0, VEC_RESET_PC = 1, VEC_ACCESS = 2, VEC_ADDRESS = 3, VEC_ILLEGAL = 4,
    VEC_ZERO_DIVIDE = 5, VEC_CHK = 6, VEC_TRAPCC = 7, VEC_PRIVILEGE = 8, VEC_TRACE = 9,
    VEC_LINE_A = 10, VEC_LINE_F = 11, VEC_FORMAT = 14, VEC_UNINITIALIZED = 15,
    VEC_SPURIOUS = 24, VEC_AUTOVECTOR = 24, VEC_TRAP = 32
};

/* events bits */
enum {
    EV_IRQ = 1,         /* an interrupt may be due */
    EV_TRACE = 2,       /* SR.T1 or T0 set: arm the trace */
    EV_WRITEBACK = 4,   /* a write faulted: access error at the boundary */
    EV_STOP = 8,        /* STOP executed */
    EV_EXIT = 16,       /* leave the run loop (halt, unimplemented) */
    EV_TRACE_NOW = 32   /* the instruction just done is to be traced */
};

/* abort kinds (longjmp to the run loop) */
enum { ABORT_ACCESS = 1, ABORT_ILLEGAL, ABORT_HALT, ABORT_UNIMPLEMENTED, ABORT_LINE_F };

/* model masks for the decode patterns */
#define MM_020 1u
#define MM_030 2u
#define MM_040 4u
#define MM_LC040 8u
#define MM_ALL 15u
#define MM_040S (MM_040 | MM_LC040)

/* --- host byte order ---------------------------------------------------- */

M68K_INLINE uint32_t ld16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return m68k_bswap16(v); }
M68K_INLINE uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return m68k_bswap32(v); }
M68K_INLINE void st16(uint8_t *p, uint32_t v) { uint16_t w = m68k_bswap16((uint16_t)v); memcpy(p, &w, 2); }
M68K_INLINE void st32(uint8_t *p, uint32_t v) { uint32_t w = m68k_bswap32(v); memcpy(p, &w, 4); }

/* --- slow paths and exceptions (m68k_run.c) ------------------------------ */

uint32_t m68k_slow_read(m68k_cpu *cpu, uint32_t address, unsigned size, unsigned fc);
void m68k_slow_write(m68k_cpu *cpu, uint32_t address, unsigned size, unsigned fc, uint32_t value);
uint32_t m68k_ea_index(m68k_cpu *cpu, uint32_t base, int pc_relative);
M68K_NORETURN void m68k_abort(m68k_cpu *cpu, int kind);
/* Exception processing (MC68040 UM section 8; MC68030 UM section 8): format
 * 0 frame unless the vector's format says otherwise. `pc` is the PC stacked;
 * `address` the format $2/$3/$4 address field. */
void m68k_exception(m68k_cpu *cpu, unsigned vector, uint32_t pc);
void m68k_exception_fmt2(m68k_cpu *cpu, unsigned vector, uint32_t pc, uint32_t address);
void m68k_exception_illegal(m68k_cpu *cpu);    /* vector 4, PC = this instruction */
void m68k_exception_privilege(m68k_cpu *cpu);  /* vector 8 */
void m68k_exception_line_a(m68k_cpu *cpu);     /* vector 10 */
void m68k_exception_line_f(m68k_cpu *cpu);     /* vector 11 */
void m68k_exception_fp_unavailable(m68k_cpu *cpu, uint32_t ea); /* LC040: vector 11, format $4, PC = next */
void m68k_exception_restart(m68k_cpu *cpu, unsigned vector);   /* undo, then a format $0 frame at this instruction */
void m68k_exception_frame(m68k_cpu *cpu, unsigned vector, unsigned format, uint32_t pc, const uint8_t *extra, unsigned n);
void m68k_undo_steps(m68k_cpu *cpu);
void m68k_op_fpu(m68k_cpu *cpu, uint32_t op);  /* every coprocessor-1 opcode (fpu.c) */
void m68k_unimplemented(m68k_cpu *cpu);        /* stop the run: an opcode of a later step */
void m68k_update_events(m68k_cpu *cpu);
void m68k_select_maps(m68k_cpu *cpu);
/* The FPU at reset (fpu.c). */
void m68k_fpu_reset(m68k_cpu *cpu);
/* Alternate-space accesses (MOVES, the FPU's operand transfers use the
 * normal ones): `fc` as SFC/DFC give it. */
uint32_t m68k_moves_read(m68k_cpu *cpu, uint32_t address, unsigned size, unsigned fc);
void m68k_moves_write(m68k_cpu *cpu, uint32_t address, unsigned size, unsigned fc, uint32_t value);

/* --- instruction timing (m68k_timing.c) ----------------------------------- */

struct m68k_timing {
    uint8_t unit;               /* untimed: one count per instruction, nothing added */
    uint16_t bcc_taken;         /* Bcc/BRA taken (the table holds Bcc not taken) */
    uint16_t dbcc_branch, dbcc_expired;     /* DBcc, cc false: branching; counter expired (the table: cc true) */
    uint16_t movem_reg[2];      /* MOVEM per register: to memory, to registers (68020/030) */
    uint8_t movem_040;          /* MOVEM per MC68040 UM 10.6 (D' + A') */
    uint16_t x_trap;            /* exception processing: TRAP #n, illegal, line A/F, privilege, format and the rest */
    uint16_t x_trace, x_access, x_address, x_int, x_int_m;  /* trace; bus/access error; address error; interrupt (+ M stack) */
    uint16_t rte[16];           /* RTE by frame format */
    const uint16_t *op;
};
void m68k_timing_init(void);
const uint16_t *m68k_timing_table(m68k_model model, int timed);
const m68k_timing *m68k_timing_of(m68k_model model, int timed);
/* What an instruction adds to its table count (0 untimed): a full-format
 * extension word `ext` of its EA in bits 5-0 (slot 0) or MOVE's
 * destination (1), BR = PC when `pc`; the conditions of m68k_timing.c's
 * Q_* flags; MOVEM's register mask. */
int m68k_timing_full(const m68k_cpu *cpu, uint16_t op, unsigned slot, uint16_t ext, int pc);
unsigned m68k_timing_slot(const m68k_cpu *cpu, uint32_t at);
int m68k_timing_query(const m68k_cpu *cpu, uint16_t op, unsigned flags);
int m68k_timing_movem(const m68k_timing *tm, uint16_t op, uint32_t mask);
enum {
    M68K_TQ_TAKEN = 1, M68K_TQ_EXPIRED = 2, M68K_TQ_TRAP = 4, M68K_TQ_REGARG = 8, M68K_TQ_BIG = 16, M68K_TQ_SPAN = 32,
    M68K_TQ_FAIL = 64, M68K_TQ_CHK2 = 128, M68K_TQ_AN = 256, M68K_TQ_WRAP = 512, M68K_TQ_SIGNED = 1024,
    M68K_TQ_WRITE = 2048, M68K_TQ_CRB = 4096
};
/* An FPU instruction's whole count (it has none in the table). */
enum { M68K_FPT_GENERAL, M68K_FPT_OUT, M68K_FPT_CTRL, M68K_FPT_DATA, M68K_FPT_BCC, M68K_FPT_DBCC, M68K_FPT_SCC,
       M68K_FPT_TRAPCC, M68K_FPT_SAVE, M68K_FPT_RESTORE };
int m68k_timing_fpu(const m68k_cpu *cpu, uint32_t op, uint32_t ext, unsigned kind, unsigned n, int cond);
/* An MMU instruction's whole count (68030, MC68851). */
enum { M68K_PMT_TO_RP, M68K_PMT_FROM_RP, M68K_PMT_TO_TT, M68K_PMT_FROM_TT, M68K_PMT_TO_SR, M68K_PMT_FROM_SR,
       M68K_PMT_TO_TC, M68K_PMT_FROM_TC, M68K_PMT_FLUSHA, M68K_PMT_FLUSH, M68K_PMT_LOAD, M68K_PMT_TEST,
       M68K_PMT_TEST_WALK, M68K_PMT_NONE };
int m68k_timing_pmmu(const m68k_cpu *cpu, uint32_t op, unsigned kind);
unsigned m68k_pmmu_kind(uint32_t ext);

/* Counts passing outside an instruction (exception processing between
 * instructions) and an instruction's own at its end. */
M68K_INLINE void m68k_charge(m68k_cpu *cpu, int64_t c) {
    cpu->cycles += (uint64_t)c;
    cpu->budget -= c;
}
/* An instruction's count, at its end: its table count and what it added. */
M68K_INLINE void m68k_retire(m68k_cpu *cpu, uint32_t op) {
    const int64_t c = (int64_t)cpu->ctab[op] + cpu->cyc_extra;
    cpu->cyc_extra = 0;
    cpu->cycles += (uint64_t)c;
    cpu->budget -= c;
    cpu->instructions++;
}

/* --- the translator (jit_x64.c) ------------------------------------------ */

/* Runs translated blocks from cpu->pc while it can: returns 1 when it ran
 * (the run loop then looks at events and the budget again), 0 when the
 * interpreter is to run the next instruction. */
int m68k_jit_exec(m68k_cpu *cpu);
/* A write reached host page `page` (its first byte) at `offset` through
 * the slow path: translations of code there are dropped. */
void m68k_jit_written(m68k_cpu *cpu, const uint8_t *page, uint32_t offset, unsigned size);
/* Whether host page `page` holds translated code (its writes must take the
 * slow path). */
int m68k_jit_is_code(m68k_cpu *cpu, const uint8_t *page);
void m68k_jit_free(m68k_cpu *cpu);
/* For the blocks' helpers (m68k_run.c): a read through the slow path that
 * records a fault instead of aborting the instruction (returns nonzero),
 * and the access error of such a fault, taken in the instruction's place. */
int m68k_try_read(m68k_cpu *cpu, uint32_t address, unsigned size, uint32_t *value);
void m68k_jit_fault(m68k_cpu *cpu);
/* The instruction at cpu->pc run by the interpreter for a block, counted,
 * its faults taken (m68k_run.c). */
void m68k_jit_step(m68k_cpu *cpu);
/* Idle loops through traps: an A-line instruction about to be taken (the
 * loop's anchor, cpu->ipc); a write about to change `size` bytes at host
 * `p` while writes are logged; the end of the slice, which ends a logged
 * iteration. */
void m68k_jit_trap(m68k_cpu *cpu);
void m68k_jit_wlog(m68k_cpu *cpu, const uint8_t *p, unsigned size);
void m68k_jit_wlog_end(m68k_cpu *cpu);

/* --- memory management (mmu.c, mmu040.c, mmu030.c) ----------------------- */

/* Soft-TLB tables: kind 0 program, 1 read, 2 write, 3 write held back from
 * the fast path because the page holds translated code (the translator's
 * write protection: the slow path writes through it without a new
 * translation), 4 and 5 read and write of pages that are not memory
 * (device registers, nothing): not pointers but the physical page, tagged
 * (TLB_DEV), which the slow path uses instead of a new translation; s 0
 * user, 1 supervisor. */
#define TLB_TABLE(kind, s) ((kind) * 2u + (s))
#define TLB_TABLES 12u
/* A kind 4/5 entry: the physical page's base | 1, | 2 when it came from an
 * ATC entry (a translation then advances the replacement counter, which
 * using the entry repeats). */
#define TLB_DEV_VALID 1u
#define TLB_DEV_ATC 2u
/* A translation (logical address `la`, function code `fc`): 0 with the
 * physical address in *pa and, in *fill, which soft-TLB tables may map the
 * page (bit 0 read or program, bit 1 write, bit 7 the fill belongs to the
 * list rather than an ATC entry); nonzero for a fault the MMU detected
 * (cpu->fault.atc says it was the MMU). */
/* Keeps the next two instruction words as fetched now (before a PMOVE to
 * TC takes effect: m68k.h, pf_addr). */
void m68k_prefetch_hold(m68k_cpu *cpu);
int m68k_mmu_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill);
/* After a write to an MMU register, a reset, a flush or new bus maps: picks
 * the maps and empties what the soft TLB holds. */
void m68k_mmu_changed(m68k_cpu *cpu);
void m68k_mmu_tlb_flush(m68k_cpu *cpu);
void m68k_mmu_tlb_fill(m68k_cpu *cpu, uint32_t la, unsigned fc, int ifetch, uint32_t pa, unsigned fill);
/* Removes the soft-TLB pages an ATC entry gave (68040: page size from TC). */
void m68k_mmu_tlb_drop(m68k_cpu *cpu, const m68k_atc_entry *e, int instruction, unsigned page_shift);
/* Moves the write mappings of host page `page` between the fast path's
 * write tables and the held-back ones (kind 3): `protect` 1 when the page
 * gets translated code, 0 when it has none left (page NULL: every page). */
void m68k_mmu_tlb_protect(m68k_cpu *cpu, const uint8_t *page, int protect);
/* Physical accesses of the table search (no translation). */
int m68k_phys_read32(m68k_cpu *cpu, uint32_t pa, uint32_t *v);
int m68k_phys_write32(m68k_cpu *cpu, uint32_t pa, uint32_t v);

int m68k_mmu040_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill);
void m68k_mmu040_pflush(m68k_cpu *cpu, unsigned opmode, uint32_t la);
void m68k_mmu040_ptest(m68k_cpu *cpu, uint32_t la, int write);
int m68k_mmu040_active(const m68k_cpu *cpu);
int m68k_mmu040_data_steps(const m68k_cpu *cpu, uint32_t la, unsigned s);
int m68k_mmu030_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill);
int m68k_mmu030_active(const m68k_cpu *cpu);
void m68k_op_pmmu030(m68k_cpu *cpu, uint32_t op);
/* What a table search of the 68030's (and the MC68851's) tables found
 * (MC68030 UM 9.5.2, 9.7.4; MC68851 UM 5.4, 6.1.8). */
typedef struct m68k_walk {
    uint32_t pa;        /* page frame of the logical page */
    unsigned status;    /* MMUSR/PSR bits B, L, S, W, I, M */
    unsigned levels;    /* descriptors fetched from tables */
    uint32_t last;      /* physical address of the last descriptor fetched */
    unsigned ci;
    unsigned g, lock, sg;   /* MC68851: the page's G and L bits; SG of the root pointer or a long descriptor */
} m68k_walk;
/* The search through TC (tc30), CRP and SRP (mmu030.c). */
void m68k_mmu030_search(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int update, unsigned max_levels, m68k_walk *w);
/* The MC68851 beside the 68020 (mmu851.c): translation and every
 * coprocessor 0 opcode word ($F000-$F1FF). */
int m68k_mmu851_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill);
int m68k_mmu851_active(const m68k_cpu *cpu);
void m68k_op_pmmu851(m68k_cpu *cpu, uint32_t op);

/* --- SR and condition codes -------------------------------------------- */

M68K_INLINE unsigned ccr_of(const m68k_cpu *cpu) {
    return ((cpu->fx & 1u) << 4) | ((cpu->fn >> 31) << 3) | ((cpu->fz == 0) << 2) | ((cpu->fv >> 31) << 1) |
           (cpu->fc & 1u);
}
M68K_INLINE void ccr_set(m68k_cpu *cpu, unsigned ccr) {
    cpu->fx = (ccr >> 4) & 1u;
    cpu->fn = (ccr & 8u) << 28;
    cpu->fz = !(ccr & 4u);
    cpu->fv = (ccr & 2u) << 30;
    cpu->fc = ccr & 1u;
}

/* Condition tests (PRM section 3, "Conditional Tests" table). */
M68K_INLINE int cond(const m68k_cpu *cpu, unsigned cc) {
    const unsigned n = cpu->fn >> 31, z = cpu->fz == 0, v = cpu->fv >> 31, c = cpu->fc & 1u;
    switch (cc & 15u) {
    case 0: return 1;               /* T */
    case 1: return 0;               /* F */
    case 2: return !c && !z;        /* HI */
    case 3: return c || z;          /* LS */
    case 4: return !c;              /* CC */
    case 5: return c;               /* CS */
    case 6: return !z;              /* NE */
    case 7: return z;               /* EQ */
    case 8: return !v;              /* VC */
    case 9: return v;               /* VS */
    case 10: return !n;             /* PL */
    case 11: return n;              /* MI */
    case 12: return n == v;         /* GE */
    case 13: return n != v;         /* LT */
    case 14: return !z && n == v;   /* GT */
    default: return z || n != v;    /* LE */
    }
}

/* --- sizes --------------------------------------------------------------- */

M68K_INLINE uint32_t sz_mask(unsigned sz) { return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu; }
M68K_INLINE uint32_t sz_sext(uint32_t v, unsigned sz) {
    return sz == 1 ? (uint32_t)(int32_t)(int8_t)v : sz == 2 ? (uint32_t)(int32_t)(int16_t)v : v;
}
M68K_INLINE unsigned sz_shift(unsigned sz) { return 32u - 8u * sz; }   /* puts the operand's msb at bit 31 */
M68K_INLINE void set_dn(m68k_cpu *cpu, unsigned n, uint32_t v, unsigned sz) {
    if (sz == 4) D(n) = v;
    else if (sz == 2) D(n) = (D(n) & 0xFFFF0000u) | (v & 0xFFFFu);
    else D(n) = (D(n) & 0xFFFFFF00u) | (v & 0xFFu);
}
/* N and Z from a result; V and C cleared (the logical-operation rule). */
M68K_INLINE void flags_logic(m68k_cpu *cpu, uint32_t v, unsigned sz) {
    cpu->fn = v << sz_shift(sz);
    cpu->fz = v & sz_mask(sz);
    cpu->fv = 0;
    cpu->fc = 0;
}

/* --- memory ------------------------------------------------------------- */

M68K_INLINE uint32_t fetch16(m68k_cpu *cpu) {
    const uint32_t pc = cpu->pc;
    const uint8_t *p = cpu->pmap[pc >> M68K_PAGE_SHIFT];
    cpu->pc = pc + 2;
    if (M68K_LIKELY(p != NULL)) return ld16(p + (pc & (M68K_PAGE_SIZE - 1)));
    return m68k_slow_read(cpu, pc, 2, cpu->fc_prog);
}
M68K_INLINE uint32_t fetch32(m68k_cpu *cpu) {
    const uint32_t pc = cpu->pc;
    const uint8_t *p = cpu->pmap[pc >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL && (pc & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - 4)) {
        cpu->pc = pc + 4;
        return ld32(p + (pc & (M68K_PAGE_SIZE - 1)));
    }
    const uint32_t hi = fetch16(cpu);
    return (hi << 16) | fetch16(cpu);
}

M68K_INLINE uint32_t rd8(m68k_cpu *cpu, uint32_t a) {
    const uint8_t *p = cpu->rmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL)) return p[a & (M68K_PAGE_SIZE - 1)];
    return m68k_slow_read(cpu, a, 1, cpu->fc_data);
}
M68K_INLINE uint32_t rd16(m68k_cpu *cpu, uint32_t a) {
    const uint8_t *p = cpu->rmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL && (a & (M68K_PAGE_SIZE - 1)) != M68K_PAGE_SIZE - 1))
        return ld16(p + (a & (M68K_PAGE_SIZE - 1)));
    return m68k_slow_read(cpu, a, 2, cpu->fc_data);
}
M68K_INLINE uint32_t rd32(m68k_cpu *cpu, uint32_t a) {
    const uint8_t *p = cpu->rmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL && (a & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - 4))
        return ld32(p + (a & (M68K_PAGE_SIZE - 1)));
    return m68k_slow_read(cpu, a, 4, cpu->fc_data);
}
M68K_INLINE void wr8(m68k_cpu *cpu, uint32_t a, uint32_t v) {
    uint8_t *p = cpu->wmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL)) p[a & (M68K_PAGE_SIZE - 1)] = (uint8_t)v;
    else m68k_slow_write(cpu, a, 1, cpu->fc_data, v & 0xFFu);
}
M68K_INLINE void wr16(m68k_cpu *cpu, uint32_t a, uint32_t v) {
    uint8_t *p = cpu->wmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL && (a & (M68K_PAGE_SIZE - 1)) != M68K_PAGE_SIZE - 1))
        st16(p + (a & (M68K_PAGE_SIZE - 1)), v);
    else m68k_slow_write(cpu, a, 2, cpu->fc_data, v & 0xFFFFu);
}
M68K_INLINE void wr32(m68k_cpu *cpu, uint32_t a, uint32_t v) {
    uint8_t *p = cpu->wmap[a >> M68K_PAGE_SHIFT];
    if (M68K_LIKELY(p != NULL && (a & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - 4))
        st32(p + (a & (M68K_PAGE_SIZE - 1)), v);
    else m68k_slow_write(cpu, a, 4, cpu->fc_data, v);
}
M68K_INLINE uint32_t rd(m68k_cpu *cpu, uint32_t a, unsigned sz) {
    return sz == 1 ? rd8(cpu, a) : sz == 2 ? rd16(cpu, a) : rd32(cpu, a);
}
M68K_INLINE void wr(m68k_cpu *cpu, uint32_t a, unsigned sz, uint32_t v) {
    if (sz == 1) wr8(cpu, a, v);
    else if (sz == 2) wr16(cpu, a, v);
    else wr32(cpu, a, v);
}
/* The read of a read-modify-write (TAS, CAS, CAS2): the MMU treats it as a
 * write (MC68040 UM 8.4.6.7: the data memory unit considers the read portion
 * of a read-modify-write transfer a write), so the fast path is taken only
 * where the page may be written. */
uint32_t m68k_slow_read_locked(m68k_cpu *cpu, uint32_t address, unsigned size);
M68K_INLINE uint32_t rd_locked(m68k_cpu *cpu, uint32_t a, unsigned sz) {
    const uint8_t *p = cpu->wmap[a >> M68K_PAGE_SHIFT];
    const uint32_t o = a & (M68K_PAGE_SIZE - 1);
    if (M68K_LIKELY(p != NULL && o <= M68K_PAGE_SIZE - sz)) return sz == 1 ? p[o] : sz == 2 ? ld16(p + o) : ld32(p + o);
    return m68k_slow_read_locked(cpu, a, sz);
}
M68K_INLINE void push32(m68k_cpu *cpu, uint32_t v) {
    A(7) -= 4;
    wr32(cpu, A(7), v);
}

/* --- effective addresses (PRM 2.2) ---------------------------------------- */

M68K_INLINE void undo_push(m68k_cpu *cpu, unsigned reg, uint32_t old) {
    const unsigned n = cpu->undo_n;
    cpu->undo_reg[n & 3u] = (uint8_t)reg;
    cpu->undo_val[n & 3u] = old;
    cpu->undo_n = (uint8_t)(n + 1);
}
/* (An)+ and -(An) step by the operand size, by 2 for a byte through A7 (PRM
 * 2.2.4, 2.2.5). */
M68K_INLINE uint32_t ea_step(unsigned reg, unsigned sz) { return (reg == 7 && sz == 1) ? 2u : sz; }

/* The address of a memory operand; mode and reg from the instruction. */
M68K_INLINE uint32_t ea_addr(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz) {
    switch (mode) {
    case 2: return A(reg);
    case 3: {
        const uint32_t a = A(reg);
        undo_push(cpu, 8 + reg, a);
        A(reg) = a + ea_step(reg, sz);
        return a;
    }
    case 4: {
        const uint32_t a = A(reg) - ea_step(reg, sz);
        undo_push(cpu, 8 + reg, A(reg));
        A(reg) = a;
        return a;
    }
    case 5: return A(reg) + (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    case 6: return m68k_ea_index(cpu, A(reg), 0);
    default:
        switch (reg) {
        case 0: return (uint32_t)(int32_t)(int16_t)fetch16(cpu);
        case 1: return fetch32(cpu);
        case 2: { const uint32_t pc = cpu->pc; return pc + (uint32_t)(int32_t)(int16_t)fetch16(cpu); }
        case 3: return m68k_ea_index(cpu, cpu->pc, 1);
        default: m68k_abort(cpu, ABORT_ILLEGAL);
        }
    }
}
M68K_INLINE uint32_t imm(m68k_cpu *cpu, unsigned sz) {
    return sz == 4 ? fetch32(cpu) : sz == 2 ? fetch16(cpu) : (fetch16(cpu) & 0xFFu);
}
/* Reads a source operand of any mode. */
M68K_INLINE uint32_t ea_read(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz) {
    if (mode == 0) return D(reg) & sz_mask(sz);
    if (mode == 1) return A(reg) & sz_mask(sz);
    if (mode == 7 && reg == 4) return imm(cpu, sz);
    return rd(cpu, ea_addr(cpu, mode, reg, sz), sz);
}
/* Writes a data-alterable destination (Dn or memory). */
M68K_INLINE void ea_write(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned sz, uint32_t v) {
    if (mode == 0) set_dn(cpu, reg, v, sz);
    else wr(cpu, ea_addr(cpu, mode, reg, sz), sz, v);
}

#define OP_MODE(op) (((op) >> 3) & 7u)
#define OP_REG(op) ((op) & 7u)
#define OP_REG9(op) (((op) >> 9) & 7u)

/* --- arithmetic with flags (PRM section 4, condition codes of ADD, SUB, CMP,
 * ADDX, SUBX, NEG, NEGX). Operands are aligned to bit 31 so one formula
 * serves every size. ---------------------------------------------------- */

M68K_INLINE uint32_t alu_add(m68k_cpu *cpu, uint32_t s, uint32_t d, unsigned sz) {
    const unsigned sh = sz_shift(sz);
    const uint32_t ss = s << sh, dd = d << sh, r = dd + ss;
    cpu->fn = r;
    cpu->fz = r;
    cpu->fv = (ss ^ r) & (dd ^ r);
    cpu->fc = cpu->fx = r < dd;
    return r >> sh;
}
M68K_INLINE uint32_t alu_sub(m68k_cpu *cpu, uint32_t s, uint32_t d, unsigned sz) {
    const unsigned sh = sz_shift(sz);
    const uint32_t ss = s << sh, dd = d << sh, r = dd - ss;
    cpu->fn = r;
    cpu->fz = r;
    cpu->fv = (ss ^ dd) & (r ^ dd);
    cpu->fc = cpu->fx = ss > dd;
    return r >> sh;
}
M68K_INLINE void alu_cmp(m68k_cpu *cpu, uint32_t s, uint32_t d, unsigned sz) {
    const unsigned sh = sz_shift(sz);
    const uint32_t ss = s << sh, dd = d << sh, r = dd - ss;
    cpu->fn = r;
    cpu->fz = r;
    cpu->fv = (ss ^ dd) & (r ^ dd);
    cpu->fc = ss > dd;
}
/* ADDX/SUBX/NEGX: X is an input; Z is only cleared (PRM, ADDX). */
M68K_INLINE uint32_t alu_addx(m68k_cpu *cpu, uint32_t s, uint32_t d, unsigned sz) {
    const unsigned sh = sz_shift(sz);
    const uint32_t ss = s << sh, dd = d << sh;
    const uint64_t t = (uint64_t)dd + ss + ((uint64_t)(cpu->fx & 1u) << sh);
    const uint32_t r = (uint32_t)t;
    cpu->fn = r;
    cpu->fz |= r;
    cpu->fv = (ss ^ r) & (dd ^ r);
    cpu->fc = cpu->fx = (uint32_t)(t >> 32) & 1u;
    return r >> sh;
}
M68K_INLINE uint32_t alu_subx(m68k_cpu *cpu, uint32_t s, uint32_t d, unsigned sz) {
    const unsigned sh = sz_shift(sz);
    const uint32_t ss = s << sh, dd = d << sh;
    const uint64_t t = (uint64_t)dd - ss - ((uint64_t)(cpu->fx & 1u) << sh);
    const uint32_t r = (uint32_t)t;
    cpu->fn = r;
    cpu->fz |= r;
    cpu->fv = (ss ^ dd) & (r ^ dd);
    cpu->fc = cpu->fx = (uint32_t)(t >> 32) & 1u;
    return r >> sh;
}

/* Handlers flag a change of flow for the T0 trace mode (PRM 1.3.1). */
M68K_INLINE void jump(m68k_cpu *cpu, uint32_t target) {
    cpu->pc = target;
    cpu->flow = 1;
}

#define PRIVILEGED()                       \
    do {                                   \
        if (!cpu->s) {                     \
            m68k_exception_privilege(cpu); \
            return;                        \
        }                                  \
    } while (0)

/* --- the decode table (m68k_table.c) -------------------------------------- */

typedef struct m68k_pattern {
    uint16_t mask, match;
    uint16_t ea;        /* allowed modes of the EA in bits 5-0 (EA_* bits), 0 = no EA there */
    uint16_t ea2;       /* allowed modes of the EA in bits 11-6 (MOVE destination) */
    uint8_t models;     /* MM_* */
    uint8_t prio;       /* a higher priority replaces a lower one (specialised handlers) */
    m68k_handler fn;
    const char *name;
} m68k_pattern;

extern const m68k_pattern m68k_patterns[];
extern const unsigned m68k_pattern_count;

/* EA class bits (PRM 2.2, "Effective Addressing Mode Categories" table). */
#define EA_DN 0x001u
#define EA_AN 0x002u
#define EA_AI 0x004u
#define EA_PI 0x008u
#define EA_PD 0x010u
#define EA_DI 0x020u
#define EA_IX 0x040u
#define EA_AW 0x080u
#define EA_AL 0x100u
#define EA_PCDI 0x200u
#define EA_PCIX 0x400u
#define EA_IMM 0x800u
#define EA_ALL 0xFFFu
#define EA_DATA (EA_ALL & ~EA_AN)
#define EA_MEM (EA_ALL & ~(EA_DN | EA_AN))
#define EA_CTRL (EA_AI | EA_DI | EA_IX | EA_AW | EA_AL | EA_PCDI | EA_PCIX)
#define EA_ALT (EA_DN | EA_AN | EA_AI | EA_PI | EA_PD | EA_DI | EA_IX | EA_AW | EA_AL)
#define EA_DALT (EA_ALT & ~EA_AN)
#define EA_MALT (EA_ALT & ~(EA_DN | EA_AN))
#define EA_CALT (EA_CTRL & EA_ALT)

#endif /* MC_M68K_INTERNAL_H */
