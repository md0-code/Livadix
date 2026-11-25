/* Livadix — IOSB, the I/O subsystem and buffer of the Centris/Quadra 610, 650
 * and Quadra 800: its configuration registers, and the CPU ID register.
 *
 * Macintosh Developer Note Number 3 (Centris 610, Centris 650 and Quadra
 * 800), chapter 2, "IOSB IC" (I/O data bus buffers with byte steering and
 * dynamic bus sizing, the SWIM2, VIA1 and VIA2, I/O address decoding, the
 * sound circuits; a 16-bit I/O data bus, "Block Diagram"), Figure 2-4 (VIA1
 * $5000 0000, VIA2 $5000 2000, sound $5001 4000, ADB $5001 6000, IOSB
 * $5001 8000, SWIM II $5001 E000, all "(IOSB)"; CPU ID (IOCB) at the top of
 * $5FFF 0000-$5FFF FFFF). The chips the IOSB holds are Livadix's own models
 * (via.c, asc.c, swim.c); this file is its register block and the CPU ID
 * register.
 *
 * Observed from ROMs F1A6F343 and F1ACAD13 and Mac OS 8.0:
 *   - the registers are 16 bits wide, one every $200 bytes (address bits
 *     9-12, as the VIAs'): a longword write leaves its low word (the ROM
 *     writes $0000 00D5 at $5001 8600 and Mac OS 8.0's SCSI code reads the
 *     word $00D5 back there), a byte at an even address is the high byte;
 *   - +$000: a configuration register, bit 3 set from the ROM's table by
 *     the CPU clock (ROM offset $49A4); +$200: the SCSI timing, bits 0-5 by
 *     the CPU clock ($1B at 20 MHz, $3F at 25, $12 at 33; ROM offset
 *     $D1E10), bits 6-7 kept; +$600: a timing value by the CPU clock ($D5 at
 *     33 MHz; Mac OS 8.0 matches it against $280, $1E0, $D5). Each reads
 *     back what was written; their effect on the bus timing is not
 *     modelled;
 *   - the CPU ID register at $5FFF FFFC reads $A55A 2BAD and ignores
 *     writes: the ROM writes 0 and $FFFFFFFF there and takes the register
 *     only if both read back $A55A in the high word (ROM offset $4B80); the
 *     low word $2BAD selects the DJMEMC ProductInfo records (their word at
 *     +$3C), and its bit 11 tells the ROM to match VIA1 PA6/PA4/PA2/PA1
 *     against each record's ID too (ROM offset $4B3C). */
#ifndef MC_IOSB_H
#define MC_IOSB_H

#include "../hw.h"

#include <stdint.h>

#define MC_IOSB_REGS 16
#define MC_IOSB_SIZE 0x2000u        /* the IOSB window, $5001 8000-$5001 9FFF */
#define MC_IOSB_CPU_ID 0xA55A2BADu  /* the CPU ID register's value */

typedef struct mc_iosb {
    mc_dev dev;
    uint16_t reg[MC_IOSB_REGS];
} mc_iosb;

void mc_iosb_init(mc_iosb *s, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_iosb_ops;        /* offsets from $5001 8000 */
/* The CPU ID register: a 4-byte window at $5FFF FFFC (offsets 0-3). */
extern const mc_dev_ops mc_iosb_cpu_id_ops;

#endif
