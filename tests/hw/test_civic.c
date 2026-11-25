/* test_civic: Civic and Sebastian (civic.h) against a fake host: one-bit
 * registers written bit 0 first, the VBL status, enable and clear, the
 * half-line counter, the sense lines (standard and extended codes), the
 * view from the timing, row, base and depth fields (the ROM's 640 x 480 at
 * 8 and 16 bits), Sebastian's CLUT and the 1-bit palette. Also the MCA
 * (mca.h): its identification inputs, bank layouts and windows. */
#include "hw_test.h"
#include "video/civic.h"
#include "asic/mca.h"

#include <stdlib.h>
#include <string.h>

static hw_fake fake;
static mc_civic civic;

static void field(uint32_t off, unsigned bits, uint32_t v) {
    for (unsigned i = 0; i < bits; i++) mc_civic_ops.write(&civic, off + 4u * i, 4, (v >> i) & 1u);
}
static uint32_t read_field(uint32_t off, unsigned bits) {
    uint32_t v = 0;
    for (unsigned i = 0; i < bits; i++) v |= (mc_civic_ops.read(&civic, off + 4u * i, 4) & 1u) << i;
    return v;
}

static void test_vbl(void) {
    CHECK_EQ(read_field(0x000, 1), 0);
    hw_fake_advance(&fake, hw_fake_deadline(&fake, &civic));
    CHECK_EQ(read_field(0x000, 1), 1);
    CHECK_EQ(hw_fake_line(&fake, &civic, MC_CIVIC_LINE_VBL), 0);    /* not enabled */
    field(0x110, 1, 1);
    CHECK_EQ(hw_fake_line(&fake, &civic, MC_CIVIC_LINE_VBL), 1);
    field(0x120, 1, 0);                                             /* the handler's clear ($1E6F78) */
    field(0x120, 1, 1);
    CHECK_EQ(read_field(0x000, 1), 0);
    CHECK_EQ(hw_fake_line(&fake, &civic, MC_CIVIC_LINE_VBL), 0);
    field(0x110, 1, 0);
    /* The half-line counter moves through the frame. */
    field(0x480, 12, 1050);
    const uint32_t a = read_field(0x140, 12);
    hw_fake_step(&fake, civic.frame_length / 4u);
    const uint32_t b = read_field(0x140, 12);
    CHECK(b != a && b < 1050);
}

/* The original PrimaryInit's pin writes and packing, not a selector. */
static unsigned extended_sense(void) {
    field(0x05C, 1, 1); field(0x060, 1, 0); field(0x064, 1, 1);
    unsigned code = (read_field(0x080, 3) & 3u) << 4;
    field(0x064, 1, 0); field(0x060, 1, 1);
    unsigned pins = read_field(0x080, 3);
    code |= ((pins & 4u) >> 1 | (pins & 1u)) << 2;
    field(0x060, 1, 0); field(0x05C, 1, 0);
    code |= (read_field(0x080, 3) & 6u) >> 1;
    field(0x05C, 1, 1);
    return code;
}

static void test_sense(void) {
    CHECK_EQ(read_field(0x080, 3), 6); /* all lines released, 13 inch RGB */
    CHECK_EQ(extended_sense(), 0x2B); /* ROM $1FF4E4's 13 inch comparison */
    CHECK_EQ(read_field(0x080, 3), 6);
    civic.monitor = MC_CIVIC_RGB16;
    CHECK_EQ(read_field(0x080, 3), 7);
    CHECK_EQ(extended_sense(), 0x2D);
    civic.monitor = MC_CIVIC_NONE;
    CHECK_EQ(read_field(0x080, 3), 7); /* no monitor must not become code 6 */
    CHECK_EQ(extended_sense(), 0x3F);
    CHECK_EQ(read_field(0x080, 3), 7);
    civic.monitor = MC_CIVIC_RGB13;
}

static void test_view(void) {
    mc_civic_view v;
    CHECK(!mc_civic_view_get(&civic, &v));                          /* video off */
    /* 640 x 480 x 8 as the ROM's driver sets it. */
    field(0x380, 12, 73);
    field(0x3C0, 12, 393);
    field(0x580, 12, 82);
    field(0x640, 12, 1042);
    field(0x08C, 8, 32);
    field(0x0C0, 9, 64);
    field(0x040, 1, 1);
    mc_civic_sebastian_ops.write(&civic, 0x20, 1, 0x13);
    field(0x004, 1, 1);
    CHECK(mc_civic_view_get(&civic, &v));
    CHECK(v.width == 640 && v.height == 480 && v.depth == 8 && v.row_bytes == 1024);
    CHECK(v.pixels == civic.vram + 0x800);
    /* 16 bits: one dot a unit on the 64-bit path. */
    field(0x380, 12, 147);
    field(0x3C0, 12, 787);
    field(0x08C, 8, 64);
    mc_civic_sebastian_ops.write(&civic, 0x20, 1, 0x54);
    CHECK(mc_civic_view_get(&civic, &v));
    CHECK(v.width == 640 && v.depth == 16 && v.row_bytes == 2048);
    /* 1 bit: pixel p shows CLUT entry p << 7 | $7F. */
    mc_civic_sebastian_ops.write(&civic, 0x00, 1, 0x7F);
    const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0x00}, black[4] = {0, 0, 0, 0};
    for (int i = 0; i < 4; i++) mc_civic_sebastian_ops.write(&civic, 0x10, 1, white[i]);
    mc_civic_sebastian_ops.write(&civic, 0x00, 1, 0xFF);
    for (int i = 0; i < 4; i++) mc_civic_sebastian_ops.write(&civic, 0x10, 1, black[i]);
    CHECK_EQ(mc_civic_sebastian_ops.read(&civic, 0x00, 1), 0x00);  /* the address advanced */
    field(0x380, 12, 73);
    field(0x3C0, 12, 393);
    mc_civic_sebastian_ops.write(&civic, 0x20, 1, 0x10);
    CHECK(mc_civic_view_get(&civic, &v));
    CHECK(v.depth == 1 && v.palette[0] == 0xFFFFFFu && v.palette[1] == 0);
}

static void test_mca(void) {
    uint32_t banks[MC_MCA_BANKS];
    CHECK(mc_mca_layout(20, 0, 8, banks) && banks[0] == 16u << 20 && banks[1] == 4u << 20 && !banks[2]);
    CHECK(mc_mca_layout(68, 4, 4, banks) && banks[0] == 4u << 20 && banks[4] == 16u << 20);
    CHECK(!mc_mca_layout(200, 0, 8, banks));
    mc_mca m;
    mc_mca_layout(24, 0, 8, banks);
    mc_mca_init(&m, &fake.host, "MCA", MC_MCA_ID_660AV, banks, NULL, NULL);
    /* The identification inputs ($438-$444, bit 31): $B. */
    CHECK_EQ(mc_mca_ops.read(&m, 0x438, 4), 0x80000000u);
    CHECK_EQ(mc_mca_ops.read(&m, 0x440, 4), 0);
    CHECK_EQ(mc_mca_ops.read(&m, 0x444, 4), 0x80000000u);
    /* Packed at start: 16 MB then 8 MB. */
    CHECK(mc_mca_bank_base(&m, 0) == 0 && mc_mca_bank_window(&m, 0) == 16u << 20);
    CHECK(mc_mca_bank_base(&m, 1) == 16u << 20 && mc_mca_bank_window(&m, 1) == 8u << 20);
    /* Bank 1 at 32 MB with code 5, as the ROM's sizing writes it ($ADB0C):
     * bit i of the base in bit 31 of register i. */
    for (unsigned i = 0; i < 7; i++) mc_mca_ops.write(&m, 0x450 + 0x28 + 4 * i, 4, ((32u >> i) & 1u) ? 0x80000000u : 0);
    for (unsigned i = 0; i < 3; i++) mc_mca_ops.write(&m, 0x450 + 0x28 + 28 + 4 * i, 4, ((5u >> i) & 1u) ? 0x80000000u : 0);
    CHECK(mc_mca_bank_base(&m, 1) == 32u << 20 && mc_mca_bank_window(&m, 1) == 16u << 20);
}

int main(void) {
    hw_fake_init(&fake);
    CHECK(mc_civic_init(&civic, &fake.host, "Civic", 2u << 20, MC_CIVIC_RGB13));
    mc_civic_ops.reset(&civic);
    test_vbl();
    test_sense();
    test_view();
    test_mca();
    mc_civic_free(&civic);
    printf("test_civic: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
