/* Livadix — the stand-in for a chip that is not modelled yet (absent.h).
 * Livadix's own code. */
#include "absent.h"

#include <string.h>

static uint8_t byte_at(const mc_absent *a, uint32_t offset) {
    if (a->regs)
        for (const mc_absent_reg *r = a->regs; r->mask; r++)
            if ((offset & r->mask) == r->offset) return r->value;
    return a->fill;
}

static uint32_t absent_read(void *dev, uint32_t offset, unsigned size) {
    const mc_absent *a = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | byte_at(a, offset + i);
    return v;
}

static void absent_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}

const mc_dev_ops mc_absent_ops = {absent_read, absent_write, NULL, NULL};

void mc_absent_init(mc_absent *a, const mc_hw_host *host, const char *name, const mc_absent_reg *regs, uint8_t fill) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_absent_ops;
    a->dev.host = host;
    a->dev.name = name;
    a->regs = regs;
    a->fill = fill;
}
