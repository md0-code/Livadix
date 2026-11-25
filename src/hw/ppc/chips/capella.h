/* Livadix — Capella, the 603-to-68040 bus bridge (phase 6.7).
 *
 * Apple Power Macintosh 5200/6200 Developer Note (1995), chapter 2,
 * "Capella IC": bus translation, arbitration, L2 cache and ROM control.
 * ROM 63ABFD3F supplies the register evidence: $FFF036F4 (ROM/cache),
 * $FFF035F4 (cache invalidation), $FFF0735C and $FFF15860 (interrupts).
 * The IPL input and change-latched INT output are a ROM-derived model†;
 * electrical timing and cache-hit timing are not modelled. POST accesses
 * the data/tag test apertures under +$0C bits 1/2 ($FFF089CC–$FFF08ADC).
 */
#ifndef MC_CAPELLA_H
#define MC_CAPELLA_H

#include "../../hw.h"

#define MC_CAPELLA_SIZE 0x1000u
#define MC_CAPELLA_ACK 0x1Cu
#define MC_CAPELLA_IPL 0x24u

enum { MC_CAPELLA_LINE_CPU };

typedef struct mc_capella {
    mc_dev dev;
    uint32_t reg[10];
    uint8_t ipl, latch, cpu;
    void (*map_cache)(void *ctx, uint32_t control);
    void *map_ctx;
} mc_capella;

void mc_capella_init(mc_capella *c, const mc_hw_host *host, const char *name);
void mc_capella_set_ipl(mc_capella *c, unsigned level);
extern const mc_dev_ops mc_capella_ops;

#endif
