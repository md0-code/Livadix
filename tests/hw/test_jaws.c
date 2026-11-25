/* test_jaws: the 68030 PowerBooks' system controllers: Jaws (asic/jaws.c,
 * also serving Niagara's registers) and the Duos' MSC (asic/msc.c).
 *
 * Livadix's own tests, from jaws.h's and msc.h's observations: Jaws's
 * registers a byte every $2000 bytes, read back as written, cleared at
 * reset, and its 60.15 Hz tick (a square wave of the classic Macintosh's
 * frame); the MSC's VIA2 registers as the RBV's but port B
 * bit 1, the PMU's /ACK, an input read from the line, and bit 2, /REQ,
 * reported on every write. Then the boards: each family built from a
 * made-up ROM answers at its decoder's addresses (Jaws's I/O block
 * repeating every MB, Niagara's and the MSC's at $5000 0000 and
 * $50F0 0000 only), the MSC's CPU ID register reads $A55A and the Duo's
 * word. */
#include "asic/jaws.h"
#include "asic/msc.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hw_fake f;

static void test_jaws(void) {
    mc_jaws j;
    hw_fake_init(&f);
    mc_jaws_init(&j, &f.host, "Jaws");
    mc_jaws_ops.reset(&j);
    mc_jaws_ops.write(&j, 0x34000, 1, 0x5A);
    CHECK_EQ(mc_jaws_ops.read(&j, 0x34000, 1), 0x5Au);
    CHECK_EQ(mc_jaws_ops.read(&j, 0x34001, 1), 0x5Au);      /* one register per $2000 */
    CHECK_EQ(mc_jaws_ops.read(&j, 0x36000, 1), 0u);
    /* The tick: about 60 rises a second. */
    hw_fake_advance(&f, MC_C15M_HZ);
    const unsigned changes = hw_fake_changes(&f, &j, MC_JAWS_LINE_TICK);
    CHECK(changes >= 119 && changes <= 122);
    mc_jaws_ops.reset(&j);
    CHECK_EQ(mc_jaws_ops.read(&j, 0x34000, 1), 0u);
}

static void test_msc(void) {
    mc_msc m;
    hw_fake_init(&f);
    mc_msc_init(&m, &f.host, "MSC");
    mc_msc_ops.reset(&m);
    mc_msc_set_ack(&m, 1);
    mc_msc_ops.write(&m, MC_RBV_DATAB, 1, 0xF9);              /* /REQ low, the written bit 1 low */
    CHECK_EQ(hw_fake_value(&f, &m.rbv, MC_RBV_LINE_PB), 0xF9);
    CHECK_EQ(mc_msc_ops.read(&m, MC_RBV_DATAB, 1) & 0x02u, 0x02u);   /* /ACK high */
    mc_msc_set_ack(&m, 0);
    CHECK_EQ(mc_msc_ops.read(&m, MC_RBV_DATAB, 1) & 0x02u, 0u);
    CHECK_EQ(mc_msc_ops.read(&m, MC_RBV_DATAB, 1) & 0x04u, 0u);       /* /REQ as written */
    /* The level-2 line from the SCSI interrupt, as the RBV's. */
    mc_msc_ops.write(&m, MC_RBV_IER, 1, 0x80 | MC_RBV_IRQ_SCSI);
    mc_rbv_set_irq(&m.rbv, MC_RBV_IRQ_SCSI, 1);
    CHECK_EQ(hw_fake_line(&f, &m.rbv, MC_RBV_LINE_IRQ), 1);
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
    return mc_bus_read(&hw->bus, address, 1, &v) == 0;     /* 0: the access completed */
}

static void test_boards(void) {
    mc_machine *m = board("powerbook-170", 0x420DBFF3u, 1u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        CHECK(answers(hw, 0x50F01C00u));            /* VIA1 */
        CHECK(answers(hw, 0x51001C00u));            /* repeated a MB on */
        CHECK(answers(hw, 0x50F80000u));            /* Jaws's registers */
        CHECK(!answers(hw, 0x50F41C00u));
        mc_machine_destroy(m);
    }
    m = board("powerbook-180", 0xE33B2724u, 1u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        CHECK(answers(hw, 0x50F01C00u));
        CHECK(answers(hw, 0x50001C00u));            /* the MMU's physical block */
        CHECK(!answers(hw, 0x50101C00u));           /* no repeat (the ROM's test) */
        CHECK(answers(hw, 0x50F20001u));            /* the GSC */
        mc_machine_destroy(m);
    }
    m = board("powerbook-duo-230", 0xECFA989Bu, 1u << 20, 8);
    CHECK(m != NULL);
    if (m) {
        mc_hw *hw = mc_machine_hw(m);
        uint32_t id = 0;
        CHECK_EQ(mc_bus_read(&hw->bus, 0x5FFFFFFCu, 4, &id), 0);
        CHECK_EQ(id, 0xA55A1005u);
        CHECK(answers(hw, 0x50F26000u));            /* the MSC's VIA2 registers */
        CHECK(!answers(hw, 0x50F02000u));           /* no VIA2 */
        mc_machine_destroy(m);
    }
}

int main(void) {
    test_jaws();
    test_msc();
    test_boards();
    printf("test_jaws: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
