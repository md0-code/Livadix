/* Livadix — PBX memory decode and Whitney interrupt control.
 * Apple PowerBook Duo 2300c and PowerBook 1400 Developer Notes, ch. 2;
 * original ROMs 83C54F75/83A21950, reset and interrupt register cycles.
 * The PBX boards attach their Singer sound device separately. */
#ifndef MC_PBX_H
#define MC_PBX_H

#include "../../hw.h"
#include "../../bus.h"

enum { MC_PBX_BANKS = 8, MC_PBX_PAGES = 32, MC_PBX_PAGE = 0x200000 };
typedef struct mc_pbx {
    mc_dev dev;
    uint32_t reg[8], banks[MC_PBX_BANKS];
    mc_bus *bus;
    uint8_t *ram;
    int duo;
} mc_pbx;
extern const mc_dev_ops mc_pbx_ops;
int mc_pbx_init(mc_pbx *p, const mc_hw_host *host, mc_bus *bus, uint8_t *ram, unsigned ram_mb, int duo);

enum { MC_WHITNEY_LINE_CPU = 1 };
typedef struct mc_whitney {
    mc_dev dev;
    uint8_t level, mask, asserted;
} mc_whitney;
extern const mc_dev_ops mc_whitney_irq_ops;
void mc_whitney_init(mc_whitney *w, const mc_hw_host *host);
void mc_whitney_set_ipl(mc_whitney *w, unsigned level);

#endif
