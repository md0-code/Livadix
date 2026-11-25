/* test_primetime2: PrimeTime II's configuration registers (primetime2.h):
 * 16-bit registers one every $100 bytes, as the ROM writes them (a byte at
 * +$000, the word $00D5 at +$600, the bytes $90 and $B7 at +$500 and
 * +$501, bit 1 of +$100 set and cleared), the byte lanes, a longword's
 * low word, the window's repetition and the reset. Against a fake host. */
#include "hw_test.h"
#include "asic/primetime2.h"

static int tests;
static hw_fake fake;
static mc_primetime2 pt;

static uint32_t rd(uint32_t offset, unsigned size) { return mc_primetime2_ops.read(&pt, offset, size); }
static void wr(uint32_t offset, unsigned size, uint32_t value) { mc_primetime2_ops.write(&pt, offset, size, value); }

static void test_registers(void) {
    tests++;
    hw_fake_init(&fake);
    mc_primetime2_init(&pt, &fake.host, "PrimeTime II");
    /* The ROM's 68040 start (ROM offset $4986). */
    wr(0x000, 1, 0x00);
    wr(0x600, 2, 0x00D5);
    wr(0x500, 1, 0x90);
    wr(0x501, 1, 0xB7);
    CHECK_EQ(rd(0x600, 2), 0x00D5);
    CHECK_EQ(rd(0x601, 1), 0xD5);
    CHECK_EQ(rd(0x600, 1), 0x00);
    CHECK_EQ(rd(0x500, 2), 0x90B7);
    /* The communication slot probe's bit 1 of +$100 (ROM offset $A7472). */
    wr(0x100, 1, (uint8_t)(rd(0x100, 1) | 0x02u));
    CHECK_EQ(rd(0x100, 1), 0x02);
    wr(0x100, 1, (uint8_t)(rd(0x100, 1) & ~0x02u));
    CHECK_EQ(rd(0x100, 1), 0x00);
    /* Registers are $100 apart: +$200 is not +$100. */
    wr(0x200, 2, 0x1234);
    CHECK_EQ(rd(0x200, 2), 0x1234);
    CHECK_EQ(rd(0x100, 2), 0x0000);
    CHECK_EQ(rd(0x2FE, 2), 0x1234);         /* any address of the register */
    /* A longword: two word cycles to the register, the low word stays. */
    wr(0x300, 4, 0x0000ABCDu);
    CHECK_EQ(rd(0x300, 2), 0xABCD);
    CHECK_EQ(rd(0x300, 4), 0xABCDABCDu);
    /* The window repeats every 32 registers. */
    CHECK_EQ(rd(0x2600, 2), 0x00D5);
    mc_primetime2_ops.reset(&pt);
    CHECK_EQ(rd(0x600, 2), 0);
}

int main(void) {
    test_registers();
    printf("test_primetime2: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
