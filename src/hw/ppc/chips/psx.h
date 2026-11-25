/* Livadix - PSX/PSX+ memory decoder.
 * Apple Power Macintosh 5500/6500 Developer Note, ch.2 (PSX+, five
 * banks, two DIMM slots); Performa 6400 Developer Note ch.4, Table 4-1
 * (4 MiB minimum bank and dual-bank 64 MiB DIMMs). Register behavior observed in original ROMs
 * 6F5724C0 and 6E92FE08, $FFF030A0-$FFF03B88: $28 bank-enable bits
 * 16..20, $40/$48/$50/$58/$60 eight low-first four-bit selectors each.
 * A selector maps one 4 MiB segment to a physical bank, wrapping at its
 * populated size. Selector 8 is empty. The reset clock strap is at $10.
 * RAM population and clock straps are board inputs. Livadix's own code. */
#ifndef MC_PSX_H
#define MC_PSX_H
#include "hammerhead.h"
#define MC_PSX_SIZE 0x1000u
#define MC_PSX_BANKS 5u
#define MC_PSX_SEGMENT (4u << 20)
#define MC_PSX_SEGMENTS 40u
#define MC_PSX_SPACE (MC_PSX_SEGMENT * MC_PSX_SEGMENTS)
enum { MC_PSX_LINE_MAP };

typedef struct mc_psx {
    mc_dev dev;
    uint32_t reg[MC_PSX_SIZE / 4];
    uint32_t bank_size[MC_PSX_BANKS];
    uint32_t strap;
} mc_psx;

/* Two 64-bit DIMM slots, 0/4/8/16/32/64 MiB each. PSX has 8 MiB
 * soldered RAM in bank 0. PSX+ leaves bank 0 empty. A 64 MiB DIMM
 * populates both 32 MiB banks of its slot. */
int mc_psx_population(uint32_t bytes, int plus, uint32_t banks[MC_PSX_BANKS]);
void mc_psx_init(mc_psx *, const mc_hw_host *, const uint32_t *banks, uint32_t strap);
unsigned mc_psx_windows(const mc_psx *, mc_mem_window *, unsigned capacity);
extern const mc_dev_ops mc_psx_ops;
#endif
