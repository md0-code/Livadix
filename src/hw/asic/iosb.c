/* Livadix — IOSB (see iosb.h). Livadix's own code. */
#include "iosb.h"

#include <string.h>

/* Register n answers at offset n x $200 (address bits 9-12). */
static unsigned reg_of(uint32_t offset) { return (offset >> 9) % MC_IOSB_REGS; }

static uint32_t iosb_read(void *dev, uint32_t offset, unsigned size) {
    const mc_iosb *s = dev;
    const uint16_t v = s->reg[reg_of(offset)];
    /* A 16-bit port: a longword is two word cycles to the same register. */
    if (size == 4) return (uint32_t)v << 16 | v;
    if (size == 2) return v;
    return (offset & 1u) ? v & 0xFFu : v >> 8;
}

static void iosb_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_iosb *s = dev;
    uint16_t *r = &s->reg[reg_of(offset)];
    if (size >= 2) *r = (uint16_t)value;     /* a longword's second cycle, its low word, stays */
    else if (offset & 1u) *r = (uint16_t)((*r & 0xFF00u) | (value & 0xFFu));
    else *r = (uint16_t)((*r & 0x00FFu) | (value & 0xFFu) << 8);
}

static void iosb_reset(void *dev) {
    mc_iosb *s = dev;
    memset(s->reg, 0, sizeof s->reg);
}

static int iosb_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}

const mc_dev_ops mc_iosb_ops = {iosb_read, iosb_write, NULL, iosb_reset, iosb_stable};

static uint32_t cpu_id_read(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | ((MC_IOSB_CPU_ID >> (8u * (3u - ((offset + i) & 3u)))) & 0xFFu);
    return v;
}

static void cpu_id_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}

const mc_dev_ops mc_iosb_cpu_id_ops = {cpu_id_read, cpu_id_write, NULL, NULL, iosb_stable};

void mc_iosb_init(mc_iosb *s, const mc_hw_host *host, const char *name) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_iosb_ops;
    s->dev.host = host;
    s->dev.name = name;
}
