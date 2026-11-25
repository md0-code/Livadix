/* Livadix — Valkyrie (see valkyrie.h for sources). Livadix's own code. */
#include "valkyrie.h"

#include <stdlib.h>
#include <string.h>

enum { R_CONTROL = 0, R_DEPTH = 1, R_MODE2 = 2, R_CONFIG = 3, R_CLEAR = 4, R_STATUS = 5, R_ENABLE = 6, R_SENSE = 7 };

typedef struct monitor_info {
    uint8_t code;           /* sense lines 2-0 (Table 3-11, standard code) */
    int8_t ext;             /* extended code: line 2 low (1, 0), 1 low (2, 0), 0 low (2, 1) in bits 5-4, 3-2, 1-0; -1 none */
    uint32_t hz100;         /* frame rate in 1/100 Hz (Tables 3-13, 3-14) */
} monitor_info;

static const monitor_info monitors[MC_VALKYRIE_MONITORS] = {
    [MC_VALKYRIE_NONE] = {7, 0x3F, 6000},
    [MC_VALKYRIE_RGB12] = {2, -1, 6015},
    [MC_VALKYRIE_RGB14] = {6, -1, 6672},
    [MC_VALKYRIE_VGA] = {7, 0x17, 5994},
    [MC_VALKYRIE_RGB16] = {7, 0x2D, 7455},
};

/* The display modes (the control register's bits 0-3) the ROM's timing
 * records give (ROM offset $F5B50, 26 bytes each: three clock words, the
 * mode at +8, the deepest depth's mode at +10, the rows less one at +12,
 * the longwords per 1-bit row less one at +14): the screen and the frame
 * rate in 1/100 Hz (Tables 3-13 and 3-14; the 16-inch monitor's 74.55 Hz
 * from HW 30). A row in display memory is the width's pixels (the ROM's
 * rowBytes for 800 x 600: 100, 200, 400 and 800, low memory ScreenRow). */
typedef struct mode_info {
    uint8_t code;
    uint16_t width, height;
    uint32_t hz100;
} mode_info;

static const mode_info modes[] = {
    {0x2, 512, 384, 6015},     /* 12-inch RGB */
    {0x6, 640, 480, 6672},     /* 14-inch RGB */
    {0x9, 832, 624, 7455},     /* 16-inch RGB */
    {0xB, 640, 480, 5994},     /* VGA */
    {0xC, 800, 600, 6031},     /* SVGA, 60 Hz */
    {0xD, 800, 600, 7218},     /* SVGA, 72 Hz */
    /* Valkyrie-AR only: 6F5724C0's native driver, saved gprf modes
     * 14/15; Performa 6400 Developer Note Table 3-14. */
    {0xE, 1024, 768, 6000},
    {0xF, 1024, 768, 7007},
};

static const mode_info *mode_of(const mc_valkyrie *v) {
    for (size_t i = 0; i < sizeof modes / sizeof *modes; i++)
        if (modes[i].code == (v->reg[R_CONTROL] & 0x0Fu) && (v->ar || modes[i].code < 0xE)) return &modes[i];
    return NULL;
}

/* The frame period: the mode's, before one is set the monitor's. */
static uint64_t frame_length(const mc_valkyrie *v) {
    const mode_info *m = mode_of(v);
    return (uint64_t)MC_C15M_HZ * 100u / (m ? m->hz100 : monitors[v->monitor].hz100);
}

mc_valkyrie_monitor mc_valkyrie_monitor_for(uint32_t width, uint32_t height) {
    if (width == 512 && height == 384) return MC_VALKYRIE_RGB12;
    if (width == 640 && height == 480) return MC_VALKYRIE_RGB14;
    if (width == 800 && height == 600) return MC_VALKYRIE_VGA;
    if (width == 832 && height == 624) return MC_VALKYRIE_RGB16;
    return MC_VALKYRIE_NONE;
}

uint32_t mc_valkyrie_max_depth(mc_valkyrie_monitor monitor) {
    switch (monitor) {
    case MC_VALKYRIE_RGB12: case MC_VALKYRIE_RGB14: return 16;
    case MC_VALKYRIE_VGA: case MC_VALKYRIE_RGB16: return 8;
    default: return 0;
    }
}

uint8_t mc_valkyrie_sense(const mc_valkyrie *v) {
    const monitor_info *m = &monitors[v->monitor];
    /* Bit n of the register drives line n low. */
    const uint8_t driven = v->reg[R_SENSE] & 7u;
    uint8_t out = 0;
    for (unsigned line = 0; line < 3; line++) {
        unsigned level;
        if (driven >> line & 1u) level = 0;
        else if (!(m->code >> line & 1u)) level = 0;
        else {
            level = 1;
            for (unsigned low = 0; low < 3 && m->ext >= 0; low++) {
                if (!(driven >> low & 1u) || low == line) continue;
                const unsigned pair = (unsigned)m->ext >> (2u * low) & 3u;
                const unsigned other_hi = low == 2 ? 1u : 2u;
                const unsigned bit = line == other_hi ? (pair >> 1) & 1u : pair & 1u;
                if (!bit) level = 0;
            }
        }
        out |= (uint8_t)(level << line);
    }
    return out;
}

/* --- VBL ------------------------------------------------------------------------ */

static void update_irq(mc_valkyrie *v) {
    const uint8_t irq = v->vbl_flag && (v->reg[R_ENABLE] & 1u);
    if (irq != v->irq) {
        v->irq = irq;
        mc_dev_line(&v->dev, MC_VALKYRIE_LINE_VBL, irq);
    }
}

static void valkyrie_event(void *dev, uint64_t now) {
    mc_valkyrie *v = dev;
    v->frame_length = frame_length(v);
    while (v->next_vbl <= now) v->next_vbl += v->frame_length;
    v->vbl_flag = 1;
    update_irq(v);
    mc_dev_schedule(&v->dev, v->next_vbl);
}

static void valkyrie_reset(void *dev) {
    mc_valkyrie *v = dev;
    memset(v->reg, 0, sizeof v->reg);
    v->reg[R_CONTROL] = 0x80;
    v->vbl_flag = 0;
    update_irq(v);
    v->next_vbl = mc_dev_now(&v->dev) + v->frame_length;
    mc_dev_schedule(&v->dev, v->next_vbl);
}

static uint32_t valkyrie_read(void *dev, uint32_t offset, unsigned size) {
    mc_valkyrie *v = dev;
    const unsigned r = (offset >> 2) & 15u;
    uint8_t b;
    switch (r) {
    case R_STATUS: b = v->vbl_flag; break;
    case R_SENSE: b = (uint8_t)((v->reg[R_SENSE] & 7u) | mc_valkyrie_sense(v) << 4); break;
    default: b = v->reg[r]; break;
    }
    (void)size;
    return size == 1 ? b : size == 2 ? (uint32_t)b << 8 : (uint32_t)b << 24;
}

static void valkyrie_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_valkyrie *v = dev;
    const unsigned r = (offset >> 2) & 15u;
    const uint8_t b = (uint8_t)(size == 1 ? value : size == 2 ? value >> 8 : value >> 24);
    switch (r) {
    case R_CLEAR:
        if (b & 1u) v->vbl_flag = 0;
        update_irq(v);
        break;
    case R_STATUS: break;
    case R_ENABLE:
        if (b & 0x80u) v->reg[r] |= (uint8_t)(b & 0x7Fu);
        else v->reg[r] &= (uint8_t)~b;
        update_irq(v);
        break;
    default: v->reg[r] = b; break;
    }
}

const mc_dev_ops mc_valkyrie_ops = {valkyrie_read, valkyrie_write, valkyrie_event, valkyrie_reset, NULL};

/* --- CLUT/DAC ------------------------------------------------------------------- */

static uint32_t clut_read(void *dev, uint32_t offset, unsigned size) {
    mc_valkyrie *v = dev;
    (void)size;
    switch ((offset >> (v->ar ? 3u : 2u)) & 3u) {
    case 0: return v->clut_index;
    case 1: {
        const uint8_t c = (uint8_t)(v->palette[v->clut_index] >> (16u - 8u * v->clut_component));
        if (++v->clut_component == 3) {
            v->clut_component = 0;
            v->clut_index++;
        }
        return c;
    }
    case 3: return v->clut_control;
    default: return 0;
    }
}

static void clut_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_valkyrie *v = dev;
    (void)size;
    const uint8_t b = (uint8_t)value;
    switch ((offset >> (v->ar ? 3u : 2u)) & 3u) {
    case 0:
        v->clut_index = b;
        v->clut_component = 0;
        break;
    case 1:
        v->clut_rgb[v->clut_component] = b;
        if (++v->clut_component == 3) {
            v->clut_component = 0;
            v->palette[v->clut_index++] = (uint32_t)v->clut_rgb[0] << 16 | (uint32_t)v->clut_rgb[1] << 8 | v->clut_rgb[2];
        }
        break;
    case 3: v->clut_control = b; break;
    default: break;
    }
}

const mc_dev_ops mc_valkyrie_clut_ops = {clut_read, clut_write, NULL, NULL, NULL};

/* --- display -------------------------------------------------------------------- */

int mc_valkyrie_view_get(const mc_valkyrie *v, mc_valkyrie_view *view) {
    const mode_info *m = mode_of(v);
    if (v->monitor == MC_VALKYRIE_NONE || (v->reg[R_CONTROL] & 0x80u) || !m) return 0;
    const uint32_t depth = 1u << (v->reg[R_DEPTH] & 7u);
    if (depth > 16) return 0;
    view->width = m->width;
    view->height = m->height;
    view->depth = depth;
    view->row_bytes = (uint32_t)m->width * depth / 8u;
    if (MC_VALKYRIE_FB_OFFSET + (uint64_t)view->row_bytes * view->height > MC_VALKYRIE_VRAM) return 0;
    view->pixels = v->vram + MC_VALKYRIE_FB_OFFSET;
    view->palette = v->palette;
    return 1;
}

int mc_valkyrie_init(mc_valkyrie *v, const mc_hw_host *host, const char *name, mc_valkyrie_monitor monitor) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_valkyrie_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->monitor = monitor;
    v->vram = calloc(1, MC_VALKYRIE_VRAM);
    v->frame_length = (uint64_t)MC_C15M_HZ * 100u / monitors[monitor].hz100;
    v->reg[R_CONTROL] = 0x80;
    return v->vram != NULL;
}

int mc_valkyrie_ar_init(mc_valkyrie *v, const mc_hw_host *host, const char *name, mc_valkyrie_monitor monitor) {
    const int ok = mc_valkyrie_init(v, host, name, monitor);
    v->ar = 1;
    return ok;
}

void mc_valkyrie_free(mc_valkyrie *v) {
    free(v->vram);
    v->vram = NULL;
}
