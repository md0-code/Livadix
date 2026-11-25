/* test_csc: the color LCD controllers: the Duos' CSC (video/csc.c) and
 * the PowerBook 165c/180c's Pangola and WD90C26 (video/wd90c26.c).
 *
 * Livadix's own tests, from csc.h's and wd90c26.h's observations of the Duo
 * 270c ROM's .Display_Video_Apple_CSC and of the video ROM
 * video-pb180c.bin: the CSC's CLUT through its address and data registers
 * (red, green, blue, the address stepping after blue, read back the same
 * way), its depth register and the 640 x 480 screen with rows of 80 to 640
 * bytes, a pixel of fewer than 8 bits addressing the CLUT with its bits on
 * top; the WD90C26's VGA DAC (six bits a component, the write index
 * stepping after blue), the vertical retrace in bit 3 of input status 1
 * once a frame, CRTC $2A bit 7 the 640 x 400 panel, Pangola's depth in
 * bits 6-5 of its control word and the display in bit 3. */
#include "video/csc.h"
#include "video/wd90c26.h"
#include "hw_test.h"

#include <stdio.h>
#include <string.h>

static hw_fake f;
static uint8_t vram[0x80000];

static void test_csc_clut(void) {
    mc_csc c;
    hw_fake_init(&f);
    mc_csc_init(&c, &f.host, "CSC", vram, 2);
    mc_csc_ops.reset(&c);
    CHECK_EQ(mc_csc_ops.read(&c, 0x01, 1) & 7u, 2u);
    mc_csc_ops.write(&c, MC_CSC_CLUT_ADDR, 1, 0x10);
    const uint8_t rgb[6] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC};
    for (int i = 0; i < 6; i++) mc_csc_ops.write(&c, MC_CSC_CLUT_DATA, 1, rgb[i]);
    CHECK_EQ(mc_csc_ops.read(&c, MC_CSC_CLUT_ADDR, 1), 0x12u);
    mc_csc_ops.write(&c, MC_CSC_CLUT_ADDR, 1, 0x10);
    for (int i = 0; i < 6; i++) CHECK_EQ(mc_csc_ops.read(&c, MC_CSC_CLUT_DATA, 1), rgb[i]);
    mc_csc_view v;
    mc_csc_ops.write(&c, MC_CSC_DEPTH, 1, 3);
    CHECK(mc_csc_view_get(&c, &v));
    CHECK_EQ(v.width, 640u);
    CHECK_EQ(v.height, 480u);
    CHECK_EQ(v.depth, 8u);
    CHECK_EQ(v.row_bytes, 640u);
    CHECK_EQ(v.palette[0x10], 0xFF123456u);
    CHECK_EQ(v.palette[0x11], 0xFF789ABCu);
    /* One bit: pixel 0 is CLUT entry $7F, pixel 1 entry $FF. */
    mc_csc_ops.write(&c, MC_CSC_CLUT_ADDR, 1, 0x7F);
    for (int i = 0; i < 3; i++) mc_csc_ops.write(&c, MC_CSC_CLUT_DATA, 1, 0xFF);
    mc_csc_ops.write(&c, MC_CSC_CLUT_ADDR, 1, 0xFF);
    for (int i = 0; i < 3; i++) mc_csc_ops.write(&c, MC_CSC_CLUT_DATA, 1, 0);
    mc_csc_ops.write(&c, MC_CSC_DEPTH, 1, 0);
    CHECK(mc_csc_view_get(&c, &v));
    CHECK_EQ(v.depth, 1u);
    CHECK_EQ(v.row_bytes, 80u);
    CHECK_EQ(v.palette[0], 0xFFFFFFFFu);
    CHECK_EQ(v.palette[1], 0xFF000000u);
    /* Timing registers kept. */
    mc_csc_ops.write(&c, 0x2A, 1, 0x06);
    CHECK_EQ(mc_csc_ops.read(&c, 0x2A, 1), 0x06u);
}

static void test_wd(void) {
    mc_wd90c26 w;
    hw_fake_init(&f);
    memset(vram, 0, sizeof vram);
    mc_wd90c26_init(&w, &f.host, "WD90C26", vram, 400);
    mc_wd90c26_ops.reset(&w);
    /* The DAC: entry 5 written through $3C8/$3C9. */
    mc_wd90c26_ops.write(&w, MC_WD_PORTS + 0x3C8, 1, 5);
    const uint8_t rgb[3] = {0x3F, 0x20, 0x00};
    for (int i = 0; i < 3; i++) mc_wd90c26_ops.write(&w, MC_WD_PORTS + 0x3C9, 1, rgb[i]);
    CHECK_EQ(mc_wd90c26_ops.read(&w, MC_WD_PORTS + 0x3C8, 1), 6u);
    mc_wd90c26_ops.write(&w, MC_WD_PORTS + 0x3C7, 1, 5);
    for (int i = 0; i < 3; i++) CHECK_EQ(mc_wd90c26_ops.read(&w, MC_WD_PORTS + 0x3C9, 1), rgb[i]);
    /* The panel's type: CRTC $2A bit 7 set on the 640 x 400 panel. */
    mc_wd90c26_ops.write(&w, MC_WD_PORTS + 0x3D4, 1, 0x2A);
    CHECK_EQ(mc_wd90c26_ops.read(&w, MC_WD_PORTS + 0x3D5, 1) & 0x80u, 0x80u);
    /* The retrace comes once a frame. */
    int seen = 0, rises = 0, last = 0;
    for (int t = 0; t < 3 * 704 * 370; t += 1000) {
        const int now = (mc_wd90c26_ops.read(&w, MC_WD_PORTS + 0x3DA, 1) & 0x08u) != 0;
        if (now && !last) rises++;
        seen |= now;
        last = now;
        hw_fake_step(&f, 1000);
    }
    CHECK(seen);
    CHECK(rises >= 2 && rises <= 4);
    /* Pangola: off until bit 3; the depth from bits 6-5. */
    mc_wd90c26_view v;
    CHECK(!mc_wd90c26_view_get(&w, &v));
    static const struct { uint16_t word; uint32_t depth; } modes[] = {{0x7E, 1}, {0x5E, 2}, {0x3E, 4}, {0x1E, 8}};
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++) {
        mc_wd90c26_ops.write(&w, MC_WD_PANGOLA, 2, modes[i].word);
        CHECK(mc_wd90c26_view_get(&w, &v));
        CHECK_EQ(v.depth, modes[i].depth);
        CHECK_EQ(v.row_bytes, 640u * modes[i].depth / 8u);
        CHECK_EQ(v.height, 400u);
        CHECK(v.pixels == vram + MC_WD_SCREEN);
    }
    CHECK_EQ(v.palette[5], 0xFFFF8100u);
}

int main(void) {
    test_csc_clut();
    test_wd();
    printf("test_csc: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
