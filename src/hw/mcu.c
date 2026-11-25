/* Livadix — the Quadra 700/900 MCU (see mcu.h). Livadix's own code. */
#include "mcu.h"

#include <string.h>

#define BANK_A (4u << 20)

static uint32_t mcu_read(void *dev, uint32_t offset, unsigned size) {
    const mc_mcu *m = dev;
    const uint32_t v = m->reg[(offset >> 2) % MC_MCU_REGS];
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}

static void mcu_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_mcu *m = dev;
    uint32_t *r = &m->reg[(offset >> 2) % MC_MCU_REGS];
    if (size == 4) *r = value;
    else if (size == 2) *r = (offset & 2u) ? (*r & 0xFFFF0000u) | (value & 0xFFFFu) : (*r & 0xFFFFu) | value << 16;
    else {
        const unsigned shift = 8u * (3u - (offset & 3u));
        *r = (*r & ~(0xFFu << shift)) | (value & 0xFFu) << shift;
    }
    /* A write to a strobe register latches the serial bits. */
    if ((offset & ~3u) == 0xA0u) {
        uint32_t base = 0, start[3] = {0, 0, 0};
        for (unsigned i = 0; i < 6; i++) base |= (m->reg[i] & 1u) << i;
        for (unsigned k = 0; k < 3; k++)
            for (unsigned i = 0; i < 6; i++) start[k] |= (m->reg[6 * k + i] & 1u) << i;
        const int changed = base != m->bank_b || (m->banks[0] && memcmp(start, m->start, sizeof start));
        m->bank_b = base;
        memcpy(m->start, start, sizeof start);
        if (changed && m->remap) m->remap(m->ctx);
    }
}

const mc_dev_ops mc_mcu_ops = {mcu_read, mcu_write, NULL, NULL};

#define WINDOW (64u << 20)
#define BANK_UNIT (4u << 20)

/* Four banks: each answers from its start through the 64 MB above it, or
 * up to the next bank's start, repeating its contents; bank A starts at 0,
 * a bank whose start is 0 is off. */
static void map_banks(const mc_mcu *m, mc_bus *bus, uint8_t *ram) {
    uint32_t base[4] = {0, m->start[0] * BANK_UNIT, m->start[1] * BANK_UNIT, m->start[2] * BANK_UNIT};
    uint32_t offset = 0;
    mc_bus_map_float(bus, 0, 0x40000000u);
    for (unsigned k = 0; k < 4; k++) {
        const uint32_t size = m->banks[k];
        if (size && (k == 0 || base[k])) {
            uint32_t window = WINDOW;
            for (unsigned j = 1; j < 4; j++)
                if (j != k && base[j] > base[k] && base[j] - base[k] < window) window = base[j] - base[k];
            mc_bus_map_memory(bus, base[k], window, ram + offset, size, 1);
        }
        offset += size;
    }
}

void mc_mcu_set_banks(mc_mcu *m, const uint32_t sizes[4]) {
    memcpy(m->banks, sizes, sizeof m->banks);
    for (unsigned k = 0; k < 3; k++) m->start[k] = 16u * (k + 1u);   /* until the ROM programs them */
}

void mc_mcu_map(const mc_mcu *m, mc_bus *bus, uint8_t *ram, uint32_t ram_size) {
    if (m->banks[0]) {
        map_banks(m, bus, ram);
        return;
    }
    /* Bank 1 (soldered) answers from 0 up to bank 2's start, repeating its
     * 4 MB; bank 2 through the 64 MB above its start, repeating its contents;
     * an empty bank and the space above float (reads 0, writes are lost). */
    const uint32_t b = m->bank_b * BANK_UNIT;
    mc_bus_map_float(bus, 0, 0x40000000u);
    mc_bus_map_memory(bus, 0, b ? b : WINDOW, ram, BANK_A, 1);
    if (!b) return;
    if (ram_size > BANK_A) mc_bus_map_memory(bus, b, WINDOW, ram + BANK_A, ram_size - BANK_A, 1);
}

void mc_mcu_init(mc_mcu *m, const mc_hw_host *host, const char *name, uint32_t ram_size, void (*remap)(void *ctx), void *ctx) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_mcu_ops;
    m->dev.host = host;
    m->dev.name = name;
    m->ram_size = ram_size;
    m->bank_b = 16;     /* until the ROM programs it: bank 2 at 64 MB */
    m->remap = remap;
    m->ctx = ctx;
}
