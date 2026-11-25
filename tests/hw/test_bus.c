/* The bus, the scheduler and the interrupt router. Livadix's own tests. */
#include "hw_test.h"
#include "../../src/hw/bus.h"
#include "../../src/hw/irq.h"
#include "../../src/hw/sched.h"

#include <stdlib.h>

typedef struct reg_dev {
    mc_dev dev;
    uint32_t last_offset, last_value;
    unsigned last_size, reads, writes;
} reg_dev;

static uint32_t reg_read(void *dev, uint32_t offset, unsigned size) {
    reg_dev *d = dev;
    d->last_offset = offset;
    d->last_size = size;
    d->reads++;
    return offset & 0xFFu;      /* each byte reads as the low byte of its offset */
}
static void reg_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    reg_dev *d = dev;
    d->last_offset = offset;
    d->last_size = size;
    d->last_value = value;
    d->writes++;
}
static const mc_dev_ops reg_ops = {reg_read, reg_write, NULL, NULL};

static void test_memory(void) {
    mc_bus bus;
    CHECK(mc_bus_init(&bus));
    uint8_t *ram = calloc(1, 0x4000), *rom = calloc(1, 0x2000);
    for (unsigned i = 0; i < 0x2000; i++) rom[i] = (uint8_t)(i * 7);
    /* 16 KiB of RAM in a 64 KiB window: it repeats every 16 KiB. */
    CHECK(mc_bus_map_memory(&bus, 0x10000, 0x10000, ram, 0x4000, 1));
    CHECK(mc_bus_map_memory(&bus, 0x40000000, 0x8000, rom, 0x2000, 0));
    uint32_t v = 0;
    CHECK_EQ(mc_bus_write(&bus, 0x10004, 4, 0x11223344), 0);
    CHECK_EQ(mc_bus_read(&bus, 0x14004, 4, &v), 0);
    CHECK_EQ(v, 0x11223344);
    CHECK_EQ(mc_bus_read(&bus, 0x1C005, 1, &v), 0);
    CHECK_EQ(v, 0x22);
    CHECK(bus.read_map[0x10] == bus.read_map[0x14] && bus.write_map[0x10] == ram);
    /* ROM: readable, repeated, writes ignored without a bus error. */
    CHECK_EQ(mc_bus_read(&bus, 0x40002003, 1, &v), 0);
    CHECK_EQ(v, (uint8_t)(3 * 7));
    CHECK_EQ(mc_bus_write(&bus, 0x40000003, 1, 0xEE), 0);
    CHECK_EQ(rom[3], (uint8_t)(3 * 7));
    CHECK(bus.write_map[0x40000] == NULL);
    /* Unmapped: a bus error both ways. */
    CHECK(mc_bus_read(&bus, 0x30000, 2, &v) != 0);
    CHECK(mc_bus_write(&bus, 0x30000, 2, 0) != 0);
    mc_bus_unmap(&bus, 0x10000, 0x10000);
    CHECK(mc_bus_read(&bus, 0x10004, 4, &v) != 0);
    mc_bus_free(&bus);
    free(ram);
    free(rom);
}

static void test_io(void) {
    mc_bus bus;
    CHECK(mc_bus_init(&bus));
    reg_dev a = {{&reg_ops, NULL, "a"}, 0, 0, 0, 0, 0}, b = a, c = a;
    /* A block repeated every 256 KiB through 1 MiB, with three windows. */
    const int r = mc_bus_add_io(&bus, 0x50000000, 0x100000, 0x3FFFF, 0);
    CHECK(r > 0);
    CHECK(mc_bus_attach(&bus, r, 0x0000, 0x2000, &a, &reg_ops, MC_WIN_PORT8));
    CHECK(mc_bus_attach(&bus, r, 0x2000, 0x2000, &b, &reg_ops, 0));
    CHECK(mc_bus_attach(&bus, r, 0x4000, 0x1000, &c, &reg_ops, MC_WIN_BUSERR));
    uint32_t v = 0;
    /* Device pages stay off the CPU's tables. */
    CHECK(bus.read_map[0x50000] == NULL && bus.write_map[0x50000] == NULL);
    /* An 8-bit port sees a longword as four byte cycles. */
    CHECK_EQ(mc_bus_read(&bus, 0x50001C00, 4, &v), 0);
    CHECK_EQ(a.reads, 4);
    CHECK_EQ(a.last_size, 1);
    CHECK_EQ(a.last_offset, 0x1C03);
    CHECK_EQ(v, 0x00010203);
    CHECK_EQ(mc_bus_write(&bus, 0x50000200, 2, 0xABCD), 0);
    CHECK_EQ(a.writes, 2);
    CHECK_EQ(a.last_value, 0xCD);
    /* The image repeats: $50042000 is window b at offset 0. */
    CHECK_EQ(mc_bus_read(&bus, 0x50042010, 2, &v), 0);
    CHECK_EQ(b.last_offset, 0x10);
    CHECK_EQ(b.last_size, 2);
    CHECK_EQ(v, 0x10);
    /* A window that bus-errors, and an offset no window claims. */
    CHECK(mc_bus_read(&bus, 0x50004000, 1, &v) != 0);
    CHECK(mc_bus_write(&bus, 0x50004000, 1, 0) != 0);
    CHECK(mc_bus_read(&bus, 0x50008000, 1, &v) != 0);
    /* Past the region: unmapped. */
    CHECK(mc_bus_read(&bus, 0x50100000, 1, &v) != 0);
    mc_bus_free(&bus);
}

static unsigned order[8], norder;
static void tick(void *dev, uint64_t now) {
    (void)now;
    order[norder++] = (unsigned)(uintptr_t)dev;
}

static void test_sched(void) {
    mc_sched s;
    mc_sched_init(&s);
    int slot[6];
    for (unsigned i = 0; i < 6; i++) slot[i] = mc_sched_add(&s, (void *)(uintptr_t)(i + 1), tick);
    CHECK_EQ(mc_sched_add(&s, (void *)(uintptr_t)3, tick), slot[2]);
    CHECK_EQ(mc_sched_next(&s), MC_NEVER);
    mc_sched_set(&s, slot[0], 500);
    mc_sched_set(&s, slot[1], 100);
    mc_sched_set(&s, slot[2], 300);
    mc_sched_set(&s, slot[3], 200);
    mc_sched_set(&s, slot[4], 400);
    mc_sched_set(&s, slot[5], 50);
    CHECK_EQ(mc_sched_next(&s), 50);
    mc_sched_set(&s, slot[5], MC_NEVER);        /* cancelled */
    mc_sched_set(&s, slot[0], 150);             /* moved earlier */
    CHECK_EQ(mc_sched_next(&s), 100);
    norder = 0;
    CHECK_EQ(mc_sched_run(&s, 350), 4);
    CHECK_EQ(norder, 4);
    CHECK_EQ(order[0], 2);
    CHECK_EQ(order[1], 1);
    CHECK_EQ(order[2], 4);
    CHECK_EQ(order[3], 3);
    CHECK_EQ(mc_sched_next(&s), 400);
    CHECK_EQ(mc_sched_run(&s, 399), 0);
    CHECK_EQ(mc_sched_run(&s, 1000), 1);
    CHECK_EQ(mc_sched_next(&s), MC_NEVER);
}

static unsigned seen_ipl, ipl_calls;
static void ipl_changed(void *ctx, unsigned ipl) {
    (void)ctx;
    seen_ipl = ipl;
    ipl_calls++;
}

static void test_irq(void) {
    mc_irq irq;
    mc_irq_init(&irq, ipl_changed, NULL);
    const int via1 = mc_irq_add(&irq, 1), via2 = mc_irq_add(&irq, 2), scc = mc_irq_add(&irq, 4);
    CHECK(via1 >= 0 && via2 >= 0 && scc >= 0);
    CHECK_EQ(mc_irq_add(&irq, 8), -1);
    mc_irq_set(&irq, via2, 1);
    CHECK_EQ(seen_ipl, 2);
    mc_irq_set(&irq, via1, 1);
    CHECK_EQ(seen_ipl, 2);
    CHECK_EQ(ipl_calls, 1);         /* the level did not change */
    mc_irq_set(&irq, scc, 1);
    CHECK_EQ(seen_ipl, 4);
    mc_irq_set(&irq, scc, 0);
    mc_irq_set(&irq, via2, 0);
    CHECK_EQ(seen_ipl, 1);
    mc_irq_set(&irq, via1, 0);
    CHECK_EQ(mc_irq_level(&irq), 0);
    CHECK_EQ(seen_ipl, 0);
}

int mc_test_bus(void) {
    test_memory();
    test_io();
    test_sched();
    test_irq();
    return 0;
}
