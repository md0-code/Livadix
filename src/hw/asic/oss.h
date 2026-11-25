/* Livadix — the OSS (Operating System Support) of the Macintosh IIfx: the
 * interrupt controller with a programmable level per source, the 60.15 Hz
 * tick and the power-off control.
 *
 * Guide to the Macintosh Family Hardware, 2nd ed., chapter 3, "Operating
 * System Support (OSS)": software-programmable interrupt priority levels
 * (the Start Manager writes the levels into the OSS's control registers),
 * address decoding and bus time-out for the I/O devices, the Shutdown
 * Manager's control register that turns the machine off; chapter 2, "In
 * the Macintosh IIfx, individual interrupt lines go to the OSS custom IC",
 * which ORs the slot interrupts.
 *
 * The register layout is as the IIfx ROM 4147DD77 uses it, at $50F1 A000:
 *   +$000-$00F  one byte per source: its interrupt level (0 off, 1-7);
 *   +$202       (word) the sources asserted, bit n source n (read only);
 *   +$204       control: bit 7 written 1 switches the power off (the
 *               ROM's ShutDown, ROM offset $9E3C); the ROM writes $0D at
 *               start; bit 3 switches the FMC's cache on (1) and off
 *               (0), reported on MC_OSS_LINE_CACHE (the ROM's cache test,
 *               ROM offset $42F04: iifx.c, "the cache"); the other bits
 *               are kept†;
 *   +$205       two bits, kept† (the ROM's register test, ROM offset
 *               $42FC4, writes and reads back 0-3 there, 0-$1F at +$204
 *               and 0-6 at +$000-$00D);
 *   +$207       a write acknowledges the 60.15 Hz tick (source 10; the
 *               ROM's level 1 handler, ROM offset $41F10).
 * Sources (MC_OSS_*): NuBus slots $9-$E 0-5, the SWIM IOP 6, the SCC IOP 7,
 * the sound chip 8, the SCSI controller 9, the tick 10, VIA1 11; 12-15
 * unused†. Other offsets read 0 and ignore writes†. */
#ifndef MC_OSS_H
#define MC_OSS_H

#include "../hw.h"

#include <stdint.h>

#define MC_OSS_SIZE 0x2000u

enum {
    MC_OSS_SLOT9 = 0,               /* slots $9-$E: MC_OSS_SLOT9 + slot - 9 */
    MC_OSS_SWIM_IOP = 6, MC_OSS_SCC_IOP = 7, MC_OSS_SOUND = 8, MC_OSS_SCSI = 9, MC_OSS_TICK = 10, MC_OSS_VIA1 = 11,
    MC_OSS_SOURCES = 16
};

/* Output lines: MC_OSS_LINE_IPL (state = the level asserted, 0-7),
 * MC_OSS_LINE_POWER_OFF (1 when the power-off bit is written) and
 * MC_OSS_LINE_CACHE (control bit 3, at reset and at each write of +$204). */
enum { MC_OSS_LINE_IPL = 0, MC_OSS_LINE_POWER_OFF = 1, MC_OSS_LINE_CACHE = 2 };

typedef struct mc_oss {
    mc_dev dev;
    uint8_t level[MC_OSS_SOURCES];
    uint16_t lines;                 /* the sources asserted */
    uint8_t control, control2;      /* +$204, +$205 */
    uint8_t ipl;                    /* the level last reported */
    uint64_t tick_start;            /* the tick's period start */
} mc_oss;

void mc_oss_init(mc_oss *o, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_oss_ops;
/* Source `n`'s line (1 = asserted). */
void mc_oss_set(mc_oss *o, unsigned n, int asserted);

#endif
