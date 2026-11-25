/* Livadix — Heathrow (heathrow.h).
 *
 * Livadix's own code. */
#include "heathrow.h"

#include <string.h>

/* The ROM's interrupt lists place Ethernet DMA at 32/33, ATA at 2/3;
 * classic peripheral inputs are level sources; DMA and NMI latch edges. */
#define MASTER 0x80000000u
#define BUDGET 64u
#define CONTINUE 16u

/* CHRP I/O Device Reference 9.8.5: the internal timer clock is
 * 921.6 kHz. Mac OS 8.0's LocalTalk HAL Wait100 loads 92 ticks at
 * the native O'Hare timer offset observed on the 6400. */
#define OHARE_SERIAL_TIMER 0x13100u
#define OHARE_SERIAL_TIMER_HZ 921600u
/* The classic nanokernel uses the bit-31 CPU latch protocol (G3 ROM
 * FFF168xx); Darwin's HeathrowInterruptController enables individual
 * sources without bit 31. Preserve the former while supporting the latter.
 * In native mode PCI inputs 20..28 are level-sensitive; other sources
 * and all bank 1 sources latch edges (AppleHeathrow kTypeLevelMask). */
static int legacy_interrupts(const mc_heathrow *g) {
    return g->ohare||(g->mask[0]&MASTER)!=0;
}
static uint32_t edge_sources(const mc_heathrow *g,unsigned bank) {
    if(legacy_interrupts(g))return bank?3u:0x001007FFu;
    return bank?0xFFFFFFFFu:~0x1FF00000u;
}
static uint32_t events(const mc_heathrow *g,unsigned bank) {
    return g->latched[bank] | (g->level[bank] & ~edge_sources(g,bank));
}
static void update(mc_heathrow *g) {
    for (unsigned bank=0;bank<2;++bank) {
        const uint32_t active=events(g,bank)&g->mask[bank]&(legacy_interrupts(g)&&!bank?~MASTER:0xFFFFFFFFu);
        if(active!=g->active[bank])g->pending_int=1;
        g->active[bank]=active;
    }
    const int irq=legacy_interrupts(g)?((g->mask[0]&MASTER)&&g->pending_int):
        (g->active[0]!=0||g->active[1]!=0);
    if(irq!=g->irq){g->irq=irq;mc_dev_line(&g->dev,MC_HEATHROW_LINE_INT,irq);}
}
void mc_heathrow_set_source(mc_heathrow *g,unsigned source,int state) {
    if(source>=64)return;
    const unsigned bank=source>>5; const uint32_t bit=1u<<(source&31u);
    if(state){if((edge_sources(g,bank)&bit)&&!(g->level[bank]&bit))g->latched[bank]|=bit;g->level[bank]|=bit;}
    else g->level[bank]&=~bit;
    update(g);
}
static void channel_interrupt(void *ctx,unsigned channel) {
    static const uint8_t sources[13]={0,1,32,33,4,5,6,7,8,9,10,2,3};
    mc_heathrow *g=ctx;mc_heathrow_set_source(g,sources[channel],1);mc_heathrow_set_source(g,sources[channel],0);
}

static int channel_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    mc_heathrow *g = ctx;
    return mc_dev_dma(&g->dev, address, data, length, write);
}

/* A device's request may come from inside a channel's transfer (its port
 * raising DREQ as the channel takes a byte): the channels run once more
 * after the current pass instead of re-entering it. */
static void run_channels(mc_heathrow *g) {
    if (g->running) { g->rerun = 1; return; }
    g->running = 1;
    int more = 0;
    do {
        g->rerun = 0;
        for (unsigned i = 0; i < MC_HEATHROW_CHANNELS; i++)
            if (mc_dbdma_busy(&g->ch[i])) more |= mc_dbdma_run(&g->ch[i], BUDGET);
    } while (g->rerun && !more);
    g->running = 0;
    if (more && !g->pending) {
        g->pending = 1;
        mc_dev_schedule(&g->dev, mc_dev_now(&g->dev) + CONTINUE);
    }
}

static void heathrow_event(void *p, uint64_t now) {
    mc_heathrow *g = p;
    (void)now;
    g->pending = 0;
    run_channels(g);
}

void mc_heathrow_reset(mc_heathrow *g) {
    /* PCI 2.0 6.2. The device IDs are in the G3 and 6400 Open Firmware. */
    mc_pci_config_init(&g->cfg, 0x106Bu, g->ohare ? 0x0007u : 0x0010u, 0x01u, 0xFF0000u, 0x00u);
    g->serial_timer_end = 0;
    g->gpio_drive = 0;
    mc_pci_config_bar(&g->cfg, 0, MC_HEATHROW_SIZE, 0);
    memset(g->mask,0,sizeof g->mask);
    memset(g->latched,0,sizeof g->latched);
    memset(g->level,0,sizeof g->level);
    memset(g->active,0,sizeof g->active);
    g->features=0;
    g->pending_int = 0;
    for (unsigned i = 0; i < MC_HEATHROW_CHANNELS; i++) mc_dbdma_reset(&g->ch[i]);
    g->pending = 0;
    if (g->dev.host) mc_dev_schedule(&g->dev, MC_NEVER);
    update(g);
}

static void init(mc_heathrow *g, const mc_hw_host *host, const char *name, int ohare) {
    memset(g, 0, sizeof *g);
    g->ohare = ohare;
    g->dev.ops = &mc_heathrow_ops;
    g->dev.host = host;
    g->dev.name = name;
    g->dma_host.ctx = g;
    g->dma_host.dma = channel_dma;
    g->dma_host.interrupt = channel_interrupt;
    for (unsigned i = 0; i < MC_HEATHROW_CHANNELS; i++) mc_dbdma_init(&g->ch[i], &g->dma_host, i, NULL);
    mc_heathrow_reset(g);
}

void mc_heathrow_init(mc_heathrow *g, const mc_hw_host *host, const char *name) {
    init(g, host, name, 0);
}

void mc_ohare_init(mc_heathrow *g, const mc_hw_host *host, const char *name) {
    init(g, host, name, 1);
}

int mc_heathrow_attach(mc_heathrow *g, uint32_t offset, uint32_t size, unsigned shift, void *dev, const mc_dev_ops *ops) {
    if (g->nwin >= MC_HEATHROW_WINDOWS || offset + size > MC_HEATHROW_SIZE) return 0;
    g->win[g->nwin++] = (mc_heathrow_window){offset, size, shift, dev, ops};
    return 1;
}

void mc_heathrow_dma_port(mc_heathrow *g, unsigned channel, const mc_dbdma_port *port) {
    if (channel < MC_HEATHROW_CHANNELS) mc_dbdma_set_port(&g->ch[channel], port);
}

void mc_heathrow_dma_bits(mc_heathrow *g, unsigned channel, uint8_t mask, uint8_t value) {
    if (channel < MC_HEATHROW_CHANNELS && mc_dbdma_set_bits(&g->ch[channel], mask, value)) run_channels(g);
}

void mc_heathrow_dma_kick(mc_heathrow *g, unsigned channel) {
    if (channel < MC_HEATHROW_CHANNELS && mc_dbdma_kick(&g->ch[channel])) run_channels(g);
}

static const mc_heathrow_window *window(const mc_heathrow *g, uint32_t offset) {
    for (unsigned i = 0; i < g->nwin; i++)
        if (offset - g->win[i].offset < g->win[i].size) return &g->win[i];
    return NULL;
}

static uint32_t serial_timer_read(const mc_heathrow *g) {
    const uint64_t now = mc_dev_now(&g->dev);
    if (now >= g->serial_timer_end) return 0;
    const uint64_t remaining = g->serial_timer_end - now;
    return (uint32_t)((remaining * OHARE_SERIAL_TIMER_HZ + MC_C15M_HZ - 1u) / MC_C15M_HZ);
}

static uint32_t heathrow_read(void *p, uint32_t offset, unsigned size) {
    mc_heathrow *g = p;
    offset &= MC_HEATHROW_SIZE - 1u;
    if (offset >= MC_HEATHROW_DBDMA && offset < MC_HEATHROW_DBDMA + MC_HEATHROW_CHANNELS * MC_DBDMA_SIZE) {
        const uint32_t o = offset - MC_HEATHROW_DBDMA;
        return mc_dbdma_read(&g->ch[o / MC_DBDMA_SIZE], o % MC_DBDMA_SIZE, size);
    }
    if (offset >= 0x10 && offset < 0x30) {
        const unsigned bank=offset<0x20?1:0; uint32_t v=0;
        switch(offset&0xCu){case 0:v=events(g,bank);break;case 4:v=g->mask[bank];break;case 12:v=g->level[bank]|g->latched[bank];break;default:break;}
        v>>=8u*(offset&3u);return size==4?v:v&((1u<<(8u*size))-1u);
    }
    if ((offset&~3u)==0x34 || (offset&~3u)==0x38) {
        const mc_heathrow_window *w=(offset&~3u)==0x34 && !g->ohare?window(g,offset):NULL;
        if (w) return w->ops->read(w->dev,(offset-w->offset)>>w->shift,size);
        uint32_t v=(offset&~3u)==0x34?g->board_id:g->features|g->feature_in;
        if (g->ohare && (offset & ~3u) == 0x34) v = (v & ~((uint32_t)g->gpio_drive << 4)) | g->gpio_drive;
        v>>=8u*(offset&3u);return size==4?v:v&((1u<<(8u*size))-1u);
    }
    if (g->ohare && (offset & ~3u) == OHARE_SERIAL_TIMER)
        return offset == OHARE_SERIAL_TIMER ? serial_timer_read(g) : 0;
    const mc_heathrow_window *w = window(g, offset);
    if(w)return w->ops->read(w->dev,(offset-w->offset)>>w->shift,size);
    if(offset>=0x100 && g->unmapped)g->unmapped(g->unmapped_ctx,offset,size,0,0);
    return 0;
}

static void heathrow_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_heathrow *g = p;
    offset &= MC_HEATHROW_SIZE - 1u;
    if (offset >= MC_HEATHROW_DBDMA && offset < MC_HEATHROW_DBDMA + MC_HEATHROW_CHANNELS * MC_DBDMA_SIZE) {
        const uint32_t o = offset - MC_HEATHROW_DBDMA;
        mc_dbdma *ch = &g->ch[o / MC_DBDMA_SIZE];
        /* The 6400's original LocalTalk RestartTxDMA polls ACTIVE, then
         * writes CommandPtr with RUN still set ($34D29C-$34D2E4 in
         * the OS 8 probe). O'Hare accepts that idle-channel update. */
        if (g->ohare && o % MC_DBDMA_SIZE == MC_DBDMA_COMMAND && size == 4 && !(ch->status & MC_DBDMA_ACTIVE))
            ch->command = value & ~15u;
        else if (mc_dbdma_write(ch, o % MC_DBDMA_SIZE, size, value)) run_channels(g);
        return;
    }
    if (g->ohare && (offset & ~3u) == 0x34) {
        if (offset == 0x34) g->gpio_drive = (uint8_t)(value & 7u);
        return;
    }
    if ((offset>=0x10 && offset<0x30) || (offset&~3u)==0x38) {
        const unsigned shift=8u*(offset&3u);
        const uint32_t mask=(size==4?0xFFFFFFFFu:(1u<<(8u*size))-1u)<<shift;
        const uint32_t v=(value<<shift)&mask;
        if((offset&~3u)==0x38)g->features=(g->features&~mask)|v;
        else {
            const unsigned bank=offset<0x20?1:0;
            switch(offset&0xCu){case 4:g->mask[bank]=(g->mask[bank]&~mask)|v;break;
            case 8:g->latched[bank]&=~v;break;default:break;}
            update(g);
            /* Clearing a source and acknowledging the CPU latch in one
             * write must not reassert that latch for the cleared edge. */
            if(legacy_interrupts(g)&&(offset&0xCu)==8&&!bank&&(v&MASTER)){g->pending_int=0;update(g);}
        }
        return;
    }
    if (g->ohare && (offset & ~3u) == OHARE_SERIAL_TIMER) {
        if (offset == OHARE_SERIAL_TIMER) {
            const uint64_t interval = (uint64_t)(value & 0xFFu) * MC_C15M_HZ / OHARE_SERIAL_TIMER_HZ;
            g->serial_timer_end = mc_dev_now(&g->dev) + interval;
        }
        return;
    }
    const mc_heathrow_window *w = window(g, offset);
    if (w) w->ops->write(w->dev, (offset - w->offset) >> w->shift, size, value);
    else if(offset>=0x100 && g->unmapped)g->unmapped(g->unmapped_ctx,offset,size,1,value);
}

static void heathrow_reset_op(void *p) { mc_heathrow_reset(p); }

static int heathrow_stable(void *p, uint32_t offset, unsigned size) {
    mc_heathrow *g = p;
    offset &= MC_HEATHROW_SIZE - 1u;
    if (offset < MC_HEATHROW_DBDMA || offset >= MC_HEATHROW_DBDMA + MC_HEATHROW_CHANNELS * MC_DBDMA_SIZE) return 0;
    const uint32_t o = offset - MC_HEATHROW_DBDMA;
    return mc_dbdma_stable(&g->ch[o / MC_DBDMA_SIZE], o % MC_DBDMA_SIZE, size);
}

const mc_dev_ops mc_heathrow_ops = {heathrow_read, heathrow_write, heathrow_event, heathrow_reset_op, heathrow_stable};

/* --- the PCI function ------------------------------------------------------------- */

static uint32_t fn_config_read(void *p, unsigned reg, unsigned size) { return mc_pci_config_read(&((mc_heathrow *)p)->cfg, reg, size); }
static void fn_config_write(void *p, unsigned reg, unsigned size, uint32_t value) { mc_pci_config_write(&((mc_heathrow *)p)->cfg, reg, size, value); }
static int fn_mem_read(void *p, uint32_t address, unsigned size, uint32_t *value) {
    mc_heathrow *g = p;
    uint32_t offset;
    if (!mc_pci_config_hit(&g->cfg, 0, address, &offset)) return 0;
    *value = heathrow_read(g, offset, size);
    return 1;
}
static int fn_mem_write(void *p, uint32_t address, unsigned size, uint32_t value) {
    mc_heathrow *g = p;
    uint32_t offset;
    if (!mc_pci_config_hit(&g->cfg, 0, address, &offset)) return 0;
    heathrow_write(g, offset, size, value);
    return 1;
}

mc_pci_function mc_heathrow_function(mc_heathrow *g) {
    return (mc_pci_function){g, fn_config_read, fn_config_write, fn_mem_read, fn_mem_write};
}
