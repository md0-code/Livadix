/* Livadix — Grand Central (grand_central.h).
 *
 * Livadix's own code. */
#include "grand_central.h"

#include <string.h>

/* Sources latched on their rising edge: the DBDMA channels (acknowledged
 * through the clear register) and the NMI†. */
#define EDGE_SOURCES (0x7FFu | (1u << MC_GC_IRQ_NMI))
#define MASTER 0x80000000u
#define BUDGET 64u
#define CONTINUE 16u                /* C15M periods before a busy channel continues */

static uint32_t events(const mc_gc *g) { return g->latched | (g->level & ~EDGE_SOURCES); }

/* The processor's interrupt: latched whenever the enabled sources change
 * (events & mask, a source rising or falling), dropped by a write of bit
 * 31 to the clear register, which the nanokernel's interrupt entry makes
 * before it reads the mask and the levels and derives the 68k interrupt
 * level from them (ROM $FFF15748-$FFF157CC): the level it hands the 68k
 * emulator stands until the next interrupt, so a source that drops
 * interrupts too, and one the 68k code has not serviced yet does not
 * interrupt again until something changes†. */
static void update(mc_gc *g) {
    const uint32_t active = events(g) & g->mask & ~MASTER;
    if (active != g->active) g->pending_int = 1;
    g->active = active;
    const int irq = (g->mask & MASTER) && g->pending_int;
    if (irq != g->irq) {
        g->irq = irq;
        mc_dev_line(&g->dev, MC_GC_LINE_INT, irq);
    }
}

void mc_gc_set_source(mc_gc *g, unsigned source, int state) {
    if (source > 30) return;
    const uint32_t bit = 1u << source;
    if (state) {
        if ((EDGE_SOURCES & bit) && !(g->level & bit)) g->latched |= bit;
        g->level |= bit;
    } else {
        g->level &= ~bit;
    }
    update(g);
}

static void channel_interrupt(void *ctx, unsigned channel) {
    mc_gc *g = ctx;
    mc_gc_set_source(g, channel, 1);
    mc_gc_set_source(g, channel, 0);
}

static int channel_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    mc_gc *g = ctx;
    return mc_dev_dma(&g->dev, address, data, length, write);
}

/* A device's request may come from inside a channel's transfer (its port
 * raising DREQ as the channel takes a byte): the channels run once more
 * after the current pass instead of re-entering it. */
static void run_channels(mc_gc *g) {
    if (g->running) { g->rerun = 1; return; }
    g->running = 1;
    int more = 0;
    do {
        g->rerun = 0;
        for (unsigned i = 0; i < MC_GC_CHANNELS; i++)
            if (mc_dbdma_busy(&g->ch[i])) more |= mc_dbdma_run(&g->ch[i], BUDGET);
    } while (g->rerun && !more);
    g->running = 0;
    if (more && !g->pending) {
        g->pending = 1;
        mc_dev_schedule(&g->dev, mc_dev_now(&g->dev) + CONTINUE);
    }
}

static void gc_event(void *p, uint64_t now) {
    mc_gc *g = p;
    (void)now;
    g->pending = 0;
    run_channels(g);
}

void mc_gc_reset(mc_gc *g) {
    /* PCI 2.0 6.2; device $0002, revision and class not in the ROM†. */
    mc_pci_config_init(&g->cfg, 0x106Bu, 0x0002u, 0x01u, 0xFF0000u, 0x00u);
    mc_pci_config_bar(&g->cfg, 0, MC_GC_SIZE, 0);
    g->mask = 0;
    g->latched = 0;
    g->active = 0;
    g->pending_int = 0;
    for (unsigned i = 0; i < MC_GC_CHANNELS; i++) mc_dbdma_reset(&g->ch[i]);
    g->pending = 0;
    if (g->dev.host) mc_dev_schedule(&g->dev, MC_NEVER);
    update(g);
}

void mc_gc_init(mc_gc *g, const mc_hw_host *host, const char *name) {
    memset(g, 0, sizeof *g);
    g->dev.ops = &mc_gc_ops;
    g->dev.host = host;
    g->dev.name = name;
    g->dma_host.ctx = g;
    g->dma_host.dma = channel_dma;
    g->dma_host.interrupt = channel_interrupt;
    for (unsigned i = 0; i < MC_GC_CHANNELS; i++) mc_dbdma_init(&g->ch[i], &g->dma_host, i, NULL);
    mc_gc_reset(g);
}

int mc_gc_attach(mc_gc *g, uint32_t offset, uint32_t size, unsigned shift, void *dev, const mc_dev_ops *ops) {
    if (g->nwin >= MC_GC_WINDOWS || offset + size > MC_GC_SIZE) return 0;
    g->win[g->nwin++] = (mc_gc_window){offset, size, shift, dev, ops};
    return 1;
}

void mc_gc_dma_port(mc_gc *g, unsigned channel, const mc_dbdma_port *port) {
    if (channel < MC_GC_CHANNELS) mc_dbdma_set_port(&g->ch[channel], port);
}

void mc_gc_dma_bits(mc_gc *g, unsigned channel, uint8_t mask, uint8_t value) {
    if (channel < MC_GC_CHANNELS && mc_dbdma_set_bits(&g->ch[channel], mask, value)) run_channels(g);
}

void mc_gc_dma_kick(mc_gc *g, unsigned channel) {
    if (channel < MC_GC_CHANNELS && mc_dbdma_kick(&g->ch[channel])) run_channels(g);
}

static const mc_gc_window *window(const mc_gc *g, uint32_t offset) {
    for (unsigned i = 0; i < g->nwin; i++)
        if (offset - g->win[i].offset < g->win[i].size) return &g->win[i];
    return NULL;
}

static uint32_t gc_read(void *p, uint32_t offset, unsigned size) {
    mc_gc *g = p;
    offset &= MC_GC_SIZE - 1u;
    if (offset >= MC_GC_DBDMA && offset < MC_GC_DBDMA + MC_GC_CHANNELS * MC_DBDMA_SIZE) {
        const uint32_t o = offset - MC_GC_DBDMA;
        return mc_dbdma_read(&g->ch[o / MC_DBDMA_SIZE], o % MC_DBDMA_SIZE, size);
    }
    if (offset < 0x40) {
        uint32_t v;
        switch (offset & ~3u) {
        case MC_GC_EVENTS: v = events(g); break;
        case MC_GC_MASK: v = g->mask; break;
        case MC_GC_LEVELS: v = g->level | g->latched; break;     /* the nanokernel takes mask & levels */
        default: v = 0; break;
        }
        v >>= 8 * (offset & 3u);
        return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
    }
    const mc_gc_window *w = window(g, offset);
    return w ? w->ops->read(w->dev, (offset - w->offset) >> w->shift, size) : 0;
}

static void gc_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_gc *g = p;
    offset &= MC_GC_SIZE - 1u;
    if (offset >= MC_GC_DBDMA && offset < MC_GC_DBDMA + MC_GC_CHANNELS * MC_DBDMA_SIZE) {
        const uint32_t o = offset - MC_GC_DBDMA;
        if (mc_dbdma_write(&g->ch[o / MC_DBDMA_SIZE], o % MC_DBDMA_SIZE, size, value)) run_channels(g);
        return;
    }
    if (offset < 0x40) {
        const unsigned shift = 8 * (offset & 3u);
        const uint32_t m = (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u) << shift;
        const uint32_t v = (value << shift) & m;
        switch (offset & ~3u) {
        case MC_GC_MASK: g->mask = (g->mask & ~m) | v; break;
        case MC_GC_CLEAR:
            g->latched &= ~v;
            if (v & MASTER) g->pending_int = 0;
            break;
        default: break;
        }
        update(g);
        return;
    }
    const mc_gc_window *w = window(g, offset);
    if (w) w->ops->write(w->dev, (offset - w->offset) >> w->shift, size, value);
}

static void gc_reset_op(void *p) { mc_gc_reset(p); }

static int gc_stable(void *p, uint32_t offset, unsigned size) {
    mc_gc *g = p;
    offset &= MC_GC_SIZE - 1u;
    if (offset < MC_GC_DBDMA || offset >= MC_GC_DBDMA + MC_GC_CHANNELS * MC_DBDMA_SIZE) return 0;
    const uint32_t o = offset - MC_GC_DBDMA;
    return mc_dbdma_stable(&g->ch[o / MC_DBDMA_SIZE], o % MC_DBDMA_SIZE, size);
}

const mc_dev_ops mc_gc_ops = {gc_read, gc_write, gc_event, gc_reset_op, gc_stable};

/* --- the PCI function ------------------------------------------------------------- */

static uint32_t fn_config_read(void *p, unsigned reg, unsigned size) { return mc_pci_config_read(&((mc_gc *)p)->cfg, reg, size); }
static void fn_config_write(void *p, unsigned reg, unsigned size, uint32_t value) { mc_pci_config_write(&((mc_gc *)p)->cfg, reg, size, value); }
static int fn_mem_read(void *p, uint32_t address, unsigned size, uint32_t *value) {
    mc_gc *g = p;
    uint32_t offset;
    if (!mc_pci_config_hit(&g->cfg, 0, address, &offset)) return 0;
    *value = gc_read(g, offset, size);
    return 1;
}
static int fn_mem_write(void *p, uint32_t address, unsigned size, uint32_t value) {
    mc_gc *g = p;
    uint32_t offset;
    if (!mc_pci_config_hit(&g->cfg, 0, address, &offset)) return 0;
    gc_write(g, offset, size, value);
    return 1;
}

mc_pci_function mc_gc_function(mc_gc *g) {
    return (mc_pci_function){g, fn_config_read, fn_config_write, fn_mem_read, fn_mem_write};
}
