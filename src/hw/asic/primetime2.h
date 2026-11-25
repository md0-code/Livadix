/* Livadix — PrimeTime II, the I/O subsystem and buffer of the Quadra 630 / LC
 * 630 and LC 580 families: its configuration registers. The chips it holds
 * are Livadix's own models (via.c for VIA1 and VIA2, swim.c for the SWIM II,
 * asc.c for the sound).
 *
 * Macintosh LC 630 and Quadra 630 Developer Note (1994), chapter 2,
 * "PrimeTime II IC" (data bus buffers for the I/O bus, address decoding for
 * the I/O devices, dynamic bus sizing and byte steering, VIA1 and VIA2,
 * interrupt controls, a SWIM II, the sound control logic and its input and
 * two output buffers; the I/O bus at 16 MHz) and "Block Diagram and Main
 * ICs".
 *
 * Observed from ROMs 06684214 and 064DC91D and Mac OS 8.0:
 *   - the sound answers as the Enhanced ASC at $5001 4000 (the decoder
 *     table's sound base): the ROM reads the version $B0 at +$800 and plays
 *     the chime through FIFO control mode 2, as on the Quadra 800 (asc.h);
 *   - 16-bit registers one every $100 bytes from $5001 8000 (address bits
 *     8-12): the ROM's 68040 start code writes +$000 (byte $00), +$600 (word
 *     $00D5, the timing value the Quadra 800's IOSB takes at 33 MHz) and
 *     +$500/+$501 (bytes $90, $B7) as on the IOSB (ROM offset $4986); the
 *     ROM sets and clears bit 1 of +$100 around its probe of the
 *     communication slot at $5000 9400 (ROM offset $A7472) and sets bit 0
 *     at ROM offset $A6870; the SCSI Manager of Mac OS 8.0 reads +$200 and
 *     +$600 and writes +$200, as on the IOSB. A byte at an even address is
 *     the high byte, a longword's second cycle (its low word) stays. Each
 *     register reads back what was written; their effect on the bus timing
 *     is not modelled†. */
#ifndef MC_PRIMETIME2_H
#define MC_PRIMETIME2_H

#include "../hw.h"

#include <stdint.h>

#define MC_PRIMETIME2_REGS 32
#define MC_PRIMETIME2_SIZE 0x2000u      /* the register window, $5001 8000-$5001 9FFF */

typedef struct mc_primetime2 {
    mc_dev dev;
    uint16_t reg[MC_PRIMETIME2_REGS];
} mc_primetime2;

void mc_primetime2_init(mc_primetime2 *p, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_primetime2_ops;     /* offsets from $5001 8000 */

#endif
