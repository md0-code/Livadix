/* Livadix — PrimeTime (see primetime.h). Livadix's own code. */
#include "primetime.h"

#include <string.h>

/* Register n answers at offset n x $100. */
static unsigned reg_of(uint32_t offset) { return (offset >> 8) % MC_PRIMETIME_REGS; }

static uint32_t primetime_read(void *dev, uint32_t offset, unsigned size) {
    const mc_primetime *p = dev;
    const uint16_t v = p->reg[reg_of(offset)];
    /* A 16-bit port: a longword is two word cycles to the same register. */
    if (size == 4) return (uint32_t)v << 16 | v;
    if (size == 2) return v;
    return (offset & 1u) ? v & 0xFFu : v >> 8;
}

static void primetime_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_primetime *p = dev;
    uint16_t *r = &p->reg[reg_of(offset)];
    if (size >= 2) *r = (uint16_t)value;     /* a longword's second cycle, its low word, stays */
    else if (offset & 1u) *r = (uint16_t)((*r & 0xFF00u) | (value & 0xFFu));
    else *r = (uint16_t)((*r & 0x00FFu) | (value & 0xFFu) << 8);
}

static void primetime_reset(void *dev) {
    mc_primetime *p = dev;
    memset(p->reg, 0, sizeof p->reg);
}

static int primetime_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}

const mc_dev_ops mc_primetime_ops = {primetime_read, primetime_write, NULL, primetime_reset, primetime_stable};

static uint32_t cpu_id_read(void *dev, uint32_t offset, unsigned size) {
    const mc_primetime *p = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | ((p->cpu_id >> (8u * (3u - ((offset + i) & 3u)))) & 0xFFu);
    return v;
}

static void cpu_id_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}

const mc_dev_ops mc_primetime_cpu_id_ops = {cpu_id_read, cpu_id_write, NULL, NULL, primetime_stable};

void mc_primetime_init(mc_primetime *p, const mc_hw_host *host, const char *name, uint16_t cpu_id) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_primetime_ops;
    p->dev.host = host;
    p->dev.name = name;
    p->cpu_id = (uint32_t)MC_PRIMETIME_CPU_ID_HIGH << 16 | cpu_id;
}
