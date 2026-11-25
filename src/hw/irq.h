/* Livadix — the interrupt router: one level per source;
 * the CPU sees the highest level asserted. The machine description gives
 * each source its level (VIA1 1, VIA2 2, SCC 4, NMI 7 on the Quadra 700:
 * Macintosh Quadra 700 Developer Note, Table 2-7). */
#ifndef MC_IRQ_H
#define MC_IRQ_H

#include <stdint.h>

#define MC_IRQ_SOURCES 32

typedef struct mc_irq {
    uint8_t level[MC_IRQ_SOURCES];  /* 1-7 per source */
    uint32_t asserted;              /* bit per source */
    unsigned count;
    unsigned ipl;                   /* current output */
    void (*changed)(void *ctx, unsigned ipl);
    void *ctx;
} mc_irq;

void mc_irq_init(mc_irq *irq, void (*changed)(void *ctx, unsigned ipl), void *ctx);
/* A new source at `level`; returns its number (-1 when full). */
int mc_irq_add(mc_irq *irq, unsigned level);
void mc_irq_set(mc_irq *irq, int source, int asserted);
/* The highest level asserted (0 for none). */
static inline unsigned mc_irq_level(const mc_irq *irq) { return irq->ipl; }

#endif
