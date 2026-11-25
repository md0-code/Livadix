/* test_mdu: the Macintosh IIci's address decode (asic/mdu.c).
 *
 * Livadix's own tests, from the Guide to the Macintosh Family Hardware, 2nd
 * ed., Figure 3-8 and chapter 5 (mdu.h): the bank populations, the reset
 * overlay, bank A from 0 and bank B from $0400 0000 each repeating through
 * its 64 MB window, an empty bank B floating, the ROM repeated through
 * $4000 0000-$4FFF FFFF, the cache card's space bus-erroring without a card;
 * then the IIci machine's I/O decode: the block repeated every $4 0000 up to
 * $50FF FFFF, the RBV also at $5100 0000, a hole the MDU acknowledges and
 * one it does not. */
#include "asic/mdu.h"
#include "bus.h"
#include "hw_machine.h"
#include "hw_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_SIZE (512u << 10)

static uint32_t rd(mc_bus *b, uint32_t a, unsigned size, int *fault) {
    uint32_t v = 0;
    *fault = mc_bus_read(b, a, size, &v);
    return v;
}

static void test_populations(void) {
    uint32_t a = 0, b = 0;
    CHECK(mc_mdu_banks(4, &a, &b) && a == 4u << 20 && b == 0);
    CHECK(mc_mdu_banks(5, &a, &b) && a == 1u << 20 && b == 4u << 20);     /* the smaller SIMMs in bank A */
    CHECK(mc_mdu_banks(8, &a, &b) && a == 4u << 20 && b == 4u << 20);
    CHECK(mc_mdu_banks(20, &a, &b) && a == 4u << 20 && b == 16u << 20);
    CHECK(mc_mdu_banks(128, &a, &b) && a == 64u << 20 && b == 64u << 20);
    CHECK(!mc_mdu_banks(6, &a, &b));
    CHECK(!mc_mdu_banks(256, &a, &b));
    for (const uint16_t *r = mc_mdu_ram_mb; *r; r++) {
        CHECK(mc_mdu_banks(*r, &a, &b));
        CHECK_EQ((a + b) >> 20, *r);
    }
}

static void test_decode(void) {
    mc_bus bus;
    CHECK(mc_bus_init(&bus));
    uint8_t *rom = calloc(1, ROM_SIZE), *ram = calloc(1, 8u << 20);
    if (!rom || !ram) return;
    for (uint32_t i = 0; i < ROM_SIZE; i++) rom[i] = (uint8_t)(i * 7u + 3u);
    mc_mdu mdu;
    mc_mdu_init(&mdu, 4u << 20, 4u << 20);
    int fault = 0;
    /* From reset the ROM answers at 0, through the RAM area. */
    CHECK(mdu.overlay);
    mc_mdu_map_fixed(&mdu, &bus, rom, ROM_SIZE);
    mc_mdu_map_ram(&mdu, &bus, ram, rom, ROM_SIZE);
    CHECK_EQ(rd(&bus, 0x10, 1, &fault), rom[0x10]);
    CHECK_EQ(rd(&bus, 0x00080010u, 1, &fault), rom[0x10]);
    CHECK(!fault);
    /* The ROM through its whole range. */
    CHECK_EQ(rd(&bus, 0x40800004u, 1, &fault), rom[4]);
    CHECK_EQ(rd(&bus, 0x4FF80004u, 1, &fault), rom[4]);
    CHECK(!fault);
    CHECK(mc_bus_write(&bus, 0x40800004u, 1, 0x55) == 0);   /* ignored, no bus error */
    CHECK_EQ(rd(&bus, 0x40800004u, 1, &fault), rom[4]);
    /* The overlay ends: the banks. */
    mdu.overlay = 0;
    mc_mdu_map_ram(&mdu, &bus, ram, rom, ROM_SIZE);
    CHECK(mc_bus_write(&bus, 0x00000100u, 4, 0x11223344u) == 0);
    CHECK_EQ(rd(&bus, 0x00000100u, 4, &fault), 0x11223344u);
    CHECK_EQ(ram[0x100], 0x11);
    /* Bank A repeats through its window every 4 MB. */
    CHECK_EQ(rd(&bus, 0x00400100u, 4, &fault), 0x11223344u);
    CHECK_EQ(rd(&bus, 0x03C00100u, 4, &fault), 0x11223344u);
    /* Bank B from $0400 0000, after bank A in the host RAM, repeating. */
    CHECK(mc_bus_write(&bus, 0x04000200u, 4, 0xCAFEF00Du) == 0);
    CHECK_EQ(ram[(4u << 20) + 0x200], 0xCA);
    CHECK_EQ(rd(&bus, 0x04400200u, 4, &fault), 0xCAFEF00Du);
    CHECK_EQ(rd(&bus, 0x07C00200u, 4, &fault), 0xCAFEF00Du);
    CHECK_EQ(rd(&bus, 0x00000200u, 4, &fault), 0);           /* not bank A */
    CHECK(!fault);
    /* Above bank B's window the RAM area floats. */
    CHECK_EQ(rd(&bus, 0x08000000u, 4, &fault), 0);
    CHECK(!fault);
    CHECK_EQ(rd(&bus, 0x3FFFFFFCu, 4, &fault), 0);
    CHECK(!fault);
    /* No cache card: its space and the expansion space bus-error. */
    rd(&bus, MC_MDU_CACHE, 4, &fault);
    CHECK(fault);
    rd(&bus, 0x58000000u, 4, &fault);
    CHECK(fault);
    /* An empty bank B floats. */
    mc_mdu_init(&mdu, 4u << 20, 0);
    mdu.overlay = 0;
    mc_mdu_map_ram(&mdu, &bus, ram, rom, ROM_SIZE);
    CHECK_EQ(rd(&bus, 0x04000200u, 4, &fault), 0);
    CHECK(!fault);
    CHECK(mc_bus_write(&bus, 0x04000200u, 4, 0x12345678u) == 0);
    CHECK_EQ(rd(&bus, 0x04000200u, 4, &fault), 0);
    mc_bus_free(&bus);
    free(rom);
    free(ram);
}

/* The machine's I/O decode, on the IIci board built around a ROM image
 * that only carries the IIci's checksum (the CPU does not run). */
static void test_machine_io(void) {
    uint8_t *rom = calloc(1, ROM_SIZE);
    if (!rom) return;
    rom[0] = 0x36; rom[1] = 0x8C; rom[2] = 0xAD; rom[3] = 0xFE;
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find("iici"));
    c.ram_mb = 8;
    c.width = 640; c.height = 480; c.depth = 8;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256] = "";
    mc_machine *m = mc_machine_create(&c, rom, ROM_SIZE, error, sizeof error);
    CHECK(m != NULL);
    if (!m) {
        printf("%s\n", error);
        free(rom);
        return;
    }
    mc_hw *hw = mc_machine_hw(m);
    CHECK(hw && hw->cpu->model == M68K_68030);
    int fault = 0;
    /* VIA1's IER (register 14 at +$1C00): set bits read back with bit 7, in
     * every image of the block ($5000 0000 + n x $4 0000 to $50FF FFFF). */
    CHECK(mc_bus_write(&hw->bus, 0x50001C00u, 1, 0x82) == 0);
    CHECK_EQ(rd(&hw->bus, 0x50001C00u, 1, &fault), 0x82);
    CHECK_EQ(rd(&hw->bus, 0x50F01C00u, 1, &fault), 0x82);
    CHECK_EQ(rd(&hw->bus, 0x50FC1C00u, 1, &fault), 0x82);
    CHECK(!fault);
    /* The RBV's interrupt enables in the block and at $5100 0000. */
    CHECK(mc_bus_write(&hw->bus, 0x50F26013u, 1, 0x90) == 0);
    CHECK_EQ(rd(&hw->bus, 0x51000013u, 1, &fault) & 0x7Fu, 0x10);
    CHECK_EQ(rd(&hw->bus, 0x50F27C13u, 1, &fault) & 0x7Fu, 0x10);    /* the VIA2 offsets the shared code uses */
    CHECK(!fault);
    /* The byte below the SCC: acknowledged, reads 0. */
    CHECK_EQ(rd(&hw->bus, 0x50F03FFFu, 1, &fault), 0);
    CHECK(!fault);
    /* Between the SWIM and the VDAC nothing answers: a bus error (the
     * ROM's decoder identification relies on it, mdu.h). */
    rd(&hw->bus, 0x50F21C00u, 1, &fault);
    CHECK(fault);
    /* The ASC's version register, in both halves of its $2000 window. */
    CHECK_EQ(rd(&hw->bus, 0x50F14800u, 1, &fault), 0x00);
    CHECK_EQ(rd(&hw->bus, 0x50F15800u, 1, &fault), 0x00);
    CHECK(!fault);
    /* NuBus: no card in slots $C-$E, and $9-$B are not slots. */
    rd(&hw->bus, 0xFCFFFFFCu, 4, &fault);
    CHECK(fault);
    rd(&hw->bus, 0xF9FFFFFCu, 4, &fault);
    CHECK(fault);
    mc_machine_destroy(m);
    free(rom);
}

int main(void) {
    test_populations();
    test_decode();
    test_machine_io();
    printf("test_mdu: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
