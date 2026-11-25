/* Livadix — 24-bit address decode (amu.h). Livadix's own
 * code. */
#include "amu.h"

#include <stdlib.h>
#include <string.h>

/* The Macintosh II ROM's 24-bit table (ROM offset $50: short page
 * descriptors $00000019 ... $00700019, $40000019, $F9000059 ... $FE000059,
 * $50000059): RAM, the ROM, slots $9-$E, the I/O space. */
const uint32_t mc_amu_mac_ii_map[16] = {
    0x00000000u, 0x00100000u, 0x00200000u, 0x00300000u, 0x00400000u, 0x00500000u, 0x00600000u, 0x00700000u,
    0x40000000u, 0xF9000000u, 0xFA000000u, 0xFB000000u, 0xFC000000u, 0xFD000000u, 0xFE000000u, 0x50000000u,
};

/* Pages in 16 MiB: the folded tables repeat every this many entries. */
#define SPAN24 (1u << (24u - MC_BUS_PAGE_SHIFT))

int mc_amu_init(mc_amu *a, const uint32_t seg[16]) {
    memset(a, 0, sizeof *a);
    memcpy(a->seg, seg, sizeof a->seg);
    a->read_map = calloc(MC_BUS_PAGES, sizeof *a->read_map);
    a->write_map = calloc(MC_BUS_PAGES, sizeof *a->write_map);
    a->stale = 1;
    if (!a->read_map || !a->write_map) {
        mc_amu_free(a);
        return 0;
    }
    return 1;
}

void mc_amu_free(mc_amu *a) {
    free(a->read_map);
    free(a->write_map);
    a->read_map = a->write_map = NULL;
}

/* The first 16 MiB from the bus's pages at the mapped addresses, then the
 * same 4096 entries for every value of A31-A24. */
static void build(mc_amu *a, const mc_bus *bus) {
    for (uint32_t p = 0; p < SPAN24; p++) {
        const uint32_t pa = mc_amu_map(a, p << MC_BUS_PAGE_SHIFT) >> MC_BUS_PAGE_SHIFT;
        a->read_map[p] = bus->read_map[pa];
        a->write_map[p] = bus->write_map[pa];
    }
    for (uint32_t p = SPAN24; p < MC_BUS_PAGES; p += SPAN24) {
        memcpy(a->read_map + p, a->read_map, SPAN24 * sizeof *a->read_map);
        memcpy(a->write_map + p, a->write_map, SPAN24 * sizeof *a->write_map);
    }
    a->stale = 0;
}

void mc_amu_bus_changed(mc_amu *a, const mc_bus *bus) {
    a->stale = 1;
    if (a->on) build(a, bus);
}

int mc_amu_set(mc_amu *a, const mc_bus *bus, int on) {
    on = on != 0;
    if (on == a->on) return 0;
    a->on = on;
    if (on && a->stale) build(a, bus);
    return 1;
}
