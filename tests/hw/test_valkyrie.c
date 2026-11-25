/* test_valkyrie: Valkyrie (valkyrie.h). The sense lines of every monitor of
 * the Developer Note's Table 3-11 read as the ROM's PrimaryInit reads them
 * (ROM offset $F9DA4: nothing driven, then lines 2, 1 and 0 driven low in
 * turn), the registers (the mode and depth, the VBL enable written as a
 * VIA's IER), the VBL at the monitor's frame rate (Tables 3-13 and 3-14)
 * with its flag, clear and interrupt line, the CLUT/DAC (address, then red,
 * green and blue, read back the same way), and what the display shows:
 * geometry, depth, the frame buffer $1000 bytes into display memory and
 * the video off while the ROM sets a mode up. Against a fake host. */
#include "hw_test.h"
#include "video/valkyrie.h"

static int tests;
static hw_fake fake;
static mc_valkyrie v;

static uint8_t rd(uint32_t offset) { return (uint8_t)mc_valkyrie_ops.read(&v, offset, 1); }
static void wr(uint32_t offset, uint8_t value) { mc_valkyrie_ops.write(&v, offset, 1, value); }

static void fresh(mc_valkyrie_monitor monitor) {
    mc_valkyrie_free(&v);
    hw_fake_init(&fake);
    CHECK(mc_valkyrie_init(&v, &fake.host, "Valkyrie", monitor));
    mc_valkyrie_ops.reset(&v);
}

/* The PrimaryInit's reading: the standard code, then the extended one
 * (lines 2, 1, 0 driven low; HW 30). Returns (standard << 8) | extended,
 * the extended code as the ROM packs it (ROM offset $F9BCA). */
static unsigned sense(void) {
    wr(0x1C, 0);
    const unsigned standard = (rd(0x1C) >> 4) & 7u;
    if (standard != 7) return standard << 8;
    wr(0x1C, 4);
    unsigned code = (rd(0x1C) >> 4) & 3u;
    wr(0x1C, 2);
    unsigned b = (rd(0x1C) >> 4) & 5u;
    if (b & 4u) b = (b & ~4u) | 2u;
    code = code << 2 | b;
    wr(0x1C, 1);
    code = code << 2 | (((rd(0x1C) >> 4) & 6u) >> 1);
    wr(0x1C, 0);
    return standard << 8 | code;
}

static void test_sense(void) {
    tests++;
    /* Table 3-11: 12-inch RGB 010, 14-inch RGB 110, VGA and SVGA 111 with
     * extended 11 10 10 ... as the ROM packs it, 16-inch RGB 111 10 11 01,
     * no monitor 111 11 11 11. */
    fresh(MC_VALKYRIE_RGB12);
    CHECK_EQ(sense(), 0x200u);
    fresh(MC_VALKYRIE_RGB14);
    CHECK_EQ(sense(), 0x600u);
    fresh(MC_VALKYRIE_VGA);
    CHECK_EQ(sense(), 0x717u);
    fresh(MC_VALKYRIE_RGB16);
    CHECK_EQ(sense(), 0x72Du);
    fresh(MC_VALKYRIE_NONE);
    CHECK_EQ(sense(), 0x73Fu);
    /* What the register keeps: the lines driven in bits 0-2. */
    wr(0x1C, 0x04);
    CHECK_EQ(rd(0x1C) & 7u, 4u);
}

static void test_registers(void) {
    tests++;
    fresh(MC_VALKYRIE_RGB14);
    /* After reset the video is off (control bit 7). */
    CHECK_EQ(rd(0x00) & 0x80u, 0x80u);
    mc_valkyrie_view view;
    CHECK(!mc_valkyrie_view_get(&v, &view));
    /* The ROM's sequence for the 14-inch monitor: mode 6 with bit 7, depth 0
     * (1 bit), the configuration bit, then bit 7 cleared. */
    wr(0x00, 0x86);
    wr(0x04, 0x00);
    wr(0x0C, 0x01);
    CHECK(!mc_valkyrie_view_get(&v, &view));
    wr(0x00, 0x06);
    CHECK(mc_valkyrie_view_get(&v, &view));
    CHECK_EQ(view.width, 640);
    CHECK_EQ(view.height, 480);
    CHECK_EQ(view.depth, 1);
    CHECK_EQ(view.row_bytes, 80);
    CHECK(view.pixels == v.vram + 0x1000);
    /* SetMode: depths 0-4 for 1 to 16 bits; rows of width x depth / 8. */
    wr(0x04, 0x03);
    CHECK(mc_valkyrie_view_get(&v, &view));
    CHECK_EQ(view.depth, 8);
    CHECK_EQ(view.row_bytes, 640);
    wr(0x04, 0x04);
    CHECK(mc_valkyrie_view_get(&v, &view));
    CHECK_EQ(view.depth, 16);
    CHECK_EQ(view.row_bytes, 1280);
    CHECK_EQ(rd(0x04), 4);
    CHECK_EQ(rd(0x0C), 1);
    /* Registers repeat every $40 bytes and keep only their byte. */
    CHECK_EQ(rd(0x44), 4);
}

static void test_vbl(void) {
    tests++;
    fresh(MC_VALKYRIE_RGB14);
    /* The 14-inch monitor at 66.72 Hz: the flag every frame, the line only
     * while enabled. */
    const uint64_t frame = (uint64_t)MC_C15M_HZ * 100u / 6672u;
    CHECK_EQ(rd(0x14) & 1u, 0);
    hw_fake_step(&fake, frame - 2);
    CHECK_EQ(rd(0x14) & 1u, 0);
    hw_fake_step(&fake, 4);
    CHECK_EQ(rd(0x14) & 1u, 1);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_VALKYRIE_LINE_VBL), 0);
    /* Enable as the driver does ($81), the line follows the flag. */
    wr(0x18, 0x81);
    CHECK_EQ(rd(0x18) & 1u, 1);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_VALKYRIE_LINE_VBL), 1);
    /* The handler's clear (bit 0 of +$10). */
    wr(0x10, 0x01);
    CHECK_EQ(rd(0x14) & 1u, 0);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_VALKYRIE_LINE_VBL), 0);
    hw_fake_step(&fake, frame);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_VALKYRIE_LINE_VBL), 1);
    /* Disabled ($01): the flag stays, the line drops. */
    wr(0x18, 0x01);
    CHECK_EQ(rd(0x18) & 1u, 0);
    CHECK_EQ(hw_fake_line(&fake, &v, MC_VALKYRIE_LINE_VBL), 0);
    CHECK_EQ(rd(0x14) & 1u, 1);
    /* The 12-inch monitor runs at 60.15 Hz. */
    fresh(MC_VALKYRIE_RGB12);
    hw_fake_step(&fake, (uint64_t)MC_C15M_HZ * 100u / 6015u + 2);
    CHECK_EQ(rd(0x14) & 1u, 1);
}

static void test_clut(void) {
    tests++;
    fresh(MC_VALKYRIE_RGB14);
    /* The address, then red, green and blue; the address advances. */
    mc_valkyrie_clut_ops.write(&v, 0x00, 1, 0x10);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x11);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x22);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x33);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x44);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x55);
    mc_valkyrie_clut_ops.write(&v, 0x04, 1, 0x66);
    CHECK_EQ(v.palette[0x10], 0x112233u);
    CHECK_EQ(v.palette[0x11], 0x445566u);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x00, 1), 0x12);
    mc_valkyrie_clut_ops.write(&v, 0x00, 1, 0x11);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x04, 1), 0x44);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x04, 1), 0x55);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x04, 1), 0x66);
    /* The control register the ROM writes $EB to. */
    mc_valkyrie_clut_ops.write(&v, 0x0C, 1, 0xEB);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x0C, 1), 0xEB);
}

static void test_monitors(void) {
    tests++;
    CHECK_EQ(mc_valkyrie_monitor_for(512, 384), MC_VALKYRIE_RGB12);
    CHECK_EQ(mc_valkyrie_monitor_for(640, 480), MC_VALKYRIE_RGB14);
    CHECK_EQ(mc_valkyrie_monitor_for(1152, 870), MC_VALKYRIE_NONE);
    /* Table 3-9: 16 bits up to 640 x 480 on Apple's monitors, 8 above. */
    CHECK_EQ(mc_valkyrie_max_depth(MC_VALKYRIE_RGB12), 16);
    CHECK_EQ(mc_valkyrie_max_depth(MC_VALKYRIE_RGB14), 16);
    CHECK_EQ(mc_valkyrie_max_depth(MC_VALKYRIE_NONE), 0);
}

static void test_ar(void) {
    tests++;
    fresh(MC_VALKYRIE_VGA);
    wr(0, 0xE);
    mc_valkyrie_view view;
    CHECK(!mc_valkyrie_view_get(&v, &view));
    mc_valkyrie_free(&v);
    CHECK(mc_valkyrie_ar_init(&v, &fake.host, "Valkyrie-AR", MC_VALKYRIE_VGA));
    mc_valkyrie_ops.reset(&v);
    /* AR palette ports have eight-byte spacing, including readback. */
    mc_valkyrie_clut_ops.write(&v, 0, 1, 2);
    mc_valkyrie_clut_ops.write(&v, 8, 1, 0x12);
    mc_valkyrie_clut_ops.write(&v, 8, 1, 0x34);
    mc_valkyrie_clut_ops.write(&v, 8, 1, 0x56);
    CHECK_EQ(v.palette[2], 0x123456);
    mc_valkyrie_clut_ops.write(&v, 0, 1, 2);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 8, 1), 0x12);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 8, 1), 0x34);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 8, 1), 0x56);
    mc_valkyrie_clut_ops.write(&v, 0x18, 1, 0xEB);
    CHECK_EQ(mc_valkyrie_clut_ops.read(&v, 0x18, 1), 0xEB);
    wr(0, 0xC);
    wr(4, 4);
    CHECK(mc_valkyrie_view_get(&v, &view));
    CHECK_EQ(view.width, 800);
    CHECK_EQ(view.height, 600);
    CHECK_EQ(view.depth, 16);
    CHECK_EQ(view.row_bytes, 1600);
    wr(0, 0xE);
    wr(4, 3);
    CHECK(mc_valkyrie_view_get(&v, &view));
    CHECK_EQ(view.width, 1024);
    CHECK_EQ(view.height, 768);
    CHECK_EQ(view.depth, 8);
    CHECK_EQ(view.row_bytes, 1024);
    hw_fake_step(&fake, v.frame_length + 1);
    CHECK_EQ(v.frame_length, MC_C15M_HZ / 60);
    wr(4, 4);
    CHECK(!mc_valkyrie_view_get(&v, &view)); /* exceeds 1 MiB */
}

int main(void) {
    test_sense();
    test_registers();
    test_vbl();
    test_clut();
    test_monitors();
    test_ar();
    mc_valkyrie_free(&v);
    printf("test_valkyrie: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
