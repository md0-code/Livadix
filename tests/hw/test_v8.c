/* test_v8: the V8 family ASIC (asic/v8.c) and the LC board's RAM decode.
 *
 * Livadix's own tests, from v8.h's sources: the RAM configuration register
 * (bits 7-6 the SIMMs' bank, read back whole, a change of the bank
 * reported), the VIA2 registers as the RBV's (the level-2 line, the sense
 * code in register $10 beside the depth), the VDAC's auto-incrementing
 * address over red, green and blue, the picture with rows 1024 bytes apart
 * and a pixel's bits on top of the CLUT index, the deepest depth each ASIC
 * shows; then, on the LC board (machines/lc.c, with a made-up ROM), the
 * RAM area for each configuration: the SIMMs from 0, the built-in RAM
 * after them, its top 2 MB at $80 0000, and the decode's mirrors through
 * A30-A28. */
#include "asic/v8.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hw_fake f;
static mc_v8 v8;
static int remaps;

static void on_remap(void *ctx) {
    (void)ctx;
    remaps++;
}
static uint8_t rd(uint32_t reg) { return (uint8_t)mc_v8_ops.read(&v8, reg, 1); }
static void wr(uint32_t reg, uint8_t v) { mc_v8_ops.write(&v8, reg, 1, v); }

static void setup(mc_v8_kind kind, uint8_t sense) {
    hw_fake_init(&f);
    remaps = 0;
    mc_v8_init(&v8, &f.host, "V8", kind, sense, on_remap, NULL);
}

static void test_ram_config(void) {
    setup(MC_V8_V8, MC_RBV_SENSE_RGB12);
    CHECK_EQ(rd(MC_V8_RAMCFG), 0);
    CHECK_EQ(mc_v8_simm_bank(&v8), 0u);
    /* The LC ROM's probe of the built-in RAM: bits 2-1 kept, no remap. */
    wr(MC_V8_RAMCFG, 0x06);
    CHECK_EQ(rd(MC_V8_RAMCFG), 0x06);
    CHECK_EQ(remaps, 0);
    /* The probe configuration and the final ones (0, 2, 4, 8 MB). */
    static const struct { uint8_t value; uint32_t bank; } cfg[] = {
        {0xC4, 8u << 20}, {0x24, 0}, {0x64, 2u << 20}, {0xA4, 4u << 20}, {0xE4, 8u << 20},
    };
    for (size_t i = 0; i < sizeof cfg / sizeof *cfg; i++) {
        wr(MC_V8_RAMCFG, cfg[i].value);
        CHECK_EQ(rd(MC_V8_RAMCFG), cfg[i].value);
        CHECK_EQ(mc_v8_simm_bank(&v8), cfg[i].bank);
    }
    CHECK_EQ(remaps, 5);
}

static void test_via2(void) {
    setup(MC_V8_V8, MC_RBV_SENSE_HIRES);
    /* Register $10: the sense code (bits 5-3) beside the depth written. */
    wr(MC_RBV_MONP, 0x02);
    CHECK_EQ(rd(MC_RBV_MONP), 0x32);
    /* The interrupt enables and flags of the RBV: the level-2 line. */
    mc_rbv_set_irq(&v8.rbv, MC_RBV_IRQ_SCSI, 1);
    CHECK(!hw_fake_line(&f, &v8.rbv, MC_RBV_LINE_IRQ));
    wr(MC_RBV_IER, 0x80 | MC_RBV_IRQ_SCSI);
    CHECK(hw_fake_line(&f, &v8.rbv, MC_RBV_LINE_IRQ));
    CHECK_EQ(rd(MC_RBV_IFR), 0x80 | MC_RBV_IRQ_SCSI);
    mc_rbv_set_irq(&v8.rbv, MC_RBV_IRQ_SCSI, 0);
    CHECK(!hw_fake_line(&f, &v8.rbv, MC_RBV_LINE_IRQ));
    /* The Classic II's six-bit register at its window's first byte. */
    setup(MC_V8_EAGLE, MC_RBV_SENSE_RGB12);
    mc_v8_eagle_ops.write(&v8, 0, 1, 0xFF);
    CHECK_EQ(mc_v8_eagle_ops.read(&v8, 0, 1), 0x3Fu);
    CHECK_EQ(mc_v8_eagle_ops.read(&v8, 0x38, 4), 0u);
}

static void test_video(void) {
    setup(MC_V8_V8, MC_RBV_SENSE_RGB12);
    /* The VDAC: the address, then red, green, blue, the address moving on. */
    mc_v8_vdac_ops.write(&v8, 0, 1, 0x7F);
    for (int i = 0; i < 3; i++) mc_v8_vdac_ops.write(&v8, 1, 1, 0xFF);
    for (int i = 0; i < 3; i++) mc_v8_vdac_ops.write(&v8, 1, 1, 0x00);
    CHECK_EQ(mc_v8_vdac_ops.read(&v8, 0, 1), 0x81u);
    mc_v8_vdac_ops.write(&v8, 0, 1, 0xFF);
    for (int i = 0; i < 3; i++) mc_v8_vdac_ops.write(&v8, 1, 1, 0x00);
    mc_v8_vdac_ops.write(&v8, 2, 1, 0x08);
    CHECK_EQ(mc_v8_vdac_ops.read(&v8, 2, 1), 0x08u);
    static uint8_t vram[512 << 10];
    mc_v8_view view;
    /* Video off (bit 6, the reset value) shows nothing. */
    CHECK(!mc_v8_view_get(&v8, vram, sizeof vram, NULL, &view));
    wr(MC_RBV_MONP, 0x00);
    CHECK(mc_v8_view_get(&v8, vram, sizeof vram, NULL, &view));
    CHECK_EQ(view.width, 512u);
    CHECK_EQ(view.height, 384u);
    CHECK_EQ(view.depth, 1u);
    CHECK_EQ(view.row_bytes, 1024u);
    /* 1 bit: pixel 0 is entry $7F (white), pixel 1 entry $FF (black). */
    CHECK_EQ(view.palette[0], 0xFFFFFFFFu);
    CHECK_EQ(view.palette[1], 0xFF000000u);
    wr(MC_RBV_MONP, 0x03);
    CHECK(mc_v8_view_get(&v8, vram, sizeof vram, NULL, &view));
    CHECK_EQ(view.depth, 8u);
    CHECK_EQ(view.palette[0x7F], 0xFFFFFFFFu);
    /* The deepest depths: the V8 8 bits at both monitors with 512 KB,
     * Spice 16 bits, Eagle 1 bit. */
    CHECK_EQ(mc_v8_max_depth(MC_V8_V8, MC_RBV_SENSE_RGB12, 512 << 10), 8u);
    CHECK_EQ(mc_v8_max_depth(MC_V8_V8, MC_RBV_SENSE_HIRES, 512 << 10), 8u);
    CHECK_EQ(mc_v8_max_depth(MC_V8_V8, MC_RBV_SENSE_HIRES, 256 << 10), 0u);
    CHECK_EQ(mc_v8_max_depth(MC_V8_SPICE, MC_RBV_SENSE_RGB12, 512 << 10), 16u);
    CHECK_EQ(mc_v8_max_depth(MC_V8_EAGLE, MC_RBV_SENSE_RGB12, 0), 1u);
    CHECK_EQ(mc_v8_max_depth(MC_V8_V8, MC_RBV_SENSE_NONE, 512 << 10), 0u);
}

/* The LC board's RAM area: a ROM image with the LC's checksum, 6 MB (4 MB
 * of SIMMs and the 2 MB built in); the V8's register written as the ROM
 * would, then the bytes read where each configuration puts them. */
static uint32_t peek(mc_hw *hw, uint32_t a) {
    uint32_t v = 0;
    return mc_bus_peek(&hw->bus, a, 4, &v) ? v : 0xDEADDEADu;
}

static void test_board_ram(void) {
    /* The header longword is all the board checks (romdb.h). */
    static uint8_t rom[512 << 10];
    const uint32_t want = 0x350EACF0u;
    for (int i = 0; i < 4; i++) rom[i] = (uint8_t)(want >> (24 - 8 * i));
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find("lc"));
    c.ram_mb = 6;
    c.width = 512;
    c.height = 384;
    c.depth = 8;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256];
    mc_machine *m = mc_machine_create(&c, rom, sizeof rom, error, sizeof error);
    CHECK(m != NULL);
    if (!m) {
        printf("  %s\n", error);
        return;
    }
    mc_hw *hw = mc_machine_hw(m);
    /* The overlay ends at the first access to the ROM's area. */
    uint32_t v = 0;
    CHECK(mc_bus_read(&hw->bus, 0x00A00000u, 4, &v) == 0);
    CHECK_EQ(v, want);
    for (uint32_t i = 0; i < 6u << 20; i += 4) {
        hw->ram[i] = (uint8_t)(i >> 20);
        hw->ram[i + 1] = (uint8_t)(i >> 12);
    }
    /* 4 MB of SIMMs ($A4): SIMMs from 0, the built-in RAM from 4 MB, its
     * top 2 MB (all of it) at $80 0000 too; mirrors at $4000 0000. */
    mc_bus_write(&hw->bus, 0x50F26001u, 1, 0xA4);
    CHECK_EQ(peek(hw, 0x00000000u) >> 16, 0x0000u);
    CHECK_EQ(peek(hw, 0x00300000u) >> 16, 0x0300u);
    CHECK_EQ(peek(hw, 0x00400000u) >> 16, 0x0400u);
    CHECK_EQ(peek(hw, 0x00500000u) >> 16, 0x0500u);
    CHECK_EQ(peek(hw, 0x00800000u) >> 16, 0x0400u);
    CHECK_EQ(peek(hw, 0x40500000u) >> 16, 0x0500u);
    /* The probe configuration ($C4): the SIMMs repeating through 8 MB,
     * the built-in RAM at $80 0000 only. */
    mc_bus_write(&hw->bus, 0x50F26001u, 1, 0xC4);
    CHECK_EQ(peek(hw, 0x00500000u) >> 16, 0x0100u);
    CHECK_EQ(peek(hw, 0x00900000u) >> 16, 0x0500u);
    /* The I/O block mirrored: VIA1's port B at $10F0 0000 as at
     * $50F0 0000. */
    CHECK_EQ(mc_bus_read(&hw->bus, 0x10F00000u, 1, &v), 0);
    mc_machine_destroy(m);
}

int main(void) {
    test_ram_config();
    test_via2();
    test_video();
    test_board_ram();
    printf("test_v8: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
