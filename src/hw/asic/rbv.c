/* Livadix — the Macintosh IIci's RBV and its Bt478 CLUT DAC (rbv.h).
 *
 * Livadix's own code. */
#include "rbv.h"

#include <string.h>

/* The 60.15 Hz tick: the classic Macintosh frame of 370
 * lines of 704 C15M periods (Guide, chapter 12, the classic Macintosh's
 * video timing), the period v2VBL keeps on the Macintosh II (Guide, Table
 * 4-15: "60.15 Hz interrupt request to VIA1", once every 16.63 ms). */
#define TICK_PERIOD (704u * 370u)

/* Frame times and vertical blanking by monitor, in C15M periods (Guide,
 * Tables 12-5 and 12-6: 640 x 480 at 66.67 Hz, 864 x 525 dots of 30.24 MHz,
 * 45 lines of blanking; 640 x 870 at 75 Hz, 832 x 918 dots of 57.28 MHz, 48
 * lines). The 12-inch RGB monitor's 512 x 384 is not in the Guide: Livadix
 * gives it 640 x 407 dots of C15M (60.15 Hz) and 23 lines of blanking†. */
static void timing(uint8_t sense, uint64_t *frame, uint64_t *blank) {
    switch (sense) {
    case MC_RBV_SENSE_PORTRAIT:
        *frame = (uint64_t)MC_C15M_HZ * 832u * 918u / 57280000u;
        *blank = (uint64_t)MC_C15M_HZ * 832u * 48u / 57280000u;
        break;
    case MC_RBV_SENSE_RGB12:
        *frame = 640u * 407u;
        *blank = 640u * 23u;
        break;
    case MC_RBV_SENSE_HIRES:
        *frame = (uint64_t)MC_C15M_HZ * 864u * 525u / 30240000u;
        *blank = (uint64_t)MC_C15M_HZ * 864u * 45u / 30240000u;
        break;
    default:
        *frame = *blank = 0;
        break;
    }
}

uint8_t mc_rbv_sense_for(uint32_t width, uint32_t height) {
    if (width == 640 && height == 480) return MC_RBV_SENSE_HIRES;
    if (width == 640 && height == 870) return MC_RBV_SENSE_PORTRAIT;
    if (width == 512 && height == 384) return MC_RBV_SENSE_RGB12;
    return MC_RBV_SENSE_NONE;
}

uint32_t mc_rbv_max_depth(uint8_t sense) {
    switch (sense) {
    case MC_RBV_SENSE_PORTRAIT: return 4;      /* Guide, Table 12-3 */
    case MC_RBV_SENSE_RGB12:
    case MC_RBV_SENSE_HIRES: return 8;
    default: return 0;
    }
}

static void update_irq(mc_rbv *r) {
    /* Bit 1 follows the enabled slot lines (†: observed, rbv.h). */
    if (r->slots & r->sier & 0x7Fu) r->ifr |= MC_RBV_IRQ_SLOT;
    else r->ifr = (uint8_t)(r->ifr & ~MC_RBV_IRQ_SLOT);
    const uint8_t irq = (r->ifr & r->ier & 0x7Fu) != 0;
    if (irq != r->irq) {
        r->irq = irq;
        mc_dev_line(&r->dev, MC_RBV_LINE_IRQ, irq);
    }
}

void mc_rbv_set_irq(mc_rbv *r, uint8_t bit, int asserted) {
    if (asserted) r->ifr |= bit;
    else r->ifr &= (uint8_t)~bit;
    update_irq(r);
}

void mc_rbv_set_slots(mc_rbv *r, uint8_t asserted) {
    r->slots = asserted & 0x7Fu;
    update_irq(r);
}

void mc_rbv_set_port_b(mc_rbv *r, uint8_t levels, uint8_t mask) {
    r->datab_in = (uint8_t)((r->datab_in & ~mask) | (levels & mask));
}

/* --- time ------------------------------------------------------------------------- */

/* The video's interrupt line (port A bit 6, active low in RvSIFR) is
 * asserted through each vertical blanking interval: the ROM's video driver
 * waits for a frame by polling the bit high, then low, with interrupts
 * masked (ROM offset $7F8D4), and its interrupt handler, after calling the
 * slot's VBL tasks, masks the interrupt in RvSIER when the line is still
 * asserted and unmasks it later ($7F9B0, $7F9DE)†. */
static int vbl_at(const mc_rbv *r, uint64_t t) {
    return r->frame && (t - r->frame_start) % r->frame < r->blank;
}

static void schedule(mc_rbv *r) {
    uint64_t next = r->tick_start + TICK_PERIOD / 2u;
    if (r->frame) {
        /* The next edge of the blanking interval. */
        const uint64_t f = r->frame_start + (r->vbl ? r->blank : r->frame);
        if (f < next) next = f;
    }
    r->next = next;
    mc_dev_schedule(&r->dev, next);
}

static void advance(mc_rbv *r, uint64_t now) {
    if (now >= r->tick_start + TICK_PERIOD / 2u) {
        const uint64_t n = (now - r->tick_start) / (TICK_PERIOD / 2u);
        r->tick_start += n * (TICK_PERIOD / 2u);
        if (n & 1u) {
            r->tick ^= 1u;
            mc_dev_line(&r->dev, MC_RBV_LINE_TICK, r->tick);
        }
    }
    if (r->frame) {
        r->frame_start += (now - r->frame_start) / r->frame * r->frame;
        const uint8_t vbl = (uint8_t)vbl_at(r, now);
        if (vbl != r->vbl) {
            r->vbl = vbl;
            r->slots = (uint8_t)(vbl ? r->slots | 0x40u : r->slots & ~0x40u);
            update_irq(r);
        }
    }
}

static void rbv_event(void *dev, uint64_t now) {
    mc_rbv *r = dev;
    advance(r, now);
    schedule(r);
}

/* --- registers -------------------------------------------------------------------- */

static uint8_t read_reg(mc_rbv *r, uint32_t reg) {
    advance(r, mc_dev_now(&r->dev));
    switch (reg) {
    /* Port B: bits 7 and 3-0 are outputs, 6-4 inputs (Guide, Table 4-15). */
    case MC_RBV_DATAB: return (uint8_t)((r->datab & 0x8Fu) | (r->datab_in & 0x70u));
    case MC_RBV_EXP: return r->exp;
    /* The lines, active low as VIA2's port A (Guide, Table 4-9); bit 7
     * reserved, read high. */
    case MC_RBV_SIFR: return (uint8_t)(~r->slots | 0x80u);
    case MC_RBV_IFR: {
        const uint8_t f = r->ifr & 0x7Fu;
        return (uint8_t)(f | ((f & r->ier) ? 0x80u : 0));
    }
    case MC_RBV_MONP: return (uint8_t)((r->monp & ~0x38u) | (r->sense << 3));
    case MC_RBV_CHPT: return r->chpt;
    case MC_RBV_SIER: return (uint8_t)(r->sier | 0x80u);
    case MC_RBV_IER: return (uint8_t)(r->ier | 0x80u);
    default: return 0;
    }
}

static void write_reg(mc_rbv *r, uint32_t reg, uint8_t v) {
    advance(r, mc_dev_now(&r->dev));
    switch (reg) {
    case MC_RBV_DATAB:
        r->datab = v;
        mc_dev_line(&r->dev, MC_RBV_LINE_PB, v);
        break;
    case MC_RBV_EXP: r->exp = v; break;
    case MC_RBV_SIFR: break;
    case MC_RBV_IFR:
        /* Writing 1s clears flags, as the 6522's IFR. */
        r->ifr &= (uint8_t)~(v & 0x7Fu);
        break;
    case MC_RBV_MONP: r->monp = v; break;
    case MC_RBV_CHPT: r->chpt = v; break;
    case MC_RBV_SIER:
        if (v & 0x80u) r->sier |= v & 0x7Fu;
        else r->sier &= (uint8_t)~(v & 0x7Fu);
        break;
    /* Five enables, bits 4-0 (Table 4-27: no timers on the IIci; the ROM's
     * RBV register test expects $1F read back after $FF, ROM offset
     * $4084533A). */
    case MC_RBV_IER:
        if (v & 0x80u) r->ier |= v & 0x1Fu;
        else r->ier &= (uint8_t)~(v & 0x1Fu);
        break;
    default: break;
    }
    update_irq(r);
}

static uint32_t rbv_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | read_reg(dev, (offset + i) & 0x1Fu);
    return v;
}

static void rbv_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) write_reg(dev, (offset + i) & 0x1Fu, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void rbv_reset(void *dev) {
    mc_rbv *r = dev;
    r->ier = r->sier = 0;
    r->ifr &= MC_RBV_IRQ_SCSI_DRQ | MC_RBV_IRQ_SCSI | MC_RBV_IRQ_ASC | MC_RBV_IRQ_EXP;
    r->datab = 0xFF;
    r->monp = 0x40;
    update_irq(r);
}

const mc_dev_ops mc_rbv_ops = {rbv_read, rbv_write, rbv_event, rbv_reset, NULL};

/* --- the Bt478 ------------------------------------------------------------------ */

static void palette_entry(mc_rbv *r, unsigned i) {
    r->palette[i] = 0xFF000000u | (uint32_t)r->clut[i][0] << 16 | (uint32_t)r->clut[i][1] << 8 | r->clut[i][2];
}

static uint8_t dac_read(mc_rbv *r, uint32_t reg) {
    switch (reg) {
    case 0: return r->dac_addr;
    case 1: {
        const uint8_t v = r->clut[r->dac_rd_addr][r->dac_rd_rgb];
        if (++r->dac_rd_rgb == 3) {
            r->dac_rd_rgb = 0;
            r->dac_rd_addr++;
        }
        return v;
    }
    case 2: return r->dac_mask;
    case 3: return r->dac_rd_addr;
    default: return 0;
    }
}

static void dac_write(mc_rbv *r, uint32_t reg, uint8_t v) {
    switch (reg) {
    case 0: r->dac_addr = v; r->dac_rgb = 0; break;
    case 1:
        r->clut[r->dac_addr][r->dac_rgb] = v;
        if (++r->dac_rgb == 3) {
            palette_entry(r, r->dac_addr);
            r->dac_rgb = 0;
            r->dac_addr++;
        }
        break;
    case 2: r->dac_mask = v; break;
    case 3: r->dac_rd_addr = v; r->dac_rd_rgb = 0; break;
    default: break;
    }
}

static uint32_t vdac_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | dac_read(dev, ((offset + i) >> 2) & 7u);
    return v;
}

static void vdac_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) dac_write(dev, ((offset + i) >> 2) & 7u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_rbv_vdac_ops = {vdac_read, vdac_write, NULL, NULL, NULL};

/* --- setup and view ------------------------------------------------------------- */

void mc_rbv_init(mc_rbv *r, const mc_hw_host *host, const char *name, uint8_t sense) {
    memset(r, 0, sizeof *r);
    r->dev.ops = &mc_rbv_ops;
    r->dev.host = host;
    r->dev.name = name;
    r->sense = sense & 7u;
    timing(r->sense, &r->frame, &r->blank);
    r->datab_in = 0xFF;
    r->dac_mask = 0xFF;
    for (unsigned i = 0; i < 256; i++) palette_entry(r, i);
    rbv_reset(r);
    const uint64_t now = mc_dev_now(&r->dev);
    r->tick_start = r->frame_start = now;
    schedule(r);
}

int mc_rbv_view_get(mc_rbv *r, const uint8_t *bank_a, uint32_t bank_a_size, mc_rbv_view *view) {
    static const uint32_t widths[8] = {0, 640, 512, 0, 0, 0, 640, 0};
    static const uint32_t heights[8] = {0, 870, 384, 0, 0, 0, 480, 0};
    if (!r->frame || (r->monp & 0x40u)) return 0;
    const uint32_t depth = 1u << (r->monp & 3u);
    view->width = widths[r->sense];
    view->height = heights[r->sense];
    view->depth = depth;
    view->row_bytes = view->width * depth / 8u;
    if ((uint64_t)view->row_bytes * view->height > bank_a_size) return 0;
    /* A pixel of d bits reaches the Bt478 with the bits above it set: the
     * ROM's PrimaryInit sets 1-bit white and black in CLUT entries $FE and
     * $FF (ROM offset $7EE6C). */
    const unsigned high = (0xFFu << depth) & 0xFFu;
    for (unsigned i = 0; i < (1u << depth); i++) {
        uint32_t c = r->palette[high | i];
        /* The portrait display is monochrome: it shows the CLUT's blue
         * output (the ROM's PrimaryInit sets its white as $00, $00, $FF,
         * ROM offset $7EE82)†. */
        if (r->sense == MC_RBV_SENSE_PORTRAIT) c = 0xFF000000u | (c & 0xFFu) * 0x010101u;
        r->view_palette[i] = c;
    }
    view->pixels = bank_a;
    view->palette = r->view_palette;
    return 1;
}
