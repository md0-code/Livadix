/* Livadix — DAFB (see dafb.h for sources). Livadix's own code. */
#include "dafb.h"

#include <stdlib.h>
#include <string.h>

/* DAFB registers ($F980 0000 + 4n, DAFB II spec 6.6.1). */
enum {
    R_BASE_HI = 0, R_BASE_LO = 1, R_ROW_WORDS = 2, R_CLOCK = 3, R_CONFIG = 4, R_BLOCK = 5,
    R_PAGE = 6, R_SENSE = 7, R_RESET = 8, R_SCSI0 = 9, R_SCSI1 = 10, R_TEST = 11
};
/* Swatch registers ($F980 0100 + 4n, 6.6.2). */
enum {
    S_MODE = 0, S_MASK = 1, S_STATUS = 2, S_CLR_CURSOR = 3, S_CLR_ANIM = 4, S_CLR_VBL = 5,
    S_CURSOR = 6, S_ANIM = 7, S_TEST = 8, S_HSERR = 9, S_HLFLN = 10, S_HEQ = 11, S_HSP = 12,
    S_HBWAY = 13, S_HBRST = 14, S_HBP = 15, S_HAL = 16, S_HFP = 17, S_HPIX = 18, S_VHLINE = 19,
    S_VSYNC = 20, S_VBPEQ = 21, S_VBP = 22, S_VAL = 23, S_VFP = 24, S_VFPEQ = 25, S_ADJUST = 26,
    S_LINE = 27
};

typedef struct monitor_info {
    uint8_t code;           /* sense lines 2-0 (HW 30, Table S-1) */
    int8_t ext;             /* type-7 code: 2 low (1, 0), 1 low (2, 0), 0 low (2, 1); -1 none */
    uint32_t hz100;         /* frame rate in 1/100 Hz */
} monitor_info;

static const monitor_info monitors[MC_MONITOR_COUNT] = {
    [MC_MONITOR_NONE] = {7, 0x3F, 6000},
    [MC_MONITOR_RGB12] = {2, -1, 6015},
    [MC_MONITOR_HIRES13] = {6, -1, 6667},
    [MC_MONITOR_PORTRAIT] = {5, -1, 7508},
    [MC_MONITOR_RGB16] = {7, 0x2D, 7455},     /* 10 11 01 */
    [MC_MONITOR_RGB19] = {7, 0x3A, 7493},     /* 11 10 10 */
    [MC_MONITOR_RGB21] = {0, -1, 7508},
    [MC_MONITOR_VGA] = {7, 0x17, 5994},       /* 01 01 11 */
};

mc_dafb_monitor mc_dafb_monitor_for(uint32_t width, uint32_t height) {
    if (width == 512 && height == 384) return MC_MONITOR_RGB12;
    if (width == 640 && height == 480) return MC_MONITOR_HIRES13;
    if (width == 640 && height == 870) return MC_MONITOR_PORTRAIT;
    if (width == 832 && height == 624) return MC_MONITOR_RGB16;
    if (width == 1152 && height == 870) return MC_MONITOR_RGB21;
    /* The Quadra 700 ROM drives no 1024 x 768 monitor (it reports none on
     * the 19" codes) and only 640 x 480 on a VGA monitor. */
    return MC_MONITOR_NONE;
}

uint32_t mc_dafb_max_depth(mc_dafb_monitor monitor) {
    switch (monitor) {
    case MC_MONITOR_RGB12: case MC_MONITOR_HIRES13: return 32;
    case MC_MONITOR_PORTRAIT: case MC_MONITOR_RGB16: case MC_MONITOR_RGB21: return 8;
    default: return 0;
    }
}

uint8_t mc_dafb_sense(const mc_dafb *d) {
    const monitor_info *m = &monitors[d->monitor];
    const uint8_t driven = (uint8_t)(~d->reg[R_SENSE] & 7u);     /* 0 = driven (6.6.1) */
    const uint8_t level = (uint8_t)(d->reg[R_TEST] & 7u);       /* the level driven (Test register) */
    uint8_t out = 0;
    for (unsigned line = 0; line < 3; line++) {
        unsigned v;
        if (driven >> line & 1u) v = level >> line & 1u;
        else if (!(m->code >> line & 1u)) v = 0;               /* grounded by the monitor */
        else {
            v = 1;
            /* Type-7: tied to a line driven low it reads the table's bit. */
            for (unsigned low = 0; low < 3 && m->ext >= 0; low++) {
                if (!(driven >> low & 1u) || (level >> low & 1u) || low == line) continue;
                const unsigned pair = (unsigned)m->ext >> (2u * low) & 3u;      /* 0 low: bits 1-0, 1 low: 3-2, 2 low: 5-4 */
                /* Within a pair the higher-numbered line is the left bit. */
                const unsigned other_hi = low == 2 ? 1u : 2u;
                const unsigned bit = line == other_hi ? (pair >> 1) & 1u : pair & 1u;
                if (!bit) v = 0;
            }
        }
        out |= (uint8_t)(v << line);
    }
    return out;
}

/* --- VBL ---------------------------------------------------------------------- */

static void update_irq(mc_dafb *d) {
    const uint8_t irq = (d->vbl_status & d->swatch[S_MASK] & 7u) != 0;
    if (irq != d->irq) {
        d->irq = irq;
        mc_dev_line(&d->dev, MC_DAFB_LINE_VBL, irq);
    }
}

static int timing_running(const mc_dafb *d) {
    return (d->swatch[S_MODE] & 1u) == 0 && (d->reg[R_RESET] & 4u) == 0;
}

static uint32_t total_lines(const mc_dafb *d) { return d->swatch[S_VHLINE] >= 4 ? d->swatch[S_VHLINE] / 2u : 525u; }

/* The time within a frame at which line `line` begins (lines counted from
 * the vertical zero reference). */
static uint64_t line_time(const mc_dafb *d, uint32_t line) {
    return (uint64_t)line * d->frame_length / total_lines(d);
}

/* The next of the three Swatch interrupt sources (6.6.2.1): vertical
 * blanking at the end of the active lines, the cursor line counted from the
 * vertical zero reference, the animate line counted from the first line of
 * vertical blanking. */
static void next_event(const mc_dafb *d, uint64_t now, uint64_t *when, unsigned *bits) {
    const uint32_t lines = total_lines(d);
    const uint32_t vbl_line = d->swatch[S_VFP] >= 2 ? d->swatch[S_VFP] / 2u % lines : 0u;
    const uint32_t points[3] = {vbl_line, (vbl_line + d->swatch[S_ANIM]) % lines, d->swatch[S_CURSOR] % lines};
    *when = MC_NEVER;
    *bits = 0;
    for (unsigned frame = 0; frame < 2 && *when == MC_NEVER; frame++)
        for (unsigned i = 0; i < 3; i++) {
            const uint64_t t = d->frame_start + frame * d->frame_length + line_time(d, points[i]);
            if (t <= now) continue;
            if (t < *when) { *when = t; *bits = 1u << i; }
            else if (t == *when) *bits |= 1u << i;
        }
}

static void arm(mc_dafb *d) {
    if (!timing_running(d)) {
        mc_dev_schedule(&d->dev, MC_NEVER);
        return;
    }
    const uint64_t now = mc_dev_now(&d->dev);
    if (now >= d->frame_start + d->frame_length) d->frame_start += (now - d->frame_start) / d->frame_length * d->frame_length;
    uint64_t when;
    unsigned bits;
    next_event(d, now, &when, &bits);
    d->pending_bits = (uint8_t)bits;
    mc_dev_schedule(&d->dev, when);
}

static void dafb_event(void *dev, uint64_t now) {
    mc_dafb *d = dev;
    if (now >= d->frame_start + d->frame_length) d->frame_start += (now - d->frame_start) / d->frame_length * d->frame_length;
    /* Status bits: 0 VBL, 1 animate line, 2 cursor line. */
    d->vbl_status |= d->pending_bits;
    update_irq(d);
    arm(d);
}

static uint16_t current_line(const mc_dafb *d) {
    const uint64_t now = mc_dev_now(&d->dev);
    return (uint16_t)(((now - d->frame_start) % d->frame_length) * total_lines(d) / d->frame_length);
}

/* --- registers ---------------------------------------------------------------- */

static uint32_t clut_read(mc_dafb *d, uint32_t offset) {
    switch ((offset >> 4) & 3u) {
    case 0: return d->clut_index;
    case 1: {
        const uint32_t rgb = d->palette[d->clut_index];
        const uint8_t v = (uint8_t)(rgb >> (16u - 8u * d->clut_component));
        if (++d->clut_component == 3) {
            d->clut_component = 0;
            d->clut_index++;
        }
        return v;
    }
    case 2: return d->antelope ? d->dac_reg[d->clut_index & 15u] : d->clut_config;
    default: return 0;
    }
}

static void clut_write(mc_dafb *d, uint32_t offset, uint8_t value) {
    switch ((offset >> 4) & 3u) {
    case 0:
        d->clut_index = value;
        d->clut_component = 0;
        break;
    case 1:
        d->clut_rgb[d->clut_component] = value;
        if (++d->clut_component == 3) {
            d->clut_component = 0;
            d->palette[d->clut_index++] = (uint32_t)d->clut_rgb[0] << 16 | (uint32_t)d->clut_rgb[1] << 8 | d->clut_rgb[2];
            d->serial++;
        }
        break;
    case 2:
        if (d->antelope) {
            d->dac_reg[d->clut_index & 15u] = value;
            if (d->clut_index & 15u) break;
        }
        d->clut_config = value;
        d->serial++;
        break;
    default: break;
    }
}

static uint32_t dafb_read(void *dev, uint32_t offset, unsigned size) {
    mc_dafb *d = dev;
    uint32_t v = 0;
    if (offset < 0x100) {
        const unsigned r = (offset >> 2) & 63u;
        v = d->reg[r] & 0xFFFu;
        /* The register reads the lines inverted: a grounded line reads 1. (The
         * Quadra 700 ROM picks the Hi-Res 13" mode for a read of 1 on sense 0,
         * the 21" mode for 7.) */
        if (r == R_SENSE) v = ~mc_dafb_sense(d) & 7u;
        else if (r == R_SCSI0 || r == R_SCSI1)                  /* bit 9: the channel's DREQ */
            v = (v & 0x1FFu) | (d->scsi_drq[r - R_SCSI0] ? 0x200u : 0);
    } else if (offset < 0x200) {
        const unsigned r = ((offset - 0x100) >> 2) & 31u;
        if (r == S_STATUS) v = d->vbl_status;
        else if (r == S_LINE) v = current_line(d);
        else if (r == S_CLR_CURSOR || r == S_CLR_ANIM || r == S_CLR_VBL) {
            d->vbl_status &= (uint8_t)~(1u << (r == S_CLR_VBL ? 0 : r == S_CLR_ANIM ? 1 : 2));
            update_irq(d);
        } else v = d->swatch[r] & 0xFFFu;
    } else if (offset < 0x300) {
        v = clut_read(d, offset);
    } else {
        v = d->clock[(offset >> 4) & 15u] & 0xFu;
    }
    /* The registers are longwords on the 32-bit bus: a byte or word read
     * takes its lane (Mac OS 8's SCSI driver tests DREQ with a byte read of
     * $F980 0026). */
    if (size == 1) v = (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
    else if (size == 2) v = (v >> (8u * (2u - (offset & 2u)))) & 0xFFFFu;
    return v;
}

static void dafb_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_dafb *d = dev;
    (void)size;
    if (offset < 0x100) {
        const unsigned r = (offset >> 2) & 63u;
        const uint16_t old = d->reg[r];
        d->reg[r] = (uint16_t)(value & 0xFFFu);
        if (r == R_RESET && ((old ^ d->reg[r]) & 4u)) {
            d->frame_start = mc_dev_now(&d->dev);
            arm(d);
        }
        d->serial++;
    } else if (offset < 0x200) {
        const unsigned r = ((offset - 0x100) >> 2) & 31u;
        if (r == S_CLR_CURSOR || r == S_CLR_ANIM || r == S_CLR_VBL) {
            d->vbl_status &= (uint8_t)~(1u << (r == S_CLR_VBL ? 0 : r == S_CLR_ANIM ? 1 : 2));
        } else if (r != S_STATUS && r != S_LINE) {
            const uint16_t old = d->swatch[r];
            d->swatch[r] = (uint16_t)(value & 0xFFFu);
            if (r == S_MODE && ((old ^ d->swatch[r]) & 1u)) d->frame_start = mc_dev_now(&d->dev);
            if (old != d->swatch[r]) arm(d);
        }
        update_irq(d);
        d->serial++;
    } else if (offset < 0x300) {
        clut_write(d, offset, (uint8_t)value);
    } else {
        d->clock[(offset >> 4) & 15u] = (uint8_t)(value & 0xFu);
    }
}

static void dafb_reset(void *dev) {
    mc_dafb *d = dev;
    /* Reset values (DAFB II spec 6.6.1-6.6.2). */
    memset(d->reg, 0, sizeof d->reg);
    memset(d->swatch, 0, sizeof d->swatch);
    d->reg[R_CONFIG] = 0xF00;
    d->reg[R_SENSE] = 7;
    d->reg[R_RESET] = 7;
    d->reg[R_SCSI0] = d->reg[R_SCSI1] = 0x101;
    d->swatch[S_MODE] = 0xFFF;
    d->vbl_status = 0;
    d->irq = 0;
    mc_dev_line(&d->dev, MC_DAFB_LINE_VBL, 0);
    mc_dev_schedule(&d->dev, MC_NEVER);
}

/* The control registers (but the sense lines, read through the monitor)
 * and the swatch's VBL status and plain registers change only at the
 * chip's events and at writes (the SCSI DREQ bits at the 53C96's events);
 * the line counter moves with time and the clear registers clear. */
static int dafb_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)size;
    if (offset < 0x100) return ((offset >> 2) & 63u) != R_SENSE;
    if (offset < 0x200) {
        const unsigned r = ((offset - 0x100) >> 2) & 31u;
        return r != S_LINE && r != S_CLR_CURSOR && r != S_CLR_ANIM && r != S_CLR_VBL;
    }
    return 0;
}

const mc_dev_ops mc_dafb_ops = {dafb_read, dafb_write, dafb_event, dafb_reset, dafb_stable};

/* The picture as the registers define it: the base (6.6.1, bits 20:5 from
 * two registers) and row bytes (row words x 4) in VRAM; the active lines
 * from Swatch's vertical active and front porch (in half-lines); the
 * active width from its horizontal active and front porch, in timing
 * clocks, times the pixels the CLUT/DAC multiplexes per clock (its
 * configuration bits 6-5: 1, 2 or 4, as the Quadra 700 ROM programs them
 * for the 1, 2 and 4 pixel-per-clock monitors); the depth from the 32 bits
 * the DAC loads per load clock, which DAFB divides from the video clock by
 * the clock configuration's divisor (6.6.1: 255 = 1, n = n + 2). */
int mc_dafb_view_get(mc_dafb *d, mc_dafb_view *view) {
    if ((d->reg[R_RESET] & 7u) || d->monitor == MC_MONITOR_NONE) return 0;
    const uint32_t base = ((uint32_t)d->reg[R_BASE_HI] << 9 | (uint32_t)(d->reg[R_BASE_LO] & 0xFu) << 5) & 0x1FFFFFu;
    const uint32_t row_bytes = (uint32_t)d->reg[R_ROW_WORDS] << 2;
    const uint32_t height = d->swatch[S_VFP] > d->swatch[S_VAL] ? (uint32_t)(d->swatch[S_VFP] - d->swatch[S_VAL]) / 2u : 0;
    const uint32_t mux = 1u << ((d->clut_config >> 5) & 3u);
    const uint32_t clocks = d->swatch[S_HFP] > d->swatch[S_HAL] ? (uint32_t)(d->swatch[S_HFP] - d->swatch[S_HAL]) : 0;
    const uint32_t div_field = d->reg[R_CLOCK] & 0xFFu;
    const uint32_t divisor = div_field == 0xFFu ? 1u : div_field + 2u;
    const uint32_t per_load = divisor * mux;
    uint32_t depth = per_load ? 32u / per_load : 0;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32) return 0;
    const uint32_t width = clocks * mux;
    if (!row_bytes || !height || !width || (uint64_t)width * depth > (uint64_t)row_bytes * 8u ||
        base + (uint64_t)row_bytes * height > d->vram_size)
        return 0;
    view->width = width;
    view->height = height;
    view->depth = depth;
    view->row_bytes = row_bytes;
    view->pixels = d->vram + base;
    view->palette = d->palette;
    return 1;
}

int mc_dafb_init(mc_dafb *d, const mc_hw_host *host, const char *name, uint32_t vram_size, mc_dafb_monitor monitor) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &mc_dafb_ops;
    d->dev.host = host;
    d->dev.name = name;
    d->vram_size = vram_size;
    d->vram = calloc(1, vram_size);
    d->monitor = monitor;
    d->frame_length = (uint64_t)MC_C15M_HZ * 100u / monitors[monitor].hz100;
    return d->vram != NULL;
}

void mc_dafb_free(mc_dafb *d) {
    free(d->vram);
    d->vram = NULL;
}
