/* Livadix — PrimeTime II (see primetime2.h). Livadix's own code. */
#include "primetime2.h"

#include <string.h>

/* Register n answers at offset n x $100 (address bits 8-12). */
static unsigned reg_of(uint32_t offset) { return (offset >> 8) % MC_PRIMETIME2_REGS; }

static uint32_t pt2_read(void *dev, uint32_t offset, unsigned size) {
    const mc_primetime2 *p = dev;
    const uint16_t v = p->reg[reg_of(offset)];
    if (size == 4) return (uint32_t)v << 16 | v;    /* two word cycles to the same register */
    if (size == 2) return v;
    return (offset & 1u) ? v & 0xFFu : v >> 8;
}

static void pt2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_primetime2 *p = dev;
    uint16_t *r = &p->reg[reg_of(offset)];
    if (size >= 2) *r = (uint16_t)value;            /* a longword's second cycle, its low word, stays */
    else if (offset & 1u) *r = (uint16_t)((*r & 0xFF00u) | (value & 0xFFu));
    else *r = (uint16_t)((*r & 0x00FFu) | (value & 0xFFu) << 8);
}

static void pt2_reset(void *dev) {
    mc_primetime2 *p = dev;
    memset(p->reg, 0, sizeof p->reg);
}

static int pt2_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}

const mc_dev_ops mc_primetime2_ops = {pt2_read, pt2_write, NULL, pt2_reset, pt2_stable};

void mc_primetime2_init(mc_primetime2 *p, const mc_hw_host *host, const char *name) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_primetime2_ops;
    p->dev.host = host;
    p->dev.name = name;
}
