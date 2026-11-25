/* Livadix — the PDM boards' CPU identification register (cpuid.h).
 *
 * Livadix's own code. */
#include "cpuid.h"

#include <string.h>

void mc_cpuid_init(mc_cpuid *c, const mc_hw_host *host, const char *name, uint32_t value) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_cpuid_ops;
    c->dev.host = host;
    c->dev.name = name;
    c->value = value;
    c->bus = value;
}

static uint32_t cpuid_read(void *dev, uint32_t offset, unsigned size) {
    const mc_cpuid *c = dev;
    if (size == 4) return c->bus;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | (uint8_t)(c->value >> (8u * (3u - ((offset + i) & 3u))));
    return v;
}

static void cpuid_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_cpuid *c = dev;
    (void)offset;
    if (size == 4) c->bus = value;
}

/* Reads change only with a write. */
static int cpuid_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev, (void)offset, (void)size;
    return 1;
}

const mc_dev_ops mc_cpuid_ops = {cpuid_read, cpuid_write, NULL, NULL, cpuid_stable};
