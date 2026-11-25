/* Livadix — VASP (vasp.h).
 *
 * Livadix's own code. */
#include "vasp.h"

#include <string.h>

/* The 60.15 Hz tick to VIA1 CA1, as the RBV's (rbv.c). */
#define TICK_PERIOD (704u * 370u)

typedef struct monitor_info {
    uint32_t width, height;
    uint8_t code;                           /* standard sense code */
    uint32_t dots, lines, blank, clock;     /* dots per line, lines per frame, blanking lines, dot clock (Hz) */
} monitor_info;

/* IIvx Developer Note, Table 1-3 and Figures 1-2 and 1-3 (the 12-inch
 * RGB's 640 x 407 dots of 15.6672 MHz with 23 lines of blanking; the
 * 13-inch's 896 x 525 dots of 31.3344 MHz with 45). */
static const monitor_info monitors[MC_VASP_MONITORS] = {
    [MC_VASP_NONE] = {0, 0, 7, 0, 0, 0, 0},
    [MC_VASP_RGB12] = {512, 384, 2, 640, 407, 23, 15667200},
    [MC_VASP_HIRES13] = {640, 480, 6, 896, 525, 45, 31334400},
};

mc_vasp_monitor mc_vasp_monitor_for(uint32_t width, uint32_t height) {
    if (width == 512 && height == 384) return MC_VASP_RGB12;
    if (width == 640 && height == 480) return MC_VASP_HIRES13;
    return MC_VASP_NONE;
}

/* Table 1-3: the 12-inch RGB to 16 bits with 512 KB; 640 x 480 to 8 bits,
 * 16 with 1 MB (rows of 1024 or 2048 bytes, vasp.h). */
uint32_t mc_vasp_max_depth(mc_vasp_monitor monitor, uint32_t vram_size) {
    switch (monitor) {
    case MC_VASP_RGB12: return 16;
    case MC_VASP_HIRES13: return vram_size >= MC_VASP_VRAM_MAX ? 16 : 8;
    default: return 0;
    }
}

/* --- interrupts ------------------------------------------------------------------- */

static void update_irq(mc_vasp *v) {
    if (v->slots & v->sier & 0x7Fu) v->ifr |= MC_VASP_IRQ_SLOT;
    else v->ifr = (uint8_t)(v->ifr & ~MC_VASP_IRQ_SLOT);
    const uint8_t irq = (v->ifr & v->ier & 0x7Fu) != 0;
    if (irq != v->irq) {
        v->irq = irq;
        mc_dev_line(&v->dev, MC_VASP_LINE_IRQ, irq);
    }
}

void mc_vasp_set_irq(mc_vasp *v, uint8_t bit, int asserted) {
    if (asserted) v->ifr |= bit;
    else v->ifr &= (uint8_t)~bit;
    update_irq(v);
}

void mc_vasp_set_slots(mc_vasp *v, uint8_t asserted, uint8_t mask) {
    v->slots = (uint8_t)((v->slots & ~mask) | (asserted & mask & 0x3Fu));
    update_irq(v);
}

/* --- time ------------------------------------------------------------------------- */

/* The video's line (slot flag bit 6, active low) is asserted through each
 * vertical blanking interval while a monitor is connected. */
static int running(const mc_vasp *v) { return v->frame != 0; }

static void schedule(mc_vasp *v) {
    uint64_t next = v->tick_start + TICK_PERIOD / 2u;
    if (running(v)) {
        const uint64_t f = v->frame_start + (v->vbl ? v->blank : v->frame);
        if (f < next) next = f;
    }
    mc_dev_schedule(&v->dev, next);
}

static void advance(mc_vasp *v, uint64_t now) {
    if (now >= v->tick_start + TICK_PERIOD / 2u) {
        const uint64_t n = (now - v->tick_start) / (TICK_PERIOD / 2u);
        v->tick_start += n * (TICK_PERIOD / 2u);
        if (n & 1u) {
            v->tick ^= 1u;
            mc_dev_line(&v->dev, MC_VASP_LINE_TICK, v->tick);
        }
    }
    if (running(v)) {
        v->frame_start += (now - v->frame_start) / v->frame * v->frame;
        const uint8_t vbl = (now - v->frame_start) < v->blank;
        if (vbl != v->vbl) {
            v->vbl = vbl;
            v->slots = (uint8_t)(vbl ? v->slots | 0x40u : v->slots & ~0x40u);
            update_irq(v);
        }
    }
}

static void vasp_event(void *dev, uint64_t now) {
    mc_vasp *v = dev;
    advance(v, now);
    schedule(v);
}

/* --- the pseudo-VIA2 -------------------------------------------------------------- */

static uint8_t read_reg(mc_vasp *v, uint32_t reg) {
    advance(v, mc_dev_now(&v->dev));
    switch (reg) {
    case MC_VASP_DATAB: return v->datab;
    case MC_VASP_CONFIG: return v->config;
    /* Active low, as VIA2's port A (Guide, Table 4-9); bit 7 reads high. */
    case MC_VASP_SIFR: return (uint8_t)(~v->slots | 0x80u);
    case MC_VASP_IFR: {
        const uint8_t f = v->ifr & 0x7Fu;
        return (uint8_t)(f | ((f & v->ier) ? 0x80u : 0));
    }
    /* The sense code in bits 5-3 (the PrimaryInit, ROM offset $FE972). */
    case MC_VASP_MONP: return (uint8_t)((v->monp & ~0x38u) | (monitors[v->monitor].code << 3));
    case MC_VASP_TEST: return v->test;
    case MC_VASP_SIER: return (uint8_t)(v->sier | 0x80u);
    case MC_VASP_IER: return (uint8_t)(v->ier | 0x80u);
    default: return 0;
    }
}

static void write_reg(mc_vasp *v, uint32_t reg, uint8_t val) {
    advance(v, mc_dev_now(&v->dev));
    switch (reg) {
    case MC_VASP_DATAB: v->datab = val; break;
    case MC_VASP_CONFIG: v->config = val; break;
    case MC_VASP_SIFR: break;
    case MC_VASP_IFR: v->ifr &= (uint8_t)~(val & 0x7Fu); break;
    case MC_VASP_MONP: v->monp = val; break;
    case MC_VASP_TEST: v->test = val; break;
    /* The POST's register test (ROM 4957EB49, as the LC III ROM's at
     * ECBBC41C offset $48856): the slot enables are bits 6-3, the enables
     * bits 6-0. */
    case MC_VASP_SIER:
        if (val & 0x80u) v->sier |= val & 0x78u;
        else v->sier &= (uint8_t)~(val & 0x78u);
        break;
    case MC_VASP_IER:
        if (val & 0x80u) v->ier |= val & 0x7Fu;
        else v->ier &= (uint8_t)~(val & 0x7Fu);
        break;
    default: break;
    }
    update_irq(v);
}

static uint32_t vasp_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | read_reg(dev, (offset + i) & 0x1Fu);
    return r;
}

static void vasp_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) write_reg(dev, (offset + i) & 0x1Fu, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void vasp_reset(void *dev) {
    mc_vasp *v = dev;
    v->ier = v->sier = 0;
    v->ifr &= MC_VASP_IRQ_SCSI_DRQ | MC_VASP_IRQ_SCSI | MC_VASP_IRQ_SOUND;
    v->datab = 0xFF;
    v->monp = 0x40;
    v->config = 0x40;
    update_irq(v);
}

const mc_dev_ops mc_vasp_ops = {vasp_read, vasp_write, vasp_event, vasp_reset, NULL};

/* --- the CLUT DAC ------------------------------------------------------------------ */

static void palette_entry(mc_vasp *v, unsigned i) {
    v->palette[i] = 0xFF000000u | (uint32_t)v->clut[i][0] << 16 | (uint32_t)v->clut[i][1] << 8 | v->clut[i][2];
}

/* The address at $00, red, green and blue through $01, the control at $02
 * (the ROM's start writes 768 bytes after address 0, ROM offset $3D0A; the
 * PrimaryInit $0B to $02 and the gray ramp, $FEAB6). */
static uint8_t dac_reg_read(mc_vasp *v, uint32_t reg) {
    switch (reg) {
    case 0: return v->dac_addr;
    case 1: {
        const uint8_t r = v->clut[v->dac_addr][v->dac_rgb];
        if (++v->dac_rgb == 3) {
            v->dac_rgb = 0;
            v->dac_addr++;
        }
        return r;
    }
    case 2: return v->dac_ctl;
    default: return 0;
    }
}

static void dac_reg_write(mc_vasp *v, uint32_t reg, uint8_t val) {
    switch (reg) {
    case 0: v->dac_addr = val; v->dac_rgb = 0; break;
    case 1:
        v->clut[v->dac_addr][v->dac_rgb] = val;
        if (++v->dac_rgb == 3) {
            palette_entry(v, v->dac_addr);
            v->dac_rgb = 0;
            v->dac_addr++;
        }
        break;
    case 2: v->dac_ctl = val; break;
    default: break;
    }
}

static uint32_t dac_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | dac_reg_read(dev, (offset + i) & 3u);
    return r;
}

static void dac_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) dac_reg_write(dev, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_vasp_dac_ops = {dac_read, dac_write, NULL, NULL, NULL};

/* --- setup and view ------------------------------------------------------------- */

void mc_vasp_init(mc_vasp *v, const mc_hw_host *host, const char *name, mc_vasp_monitor monitor, uint8_t *vram,
                  uint32_t vram_size) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_vasp_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->monitor = monitor;
    v->vram = vram;
    v->vram_size = vram_size;
    const monitor_info *m = &monitors[monitor];
    if (m->clock) {
        v->frame = (uint64_t)MC_C15M_HZ * m->dots * m->lines / m->clock;
        v->blank = (uint64_t)MC_C15M_HZ * m->dots * m->blank / m->clock;
    }
    for (unsigned i = 0; i < 256; i++) palette_entry(v, i);
    vasp_reset(v);
    const uint64_t now = mc_dev_now(&v->dev);
    v->tick_start = v->frame_start = now;
    schedule(v);
}

int mc_vasp_view_get(mc_vasp *v, mc_vasp_view *view) {
    const monitor_info *m = &monitors[v->monitor];
    const unsigned code = v->monp & 7u;
    if (!running(v) || (v->monp & 0x40u) || code > 4) return 0;
    const uint32_t depth = 1u << code;
    view->width = m->width;
    view->height = m->height;
    view->depth = depth;
    view->row_bytes = v->vram_size >= MC_VASP_VRAM_MAX ? 2048u : 1024u;
    if ((uint64_t)view->row_bytes * view->height > v->vram_size || view->width * depth / 8u > view->row_bytes) return 0;
    /* A pixel of d bits reaches the CLUT in its top bits, the rest set, as
     * on Sonora (sonora.c)†. */
    if (depth <= 8) {
        const unsigned shift = 8u - depth, low = (1u << shift) - 1u;
        for (unsigned i = 0; i < (1u << depth); i++) v->view_palette[i] = v->palette[(i << shift | low) & 0xFFu];
    }
    view->pixels = v->vram;
    view->palette = v->view_palette;
    return 1;
}
