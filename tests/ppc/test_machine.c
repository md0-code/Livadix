/* The PowerPC machine back end (src/hw/ppc): the bus as the CPU sees it
 * (widths, sized ports, byte lanes, unanswered accesses as transfer errors:
 * MPC601UM 5.4.2, 9.3.2.3 table 9-2, 9.4.3.2), the scheduler and guest
 * time on the processor clock, the external interrupt input (9.7.1), and
 * the time base and decrementer counted from guest time (MPC601UM 2.2.5,
 * MPC604EUM 2.1.2). `test_machine probe [ROM]` runs pm-6100.rom on the PDM
 * description of RAM and ROM to its first hardware probe.
 *
 * Livadix's own code. */
#include "ppc/ppc_machine.h"
#include "ppc/ppc_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

/* --- a device that logs its accesses --------------------------------------------- */

typedef struct access { uint32_t offset, value; unsigned size; int write; } access;
typedef struct fake {
    mc_dev dev;
    access log[16];
    unsigned n;
    uint64_t fired;         /* the guest time its event came, 0: none */
    uint64_t at;            /* the deadline it armed */
    int polling;
    uint32_t last_read;
} fake;

static uint32_t fake_read(void *p, uint32_t offset, unsigned size) {
    fake *f = p;
    if (f->polling && offset == 4 && size == 4) {
        f->last_read = f->fired ? 0u : 0x00040000u;
        return f->last_read;
    }
    const uint32_t v = size == 1 ? 0xA0u + (offset & 0x0Fu) : size == 2 ? 0xB000u + (offset & 0xFFu) : 0xC0000000u + offset;
    if (f->n < 16) f->log[f->n++] = (access){offset, v, size, 0};
    return v;
}
static void fake_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    fake *f = p;
    if (f->n < 16) f->log[f->n++] = (access){offset, value, size, 1};
}
static void fake_event(void *p, uint64_t now) {
    fake *f = p;
    f->fired = now;
    mc_dev_line(&f->dev, 0, 1);
}
static int fake_stable(void *p, uint32_t offset, unsigned size) {
    const fake *f = p;
    return f->polling && offset == 4 && size == 4 && f->last_read == (f->fired ? 0u : 0x00040000u);
}
static const mc_dev_ops fake_ops = {fake_read, fake_write, fake_event, NULL, fake_stable};

/* --- the test board ------------------------------------------------------------------- */

enum { DIRECT, SIZED8, SIZED16, LANE0, LANE3, LANE2W, TIMER, NDEV };
typedef struct board { mc_ppc *m; fake dev[NDEV]; int bad_attach[2]; } board;
static board the_board;

static void *build(mc_ppc *m) {
    board *b = &the_board;
    const int io = mc_ppc_region(m, "io");
    memset(b, 0, sizeof *b);
    b->m = m;
    for (int i = 0; i < NDEV; i++) {
        b->dev[i].dev.ops = &fake_ops;
        b->dev[i].dev.host = &m->host;
        b->dev[i].dev.name = "fake";
    }
    if (!mc_ppc_attach(m, io, 0x0000, 0x100, &b->dev[DIRECT], &fake_ops, (mc_ppc_port){MC_PPC_PORT_DIRECT, 4, 0, 0}) ||
        !mc_ppc_attach(m, io, 0x1000, 0x100, &b->dev[SIZED8], &fake_ops, (mc_ppc_port){MC_PPC_PORT_SIZED, 1, 0, 0}) ||
        !mc_ppc_attach(m, io, 0x2000, 0x100, &b->dev[SIZED16], &fake_ops, (mc_ppc_port){MC_PPC_PORT_SIZED, 2, 0, 0}) ||
        !mc_ppc_attach(m, io, 0x3000, 0x100, &b->dev[LANE0], &fake_ops, (mc_ppc_port){MC_PPC_PORT_LANES, 1, 0, 0xFF}) ||
        !mc_ppc_attach(m, io, 0x4000, 0x100, &b->dev[LANE3], &fake_ops, (mc_ppc_port){MC_PPC_PORT_LANES, 1, 3, 0xFF}) ||
        !mc_ppc_attach(m, io, 0x5000, 0x100, &b->dev[LANE2W], &fake_ops, (mc_ppc_port){MC_PPC_PORT_LANES, 2, 2, 0x00}))
        return NULL;
    b->bad_attach[0] = mc_ppc_attach(m, io, 0x6000, 0x100, &b->dev[TIMER], &fake_ops, (mc_ppc_port){MC_PPC_PORT_LANES, 2, 1, 0});
    b->bad_attach[1] = mc_ppc_attach(m, io, 0x6004, 0x100, &b->dev[TIMER], &fake_ops, (mc_ppc_port){MC_PPC_PORT_DIRECT, 4, 0, 0});
    return b;
}
static void line(void *p, void *dev, int which, int state) {
    board *b = p;
    (void)which;
    if (dev == &b->dev[TIMER]) mc_ppc_set_interrupt(b->m, state);
}
static void destroy(void *p) { (void)p; }
static const mc_board_ops board_ops = {.line = line, .destroy = destroy};

#define ROM_ID 0x50504333u
static const uint32_t test_roms[] = {ROM_ID, 0};
static const uint16_t test_ram[] = {1, 0};
static const mc_region test_map[] = {
    {"ram", 0x00000000u, 0x10000000u, 0, MC_REGION_RAM},
    {"io", 0x50000000u, 0x00100000u, 0x000FFFFFu, MC_REGION_IO},
    {"empty", 0x60000000u, 0x00100000u, 0, MC_REGION_EMPTY},
    {"rom", 0xFFF00000u, 0x00100000u, 0, MC_REGION_ROM},
};
/* The processor at twice C15M: an instruction is half a C15M period. */
#define RATE (2u * MC_C15M_HZ)
static mc_ppc_desc test_desc = {
    .key = "test", .name = "Test board", .cpu = PPC_MODEL_601, .cpu_hz = RATE, .timebase_hz = 7812500u, .cpi = 1,
    .roms = test_roms, .ram_mb = test_ram, .map = test_map, .nmap = 4,
    .unmapped = MC_PPC_UNMAPPED_TEA, .build = build, .ops = &board_ops,
};

static uint8_t rom[65536];
static void put(uint32_t offset, uint32_t word) {
    rom[offset] = (uint8_t)(word >> 24); rom[offset + 1] = (uint8_t)(word >> 16);
    rom[offset + 2] = (uint8_t)(word >> 8); rom[offset + 3] = (uint8_t)word;
}
static uint32_t d_form(unsigned op, unsigned d, unsigned a, unsigned imm) { return op << 26 | d << 21 | a << 16 | (imm & 0xFFFFu); }
static uint32_t x_form(unsigned xo, unsigned d, unsigned a, unsigned b) { return 31u << 26 | d << 21 | a << 16 | b << 11 | xo << 1; }
#define LOOP 0x48000000u    /* b . */
static uint32_t spr_form(unsigned xo, unsigned reg, unsigned spr) { return x_form(xo, reg, spr & 31u, spr >> 5); }

static mc_ppc *create(void) {
    mc_config c;
    char error[256] = "";
    mc_config_defaults(&c);
    mc_config_set_ram_kb(&c, 1024);
    put(0, ROM_ID);
    mc_ppc *m = mc_ppc_create(&test_desc, &c, rom, sizeof rom, error, sizeof error);
    if (!m) fprintf(stderr, "create: %s\n", error);
    return m;
}
static uint32_t rd(mc_ppc *m, uint32_t a, unsigned size, int *ok) {
    uint32_t v = 0;
    *ok = m->cpu.bus.read(m->cpu.bus.opaque, a, size, &v);
    return v;
}
static int wr(mc_ppc *m, uint32_t a, unsigned size, uint32_t v) { return m->cpu.bus.write(m->cpu.bus.opaque, a, size, v); }
static fake *dev(int i) { the_board.dev[i].n = 0; return &the_board.dev[i]; }

static void bus_widths(void) {
    memset(rom, 0, sizeof rom);
    put(0x100, LOOP);
    mc_ppc *m = create();
    int ok;
    fake *f;
    CHECK(m != NULL);
    if (!m) return;
    CHECK(!the_board.bad_attach[0] && !the_board.bad_attach[1]);
    /* Memory: RAM, ROM through its window, ROM writes ignored. */
    CHECK(wr(m, 0x1000, 4, 0x11223344u) && rd(m, 0x1002, 2, &ok) == 0x3344u && ok);
    CHECK(rd(m, 0x1000, 1, &ok) == 0x11u && ok);
    CHECK(rd(m, 0xFFF00000u, 4, &ok) == ROM_ID && ok);
    CHECK(rd(m, 0xFFF10000u, 4, &ok) == ROM_ID && ok);
    CHECK(wr(m, 0xFFF00000u, 4, 0) && rd(m, 0xFFF00000u, 4, &ok) == ROM_ID);
    /* A device taking accesses as made. */
    f = dev(DIRECT);
    CHECK(rd(m, 0x50000010u, 4, &ok) == 0xC0000010u && ok && f->n == 1 && f->log[0].size == 4 && f->log[0].offset == 0x10);
    CHECK(rd(m, 0x50000013u, 1, &ok) == 0xA3u && f->n == 2 && f->log[1].size == 1);
    CHECK(wr(m, 0x50000012u, 2, 0xBEEFu) && f->n == 3 && f->log[2].write && f->log[2].offset == 0x12 && f->log[2].value == 0xBEEFu);
    /* An 8-bit port behind a sizing bridge: four byte cycles, big-endian. */
    f = dev(SIZED8);
    CHECK(rd(m, 0x50001004u, 4, &ok) == 0xA4A5A6A7u && f->n == 4);
    CHECK(f->log[0].offset == 4 && f->log[3].offset == 7 && f->log[3].size == 1);
    f = dev(SIZED8);
    CHECK(wr(m, 0x50001004u, 4, 0x11223344u) && f->n == 4);
    CHECK(f->log[0].value == 0x11u && f->log[1].offset == 5 && f->log[1].value == 0x22u && f->log[3].value == 0x44u);
    f = dev(SIZED8);
    CHECK(rd(m, 0x50001009u, 1, &ok) == 0xA9u && f->n == 1);
    /* A 16-bit port: halfword cycles. */
    f = dev(SIZED16);
    CHECK(rd(m, 0x50002008u, 4, &ok) == 0xB008B00Au && f->n == 2 && f->log[1].offset == 10 && f->log[1].size == 2);
    f = dev(SIZED16);
    CHECK(wr(m, 0x50002008u, 4, 0xAAAABBBBu) && f->n == 2 && f->log[0].value == 0xAAAAu && f->log[1].value == 0xBBBBu);
    f = dev(SIZED16);
    CHECK(rd(m, 0x5000200Bu, 1, &ok) == 0xABu && f->n == 1 && f->log[0].size == 1);
    /* An 8-bit chip on byte lane 0: other lanes float (fill FF). */
    f = dev(LANE0);
    CHECK(rd(m, 0x50003010u, 1, &ok) == 0xA0u && f->n == 1 && f->log[0].offset == 0x10);
    f = dev(LANE0);
    CHECK(rd(m, 0x50003011u, 1, &ok) == 0xFFu && ok && f->n == 0);
    CHECK(rd(m, 0x50003010u, 4, &ok) == 0xA0FFFFFFu && f->n == 1 && f->log[0].size == 1);
    f = dev(LANE0);
    CHECK(rd(m, 0x50003014u, 4, &ok) == 0xFFFFFFFFu && f->n == 0);
    CHECK(wr(m, 0x50003010u, 4, 0x12345678u) && f->n == 1 && f->log[0].value == 0x12u && f->log[0].offset == 0x10);
    f = dev(LANE0);
    CHECK(wr(m, 0x50003012u, 2, 0x5678u) && f->n == 0);
    /* On byte lane 3. */
    f = dev(LANE3);
    CHECK(rd(m, 0x50004000u, 4, &ok) == 0xFFFFFFA3u && f->n == 1 && f->log[0].offset == 3);
    CHECK(rd(m, 0x50004003u, 1, &ok) == 0xA3u);
    f = dev(LANE3);
    CHECK(wr(m, 0x50004002u, 2, 0xAABBu) && f->n == 1 && f->log[0].offset == 3 && f->log[0].value == 0xBBu);
    f = dev(LANE3);
    CHECK(rd(m, 0x50004004u, 4, &ok) == 0xFFFFFFFFu && f->n == 0);
    /* A 16-bit chip on lanes 2-3 (fill 00). */
    f = dev(LANE2W);
    CHECK(rd(m, 0x50005000u, 4, &ok) == 0x0000B002u && f->n == 1 && f->log[0].size == 2 && f->log[0].offset == 2);
    f = dev(LANE2W);
    CHECK(rd(m, 0x50005003u, 1, &ok) == 0xA3u && f->n == 1 && f->log[0].size == 1);
    f = dev(LANE2W);
    CHECK(wr(m, 0x50005000u, 4, 0x11223344u) && f->n == 1 && f->log[0].value == 0x3344u && f->log[0].offset == 2);
    /* Unanswered: an IO offset with no window, an empty region, nothing:
       transfer errors, the first one kept. */
    CHECK(!m->probe.hit);
    rd(m, 0x50007000u, 4, &ok);
    CHECK(!ok && m->probe.hit && m->probe.address == 0x50007000u && m->probe.size == 4 && !m->probe.write);
    CHECK(!wr(m, 0x60000000u, 2, 0x1234u) && m->probe.address == 0x50007000u);
    rd(m, 0x70000000u, 1, &ok);
    CHECK(!ok);
    /* Bus mastering into RAM and from ROM, and through a device. */
    {
        uint8_t block[16], back[16];
        for (int i = 0; i < 16; i++) block[i] = (uint8_t)(i * 3);
        CHECK(mc_ppc_dma(m, 0x2FF8, block, 16, 1) && mc_ppc_dma(m, 0x2FF8, back, 16, 0) && !memcmp(block, back, 16));
        CHECK(mc_ppc_dma(m, 0xFFF00000u, back, 4, 0) && back[0] == 0x50 && back[3] == 0x33);
        f = dev(SIZED8);
        CHECK(mc_ppc_dma(m, 0x50001000u, back, 2, 0) && back[1] == 0xA1 && f->n == 2);
        f = dev(DIRECT);
        CHECK(mc_ppc_dma(m, 0x50000000u, block, 4, 1));
        CHECK(f->n == 1 && f->log[0].size == 4 && f->log[0].value == 0x00030609);
        f = dev(DIRECT);
        CHECK(mc_ppc_dma(m, 0x50000000u, back, 4, 0));
        CHECK(f->n == 1 && f->log[0].size == 4 && back[0] == 0xC0 && back[3] == 0);
        f = dev(DIRECT);
        CHECK(mc_ppc_dma(m, 0x50000001u, block, 7, 1));
        CHECK(f->n == 3 && f->log[0].size == 1 && f->log[1].size == 2 && f->log[2].size == 4);
        CHECK(f->log[1].offset == 2 && f->log[1].value == 0x0306);
        CHECK(f->log[2].offset == 4 && f->log[2].value == 0x090C0F12);
        CHECK(!mc_ppc_dma(m, 0x60000000u, back, 2, 0));
    }
    mc_ppc_destroy(m);
}

static void transfer_errors(void) {
    /* lwz r3,0(r4) from the empty region: the machine check, MSR[IP] set at
       reset, SRR0 the load (MPC601UM 5.4.2); the run goes on. */
    memset(rom, 0, sizeof rom);
    put(0x100, d_form(15, 4, 0, 0x6000));       /* lis r4,$6000 */
    put(0x104, d_form(32, 3, 4, 0));            /* lwz r3,0(r4) */
    put(0x108, LOOP);
    put(0x200, LOOP);
    mc_ppc *m = create();
    CHECK(m != NULL);
    if (!m) return;
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_OK);
    CHECK(m->cpu.pc == 0xFFF00200u && m->cpu.srr0 == 0xFFF00104u && !(m->cpu.msr & PPC_MSR_ME));
    CHECK(m->probe.hit && m->probe.address == 0x60000000u && m->probe.pc == 0xFFF00104u && !m->probe.fetch);
    mc_ppc_destroy(m);
    /* With MSR[ME] clear: checkstop, the run stops for good. */
    put(0x100, d_form(15, 4, 0, 0x6000));
    put(0x104, x_form(146, 0, 0, 0));           /* mtmsr r0: ME, IP clear */
    put(0x108, d_form(32, 3, 4, 0));
    m = create();
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_ERROR);
    CHECK(strstr(mc_ppc_error(m), "checkstop") != NULL);
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_ERROR);
    mc_ppc_destroy(m);
    /* The bring-up policy: the run stops at the first unanswered access,
       reported. A store, and an instruction fetch. */
    test_desc.unmapped = MC_PPC_UNMAPPED_STOP;
    put(0x104, d_form(14, 3, 0, 0x5A));         /* li r3,$5A */
    put(0x108, d_form(38, 3, 4, 2));            /* stb r3,2(r4) */
    m = create();
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_ERROR);
    CHECK(!strcmp(mc_ppc_error(m), "First hardware probe: write of 1 byte at 60000002 by the instruction at FFF00108: no device answers (value 5A)"));
    CHECK(m->cpu.instructions == 2);
    mc_ppc_destroy(m);
    put(0x104, d_form(15, 3, 0, 0x6000));       /* lis r3,$6000 */
    put(0x108, x_form(467, 3, 9, 0));           /* mtctr r3 */
    put(0x10C, 0x4E800420u);                    /* bctr */
    m = create();
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_ERROR);
    CHECK(!strcmp(mc_ppc_error(m), "First hardware probe: instruction fetch of 4 bytes at 60000000 by the instruction at 60000000: no device answers"));
    mc_ppc_destroy(m);
    test_desc.unmapped = MC_PPC_UNMAPPED_TEA;
    /* An opcode the CPU does not have (primary 1). */
    put(0x100, 0x04000000u);
    m = create();
    CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_ERROR);
    CHECK(!strcmp(mc_ppc_error(m), "Opcode 04000000 at FFF00100 is not implemented"));
    mc_ppc_destroy(m);
}

static void time_and_events(void) {
    /* The 601's DEC is 0 after reset: set it far first (MPC601UM 2.7.1). */
    memset(rom, 0, sizeof rom);
    put(0x100, d_form(15, 4, 0, 0x7FFF));       /* lis r4,$7FFF */
    put(0x104, spr_form(467, 4, 22));           /* mtdec r4 */
    put(0x108, d_form(24, 3, 3, 0x9040));       /* ori r3,r3,$9040: EE, ME, IP */
    put(0x10C, x_form(146, 3, 0, 0));           /* mtmsr r3 */
    put(0x110, LOOP);
    put(0x500, LOOP);
    mc_ppc *m = create();
    CHECK(m != NULL);
    if (!m) return;
    /* Guest time is clocks x C15M / rate, exactly: 1001 clocks, 500.5. */
    CHECK(mc_ppc_run(m, 1001, 0) == MC_RUN_OK);
    CHECK(m->cycles == 1001 && mc_ppc_now(m) == 500 && m->cpu.instructions == 1001);
    CHECK(mc_ppc_run(m, 1, 0) == MC_RUN_OK && mc_ppc_now(m) == 501);
    /* A deadline cuts the slice: the device sees its time exactly; its
       line raises the external interrupt, taken with MSR[EE]. */
    fake *t = &the_board.dev[TIMER];
    mc_dev_schedule(&t->dev, 1234);
    CHECK(mc_ppc_run(m, 100000, 0) == MC_RUN_OK);
    CHECK(t->fired == 1234 && m->interrupt);
    CHECK(m->cpu.pc == 0xFFF00500u && m->cpu.srr0 == 0xFFF00110u && !(m->cpu.msr & PPC_MSR_EE));
    /* A run's deadline ends it there. */
    const uint64_t at = mc_ppc_now(m);
    CHECK(mc_ppc_run(m, 1000000, at + 100) == MC_RUN_OK && mc_ppc_now(m) == at + 100);
    mc_ppc_set_interrupt(m, 0);
    CHECK(!m->cpu.external_irq);
    /* The 601's RTC counts 128 ns ticks of guest time (MPC601UM 2.2.5.3). */
    {
        const uint64_t now = mc_ppc_now(m);
        const uint64_t ticks = now * 7812500u / MC_C15M_HZ;
        CHECK(m->tb_ticks == ticks);
        CHECK((uint64_t)m->cpu.rtcu * 1000000000u + m->cpu.rtcl == ticks * 128u);
    }
    mc_ppc_destroy(m);

    /* The 601's decrementer: 10 ticks (DEC's low seven bits are not
       counted), then the decrementer exception with MSR[EE]. */
    memset(rom, 0, sizeof rom);
    put(0x100, d_form(14, 4, 0, 0x500));        /* li r4,$500 */
    put(0x104, spr_form(467, 4, 22));           /* mtdec r4 */
    put(0x108, d_form(24, 3, 3, 0x9040));
    put(0x10C, x_form(146, 3, 0, 0));
    put(0x110, LOOP);
    put(0x900, LOOP);
    m = create();
    /* A tick is 31334400 / 7812500 = 4.01 clocks: after 30, 7 ticks (DEC
       $180); the eleventh, at clock 44.1, takes it below zero. */
    CHECK(mc_ppc_run(m, 30, 0) == MC_RUN_OK);
    CHECK(m->cpu.pc == 0xFFF00110u && m->cpu.dec == 0x180u && m->tb_ticks == 7);
    CHECK(mc_ppc_run(m, 14, 0) == MC_RUN_OK && m->cpu.pc == 0xFFF00110u);
    CHECK(mc_ppc_run(m, 400, 0) == MC_RUN_OK);
    CHECK(m->cpu.pc == 0xFFF00900u && m->cpu.srr0 == 0xFFF00110u);
    mc_ppc_destroy(m);

    /* A later model: the time base at timebase_hz, read with mftb. */
    test_desc.cpu = PPC_MODEL_604;
    test_desc.timebase_hz = 10000000u;
    memset(rom, 0, sizeof rom);
    put(0x100, LOOP);
    m = create();
    CHECK(mc_ppc_run(m, 300001, 0) == MC_RUN_OK);
    {
        const uint64_t now = mc_ppc_now(m);
        CHECK(m->cpu.time_base == now * 10000000u / MC_C15M_HZ);
        CHECK(m->cpu.dec == UINT32_MAX - (uint32_t)m->cpu.time_base);
    }
    mc_ppc_destroy(m);
    test_desc.cpu = PPC_MODEL_601;
    test_desc.timebase_hz = 7812500u;
}

/* A DMA wait ending at a timed device event: folding it must preserve the
 * ordinary execution's registers, instruction count, clock and event time. */
static void device_poll(void) {
    memset(rom, 0, sizeof rom);
    put(0x100, d_form(15, 8, 0, 0x5000));
    put(0x104, d_form(15, 4, 0, 0));
    put(0x108, d_form(24, 4, 4, 4));
    put(0x10C, x_form(534, 5, 8, 4));
    put(0x110, 0x7C0006ACu);
    put(0x114, d_form(28, 5, 4, 0x800));
    put(0x118, 0x40820010u);
    put(0x11C, d_form(28, 5, 5, 0x400));
    put(0x120, 0x4082FFE4u);
    put(0x124, d_form(14, 6, 0, 123));
    put(0x128, LOOP);
    ppc_cpu ordinary = {0};
    uint64_t clock = 0, fired = 0;
    for (int fold = 0; fold < 2; ++fold) {
        mc_ppc *m = create();
        CHECK(m != NULL);
        if (!m) return;
        m->poll_disabled = !fold;
        m->bus.region[mc_ppc_region(m, "io")].cost = 3;
        fake *f = &the_board.dev[DIRECT];
        f->polling = 1;
        mc_dev_schedule(&f->dev, 1001);
        CHECK(mc_ppc_run(m, 10000, 0) == MC_RUN_OK);
        CHECK(m->cpu.gpr[6] == 123 && f->fired >= 1001);
        ppc_cpu state = m->cpu;
        memset(&state.bus, 0, sizeof state.bus);
        if (!fold) { ordinary = state; clock = mc_ppc_now(m); fired = f->fired; }
        else {
            CHECK(!m->jit || m->poll_steps > 0);   /* the folding is the translator loop's */
            CHECK(!memcmp(&ordinary, &state, sizeof state));
            CHECK(clock == mc_ppc_now(m) && fired == f->fired);
        }
        mc_ppc_destroy(m);
    }
}

/* Hardware doze stops retirement, not guest time or interrupt delivery. */
static void power_wait(void) {
    const ppc_model families[] = {PPC_MODEL_603E, PPC_MODEL_604, PPC_MODEL_750};
    memset(rom, 0, sizeof rom);
    put(0x100, LOOP); put(0x500, LOOP); put(0x900, LOOP);
    for (unsigned i = 0; i < sizeof families / sizeof *families; ++i) {
        test_desc.cpu = families[i];
        mc_ppc *m = create(); CHECK(m != NULL); if (!m) continue;
        m->cpu.hid[0] = families[i] == PPC_MODEL_604 ? 0 : 0x00800000u;
        m->cpu.msr = PPC_MSR_IP | PPC_MSR_EE | PPC_MSR_POW;
        CHECK(ppc_step(&m->cpu) == PPC_OK && m->cpu.instructions == 0);
        if (m->jit) CHECK(ppc_jit_run(m->jit, 10) == 0);
        fake *t = &the_board.dev[TIMER]; mc_dev_schedule(&t->dev, 100);
        CHECK(mc_ppc_run(m, 150, 0) == MC_RUN_OK);
        CHECK(m->cpu.instructions == 0 && m->cpu.pc == 0xFFF00100u && m->cpu.time_base > 0);
        CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_OK);
        CHECK(t->fired == 100 && m->cpu.pc == 0xFFF00500u);
        CHECK(m->cpu.srr0 == 0xFFF00100u && !(m->cpu.msr & PPC_MSR_POW));
        mc_ppc_set_interrupt(m, 0);
        m->cpu.pc = 0xFFF00100u; m->cpu.msr = PPC_MSR_IP | PPC_MSR_EE | PPC_MSR_POW;
        m->cpu.dec = 5; m->cpu.decrementer_pending = false;
        const uint64_t instructions = m->cpu.instructions;
        CHECK(mc_ppc_run(m, 20, 0) == MC_RUN_OK && m->cpu.instructions == instructions);
        CHECK(mc_ppc_run(m, 100, 0) == MC_RUN_OK);
        CHECK(m->cpu.srr0 == 0xFFF00100u && m->cpu.pc == 0xFFF00900u && !(m->cpu.msr & PPC_MSR_POW));
        mc_ppc_destroy(m);
    }
    test_desc.cpu = PPC_MODEL_601;
}

static int probe(const char *path) {
    char buffer[512];
    const char *roms = getenv("LIVADIX_ROMS");
    if (!path) {
        if (!roms) { printf("SKIP: LIVADIX_ROMS is not set\n"); return 77; }
        snprintf(buffer, sizeof buffer, "%s/power-macintosh-6100.rom", roms);
        path = buffer;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { printf("SKIP: %s is absent\n", path); return 77; }
    static uint8_t image[4u << 20];
    const size_t size = fread(image, 1, sizeof image, f);
    fclose(f);
    mc_config c;
    char error[256] = "";
    mc_config_defaults(&c);
    mc_config_set_ram_kb(&c, 8u << 10);
    mc_ppc *m = mc_ppc_create(&mc_pdm_desc, &c, image, size, error, sizeof error);
    if (!m) { printf("FAIL: %s\n", error); return 1; }
    mc_run_status st = MC_RUN_OK;
    for (int i = 0; i < 200 && st == MC_RUN_OK; i++) st = mc_ppc_run(m, 1000000, 0);
    printf("%s after %llu instructions\n", st == MC_RUN_ERROR ? mc_ppc_error(m) : "no probe",
           (unsigned long long)m->cpu.instructions);
    const int ok = st == MC_RUN_ERROR && m->probe.hit && !strncmp(mc_ppc_error(m), "First hardware probe", 20);
    mc_ppc_destroy(m);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "probe")) return probe(argc > 2 ? argv[2] : NULL);
    bus_widths();
    transfer_errors();
    time_and_events();
    device_poll();
    power_wait();
    printf("PowerPC machine: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
