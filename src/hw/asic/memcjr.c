/* Livadix — MEMCjr (see memcjr.h). Livadix's own code. */
#include "memcjr.h"

#include <string.h>

#define REG_BANK0 (0x04u / 4u)
#define REG_END (0x2Cu / 4u)
#define REG_WINDOW (0x7Cu / 4u)
#define RAM_SPACE 0x40000000u   /* RAM's share of the address space (Developer Note, "RAM Addresses") */
#define BANK_OFF 0x400u         /* bit 10 (the window's bit 4): a bank the ROM found empty */
#define BANK_UNITS 0x3FFu

static uint32_t memcjr_read(void *dev, uint32_t offset, unsigned size) {
    mc_memcjr *m = dev;
    const unsigned n = (offset >> 2) % MC_MEMCJR_REGS;
    uint32_t v;
    if (n == REG_WINDOW) v = m->window;
    else {
        v = m->reg[n];
        m->window = v >> 6 & 0x3Fu;
    }
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}

static uint32_t merge(uint32_t old, uint32_t offset, unsigned size, uint32_t value) {
    if (size == 4) return value;
    if (size == 2) return (offset & 2u) ? (old & 0xFFFF0000u) | (value & 0xFFFFu) : (old & 0xFFFFu) | value << 16;
    const unsigned shift = 8u * (3u - (offset & 3u));
    return (old & ~(0xFFu << shift)) | (value & 0xFFu) << shift;
}

static void memcjr_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_memcjr *m = dev;
    const unsigned n = (offset >> 2) % MC_MEMCJR_REGS;
    if (n == REG_WINDOW) {
        m->window = merge(m->window, offset, size, value);
        return;
    }
    const uint32_t before = m->reg[n];
    m->reg[n] = mc_memcjr_compose(m, merge(m->reg[n], offset, size, value));
    /* The bank starts and the end decode the RAM. */
    if (n >= REG_BANK0 && n <= REG_END && m->remap && m->reg[n] != before) m->remap(m->ctx);
}

static int memcjr_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 0;   /* a read moves the window */
}

const mc_dev_ops mc_memcjr_ops = {memcjr_read, memcjr_write, NULL, NULL, memcjr_stable};

uint32_t mc_memcjr_compose(const mc_memcjr *m, uint32_t low) { return (m->window & 0x3Fu) << 6 | (low & 0x3Fu); }

static uint64_t unit_address(uint32_t units) {
    const uint64_t a = (uint64_t)units * MC_MEMCJR_UNIT;
    return a > RAM_SPACE ? RAM_SPACE : a;
}

void mc_memcjr_map(const mc_memcjr *m, mc_bus *bus, uint8_t *ram) {
    mc_bus_map_float(bus, 0, RAM_SPACE);
    const uint64_t end = unit_address((m->reg[REG_END] & BANK_UNITS) + 1u);
    uint32_t host = 0;
    for (unsigned i = 0; i < MC_MEMCJR_BANKS; i++) {
        const uint32_t size = m->bank_size[i];
        const uint64_t start = unit_address(m->reg[REG_BANK0 + i] & BANK_UNITS);
        uint64_t stop = end;
        /* Up to the next bank that answers above this one. */
        for (unsigned k = i + 1; k < MC_MEMCJR_BANKS; k++) {
            const uint64_t s = unit_address(m->reg[REG_BANK0 + k] & BANK_UNITS);
            if (!(m->reg[REG_BANK0 + k] & BANK_OFF) && s > start) {
                stop = s < stop ? s : stop;
                break;
            }
        }
        if (stop > end) stop = end;
        if (size && !(m->reg[REG_BANK0 + i] & BANK_OFF) && stop > start)
            mc_bus_map_memory(bus, (uint32_t)start, (uint32_t)(stop - start), ram + host, size, 1);
        host += size;
    }
}

int mc_memcjr_layout(uint32_t ram_mb, uint32_t bank_size[MC_MEMCJR_BANKS]) {
    static const struct { uint8_t total, side1, side2; } sizes[] = {
        {4, 0, 0}, {5, 1, 0}, {6, 1, 1}, {8, 4, 0}, {12, 4, 4}, {20, 16, 0}, {36, 16, 16},
    };
    memset(bank_size, 0, MC_MEMCJR_BANKS * sizeof *bank_size);
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        if (sizes[i].total != ram_mb) continue;
        bank_size[0] = 4u << 20;
        bank_size[1] = (uint32_t)sizes[i].side1 << 20;
        bank_size[2] = (uint32_t)sizes[i].side2 << 20;
        return 1;
    }
    return 0;
}

void mc_memcjr_init(mc_memcjr *m, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_MEMCJR_BANKS],
                    void (*remap)(void *ctx), void *ctx) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_memcjr_ops;
    m->dev.host = host;
    m->dev.name = name;
    memcpy(m->bank_size, bank_size, sizeof m->bank_size);
    /* Until the ROM programs it: the banks packed from 0, as after a start. */
    uint32_t at = 0;
    for (unsigned i = 0; i < MC_MEMCJR_BANKS; i++) {
        m->reg[REG_BANK0 + i] = at / MC_MEMCJR_UNIT | (bank_size[i] ? 0 : BANK_OFF);
        at += bank_size[i];
    }
    m->reg[REG_END] = at / MC_MEMCJR_UNIT - 1u;
    m->remap = remap;
    m->ctx = ctx;
}
