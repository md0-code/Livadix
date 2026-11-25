/* Livadix — Plan B (343S1138), the video input DMA controller of the 7500,
 * 8500, 7600 and 8600, on Chaos's video bus: a stub that reports no video
 * signal.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * "Video Input" (the 8758 ADC and 7196 decoder; Plan B's DBDMA engine with
 * a write channel for the 7196's pixels and a read channel for the clip
 * mask); ROM 9630C68B's planb FCode (file $346D4C):
 * "planb", device type "video-in", "AAPL,343S1138", its registers at BAR
 * $10, $400 bytes; its IDs, vendor $106B device $0004 (the record before
 * the FCode, file $346D42). The ROM has no driver for it and no code that
 * touches its registers.
 *
 * Livadix's model: the function's configuration space with BAR $10; its
 * registers read 0 (no decoder, no signal, both channels idle) and ignore
 * writes. The class code is not in the ROM†.
 *
 * Livadix's own code. */
#ifndef MC_PLANB_H
#define MC_PLANB_H

#include "hw.h"
#include "pci.h"

#define MC_PLANB_DEVICE 13u             /* on Chaos's bus */
#define MC_PLANB_REGS 0x400u

typedef struct mc_planb {
    mc_dev dev;
    mc_pci_config cfg;
} mc_planb;

void mc_planb_init(mc_planb *p, const mc_hw_host *host, const char *name);
mc_pci_function mc_planb_function(mc_planb *p);

#endif
