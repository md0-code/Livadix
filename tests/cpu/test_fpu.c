/* mc_cpu_tests fpu: the FPU's programming model, special values,
 * conditionals, FMOVEM, the exceptions and their frames, FSAVE/FRESTORE per
 * model, the 68040's traps and the 68LC040. Expected values from the
 * MC68881/MC68882 User's Manual and the MC68040 User's Manual section 9 as
 * cited per case; numeric accuracy is tests/fpu_suite.py's job. */
#include "cpu_test.h"
#include "../test.h"
#include "fpu_soft.h"

#define CODE 0x1000u
#define STACK 0x8000u
#define HANDLERS 0x10000u
#define DATA 0x3000u

typedef struct env {
    testbus bus;
    m68k_cpu *cpu;
    unsigned vectors[8], nvec;
} env;

static void hook(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc) {
    env *e = ctx;
    (void)cpu;
    (void)pc;
    if (e->nvec < 8) e->vectors[e->nvec] = vector;
    e->nvec++;
}

static env *setup(m68k_model model, m68k_fpu_type fpu, const uint16_t *code, size_t words) {
    env *e = calloc(1, sizeof *e);
    tb_init(&e->bus, 1u << 20, 0xFFFFFFFFu, 0);
    for (unsigned v = 0; v < 256; v++) {
        tb_put(&e->bus, 4 * v, 4, HANDLERS + 16 * v);
        tb_put(&e->bus, HANDLERS + 16 * v, 2, 0x4E71);
    }
    for (size_t i = 0; i < words; i++) tb_put(&e->bus, CODE + 2 * (uint32_t)i, 2, code[i]);
    m68k_bus mb = tb_bus(&e->bus);
    e->cpu = calloc(1, sizeof *e->cpu);
    m68k_init(e->cpu, model, &mb);
    if (fpu != (m68k_fpu_type)-1) m68k_set_fpu(e->cpu, fpu, 0);
    m68k_reset(e->cpu);
    e->cpu->isp = STACK;
    e->cpu->r[15] = STACK;
    e->cpu->pc = CODE;
    e->cpu->on_exception = hook;
    e->cpu->hook_ctx = e;
    return e;
}
static void done(env *e) {
    tb_free(&e->bus);
    m68k_free(e->cpu);
    free(e->cpu);
    free(e);
}
#define SETUP(model, fput, ...)                                             \
    static const uint16_t code_[] = {__VA_ARGS__};                         \
    env *e = setup(model, fput, code_, sizeof code_ / sizeof code_[0]);     \
    m68k_cpu *cpu = e->cpu;                                                \
    m68k_fpu *f = &cpu->fpu;                                               \
    (void)f
#define RUN(n) m68k_run(cpu, (n))
#define D(n) cpu->r[(n)]
#define A(n) cpu->r[8 + (n)]
#define MEM(a, size) tb_get(&e->bus, (a), (size))
#define PUT(a, size, v) tb_put(&e->bus, (a), (size), (v))
#define HANDLER(v) (HANDLERS + 16u * (v))
#define FP040 M68K_FPU_68040
#define CC() (f->fpsr >> 24 & 15u)
#define EXC() (f->fpsr & 0xFF00u)

static m68k_fx80 X(double d) {
    uint64_t b;
    memcpy(&b, &d, 8);
    return fx_to_x(fx_from_d(b));
}
static double DBL(m68k_fx80 r) {
    unsigned exc = 0;
    const uint64_t b = fx_to_d(fx_round(fx_from_x(r), &FMT_D, RND_N, &exc));
    double d;
    memcpy(&d, &b, 8);
    return d;
}
static const m68k_fx80 SNAN = {0xA000000000000000ull, 0x7FFF};
static const m68k_fx80 QNAN1 = {0xC000000000001111ull, 0x7FFF};
static const m68k_fx80 QNAN2 = {0xC000000000002222ull, 0x7FFF};
static const m68k_fx80 PINF = {0, 0x7FFF}, NINF = {0, 0xFFFF};
static const m68k_fx80 DENORM = {0x0000000000000123ull, 0x0000};

/* --- special values (MC68881 UM 4.5.3, 4.5.4) --------------------------------- */

static void test_specials(void) {
    {   /* FADD FP1,FP0: two NaNs give the destination's; one SNaN sets SNAN
         * and is quieted; +inf + -inf is OPERR and the default NaN */
        SETUP(M68K_68040, FP040, 0xF200, 0x0422, 0xF200, 0x0422, 0xF200, 0x0422, 0xF200, 0x0420);
        f->fp[0] = QNAN1; f->fp[1] = QNAN2;
        RUN(1);
        CHECK(f->fp[0].m == QNAN1.m);
        CHECK_EQ(CC(), 1);
        f->fp[0] = X(1.0); f->fp[1] = SNAN;
        RUN(1);
        CHECK(f->fp[0].m == 0xE000000000000000ull);
        CHECK_EQ(EXC(), 0x4000);
        CHECK_EQ(f->fpsr & 0x80, 0x80);                        /* AEXC IOP */
        f->fp[0] = PINF; f->fp[1] = NINF;
        RUN(1);
        CHECK(f->fp[0].se == 0x7FFF && f->fp[0].m == ~0ull);
        CHECK_EQ(EXC(), 0x2000);
        f->fp[0] = X(1.0); f->fp[1] = X(0.0);                  /* FDIV 1/0: DZ, +inf */
        RUN(1);
        CHECK(f->fp[0].se == 0x7FFF && f->fp[0].m == 0);
        CHECK_EQ(EXC(), 0x0400);
        CHECK_EQ(CC(), 2);
        CHECK_EQ(f->fpsr & 0x10, 0x10);                        /* AEXC DZ */
        CHECK_EQ(f->fpiar, CODE + 12);
        done(e);
    }
    {   /* rounding precision: FADD in single rounds the mantissa to 24 bits;
         * x - x in RM is -0 */
        SETUP(M68K_68040, FP040, 0xF200, 0x0422, 0xF200, 0x0428);
        f->fpcr = 0x40;                                         /* single, RN */
        f->fp[0] = X(1.0); f->fp[1] = X(1.0 / 3.0);
        RUN(1);
        CHECK_EQ(f->fp[0].m, 0xAAAAAB0000000000ull);
        CHECK_EQ(EXC(), 0x0200);
        f->fpcr = 0x20;                                         /* extended, RM */
        f->fp[1] = f->fp[0];
        RUN(1);
        CHECK_EQ(f->fp[0].se, 0x8000);
        CHECK_EQ(CC(), 0xC);
        done(e);
    }
    {   /* FCMP: dst - src; FTST of -inf */
        SETUP(M68K_68040, FP040, 0xF200, 0x0438, 0xF200, 0x0438, 0xF200, 0x043A);
        f->fp[0] = X(1.0); f->fp[1] = X(2.0);
        RUN(1);
        CHECK_EQ(CC(), 8);
        f->fp[0] = X(2.0);
        RUN(1);
        CHECK_EQ(CC(), 4);
        f->fp[1] = NINF;
        RUN(1);
        CHECK_EQ(CC(), 0xA);
        done(e);
    }
    {   /* FMOVE.L FP1,D0 of a NaN: OPERR, the mantissa's top 32 bits; of a
         * large value: the largest integer (MC68881 UM 6.1.3) */
        SETUP(M68K_68040, FP040, 0xF200, 0x6080, 0xF200, 0x6080);
        f->fp[1] = QNAN1;
        RUN(1);
        CHECK_EQ(D(0), 0xC0000000);
        CHECK_EQ(EXC(), 0x2000);
        f->fp[1] = X(-1e12);
        RUN(1);
        CHECK_EQ(D(0), 0x80000000);
        done(e);
    }
    {   /* FMOD and FREM set the quotient byte (MC68881 UM 2.3.2; a 68882:
         * the 68040 leaves them to software) */
        SETUP(M68K_68030, M68K_FPU_68882, 0xF200, 0x0421, 0xF200, 0x0425);
        f->fp[0] = X(-7.0); f->fp[1] = X(2.0);
        RUN(1);
        CHECK(DBL(f->fp[0]) == -1.0);
        CHECK_EQ(f->fpsr >> 16 & 0xFF, 0x83);
        f->fp[0] = X(7.0); f->fp[1] = X(2.0);
        RUN(1);
        CHECK(DBL(f->fp[0]) == -1.0);                          /* 7 - 4 x 2 */
        CHECK_EQ(f->fpsr >> 16 & 0xFF, 0x04);
        done(e);
    }
}

/* --- conditionals (MC68040 UM Table 9-8) ---------------------------------------- */

static void test_conditionals(void) {
    {   /* FBGT on NaN: false, BSUN set (nonaware); FBOGT: false, no BSUN */
        SETUP(M68K_68040, FP040, 0xF292, 0x0010, 0xF282, 0x0010, 0x4E71);
        f->fpsr = 0x01000000;
        RUN(1);
        CHECK_EQ(cpu->pc, CODE + 4);
        CHECK_EQ(EXC(), 0x8000);
        CHECK_EQ(f->fpsr & 0x80, 0x80);
        f->fpsr = 0x01000000;
        RUN(1);
        CHECK_EQ(cpu->pc, CODE + 8);
        CHECK_EQ(EXC(), 0);
        done(e);
    }
    {   /* BSUN enabled: the conditional traps before it executes (vector 48,
         * format $0 at the instruction) */
        SETUP(M68K_68040, FP040, 0xF292, 0x0010);
        f->fpsr = 0x01000000;
        f->fpcr = 0x8000;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(48));
        CHECK_EQ(MEM(STACK - 6, 4), CODE);
        done(e);
    }
    {   /* FScc, FDBcc, FTRAPcc; FBEQ.L taken */
        SETUP(M68K_68040, FP040, 0xF240, 0x0001, 0xF248, 0x000E, 0xFFFC, 0xF2C1, 0x0000, 0x0010, 0xF27C, 0x000F);
        f->fpsr = 0x04000000;                                   /* Z */
        D(0) = 0x12345600;
        RUN(1);                                                 /* FSEQ D0 */
        CHECK_EQ(D(0) & 0xFF, 0xFF);
        D(0) = 2;
        RUN(1);                                                 /* FDBNE D0: Z set, NE false: decrement, loop */
        CHECK_EQ(D(0), 1);
        CHECK_EQ(cpu->pc, CODE + 4);                            /* the displacement word at +8, minus 4 */
        cpu->pc = CODE + 10;
        RUN(1);                                                 /* FBEQ.L */
        CHECK_EQ(cpu->pc, CODE + 12 + 0x10);
        cpu->pc = CODE + 16;
        RUN(1);                                                 /* FTRAPT */
        CHECK_EQ(cpu->pc, HANDLER(7));
        CHECK_EQ(MEM(STACK - 4, 4), CODE + 16);
        done(e);
    }
}

/* --- FMOVEM (PRM FMOVEM) ------------------------------------------------------------- */

static void test_fmovem(void) {
    {   /* FMOVEM.X FP0/FP1,-(A7); FMOVEM.X (A7)+,FP2/FP3 via a dynamic list */
        SETUP(M68K_68040, FP040, 0xF227, 0xE003, 0xF21F, 0xD820);
        f->fp[0] = X(1.5); f->fp[1] = X(-2.0);
        D(2) = 0x30;                                            /* FP2, FP3 in the postincrement order */
        RUN(1);
        CHECK_EQ(A(7), STACK - 24);
        CHECK_EQ(MEM(STACK - 24, 2), 0x3FFF);
        CHECK_EQ(MEM(STACK - 12, 2), 0xC000);
        RUN(1);
        CHECK_EQ(A(7), STACK);
        CHECK(DBL(f->fp[2]) == 1.5 && DBL(f->fp[3]) == -2.0);
        done(e);
    }
    {   /* control registers: FMOVE.L #imm,FPCR; FMOVEM.L all three to -(A7) in
         * the order FPCR, FPSR, FPIAR */
        SETUP(M68K_68040, FP040, 0xF23C, 0x9000, 0x0000, 0xFFFF, 0xF227, 0xBC00);
        f->fpsr = 0x12345678; f->fpiar = 0xCAFE0000;
        RUN(2);
        CHECK_EQ(f->fpcr, 0xFFF0);
        CHECK_EQ(MEM(STACK - 12, 4), 0xFFF0);
        CHECK_EQ(MEM(STACK - 8, 4), 0x12345678);
        CHECK_EQ(MEM(STACK - 4, 4), 0xCAFE0000);
        done(e);
    }
}

/* --- FSAVE/FRESTORE (MC68881 UM 6.4.2; MC68040 UM 9.8) ---------------------------------- */

static void test_state_frames(void) {
    {   /* 68040: null after reset; idle ($41000000) after an instruction;
         * FRESTORE of the null frame resets the registers */
        SETUP(M68K_68040, FP040, 0xF327, 0xF200, 0x0422, 0xF327, 0xF35F, 0xF35F);
        RUN(1);
        CHECK_EQ(A(7), STACK - 4);
        CHECK_EQ(MEM(STACK - 4, 4), 0);
        RUN(2);
        CHECK_EQ(A(7), STACK - 8);
        CHECK_EQ(MEM(STACK - 8, 4), 0x41000000);
        f->fpcr = 0x30;
        RUN(2);                                                 /* FRESTORE idle, then null */
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(f->fpcr, 0);
        CHECK_EQ(f->fp[3].se, 0x7FFF);
        done(e);
    }
    {   /* 68881 idle frame $1F18 (28 bytes), 68882 $1F38 (60 bytes); a
         * round trip through FRESTORE */
        for (int k = 0; k < 2; k++) {
            SETUP(M68K_68030, k ? M68K_FPU_68882 : M68K_FPU_68881, 0xF200, 0x0422, 0xF327, 0xF35F);
            RUN(2);
            const unsigned size = k ? 0x3C : 0x1C;
            CHECK_EQ(A(7), STACK - size);
            CHECK_EQ(MEM(STACK - size, 4), k ? 0x1F380000u : 0x1F180000u);
            CHECK(MEM(STACK - 4, 4) & 0x08000000u);            /* BIU bit 27: nothing pending */
            RUN(1);
            CHECK_EQ(A(7), STACK);
            CHECK_EQ(e->nvec, 0);
            done(e);
        }
    }
    {   /* a frame of an unknown version is a format error at the FRESTORE */
        SETUP(M68K_68040, FP040, 0xF35F);
        A(7) = STACK - 4;
        PUT(STACK - 4, 4, 0x1F180000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        done(e);
    }
    {   /* FSAVE is privileged */
        SETUP(M68K_68040, FP040, 0xF327);
        cpu->usp = 0x7000;
        m68k_set_sr(cpu, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
}

/* --- exceptions (MC68881 UM 6.1; MC68040 UM 9.6-9.7) ------------------------------------ */

static void test_exceptions(void) {
    {   /* 68881, DZ enabled: reported at the next FP instruction (format $0,
         * that instruction's PC, vector 50); the destination is unchanged;
         * the 68881 clears it on the acknowledge */
        SETUP(M68K_68020, M68K_FPU_68881, 0xF200, 0x0420, 0x4E71, 0xF200, 0x0422);
        f->fpcr = 0x0400;
        f->fp[0] = X(1.0); f->fp[1] = X(0.0);
        RUN(2);
        CHECK_EQ(e->nvec, 0);
        CHECK(DBL(f->fp[0]) == 1.0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(50));
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 6);
        CHECK_EQ(f->fpiar, CODE);
        cpu->pc = CODE + 6;
        RUN(1);
        CHECK_EQ(e->nvec, 1);
        done(e);
    }
    {   /* 68882: still pending until the handler's FSAVE (BIU bit 27 clear in
         * the frame) */
        SETUP(M68K_68030, M68K_FPU_68882, 0xF200, 0x0420, 0xF200, 0x0422, 0xF327);
        f->fpcr = 0x0400;
        f->fp[0] = X(1.0); f->fp[1] = X(0.0);
        RUN(2);
        CHECK_EQ(cpu->pc, HANDLER(50));
        cpu->pc = CODE + 8;
        RUN(1);                                                 /* FSAVE */
        CHECK_EQ(e->nvec, 1);
        CHECK(!(MEM(A(7) + 0x38, 4) & 0x08000000u));
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(e->nvec, 1);
        done(e);
    }
    {   /* 68881, FMOVE.L FP1,(A0) with OPERR enabled: stored, then a
         * mid-instruction exception (format $9: next PC, the instruction,
         * the destination at +$10) */
        SETUP(M68K_68020, M68K_FPU_68881, 0xF210, 0x6080, 0x4E71);
        f->fpcr = 0x2000;
        f->fp[1] = X(1e20);
        A(0) = DATA;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(52));
        CHECK_EQ(MEM(DATA, 4), 0x7FFFFFFF);
        CHECK_EQ(MEM(A(7) + 6, 2), 0x90D0);
        CHECK_EQ(MEM(A(7) + 2, 4), CODE + 4);
        CHECK_EQ(MEM(A(7) + 8, 4), CODE);
        CHECK_EQ(MEM(A(7) + 0x10, 4), DATA);
        done(e);
    }
    {   /* 68040 by default: FSIN is unimplemented: vector 11, format $2 with
         * the next PC and the effective address; FSAVE gives the 26-word
         * frame with CMDREG1B and ETEMP (MC68040 UM 9.6.1, Figure 9-10), E1
         * clear (the ROM's package reads E1 there as a packed operand) */
        SETUP(M68K_68040, FP040, 0xF228, 0x540E, 0x0010, 0xF327);   /* FSIN.D (16,A0),FP0 */
        A(0) = DATA;
        PUT(DATA + 16, 4, 0x3FF00000); PUT(DATA + 20, 4, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(11));
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 6);
        CHECK_EQ(MEM(STACK - 6, 2), 0x202C);
        CHECK_EQ(MEM(STACK - 4, 4), DATA + 16);
        A(7) = STACK;
        cpu->pc = CODE + 6;
        RUN(1);                                                 /* FSAVE */
        const uint32_t fr = STACK - 0x34;
        CHECK_EQ(A(7), fr);
        CHECK_EQ(MEM(fr, 4), 0x41300000);
        CHECK_EQ(MEM(fr + 0x10, 2), 0x540E);                   /* CMDREG1B */
        CHECK_EQ(MEM(fr + 0x18, 1) & 0x04, 0);                 /* E1 clear */
        CHECK_EQ(MEM(fr + 0x28, 2), 0x3FFF);                   /* ETEMP = 1.0 */
        CHECK_EQ(MEM(fr + 0x2C, 4), 0x80000000);
        done(e);
    }
    {   /* ... and with the full set, FSIN executes */
        SETUP(M68K_68040, (m68k_fpu_type)-1, 0xF200, 0x040E);
        m68k_set_fpu(cpu, M68K_FPU_68040, 1);
        f = &cpu->fpu;
        f->fp[1] = X(0.5);
        RUN(1);
        CHECK_EQ(e->nvec, 0);
        CHECK(DBL(f->fp[0]) == 0.479425538604203);
        done(e);
    }
    {   /* 68040: a denormal operand is an unsupported data type: vector 55,
         * format $0 with the next instruction's PC (the ROM's package
         * returns there); busy frame with STAG 100 */
        SETUP(M68K_68040, FP040, 0xF200, 0x0422, 0xF327);
        f->fp[0] = X(1.0); f->fp[1] = DENORM;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(55));
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 4);
        CHECK_EQ(MEM(STACK - 2, 2), 55 * 4);
        A(7) = STACK;
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(MEM(STACK - 0x64, 4), 0x41600000);
        CHECK_EQ(MEM(STACK - 0x64 + 0x3C, 1) >> 5, 4);         /* STAG */
        CHECK_EQ(MEM(STACK - 0x64 + 0x58, 2), 0x0000);         /* ETEMP: the denormal as is */
        CHECK_EQ(MEM(STACK - 0x64 + 0x60, 4), 0x123);
        done(e);
    }
    {   /* 68040: FMOVE.P out is unsupported, post-instruction: format $3 with
         * the destination address, nothing stored */
        SETUP(M68K_68040, FP040, 0xF210, 0x6C85);
        A(0) = DATA;
        f->fp[1] = X(3.25);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(55));
        CHECK_EQ(MEM(STACK - 6, 2), 0x30DC);
        CHECK_EQ(MEM(STACK - 4, 4), DATA);
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 4);
        CHECK_EQ(MEM(DATA, 4), 0);
        done(e);
    }
    {   /* 68040, OPERR enabled: posted and reported at the next FP
         * instruction, busy frame E1 with the operands */
        SETUP(M68K_68040, FP040, 0xF200, 0x0422, 0xF200, 0x0422, 0xF327);
        f->fpcr = 0x2000;
        f->fp[0] = PINF; f->fp[1] = NINF;
        RUN(1);
        CHECK_EQ(e->nvec, 0);
        CHECK_EQ(f->fp[0].se, 0x7FFF);                         /* unchanged: +inf */
        CHECK_EQ(f->fp[0].m, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(52));
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 4);
        A(7) = STACK;
        cpu->pc = CODE + 8;
        RUN(1);
        CHECK_EQ(MEM(STACK - 0x64, 4), 0x41600000);
        CHECK_EQ(MEM(STACK - 0x64 + 0x48, 1) & 0x04, 0x04);    /* E1 */
        CHECK_EQ(MEM(STACK - 0x64 + 0x40, 2), 0x0422);         /* CMDREG1B */
        done(e);
    }
    {   /* a restored busy frame with E1 cleared: the FPU completes its
         * instruction at the FRESTORE with the frame's operands (the FPSP's
         * normalized ETEMP); the FADD after it runs as usual (1 + 3) */
        SETUP(M68K_68040, FP040, 0xF200, 0x0422, 0xF327, 0xF35F, 0xF200, 0x0422);
        f->fp[0] = X(1.0); f->fp[1] = DENORM;
        RUN(1);                                                 /* traps to 55 */
        A(7) = STACK;
        cpu->pc = CODE + 4;
        RUN(1);                                                 /* FSAVE */
        const uint32_t fr = A(7);
        PUT(fr + 0x48, 1, 0);                                   /* E1 cleared */
        PUT(fr + 0x58, 2, 0x4000);                              /* ETEMP = 2.0 */
        PUT(fr + 0x5C, 4, 0x80000000); PUT(fr + 0x60, 4, 0);
        RUN(1);                                                 /* FRESTORE */
        CHECK(DBL(f->fp[0]) == 3.0);
        f->fp[1] = X(1.0);
        RUN(1);                                                 /* FADD FP1,FP0 */
        CHECK(DBL(f->fp[0]) == 4.0);
        CHECK_EQ(e->nvec, 1);
        done(e);
    }
}

/* --- packed decimal and FMOVECR through instructions ---------------------------------- */

static void test_packed(void) {
    SETUP(M68K_68040, (m68k_fpu_type)-1, 0xF210, 0x4C00, 0xF210, 0x6C85, 0xF200, 0x5C0F);
    m68k_set_fpu(cpu, M68K_FPU_68040, 1);
    f = &cpu->fpu;
    A(0) = DATA;
    PUT(DATA, 4, 0x40020001);                                   /* +1.25E-2 */
    PUT(DATA + 4, 4, 0x25000000);
    PUT(DATA + 8, 4, 0);
    RUN(1);
    CHECK(DBL(f->fp[0]) == 0.0125);
    CHECK_EQ(EXC(), 0x0100);                                    /* INEX1: 0.0125 is not binary */
    f->fp[1] = X(-12345.678765);
    RUN(1);                                                     /* FMOVE.P FP1,(A0){#5} */
    CHECK_EQ(MEM(DATA, 4), 0x80040001);
    CHECK_EQ(MEM(DATA + 4, 4), 0x23460000);
    RUN(1);                                                     /* FMOVECR #$0F: 0.0 */
    CHECK_EQ(f->fp[0].se, 0);
    CHECK_EQ(CC(), 4);
    done(e);
}

int mc_cpu_fpu_unit(void) {
    test_specials();
    test_conditionals();
    test_fmovem();
    test_state_frames();
    test_exceptions();
    test_packed();
    printf("fpu: %d failure(s)\n", mc_test_failures);
    return mc_test_failures;
}
