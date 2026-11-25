/* test_oss: the Macintosh IIfx's OSS (asic/oss.c).
 *
 * Livadix's own tests, from oss.h (the IIfx ROM's use of the OSS, the Guide
 * to the Macintosh Family Hardware, chapter 3): the per-source level
 * registers and the ROM's register test values (0-6 at +$000-$00D, 0-$1F at
 * +$204, 0-3 at +$205), the pending word at +$202, the level reported to
 * the processor (the highest level among the asserted sources, 0 for a
 * source at level 0), the 60.15 Hz tick (source 10) and its acknowledge at
 * +$207, the power-off bit (+$204 bit 7) and reset. */
#include "asic/oss.h"
#include "hw_test.h"

#include <stdio.h>

static hw_fake fake;
static mc_oss oss;

static uint32_t rd(uint32_t offset, unsigned size) { return mc_oss_ops.read(&oss, offset, size); }
static void wr(uint32_t offset, unsigned size, uint32_t v) { mc_oss_ops.write(&oss, offset, size, v); }
static int ipl(void) { return hw_fake_value(&fake, &oss, MC_OSS_LINE_IPL); }

#define TICK (704u * 370u)

static void test_registers(void) {
    hw_fake_init(&fake);
    mc_oss_init(&oss, &fake.host, "OSS");
    mc_oss_ops.reset(&oss);
    /* The ROM's register test (ROM offset $42FC4). */
    for (uint32_t r = 0; r <= 0xD; r++)
        for (uint32_t v = 0; v <= 6; v++) {
            wr(r, 1, v);
            CHECK_EQ(rd(r, 1) & 7u, v);
        }
    for (uint32_t v = 0; v <= 0x1F; v++) {
        wr(0x204, 1, v);
        CHECK_EQ(rd(0x204, 1) & 0x1Fu, v);
    }
    for (uint32_t v = 0; v <= 3; v++) {
        wr(0x205, 1, v);
        CHECK_EQ(rd(0x205, 1) & 3u, v);
    }
    CHECK_EQ(hw_fake_reports(&fake, &oss, MC_OSS_LINE_POWER_OFF), 0u);
    /* Nothing asserted: nothing pending, level 0. */
    for (uint32_t r = 0; r < 16; r++) wr(r, 1, 0);
    CHECK_EQ(rd(0x202, 2), 0u);
}

static void test_levels(void) {
    hw_fake_init(&fake);
    mc_oss_init(&oss, &fake.host, "OSS");
    mc_oss_ops.reset(&oss);
    /* A source at level 0 is pending but interrupts nothing. */
    mc_oss_set(&oss, MC_OSS_SCSI, 1);
    CHECK_EQ(rd(0x202, 2), 1u << MC_OSS_SCSI);
    CHECK(ipl() <= 0);
    wr(MC_OSS_SCSI, 1, 2);
    CHECK_EQ(ipl(), 2);
    /* The highest level among the asserted sources. */
    wr(MC_OSS_SCC_IOP, 1, 4);
    mc_oss_set(&oss, MC_OSS_SCC_IOP, 1);
    CHECK_EQ(ipl(), 4);
    CHECK_EQ(rd(0x202, 2), (1u << MC_OSS_SCSI) | (1u << MC_OSS_SCC_IOP));
    CHECK_EQ(rd(0x203, 1), (1u << MC_OSS_SCSI | 1u << MC_OSS_SCC_IOP) & 0xFFu);
    mc_oss_set(&oss, MC_OSS_SCC_IOP, 0);
    CHECK_EQ(ipl(), 2);
    /* Moving a source's level moves the request. */
    wr(MC_OSS_SCSI, 1, 6);
    CHECK_EQ(ipl(), 6);
    mc_oss_set(&oss, MC_OSS_SCSI, 0);
    CHECK_EQ(ipl(), 0);
    /* A slot's line (slot $C = source 3). */
    wr(MC_OSS_SLOT9 + 3, 1, 2);
    mc_oss_set(&oss, MC_OSS_SLOT9 + 3, 1);
    CHECK_EQ(ipl(), 2);
    mc_oss_set(&oss, MC_OSS_SLOT9 + 3, 0);
    CHECK_EQ(ipl(), 0);
}

static void test_tick(void) {
    hw_fake_init(&fake);
    mc_oss_init(&oss, &fake.host, "OSS");
    mc_oss_ops.reset(&oss);
    wr(MC_OSS_TICK, 1, 1);
    CHECK_EQ(hw_fake_deadline(&fake, &oss), (uint64_t)TICK);
    hw_fake_advance(&fake, TICK - 1u);
    CHECK_EQ(rd(0x202, 2) & (1u << MC_OSS_TICK), 0u);
    hw_fake_advance(&fake, TICK);
    CHECK_EQ(rd(0x202, 2) & (1u << MC_OSS_TICK), 1u << MC_OSS_TICK);
    CHECK_EQ(ipl(), 1);
    /* Held until acknowledged at +$207. */
    hw_fake_advance(&fake, TICK + TICK / 2u);
    CHECK_EQ(ipl(), 1);
    wr(0x207, 1, 0);
    CHECK_EQ(rd(0x202, 2), 0u);
    CHECK_EQ(ipl(), 0);
    hw_fake_advance(&fake, 2u * TICK);
    CHECK_EQ(ipl(), 1);
    /* Sixty ticks in a second's worth of periods, near enough. */
    unsigned ticks = 0;
    for (uint64_t t = 2u * TICK; t < 2u * TICK + MC_C15M_HZ; t += TICK / 4u) {
        hw_fake_advance(&fake, t);
        if (rd(0x202, 2) & (1u << MC_OSS_TICK)) {
            ticks++;
            wr(0x207, 1, 0);
        }
    }
    CHECK(ticks >= 59 && ticks <= 61);
}

static void test_power(void) {
    hw_fake_init(&fake);
    mc_oss_init(&oss, &fake.host, "OSS");
    mc_oss_ops.reset(&oss);
    /* The ROM's start value, then its ShutDown's bset #7 (ROM offset $9E3C). */
    wr(0x204, 1, 0x0D);
    CHECK_EQ(hw_fake_reports(&fake, &oss, MC_OSS_LINE_POWER_OFF), 0u);
    wr(0x204, 1, 0x8D);
    CHECK_EQ(hw_fake_line(&fake, &oss, MC_OSS_LINE_POWER_OFF), 1);
    /* Reset clears the levels and the controls. */
    wr(MC_OSS_VIA1, 1, 1);
    mc_oss_ops.reset(&oss);
    CHECK_EQ(rd(MC_OSS_VIA1, 1), 0u);
    CHECK_EQ(rd(0x204, 1), 0u);
}

int main(void) {
    test_registers();
    test_levels();
    test_tick();
    test_power();
    printf("test_oss: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
