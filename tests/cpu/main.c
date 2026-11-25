/* mc_cpu_tests unit | mmu | 68851 | fpu | timing | fpu-eval | faults | sst DIR [GROUP] [--jit | --68000 [--show=N]] | jit [CASES [SEED [MAXBLOCK]]] | bench: the CPU's tests. */
#include "cpu_test.h"
#include "../test.h"

int mc_test_failures;

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "unit")) return mc_cpu_unit() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "faults")) return mc_cpu_fault_sweep() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "mmu")) return mc_cpu_mmu_unit() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "68851")) return mc_cpu_68851_unit() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "fpu")) return mc_cpu_fpu_unit() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "timing")) return mc_cpu_timing_unit() ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "fpu-eval")) return mc_cpu_fpu_eval();
    if (argc >= 3 && !strcmp(argv[1], "sst")) {
        extern int mc_cpu_sst_jit, mc_cpu_sst_68000, mc_cpu_sst000_verbose;
        const char *group = NULL;
        for (int i = 3; i < argc; i++) {
            if (!strcmp(argv[i], "--jit")) mc_cpu_sst_jit = 1;
            else if (!strcmp(argv[i], "--68000")) mc_cpu_sst_68000 = 1;
            else if (!strncmp(argv[i], "--show=", 7)) mc_cpu_sst000_verbose = atoi(argv[i] + 7);
            else group = argv[i];
        }
        return mc_cpu_sst(argv[2], group);
    }
    if (argc >= 2 && !strcmp(argv[1], "jit"))
        return mc_cpu_jit_diff(argc >= 3 ? (unsigned)atoi(argv[2]) : 2000, argc >= 4 ? strtoull(argv[3], NULL, 0) : 1,
                               argc >= 5 ? (unsigned)atoi(argv[4]) : 0) ? 1 : 0;
    if (argc >= 2 && !strcmp(argv[1], "bench")) {
        double mips = 0;
        const int r = mc_cpu_bench(&mips);
        printf("interpreter: %.1f MIPS\n", mips);
        return r;
    }
    fprintf(stderr, "usage: mc_cpu_tests unit | mmu | 68851 | fpu | timing | fpu-eval | faults | sst DIR [GROUP] [--jit | --68000 [--show=N]] | jit [CASES [SEED [MAXBLOCK]]] | bench\n");
    return 2;
}
