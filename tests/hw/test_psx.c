/* Original-ROM PSX segment selectors and physical DIMM populations. */
#include "hw_test.h"
#include "ppc/chips/psx.h"
#include <stdio.h>

int main(void) {
    hw_fake fake;
    hw_fake_init(&fake);
    uint32_t banks[MC_PSX_BANKS];
    CHECK(mc_psx_population(16u << 20, 0, banks));
    CHECK_EQ(banks[0], 8u << 20);
    CHECK_EQ(banks[1], 8u << 20);
    CHECK(mc_psx_population(32u << 20, 0, banks));
    CHECK_EQ(banks[1], 16u << 20);
    CHECK_EQ(banks[3], 8u << 20);
    CHECK(mc_psx_population(136u << 20, 0, banks));
    for (unsigned bank = 1; bank < 5; bank++) CHECK_EQ(banks[bank], 32u << 20);
    CHECK(mc_psx_population(12u << 20, 0, banks));
    CHECK_EQ(banks[0], 8u << 20);
    CHECK_EQ(banks[1], 4u << 20);
    CHECK_EQ(banks[2] | banks[3] | banks[4], 0);
    CHECK(mc_psx_population(76u << 20, 0, banks));
    CHECK_EQ(banks[1], 32u << 20);
    CHECK_EQ(banks[2], 32u << 20);
    CHECK_EQ(banks[3], 4u << 20);
    CHECK(!mc_psx_population(64u << 20, 0, banks));
    CHECK(mc_psx_population(128u << 20, 1, banks));
    CHECK_EQ(banks[0], 0);
    CHECK(!mc_psx_population(136u << 20, 1, banks));
    CHECK(mc_psx_population(16u << 20, 0, banks));
    mc_psx chip;
    mc_psx_init(&chip, &fake.host, banks, 2);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x10, 4), 2);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x13, 1), 2);
    mc_psx_ops.write(&chip, 0x40, 4, 0x88881100);
    for (unsigned reg = 0x48; reg <= 0x60; reg += 8) mc_psx_ops.write(&chip, reg, 4, 0x88888888);
    mc_mem_window windows[MC_PSX_SEGMENTS];
    unsigned n = mc_psx_windows(&chip, windows, MC_PSX_SEGMENTS);
    CHECK_EQ(n, 4);
    for (unsigned i = 0; i < 4; i++) {
        CHECK_EQ(windows[i].base, i * MC_PSX_SEGMENT);
        CHECK_EQ(windows[i].offset, i * MC_PSX_SEGMENT);
    }
    mc_psx_ops.write(&chip, 0x28, 4, 0x10000);
    CHECK_EQ(mc_psx_windows(&chip, windows, MC_PSX_SEGMENTS), 2);
    mc_psx_ops.write(&chip, 0x40, 4, 0x00000000);
    CHECK_EQ(mc_psx_windows(&chip, windows, 2), 8);
    CHECK_EQ(windows[0].offset, 0);
    CHECK_EQ(windows[1].offset, MC_PSX_SEGMENT);
    mc_psx_ops.write(&chip, 0x42, 2, 0x8888);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x40, 4), 0x00008888);
    mc_psx_ops.write(&chip, 0x40, 1, 0x11);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x40, 4), 0x11008888);
    mc_psx_ops.reset(&chip);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x40, 4), 0);
    CHECK_EQ(mc_psx_ops.read(&chip, 0x28, 4), 0x1F0000);
    printf("PSX: %d failures\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
