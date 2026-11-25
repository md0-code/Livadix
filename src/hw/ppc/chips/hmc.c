/* Livadix — the HMC of the Power Macintosh 6100, 7100 and 8100 (hmc.h).
 *
 * Livadix's own code. */
#include "hmc.h"

#include <string.h>

#define MB (1024u * 1024u)

static uint8_t cache_code(uint32_t kb) {
    /* 64 KB << n on the original boards ($FFF032F4); the 9150's
     * 1 MB SIMM reuses code 01 (9150 Developer Note, Table 3). */
    switch (kb) {
    case 128: case 1024: return 1;
    case 256: return 2;
    case 512: return 3;
    default: return 0;
    }
}

void mc_hmc_init(mc_hmc *h, const mc_hw_host *host, const char *name, mc_hmc_board board, uint32_t board_ram,
                 const uint32_t *simm_bank, unsigned n, uint32_t cache_kb) {
    memset(h, 0, sizeof *h);
    h->dev.ops = &mc_hmc_ops;
    h->dev.host = host;
    h->dev.name = name;
    h->cache_code = cache_code(cache_kb);
    /* The board's RAM: an 8 MB window at 0 ($FFF03060: sized from $0080 0000
     * down in 2 MB steps). */
    h->bank[0].window = 0;
    h->bank[0].window_size = 8u * MB;
    h->bank[0].size = board_ram > 8u * MB ? 8u * MB : board_ram;
    h->board = board;
    unsigned simms = board == MC_HMC_BOARD_6100 ? 2u : 8u;
    for (unsigned i = 0; i < simms; i++) {
        mc_hmc_bank *b = &h->bank[1 + i];
        if (board == MC_HMC_BOARD_6100) {
            /* Bank B at $1000 0000, bank C at $0800 0000, 128 MB each
             * ($FFF03078-$FFF030A0). */
            b->window = i == 0 ? 0x10000000u : 0x08000000u;
            b->window_size = 128u * MB;
        } else {
            /* 16 MB into each 64 MB, 32 MB long ($FFF030DC-$FFF03194;
             * Enhanced Developer Note, Table A-2). */
            b->window = 0x01000000u + i * 0x04000000u;
            b->window_size = 32u * MB;
        }
        uint32_t s = i < n && simm_bank ? simm_bank[i] : 0;
        if (s > b->window_size) s = b->window_size;
        b->size = s;
    }
    uint32_t offset = 0;
    for (unsigned i = 0; i < MC_HMC_BANKS; i++) {
        h->bank[i].offset = offset;
        offset += h->bank[i].size;
    }
    h->ram_size = offset;
}

/* The 6100's banks once the ROM has written their size code (bits 30-29,
 * nonzero for its 2, 8 and 32 MB banks): bank B right after the board's
 * RAM and bank C after it, the addresses the ROM rewrites into its table
 * ($FFF03D04-$FFF03D18) and its nanokernel then uses; before, the sizing
 * windows†. */
static void layout(mc_hmc *h) {
    if (h->board != MC_HMC_BOARD_6100) return;
    mc_hmc_bank *b = &h->bank[1], *c = &h->bank[2];
    if ((h->config >> 29) & 3u) {
        b->window = h->bank[0].size;
        b->window_size = b->size ? b->size : 2u * MB;
        c->window = b->window + b->window_size;
        c->window_size = c->size ? c->size : 2u * MB;
    } else {
        b->window = 0x10000000u;
        c->window = 0x08000000u;
        b->window_size = c->window_size = 128u * MB;
    }
}

void mc_hmc_set_remap(mc_hmc *h, void (*remap)(void *ctx), void *ctx) {
    h->remap = remap;
    h->remap_ctx = ctx;
}

int64_t mc_hmc_ram_offset(const mc_hmc *h, uint32_t address) {
    for (unsigned i = 0; i < MC_HMC_BANKS; i++) {
        const mc_hmc_bank *b = &h->bank[i];
        if (!b->window_size || address < b->window || address - b->window >= b->window_size) continue;
        if (!b->size) return -1;
        return (int64_t)b->offset + (address - b->window) % b->size;
    }
    return -1;
}

int64_t mc_hmc_rom_offset(uint32_t address) {
    if ((address >= 0x40000000u && address < 0x50000000u) || address >= 0xFF000000u)
        return (int64_t)(address & (MC_HMC_ROM_SIZE - 1u));
    return -1;
}

uint64_t mc_hmc_config(const mc_hmc *h) {
    return (h->config & ~(uint64_t)3u) | h->cache_code;
}

/* --- registers -------------------------------------------------------------------- */

static uint32_t hmc_read(void *dev, uint32_t offset, unsigned size) {
    mc_hmc *h = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t bit = 0;
        if (((offset + i) & 0x8u) == 0) {
            bit = (uint32_t)(mc_hmc_config(h) >> (h->bit % MC_HMC_BITS)) & 1u;
            h->bit = (h->bit + 1u) % MC_HMC_BITS;
        }
        v = v << 8 | bit;
    }
    return v;
}

static void hmc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_hmc *h = dev;
    for (unsigned i = 0; i < size; i++) {
        const uint8_t b = (uint8_t)(value >> (8 * (size - 1 - i)));
        if ((offset + i) & 0x8u) {
            h->bit = 0;
        } else {
            const uint64_t m = (uint64_t)1 << (h->bit % MC_HMC_BITS);
            const unsigned code = (unsigned)(h->config >> 29) & 3u;
            h->config = (b & 1u) ? h->config | m : h->config & ~m;
            h->bit = (h->bit + 1u) % MC_HMC_BITS;
            if (((unsigned)(h->config >> 29) & 3u) != code && h->board == MC_HMC_BOARD_6100) {
                layout(h);
                if (h->remap) h->remap(h->remap_ctx);
            }
        }
    }
}

static void hmc_reset(void *dev) {
    mc_hmc *h = dev;
    h->config = 0;
    h->bit = 0;
    layout(h);
    if (h->remap) h->remap(h->remap_ctx);
}

const mc_dev_ops mc_hmc_ops = {hmc_read, hmc_write, NULL, hmc_reset, NULL};
