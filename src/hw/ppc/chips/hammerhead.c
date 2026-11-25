/* Livadix — Hammerhead, the memory controller (hammerhead.h).
 *
 * Livadix's own code. */
#include "hammerhead.h"

#include <string.h>

unsigned mc_mem_bank_windows(const uint32_t *base, const uint32_t *size, unsigned n, uint32_t limit, mc_mem_window *w, unsigned max) {
    uint32_t offset[32];
    unsigned order[32], k = 0;
    uint32_t at = 0;
    if (n > 32) n = 32;
    for (unsigned i = 0; i < n; i++) {
        offset[i] = at;
        at += size[i];
        if (!size[i]) continue;
        unsigned j = k++;
        while (j && (base[order[j - 1]] > base[i])) { order[j] = order[j - 1]; j--; }
        order[j] = i;
    }
    unsigned count = 0;
    for (unsigned j = 0; j < k && count < max; j++) {
        const unsigned b = order[j];
        if (j && base[order[j - 1]] == base[b]) continue;      /* the lower-numbered bank answers */
        uint64_t end = (uint64_t)base[b] + limit;
        for (unsigned q = j + 1; q < k; q++)
            if (base[order[q]] > base[b]) { if (base[order[q]] < end) end = base[order[q]]; break; }
        if (end > 0x100000000ull) end = 0x100000000ull;
        w[count++] = (mc_mem_window){base[b], (uint32_t)(end - base[b]), offset[b], size[b], b};
    }
    return count;
}

uint32_t mc_hammerhead_base(const mc_hammerhead *h, unsigned n) {
    if (n >= MC_HAMMERHEAD_BANKS) return 0;
    const unsigned r = (MC_HAMMERHEAD_BANK >> 4) + 2 * n;
    return ((uint32_t)(h->reg[r] & 1u) << 8 | h->reg[r + 1]) << 22;
}

/* Adjacent equal DIMMs form a 128-bit array (9500 Developer Note,
 * "Data Interleaving"). POST sets bit 2 on the even bank at $FFF042D4. */
int mc_hammerhead_interleaved(const mc_hammerhead *h, unsigned n) {
    return !(n & 1u) && n + 1 < MC_HAMMERHEAD_BANKS && h->bank_size[n] &&
           h->bank_size[n] == h->bank_size[n + 1] &&
           (h->reg[(MC_HAMMERHEAD_BANK >> 4) + 2 * n] & 4u);
}

unsigned mc_hammerhead_windows(const mc_hammerhead *h, mc_mem_window *w, unsigned max) {
    uint32_t base[MC_HAMMERHEAD_BANKS], size[MC_HAMMERHEAD_BANKS];
    for (unsigned i = 0; i < MC_HAMMERHEAD_BANKS; i++) {
        base[i] = mc_hammerhead_base(h, i);
        size[i] = h->bank_size[i];
    }
    for (unsigned i = 0; i + 1 < MC_HAMMERHEAD_BANKS; i += 2) {
        if (mc_hammerhead_interleaved(h, i)) {
            size[i] *= 2;
            size[i + 1] = 0;  /* partner is part of the even bank's window */
        }
    }
    const unsigned n = mc_mem_bank_windows(base, size, MC_HAMMERHEAD_BANKS,
                                           2u * MC_HAMMERHEAD_BANK_WINDOW, w, max);
    for (unsigned i = 0; i < n; i++) {
        const uint32_t limit = MC_HAMMERHEAD_BANK_WINDOW *
                               (mc_hammerhead_interleaved(h, w[i].bank) ? 2u : 1u);
        if (w[i].size > limit) w[i].size = limit;
    }
    return n;
}

int mc_hammerhead_decode(const mc_hammerhead *h, uint32_t address, unsigned *bank, uint32_t *offset) {
    mc_mem_window w[MC_HAMMERHEAD_BANKS];
    const unsigned n = mc_hammerhead_windows(h, w, MC_HAMMERHEAD_BANKS);
    for (unsigned i = 0; i < n; i++) {
        if (address - w[i].base < w[i].size) {
            const unsigned b = w[i].bank, even = b & ~1u;
            const uint32_t local = (address - w[i].base) % w[i].bank_size;
            uint32_t at = w[i].offset + local;
            unsigned selected = b;
            if (mc_hammerhead_interleaved(h, b)) {
                selected += (local >> 3) & 1u;
            } else if (h->bank_size[even] && h->bank_size[even] == h->bank_size[even + 1]) {
                /* Backing bytes always use the pair's eight-byte lanes.
                 * A separate-bank view scatters into those lanes, so
                 * enabling or disabling interleave preserves DIMM data. */
                const uint32_t start = w[i].offset - ((b & 1u) ? h->bank_size[b] : 0);
                at = start + ((local & ~7u) << 1) + ((b & 1u) << 3) + (local & 7u);
            }
            if (bank) *bank = selected;
            if (offset) *offset = at;
            return 1;
        }
    }
    return 0;
}

void mc_hammerhead_reset(mc_hammerhead *h) {
    memset(h->reg, 0, sizeof h->reg);
    /* The cache SIMM's pins (Developer Note): present and its size, or
     * none. $0E0 bit 7 and bits 1-0. */
    if (h->l2 != MC_HAMMERHEAD_L2_NONE) h->reg[MC_HAMMERHEAD_L2 >> 4] = (uint8_t)(0x80u | h->l2);
    h->reg[MC_HAMMERHEAD_STRAPS >> 4] = h->straps;
    h->reg[0x030 >> 4] = (uint8_t)(h->bus_code << 5);
}

void mc_hammerhead_set_straps(mc_hammerhead *h, uint8_t straps, uint8_t bus_code) {
    h->straps = straps;
    h->bus_code = bus_code & 7u;
    h->reg[MC_HAMMERHEAD_STRAPS >> 4] = straps;
    h->reg[0x030 >> 4] = (uint8_t)((h->reg[0x030 >> 4] & 0x1Fu) | h->bus_code << 5);
}

void mc_hammerhead_init(mc_hammerhead *h, const mc_hw_host *host, const char *name, uint32_t id, unsigned l2, const uint32_t *bank_size) {
    memset(h, 0, sizeof *h);
    h->dev.ops = &mc_hammerhead_ops;
    h->dev.host = host;
    h->dev.name = name;
    h->id = id;
    h->l2 = (uint8_t)(l2 > MC_HAMMERHEAD_L2_NONE ? MC_HAMMERHEAD_L2_NONE : l2);
    if (bank_size) memcpy(h->bank_size, bank_size, sizeof h->bank_size);
    mc_hammerhead_reset(h);
}

static uint32_t hh_read(void *p, uint32_t offset, unsigned size) {
    mc_hammerhead *h = p;
    offset &= MC_HAMMERHEAD_SIZE - 1u;
    if (offset < 16) {                                  /* the identification word */
        const uint32_t v = h->id << (8 * (offset & 3u));
        return size == 4 ? v : size == 2 ? v >> 16 : v >> 24;
    }
    if (offset & 15u) return 0;
    const uint32_t v = h->reg[offset >> 4];
    return size == 4 ? v << 24 : size == 2 ? v << 8 : v;
}

static void hh_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_hammerhead *h = p;
    offset &= MC_HAMMERHEAD_SIZE - 1u;
    if (offset < 16 || (offset & 15u) || offset == MC_HAMMERHEAD_STRAPS) return;
    const unsigned r = offset >> 4;
    const uint8_t v = (uint8_t)(size == 4 ? value >> 24 : size == 2 ? value >> 8 : value);
    if (offset == MC_HAMMERHEAD_L2) {                   /* presence and size stay the pins' */
        h->reg[r] = (uint8_t)((h->reg[r] & 0x83u) | (v & 0x7Cu));
        return;
    }
    h->reg[r] = offset == 0x030 ? (uint8_t)((v & 0x1Fu) | h->bus_code << 5) : v;
    if (offset >= MC_HAMMERHEAD_BANK && offset < MC_HAMMERHEAD_BANK + 0x20u * MC_HAMMERHEAD_BANKS) {
        mc_dev_line(&h->dev, MC_HAMMERHEAD_LINE_MAP, 1);
        mc_dev_line(&h->dev, MC_HAMMERHEAD_LINE_MAP, 0);
    }
}

static void hh_reset_op(void *p) { mc_hammerhead_reset(p); }

const mc_dev_ops mc_hammerhead_ops = {hh_read, hh_write, NULL, hh_reset_op, NULL};
