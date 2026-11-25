/* DAFB against the DAFB II specification (6.6.1 registers and reset values,
 * 6.6.2 Swatch interrupts, mask, status, clears and the current line,
 * 6.6.3 the CLUT/DAC) and Apple Technical Note HW 30 (the sense codes and
 * the type-7 extended codes), with the screen geometry as the Quadra 700 ROM
 * programs it (dafb.c). */
#include "hw_test.h"
#include "../../src/hw/video/dafb.h"

static hw_fake f;
static mc_dafb d;

static uint32_t rd(uint32_t offset) { return mc_dafb_ops.read(&d, offset, 4); }
static void wr(uint32_t offset, uint32_t v) { mc_dafb_ops.write(&d, offset, 4, v); }

static void fresh(mc_dafb_monitor monitor) {
    hw_fake_init(&f);
    CHECK(mc_dafb_init(&d, &f.host, "DAFB", 1u << 20, monitor));
    mc_dafb_ops.reset(&d);
}

/* The sense register with `drive` written (0 = driven low). */
static uint32_t sense(uint32_t drive) {
    wr(0x1C, drive);
    return rd(0x1C) & 7u;
}

static void test_sense(void) {
    /* Table S-1 codes, read back inverted by the register. */
    static const struct { mc_dafb_monitor m; uint32_t code; } simple[] = {
        {MC_MONITOR_RGB21, 0}, {MC_MONITOR_RGB12, 2}, {MC_MONITOR_PORTRAIT, 5}, {MC_MONITOR_HIRES13, 6}, {MC_MONITOR_NONE, 7},
    };
    for (unsigned i = 0; i < sizeof simple / sizeof *simple; i++) {
        fresh(simple[i].m);
        CHECK_EQ(rd(0x1C), ~simple[i].code & 7u);       /* reset: all lines released */
        mc_dafb_free(&d);
    }
    /* Table S-2: the 16" RGB is 10 11 01 (2 low: 1 and 0; 1 low: 2 and 0;
     * 0 low: 2 and 1). */
    fresh(MC_MONITOR_RGB16);
    CHECK_EQ(sense(7), 0);                              /* no line grounded */
    CHECK_EQ(sense(3), ~0x2u & 7u);                     /* 2 driven low: 1 = 1, 0 = 0 */
    CHECK_EQ(sense(5), ~0x5u & 7u);                     /* 1 low: 2 = 1, 0 = 1 */
    CHECK_EQ(sense(6), ~0x2u & 7u);                     /* 0 low: 2 = 0, 1 = 1 */
    mc_dafb_free(&d);
    /* The VGA code 01 01 11. */
    fresh(MC_MONITOR_VGA);
    CHECK_EQ(sense(3), ~0x1u & 7u);
    CHECK_EQ(sense(5), ~0x1u & 7u);
    CHECK_EQ(sense(6), ~0x6u & 7u);
    mc_dafb_free(&d);
    /* No monitor: every line follows only what drives it. */
    fresh(MC_MONITOR_NONE);
    CHECK_EQ(sense(3), ~0x3u & 7u);
    CHECK_EQ(sense(6), ~0x6u & 7u);
    mc_dafb_free(&d);
    CHECK_EQ(mc_dafb_monitor_for(640, 480), MC_MONITOR_HIRES13);
    CHECK_EQ(mc_dafb_monitor_for(1152, 870), MC_MONITOR_RGB21);
    CHECK_EQ(mc_dafb_monitor_for(832, 624), MC_MONITOR_RGB16);
    CHECK_EQ(mc_dafb_monitor_for(1024, 768), MC_MONITOR_NONE);
}

/* The 640 x 480 mode as the Quadra 700 ROM sets it, with the clock divisor
 * and CLUT/DAC configuration given. */
static void program_640(uint32_t clock, uint32_t clut) {
    wr(0x00, 0x008);                    /* video base bits 20:9: $1000 */
    wr(0x04, 0x000);
    wr(0x08, 0x100);                    /* row words: 1024 bytes */
    wr(0x0C, clock);
    wr(0x200, 0);
    wr(0x220, clut);
    static const uint32_t swatch[] = {
        0x31E, 0x1B0, 0x020, 0x35F, 0x03F, 0x04B, 0x06F, 0x098, 0x318, 0x35E,
        0x41A, 0x418, 0x004, 0x009, 0x052, 0x412, 0x416, 0x03A,
    };
    for (unsigned i = 0; i < sizeof swatch / sizeof *swatch; i++) wr(0x124 + 4 * i, swatch[i]);
    wr(0x100, 0xFF2);                   /* Swatch enabled */
    wr(0x20, 3);
    wr(0x20, 2);
    wr(0x20, 0);                        /* out of reset */
}

static void test_view(void) {
    fresh(MC_MONITOR_HIRES13);
    mc_dafb_view v;
    CHECK(!mc_dafb_view_get(&d, &v));                   /* held in reset */
    CHECK_EQ(rd(0x10), 0xF00);
    CHECK_EQ(rd(0x20), 7);
    CHECK_EQ(rd(0x24), 0x101);
    program_640(0x61E, 0x80);
    CHECK(mc_dafb_view_get(&d, &v));
    CHECK_EQ(v.width, 640);
    CHECK_EQ(v.height, 480);
    CHECK_EQ(v.depth, 1);
    CHECK_EQ(v.row_bytes, 1024);
    CHECK(v.pixels == d.vram + 0x1000);
    /* A divisor of 4 loads 32 bits every 4 pixels: 8 bits per pixel; 1 (255)
     * gives 32. */
    wr(0x0C, 0x602);
    CHECK(mc_dafb_view_get(&d, &v) && v.depth == 8);
    wr(0x00, 0);
    wr(0x0C, 0x6FF);
    wr(0x08, 0x280);
    CHECK(!mc_dafb_view_get(&d, &v));                   /* 1.2 MB: more than two banks hold */
    mc_dafb_free(&d);
    CHECK(mc_dafb_init(&d, &f.host, "DAFB", 2u << 20, MC_MONITOR_HIRES13));
    mc_dafb_ops.reset(&d);
    program_640(0x6FF, 0x80);
    wr(0x00, 0);
    wr(0x08, 0x280);
    CHECK(mc_dafb_view_get(&d, &v) && v.depth == 32 && v.row_bytes == 2560);
    /* Four pixels per timing clock (the 21" monitor's CLUT/DAC setting). */
    wr(0x220, 0xC0);
    wr(0x0C, 0x406);
    wr(0x08, 0x100);
    CHECK(mc_dafb_view_get(&d, &v) && v.width == 2560 && v.depth == 1);
    /* The CLUT: an address, then red, green and blue. */
    wr(0x200, 5);
    wr(0x210, 0x12);
    wr(0x210, 0x34);
    wr(0x210, 0x56);
    CHECK_EQ(d.palette[5], 0x123456);
    wr(0x200, 5);
    CHECK_EQ(rd(0x210), 0x12);
    CHECK_EQ(rd(0x210), 0x34);
    CHECK_EQ(rd(0x210), 0x56);
    mc_dafb_free(&d);
}

static void test_interrupts(void) {
    fresh(MC_MONITOR_HIRES13);
    program_640(0x61E, 0x80);
    const uint64_t frame = d.frame_length;
    CHECK_EQ(frame, (uint64_t)MC_C15M_HZ * 100u / 6667u);        /* 66.67 Hz (HW 30) */
    /* Masked: the status sets, the line stays low. */
    hw_fake_advance(&f, frame);
    CHECK_EQ(rd(0x108) & 1, 1);
    CHECK(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL) != 1);
    wr(0x114, 0);                                                 /* clear VBL */
    CHECK_EQ(rd(0x108) & 1, 0);
    /* Enabled: the line follows the status until it is cleared. */
    wr(0x104, 1);
    hw_fake_advance(&f, 2 * frame + frame / 2);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 1);
    rd(0x114);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 0);
    /* VBL begins after the active lines: VFP / 2 = 521 lines of 525. */
    const uint64_t vbl = 2 * frame + frame * 521u / 525u;
    hw_fake_advance(&f, vbl - 2);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 0);
    hw_fake_advance(&f, vbl + 2);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 1);
    wr(0x114, 0);
    /* The cursor line interrupt, status bit 2. */
    wr(0x104, 4);
    wr(0x118, 100);
    const uint64_t cursor = 4 * frame + frame * 100u / 525u;
    hw_fake_advance(&f, cursor + 2);
    CHECK_EQ(rd(0x108) & 4, 4);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 1);
    CHECK(rd(0x16C) >= 100 && rd(0x16C) <= 101);                 /* the current line */
    wr(0x10C, 0);
    CHECK_EQ(rd(0x108) & 4, 0);
    CHECK_EQ(hw_fake_value(&f, &d, MC_DAFB_LINE_VBL), 0);
    mc_dafb_free(&d);
}

/* The DAC of the Centris/Quadra 610-800 and LC 475 boards (antelope): its
 * configuration registers follow the address register, as the ROMs' check
 * for it reads them (ROM F1ACAD13 offset $F8F82: 6 to register 0, 0 to
 * register 1, register 0 read back as 6); the Quadra 700's DAC has one. */
static void test_indexed_dac(void) {
    for (int antelope = 0; antelope < 2; antelope++) {
        fresh(MC_MONITOR_HIRES13);
        d.antelope = (uint8_t)antelope;
        wr(0x200, 0);
        wr(0x220, 6);
        wr(0x200, 1);
        wr(0x220, 0);
        wr(0x200, 0);
        CHECK_EQ(rd(0x220) & 0xFu, antelope ? 6u : 0u);
        mc_dafb_free(&d);
    }
}

int mc_test_dafb(void) {
    test_indexed_dac();
    test_sense();
    test_view();
    test_interrupts();
    return 0;
}
