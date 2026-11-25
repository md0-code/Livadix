/* test_primetime: PrimeTime's register block and the CPU ID register
 * (primetime.h): 16-bit registers every $100 bytes, the ROM's timing
 * writes (a byte at +$000, a word at +$600, bit 3 of +$100), byte lanes,
 * the reset, and the CPU ID register of each model under the ROM's probe
 * (ROM offset $4BE4) and its clock code (ROM offset $4958). */
#include "hw_test.h"
#include "asic/primetime.h"

static int tests;
static hw_fake fake;
static mc_primetime pt;

static uint32_t rd(uint32_t offset, unsigned size) { return mc_primetime_ops.read(&pt, offset, size); }
static void wr(uint32_t offset, unsigned size, uint32_t value) { mc_primetime_ops.write(&pt, offset, size, value); }

static void test_registers(void) {
    tests++;
    hw_fake_init(&fake);
    mc_primetime_init(&pt, &fake.host, "PrimeTime", MC_PRIMETIME_ID_Q605);
    CHECK_EQ(rd(0x000, 2), 0);
    /* The ROM's timing values for a 25 MHz board (ROM offset $498C). */
    wr(0x000, 1, 0x01);
    wr(0x600, 2, 0x01E0);
    CHECK_EQ(rd(0x000, 1), 0x01);
    CHECK_EQ(rd(0x000, 2), 0x0100);
    CHECK_EQ(rd(0x600, 2), 0x01E0);
    CHECK_EQ(rd(0x600, 4), 0x01E001E0u);
    /* Bit 3 of +$100 (ROM offset $49B2) is a register of its own. */
    wr(0x100, 1, 0x08);
    CHECK_EQ(rd(0x100, 1), 0x08);
    CHECK_EQ(rd(0x000, 1), 0x01);
    /* Byte lanes: an even address is the high byte; a longword keeps its
     * low word. */
    wr(0x201, 1, 0x3F);
    CHECK_EQ(rd(0x200, 2), 0x003F);
    wr(0x300, 4, 0x12345678u);
    CHECK_EQ(rd(0x300, 2), 0x5678);
    /* Address bits 0-7 do not select, and the block repeats. */
    CHECK_EQ(rd(0x6FE, 2), 0x01E0);
    CHECK_EQ(rd(0x2600, 2), 0x01E0);
    CHECK(mc_primetime_ops.stable(&pt, 0x600, 2));
    mc_primetime_ops.reset(&pt);
    CHECK_EQ(rd(0x600, 2), 0);
    CHECK_EQ(rd(0x100, 1), 0);
}

/* The probe: read, write 0, compare, write $FFFFFFFF, compare, then $A55A
 * in the high word; the low word selects the ProductInfo record. */
static void probe(uint16_t id, unsigned clock) {
    hw_fake_init(&fake);
    mc_primetime_init(&pt, &fake.host, "PrimeTime", id);
    const mc_dev_ops *o = &mc_primetime_cpu_id_ops;
    const uint32_t first = o->read(&pt, 0, 4);
    o->write(&pt, 0, 4, 0);
    CHECK_EQ(o->read(&pt, 0, 4), first);
    o->write(&pt, 0, 4, 0xFFFFFFFFu);
    CHECK_EQ(o->read(&pt, 0, 4), first);
    CHECK_EQ(first >> 16, 0xA55Au);
    CHECK_EQ(first & 0xFFFFu, id);
    CHECK_EQ(first & 3u, clock);
    CHECK_EQ(o->read(&pt, 2, 2), id);
    CHECK_EQ(o->read(&pt, 3, 1), (uint32_t)(id & 0xFFu));
    CHECK_EQ(o->read(&pt, 0, 1), 0xA5u);
}

static void test_cpu_id(void) {
    tests++;
    probe(MC_PRIMETIME_ID_LC475, 1);      /* box 83, 25 MHz */
    probe(MC_PRIMETIME_ID_Q605, 1);       /* box 88, 25 MHz */
    probe(MC_PRIMETIME_ID_LC575, 2);      /* box 86, 33 MHz */
}

int main(void) {
    test_registers();
    test_cpu_id();
    printf("test_primetime: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
