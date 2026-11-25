/* Livadix - PSX memory decoder (psx.h). */
#include "psx.h"
#include <string.h>
#define MB (1u << 20)

int mc_psx_population(uint32_t bytes, int plus, uint32_t banks[MC_PSX_BANKS]) {
    static const unsigned dimms[] = {64, 32, 16, 8, 4, 0};
    const uint32_t onboard = plus ? 0 : 8u * MB;
    memset(banks, 0, MC_PSX_BANKS * sizeof *banks);
    if (bytes < onboard || bytes > onboard + 128u * MB) return 0;
    for (unsigned first = 0; first < sizeof dimms / sizeof *dimms; first++) {
        for (unsigned second = first; second < sizeof dimms / sizeof *dimms; second++) {
            if (onboard + (dimms[first] + dimms[second]) * MB != bytes) continue;
            banks[0] = onboard;
            banks[1] = (dimms[first] > 32 ? 32u : dimms[first]) * MB;
            banks[2] = dimms[first] == 64 ? 32u * MB : 0;
            banks[3] = (dimms[second] > 32 ? 32u : dimms[second]) * MB;
            banks[4] = dimms[second] == 64 ? 32u * MB : 0;
            return 1;
        }
    }
    return 0;
}

unsigned mc_psx_windows(const mc_psx *p, mc_mem_window *windows, unsigned capacity) {
    uint32_t base[MC_PSX_BANKS], used[MC_PSX_BANKS] = {0};
    uint32_t total = 0;
    for (unsigned bank = 0; bank < MC_PSX_BANKS; bank++) {
        base[bank] = total;
        total += p->bank_size[bank];
    }
    unsigned count = 0;
    for (unsigned segment = 0; segment < MC_PSX_SEGMENTS; segment++) {
        const unsigned reg = 0x40 / 4 + (segment / 8u) * 2u;
        const unsigned bank = (p->reg[reg] >> (4u * (segment & 7u))) & 15u;
        if (bank >= MC_PSX_BANKS || !p->bank_size[bank] || !(p->reg[0x28 / 4] & (1u << (16u + bank)))) continue;
        const uint32_t offset = base[bank] + (used[bank]++ * MC_PSX_SEGMENT) % p->bank_size[bank];
        if (count < capacity) windows[count] = (mc_mem_window){segment * MC_PSX_SEGMENT, MC_PSX_SEGMENT,
                                                              offset, MC_PSX_SEGMENT, bank};
        count++;
    }
    return count;
}

static uint32_t read(void *dev, uint32_t offset, unsigned size) {
    const mc_psx *p = dev;
    offset &= MC_PSX_SIZE - 1u;
    const unsigned shift = 8u * (4u - size - (offset & 3u));
    return p->reg[offset / 4] >> shift & (UINT32_MAX >> (32u - 8u * size));
}

static void write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_psx *p = dev;
    offset &= MC_PSX_SIZE - 1u;
    const unsigned shift = 8u * (4u - size - (offset & 3u));
    const uint32_t mask = (UINT32_MAX >> (32u - 8u * size)) << shift;
    uint32_t *reg = &p->reg[offset / 4];
    const uint32_t old = *reg;
    *reg = (*reg & ~mask) | ((value << shift) & mask);
    if (*reg != old && ((offset & ~3u) == 0x28 || (offset >= 0x40 && offset < 0x64)))
        mc_dev_line(&p->dev, MC_PSX_LINE_MAP, 1);
}

static void reset(void *dev) {
    mc_psx *p = dev;
    memset(p->reg, 0, sizeof p->reg);
    p->reg[0x10 / 4] = p->strap;
    p->reg[0x28 / 4] = 0x1F0000;
    mc_dev_line(&p->dev, MC_PSX_LINE_MAP, 1);
}

void mc_psx_init(mc_psx *p, const mc_hw_host *host, const uint32_t *banks, uint32_t strap) {
    memset(p, 0, sizeof *p);
    p->dev = (mc_dev){&mc_psx_ops, host, "PSX memory controller"};
    memcpy(p->bank_size, banks, sizeof p->bank_size);
    p->strap = strap;
    reset(p);
}

const mc_dev_ops mc_psx_ops = {read, write, NULL, reset, NULL};
