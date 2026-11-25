/* test_livadix_card: the Livadix Display Card (src/hw/livadix_card.h), its NuBus
 * slots (src/hw/nubus.h) and its PCI function (ppc/chips/pci.h) against
 * their own descriptions: register reset values and the resolution table
 * (docs/NOTES.md's order), byte and word lanes, the CLUT and gamma
 * data ports, the picture the display shows at every depth, the vertical
 * blank at 60 Hz of guest time and its interrupt, the declaration ROM at
 * the top of the slot's space (Designing Cards and Drivers for the
 * Macintosh Family, 3rd ed., chapter 8), the slot's windows and bus errors,
 * the blitter, and the PCI configuration space, BARs and FCode expansion
 * ROM (PCI Local Bus Specification 2.0, 6.3). */
#include "hw_test.h"
#include "bus.h"
#include "config.h"
#include "guest.h"
#include "livadix_card.h"
#include "nubus.h"
#include "ppc/chips/pci.h"

#include <stdlib.h>
#include <string.h>

static int tests;
#define T() (tests++)

static hw_fake fake;
static mc_livadix_card card;
static const uint8_t rom_image[] = {1, 2, 3, 4, 0x0F};

static uint32_t r(uint32_t reg) { return card.dev.ops->read(&card, reg, 4); }
static void w(uint32_t reg, uint32_t v) { card.dev.ops->write(&card, reg, 4, v); }
static int irq(void) { return hw_fake_line(&fake, &card, MC_LIVADIX_CARD_LINE_IRQ); }

static void fresh(unsigned monitor) {
    if (card.vram) mc_livadix_card_free(&card);
    hw_fake_init(&fake);
    CHECK(mc_livadix_card_init(&card, &fake.host, "card", MC_LIVADIX_CARD_VRAM, monitor, rom_image, sizeof rom_image, 1));
}

static void test_reset(void) {
    fresh(3);                                           /* 640 x 480 */
    CHECK_EQ(r(MC_LIVADIX_REG_ID), 0x4C564458); T();
    CHECK_EQ(card.dev.ops->read(&card, MC_LIVADIX_REG_ID, 1), 'L'); T();
    CHECK_EQ(card.dev.ops->read(&card, MC_LIVADIX_REG_ID + 3, 1), 'X'); T();
    CHECK_EQ(card.dev.ops->read(&card, MC_LIVADIX_REG_ID + 2, 2), 0x4458); T();
    CHECK_EQ(r(MC_LIVADIX_REG_VERSION), 1); T();
    CHECK_EQ(r(MC_LIVADIX_REG_VRAM), 2u << 20); T();
    CHECK_EQ(r(MC_LIVADIX_REG_MONITOR), 3); T();
    CHECK_EQ(r(MC_LIVADIX_REG_MODES), 12); T();
    CHECK_EQ(r(MC_LIVADIX_REG_REFRESH), 60000); T();
    CHECK_EQ(r(MC_LIVADIX_REG_WIDTH), 640); T();
    CHECK_EQ(r(MC_LIVADIX_REG_HEIGHT), 480); T();
    CHECK_EQ(r(MC_LIVADIX_REG_DEPTH), 1); T();
    CHECK_EQ(r(MC_LIVADIX_REG_STRIDE), 80); T();
    CHECK_EQ(r(MC_LIVADIX_REG_BASE), 0); T();
    CHECK_EQ(r(MC_LIVADIX_REG_CONTROL), MC_LIVADIX_CONTROL_DISPLAY); T();
    CHECK_EQ(r(MC_LIVADIX_REG_STATUS), 0); T();
    CHECK_EQ(irq(), 0); T();
    /* The resolution table is the configuration's, in its order. */
    CHECK_EQ(MC_LIVADIX_CARD_MODES, MC_CONFIG_RESOLUTIONS); T();
    for (unsigned i = 0; i < MC_LIVADIX_CARD_MODES; i++) {
        CHECK_EQ(r(MC_LIVADIX_REG_RES + 4 * i), (uint32_t)mc_config_resolutions[i][0] << 16 | mc_config_resolutions[i][1]); T();
        CHECK_EQ(mc_livadix_card_mode(mc_config_resolutions[i][0], mc_config_resolutions[i][1]), (int)i); T();
    }
    CHECK_EQ(mc_livadix_card_mode(1000, 700), -1); T();
    /* A 1152 x 870 display starts in its own size; 800 x 600 rows of 1 bit
     * are 100 bytes, whole longwords. */
    fresh(8);
    CHECK_EQ(r(MC_LIVADIX_REG_WIDTH), 1152); T();
    CHECK_EQ(r(MC_LIVADIX_REG_STRIDE), 144); T();
    fresh(4);
    CHECK_EQ(r(MC_LIVADIX_REG_STRIDE), 100); T();
    /* Out-of-range set-ups are refused. */
    mc_livadix_card bad;
    CHECK(!mc_livadix_card_init(&bad, &fake.host, "bad", MC_LIVADIX_CARD_VRAM, 12, NULL, 0, 1)); T();
    CHECK(!mc_livadix_card_init(&bad, &fake.host, "bad", MC_LIVADIX_CARD_VRAM, 0, NULL, MC_LIVADIX_CARD_ROM_WINDOW + 1, 1)); T();
}

static void test_registers(void) {
    fresh(3);
    w(MC_LIVADIX_REG_WIDTH, 800);
    w(MC_LIVADIX_REG_HEIGHT, 600);
    w(MC_LIVADIX_REG_DEPTH, 16);
    w(MC_LIVADIX_REG_STRIDE, 1600);
    w(MC_LIVADIX_REG_BASE, 0x1000);
    CHECK_EQ(r(MC_LIVADIX_REG_WIDTH), 800); T();
    CHECK_EQ(r(MC_LIVADIX_REG_HEIGHT), 600); T();
    CHECK_EQ(r(MC_LIVADIX_REG_DEPTH), 16); T();
    CHECK_EQ(r(MC_LIVADIX_REG_STRIDE), 1600); T();
    CHECK_EQ(r(MC_LIVADIX_REG_BASE), 0x1000); T();
    /* Read-only registers keep their values. */
    w(MC_LIVADIX_REG_ID, 0);
    w(MC_LIVADIX_REG_MONITOR, 7);
    CHECK_EQ(r(MC_LIVADIX_REG_ID), 0x4C564458); T();
    CHECK_EQ(r(MC_LIVADIX_REG_MONITOR), 3); T();
    /* A byte write drives its lane: CONTROL's low byte at +3. */
    card.dev.ops->write(&card, MC_LIVADIX_REG_CONTROL + 3, 1, 0x03);
    CHECK_EQ(r(MC_LIVADIX_REG_CONTROL), 3); T();
    card.dev.ops->write(&card, MC_LIVADIX_REG_CONTROL, 1, 0x03);   /* the high lane: nothing in bits 0-1 */
    CHECK_EQ(r(MC_LIVADIX_REG_CONTROL), 0); T();
    /* Data ports change their index; the others are stable. */
    CHECK(card.dev.ops->stable(&card, MC_LIVADIX_REG_STATUS, 4)); T();
    CHECK(card.dev.ops->stable(&card, MC_LIVADIX_REG_WIDTH, 4)); T();
    CHECK(!card.dev.ops->stable(&card, MC_LIVADIX_REG_CLUT_DATA, 4)); T();
    CHECK(!card.dev.ops->stable(&card, MC_LIVADIX_REG_GAMMA_DATA, 4)); T();
}

static void test_clut_gamma(void) {
    fresh(3);
    /* Three bytes per entry, the index stepping after blue. */
    w(MC_LIVADIX_REG_CLUT_INDEX, 254);
    for (uint32_t v = 0x10; v < 0x16; v++) w(MC_LIVADIX_REG_CLUT_DATA, v);
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_INDEX), 0); T();          /* 254, 255, then wrapped */
    w(MC_LIVADIX_REG_CLUT_INDEX, 254);
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_DATA), 0x10); T();
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_DATA), 0x11); T();
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_DATA), 0x12); T();
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_INDEX), 255); T();
    CHECK_EQ(r(MC_LIVADIX_REG_CLUT_DATA), 0x13); T();
    /* Writing the index restarts at red. */
    w(MC_LIVADIX_REG_CLUT_INDEX, 1);
    w(MC_LIVADIX_REG_CLUT_DATA, 0xAA);
    w(MC_LIVADIX_REG_CLUT_INDEX, 1);
    w(MC_LIVADIX_REG_CLUT_DATA, 0xFF);
    w(MC_LIVADIX_REG_CLUT_DATA, 0x80);
    w(MC_LIVADIX_REG_CLUT_DATA, 0x00);
    mc_livadix_card_view v;
    CHECK(mc_livadix_card_view_get(&card, &v)); T();
    CHECK_EQ(v.depth, 1); T();
    CHECK_EQ(v.palette[1], 0xFF8000); T();
    CHECK_EQ(v.palette[254], 0x101112); T();
    /* The gamma tables apply after the CLUT, per channel. */
    w(MC_LIVADIX_REG_GAMMA_INDEX, 0xFF);
    w(MC_LIVADIX_REG_GAMMA_DATA, 0x40);                      /* red 255 -> 64 */
    w(MC_LIVADIX_REG_GAMMA_DATA, 0xFF);
    w(MC_LIVADIX_REG_GAMMA_DATA, 0xFF);
    w(MC_LIVADIX_REG_GAMMA_INDEX, 0x80);
    w(MC_LIVADIX_REG_GAMMA_DATA, 0x80);
    w(MC_LIVADIX_REG_GAMMA_DATA, 0x20);                      /* green 128 -> 32 */
    CHECK(mc_livadix_card_view_get(&card, &v)); T();
    CHECK_EQ(v.palette[1], 0x402000); T();
    w(MC_LIVADIX_REG_GAMMA_INDEX, 0x80);
    CHECK_EQ(r(MC_LIVADIX_REG_GAMMA_DATA), 0x80); T();
    CHECK_EQ(r(MC_LIVADIX_REG_GAMMA_DATA), 0x20); T();
    CHECK_EQ(r(MC_LIVADIX_REG_GAMMA_DATA), 0x80); T();       /* blue untouched: linear */
}

static void put32(uint32_t at, uint32_t v) {
    card.vram[at] = (uint8_t)(v >> 24);
    card.vram[at + 1] = (uint8_t)(v >> 16);
    card.vram[at + 2] = (uint8_t)(v >> 8);
    card.vram[at + 3] = (uint8_t)v;
}

static void test_view(void) {
    fresh(3);
    mc_livadix_card_view v;
    /* Every depth at 640 x 480 with whole-longword rows. */
    static const uint32_t depths[] = {1, 2, 4, 8, 16, 32};
    for (unsigned i = 0; i < 6; i++) {
        w(MC_LIVADIX_REG_DEPTH, depths[i]);
        w(MC_LIVADIX_REG_STRIDE, 640 * depths[i] / 8);
        CHECK(mc_livadix_card_view_get(&card, &v)); T();
        CHECK(v.width == 640 && v.height == 480 && v.depth == depths[i] && v.row_bytes == 640 * depths[i] / 8); T();
        CHECK(v.pixels == card.vram); T();                /* identity tables: VRAM shown as it is */
    }
    /* No picture: display off, a depth not offered, rows too short, the
     * picture past the VRAM's end. */
    w(MC_LIVADIX_REG_CONTROL, 0);
    CHECK(!mc_livadix_card_view_get(&card, &v)); T();
    w(MC_LIVADIX_REG_CONTROL, 1);
    w(MC_LIVADIX_REG_DEPTH, 24);
    CHECK(!mc_livadix_card_view_get(&card, &v)); T();
    w(MC_LIVADIX_REG_DEPTH, 8);
    w(MC_LIVADIX_REG_STRIDE, 639);
    CHECK(!mc_livadix_card_view_get(&card, &v)); T();
    w(MC_LIVADIX_REG_STRIDE, 640);
    w(MC_LIVADIX_REG_BASE, MC_LIVADIX_CARD_VRAM - 640 * 479);
    CHECK(!mc_livadix_card_view_get(&card, &v)); T();
    w(MC_LIVADIX_REG_BASE, MC_LIVADIX_CARD_VRAM - 640 * 480);
    CHECK(mc_livadix_card_view_get(&card, &v) && v.pixels == card.vram + MC_LIVADIX_CARD_VRAM - 640 * 480); T();
    /* The largest modes of 2 MiB: 1920 x 1080 x 8 and 832 x 624 x 32. */
    w(MC_LIVADIX_REG_BASE, 0);
    w(MC_LIVADIX_REG_WIDTH, 1920);
    w(MC_LIVADIX_REG_HEIGHT, 1080);
    w(MC_LIVADIX_REG_STRIDE, 1920);
    CHECK(mc_livadix_card_view_get(&card, &v)); T();
    w(MC_LIVADIX_REG_WIDTH, 832);
    w(MC_LIVADIX_REG_HEIGHT, 624);
    w(MC_LIVADIX_REG_DEPTH, 32);
    w(MC_LIVADIX_REG_STRIDE, 832 * 4);
    CHECK(mc_livadix_card_view_get(&card, &v) && v.depth == 32); T();
    /* Direct depths through tables that are not the identity: the CLUT's
     * channels (a ramp with red inverted) and the gamma; the picture is
     * converted to x-R8-G8-B8. */
    w(MC_LIVADIX_REG_CLUT_INDEX, 0);
    for (unsigned i = 0; i < 256; i++) {
        w(MC_LIVADIX_REG_CLUT_DATA, 255 - i);
        w(MC_LIVADIX_REG_CLUT_DATA, i);
        w(MC_LIVADIX_REG_CLUT_DATA, i);
    }
    put32(0, 0x00102030);
    CHECK(mc_livadix_card_view_get(&card, &v) && v.depth == 32 && v.row_bytes == 832 * 4 && v.pixels != card.vram); T();
    CHECK(v.pixels[0] == 0 && v.pixels[1] == 0xEF && v.pixels[2] == 0x20 && v.pixels[3] == 0x30); T();
    w(MC_LIVADIX_REG_DEPTH, 16);
    w(MC_LIVADIX_REG_STRIDE, 832 * 2);
    card.vram[0] = 0x7C;                                  /* red 31, green 0, blue 31: x11111 00000 11111 */
    card.vram[1] = 0x1F;
    CHECK(mc_livadix_card_view_get(&card, &v) && v.depth == 32); T();
    CHECK(v.pixels[1] == 0x00 && v.pixels[2] == 0x00 && v.pixels[3] == 0xFF); T();   /* red 255 inverted */
}

static void test_vbl(void) {
    fresh(3);
    const uint64_t period = (uint64_t)MC_C15M_HZ / 60u;   /* 261,120 C15M periods */
    CHECK_EQ(card.period, period); T();
    CHECK_EQ(hw_fake_deadline(&fake, &card), period); T();
    hw_fake_advance(&fake, period - 1);
    CHECK_EQ(r(MC_LIVADIX_REG_STATUS), 0); T();
    hw_fake_advance(&fake, period);
    CHECK_EQ(r(MC_LIVADIX_REG_STATUS), MC_LIVADIX_STATUS_VBL); T();
    CHECK_EQ(irq(), 0); T();                              /* interrupts not enabled */
    CHECK_EQ(hw_fake_deadline(&fake, &card), 2 * period); T();
    /* Enabling them with a blank pending interrupts at once; the
     * acknowledge releases the line. */
    w(MC_LIVADIX_REG_CONTROL, MC_LIVADIX_CONTROL_DISPLAY | MC_LIVADIX_CONTROL_VBL_IRQ);
    CHECK_EQ(irq(), 1); T();
    w(MC_LIVADIX_REG_STATUS, 0);                             /* writing 0 clears nothing */
    CHECK_EQ(irq(), 1); T();
    w(MC_LIVADIX_REG_STATUS, MC_LIVADIX_STATUS_VBL);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(r(MC_LIVADIX_REG_STATUS), 0); T();
    /* Sixty blanks a second of guest time, one interrupt each when
     * acknowledged. */
    unsigned interrupts = 0;
    for (uint64_t t = 2 * period; t <= 61 * period; t += period) {
        hw_fake_advance(&fake, t);
        if (irq()) {
            interrupts++;
            w(MC_LIVADIX_REG_STATUS, MC_LIVADIX_STATUS_VBL);
        }
    }
    CHECK_EQ(interrupts, 60); T();
    CHECK_EQ(card.vbl_count, 61); T();
    /* Disabled again: the blank is seen in STATUS only. */
    w(MC_LIVADIX_REG_CONTROL, MC_LIVADIX_CONTROL_DISPLAY);
    hw_fake_advance(&fake, 62 * period);
    CHECK(r(MC_LIVADIX_REG_STATUS) & MC_LIVADIX_STATUS_VBL); T();
    CHECK_EQ(irq(), 0); T();
    /* Reset stops the interrupt and starts a new frame. */
    w(MC_LIVADIX_REG_CONTROL, 3);
    CHECK_EQ(irq(), 1); T();
    card.dev.ops->reset(&card);
    CHECK_EQ(irq(), 0); T();
    CHECK_EQ(hw_fake_deadline(&fake, &card), 63 * period); T();
}

/* The NuBus slots: windows, bus errors, interrupt lines. */
static uint8_t pa_levels = 0xFF, pa_mask;
static int slotirq = 1;
static void pins(void *ctx, uint8_t levels, uint8_t mask, int high) {
    (void)ctx;
    pa_levels = levels;
    pa_mask = mask;
    slotirq = high;
}
static void on_line(hw_fake *f, void *dev, int line, int state) {
    (void)line;
    mc_nubus_irq((mc_nubus *)f->user, 0xD, state);
    (void)dev;
}

static void test_nubus(void) {
    mc_bus bus;
    mc_nubus nubus;
    CHECK(mc_bus_init(&bus)); T();
    mc_nubus_init(&nubus, &bus, (uint8_t)(1u << 1 | 1u << 5 | 1u << 6), pins, NULL);   /* slots $9, $D, $E */
    mc_nubus_claim(&nubus, 0x9);
    mc_nubus_line(&nubus, 0xD, 4);
    mc_nubus_line(&nubus, 0xE, 5);
    CHECK_EQ(pa_mask, 0x30); T();
    CHECK_EQ(pa_levels, 0x30); T();
    CHECK_EQ(mc_nubus_free_slot(&nubus), 0xD); T();
    CHECK_EQ(mc_nubus_slot_base(0xD), 0xFD000000u); T();
    CHECK_EQ(mc_nubus_super_base(0xD), 0xD0000000u); T();
    /* The card with Livadix's own declaration ROM. */
    size_t rom_size = 0;
    const uint8_t *rom = mc_guest_livadixcard(&rom_size);
    if (card.vram) mc_livadix_card_free(&card);
    hw_fake_init(&fake);
    fake.on_line = on_line;
    fake.user = &nubus;
    CHECK(mc_livadix_card_init(&card, &fake.host, "card", MC_LIVADIX_CARD_VRAM, 3, rom, rom_size, 1)); T();
    CHECK(mc_livadix_card_attach(&card, &nubus, 0xD)); T();
    CHECK(!mc_nubus_fit(&nubus, 0xD)); T();                /* taken */
    CHECK_EQ(mc_nubus_free_slot(&nubus), 0xE); T();
    uint32_t v = 0;
    CHECK(!mc_bus_read(&bus, 0xFDC00000u, 4, &v) && v == 0x4C564458); T();
    CHECK(!mc_bus_write(&bus, 0xFDC00020u, 4, 1024)); T();
    CHECK_EQ(card.width, 1024); T();
    /* VRAM is memory: the CPU's fast path reaches it. */
    CHECK(bus.read_map[0xFD000000u >> MC_BUS_PAGE_SHIFT] == card.vram); T();
    CHECK(bus.write_map[(0xFD000000u + MC_LIVADIX_CARD_VRAM - 1) >> MC_BUS_PAGE_SHIFT] != NULL); T();
    /* The declaration ROM ends at $FDFF FFFF: ByteLanes $0F, the reserved
     * byte, TestPattern $5A932BC7, Format 1 (chapter 8, Figure 8-6). */
    CHECK(!mc_bus_read(&bus, 0xFDFFFFFCu, 4, &v) && v == 0x2BC7000F); T();
    CHECK(!mc_bus_read(&bus, 0xFDFFFFF8u, 4, &v) && v == 0x01015A93); T();
    CHECK(bus.write_map[0xFDFFF000u >> MC_BUS_PAGE_SHIFT] == NULL); T();
    /* The CRC over Length bytes, its own field as zero (chapter 8, "CRC"). */
    uint32_t length = 0, crc = 0, sum = 0;
    CHECK(!mc_bus_read(&bus, 0xFDFFFFF0u, 4, &length)); T();
    CHECK(!mc_bus_read(&bus, 0xFDFFFFF4u, 4, &crc)); T();
    CHECK_EQ(length, rom_size); T();
    for (uint32_t a = 0xFE000000u - length; a != 0xFE000000u; a++) {
        uint32_t b = 0;
        mc_bus_read(&bus, a, 1, &b);
        sum = sum << 1 | sum >> 31;
        if (a < 0xFDFFFFF4u || a > 0xFDFFFFF7u) sum += b;
    }
    CHECK_EQ(sum, crc); T();
    /* The board sResource's BoardId entry (ID 32, word data) is the one the
     * machine looks for in slot PRAM. */
    int board_id = 0;
    for (size_t i = 0; i + 4 <= rom_size; i += 2)
        if (rom[i] == 32 && rom[i + 1] == 0 && (unsigned)(rom[i + 2] << 8 | rom[i + 3]) == MC_LIVADIX_CARD_BOARD_ID) board_id = 1;
    CHECK(board_id); T();
    /* Everything else in the slot, and the empty slot, bus-errors. */
    CHECK(mc_bus_read(&bus, 0xFD800000u, 4, &v)); T();
    CHECK(mc_bus_read(&bus, 0xFDFE0000u, 4, &v)); T();
    CHECK(mc_bus_read(&bus, 0xFEFFFFFCu, 4, &v)); T();
    CHECK(mc_bus_read(&bus, 0xD0000000u, 4, &v)); T();
    /* The card's interrupt: slot $D's line, low on PA4, and /SLOTIRQ. */
    card.dev.ops->write(&card, MC_LIVADIX_REG_CONTROL, 4, 3);
    hw_fake_advance(&fake, card.period);
    CHECK_EQ(pa_levels, 0x20); T();
    CHECK_EQ(slotirq, 0); T();
    card.dev.ops->write(&card, MC_LIVADIX_REG_STATUS, 4, 1);
    CHECK_EQ(pa_levels, 0x30); T();
    CHECK_EQ(slotirq, 1); T();
    mc_nubus_irq_bit(&nubus, 5, 1);
    CHECK(pa_levels == 0x10 && !slotirq); T();
    mc_livadix_card_free(&card);
    mc_bus_free(&bus);
}

/* The blitter's registers for an operation on the map at VRAM 0, rows of
 * `stride` bytes, `d` bits per pixel. */
static void blt(uint32_t op, uint32_t d, uint32_t stride, uint32_t x, uint32_t y, uint32_t wd, uint32_t ht) {
    w(MC_LIVADIX_REG_BLT_OP, op);
    w(MC_LIVADIX_REG_BLT_DEPTH, d);
    w(MC_LIVADIX_REG_BLT_DST, 0);
    w(MC_LIVADIX_REG_BLT_DST_STRIDE, stride);
    w(MC_LIVADIX_REG_BLT_SRC, 0);
    w(MC_LIVADIX_REG_BLT_SRC_STRIDE, stride);
    w(MC_LIVADIX_REG_BLT_DX, x);
    w(MC_LIVADIX_REG_BLT_DY, y);
    w(MC_LIVADIX_REG_BLT_W, wd);
    w(MC_LIVADIX_REG_BLT_H, ht);
}

static void test_blitter(void) {
    fresh(3);
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_CAPS), 0x101); T();
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_STATUS), 0); T();
    /* A fill at 8 bits: 50% gray in pixel values 7 and 9, the pattern's
     * phase moving it a column and a row. */
    blt(MC_LIVADIX_BLT_FILL, 8, 64, 3, 2, 5, 3);
    w(MC_LIVADIX_REG_BLT_FG, 7);
    w(MC_LIVADIX_REG_BLT_BG, 9);
    w(MC_LIVADIX_REG_BLT_PAT0, 0xAA55AA55);
    w(MC_LIVADIX_REG_BLT_PAT1, 0xAA55AA55);
    w(MC_LIVADIX_REG_BLT_PATX, 1);
    w(MC_LIVADIX_REG_BLT_PATY, 0);
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_COUNT), 1); T();
    CHECK_EQ(card.vram[2 * 64 + 2], 0); T();              /* outside: untouched */
    CHECK_EQ(card.vram[2 * 64 + 3], 7); T();              /* row 2 ($AA), column 4: 1 */
    CHECK_EQ(card.vram[2 * 64 + 4], 9); T();
    CHECK_EQ(card.vram[3 * 64 + 3], 9); T();              /* row 3 ($55) */
    CHECK_EQ(card.vram[4 * 64 + 7], 7); T();
    CHECK_EQ(card.vram[4 * 64 + 8], 0); T();
    CHECK_EQ(card.vram[5 * 64 + 3], 0); T();
    /* Busy for the engine's time: 1 us and two 30 ns cycles (15 bytes),
     * in C15M periods, then idle at the card's event. */
    const uint64_t busy = (1000u + 2u * 30u) * (uint64_t)MC_C15M_HZ / 1000000000u + 1u;
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_STATUS), 1); T();
    CHECK_EQ(hw_fake_deadline(&fake, &card), busy); T();
    hw_fake_advance(&fake, busy);
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_STATUS), 0); T();
    CHECK_EQ(hw_fake_deadline(&fake, &card), card.period); T();
    /* Inversion at 1 bit with a clip list: only where the clips meet the
     * rectangle, each pixel once. */
    memset(card.vram, 0, 4096);
    blt(MC_LIVADIX_BLT_INVERT, 1, 8, 2, 0, 20, 2);
    w(MC_LIVADIX_REG_BLT_FG, 1);
    w(MC_LIVADIX_REG_BLT_PAT0, 0xFFFFFFFF);
    w(MC_LIVADIX_REG_BLT_PAT1, 0xFFFFFFFF);
    w(MC_LIVADIX_REG_BLT_CLIPS, 2);
    w(MC_LIVADIX_REG_CLIP, 0 << 16 | 0);
    w(MC_LIVADIX_REG_CLIP + 4, 1 << 16 | 8);                 /* row 0, pixels 0-7 */
    w(MC_LIVADIX_REG_CLIP + 8, 1 << 16 | 16);
    w(MC_LIVADIX_REG_CLIP + 12, 2 << 16 | 40);               /* row 1, pixels 16-39 */
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(card.vram[0], 0x3F); T();                    /* pixels 2-7 */
    CHECK_EQ(card.vram[1], 0x00); T();
    CHECK_EQ(card.vram[8 + 2], 0xFC); T();                /* pixels 16-21 */
    CHECK_EQ(card.vram[8 + 3], 0x00); T();
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_CLIPS), 2); T();
    w(MC_LIVADIX_REG_BLT_CLIPS, 0);
    /* Copies at 16 bits overlapping down and right, and at 4 bits to the
     * left with the pixels' nibbles shifting: as if through a buffer. */
    for (unsigned i = 0; i < 64; i++) card.vram[i] = (uint8_t)i;
    blt(MC_LIVADIX_BLT_COPY, 16, 16, 1, 1, 5, 2);
    w(MC_LIVADIX_REG_BLT_SX, 0);
    w(MC_LIVADIX_REG_BLT_SY, 0);
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(card.vram[16 + 2], 0); T();                  /* row 1, pixel 1 = row 0, pixel 0 */
    CHECK_EQ(card.vram[16 + 3], 1); T();
    CHECK_EQ(card.vram[32 + 2], 16); T();                 /* row 2 = the old row 1 */
    CHECK_EQ(card.vram[32 + 11], 25); T();
    for (unsigned i = 0; i < 8; i++) card.vram[i] = (uint8_t)(0x12 + 0x22 * i);
    blt(MC_LIVADIX_BLT_COPY, 4, 8, 0, 0, 5, 1);
    w(MC_LIVADIX_REG_BLT_SX, 3);
    w(MC_LIVADIX_REG_BLT_SY, 0);
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(card.vram[0], 0x45); T();                    /* pixels 3-7 of 12 34 56 78 9A */
    CHECK_EQ(card.vram[1], 0x67); T();
    CHECK_EQ(card.vram[2], 0x86); T();                    /* pixel 4 copied, pixel 5 kept */
    /* Outside VRAM, or without the blitter: nothing. */
    const uint32_t n = r(MC_LIVADIX_REG_BLT_COUNT);
    blt(MC_LIVADIX_BLT_FILL, 8, 4096, 0, 600, 10, 1);
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_COUNT), n); T();
    mc_livadix_card_free(&card);
    hw_fake_init(&fake);
    CHECK(mc_livadix_card_init(&card, &fake.host, "card", MC_LIVADIX_CARD_VRAM, 3, rom_image, sizeof rom_image, 0));
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_CAPS), 0); T();
    card.vram[0] = 5;
    blt(MC_LIVADIX_BLT_FILL, 8, 64, 0, 0, 4, 4);
    w(MC_LIVADIX_REG_BLT_GO, 1);
    CHECK_EQ(card.vram[0], 5); T();
    CHECK_EQ(r(MC_LIVADIX_REG_BLT_COUNT), 0); T();
}

/* The PCI function: configuration space, BARs, expansion ROM header. */
static void test_pci(void) {
    size_t rom_size = 0;
    const uint8_t *rom = mc_guest_livadixcard_pci(&rom_size);
    CHECK(rom && rom_size >= 512 && (rom_size % 512) == 0); T();
    CHECK_EQ(rom[0], 0x55); T();
    CHECK_EQ(rom[1], 0xAA); T();
    CHECK_EQ(rom[0x02] | rom[0x03] << 8, 0x34); T();       /* LE FCode pointer (PCI binding ch. 9) */
    CHECK_EQ(rom[0x1C], 'P'); T();
    CHECK_EQ(rom[0x1D], 'C'); T();
    CHECK_EQ(rom[0x1E], 'I'); T();
    CHECK_EQ(rom[0x1F], 'R'); T();
    CHECK_EQ(rom[0x20] | rom[0x21] << 8, MC_LIVADIX_PCI_VENDOR); T();
    CHECK_EQ(rom[0x22] | rom[0x23] << 8, MC_LIVADIX_PCI_DEVICE); T();
    CHECK_EQ(rom[0x30], 1); T();                           /* PCIR+$14: Open Firmware FCode */
    CHECK_EQ(rom[0x31], 0x80); T();                        /* PCIR+$15: last image */
    CHECK_EQ(rom[0x34], 0xF1); T();                        /* start1 */
    {
        /* IEEE 1275 5.2.2.5: checksum is the sum of body bytes. */
        const uint32_t total = (uint32_t)rom[0x38] << 24 | (uint32_t)rom[0x39] << 16 |
                               (uint32_t)rom[0x3A] << 8 | rom[0x3B];
        uint32_t sum = 0;
        CHECK(total >= 8 && 0x34 + total <= rom_size); T();
        for (uint32_t i = 8; i < total; i++) sum += rom[0x34 + i];
        CHECK_EQ(sum & 0xFFFFu, (uint32_t)rom[0x36] << 8 | rom[0x37]); T();
    }
    {
        /* The native driver's PEF container (Mac OS Runtime Architectures,
         * chapter 8): its 40-byte header in the FCode's first string of the
         * "driver,AAPL,MacOS,PowerPC" property, three sections, two of them
         * instantiated (code, data; the loader is not). */
        size_t at = 0;
        while (at + 40 <= rom_size && memcmp(rom + at, "Joy!peffpwpc", 12)) at++;
        CHECK(at + 40 <= rom_size); T();
        if (at + 40 <= rom_size) {
            CHECK_EQ(rom[at + 12] << 24 | rom[at + 13] << 16 | rom[at + 14] << 8 | rom[at + 15], 1); T();
            CHECK_EQ(rom[at + 32] << 8 | rom[at + 33], 3); T();
            CHECK_EQ(rom[at + 34] << 8 | rom[at + 35], 2); T();
        }
    }

    if (card.vram) mc_livadix_card_free(&card);
    hw_fake_init(&fake);
    CHECK(mc_livadix_card_init(&card, &fake.host, "card", MC_LIVADIX_CARD_VRAM, 3, rom, rom_size, 1)); T();
    mc_livadix_card_set_depth(&card, 8);
    CHECK_EQ(r(MC_LIVADIX_REG_INIT_DEPTH), 8); T();
    CHECK_EQ(mc_pci_config_read(&card.cfg, MC_PCI_VENDOR, 2), MC_LIVADIX_PCI_VENDOR); T();
    CHECK_EQ(mc_pci_config_read(&card.cfg, MC_PCI_DEVICE, 2), MC_LIVADIX_PCI_DEVICE); T();
    CHECK_EQ(card.cfg.reg[MC_PCI_INT_PIN], 1); T();
    /* Size probes: BAR0 2 MiB memory, BAR1 4 KiB, ROM 64 KiB. */
    mc_pci_config_write(&card.cfg, MC_PCI_BAR0, 4, 0xFFFFFFFFu);
    CHECK_EQ(mc_pci_config_read(&card.cfg, MC_PCI_BAR0, 4) & 0xFFFFFFF0u, ~(MC_LIVADIX_CARD_VRAM - 1u)); T();
    mc_pci_config_write(&card.cfg, MC_PCI_BAR0 + 4, 4, 0xFFFFFFFFu);
    CHECK_EQ(mc_pci_config_read(&card.cfg, MC_PCI_BAR0 + 4, 4) & 0xFFFFFFF0u, ~0xFFFu); T();
    mc_pci_config_write(&card.cfg, MC_PCI_ROM, 4, 0xFFFFFFFFu);
    CHECK_EQ(mc_pci_config_read(&card.cfg, MC_PCI_ROM, 4) & 0xFFFFF800u, ~(MC_LIVADIX_CARD_ROM_WINDOW - 1u)); T();
    /* Assign BARs and enable memory + ROM decode. */
    mc_pci_config_write(&card.cfg, MC_PCI_BAR0, 4, 0x80000000u);
    mc_pci_config_write(&card.cfg, MC_PCI_BAR0 + 4, 4, 0x81000000u);
    mc_pci_config_write(&card.cfg, MC_PCI_ROM, 4, 0x82000001u);
    mc_pci_config_write(&card.cfg, MC_PCI_COMMAND, 2, MC_PCI_CMD_MEMORY);
    CHECK(mc_livadix_card_pci_decoding(&card)); T();
    mc_pci_function fn = mc_livadix_card_function(&card);
    uint32_t v = 0;
    CHECK(fn.mem_read(&card, 0x81000000u, 4, &v) && mc_pci_swap(v, 4) == 0x4C564458u); T();
    card.vram[0] = 0xAB;
    CHECK(fn.mem_read(&card, 0x80000000u, 1, &v) && (v & 0xFFu) == 0xAB); T();
    CHECK(fn.mem_read(&card, 0x82000000u, 1, &v) && (v & 0xFFu) == 0x55); T();
    CHECK(fn.mem_read(&card, 0x82000001u, 1, &v) && (v & 0xFFu) == 0xAA); T();
}

int main(void) {
    test_reset();
    test_registers();
    test_clut_gamma();
    test_view();
    test_vbl();
    test_blitter();
    test_nubus();
    test_pci();
    printf("test_livadix_card: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
