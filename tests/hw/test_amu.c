/* test_amu: the 24-bit decode (amu.c) and the CPU across mode switches.
 *
 * Livadix's own tests. The decode's tables against the Macintosh II's map
 * (amu.h): A31-A24 ignored, each MiB at its base, device pages left to the
 * slow path, rebuilt when the bus's tables change. Then a 68020 on a bus
 * laid out as the Macintosh II's (RAM at 0, ROM at $40000000, slot 9
 * memory at $F9000000, the I/O space at $50000000 with a register standing
 * for VIA2 PB3, low for 24-bit) runs a program that switches modes by
 * writing the register, from main code and from code whose own page maps
 * elsewhere in the other mode: the interpreter's results are worked out
 * from the map, and the translator (blocks of every length, call-out or
 * leave-on-miss) gives the same registers, RAM and instruction counts
 * after every slice. */
#include "amu.h"
#include "hw_test.h"
#include "m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SIZE (16u << 20)
#define ROM_SIZE 0x10000u
#define SLOT_SIZE 0x100000u

typedef struct env {
    mc_bus bus;
    mc_amu amu;
    m68k_cpu cpu;
    uint8_t *ram, *rom, *slot;
    int io;             /* the I/O region */
    unsigned switches;
} env;

static void set24(env *e, int on) {
    if (!mc_amu_set(&e->amu, &e->bus, on)) return;
    uint8_t **r = e->amu.on ? e->amu.read_map : e->bus.read_map, **w = e->amu.on ? e->amu.write_map : e->bus.write_map;
    m68k_set_maps(&e->cpu, r, w, r, w);
    e->switches++;
}

/* The register: bit 3 high selects 32-bit mode (VIA2 PB3 on the Macintosh
 * II, the ROM's SwapMMUMode). */
static uint32_t ctl_read(void *dev, uint32_t offset, unsigned size) {
    env *e = dev;
    (void)offset;
    (void)size;
    return e->amu.on ? 0 : 8;
}
static void ctl_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    set24(dev, !(value & 8u));
}
static const mc_dev_ops ctl_ops = {ctl_read, ctl_write, NULL, NULL, NULL};

/* The machine's CPU callbacks (machine.c): in 24-bit mode the address is
 * mapped before the bus decodes it. */
static int cpu_read(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t *v) {
    env *e = ctx;
    (void)fc;
    return mc_bus_read(&e->bus, e->amu.on ? mc_amu_map(&e->amu, a) : a, size, v);
}
static int cpu_write(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    env *e = ctx;
    (void)fc;
    return mc_bus_write(&e->bus, e->amu.on ? mc_amu_map(&e->amu, a) : a, size, v);
}

static void put16(uint8_t *p, uint32_t off, const uint16_t *w, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        p[off + 2 * i] = (uint8_t)(w[i] >> 8);
        p[off + 2 * i + 1] = (uint8_t)w[i];
    }
}
static void put32(uint8_t *p, uint32_t off, uint32_t v) {
    p[off] = (uint8_t)(v >> 24); p[off + 1] = (uint8_t)(v >> 16); p[off + 2] = (uint8_t)(v >> 8); p[off + 3] = (uint8_t)v;
}

/* The program (at $1000, supervisor):
 *   loop: LEA $00900000,A0
 *         JSR (A0)                 24-bit: slot 9 code, ADDQ.L #1,D0; RTS
 *         MOVE.B #8,$50F00000      to 32-bit
 *         JSR (A0)                 32-bit: RAM code, ADDQ.L #1,D1; RTS
 *         MOVE.L $00800000,D2      RAM
 *         MOVE.B #0,$50F00000      to 24-bit
 *         MOVE.L $00800000,D3      the ROM
 *         MOVE.L $FF002000,D4      RAM $2000 (A31-A24 ignored)
 *         JSR 16(A0)               slot code that switches to 32-bit; the
 *                                  next instruction then comes from RAM:
 *                                  MOVEQ #7,D6; RTS (the slot has MOVEQ #9)
 *         MOVE.B #0,$50F00000      to 24-bit
 *         ADDQ.L #1,D5
 *         BRA.S loop */
static const uint16_t program[] = {
    0x41F9, 0x0090, 0x0000, 0x4E90, 0x13FC, 0x0008, 0x50F0, 0x0000, 0x4E90, 0x2439, 0x0080, 0x0000,
    0x13FC, 0x0000, 0x50F0, 0x0000, 0x2639, 0x0080, 0x0000, 0x2839, 0xFF00, 0x2000, 0x4EA8, 0x0010,
    0x13FC, 0x0000, 0x50F0, 0x0000, 0x5285, 0x60C4,
};

static void setup(env *e) {
    memset(e, 0, sizeof *e);
    e->ram = calloc(RAM_SIZE, 1);
    e->rom = calloc(ROM_SIZE, 1);
    e->slot = calloc(SLOT_SIZE, 1);
    mc_bus_init(&e->bus);
    mc_bus_map_memory(&e->bus, 0, RAM_SIZE, e->ram, RAM_SIZE, 1);
    mc_bus_map_memory(&e->bus, 0x40000000u, 0x10000000u, e->rom, ROM_SIZE, 0);
    mc_bus_map_memory(&e->bus, 0xF9000000u, 0x01000000u, e->slot, SLOT_SIZE, 1);
    e->io = mc_bus_add_io(&e->bus, 0x50000000u, 0x10000000u, 0x000FFFFFu, 1);
    mc_bus_attach(&e->bus, e->io, 0, 0x100, e, &ctl_ops, MC_WIN_PORT8);
    mc_amu_init(&e->amu, mc_amu_mac_ii_map);
    put16(e->ram, 0x1000, program, sizeof program / sizeof program[0]);
    static const uint16_t slot0[] = {0x5280, 0x4E75}, slot16[] = {0x13FC, 0x0008, 0x50F0, 0x0000, 0x7C09, 0x4E75};
    static const uint16_t ram0[] = {0x5281, 0x4E75}, ram24[] = {0x7C07, 0x4E75};
    put16(e->slot, 0, slot0, 2);
    put16(e->slot, 0x10, slot16, 6);
    put16(e->ram, 0x900000, ram0, 2);
    put16(e->ram, 0x900018, ram24, 2);
    put32(e->ram, 0x800000, 0x0BADBEEF);
    put32(e->rom, 0, 0xCAFEF00D);
    put32(e->ram, 0x2000, 0x12345678);
    m68k_bus mb;
    memset(&mb, 0, sizeof mb);
    mb.ctx = e;
    mb.read_map[0] = mb.read_map[1] = e->bus.read_map;
    mb.write_map[0] = mb.write_map[1] = e->bus.write_map;
    mb.read = cpu_read;
    mb.write = cpu_write;
    m68k_init(&e->cpu, M68K_68020, &mb);
    set24(e, 1);
    m68k_set_sr(&e->cpu, 0x2700);
    e->cpu.r[15] = 0x8000;
    e->cpu.pc = 0x1000;
}

static void release(env *e) {
    m68k_free(&e->cpu);
    mc_amu_free(&e->amu);
    mc_bus_free(&e->bus);
    free(e->ram);
    free(e->rom);
    free(e->slot);
}

static void test_map(void) {
    env *e = malloc(sizeof *e);
    setup(e);
    mc_amu *a = &e->amu;
    CHECK_EQ(a->on, 1);
    CHECK(a->read_map[0x00012] == e->ram + 0x12000);
    CHECK(a->read_map[0xFF012] == e->ram + 0x12000);        /* A31-A24 ignored */
    CHECK(a->write_map[0xAB7FF] == e->ram + 0x7FF000);      /* the last page of RAM */
    CHECK(a->read_map[0x00800] == e->rom);                   /* the ROM at $800000 */
    CHECK(a->write_map[0x00800] == NULL);
    CHECK(a->read_map[0x12900] == e->slot);                  /* slot 9 */
    CHECK(a->read_map[0x00F00] == NULL);                     /* I/O: the slow path */
    CHECK(a->read_map[0x00A00] == NULL);                     /* slot $A: nothing there */
    CHECK_EQ(mc_amu_map(a, 0xABF00010u), 0x50000010u);
    CHECK_EQ(mc_amu_map(a, 0x00E12345u), 0xFE012345u);
    CHECK_EQ(mc_amu_map(a, 0x00812345u), 0x40012345u);
    /* The bus's tables change (a remap): rebuilt while on, deferred while
     * off and built at the next switch. */
    mc_bus_unmap(&e->bus, 0xF9000000u, 0x01000000u);
    mc_amu_bus_changed(a, &e->bus);
    CHECK(a->read_map[0x00900] == NULL);
    CHECK_EQ(mc_amu_set(a, &e->bus, 0), 1);
    mc_bus_map_memory(&e->bus, 0xF9000000u, 0x01000000u, e->slot, SLOT_SIZE, 1);
    mc_amu_bus_changed(a, &e->bus);
    CHECK_EQ(a->stale, 1);
    CHECK_EQ(mc_amu_set(a, &e->bus, 0), 0);
    CHECK_EQ(mc_amu_set(a, &e->bus, 1), 1);
    CHECK(a->read_map[0x77900] == e->slot);
    release(e);
    free(e);
}

/* The CPU's tables follow the switch: the interpreter reads through the
 * decode's tables in 24-bit mode and the bus's in 32-bit mode; the
 * translator's soft TLB is emptied by the switch. */
static void test_switch_tables(void) {
    env *e = malloc(sizeof *e);
    setup(e);
    CHECK(e->cpu.rmap == e->amu.read_map);
    set24(e, 0);
    CHECK(e->cpu.rmap == e->bus.read_map);
    set24(e, 1);
    if (m68k_jit_enable(&e->cpu, 1)) {
        m68k_jit_threshold(&e->cpu, 0);
        m68k_run(&e->cpu, 4);                               /* LEA, JSR, ADDQ in the slot, RTS */
        CHECK(e->cpu.pmap[0x00900] != NULL);                /* the slot page, translated through the decode */
        CHECK(e->cpu.pmap[0x00900] == e->slot);
        set24(e, 0);
        CHECK(e->cpu.pmap[0x00900] == NULL);                /* emptied by the switch */
        m68k_run(&e->cpu, 3);                               /* MOVE.B (to 32-bit: no change), JSR, ADDQ in RAM */
        CHECK(e->cpu.pmap[0x00900] == e->ram + 0x900000);
    }
    release(e);
    free(e);
}

static uint32_t rnd_step(uint64_t *rng) { *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17; return (uint32_t)(*rng >> 16); }

/* The program on the interpreter: per iteration D0 and D1 count, D2 the
 * RAM's long word, D3 the ROM's, D4 RAM $2000's, D6 7, D5 the
 * iterations; then the translator against it. */
static int diff_case(uint64_t seed) {
    uint64_t rng = seed * 0x9E3779B97F4A7C15ull + 3;
#define RND() rnd_step(&rng)
    env *x = malloc(sizeof *x), *y = malloc(sizeof *y);
    setup(x);
    setup(y);
    if (!m68k_jit_enable(&y->cpu, 1)) {
        release(x); release(y); free(x); free(y);
        return -1;
    }
    m68k_jit_options(&y->cpu, RND() % 4 != 0, (int)(RND() & 1), RND() % 4 == 0 ? 1 + RND() % 8 : 0);
    m68k_jit_threshold(&y->cpu, RND() % 2 ? 0 : 32);
    int fail = 0;
    for (unsigned n = 0; n < 200 && !fail; n++) {
        const int64_t len = 1 + (int64_t)(RND() % (RND() % 4 == 0 ? 4 : 300));
        m68k_run(&x->cpu, len);
        m68k_run(&y->cpu, len);
        if (memcmp(x->cpu.r, y->cpu.r, sizeof x->cpu.r) || x->cpu.pc != y->cpu.pc || x->cpu.instructions != y->cpu.instructions ||
            m68k_sr(&x->cpu) != m68k_sr(&y->cpu) || x->amu.on != y->amu.on || memcmp(x->ram, y->ram, 0x10000) ||
            x->switches != y->switches) {
            printf("  case %llu slice %u: PC %08X/%08X D0 %08X/%08X D1 %08X/%08X D6 %08X/%08X mode %d/%d\n",
                   (unsigned long long)seed, n, (unsigned)x->cpu.pc, (unsigned)y->cpu.pc, (unsigned)x->cpu.r[0],
                   (unsigned)y->cpu.r[0], (unsigned)x->cpu.r[1], (unsigned)y->cpu.r[1], (unsigned)x->cpu.r[6],
                   (unsigned)y->cpu.r[6], x->amu.on, y->amu.on);
            fail = 1;
        }
    }
    /* The interpreter against the map: after whole iterations. */
    const uint32_t k = x->cpu.r[5];
    CHECK(k > 50);
    CHECK(x->cpu.r[0] == k || x->cpu.r[0] == k + 1);
    CHECK(x->cpu.r[1] == k || x->cpu.r[1] == k + 1);
    if (k) {
        CHECK_EQ(x->cpu.r[2], 0x0BADBEEF);
        CHECK_EQ(x->cpu.r[3], 0xCAFEF00D);
        CHECK_EQ(x->cpu.r[4], 0x12345678);
        CHECK_EQ(x->cpu.r[6], 7);
    }
    CHECK_EQ(x->cpu.status, M68K_OK);
    release(x);
    release(y);
    free(x);
    free(y);
    return fail;
#undef RND
}

static void test_diff(void) {
    unsigned failed = 0, cases = 60;
    for (unsigned i = 0; i < cases; i++) {
        const int r = diff_case(1 + i);
        if (r < 0) {
            printf("  translator not available: interpreter only\n");
            return;
        }
        failed += (unsigned)r;
    }
    printf("  %u programs switching modes, %u differ between the interpreter and the translator\n", cases, failed);
    CHECK_EQ(failed, 0);
}

int main(void) {
    test_map();
    test_switch_tables();
    test_diff();
    printf("test_amu: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
