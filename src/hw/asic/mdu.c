/* Livadix — the Macintosh IIci's Memory Decode Unit (mdu.h).
 *
 * Livadix's own code. */
#include "mdu.h"

#include <string.h>

/* Four equal SIMMs per bank: 256 KB, 1 MB, 4 MB or 16 MB give a bank of
 * 1, 4, 16 or 64 MB (Guide to the Macintosh Family Hardware, 2nd ed.,
 * Figure 5-11 and its note; 4 and 16 MB SIMMs for the 128 MB maximum).
 * Totals of at least Livadix's 4 MB minimum; for each, bank A gets the
 * smaller SIMMs. */
static const struct { uint16_t total, a, b; } populations[] = {
    {4, 4, 0}, {5, 1, 4}, {8, 4, 4}, {16, 16, 0}, {17, 1, 16}, {20, 4, 16}, {32, 16, 16},
    {64, 64, 0}, {65, 1, 64}, {68, 4, 64}, {80, 16, 64}, {128, 64, 64},
};

const uint16_t mc_mdu_ram_mb[] = {4, 5, 8, 16, 17, 20, 32, 64, 65, 68, 80, 128, 0};

int mc_mdu_banks(uint32_t ram_mb, uint32_t *bank_a, uint32_t *bank_b) {
    for (size_t i = 0; i < sizeof populations / sizeof *populations; i++)
        if (populations[i].total == ram_mb) {
            *bank_a = (uint32_t)populations[i].a << 20;
            *bank_b = (uint32_t)populations[i].b << 20;
            return 1;
        }
    return 0;
}

void mc_mdu_init(mc_mdu *m, uint32_t bank_a, uint32_t bank_b) {
    memset(m, 0, sizeof *m);
    m->bank_a = bank_a;
    m->bank_b = bank_b;
    m->overlay = 1;
}

void mc_mdu_map_ram(const mc_mdu *m, mc_bus *bus, uint8_t *ram, const uint8_t *rom, uint32_t rom_size) {
    /* The RAM area floats where no bank answers (†, mdu.h). */
    mc_bus_map_float(bus, 0, MC_MDU_ROM);
    if (m->overlay) {
        /* The reset overlay: the ROM repeated through the RAM area. */
        mc_bus_map_memory(bus, 0, MC_MDU_ROM, (uint8_t *)rom, rom_size, 0);
        return;
    }
    mc_bus_map_memory(bus, MC_MDU_BANK_A, MC_MDU_WINDOW, ram, m->bank_a, 1);
    if (m->bank_b) mc_bus_map_memory(bus, MC_MDU_BANK_B, MC_MDU_WINDOW, ram + m->bank_a, m->bank_b, 1);
}

void mc_mdu_map_fixed(const mc_mdu *m, mc_bus *bus, const uint8_t *rom, uint32_t rom_size) {
    (void)m;
    mc_bus_map_memory(bus, MC_MDU_ROM, 0x10000000u, (uint8_t *)rom, rom_size, 0);
    /* No cache card, nothing in the expansion and factory test space: a
     * bus error (†, mdu.h). */
    mc_bus_unmap(bus, MC_MDU_CACHE, 0x0E000000u);
}
