/* Livadix — Grand Central's sound interface (awacs.h).
 *
 * Livadix's own code. */
#include "awacs.h"

#include <string.h>

#define TICK (MC_C15M_HZ / 1000u)

/* 44,100 Hz divided by 1, 1.5, 2, 2.5, 3, 4, 5, 6 (x 2 to stay whole)†. */
static const uint32_t rate_div2[8] = {2, 3, 4, 5, 6, 8, 10, 12};

uint32_t mc_awacs_rate(const mc_awacs *a) {
    return 88200u / rate_div2[(a->control >> 8) & 7u];
}

static void pulse(mc_awacs *a, int line) {
    mc_dev_line(&a->dev, line, 1);
    mc_dev_line(&a->dev, line, 0);
}

/* The frames due since the last tick leave the FIFO for the host. */
static void play(mc_awacs *a, uint64_t now) {
    const uint64_t elapsed = now > a->last ? now - a->last : 0;
    a->last = now;
    a->frac += elapsed * mc_awacs_rate(a);
    unsigned frames = (unsigned)(a->frac / MC_C15M_HZ);
    a->frac %= MC_C15M_HZ;
    for (unsigned f = 0; f < frames; f++) {
        int16_t l = 0, r = 0;
        if (a->out_n >= 4) {
            const uint8_t *p = a->out;
            if (a->swap & 1u) { l = (int16_t)(p[0] | p[1] << 8); r = (int16_t)(p[2] | p[3] << 8); }
            else { l = (int16_t)(p[0] << 8 | p[1]); r = (int16_t)(p[2] << 8 | p[3]); }
            a->out_n -= 4;
            memmove(a->out, a->out + 4, a->out_n);
        }
        const unsigned at = (a->ring_head + a->ring_n) % MC_AWACS_RING;
        a->ring[2 * at] = l;
        a->ring[2 * at + 1] = r;
        if (a->ring_n < MC_AWACS_RING) a->ring_n++;
        else a->ring_head = (a->ring_head + 1) % MC_AWACS_RING;
    }
    a->in_n += frames * 4u;
    if (a->in_n > MC_AWACS_FIFO) a->in_n = MC_AWACS_FIFO;
}

static void awacs_event(void *p, uint64_t now) {
    mc_awacs *a = p;
    play(a, now);
    if (a->out_n < MC_AWACS_FIFO) pulse(a, MC_AWACS_LINE_OUT_DRQ);
    if (a->in_n) pulse(a, MC_AWACS_LINE_IN_DRQ);
    mc_dev_schedule(&a->dev, now + TICK);
}

void mc_awacs_reset(mc_awacs *a) {
    a->control = a->word = a->swap = 0;
    a->busy_until = 0;
    a->out_n = a->in_n = 0;
    a->played = 0;
    a->ring_head = a->ring_n = 0;
    a->frac = 0;
    if (a->dev.host) {
        a->last = mc_dev_now(&a->dev);
        mc_dev_schedule(&a->dev, a->last + TICK);
    }
}

void mc_awacs_init(mc_awacs *a, const mc_hw_host *host, const char *name, const mc_awacs_codec *codec) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_awacs_ops;
    a->dev.host = host;
    a->dev.name = name;
    if (codec) a->codec = *codec;
    mc_awacs_reset(a);
}

static uint32_t awacs_read(void *p, uint32_t offset, unsigned size) {
    mc_awacs *a = p;
    uint32_t v;
    switch (offset & 0xFF0u) {
    case MC_AWACS_CONTROL: v = a->control; break;
    case MC_AWACS_CODEC:
        v = a->word & 0x00FFFFFFu;
        if (mc_dev_now(&a->dev) < a->busy_until) v |= MC_AWACS_BUSY;
        break;
    case MC_AWACS_STATUS: v = a->codec.status ? a->codec.status(a->codec.ctx) : 0; break;
    case MC_AWACS_SWAP: v = a->swap; break;
    default: v = 0; break;                  /* the clipping count: nothing clips† */
    }
    v >>= 8 * (offset & 3u);
    return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
}

static void awacs_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_awacs *a = p;
    if (size < 4) value = (value & ((1u << (8 * size)) - 1u)) << (8 * (offset & 3u));
    switch (offset & 0xFF0u) {
    case MC_AWACS_CONTROL:
        play(a, mc_dev_now(&a->dev));       /* frames so far at the old rate */
        a->control = value;
        break;
    case MC_AWACS_CODEC:
        a->word = value & 0x00FFFFFFu;
        a->busy_until = mc_dev_now(&a->dev) + MC_C15M_HZ / mc_awacs_rate(a) + 1;
        if (a->codec.control) a->codec.control(a->codec.ctx, a->word);
        break;
    case MC_AWACS_SWAP: a->swap = value; break;
    default: break;
    }
}

int mc_awacs_dma_write(void *p, uint8_t byte) {
    mc_awacs *a = p;
    if (a->out_n >= MC_AWACS_FIFO) return 0;
    a->out[a->out_n++] = byte;
    a->played = 1;
    return 1;
}

int mc_awacs_dma_read(void *p, uint8_t *byte) {
    mc_awacs *a = p;
    if (!a->in_n) return 0;                 /* MC_DBDMA_NONE */
    a->in_n--;
    *byte = 0;                              /* no sound input: silence */
    return 1;                               /* MC_DBDMA_BYTE */
}

size_t mc_awacs_pull(mc_awacs *a, int16_t *stereo, size_t frames) {
    size_t n = 0;
    while (n < frames && a->ring_n) {
        stereo[2 * n] = a->ring[2 * a->ring_head];
        stereo[2 * n + 1] = a->ring[2 * a->ring_head + 1];
        a->ring_head = (a->ring_head + 1) % MC_AWACS_RING;
        a->ring_n--;
        n++;
    }
    return n;
}

static void awacs_reset_op(void *p) { mc_awacs_reset(p); }

const mc_dev_ops mc_awacs_ops = {awacs_read, awacs_write, awacs_event, awacs_reset_op, NULL};
