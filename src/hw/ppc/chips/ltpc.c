/* Livadix - LocalTalk Packet Control assist (ltpc.h). */
#include "ltpc.h"
#include <string.h>

static uint8_t bit(unsigned channel) { return channel == MC_SCC_A ? 2u : 1u; }

static void schedule(mc_ltpc *p) {
    const uint64_t next = p->due[0] < p->due[1] ? p->due[0] : p->due[1];
    mc_dev_schedule(&p->dev, next);
}

/* With no packet bits left, SDLC mark idle emits the abort sequence.
 * A shift register or buffer still occupied delays that observation. */
static uint64_t abort_due(const mc_ltpc *p, unsigned channel, uint64_t now) {
    const mc_scc_chan *c = &p->scc->ch[channel];
    if ((c->wr[4] & 0x3Cu) != 0x20u || !(c->wr[10] & 8u) || (c->wr[5] & 0x0Au) != 0x0Au)
        return MC_NEVER;
    const uint64_t marks = mc_scc_tx_time(p->scc, (int)channel, 16);
    if (marks == MC_NEVER) return MC_NEVER;
    if (c->tx_busy && c->tx_done > now) now = c->tx_done;
    return now + marks;
}

void mc_ltpc_changed(mc_ltpc *p, unsigned channel) {
    if (channel >= 2) return;
    const mc_scc_chan *c = &p->scc->ch[channel];
    const uint64_t period = mc_scc_tx_time(p->scc, (int)channel, 16);
    uint64_t next = MC_NEVER;
    if ((p->start & bit(channel)) && !(p->detect & bit(channel)))
        next = abort_due(p, channel, mc_dev_now(&p->dev));
    /* Receive-register writes do not interrupt marks already on TxD. */
    if (next == MC_NEVER || p->due[channel] == MC_NEVER ||
        c->tx_busy || c->tx_full || period != p->period[channel]) p->due[channel] = next;
    p->period[channel] = period;
    schedule(p);
}

static void event(void *dev, uint64_t now) {
    mc_ltpc *p = dev;
    for (unsigned channel = 0; channel < 2; channel++) {
        if (p->due[channel] > now) continue;
        p->due[channel] = MC_NEVER;
        const mc_scc_chan *c = &p->scc->ch[channel];
        if (c->tx_busy || c->tx_full) {
            p->due[channel] = abort_due(p, channel, now);
        } else {
            p->detect |= bit(channel);
            mc_dev_line(&p->dev, channel == MC_SCC_A ? MC_LTPC_LINE_DETECT_A : MC_LTPC_LINE_DETECT_B, 1);
        }
    }
    schedule(p);
}

static uint32_t read(void *dev, uint32_t offset, unsigned size) {
    const mc_ltpc *p = dev;
    (void)size;
    if (offset == MC_LTPC_DETECT) return p->detect;
    if (offset == MC_LTPC_START_A) return (p->start >> 1) & 1u;
    if (offset == MC_LTPC_START_B) return p->start & 1u;
    return 0;
}

static void write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ltpc *p = dev;
    (void)size;
    if (offset != MC_LTPC_START_A && offset != MC_LTPC_START_B) return;
    const unsigned channel = offset == MC_LTPC_START_A ? MC_SCC_A : MC_SCC_B;
    const uint8_t mask = bit(channel);
    if (value & 1u) {
        if (p->start & mask) return;
        p->start |= mask;
    } else {
        p->start &= (uint8_t)~mask;
        if (p->detect & mask) {
            p->detect &= (uint8_t)~mask;
            mc_dev_line(&p->dev, channel == MC_SCC_A ? MC_LTPC_LINE_DETECT_A : MC_LTPC_LINE_DETECT_B, 0);
        }
    }
    mc_ltpc_changed(p, channel);
}

static void reset(void *dev) {
    mc_ltpc *p = dev;
    p->start = p->detect = 0;
    p->due[0] = p->due[1] = MC_NEVER;
    mc_dev_line(&p->dev, MC_LTPC_LINE_DETECT_A, 0);
    mc_dev_line(&p->dev, MC_LTPC_LINE_DETECT_B, 0);
    schedule(p);
}

void mc_ltpc_init(mc_ltpc *p, const mc_hw_host *host, mc_scc *scc) {
    memset(p, 0, sizeof *p);
    p->dev = (mc_dev){&mc_ltpc_ops, host, "LocalTalk packet control"};
    p->scc = scc;
    reset(p);
}

int mc_ltpc_tristated(const mc_ltpc *p, unsigned channel) {
    return channel < 2 && (p->detect & bit(channel)) != 0;
}

const mc_dev_ops mc_ltpc_ops = {read, write, event, reset, NULL};
