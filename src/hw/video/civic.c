/* Livadix — Civic and Sebastian (see civic.h for sources). Livadix's own code. */
#include "civic.h"

#include <stdlib.h>
#include <string.h>

/* Field offsets (civic.h). */
#define F_VBL 0x000u
#define F_ENABLE 0x004u
#define F_DEPTH 0x020u
#define F_WIDE 0x040u
#define F_SENSE_DRIVE0 0x05Cu
#define F_SENSE_DRIVE1 0x060u
#define F_SENSE_DRIVE2 0x064u
#define F_SENSE 0x080u
#define F_VBL_ENABLE 0x110u
#define F_VBL_CLEAR 0x120u
#define F_LINE 0x140u
#define F_HTOTAL 0x300u
#define F_HACTIVE_START 0x380u
#define F_HACTIVE_END 0x3C0u
#define F_ROW 0x08Cu
#define F_BASE 0x0C0u
#define F_VTOTAL 0x480u
#define F_VACTIVE_START 0x580u
#define F_VACTIVE_END 0x640u

typedef struct monitor_info {
    uint8_t code;           /* sense lines 2-0 (HW 30) */
    int8_t ext;             /* type-7 code: 2 low (1, 0), 1 low (2, 0), 0 low (2, 1); -1 none */
    uint16_t width, height;
    uint32_t hz100;         /* frame rate in 1/100 Hz (Table 2-14; HW 30) */
} monitor_info;

static const monitor_info monitors[MC_CIVIC_MONITORS] = {
    [MC_CIVIC_NONE] = {7, 0x3F, 640, 480, 6000},
    [MC_CIVIC_RGB12] = {2, -1, 512, 384, 6015},
    [MC_CIVIC_RGB13] = {6, -1, 640, 480, 6667},
    [MC_CIVIC_PORTRAIT] = {5, -1, 640, 870, 7499},
    [MC_CIVIC_RGB16] = {7, 0x2D, 832, 624, 7455},
    [MC_CIVIC_RGB19] = {7, 0x3A, 1024, 768, 7493},
    [MC_CIVIC_RGB21] = {0, -1, 1152, 870, 7503},
    [MC_CIVIC_VGA] = {7, 0x17, 640, 480, 5994},
    [MC_CIVIC_SVGA] = {7, 0x17, 800, 600, 6032},
};

mc_civic_monitor mc_civic_monitor_for(uint32_t width, uint32_t height) {
    for (int m = MC_CIVIC_RGB12; m <= MC_CIVIC_RGB21; m++)
        if (monitors[m].width == width && monitors[m].height == height) return (mc_civic_monitor)m;
    if (width == 800 && height == 600) return MC_CIVIC_SVGA;
    return MC_CIVIC_NONE;
}

uint32_t mc_civic_max_depth(mc_civic_monitor monitor, uint32_t vram) {
    const int big = vram > (1u << 20);
    switch (monitor) {
    case MC_CIVIC_RGB12: case MC_CIVIC_RGB13: case MC_CIVIC_PORTRAIT: case MC_CIVIC_RGB16: return 32;
    case MC_CIVIC_RGB19: return big ? 32 : 16;
    case MC_CIVIC_RGB21: return 8;
    case MC_CIVIC_VGA: return 32;
    case MC_CIVIC_SVGA: return big ? 32 : 16;
    default: return 0;
    }
}

uint32_t mc_civic_field(const mc_civic *c, uint32_t offset, unsigned bits) {
    uint32_t v = 0;
    for (unsigned i = 0; i < bits; i++)
        if (c->bit[(offset / 4u + i) % (MC_CIVIC_SIZE / 4u)] & 1u) v |= 1u << i;
    return v;
}

/* The sense lines' levels (bit n line n), HW 30. */
static uint8_t sense(const mc_civic *c) {
    const monitor_info *m = &monitors[c->monitor];
    /* ROM 5BF10FD1 $1FF3BE releases all lines with 001 (registers
     * $064/$060/$05C). $1FEEFC drives 2, 1, 0 with 101, 011, 000.
     * These are three pin controls; line 0 has the opposite polarity. */
    const unsigned low = (!mc_civic_field(c, F_SENSE_DRIVE0, 1) ? 1u : 0u)
        | (mc_civic_field(c, F_SENSE_DRIVE1, 1) ? 2u : 0u)
        | (mc_civic_field(c, F_SENSE_DRIVE2, 1) ? 4u : 0u);
    uint8_t out = m->code & (uint8_t)~low;
    if (m->ext >= 0) {
        for (unsigned driven = 0; driven < 3; driven++) {
            if (!(low & (1u << driven))) continue;
            const unsigned pair = (unsigned)m->ext >> (2u * driven) & 3u;
            const unsigned other_hi = driven == 2 ? 1u : 2u;
            const unsigned other_lo = driven == 0 ? 1u : 0u;
            out &= (uint8_t)((1u << driven) | ((pair >> 1) << other_hi)
                             | ((pair & 1u) << other_lo));
        }
    }
    return out;
}

/* --- timing ------------------------------------------------------------------- */

static uint32_t vtotal(const mc_civic *c) {
    const uint32_t v = mc_civic_field(c, F_VTOTAL, 12);
    return v >= 8 ? v : 1050u;
}

static void update_irq(mc_civic *c) {
    const uint8_t irq = c->vbl && mc_civic_field(c, F_VBL_ENABLE, 1);
    if (irq != c->irq) {
        c->irq = irq;
        mc_dev_line(&c->dev, MC_CIVIC_LINE_VBL, irq);
    }
}

/* The half-line the beam is on. */
static uint32_t current_halfline(const mc_civic *c) {
    const uint64_t now = mc_dev_now(&c->dev);
    const uint64_t in = now >= c->frame_start ? (now - c->frame_start) % c->frame_length : 0;
    return (uint32_t)(in * vtotal(c) / c->frame_length);
}

/* Each frame the VBL status is set at the start of vertical blanking (the
 * frame's start here). */
static void civic_event(void *dev, uint64_t now) {
    mc_civic *c = dev;
    while (c->frame_start + c->frame_length <= now) c->frame_start += c->frame_length;
    c->vbl = 1;
    update_irq(c);
    mc_dev_schedule(&c->dev, c->frame_start + c->frame_length);
}

static void civic_reset(void *dev) {
    mc_civic *c = dev;
    memset(c->bit, 0, sizeof c->bit);
    c->bit[F_SENSE_DRIVE0 / 4u] = 1; /* sense pins released (ROM idle state) */
    c->vbl = 0;
    update_irq(c);
    c->frame_start = mc_dev_now(&c->dev);
    mc_dev_schedule(&c->dev, c->frame_start + c->frame_length);
}

static uint32_t civic_read(void *dev, uint32_t offset, unsigned size) {
    mc_civic *c = dev;
    const uint32_t r = (offset % MC_CIVIC_SIZE) & ~3u;
    uint32_t v;
    if (r == F_VBL) v = c->vbl;
    else if (r >= F_SENSE && r < F_SENSE + 12u) v = (sense(c) >> ((r - F_SENSE) / 4u)) & 1u;
    else if (r >= F_LINE && r < F_LINE + 48u) v = (current_halfline(c) >> ((r - F_LINE) / 4u)) & 1u;
    else v = c->bit[r / 4u];
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v : 0;
    return (offset & 3u) == 3u ? v : 0;
}

static void civic_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_civic *c = dev;
    const uint32_t r = (offset % MC_CIVIC_SIZE) & ~3u;
    /* The register's bit 0 is the data line D0 (the low byte of a longword). */
    if ((size == 1 && (offset & 3u) != 3u) || (size == 2 && !(offset & 2u))) return;
    c->bit[r / 4u] = (uint8_t)(value & 1u);
    if (r == F_VBL_CLEAR && !(value & 1u)) c->vbl = 0;
    if (r == F_VBL_CLEAR || r == F_VBL_ENABLE) update_irq(c);
}

const mc_dev_ops mc_civic_ops = {civic_read, civic_write, civic_event, civic_reset, NULL};

/* --- Sebastian ------------------------------------------------------------------ */

static uint32_t sebastian_read(void *dev, uint32_t offset, unsigned size) {
    mc_civic *c = dev;
    (void)size;
    switch ((offset >> 4) & 3u) {
    case 0: return c->clut_address;
    case 1: {
        const uint32_t e = c->clut[c->clut_address];
        const unsigned b = c->clut_byte;
        if (++c->clut_byte == 4) {
            c->clut_byte = 0;
            c->clut_address++;
        }
        return b < 3 ? (e >> (16u - 8u * b)) & 0xFFu : 0;
    }
    case 2: return c->clut_format;
    default: return 0;
    }
}

static void sebastian_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_civic *c = dev;
    (void)size;
    const uint8_t b = (uint8_t)value;
    switch ((offset >> 4) & 3u) {
    case 0:
        c->clut_address = b;
        c->clut_byte = 0;
        break;
    case 1:
        c->clut_rgb[c->clut_byte] = b;
        if (++c->clut_byte == 4) {
            c->clut_byte = 0;
            c->clut[c->clut_address++] = (uint32_t)c->clut_rgb[0] << 16 | (uint32_t)c->clut_rgb[1] << 8 | c->clut_rgb[2];
        }
        break;
    case 2: c->clut_format = b; break;
    default: break;
    }
}

const mc_dev_ops mc_civic_sebastian_ops = {sebastian_read, sebastian_write, NULL, NULL, NULL};

static uint32_t clock_read(void *dev, uint32_t offset, unsigned size) {
    const mc_civic *c = dev;
    (void)size;
    const unsigned r = (offset >> 4) & 0xFFu;
    return r < 3 ? c->clock[r] : 0;
}

static void clock_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_civic *c = dev;
    (void)size;
    const unsigned r = (offset >> 4) & 0xFFu;
    if (r < 3) c->clock[r] = (uint8_t)value;
}

const mc_dev_ops mc_civic_clock_ops = {clock_read, clock_write, NULL, NULL, NULL};

/* --- display -------------------------------------------------------------------- */

int mc_civic_view_get(mc_civic *c, mc_civic_view *view) {
    if (c->monitor == MC_CIVIC_NONE || !mc_civic_field(c, F_ENABLE, 1)) return 0;
    const uint32_t hs = mc_civic_field(c, F_HACTIVE_START, 12), he = mc_civic_field(c, F_HACTIVE_END, 12);
    const uint32_t vs = mc_civic_field(c, F_VACTIVE_START, 12), ve = mc_civic_field(c, F_VACTIVE_END, 12);
    const uint32_t depth = 1u << (c->clut_format & 7u);
    if (he <= hs || ve <= vs || depth > 32) return 0;
    /* Two dots a unit, one at 16 and 32 bits on the 64-bit path ($040). */
    view->width = (depth >= 16 && mc_civic_field(c, F_WIDE, 1) ? 1u : 2u) * (he - hs);
    view->height = (ve - vs) / 2u;
    view->depth = depth;
    view->row_bytes = 32u * mc_civic_field(c, F_ROW, 8);
    const uint32_t base = 32u * mc_civic_field(c, F_BASE, 9);
    if (view->row_bytes < view->width * depth / 8u) view->row_bytes = view->width * depth / 8u;
    if (base + (uint64_t)view->row_bytes * view->height > c->vram_size) return 0;
    view->pixels = c->vram + base;
    if (depth <= 8) {
        const unsigned fill = (1u << (8u - depth)) - 1u;
        for (unsigned p = 0; p < (1u << depth); p++) c->palette[p] = c->clut[(p << (8u - depth) | fill) & 0xFFu];
    }
    view->palette = c->palette;
    return 1;
}

int mc_civic_init(mc_civic *c, const mc_hw_host *host, const char *name, uint32_t vram_size, mc_civic_monitor monitor) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_civic_ops;
    c->dev.host = host;
    c->dev.name = name;
    c->monitor = monitor;
    c->vram_size = vram_size;
    c->vram = calloc(1, vram_size);
    c->frame_length = (uint64_t)MC_C15M_HZ * 100u / monitors[monitor].hz100;
    return c->vram != NULL;
}

void mc_civic_free(mc_civic *c) {
    free(c->vram);
    c->vram = NULL;
}
