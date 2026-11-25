/* Livadix — the GLUE of the Macintosh II family and the SE/30 (glue.h).
 *
 * Livadix's own code. */
#include "glue.h"

#include <string.h>

/* Four equal SIMMs per bank (256 KB, 1, 4 or 16 MB): banks of 1, 4, 16 or
 * 64 MB, the larger in bank A (its ICs set the window bank B follows). */
static const struct { uint16_t total, a, b; } populations[] = {
    {4, 4, 0}, {5, 4, 1}, {8, 4, 4}, {16, 16, 0}, {17, 16, 1}, {20, 16, 4}, {32, 16, 16},
    {64, 64, 0}, {65, 64, 1}, {68, 64, 4}, {80, 64, 16}, {128, 64, 64},
};

const uint16_t mc_glue_ram_mb[] = {4, 5, 8, 16, 17, 20, 32, 64, 65, 68, 80, 128, 0};

int mc_glue_banks(uint32_t ram_mb, uint32_t *bank_a, uint32_t *bank_b) {
    for (size_t i = 0; i < sizeof populations / sizeof *populations; i++)
        if (populations[i].total == ram_mb) {
            *bank_a = (uint32_t)populations[i].a << 20;
            *bank_b = (uint32_t)populations[i].b << 20;
            return 1;
        }
    return 0;
}

void mc_glue_init(mc_glue *g, uint32_t bank_a, uint32_t bank_b) {
    memset(g, 0, sizeof *g);
    g->bank_a = bank_a;
    g->bank_b = bank_b;
    g->overlay = 1;
}

void mc_glue_map_ram(const mc_glue *g, mc_bus *bus, uint8_t *ram, const uint8_t *rom, uint32_t rom_size) {
    if (g->overlay) {
        /* The ROM overlay map: the ROM through the whole RAM space. */
        mc_bus_map_memory(bus, 0, MC_GLUE_RAM_SPACE, (uint8_t *)rom, rom_size, 0);
        return;
    }
    /* The RAM space modulo twice bank A's window: bank A, then bank B. */
    const uint32_t w = mc_glue_window(g);
    for (uint32_t base = 0; base < MC_GLUE_RAM_SPACE; base += 2u * w) {
        mc_bus_map_memory(bus, base, w, ram, g->bank_a, 1);
        if (g->bank_b) mc_bus_map_memory(bus, base + w, w, ram + g->bank_a, g->bank_b, 1);
        else mc_bus_map_float(bus, base + w, w);
    }
}

void mc_glue_map_fixed(const mc_glue *g, mc_bus *bus, const uint8_t *rom, uint32_t rom_size) {
    (void)g;
    mc_bus_map_memory(bus, MC_GLUE_ROM, MC_GLUE_ROM_SPACE, (uint8_t *)rom, rom_size, 0);
    mc_bus_unmap(bus, MC_GLUE_UNDECODED, 0x0F000000u);
    mc_bus_unmap(bus, MC_GLUE_NUBUS_LOCAL, 0x01000000u);
}

int mc_glue_set_size_bits(mc_glue *g, uint8_t pa) {
    const uint8_t bits = (uint8_t)(pa >> 6 & 3u);
    if (bits == g->size_bits) return 0;
    g->size_bits = bits;
    return 1;
}
