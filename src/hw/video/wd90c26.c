/* Livadix — the PowerBook 165c/180c color LCD video (wd90c26.h). Livadix's own
 * code. */
#include "wd90c26.h"

#include <string.h>

/* A frame of the panel at 60.15 Hz, its last 1/20 the
 * vertical retrace that bit 3 of input status 1 shows†. */
#define FRAME (704u * 370u)
#define RETRACE (FRAME / 20u)

static uint8_t status1(mc_wd90c26 *w) {
    const uint64_t t = (mc_dev_now(&w->dev) - w->frame_start) % FRAME;
    /* bit 3 the vertical retrace, bit 0 the display disabled (in it) */
    return (uint8_t)(t >= FRAME - RETRACE ? 0x09u : 0x00u);
}

static uint8_t port_read(mc_wd90c26 *w, unsigned port) {
    switch (port) {
    case 0x3C0: return w->attr_index;
    case 0x3C1: return w->attr[w->attr_index & 0x1Fu];
    case 0x3C4: return w->seq_index;
    case 0x3C5: return w->seq[w->seq_index & 0x1Fu];
    case 0x3C6: return w->dac_mask;
    case 0x3C7: return w->dac_read;
    case 0x3C8: return w->dac_write;
    case 0x3C9: {
        const uint8_t v = w->dac[w->dac_read][w->dac_step];
        if (++w->dac_step == 3) {
            w->dac_step = 0;
            w->dac_read++;
        }
        return v;
    }
    case 0x3CA: return w->feature;
    case 0x3CC: return w->misc;
    case 0x3CE: return w->gc_index;
    case 0x3CF: return w->gc[w->gc_index & 0x3Fu];
    case 0x3B4: case 0x3D4: return w->crtc_index;
    case 0x3B5: case 0x3D5:
        /* CRTC $2A bit 7: the panel's type, 1 for the 640 x 400 panel (the
         * PrimaryInit at ROM offset $7AA picks sResource $80 or $81 from
         * it)†. */
        if ((w->crtc_index & 0x7Fu) == 0x2A) return (uint8_t)((w->crtc[0x2A] & 0x7Fu) | (w->height == 400 ? 0x80u : 0));
        return w->crtc[w->crtc_index & 0x7Fu];
    case 0x3BA: case 0x3DA:
        w->attr_flip = 0;
        return status1(w);
    default: return w->other[port & 0x0Fu];
    }
}

static void port_write(mc_wd90c26 *w, unsigned port, uint8_t v) {
    switch (port) {
    case 0x3C0:
        if (!w->attr_flip) w->attr_index = v;
        else w->attr[w->attr_index & 0x1Fu] = v;
        w->attr_flip ^= 1u;
        break;
    case 0x3C2: w->misc = v; break;
    case 0x3C4: w->seq_index = v; break;
    case 0x3C5: w->seq[w->seq_index & 0x1Fu] = v; break;
    case 0x3C6: w->dac_mask = v; break;
    case 0x3C7: w->dac_read = v; w->dac_step = 0; break;
    case 0x3C8: w->dac_write = v; w->dac_step = 0; break;
    case 0x3C9:
        w->dac[w->dac_write][w->dac_step] = (uint8_t)(v & 0x3Fu);
        if (++w->dac_step == 3) {
            w->dac_step = 0;
            w->dac_write++;
        }
        break;
    case 0x3CE: w->gc_index = v; break;
    case 0x3CF: w->gc[w->gc_index & 0x3Fu] = v; break;
    case 0x3B4: case 0x3D4: w->crtc_index = v; break;
    case 0x3B5: case 0x3D5: w->crtc[w->crtc_index & 0x7Fu] = v; break;
    case 0x3BA: case 0x3DA: w->feature = v; break;
    default: w->other[port & 0x0Fu] = v; break;
    }
}

static uint32_t wd_read(void *dev, uint32_t offset, unsigned size) {
    mc_wd90c26 *w = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i;
        uint8_t b = 0xFF;
        if (a >= MC_WD_PORTS && a < MC_WD_PORTS + 0x10000u) b = port_read(w, (unsigned)(a - MC_WD_PORTS));
        else if ((a & ~1u) == MC_WD_PANGOLA) b = (uint8_t)(w->pangola >> ((a & 1u) ? 0 : 8));
        v = v << 8 | b;
    }
    return v;
}

static void wd_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_wd90c26 *w = dev;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i;
        const uint8_t b = (uint8_t)(value >> (8 * (size - 1 - i)));
        if (a >= MC_WD_PORTS && a < MC_WD_PORTS + 0x10000u) {
            port_write(w, (unsigned)(a - MC_WD_PORTS), b);
        } else if ((a & ~1u) == MC_WD_PANGOLA) {
            if (a & 1u) w->pangola = (uint16_t)((w->pangola & 0xFF00u) | b);
            else w->pangola = (uint16_t)((w->pangola & 0x00FFu) | b << 8);
        }
    }
}

static void wd_reset(void *dev) {
    mc_wd90c26 *w = dev;
    w->frame_start = mc_dev_now(&w->dev);
    w->dac_mask = 0xFF;
}

const mc_dev_ops mc_wd90c26_ops = {wd_read, wd_write, NULL, wd_reset, NULL};

void mc_wd90c26_init(mc_wd90c26 *w, const mc_hw_host *host, const char *name, uint8_t *vram, uint32_t height) {
    memset(w, 0, sizeof *w);
    w->dev.ops = &mc_wd90c26_ops;
    w->dev.host = host;
    w->dev.name = name;
    w->vram = vram;
    w->height = height;
    w->dac_mask = 0xFF;
}

int mc_wd90c26_view_get(mc_wd90c26 *w, mc_wd90c26_view *v) {
    if (!(w->pangola & MC_PANGOLA_ON)) return 0;
    const uint32_t depth = 8u >> ((w->pangola >> 5) & 3u);
    for (unsigned i = 0; i < 256; i++) {
        const uint32_t r = w->dac[i][0] * 255u / 63u, g = w->dac[i][1] * 255u / 63u, b = w->dac[i][2] * 255u / 63u;
        w->palette[i] = 0xFF000000u | r << 16 | g << 8 | b;
    }
    v->width = 640;
    v->height = w->height;
    v->depth = depth;
    v->row_bytes = 640u * depth / 8u;
    v->pixels = w->vram + MC_WD_SCREEN;
    v->palette = w->palette;
    return 1;
}
