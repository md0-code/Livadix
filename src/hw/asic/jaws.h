/* Livadix — Jaws, the memory and I/O controller of the PowerBook 140, 145,
 * 145B and 170.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 3
 * (the PowerBook's architecture as the Portable's: RAM, ROM and the I/O
 * devices decoded by one controller). Everything else is observed from
 * ROM 420DBFF3:
 *   - the ProductInfo record of box flag 15 (ROM offset $394C, decoder
 *     kind 9) and its decoder record ($360C): ROM $4080 0000, VIA1
 *     $50F0 0000, SCC $50F0 4000, SWIM $50F1 6000, SCSI $50F1 0000, its
 *     pseudo-DMA window $50F0 6000, VIA2 $50F0 2000, sound $50F1 4000 and
 *     Jaws's own registers at $50F8 0000; the record's VIA1 identity:
 *     PA6, PA4, PA2, PA1 = 0, 1, 0, 1;
 *   - the RAM information ($3BD0): four banks of 2 MB from 0 (8 MB at
 *     most), contiguous;
 *   - the video information ($3C38): the screen at $FEE0 8000.
 *
 * Jaws's registers (jaws.c): a byte each, every $2000 bytes from
 * $50F8 0000, kept as the ROM writes them†; what each controls is listed
 * there as far as the ROM's use shows it.
 *
 * Livadix's own code. */
#ifndef MC_JAWS_H
#define MC_JAWS_H

#include "../hw.h"

#include <stdint.h>

#define MC_JAWS_REGS 0x00080000u     /* offset of the registers in the I/O block */
#define MC_JAWS_NREG 64u

/* The 60.15 Hz tick to VIA1 CA1 (a square wave, the classic Macintosh's
 * frame of 370 lines of 704 C15M periods; its source on
 * the board, the panel's frame or Jaws, is †): line MC_JAWS_LINE_TICK. */
enum { MC_JAWS_LINE_TICK = 0 };
#define MC_JAWS_TICK_PERIOD (704u * 370u)

typedef struct mc_jaws {
    mc_dev dev;
    uint8_t reg[MC_JAWS_NREG];
    uint8_t tick;
    uint64_t tick_next;
} mc_jaws;

void mc_jaws_init(mc_jaws *j, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_jaws_ops;

#endif
