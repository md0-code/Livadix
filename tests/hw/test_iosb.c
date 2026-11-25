/* test_iosb: the IOSB's register block and the CPU ID register (iosb.h):
 * 16-bit registers every $200 bytes, a longword write leaving its low word
 * (the ROM's $0000 00D5 read back as the word $00D5), byte lanes, the
 * reset, and the CPU ID register's value under the ROM's probe. */
#include "hw_test.h"
#include "asic/iosb.h"

static int tests;
static hw_fake fake;
static mc_iosb iosb;

static uint32_t rd(uint32_t offset, unsigned size) { return mc_iosb_ops.read(&iosb, offset, size); }
static void wr(uint32_t offset, unsigned size, uint32_t value) { mc_iosb_ops.write(&iosb, offset, size, value); }

static void test_registers(void) {
    tests++;
    hw_fake_init(&fake);
    mc_iosb_init(&iosb, &fake.host, "IOSB");
    CHECK_EQ(rd(0x000, 4), 0);
    /* The ROM's timing value (ROM offset $49B8), as Mac OS 8.0 reads it. */
    wr(0x600, 4, 0x000000D5u);
    CHECK_EQ(rd(0x600, 2) & 0x7FF, 0xD5);
    CHECK_EQ(rd(0x600, 4), 0x00D500D5u);
    /* Address bits 1-8 do not select: the register answers through its
     * $200 bytes. */
    CHECK_EQ(rd(0x602, 2), 0xD5);
    CHECK_EQ(rd(0x7FE, 2), 0xD5);
    CHECK_EQ(rd(0x800, 2), 0);
    /* Byte lanes: an even address is the high byte. The SCSI timing as the
     * ROM sets it (ROM offset $D1E38) and as Mac OS 8.0 changes it. */
    wr(0x200, 1, 0x12);
    CHECK_EQ(rd(0x200, 1), 0x12);
    CHECK_EQ(rd(0x201, 1), 0);
    CHECK_EQ(rd(0x200, 2), 0x1200);
    wr(0x201, 1, 0x34);
    CHECK_EQ(rd(0x200, 2), 0x1234);
    wr(0x200, 1, 0x00);
    CHECK_EQ(rd(0x200, 2), 0x0034);
    /* The configuration register's read-modify-write by longword (ROM
     * offset $49A4): the low byte carries through. */
    uint32_t v = rd(0x000, 4);
    v = (v & ~0xFFu) | ((v & 0xF7u) | 0x08u);
    wr(0x000, 4, v);
    CHECK_EQ(rd(0x000, 2), 0x0008);
    /* Sixteen registers, then the block repeats. */
    wr(0x1E00, 2, 0xBEEF);
    CHECK_EQ(rd(0x1E00, 2), 0xBEEF);
    CHECK_EQ(rd(0x3E00, 2), 0xBEEF);
    CHECK(mc_iosb_ops.stable(&iosb, 0x600, 2));
    mc_iosb_ops.reset(&iosb);
    CHECK_EQ(rd(0x600, 2), 0);
    CHECK_EQ(rd(0x1E00, 2), 0);
}

/* The ROM's probe (ROM offset $4B80): read, write 0, compare, write
 * $FFFFFFFF, compare, then $A55A in the high word. */
static void test_cpu_id(void) {
    tests++;
    const mc_dev_ops *o = &mc_iosb_cpu_id_ops;
    const uint32_t first = o->read(&iosb, 0, 4);
    CHECK_EQ(first, MC_IOSB_CPU_ID);
    o->write(&iosb, 0, 4, 0);
    CHECK_EQ(o->read(&iosb, 0, 4), first);
    o->write(&iosb, 0, 4, 0xFFFFFFFFu);
    CHECK_EQ(o->read(&iosb, 0, 4), first);
    CHECK_EQ(first >> 16, 0xA55A);
    /* The low word selects the DJMEMC records (their word at +$3C), bit 11
     * the VIA identity check. */
    CHECK_EQ(first & 0xFFFF, 0x2BAD);
    CHECK((first & 0x0800u) != 0);
    CHECK_EQ(o->read(&iosb, 2, 2), 0x2BAD);
    CHECK_EQ(o->read(&iosb, 0, 1), 0xA5);
    CHECK_EQ(o->read(&iosb, 3, 1), 0xAD);
}

int main(void) {
    test_registers();
    test_cpu_id();
    printf("test_iosb: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
