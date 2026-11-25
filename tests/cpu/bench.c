/* mc_cpu_tests bench: interpreter and translator speed on simple loops,
 * headless. */
#include "cpu_test.h"

#include <time.h>

#define A(n) cpu->r[8 + (n)]
#define D(n) cpu->r[(n)]

static double now(void) {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* Identity-mapped 4 KiB page tables for the 1 MiB of RAM (MC68040 UM 3.2):
 * root at $C0000, one pointer table, four page tables. */
static void identity_tables(testbus *bus) {
    const uint32_t root = 0xC0000, ptr = 0xC0200, pages = 0xC0400;
    for (uint32_t a = root; a < root + 0x400; a += 4) tb_put(bus, a, 4, 0);
    tb_put(bus, root, 4, ptr | 3);
    for (uint32_t i = 0; i < 4; i++) tb_put(bus, ptr + 4 * i, 4, (pages + 256 * i) | 3);
    for (uint32_t p = 0; p < 256; p++) tb_put(bus, pages + 4 * p, 4, p << 12 | 1);
}

/* Runs the loop at $1000 for `n` instructions; returns MIPS. `mmu`: with
 * translation on through identity tables. */
static double run_loop_fp(const uint16_t *code, size_t words, int64_t n, int mmu, int jit, int fp) {
    testbus bus;
    tb_init(&bus, 1u << 20, 0xFFFFFFFFu, 0);
    for (size_t i = 0; i < words; i++) tb_put(&bus, 0x1000 + 2 * (uint32_t)i, 2, code[i]);
    m68k_bus mb = tb_bus(&bus);
    m68k_cpu *cpu = calloc(1, sizeof *cpu);
    m68k_init(cpu, M68K_68040, &mb);
    if (jit && !m68k_jit_enable(cpu, 1)) {
        m68k_free(cpu);
        free(cpu);
        tb_free(&bus);
        return 0;
    }
    if (mmu) {
        identity_tables(&bus);
        m68k_movec_write(cpu, 0x806, 0xC0000);
        m68k_movec_write(cpu, 0x807, 0xC0000);
        m68k_movec_write(cpu, 0x003, 0x8000);
    }
    if (fp) {
        /* FP0 1.5, FP2 1.0000001, FP3 0.25, FP4 1.0001 (extended images). */
        cpu->fpu.fp[0] = (m68k_fx80){0xC000000000000000ull, 0x3FFF};
        cpu->fpu.fp[2] = (m68k_fx80){0x80000035A6E9DC80ull, 0x3FFF};
        cpu->fpu.fp[3] = (m68k_fx80){0x8000000000000000ull, 0x3FFD};
        cpu->fpu.fp[4] = (m68k_fx80){0x8006A7EF9DB22D0Eull, 0x3FFF};
    }
    m68k_set_sr(cpu, 0x2700);
    A(7) = 0x8000;
    cpu->pc = 0x1000;
    D(2) = 0x00FF00FFu;
    D(4) = 0xFFFFFFFFu;
    D(5) = 0xFFFFu;
    A(0) = 0x20000;
    A(1) = 0x40000;
    const double t0 = now();
    int64_t left = n;
    while (left > 0) {
        const int64_t slice = left < 1000000 ? left : 1000000;
        m68k_run(cpu, slice);
        left -= slice;
        if (cpu->pc < 0x1000 || cpu->pc >= 0x1000 + 2 * words) break;
    }
    const double t = now() - t0;
    const double mips = (double)cpu->instructions / t / 1e6;
    m68k_free(cpu);
    free(cpu);
    tb_free(&bus);
    return mips;
}
static double run_loop(const uint16_t *code, size_t words, int64_t n, int mmu, int jit) {
    return run_loop_fp(code, words, n, mmu, jit, 0);
}

int mc_cpu_bench(double *mips) {
    /* Register loop: ADDQ.L, MOVE.L, AND.L, ADD.L, SUBQ.L, BNE.S */
    static const uint16_t reg_loop[] = {0x5280, 0x2200, 0xC282, 0xD681, 0x5384, 0x66F4};
    /* Memory loop: MOVE.L (A0)+,D1; ADD.L D1,(A1); ADDQ.L #4,A1; DBRA D5;
     * then LEA $20000,A0; LEA $40000,A1; BRA back. */
    static const uint16_t mem_loop[] = {0x2218, 0xD391, 0x5889, 0x51CD, 0xFFF8, 0x41F9, 0x0002, 0x0000,
                                        0x43F9, 0x0004, 0x0000, 0x60E8};
    const double r = run_loop(reg_loop, 6, 300000000, 0, 0);
    const double m = run_loop(mem_loop, 12, 200000000, 0, 0);
    const double rt = run_loop(reg_loop, 6, 300000000, 1, 0);
    const double mt = run_loop(mem_loop, 12, 200000000, 1, 0);
    printf("register loop: %.1f MIPS\nmemory loop:   %.1f MIPS\n", r, m);
    printf("register loop, MMU on: %.1f MIPS\nmemory loop, MMU on:   %.1f MIPS\n", rt, mt);
    double low = r;
    if (m < low) low = m;
    if (rt < low) low = rt;
    if (mt < low) low = mt;
    /* FPU loop (68882 register forms): FMOVE.X FP0,FP1; FMUL.X FP2,FP1;
     * FADD.X FP3,FP1; FDIV.X FP4,FP1; FSUB.X FP2,FP1; SUBQ.L #1,D4; BNE.S. */
    static const uint16_t fpu_loop[] = {0xF200, 0x0080, 0xF200, 0x08A3, 0xF200, 0x0CA2, 0xF200, 0x10A0,
                                        0xF200, 0x08A8, 0x5384, 0x66EA};
    const double f = run_loop_fp(fpu_loop, 12, 50000000, 0, 0, 1);
    printf("FPU loop:      %.1f MIPS\n", f);
    if (getenv("LIVADIX_BENCH_FPU")) {
        /* FMOVE.X FP0,FP1, then four of one operation on FP1 */
        static const uint16_t np[] = {0xF280, 0x0000, 0xF280, 0x0000, 0xF280, 0x0000, 0xF280, 0x0000, 0xF280, 0x0000, 0x5384, 0x66EA};
        printf("FNOP: %.1f MIPS\n", run_loop_fp(np, 12, 50000000, 0, 0, 1));
        static const uint16_t mv[] = {0xF200, 0x0080, 0xF200, 0x0080, 0xF200, 0x0080, 0xF200, 0x0080, 0xF200, 0x0080, 0x5384, 0x66EA};
        static const uint16_t ml[] = {0xF200, 0x0080, 0xF200, 0x08A3, 0xF200, 0x08A3, 0xF200, 0x08A3, 0xF200, 0x08A3, 0x5384, 0x66EA};
        static const uint16_t ad[] = {0xF200, 0x0080, 0xF200, 0x0CA2, 0xF200, 0x0CA2, 0xF200, 0x0CA2, 0xF200, 0x0CA2, 0x5384, 0x66EA};
        static const uint16_t dv[] = {0xF200, 0x0080, 0xF200, 0x10A0, 0xF200, 0x10A0, 0xF200, 0x10A0, 0xF200, 0x10A0, 0x5384, 0x66EA};
        printf("FMOVE: %.1f  FMUL: %.1f  FADD: %.1f  FDIV: %.1f MIPS\n", run_loop_fp(mv, 12, 50000000, 0, 0, 1),
               run_loop_fp(ml, 12, 50000000, 0, 0, 1), run_loop_fp(ad, 12, 50000000, 0, 0, 1), run_loop_fp(dv, 12, 50000000, 0, 0, 1));
    }
    if (m68k_jit_available()) {
        const double jf = run_loop_fp(fpu_loop, 12, 100000000, 0, 1, 1);
        printf("translator, FPU loop: %.1f MIPS\n", jf);
        const double jr = run_loop(reg_loop, 6, 2000000000, 0, 1);
        const double jm = run_loop(mem_loop, 12, 1000000000, 0, 1);
        const double jrt = run_loop(reg_loop, 6, 2000000000, 1, 1);
        const double jmt = run_loop(mem_loop, 12, 1000000000, 1, 1);
        printf("translator, register loop: %.1f MIPS\ntranslator, memory loop:   %.1f MIPS\n", jr, jm);
        printf("translator, register loop, MMU on: %.1f MIPS\ntranslator, memory loop, MMU on:   %.1f MIPS\n", jrt, jmt);
    }
    *mips = low;
    return 0;
}
