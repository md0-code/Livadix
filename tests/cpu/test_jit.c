/* mc_cpu_tests jit [CASES [SEED]]: the translator against the interpreter.
 *
 * Livadix's own differential test.
 * Each case builds one random machine state and runs it on two CPUs, one
 * interpreting and one translating, in slices of random lengths, comparing
 * after every slice every register, the status register, the PC, the
 * instruction count, the run status and all of RAM. The programs are random
 * instruction words of the model (mostly from the families the translator
 * handles, the rest anything the model decodes), with random extension
 * words; address registers point into data, into an area that bus-errors,
 * across page ends, and into the program itself (self-modifying code).
 * Cases vary the CPU model, translation on or off (identity tables with
 * some pages invalid or write-protected), writes through the fast path or
 * the slow one, call-out or leave-on-miss, idle skipping, block length, and
 * raise interrupts between slices. Two further sets run the 68020 with an
 * MC68851 translating through its tables (MC68851 UM 5.1: 4 KiB pages
 * through two levels, or the Macintosh II ROM's 32 KiB pages with A31-A24
 * ignored), and the 68020 without a PMMU on a 24-bit decode (A31-A24
 * ignored by the bus, address registers with random top bytes) switched
 * to 32-bit and back between slices, as the AMU switches. */
#include "cpu_test.h"
#include "../test.h"

#define RAM_SIZE (1u << 20)
#define CODE 0x10000u
#define CODE_SIZE 0x3000u
#define DATA 0x40000u
#define FAULT_LO 0x70000u
#define FAULT_HI 0x71000u
#define STACK 0x90000u
#define HANDLER 0x8000u
#define TABLES 0xC0000u

static uint64_t rng;
static uint32_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint32_t)(rng >> 16);
}
static uint32_t below(uint32_t n) { return n ? rnd() % n : 0; }

/* The set of programs with full-format extension words, memory indirect or
 * not (PRM Table 2-2), MOVE16 and the SR writes (jit_diff): one of its own,
 * so the other sets' programs stay as they were. */
static int full_ext;
static int extra_op(const char *name) {
    static const char *const names[] = {"MOVE16", "MOVE to SR", "ORI to SR", "ANDI to SR", "EORI to SR", "BFTST", "BFEXTU",
                                        "BFCHG", "BFEXTS", "BFCLR", "BFSET", "BFINS", "BFFFO", "DIVU.W", "DIVS.W", "DIVx.L",
                                        "MULx.L", "FPU"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(name, names[i])) return 1;
    return 0;
}

/* Families the translator handles (m68k_decode names). */
static int preferred(const char *name) {
    if (full_ext && extra_op(name)) return 1;
    static const char *const prefixes[] = {"MOVE.", "MOVEA", "MOVEQ", "LEA", "PEA", "CLR", "TST", "NEG.", "NOT", "EXT",
                                           "SWAP", "EXG", "ADD", "SUB", "CMP", "AND", "OR", "EOR", "MULU.W", "MULS.W",
                                           "BTST", "BCHG", "BCLR", "BSET", "shift", "Bcc", "DBcc", "Scc", "JMP", "JSR",
                                           "RTS", "LINK.W", "UNLK", "MOVEM", "NOP"};
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return 1;
    return 0;
}

/* Instructions that would end the test early or take it off the rails on
 * both CPUs alike, so they only waste the case: STOP, RESET, RTE (random
 * frames), MOVEC/MOVES, the MMU and cache instructions, line A/F, MOVE to SR
 * (trace bits stop the translator: covered, but rarely), TRAP. */
static int excluded(const char *name) {
    static const char *const names[] = {"STOP", "RESET", "RTE", "MOVEC", "MOVES", "PFLUSH", "PTEST", "PMMU", "CINV",
                                        "CPUSH", "line A", "line F", "FPU", "FSAVE", "TRAP", "CHK", "ILLEGAL", "BKPT",
                                        "MOVE to SR", "ANDI to SR", "EORI to SR", "ORI to SR", "MOVE to USP", "RTM", "CALLM",
                                        "MOVE16", "TAS", "CAS", "DIV"};
    if (full_ext && extra_op(name)) return 0;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strncmp(name, names[i], strlen(names[i]))) return 1;
    return 0;
}

static uint16_t random_op(m68k_model model) {
    for (;;) {
        const uint16_t op = (uint16_t)rnd();
        const m68k_opinfo info = m68k_decode(model, op);
        if (!info.valid || !info.name || excluded(info.name)) continue;
        if (preferred(info.name) || below(8) == 0) return op;
    }
}

static uint16_t random_ext(void) {
    if (full_ext && below(3) == 0)
        return (uint16_t)(below(16) << 12 | below(2) << 11 | below(4) << 9 | 0x0100u | below(2) << 7 | below(2) << 6 |
                          (1u + below(3)) << 4 | below(8));
    switch (below(7)) {
    case 6:     /* a full-format word in every set (a table walk's U bit over translated code, mmu.c) */
        return (uint16_t)(below(16) << 12 | below(2) << 11 | below(4) << 9 | 0x0100u | below(2) << 7 | below(2) << 6 |
                          (1u + below(3)) << 4 | below(8));
    case 0: return (uint16_t)rnd();
    case 1: return (uint16_t)(below(64) - 32);
    case 2: return (uint16_t)(below(16) << 12 | below(2) << 11 | below(4) << 9 | (below(64) & 0xFE));   /* brief index word */
    case 3: return (uint16_t)(DATA >> 16);
    case 4: return (uint16_t)below(0x400);
    default: return (uint16_t)(below(8) * 2);
    }
}

typedef struct side {
    testbus bus;
    m68k_cpu *cpu;
    uint8_t **map24, **wmap24;      /* 24-bit decode: the RAM at every value of A31-A24 */
    int on24;
} side;

/* Which set of cases build() makes: 0 every model, 1 the 68020 with an
 * MC68851, 2 the 68020 on a 24-bit decode. */
static int variant;
/* Both sides timed (m68k_set_timing): counts are cycles, compared too. */
static int timed;

static void identity_tables(testbus *bus, int p8k, int holes) {
    /* MC68040 UM 3.2: root at TABLES, one pointer table, page tables
     * identity-mapping the RAM; a few pages invalid or write-protected. */
    const uint32_t root = TABLES, ptr = TABLES + 0x200, pages = TABLES + 0x400;
    for (uint32_t a = root; a < root + 0x400; a += 4) tb_put(bus, a, 4, 0);
    tb_put(bus, root, 4, ptr | 3);
    const unsigned per = p8k ? 32 : 64, size = p8k ? 0x2000 : 0x1000;
    const unsigned tables = RAM_SIZE / size / per;
    for (uint32_t i = 0; i < tables; i++) tb_put(bus, ptr + 4 * i, 4, (pages + 256 * i) | 3);
    for (uint32_t p = 0; p < RAM_SIZE / size; p++) {
        uint32_t d = p * size | 1;
        const uint32_t a = p * size;
        if (holes && a >= DATA && a < STACK - 0x10000) {
            const uint32_t r = (p * 2654435761u) >> 28;
            if (r == 1) d = 0;              /* invalid */
            else if (r == 2) d |= 4;        /* write protected */
        }
        tb_put(bus, pages + 4 * p, 4, d);
    }
}

/* MC68851 tables (MC68851 UM 5.1, short descriptors): big 0, TC $80C0AA00
 * (4 KiB pages, TIA 10, TIB 10) with page tables identity-mapping the RAM
 * and some data pages invalid or write-protected; big 1, the Macintosh II
 * ROM's TC $80F84500 (32 KiB pages, IS 8, TIA 4, TIB 5) with one early-
 * terminated descriptor per MiB, segment 0 the RAM. */
static void tables851(testbus *bus, int big, int holes) {
    for (uint32_t a = TABLES; a < TABLES + 0x2000; a += 4) tb_put(bus, a, 4, 0);
    if (big) {
        tb_put(bus, TABLES, 4, 0x00000001u);
        if (holes) tb_put(bus, TABLES + 4, 4, 0x00000005u);     /* segment 1: the RAM again, write-protected */
        return;
    }
    tb_put(bus, TABLES, 4, (TABLES + 0x1000) | 2);
    for (uint32_t p = 0; p < RAM_SIZE / 0x1000; p++) {
        uint32_t d = p * 0x1000 | 1;
        const uint32_t a = p * 0x1000;
        if (holes && a >= DATA && a < STACK - 0x10000) {
            const uint32_t r = (p * 2654435761u) >> 28;
            if (r == 1) d = 0;
            else if (r == 2) d |= 4;
        }
        tb_put(bus, TABLES + 0x1000 + 4 * p, 4, d);
    }
}

static void use24(side *s, int on) {
    s->on24 = on;
    s->bus.mask = on ? 0x00FFFFFFu : 0xFFFFFFFFu;
    uint8_t **r = on ? s->map24 : s->bus.map, **w = on ? s->wmap24 : s->bus.wmap;
    m68k_set_maps(s->cpu, r, w, r, w);
}

static void setup(side *s, m68k_model model, const uint8_t *image, int track, int mmu, int p8k, int holes) {
    s->map24 = s->wmap24 = NULL;
    s->on24 = 0;
    tb_init(&s->bus, RAM_SIZE, 0xFFFFFFFFu, track);
    memcpy(s->bus.ram, image, RAM_SIZE);
    s->bus.fault_lo = FAULT_LO;
    s->bus.fault_hi = FAULT_HI;
    m68k_bus mb = tb_bus(&s->bus);
    s->cpu = calloc(1, sizeof *s->cpu);
    m68k_init(s->cpu, model, &mb);
    m68k_reset(s->cpu);
    if (mmu && (model == M68K_68040 || model == M68K_68LC040)) {
        identity_tables(&s->bus, p8k, holes);
        m68k_movec_write(s->cpu, 0x806, TABLES);
        m68k_movec_write(s->cpu, 0x807, TABLES);
        m68k_movec_write(s->cpu, 0x003, p8k ? 0xC000u : 0x8000u);
    }
    if (variant == 1) {
        /* PMOVE (A0),CRP; PMOVE (A1),TC from a scratch area, before the
         * case's state is set. */
        m68k_set_pmmu(s->cpu, 1);
        if (mmu) {
            tables851(&s->bus, p8k, holes);
            static const uint16_t init[] = {0xF010, 0x4C00, 0xF011, 0x4000};
            for (unsigned i = 0; i < 4; i++) tb_put(&s->bus, TABLES + 0x2100 + 2 * i, 2, init[i]);
            tb_put(&s->bus, TABLES + 0x2110, 4, 0x7FFF0002u);
            tb_put(&s->bus, TABLES + 0x2114, 4, TABLES);
            tb_put(&s->bus, TABLES + 0x2118, 4, p8k ? 0x80F84500u : 0x80C0AA00u);
            s->cpu->r[8] = TABLES + 0x2110;
            s->cpu->r[9] = TABLES + 0x2118;
            s->cpu->pc = TABLES + 0x2100;
            m68k_run(s->cpu, 2);
            s->cpu->instructions = 0;
            s->cpu->cycles = 0;
        }
    }
    m68k_set_timing(s->cpu, timed);
    if (variant == 2) {
        s->map24 = calloc(M68K_PAGE_COUNT, sizeof *s->map24);
        s->wmap24 = calloc(M68K_PAGE_COUNT, sizeof *s->wmap24);
        for (uint64_t p = 0; p < M68K_PAGE_COUNT; p++) {
            const uint32_t q = (uint32_t)(p & ((1u << (24 - M68K_PAGE_SHIFT)) - 1));
            s->map24[p] = s->bus.map[q];
            s->wmap24[p] = s->bus.wmap[q];
        }
        use24(s, 1);
    }
}

static int same(const side *a, const side *b, char *why, size_t cap) {
    const m68k_cpu *x = a->cpu, *y = b->cpu;
    for (unsigned i = 0; i < 16; i++)
        if (x->r[i] != y->r[i]) {
            snprintf(why, cap, "%c%u: interpreter %08X, translator %08X", i < 8 ? 'D' : 'A', i & 7u, (unsigned)x->r[i], (unsigned)y->r[i]);
            return 0;
        }
    if (x->pc != y->pc) { snprintf(why, cap, "PC: %08X vs %08X", (unsigned)x->pc, (unsigned)y->pc); return 0; }
    if (m68k_sr(x) != m68k_sr(y)) { snprintf(why, cap, "SR: %04X vs %04X", m68k_sr(x), m68k_sr(y)); return 0; }
    if (x->instructions != y->instructions) {
        snprintf(why, cap, "instructions: %llu vs %llu", (unsigned long long)x->instructions, (unsigned long long)y->instructions);
        return 0;
    }
    if (x->cycles != y->cycles || x->budget != y->budget) {
        snprintf(why, cap, "cycles: %llu vs %llu (budget %lld vs %lld)", (unsigned long long)x->cycles,
                 (unsigned long long)y->cycles, (long long)x->budget, (long long)y->budget);
        return 0;
    }
    if (x->status != y->status) { snprintf(why, cap, "status: %d vs %d", (int)x->status, (int)y->status); return 0; }
    if (m68k_get_usp(x) != m68k_get_usp(y) || m68k_get_isp(x) != m68k_get_isp(y) || m68k_get_msp(x) != m68k_get_msp(y)) {
        snprintf(why, cap, "stack pointers differ");
        return 0;
    }
    if (x->mmusr != y->mmusr || x->vbr != y->vbr || x->cacr != y->cacr) { snprintf(why, cap, "control registers differ"); return 0; }
    /* The MMU's own state: the translation caches and their replacement. */
    if (memcmp(x->mmu.atc, y->mmu.atc, sizeof x->mmu.atc) || memcmp(x->mmu.ctr, y->mmu.ctr, sizeof x->mmu.ctr) ||
        memcmp(x->mmu.atc030, y->mmu.atc030, sizeof x->mmu.atc030) || x->mmu.next030 != y->mmu.next030 ||
        memcmp(x->pmmu.atc, y->pmmu.atc, sizeof x->pmmu.atc) || x->pmmu.next != y->pmmu.next ||
        x->mmu.mmusr30 != y->mmu.mmusr30 || a->on24 != b->on24) {
        snprintf(why, cap, "ATC: counters %u/%u vs %u/%u", x->mmu.ctr[0], x->mmu.ctr[1], y->mmu.ctr[0], y->mmu.ctr[1]);
        return 0;
    }
    if (memcmp(a->bus.ram, b->bus.ram, RAM_SIZE) != 0) {
        for (uint32_t i = 0; i < RAM_SIZE; i++)
            if (a->bus.ram[i] != b->bus.ram[i]) {
                snprintf(why, cap, "RAM at %06X: %02X vs %02X", (unsigned)i, a->bus.ram[i], b->bus.ram[i]);
                break;
            }
        return 0;
    }
    return 1;
}

static void dump(const side *s, const char *who) {
    const m68k_cpu *c = s->cpu;
    printf("    %s: PC %08X SR %04X", who, (unsigned)c->pc, m68k_sr(c));
    printf("%c     D", 10);
    for (unsigned i = 0; i < 8; i++) printf(" %08X", (unsigned)c->r[i]);
    printf("%c     A", 10);
    for (unsigned i = 8; i < 16; i++) printf(" %08X", (unsigned)c->r[i]);
    printf("%c     VBR %08X vector 2 %08X status %d events %X instructions %llu cycles %llu", 10, (unsigned)c->vbr,
           (unsigned)tb_get((testbus *)&s->bus, c->vbr + 8, 4), (int)c->status, (unsigned)c->events,
           (unsigned long long)c->instructions, (unsigned long long)c->cycles);
    printf("%c     (A7)", 10);
    for (unsigned i = 0; i < 16; i += 2) printf(" %04X", (unsigned)tb_get((testbus *)&s->bus, c->r[15] + i, 2));
    printf("%c", 10);
}

typedef struct params {
    m68k_model model;
    int track, mmu, p8k, holes, callout, idle;
    unsigned max_block;
} params;

/* Builds case `seed` on both sides (the random stream then continues for
 * the slices). Returns 0 when the translator is not available. */
static int build(uint64_t seed, unsigned max_block_override, side s[2], params *p) {
    rng = seed * 0x9E3779B97F4A7C15ull + 1;
    static const m68k_model models[] = {M68K_68040, M68K_68040, M68K_68040, M68K_68LC040, M68K_68030, M68K_68020};
    p->model = models[below(6)];
    p->track = below(3) == 0;
    p->mmu = below(2);
    p->p8k = below(2);
    p->holes = below(2);
    p->callout = below(4) != 0;
    p->idle = below(2);
    p->max_block = max_block_override ? max_block_override : (below(4) == 0 ? 1 + below(8) : 0);
    if (variant) p->model = M68K_68020;
    if (variant == 2) p->mmu = 0;
    uint8_t *image = calloc(RAM_SIZE, 1);
    for (uint32_t i = 0; i < RAM_SIZE; i += 4) {
        const uint32_t v = rnd();
        memcpy(image + i, &v, 4);
    }
    /* Vectors to a handler that steps the stacked PC past two bytes. */
    for (unsigned v = 0; v < 256; v++) {
        image[4 * v] = 0; image[4 * v + 1] = 0; image[4 * v + 2] = HANDLER >> 8; image[4 * v + 3] = HANDLER & 0xFF;
    }
    static const uint8_t handler[] = {0x54, 0xAF, 0x00, 0x02, 0x4E, 0x73};    /* ADDQ.L #2,2(A7); RTE */
    memcpy(image + HANDLER, handler, sizeof handler);
    for (uint32_t a = CODE; a < CODE + CODE_SIZE;) {
        const uint16_t op = random_op(p->model);
        image[a] = (uint8_t)(op >> 8);
        image[a + 1] = (uint8_t)op;
        a += 2;
        const unsigned ext = below(4) == 0 ? 0 : below(3);
        for (unsigned k = 0; k < ext && a < CODE + CODE_SIZE; k++, a += 2) {
            const uint16_t w = random_ext();
            image[a] = (uint8_t)(w >> 8);
            image[a + 1] = (uint8_t)w;
        }
    }
    setup(&s[0], p->model, image, p->track, p->mmu, p->p8k, p->holes);
    setup(&s[1], p->model, image, p->track, p->mmu, p->p8k, p->holes);
    free(image);
    if (!m68k_jit_enable(s[1].cpu, 1)) return 0;
    m68k_jit_options(s[1].cpu, p->callout, p->idle, p->max_block);
    m68k_jit_threshold(s[1].cpu, 0);
    uint32_t regs[16];
    for (unsigned i = 0; i < 8; i++) regs[i] = below(3) ? (uint32_t)below(64) : rnd();
    for (unsigned i = 8; i < 15; i++) {
        switch (below(6)) {
        case 0: regs[i] = CODE + below(CODE_SIZE); break;                       /* self-modifying */
        case 1: regs[i] = FAULT_LO - 64 + below(256); break;                    /* bus errors */
        case 2: regs[i] = DATA + 0x1000 * below(32) - 2 + below(4); break;      /* page ends */
        default: regs[i] = DATA + below(0x20000); break;
        }
    }
    regs[15] = STACK - below(64) * 2;
    /* 24-bit: A31-A24 are not decoded (or, after a switch, are). */
    if (variant == 2)
        for (unsigned i = 8; i < 15; i++)
            if (below(2)) regs[i] |= below(256) << 24;
    const uint32_t pc = CODE + 2 * below(64);
    for (unsigned k = 0; k < 2; k++) {
        m68k_cpu *c = s[k].cpu;
        memcpy(c->r, regs, sizeof regs);
        m68k_set_sr(c, 0x2700);
        c->isp = STACK;
        c->r[15] = regs[15];
        c->pc = pc;
    }
    return 1;
}

static void release(side s[2]) {
    for (unsigned k = 0; k < 2; k++) {
        m68k_free(s[k].cpu);
        free(s[k].cpu);
        tb_free(&s[k].bus);
        free(s[k].map24);
        free(s[k].wmap24);
    }
}

/* Between slices of a 24-bit case: now and then the other mode. */
static void maybe_switch(side s[2]) {
    if (variant != 2 || below(8) != 0) return;
    use24(&s[0], !s[0].on24);
    use24(&s[1], !s[1].on24);
}

/* MC_JIT_DUMP=FILE: the blocks of the located case, for a disassembler
 * (records: u32 pc, u32 instructions, u32 size, the code). */
static FILE *dump_file;
static void dump_block(void *ctx, uint32_t pc, unsigned n, const uint8_t *code, size_t size) {
    (void)ctx;
    if (!dump_file) return;
    const uint32_t h[3] = {pc, n, (uint32_t)size};
    fwrite(h, 4, 3, dump_file);
    fwrite(code, 1, size, dump_file);
}

/* One slice of the schedule: its length, and an interrupt level to raise
 * first (-1: none); drawn from the case's random stream. */
static int64_t next_slice(int *irq) {
    const int64_t len = 1 + (int64_t)(below(4) == 0 ? below(4) : below(300));
    *irq = -1;
    if (below(16) == 0) *irq = (int)below(8);
    return len;
}

/* Rebuilds the case and runs its first `slices` slices on both sides. */
static void replay(uint64_t seed, unsigned max_block, unsigned slices, side s[2], params *p) {
    build(seed, max_block, s, p);
    (void)below(40);
    for (unsigned n = 0; n < slices; n++) {
        int irq;
        const int64_t len = next_slice(&irq);
        if (irq >= 0) {
            m68k_set_irq(s[0].cpu, (unsigned)irq);
            m68k_set_irq(s[1].cpu, (unsigned)irq);
        }
        m68k_run(s[0].cpu, len);
        m68k_run(s[1].cpu, len);
        maybe_switch(s);
    }
}

/* The first instruction of the failing slice where the two differ: the
 * case replayed to that slice, then run k instructions, k = 1, 2, ... */
static void locate(uint64_t seed, unsigned max_block, unsigned slice) {
    char why[128];
    side s[2];
    params p;
    replay(seed, max_block, slice, s, &p);
    int irq;
    const int64_t len = next_slice(&irq);
    release(s);
    for (int64_t k = 1; k <= len; k++) {
        replay(seed, max_block, slice, s, &p);
        if (irq >= 0) m68k_set_irq(s[0].cpu, (unsigned)irq);
        m68k_run(s[0].cpu, k - 1);
        const uint32_t pc = s[0].cpu->pc;
        uint16_t words[5];
        for (unsigned i = 0; i < 5; i++) words[i] = (uint16_t)tb_get(&s[0].bus, pc + 2 * i, 2);
        release(s);
        replay(seed, max_block, slice, s, &p);
        const char *path = getenv("MC_JIT_DUMP");
        if (path) {
            dump_file = fopen(path, "wb");
            m68k_jit_set_block_hook(s[1].cpu, dump_block, NULL);
            m68k_jit_flush(s[1].cpu);
        }
        if (irq >= 0) {
            m68k_set_irq(s[0].cpu, (unsigned)irq);
            m68k_set_irq(s[1].cpu, (unsigned)irq);
        }
        m68k_run(s[0].cpu, k);
        m68k_run(s[1].cpu, k);
        if (dump_file) {
            fclose(dump_file);
            dump_file = NULL;
        }
        if (!same(&s[0], &s[1], why, sizeof why)) {
            printf("    first difference at instruction %lld of the slice, PC %08X: %04X %04X %04X %04X %04X (%s): %s\n",
                   (long long)k, (unsigned)pc, words[0], words[1], words[2], words[3], words[4],
                   m68k_decode(p.model, words[0]).name, why);
            dump(&s[0], "interpreter");
            dump(&s[1], "translator ");
            release(s);
            /* Both just before that instruction. */
            replay(seed, max_block, slice, s, &p);
            if (irq >= 0) {
                m68k_set_irq(s[0].cpu, (unsigned)irq);
                m68k_set_irq(s[1].cpu, (unsigned)irq);
            }
            m68k_run(s[0].cpu, k - 1);
            m68k_run(s[1].cpu, k - 1);
            printf("    before it:%c", 10);
            dump(&s[0], "interpreter");
            dump(&s[1], "translator ");
            release(s);
            return;
        }
        release(s);
    }
    printf("    no difference when run in one slice\n");
}

/* One case; returns 0 when both CPUs agree throughout. */
static int one_case(uint64_t seed, int verbose, unsigned max_block_override) {
    side s[2];
    params p;
    if (!build(seed, max_block_override, s, &p)) {
        printf("  translator not available on this host\n");
        return -1;
    }
    char why[128];
    int fail = 0;
    const unsigned slices = 40 + below(40);
    for (unsigned n = 0; n < slices && !fail; n++) {
        int irq;
        const int64_t len = next_slice(&irq);
        const uint32_t pc_before = s[0].cpu->pc;
        if (irq >= 0) {
            m68k_set_irq(s[0].cpu, (unsigned)irq);
            m68k_set_irq(s[1].cpu, (unsigned)irq);
        }
        m68k_run(s[0].cpu, len);
        m68k_run(s[1].cpu, len);
        if (!same(&s[0], &s[1], why, sizeof why)) {
            fail = 1;
            printf("  case %llu (model %s%s, %s, %s writes, %s, idle %s, max block %u): slice %u of %lld from %08X: %s\n",
                   (unsigned long long)seed, m68k_model_name(p.model),
                   variant == 1 ? " + 68851" : variant == 2 ? (s[0].on24 ? " 24-bit" : " 32-bit") : "",
                   p.mmu ? (p.p8k ? "MMU 8K" : "MMU 4K") : "no MMU",
                   p.track ? "slow" : "fast", p.callout ? "call-out" : "leave", p.idle ? "on" : "off", p.max_block, n,
                   (long long)len, (unsigned)pc_before, why);
            if (verbose) {
                release(s);
                locate(seed, max_block_override, n);
                return 1;
            }
        }
        if (s[0].cpu->status == M68K_HALTED || s[0].cpu->status == M68K_UNIMPLEMENTED) break;
        maybe_switch(s);
    }
    release(s);
    return fail;
}

/* --- idle loops through traps ------------------------------------------------------------- */

/* A loop that calls a trap (A-line) each iteration, as an idle event loop
 * calls the System (the translator counts the repeating iterations of such
 * a loop at once), with parts that keep each iteration
 * the same as the last or not: a counter in memory or in a register, a
 * value written and written back, TAS of a set byte, MOVEM and exception
 * frames across a page end, interrupts masked and unmasked through SR, a
 * flag the harness sets between slices (a device's DMA), interrupts whose
 * handler counts in memory; with the MMU on and off, writes through the
 * fast path or the bus. Compared with the interpreter after every slice. */
#define TRAPH 0x9000u
#define IRQH 0x9100u
#define FLAG 0x7000u
#define TICKS 0x7004u

static side s_trap[2];

static unsigned put_words(uint8_t *image, uint32_t a, const uint16_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        image[a + 2 * i] = (uint8_t)(w[i] >> 8);
        image[a + 2 * i + 1] = (uint8_t)w[i];
    }
    return 2 * n;
}

static int trap_loop_case(uint64_t seed, uint64_t *skipped, int *repeats) {
    rng = seed * 0x2545F4914F6CDD1Dull + 7;
    static const m68k_model models[] = {M68K_68040, M68K_68040, M68K_68LC040, M68K_68030};
    params p;
    memset(&p, 0, sizeof p);
    p.model = models[below(4)];
    p.track = below(6) == 0;
    p.mmu = below(3) != 0;
    p.p8k = below(2);
    p.holes = below(5) == 0;
    p.callout = below(4) != 0;
    p.idle = below(5) != 0;
    uint8_t *image = calloc(RAM_SIZE, 1);
    for (unsigned v = 0; v < 256; v++) {
        const uint32_t h = v == 10 ? TRAPH : (v >= 25 && v <= 31) ? IRQH : HANDLER;
        image[4 * v] = (uint8_t)(h >> 24); image[4 * v + 1] = (uint8_t)(h >> 16);
        image[4 * v + 2] = (uint8_t)(h >> 8); image[4 * v + 3] = (uint8_t)h;
    }
    static const uint16_t handler[] = {0x54AF, 0x0002, 0x4E73};    /* ADDQ.L #2,2(A7); RTE */
    put_words(image, HANDLER, handler, 3);
    /* The trap's handler, as a dispatcher: the trap word read through the
     * stacked PC, the return past it. */
    static const uint16_t traph[] = {0x2F0A, 0x246F, 0x0006, 0x341A, 0x2F4A, 0x0006, 0x245F, 0x4E73};
    put_words(image, TRAPH, traph, 8);
    static const uint16_t irqh[] = {0x52B8, TICKS, 0x4E73};        /* ADDQ.L #1,TICKS.W; RTE */
    put_words(image, IRQH, irqh, 3);
    uint16_t w[64];
    unsigned n = 0;
    const int counter_mem = below(6) == 0, counter_reg = below(6) == 0, restore = below(2), flag = below(3) == 0,
              tas = below(3) == 0, movem = below(2), sr = below(2);
    *repeats = !counter_mem && !counter_reg && !p.track;
    if (flag) { w[n++] = 0x4A94; w[n++] = 0x6600; w[n++] = 0; }    /* TST.L (A4); BNE exit */
    const unsigned bne = flag ? n - 1 : 0;
    if (movem) { w[n++] = 0x48E7; w[n++] = 0xF000; }                /* MOVEM.L D0-D3,-(A7) */
    if (sr) { w[n++] = 0x40E7; w[n++] = 0x007C; w[n++] = 0x0700; }  /* MOVE SR,-(A7); ORI #$700,SR */
    w[n++] = 0x2F01;                                                /* MOVE.L D1,-(A7) */
    w[n++] = 0xA030;
    w[n++] = 0x221F;                                                /* MOVE.L (A7)+,D1 */
    if (sr) w[n++] = 0x46DF;                                        /* MOVE (A7)+,SR */
    if (restore) { w[n++] = 0x2613; w[n++] = 0x4693; w[n++] = 0x4693; }   /* MOVE.L (A3),D3; NOT.L (A3) twice */
    if (tas) w[n++] = 0x4AD3;                                       /* TAS (A3) */
    if (counter_mem) w[n++] = 0x5293;                               /* ADDQ.L #1,(A3) */
    if (counter_reg) w[n++] = 0x5284;                               /* ADDQ.L #1,D4 */
    if (movem) { w[n++] = 0x4CDF; w[n++] = 0x000F; }                /* MOVEM.L (A7)+,D0-D3 */
    w[n] = (uint16_t)(0x6000u | ((uint8_t)(-(int)(2 * n + 2))));   /* BRA.S loop */
    n++;
    const unsigned exit_at = n;
    w[n++] = 0x60FE;                                                /* exit: BRA.S * */
    if (flag) w[bne] = (uint16_t)(2 * exit_at - 2 * (bne - 1) - 2);
    put_words(image, CODE, w, n);
    const uint32_t data = DATA + 0x1000u * below(16) + (below(2) ? 0xFFEu : 4u * below(64));
    const uint32_t stack = STACK + 2u * below(12);
    image[data] = tas ? 0x80 : (uint8_t)rnd();
    setup(&s_trap[0], p.model, image, p.track, p.mmu, p.p8k, p.holes);
    setup(&s_trap[1], p.model, image, p.track, p.mmu, p.p8k, p.holes);
    free(image);
    side *s = s_trap;
    if (!m68k_jit_enable(s[1].cpu, 1)) { release(s); return -1; }
    m68k_jit_options(s[1].cpu, p.callout, p.idle, 0);
    m68k_jit_threshold(s[1].cpu, below(2) ? 0 : 32);
    for (unsigned k = 0; k < 2; k++) {
        m68k_cpu *c = s[k].cpu;
        for (unsigned r = 0; r < 8; r++) c->r[r] = 0x1111u * r;
        c->r[8 + 3] = data;
        c->r[8 + 4] = FLAG;
        m68k_set_sr(c, 0x2000);
        c->r[15] = stack;
        c->pc = CODE;
    }
    char why[128];
    int fail = 0;
    const unsigned slices = 30 + below(40);
    int raised = 0;
    for (unsigned i = 0; i < slices && !fail; i++) {
        const int64_t len = 1 + (int64_t)(below(3) == 0 ? below(64) : below(20000));
        if (raised) {
            m68k_set_irq(s[0].cpu, 0);
            m68k_set_irq(s[1].cpu, 0);
            raised = 0;
        } else if (below(10) == 0) {
            const unsigned level = 1 + below(7);
            m68k_set_irq(s[0].cpu, level);
            m68k_set_irq(s[1].cpu, level);
            raised = 1;
        }
        if (flag && below(25) == 0) {
            tb_put(&s[0].bus, FLAG, 4, 1);
            tb_put(&s[1].bus, FLAG, 4, 1);
        }
        m68k_run(s[0].cpu, len);
        m68k_run(s[1].cpu, len);
        if (!same(&s[0], &s[1], why, sizeof why)) {
            fail = 1;
            printf("  trap loop %llu (model %s, %s, %s writes, %s, idle %s; counters %d/%d, restore %d, flag %d, TAS %d, "
                   "MOVEM %d, SR %d, data %08X, stack %08X): slice %u of %lld: %s\n",
                   (unsigned long long)seed, m68k_model_name(p.model), p.mmu ? (p.p8k ? "MMU 8K" : "MMU 4K") : "no MMU",
                   p.track ? "slow" : "fast", p.callout ? "call-out" : "leave", p.idle ? "on" : "off", counter_mem,
                   counter_reg, restore, flag, tas, movem, sr, (unsigned)data, (unsigned)stack, i, (long long)len, why);
            dump(&s[0], "interpreter");
            dump(&s[1], "translator ");
        }
        if (s[0].cpu->status != M68K_OK) break;
    }
    m68k_jit_stats st;
    m68k_jit_get_stats(s[1].cpu, &st);
    *skipped = st.fast_forwarded;
    release(s);
    return fail;
}

static int jit_diff(unsigned cases, uint64_t seed0, unsigned max_block) {
    const char *how = timed ? " (timed: cycles)" : "";
    unsigned failed = 0;
    for (unsigned i = 0; i < cases; i++) {
        const int r = one_case(seed0 + i, failed < 3, max_block);
        if (r < 0) return 0;
        if (r) failed++;
    }
    printf("jit: %u random programs%s, %u differ from the interpreter\n", cases, how, failed);
    /* The 68020 with an MC68851, and on a 24-bit decode. */
    static const char *const sets[3] = {NULL, "on the 68020 with an MC68851", "on the 68020 switching 24/32-bit"};
    for (variant = 1; variant <= 2; variant++) {
        unsigned n = cases / 4 + 1, bad = 0;
        for (unsigned i = 0; i < n; i++) {
            const int r = one_case(seed0 + i, bad < 3, max_block);
            if (r < 0) return 0;
            if (r) bad++;
        }
        printf("jit: %u random programs %s%s, %u differ from the interpreter\n", n, sets[variant], how, bad);
        failed += bad;
    }
    variant = 0;
    full_ext = 1;
    {
        unsigned n = cases / 4 + 1, bad = 0;
        for (unsigned i = 0; i < n; i++) {
            const int r = one_case(seed0 + i, bad < 3, max_block);
            if (r < 0) return 0;
            if (r) bad++;
        }
        printf("jit: %u random programs with full-format extension words, MOVE16, SR writes, bit fields, long multiplies and divides and the FPU%s, %u differ from the interpreter\n", n, how, bad);
        failed += bad;
    }
    full_ext = 0;
    /* The idle loops through traps: the same, and counted at once where
     * they repeat. */
    unsigned loops = cases / 10 + 1, loop_failed = 0, repeating = 0, skipped_cases = 0;
    uint64_t skipped_all = 0;
    for (unsigned i = 0; i < loops; i++) {
        uint64_t skipped = 0;
        int repeats = 0;
        const int r = trap_loop_case(seed0 + i, &skipped, &repeats);
        if (r < 0) return 0;
        if (r) loop_failed++;
        repeating += (unsigned)repeats;
        if (repeats && skipped) skipped_cases++;
        skipped_all += skipped;
    }
    printf("jit: %u loops through a trap%s, %u differ from the interpreter; %u of the %u that repeat counted at once "
           "(%llu instructions)\n", loops, how, loop_failed, skipped_cases, repeating, (unsigned long long)skipped_all);
    if (repeating && !skipped_cases) {
        printf("jit: no repeating loop was counted at once\n");
        return 1;
    }
    return failed != 0 || loop_failed != 0;
}

/* Untimed (one count per instruction) and timed (each instruction its
 * cycles, m68k_timing.c): the same results, instruction and cycle counts. */
int mc_cpu_jit_diff(unsigned cases, uint64_t seed0, unsigned max_block) {
    if (!m68k_jit_available()) {
        printf("jit: no translator on this host; skipped\n");
        return 0;
    }
    int bad = 0;
    for (timed = 0; timed <= 1; timed++) bad |= jit_diff(cases, seed0, max_block);
    timed = 0;
    return bad;
}
