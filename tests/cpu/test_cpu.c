/* mc_cpu_tests unit: the 68020+/68040 instructions (and the 68000's own
 * frames, interrupts, bus cycles and decode table, test_68000), addressing modes,
 * control registers, exception frames, interrupts and access faults, each
 * checked against the operation the Programmer's Reference Manual (PRM) or
 * the MC68040 User's Manual defines. Expected values are worked out by hand
 * from those definitions in the comments. */
#include "cpu_test.h"
#include "../test.h"

#define CODE 0x1000u
#define STACK 0x8000u
#define HANDLERS 0x10000u   /* vector v's handler at HANDLERS + 16 v */

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

/* RAM 1 MiB; vector table at 0; code at CODE; ISP at STACK; SR $2700. */
static env *setup(m68k_model model, const uint16_t *code, size_t words) {
    env *e = calloc(1, sizeof *e);
    tb_init(&e->bus, 1u << 20, 0xFFFFFFFFu, 0);
    for (unsigned v = 0; v < 256; v++) {
        tb_put(&e->bus, 4 * v, 4, HANDLERS + 16 * v);
        tb_put(&e->bus, HANDLERS + 16 * v, 2, 0x4E71);   /* NOP */
    }
    for (size_t i = 0; i < words; i++) tb_put(&e->bus, CODE + 2 * (uint32_t)i, 2, code[i]);
    m68k_bus mb = tb_bus(&e->bus);
    e->cpu = calloc(1, sizeof *e->cpu);
    m68k_init(e->cpu, model, &mb);
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
#define SETUP(model, ...)                                         \
    static const uint16_t code_[] = {__VA_ARGS__};                \
    env *e = setup(model, code_, sizeof code_ / sizeof code_[0]); \
    m68k_cpu *cpu = e->cpu
#define RUN(n) m68k_run(cpu, (n))
#define D(n) cpu->r[(n)]
#define A(n) cpu->r[8 + (n)]
#define MEM(a, size) tb_get(&e->bus, (a), (size))
#define PUT(a, size, v) tb_put(&e->bus, (a), (size), (v))
#define HANDLER(v) (HANDLERS + 16u * (v))
#define CCR() (m68k_sr(cpu) & 0x1Fu)

/* --- the decode tables --------------------------------------------------- */

static void test_tables(void) {
    for (int m = 0; m < M68K_MODEL_COUNT; m++) CHECK_EQ(m68k_table_conflicts((m68k_model)m), 0);
    CHECK(m68k_decode(M68K_68040, 0xF620).valid);                    /* MOVE16 (Ax)+,(Ay)+ */
    CHECK(!m68k_decode(M68K_68030, 0xF620).valid);
    CHECK(m68k_decode(M68K_68020, 0x06D0).valid);                    /* CALLM (A0) */
    CHECK(!m68k_decode(M68K_68040, 0x06D0).valid);
    CHECK(m68k_decode(M68K_68040, 0x4E7A).valid);                    /* MOVEC */
    CHECK(!m68k_decode(M68K_68040, 0x4E78).valid);
    CHECK_STR(m68k_decode(M68K_68040, 0x49C0).name, "EXTB.L");
    CHECK_STR(m68k_decode(M68K_68040, 0xF4D8).name, "CINV");         /* CINVA both caches */
    CHECK_STR(m68k_decode(M68K_68040, 0xF548).name, "PTEST");
    CHECK_STR(m68k_decode(M68K_68030, 0xF010).name, "PMMU");
    CHECK(!m68k_decode(M68K_68040, 0xF010).valid);
    CHECK_STR(m68k_decode(M68K_68LC040, 0xF200).name, "FPU");
    CHECK(!m68k_decode(M68K_68040, 0xA000).valid);                   /* line A */
    CHECK(!m68k_decode(M68K_68040, 0x0AFC).valid);                   /* CAS.B #imm */
    CHECK(m68k_decode(M68K_68040, 0x0C7A).valid);                    /* CMPI.W (d16,PC): 68020+ */
    CHECK(m68k_decode(M68K_68040, 0x4A48).valid);                    /* TST.W A0: 68020+ */
    CHECK(!m68k_decode(M68K_68040, 0x4A08).valid);                   /* TST.B A0 */
    CHECK(!m68k_decode(M68K_68040, 0x5008).valid);                   /* ADDQ.B #8,A0 */
    for (int m = 0; m < M68K_MODEL_COUNT; m++) {
        unsigned n = 0;
        for (unsigned op = 0; op < 65536; op++) n += m68k_decode((m68k_model)m, (uint16_t)op).valid;
        printf("  %s: %u valid opcode words\n", m68k_model_name((m68k_model)m), n);
    }
}

/* --- addressing modes (PRM 2.2.6-2.2.13) --------------------------------- */

static void test_addressing(void) {
    {   /* MOVE.L (4,A0,D1.L*4),D0: brief format with scale; 0x2000 + 0x40 + 4 */
        SETUP(M68K_68040, 0x2030, 0x1C04);
        A(0) = 0x2000; D(1) = 0x10;
        PUT(0x2044, 4, 0x11223344);
        RUN(1);
        CHECK_EQ(D(0), 0x11223344);
        done(e);
    }
    {   /* MOVE.L ([8,A0,D1.W*2],12),D0: preindexed; pointer at 0x2000+8+0x20 */
        SETUP(M68K_68040, 0x2030, 0x1322, 0x0008, 0x000C);
        A(0) = 0x2000; D(1) = 0xFFFF0010;
        PUT(0x2028, 4, 0x3000);
        PUT(0x300C, 4, 0xCAFEBABE);
        RUN(1);
        CHECK_EQ(D(0), 0xCAFEBABE);
        CHECK_EQ(cpu->pc, CODE + 8);
        done(e);
    }
    {   /* MOVE.L ([8,A0],D1.L*4,$100.L),D0: postindexed; 0x4000 + 0x40 + 0x100 */
        SETUP(M68K_68040, 0x2030, 0x1D27, 0x0008, 0x0000, 0x0100);
        A(0) = 0x2000; D(1) = 0x10;
        PUT(0x2008, 4, 0x4000);
        PUT(0x4140, 4, 0x55667788);
        RUN(1);
        CHECK_EQ(D(0), 0x55667788);
        CHECK_EQ(cpu->pc, CODE + 10);
        done(e);
    }
    {   /* MOVE.L ([$5000.L]),D0: base and index suppressed, null outer */
        SETUP(M68K_68040, 0x2030, 0x01F1, 0x0000, 0x5000);
        A(0) = 0x7777;
        PUT(0x5000, 4, 0x6000);
        PUT(0x6000, 4, 0x01020304);
        RUN(1);
        CHECK_EQ(D(0), 0x01020304);
        done(e);
    }
    {   /* MOVE.L ([bd,PC]),D0: PC-relative memory indirect; base = 0x1002 */
        SETUP(M68K_68040, 0x203B, 0x0161, 0x0FFE);
        PUT(0x2000, 4, 0x6100);
        PUT(0x6100, 4, 0xA5A5A5A5);
        RUN(1);
        CHECK_EQ(D(0), 0xA5A5A5A5);
        done(e);
    }
    {   /* (d8,PC,Xn): MOVE.W (6,PC,D0.W),D1; base 0x1002 + 6 + 2 */
        SETUP(M68K_68040, 0x323B, 0x0006);
        D(0) = 2;
        PUT(0x100A, 2, 0xBEEF);
        RUN(1);
        CHECK_EQ(D(1) & 0xFFFF, 0xBEEF);
        done(e);
    }
    {   /* reserved I/IS = 100 and BD SIZE = 00: illegal instruction, nothing changed */
        SETUP(M68K_68040, 0x2018, 0x2030, 0x0124, 0x0000);   /* MOVE.L (A0)+,D0 then the bad one */
        A(0) = 0x2000;
        RUN(1);
        CHECK_EQ(A(0), 0x2004);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(4));
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 2);
        done(e);
    }
    {
        SETUP(M68K_68040, 0x2030, 0x0100);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(4));
        done(e);
    }
}

/* --- bit fields (PRM 4, BFxxx) -------------------------------------------- */

static void test_bitfields(void) {
    {   /* BFEXTU D0{4:8},D1 */
        SETUP(M68K_68040, 0xE9C0, 0x1108);
        D(0) = 0x12345678;
        RUN(1);
        CHECK_EQ(D(1), 0x23);
        CHECK_EQ(CCR() & 0xF, 0);
        done(e);
    }
    {   /* BFEXTS D0{0:4},D1 */
        SETUP(M68K_68040, 0xEBC0, 0x1004);
        D(0) = 0xF2345678;
        RUN(1);
        CHECK_EQ(D(1), 0xFFFFFFFF);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* BFEXTU D0{28:8},D1: wraps from bit 0 to bit 31 */
        SETUP(M68K_68040, 0xE9C0, 0x1708);
        D(0) = 0x12345678;
        RUN(1);
        CHECK_EQ(D(1), 0x81);
        done(e);
    }
    {   /* BFINS D1,D0{8:16} */
        SETUP(M68K_68040, 0xEFC0, 0x1210);
        D(0) = 0x12345678; D(1) = 0xABCD;
        RUN(1);
        CHECK_EQ(D(0), 0x12ABCD78);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* BFFFO D0{0:0},D1: width 0 means 32 */
        SETUP(M68K_68040, 0xEDC0, 0x1000);
        D(0) = 0x00010000;
        RUN(1);
        CHECK_EQ(D(1), 15);
        done(e);
    }
    {   /* BFCHG (A0){D2:12} with D2 = -3: field starts at bit 5 of A0-1 */
        SETUP(M68K_68040, 0xEAD0, 0x088C);
        A(0) = 0x2000; D(2) = 0xFFFFFFFD;
        PUT(0x1FFF, 1, 0x0F); PUT(0x2000, 1, 0xF0); PUT(0x2001, 1, 0x00);
        RUN(1);
        CHECK_EQ(MEM(0x1FFF, 1), 0x08);
        CHECK_EQ(MEM(0x2000, 1), 0x0F);
        CHECK_EQ(MEM(0x2001, 1), 0x80);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* BFFFO (A0){D2:12},D1: the result is the offset plus the bit number */
        SETUP(M68K_68040, 0xEDD0, 0x188C);
        A(0) = 0x2000; D(2) = 0xFFFFFFFD;
        PUT(0x1FFF, 1, 0x0F); PUT(0x2000, 1, 0xF0);
        RUN(1);
        CHECK_EQ(D(1), 0xFFFFFFFD);
        done(e);
    }
    {   /* BFCLR (A0){4:32}: spans five bytes */
        SETUP(M68K_68040, 0xECD0, 0x0100);
        A(0) = 0x2000;
        for (unsigned i = 0; i < 5; i++) PUT(0x2000 + i, 1, 0xFF);
        RUN(1);
        CHECK_EQ(MEM(0x2000, 1), 0xF0);
        CHECK_EQ(MEM(0x2001, 3), 0);
        CHECK_EQ(MEM(0x2004, 1), 0x0F);
        done(e);
    }
}

/* --- CAS, CAS2, CMP2, CHK2 ------------------------------------------------- */

static void test_cas_cmp2(void) {
    {   /* CAS.L D1,D2,(A0): equal -> D2 to memory */
        SETUP(M68K_68040, 0x0ED0, 0x0081, 0x0ED0, 0x0081);
        A(0) = 0x2000; PUT(0x2000, 4, 5); D(1) = 5; D(2) = 0x99;
        RUN(1);
        CHECK_EQ(MEM(0x2000, 4), 0x99);
        CHECK(CCR() & 4);
        D(1) = 6;   /* not equal -> memory to D1 */
        RUN(1);
        CHECK_EQ(D(1), 0x99);
        CHECK(!(CCR() & 4));
        done(e);
    }
    {   /* CAS2.L D0:D1,D2:D3,(A0):(A1) */
        SETUP(M68K_68040, 0x0EFC, 0x8080, 0x90C1, 0x0EFC, 0x8080, 0x90C1);
        A(0) = 0x2000; A(1) = 0x3000;
        PUT(0x2000, 4, 1); PUT(0x3000, 4, 2);
        D(0) = 1; D(1) = 2; D(2) = 0x11; D(3) = 0x22;
        RUN(1);
        CHECK_EQ(MEM(0x2000, 4), 0x11);
        CHECK_EQ(MEM(0x3000, 4), 0x22);
        CHECK(CCR() & 4);
        RUN(1);   /* now unequal: both compare registers loaded */
        CHECK_EQ(D(0), 0x11);
        CHECK_EQ(D(1), 0x22);
        CHECK(!(CCR() & 4));
        done(e);
    }
    {   /* CMP2.W (A0),D1 with bounds $10..$20 */
        SETUP(M68K_68040, 0x02D0, 0x1000, 0x02D0, 0x1000, 0x02D0, 0x1000);
        A(0) = 0x2000; PUT(0x2000, 2, 0x10); PUT(0x2002, 2, 0x20);
        D(1) = 0x15; RUN(1); CHECK_EQ(CCR() & 5, 0);
        D(1) = 0x20; RUN(1); CHECK_EQ(CCR() & 5, 4);
        D(1) = 0x21; RUN(1); CHECK_EQ(CCR() & 5, 1);
        done(e);
    }
    {   /* signed bounds -5..5, Dn and An (An: bounds sign-extended, 32 bits) */
        SETUP(M68K_68040, 0x02D0, 0x1000, 0x02D0, 0x1000, 0x02D0, 0x9000, 0x02D0, 0x9000);
        A(0) = 0x2000; PUT(0x2000, 2, 0xFFFB); PUT(0x2002, 2, 0x0005);
        D(1) = 0xFFFE; RUN(1); CHECK_EQ(CCR() & 1, 0);
        D(1) = 0x0006; RUN(1); CHECK_EQ(CCR() & 1, 1);
        A(1) = 0xFFFFFFFE; RUN(1); CHECK_EQ(CCR() & 1, 0);
        A(1) = 0x0000FFFE; RUN(1); CHECK_EQ(CCR() & 1, 1);
        done(e);
    }
    {   /* CHK2.L (A0),D1 out of bounds: vector 6, format $2 */
        SETUP(M68K_68040, 0x04D0, 0x1800);
        A(0) = 0x2000; PUT(0x2000, 4, 0); PUT(0x2004, 4, 100);
        D(1) = 200;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(6));
        CHECK_EQ(A(7), STACK - 12);
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 4);
        CHECK_EQ(MEM(STACK - 6, 2), 0x2018);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        done(e);
    }
}

/* --- 32-bit multiply and divide ------------------------------------------- */

static void test_muldiv(void) {
    {   /* MULU.L D1,D2: overflow */
        SETUP(M68K_68040, 0x4C01, 0x2000);
        D(1) = 0x10000; D(2) = 0x10000;
        RUN(1);
        CHECK_EQ(D(2), 0);
        CHECK_EQ(CCR() & 0xF, 0x6);   /* Z, V */
        done(e);
    }
    {   /* MULS.L D1,D3:D2: -2 * 2^30 */
        SETUP(M68K_68040, 0x4C01, 0x2C03);
        D(1) = 0xFFFFFFFE; D(2) = 0x40000000;
        RUN(1);
        CHECK_EQ(D(3), 0xFFFFFFFF);
        CHECK_EQ(D(2), 0x80000000);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* DIVU.L D1,D2: 100 / 7 */
        SETUP(M68K_68040, 0x4C41, 0x2002);
        D(1) = 7; D(2) = 100;
        RUN(1);
        CHECK_EQ(D(2), 14);
        done(e);
    }
    {   /* DIVSL.L D1,D3:D2 (64/32): -100 / 7 = -14 remainder -2 */
        SETUP(M68K_68040, 0x4C41, 0x2C03);
        D(1) = 7; D(3) = 0xFFFFFFFF; D(2) = 0xFFFFFF9C;
        RUN(1);
        CHECK_EQ(D(2), 0xFFFFFFF2);
        CHECK_EQ(D(3), 0xFFFFFFFE);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* DIVU.L D1,D3:D2 overflow: 2^32 / 1 */
        SETUP(M68K_68040, 0x4C41, 0x2403);
        D(1) = 1; D(3) = 1; D(2) = 0;
        RUN(1);
        CHECK_EQ(D(3), 1);
        CHECK_EQ(D(2), 0);
        CHECK_EQ(CCR() & 3, 2);
        done(e);
    }
    {   /* DIVU.L by zero: vector 5, format $2, PC = next, address = the DIVU */
        SETUP(M68K_68040, 0x4C41, 0x2002);
        D(1) = 0;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(5));
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 4);
        CHECK_EQ(MEM(STACK - 6, 2), 0x2014);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        done(e);
    }
}

/* --- control registers (PRM MOVEC; MC68040 UM sections 3, 4) --------------- */

static void test_movec(void) {
    {   /* 68040: TC keeps E and P (P only here: E would translate through
         * the empty tables at 0); CACR DE and IE; CAAR is illegal */
        SETUP(M68K_68040, 0x4E7B, 0x0003, 0x4E7A, 0x1003, 0x4E7B, 0x0002, 0x4E7A, 0x2002, 0x4E7A, 0x0802);
        D(0) = 0xFFFF7FFF;
        RUN(1);
        D(0) = 0xFFFFFFFF;
        RUN(3);
        CHECK_EQ(D(1), 0x4000);
        CHECK_EQ(D(2), 0x80008000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(4));
        done(e);
    }
    {   /* 68030: no TC through MOVEC; CACR bits that read back */
        SETUP(M68K_68030, 0x4E7B, 0x0002, 0x4E7A, 0x2002, 0x4E7B, 0x0003);
        D(0) = 0xFFFFFFFF;
        RUN(2);
        CHECK_EQ(D(2), 0x3313);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(4));
        done(e);
    }
    {   /* USP through MOVEC; MSP becomes A7 when SR.M is set */
        SETUP(M68K_68040, 0x4E7B, 0x0800, 0x4E69, 0x4E7B, 0x1803, 0x46FC, 0x3700, 0x2E3C, 0x0000, 0x9100,
              0x46FC, 0x2700);
        D(0) = 0x1234; D(1) = 0x9000;
        RUN(4);
        CHECK_EQ(A(1), 0x1234);                 /* MOVE USP,A1 */
        CHECK_EQ(A(7), 0x9000);                 /* M set: A7 = MSP */
        CHECK_EQ(m68k_get_isp(cpu), STACK);
        RUN(2);                                 /* MOVE.L #$9100,D7 (not A7); then M cleared */
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(m68k_get_msp(cpu), 0x9000);
        done(e);
    }
}

/* --- exceptions and their frames (MC68040 UM section 8) -------------------- */

static void test_exceptions(void) {
    {   /* ILLEGAL: format $0, PC = the instruction */
        SETUP(M68K_68040, 0x4AFC);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(4));
        CHECK_EQ(A(7), STACK - 8);
        CHECK_EQ(MEM(STACK - 8, 2), 0x2700);
        CHECK_EQ(MEM(STACK - 6, 4), CODE);
        CHECK_EQ(MEM(STACK - 2, 2), 0x0010);
        done(e);
    }
    {   /* TRAP #3: format $0, PC = next */
        SETUP(M68K_68040, 0x4E43);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(35));
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 2);
        CHECK_EQ(MEM(STACK - 2, 2), 0x008C);
        done(e);
    }
    {   /* CHK.W D1,D0 with D0 < 0: format $2 */
        SETUP(M68K_68040, 0x4181);
        D(0) = 0xFFFF; D(1) = 10;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(6));
        CHECK_EQ(MEM(STACK - 6, 2), 0x2018);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        CHECK(CCR() & 8);
        done(e);
    }
    {   /* T1 trace: format $2, PC = next, address = the traced instruction */
        SETUP(M68K_68040, 0x4E71, 0x4E71);
        m68k_set_sr(cpu, 0xA700);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(9));
        CHECK_EQ(MEM(STACK - 12, 2), 0xA700);
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 2);
        CHECK_EQ(MEM(STACK - 6, 2), 0x2024);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        CHECK_EQ(m68k_sr(cpu) & 0xC000, 0);
        done(e);
    }
    {   /* T0 trace: only the change of flow is traced */
        SETUP(M68K_68040, 0x4E71, 0x6002, 0x4E71, 0x4E71);
        m68k_set_sr(cpu, 0x6700);
        RUN(1);
        CHECK_EQ(cpu->pc, CODE + 2);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(9));
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 6);
        done(e);
    }
    {   /* privilege violation from user mode */
        SETUP(M68K_68040, 0x46FC, 0x0000);
        cpu->usp = 0x7000;
        m68k_set_sr(cpu, 0x0000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        CHECK_EQ(A(7), STACK - 8);
        CHECK_EQ(MEM(STACK - 6, 4), CODE);
        CHECK_EQ(m68k_get_usp(cpu), 0x7000);
        done(e);
    }
    {   /* line A, line F, BKPT */
        SETUP(M68K_68040, 0xA123, 0xFF00, 0x4848);
        RUN(1); CHECK_EQ(cpu->pc, HANDLER(10)); CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        cpu->pc = CODE + 2; RUN(1); CHECK_EQ(cpu->pc, HANDLER(11)); CHECK_EQ(MEM(A(7) + 2, 4), CODE + 2);
        cpu->pc = CODE + 4; RUN(1); CHECK_EQ(cpu->pc, HANDLER(4));
        done(e);
    }
    {   /* RTE: a format $F frame is a format error (vector 14, PC = the RTE) */
        SETUP(M68K_68040, 0x4E73);
        A(7) = STACK - 8;
        PUT(STACK - 8, 2, 0x2700); PUT(STACK - 6, 4, 0x3000); PUT(STACK - 2, 2, 0xF000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(14));
        CHECK_EQ(MEM(A(7) + 2, 4), CODE);
        done(e);
    }
    {   /* RTE of a format $2 frame pops 12 bytes; of a $7 frame, 60 */
        SETUP(M68K_68040, 0x4E73);
        A(7) = STACK - 12;
        PUT(STACK - 12, 2, 0x2004); PUT(STACK - 10, 4, 0x3000); PUT(STACK - 6, 2, 0x2018);
        RUN(1);
        CHECK_EQ(cpu->pc, 0x3000);
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(CCR(), 4);
        cpu->pc = CODE;
        A(7) = STACK - 60;
        PUT(STACK - 60, 2, 0x2700); PUT(STACK - 58, 4, 0x3100); PUT(STACK - 54, 2, 0x7008);
        RUN(1);
        CHECK_EQ(cpu->pc, 0x3100);
        CHECK_EQ(A(7), STACK);
        done(e);
    }
    {   /* address error: a jump to an odd address (format $2) */
        SETUP(M68K_68040, 0x4EF9, 0x0000, 0x2001);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(3));
        CHECK_EQ(MEM(STACK - 10, 4), CODE);
        CHECK_EQ(MEM(STACK - 6, 2), 0x200C);
        CHECK_EQ(MEM(STACK - 4, 4), 0x2000);    /* bit 0 cleared (MC68040 UM 8.4.6.7) */
        done(e);
    }
    {   /* 68040: a Bcc not taken with an odd offset still takes the address
         * error (MC68040 UM 8.2.2); the 68030 falls through */
        for (int m = 0; m < 2; m++) {
            SETUP(m ? M68K_68030 : M68K_68040, 0x6701, 0x4E71);
            RUN(1);
            if (m) {
                CHECK_EQ(cpu->pc, CODE + 2);
            } else {
                CHECK_EQ(cpu->pc, HANDLER(3));
                CHECK_EQ(MEM(STACK - 10, 4), CODE);
                CHECK_EQ(MEM(STACK - 4, 4), CODE + 2);
            }
            done(e);
        }
    }
    {   /* privileged 68040 cache instruction from user mode */
        SETUP(M68K_68040, 0xF4D8);
        cpu->usp = 0x7000;
        m68k_set_sr(cpu, 0x0000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
}

/* --- interrupts (MC68040 UM section 8, "Interrupt Exceptions") ------------- */

static void test_interrupts(void) {
    {   /* autovector, mask raised to the level, PC = next instruction */
        SETUP(M68K_68040, 0x4E71, 0x4E71);
        m68k_set_sr(cpu, 0x2200);
        m68k_set_irq(cpu, 3);
        RUN(0);
        CHECK_EQ(cpu->pc, HANDLER(27));
        CHECK_EQ(MEM(STACK - 6, 4), CODE);
        CHECK_EQ(MEM(STACK - 2, 2), 27 * 4);
        CHECK_EQ(m68k_sr(cpu) & 0x0700, 0x0300);
        CHECK_EQ(e->bus.iack_level, 3);
        done(e);
    }
    {   /* masked level: not taken; vectored; spurious */
        SETUP(M68K_68040, 0x4E71, 0x4E71, 0x4E71);
        m68k_set_sr(cpu, 0x2300);
        m68k_set_irq(cpu, 3);
        RUN(1);
        CHECK_EQ(cpu->pc, CODE + 2);
        e->bus.iack = 0x40;
        m68k_set_irq(cpu, 4);
        RUN(0);
        CHECK_EQ(cpu->pc, HANDLER(64));
        cpu->pc = CODE;
        m68k_set_sr(cpu, 0x2000);
        e->bus.iack = M68K_IACK_SPURIOUS;
        RUN(0);
        CHECK_EQ(cpu->pc, HANDLER(24));
        done(e);
    }
    {   /* level 7 is taken once per transition, whatever the mask */
        SETUP(M68K_68040, 0x4E71, 0x4E71, 0x4E71);
        m68k_set_irq(cpu, 7);
        RUN(0);
        CHECK_EQ(cpu->pc, HANDLER(31));
        CHECK_EQ(e->nvec, 1);
        RUN(1);
        CHECK_EQ(e->nvec, 1);
        done(e);
    }
    {   /* M set: format $0 on the master stack, format $1 throwaway on the
         * interrupt stack with M still set; RTE goes through both */
        SETUP(M68K_68040, 0x4E71, 0x4E71);
        cpu->msp = 0x9000;
        m68k_set_sr(cpu, 0x3000);
        CHECK_EQ(A(7), 0x9000);
        PUT(HANDLER(26), 2, 0x4E73);   /* the handler returns at once */
        m68k_set_irq(cpu, 2);
        RUN(0);
        CHECK_EQ(cpu->pc, HANDLER(26));
        CHECK_EQ(m68k_get_msp(cpu), 0x9000 - 8);
        CHECK_EQ(MEM(0x9000 - 2, 2), 26 * 4);
        CHECK_EQ(A(7), STACK - 8);
        CHECK_EQ(MEM(STACK - 8, 2), 0x3200);
        CHECK_EQ(MEM(STACK - 2, 2), 0x1000 | 26 * 4);
        CHECK(!(m68k_sr(cpu) & 0x1000));
        m68k_set_irq(cpu, 0);
        RUN(1);   /* RTE: throwaway, then the master frame */
        CHECK_EQ(cpu->pc, CODE);
        CHECK_EQ(m68k_sr(cpu), 0x3000);
        CHECK_EQ(A(7), 0x9000);
        CHECK_EQ(m68k_get_isp(cpu), STACK);
        done(e);
    }
    {   /* STOP waits for an interrupt above the new mask */
        SETUP(M68K_68040, 0x4E72, 0x2100, 0x4E71);
        CHECK_EQ(RUN(10), M68K_STOPPED);
        CHECK_EQ(cpu->pc, CODE + 4);
        CHECK_EQ(RUN(10), M68K_STOPPED);
        m68k_set_irq(cpu, 1);
        CHECK_EQ(RUN(10), M68K_STOPPED);
        m68k_set_irq(cpu, 2);
        RUN(1);
        CHECK_EQ(MEM(STACK - 6, 4), CODE + 4);
        CHECK_EQ(cpu->pc, HANDLER(26) + 2);
        done(e);
    }
    {   /* RESET reaches the machine */
        SETUP(M68K_68040, 0x4E70);
        RUN(1);
        CHECK_EQ(e->bus.resets, 1);
        done(e);
    }
}

/* --- access faults (MC68040 UM section 8, format $7) ----------------------- */

static void test_faults(void) {
    {   /* read fault: the instruction is undone and restarted by RTE */
        SETUP(M68K_68040, 0x2018, 0x4E71);
        PUT(HANDLER(2), 2, 0x4E73);
        A(0) = 0x30000;
        PUT(0x30000, 4, 0xDEADBEEF);
        tb_unmap(&e->bus, 0x30000, 0x31000);
        e->bus.fault_lo = 0x30000; e->bus.fault_hi = 0x31000;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(A(0), 0x30000);
        CHECK_EQ(A(7), STACK - 60);
        const uint32_t f = STACK - 60;
        CHECK_EQ(MEM(f + 2, 4), CODE);              /* PC: the faulted instruction */
        CHECK_EQ(MEM(f + 6, 2), 0x7008);
        CHECK_EQ(MEM(f + 0x0C, 2), 0x0105);         /* SSW: RW read, long, TM 5 */
        CHECK_EQ(MEM(f + 0x14, 4), 0x30000);        /* FA */
        e->bus.fault_hi = 0;                        /* the handler "maps the page" */
        RUN(2);                                     /* RTE, then MOVE.L again */
        CHECK_EQ(D(0), 0xDEADBEEF);
        CHECK_EQ(A(0), 0x30004);
        CHECK_EQ(A(7), STACK);
        done(e);
    }
    {   /* write bus error: completes, access error at the boundary, the
         * write in WB1 (MC68040 UM 8.4.6.7, case 3: FA = WB1A) */
        SETUP(M68K_68040, 0x2080, 0x4E71);
        A(0) = 0x30010; D(0) = 0x12345678;
        tb_unmap(&e->bus, 0x30000, 0x31000);
        e->bus.fault_lo = 0x30000; e->bus.fault_hi = 0x31000; e->bus.fault_writes_only = 1;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        const uint32_t f = STACK - 60;
        CHECK_EQ(MEM(f + 2, 4), CODE + 2);          /* PC: next instruction */
        CHECK_EQ(MEM(f + 0x0C, 2), 0x0005);         /* SSW: write, long, TM 5 */
        CHECK_EQ(MEM(f + 0x12, 2), 0x0085);         /* WB1S: valid, long, TM 5 */
        CHECK_EQ(MEM(f + 0x28, 4), 0x30010);        /* WB1A */
        CHECK_EQ(MEM(f + 0x2C, 4), 0x12345678);     /* WB1D */
        CHECK_EQ(MEM(f + 0x10, 2), 0);              /* WB2S empty */
        CHECK_EQ(MEM(f + 0x14, 4), 0x30010);
        done(e);
    }
    {   /* WB1D is memory aligned (Table 8-5): a word at offset 1 in lanes 2-1,
         * a byte at offset 3 in lane 0; a second write of the instruction
         * waits in WB2, register aligned */
        SETUP(M68K_68040, 0x3140, 0x0001, 0x4E71);  /* MOVE.W D0,(1,A0) */
        A(0) = 0x30010; D(0) = 0xABCD;
        tb_unmap(&e->bus, 0x30000, 0x31000);
        e->bus.fault_lo = 0x30000; e->bus.fault_hi = 0x31000; e->bus.fault_writes_only = 1;
        RUN(1);
        const uint32_t f = STACK - 60;
        CHECK_EQ(MEM(f + 0x12, 2), 0x00C5);         /* WB1S: valid, word, TM 5 */
        CHECK_EQ(MEM(f + 0x2C, 4), 0x00ABCD00);
        done(e);
    }
    {   /* instruction fetch fault: TM = supervisor program */
        SETUP(M68K_68040, 0x4EF9, 0x0003, 0x0000);
        tb_unmap(&e->bus, 0x30000, 0x31000);
        e->bus.fault_lo = 0x30000; e->bus.fault_hi = 0x31000;
        RUN(2);
        CHECK_EQ(cpu->pc, HANDLER(2));
        const uint32_t f = STACK - 60;
        CHECK_EQ(MEM(f + 2, 4), 0x30000);
        CHECK_EQ(MEM(f + 0x0C, 2), 0x0146);         /* read, word, TM 6 */
        done(e);
    }
    {   /* 68030: a bus error builds a format $B frame */
        SETUP(M68K_68030, 0x2010);
        A(0) = 0x30000;
        tb_unmap(&e->bus, 0x30000, 0x31000);
        e->bus.fault_lo = 0x30000; e->bus.fault_hi = 0x31000;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(A(7), STACK - 92);
        CHECK_EQ(MEM(STACK - 92 + 6, 2), 0xB008);
        CHECK_EQ(MEM(STACK - 92 + 0x10, 4), 0x30000);
        done(e);
    }
    {   /* misaligned long split into byte, word, byte cycles (MC68040 UM 7) */
        SETUP(M68K_68040, 0x2010);
        A(0) = 0x30001;
        tb_unmap(&e->bus, 0x30000, 0x31000);
        PUT(0x30001, 4, 0x0A0B0C0D);
        RUN(1);
        CHECK_EQ(D(0), 0x0A0B0C0D);
        done(e);
    }
}

/* --- other 68020+/68040 instructions --------------------------------------- */

static void test_misc(void) {
    {   /* PACK D0,D1,#0; UNPK D0,D2,#$3030 */
        SETUP(M68K_68040, 0x8340, 0x0000, 0x8580, 0x3030);
        D(0) = 0x0304;
        RUN(1);
        CHECK_EQ(D(1) & 0xFF, 0x34);
        D(0) = 0x34;
        RUN(1);
        CHECK_EQ(D(2) & 0xFFFF, 0x3334);
        done(e);
    }
    {   /* PACK -(A0),-(A1),#0 */
        SETUP(M68K_68040, 0x8348, 0x0000);
        A(0) = 0x2002; A(1) = 0x3001;
        PUT(0x2000, 1, 0x05); PUT(0x2001, 1, 0x07);
        RUN(1);
        CHECK_EQ(MEM(0x3000, 1), 0x57);
        CHECK_EQ(A(0), 0x2000);
        CHECK_EQ(A(1), 0x3000);
        done(e);
    }
    {   /* EXTB.L; LINK.L A6,#-8 */
        SETUP(M68K_68040, 0x49C0, 0x480E, 0xFFFF, 0xFFF8);
        D(0) = 0x80; A(6) = 0xAAAA;
        RUN(2);
        CHECK_EQ(D(0), 0xFFFFFF80);
        CHECK_EQ(MEM(STACK - 4, 4), 0xAAAA);
        CHECK_EQ(A(6), STACK - 4);
        CHECK_EQ(A(7), STACK - 12);
        done(e);
    }
    {   /* RTD #8 */
        SETUP(M68K_68040, 0x4E74, 0x0008);
        A(7) = STACK - 4;
        PUT(STACK - 4, 4, 0x2000);
        RUN(1);
        CHECK_EQ(cpu->pc, 0x2000);
        CHECK_EQ(A(7), STACK + 8);
        done(e);
    }
    {   /* TRAPEQ.W with Z set: vector 7, format $2 */
        SETUP(M68K_68040, 0x57FA, 0x1234);
        m68k_set_ccr(cpu, 4);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(7));
        CHECK_EQ(MEM(STACK - 10, 4), CODE + 4);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        done(e);
    }
    {   /* MOVE16 (A0)+,(A1)+ and (A0)+,($4000).L */
        SETUP(M68K_68040, 0xF620, 0x9000, 0xF600, 0x0000, 0x4000);
        A(0) = 0x2004; A(1) = 0x3008;
        for (unsigned i = 0; i < 32; i++) PUT(0x2000 + i, 1, i + 1);
        RUN(1);
        CHECK_EQ(MEM(0x3000, 4), 0x01020304);
        CHECK_EQ(MEM(0x300C, 4), 0x0D0E0F10);
        CHECK_EQ(A(0), 0x2014);
        CHECK_EQ(A(1), 0x3018);
        RUN(1);
        CHECK_EQ(MEM(0x4000, 4), 0x11121314);
        CHECK_EQ(A(0), 0x2024);
        done(e);
    }
    {   /* MOVES.L D0,(A0) uses DFC */
        SETUP(M68K_68040, 0x4E7B, 0x1001, 0x0E90, 0x0800);
        D(1) = 1; D(0) = 0x55AA55AA; A(0) = 0x30000;
        tb_unmap(&e->bus, 0x30000, 0x31000);
        RUN(2);
        CHECK_EQ(MEM(0x30000, 4), 0x55AA55AA);
        CHECK_EQ(e->bus.last_fc, 1);
        done(e);
    }
    {   /* MOVEM.L A0/A7,-(A7): A7 stored as its initial value minus 4 */
        SETUP(M68K_68040, 0x48E7, 0x0081);
        A(0) = 0x1111;
        RUN(1);
        CHECK_EQ(MEM(STACK - 4, 4), STACK - 4);
        CHECK_EQ(MEM(STACK - 8, 4), 0x1111);
        CHECK_EQ(A(7), STACK - 8);
        done(e);
    }
    {   /* Bcc.L (32-bit displacement), CMPI.W #,(d16,PC), TST.W A0 */
        SETUP(M68K_68040, 0x60FF, 0x0000, 0x0010);
        RUN(1);
        CHECK_EQ(cpu->pc, CODE + 2 + 0x10);
        done(e);
    }
    {
        SETUP(M68K_68040, 0x0C7A, 0x4E71, 0xFFFE, 0x4A48);
        A(0) = 0x8000;
        RUN(1);
        CHECK(CCR() & 4);       /* compares with the CMPI's own extension word */
        RUN(1);
        CHECK_EQ(CCR() & 0xF, 0x8);
        done(e);
    }
    {   /* 68LC040: an FP instruction raises line F with a format $4 frame:
         * the next PC, the effective address, the instruction's PC (MC68040
         * UM A.5.1) */
        SETUP(M68K_68LC040, 0xF228, 0x4000, 0x0010, 0x4E71);   /* FMOVE.L (16,A0),FP0 */
        A(0) = 0x3000;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(11));
        CHECK_EQ(A(7), STACK - 16);
        CHECK_EQ(MEM(STACK - 14, 4), CODE + 6);
        CHECK_EQ(MEM(STACK - 10, 2), 0x402C);
        CHECK_EQ(MEM(STACK - 8, 4), 0x3010);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        CHECK_EQ(A(0), 0x3000);
        done(e);
    }
    {   /* a 68030 without an FPU: coprocessor instructions take line F */
        SETUP(M68K_68030, 0xF200, 0x0000);
        m68k_set_fpu(cpu, M68K_FPU_NONE, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(11));
        CHECK_EQ(MEM(STACK - 6, 4), CODE);
        CHECK_EQ(MEM(STACK - 2, 2), 0x002C);
        done(e);
    }
}

/* --- the 68000 (m68k_000.c; MC68000 UM sections 5, 6 and 8) ---------------- */

static unsigned wait_calls;
static unsigned wait2(void *ctx, uint32_t address, unsigned fc, unsigned kind, uint64_t cycle) {
    (void)ctx;
    (void)address;
    (void)fc;
    (void)kind;
    (void)cycle;
    wait_calls++;
    return 2;
}

static void test_68000(void) {
    /* The 68000's instruction set: 68010 and 68020 additions are illegal,
     * every line F opcode is line F. */
    CHECK_EQ(m68k_table_conflicts(M68K_68000), 0);
    CHECK(!m68k_decode(M68K_68000, 0x49C0).valid);                   /* EXTB.L */
    CHECK(!m68k_decode(M68K_68000, 0x4E7A).valid);                   /* MOVEC */
    CHECK(!m68k_decode(M68K_68000, 0x4E74).valid);                   /* RTD */
    CHECK(!m68k_decode(M68K_68000, 0x42C0).valid);                   /* MOVE from CCR */
    CHECK(!m68k_decode(M68K_68000, 0x4C00).valid);                   /* MULU.L */
    CHECK(!m68k_decode(M68K_68000, 0x0C7A).valid);                   /* CMPI.W (d16,PC) */
    CHECK(!m68k_decode(M68K_68000, 0x4A48).valid);                   /* TST.W A0 */
    CHECK(!m68k_decode(M68K_68000, 0xE8C0).valid);                   /* BFTST */
    CHECK(!m68k_decode(M68K_68000, 0x0AD0).valid);                   /* CAS.B */
    CHECK_STR(m68k_decode(M68K_68000, 0xF200).name, "line F");
    CHECK_STR(m68k_decode(M68K_68000, 0x40C0).name, "MOVE from SR");
    CHECK_STR(m68k_model_name(M68K_68000), "68000");
    {   /* MOVE.W $20000,D0 on a bus error: the group 0 frame (UM 6.3.9):
         * R/W, I/N, FC over IR's upper bits; the address; IR; SR; PC. */
        SETUP(M68K_68000, 0x3039, 0x0002, 0x0000, 0x4E71);
        e->bus.fault_lo = 0x20000;
        e->bus.fault_hi = 0x20002;
        tb_unmap(&e->bus, 0x20000, 0x21000);
        RUN(1);
        CHECK_EQ(e->nvec, 1);
        CHECK_EQ(e->vectors[0], 2);
        CHECK_EQ(A(7), STACK - 14);
        CHECK_EQ(MEM(STACK - 14, 2), (0x3039u & 0xFFE0u) | 0x10u | 5u);
        CHECK_EQ(MEM(STACK - 12, 4), 0x20000);
        CHECK_EQ(MEM(STACK - 8, 2), 0x3039);
        CHECK_EQ(MEM(STACK - 6, 2), 0x2700);
        CHECK_EQ(MEM(STACK - 4, 4), CODE + 4);
        CHECK_EQ(cpu->pc, HANDLER(2));
        done(e);
    }
    {   /* Line A: the 3-word frame with the instruction's own address; RTE
         * pops 6 bytes. */
        SETUP(M68K_68000, 0xA123);
        tb_put(&e->bus, HANDLER(10), 2, 0x4E73);    /* RTE */
        RUN(1);
        CHECK_EQ(e->vectors[0], 10);
        CHECK_EQ(A(7), STACK - 6);
        CHECK_EQ(MEM(STACK - 6, 2), 0x2700);
        CHECK_EQ(MEM(STACK - 4, 4), CODE);
        RUN(1);
        CHECK_EQ(A(7), STACK);
        CHECK_EQ(cpu->pc, CODE);
        done(e);
    }
    {   /* MOVE from SR is not privileged on the 68000; a 68020 opcode is an
         * illegal instruction. */
        SETUP(M68K_68000, 0x40C0, 0x49C0);
        m68k_set_sr(cpu, 0x0004);
        cpu->usp = STACK - 0x100;
        RUN(1);
        CHECK_EQ(e->nvec, 0);
        CHECK_EQ(D(0) & 0xFFFF, 0x0004);
        RUN(1);
        CHECK_EQ(e->vectors[0], 4);
        CHECK_EQ(MEM(STACK - 4, 4), CODE + 2);
        done(e);
    }
    {   /* An autovectored interrupt (UM 6.3.2): 44 clocks, the mask raised,
         * the frame's PC the next instruction; NOP 4 clocks. */
        SETUP(M68K_68000, 0x4E71, 0x4E71);
        m68k_set_timing(cpu, 1);
        m68k_set_sr(cpu, 0x2000);
        RUN(1);
        CHECK_EQ(cpu->cycles, 4);
        m68k_set_irq(cpu, 3);
        RUN(1);
        CHECK_EQ(e->vectors[0], 27);
        CHECK_EQ(m68k_sr(cpu) & 0x0700u, 0x0300u);
        CHECK_EQ(MEM(STACK - 4, 4), CODE + 2);
        CHECK_EQ(cpu->cycles, 4 + 44);
        CHECK_EQ(cpu->pc, HANDLER(27));
        done(e);
    }
    {   /* Wait states stretch every bus cycle: NOP's prefetch 4 + 2. */
        SETUP(M68K_68000, 0x4E71);
        cpu->bus.wait = wait2;
        m68k_set_timing(cpu, 1);
        wait_calls = 0;
        RUN(1);
        CHECK_EQ(cpu->cycles, 6);
        CHECK_EQ(wait_calls, 1);
        done(e);
    }
    {   /* The 24-bit bus: $FF001000 is $001000; SR keeps T S I X N Z V C. */
        SETUP(M68K_68000, 0x3039, 0xFF00, 0x2000);
        PUT(0x2000, 2, 0x1234);
        RUN(1);
        CHECK_EQ(D(0) & 0xFFFF, 0x1234);
        m68k_set_sr(cpu, 0xFFFF);
        CHECK_EQ(m68k_sr(cpu), 0xA71F);
        done(e);
    }
}

int mc_cpu_unit(void) {
    test_tables();
    test_addressing();
    test_bitfields();
    test_cas_cmp2();
    test_muldiv();
    test_movec();
    test_exceptions();
    test_interrupts();
    test_faults();
    test_misc();
    test_68000();
    printf("cpu: %d failure(s)\n", mc_test_failures);
    return mc_test_failures;
}
