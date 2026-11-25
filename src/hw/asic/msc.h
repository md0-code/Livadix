/* Livadix — the MSC, the PowerBook Duo's main system controller (Duo 210, 230,
 * 250; the 270c's and 280's are its successors).
 *
 * Sources: the PowerBook Duo 250 Developer Note supplement's summary (the
 * MSC "controls DRAM, ROM, built-in I/O, sound, and some power-saving
 * features"); Guide to the Macintosh Family Hardware, 2nd ed., chapter 3
 * and Table 4-27 for the RBV's VIA2 registers, which the MSC keeps (rbv.h).
 * Observed from ROM ECFA989B:
 *   - the decoder record of decoder kind 10 (ROM offset $D1B44): VIA1
 *     $50F0 0000, SCC $50F0 4000, SWIM $50F1 6000, SCSI $50F1 0000, its
 *     pseudo-DMA windows $50F1 2000 and $50F0 6000, sound $50F1 4000, the
 *     VIA2 functions $50F2 6000 (the RBV entry), the GSC $50F2 0000; no
 *     VIA2;
 *   - the ProductInfo records of box flags 23, 26 and 32 ($D1598, $D15D8,
 *     $D1618): no VIA1 identity bits (PA6/4/2/1 all 0), the CPU ID
 *     register's low word $1004, $1005, $1006 (the board's register at
 *     $5FFF FFFC, $A55A in the high word);
 *   - port B of the VIA2 functions: bit 2 /REQ to the PMU (an output),
 *     bit 1 /ACK from it (an input; the ROM's early PMU byte send at
 *     $476C6 drives bit 2 and waits on bit 1), the other bits as the
 *     RBV's†.
 *
 * Livadix's own code. */
#ifndef MC_MSC_H
#define MC_MSC_H

#include "../hw.h"
#include "rbv.h"

#include <stdint.h>

typedef struct mc_msc {
    mc_rbv rbv;                 /* first: its lines come from &msc->rbv */
} mc_msc;

void mc_msc_init(mc_msc *m, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_msc_ops;
/* The PMU's /ACK on port B bit 1 (1 high). */
void mc_msc_set_ack(mc_msc *m, int level);

#endif
