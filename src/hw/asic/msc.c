/* Livadix — the MSC (msc.h). Livadix's own code. */
#include "msc.h"

static uint32_t msc_read(void *dev, uint32_t offset, unsigned size) {
    mc_msc *m = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t o = offset + i;
        uint8_t b = (uint8_t)mc_rbv_ops.read(&m->rbv, o, 1);
        if ((o & 0x1Fu) == MC_RBV_DATAB) b = (uint8_t)((b & ~0x02u) | (m->rbv.datab_in & 0x02u));
        v = v << 8 | b;
    }
    return v;
}

static void msc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_msc *m = dev;
    mc_rbv_ops.write(&m->rbv, offset, size, value);
}

static void msc_event(void *dev, uint64_t now) {
    mc_msc *m = dev;
    mc_rbv_ops.event(&m->rbv, now);
}

static void msc_reset(void *dev) {
    mc_msc *m = dev;
    const uint8_t ack = m->rbv.datab_in & 0x02u;
    mc_rbv_ops.reset(&m->rbv);
    m->rbv.datab_in = (uint8_t)((m->rbv.datab_in & ~0x02u) | ack);
}

const mc_dev_ops mc_msc_ops = {msc_read, msc_write, msc_event, msc_reset, NULL};

void mc_msc_init(mc_msc *m, const mc_hw_host *host, const char *name) {
    mc_rbv_init(&m->rbv, host, name, MC_RBV_SENSE_NONE);
}

void mc_msc_set_ack(mc_msc *m, int level) { mc_rbv_set_port_b(&m->rbv, level ? 0x02u : 0, 0x02u); }
