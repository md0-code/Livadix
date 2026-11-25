/* test_gsc: the gray-scale LCD controller (video/gsc.c).
 *
 * Livadix's own tests, from gsc.h's observations of the PowerBook 160 ROM's
 * .Display_Video_Apple_DBLite driver: register 1 shows the panel's type in
 * bits 2-0 beside what was written, the display is off until register 4's
 * bit 5 is set, its bits 2-0 select 1, 2 or 4 bits (rows of 80, 160 and
 * 320 bytes), the gray levels run from white (0) to black, and the other
 * registers read back what the ROM wrote. */
#include "video/gsc.h"
#include "hw_test.h"

#include <stdio.h>

static hw_fake f;
static mc_gsc g;
static uint8_t screen[640 * 400 / 2];

static uint8_t rd(uint32_t reg) { return (uint8_t)mc_gsc_ops.read(&g, reg, 1); }
static void wr(uint32_t reg, uint8_t v) { mc_gsc_ops.write(&g, reg, 1, v); }

static void test_registers(void) {
    hw_fake_init(&f);
    mc_gsc_init(&g, &f.host, "GSC", screen, 5);
    mc_gsc_ops.reset(&g);
    CHECK_EQ(rd(MC_GSC_PANEL), 5);
    wr(MC_GSC_PANEL, 0xF0);
    CHECK_EQ(rd(MC_GSC_PANEL), 0xF5);
    /* The ROM's start values (gsc.h), read back. */
    static const uint8_t start[][2] = {{3, 0x10}, {5, 0x64}, {6, 0x80}, {7, 0x80}, {8, 0x02}, {9, 0x05}, {0x0A, 0xA0}, {0x1F, 0x03}};
    for (size_t i = 0; i < sizeof start / sizeof *start; i++) {
        wr(start[i][0], start[i][1]);
        CHECK_EQ(rd(start[i][0]), start[i][1]);
    }
    /* The block repeats every 32 bytes. */
    CHECK_EQ(rd(0x20 + 5), 0x64);
}

static void test_view(void) {
    hw_fake_init(&f);
    mc_gsc_init(&g, &f.host, "GSC", screen, 0);
    mc_gsc_ops.reset(&g);
    mc_gsc_view v;
    CHECK(!mc_gsc_view_get(&g, &v));               /* off after reset */
    static const struct { uint8_t control; uint32_t depth, row; } modes[] = {
        {0x20, 1, 80}, {0x21, 2, 160}, {0x22, 4, 320},
    };
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        wr(MC_GSC_CONTROL, modes[i].control);
        CHECK(mc_gsc_view_get(&g, &v));
        CHECK_EQ(v.width, 640u);
        CHECK_EQ(v.height, 400u);
        CHECK_EQ(v.depth, modes[i].depth);
        CHECK_EQ(v.row_bytes, modes[i].row);
        CHECK(v.pixels == screen);
        CHECK_EQ(v.palette[0], 0xFFFFFFFFu);
        CHECK_EQ(v.palette[(1u << modes[i].depth) - 1u], 0xFF000000u);
    }
    /* Four bits: a middle level is a middle gray. */
    CHECK_EQ(v.palette[8] & 0xFFu, 255u - 8u * 255u / 15u);
    /* The display off while the depth changes (bit 5 clear). */
    wr(MC_GSC_CONTROL, 0x02);
    CHECK(!mc_gsc_view_get(&g, &v));
}

int main(void) {
    test_registers();
    test_view();
    printf("test_gsc: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
