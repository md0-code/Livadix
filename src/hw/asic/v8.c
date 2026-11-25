/* Livadix — the V8 family (v8.h).
 *
 * Livadix's own code. */
#include "v8.h"

#include <string.h>

/* The 68020 LC's 24-bit decode (Color Classic Developer Note, Figure 2-2,
 * "similar to that of the Macintosh LC"): RAM $00-$9F, ROM $A0-$DF at
 * $40A0 0000, the expansion slot $E0 at $FE00 0000, I/O $F0 at
 * $50F0 0000. */
const uint32_t mc_v8_map24[16] = {
    0x00000000u, 0x00100000u, 0x00200000u, 0x00300000u, 0x00400000u, 0x00500000u, 0x00600000u, 0x00700000u,
    0x00800000u, 0x00900000u, 0x40A00000u, 0x40B00000u, 0x40C00000u, 0x40D00000u, 0xFE000000u, 0x50F00000u,
};

uint32_t mc_v8_simm_bank(const mc_v8 *v) { return (uint32_t)v->simm_mb[v->ramcfg >> 6] << 20; }

void mc_v8_set_simm_sizes(mc_v8 *v, const uint8_t mb[4]) { memcpy(v->simm_mb, mb, 4); }

/* --- the registers -------------------------------------------------------------- */

static uint32_t v8_read(void *dev, uint32_t offset, unsigned size) {
    mc_v8 *v = dev;
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t reg = (offset + i) & 0x1Fu;
        uint8_t b;
        if (reg == MC_V8_RAMCFG) b = v->ramcfg;
        else if (reg == MC_RBV_MONP) b = (uint8_t)((v->rbv.monp & ~0x38u) | (v->rbv.sense << 3));
        else b = (uint8_t)mc_rbv_ops.read(&v->rbv, reg, 1);
        r = r << 8 | b;
    }
    return r;
}

static void v8_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_v8 *v = dev;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t reg = (offset + i) & 0x1Fu;
        const uint8_t b = (uint8_t)(value >> (8 * (size - 1 - i)));
        if (reg == MC_V8_RAMCFG) {
            const uint8_t old = v->ramcfg;
            v->ramcfg = b;
            if ((old ^ b) & 0xC0u && v->remap) v->remap(v->ctx);
        } else {
            mc_rbv_ops.write(&v->rbv, reg, 1, b);
        }
    }
}

static void v8_event(void *dev, uint64_t now) { mc_rbv_ops.event(&((mc_v8 *)dev)->rbv, now); }

static void v8_reset(void *dev) {
    mc_v8 *v = dev;
    mc_rbv_ops.reset(&v->rbv);
}

const mc_dev_ops mc_v8_ops = {v8_read, v8_write, v8_event, v8_reset, NULL};

/* The Classic II's six-bit register at its window's first byte; the rest
 * of the window reads 0 (the ROM's start reads a long word at +$38 and
 * uses it as an address, ROM 3193670E at $40A0 3CE2)†. */
static uint32_t eagle_read(void *dev, uint32_t offset, unsigned size) {
    const mc_v8 *v = dev;
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | (offset + i == 0 ? v->eagle : 0);
    return r;
}
static void eagle_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++)
        if (offset + i == 0) ((mc_v8 *)dev)->eagle = (uint8_t)(value >> (8 * (size - 1 - i)) & 0x3Fu);
}
const mc_dev_ops mc_v8_eagle_ops = {eagle_read, eagle_write, NULL, NULL, NULL};

/* --- the VDAC ------------------------------------------------------------------- */

static void palette_entry(mc_v8 *v, unsigned i) {
    v->palette[i] = 0xFF000000u | (uint32_t)v->clut[i][0] << 16 | (uint32_t)v->clut[i][1] << 8 | v->clut[i][2];
}

static uint8_t dac_read(mc_v8 *v, uint32_t reg) {
    switch (reg) {
    case 0: return v->dac_addr;
    case 1: {
        const uint8_t b = v->clut[v->dac_addr][v->dac_rgb];
        if (++v->dac_rgb == 3) {
            v->dac_rgb = 0;
            v->dac_addr++;
        }
        return b;
    }
    case 2: return v->dac_ctl;
    default: return 0;
    }
}

static void dac_write(mc_v8 *v, uint32_t reg, uint8_t b) {
    switch (reg) {
    case 0: v->dac_addr = b; v->dac_rgb = 0; break;
    case 1:
        v->clut[v->dac_addr][v->dac_rgb] = b;
        if (++v->dac_rgb == 3) {
            palette_entry(v, v->dac_addr);
            v->dac_rgb = 0;
            v->dac_addr++;
        }
        break;
    case 2: v->dac_ctl = b; break;
    default: break;
    }
}

static uint32_t vdac_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | dac_read(dev, (offset + i) & 3u);
    return r;
}
static void vdac_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) dac_write(dev, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}
const mc_dev_ops mc_v8_vdac_ops = {vdac_read, vdac_write, NULL, NULL, NULL};

/* --- setup and the picture ------------------------------------------------------ */

void mc_v8_init(mc_v8 *v, const mc_hw_host *host, const char *name, mc_v8_kind kind, uint8_t sense,
                void (*remap)(void *ctx), void *ctx) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_v8_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->kind = kind;
    v->remap = remap;
    v->ctx = ctx;
    static const uint8_t simm_mb[4] = {0, 2, 4, 8};
    memcpy(v->simm_mb, simm_mb, 4);
    for (unsigned i = 0; i < 256; i++) palette_entry(v, i);
    /* The RBV part schedules itself under the V8's name. */
    mc_rbv_init(&v->rbv, host, name, sense);
}

uint32_t mc_v8_max_depth(mc_v8_kind kind, uint8_t sense, uint32_t vram) {
    if (kind == MC_V8_EAGLE) return 1;
    uint32_t w = 0, h = 0;
    if (sense == MC_RBV_SENSE_RGB12) w = 512, h = 384;
    else if (sense == MC_RBV_SENSE_HIRES) w = 640, h = 480;
    else return 0;
    /* The LC's ROM sets a 16-bit mode written in its slot PRAM back to 1
     * bit at 512 x 384; Spice shows 16 bits with 512 KB of VRAM (Color
     * Classic Developer Note, Table 1-1). */
    uint32_t d = kind == MC_V8_SPICE ? 16u : 8u;
    /* Rows 1024 bytes apart (mc_v8_view_get): a depth fits when a row's
     * pixels do and the rows fit the VRAM. */
    if (1024u * h > vram) return 0;
    while (d > 1 && w * d / 8u > 1024u) d >>= 1;
    return d;
}

int mc_v8_view_get(mc_v8 *v, const uint8_t *vram, uint32_t vram_size, const uint8_t *screen, mc_v8_view *view) {
    if (v->kind == MC_V8_EAGLE) {
        /* The Classic II's built-in 9-inch screen: 512 x 342, one bit,
         * black on white (pixel 1 black). */
        if (!screen) return 0;
        view->width = 512;
        view->height = 342;
        view->depth = 1;
        view->row_bytes = 64;
        view->pixels = screen;
        v->view_palette[0] = 0xFFFFFFFFu;
        v->view_palette[1] = 0xFF000000u;
        view->palette = v->view_palette;
        return 1;
    }
    if (!v->rbv.frame || (v->rbv.monp & 0x40u)) return 0;
    const unsigned code = v->rbv.monp & 7u;
    if (code > 4) return 0;
    const uint32_t depth = 1u << code;
    view->width = v->rbv.sense == MC_RBV_SENSE_HIRES ? 640 : 512;
    view->height = v->rbv.sense == MC_RBV_SENSE_HIRES ? 480 : 384;
    view->depth = depth;
    /* Rows 1024 bytes apart at every depth and monitor (the LC ROM's
     * Finder screen: the menu bar's lower line at VRAM offset 19 x 1024
     * in 1 to 8 bits, at 512 x 384 and 640 x 480; low memory ScreenRow
     * $400). */
    view->row_bytes = 1024;
    if ((uint64_t)view->row_bytes * view->height > vram_size) return 0;
    if (depth <= 8) {
        /* A pixel's bits on top, the low bits set (v8.h). */
        const unsigned low = (1u << (8u - depth)) - 1u;
        for (unsigned i = 0; i < (1u << depth); i++) v->view_palette[i] = v->palette[(i << (8u - depth)) | low];
        view->palette = v->view_palette;
    } else {
        view->palette = NULL;
    }
    view->pixels = vram;
    return 1;
}
