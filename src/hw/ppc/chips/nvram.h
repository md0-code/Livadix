/* Livadix — the NVRAM of the first PCI Power Macintosh generation, on Grand
 * Central's 16-bit bus: 8 KB holding Open Firmware's configuration and the
 * Mac OS parameter RAM.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * "Grand Central I/O Subsystem IC" (a 16-bit bus to the nonvolatile RAM);
 * Designing PCI Cards and Drivers for Power Macintosh Computers (Apple,
 * 1995), chapter 1 "Nonvolatile RAM" (at least 4 KB) and "Typical NVRAM
 * Structure".
 *
 * As ROM 9630C68B reaches it ($FFF035E0-$FFF03644):
 * a page register at Grand Central + $1D000 takes address bits 14-5
 * (written little-endian), then the page's 32 bytes are at + $1F000 + $10
 * n; the classic parameter RAM is at NVRAM $1300 (POST reads the sound
 * volume, parameter RAM byte 8, at $1308). Livadix keeps 8 KB†, repeating
 * through the page register's range.
 *
 * Livadix's own code. */
#ifndef MC_NVRAM_H
#define MC_NVRAM_H

#include "hw.h"

#include <stdint.h>

#define MC_NVRAM_SIZE 0x2000u
#define MC_NVRAM_PAGE_OFFSET 0x1D000u   /* Grand Central offsets */
#define MC_NVRAM_DATA_OFFSET 0x1F000u
#define MC_NVRAM_WINDOW 0x1000u
#define MC_NVRAM_PRAM 0x1300u

typedef struct mc_nvram {
    mc_dev dev;
    uint32_t page;
    uint8_t data[MC_NVRAM_SIZE];
    int dirty;                      /* written since the board last saved it */
} mc_nvram;

extern const mc_dev_ops mc_nvram_page_ops;     /* Grand Central + $1D000 */
extern const mc_dev_ops mc_nvram_data_ops;     /* Grand Central + $1F000 */

/* Contents from `image` (MC_NVRAM_SIZE bytes; NULL: all zero). */
void mc_nvram_init(mc_nvram *n, const mc_hw_host *host, const char *name, const uint8_t *image);

#endif
