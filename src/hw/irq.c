/* Livadix — the interrupt router. Livadix's own code. */
#include "irq.h"

#include <string.h>

void mc_irq_init(mc_irq *irq, void (*changed)(void *ctx, unsigned ipl), void *ctx) {
    memset(irq, 0, sizeof *irq);
    irq->changed = changed;
    irq->ctx = ctx;
}

int mc_irq_add(mc_irq *irq, unsigned level) {
    if (irq->count >= MC_IRQ_SOURCES || level < 1 || level > 7) return -1;
    irq->level[irq->count] = (uint8_t)level;
    return (int)irq->count++;
}

void mc_irq_set(mc_irq *irq, int source, int asserted) {
    if (source < 0 || (unsigned)source >= irq->count) return;
    const uint32_t bit = 1u << source;
    const uint32_t was = irq->asserted;
    irq->asserted = asserted ? was | bit : was & ~bit;
    if (irq->asserted == was) return;
    unsigned ipl = 0;
    for (unsigned i = 0; i < irq->count; i++)
        if ((irq->asserted >> i & 1u) && irq->level[i] > ipl) ipl = irq->level[i];
    if (ipl != irq->ipl) {
        irq->ipl = ipl;
        if (irq->changed) irq->changed(irq->ctx, ipl);
    }
}
