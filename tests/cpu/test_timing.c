/* mc_cpu_tests timing: the instruction timing tables (m68k_timing.c) spot
 * checked against Motorola's, per model. The expected values are worked
 * out here from the manuals' tables, independently of the module's
 * transcription: MC68040 UM section 10 (the longer of <ea> calculate and
 * execute), MC68030 UM section 11 (instruction-cache case, the operation
 * plus its effective-address table), MC68020 UM section 10 (cache case),
 * MC68881/MC68882 UM section 8 (Tables 8-1, 8-2, 8-3). Each instruction
 * runs alone on a timed CPU and its cycles are compared. */
#include "cpu_test.h"
#include "m68k_internal.h"
#include "../test.h"

#define CODE 0x1000u
#define DATA 0x4000u
#define STACK 0x8000u

static testbus bus;
static m68k_cpu cpu;

static void start(m68k_model model, m68k_fpu_type fpu, const uint16_t *words, unsigned n) {
    tb_init(&bus, 0x10000, 0xFFFFFFFFu, 0);
    tb_put(&bus, 0, 4, STACK);
    tb_put(&bus, 4, 4, CODE);
    for (unsigned v = 2; v < 64; v++) tb_put(&bus, 4 * v, 4, 0x3000);    /* every vector: a handler that stops */
    tb_put(&bus, 0x3000, 2, 0x60FE);
    for (unsigned i = 0; i < n; i++) tb_put(&bus, CODE + 2 * i, 2, words[i]);
    const m68k_bus b = tb_bus(&bus);
    m68k_init(&cpu, model, &b);
    m68k_set_fpu(&cpu, fpu, 0);
    m68k_set_timing(&cpu, 1);
    m68k_reset(&cpu);
    for (unsigned r = 0; r < 8; r++) cpu.r[r] = 2;
    for (unsigned r = 8; r < 15; r++) cpu.r[r] = DATA + 0x100 * (r - 8);
}

/* The cycles of the first instruction of `words`. */
static unsigned one(m68k_model model, m68k_fpu_type fpu, const uint16_t *words, unsigned n) {
    start(model, fpu, words, n);
    const uint64_t c0 = cpu.cycles;
    m68k_run(&cpu, 1);
    const unsigned c = (unsigned)(cpu.cycles - c0);
    tb_free(&bus);
    return c;
}

#define ONE(model, expect, ...)                                                                     \
    do {                                                                                            \
        static const uint16_t w_[] = {__VA_ARGS__};                                                 \
        const unsigned got_ = one(model, M68K_FPU_NONE, w_, sizeof w_ / 2);                         \
        if (got_ != (expect)) {                                                                     \
            fprintf(stderr, "%s: %04X: %u cycles, expected %u (line %d)\n", m68k_model_name(model), \
                    w_[0], got_, (unsigned)(expect), __LINE__);                                     \
            mc_test_failures++;                                                                     \
        }                                                                                           \
    } while (0)
#define FP(model, fpu, expect, ...)                                                                 \
    do {                                                                                            \
        static const uint16_t w_[] = {__VA_ARGS__};                                                 \
        const unsigned got_ = one(model, fpu, w_, sizeof w_ / 2);                                   \
        if (got_ != (expect)) {                                                                     \
            fprintf(stderr, "%s FPU %d: %04X %04X: %u cycles, expected %u (line %d)\n",             \
                    m68k_model_name(model), (int)(fpu), w_[0], w_[1], got_, (unsigned)(expect),      \
                    __LINE__);                                                                      \
            mc_test_failures++;                                                                     \
        }                                                                                           \
    } while (0)

/* The cycles of `iterations` turns of a loop of `words`, run from CODE. */
static uint64_t loop(m68k_model model, const uint16_t *words, unsigned n, unsigned per_turn, uint64_t turns, uint32_t d2) {
    start(model, M68K_FPU_NONE, words, n);
    cpu.r[2] = d2;
    cpu.r[3] = 0;
    const uint64_t c0 = cpu.cycles, i0 = cpu.instructions;
    while (cpu.instructions - i0 < per_turn * turns) m68k_run(&cpu, 1);
    const uint64_t c = cpu.cycles - c0;
    tb_free(&bus);
    return c;
}

static void test_040(void) {
    ONE(M68K_68040, 2, 0x013C, 0x0001);     /* BTST D0,#1: the Dn row (10.6 has no #<xxx> row) */
    const m68k_model M = M68K_68040;
    /* 10.4 MOVE */
    ONE(M, 1, 0x2200);                      /* MOVE.L D0,D1: Dn,Dn 1/1 */
    ONE(M, 1, 0x2210);                      /* MOVE.L (A0),D1: 1/1 */
    ONE(M, 2, 0x22D8);                      /* MOVE.L (A0)+,(A1)+: 2/1L+1 */
    ONE(M, 4, 0x22B0, 0x0000);              /* MOVE.L (d8,A0,D0),(A1): 4/4 */
    ONE(M, 7, 0x2230, 0x0120, 0x0010);      /* MOVE.L (bd16,A0,D0),D1: (b16,BR,Xn) 7/1L+6; full format */
    /* 10.5 */
    ONE(M, 3, 0x6702);                      /* BEQ.S not taken (Z clear): 3/3 */
    ONE(M, 2, 0x6002);                      /* BRA.S: 2/2 */
    ONE(M, 2, 0x6100, 0x0010);              /* BSR: 2/1L+1 */
    ONE(M, 4, 0x50C8, 0x0010);              /* DBT: cc true 4/4 */
    ONE(M, 3, 0x51C8, 0x0010);              /* DBF D0 (D0.W = 2): false, count > -1, 3/3 */
    ONE(M, 8, 0x4E71);                      /* NOP: 8/1L+7 */
    ONE(M, 5, 0x4E75);                      /* RTS: 5/5 */
    ONE(M, 2, 0x4840);                      /* SWAP: 1/2 */
    ONE(M, 16, 0x4E40);                     /* TRAP #0: 16/16 */
    ONE(M, 16, 0xA000);                     /* A-line: 16/16 */
    /* 10.6 */
    ONE(M, 1, 0x0C40, 0x000A);              /* CMPI.W #10,D0: 1/1 */
    ONE(M, 1, 0x5382);                      /* SUBQ.L #1,D2: 1/1 */
    ONE(M, 3, 0xD0BA, 0x0010);              /* ADD.L (d16,PC),D0: 3/2L+1 */
    ONE(M, 2, 0xD1C0);                      /* ADDA.L D0,A0: 1/2 */
    ONE(M, 14, 0xC0C1);                     /* MULU.W D1,D0: 1/14 */
    ONE(M, 16, 0xC1C1);                     /* MULS.W D1,D0: 1/16 */
    ONE(M, 20, 0x4C01, 0x0000);             /* MULU.L D1,D0: 1/20 */
    ONE(M, 27, 0x80C1);                     /* DIVU.W D1,D0: 8/27 */
    ONE(M, 44, 0x4C41, 0x0000);             /* DIVU.L D1,D0: 9/44 */
    ONE(M, 2, 0xE388);                      /* LSL.L #1,D0: immediate count 1/2 */
    ONE(M, 3, 0xE3A8);                      /* LSL.L D1,D0: register count 1/3 */
    ONE(M, 1, 0x43D0);                      /* LEA (A0),A1: 1/1 */
    ONE(M, 3, 0x4E90);                      /* JSR (A0): 3/2L+1 */
    ONE(M, 4, 0x48E7, 0xC000);              /* MOVEM.L D0-D1,-(A7): 2+D'+A' = 4 */
    ONE(M, 6, 0x4CDF, 0x0007);              /* MOVEM.L (A7)+,D0-D2: 3 + D' 3 */
    ONE(M, 6, 0x4CDF, 0x0300);              /* MOVEM.L (A7)+,A0-A1: 3 + D' 1 (none: one) + A' 2 */
    /* The power-on VIA test of the Quadra 800 ROM (ROM offset $48144):
     * CMPI.W, BEQ not taken, SUBQ.L, BNE taken, 1 + 3 + 1 + 2 = 7 clocks. */
    {
        static const uint16_t w[] = {0x0C43, 0x000A, 0x6704, 0x5382, 0x66F6, 0x60FE};
        const uint64_t c = loop(M, w, 6, 4, 1000, 100000);
        if (c != 7000) {
            fprintf(stderr, "68040 timing loop: %llu cycles for 1000 turns, expected 7000\n", (unsigned long long)c);
            mc_test_failures++;
        }
    }
    /* 10.7: FADD FP1,FP0, the integer unit's FPn row 2/1L+2 and the FPU's
     * 2 + 3 + 2; FMOVE.D (A0),FP0, 2/2 and 3 (D source, X precision). */
    FP(M, M68K_FPU_68040, 10, 0xF200, 0x0422);
    FP(M, M68K_FPU_68040, 5, 0xF210, 0x5400);
    /* The 68LC040: line F, the exception's 16. */
    FP(M68K_68LC040, M68K_FPU_NONE, 16, 0xF200, 0x0422);
}

static void test_030(void) {
    const m68k_model M = M68K_68030;
    ONE(M, 2, 0x2200);                      /* MOVE.L D0,D1: Rn,Dn 2 */
    ONE(M, 5, 0x2210);                      /* MOVE.L (A0),D1: EA,Dn 2 + fea (An) 3 */
    ONE(M, 3, 0x2280);                      /* MOVE.L D0,(A1): Rn,(An) 3 */
    ONE(M, 7, 0x2290);                      /* MOVE.L (A0),(A1): SOURCE,(An) 4 + 3 */
    ONE(M, 6, 0x203C, 0x1234, 0x5678);      /* MOVE.L #imm,D0: 2 + fea #.L 4 */
    ONE(M, 5, 0xD090);                      /* ADD.L (A0),D0: 2 + 3 */
    ONE(M, 6, 0xD190);                      /* ADD.L D0,(A0): 3 + 3 */
    ONE(M, 6, 0x0680, 0x0000, 0x0001);      /* ADDI.L #1,D0: 2 + fiea #.L,Dn 4 */
    ONE(M, 4, 0x6702);                      /* BEQ.S not taken: 4 */
    ONE(M, 6, 0x6700, 0x0010);              /* BEQ.W not taken: 6 */
    ONE(M, 6, 0x6002);                      /* BRA.S: 6 */
    ONE(M, 6, 0x50C8, 0x0010);              /* DBT: 6 */
    ONE(M, 6, 0x51C8, 0x0010);              /* DBF D0 (2): count not expired 6 */
    ONE(M, 28, 0xC0C1);                     /* MULU.W D1,D0: 28 */
    ONE(M, 46, 0x4C01, 0x0000);             /* MULU.L D1,D0: 44 + fiea #.W,Dn 2 */
    ONE(M, 44, 0x80C1);                     /* DIVU.W D1,D0: 44 */
    ONE(M, 56, 0x81C1);                     /* DIVS.W D1,D0: 56 */
    ONE(M, 80, 0x4C41, 0x0000);             /* DIVU.L D1,D0: 78 + 2 */
    ONE(M, 92, 0x4C41, 0x0800);             /* DIVS.L D1,D0: 90 + 2 */
    ONE(M, 2, 0x4E71);                      /* NOP: 2 */
    ONE(M, 9, 0x4E75);                      /* RTS: 9 */
    ONE(M, 6, 0x4E90);                      /* JSR (A0): 4 + jea 2 */
    ONE(M, 4, 0x43D0);                      /* LEA (A0),A1: 2 + cea 2 */
    ONE(M, 4, 0xE388);                      /* LSL.L #1,D0: 4 */
    ONE(M, 6, 0xE380);                      /* ASL.L #1,D0: 6 */
    ONE(M, 14, 0x48E7, 0xF000);             /* MOVEM.L D0-D3,-(A7): 4 + 2n + ciea #.W,-(An) 2 */
    ONE(M, 28, 0x4CDF, 0x000F);             /* MOVEM.L (A7)+,D0-D3: 8 + 4n + ciea #.W,(An)+ 4 */
    ONE(M, 18, 0x4E40);                     /* TRAP #0: 18 */
    ONE(M, 18, 0x4AFC);                     /* ILLEGAL: 18 */
    ONE(M, 4, 0x4E76);                      /* TRAPV, no trap: 4 */
    /* Bcc taken: 6 for the turn's BNE; with SUBQ 2, a DBRA loop of 6. */
    {
        static const uint16_t w[] = {0x5382, 0x66FC, 0x60FE};
        const uint64_t c = loop(M, w, 3, 2, 1000, 100000);
        if (c != 8000) {
            fprintf(stderr, "68030 SUBQ/BNE loop: %llu cycles for 1000 turns, expected 8000\n", (unsigned long long)c);
            mc_test_failures++;
        }
    }
    /* 68882 beside a 68030 (Table 8-3): FADD FP1,FP0 56; FMOVE.D (A0),FP0
     * 40 + EA (An) 2. */
    FP(M, M68K_FPU_68882, 56, 0xF200, 0x0422);
    FP(M, M68K_FPU_68882, 42, 0xF210, 0x5400);
    FP(M, M68K_FPU_68881, 51, 0xF200, 0x0422);
    FP(M, M68K_FPU_68881, 60, 0xF210, 0x5400);
    FP(M, M68K_FPU_68881, 103, 0xF200, 0x0420);   /* FDIV FP1,FP0: 103 (Table 8-2) */
}

static void test_020(void) {
    const m68k_model M = M68K_68020;
    ONE(M, 2, 0x2200);                      /* MOVE.L D0,D1: Rn,Dn 2 */
    ONE(M, 6, 0x2210);                      /* MOVE.L (A0),D1: (An),Dn 6 */
    ONE(M, 4, 0x2280);                      /* MOVE.L D0,(A1): Rn,(An) 4 */
    ONE(M, 7, 0x2290);                      /* MOVE.L (A0),(A1): 7 */
    ONE(M, 6, 0xD090);                      /* ADD.L (A0),D0: 2 + fea 4 */
    ONE(M, 8, 0xD190);                      /* ADD.L D0,(A0): 4 + 4 */
    ONE(M, 27, 0xC0C1);                     /* MULU.W: 27 */
    ONE(M, 44, 0x80C1);                     /* DIVU.W: 44 */
    ONE(M, 2, 0x4E71);                      /* NOP: 2 */
    ONE(M, 10, 0x4E75);                     /* RTS: 10 */
    ONE(M, 7, 0x6100, 0x0010);              /* BSR: 7 */
    ONE(M, 4, 0x6702);                      /* BEQ.S not taken: 4 */
    ONE(M, 7, 0x4E90);                      /* JSR (A0): 5 + jea 2 */
    ONE(M, 18, 0x48E7, 0xF000);             /* MOVEM.L D0-D3,-(A7): 4 + 3n + ciea 2 */
    ONE(M, 20, 0x4E40);                     /* TRAP #0: 20 */
    ONE(M, 6, 0xE3A8);                      /* LSL.L D1,D0: dynamic 6 */
    ONE(M, 4, 0xE388);                      /* LSL.L #1,D0: static 4 */
}

/* What is charged apart from the table: the exception (ILLEGAL, TRAP,
 * BKPT where it traps), RTE by its frame, the coprocessor dispatchers. */
static int charged_apart(const char *name) {
    static const char *const apart[] = {"ILLEGAL", "TRAP", "BKPT", "RTE", "FPU", "FSAVE/FRESTORE", "PMMU"};
    for (size_t i = 0; i < sizeof apart / sizeof *apart; i++)
        if (!strcmp(name, apart[i])) return 1;
    return 0;
}
/* Every other opcode a model decodes costs at least a cycle: guest time
 * always moves, and a one-cycle slice runs one instruction. */
static void test_nonzero(void) {
    static const m68k_model models[] = {M68K_68020, M68K_68030, M68K_68040, M68K_68LC040};
    for (unsigned i = 0; i < sizeof models / sizeof *models; i++) {
        const uint16_t *t = m68k_timing_table(models[i], 1);
        unsigned zero = 0, first = 0;
        for (unsigned op = 0; op < 65536; op++)
            if (m68k_decode(models[i], (uint16_t)op).valid && !t[op] && !charged_apart(m68k_decode(models[i], (uint16_t)op).name) &&
                !zero++)
                first = op;
        if (zero) {
            fprintf(stderr, "%s: %u decoded opcodes cost 0 cycles (first %04X)\n", m68k_model_name(models[i]), zero, first);
            mc_test_failures++;
        }
    }
}

int mc_cpu_timing_unit(void) {
    test_nonzero();
    test_040();
    test_030();
    test_020();
    printf("timing: %d failure(s)\n", mc_test_failures);
    return mc_test_failures;
}
