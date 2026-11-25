/* test_djmemc: the DJMEMC's RAM decoding (djmemc.h): the bank layouts of
 * the boards' sockets, the registers, the sizing configuration the ROM sets
 * (each bank at n x 64 MB, repeating through its window, empty banks
 * floating), the ROM's own sizing walk finding each bank's size, and the
 * packed configuration it leaves (the banks contiguous from 0, the space
 * above the end floating). Against a fake host and a real bus. */
#include "hw_test.h"
#include "asic/djmemc.h"

#include <stdlib.h>

static int tests;
static hw_fake fake;
static mc_bus bus;
static mc_djmemc memc;
static uint8_t *ram;
static unsigned remaps;

#define MB (1u << 20)
#define WINDOW (64u * MB)

static void remap(void *ctx) {
    (void)ctx;
    remaps++;
    mc_djmemc_map(&memc, &bus, ram);
}

static uint32_t rd(uint32_t address) {
    uint32_t v = 0xDEADBEEFu;
    CHECK_EQ(mc_bus_read(&bus, address, 4, &v), 0);     /* RAM space never bus-errors */
    return v;
}
static void wr(uint32_t address, uint32_t value) { CHECK_EQ(mc_bus_write(&bus, address, 4, value), 0); }
static void reg_write(uint32_t offset, uint32_t value) { mc_djmemc_ops.write(&memc, offset, 4, value); }
static uint32_t reg_read(uint32_t offset, unsigned size) { return mc_djmemc_ops.read(&memc, offset, size); }

/* A board of `mb` MiB with `onboard` MiB on the board. */
static void fresh(uint32_t mb, uint32_t onboard, unsigned simm_banks) {
    uint32_t banks[MC_DJMEMC_BANKS];
    CHECK(mc_djmemc_layout(mb, onboard, simm_banks, banks));
    free(ram);
    ram = calloc(1, (size_t)mb * MB);
    hw_fake_init(&fake);
    mc_djmemc_init(&memc, &fake.host, "DJMEMC", banks, remap, NULL);
    remaps = 0;
    mc_djmemc_map(&memc, &bus, ram);
}

/* The ROM's sizing configuration (ROM offset $A0160): configuration 0, the
 * end at $FFFFFFFF, bank n at n x 64 MB (bit 8 set). */
static void sizing_configuration(void) {
    reg_write(0x00, 0);
    reg_write(0x2C, 0xFFFFFFFFu);
    for (unsigned n = 0; n < MC_DJMEMC_BANKS; n++) reg_write(0x04 + 4 * n, 0x100u + 0x10u * n);
}

/* The ROM's size probe of a bank (ROM offset $A01FE): a pattern written
 * at the top longword of each 4 MB step from the window's top down, until
 * one reads back; then the lowest address the pattern shows at is the
 * bank's size. 0 for an empty bank. */
static uint32_t rom_size_probe(uint32_t base) {
    const uint32_t pattern = 0xAB96919Eu;
    for (uint32_t top = WINDOW; top >= 4u * MB; top -= 4u * MB) {
        const uint32_t a = base + top - 4u;
        const uint32_t saved = rd(a);
        wr(a, pattern);
        if (rd(a) == pattern) {
            uint32_t size = 4u * MB;
            while (size < top && rd(base + size - 4u) != pattern) size *= 2u;
            wr(a, saved);
            return size;
        }
    }
    return 0;
}

static void test_layout(void) {
    tests++;
    uint32_t b[MC_DJMEMC_BANKS];
    /* 24 MB on a Quadra 800: 8 on the board (two banks), one 16 MB bank. */
    CHECK(mc_djmemc_layout(24, 8, 8, b));
    CHECK_EQ(b[0], 4 * MB); CHECK_EQ(b[1], 4 * MB); CHECK_EQ(b[2], 16 * MB); CHECK_EQ(b[3], 0);
    /* 136 MB: the maximum, four 32 MB SIMMs as eight 16 MB banks. */
    CHECK(mc_djmemc_layout(136, 8, 8, b));
    for (unsigned i = 2; i < MC_DJMEMC_BANKS; i++) CHECK_EQ(b[i], 16 * MB);
    CHECK(!mc_djmemc_layout(152, 8, 8, b));
    /* The Centris 610: 4 MB on the board, two sockets (four banks). */
    CHECK(mc_djmemc_layout(68, 4, 4, b));
    CHECK_EQ(b[0], 4 * MB); CHECK_EQ(b[1], 0); CHECK_EQ(b[2], 16 * MB); CHECK_EQ(b[5], 16 * MB); CHECK_EQ(b[6], 0);
    CHECK(!mc_djmemc_layout(84, 4, 4, b));
    CHECK(mc_djmemc_layout(12, 8, 8, b));
    CHECK_EQ(b[2], 4 * MB);
    CHECK(!mc_djmemc_layout(10, 8, 8, b));
    CHECK(mc_djmemc_layout(4, 8, 8, b));        /* a board with 4 MB of its 8 */
    CHECK_EQ(b[0], 4 * MB); CHECK_EQ(b[1], 0);
}

static void test_registers(void) {
    tests++;
    fresh(24, 8, 8);
    /* Until programmed: the banks packed from 0, the end at 24 MB. */
    CHECK_EQ(reg_read(0x04, 4), 0);
    CHECK_EQ(reg_read(0x08, 4), 1);
    CHECK_EQ(reg_read(0x0C, 4), 2);
    CHECK_EQ(reg_read(0x10, 4), 6);
    CHECK_EQ(reg_read(0x2C, 4), 6);
    /* Longword registers, read back whole or in parts. */
    reg_write(0x30, 0x000000FBu);
    reg_write(0x34, 0x000001E7u);
    CHECK_EQ(reg_read(0x34, 4), 0x1E7);
    CHECK_EQ(reg_read(0x36, 2), 0x1E7);
    CHECK_EQ(reg_read(0x34, 2), 0);
    CHECK_EQ(reg_read(0x37, 1), 0xE7);
    CHECK_EQ(remaps, 0);                    /* timing registers decode nothing */
    mc_djmemc_ops.write(&memc, 0x0B, 1, 0x05);
    CHECK_EQ(reg_read(0x08, 4), 5);
    CHECK_EQ(remaps, 1);                    /* a bank start remaps */
    mc_djmemc_ops.write(&memc, 0x0B, 1, 0x05);
    CHECK_EQ(remaps, 1);                    /* the same value does not */
}

static void test_sizing_windows(void) {
    tests++;
    fresh(24, 8, 8);
    sizing_configuration();
    /* Bank 0 (4 MB) repeats through its 64 MB window. */
    wr(0x00000010u, 0x11111111u);
    CHECK_EQ(rd(0x00400010u), 0x11111111u);
    CHECK_EQ(rd(0x03C00010u), 0x11111111u);
    /* Bank 1 at 64 MB is the second 4 MB of the board. */
    wr(0x04000020u, 0x22222222u);
    CHECK_EQ(rd(0x07C00020u), 0x22222222u);
    CHECK_EQ(rd(0x00000020u), 0);
    CHECK_EQ(ram[4 * MB + 0x23], 0x22);
    /* Bank 2 (16 MB) at 128 MB, every 16 MB. */
    wr(0x08000030u, 0x33333333u);
    CHECK_EQ(rd(0x09000030u), 0x33333333u);
    CHECK_EQ(rd(0x0B000030u), 0x33333333u);
    CHECK_EQ(ram[8 * MB + 0x33], 0x33);
    /* An empty bank floats: reads 0, writes are lost. */
    wr(0x0C000040u, 0x44444444u);
    CHECK_EQ(rd(0x0C000040u), 0);
    CHECK_EQ(rd(0x24000000u), 0);
    /* The ROM's probe sizes every bank. */
    CHECK_EQ(rom_size_probe(0x00000000u), 4 * MB);
    CHECK_EQ(rom_size_probe(0x04000000u), 4 * MB);
    CHECK_EQ(rom_size_probe(0x08000000u), 16 * MB);
    for (unsigned n = 3; n < MC_DJMEMC_BANKS; n++) CHECK_EQ(rom_size_probe(n * WINDOW), 0);
}

static void test_packed(void) {
    tests++;
    fresh(40, 8, 8);
    sizing_configuration();
    for (unsigned n = 0; n < 4; n++) wr(n * WINDOW + 0x100u, 0xB0000000u + n);
    /* What the ROM leaves (ROM offset $A052A): each bank's start in 4 MB
     * units in the low byte, bit 8 kept, the end at 40 MB. */
    const uint32_t starts[MC_DJMEMC_BANKS] = {0, 1, 2, 6, 10, 10, 10, 10, 10, 10};
    for (unsigned n = 0; n < MC_DJMEMC_BANKS; n++) reg_write(0x04 + 4 * n, 0x100u | starts[n]);
    reg_write(0x2C, 10);
    CHECK_EQ(rd(0x00000100u), 0xB0000000u);
    CHECK_EQ(rd(0x00400100u), 0xB0000001u);
    CHECK_EQ(rd(0x00800100u), 0xB0000002u);
    CHECK_EQ(rd(0x01800100u), 0xB0000003u);
    /* Contiguous: the last longword of one bank is followed by the next. */
    wr(0x017FFFFCu, 0x12345678u);
    CHECK_EQ(ram[24 * MB - 1], 0x78);
    wr(0x027FFFFCu, 0x9ABCDEF0u);
    CHECK_EQ(ram[40 * MB - 1], 0xF0);
    /* Above the end, nothing. */
    wr(0x02800000u, 0x55555555u);
    CHECK_EQ(rd(0x02800000u), 0);
    CHECK_EQ(rd(0x3FFFFFFCu), 0);
}

int main(void) {
    CHECK(mc_bus_init(&bus));
    test_layout();
    test_registers();
    test_sizing_windows();
    test_packed();
    mc_bus_free(&bus);
    free(ram);
    printf("test_djmemc: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
