/* Livadix — 24-bit address decode.
 *
 * On a 68020 machine without a PMMU the board decodes the CPU's addresses
 * itself. In 24-bit mode A31-A24 are ignored and each MiB of the 24-bit
 * space goes to a 32-bit base: the Macintosh II's AMU (Address Mapping
 * Unit, in the 68851's socket) and the LC's V8 do this. The map is the
 * machine description's (`mc_hw_desc.map24`, 16 bases); the Macintosh II's
 * is the one its ROM builds for a 68851 to stand in for the AMU (16 short
 * page descriptors at ROM offset $50): RAM at $000000-
 * $7FFFFF, the ROM at $800000, slot s ($9-$E) at $s00000 -> $Fs000000, I/O
 * at $F00000 -> $50000000.
 *
 * The decode is a pair of page tables the CPU uses directly (the fast path
 * stays): the 4 GiB logical space folded onto the 24-bit map, entry for
 * entry the bus's tables at the mapped physical pages, rebuilt when the
 * bus's tables change and only while the mode is on. Device pages stay
 * NULL; their accesses reach the machine, which maps the address before
 * the bus decodes it (mc_amu_map). Switching modes gives the CPU the other
 * tables (m68k_set_maps): its soft TLB and the translator's blocks are
 * checked against them, so nothing translated in one mode runs in the
 * other. Livadix's own design. */
#ifndef MC_AMU_H
#define MC_AMU_H

#include "bus.h"

#include <stdint.h>

typedef struct mc_amu {
    uint32_t seg[16];               /* 32-bit base of each MiB of the 24-bit space */
    uint8_t **read_map, **write_map;/* MC_BUS_PAGES entries each, built while on */
    int on;                         /* 24-bit mode */
    int stale;                      /* the bus's tables changed since the last build */
} mc_amu;

/* A decoder over `seg` (16 bases, each MiB-aligned); 0 when out of memory.
 * It starts in 32-bit mode (addresses go to the bus unchanged). */
int mc_amu_init(mc_amu *a, const uint32_t seg[16]);
void mc_amu_free(mc_amu *a);
/* The bus's tables changed: rebuild now if on, else at the next switch. */
void mc_amu_bus_changed(mc_amu *a, const mc_bus *bus);
/* 24-bit mode on or off; 1 when the mode changed (the CPU then needs the
 * other tables: machine.c's mc_hw_set_addr24). */
int mc_amu_set(mc_amu *a, const mc_bus *bus, int on);

/* The Macintosh II's map (Macintosh II ROMs 97851DB6 and 9779D2C4, the
 * table at ROM offset $50 that stands in for the AMU when a 68851 is
 * fitted): the Macintosh II and II FDHD descriptions' (machines/macii.c),
 * switched by VIA2 PB3, which floats high (32 bits) while an input. */
extern const uint32_t mc_amu_mac_ii_map[16];

/* The physical address of logical `la` in 24-bit mode. */
static inline uint32_t mc_amu_map(const mc_amu *a, uint32_t la) { return a->seg[(la >> 20) & 15u] + (la & 0xFFFFFu); }

#endif
