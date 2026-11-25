/* mc_cpu_tests 68851: the MC68851 beside the 68020, and CALLM/RTM.
 *
 * Each case builds tables in RAM as the MC68851 User's Manual section 5
 * lays them out, runs a few instructions and checks what the manual says
 * the chip does: the coprocessor-0 instructions (PMOVE, PFLUSH, PFLUSHR,
 * PLOAD, PTEST, PBcc, PDBcc, PScc, PTRAPcc, PSAVE, PRESTORE; appendix A),
 * the PSR bits (6.1.8), the root pointer table and task aliases (5.3), the
 * refused read-modify-write cycle (6.3.1.7), the post-instruction
 * exceptions and their format $2 frames (6.3.2, MC68020 UM 6.5.3), bus
 * errors through the 68020's format $B frame; line F without the 68851
 * (MC68020 UM 8.16.5). The first case runs the Macintosh II ROM's own
 * sequence (its 24-bit table, TC $80F84500). CALLM and
 * RTM per the MC68020 UM appendix D. */
#include "cpu_test.h"
#include "../test.h"

#define CODE 0x1000u
#define STACK 0x8000u
#define HANDLERS 0x10000u
#define ROOT 0x80000u
#define TABLE2 0x81000u
#define TABLEB 0x82000u
#define IOREGS 0x9000u

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

static env *setup(int pmmu, const uint16_t *code, size_t words) {
    env *e = calloc(1, sizeof *e);
    tb_init(&e->bus, 1u << 20, 0xFFFFFFFFu, 0);
    for (unsigned v = 0; v < 256; v++) {
        tb_put(&e->bus, 4 * v, 4, HANDLERS + 16 * v);
        tb_put(&e->bus, HANDLERS + 16 * v, 2, 0x4E71);
    }
    for (size_t i = 0; i < words; i++) tb_put(&e->bus, CODE + 2 * (uint32_t)i, 2, code[i]);
    m68k_bus mb = tb_bus(&e->bus);
    e->cpu = calloc(1, sizeof *e->cpu);
    m68k_init(e->cpu, M68K_68020, &mb);
    m68k_set_pmmu(e->cpu, pmmu);
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
#define SETUP(pmmu, ...)                                         \
    static const uint16_t code_[] = {__VA_ARGS__};               \
    env *e = setup(pmmu, code_, sizeof code_ / sizeof code_[0]); \
    m68k_cpu *cpu = e->cpu
#define RUN(n) m68k_run(cpu, (n))
#define D(n) cpu->r[(n)]
#define A(n) cpu->r[8 + (n)]
#define MEM(a, size) tb_get(&e->bus, (a), (size))
#define PUT(a, size, v) tb_put(&e->bus, (a), (size), (v))
#define HANDLER(v) (HANDLERS + 16u * (v))

/* The Macintosh II ROM's 24-bit table (at ROM offset $50 in 97851DB6 and
 * 9779D2C4): 16 short page descriptors at level A, one per MiB, U and M
 * set, slots and I/O cache-inhibited. Here segment 0 maps RAM, segment 1
 * aliases it and segment $F maps it at $40000. */
static void table24(env *e) {
    for (uint32_t i = 0; i < 16; i++) PUT(ROOT + 4 * i, 4, 0x00000059u);
    PUT(ROOT, 4, 0x00000019u);
    PUT(ROOT + 4, 4, 0x00000019u);
    PUT(ROOT + 4 * 15, 4, 0x00040059u);
}

/* TC $80C04880: PS 4 KiB, IS 0, TIA 4, TIB 8, TIC 8. Table A (long
 * descriptors) at ROOT:
 *   0: long table descriptor -> table B of short descriptors whose entry 0
 *      is an early-terminated page (1 MiB identity) with G and L set;
 *   1: long page descriptor (type 2), S, limit 0 on the B index;
 *   2: long table descriptor with WP and SG -> the same table B;
 *   3 and up: invalid. */
static void tables_long(env *e) {
    for (uint32_t i = 0; i < 32; i++) PUT(ROOT + 4 * i, 4, 0);
    PUT(ROOT + 0, 4, 0x7FFF0002u);
    PUT(ROOT + 4, 4, TABLEB);
    PUT(ROOT + 8, 4, 0x00000101u);
    PUT(ROOT + 12, 4, 0x00000000u);
    PUT(ROOT + 16, 4, 0x7FFF0206u);
    PUT(ROOT + 20, 4, TABLEB);
    for (uint32_t i = 0; i < 256; i++) PUT(TABLEB + 4 * i, 4, 0);
    PUT(TABLEB, 4, 0x000000A1u);                    /* G, L, DT page */
    PUT(IOREGS, 4, 0x7FFF0003u);                    /* CRP: DT long */
    PUT(IOREGS + 4, 4, ROOT);
    PUT(IOREGS + 8, 4, 0x80C04880u);
}

static void test_detect(void) {
    {   /* The ROM's sequence: PMOVE TC,A0 (the 68851 allows An), CRP from
         * the stack, TC $80F84500; then a 24-bit address with a top byte
         * reads through segment 0, IS = 8 ignoring A31-A24 */
        SETUP(1, 0xF008, 0x4200, 0xF01F, 0x4C00, 0x203C, 0x80F8, 0x4500, 0xF000, 0x4000, 0x2239, 0xAB0F, 0x1234,
              0x2439, 0x0101, 0x2340, 0x2639, 0x00F0, 0x0010, 0xF006, 0x6600);
        table24(e);
        A(0) = 0xDEADBEEF;
        A(7) = STACK - 8;
        PUT(STACK - 8, 4, 0x7FFF0202u);             /* SG, DT short */
        PUT(STACK - 4, 4, ROOT);
        PUT(0x0F1234, 4, 0x11223344);
        PUT(0x012340, 4, 0x55667788);
        PUT(0x040010, 4, 0x99AABBCC);
        RUN(8);
        CHECK_EQ(A(0), 0);                          /* TC was 0 */
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(cpu->mmu.crp[0], 0x7FFF0202u);
        CHECK_EQ(D(1), 0x11223344);
        CHECK_EQ(D(2), 0x55667788);                 /* segment 1 */
        CHECK_EQ(D(3), 0x99AABBCC);                 /* segment $F */
        CHECK_EQ(D(6) & 0xFFFF, 0x8000);            /* PCSR: F, alias 0 */
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* PCSR after the first CRP: task alias 0, F set (a new entry) */
        SETUP(1, 0xF010, 0x4C00, 0xF002, 0x6600);
        PUT(IOREGS, 4, 0x7FFF0002u);
        PUT(IOREGS + 4, 4, ROOT);
        A(0) = IOREGS;
        RUN(2);
        CHECK_EQ(D(2) & 0xFFFF, 0x8000);
        done(e);
    }
    {   /* Without the 68851 every coprocessor-0 word takes line F at the
         * instruction (the ROM's probe sees its handler) */
        SETUP(0, 0xF008, 0x4200);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(11));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        CHECK_EQ(MEM(A(7) + 6, 2), 0x002C);
        done(e);
    }
    {   /* ... but PSAVE in user state is a privilege violation first */
        SETUP(0, 0xF110);
        m68k_set_sr(cpu, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
    {   /* The 68881 (coprocessor 1) and the 68851 (coprocessor 0) beside
         * one 68020, as a description's cpu, fpu and mmu fields combine
         * them: FMOVE.L #5,FP0; FMOVE.L FP0,D0; PMOVE TC,A0 */
        SETUP(1, 0xF23C, 0x4000, 0x0000, 0x0005, 0xF200, 0x6000, 0xF008, 0x4200);
        A(0) = 0xFFFFFFFF;
        RUN(3);
        CHECK_EQ(D(0), 5);
        CHECK_EQ(A(0), 0);
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* Decode: the 68020's coprocessor-0 words go to the PMMU handler */
        CHECK_STR(m68k_decode(M68K_68020, 0xF000).name, "PMMU");
        CHECK_STR(m68k_decode(M68K_68020, 0xF17F).name, "PMMU");
        CHECK_STR(m68k_decode(M68K_68030, 0xF080).name, "line F");
        CHECK_EQ(m68k_table_conflicts(M68K_68020), 0);
    }
}

static void test_registers(void) {
    {   /* An inconsistent TC: configuration error (vector 56) after the
         * instruction, format $2 with the next PC and the instruction's
         * address; TC loaded with E clear */
        SETUP(1, 0xF011, 0x4000, 0x4E71, 0xF001, 0x4200);
        A(1) = IOREGS + 8;
        PUT(IOREGS + 8, 4, 0x80C0A900);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(56));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE + 4);
        CHECK_EQ(MEM(A(7) + 6, 2), 0x20E0);
        CHECK_EQ(MEM(A(7) + 8, 4), CODE);
        cpu->pc = CODE + 6;
        RUN(1);
        CHECK_EQ(D(1), 0x00C0A900);
        done(e);
    }
    {   /* PS bit 3 clear is inconsistent too (6.1.3.4) */
        SETUP(1, 0xF011, 0x4000);
        A(1) = IOREGS + 8;
        PUT(IOREGS + 8, 4, 0x8070A9A0);             /* PS 7, sum 7 + 0 + 10 + 9 + 10 = 36 */
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(56));
        done(e);
    }
    {   /* A root pointer with DT invalid: configuration error, not loaded */
        SETUP(1, 0xF010, 0x4C00);
        PUT(IOREGS, 4, 0x7FFF0000);
        PUT(IOREGS + 4, 4, ROOT);
        A(0) = IOREGS;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(56));
        CHECK_EQ(cpu->mmu.crp[0], 0);
        done(e);
    }
    {   /* PMOVE immediate to CRP (8 bytes from the stream), CRP to -(A2),
         * PSR from and to Dn, PCSR to memory */
        SETUP(1, 0xF03C, 0x4C00, 0x7FFF, 0x0002, 0x0008, 0x0000, 0xF022, 0x4E00, 0xF000, 0x6000, 0xF003, 0x6200,
              0xF013, 0x6600);
        A(2) = IOREGS + 0x20;
        A(3) = IOREGS + 0x30;
        D(0) = 0xFFFF0480;
        RUN(5);
        CHECK_EQ(cpu->mmu.crp[1], 0x00080000);
        CHECK_EQ(A(2), IOREGS + 0x18);
        CHECK_EQ(MEM(IOREGS + 0x18, 4), 0x7FFF0002);
        CHECK_EQ(MEM(IOREGS + 0x1C, 4), 0x00080000);
        CHECK_EQ(cpu->mmu.mmusr30, 0x0480);
        CHECK_EQ(D(3) & 0xFFFF, 0x0480);
        CHECK_EQ(MEM(A(3), 2), 0x8000);             /* PCSR: F, alias 0 */
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* Not modelled, line F: PMOVE to CAL, PVALID, PMOVE to PCSR, BADx;
         * CRP to Dn is no mode for a 64-bit register */
        static const uint16_t forms[][2] = {{0xF010, 0x5000}, {0xF010, 0x2800}, {0xF010, 0x6400}, {0xF010, 0x7000},
                                            {0xF000, 0x4E00}, {0xF010, 0x0800}};
        for (unsigned i = 0; i < 6; i++) {
            SETUP(1, 0x4E71);
            PUT(CODE, 2, forms[i][0]);
            PUT(CODE + 2, 2, forms[i][1]);
            A(0) = IOREGS;
            RUN(1);
            CHECK_EQ(cpu->pc, HANDLER(11));
            CHECK_EQ(MEM(A(7) + 2, 4), CODE);
            done(e);
        }
    }
    {   /* Privileged: PMOVE in user state */
        SETUP(1, 0xF010, 0x6200);
        m68k_set_sr(cpu, 0);
        A(0) = IOREGS;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
    {   /* PTEST and PLOAD with E clear: illegal operation (vector 57) */
        SETUP(1, 0xF010, 0x8215, 0x4E71, 0xF010, 0x2015);
        A(0) = 0x4000;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(57));
        CHECK_EQ(MEM(A(7) + 6, 2), 0x20E4);
        cpu->pc = CODE + 6;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(57));
        done(e);
    }
}

static void test_walks(void) {
    {   /* Long and short descriptors: a read through table B, U written
         * back at both levels; PTEST level 7 finds two levels, G and the
         * last descriptor's address */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2412, 0xF012, 0x9F75, 0xF014, 0x6200);
        tables_long(e);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x00012340; A(4) = IOREGS + 16;
        PUT(0x12340, 4, 0x0BADF00D);
        RUN(5);
        CHECK_EQ(D(2), 0x0BADF00D);
        CHECK_EQ(MEM(ROOT, 4), 0x7FFF000A);         /* U of the long table descriptor */
        CHECK_EQ(MEM(TABLEB, 4), 0x000000B9);       /* U, and M from the PSR store in the same page */
        CHECK_EQ(MEM(IOREGS + 16, 2), 0x0102);      /* G, two levels */
        CHECK_EQ(A(3), TABLEB);
        done(e);
    }
    {   /* Supervisor-only long page: a user-space test reports S and I,
         * the supervisor one passes; the limit on the B index: L and I */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF012, 0x8E11, 0xF014, 0x6200, 0xF012, 0x8E15, 0xF015, 0x6200,
              0xF013, 0x8E15, 0xF016, 0x6200);
        tables_long(e);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x10012340; A(3) = 0x10100000;
        A(4) = IOREGS + 16; A(5) = IOREGS + 18; A(6) = IOREGS + 20;
        RUN(8);
        CHECK_EQ(MEM(IOREGS + 16, 2), 0x2401);      /* S, I, one level */
        CHECK_EQ(MEM(IOREGS + 18, 2), 0x0001);
        CHECK_EQ(MEM(IOREGS + 20, 2) & 0xC400, 0x4400);
        done(e);
    }
    {   /* WP and SG of a long table descriptor: PSR W and C; a write there
         * is a bus error (format $B, DF, a write) */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF012, 0x8E15, 0xF014, 0x6200, 0x2483);
        tables_long(e);
        PUT(HANDLER(2), 2, 0x4E71);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x20012340; A(4) = IOREGS + 16;
        D(3) = 0x12345678;
        RUN(5);
        CHECK_EQ(MEM(IOREGS + 16, 2), 0x0982);      /* W, G, C, two levels */
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(A(7) + 6, 2), 0xB008);
        CHECK_EQ(MEM(A(7) + 0x0A, 2) & 0x01C0, 0x0100);
        CHECK_EQ(MEM(A(7) + 0x10, 4), 0x20012340);
        CHECK_EQ(MEM(0x12340, 4), 0);
        done(e);
    }
    {   /* An invalid descriptor: the read bus-errors; RTE reruns it once
         * the table is fixed and the ATC flushed */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2412, 0x4E71);
        tables_long(e);
        PUT(HANDLER(2), 2, 0xF000);
        PUT(HANDLER(2) + 2, 2, 0x2400);             /* PFLUSHA */
        PUT(HANDLER(2) + 4, 2, 0x4E73);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x30012350;
        RUN(3);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(A(7) + 0x10, 4), 0x30012350);
        PUT(ROOT + 24, 4, 0x7FFF0002u);
        PUT(ROOT + 28, 4, TABLEB);
        PUT(0x12350, 4, 0xCAFEBABE);
        RUN(3);
        CHECK_EQ(D(2), 0xCAFEBABE);
        done(e);
    }
}

static void test_atc(void) {
    {   /* Task aliases (5.3): CRP 1 then CRP 2 map segment 1 differently;
         * back to CRP 1 the alias is found again (F clear) and its ATC
         * entry kept, so a table changed without a flush is not seen;
         * PFLUSHR of CRP 1 removes it */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2439, 0x0010, 0x5000, 0xF012, 0x4C00, 0x2639, 0x0010, 0x5000,
              0xF006, 0x6600, 0xF010, 0x4C00, 0xF003, 0x6600, 0x2839, 0x0010, 0x5000, 0xF010, 0xA000,
              0x2A39, 0x0010, 0x5000);
        table24(e);
        for (uint32_t i = 0; i < 16; i++) PUT(TABLE2 + 4 * i, 4, 0);
        PUT(TABLE2, 4, 0x00000019);
        PUT(TABLE2 + 4, 4, 0x00040019);
        PUT(IOREGS, 4, 0x7FFF0002); PUT(IOREGS + 4, 4, ROOT);
        PUT(IOREGS + 8, 4, 0x80F84500);
        PUT(IOREGS + 16, 4, 0x7FFF0002); PUT(IOREGS + 20, 4, TABLE2);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = IOREGS + 16;
        PUT(0x05000, 4, 0x11111111);
        PUT(0x45000, 4, 0x22222222);
        PUT(0x85000, 4, 0x33333333);
        RUN(6);
        CHECK_EQ(D(2), 0x11111111);
        CHECK_EQ(D(3), 0x22222222);
        CHECK_EQ(D(6) & 0xFFFF, 0x8001);            /* F, alias 1 */
        PUT(ROOT + 4, 4, 0x00080019);               /* changed behind the ATC's back */
        RUN(3);
        CHECK_EQ(D(3) & 0xFFFF, 0x0000);            /* alias 0 again, F clear */
        CHECK_EQ(D(4), 0x11111111);                 /* the kept entry */
        RUN(2);
        CHECK_EQ(D(5), 0x33333333);                 /* after PFLUSHR: searched again */
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* PLOADW sets U and M without an access; PFLUSH by function code
         * and mask removes only fc 1; PTEST level 0 sees the ATC */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF014, 0x2011, 0xF014, 0x2015, 0xF000, 0x31F1,
              0xF014, 0x8215, 0xF015, 0x6200, 0xF014, 0x8211, 0xF016, 0x6200);
        table24(e);
        PUT(IOREGS, 4, 0x7FFF0002); PUT(IOREGS + 4, 4, ROOT); PUT(IOREGS + 8, 4, 0x80F84500);
        PUT(ROOT, 4, 0x00000001);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x6000; A(5) = IOREGS + 16; A(6) = IOREGS + 20;
        RUN(4);
        CHECK_EQ(MEM(ROOT, 4), 0x00000019);
        RUN(5);
        CHECK_EQ(MEM(IOREGS + 16, 2), 0x0200);      /* fc 5: resident, M */
        CHECK_EQ(MEM(IOREGS + 20, 2), 0x0400);      /* fc 1: flushed */
        done(e);
    }
    {   /* Shared entries (SG in the root pointer) survive PFLUSH by
         * function code and address; PFLUSHS takes them */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2414, 0xF014, 0x39F5, 0xF014, 0x8215, 0xF015, 0x6200,
              0xF014, 0x3DF5, 0xF014, 0x8215, 0xF016, 0x6200);
        table24(e);
        PUT(IOREGS, 4, 0x7FFF0202); PUT(IOREGS + 4, 4, ROOT); PUT(IOREGS + 8, 4, 0x80F84500);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x6000; A(5) = IOREGS + 16; A(6) = IOREGS + 20;
        RUN(6);
        CHECK_EQ(MEM(IOREGS + 16, 2) & 0x0480, 0x0080);   /* still there, C */
        RUN(3);
        CHECK_EQ(MEM(IOREGS + 20, 2) & 0x0400, 0x0400);   /* flushed */
        done(e);
    }
    {   /* A read-modify-write cycle without an ATC entry is refused before
         * any search (6.3.1.7): format $B with RM; the handler's PLOADW
         * and RTE rerun the TAS, which then completes */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x4AD4, 0x4E71);
        table24(e);
        PUT(IOREGS, 4, 0x7FFF0002); PUT(IOREGS + 4, 4, ROOT); PUT(IOREGS + 8, 4, 0x80F84500);
        PUT(ROOT, 4, 0x00000001);
        PUT(HANDLER(2), 2, 0xF014);
        PUT(HANDLER(2) + 2, 2, 0x2015);             /* PLOADW #5,(A4) */
        PUT(HANDLER(2) + 4, 2, 0x4E73);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x6001;
        RUN(3);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(A(7) + 0x0A, 2) & 0x01C0, 0x01C0);   /* DF, RM, read */
        CHECK_EQ(MEM(A(7) + 0x10, 4), 0x6001);
        RUN(3);
        CHECK_EQ(MEM(0x6001, 1), 0x80);
        CHECK_EQ(MEM(ROOT, 4), 0x00000019);
        CHECK_EQ(cpu->pc, CODE + 10);
        done(e);
    }
    {   /* A locked entry (L) is never replaced: 70 other pages later PTEST
         * level 0 still finds it */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2414, 0x4E71);
        PUT(IOREGS, 4, 0x7FFF0002); PUT(IOREGS + 4, 4, ROOT); PUT(IOREGS + 8, 4, 0x80C0AA00);
        for (uint32_t i = 0; i < 1024; i++) PUT(ROOT + 4 * i, 4, 0);
        PUT(ROOT, 4, (ROOT + 0x1000) | 2);
        for (uint32_t i = 0; i < 1024; i++) PUT(ROOT + 0x1000 + 4 * i, 4, i < 256 ? (i << 12) | 1 : 0);
        PUT(ROOT + 0x1000 + 4 * 0x60, 4, 0x60021);  /* $60000: L */
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x60000;
        RUN(3);
        for (uint32_t p = 0; p < 70; p++) {
            PUT(CODE + 6, 2, 0x2428);
            PUT(CODE + 8, 2, 0);
            A(0) = 0x10000 + 0x1000 * p;
            cpu->pc = CODE + 6;
            RUN(1);
        }
        PUT(CODE + 6, 2, 0xF014); PUT(CODE + 8, 2, 0x8215); PUT(CODE + 10, 2, 0xF001); PUT(CODE + 12, 2, 0x6200);
        cpu->pc = CODE + 6;
        RUN(2);
        CHECK_EQ(D(1) & 0x0400, 0);                 /* still resident */
        done(e);
    }
}

static void test_conditionals(void) {
    {   /* PSR = I: PBIS.W taken, PBIC.W not, PSIS D4, PDBIC D3 counts
         * down and branches, PDBIS does not; PTRAPIS traps (vector 7,
         * format $2, next PC, the instruction's address) */
        SETUP(1, 0xF010, 0x6000, 0xF08A, 0x0004, 0x4E71, 0xF08B, 0x0004, 0xF044, 0x000A, 0xF04B, 0x000B, 0xFFFC,
              0xF04B, 0x000A, 0x0000, 0xF07C, 0x000A, 0x4E71);
        PUT(IOREGS, 2, 0x0400);
        A(0) = IOREGS;
        D(3) = 1;
        RUN(4);                                     /* PMOVE, PBIS (to the PBIC), PBIC, PSIS */
        CHECK_EQ(D(4) & 0xFF, 0xFF);
        CHECK_EQ(cpu->pc, CODE + 18);
        RUN(1);                                     /* PDBIC: D3 1 -> 0, branch to itself */
        CHECK_EQ(D(3) & 0xFFFF, 0);
        CHECK_EQ(cpu->pc, CODE + 18);
        RUN(1);                                     /* 0 -> -1: falls through */
        CHECK_EQ(D(3) & 0xFFFF, 0xFFFF);
        CHECK_EQ(cpu->pc, CODE + 24);
        RUN(2);                                     /* PDBIS: true, nothing; PTRAPIS */
        CHECK_EQ(cpu->pc, HANDLER(7));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE + 34);
        CHECK_EQ(MEM(A(7) + 6, 2), 0x201C);
        CHECK_EQ(MEM(A(7) + 8, 4), CODE + 30);
        done(e);
    }
    {   /* A condition above $0F is unrecognised: line F; user state:
         * privilege violation */
        SETUP(1, 0xF090, 0x0002, 0xF08A, 0x0002);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(11));
        cpu->pc = CODE + 4;
        m68k_set_sr(cpu, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
}

static void test_state(void) {
    {   /* PSAVE -(A7) writes the idle frame; PRESTORE (A7)+ brings CRP,
         * SRP and PSR back */
        SETUP(1, 0xF010, 0x4C00, 0xF011, 0x4800, 0xF013, 0x6000, 0xF127, 0xF012, 0x4C00, 0xF15F, 0xF001, 0x6200);
        PUT(IOREGS, 4, 0x7FFF0002); PUT(IOREGS + 4, 4, ROOT);
        PUT(IOREGS + 8, 4, 0x7FFF0003); PUT(IOREGS + 12, 4, TABLE2);
        PUT(IOREGS + 16, 4, 0x00010002); PUT(IOREGS + 20, 4, TABLEB);
        PUT(IOREGS + 24, 2, 0x2001);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = IOREGS + 16; A(3) = IOREGS + 24;
        RUN(4);
        CHECK_EQ(A(7), STACK - 36);
        CHECK_EQ(MEM(STACK - 36, 4), 0xC0200000);
        CHECK_EQ(MEM(STACK - 32, 2), 0x2001);
        CHECK_EQ(MEM(STACK - 28, 4), TABLE2);       /* SRP low, high */
        CHECK_EQ(MEM(STACK - 24, 4), 0x7FFF0003);
        CHECK_EQ(MEM(STACK - 20, 4), ROOT);         /* CRP low, high */
        CHECK_EQ(MEM(STACK - 16, 4), 0x7FFF0002);
        RUN(1);
        CHECK_EQ(cpu->mmu.crp[1], TABLEB);
        RUN(2);
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(cpu->mmu.crp[1], ROOT);
        CHECK_EQ(cpu->mmu.srp30[1], TABLE2);
        CHECK_EQ(D(1) & 0xFFFF, 0x2001);
        done(e);
    }
    {   /* PRESTORE of an unknown frame: format error (vector 14) at the
         * instruction, A0 not stepped; the reset frame changes nothing */
        SETUP(1, 0xF158, 0x4E71, 0xF158);
        PUT(IOREGS, 4, 0x12340000);
        PUT(IOREGS + 4, 4, 0xC0000000);
        A(0) = IOREGS;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        CHECK_EQ(A(0), IOREGS);
        A(0) = IOREGS + 4;
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(A(0), IOREGS + 8);
        CHECK_EQ(cpu->pc, CODE + 6);
        done(e);
    }
}

/* CALLM/RTM (MC68020 UM appendix D): descriptor at $3000, module at $3100
 * whose entry word names A0. */
static void test_modules(void) {
    for (int pmmu = 0; pmmu < 2; pmmu++) {
        SETUP(pmmu, 0x2F3C, 0x0000, 0x0001, 0x44FC, 0x0005, 0x06D1, 0x0004, 0x4E71);
        PUT(0x3000, 4, 0x00000000);                 /* Opt 000, Type $00 */
        PUT(0x3004, 4, 0x3100);
        PUT(0x3008, 4, 0xDA7A0000);
        PUT(0x3100, 2, 0x8000);                     /* A0 */
        PUT(0x3102, 2, 0x2A08);                     /* MOVE.L A0,D5 */
        PUT(0x3104, 2, 0x44FC);
        PUT(0x3106, 2, 0x0000);                     /* MOVE #0,CCR */
        PUT(0x3108, 2, 0x06C8);                     /* RTM A0 */
        A(0) = 0xA0A0A0A0;
        A(1) = 0x3000;
        RUN(3);
        CHECK_EQ(cpu->pc, 0x3102);
        CHECK_EQ(A(0), 0xDA7A0000);
        CHECK_EQ(A(7), STACK - 4 - 24);
        const uint32_t f = STACK - 28;
        CHECK_EQ(MEM(f, 2), 0x0000);
        CHECK_EQ(MEM(f + 2, 2), 0x0005);            /* CCR */
        CHECK_EQ(MEM(f + 4, 2), 4);                 /* argument count */
        CHECK_EQ(MEM(f + 8, 4), 0x3000);
        CHECK_EQ(MEM(f + 12, 4), CODE + 14);
        CHECK_EQ(MEM(f + 16, 4), 0xA0A0A0A0);
        CHECK_EQ(MEM(f + 20, 4), STACK - 4);
        RUN(3);
        CHECK_EQ(D(5), 0xDA7A0000);
        CHECK_EQ(cpu->pc, CODE + 14);
        CHECK_EQ(A(0), 0xA0A0A0A0);
        CHECK_EQ(A(7), STACK);                      /* frame and arguments removed */
        CHECK_EQ(m68k_ccr(cpu), 0x05);
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* Type $01 (an access-level change) and an unknown Opt: format
         * error at the instruction, nothing changed */
        SETUP(1, 0x06D1, 0x0000, 0x06D2, 0x0000);
        PUT(0x3000, 4, 0x01000000);
        PUT(0x3010, 4, 0x20000000);
        A(1) = 0x3000;
        A(2) = 0x3010;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        CHECK_EQ(A(7), STACK - 8);
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        done(e);
    }
    {   /* The entry word naming A7: A7 ends as the frame's address; an
         * RTM of a bad frame is a format error */
        SETUP(0, 0x06D1, 0x0000, 0x06C0);
        PUT(0x3000, 4, 0x80000000);                 /* Opt 100 */
        PUT(0x3004, 4, 0x3100);
        PUT(0x3008, 4, 0x12345678);
        PUT(0x3100, 2, 0xF000);
        A(1) = 0x3000;
        RUN(1);
        CHECK_EQ(A(7), STACK - 24);
        CHECK_EQ(MEM(STACK - 24, 2), 0x8000);
        CHECK_EQ(MEM(STACK - 4, 4), STACK);
        PUT(STACK - 24, 2, 0x0300);
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        done(e);
    }
}

int mc_cpu_68851_unit(void) {
    test_detect();
    test_registers();
    test_walks();
    test_atc();
    test_conditionals();
    test_state();
    test_modules();
    printf("68851: %d failure(s)\n", mc_test_failures);
    return mc_test_failures;
}
