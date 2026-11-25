/* test_pratt: the PowerBook 520/540 and 190 system ASIC (asic/pratt.c)
 * and the boards of 5.2h (machines/powerbook500.c, the PowerBook 150 in
 * machines/powerbook.c).
 *
 * Livadix's own tests, from pratt.h's and powerbook500.c's observations:
 * Pratt's register windows read back as written and clear at reset, its
 * 60.15 Hz tick; then each board built from a made-up ROM answers at its
 * decoder record's addresses, its CPU ID register reads $A55A and the
 * board's word, the 190's IDE window keeps its control word apart from the
 * task file, the 150's ATA task file answers at $50F1 A000. */
#include "asic/pratt.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hw_fake f;

static void test_registers(void) {
    mc_pratt p;
    hw_fake_init(&f);
    mc_pratt_init(&p, &f.host, "Pratt", 0);
    mc_pratt_ops.reset(&p);
    mc_pratt_ops.write(&p, 0, 1, 0x80);
    CHECK_EQ(mc_pratt_ops.read(&p, 0, 1), 0x80u);
    mc_pratt_c_ops.write(&p, 0x0F, 1, 0x07);
    CHECK_EQ(mc_pratt_c_ops.read(&p, 0x0F, 1), 0x07u);
    CHECK_EQ(mc_pratt_b_ops.read(&p, 0x0F, 1), 0u);         /* each window its own */
    mc_pratt_d_ops.write(&p, 0, 2, 0x1234);
    CHECK_EQ(mc_pratt_d_ops.read(&p, 0, 2), 0x1234u);
    hw_fake_advance(&f, MC_C15M_HZ);
    const unsigned changes = hw_fake_changes(&f, &p, MC_PRATT_LINE_TICK);
    CHECK(changes >= 119 && changes <= 122);
    mc_pratt_ops.reset(&p);
    CHECK_EQ(mc_pratt_ops.read(&p, 0, 1), 0u);
}

static mc_machine *board(const char *model, uint32_t checksum, size_t rom_size, uint32_t ram) {
    uint8_t *rom = calloc(1, rom_size);
    if (!rom) return NULL;
    rom[0] = (uint8_t)(checksum >> 24);
    rom[1] = (uint8_t)(checksum >> 16);
    rom[2] = (uint8_t)(checksum >> 8);
    rom[3] = (uint8_t)checksum;
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find(model));
    c.ram_mb = ram;
    c.cpu_hz = mc_model_find(model)->cpu_hz;
    c.controller = MC_CONTROLLER_PROTOCOL;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256];
    mc_machine *m = mc_machine_create(&c, rom, rom_size, error, sizeof error);
    free(rom);
    if (!m) printf("  %s: %s\n", model, error);
    return m;
}

static int answers(mc_hw *hw, uint32_t address) {
    uint32_t v;
    return mc_bus_read(&hw->bus, address, 1, &v) == 0;
}

static void test_boards(void) {
    mc_machine *m = board("powerbook-520", 0xB6909089u, 2u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        uint32_t v = 0;
        CHECK_EQ(mc_bus_read(&hw->bus, 0x5FFFFFFCu, 4, &v), 0);
        CHECK_EQ(v, 0xA55A1808u);
        CHECK_EQ(mc_bus_read(&hw->bus, 0x5FFFFFF8u, 4, &v), 0);
        CHECK_EQ(v, 0u);
        CHECK(answers(hw, 0x50F01C00u));            /* VIA1 */
        CHECK(answers(hw, 0x50F03C00u));            /* VIA2 */
        CHECK(answers(hw, 0x50F10000u));            /* the 5380 */
        CHECK(answers(hw, 0x50F0A000u));            /* Pratt */
        CHECK(answers(hw, 0x50080007u));
        CHECK(answers(hw, 0x50F20000u));            /* the CSC */
        CHECK(answers(hw, 0xFEEFE004u));            /* the external video's sense */
        CHECK_EQ(mc_bus_read(&hw->bus, 0xFEEFE004u, 1, &v), 0);
        CHECK_EQ(v & 0xF0u, 0x70u);                 /* no monitor */
        CHECK(!answers(hw, 0x50F1A000u));           /* no ATA */
        mc_machine_destroy(m);
    }
    m = board("powerbook-190", 0x4D27039Cu, 2u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        uint32_t v = 0;
        CHECK_EQ(mc_bus_read(&hw->bus, 0x5FFFFFFCu, 4, &v), 0);
        CHECK_EQ(v, 0xA55A180Bu);
        CHECK_EQ(mc_bus_write(&hw->bus, 0x50F1A0D0u, 2, 0x0054u), 0);
        CHECK_EQ(mc_bus_read(&hw->bus, 0x50F1A0D0u, 2, &v), 0);
        CHECK_EQ(v, 0x0054u);                       /* the IDE control word */
        CHECK_EQ(mc_bus_read(&hw->bus, 0x50F1A0D4u, 2, &v), 0);
        CHECK_EQ(v, 0u);                            /* no INTRQ */
        CHECK_EQ(mc_bus_read(&hw->bus, 0x50F20000u, 1, &v), 0);
        CHECK_EQ(v, 0u);                            /* the gray 190's GSC: register 0 reads 0 */
        mc_machine_destroy(m);
    }
    m = board("powerbook-190cs", 0x4D27039Cu, 2u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        uint32_t v = 0;
        CHECK_EQ(mc_bus_read(&mc_machine_hw(m)->bus, 0x50F20000u, 1, &v), 0);
        CHECK(v != 0);                              /* the CSC */
        mc_machine_destroy(m);
    }
    m = board("powerbook-150", 0xFDA22562u, 1u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        uint32_t v = 0;
        CHECK_EQ(mc_bus_read(&hw->bus, 0x5FFFFFFCu, 4, &v), 0);
        CHECK_EQ(v, 0xA55A1001u);
        CHECK(answers(hw, 0x50F1A01Cu));            /* the ATA's status */
        CHECK(answers(hw, 0x50F26000u));            /* the MSC's VIA2 registers */
        mc_machine_destroy(m);
    }
}

int main(void) {
    test_registers();
    test_boards();
    printf("test_pratt: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
