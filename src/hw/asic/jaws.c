/* Livadix — Jaws (jaws.h). Livadix's own code. */
#include "jaws.h"

#include <string.h>


static unsigned reg_of(uint32_t offset) { return (offset >> 13) & (MC_JAWS_NREG - 1u); }

static uint32_t jaws_read(void *dev, uint32_t offset, unsigned size) {
    mc_jaws *j = dev;
    (void)size;
    return j->reg[reg_of(offset)];
}

static void jaws_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_jaws *j = dev;
    (void)size;
    j->reg[reg_of(offset)] = (uint8_t)value;
}

static void jaws_event(void *dev, uint64_t now) {
    mc_jaws *j = dev;
    while (j->tick_next <= now) {
        j->tick ^= 1u;
        mc_dev_line(&j->dev, MC_JAWS_LINE_TICK, j->tick);
        j->tick_next += MC_JAWS_TICK_PERIOD / 2u;
    }
    mc_dev_schedule(&j->dev, j->tick_next);
}

static void jaws_reset(void *dev) {
    mc_jaws *j = dev;
    memset(j->reg, 0, sizeof j->reg);
    if (!j->tick_next) {
        j->tick_next = mc_dev_now(&j->dev) + MC_JAWS_TICK_PERIOD / 2u;
        mc_dev_schedule(&j->dev, j->tick_next);
    }
}

const mc_dev_ops mc_jaws_ops = {jaws_read, jaws_write, jaws_event, jaws_reset, NULL};

void mc_jaws_init(mc_jaws *j, const mc_hw_host *host, const char *name) {
    memset(j, 0, sizeof *j);
    j->dev.ops = &mc_jaws_ops;
    j->dev.host = host;
    j->dev.name = name;
}
