/* Livadix — the Macintosh IIfx's OSS (oss.h). Livadix's own code. */
#include "oss.h"

#include <string.h>

/* The 60.15 Hz tick: the classic Macintosh frame of 370 lines of 704 C15M
 * periods, as the RBV's (rbv.c). */
#define TICK_PERIOD (704u * 370u)

static void update(mc_oss *o) {
    uint8_t ipl = 0;
    for (unsigned n = 0; n < MC_OSS_SOURCES; n++)
        if ((o->lines >> n & 1u) && (o->level[n] & 7u) > ipl) ipl = o->level[n] & 7u;
    if (ipl != o->ipl) {
        o->ipl = ipl;
        mc_dev_line(&o->dev, MC_OSS_LINE_IPL, ipl);
    }
}

void mc_oss_set(mc_oss *o, unsigned n, int asserted) {
    if (n >= MC_OSS_SOURCES) return;
    const uint16_t bit = (uint16_t)(1u << n);
    o->lines = (uint16_t)(asserted ? o->lines | bit : o->lines & ~bit);
    update(o);
}

static uint32_t read8(mc_oss *o, uint32_t offset) {
    if (offset < MC_OSS_SOURCES) return o->level[offset];
    if (offset == 0x202) return o->lines >> 8;
    if (offset == 0x203) return o->lines & 0xFFu;
    if (offset == 0x204) return o->control;
    if (offset == 0x205) return o->control2;
    return 0;
}

static void write8(mc_oss *o, uint32_t offset, uint8_t v) {
    if (offset < MC_OSS_SOURCES) {
        o->level[offset] = v & 7u;
        update(o);
    } else if (offset == 0x204) {
        o->control = v;
        mc_dev_line(&o->dev, MC_OSS_LINE_CACHE, (v >> 3) & 1u);
        if (v & 0x80u) mc_dev_line(&o->dev, MC_OSS_LINE_POWER_OFF, 1);
    } else if (offset == 0x205) {
        o->control2 = v & 3u;
    } else if (offset == 0x207) {
        mc_oss_set(o, MC_OSS_TICK, 0);
    }
}

static uint32_t oss_read(void *dev, uint32_t offset, unsigned size) {
    mc_oss *o = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | read8(o, (offset + i) % MC_OSS_SIZE);
    return v;
}

static void oss_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_oss *o = dev;
    for (unsigned i = 0; i < size; i++) write8(o, (offset + i) % MC_OSS_SIZE, (uint8_t)(value >> 8 * (size - 1u - i)));
}

static void oss_event(void *dev, uint64_t now) {
    mc_oss *o = dev;
    if (now >= o->tick_start + TICK_PERIOD) {
        o->tick_start += (now - o->tick_start) / TICK_PERIOD * TICK_PERIOD;
        mc_oss_set(o, MC_OSS_TICK, 1);
    }
    mc_dev_schedule(&o->dev, o->tick_start + TICK_PERIOD);
}

static void oss_reset(void *dev) {
    mc_oss *o = dev;
    memset(o->level, 0, sizeof o->level);
    o->control = o->control2 = 0;
    mc_dev_line(&o->dev, MC_OSS_LINE_CACHE, 0);
    o->lines = (uint16_t)(o->lines & ~(1u << MC_OSS_TICK));
    update(o);
    o->tick_start = mc_dev_now(&o->dev);
    mc_dev_schedule(&o->dev, o->tick_start + TICK_PERIOD);
}

const mc_dev_ops mc_oss_ops = {oss_read, oss_write, oss_event, oss_reset, NULL};

void mc_oss_init(mc_oss *o, const mc_hw_host *host, const char *name) {
    memset(o, 0, sizeof *o);
    o->dev.ops = &mc_oss_ops;
    o->dev.host = host;
    o->dev.name = name;
}
