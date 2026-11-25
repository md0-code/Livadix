/* Livadix — the PowerBook 520/540 and 190 system ASIC (pratt.h).
 *
 * Livadix's own code. */
#include "pratt.h"

#include <string.h>

static uint32_t get(const uint8_t *r, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | r[(offset + i) & (MC_PRATT_WINDOW - 1u)];
    return v;
}
static void put(uint8_t *r, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) r[(offset + i) & (MC_PRATT_WINDOW - 1u)] = (uint8_t)(value >> (8u * (size - 1u - i)));
}

static uint32_t a_read(void *dev, uint32_t offset, unsigned size) { return get(((mc_pratt *)dev)->a, offset, size); }
static void a_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { put(((mc_pratt *)dev)->a, offset, size, value); }
static uint32_t b_read(void *dev, uint32_t offset, unsigned size) { return get(((mc_pratt *)dev)->b, offset, size); }
static void b_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { put(((mc_pratt *)dev)->b, offset, size, value); }
static uint32_t c_read(void *dev, uint32_t offset, unsigned size) { return get(((mc_pratt *)dev)->c, offset, size); }
static void c_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { put(((mc_pratt *)dev)->c, offset, size, value); }

static uint32_t d_read(void *dev, uint32_t offset, unsigned size) { return get(((mc_pratt *)dev)->d, offset, size); }
static void d_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { put(((mc_pratt *)dev)->d, offset, size, value); }

static void pratt_event(void *dev, uint64_t now) {
    mc_pratt *p = dev;
    while (p->tick_next <= now) {
        p->tick ^= 1u;
        mc_dev_line(&p->dev, MC_PRATT_LINE_TICK, p->tick);
        p->tick_next += MC_PRATT_TICK_PERIOD / 2u;
    }
    mc_dev_schedule(&p->dev, p->tick_next);
}

static void pratt_reset(void *dev) {
    mc_pratt *p = dev;
    memset(p->a, 0, sizeof p->a);
    memset(p->b, 0, sizeof p->b);
    memset(p->c, 0, sizeof p->c);
    memset(p->d, 0, sizeof p->d);
    if (!p->tick_next) {
        p->tick_next = mc_dev_now(&p->dev) + MC_PRATT_TICK_PERIOD / 2u;
        mc_dev_schedule(&p->dev, p->tick_next);
    }
}

const mc_dev_ops mc_pratt_ops = {a_read, a_write, pratt_event, pratt_reset, NULL};
const mc_dev_ops mc_pratt_b_ops = {b_read, b_write, NULL, NULL, NULL};
const mc_dev_ops mc_pratt_c_ops = {c_read, c_write, NULL, NULL, NULL};
const mc_dev_ops mc_pratt_d_ops = {d_read, d_write, NULL, NULL, NULL};

void mc_pratt_init(mc_pratt *p, const mc_hw_host *host, const char *name, int is190) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_pratt_ops;
    p->dev.host = host;
    p->dev.name = name;
    p->is190 = (uint8_t)(is190 != 0);
}
