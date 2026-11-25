/* mc_cpu_tests sst DIR [NAME] [--68000]: the SingleStepTests 68000 suite
 * (converted by tests/cpu_suite.py) run on the 68040 core, or with --68000
 * on the 68000 model with nothing skipped (see mc_cpu_sst000 below).
 *
 * Every test whose outcome the 68040 defines the same way as the 68000 is
 * compared in full: registers, SR, PC and every RAM byte the test lists. The
 * 68000 test bus decodes 24 address bits, so this bus masks addresses to 24
 * bits too. Undefined condition codes (per the PRM's instruction pages) are
 * not compared. Differences the 68040 defines differently are classified and
 * skipped (see `skip_names`); when the 68000 took an exception the 68040
 * takes too, the 6-byte 68000 frame is compared with the 68040's format
 * $0/$2 frame field by field (SR and PC stacked, vector taken, stack depth).
 */
#include "cpu_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <io.h>
#else
#include <dirent.h>
#endif

typedef struct state {
    uint32_t reg[19];   /* d0-d7, a0-a6, usp, ssp, sr, pc */
    uint32_t prefetch[2];
    uint32_t n;
    const uint8_t *ram; /* n x (u32 address, u32 byte) */
} state;

/* A test's clock cycles and bus transactions (format SST2). */
typedef struct trans {
    uint32_t length, m;
    const uint8_t *t;   /* m x 10 bytes: kind, cycles, fc, size, u32 address, u16 value */
} trans;
static const uint8_t *read_trans(const uint8_t *p, trans *t) {
    memcpy(&t->length, p, 4);
    memcpy(&t->m, p + 4, 4);
    t->t = p + 8;
    return p + 8 + 10u * t->m;
}

enum { R_USP = 15, R_SSP = 16, R_SR = 17, R_PC = 18 };

enum {
    SKIP_RTE,           /* 68000 RTE pops no format word */
    SKIP_ADDRESS_ERROR, /* odd word/long data access or odd PC: 68000 group-0 frame */
    SKIP_MOVE_FROM_SR,  /* privileged on the 68010 and later */
    SKIP_BCC_FF,        /* displacement $FF: 32-bit displacement on the 68020+ */
    SKIP_MOVEM_PREDEC,  /* -(An) with An in the list: 68020+ stores An minus the size */
    SKIP_EXT020,        /* scale or full-format bits in an index word: 68000 ignores them */
    SKIP_SR_T0_M,       /* SR bits T0 and M exist on the 68020+ */
    SKIP_NESTED,        /* two exceptions with 68000 frames (trace after a trap) */
    SKIP_ASR_LONG,      /* ASR by a count >= the operand size: C/X */
    SKIP_SUITE,         /* tests whose expected state the instruction cannot produce */
    SKIP_COUNT
};
static const char *const skip_names[SKIP_COUNT] = {
    "RTE (68000 frames have no format word)",
    "address error (68000 only: misaligned data, group-0 frame)",
    "MOVE from SR in user mode (privileged on 68010+)",
    "Bcc/BSR displacement $FF (32-bit displacement on 68020+)",
    "MOVEM -(An) with An in the list (68020+ stores An - size)",
    "index word with scale or full-format bits (ignored by the 68000)",
    "SR T0/M bits set (implemented on 68020+)",
    "trace after a trap (two 68000 frames)",
    "ASR count >= operand size: C/X (PRM: last bit out = sign)",
    "suite errors (expected values the instruction cannot produce)",
};

/* ASL.B #2,D2 cannot change bits 31-8 of D2, yet the first two expect it;
 * the third, a divide by zero, stacks the DIVU's own address where every
 * other divide-by-zero test of the suite stacks the next instruction's. */
static const char *const suite_errors[] = {"e502 [ASL.b Q, D2] 1583", "e502 [ASL.b Q, D2] 1761",
                                           "80ef [DIVU (d16, A7), D0] 5745"};

typedef struct counts {
    unsigned long total, passed, frames, failed, skipped[SKIP_COUNT];
} counts;

static const uint8_t *read_state(const uint8_t *p, state *s) {
    memcpy(s->reg, p, sizeof s->reg);
    p += sizeof s->reg;
    memcpy(s->prefetch, p, sizeof s->prefetch);
    p += sizeof s->prefetch;
    memcpy(&s->n, p, 4);
    p += 4;
    s->ram = p;
    return p + 8u * s->n;
}
static void ram_entry(const state *s, uint32_t i, uint32_t *a, uint32_t *v) {
    memcpy(a, s->ram + 8u * i, 4);
    memcpy(v, s->ram + 8u * i + 4, 4);
}

/* A byte span of the expected final RAM (absent bytes read as 0). */
static uint32_t tb_get_expected(const state *s, uint32_t a, unsigned size) {
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++) {
        uint32_t byte = 0;
        for (uint32_t i = 0; i < s->n; i++) {
            uint32_t ea, ev;
            ram_entry(s, i, &ea, &ev);
            if (((ea ^ (a + k)) & 0xFFFFFFu) == 0) {
                byte = ev;
                break;
            }
        }
        v = (v << 8) | byte;
    }
    return v;
}

static int has_expected(const state *s, uint32_t a, unsigned size) {
    unsigned found = 0;
    for (unsigned k = 0; k < size; k++)
        for (uint32_t i = 0; i < s->n; i++) {
            uint32_t ea, ev;
            ram_entry(s, i, &ea, &ev);
            if (((ea ^ (a + k)) & 0xFFFFFFu) == 0) {
                found++;
                break;
            }
        }
    return found == size;
}

typedef struct run_ctx {
    unsigned exceptions;
    unsigned vector[4];
} run_ctx;
static void on_exception(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc) {
    run_ctx *r = ctx;
    (void)cpu;
    (void)pc;
    if (r->exceptions < 4) r->vector[r->exceptions] = vector;
    r->exceptions++;
}

/* Condition codes the PRM leaves undefined for the instruction group. */
static unsigned undefined_ccr(const char *group, const m68k_cpu *cpu, const run_ctx *r) {
    if (!strcmp(group, "ABCD") || !strcmp(group, "SBCD") || !strcmp(group, "NBCD")) return 0x0A;  /* N V */
    if (!strcmp(group, "CHK")) return 0x07;                                                         /* Z V C */
    if (!strcmp(group, "DIVU") || !strcmp(group, "DIVS")) {
        if (r->exceptions && r->vector[0] == 5) return 0x0E;   /* divide by zero: N Z V */
        if (cpu->fv >> 31) return 0x0C;                        /* overflow: N Z */
    }
    return 0;
}

static int frame_size(unsigned vector) {
    /* 68040 formats: $2 for CHK, TRAPcc/TRAPV, divide by zero, trace; $0 else. */
    return (vector == 5 || vector == 6 || vector == 7 || vector == 9) ? 12 : 8;
}

static unsigned fails_shown;

static void report(const char *name, const char *what, uint32_t want, uint32_t got) {
    if (fails_shown++ < 40) fprintf(stderr, "  %s: %s want %08X got %08X\n", name, what, (unsigned)want, (unsigned)got);
}

/* Runs one test; returns 1 pass, 2 pass with translated frame, 0 fail,
 * -1-SKIP_x skipped. */
static int run_test(const char *group, const char *name, const state *in, const state *out, m68k_cpu *cpu, testbus *bus) {
    const uint32_t op = in->prefetch[0];
    const uint32_t pc0 = in->reg[R_PC];
    /* Skips decided from the test itself. */
    if (!strcmp(group, "RTE")) return -1 - SKIP_RTE;
    if (!strcmp(group, "MOVEfromSR") && !(in->reg[R_SR] & 0x2000u)) return -1 - SKIP_MOVE_FROM_SR;
    if ((op & 0xF0FFu) == 0x60FFu) return -1 - SKIP_BCC_FF;
    if ((op & 0xFBB8u) == 0x48A0u && (in->prefetch[1] & (1u << (7u - (op & 7u))))) return -1 - SKIP_MOVEM_PREDEC;
    for (size_t i = 0; i < sizeof suite_errors / sizeof suite_errors[0]; i++)
        if (!strcmp(name, suite_errors[i])) return -1 - SKIP_SUITE;
    /* The 68000 took an address error: it ended in the vector 3 handler. */
    if (has_expected(in, 0x0C, 4) && out->reg[R_PC] == tb_get_expected(in, 0x0C, 4)) return -1 - SKIP_ADDRESS_ERROR;

    /* Set up. */
    bus->ndirty = 0;
    for (uint32_t i = 0; i < in->n; i++) {
        uint32_t a, v;
        ram_entry(in, i, &a, &v);
        tb_put(bus, a, 1, v);
    }
    tb_put(bus, pc0, 2, in->prefetch[0]);
    tb_put(bus, pc0 + 2, 2, in->prefetch[1]);
    /* Through the translator: what the test wrote is no longer what any
     * block was translated from. */
    if (cpu->jit) {
        m68k_jit_invalidate(cpu, pc0 & 0x00FFFFFFu, 4);
        for (uint32_t i = 0; i < in->n; i++) {
            uint32_t a, v;
            ram_entry(in, i, &a, &v);
            m68k_jit_invalidate(cpu, a & 0x00FFFFFFu, 1);
        }
    }
    run_ctx rc;
    memset(&rc, 0, sizeof rc);
    cpu->on_exception = on_exception;
    cpu->hook_ctx = &rc;
    cpu->events = 0;
    cpu->status = M68K_OK;
    cpu->wb_n = 0;
    cpu->ea_ext020 = 0;
    cpu->vbr = 0;
    cpu->ipl = 0;
    m68k_set_sr(cpu, (uint16_t)in->reg[R_SR]);
    for (int i = 0; i < 15; i++) cpu->r[i] = in->reg[i];
    m68k_set_usp(cpu, in->reg[R_USP]);
    m68k_set_isp(cpu, in->reg[R_SSP]);
    m68k_set_msp(cpu, 0);
    cpu->pc = pc0;
    m68k_run(cpu, 1);

    int result = 1;
    if (cpu->ea_ext020) result = -1 - SKIP_EXT020;
    const uint32_t sr = m68k_sr(cpu);
    if (result == 1 && (sr & 0x5000u) && (sr & 0xA71Fu) == out->reg[R_SR]) result = -1 - SKIP_SR_T0_M;
    if (result == 1 && rc.exceptions && rc.vector[0] == 3) result = -1 - SKIP_ADDRESS_ERROR;
    if (result == 1 && rc.exceptions == 2) result = -1 - SKIP_NESTED;
    if (result == 1 && !strncmp(group, "ASR", 3) && (op & 0x20u)) {
        const unsigned bits = ((op >> 6) & 3u) == 0 ? 8u : ((op >> 6) & 3u) == 1 ? 16u : 32u;
        if ((in->reg[(op >> 9) & 7u] & 63u) >= bits && (sr & ~0x11u) == (out->reg[R_SR] & ~0x11u)) {
            int same = 1;
            for (int i = 0; i < 15; i++) same &= cpu->r[i] == out->reg[i];
            if (same && (sr & 0x11u) != (out->reg[R_SR] & 0x11u)) result = -1 - SKIP_ASR_LONG;
        }
    }
    if (result != 1) goto clean;

    /* Compare. */
    {
        const unsigned undef = undefined_ccr(group, cpu, &rc);
        const int translated = rc.exceptions == 1;
        uint32_t got[19];
        for (int i = 0; i < 15; i++) got[i] = cpu->r[i];
        got[R_USP] = m68k_get_usp(cpu);
        got[R_SSP] = m68k_get_isp(cpu);
        got[R_SR] = sr;
        got[R_PC] = cpu->pc;
        uint32_t want_ssp = out->reg[R_SSP];
        uint32_t frame_lo = 0, frame_hi = 0;
        if (translated) {
            const int size = frame_size(rc.vector[0]);
            want_ssp -= (uint32_t)(size - 6);
            frame_lo = out->reg[R_SSP];
            frame_hi = frame_lo + 6;
            /* SR and PC stacked as the 68000 stacked them. */
            const uint32_t sp = got[R_SSP];
            if ((tb_get(bus, sp, 2) & ~undef) != (tb_get_expected(out, frame_lo, 2) & ~undef)) {
                report(name, "stacked SR", tb_get_expected(out, frame_lo, 2), tb_get(bus, sp, 2));
                result = 0;
            }
            if (tb_get(bus, sp + 2, 4) != tb_get_expected(out, frame_lo + 2, 4)) {
                report(name, "stacked PC", tb_get_expected(out, frame_lo + 2, 4), tb_get(bus, sp + 2, 4));
                result = 0;
            }
        }
        static const char *const names[19] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1",
                                              "A2", "A3", "A4", "A5", "A6", "USP", "SSP", "SR", "PC"};
        for (int i = 0; i < 19; i++) {
            uint32_t want = i == R_SSP ? want_ssp : out->reg[i], g = got[i];
            if (i == R_SR) {
                want &= ~undef;
                g &= ~undef;
            }
            if (want != g) {
                report(name, names[i], want, g);
                result = 0;
            }
        }
        for (uint32_t i = 0; i < out->n; i++) {
            uint32_t a, v;
            ram_entry(out, i, &a, &v);
            if (translated && a >= frame_lo && a < frame_hi) continue;
            if (translated && a >= want_ssp && a < frame_lo) continue;
            const uint32_t g = tb_get(bus, a, 1);
            if (g != v) {
                char what[32];
                snprintf(what, sizeof what, "byte %06X", (unsigned)(a & 0xFFFFFFu));
                report(name, what, v, g);
                result = 0;
            }
        }
        if (result == 1 && translated) result = 2;
    }
clean:
    /* Clear what the test touched. */
    for (uint32_t i = 0; i < in->n; i++) {
        uint32_t a, v;
        ram_entry(in, i, &a, &v);
        tb_put(bus, a, 1, 0);
    }
    tb_put(bus, pc0, 4, 0);
    for (size_t i = 0; i < bus->ndirty; i++) bus->ram[bus->dirty[i]] = 0;
    return result;
}

static int run_file(const char *path, const char *group, counts *c, m68k_cpu *cpu, testbus *bus) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len);
    if (fread(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    if (len < 8 || (memcmp(data, "SST1", 4) && memcmp(data, "SST2", 4))) {
        free(data);
        return -1;
    }
    const int v2 = !memcmp(data, "SST2", 4);
    uint32_t count;
    memcpy(&count, data + 4, 4);
    const uint8_t *p = data + 8;
    counts local;
    memset(&local, 0, sizeof local);
    for (uint32_t t = 0; t < count; t++) {
        uint16_t nl;
        memcpy(&nl, p, 2);
        char name[128];
        const size_t n = nl < sizeof name - 1 ? nl : sizeof name - 1;
        memcpy(name, p + 2, n);
        name[n] = 0;
        p += 2 + nl;
        state in, out;
        p = read_state(p, &in);
        p = read_state(p, &out);
        if (v2) {
            trans tr;
            p = read_trans(p, &tr);
        }
        const int r = run_test(group, name, &in, &out, cpu, bus);
        local.total++;
        if (r == 1) local.passed++;
        else if (r == 2) local.frames++;
        else if (r == 0) local.failed++;
        else local.skipped[-1 - r]++;
    }
    free(data);
    unsigned long skipped = 0;
    for (int i = 0; i < SKIP_COUNT; i++) {
        skipped += local.skipped[i];
        c->skipped[i] += local.skipped[i];
    }
    printf("%-12s %6lu tests: %6lu passed, %5lu passed with 68040 frames, %5lu skipped, %5lu failed\n", group,
           local.total, local.passed, local.frames, skipped, local.failed);
    c->total += local.total;
    c->passed += local.passed;
    c->frames += local.frames;
    c->failed += local.failed;
    return 0;
}

int mc_cpu_sst_jit;

#if !defined(_WIN32)
/* The converted suite's file names, sorted as the Windows listing is, so a
 * slice ("%N/M") holds the same groups on every host. */
static int sst_name_order(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int sst_list(const char *dir, char ***out) {
    DIR *d = opendir(dir);
    char **names = NULL;
    int n = 0, cap = 0;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        const size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".sst") != 0) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            names = realloc(names, (size_t)cap * sizeof *names);
        }
        names[n++] = strdup(e->d_name);
    }
    if (d) closedir(d);
    if (n) qsort(names, (size_t)n, sizeof *names, sst_name_order);
    *out = names;
    return n;
}
static void sst_list_free(char **names, int n) {
    for (int k = 0; k < n; k++) free(names[k]);
    free(names);
}
#endif

/* `only`: one group's name, or "%N/M": the groups whose position in the
 * (sorted) listing is N modulo M, so a slice of the suite runs in one process. */
static int sst_wanted(const char *only, const char *group) {
    static int position;
    if (!only) return 1;
    if (only[0] != '%') return !strcmp(only, group);
    int n = 0, m = 1;
    sscanf(only + 1, "%d/%d", &n, &m);
    return m > 0 && position++ % m == n;
}

int mc_cpu_sst000(const char *dir, const char *only);
int mc_cpu_sst_68000;

int mc_cpu_sst(const char *dir, const char *only) {
    if (mc_cpu_sst_68000) return mc_cpu_sst000(dir, only);
    testbus bus;
    tb_init(&bus, 1u << 24, 0x00FFFFFFu, 1);
    m68k_bus mb = tb_bus(&bus);
    m68k_cpu *cpu = calloc(1, sizeof *cpu);
    m68k_init(cpu, M68K_68040, &mb);
    /* --jit: each test's instruction through the translator (one-instruction
     * blocks: every test runs one instruction). */
    if (mc_cpu_sst_jit) {
        if (!m68k_jit_enable(cpu, 1)) {
            printf("sst: no translator on this host; skipped%c", 10);
            return 77;
        }
        m68k_jit_options(cpu, 1, 1, 1);
        m68k_jit_threshold(cpu, 0);
    }
    counts c;
    memset(&c, 0, sizeof c);
    int files = 0;
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.sst", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h != -1) {
        do {
            char group[256], path[1024];
            snprintf(group, sizeof group, "%s", fd.name);
            char *dot = strrchr(group, '.');
            if (dot) *dot = 0;
            if (!sst_wanted(only, group)) continue;
            snprintf(path, sizeof path, "%s/%s", dir, fd.name);
            if (run_file(path, group, &c, cpu, &bus) == 0) files++;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    char **names;
    const int count = sst_list(dir, &names);
    for (int k = 0; k < count; k++) {
        char group[256], path[1024];
        snprintf(group, sizeof group, "%s", names[k]);
        char *dot = strrchr(group, '.');
        if (dot) *dot = 0;
        if (!sst_wanted(only, group)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        if (run_file(path, group, &c, cpu, &bus) == 0) files++;
    }
    sst_list_free(names, count);
#endif
    if (!files) {
        printf("sst: no converted suite in %s (python tests/cpu_suite.py fetch)\n", dir);
        free(cpu);
        tb_free(&bus);
        return 77;
    }
    unsigned long skipped = 0;
    for (int i = 0; i < SKIP_COUNT; i++) skipped += c.skipped[i];
    printf("\nSingleStepTests 68000 on the 68040 core: %lu tests in %d files\n", c.total, files);
    printf("  passed                      %8lu\n", c.passed);
    printf("  passed with 68040 frames    %8lu\n", c.frames);
    printf("  skipped (68000-only)        %8lu\n", skipped);
    for (int i = 0; i < SKIP_COUNT; i++)
        if (c.skipped[i]) printf("    %-62s %7lu\n", skip_names[i], c.skipped[i]);
    printf("  failed                      %8lu\n", c.failed);
    if (cpu->jit) {
        m68k_jit_stats js;
        m68k_jit_get_stats(cpu, &js);
        fprintf(stderr, "translator: %llu blocks, %llu runs%c", (unsigned long long)js.blocks, (unsigned long long)js.exits, 10);
    }
    m68k_free(cpu);
    free(cpu);
    tb_free(&bus);
    return c.failed ? 1 : 0;
}

/* --- the 68000 model: mc_cpu_tests sst DIR [GROUP] --68000 ------------------
 *
 * Every test is run on the 68000 model, timed, and compared in full: the
 * nineteen registers with every SR bit (the condition codes the PRM leaves
 * undefined included), every RAM byte the test lists (exception frames,
 * group 0 frames included), the clock cycles and the bus transactions: each
 * read, write and read-modify-write cycle the test records at the clock
 * cycle it starts, its address, function code and direction (m68k_bus.wait
 * sees every bus cycle the model runs). Nothing is skipped. Counted apart:
 * the tests whose expected state no instruction can produce (`suite_errors`),
 * and the two cases where the suite contradicts Motorola's manuals and the
 * model follows the manuals, each recognised by its exact difference with
 * nothing else differing:
 *   - ASR Dn,Dy by a count above the operand size with a negative operand:
 *     the suite clears C and X; the PRM (ASR: "set according to the last
 *     bit shifted out") gives the sign bit, the last bit shifted out (the
 *     suite itself sets it for a count equal to the size);
 *   - ADDQ.L and SUBQ.L #<data>,An: the suite times 6 clocks, the MC68000
 *     UM (Table 8-6, immediate instruction execution times) 8(1/0). */

typedef struct bus_rec {
    uint32_t address, cycle;
    uint8_t fc, kind;
} bus_rec;
static bus_rec seen[256];
static unsigned nseen;
static uint64_t seen_base;

static unsigned log_wait(void *ctx, uint32_t address, unsigned fc, unsigned kind, uint64_t cycle) {
    (void)ctx;
    if (nseen < 256) {
        seen[nseen].address = address;
        seen[nseen].cycle = (uint32_t)(cycle - seen_base);
        seen[nseen].fc = (uint8_t)fc;
        seen[nseen].kind = (uint8_t)kind;
    }
    nseen++;
    return 0;
}

int mc_cpu_sst000_verbose;

enum { R000_PASS, R000_FAIL, R000_SUITE, R000_ASR, R000_ADDQ, R000_KINDS };

static int run000(const char *group, const char *name, const state *in, const state *out, const trans *tr, m68k_cpu *cpu,
                  testbus *bus, char *why, size_t whylen) {
    const uint32_t pc0 = in->reg[R_PC];
    why[0] = 0;
    bus->ndirty = 0;
    for (uint32_t i = 0; i < in->n; i++) {
        uint32_t a, v;
        ram_entry(in, i, &a, &v);
        tb_put(bus, a, 1, v);
    }
    tb_put(bus, pc0, 2, in->prefetch[0]);
    tb_put(bus, pc0 + 2, 2, in->prefetch[1]);
    cpu->events = 0;
    cpu->status = M68K_OK;
    cpu->ipl = 0;
    cpu->on_exception = NULL;
    m68k_set_sr(cpu, (uint16_t)in->reg[R_SR]);
    for (int i = 0; i < 15; i++) cpu->r[i] = in->reg[i];
    m68k_set_usp(cpu, in->reg[R_USP]);
    m68k_set_isp(cpu, in->reg[R_SSP]);
    cpu->pc = pc0;
    nseen = 0;
    seen_base = cpu->cycles;
    const uint64_t c0 = cpu->cycles;
    m68k_run(cpu, 1);
    const uint32_t cycles = (uint32_t)(cpu->cycles - c0);

    int result = R000_PASS;
    char *w = why;
    size_t left = whylen;
#define WHY(...)                                                                    \
    do {                                                                            \
        if (left > 1) {                                                             \
            const int k_ = snprintf(w, left, __VA_ARGS__);                          \
            if (k_ > 0 && (size_t)k_ < left) {                                      \
                w += k_;                                                            \
                left -= (size_t)k_;                                                 \
            } else {                                                                \
                left = 0;                                                           \
            }                                                                       \
        }                                                                           \
        result = R000_FAIL;                                                         \
    } while (0)
    uint32_t got[19];
    for (int i = 0; i < 15; i++) got[i] = cpu->r[i];
    got[R_USP] = m68k_get_usp(cpu);
    got[R_SSP] = m68k_get_isp(cpu);
    got[R_SR] = m68k_sr(cpu);
    got[R_PC] = cpu->pc;
    static const char *const names[19] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "A0", "A1",
                                          "A2", "A3", "A4", "A5", "A6", "USP", "SSP", "SR", "PC"};
    int other = 0, srcx = 0;
    for (int i = 0; i < 19; i++)
        if (got[i] != out->reg[i]) {
            if (i == R_SR && ((got[i] ^ out->reg[i]) & ~0x11u) == 0) srcx = 1;
            else other = 1;
            WHY(" %s %08X/%08X", names[i], (unsigned)out->reg[i], (unsigned)got[i]);
        }
    for (uint32_t i = 0; i < out->n; i++) {
        uint32_t a, v;
        ram_entry(out, i, &a, &v);
        const uint32_t g = tb_get(bus, a, 1);
        if (g != v) {
            other = 1;
            WHY(" [%06X] %02X/%02X", (unsigned)(a & 0xFFFFFFu), (unsigned)v, (unsigned)g);
        }
    }
    if (cycles != tr->length) WHY(" cycles %u/%u", (unsigned)tr->length, (unsigned)cycles);
    /* The bus cycles, in order, at their clock cycles. */
    {
        uint32_t at = 0;
        unsigned k = 0;
        int bad = 0;
        for (uint32_t i = 0; i < tr->m && !bad; i++) {
            const uint8_t *t = tr->t + 10u * i;
            uint32_t address;
            memcpy(&address, t + 4, 4);
            if (t[0] != 0) {
                const unsigned kind = t[0] == 1 ? 0u : t[0] == 2 ? 1u : 2u;
                if (k >= nseen || k >= 256) {
                    WHY(" bus %u: %c %06X @%u missing", k, "rwt"[kind], (unsigned)(address & 0xFFFFFFu), (unsigned)at);
                    bad = 1;
                    break;
                }
                const bus_rec *s = &seen[k];
                if (s->kind != kind || s->address != (address & 0xFFFFFFu) || s->fc != t[2] || s->cycle != at) {
                    WHY(" bus %u: %c%u %06X @%u / %c%u %06X @%u", k, "rwt"[kind], t[2], (unsigned)(address & 0xFFFFFFu),
                        (unsigned)at, "rwti"[s->kind & 3u], s->fc, (unsigned)s->address, (unsigned)s->cycle);
                    bad = 1;
                    break;
                }
                k++;
            }
            at += t[1];
        }
        if (!bad && k != nseen) {
            bad = 1;
            WHY(" bus: %u cycles more than recorded", nseen - k);
        }
        other |= bad;
    }
#undef WHY
    if (result != R000_PASS) {
        const uint32_t op = in->prefetch[0];
        for (size_t i = 0; i < sizeof suite_errors / sizeof suite_errors[0]; i++)
            if (!strcmp(name, suite_errors[i])) result = R000_SUITE;
        if (result == R000_FAIL && !other && srcx && cycles == tr->length && !strncmp(group, "ASR", 3) &&
            (op & 0xF038u) == 0xE020u) {
            const unsigned bits = ((op >> 6) & 3u) == 0 ? 8u : ((op >> 6) & 3u) == 1 ? 16u : 32u;
            const uint32_t v = in->reg[op & 7u];
            if ((in->reg[(op >> 9) & 7u] & 63u) > bits && ((v >> (bits - 1u)) & 1u)) result = R000_ASR;
        }
        if (result == R000_FAIL && !other && !srcx && (op & 0xF0F8u) == 0x5088u && tr->length == 6 && cycles == 8)
            result = R000_ADDQ;
    }

    for (uint32_t i = 0; i < in->n; i++) {
        uint32_t a, v;
        ram_entry(in, i, &a, &v);
        tb_put(bus, a, 1, 0);
    }
    tb_put(bus, pc0, 4, 0);
    for (size_t i = 0; i < bus->ndirty; i++) bus->ram[bus->dirty[i]] = 0;
    return result;
}

static int run_file000(const char *path, const char *group, unsigned long c[R000_KINDS], m68k_cpu *cpu, testbus *bus) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)len);
    if (fread(data, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);
    if (len < 8 || memcmp(data, "SST2", 4)) {
        free(data);
        return -2;
    }
    uint32_t count;
    memcpy(&count, data + 4, 4);
    const uint8_t *p = data + 8;
    unsigned long local[R000_KINDS] = {0};
    unsigned shown = 0;
    for (uint32_t t = 0; t < count; t++) {
        uint16_t nl;
        memcpy(&nl, p, 2);
        char name[128];
        const size_t n = nl < sizeof name - 1 ? nl : sizeof name - 1;
        memcpy(name, p + 2, n);
        name[n] = 0;
        p += 2 + nl;
        state in, out;
        trans tr;
        p = read_state(p, &in);
        p = read_state(p, &out);
        p = read_trans(p, &tr);
        char why[512];
        const int r = run000(group, name, &in, &out, &tr, cpu, bus, why, sizeof why);
        local[r]++;
        if (r == R000_FAIL && shown < (unsigned)mc_cpu_sst000_verbose) {
            shown++;
            fprintf(stderr, "  %s:%s\n", name, why);
        }
    }
    free(data);
    unsigned long total = 0;
    for (int i = 0; i < R000_KINDS; i++) {
        total += local[i];
        c[i] += local[i];
    }
    printf("%-12s %6lu tests: %6lu passed, %5lu failed", group, total, local[R000_PASS], local[R000_FAIL]);
    if (total != local[R000_PASS] + local[R000_FAIL]) printf(", %lu counted apart", total - local[R000_PASS] - local[R000_FAIL]);
    printf("\n");
    return 0;
}

int mc_cpu_sst000(const char *dir, const char *only) {
    testbus bus;
    tb_init(&bus, 1u << 24, 0x00FFFFFFu, 1);
    m68k_bus mb = tb_bus(&bus);
    mb.wait = log_wait;
    m68k_cpu *cpu = calloc(1, sizeof *cpu);
    m68k_init(cpu, M68K_68000, &mb);
    m68k_set_timing(cpu, 1);
    unsigned long c[R000_KINDS] = {0};
    int files = 0, old = 0;
#if defined(_WIN32)
    char pattern[1024];
    snprintf(pattern, sizeof pattern, "%s/*.sst", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h != -1) {
        do {
            char group[256], path[1024];
            snprintf(group, sizeof group, "%s", fd.name);
            char *dot = strrchr(group, '.');
            if (dot) *dot = 0;
            if (!sst_wanted(only, group)) continue;
            snprintf(path, sizeof path, "%s/%s", dir, fd.name);
            const int r = run_file000(path, group, c, cpu, &bus);
            if (r == 0) files++;
            else if (r == -2) old++;
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    char **names;
    const int count = sst_list(dir, &names);
    for (int k = 0; k < count; k++) {
        char group[256], path[1024];
        snprintf(group, sizeof group, "%s", names[k]);
        char *dot = strrchr(group, '.');
        if (dot) *dot = 0;
        if (!sst_wanted(only, group)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, names[k]);
        const int r = run_file000(path, group, c, cpu, &bus);
        if (r == 0) files++;
        else if (r == -2) old++;
    }
    sst_list_free(names, count);
#endif
    if (!files) {
        printf("sst: no converted suite in %s%s (python tests/cpu_suite.py fetch)\n", dir,
               old ? " in the current format" : "");
        m68k_free(cpu);
        free(cpu);
        tb_free(&bus);
        return 77;
    }
    unsigned long total = 0;
    for (int i = 0; i < R000_KINDS; i++) total += c[i];
    printf("\nSingleStepTests 68000 on the 68000 model: %lu tests in %d files, nothing skipped\n", total, files);
    printf("  passed (registers, RAM, cycles, bus cycles)          %8lu\n", c[R000_PASS]);
    printf("  the manuals followed where the suite differs:\n");
    printf("    ASR by more than the size, negative: C and X       %8lu\n", c[R000_ASR]);
    printf("    ADDQ.L/SUBQ.L #,An: 8 clocks (UM Table 8-6), not 6 %8lu\n", c[R000_ADDQ]);
    printf("  the suite's own errors                               %8lu\n", c[R000_SUITE]);
    printf("  failed                                               %8lu\n", c[R000_FAIL]);
    m68k_free(cpu);
    free(cpu);
    tb_free(&bus);
    return c[1] ? 1 : 0;
}
