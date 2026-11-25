/* Livadix — BART, the PDM boards' NuBus controller (bart.h).
 *
 * Livadix's own code. */
#include "bart.h"

#include <string.h>

void mc_bart_init(mc_bart *b, const mc_hw_host *host, const char *name, uint16_t slots) {
    memset(b, 0, sizeof *b);
    b->dev.ops = &mc_bart_ops;
    b->dev.host = host;
    b->dev.name = name;
    b->id = MC_BART_ID_4;
    b->slots = slots & 0x7E00u;     /* $9-$E */
}

int mc_bart_fit(mc_bart *b, unsigned slot) {
    if (slot > 15 || !(b->slots >> slot & 1u) || b->slot[slot].fitted) return 0;
    b->slot[slot].fitted = 1;
    return 1;
}

unsigned mc_bart_free_slot(const mc_bart *b) {
    for (unsigned s = 0x9; s <= 0xE; s++)
        if ((b->slots >> s & 1u) && !b->slot[s].fitted) return s;
    return 0;
}

static mc_bart_window *add_window(mc_bart *b, unsigned slot, uint32_t offset, uint32_t size) {
    if (slot > 15 || !b->slot[slot].fitted || size == 0 || offset >= 0x01000000u || size > 0x01000000u - offset)
        return NULL;
    mc_bart_slot *s = &b->slot[slot];
    if (s->n == MC_BART_WINDOWS) return NULL;
    for (unsigned i = 0; i < s->n; i++)
        if (offset < s->w[i].offset + s->w[i].size && s->w[i].offset < offset + size) return NULL;
    mc_bart_window *w = &s->w[s->n++];
    memset(w, 0, sizeof *w);
    w->offset = offset;
    w->size = size;
    return w;
}

int mc_bart_map_memory(mc_bart *b, unsigned slot, uint32_t offset, uint32_t size, uint8_t *host, int writable) {
    mc_bart_window *w = add_window(b, slot, offset, size);
    if (!w || !host) return 0;
    w->host = host;
    w->writable = writable;
    return 1;
}

int mc_bart_map_device(mc_bart *b, unsigned slot, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops) {
    mc_bart_window *w = add_window(b, slot, offset, size);
    if (!w || !ops) return 0;
    w->dev = dev;
    w->ops = ops;
    return 1;
}

void mc_bart_irq(mc_bart *b, unsigned slot, int asserted) {
    if (slot > 15) return;
    const uint16_t before = b->asserted;
    if (asserted) b->asserted |= (uint16_t)(1u << slot);
    else b->asserted &= (uint16_t)~(1u << slot);
    if (b->asserted != before) mc_dev_line(&b->dev, MC_BART_LINE_SLOTS, b->asserted);
}

/* The window answering `address`, or NULL (a timeout). */
static mc_bart_window *decode(mc_bart *b, uint32_t address, unsigned size, uint32_t *offset) {
    if (address < 0xF1000000u || address >= 0xFF000000u) return NULL;   /* super slot space: not decoded */
    const unsigned slot = (address >> 24) & 15u;
    mc_bart_slot *s = &b->slot[slot];
    if (!(b->slots >> slot & 1u) || !s->fitted) return NULL;
    const uint32_t off = address & 0x00FFFFFFu;
    for (unsigned i = 0; i < s->n; i++) {
        mc_bart_window *w = &s->w[i];
        if (off >= w->offset && off - w->offset + size <= w->size) {
            *offset = off - w->offset;
            return w;
        }
    }
    return NULL;
}

int mc_bart_read(mc_bart *b, uint32_t address, unsigned size, uint32_t *value) {
    uint32_t off;
    mc_bart_window *w = decode(b, address, size, &off);
    if (!w) return 0;
    if (w->host) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++) v = v << 8 | w->host[off + i];
        *value = v;
    } else {
        *value = w->ops->read ? w->ops->read(w->dev, off, size) : 0;
    }
    return 1;
}

int mc_bart_write(mc_bart *b, uint32_t address, unsigned size, uint32_t value) {
    uint32_t off;
    mc_bart_window *w = decode(b, address, size, &off);
    if (!w) return 0;
    if (w->host) {
        if (w->writable)
            for (unsigned i = 0; i < size; i++) w->host[off + i] = (uint8_t)(value >> (8 * (size - 1 - i)));
    } else if (w->ops->write) {
        w->ops->write(w->dev, off, size, value);
    }
    return 1;
}

/* --- registers -------------------------------------------------------------------- */

static uint32_t bart_read(void *dev, uint32_t offset, unsigned size) {
    const mc_bart *b = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const unsigned o = (offset + i) & 0xFFu;
        const uint8_t byte = o >= 8 && o < 12 ? (uint8_t)(b->id >> (8 * (11 - o))) : b->reg[o];
        v = v << 8 | byte;
    }
    return v;
}

static void bart_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_bart *b = dev;
    for (unsigned i = 0; i < size; i++) {
        const unsigned o = (offset + i) & 0xFFu;
        if (o < 8 || o >= 12) b->reg[o] = (uint8_t)(value >> (8 * (size - 1 - i)));
    }
}

static void bart_reset(void *dev) {
    mc_bart *b = dev;
    memset(b->reg, 0, sizeof b->reg);
}

const mc_dev_ops mc_bart_ops = {bart_read, bart_write, NULL, bart_reset, NULL};
