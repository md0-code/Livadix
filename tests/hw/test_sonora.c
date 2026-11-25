/* test_sonora: Sonora (asic/sonora.c), the LC III family's integrated
 * controller, and the V8 family's ASC variant (asc.h, MC_ASC_V8).
 *
 * Livadix's own tests, from the Macintosh LC III Developer Note (Table 1-2,
 * the RAM SIMM configurations; Table 1-6, the sense codes; Figures 1-3 to
 * 1-9, the frame timing) and the LC III ROM's use of the registers
 * (sonora.h): the pseudo-VIA2's enables and flags with the 6522's
 * set/clear and write-1-to-clear rules and the masks the POST's register
 * test expects, the slot lines active low, the video's line through each
 * vertical blanking interval, the 60.15 Hz tick, the RAM configuration's
 * layouts while sizing and after, the extended sense codes as the
 * PrimaryInit assembles them, the video registers, the CLUT DAC's
 * auto-incrementing address and the picture's palette; the ASC variant's
 * register test and its FIFOs' event when it leaves FIFO mode. */
#include "asic/sonora.h"
#include "asc.h"
#include "hw_test.h"

#include <stdio.h>
#include <string.h>

static hw_fake f;
static mc_sonora s;
static uint8_t vram[MC_SONORA_VRAM_MAX];
static unsigned remaps;

static uint8_t rd(uint32_t reg) { return (uint8_t)mc_sonora_ops.read(&s, reg, 1); }
static void wr(uint32_t reg, uint8_t v) { mc_sonora_ops.write(&s, reg, 1, v); }
static uint8_t vrd(uint32_t reg) { return (uint8_t)mc_sonora_video_ops.read(&s, reg, 1); }
static void vwr(uint32_t reg, uint8_t v) { mc_sonora_video_ops.write(&s, reg, 1, v); }
static int irq(void) { return hw_fake_line(&f, &s, MC_SONORA_LINE_IRQ); }
static void remap(void *ctx) { (void)ctx; remaps++; }

static void setup(mc_sonora_monitor monitor) {
    hw_fake_init(&f);
    memset(vram, 0, sizeof vram);
    remaps = 0;
    mc_sonora_init(&s, &f.host, "Sonora", monitor, vram, 0x80000u, remap, NULL);
}

static void test_interrupts(void) {
    setup(MC_SONORA_NONE);
    CHECK_EQ(rd(MC_SONORA_IER), 0x80);
    mc_sonora_set_irq(&s, MC_SONORA_IRQ_SOUND, 1);
    CHECK_EQ(rd(MC_SONORA_IFR), MC_SONORA_IRQ_SOUND);
    CHECK(!irq());
    wr(MC_SONORA_IER, 0x90);
    CHECK_EQ(rd(MC_SONORA_IFR), 0x80 | MC_SONORA_IRQ_SOUND);
    CHECK(irq());
    mc_sonora_set_irq(&s, MC_SONORA_IRQ_SOUND, 0);
    wr(MC_SONORA_IFR, MC_SONORA_IRQ_SOUND);
    CHECK(!irq());
    /* The POST's register test (ROM ECBBC41C offset $48856, masks at
     * $4892A): $FF sets the enables' seven bits, the slot enables' four. */
    wr(MC_SONORA_IER, 0xFF);
    CHECK_EQ(rd(MC_SONORA_IER), 0xFF);
    wr(MC_SONORA_IER, 0x7F);
    CHECK_EQ(rd(MC_SONORA_IER), 0x80);
    wr(MC_SONORA_SIER, 0xFF);
    CHECK_EQ(rd(MC_SONORA_SIER) & 0x7F, 0x78);
    wr(MC_SONORA_SIER, 0x78);
    CHECK_EQ(rd(MC_SONORA_SIER), 0x80);
    /* The block repeats every $20: IER at +$1C13 (the shared VIA2 code). */
    wr(MC_SONORA_IER, 0x88);
    CHECK_EQ((uint8_t)mc_sonora_ops.read(&s, 0x1C13, 1), 0x88);
}

static void test_slots(void) {
    setup(MC_SONORA_NONE);
    hw_fake_step(&f, 30000);                /* past the frame's blanking */
    CHECK_EQ(rd(MC_SONORA_SIFR), 0xFF);
    wr(MC_SONORA_SIER, 0xFF);
    wr(MC_SONORA_IER, 0x82);
    mc_sonora_set_slots(&s, 0x20, 0x20);    /* the PDS (slot $E) */
    CHECK_EQ(rd(MC_SONORA_SIFR), 0xDF);
    CHECK_EQ(rd(MC_SONORA_IFR), 0x80 | MC_SONORA_IRQ_SLOT);
    CHECK(irq());
    wr(MC_SONORA_SIER, 0x20);               /* its enable cleared */
    CHECK(!irq());
    mc_sonora_set_slots(&s, 0, 0x20);
    CHECK_EQ(rd(MC_SONORA_SIFR), 0xFF);
}

static void test_vbl_and_tick(void) {
    setup(MC_SONORA_HIRES13);
    /* The 13-inch: 864 x 525 dots of 30.24 MHz, 45 lines of blanking
     * (Figure 1-6): the video's line (slot flag bit 6, active low) is
     * asserted at the frame's start and through the blanking. */
    const uint64_t frame = (uint64_t)MC_C15M_HZ * 864u * 525u / 30240000u;
    const uint64_t blank = (uint64_t)MC_C15M_HZ * 864u * 45u / 30240000u;
    hw_fake_step(&f, 10);
    CHECK_EQ(rd(MC_SONORA_SIFR) & 0x40, 0);
    hw_fake_step(&f, blank);
    CHECK_EQ(rd(MC_SONORA_SIFR) & 0x40, 0x40);
    hw_fake_step(&f, frame - blank);
    CHECK_EQ(rd(MC_SONORA_SIFR) & 0x40, 0);
    /* With its enable, the frame interrupts (level 2). */
    wr(MC_SONORA_SIER, 0xC0);
    wr(MC_SONORA_IER, 0x82);
    CHECK(irq());
    /* The 60.15 Hz tick: a square wave of 704 x 370 C15M periods. */
    setup(MC_SONORA_NONE);
    const unsigned before = hw_fake_changes(&f, &s, MC_SONORA_LINE_TICK);
    hw_fake_step(&f, 704u * 370u * 10u + 1u);
    CHECK_EQ(hw_fake_changes(&f, &s, MC_SONORA_LINE_TICK) - before, 20u);
    /* No monitor: the timing still runs at the 13-inch's frame (the ROM
     * waits for one before it senses the monitor). */
    setup(MC_SONORA_NONE);
    hw_fake_step(&f, 10);
    CHECK_EQ(rd(MC_SONORA_SIFR) & 0x40, 0);
    hw_fake_step(&f, blank);
    CHECK_EQ(rd(MC_SONORA_SIFR) & 0x40, 0x40);
}

typedef struct bank { uint32_t base, window, offset, size; } bank;
static bank banks[4];
static unsigned nbank;
static void map(void *ctx, uint32_t base, uint32_t window, uint32_t offset, uint32_t size) {
    (void)ctx;
    banks[nbank++] = (bank){base, window, offset, size};
}

static void test_ram(void) {
    setup(MC_SONORA_NONE);
    /* After reset the board's RAM alone ($40, the ROM's first write). */
    CHECK_EQ(rd(MC_SONORA_RAMCFG), 0x40);
    nbank = 0;
    mc_sonora_ram_layout(&s, 4u << 20, 8u << 20, map, NULL);
    CHECK_EQ(nbank, 1u);
    CHECK(banks[0].base == 0 && banks[0].size == (4u << 20));
    /* Sizing ($7F, ROM offset $A013A): 16 MB windows, the board from 0 and
     * an 8 MB SIMM's two banks of 4 MB from $0100 0000 and $0200 0000
     * (Table 1-2). */
    wr(MC_SONORA_RAMCFG, 0x7F);
    CHECK_EQ(remaps, 1u);
    nbank = 0;
    mc_sonora_ram_layout(&s, 4u << 20, 8u << 20, map, NULL);
    CHECK_EQ(nbank, 3u);
    CHECK(banks[0].base == 0 && banks[0].window == (16u << 20) && banks[0].size == (4u << 20));
    CHECK(banks[1].base == (16u << 20) && banks[1].offset == (4u << 20) && banks[1].size == (4u << 20));
    CHECK(banks[2].base == (32u << 20) && banks[2].offset == (8u << 20) && banks[2].size == (4u << 20));
    /* The sizes found: $44 for 8 MB (log2 8 + 1 = 4), contiguous from 4 MB. */
    wr(MC_SONORA_RAMCFG, 0x44);
    nbank = 0;
    mc_sonora_ram_layout(&s, 4u << 20, 8u << 20, map, NULL);
    CHECK_EQ(nbank, 2u);
    CHECK(banks[1].base == (4u << 20) && banks[1].window == (8u << 20) && banks[1].size == (8u << 20));
    /* A 1 MB SIMM: one bank ($41). */
    wr(MC_SONORA_RAMCFG, 0x7F);
    nbank = 0;
    mc_sonora_ram_layout(&s, 4u << 20, 1u << 20, map, NULL);
    CHECK_EQ(nbank, 2u);
    CHECK(banks[1].base == (16u << 20) && banks[1].size == (1u << 20));
}

static void test_sense(void) {
    /* The standard codes, read in bits 6-4 with no line driven ($07). */
    setup(MC_SONORA_RGB12);
    CHECK_EQ(vrd(2) >> 4 & 7, 2);
    setup(MC_SONORA_HIRES13);
    CHECK_EQ(vrd(2) >> 4 & 7, 6);
    setup(MC_SONORA_PORTRAIT);
    CHECK_EQ(vrd(2) >> 4 & 7, 1);
    /* Extended sense as the PrimaryInit reads it (ROM ECBBC41C offset
     * $F9F10): line 2 low ($03) gives lines 1-0, line 1 low ($05) lines 2
     * and 0, line 0 low ($06) lines 2-1. */
    static const struct { mc_sonora_monitor m; uint8_t code; } ext[] = {
        {MC_SONORA_VGA, 0x17}, {MC_SONORA_RGB16, 0x2D}, {MC_SONORA_NONE, 0x3F},
    };
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) {
        setup(ext[i].m);
        CHECK_EQ(vrd(2) >> 4 & 7, 7);
        vwr(2, 3);
        unsigned code = (vrd(2) >> 4) & 3u;
        vwr(2, 5);
        unsigned l = (vrd(2) >> 4) & 5u;
        code = code << 2 | ((l & 4u) ? 2u : 0u) | (l & 1u);
        vwr(2, 6);
        code = code << 2 | ((vrd(2) >> 4 & 6u) >> 1);
        CHECK_EQ(code, ext[i].code);
        vwr(2, 7);
    }
}

static void test_video(void) {
    setup(MC_SONORA_HIRES13);
    mc_sonora_view v;
    /* Bit 7 of $00 holds the video off while the clock is set. */
    vwr(0, 0x86);
    CHECK(!mc_sonora_view_get(&s, &v));
    vwr(0, 0x06);
    vwr(1, 3);                              /* 8 bits */
    CHECK(mc_sonora_view_get(&s, &v));
    CHECK(v.width == 640 && v.height == 480 && v.depth == 8 && v.row_bytes == 640);
    CHECK(v.pixels == vram);
    /* The DAC: address, then red, green and blue with the address
     * advancing; 1-bit white and black at $7F and $FF. */
    mc_sonora_dac_ops.write(&s, 0, 1, 0x7F);
    const uint8_t rgb[6] = {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00};
    for (int i = 0; i < 3; i++) mc_sonora_dac_ops.write(&s, 1, 1, rgb[i]);
    CHECK_EQ((uint8_t)mc_sonora_dac_ops.read(&s, 0, 1), 0x80);
    mc_sonora_dac_ops.write(&s, 0, 1, 0xFF);
    for (int i = 3; i < 6; i++) mc_sonora_dac_ops.write(&s, 1, 1, rgb[i]);
    vwr(1, 0);                              /* 1 bit */
    CHECK(mc_sonora_view_get(&s, &v));
    CHECK(v.row_bytes == 80 && v.palette[0] == 0xFFFFFFFFu && v.palette[1] == 0xFF000000u);
    /* 640 x 480 at 16 bits needs the 768 KB (the VRAM SIMM). */
    vwr(1, 4);
    CHECK(!mc_sonora_view_get(&s, &v));
    CHECK_EQ(mc_sonora_max_depth(MC_SONORA_HIRES13, 0x80000u), 8u);
    CHECK_EQ(mc_sonora_max_depth(MC_SONORA_HIRES13, 0xC0000u), 16u);
    CHECK_EQ(mc_sonora_max_depth(MC_SONORA_RGB12, 0x80000u), 16u);
    CHECK_EQ(mc_sonora_max_depth(MC_SONORA_PORTRAIT, 0x80000u), 4u);
    CHECK_EQ(mc_sonora_max_depth(MC_SONORA_RGB16, 0x80000u), 8u);
}

static void test_asc_v8(void) {
    hw_fake_init(&f);
    static mc_asc a;
    mc_asc_init(&a, &f.host, MC_ASC_V8);
    /* The register test (ROM ECBBC41C offset $48418, table $4858E): the
     * mode reads back, ends in 0; $804 reads back what was written. */
    mc_asc_ops.write(&a, 0x801, 1, 1);
    mc_asc_ops.write(&a, 0x801, 1, 0);
    (void)mc_asc_ops.read(&a, 0x804, 1);
    for (uint8_t v = 0; v <= 8; v = v ? (uint8_t)(v << 1) : 1) {
        mc_asc_ops.write(&a, 0x804, 1, v);
        CHECK_EQ((uint8_t)mc_asc_ops.read(&a, 0x804, 1), v);
    }
    /* Leaving FIFO mode stops the FIFOs with an event on each, which the
     * IIvx ROM's start beep waits for (bit 0, ROM 4957EB49 offset $85F0E). */
    mc_asc_ops.write(&a, 0x801, 1, 1);
    mc_asc_ops.write(&a, 0x000, 1, 0x80);
    (void)mc_asc_ops.read(&a, 0x804, 1);
    mc_asc_ops.write(&a, 0x801, 1, 0);
    CHECK_EQ((uint8_t)mc_asc_ops.read(&a, 0x804, 1) & 0x0F, 0x0F);
    CHECK_EQ((uint8_t)mc_asc_ops.read(&a, 0x804, 1), 0);
}

int main(void) {
    test_interrupts();
    test_slots();
    test_vbl_and_tick();
    test_ram();
    test_sense();
    test_video();
    test_asc_v8();
    printf("test_sonora: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
