/* Livadix — Ariel II, the PDM boards' CLUT DAC (ariel.h).
 *
 * Livadix's own code. */
#include "ariel.h"

#include <string.h>

static void entry(mc_ariel *a, unsigned i) {
    a->palette[i] = 0xFF000000u | (uint32_t)a->clut[i][0] << 16 | (uint32_t)a->clut[i][1] << 8 | a->clut[i][2];
}

void mc_ariel_init(mc_ariel *a, const mc_hw_host *host, const char *name) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_ariel_ops;
    a->dev.host = host;
    a->dev.name = name;
    for (unsigned i = 0; i < 256; i++) entry(a, i);
}

const uint32_t *mc_ariel_palette(const mc_ariel *a) { return a->palette; }

void mc_ariel_view_palette(const mc_ariel *a, unsigned depth, uint32_t *out) {
    if (depth == 0 || depth > 8) return;
    const unsigned shift = 8u - depth, low = (1u << shift) - 1u;
    for (unsigned i = 0; i < (1u << depth); i++) out[i] = a->palette[(i << shift | low) & 0xFFu];
}

static uint8_t reg_read(mc_ariel *a, unsigned reg) {
    switch (reg) {
    case 0: return a->addr;
    case 1: {
        const uint8_t v = a->clut[a->addr][a->rgb];
        if (++a->rgb == 3) {
            a->rgb = 0;
            a->addr++;
        }
        return v;
    }
    case 2: return a->ctl;
    default: return a->reg3;
    }
}

static void reg_write(mc_ariel *a, unsigned reg, uint8_t v) {
    switch (reg) {
    case 0:
        a->addr = v;
        a->rgb = 0;
        break;
    case 1:
        a->clut[a->addr][a->rgb] = v;
        if (++a->rgb == 3) {
            entry(a, a->addr);
            a->rgb = 0;
            a->addr++;
        }
        break;
    case 2: a->ctl = v; break;
    default: a->reg3 = v; break;
    }
}

static uint32_t ariel_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | reg_read(dev, (offset + i) & 3u);
    return v;
}

static void ariel_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) reg_write(dev, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void ariel_reset(void *dev) {
    mc_ariel *a = dev;
    a->addr = a->rgb = a->ctl = a->reg3 = 0;
}

const mc_dev_ops mc_ariel_ops = {ariel_read, ariel_write, NULL, ariel_reset, NULL};
