/* Livadix — NuBus slots (see nubus.h for sources). Livadix's own code. */
#include "nubus.h"

#include <string.h>

static void update(mc_nubus *n) {
    if (n->pins) n->pins(n->ctx, (uint8_t)~n->asserted & n->mask, n->mask, n->asserted == 0);
}

void mc_nubus_init(mc_nubus *n, mc_bus *bus, uint8_t slots,
                   void (*pins)(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high), void *ctx) {
    memset(n, 0, sizeof *n);
    n->bus = bus;
    n->slots = slots;
    n->pins = pins;
    n->ctx = ctx;
    memset(n->line_bit, 0xFF, sizeof n->line_bit);
}

void mc_nubus_line(mc_nubus *n, unsigned slot, unsigned bit) {
    if (slot >= 16 || bit >= 8) return;
    n->line_bit[slot] = (uint8_t)bit;
    n->mask |= (uint8_t)(1u << bit);
    update(n);
}

void mc_nubus_irq_bit(mc_nubus *n, unsigned bit, int asserted) {
    if (bit >= 8) return;
    const uint8_t was = n->asserted;
    n->asserted = (uint8_t)(asserted ? n->asserted | 1u << bit : n->asserted & ~(1u << bit));
    if (n->asserted != was) update(n);
}

void mc_nubus_irq(mc_nubus *n, unsigned slot, int asserted) {
    if (slot < 16 && n->line_bit[slot] != 0xFF) mc_nubus_irq_bit(n, n->line_bit[slot], asserted);
}

void mc_nubus_claim(mc_nubus *n, unsigned slot) {
    if (slot >= 8 && slot < 16) n->fitted |= (uint8_t)(1u << (slot - 8u));
}

unsigned mc_nubus_free_slot(const mc_nubus *n) {
    for (unsigned s = MC_NUBUS_SLOT_FIRST; s <= MC_NUBUS_SLOT_LAST; s++)
        if ((n->slots >> (s - 8u) & 1u) && !(n->fitted >> (s - 8u) & 1u)) return s;
    return 0;
}

int mc_nubus_fit(mc_nubus *n, unsigned slot) {
    if (slot < MC_NUBUS_SLOT_FIRST || slot > MC_NUBUS_SLOT_LAST) return 0;
    const unsigned bit = slot - 8u;
    if (!(n->slots >> bit & 1u) || (n->fitted >> bit & 1u)) return 0;
    n->fitted |= (uint8_t)(1u << bit);
    return 1;
}

int mc_nubus_map_memory(mc_nubus *n, unsigned slot, uint32_t offset, uint32_t size, uint8_t *host, int writable) {
    if (offset >= 0x01000000u || size > 0x01000000u - offset) return 0;
    return mc_bus_map_memory(n->bus, mc_nubus_slot_base(slot) + offset, size, host, size, writable) != 0;
}

int mc_nubus_map_device(mc_nubus *n, unsigned slot, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops) {
    if (offset >= 0x01000000u || size > 0x01000000u - offset) return 0;
    const int region = mc_bus_add_io(n->bus, mc_nubus_slot_base(slot) + offset, size, size - 1u, 0);
    return region && mc_bus_attach(n->bus, region, 0, size, dev, ops, 0);
}
