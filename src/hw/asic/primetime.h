/* Livadix — PrimeTime, the I/O subsystem and buffer of the LC 475, LC 575,
 * Quadra 605 and Performa 475/476/575/577/578: its configuration
 * registers and the CPU ID register.
 *
 * Macintosh LC 575 Developer Note (Apple, 1994), chapter 2, "PrimeTime IC"
 * (I/O data bus buffers with byte steering and dynamic bus sizing, the
 * SWIM II, VIA1 and VIA2, I/O address decoding, the sound control logic
 * and its three buffers), Figure 2-1 (a 16-bit I/O data bus, IOD15-0, to
 * the I/O devices), "Address Maps" (the map of the Quadra 610 and 650).
 * The chips PrimeTime holds are Livadix's own models (via.c, swim.c and the
 * sound, asc.c); this file is its register block and the CPU ID register.
 *
 * Observed from ROM FF7439EE:
 *   - the CPU ID register at $5FFF FFFC reads $A55A in the high word and
 *     ignores writes (the probe at ROM offset $4BE4 writes 0 and
 *     $FFFFFFFF and reads $A55A back); the low word selects the ProductInfo
 *     record (word at +$44, list at ROM offset $A79C0): $2221 LC 475 (box
 *     83), $2225 Quadra 605 (88), $222E LC 575 (86); its low two bits are
 *     the CPU clock the ROM sets the MEMCjr and PrimeTime timings from (1
 *     for 25 MHz, 2 for 33 MHz; ROM offset $4958);
 *   - the registers are 16 bits wide at $5001 8000 + $100 n, as the IOSB's
 *     (iosb.h), a byte at an even address the high byte: +$000 a timing
 *     byte and +$600 a timing word by the CPU clock (ROM offset $4986),
 *     +$100 bit 3 set on some clocks (ROM offset $49B2). Each reads back
 *     what was written; their effect on the bus timing is not modelled. */
#ifndef MC_PRIMETIME_H
#define MC_PRIMETIME_H

#include "../hw.h"

#include <stdint.h>

#define MC_PRIMETIME_REGS 32
#define MC_PRIMETIME_SIZE 0x2000u       /* the window, $5001 8000-$5001 9FFF */
#define MC_PRIMETIME_CPU_ID_HIGH 0xA55Au

/* The CPU ID register's low word per model (the ProductInfo records). */
#define MC_PRIMETIME_ID_LC475 0x2221u
#define MC_PRIMETIME_ID_Q605 0x2225u
#define MC_PRIMETIME_ID_LC575 0x222Eu

typedef struct mc_primetime {
    mc_dev dev;
    uint16_t reg[MC_PRIMETIME_REGS];
    uint32_t cpu_id;
} mc_primetime;

void mc_primetime_init(mc_primetime *p, const mc_hw_host *host, const char *name, uint16_t cpu_id);
extern const mc_dev_ops mc_primetime_ops;          /* offsets from $5001 8000 */
/* The CPU ID register: a 4-byte window at $5FFF FFFC (offsets 0-3). */
extern const mc_dev_ops mc_primetime_cpu_id_ops;

#endif
