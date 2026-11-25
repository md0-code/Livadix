/* test_hmc: the PDM boards' HMC (hmc.h) and CPU ID register (cpuid.h): the
 * 35-bit serial register as the ROM writes and reads it, the cache size
 * pins, the bank windows and their repetition as the ROM's sizing sees
 * them, the ROM decode, the CPU ID's value and its read-only behaviour. */
#include "hw_test.h"
#include "ppc/chips/cpuid.h"
#include "ppc/chips/hmc.h"

#define MB (1024u * 1024u)

static hw_fake fake;

/* The ROM's write of r13/r14 ($FFF03838): +8, then 32 + 3 bits LSB first. */
static void put_config(mc_hmc *h, uint32_t lo, uint32_t hi) {
    mc_hmc_ops.write(h, 8, 1, 0x50);
    for (unsigned i = 0; i < 32; i++) mc_hmc_ops.write(h, 0, 1, (lo >> i) & 0xFFu);
    for (unsigned i = 0; i < 3; i++) mc_hmc_ops.write(h, 0, 1, (hi >> i) & 0xFFu);
}

static uint64_t get_config(mc_hmc *h) {
    mc_hmc_ops.write(h, 8, 1, 0x50);
    uint64_t v = 0;
    for (unsigned i = 0; i < 35; i++) v |= (uint64_t)(mc_hmc_ops.read(h, 0, 1) & 1u) << i;
    return v;
}

static void test_register(void) {
    mc_hmc h;
    const uint32_t simm[2] = {32u * MB, 0};
    mc_hmc_init(&h, &fake.host, "HMC", MC_HMC_BOARD_7100_8100, 8u * MB, simm, 2, 256);
    mc_hmc_ops.reset(&h);
    /* Only bit 0 of each byte counts (the ROM writes whole bytes of r13). */
    put_config(&h, 0x0D532010u | 0x00000002u, 5);
    const uint64_t v = get_config(&h);
    /* Bits 1-0 are the cache SIMM's pins: 256 KB reads 2. */
    CHECK_EQ(v & 3u, 2);
    CHECK_EQ(v >> 2 & 0x3FFFFFFFu, 0x0D532010u >> 2);
    CHECK_EQ(v >> 32, 5);
    mc_hmc n;
    mc_hmc_init(&n, &fake.host, "HMC", MC_HMC_BOARD_7100_8100, 8u * MB, NULL, 0, 0);
    put_config(&n, 0xFFFFFFFFu, 0);
    CHECK_EQ(get_config(&n) & 3u, 0);          /* no cache SIMM */
    mc_hmc_init(&n, &fake.host, "HMC", MC_HMC_BOARD_7100_8100, 8u * MB, NULL, 0, 1024);
    mc_hmc_ops.reset(&n);
    CHECK_EQ(get_config(&n) & 3u, 1);          /* 9150/120: 1 MB, Table 3 */
}

/* The ROM's sizing ($FFF03B00): a signature at the window's top, then the
 * lowest address that reads it back. */
static void test_banks(void) {
    mc_hmc h;
    const uint32_t simm[8] = {8u * MB, 0, 32u * MB, 2u * MB};
    mc_hmc_init(&h, &fake.host, "HMC", MC_HMC_BOARD_7100_8100, 8u * MB, simm, 8, 0);
    CHECK_EQ(h.ram_size, 50u * MB);
    CHECK_EQ(mc_hmc_ram_offset(&h, 0), 0);
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x007FFFFFu), 8 * MB - 1);
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x00800000u), -1);           /* between the windows */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x01000000u), 8 * MB);       /* bank 1 */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x01800000u), 8 * MB);       /* 8 MB repeats through 32 */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x05000000u), -1);           /* bank 2 empty */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x09000004u), 16 * MB + 4);  /* bank 3 */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x0D200000u), 48 * MB);      /* 2 MB repeating */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x1D000000u), -1);           /* bank 8 empty */
    CHECK_EQ(mc_hmc_ram_offset(&h, 0x0F000000u), -1);
    mc_hmc s;
    const uint32_t six[2] = {16u * MB, 4u * MB};
    mc_hmc_init(&s, &fake.host, "HMC", MC_HMC_BOARD_6100, 8u * MB, six, 2, 0);
    CHECK_EQ(mc_hmc_ram_offset(&s, 0x10000000u), 8 * MB);       /* bank B */
    CHECK_EQ(mc_hmc_ram_offset(&s, 0x17000000u), 8 * MB);
    CHECK_EQ(mc_hmc_ram_offset(&s, 0x08000010u), 24 * MB + 16); /* bank C */
    CHECK_EQ(mc_hmc_ram_offset(&s, 0x01000000u), -1);
}

static void test_rom(void) {
    CHECK_EQ(mc_hmc_rom_offset(0xFFF00100u), 0x300100);
    CHECK_EQ(mc_hmc_rom_offset(0x40000000u), 0);
    CHECK_EQ(mc_hmc_rom_offset(0x4080C358u), 0xC358);          /* the 68k code's view */
    CHECK_EQ(mc_hmc_rom_offset(0x50000000u), -1);
    CHECK_EQ(mc_hmc_rom_offset(0xFEFFFFFFu), -1);
}

static void test_cpuid(void) {
    mc_cpuid c;
    mc_cpuid_init(&c, &fake.host, "CPUID", MC_CPUID_PDM_7100);
    CHECK_EQ(mc_cpuid_ops.read(&c, 0, 4), 0xA55A3012u);
    mc_cpuid_ops.write(&c, 0, 4, 0);                           /* the 68k code's longword test ($FFC0FFFA) */
    CHECK_EQ(mc_cpuid_ops.read(&c, 0, 4), 0);                  /* the bus's value†: on to the byte test */
    mc_cpuid_ops.write(&c, 0, 1, 0x6A);                        /* the byte test ($FFC1001E) */
    CHECK_EQ(mc_cpuid_ops.read(&c, 0, 1), 0xA5);
    CHECK_EQ(mc_cpuid_ops.read(&c, 1, 1), 0x5A);
    /* The PowerPC code's two byte reads ($FFF03044). */
    CHECK_EQ(mc_cpuid_ops.read(&c, 2, 1) << 8 | mc_cpuid_ops.read(&c, 3, 1), 0x3012);
    CHECK_EQ(MC_CPUID_PDM_8100 & 7u, 3);                       /* Table 1-5 */
    CHECK_EQ(MC_CPUID_PDM_6100 & 7u, 0);
}

int main(void) {
    hw_fake_init(&fake);
    test_register();
    test_banks();
    test_rom();
    test_cpuid();
    printf("test_hmc: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
