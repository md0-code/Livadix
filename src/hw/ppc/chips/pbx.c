/* Livadix — PBX and Whitney (pbx.h). Livadix's own code. */
#include "pbx.h"

#include <stdbool.h>
#include <string.h>

static int cycle(uint32_t offset, unsigned size, uint32_t limit) {
    return (size == 1 || size == 2 || size == 4) && offset < limit &&
           size <= limit - offset && (offset & 3u) + size <= 4;
}
static uint32_t extract(uint32_t value, uint32_t offset, unsigned size) {
    return value >> (8u * (4u - (offset & 3u) - size)) & (UINT32_MAX >> (32u - size * 8u));
}
static void map_ram(mc_pbx *p) {
    uint32_t start[MC_PBX_BANKS], used[MC_PBX_BANKS] = {0}, total = 0;
    for (unsigned bank = 0; bank < MC_PBX_BANKS; bank++) {
        start[bank] = total;
        total += p->banks[bank];
    }
    mc_bus_map_float(p->bus, 0, MC_PBX_PAGES * MC_PBX_PAGE);
    for (unsigned page = 0; page < MC_PBX_PAGES; page++) {
        const unsigned bank = (p->reg[4 + page / 8] >> (4u * (page & 7u))) & 15u;
        if (bank >= MC_PBX_BANKS || !p->banks[bank]) continue;
        const uint32_t address = page * MC_PBX_PAGE;
        /* PowerBook 1400 Developer Note, Table 4-10: the row and column
         * pins use CPU address bits. A decoder nibble only selects the
         * bank; it cannot shift the other pages that address that bank. */
        const uint32_t offset = p->duo ? used[bank]++ * MC_PBX_PAGE : address;
        const uint32_t at = start[bank] + offset % p->banks[bank];
        mc_bus_map_memory(p->bus, address, MC_PBX_PAGE, p->ram + at, MC_PBX_PAGE, 1);
    }
}
static uint32_t pbx_read(void *dev, uint32_t offset, unsigned size) {
    mc_pbx *p = dev;
    return cycle(offset, size, sizeof p->reg) ? extract(p->reg[offset / 4], offset, size) : 0;
}
static void pbx_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_pbx *p = dev;
    if (!cycle(offset, size, sizeof p->reg) || offset < 0x10) return;
    const unsigned shift = 8u * (4u - (offset & 3u) - size);
    const uint32_t mask = (UINT32_MAX >> (32u - 8u * size)) << shift;
    p->reg[offset / 4] = (p->reg[offset / 4] & ~mask) | ((value << shift) & mask);
    map_ram(p);
}
static void pbx_reset(void *dev) {
    mc_pbx *p = dev;
    memset(p->reg, 0, sizeof p->reg);
    p->reg[0] = 1; /* ROM clock selector: 33.33 MHz bus. */
    for (unsigned i = 4; i < 8; i++) p->reg[i] = 0x88888888;
    p->reg[4] = p->duo ? 0x88880000 : 0x88110000;
    map_ram(p);
}
const mc_dev_ops mc_pbx_ops = {pbx_read, pbx_write, NULL, pbx_reset, NULL};
/* PowerBook 1400 Developer Note, ch. 2 RAM and ch. 4 Tables 4-11/12:
 * bank 0 is soldered, bank 1 is a single 4 or 8 MiB factory card,
 * and each user card has up to three equal banks (2-4 and 5-7).
 * Prefer one user card and the largest possible banks for a given size. */
static unsigned card_bank_mb(unsigned mb) {
    for (unsigned bank = 16; bank >= 2; bank >>= 1)
        if (mb && mb % bank == 0 && mb / bank <= 3) return bank;
    return 0;
}
static void populate_card(uint32_t *banks, unsigned mb, unsigned bank_mb) {
    for (unsigned i = 0; i < mb / bank_mb; i++) banks[i] = bank_mb << 20;
}
static bool populate_1400(uint32_t *banks, unsigned ram_mb) {
    const unsigned factory = ram_mb == 12 ? 4u : 8u;
    banks[1] = factory << 20;
    const unsigned extra = ram_mb - 8u - factory;
    if (!extra) return true;
    const unsigned single = card_bank_mb(extra);
    if (single) {
        populate_card(banks + 2, extra, single);
        return true;
    }
    for (unsigned first = extra - 2; first >= 2; first -= 2) {
        const unsigned a = card_bank_mb(first), b = card_bank_mb(extra - first);
        if (a && b) {
            populate_card(banks + 2, first, a);
            populate_card(banks + 5, extra - first, b);
            return true;
        }
    }
    return false;
}
int mc_pbx_init(mc_pbx *p, const mc_hw_host *host, mc_bus *bus, uint8_t *ram, unsigned ram_mb, int duo) {
    if (!bus || !ram || ram_mb < (duo ? 8u : 12u) || ram_mb > (duo ? 56u : 64u) || ram_mb % 4u) return 0;
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_pbx_ops; p->dev.host = host; p->dev.name = "PBX";
    p->bus = bus; p->ram = ram; p->duo = duo != 0;
    p->banks[0] = 8u << 20;
    if (duo) {
        unsigned remaining = ram_mb - 8;
        for (unsigned bank = 1; remaining && bank < MC_PBX_BANKS; bank++) {
            unsigned mb = 16;
            while (mb > remaining) mb >>= 1;
            p->banks[bank] = mb << 20; remaining -= mb;
        }
        if (remaining) return 0;
    } else if (!populate_1400(p->banks, ram_mb)) return 0;
    pbx_reset(p);
    return 1;
}

static void cpu_line(mc_whitney *w, int state) {
    if (w->asserted == (state != 0)) return;
    w->asserted = state != 0;
    mc_dev_line(&w->dev, MC_WHITNEY_LINE_CPU, w->asserted);
}
void mc_whitney_set_ipl(mc_whitney *w, unsigned level) {
    w->level = (uint8_t)(level & 7u);
    cpu_line(w, w->level > w->mask);
}
static uint32_t irq_read(void *dev, uint32_t offset, unsigned size) {
    mc_whitney *w = dev;
    const uint32_t value = w->level | (uint32_t)w->mask << 3 | (w->asserted ? 0x80u : 0u);
    return cycle(offset, size, 4) ? extract(value, offset, size) : 0;
}
static void irq_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_whitney *w = dev;
    if (!cycle(offset, size, 4) || offset + size != 4) return;
    w->mask = (uint8_t)((value >> 3) & 7u);
    /* The ROM acknowledges, then writes IPL into mask bits 3-5. */
    cpu_line(w, !(value & 0x80u) && w->level > w->mask);
}
static void irq_reset(void *dev) {
    mc_whitney *w = dev;
    w->level = w->mask = 0;
    cpu_line(w, 0);
}
const mc_dev_ops mc_whitney_irq_ops = {irq_read, irq_write, NULL, irq_reset, NULL};

void mc_whitney_init(mc_whitney *w, const mc_hw_host *host) {
    memset(w, 0, sizeof *w);
    w->dev.ops = &mc_whitney_irq_ops;
    w->dev.host = host;
    w->dev.name = "Whitney";
}
