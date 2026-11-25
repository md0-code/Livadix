/* Livadix — the GSC (gsc.h). Livadix's own code. */
#include "gsc.h"

#include <string.h>

static uint32_t gsc_read(void *dev, uint32_t offset, unsigned size) {
    mc_gsc *g = dev;
    (void)size;
    const unsigned r = offset % MC_GSC_NREG;
    if (r == MC_GSC_PANEL) return (g->reg[r] & 0xF8u) | g->panel;
    return g->reg[r];
}

static void gsc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_gsc *g = dev;
    (void)size;
    g->reg[offset % MC_GSC_NREG] = (uint8_t)value;
}

static void gsc_reset(void *dev) {
    mc_gsc *g = dev;
    memset(g->reg, 0, sizeof g->reg);
}

static int gsc_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}

const mc_dev_ops mc_gsc_ops = {gsc_read, gsc_write, NULL, gsc_reset, gsc_stable};

void mc_gsc_init(mc_gsc *g, const mc_hw_host *host, const char *name, const uint8_t *screen, uint8_t panel) {
    memset(g, 0, sizeof *g);
    g->dev.ops = &mc_gsc_ops;
    g->dev.host = host;
    g->dev.name = name;
    g->screen = screen;
    g->panel = (uint8_t)(panel & 7u);
}

int mc_gsc_view_get(mc_gsc *g, mc_gsc_view *v) {
    const uint8_t c = g->reg[MC_GSC_CONTROL];
    if (!(c & 0x20u)) return 0;
    unsigned shift = c & 7u;
    if (shift > 2) shift = 2;
    const uint32_t depth = 1u << shift, levels = 1u << depth;
    for (uint32_t i = 0; i < levels; i++) {
        const uint32_t y = 255u - i * 255u / (levels - 1u);
        g->palette[i] = 0xFF000000u | y << 16 | y << 8 | y;
    }
    v->width = MC_GSC_WIDTH;
    v->height = g->height ? g->height : MC_GSC_HEIGHT;
    v->depth = depth;
    v->row_bytes = MC_GSC_WIDTH * depth / 8u;
    v->pixels = g->screen;
    v->palette = g->palette;
    return 1;
}
