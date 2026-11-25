/* mc_cpu_tests fpu-eval: runs FPU instructions given on standard input and
 * prints their results, for tests/fpu_suite.py, which checks them against
 * an independent reference. One request per line (hexadecimal fields):
 *
 *   G opmode fpcr dse dm sse sm      FPop FP1,FP0 (opclass 000) on the
 *                                    68040 with the full set; prints FP0's
 *                                    se and m, FP1's (FSINCOS: FPc = FP1 is
 *                                    not used, FP2 is), and FPSR
 *   I fmt fpcr w0 w1 w2              FMOVE.<fmt> from memory to FP0
 *                                    (fmt 0 L, 1 S, 2 X, 3 P, 4 W, 5 D, 6 B)
 *   O fmt k fpcr sse sm              FMOVE.<fmt> FP1 to memory (k: the
 *                                    static k-factor for P); prints the
 *                                    three words and FPSR
 *   C offset fpcr                    FMOVECR #offset,FP0
 *
 * Every instruction runs through the CPU's decoder on a flat test bus. */
#include "cpu_test.h"

#include <stdio.h>

#define CODE 0x1000u
#define DATA 0x2000u

static m68k_cpu *cpu;
static testbus bus;

static void run_code(const uint16_t *code, unsigned words) {
    for (unsigned i = 0; i < words; i++) tb_put(&bus, CODE + 2 * i, 2, code[i]);
    cpu->pc = CODE;
    cpu->r[8] = DATA;
    cpu->r[15] = 0x8000;
    m68k_run(cpu, 1);
}

int mc_cpu_fpu_eval(void) {
    tb_init(&bus, 1u << 16, 0xFFFFFFFFu, 0);
    m68k_bus mb = tb_bus(&bus);
    cpu = calloc(1, sizeof *cpu);
    m68k_init(cpu, M68K_68040, &mb);
    m68k_set_fpu(cpu, M68K_FPU_68040, 1);
    m68k_reset(cpu);
    char line[512];
    while (fgets(line, sizeof line, stdin)) {
        char kind = 0;
        unsigned a = 0, b = 0, c = 0, d = 0;
        unsigned long long m1 = 0, m2 = 0;
        m68k_fpu *f = &cpu->fpu;
        if (line[0] == 'G') {
            if (sscanf(line, "G %x %x %x %llx %x %llx", &a, &b, &c, &m1, &d, &m2) != 6) { puts("?"); continue; }
            f->fpcr = b;
            f->fpsr = 0;
            f->fp[0].se = (uint16_t)c; f->fp[0].m = m1;
            f->fp[1].se = (uint16_t)d; f->fp[1].m = m2;
            f->fp[2] = f->fp[0];
            /* FSINCOS FP1,FP2:FP0 when the opmode is $30-$37 (FPc = opmode & 7) */
            const uint16_t ext = (uint16_t)((1u << 10) | (0u << 7) | (a & 0x7Fu));
            const uint16_t code[2] = {0xF200, ext};
            run_code(code, 2);
            const unsigned cr = (a >= 0x30 && a <= 0x37) ? (a & 7u) : 1u;
            printf("%04X %016llX %04X %016llX %08X\n", f->fp[0].se, (unsigned long long)f->fp[0].m, f->fp[cr].se,
                   (unsigned long long)f->fp[cr].m, (unsigned)f->fpsr);
        } else if (line[0] == 'I') {
            unsigned w0, w1, w2;
            if (sscanf(line, "I %x %x %x %x %x", &a, &b, &w0, &w1, &w2) != 5) { puts("?"); continue; }
            f->fpcr = b;
            f->fpsr = 0;
            tb_put(&bus, DATA, 4, w0); tb_put(&bus, DATA + 4, 4, w1); tb_put(&bus, DATA + 8, 4, w2);
            if (a == 4) tb_put(&bus, DATA, 2, w0);
            if (a == 6) tb_put(&bus, DATA, 1, w0);
            const uint16_t code[2] = {0xF210, (uint16_t)(0x4000u | (a & 7u) << 10)};
            run_code(code, 2);
            printf("%04X %016llX %08X\n", f->fp[0].se, (unsigned long long)f->fp[0].m, (unsigned)f->fpsr);
        } else if (line[0] == 'O') {
            unsigned k;
            if (sscanf(line, "O %x %x %x %x %llx", &a, &k, &b, &c, &m1) != 5) { puts("?"); continue; }
            f->fpcr = b;
            f->fpsr = 0;
            f->fp[1].se = (uint16_t)c; f->fp[1].m = m1;
            tb_put(&bus, DATA, 4, 0); tb_put(&bus, DATA + 4, 4, 0); tb_put(&bus, DATA + 8, 4, 0);
            const uint16_t code[2] = {0xF210, (uint16_t)(0x6000u | (a & 7u) << 10 | 1u << 7 | (k & 0x7Fu))};
            run_code(code, 2);
            const unsigned size = a == 4 ? 2 : a == 6 ? 1 : 4;
            printf("%08X %08X %08X %08X\n", (unsigned)tb_get(&bus, DATA, size), (unsigned)tb_get(&bus, DATA + 4, 4),
                   (unsigned)tb_get(&bus, DATA + 8, 4), (unsigned)f->fpsr);
        } else if (line[0] == 'C') {
            if (sscanf(line, "C %x %x", &a, &b) != 2) { puts("?"); continue; }
            f->fpcr = b;
            f->fpsr = 0;
            const uint16_t code[2] = {0xF200, (uint16_t)(0x5C00u | (a & 0x7Fu))};
            run_code(code, 2);
            printf("%04X %016llX %08X\n", f->fp[0].se, (unsigned long long)f->fp[0].m, (unsigned)f->fpsr);
        } else {
            (void)kind;
            puts("?");
        }
        fflush(stdout);
    }
    m68k_free(cpu);
    free(cpu);
    tb_free(&bus);
    return 0;
}
