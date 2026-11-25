/* mc_ppc_jit_tests: the PowerPC translator (src/core/ppc/ppc_jit.c) against
 * the interpreter, on the PowerPC machine back end.
 *
 *   diff N SEED [MODEL|all [MAXBLOCK]]
 *                        N random programs for each model (or MODEL only),
 *                        run by both on a test board in random slices,
 *                        compared after every slice: the whole CPU state,
 *                        RAM, the device, the clock count, guest time and
 *                        the time base; translated blocks of at most
 *                        MAXBLOCK instructions (default 64)
 *   oracle FILE          the same for programs written by oracle.py's
 *                        generator (tests/ppc/oracle.py, program())
 *   rom [PATH]           pm-6100.rom on the PDM board to its first hardware
 *                        probe, compared every million clocks; MIPS of both
 *   speed                a CPU-bound loop: MIPS of both
 *
 * The random programs (Livadix's own; encodings from MPC601UM chapter 10 and
 * PEM chapter 8) mix integer, rotate, compare, CR and branch instructions,
 * loads and stores of every form and alignment, loops, calls, code that
 * rewrites itself (by stores and by a device's DMA), device reads with side
 * effects and guest time, interrupts, the time base and decrementer, system
 * calls, traps, user mode, floating point, strings and multiples,
 * reservations, and the MMU: real mode, BATs (rewritten), the hashed page
 * table with invalid, read-only and remapped pages, its entries rewritten
 * and a segment register changed, and the 603's software table reload. */
#include "../../src/hw/ppc/ppc_machine.h"
#include "ppc_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- the test board: RAM, ROM and one device -------------------------------------------- */

/* The device: +0 reads count (a side effect), +4 its interrupt line, +8
 * arms its event (value & $3FF + 1 periods on), +C reads guest time, +10
 * and +14 a word its event writes to RAM by DMA. Writes are hashed. */
typedef struct tick {
    mc_dev dev;
    mc_ppc *machine;
    uint32_t count, hash, dma_addr, dma_value;
    uint64_t at;
    int line;
} tick;

static uint32_t tick_read(void *p, uint32_t offset, unsigned size) {
    tick *t = p;
    (void)size;
    switch (offset & ~3u) {
    case 0x00: return ++t->count;
    case 0x04: return (uint32_t)t->line;
    case 0x08: return (uint32_t)t->at;
    case 0x0C: return (uint32_t)mc_dev_now(&t->dev);
    default: return t->hash;
    }
}
static void tick_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    tick *t = p;
    t->hash = t->hash * 31u + (offset ^ value ^ size);
    switch (offset & ~3u) {
    case 0x04:
        t->line = (int)(value & 1u);
        mc_dev_line(&t->dev, 0, t->line);
        break;
    case 0x08:
        t->at = mc_dev_now(&t->dev) + (value & 0x3FFu) + 1u;
        mc_dev_schedule(&t->dev, t->at);
        break;
    case 0x18: ppc_reset(&t->machine->cpu); ppc_jit_cpu_reset(t->machine->jit); break;
    case 0x10: t->dma_addr = value; break;
    case 0x14: t->dma_value = value; break;
    default: break;
    }
}
static void tick_event(void *p, uint64_t now) {
    tick *t = p;
    (void)now;
    if (t->dma_addr) {
        uint8_t b[4] = {(uint8_t)(t->dma_value >> 24), (uint8_t)(t->dma_value >> 16), (uint8_t)(t->dma_value >> 8), (uint8_t)t->dma_value};
        mc_dev_dma(&t->dev, t->dma_addr, b, 4, 1);
        t->dma_addr = 0;
    }
    t->line = 1;
    mc_dev_line(&t->dev, 0, 1);
}
static const mc_dev_ops tick_ops = {tick_read, tick_write, tick_event, NULL, NULL};

typedef struct board { mc_ppc *m; tick t; } board;

static void *build(mc_ppc *m) {
    board *b = calloc(1, sizeof *b);
    if (!b) return NULL;
    b->m = m;
    b->t.machine = m;
    b->t.dev.ops = &tick_ops;
    b->t.dev.host = &m->host;
    b->t.dev.name = "tick";
    if (!mc_ppc_attach(m, mc_ppc_region(m, "io"), 0, 0x100, &b->t, &tick_ops, (mc_ppc_port){MC_PPC_PORT_DIRECT, 4, 0, 0})) {
        free(b);
        return NULL;
    }
    return b;
}
static void line(void *p, void *dev, int which, int state) {
    board *b = p;
    (void)dev; (void)which;
    mc_ppc_set_interrupt(b->m, state);
}
static void destroy(void *p) { free(p); }
static const mc_board_ops board_ops = {.line = line, .destroy = destroy};

#define ROM_ID 0x4A495431u
static const uint32_t test_roms[] = {ROM_ID, 0};
static const uint16_t test_ram[] = {1, 0};
static const mc_region test_map[] = {
    {"ram", 0x00000000u, 0x10000000u, 0, MC_REGION_RAM},
    {"io", 0xF0000000u, 0x00010000u, 0x0000FFFFu, MC_REGION_IO},
    {"rom", 0xFFF00000u, 0x00100000u, 0, MC_REGION_ROM},
};
static mc_ppc_desc board_desc = {
    .key = "jit-test", .name = "Translator test board", .cpu = PPC_MODEL_601, .cpu_hz = 60000000u,
    .timebase_hz = 7812500u, .cpi = 1, .roms = test_roms, .ram_mb = test_ram, .map = test_map, .nmap = 3,
    .unmapped = MC_PPC_UNMAPPED_TEA, .build = build, .ops = &board_ops,
};
static uint8_t rom[65536];

#define RAM_SIZE (1u << 20)

/* The CPU state a program starts from. */
typedef struct start {
    uint32_t pc, msr, gpr[32], cr, xer, lr, ctr, dec, sdr1, sr[16], bat[4][2], dbat[4][2];
    uint64_t fpr[32];
} start;

static unsigned max_block;      /* translator blocks of at most this (0: its default) */

static mc_ppc *machine(int jit) {
    mc_config c;
    char error[256] = "";
    mc_config_defaults(&c);
    mc_config_set_ram_kb(&c, 1024);
    c.jit = jit;
    for (int i = 0; i < 4; i++) rom[i] = (uint8_t)((uint32_t)ROM_ID >> (24 - 8 * i) & 0xFFu);
    mc_ppc *m = mc_ppc_create(&board_desc, &c, rom, sizeof rom, error, sizeof error);
    if (!m) fprintf(stderr, "create: %s\n", error);
    else if (m->jit && max_block) ppc_jit_options(m->jit, max_block);
    return m;
}

static void load(mc_ppc *m, const uint8_t *image, const start *s) {
    ppc_cpu *c = &m->cpu;
    memcpy(m->ram, image, RAM_SIZE);
    c->pc = s->pc;
    c->msr = s->msr;
    memcpy(c->gpr, s->gpr, sizeof c->gpr);
    memcpy(c->fpr, s->fpr, sizeof c->fpr);
    c->cr = s->cr; c->xer = s->xer; c->lr = s->lr; c->ctr = s->ctr; c->dec = s->dec; c->sdr1 = s->sdr1;
    memcpy(c->sr, s->sr, sizeof c->sr);
    memcpy(c->bat, s->bat, sizeof c->bat);
    memcpy(c->dbat, s->dbat, sizeof c->dbat);
}

/* The first difference between the two machines, or NULL. */
static const char *compare(mc_ppc *a, mc_ppc *b, char *why, size_t n) {
    ppc_cpu x = a->cpu, y = b->cpu;
    memset(&x.bus, 0, sizeof x.bus);
    memset(&y.bus, 0, sizeof y.bus);
    x.mmu_wim = y.mmu_wim = 0;      /* the interpreter's scratch of one access */
#define FIELD(f) if (x.f != y.f) { snprintf(why, n, #f ": interpreter %08llX, translator %08llX", (unsigned long long)x.f, (unsigned long long)y.f); return why; }
    FIELD(pc) FIELD(msr) FIELD(cr) FIELD(xer) FIELD(lr) FIELD(ctr) FIELD(mq) FIELD(fpscr) FIELD(instructions)
    for (int i = 0; i < 32; i++) {
        if (x.gpr[i] != y.gpr[i]) { snprintf(why, n, "r%d: interpreter %08X, translator %08X", i, (unsigned)x.gpr[i], (unsigned)y.gpr[i]); return why; }
        if (x.fpr[i] != y.fpr[i]) { snprintf(why, n, "f%d: interpreter %016llX, translator %016llX", i, (unsigned long long)x.fpr[i], (unsigned long long)y.fpr[i]); return why; }
    }
    FIELD(srr0) FIELD(srr1) FIELD(dar) FIELD(dsisr) FIELD(dec) FIELD(rtcu) FIELD(rtcl) FIELD(time_base)
    FIELD(last_pc) FIELD(last_opcode) FIELD(last_exception) FIELD(result) FIELD(reserved) FIELD(reservation)
    FIELD(external_irq) FIELD(decrementer_pending) FIELD(exceptions) FIELD(tlb_writes)
    FIELD(imiss) FIELD(icmp) FIELD(dmiss) FIELD(dcmp) FIELD(hash1) FIELD(hash2) FIELD(rpa) FIELD(sdr1)
#undef FIELD
    for (int w = 0; w < 2; w++)
        for (int i = 0; i < 32; i++) {
            if (x.tlb_lru[w][i] != y.tlb_lru[w][i]) {
                snprintf(why, n, "%s TLB LRU %d: interpreter %u, translator %u", w ? "data" : "instruction", i, x.tlb_lru[w][i], y.tlb_lru[w][i]);
                return why;
            }
            for (int k = 0; k < 2; k++)
                if (memcmp(&x.tlb[w][i][k], &y.tlb[w][i][k], sizeof x.tlb[w][i][k])) {
                    snprintf(why, n, "%s TLB %d way %d", w ? "data" : "instruction", i, k);
                    return why;
                }
        }
    if (memcmp(&x, &y, sizeof x)) { snprintf(why, n, "other CPU state (SPRs, TLB)"); return why; }
    if (memcmp(a->ram, b->ram, a->ram_size)) {
        uint32_t i = 0;
        while (a->ram[i] == b->ram[i]) i++;
        snprintf(why, n, "RAM at %06X: interpreter %02X, translator %02X", (unsigned)i, a->ram[i], b->ram[i]);
        return why;
    }
    if (a->cycles != b->cycles || mc_ppc_now(a) != mc_ppc_now(b) || a->tb_ticks != b->tb_ticks) {
        snprintf(why, n, "clocks %llu/%llu, time %llu/%llu", (unsigned long long)a->cycles, (unsigned long long)b->cycles,
                 (unsigned long long)mc_ppc_now(a), (unsigned long long)mc_ppc_now(b));
        return why;
    }
    if (a->desc == &board_desc && a->board && b->board) {
        const tick *ta = &((board *)a->board)->t, *tb = &((board *)b->board)->t;
        if (ta->count != tb->count || ta->hash != tb->hash || ta->at != tb->at || ta->line != tb->line || ta->dma_addr != tb->dma_addr) {
            snprintf(why, n, "device: reads %u/%u, writes %08X/%08X", ta->count, tb->count, ta->hash, tb->hash);
            return why;
        }
    }
    if (a->failed != b->failed || strcmp(a->error, b->error)) {
        snprintf(why, n, "run: \"%s\" / \"%s\"", a->error, b->error);
        return why;
    }
    return NULL;
}

/* --- random programs ------------------------------------------------------------------- */

static uint64_t rng_state = 1;
static uint32_t rnd(uint32_t n) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return n ? (uint32_t)((rng_state >> 11) % n) : 0;
}
static uint32_t rnd32(void) { return rnd(65536) << 16 | rnd(65536); }

static uint32_t D(unsigned op, unsigned t, unsigned a, uint32_t imm) { return op << 26 | (t & 31u) << 21 | (a & 31u) << 16 | (imm & 0xFFFFu); }
static uint32_t X(unsigned xo, unsigned t, unsigned a, unsigned b, int rc) { return 31u << 26 | (t & 31u) << 21 | (a & 31u) << 16 | (b & 31u) << 11 | xo << 1 | (rc ? 1u : 0u); }
static uint32_t SPR(unsigned xo, unsigned r, unsigned spr) { return X(xo, r, spr & 31u, spr >> 5, 0); }
static uint32_t RLW(unsigned op, unsigned s, unsigned a, unsigned sh, unsigned mb, unsigned me, int rc) {
    return op << 26 | s << 21 | a << 16 | sh << 11 | mb << 6 | me << 1 | (rc ? 1u : 0u);
}
static uint32_t BC(unsigned bo, unsigned bi, int32_t disp, int lk) { return 16u << 26 | bo << 21 | bi << 16 | ((uint32_t)disp & 0xFFFCu) | (lk ? 1u : 0u); }
static uint32_t B(int32_t disp, int lk) { return 18u << 26 | ((uint32_t)disp & 0x03FFFFFCu) | (lk ? 1u : 0u); }
#define MTSPR(spr, r) SPR(467, r, spr)
#define MFSPR(r, spr) SPR(339, r, spr)
#define RFI 0x4C000064u
#define SC 0x44000002u
#define BLR 0x4E800020u
#define SYNC 0x7C0004ACu
#define ISYNC 0x4C00012Cu

enum { M_REAL, M_BAT, M_TABLE, M_SOFT };
enum { CODE = 0x10000, CODE_END = 0x2E000, SUBS = 0x30000, DATA = 0x40000, ALIAS = 0x50000, INVALID = 0x51000,
       READONLY = 0x52000, ALIAS_A = 0x58000, ALIAS_B = 0x59000, HTAB = 0x80000, SCRATCH = 0x3000, NSUBS = 8 };

typedef struct gctx {
    uint8_t *ram;
    uint32_t at, subs[NSUBS];
    ppc_model model;
    int mmu, depth, user;
    uint32_t alias_pte, data_pte, dec_rearm, sr6;
} gctx;

static void poke32(uint8_t *ram, uint32_t a, uint32_t v) {
    ram[a] = (uint8_t)(v >> 24); ram[a + 1] = (uint8_t)(v >> 16); ram[a + 2] = (uint8_t)(v >> 8); ram[a + 3] = (uint8_t)v;
}
static void put(gctx *g, uint32_t word) { poke32(g->ram, g->at, word); g->at += 4; }
static void li32(gctx *g, unsigned r, uint32_t v) { put(g, D(15, r, 0, v >> 16)); put(g, D(24, r, r, v)); }
static unsigned dest(void) { unsigned r = rnd(27); return r ? r + 2u : 0u; }      /* r0, r3-r28 */
static unsigned src(void) { return rnd(32); }

/* Exception handlers (real mode); they save r3, r4 in SPRG0, SPRG1. */
static void vectors(gctx *g) {
    const uint32_t skip[] = {MTSPR(272, 3), MFSPR(3, 26), D(14, 3, 3, 4), MTSPR(26, 3), MFSPR(3, 272), RFI};
    for (uint32_t v = 0x100; v < 0x3000; v += 0x100) {
        g->at = v;
        put(g, RFI);
    }
    g->at = 0x200;                                  /* machine check: skip */
    for (unsigned i = 0; i < 6; i++) put(g, skip[i]);
    g->at = 0x300;                                  /* DSI: DAR, DSISR kept; skip */
    put(g, MTSPR(272, 3)); put(g, MFSPR(3, 19)); put(g, D(36, 3, 0, SCRATCH)); put(g, MFSPR(3, 18)); put(g, D(36, 3, 0, SCRATCH + 4));
    for (unsigned i = 1; i < 6; i++) put(g, skip[i]);
    g->at = 0x400;                                  /* ISI: return to LR */
    put(g, MTSPR(272, 3)); put(g, MFSPR(3, 8)); put(g, MTSPR(26, 3)); put(g, MFSPR(3, 272)); put(g, RFI);
    g->at = 0x500;                                  /* external: acknowledge the device, count */
    put(g, MTSPR(272, 3)); put(g, MTSPR(273, 4));
    put(g, D(15, 3, 0, 0xF000)); put(g, D(14, 4, 0, 0)); put(g, D(36, 4, 3, 4));
    put(g, D(32, 4, 0, SCRATCH + 8)); put(g, D(14, 4, 4, 1)); put(g, D(36, 4, 0, SCRATCH + 8));
    put(g, MFSPR(4, 273)); put(g, MFSPR(3, 272)); put(g, RFI);
    g->at = 0x600;                                  /* alignment: DAR kept; skip */
    put(g, MTSPR(272, 3)); put(g, MFSPR(3, 19)); put(g, D(36, 3, 0, SCRATCH + 12));
    for (unsigned i = 1; i < 6; i++) put(g, skip[i]);
    g->at = 0x700;                                  /* program: skip */
    for (unsigned i = 0; i < 6; i++) put(g, skip[i]);
    g->at = 0x800;                                  /* FPU unavailable: MSR[FP] on, again */
    put(g, MTSPR(272, 3)); put(g, MFSPR(3, 27)); put(g, D(24, 3, 3, 0x2000)); put(g, MTSPR(27, 3)); put(g, MFSPR(3, 272)); put(g, RFI);
    g->at = 0x900;                                  /* decrementer: rearm, count */
    put(g, MTSPR(272, 3)); li32(g, 3, g->dec_rearm); put(g, MTSPR(22, 3));
    put(g, D(32, 3, 0, SCRATCH + 16)); put(g, D(14, 3, 3, 1)); put(g, D(36, 3, 0, SCRATCH + 16));
    put(g, MFSPR(3, 272)); put(g, RFI);
    g->at = 0xC00;                                  /* system call: back in supervisor state */
    put(g, MTSPR(272, 3)); put(g, MFSPR(3, 27)); put(g, RLW(21, 3, 3, 0, 18, 16, 0));
    put(g, MTSPR(27, 3)); put(g, MFSPR(3, 272)); put(g, RFI);
    if (g->mmu == M_SOFT) {
        /* 603 TLB misses (MPC603eUM 5), r0-r3 the shadow GPRs: identity,
         * the alias page moved, the read-only page PP=3, R and C set. */
        g->at = 0x1000;
        put(g, MFSPR(0, 980)); put(g, RLW(21, 0, 1, 0, 0, 19, 0)); put(g, D(24, 1, 1, 0x182));
        put(g, MTSPR(982, 1)); put(g, X(1010, 0, 0, 0, 0)); put(g, RFI);
        for (uint32_t v = 0x1100; v <= 0x1200; v += 0x100) {
            g->at = v;
            put(g, MFSPR(0, 976)); put(g, X(19, 3, 0, 0, 0));
            put(g, RLW(21, 0, 1, 0, 0, 19, 0)); put(g, RLW(21, 0, 2, 20, 12, 31, 0));
            put(g, D(10, 0, 2, ALIAS >> 12)); put(g, BC(4, 2, 8, 0)); put(g, D(24, 1, 1, ALIAS_A - ALIAS));
            put(g, D(24, 1, 1, 0x182));
            put(g, D(10, 0, 2, READONLY >> 12)); put(g, BC(4, 2, 8, 0)); put(g, D(24, 1, 1, 0x183));
            put(g, MTSPR(982, 1)); put(g, X(144, 3, 0, 0, 0) | 0xFFu << 12); put(g, X(978, 0, 0, 0, 0)); put(g, RFI);
        }
    }
}

/* The hashed page table (MPC601UM 6.9, PEM 7.6): ea -> pa, R and C clear. */
static uint32_t pte(gctx *g, const start *s, uint32_t ea, uint32_t pa, unsigned pp) {
    const uint32_t vsid = s->sr[ea >> 28] & 0xFFFFFFu, index = (ea >> 12) & 0xFFFFu;
    const uint32_t mask = ((s->sdr1 & 0x1FFu) << 10) | 0x3FFu;
    for (unsigned h = 0; h < 2; h++) {
        const uint32_t hash = h ? ~(vsid ^ index) : vsid ^ index;
        const uint32_t group = (s->sdr1 & 0xFFFF0000u) | ((hash & mask) << 6);
        for (unsigned i = 0; i < 8; i++) {
            const uint32_t a = group + 8u * i;
            if (g->ram[a] & 0x80u) continue;
            poke32(g->ram, a, 0x80000000u | vsid << 7 | h << 6 | (ea >> 22 & 0x3Fu));
            poke32(g->ram, a + 4, (pa & 0xFFFFF000u) | pp);
            return a + 4;
        }
    }
    return 0;
}

static void setup_mmu(gctx *g, start *s) {
    const int six = g->model == PPC_MODEL_601;
    s->msr = PPC_MSR_ME | PPC_MSR_FP | (rnd(2) ? PPC_MSR_EE : 0);
    for (int i = 0; i < 16; i++) s->sr[i] = 0x00000100u + (uint32_t)i * 0x111u;
    g->sr6 = s->sr[6];
    if (g->mmu == M_REAL) return;
    s->msr |= PPC_MSR_IR | PPC_MSR_DR;
    /* the device through BAT 3 */
    if (six) { s->bat[3][0] = 0xF0000002u; s->bat[3][1] = 0xF0000040u; }
    else { s->dbat[3][0] = 0xF0000003u; s->dbat[3][1] = 0xF000002Au; }
    if (g->mmu == M_BAT) {
        if (six) {
            s->bat[0][0] = 0x00000002u; s->bat[0][1] = 0x00000047u;     /* 1 MB identity */
            s->bat[2][0] = 0x10000002u; s->bat[2][1] = 0x00040040u;     /* $10000000 -> $40000, 128 KB */
        } else {
            s->bat[0][0] = s->dbat[0][0] = 0x0000001Fu;
            s->bat[0][1] = s->dbat[0][1] = 0x00000002u;
            s->dbat[2][0] = 0x10000003u; s->dbat[2][1] = 0x00040002u;
        }
        return;
    }
    if (g->mmu == M_SOFT) return;
    s->sdr1 = HTAB;
    for (uint32_t a = 0; a < 0x90000u; a += 0x1000u) {
        if (a == INVALID) continue;
        const uint32_t p = pte(g, s, a, a == ALIAS ? ALIAS_A : a, a == READONLY ? 3u : 2u);
        if (a == ALIAS) g->alias_pte = p;
        if (a == DATA) g->data_pte = p;
    }
    /* segment 6: two VSIDs map $60040000 to different pages */
    pte(g, s, 0x60040000u, DATA, 2);
    {
        start t = *s;
        t.sr[6] = 0x00777777u;
        pte(g, &t, 0x60040000u, DATA + 0x1000u, 2);
    }
}

static void item(gctx *g);

static uint32_t alu_word(void) {
    static const unsigned arith[] = {8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491};
    static const unsigned logic[] = {11, 75, 24, 26, 28, 60, 124, 284, 316, 412, 444, 476, 536, 792, 824, 922, 954};
    static const unsigned dops[] = {7, 8, 12, 13, 14, 15};
    static const unsigned rops[] = {20, 21, 23};
    static const unsigned xl[] = {33, 129, 193, 225, 257, 289, 417, 449, 0};
    const unsigned t = dest(), a = src(), b = src();
    const int rc = (int)rnd(2);
    switch (rnd(9)) {
    case 0: return D(dops[rnd(6)], t, a, rnd(65536));
    case 1: return D(24 + rnd(6), a, t, rnd(65536));
    case 2: {
        const unsigned xo = arith[rnd(14)];
        const int no_b = xo == 104 || xo == 200 || xo == 202 || xo == 232 || xo == 234;
        return X(xo + (rnd(2) ? 512u : 0u), t, a, no_b ? 0 : b, rc);
    }
    case 3: {
        const unsigned xo = logic[rnd(17)];
        return xo == 11 || xo == 75 ? X(xo, t, a, b, rc) : X(xo, a, t, b, rc);
    }
    case 4: return RLW(rops[rnd(3)], a, t, b, rnd(32), rnd(32), rc);
    case 5: return rnd(2) ? X(rnd(2) ? 0 : 32, rnd(8) << 2, a, b, 0) : D(rnd(2) ? 10 : 11, rnd(8) << 2, a, rnd(65536));
    case 6: return 19u << 26 | rnd(32) << 21 | rnd(32) << 16 | rnd(32) << 11 | xl[rnd(9)] << 1;
    case 7:
        switch (rnd(7)) {
        case 0: return X(19, t, 0, 0, 0);                                   /* mfcr */
        case 1: return X(144, a, 0, 0, 0) | rnd(256) << 12;                 /* mtcrf */
        case 2: return MTSPR(1, a);
        case 3: return MFSPR(t, 1);
        case 4: return X(512, rnd(8) << 2, 0, 0, 0);                        /* mcrxr */
        case 5: return MTSPR(8, a);
        default: return MFSPR(t, rnd(2) ? 8 : 9);
        }
    default: return X(444, a, t, a, 0);                                     /* mr */
    }
}

static void alu(gctx *g) { put(g, alu_word()); }

/* Loads and stores (MPC601UM 10; PEM 8): every form, size and alignment,
 * in the data page, at page ends, in the special pages, in a code page. */
static void mem(gctx *g) {
    static const struct { unsigned op, size, fp, store; } dform[] = {
        {32, 4, 0, 0}, {34, 1, 0, 0}, {40, 2, 0, 0}, {42, 2, 0, 0}, {36, 4, 0, 1}, {38, 1, 0, 1}, {44, 2, 0, 1},
        {48, 4, 1, 0}, {50, 8, 1, 0}, {52, 4, 1, 1}, {54, 8, 1, 1}};
    static const struct { unsigned xo, size, fp, update; } xform[] = {
        {23, 4, 0, 1}, {87, 1, 0, 1}, {279, 2, 0, 1}, {343, 2, 0, 1}, {151, 4, 0, 1}, {215, 1, 0, 1}, {407, 2, 0, 1},
        {534, 4, 0, 0}, {790, 2, 0, 0}, {662, 4, 0, 0}, {918, 2, 0, 0}, {535, 4, 1, 1}, {599, 8, 1, 1}, {663, 4, 1, 1}, {727, 8, 1, 1}};
    uint32_t base, off;
    const unsigned r = rnd(12);
    if (r < 7) base = DATA;
    else if (r == 7) base = DATA + 0x1000u;
    else if (r == 8) base = rnd(2) ? SUBS : SCRATCH;
    else if (g->mmu == M_TABLE && r == 9) base = 0x60040000u;
    else if (g->mmu == M_BAT && r == 9) base = 0x10000000u;
    else base = rnd(2) ? ALIAS : rnd(2) ? READONLY : INVALID;
    if (base == SUBS) off = 0xF00u + rnd(0xF8);
    else if (base == SCRATCH) off = 0x100u + rnd(0xE00);
    else if (rnd(5) == 0) off = 0xFF0u + rnd(16);                            /* the page's end */
    else off = rnd(0xFF8);
    if (base == DATA + 0x1000u) base = DATA, off = 0xFF8u + rnd(8);
    if (rnd(4)) {
        const unsigned i = rnd(11);
        const unsigned t = dform[i].fp ? rnd(32) : dform[i].store ? src() : dest();
        const int update = rnd(4) == 0;
        if (rnd(6)) off &= ~(dform[i].size - 1u);
        li32(g, 29, base);
        put(g, D(dform[i].op | (update ? 1u : 0u), t, 29, off));
    } else {
        const unsigned i = rnd(15);
        const int update = xform[i].update && rnd(3) == 0;
        const int store = xform[i].xo == 151 || xform[i].xo == 215 || xform[i].xo == 407 || xform[i].xo == 662 ||
                          xform[i].xo == 918 || xform[i].xo == 663 || xform[i].xo == 727;
        const unsigned t = xform[i].fp ? rnd(32) : store ? src() : dest();
        if (rnd(6)) off &= ~(xform[i].size - 1u);
        li32(g, 29, base);
        put(g, D(14, 31, 0, off));
        put(g, X(xform[i].xo + (update ? 32u : 0u), t, 29, 31, 0));
    }
}

static void branch(gctx *g) {
    static const unsigned plain[] = {4, 12, 20, 4, 12};
    static const unsigned counting[] = {0, 2, 8, 10, 16, 18};
    const unsigned bo = g->depth || rnd(3) ? plain[rnd(5)] : counting[rnd(6)];
    switch (rnd(4)) {
    case 0: case 1:                                 /* bc over one instruction */
        put(g, BC(bo, rnd(32), 8, rnd(4) == 0));
        alu(g);
        break;
    case 2: {                                       /* bclr over one instruction */
        put(g, B(4, 1));
        put(g, MFSPR(31, 8)); put(g, D(14, 31, 31, 20)); put(g, MTSPR(8, 31));
        put(g, 19u << 26 | bo << 21 | rnd(32) << 16 | 16u << 1 | (rnd(4) == 0 ? 1u : 0u));
        alu(g);
        break;
    }
    default:
        if (g->depth) { put(g, B(8, 0)); alu(g); break; }
        {                                           /* bcctr over one instruction */
            const uint32_t at = g->at;
            li32(g, 31, at + 20);
            put(g, MTSPR(9, 31));
            put(g, 19u << 26 | (rnd(2) ? 20u : 4u + 8u * rnd(2)) << 21 | rnd(32) << 16 | 528u << 1 | (rnd(4) == 0 ? 1u : 0u));
            alu(g);
        }
        break;
    }
}

static void call(gctx *g) {
    const uint32_t sub = g->subs[rnd(NSUBS)];
    if (g->depth || rnd(2)) put(g, B((int32_t)(sub - g->at), 1));
    else {
        li32(g, 31, sub);
        put(g, MTSPR(9, 31));
        put(g, 0x4E800421u);                        /* bctrl */
    }
}

static void loop(gctx *g) {
    const unsigned n = 1 + rnd(12), body = 1 + rnd(5);
    uint32_t top;
    put(g, D(14, 30, 0, n));
    put(g, MTSPR(9, 30));
    top = g->at;
    g->depth++;
    for (unsigned i = 0; i < body; i++) item(g);
    g->depth--;
    put(g, BC(16, 0, (int32_t)(top - g->at), 0));    /* bdnz */
}

/* A loop that rewrites the instruction it is about to run, in its own block. */
static void smc_loop(gctx *g) {
    const unsigned n = 1 + rnd(10);
    const uint32_t origin = g->at;
    put(g, D(14, 30, 0, n));
    put(g, MTSPR(9, 30));
    li32(g, 29, D(14, 5, 5, 0));
    li32(g, 31, origin + 32);
    put(g, D(14, 29, 29, 1));
    put(g, D(36, 29, 31, 0));
    put(g, D(14, 5, 5, 0));                         /* rewritten */
    alu(g);
    put(g, BC(16, 0, -16, 0));
}

/* A store to an instruction a few words on (maybe already translated). */
static void smc_ahead(gctx *g) {
    const unsigned gap = rnd(4);
    const uint32_t slot = g->at + 20u + 4u * gap;
    li32(g, 29, alu_word());
    li32(g, 31, slot);
    put(g, D(36, 29, 31, 0));
    for (unsigned i = 0; i < gap; i++) alu(g);
    put(g, D(14, 6, 6, 1));                         /* rewritten */
}

/* The device writes an instruction by DMA at its event. */
static void dma_patch(gctx *g) {
    const unsigned gap = 2 + rnd(6);
    const uint32_t slot = g->at + 4u * (2 + 1 + 2 + 1 + 1 + 1) + 4u * gap;
    li32(g, 31, slot); put(g, D(36, 31, 2, 0x10));
    li32(g, 31, alu_word()); put(g, D(36, 31, 2, 0x14));
    put(g, D(14, 31, 0, (uint32_t)rnd(64)));
    put(g, D(36, 31, 2, 8));
    for (unsigned i = 0; i < gap; i++) alu(g);
    put(g, D(14, 7, 7, 1));
}

static void device(gctx *g) {
    switch (rnd(7)) {
    case 0: put(g, D(32, dest(), 2, 0)); break;
    case 1: put(g, D(32, dest(), 2, 0x0C)); break;
    case 2: put(g, D(34, dest(), 2, 0)); break;
    case 3: put(g, D(14, 31, 0, rnd(0x200))); put(g, D(36, 31, 2, 8)); break;
    case 4: put(g, D(36, src(), 2, 0x20)); break;
    case 5: put(g, D(14, 31, 0, 1)); put(g, D(36, 31, 2, 4)); break;
    default: put(g, D(40, dest(), 2, 0x0E)); break;
    }
}

static void timebase(gctx *g) {
    switch (rnd(6)) {
    case 0: put(g, MFSPR(dest(), 22)); break;
    case 1: put(g, X(371, dest(), 12, 8, 0)); break;                       /* mftb */
    case 2: put(g, MFSPR(dest(), rnd(2) ? 4 : 5)); break;                  /* the 601's RTC */
    case 3: put(g, MFSPR(dest(), rnd(2) ? 268 : 269)); break;
    case 4: li32(g, 31, g->model == PPC_MODEL_601 ? 0x1000u + rnd(0x40000) : 50u + rnd(3000)); put(g, MTSPR(22, 31)); break;
    default: put(g, MFSPR(dest(), 6)); break;
    }
}

static void msr_bits(gctx *g) {
    put(g, X(83, 31, 0, 0, 0));
    switch (rnd(3)) {
    case 0: put(g, D(24, 31, 31, 0x8000)); break;                           /* EE on */
    case 1: put(g, RLW(21, 31, 31, 0, 17, 15, 0)); break;                   /* EE off */
    default: put(g, RLW(21, 31, 31, 0, 19, 17, 0)); break;                  /* FP off */
    }
    put(g, X(146, 31, 0, 0, 0));
}

static void user(gctx *g) {
    put(g, X(83, 31, 0, 0, 0));
    put(g, D(24, 31, 31, 0x4000));
    put(g, X(146, 31, 0, 0, 0));
    g->user = 1;
    for (unsigned i = 1 + rnd(6); i; i--) item(g);
    g->user = 0;
    put(g, SC);
}

static void fpu(gctx *g) {
    static const unsigned a_ops[] = {18, 20, 21, 25, 28, 29, 30, 31, 23};
    static const unsigned x_ops[] = {72, 40, 264, 136, 12, 14, 15, 0, 583, 70, 38};
    const unsigned t = rnd(32), a = rnd(32), b = rnd(32), c = rnd(32);
    const int rc = (int)rnd(2);
    if (rnd(2)) {
        const unsigned xo = a_ops[rnd(g->model == PPC_MODEL_601 ? 8 : 9)];
        put(g, (rnd(3) ? 63u : 59u) << 26 | t << 21 | a << 16 | (xo == 25 ? 0 : b) << 11 | (xo >= 23 ? c : 0) << 6 | xo << 1 | (unsigned)rc);
    } else {
        const unsigned xo = x_ops[rnd(11)];
        if (xo == 0) put(g, 63u << 26 | rnd(8) << 23 | a << 16 | b << 11);
        else if (xo == 70 || xo == 38) put(g, 63u << 26 | rnd(32) << 21 | xo << 1);   /* mtfsb0/1 */
        else put(g, 63u << 26 | t << 21 | b << 11 | xo << 1 | (unsigned)rc);
    }
}

static void mmu_item(gctx *g) {
    const int six = g->model == PPC_MODEL_601;
    switch (g->mmu) {
    case M_TABLE:
        if (rnd(5) == 0) {                          /* a call into the unmapped page: ISI, back at LR */
            put(g, B((int32_t)(INVALID + 4u * rnd(1024) - g->at), 1));
            break;
        }
        switch (rnd(4)) {
        case 0:                                     /* the alias page's PTE moved */
            li32(g, 29, g->alias_pte);
            li32(g, 31, (rnd(2) ? ALIAS_A : ALIAS_B) | 2u);
            put(g, D(36, 31, 29, 0));
            if (rnd(2)) { li32(g, 31, ALIAS); put(g, SYNC); put(g, X(306, 0, 0, 31, 0)); put(g, SYNC); }
            break;
        case 1:                                     /* R and C cleared */
            li32(g, 29, g->data_pte);
            li32(g, 31, DATA | 2u);
            put(g, D(36, 31, 29, 0));
            break;
        case 2:                                     /* segment 6's VSID */
            li32(g, 31, rnd(2) ? 0x00777777u : g->sr6);
            put(g, X(210, 31, 6, 0, 0));
            put(g, ISYNC);
            break;
        default:
            li32(g, 29, 0x60040000u);
            put(g, D(rnd(2) ? 32 : 36, rnd(2) ? dest() : src(), 29, rnd(0x400) * 4u));
            break;
        }
        break;
    case M_BAT:                                     /* BAT 2's block moved */
        li32(g, 31, (rnd(2) ? 0x40000u : 0x60000u) | (six ? 0x40u : 0x02u));
        put(g, MTSPR(six ? 533u : 541u, 31));
        put(g, ISYNC);
        break;
    default:
        alu(g);
        break;
    }
}

static void misc(gctx *g) {
    switch (rnd(8)) {
    case 0: li32(g, 29, DATA + rnd(0x1000)); put(g, X(1014, 0, 0, 29, 0)); break;                 /* dcbz */
    case 1: put(g, D(14, 31, 0, rnd(0x400) * 4u)); put(g, X(20, dest(), 1, 31, 0)); put(g, X(150, src(), 1, 31, 1)); break;
    case 2: put(g, D(rnd(2) ? 46 : 47, 27, 1, rnd(0x3F0) * 4u + (rnd(8) ? 0 : 2u))); break;       /* lmw, stmw */
    case 3:
        if (rnd(3)) { put(g, X(rnd(2) ? 597 : 725, 27, 1, 1 + rnd(20), 0)); break; }             /* lswi, stswi */
        put(g, D(14, 30, 0, rnd(21))); put(g, MTSPR(1, 30));                                      /* lswx, stswx */
        put(g, D(14, 31, 0, rnd(0x3F0) * 4u + rnd(4))); put(g, X(rnd(2) ? 533 : 661, 27, 1, 31, 0));
        break;
    case 4: put(g, X(4, rnd(2) ? 31 : rnd(32), src(), src(), 0)); break;                          /* tw */
    case 5: put(g, D(3, rnd(32), src(), rnd(65536))); break;                                      /* twi */
    case 6: put(g, rnd(2) ? SYNC : rnd(2) ? ISYNC : rnd(2) ? X(83, dest(), 0, 0, 0) : X(rnd(2) ? 86 : 982, 0, 1, 31, 0)); break;
    default: li32(g, 29, 0x70000000u); put(g, D(32, dest(), 29, 0)); break;                      /* nothing there */
    }
}

static void item(gctx *g) {
    const unsigned r = rnd(100);
    if (g->at > CODE_END) { alu(g); return; }
    if (r < 26) alu(g);
    else if (r < 46) mem(g);
    else if (r < 52) branch(g);
    else if (r < 55) call(g);
    else if (r < 59 && !g->depth) loop(g);
    else if (r < 61 && !g->depth) smc_loop(g);
    else if (r < 63) smc_ahead(g);
    else if (r < 64 && !g->depth) dma_patch(g);
    else if (r < 69) device(g);
    else if (r < 71) timebase(g);
    else if (r < 73) msr_bits(g);
    else if (r < 74 && !g->depth && !g->user) user(g);
    else if (r < 75) put(g, SC);
    else if (r < 80) fpu(g);
    else if (r < 85) mmu_item(g);
    else if (r < 90) misc(g);
    else alu(g);
}

static void generate(uint8_t *ram, start *s, ppc_model model, int mmu, unsigned items) {
    gctx g;
    memset(ram, 0, RAM_SIZE);
    memset(s, 0, sizeof *s);
    memset(&g, 0, sizeof g);
    g.ram = ram;
    g.model = model;
    g.mmu = mmu;
    g.dec_rearm = model == PPC_MODEL_601 ? 0x4000u + rnd(0x40000) : 200u + rnd(5000);
    setup_mmu(&g, s);
    vectors(&g);
    g.at = SUBS;
    for (unsigned i = 0; i < NSUBS; i++) {
        g.subs[i] = g.at;
        for (unsigned k = 1 + rnd(4); k; k--) alu(&g);
        put(&g, BLR);
    }
    for (uint32_t a = DATA; a < DATA + 0x3000u; a += 4) poke32(ram, a, rnd32());
    for (uint32_t a = ALIAS_A; a < ALIAS_B + 0x1000u; a += 4) poke32(ram, a, rnd32());
    g.at = CODE;
    for (unsigned i = 0; i < items && g.at < CODE_END; i++) item(&g);
    put(&g, B(0, 0));                               /* b . */
    s->pc = CODE;
    for (int i = 0; i < 32; i++) s->gpr[i] = rnd32();
    for (int i = 0; i < 32; i++) s->fpr[i] = (uint64_t)rnd32() << 32 | rnd32();
    s->gpr[1] = DATA;
    s->gpr[2] = 0xF0000000u;
    s->cr = rnd32();
    s->xer = rnd32() & 0xE000007Fu;
    s->lr = rnd32();
    s->ctr = rnd32();
    s->dec = rnd(3) ? g.dec_rearm * (1 + rnd(4)) : 0x7FFFFFFFu;
}

/* --- running both ---------------------------------------------------------------------- */

static unsigned failures;
static ppc_jit_stats totals;

static int lockstep(mc_ppc *a, mc_ppc *b, uint64_t cycles, const char *what) {
    char why[256];
    uint64_t done = 0;
    unsigned slice = 0;
    while (done < cycles) {
        const uint64_t n = 1 + rnd(rnd(4) ? 4000 : 40);
        const mc_run_status sa = mc_ppc_run(a, n, 0), sb = mc_ppc_run(b, n, 0);
        slice++;
        if (sa != sb || compare(a, b, why, sizeof why)) {
            if (sa != sb) snprintf(why, sizeof why, "run status %d / %d", (int)sa, (int)sb);
            printf("FAIL %s, slice %u (clock %llu, pc %08X, last %08X at %08X): %s\n", what, slice,
                   (unsigned long long)a->cycles, (unsigned)a->cpu.pc, (unsigned)a->cpu.last_opcode, (unsigned)a->cpu.last_pc, why);
            failures++;
            return 0;
        }
        if (sa != MC_RUN_OK) break;
        done += n;
    }
    if (b->jit) {
        ppc_jit_stats s;
        ppc_jit_get_stats(b->jit, &s);
        totals.blocks += s.blocks; totals.instructions += s.instructions; totals.steps += s.steps;
        totals.fills += s.fills; totals.flushes += s.flushes; totals.invalidated += s.invalidated; totals.declines += s.declines;
        totals.entries += s.entries; totals.idle_steps += s.idle_steps;
    }
    return 1;
}

static uint64_t all_steps;

/* The first clock count at which a single run of each to it differs: the
 * instruction there and around it (a failure's diagnosis). */
static void bisect(const uint8_t *image, const start *s, uint64_t hi) {
    char why[256] = "";
    uint64_t lo = 0;
    while (lo + 1 < hi) {
        const uint64_t mid = lo + (hi - lo) / 2;
        mc_ppc *a = machine(0), *b = machine(1);
        load(a, image, s);
        load(b, image, s);
        mc_ppc_run(a, mid, 0);
        mc_ppc_run(b, mid, 0);
        if (compare(a, b, why, sizeof why)) hi = mid;
        else lo = mid;
        mc_ppc_destroy(a);
        mc_ppc_destroy(b);
    }
    mc_ppc *a = machine(0), *b = machine(1);
    load(a, image, s);
    load(b, image, s);
    mc_ppc_run(a, lo, 0);
    mc_ppc_run(b, lo, 0);
    printf("  last equal at clock %llu: pc %08X\n", (unsigned long long)lo, (unsigned)a->cpu.pc);
    mc_ppc_run(a, hi - lo, 0);
    mc_ppc_run(b, hi - lo, 0);
    compare(a, b, why, sizeof why);
    printf("  first different at clock %llu/%llu: interpreter ran %08X at %08X (pc %08X), translator pc %08X: %s\n",
           (unsigned long long)a->cycles, (unsigned long long)b->cycles, (unsigned)a->cpu.last_opcode, (unsigned)a->cpu.last_pc,
           (unsigned)a->cpu.pc, (unsigned)b->cpu.pc, why);
    for (uint32_t p = (a->cpu.last_pc & ~3u) - 16u; p <= (a->cpu.last_pc & ~3u) + 16u && p < RAM_SIZE; p += 4)
        printf("    %08X: %08X\n", (unsigned)p, (unsigned)(image[p] << 24 | image[p + 1] << 16 | image[p + 2] << 8 | image[p + 3]));
    mc_ppc_destroy(a);
    mc_ppc_destroy(b);
}

static int diff(unsigned programs, uint64_t seed, int only) {
    static uint8_t image[RAM_SIZE];
    static const char *names[] = {"601", "603", "603e", "604", "604e", "750"};
    unsigned run = 0;
    for (int model = 0; model < PPC_MODEL_COUNT; model++) {
        if (only >= 0 && model != only) continue;
        for (unsigned p = 0; p < programs; p++) {
            start s;
            char what[96];
            const int soft = model == PPC_MODEL_603 || model == PPC_MODEL_603E;
            static const int modes[2][4] = {{M_REAL, M_BAT, M_TABLE, M_TABLE}, {M_REAL, M_BAT, M_SOFT, M_SOFT}};
            const int mmu = modes[soft][(p + (unsigned)seed) % 4];
            rng_state = (seed + p * 7919u + (uint64_t)model * 104729u) * 0x9E3779B97F4A7C15ull | 1u;
            board_desc.cpu = (uint8_t)model;
            board_desc.timebase_hz = model == PPC_MODEL_601 ? 7812500u : (uint32_t[]){4166666u, 8333333u, 16666666u}[rnd(3)];
            board_desc.cpu_hz = (uint32_t[]){33000000u, 60000000u, 100000000u, 233000000u}[rnd(4)];
            board_desc.cpi = 1 + rnd(3);
            generate(image, &s, (ppc_model)model, mmu, 150 + rnd(300));
            snprintf(what, sizeof what, "%s program %u (seed %llu, mmu %d)", names[model], p, (unsigned long long)seed, mmu);
            mc_ppc *a = machine(0), *b = machine(1);
            if (!a || !b || !b->jit) {
                printf("FAIL %s: no machine or no translator\n", what);
                failures++;
                mc_ppc_destroy(a);
                mc_ppc_destroy(b);
                continue;
            }
            load(a, image, &s);
            load(b, image, &s);
            if (!lockstep(a, b, 30000 + rnd(90000), what) && getenv("LIVADIX_JIT_BISECT")) bisect(image, &s, a->cycles);
            all_steps += a->cycles / board_desc.cpi;
            mc_ppc_destroy(a);
            mc_ppc_destroy(b);
            run++;
        }
    }
    printf("ppc-jit: %u random programs (%llu steps), %u differ from the interpreter; %llu blocks of %llu instructions translated, "
           "%.1f%% of the steps translated, %llu TLB fills, %llu flushes, %llu blocks invalidated\n",
           run, (unsigned long long)all_steps, failures, (unsigned long long)totals.blocks, (unsigned long long)totals.instructions,
           all_steps ? 100.0 * (double)totals.steps / (double)all_steps : 0.0, (unsigned long long)totals.fills,
           (unsigned long long)totals.flushes, (unsigned long long)totals.invalidated);
    return failures ? 1 : 0;
}

/* Programs from oracle.py's generator: a count, then per program its model
 * and its words (little-endian counts, big-endian words). */
static int oracle(const char *path) {
    static uint8_t image[RAM_SIZE];
    FILE *f = fopen(path, "rb");
    uint32_t count = 0;
    unsigned run = 0;
    if (!f || fread(&count, 4, 1, f) != 1) { printf("FAIL: cannot read %s\n", path); if (f) fclose(f); return 1; }
    for (uint32_t p = 0; p < count; p++) {
        uint32_t model, n;
        start s;
        char what[64];
        if (fread(&model, 4, 1, f) != 1 || fread(&n, 4, 1, f) != 1 || model >= PPC_MODEL_COUNT || n > 0x8000u) { printf("FAIL: bad program file\n"); fclose(f); return 1; }
        memset(image, 0, sizeof image);
        if (fread(image + 0x11000, 4, n, f) != n) { printf("FAIL: short program file\n"); fclose(f); return 1; }
        poke32(image, 0x11000 + 4 * n, B(0, 0));
        memset(&s, 0, sizeof s);
        s.pc = 0x11000;
        s.msr = PPC_MSR_PR | PPC_MSR_ME | PPC_MSR_FP;
        for (int i = 0; i < 16; i++) s.sr[i] = 0;
        rng_state = p * 0x9E3779B97F4A7C15ull | 1u;
        board_desc.cpu = (uint8_t)model;
        board_desc.timebase_hz = model == PPC_MODEL_601 ? 7812500u : 16666666u;
        board_desc.cpu_hz = 60000000u;
        board_desc.cpi = 1;
        mc_ppc *a = machine(0), *b = machine(1);
        snprintf(what, sizeof what, "oracle program %u (model %u)", p, model);
        if (!a || !b || !b->jit) { printf("FAIL %s: no machine or no translator\n", what); failures++; }
        else {
            /* the 601's and 604's resets leave DEC near zero or at its maximum: keep it far */
            a->cpu.dec = b->cpu.dec = 0x7FFFFFFFu;
            load(a, image, &s);
            load(b, image, &s);
            a->cpu.dec = b->cpu.dec = 0x7FFFFFFFu;
            lockstep(a, b, 4 * n + 2000, what);
            run++;
        }
        mc_ppc_destroy(a);
        mc_ppc_destroy(b);
    }
    fclose(f);
    printf("ppc-jit: %u oracle.py programs, %u differ from the interpreter\n", run, failures);
    return failures ? 1 : 0;
}

/* --- pm-6100.rom and speed ---------------------------------------------------------------- */

static double seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static int rom_run(const char *path) {
    char buffer[512], why[256];
    const char *roms = getenv("LIVADIX_ROMS");
    static uint8_t image[4u << 20];
    if (!path) {
        if (!roms) { printf("SKIP: LIVADIX_ROMS is not set\n"); return 77; }
        snprintf(buffer, sizeof buffer, "%s/power-macintosh-6100.rom", roms);
        path = buffer;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP: %s is absent\n", path); return 77; }
    const size_t size = fread(image, 1, sizeof image, f);
    fclose(f);
    mc_ppc *m[2];
    double t[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        mc_config c;
        char error[256] = "";
        mc_config_defaults(&c);
        mc_config_set_ram_kb(&c, 8u << 10);
        c.jit = i;
        m[i] = mc_ppc_create(&mc_pdm_desc, &c, image, size, error, sizeof error);
        if (!m[i]) { printf("FAIL: %s\n", error); return 1; }
    }
    if (!m[1]->jit) { printf("SKIP: no translator on this host\n"); return 77; }
    mc_run_status st[2] = {MC_RUN_OK, MC_RUN_OK};
    int ok = 1;
    for (int k = 0; k < 200 && st[0] == MC_RUN_OK && ok; k++) {
        for (int i = 0; i < 2; i++) {
            const double t0 = seconds();
            st[i] = mc_ppc_run(m[i], 1000000, 0);
            t[i] += seconds() - t0;
        }
        if (st[0] != st[1] || compare(m[0], m[1], why, sizeof why)) {
            if (st[0] != st[1]) snprintf(why, sizeof why, "run status %d / %d", (int)st[0], (int)st[1]);
            printf("FAIL pm-6100.rom at clock %llu (pc %08X): %s\n", (unsigned long long)m[0]->cycles, (unsigned)m[0]->cpu.pc, why);
            ok = 0;
        }
    }
    const uint64_t n = m[0]->cpu.instructions;
    if (ok) {
        ppc_jit_stats s;
        ppc_jit_get_stats(m[1]->jit, &s);
        printf("pm-6100.rom: %s after %llu instructions, identical (state, RAM, instructions, clocks every 1,000,000 clocks)\n",
               st[0] == MC_RUN_ERROR ? mc_ppc_error(m[0]) : "no probe", (unsigned long long)n);
        printf("pm-6100.rom: interpreter %.3f s (%.0f MIPS), translator %.3f s (%.0f MIPS); %llu blocks, %llu instructions translated, "
               "%.1f%% of the steps translated\n", t[0], (double)n / t[0] / 1e6, t[1], (double)n / t[1] / 1e6,
               (unsigned long long)s.blocks, (unsigned long long)s.instructions, n ? 100.0 * (double)s.steps / (double)m[1]->cycles : 0.0);
        ok = st[0] == MC_RUN_ERROR && m[0]->probe.hit && !strncmp(mc_ppc_error(m[0]), "First hardware probe", 20);
    }
    mc_ppc_destroy(m[0]);
    mc_ppc_destroy(m[1]);
    return ok ? 0 : 1;
}

/* Real boot checkpoints additionally compare the complete MMIO transcript
 * (values, faults and guest access times), scheduler deadlines and display.
 * Disks are supplied by verify.py as work copies and opened read-only. */
typedef struct boot_observer { mc_ppc *m; uint64_t count, hash; } boot_observer;
static void boot_observe(void *ctx, uint32_t address, unsigned size, int write, uint32_t value, int fault)
{
    boot_observer *o = ctx;
    if (o->m->bus.read_map[address >> 12]) return;
    const uint64_t parts[] = {address, size, (unsigned)write, value, (unsigned)fault, mc_ppc_now(o->m)};
    for (unsigned k = 0; k < sizeof parts / sizeof *parts; ++k) o->hash = (o->hash ^ parts[k]) * UINT64_C(1099511628211);
    ++o->count;
}
static int boot_devices(mc_ppc *a, mc_ppc *b, const boot_observer *oa, const boot_observer *ob, char *why, size_t n)
{
    if (oa->count != ob->count || oa->hash != ob->hash) {
        snprintf(why, n, "MMIO transcript: %llu/%llu accesses, %016llX/%016llX",
                 (unsigned long long)oa->count, (unsigned long long)ob->count,
                 (unsigned long long)oa->hash, (unsigned long long)ob->hash); return 0;
    }
    if (a->sched.count != b->sched.count || a->sched.queued != b->sched.queued) {
        snprintf(why, n, "scheduler entry count"); return 0;
    }
    for (unsigned k = 0; k < a->sched.count; ++k)
        if (a->sched.entry[k].when != b->sched.entry[k].when || a->sched.entry[k].pos != b->sched.entry[k].pos) {
            snprintf(why, n, "scheduler entry %u", k); return 0;
        }
    mc_framebuffer fa = {0}, fb = {0};
    const int ha = mc_ppc_framebuffer(a, &fa), hb = mc_ppc_framebuffer(b, &fb);
    if (ha != hb || (ha && (fa.width != fb.width || fa.height != fb.height || fa.depth != fb.depth || fa.row_bytes != fb.row_bytes))) {
        snprintf(why, n, "framebuffer mode"); return 0;
    }
    if (ha && fa.width && fa.height) {
        const size_t bytes = (size_t)fa.width * fa.height * 4;
        uint32_t *pa = malloc(bytes), *pb = malloc(bytes);
        if (!pa || !pb) { free(pa); free(pb); snprintf(why, n, "framebuffer allocation"); return 0; }
        mc_framebuffer_argb(&fa, pa); mc_framebuffer_argb(&fb, pb);
        const int same = !memcmp(pa, pb, bytes);
        free(pa); free(pb);
        if (!same) { snprintf(why, n, "framebuffer pixels/CLUT"); return 0; }
    }
    return 1;
}
static int boot_menu_bar(mc_ppc *m) {
    mc_framebuffer fb;
    memset(&fb, 0, sizeof fb);
    if (!mc_ppc_framebuffer(m, &fb) || fb.height < 20 || fb.width < 64) return 0;
    uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
    if (!argb) return 0;
    mc_framebuffer_argb(&fb, argb);
    unsigned dark19 = 0, light1 = 0, dark10 = 0, title_ink = 0;
    for (unsigned x = 0; x < fb.width; x++) {
        const uint32_t a = argb[19u * fb.width + x], b = argb[1u * fb.width + x], c = argb[10u * fb.width + x];
        const unsigned amax = (a >> 16 & 255) > (a >> 8 & 255) ? (a >> 16 & 255) : (a >> 8 & 255);
        const unsigned cmax = (c >> 16 & 255) > (c >> 8 & 255) ? (c >> 16 & 255) : (c >> 8 & 255);
        const unsigned bmin = (b >> 16 & 255) < (b >> 8 & 255) ? (b >> 16 & 255) : (b >> 8 & 255);
        dark19 += (amax > (a & 255) ? amax : (a & 255)) <= 0x40;
        light1 += (bmin < (b & 255) ? bmin : (b & 255)) >= 0xC0;
        dark10 += (cmax > (c & 255) ? cmax : (c & 255)) <= 0x40;
        if (x >= 24u && x < fb.width / 2u && x < 320u)
            title_ink += (cmax > (c & 255) ? cmax : (c & 255)) <= 0x40;
    }
    free(argb);
    return dark19 * 100 >= fb.width * 98u && light1 * 10 >= fb.width * 9u && dark10 * 50 >= fb.width && dark10 * 5 <= fb.width * 2u && title_ink >= 18u;
}
static int boot_run(const char *rom_path, const char *model_key, const char *disk_path, double duration, int ata)
{
    static uint8_t image[4u << 20];
    char why[256] = "", error[256] = "";
    mc_ppc *m[3] = {NULL, NULL, NULL};
    boot_observer obs[3] = {0};
    const mc_ppc_desc *desc = mc_ppc_find_board(model_key);
    const mc_model *model = mc_model_find(model_key);
    FILE *f = fopen(rom_path, "rb");
    if (!f) { printf("SKIP: no %s\n", rom_path); return 77; }
    const size_t size = fread(image, 1, sizeof image, f);
    fclose(f);
    if (!desc || !model || !ppc_jit_available()) { printf("FAIL: no model or translator\n"); return 1; }
    int ok = 1;
    for (int k = 0; k < 3 && ok; ++k) {
        mc_config c;
        mc_config_defaults(&c);
        mc_config_set_ram_kb(&c, (desc == &mc_pdm6100_desc || desc == &mc_pdm7100_desc || desc == &mc_pdm8100_desc ? 40u : 64u) << 10);
        c.model = (uint32_t)mc_model_index(model); c.cpu_hz = model->cpu_hz;
        c.jit = k != 0; c.disk_count = 1; c.disks[0].bus = ata ? MC_BUS_ATA : MC_BUS_SCSI;
        m[k] = mc_ppc_create(desc, &c, image, size, error, sizeof error);
        if (!m[k]) { printf("FAIL: %s\n", error); ok = 0; break; }
        mc_ppc_set_clock(m[k], INT64_C(1700000000));
        ppc_jit_idle_enable(m[k]->jit, k == 2);
        obs[k].m = m[k]; obs[k].hash = UINT64_C(14695981039346656037);
        m[k]->bus.observe = boot_observe; m[k]->bus.observe_ctx = &obs[k];
        mc_disk *disk = mc_disk_open(disk_path, 0, 512, error, sizeof error);
        if (!disk || !mc_ppc_insert(m[k], MC_MEDIA_DISK, 0, disk, error, sizeof error)) {
            printf("FAIL: disk: %s\n", error); ok = 0;
        }
    }
    unsigned checkpoints = 0;
    uint64_t end = (uint64_t)(duration * MC_C15M_HZ);
    int finder = 0;
    while (ok && mc_ppc_now(m[0]) < end) {
        mc_run_status st[3];
        for (int k = 0; k < 3; ++k) st[k] = mc_ppc_run(m[k], duration > 2 ? 10000000 : 2000000, 0);
        ++checkpoints;
        for (int k = 1; k < 3 && ok; ++k) {
            if (st[0] != st[k]) snprintf(why, sizeof why, "run status %d/%d", (int)st[0], (int)st[k]);
            else if (!compare(m[0], m[k], why, sizeof why) && boot_devices(m[0], m[k], &obs[0], &obs[k], why, sizeof why)) continue;
            printf("FAIL %s checkpoint %u, idle %s, t=%.6f pc=%08X: %s\n", model_key, checkpoints,
                   k == 2 ? "on" : "off", (double)mc_ppc_now(m[0]) / MC_C15M_HZ, m[0]->cpu.pc, why);
            ok = 0;
        }
        if (st[0] != MC_RUN_OK) { printf("FAIL: boot stopped: %s\n", mc_ppc_error(m[0])); ok = 0; }
        if (ok && duration > 2 && !finder && boot_menu_bar(m[0])) {
            const uint64_t settle = mc_ppc_now(m[0]) + 20u * MC_C15M_HZ;   /* into the idle task */
            if (settle < end) end = settle;
            finder = 1;
        }
    }
    if (ok && duration > 2 && !boot_menu_bar(m[0])) { printf("FAIL: no Finder menu bar at the final checkpoint\n"); ok = 0; }
    if (ok) {
        ppc_jit_stats stats;
        ppc_jit_get_stats(m[2]->jit, &stats);
        if (duration > 2 && !stats.idle_steps) { printf("FAIL: %s: the idle task was not folded\n", model_key); ok = 0; }
        printf("%s: %u boot checkpoints identical (CPU, RAM, MMIO and times, scheduler, display; interpreter and translator idle off/on), %.3f guest s, %llu idle steps folded\n",
               model_key, checkpoints, (double)mc_ppc_now(m[0]) / MC_C15M_HZ, (unsigned long long)stats.idle_steps);
    }
    for (int k = 0; k < 3; ++k) mc_ppc_destroy(m[k]);
    return ok ? 0 : 1;
}

/* Event and DEC boundaries while folding a stationary loop. The fake
 * device performs DMA and raises INT at its deadline, as in random tests. */
static int idle_test(void)
{
    static uint8_t image[RAM_SIZE];
    start s;
    gctx g;
    for (int model = 0; model < PPC_MODEL_COUNT; ++model) for (int variant = 0; variant < 5; ++variant) {
        memset(image, 0, sizeof image); memset(&s, 0, sizeof s); memset(&g, 0, sizeof g);
        g.ram = image; g.model = (ppc_model)model; g.dec_rearm = model == PPC_MODEL_601 ? 0x100000u : 1000u;
        vectors(&g);
        poke32(image, CODE, B(0, 0));
        s.pc = CODE; s.msr = PPC_MSR_ME | PPC_MSR_EE; s.dec = model == PPC_MODEL_601 ? 0x2000u : 64u;
        s.gpr[1] = DATA;
        if (variant == 4) {
            /* Mac OS 9's idle task: registers rotated through one another, the
             * state the same again only after as many iterations as registers. */
            static const unsigned ring[] = {20, 11, 12, 13, 14, 15, 16, 20};
            for (unsigned r = 0; r < 7; ++r) poke32(image, CODE + 4 * r, X(444, ring[r + 1], ring[r], ring[r + 1], 0));
            poke32(image, CODE + 28, B(-28, 0));
            for (unsigned r = 11; r <= 16; ++r) s.gpr[r] = 0x1000u * r + 7u;
        } else if (variant) {
            poke32(image, CODE, D(3, 31, 31, 0));
            s.gpr[3] = 0x12345678u; s.gpr[4] = DATA + 64;
            poke32(image, DATA + 64, s.gpr[3]);
            if (variant == 1) { /* Idempotent RAM write, exact trap/return recurrence. */
                poke32(image, 0x700, D(36, 3, 4, 0)); poke32(image, 0x704, RFI);
            } else if (variant == 2) { /* A changing RAM/register state is busy. */
                poke32(image, 0x700, D(14, 3, 3, 1)); poke32(image, 0x704, D(36, 3, 4, 0)); poke32(image, 0x708, RFI);
            } else { /* Reading the clock prevents recurrence folding. */
                poke32(image, 0x700, MFSPR(3, model == PPC_MODEL_601 ? 5 : 268)); poke32(image, 0x704, RFI);
            }
        }
        board_desc.cpu = (uint8_t)model; board_desc.cpu_hz = 60000000u; board_desc.timebase_hz = 7812500u; board_desc.cpi = 1;
        mc_ppc *a = machine(0), *b = machine(1);
        if (!a || !b || !b->jit) { printf("FAIL: idle test machine\n"); mc_ppc_destroy(a); mc_ppc_destroy(b); return 1; }
        load(a, image, &s); load(b, image, &s);
        for (int i = 0; i < 2; ++i) {
            mc_ppc *m = i ? b : a;
            tick *t = &((board *)m->board)->t;
            t->at = 5000; t->dma_addr = DATA + 32; t->dma_value = 0x12345678u;
            mc_dev_schedule(&t->dev, t->at);
        }
        char why[256];
        for (unsigned k = 0; k < 600; ++k) {
            const uint64_t clocks = 2000u + (k * 7919u) % 10000u;
            mc_run_status sa = mc_ppc_run(a, clocks, 0), sb = mc_ppc_run(b, clocks, 0);
            if (sa != sb || compare(a, b, why, sizeof why)) {
                printf("FAIL idle model %d slice %u: %s\n", model, k, sa != sb ? "run status" : why);
                mc_ppc_destroy(a); mc_ppc_destroy(b); return 1;
            }
        }
        ppc_jit_stats stats;
        ppc_jit_get_stats(b->jit, &stats);
        if ((variant < 2 || variant == 4) != (stats.idle_steps != 0)) { printf("FAIL idle model %d variant %d: unexpected folding\n", model, variant); mc_ppc_destroy(a); mc_ppc_destroy(b); return 1; }
        printf("idle model %d variant %d: %llu folded steps, CPU/RAM/device/clocks exact across DMA, INT and DEC\n", model, variant, (unsigned long long)stats.idle_steps);
        mc_ppc_destroy(a); mc_ppc_destroy(b);
    }
    return 0;
}

static int reset_test(void)
{
    static uint8_t image[RAM_SIZE];
    start s;
    for (int model = 0; model < PPC_MODEL_COUNT; ++model) {
        memset(image, 0, sizeof image); memset(&s, 0, sizeof s);
        poke32(rom, 0x100, B(0, 0));
        for (unsigned k = 0; k < 12; ++k) poke32(image, CODE + 4u * k, D(14, 7, 7, 1));
        poke32(image, CODE + 48, D(36, 7, 3, 0x18));
        s.pc = CODE; s.msr = PPC_MSR_ME; s.gpr[3] = 0xF0000000u;
        board_desc.cpu = (uint8_t)model; board_desc.cpi = 1;
        mc_ppc *a = machine(0), *b = machine(1);
        if (!a || !b || !b->jit) { mc_ppc_destroy(a); mc_ppc_destroy(b); return 1; }
        load(a, image, &s); load(b, image, &s);
        mc_run_status sa = mc_ppc_run(a, 100, 0), sb = mc_ppc_run(b, 100, 0);
        char why[256];
        const int same = sa == sb && !compare(a, b, why, sizeof why);
        printf("reset model %d: %s\n", model, same ? "CPU/RAM/device/clocks exact" : sa != sb ? "run status differs" : why);
        mc_ppc_destroy(a); mc_ppc_destroy(b);
        if (!same) return 1;
    }
    return 0;
}

/* MPC603EUM 2.3.4.7 and the Pippin's 603 ROM: the SCSI queue reserves
 * its head, then stores to a link in another cache block. The store
 * consumes the reservation; a second store with no lwarx must fail. */
static int reservation_test(void)
{
    static uint8_t image[RAM_SIZE];
    const start s = {.pc = CODE, .msr = PPC_MSR_ME, .dec = 0x7FFFFFFFu,
                     .gpr = {[3] = DATA, [4] = DATA + 32, [5] = 0x1234, [6] = 0x5678}};
    poke32(image, CODE + 0, D(36, 0, 4, 0));  /* warm the write mapping */
    poke32(image, CODE + 4, D(32, 0, 3, 0));  /* warm the read mapping */
    poke32(image, CODE + 8, X(20, 7, 0, 3, 0));
    poke32(image, CODE + 12, X(150, 5, 0, 4, 1));
    poke32(image, CODE + 16, X(19, 8, 0, 0, 0)); /* mfcr r8 */
    poke32(image, CODE + 20, X(150, 6, 0, 4, 1));
    for (int model = PPC_MODEL_603; model <= PPC_MODEL_603E; model++) {
        board_desc.cpu = (uint8_t)model;
        for (int jit = 0; jit <= 1; jit++) {
            mc_ppc *m = machine(jit);
            if (!m || (jit && !m->jit)) { mc_ppc_destroy(m); return 1; }
            load(m, image, &s);
            mc_ppc_run(m, 6, 0);
            uint32_t value;
            const int ok = mc_ppc_peek(m, DATA + 32, 4, &value) && value == 0x1234 &&
                           (m->cpu.gpr[8] >> 28) == 2 && (m->cpu.cr >> 28) == 0 && !m->cpu.reserved;
            mc_ppc_destroy(m);
            if (!ok) { printf("FAIL: reservation model %d JIT %d\n", model, jit); return 1; }
        }
    }
    return 0;
}

static int peek_test(void)
{
    static const ppc_model models[] = {PPC_MODEL_601, PPC_MODEL_603, PPC_MODEL_603E, PPC_MODEL_604, PPC_MODEL_604E, PPC_MODEL_750};
    for (unsigned k = 0; k < sizeof models / sizeof *models; ++k) {
        board_desc.cpu = (uint8_t)models[k];
        mc_ppc *m = machine(0);
        if (!m) return 1;
        m->cpu.msr = PPC_MSR_DR | PPC_MSR_ME;
        m->cpu.sr[0] = 1; m->cpu.sdr1 = 0x80000;
        poke32(m->ram, 0x80000, 0x80000080u);
        poke32(m->ram, 0x80004, 0x00002002u); /* valid page, R/C clear */
        poke32(m->ram, 0x2000, 0x12345678u);
        const ppc_cpu before = m->cpu;
        uint32_t value = 0;
        const int ok = mc_ppc_peek(m, 0x1000, 4, &value) && value == 0x12345678u &&
            !memcmp(&before, &m->cpu, sizeof before) && m->ram[0x80006] == 0x20 && m->ram[0x80007] == 2;
        printf("peek model %d: %s\n", models[k], ok ? "valid unreferenced page read without CPU/PTE changes" : "FAIL");
        if (ok) {
            poke32(m->ram, 0x80000, 0); /* Absent mappings must still fail. */
            const int rejected = !mc_ppc_peek(m, 0x1000, 4, &value) && !memcmp(&before, &m->cpu, sizeof before);
            if (!rejected) { mc_ppc_destroy(m); printf("FAIL: absent debug mapping\n"); return 1; }
        }
        mc_ppc_destroy(m);
        if (!ok) return 1;
    }
    return 0;
}

static int speed(void) {
    static uint8_t image[RAM_SIZE];
    start s;
    gctx g;
    double t[2];
    uint64_t n[2];
    mc_ppc *m[2];
    char why[256];
    memset(image, 0, sizeof image);
    memset(&s, 0, sizeof s);
    memset(&g, 0, sizeof g);
    g.ram = image;
    g.at = CODE;
    /* a loop of integer, rotate, compare, branch, store and load */
    put(&g, D(14, 3, 0, 0)); put(&g, D(14, 4, 0, 0)); put(&g, D(15, 5, 0, 0x7FFF)); put(&g, MTSPR(9, 5));
    put(&g, X(266, 4, 4, 3, 0)); put(&g, D(14, 3, 3, 1)); put(&g, RLW(21, 4, 6, 3, 0, 28, 0)); put(&g, X(316, 6, 7, 3, 0));
    put(&g, D(36, 7, 1, 0)); put(&g, D(32, 8, 1, 0)); put(&g, X(0, 0, 8, 7, 0)); put(&g, BC(4, 2, 8, 0)); put(&g, D(14, 9, 9, 1));
    put(&g, BC(16, 0, -36, 0));
    put(&g, B(0, 0));
    s.pc = CODE;
    s.msr = PPC_MSR_ME;
    s.gpr[1] = DATA;
    s.dec = 0x7FFFFFFFu;
    board_desc.cpu = PPC_MODEL_601;
    board_desc.cpi = 1;
    board_desc.cpu_hz = 60000000u;
    board_desc.timebase_hz = 7812500u;
    for (int i = 0; i < 2; i++) {
        m[i] = machine(i);
        if (!m[i] || (i && !m[i]->jit)) { printf("SKIP: no translator\n"); mc_ppc_destroy(m[0]); mc_ppc_destroy(m[1]); return 77; }
        load(m[i], image, &s);
        const double t0 = seconds();
        for (int k = 0; k < 100; k++) mc_ppc_run(m[i], 1000000, 0);
        t[i] = seconds() - t0;
        n[i] = m[i]->cpu.instructions;
    }
    const int same = !compare(m[0], m[1], why, sizeof why);
    printf("CPU-bound loop (601): interpreter %.0f MIPS, translator %.0f MIPS (%llu instructions each), %s\n",
           (double)n[0] / t[0] / 1e6, (double)n[1] / t[1] / 1e6, (unsigned long long)n[1], same ? "identical" : why);
    mc_ppc_destroy(m[0]);
    mc_ppc_destroy(m[1]);
    return same ? 0 : 1;
}

/* --- ppc_fpu_fast against ppc_fpu ------------------------------------------------------------ */

static uint64_t fp_bits(double d) { uint64_t b; memcpy(&b, &d, sizeof b); return b; }
static double fp_value(uint64_t b) { double d; memcpy(&d, &b, sizeof d); return d; }

/* An operand: mostly ordinary values (small integers and fractions, values
 * a single holds, values halfway between two singles, random fractions in
 * a moderate exponent range), some at the ends of the range and special. */
static uint64_t fp_operand(void) {
    static const uint64_t special[] = {
        UINT64_C(0x7FF0000000000000), UINT64_C(0xFFF0000000000000), UINT64_C(0x7FF8000000000000), UINT64_C(0x7FF4000000000000),
        UINT64_C(0x0000000000000001), UINT64_C(0x800FFFFFFFFFFFFF), UINT64_C(0x0010000000000000), UINT64_C(0x7FEFFFFFFFFFFFFF),
        UINT64_C(0x3810000000000000), UINT64_C(0x47EFFFFFE0000000), UINT64_C(0x41DFFFFFFFC00000), UINT64_C(0xC1E0000000000000)};
    const uint64_t sign = (uint64_t)rnd(2) << 63, fraction = ((uint64_t)rnd32() << 20 | rnd(1u << 20)) & UINT64_C(0xFFFFFFFFFFFFF);
    switch (rnd(16)) {
    case 0: return sign;
    case 1: return special[rnd(12)] ^ (rnd(4) ? 0 : sign);
    case 2: return (uint64_t)rnd32() << 32 | rnd32();
    case 3: case 4: return fp_bits((double)(int32_t)(rnd32() >> rnd(31)) / (double)(1u << rnd(8)) * (rnd(2) ? 1.0 : -1.0));
    case 5: case 6: return fp_bits((double)((float)(int32_t)rnd32() / (float)(1u + rnd(4096))));
    case 7: return (fp_bits((double)((float)(int32_t)rnd32() / (float)(1u + rnd(4096)))) | UINT64_C(0x10000000)) + rnd(3) - 1u;
    case 8: return sign | (uint64_t)(1023u - 520u + rnd(1040)) << 52 | fraction;
    case 9: return sign | (uint64_t)(1u + rnd(2046)) << 52 | fraction;
    case 10: return sign | (uint64_t)(1023u - 30u + rnd(60)) << 52 | (fraction & ~UINT64_C(0x1FFFFFFF));
    default: return sign | (uint64_t)(1023u - 40u + rnd(80)) << 52 | (rnd(3) ? fraction : fraction & (UINT64_C(0xFFFFFFFFFFFFF) << (20u + rnd(32))));
    }
}

static int fp_fast_test(unsigned long cases, uint64_t seed) {
    static const unsigned a_ops[] = {18, 20, 21, 25, 28, 29, 30, 31};
    static const unsigned x_ops[] = {12, 14, 15, 0, 32, 72, 40, 583};
    static const uint32_t states[] = {0, 0x02000000u, 0x82020000u, 0x00000004u, 0x00000008u, 0x00000001u, 0x00000002u, 0x00000003u,
                                      0x000000F8u, 0xFFF80700u, 0x20000080u, 0x00064000u};
    unsigned long handled = 0, ordinary = 0, ordinary_handled = 0;
    rng_state = seed ? seed : 0x9E3779B97F4A7C15ull;
    for (unsigned long n = 0; n < cases; ++n) {
        ppc_cpu base, fast, soft;
        const unsigned t = rnd(32), a = rnd(32), b = rnd(32), c = rnd(32);
        uint32_t op;
        int plain = 0;
        memset(&base, 0, sizeof base);
        base.model = (ppc_model)rnd(PPC_MODEL_COUNT);
        base.msr = (rnd(64) ? PPC_MSR_FP : 0u) | (rnd(4) ? 0u : PPC_MSR_FE0) | PPC_MSR_ME;
        base.fpscr = rnd(3) ? 0u : rnd(4) ? states[rnd(12)] : rnd32() & ~0x60000804u;
        base.cr = rnd32();
        for (unsigned r = 0; r < 32; ++r) base.fpr[r] = fp_operand();
        if (rnd(4)) {
            const unsigned xo = a_ops[rnd(8)];
            op = (rnd(3) ? 63u : 59u) << 26 | t << 21 | a << 16 | (xo == 25 && rnd(50) ? 0 : b) << 11 |
                 (xo >= 25 || !rnd(50) ? c : 0) << 6 | xo << 1 | (rnd(8) ? 0u : 1u);
            if (!rnd(6)) {                         /* cancellation: b near -(a*c) or -a, and their neighbours */
                const double x = fp_value(base.fpr[a]), z = fp_value(base.fpr[c]);
                base.fpr[b] = fp_bits(xo >= 28 ? -(x * z) : -x) + rnd(5) - 2u;
                if (rnd(2)) base.fpr[b] ^= UINT64_C(0x8000000000000000);
            }
        } else {
            const unsigned xo = x_ops[rnd(8)];
            if (xo == 0 || xo == 32) op = 63u << 26 | rnd(8) << 23 | a << 16 | b << 11 | xo << 1;
            else op = 63u << 26 | t << 21 | (rnd(50) ? 0 : a) << 16 | b << 11 | xo << 1 | (rnd(8) ? 0u : 1u);
        }
        fast = soft = base;
        if (!base.fpscr && (base.msr & PPC_MSR_FP)) {
            plain = 1;
            for (unsigned r = 0; r < 32 && plain; ++r) {
                const unsigned e = (unsigned)(base.fpr[r] >> 52) & 2047u;
                if (r == a || r == b || r == c) plain = e > 1023u - 60u && e < 1023u + 60u;
            }
            ordinary += (unsigned long)plain;
        }
        if (!ppc_fpu_fast(&fast, op)) {
            if (memcmp(&fast, &base, sizeof base)) { printf("FAIL fp case %lu op %08X: state changed without a result\n", n, (unsigned)op); return 1; }
            continue;
        }
        ++handled;
        ordinary_handled += (unsigned long)plain;
        ppc_fpu(&soft, op);
        if (memcmp(&fast, &soft, sizeof soft)) {
            printf("FAIL fp case %lu model %d op %08X fpscr %08X a %016llX b %016llX c %016llX: fast %016llX fpscr %08X cr %08X, reference %016llX fpscr %08X cr %08X result %d\n",
                   n, (int)base.model, (unsigned)op, (unsigned)base.fpscr, (unsigned long long)base.fpr[a], (unsigned long long)base.fpr[b],
                   (unsigned long long)base.fpr[c], (unsigned long long)fast.fpr[t], (unsigned)fast.fpscr, (unsigned)fast.cr,
                   (unsigned long long)soft.fpr[t], (unsigned)soft.fpscr, (unsigned)soft.cr, (int)soft.result);
            return 1;
        }
    }
    printf("ppc-fpu-fast: %lu cases, %lu made on host arithmetic (%lu of %lu with ordinary operands), 0 differ from ppc_fpu\n",
           cases, handled, ordinary_handled, ordinary);
    if (ordinary_handled * 10u < ordinary * 8u) { printf("FAIL: the host path declined too many ordinary cases\n"); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (getenv("LIVADIX_JIT_MAXBLOCK")) max_block = (unsigned)strtoul(getenv("LIVADIX_JIT_MAXBLOCK"), NULL, 10);
    if (argc >= 2 && !strcmp(argv[1], "rom")) return rom_run(argc > 2 ? argv[2] : NULL);
    if (argc >= 2 && !strcmp(argv[1], "speed")) return speed();
    if (argc >= 2 && !strcmp(argv[1], "idle") && !ppc_jit_available()) { printf("SKIP: no translator on this host\n"); return 77; }
    if (argc >= 2 && !strcmp(argv[1], "idle")) return idle_test() || reset_test() || reservation_test() || peek_test();
    if (argc >= 2 && !strcmp(argv[1], "fp")) return fp_fast_test(argc > 2 ? strtoul(argv[2], NULL, 10) : 200000ul, argc > 3 ? strtoull(argv[3], NULL, 10) : 0);
    if (argc >= 6 && !strcmp(argv[1], "boot")) return boot_run(argv[2], argv[3], argv[4], atof(argv[5]), argc > 6 && !strcmp(argv[6], "ata"));
    if (argc >= 3 && !strcmp(argv[1], "oracle")) return oracle(argv[2]);
    if (argc >= 2 && !strcmp(argv[1], "diff")) {
        ppc_model only = PPC_MODEL_COUNT;
        if (!ppc_jit_available()) { printf("SKIP: no translator on this host\n"); return 77; }
        if (argc > 4 && strcmp(argv[4], "all") && !ppc_model_parse(argv[4], &only)) { fprintf(stderr, "unknown model %s\n", argv[4]); return 2; }
        if (argc > 5) max_block = (unsigned)strtoul(argv[5], NULL, 10);
        return diff(argc > 2 ? (unsigned)strtoul(argv[2], NULL, 10) : 20, argc > 3 ? strtoull(argv[3], NULL, 10) : 1,
                    only == PPC_MODEL_COUNT ? -1 : (int)only);
    }
    fprintf(stderr, "usage: mc_ppc_jit_tests diff [N [SEED [MODEL|all [MAXBLOCK]]]] | oracle FILE | rom [PATH] | speed\n");
    return 2;
}
