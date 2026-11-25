/* Livadix — RaDACal and DACula, the built-in video's DAC (radacal.h).
 *
 * Livadix's own code. */
#include "radacal.h"

#include <string.h>

void mc_radacal_reset(mc_radacal *d) {
    d->address = d->component = 0;
    memset(d->reg, 0, sizeof d->reg);
}

void mc_radacal_init(mc_radacal *d, const mc_hw_host *host, const char *name, mc_dac_kind kind) {
    memset(d, 0, sizeof *d);
    d->dev.ops = &mc_radacal_ops;
    d->dev.host = host;
    d->dev.name = name;
    d->kind = (uint8_t)kind;
    mc_radacal_reset(d);
}

static uint32_t dac_read(void *p, uint32_t offset, unsigned size) {
    mc_radacal *d = p;
    (void)size;
    switch (offset & 3u) {
    case MC_DAC_ADDRESS: return d->address;
    case MC_DAC_CONTROL:
        if (d->address == MC_DAC_ID_REGISTER && d->kind == MC_DAC_DACULA) return MC_DACULA_ID;
        return d->reg[d->address];
    case MC_DAC_CLUT: {
        const uint8_t v = d->clut[d->address][d->component];
        if (++d->component == 3) { d->component = 0; d->address++; }
        return v;
    }
    default: {
        const uint8_t v = d->overlay[d->address & 7u][d->component];
        if (++d->component == 3) { d->component = 0; d->address++; }
        return v;
    }
    }
}

static void dac_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_radacal *d = p;
    const uint8_t v = (uint8_t)value;
    (void)size;
    switch (offset & 3u) {
    case MC_DAC_ADDRESS: d->address = v; d->component = 0; break;
    case MC_DAC_CONTROL: d->reg[d->address] = v; break;
    case MC_DAC_CLUT: {
        const uint8_t a = d->address;
        d->clut[a][d->component] = v;
        if (++d->component == 3) {
            d->component = 0;
            d->palette[a] = (uint32_t)d->clut[a][0] << 16 | (uint32_t)d->clut[a][1] << 8 | d->clut[a][2];
            d->serial++;
            d->address++;
        }
        break;
    }
    default:
        d->overlay[d->address & 7u][d->component] = v;
        if (++d->component == 3) { d->component = 0; d->address++; }
        break;
    }
}

static void dac_reset_op(void *p) { mc_radacal_reset(p); }

const mc_dev_ops mc_radacal_ops = {dac_read, dac_write, NULL, dac_reset_op, NULL};
