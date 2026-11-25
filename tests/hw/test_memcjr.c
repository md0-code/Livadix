/* test_memcjr: the MEMCjr's RAM decoding (memcjr.h): the bank layouts of
 * the Developer Note's sizes, the six-bit data path (a register write
 * takes its upper six bits from the window at +$7C, a register read loads
 * the window), the ROM's sizing configuration (bank n at n x 64 MB, each
 * repeating through its window, empty banks floating), the packed
 * configuration it leaves (the banks contiguous from 0 in 1 MB units, the
 * last MB in +$2C) and a bank marked empty (bit 10). Against a fake host
 * and a real bus. */
#include "hw_test.h"
#include "asic/memcjr.h"

#include <stdlib.h>

static int tests;
static hw_fake fake;
static mc_bus bus;
static mc_memcjr memc;
static uint8_t *ram;
static unsigned remaps;

#define MB (1u << 20)
#define WINDOW (64u * MB)
#define REG_WINDOW 0x7Cu

static void remap(void *ctx) {
    (void)ctx;
    remaps++;
    mc_memcjr_map(&memc, &bus, ram);
}

static uint32_t rd(uint32_t address) {
    uint32_t v = 0xDEADBEEFu;
    CHECK_EQ(mc_bus_read(&bus, address, 4, &v), 0);     /* RAM space never bus-errors */
    return v;
}
static void wr(uint32_t address, uint32_t value) { CHECK_EQ(mc_bus_write(&bus, address, 4, value), 0); }
static void reg_write(uint32_t offset, uint32_t value) { mc_memcjr_ops.write(&memc, offset, 4, value); }
static uint32_t reg_read(uint32_t offset) { return mc_memcjr_ops.read(&memc, offset, 4); }

/* A register written as the ROM writes it (ROM offset $4968): the value
 * rotated right by six to the window, then the value itself. */
static void rom_write(uint32_t offset, uint32_t value) {
    reg_write(REG_WINDOW, value >> 6 | value << 26);
    reg_write(offset, value);
}
/* ... and read (ROM offset $F8446): the register's low six bits joined
 * with the window's six. */
static uint32_t rom_read(uint32_t offset) {
    const uint32_t low = reg_read(offset) & 0x3Fu;
    return (reg_read(REG_WINDOW) & 0x3Fu) << 6 | low;
}

static void fresh(uint32_t mb) {
    uint32_t banks[MC_MEMCJR_BANKS];
    CHECK(mc_memcjr_layout(mb, banks));
    free(ram);
    ram = calloc(1, (size_t)mb * MB);
    hw_fake_init(&fake);
    mc_memcjr_init(&memc, &fake.host, "MEMCjr", banks, remap, NULL);
    remaps = 0;
    mc_memcjr_map(&memc, &bus, ram);
}

/* The ROM's sizing configuration (ROM offset $A0156): the window and the
 * end at $FFFFFFFF, then bank n at n x $40 (64 MB) with the window n. */
static void sizing_configuration(void) {
    reg_write(REG_WINDOW, 0xFFFFFFFFu);
    reg_write(0x2C, 0xFFFFFFFFu);
    for (unsigned n = 0; n < MC_MEMCJR_BANKS; n++) rom_write(0x04 + 4 * n, 0x40u * n);
}

static void test_layout(void) {
    tests++;
    uint32_t b[MC_MEMCJR_BANKS];
    /* Figure 1-4: 4 MB on the board and a SIMM of one or two sides. */
    static const struct { uint32_t total, b1, b2; } sizes[] = {
        {4, 0, 0}, {5, 1, 0}, {6, 1, 1}, {8, 4, 0}, {12, 4, 4}, {20, 16, 0}, {36, 16, 16},
    };
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        CHECK(mc_memcjr_layout(sizes[i].total, b));
        CHECK_EQ(b[0], 4 * MB);
        CHECK_EQ(b[1], sizes[i].b1 * MB);
        CHECK_EQ(b[2], sizes[i].b2 * MB);
        CHECK_EQ(b[3], 0);
        CHECK_EQ(b[4], 0);
    }
    CHECK(!mc_memcjr_layout(7, b));
    CHECK(!mc_memcjr_layout(40, b));
    CHECK(!mc_memcjr_layout(3, b));
}

static void test_registers(void) {
    tests++;
    fresh(20);
    /* Until the ROM programs it: the banks packed from 0, the last MB 19. */
    CHECK_EQ(rom_read(0x04), 0);
    CHECK_EQ(rom_read(0x08), 4);
    CHECK_EQ(rom_read(0x2C), 19);
    CHECK_EQ(rd(0x00400010u), 0);
    wr(0x00400010u, 0x01020304u);
    CHECK_EQ(ram[4 * MB + 0x13], 0x04);
    /* The six-bit path: the upper six bits come from the window. */
    reg_write(REG_WINDOW, 0x15);
    reg_write(0x30, 0x2A);
    CHECK_EQ(reg_read(0x30) & 0xFFFu, 0x56A);
    reg_write(REG_WINDOW, 0);
    CHECK_EQ(reg_read(REG_WINDOW), 0);
    (void)reg_read(0x30);
    CHECK_EQ(reg_read(REG_WINDOW), 0x15);                 /* the read loaded it */
    /* The ROM's timing values (ROM offset $4962) through its accessors. */
    rom_write(0x30, 0x6A9);
    rom_write(0x34, 0xBA5);
    CHECK_EQ(rom_read(0x30), 0x6A9);
    CHECK_EQ(rom_read(0x34), 0xBA5);
    /* Other registers read back what was written; a timing register does
     * not change the decoding. */
    const unsigned before = remaps;
    rom_write(0x40, 0x123);
    CHECK_EQ(rom_read(0x40), 0x123);
    CHECK_EQ(remaps, before);
    CHECK(!mc_memcjr_ops.stable(&memc, 0x30, 4));
}

static void test_sizing_windows(void) {
    tests++;
    fresh(36);
    sizing_configuration();
    CHECK(remaps > 0);
    /* Bank 0 (4 MB on the board) at 0, repeating through 64 MB. */
    wr(0x00000010u, 0x11111111u);
    CHECK_EQ(rd(0x00400010u), 0x11111111u);
    CHECK_EQ(rd(0x03C00010u), 0x11111111u);
    /* Bank 1 (16 MB) at 64 MB, every 16 MB. */
    wr(0x04000020u, 0x22222222u);
    CHECK_EQ(rd(0x05000020u), 0x22222222u);
    CHECK_EQ(rd(0x07000020u), 0x22222222u);
    CHECK_EQ(ram[4 * MB + 0x23], 0x22);
    /* Bank 2 (the SIMM's second side) at 128 MB. */
    wr(0x08000030u, 0x33333333u);
    CHECK_EQ(ram[20 * MB + 0x33], 0x33);
    /* Banks 3 and 4 are empty: they float. */
    wr(0x0C000040u, 0x44444444u);
    CHECK_EQ(rd(0x0C000040u), 0);
    CHECK_EQ(rd(0x10000040u), 0);
    CHECK_EQ(rd(0x30000000u), 0);
}

static void test_packed(void) {
    tests++;
    fresh(36);
    sizing_configuration();
    for (unsigned n = 0; n < 3; n++) wr(n * WINDOW + 0x100u, 0xC0000000u + n);
    /* What the ROM leaves (ROM offset $A05DA): each bank's start in MB
     * (window bit 4, the bank's mark, kept from its read), the last MB in
     * +$2C. */
    const uint32_t starts[MC_MEMCJR_BANKS] = {0, 4, 20, 36, 36};
    for (unsigned n = 0; n < MC_MEMCJR_BANKS; n++) {
        (void)reg_read(0x04 + 4 * n);
        const uint32_t mark = reg_read(REG_WINDOW) & 0x10u;
        rom_write(0x04 + 4 * n, mark << 6 | starts[n]);
    }
    rom_write(0x2C, 35);
    CHECK_EQ(rom_read(0x08), 4);
    CHECK_EQ(rom_read(0x2C), 35);
    CHECK_EQ(rd(0x00000100u), 0xC0000000u);
    CHECK_EQ(rd(0x00400100u), 0xC0000001u);
    CHECK_EQ(rd(0x01400100u), 0xC0000002u);
    /* Contiguous: 4 + 16 + 16 MB. */
    wr(0x013FFFFCu, 0x12345678u);
    CHECK_EQ(ram[20 * MB - 1], 0x78);
    wr(0x023FFFFCu, 0x9ABCDEF0u);
    CHECK_EQ(ram[36 * MB - 1], 0xF0);
    /* Above the end, nothing. */
    wr(0x02400000u, 0x55555555u);
    CHECK_EQ(rd(0x02400000u), 0);
    /* A bank marked empty (bit 10, ROM offset $A0598) does not answer: the
     * bank below repeats up to the next bank that does. */
    rom_write(0x08, 0x400u | 4u);
    CHECK_EQ(rd(0x00400100u), 0xC0000000u);
    CHECK_EQ(ram[4 * MB + 0x100], 0xC0);
    CHECK_EQ(rd(0x01400100u), 0xC0000002u);
    rom_write(0x08, 4u);
    CHECK_EQ(rd(0x00400100u), 0xC0000001u);
}

int main(void) {
    CHECK(mc_bus_init(&bus));
    test_layout();
    test_registers();
    test_sizing_windows();
    test_packed();
    mc_bus_free(&bus);
    free(ram);
    printf("test_memcjr: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
