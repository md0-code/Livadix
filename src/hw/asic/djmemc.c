/* Livadix — DJMEMC (see djmemc.h). Livadix's own code. */
#include "djmemc.h"

#include <string.h>

#define REG_CONFIG 0x00u
#define REG_BANK0 0x04u
#define REG_END 0x2Cu
#define RAM_SPACE 0x40000000u   /* RAM's share of the address space (Developer Note, Figure 2-4) */

static uint32_t djmemc_read(void *dev, uint32_t offset, unsigned size) {
    const mc_djmemc *m = dev;
    const uint32_t v = m->reg[(offset >> 2) % MC_DJMEMC_REGS];
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}

static void djmemc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_djmemc *m = dev;
    const unsigned n = (offset >> 2) % MC_DJMEMC_REGS;
    uint32_t *r = &m->reg[n];
    const uint32_t before = *r;
    if (size == 4) *r = value;
    else if (size == 2) *r = (offset & 2u) ? (*r & 0xFFFF0000u) | (value & 0xFFFFu) : (*r & 0xFFFFu) | value << 16;
    else {
        const unsigned shift = 8u * (3u - (offset & 3u));
        *r = (*r & ~(0xFFu << shift)) | (value & 0xFFu) << shift;
    }
    /* The bank starts and the end decode the RAM. */
    if (n >= REG_BANK0 / 4u && n <= REG_END / 4u && *r != before && m->remap) m->remap(m->ctx);
}

const mc_dev_ops mc_djmemc_ops = {djmemc_read, djmemc_write, NULL, NULL};

/* A register's address in 4 MB units, as a byte address clipped to the
 * RAM space. */
static uint64_t unit_address(uint32_t units) {
    const uint64_t a = (uint64_t)units * MC_DJMEMC_UNIT;
    return a > RAM_SPACE ? RAM_SPACE : a;
}

void mc_djmemc_map(const mc_djmemc *m, mc_bus *bus, uint8_t *ram) {
    mc_bus_map_float(bus, 0, RAM_SPACE);
    const uint64_t end = unit_address(m->reg[REG_END / 4u] == 0xFFFFFFFFu ? 0xFFFFFFFFu : m->reg[REG_END / 4u] & 0xFFu);
    uint32_t host = 0;
    for (unsigned i = 0; i < MC_DJMEMC_BANKS; i++) {
        const uint32_t size = m->bank_size[i];
        const uint64_t start = unit_address(m->reg[REG_BANK0 / 4u + i] & 0xFFu);
        uint64_t stop = i + 1 < MC_DJMEMC_BANKS ? unit_address(m->reg[REG_BANK0 / 4u + i + 1] & 0xFFu) : end;
        if (stop > end) stop = end;
        if (size && stop > start) mc_bus_map_memory(bus, (uint32_t)start, (uint32_t)(stop - start), ram + host, size, 1);
        host += size;
    }
}

int mc_djmemc_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_DJMEMC_BANKS]) {
    memset(bank_size, 0, MC_DJMEMC_BANKS * sizeof *bank_size);
    if (ram_mb < onboard_mb) onboard_mb = ram_mb;
    if (onboard_mb != 4 && onboard_mb != 8) return 0;
    bank_size[0] = 4u << 20;
    if (onboard_mb == 8) bank_size[1] = 4u << 20;
    uint32_t left = ram_mb - onboard_mb;
    for (unsigned i = 2; i < MC_DJMEMC_BANKS && i < 2 + simm_banks && left; i++) {
        const uint32_t mb = left >= 16 ? 16 : 4;
        if (left < mb) return 0;
        bank_size[i] = mb << 20;
        left -= mb;
    }
    return left == 0;
}

void mc_djmemc_init(mc_djmemc *m, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_DJMEMC_BANKS],
                    void (*remap)(void *ctx), void *ctx) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_djmemc_ops;
    m->dev.host = host;
    m->dev.name = name;
    memcpy(m->bank_size, bank_size, sizeof m->bank_size);
    /* Until the ROM programs it: the banks packed from 0, as after a start. */
    uint32_t at = 0;
    for (unsigned i = 0; i < MC_DJMEMC_BANKS; i++) {
        m->reg[REG_BANK0 / 4u + i] = at / MC_DJMEMC_UNIT;
        at += bank_size[i];
    }
    m->reg[REG_END / 4u] = at / MC_DJMEMC_UNIT;
    m->remap = remap;
    m->ctx = ctx;
}
