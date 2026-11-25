/* Livadix — the CSC (csc.h). Livadix's own code. */
#include "csc.h"

#include <string.h>

static uint32_t csc_read(void *dev, uint32_t offset, unsigned size) {
    mc_csc *c = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const unsigned r = offset + i;
        if (r >= MC_CSC_NREG) {         /* past the registers: zeros (csc.h)† */
            v <<= 8;
            continue;
        }
        uint8_t b = c->reg[r];
        if (r == 0x00 && c->id) b = c->id;
        else if (r == 0x01 || r == 0x02) b = (uint8_t)((b & 0xF8u) | c->panel);
        else if (r == MC_CSC_CLUT_DATA) {
            b = c->clut[c->reg[MC_CSC_CLUT_ADDR]][c->clut_step];
            if (++c->clut_step == 3) {
                c->clut_step = 0;
                c->reg[MC_CSC_CLUT_ADDR]++;
            }
        }
        v = v << 8 | b;
    }
    return v;
}

static void csc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_csc *c = dev;
    for (unsigned i = 0; i < size; i++) {
        const unsigned r = offset + i;
        if (r >= MC_CSC_NREG) continue;
        const uint8_t b = (uint8_t)(value >> (8 * (size - 1 - i)));
        if (r == MC_CSC_CLUT_DATA) {
            c->clut[c->reg[MC_CSC_CLUT_ADDR]][c->clut_step] = b;
            if (++c->clut_step == 3) {
                c->clut_step = 0;
                c->reg[MC_CSC_CLUT_ADDR]++;
            }
            continue;
        }
        if (r == MC_CSC_CLUT_ADDR) c->clut_step = 0;
        c->reg[r] = b;
    }
}

static void csc_reset(void *dev) {
    mc_csc *c = dev;
    memset(c->reg, 0, sizeof c->reg);
    c->clut_step = 0;
}

const mc_dev_ops mc_csc_ops = {csc_read, csc_write, NULL, csc_reset, NULL};

void mc_csc_init(mc_csc *c, const mc_hw_host *host, const char *name, const uint8_t *screen, uint8_t panel) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_csc_ops;
    c->dev.host = host;
    c->dev.name = name;
    c->screen = screen;
    c->panel = (uint8_t)(panel & 7u);
}

int mc_csc_view_get(mc_csc *c, mc_csc_view *v) {
    const uint32_t depth = 1u << (c->reg[MC_CSC_DEPTH] & 3u);
    const uint32_t entries = depth == 8 ? 256u : 1u << depth;
    for (uint32_t i = 0; i < entries; i++) {
        /* A pixel of d bits addresses the CLUT entry with its bits on top
         * (the driver's tables for the lower depths)†. */
        const uint32_t e = depth == 8 ? i : (i << (8 - depth)) | ((1u << (8 - depth)) - 1u);
        if (c->panel == MC_CSC_PANEL_GRAY) {
            /* The gray panel: no CLUT (its driver writes none), the pixel's
             * value its level, 0 white†. */
            const uint32_t g = 255u - i * 255u / (entries - 1u);
            c->palette[i] = 0xFF000000u | g << 16 | g << 8 | g;
            continue;
        }
        c->palette[i] = 0xFF000000u | (uint32_t)c->clut[e][0] << 16 | (uint32_t)c->clut[e][1] << 8 | c->clut[e][2];
    }
    v->width = MC_CSC_WIDTH;
    v->height = MC_CSC_HEIGHT;
    v->depth = depth;
    v->row_bytes = MC_CSC_WIDTH * depth / 8u;
    v->pixels = c->screen;
    v->palette = c->palette;
    return 1;
}
