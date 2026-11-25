/* Livadix — the system ASIC of the PowerBook 520/540 and 190 boards (Pratt,
 * with Whitney's I/O functions†), decoder kinds 16 and 21.
 *
 * Sources: no Pratt or Whitney document was available; observed from ROMs
 * B6909089 and 4D27039C (powerbook500.c): the decoder
 * records' entries at $50F0 A000, $5014 0000 and $5008 0000, and the
 * register at $50F9 6000 the ROMs write from their ProductInfo flag:
 *   - $50F0 A000: bit 7 set on each pass of the ROMs' decoder check (ROM
 *     B6909089 offset $47B2);
 *   - $50F9 6000: written $12 once when the ProductInfo's feature bit 31
 *     is set ($4AA6), read later;
 *   - $5008 0003-$5008 0017: byte registers every 4 bytes, written $07,
 *     $3F, $02, $03, $01 and read by ROM B6909089's code at $D5948-$D5F40;
 *   - $5014 0000: never touched by the ROMs' start.
 * What they control is †: every register keeps what was written. Pratt's
 * tick to VIA1 CA1 drives the ROMs' 60.15 Hz interrupt.
 *
 * Livadix's own code. */
#ifndef MC_PRATT_H
#define MC_PRATT_H

#include "../hw.h"

#include <stdint.h>

#define MC_PRATT_WINDOW 0x2000u

/* The 60.15 Hz tick to VIA1 CA1 (as Jaws's, jaws.h; its source on the
 * board is †): line MC_PRATT_LINE_TICK. */
enum { MC_PRATT_LINE_TICK = 0 };
#define MC_PRATT_TICK_PERIOD (704u * 370u)

typedef struct mc_pratt {
    mc_dev dev;
    uint8_t a[MC_PRATT_WINDOW];     /* $50F0 A000 */
    uint8_t b[MC_PRATT_WINDOW];     /* $5014 0000 */
    uint8_t c[MC_PRATT_WINDOW];     /* $5008 0000 */
    uint8_t d[MC_PRATT_WINDOW];     /* $50F9 6000 */
    uint8_t is190;
    uint8_t tick;
    uint64_t tick_next;
} mc_pratt;

void mc_pratt_init(mc_pratt *p, const mc_hw_host *host, const char *name, int is190);
extern const mc_dev_ops mc_pratt_ops, mc_pratt_b_ops, mc_pratt_c_ops, mc_pratt_d_ops;

#endif
