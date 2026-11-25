/* Livadix — Capella (capella.h). Livadix's own code. */
#include "capella.h"

#include <string.h>

static void update(mc_capella *c) {
    if (c->cpu != c->latch) {
        c->cpu = c->latch;
        mc_dev_line(&c->dev, MC_CAPELLA_LINE_CPU, c->cpu);
    }
}

void mc_capella_set_ipl(mc_capella *c, unsigned level) {
    if (level > 7 || level == c->ipl) return;
    c->ipl = (uint8_t)level;
    c->latch = 1;
    update(c);
}

static uint32_t capella_read(void *dev, uint32_t offset, unsigned size) {
    const mc_capella *c = dev;
    uint32_t result = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i, r = a & ~3u;
        uint32_t value = 0;
        if (r == MC_CAPELLA_IPL) value = c->ipl ^ 7u;
        else if (r == MC_CAPELLA_ACK) value = c->latch;
        else if (r / 4 < sizeof c->reg / sizeof *c->reg) value = c->reg[r / 4];
        result = result << 8 | (uint8_t)(value >> (8u * (3u - (a & 3u))));
    }
    return result;
}

static void capella_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_capella *c = dev;
    const uint32_t previous = c->reg[3];
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i, r = a & ~3u;
        if (r == MC_CAPELLA_IPL || r / 4 >= sizeof c->reg / sizeof *c->reg) continue;
        const unsigned shift = 8u * (3u - (a & 3u));
        const uint32_t byte = value >> (8u * (size - i - 1u)) & 0xFFu;
        c->reg[r / 4] = (c->reg[r / 4] & ~(0xFFu << shift)) | byte << shift;
    }
    if (((previous ^ c->reg[3]) & 6u) && c->map_cache) c->map_cache(c->map_ctx, c->reg[3]);
    /* Both nanokernel acknowledge paths store a zero longword, then read
     * twice to drain posted cycles ($FFF0736C, $FFF158B0). */
    if (offset == MC_CAPELLA_ACK && size == 4 && !value) {
        c->latch = 0;
        update(c);
    }
}

static void capella_reset(void *dev) {
    mc_capella *c = dev;
    memset(c->reg, 0, sizeof c->reg);
    if (c->map_cache) c->map_cache(c->map_ctx, 0);
    c->ipl = c->latch = 0;
    update(c);
}

static int capella_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev; (void)offset; (void)size;
    return 1;
}

const mc_dev_ops mc_capella_ops = {.read = capella_read, .write = capella_write, .reset = capella_reset, .stable = capella_stable};

void mc_capella_init(mc_capella *c, const mc_hw_host *host, const char *name) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_capella_ops;
    c->dev.host = host;
    c->dev.name = name;
}
