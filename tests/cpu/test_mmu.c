/* mc_cpu_tests mmu: the 68040 and 68030 MMUs and the cache instructions.
 * Each case builds translation tables in RAM as the MC68040 User's Manual
 * section 3 (and the MC68030 User's Manual section 9) lays them out, runs a
 * few instructions and checks what the manual says the chip does: the
 * translation, the U and M bits written back, the access fault and its SSW,
 * the ATC's flush semantics, PTEST's MMUSR. */
#include "cpu_test.h"
#include "../test.h"

#define CODE 0x1000u
#define STACK 0x8000u
#define HANDLERS 0x10000u
#define ROOT 0x80000u       /* 68040 root table; 68030 table A */
#define HEAP 0x88000u       /* further tables */
#define IOREGS 0x9000u      /* PMOVE operands */

typedef struct env {
    testbus bus;
    m68k_cpu *cpu;
    unsigned vectors[8], nvec;
    uint32_t heap;
    int p8k;
    unsigned cache_calls, cache_caches, cache_scope;
    uint32_t cache_addr;
} env;

static void hook(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc) {
    env *e = ctx;
    (void)cpu;
    (void)pc;
    if (e->nvec < 8) e->vectors[e->nvec] = vector;
    e->nvec++;
}
static void cache_hook(void *ctx, unsigned caches, unsigned scope, uint32_t address) {
    env *e = ctx;
    e->cache_calls++;
    e->cache_caches = caches;
    e->cache_scope = scope;
    e->cache_addr = address;
}

static env *setup(m68k_model model, const uint16_t *code, size_t words) {
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
    m68k_reset(e->cpu);
    e->cpu->isp = STACK;
    e->cpu->r[15] = STACK;
    e->cpu->pc = CODE;
    e->cpu->on_exception = hook;
    e->cpu->hook_ctx = e;
    e->cpu->on_cache = cache_hook;
    e->cpu->cache_ctx = e;
    e->heap = HEAP;
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
#define MOVEC(code, v) m68k_movec_write(cpu, (code), (v))

/* --- 68040 tables (MC68040 UM 3.2) ---------------------------------------- */

static uint32_t alloc(env *e, uint32_t size) {
    e->heap = (e->heap + size - 1) & ~(size - 1);
    const uint32_t a = e->heap;
    for (uint32_t i = 0; i < size; i += 4) PUT(a + i, 4, 0);
    e->heap += size;
    return a;
}
/* Address of the page descriptor for `la`, creating the tables on the way
 * (resident, UDT = 11). */
static uint32_t pd_addr(env *e, uint32_t la) {
    const uint32_t ra = ROOT + 4 * (la >> 25);
    if (!(MEM(ra, 4) & 2)) PUT(ra, 4, alloc(e, 512) | 3);
    const uint32_t pa = (MEM(ra, 4) & ~0x1FFu) + 4 * ((la >> 18) & 0x7F);
    const uint32_t psize = e->p8k ? 128u : 256u;
    if (!(MEM(pa, 4) & 2)) PUT(pa, 4, alloc(e, psize) | 3);
    const uint32_t base = MEM(pa, 4) & ~(psize - 1);
    return base + 4 * (e->p8k ? (la >> 13) & 0x1F : (la >> 12) & 0x3F);
}
static void map(env *e, uint32_t la, uint32_t pd) { PUT(pd_addr(e, la), 4, pd); }
/* RAM 0-1 MiB identity mapped, tables at ROOT, URP = SRP = ROOT, TC set. */
static void mmu040(env *e, int p8k) {
    m68k_cpu *cpu = e->cpu;
    e->p8k = p8k;
    for (uint32_t a = ROOT; a < ROOT + 512; a += 4) PUT(a, 4, 0);
    const uint32_t ps = p8k ? 0x2000u : 0x1000u;
    for (uint32_t a = 0; a < 0x100000u; a += ps) map(e, a, a | 1u);
    MOVEC(0x806, ROOT);
    MOVEC(0x807, ROOT);
    MOVEC(0x003, p8k ? 0xC000u : 0x8000u);
}

static void test_walk(void) {
    {   /* 4 KiB pages: $00400010 -> $20010; U set at all three levels, M on
         * the first write (MC68040 UM 3.2.5, Table 3-1) */
        SETUP(M68K_68040, 0x2010, 0x2081);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020001);
        PUT(0x20010, 4, 0xCAFEF00D);
        A(0) = 0x00400010; D(1) = 0x11223344;
        RUN(1);
        CHECK_EQ(D(0), 0xCAFEF00D);
        const uint32_t pd = pd_addr(e, 0x00400000);
        CHECK(MEM(ROOT, 4) & 8);
        CHECK(MEM((MEM(ROOT, 4) & ~0x1FFu) + 4 * 16, 4) & 8);
        CHECK_EQ(MEM(pd, 4), 0x00020009);       /* U set, M clear */
        RUN(1);
        CHECK_EQ(MEM(0x20010, 4), 0x11223344);
        CHECK_EQ(MEM(pd, 4), 0x00020019);       /* M set by the write */
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* 8 KiB pages: PGI = LA17-13, the page offset 13 bits */
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 1);
        map(e, 0x00402000, 0x00030001);
        PUT(0x31FFC, 4, 0x55AA55AA);
        A(0) = 0x00403FFC;
        RUN(1);
        CHECK_EQ(D(0), 0x55AA55AA);
        done(e);
    }
    {   /* indirect descriptor: U is set in the descriptor it points to */
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 0);
        PUT(0x84000, 4, 0x00020001);
        map(e, 0x00400000, 0x00084002);
        PUT(0x20004, 4, 0x01020304);
        A(0) = 0x00400004;
        RUN(1);
        CHECK_EQ(D(0), 0x01020304);
        CHECK_EQ(MEM(0x84000, 4), 0x00020009);
        CHECK_EQ(MEM(pd_addr(e, 0x00400000), 4), 0x00084002);
        done(e);
    }
}

/* An access error from a read at A0 (MOVE.L (A0),D0): the frame, SSW. */
static void expect_read_fault(env *e, uint32_t fa, unsigned ssw) {
    m68k_cpu *cpu = e->cpu;
    CHECK_EQ(cpu->pc, HANDLER(2));
    CHECK_EQ(MEM(STACK - 60 + 6, 2), 0x7008);
    CHECK_EQ(MEM(STACK - 60 + 2, 4), CODE);
    CHECK_EQ(MEM(STACK - 60 + 0x0C, 2), ssw);
    CHECK_EQ(MEM(STACK - 60 + 0x14, 4), fa);
}

static void test_faults(void) {
    static const struct { uint32_t la; int kind; } cases[] = {
        {0x02000010, 0},    /* root descriptor invalid (UDT 00) */
        {0x00800010, 1},    /* pointer descriptor invalid */
        {0x00400010, 2},    /* page descriptor invalid (PDT 00) */
        {0x00400010, 3},    /* indirect to an indirect: invalid */
    };
    for (unsigned i = 0; i < 4; i++) {
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 0);
        if (cases[i].kind == 1) PUT((MEM(ROOT, 4) & ~0x1FFu) + 4 * 32, 4, 0);
        if (cases[i].kind == 2) map(e, cases[i].la, 0);
        if (cases[i].kind == 3) {
            PUT(0x84000, 4, 0x00085002);
            map(e, cases[i].la, 0x00084002);
        }
        A(0) = cases[i].la;
        RUN(1);
        /* ATC (10), RW read (8), long, TT 00, TM 5 */
        expect_read_fault(e, cases[i].la, 0x0505);
        CHECK_EQ(A(0), cases[i].la);
        done(e);
    }
    {   /* write protection in the root descriptor: reads pass, a write waits
         * in WB2 with WB1 empty (MC68040 UM 8.4.6.7, case 5) */
        SETUP(M68K_68040, 0x2010, 0x2081, 0x4E71);
        mmu040(e, 0);
        map(e, 0x02400000, 0x00020001);             /* root entry 1: the stack stays writable */
        PUT(ROOT + 4, 4, MEM(ROOT + 4, 4) | 4);
        A(0) = 0x02400010; D(1) = 0xDEADBEEF;
        RUN(1);
        CHECK_EQ(e->nvec, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        const uint32_t f = STACK - 60;
        CHECK_EQ(MEM(f + 2, 4), CODE + 4);          /* the next instruction */
        CHECK_EQ(MEM(f + 0x0C, 2), 0x0405);         /* ATC, write, long, TM 5 */
        CHECK_EQ(MEM(f + 0x12, 2), 0);              /* WB1S */
        CHECK_EQ(MEM(f + 0x10, 2), 0x0085);         /* WB2S */
        CHECK_EQ(MEM(f + 0x20, 4), 0x02400010);
        CHECK_EQ(MEM(f + 0x24, 4), 0xDEADBEEF);
        CHECK_EQ(MEM(0x20010, 4), 0);
        CHECK_EQ(MEM(pd_addr(e, 0x02400000), 4) & 0x10, 0);   /* no M under write protection */
        done(e);
    }
    {   /* supervisor-only page: the supervisor reads it, the user faults */
        SETUP(M68K_68040, 0x2010, 0x2010);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020081);
        PUT(0x20000, 4, 0x77777777);
        A(0) = 0x00400000;
        RUN(1);
        CHECK_EQ(D(0), 0x77777777);
        D(0) = 0;
        cpu->usp = 0x7000;
        m68k_set_sr(cpu, 0x0000);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(STACK - 60 + 0x0C, 2), 0x0501);   /* ATC, read, long, TM 1 */
        CHECK_EQ(D(0), 0);
        done(e);
    }
    {   /* a long spanning into an invalid page: FA the first byte, MA set */
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020001);
        map(e, 0x00401000, 0);
        A(0) = 0x00400FFE;
        RUN(1);
        expect_read_fault(e, 0x00400FFE, 0x0D05);
        done(e);
    }
    {   /* a bus error during the table search is an ATC fault too */
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 0);
        PUT(ROOT + 4 * 1, 4, 0x00F00003);           /* pointer table outside RAM */
        A(0) = 0x02000000;
        RUN(1);
        expect_read_fault(e, 0x02000000, 0x0505);
        done(e);
    }
    {   /* TAS: the read of a read-modify-write is checked as a write; the
         * fault restarts the instruction with LK set */
        SETUP(M68K_68040, 0x4AD0);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020005);             /* write protected */
        A(0) = 0x00400000;
        RUN(1);
        expect_read_fault(e, 0x00400000, 0x0725);   /* ATC, LK, read, byte, TM 5 */
        done(e);
    }
}

/* --- transparent translation (MC68040 UM 3.1.3, 3.4) ------------------------ */

static void test_tt(void) {
    {   /* DTT0 maps $00xxxxxx for data; instruction fetches still use the
         * tables (no ITT): the data read sees physical $5010, not the page
         * the tables give */
        SETUP(M68K_68040, 0x2010);
        mmu040(e, 0);
        map(e, 0x00005000, 0x00020001);
        PUT(0x5010, 4, 0x11111111);
        PUT(0x20010, 4, 0x22222222);
        MOVEC(0x006, 0x0000C000u);                  /* base 0, mask 0, E, S = 10 */
        A(0) = 0x5010;
        RUN(1);
        CHECK_EQ(D(0), 0x11111111);
        MOVEC(0x006, 0x00008000u);                  /* S = 00: user accesses only */
        RUN(0);
        cpu->pc = CODE;
        RUN(1);
        CHECK_EQ(D(0), 0x22222222);
        done(e);
    }
    {   /* a write-protected transparent block faults writes even with the
         * tables off (the TTRs work whatever E is) */
        SETUP(M68K_68040, 0x2081, 0x4E71);
        MOVEC(0x007, 0x0100C004u);                  /* DTT1: $01xxxxxx, W (not the stack's block) */
        A(0) = 0x01005000; D(1) = 1;
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(STACK - 60 + 0x0C, 2), 0x0405);
        CHECK_EQ(MEM(STACK - 60 + 0x10, 2), 0x0085);
        CHECK_EQ(MEM(STACK - 60 + 0x20, 4), 0x01005000);
        done(e);
    }
    {   /* ITT and DTT apart: code fetched through ITT0 from $00xxxxxx while
         * the tables map the code page elsewhere */
        SETUP(M68K_68040, 0x7005);                  /* MOVEQ #5,D0 at $1000 */
        PUT(0x21000, 2, 0x7009);                    /* MOVEQ #9,D0 where the tables point */
        mmu040(e, 0);
        map(e, CODE, 0x00021001);
        MOVEC(0x004, 0x0000C000u);
        RUN(1);
        CHECK_EQ(D(0), 5);
        MOVEC(0x004, 0);
        cpu->pc = CODE;
        RUN(1);
        CHECK_EQ(D(0), 9);
        done(e);
    }
}

/* --- the ATCs and PFLUSH (MC68040 UM 3.3, PRM PFLUSH) ------------------------ */

static void test_flush(void) {
    {   /* an entry outlives a table edit until PFLUSH (An) for its FC2 */
        SETUP(M68K_68040, 0x2010, 0x2010, 0xF508, 0x2010, 0xF508, 0x2010);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020001);
        PUT(0x20000, 4, 0xAAAA0000);
        PUT(0x30000, 4, 0xBBBB0000);
        A(0) = 0x00400000;
        RUN(1);
        CHECK_EQ(D(0), 0xAAAA0000);
        map(e, 0x00400000, 0x00030001);
        RUN(1);
        CHECK_EQ(D(0), 0xAAAA0000);                 /* stale, as on the chip */
        MOVEC(0x001, 1);                            /* DFC = user data: not this entry */
        RUN(2);
        CHECK_EQ(D(0), 0xAAAA0000);
        MOVEC(0x001, 5);
        RUN(2);
        CHECK_EQ(D(0), 0xBBBB0000);
        done(e);
    }
    {   /* global entries survive PFLUSHN and PFLUSHAN, not PFLUSH/PFLUSHA */
        SETUP(M68K_68040, 0x2010, 0xF500, 0x2010, 0xF510, 0x2010, 0xF518, 0x2010);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020401);             /* G */
        PUT(0x20000, 4, 1);
        PUT(0x30000, 4, 2);
        A(0) = 0x00400000;
        MOVEC(0x001, 5);
        RUN(1);
        map(e, 0x00400000, 0x00030401);
        RUN(2);
        CHECK_EQ(D(0), 1);                          /* PFLUSHN (A0) kept it */
        RUN(2);
        CHECK_EQ(D(0), 1);                          /* PFLUSHAN kept it */
        RUN(2);
        CHECK_EQ(D(0), 2);                          /* PFLUSHA */
        done(e);
    }
    {   /* reset clears E and keeps P and the ATCs (MC68040 UM 3.6.1) */
        SETUP(M68K_68040, 0x4E71);
        mmu040(e, 1);
        m68k_reset(cpu);
        uint32_t tc = 0;
        m68k_movec_read(cpu, 0x003, &tc);
        CHECK_EQ(tc, 0x4000);
        done(e);
    }
}

/* --- PTEST and MMUSR (PRM PTEST; MC68040 UM 3.1.4) ------------------------- */

static void test_ptest(void) {
    {   /* PTESTR: PA, G, U1 U0, CM, R; then PTESTW sets M */
        SETUP(M68K_68040, 0xF568, 0x4E7A, 0x1805, 0xF548, 0x4E7A, 0x2805);
        mmu040(e, 0);
        map(e, 0x00400000, 0x00020721);             /* G, U1 U0, CM 01 */
        A(0) = 0x00400123;
        MOVEC(0x001, 5);
        RUN(2);
        CHECK_EQ(D(1), 0x00020721);
        CHECK_EQ(MEM(pd_addr(e, 0x00400000), 4), 0x00020729);   /* U set */
        RUN(2);
        CHECK_EQ(D(2), 0x00020731);
        CHECK_EQ(MEM(pd_addr(e, 0x00400000), 4), 0x00020739);   /* M set */
        done(e);
    }
    {   /* invalid: R clear; write protected and supervisor bits reported */
        SETUP(M68K_68040, 0xF568, 0x4E7A, 0x1805, 0xF569, 0x4E7A, 0x2805);
        mmu040(e, 0);
        map(e, 0x00400000, 0);
        map(e, 0x00401000, 0x00021085);             /* S, W */
        A(0) = 0x00400000; A(1) = 0x00401000;
        MOVEC(0x001, 5);
        RUN(4);
        CHECK_EQ(D(1), 0);
        CHECK_EQ(D(2), 0x00021085);
        done(e);
    }
    {   /* a transparent hit: T and R only; a bus error in the search: B */
        SETUP(M68K_68040, 0xF568, 0x4E7A, 0x1805, 0xF569, 0x4E7A, 0x2805);
        mmu040(e, 0);
        PUT(ROOT + 4, 4, 0x00F00003);
        MOVEC(0x006, 0x4000C000u);                  /* DTT0: $40xxxxxx */
        A(0) = 0x40001000; A(1) = 0x02000000;
        MOVEC(0x001, 5);
        RUN(4);
        CHECK_EQ(D(1), 3);
        CHECK_EQ(D(2), 0x800);
        done(e);
    }
    {   /* privileged */
        SETUP(M68K_68040, 0xF568);
        m68k_set_sr(cpu, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
}

/* --- MOVES (MC68040 UM Table 3-2) ------------------------------------------ */

static void test_moves(void) {
    {   /* DFC = 1: the user tables (URP), from supervisor mode */
        SETUP(M68K_68040, 0x0E90, 0x0800);           /* MOVES.L D0,(A0) */
        mmu040(e, 0);
        const uint32_t user_root = 0x86000;
        for (uint32_t a = 0; a < 512; a += 4) PUT(user_root + a, 4, MEM(ROOT + a, 4));
        PUT(user_root, 4, 0);
        PUT(user_root + 4 * 2, 4, alloc(e, 512) | 3);
        {   /* user: $04000000 -> $20000 */
            const uint32_t pt = MEM(user_root + 8, 4) & ~0x1FFu;
            const uint32_t pg = alloc(e, 256) | 3;
            PUT(pt, 4, pg);
            PUT(pg & ~0xFFu, 4, 0x00020001);
        }
        MOVEC(0x806, user_root);
        MOVEC(0x001, 1);
        A(0) = 0x04000008; D(0) = 0x12345678;
        RUN(1);
        CHECK_EQ(MEM(0x20008, 4), 0x12345678);
        done(e);
    }
    {   /* SFC = 6 becomes a data access: DTT, not ITT, applies */
        SETUP(M68K_68040, 0x0E90, 0x3000);           /* MOVES.L (A0),D3 */
        mmu040(e, 0);
        map(e, 0x00005000, 0x00020001);
        PUT(0x5000, 4, 0x11111111);
        PUT(0x20000, 4, 0x22222222);
        MOVEC(0x004, 0x0000C000u);                  /* ITT0 over $00xxxxxx */
        MOVEC(0x000, 6);
        A(0) = 0x5000;
        RUN(1);
        CHECK_EQ(D(3), 0x22222222);
        done(e);
    }
}

/* --- caches (MC68040 UM 4.4; CINV/CPUSH in PRM 6) ----------------------------- */

static void test_caches(void) {
    SETUP(M68K_68040, 0xF4D8, 0xF4B1, 0xF448, 0xF400 | 0x08);
    A(0) = 0x1234; A(1) = 0x5678;
    RUN(1);                                         /* CINVA IC/DC */
    CHECK_EQ(e->cache_calls, 1);
    CHECK_EQ(e->cache_caches, 3);
    CHECK_EQ(e->cache_scope, 3);
    RUN(1);                                         /* CPUSHP IC,(A1) */
    CHECK_EQ(e->cache_caches, 2);
    CHECK_EQ(e->cache_scope, 2);
    CHECK_EQ(e->cache_addr, 0x5678);
    RUN(1);                                         /* CINVL DC,(A0) */
    CHECK_EQ(e->cache_caches, 1);
    CHECK_EQ(e->cache_scope, 1);
    CHECK_EQ(e->cache_addr, 0x1234);
    RUN(1);                                         /* CINVL with no cache: nothing to report */
    CHECK_EQ(e->cache_calls, 3);
    CHECK_EQ(e->nvec, 0);
    done(e);
}

/* --- frames the 68040 refuses (MC68040 UM 8.4.5) ------------------------------ */

static void test_frames(void) {
    for (int lc = 0; lc < 2; lc++) {
        SETUP(lc ? M68K_68LC040 : M68K_68040, 0x4E73);
        A(7) = STACK - 16;
        PUT(STACK - 16, 2, 0x2700); PUT(STACK - 14, 4, 0x3000); PUT(STACK - 10, 2, 0x402C);
        RUN(1);
        if (lc) {
            CHECK_EQ(cpu->pc, 0x3000);
            CHECK_EQ(A(7), STACK);
        } else {
            CHECK_EQ(cpu->pc, HANDLER(14));
        }
        done(e);
    }
}

/* --- the 68030 MMU (MC68030 UM section 9) ----------------------------------- */

/* TC: PS 4K, IS 0, TIA 10, TIB 10. Table A (1024 short entries) at ROOT,
 * table B for A index 0 at ROOT + $1000 mapping 0-1 MiB identity; A index 1
 * an early-termination page descriptor $400000-$7FFFFF -> $20000. */
static void mmu030(env *e, uint32_t tc) {
    for (uint32_t i = 0; i < 1024; i++) PUT(ROOT + 4 * i, 4, 0);
    PUT(ROOT, 4, (ROOT + 0x1000) | 2);
    for (uint32_t i = 0; i < 1024; i++) PUT(ROOT + 0x1000 + 4 * i, 4, i < 256 ? (i << 12) | 1 : 0);
    PUT(ROOT + 4, 4, 0x00020001);
    PUT(IOREGS, 4, 0x7FFF0002);                     /* CRP: no limit, DT short */
    PUT(IOREGS + 4, 4, ROOT);
    PUT(IOREGS + 8, 4, tc);
}

static void test_mmu030(void) {
    {   /* PMOVE CRP and TC; an early-terminated mapping; U written back */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2412);
        mmu030(e, 0x80C0AA00);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x00400010;
        PUT(0x20010, 4, 0x0BADF00D);
        RUN(3);
        CHECK_EQ(D(2), 0x0BADF00D);
        CHECK_EQ(MEM(ROOT + 4, 4), 0x00020009);     /* U */
        CHECK_EQ(e->nvec, 0);
        done(e);
    }
    {   /* PTEST level 7 with An: levels, the last descriptor's address;
         * invalid entry: I */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF012, 0x9F55, 0xF013, 0x6200, 0xF014, 0x9F75, 0xF015, 0x6200);
        mmu030(e, 0x80C0AA00);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x00400010; A(3) = IOREGS + 16;
        A(4) = 0x00800000; A(5) = IOREGS + 20;
        RUN(6);
        CHECK_EQ(MEM(IOREGS + 16, 2), 0x0001);      /* one level, valid */
        CHECK_EQ(A(2), ROOT + 4);
        CHECK_EQ(MEM(IOREGS + 20, 2), 0x0401);      /* I, one level */
        CHECK_EQ(A(3), ROOT + 8);
        done(e);
    }
    {   /* an invalid page: bus error (format $B); fixed, RTE restarts */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2414);
        mmu030(e, 0x80C0AA00);
        PUT(HANDLER(2), 2, 0x4E73);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x00005000;
        PUT(ROOT + 0x1000 + 4 * 5, 4, 0);
        PUT(0x5000, 4, 0x99999999);
        RUN(3);
        CHECK_EQ(cpu->pc, HANDLER(2));
        CHECK_EQ(MEM(A(7) + 6, 2), 0xB008);
        CHECK_EQ(MEM(A(7) + 0x10, 4), 0x5000);
        PUT(ROOT + 0x1000 + 4 * 5, 4, 0x5001);
        m68k_pflusha(cpu);
        RUN(2);
        CHECK_EQ(D(2), 0x99999999);
        done(e);
    }
    {   /* a write-protected page: the write is rerun by RTE once allowed */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0x2883, 0x4E71);
        mmu030(e, 0x80C0AA00);
        PUT(HANDLER(2), 2, 0x4E73);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x00005004; D(3) = 0x13579BDF;
        PUT(ROOT + 0x1000 + 4 * 5, 4, 0x5005);      /* WP */
        RUN(3);
        CHECK_EQ(cpu->pc, HANDLER(2));
        const uint32_t ssw = MEM(A(7) + 0x0A, 2);
        CHECK_EQ(ssw & 0x0140, 0x0100);             /* DF, write */
        CHECK_EQ(MEM(A(7) + 0x18, 4), 0x13579BDF);
        CHECK_EQ(MEM(0x5004, 4), 0);
        PUT(ROOT + 0x1000 + 4 * 5, 4, 0x5001);
        m68k_pflusha(cpu);
        RUN(1);                                     /* RTE: rerun */
        CHECK_EQ(MEM(0x5004, 4), 0x13579BDF);
        CHECK_EQ(MEM(ROOT + 0x1000 + 4 * 5, 4), 0x5019);   /* U and M */
        done(e);
    }
    {   /* TT0 for reads only: reads of $5010 transparent, writes translated */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF012, 0x0800, 0x2414, 0x2883);
        mmu030(e, 0x80C0AA00);
        PUT(ROOT + 0x1000 + 4 * 5, 4, 0x20001);
        PUT(IOREGS + 16, 4, 0x00008207);            /* base 0, E, R/W read, FC mask 7 */
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = IOREGS + 16; A(4) = 0x5010;
        PUT(0x5010, 4, 0x11111111);
        D(3) = 0x44444444;
        RUN(5);
        CHECK_EQ(D(2), 0x11111111);
        CHECK_EQ(MEM(0x20010, 4), 0x44444444);
        CHECK_EQ(MEM(0x5010, 4), 0x11111111);
        done(e);
    }
    {   /* a long-format root with an upper limit of 0: index 1 is a limit
         * violation (bus error); PTEST reports L and I */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF012, 0x9F55, 0xF013, 0x6200);
        mmu030(e, 0x80C0AA00);
        for (uint32_t i = 0; i < 4; i++) PUT(0x84000 + 4 * i, 4, 0);
        PUT(0x84000, 4, 0x7FFF0002);                /* long table descriptor -> short table B */
        PUT(0x84004, 4, ROOT + 0x1000);
        PUT(IOREGS, 4, 0x00000003);                 /* L/U 0, limit 0, DT long */
        PUT(IOREGS + 4, 4, 0x84000);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(2) = 0x00400000; A(3) = IOREGS + 16;
        RUN(4);
        CHECK_EQ(MEM(IOREGS + 16, 2) & 0xC400, 0x4400);
        done(e);
    }
    {   /* an inconsistent TC takes the MMU configuration exception (vector
         * 56) and is loaded with E clear */
        SETUP(M68K_68030, 0xF011, 0x4000, 0xF012, 0x4200);
        A(1) = IOREGS + 8; A(2) = IOREGS + 12;
        PUT(IOREGS + 8, 4, 0x80C0A900);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(56));
        cpu->pc = CODE + 4;
        RUN(1);
        CHECK_EQ(MEM(IOREGS + 12, 4), 0x00C0A900);
        done(e);
    }
    {   /* PLOADW sets M; PFLUSH by function code; PTEST level 0 sees the ATC */
        SETUP(M68K_68030, 0xF010, 0x4C00, 0xF011, 0x4000, 0xF014, 0x2011, 0xF014, 0x2015, 0xF000, 0x30F1,
              0xF014, 0x8215, 0xF015, 0x6200, 0xF014, 0x8211, 0xF016, 0x6200);
        mmu030(e, 0x80C0AA00);
        A(0) = IOREGS; A(1) = IOREGS + 8; A(4) = 0x6000; A(5) = IOREGS + 16; A(6) = IOREGS + 20;
        RUN(4);                                     /* PLOADW #1 and #5 */
        CHECK_EQ(MEM(ROOT + 0x1000 + 4 * 6, 4), 0x6019);
        RUN(1);                                     /* PFLUSH #1,#7 */
        RUN(4);
        CHECK_EQ(MEM(IOREGS + 16, 2) & 0x0600, 0x0200);   /* fc 5: resident, M */
        CHECK_EQ(MEM(IOREGS + 20, 2) & 0x0400, 0x0400);   /* fc 1: flushed */
        done(e);
    }
    {   /* PMMU instructions are privileged */
        SETUP(M68K_68030, 0xF000, 0x2400);
        m68k_set_sr(cpu, 0);
        RUN(1);
        CHECK_EQ(cpu->pc, HANDLER(8));
        done(e);
    }
}

int mc_cpu_mmu_unit(void) {
    test_walk();
    test_faults();
    test_tt();
    test_flush();
    test_ptest();
    test_moves();
    test_caches();
    test_frames();
    test_mmu030();
    printf("mmu: %d failure(s)\n", mc_test_failures);
    return mc_test_failures;
}
