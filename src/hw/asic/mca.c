/* Livadix — the MCA (see mca.h). Livadix's own code. */
#include "mca.h"

#include <string.h>

#define REG_ID 0x438u           /* $438-$444: the identification inputs */
#define REG_BANK 0x450u         /* bank n's registers from $450 + $28n */
#define BANK_STRIDE 0x28u
#define RAM_SPACE 0x40000000u   /* RAM's share of the address space (the I/O from $5000 0000) */

static uint32_t reg_value(const mc_mca *m, unsigned n) {
    const uint32_t off = n * 4u;
    if (off >= REG_ID && off < REG_ID + 16u) return ((m->id >> ((off - REG_ID) / 4u)) & 1u) ? 0x80000000u : 0;
    return m->reg[n];
}

static uint32_t mca_read(void *dev, uint32_t offset, unsigned size) {
    const mc_mca *m = dev;
    const uint32_t v = reg_value(m, (offset % MC_MCA_SIZE) >> 2);
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}

static void mca_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_mca *m = dev;
    const unsigned n = (offset % MC_MCA_SIZE) >> 2;
    uint32_t *r = &m->reg[n];
    const uint32_t before = *r;
    if (size == 4) *r = value;
    else if (size == 2) *r = (offset & 2u) ? (*r & 0xFFFF0000u) | (value & 0xFFFFu) : (*r & 0xFFFFu) | value << 16;
    else {
        const unsigned shift = 8u * (3u - (offset & 3u));
        *r = (*r & ~(0xFFu << shift)) | (value & 0xFFu) << shift;
    }
    /* The banks' bases and size codes decode the RAM. */
    const uint32_t off = n * 4u;
    if (off >= REG_BANK && off < REG_BANK + MC_MCA_BANKS * BANK_STRIDE && ((*r ^ before) & 0x80000000u) && m->remap)
        m->remap(m->ctx);
}

const mc_dev_ops mc_mca_ops = {mca_read, mca_write, NULL, NULL};

/* Register bits from bit 31 of `count` registers from `off`, bit 0 first. */
static uint32_t field(const mc_mca *m, uint32_t off, unsigned count) {
    uint32_t v = 0;
    for (unsigned i = 0; i < count; i++)
        if (m->reg[(off + 4u * i) / 4u] & 0x80000000u) v |= 1u << i;
    return v;
}

static void set_field(mc_mca *m, uint32_t off, unsigned count, uint32_t v) {
    for (unsigned i = 0; i < count; i++) m->reg[(off + 4u * i) / 4u] = ((v >> i) & 1u) ? 0xFFFFFFFFu : 0;
}

uint32_t mc_mca_bank_base(const mc_mca *m, unsigned n) { return field(m, REG_BANK + BANK_STRIDE * n, 7) << 20; }

uint32_t mc_mca_bank_window(const mc_mca *m, unsigned n) {
    const uint32_t code = field(m, REG_BANK + BANK_STRIDE * n + 28u, 3);
    return code ? (1u << 20) << (code - 1u) : 0;
}

void mc_mca_map(const mc_mca *m, mc_bus *bus, uint8_t *ram) {
    mc_bus_map_float(bus, 0, RAM_SPACE);
    uint32_t host = 0;
    for (unsigned i = 0; i < MC_MCA_BANKS; i++) {
        const uint32_t size = m->bank_size[i], base = mc_mca_bank_base(m, i);
        uint64_t window = mc_mca_bank_window(m, i);
        if (base + window > RAM_SPACE) window = RAM_SPACE - base;
        if (size && window) mc_bus_map_memory(bus, base, (uint32_t)window, ram + host, size, 1);
        host += size;
    }
}

static unsigned code_of(uint32_t bytes) {
    unsigned c = 1;
    while (c < 7 && ((1u << 20) << (c - 1u)) < bytes) c++;
    return c;
}

int mc_mca_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_MCA_BANKS]) {
    memset(bank_size, 0, MC_MCA_BANKS * sizeof *bank_size);
    if (ram_mb < onboard_mb) return 0;
    unsigned b = 0;
    if (onboard_mb) bank_size[b++] = onboard_mb << 20;
    uint32_t left = ram_mb - onboard_mb;
    for (unsigned i = 0; i < simm_banks && b < MC_MCA_BANKS && left; i++) {
        uint32_t mb = 16;
        while (mb > left) mb >>= 1;
        bank_size[b++] = mb << 20;
        left -= mb;
    }
    return left == 0;
}

void mc_mca_init(mc_mca *m, const mc_hw_host *host, const char *name, uint8_t id,
                 const uint32_t bank_size[MC_MCA_BANKS], void (*remap)(void *ctx), void *ctx) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_mca_ops;
    m->dev.host = host;
    m->dev.name = name;
    m->id = id;
    memcpy(m->bank_size, bank_size, sizeof m->bank_size);
    /* Until the ROM programs it: the banks packed from 0, as after a start. */
    uint32_t at = 0;
    for (unsigned i = 0; i < MC_MCA_BANKS; i++) {
        if (!bank_size[i]) continue;
        set_field(m, REG_BANK + BANK_STRIDE * i, 7, at >> 20);
        set_field(m, REG_BANK + BANK_STRIDE * i + 28u, 3, code_of(bank_size[i]));
        at += bank_size[i];
    }
    m->remap = remap;
    m->ctx = ctx;
}
