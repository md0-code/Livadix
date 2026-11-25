/* Livadix — Platinum, the 7200's memory and video controller (platinum.h).
 *
 * Livadix's own code. */
#include "platinum.h"

#include <stdlib.h>
#include <string.h>

uint32_t mc_platinum_base(const mc_platinum *p, unsigned n) {
    return n < MC_PLATINUM_BANKS ? p->reg[(MC_PLATINUM_BANK >> 4) + n] & 0x7FE00000u : 0;
}

unsigned mc_platinum_windows(const mc_platinum *p, mc_mem_window *w, unsigned max) {
    uint32_t base[MC_PLATINUM_BANKS];
    for (unsigned i = 0; i < MC_PLATINUM_BANKS; i++) base[i] = mc_platinum_base(p, i);
    return mc_mem_bank_windows(base, p->bank_size, MC_PLATINUM_BANKS, MC_PLATINUM_BANK_WINDOW, w, max);
}

static void set_vbl(mc_platinum *p, int on) {
    if (on) p->reg[MC_PLATINUM_VBL_STATUS >> 4] |= 4u;
    else p->reg[MC_PLATINUM_VBL_STATUS >> 4] &= ~4u;
    if (on != p->vbl) {
        p->vbl = on;
        mc_dev_line(&p->dev, MC_PLATINUM_LINE_VBL, on);
    }
}

static void plat_event(void *d, uint64_t now) {
    mc_platinum *p = d;
    /* The original OF 1.0.5 quit word clears GC events until every
     * source drops ($FF815884-$FF8158C0). VBL is a bounded blanking
     * interval, rather than a level held until a Macintosh driver reads
     * the acknowledge register. Keep its phase independent of that ACK. */
    p->blank = !p->blank;
    set_vbl(p, p->blank);
    mc_dev_schedule(&p->dev, now + (p->blank ? p->frame / 50u : p->frame - p->frame / 50u));
}

void mc_platinum_reset(mc_platinum *p) {
    memset(p->reg, 0, sizeof p->reg);
    p->blank = 0;
    if (p->l2 != MC_PLATINUM_L2_NONE) p->reg[MC_PLATINUM_CACHE >> 4] = 0x20u | (uint32_t)p->l2 << 3;
    if (p->vbl) set_vbl(p, 0);
    if (p->dev.host) mc_dev_schedule(&p->dev, mc_dev_now(&p->dev) + p->frame);
}

int mc_platinum_init(mc_platinum *p, const mc_hw_host *host, const char *name, uint16_t id_low, unsigned l2, const uint32_t *bank_size, uint32_t vram_size) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_platinum_ops;
    p->dev.host = host;
    p->dev.name = name;
    p->id = (uint32_t)MC_PLATINUM_ID_VALUE << 16 | id_low;
    p->l2 = (uint8_t)(l2 > MC_PLATINUM_L2_1M ? MC_PLATINUM_L2_NONE : l2);
    if (bank_size) memcpy(p->bank_size, bank_size, sizeof p->bank_size);
    if (vram_size != (1u << 20) && vram_size != (2u << 20) && vram_size != (4u << 20)) return 0;
    p->vram = calloc(1, vram_size);
    if (!p->vram) return 0;
    p->vram_size = vram_size;
    p->frame = MC_C15M_HZ / 60u;
    mc_platinum_reset(p);
    return 1;
}

void mc_platinum_free(mc_platinum *p) {
    free(p->vram);
    p->vram = NULL;
}

void mc_platinum_set_monitor(mc_platinum *p, uint8_t sense, uint8_t sense_ext) {
    p->sense = sense;
    p->sense_ext = sense_ext;
}

/* The sense lines (2-0) as the monitor answers the lines $170 pulls low. */
static uint32_t sense_lines(const mc_platinum *p) {
    const unsigned released = p->reg[MC_PLATINUM_SENSE >> 4] & 7u, e = p->sense_ext;
    unsigned lines = p->sense & 7u;
    if (released == 7u) return lines;
    if (p->sense < 6) return lines & released;
    if (!(released & 4u)) lines = (e >> 4) & 3u;
    else if (!(released & 2u)) lines = ((e >> 3) & 1u) << 2 | ((e >> 2) & 1u);
    else lines = (e & 3u) << 1;
    return lines & released;
}

/* A register's bytes as the bus carries them (big-endian word at $10 n). */
static uint32_t plat_read(void *d, uint32_t offset, unsigned size) {
    mc_platinum *p = d;
    offset &= MC_PLATINUM_SIZE - 1u;
    if ((offset & ~15u) == MC_PLATINUM_VBL_ACK) set_vbl(p, 0);
    if ((offset & 15u) >= 4) return 0;
    uint32_t v = offset < 16 ? p->id : p->reg[offset >> 4];
    if ((offset & ~15u) == MC_PLATINUM_SENSE) v = (v & ~7u) | (~sense_lines(p) & 7u);
    if (size == 4) return v;
    const unsigned shift = 8 * (4 - size - (offset & 3u));
    return (v >> shift) & ((1u << (8 * size)) - 1u);
}

static void plat_write(void *d, uint32_t offset, unsigned size, uint32_t value) {
    mc_platinum *p = d;
    offset &= MC_PLATINUM_SIZE - 1u;
    if (offset < 16 || (offset & 15u) >= 4) return;
    const unsigned r = offset >> 4, shift = size == 4 ? 0 : 8 * (4 - size - (offset & 3u));
    const uint32_t m = (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u) << shift;
    uint32_t v = (p->reg[r] & ~m) | ((value << shift) & m);
    if (r == MC_PLATINUM_CACHE >> 4) v = (v & ~0x38u) | (p->reg[r] & 0x38u);  /* presence and size: the pins */
    p->reg[r] = v;
    if (offset >= MC_PLATINUM_BANK && offset < MC_PLATINUM_BANK + 0x10u * MC_PLATINUM_BANKS) {
        mc_dev_line(&p->dev, MC_PLATINUM_LINE_MAP, 1);
        mc_dev_line(&p->dev, MC_PLATINUM_LINE_MAP, 0);
    }
}

static void plat_reset_op(void *d) { mc_platinum_reset(d); }

const mc_dev_ops mc_platinum_ops = {plat_read, plat_write, plat_event, plat_reset_op, NULL};

/* --- VRAM: big-endian bytes, repeating through the window ------------------------- */

static uint32_t vram_read(void *d, uint32_t offset, unsigned size) {
    mc_platinum *p = d;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | p->vram[(offset + i) & (p->vram_size - 1u)];
    return v;
}

static void vram_write(void *d, uint32_t offset, unsigned size, uint32_t value) {
    mc_platinum *p = d;
    for (unsigned i = 0; i < size; i++) p->vram[(offset + i) & (p->vram_size - 1u)] = (uint8_t)(value >> (8 * (size - 1 - i)));
}

const mc_dev_ops mc_platinum_vram_ops = {vram_read, vram_write, NULL, NULL, NULL};

/* --- the picture ------------------------------------------------------------------ */

int mc_platinum_view_get(const mc_platinum *p, mc_platinum_view *v) {
    const uint32_t h_start = p->reg[0x300 >> 4] & 0xFFFu, h_end = p->reg[0x310 >> 4] & 0xFFFu;
    const uint32_t v_start = p->reg[0x370 >> 4] & 0xFFFu, v_end = p->reg[0x380 >> 4] & 0xFFFu;
    const uint32_t stride = p->reg[0x120 >> 4];
    if (h_end <= h_start || v_end <= v_start || stride <= 0x20u) return 0;
    const uint32_t width = (h_end - h_start) * 2u, height = (v_end - v_start) / 2u, bits = (stride - 0x20u) * 8u;
    if (bits % width) return 0;
    const uint32_t depth = bits / width;
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16 && depth != 32) return 0;
    const uint32_t start = (p->reg[0x100 >> 4] + 0x10u) & (p->vram_size - 1u);
    if ((uint64_t)start + (uint64_t)stride * height > p->vram_size) return 0;
    v->width = width;
    v->height = height;
    v->depth = depth;
    v->row_bytes = stride;
    v->pixels = p->vram + start;
    return 1;
}
