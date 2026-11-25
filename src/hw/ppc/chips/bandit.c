/* Livadix — Bandit and Chaos, the PCI and VCI host bridges (bandit.h).
 *
 * Livadix's own code. */
#include "bandit.h"

#include <string.h>

static int self_device(const mc_bandit *b, unsigned device) {
    return b->kind == MC_BANDIT_BANDIT && device == MC_BANDIT_SELF;
}

void mc_bandit_reset(mc_bandit *b) {
    b->config_address = 0;
    if (b->kind == MC_BANDIT_BANDIT) {
        /* PCI 2.0 6.2: vendor, device, revision, class (host bridge
         * $060000); Apple's IDs from the bandit FCode's "pci106b,1" node.
         * The revision is not in the ROM†. */
        mc_pci_config_init(&b->cfg, 0x106Bu, 0x0001u, 0x03u, 0x060000u, 0x00u);
        mc_pci_config_writable(&b->cfg, 0x40, 0x40);
        /* Register $50 bit 0: Open Firmware clears it for big-endian
         * operation; set at reset†. */
        b->cfg.reg[MC_BANDIT_MODE] = 0x01u;
    }
}

void mc_bandit_init(mc_bandit *b, const mc_hw_host *host, const char *name, mc_bandit_kind kind, uint32_t base) {
    memset(b, 0, sizeof *b);
    b->dev.ops = &mc_bandit_ops;
    b->dev.host = host;
    b->dev.name = name;
    b->kind = (uint8_t)kind;
    b->base = base;
    mc_bandit_reset(b);
}

int mc_bandit_attach(mc_bandit *b, unsigned device, const mc_pci_function *fn) {
    if (device >= MC_BANDIT_DEVICES || self_device(b, device)) return 0;
    b->fn[device] = *fn;
    return 1;
}

uint32_t mc_bandit_config_read(mc_bandit *b, unsigned device, unsigned function, unsigned reg, unsigned size) {
    const uint32_t ones = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    if (device >= MC_BANDIT_DEVICES || function) { b->aborts++; return ones; }
    if (self_device(b, device)) return mc_pci_config_read(&b->cfg, reg, size);
    const mc_pci_function *f = &b->fn[device];
    if (!f->config_read) { b->aborts++; return ones; }
    return f->config_read(f->dev, reg, size);
}

void mc_bandit_config_write(mc_bandit *b, unsigned device, unsigned function, unsigned reg, unsigned size, uint32_t value) {
    if (device >= MC_BANDIT_DEVICES || function) { b->aborts++; return; }
    if (self_device(b, device)) { mc_pci_config_write(&b->cfg, reg, size, value); return; }
    const mc_pci_function *f = &b->fn[device];
    if (!f->config_write) { b->aborts++; return; }
    f->config_write(f->dev, reg, size, value);
}

/* The configuration address register's cycle (PCI 2.0 3.7.4.1): type 0
 * selects the device whose IDSEL bit (11-31) is the only one set above
 * bit 10; type 1 goes to a bus behind a PCI-to-PCI bridge, which no board
 * has (master abort). 0 when the cycle has no target. */
static int decode(const mc_bandit *b, unsigned *device, unsigned *function, unsigned *reg) {
    const uint32_t a = b->config_address;
    if (a & 1u) return 0;
    const uint32_t idsel = a & 0xFFFFF800u;
    if (!idsel || (idsel & (idsel - 1u))) return 0;
    unsigned d = 11;
    while (!(idsel & (1u << d))) d++;
    *device = d;
    *function = (a >> 8) & 7u;
    *reg = a & 0xFCu;
    return 1;
}

static uint32_t ones(unsigned size) { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }

static void abort_status(mc_bandit *b) {
    b->aborts++;
    if (b->kind == MC_BANDIT_BANDIT) b->cfg.reg[MC_PCI_STATUS + 1] |= (uint8_t)(MC_PCI_ST_MASTER_ABORT >> 8);
}

int mc_bandit_decodes(const mc_bandit *b, uint32_t address) {
    if (address - (b->base + MC_BANDIT_MEMORY) < MC_BANDIT_SPACE - MC_BANDIT_MEMORY) return 1;
    if (b->kind != MC_BANDIT_BANDIT) return 0;
    /* Register $48: bits 0-15 open the 16 MB window $F000 0000 + n x $100 0000
     * (fine); bits 16-30 open the 256 MB window (n - 16) x $1000 0000 (coarse).
     * Open Firmware's bandit map-range and the 9500 device-tree ranges use this
     * layout (bit 24 → $8000 0000). */
    const uint32_t mask = mc_pci_config_read(&b->cfg, MC_BANDIT_ADDRESS_MASK, 4);
    const unsigned top = address >> 28;
    if (top < 0xFu) return (mask >> (16 + top)) & 1u;
    return (mask >> ((address >> 24) & 0xFu)) & 1u;
}

int mc_bandit_mem_read(mc_bandit *b, uint32_t address, unsigned size, uint32_t *value) {
    for (unsigned d = 0; d < MC_BANDIT_DEVICES; d++) {
        const mc_pci_function *f = &b->fn[d];
        uint32_t v;
        if (f->mem_read && f->mem_read(f->dev, address, size, &v)) { *value = mc_pci_swap(v, size); return 1; }
    }
    abort_status(b);
    *value = ones(size);
    return 0;
}

int mc_bandit_mem_write(mc_bandit *b, uint32_t address, unsigned size, uint32_t value) {
    const uint32_t v = mc_pci_swap(value, size);
    for (unsigned d = 0; d < MC_BANDIT_DEVICES; d++) {
        const mc_pci_function *f = &b->fn[d];
        if (f->mem_write && f->mem_write(f->dev, address, size, v)) return 1;
    }
    abort_status(b);
    return 0;
}

static uint32_t bandit_read(void *p, uint32_t offset, unsigned size) {
    mc_bandit *b = p;
    if (offset >= MC_BANDIT_MEMORY) {
        uint32_t v;
        mc_bandit_mem_read(b, b->base + offset, size, &v);
        return v;
    }
    if (offset >= MC_BANDIT_CONFIG_DATA) {
        unsigned device, function, reg;
        if (!decode(b, &device, &function, &reg)) { abort_status(b); return ones(size); }
        return mc_pci_swap(mc_bandit_config_read(b, device, function, reg + (offset & 3u), size), size);
    }
    if (offset >= MC_BANDIT_CONFIG_ADDRESS) return mc_pci_swap(b->config_address >> (8 * (offset & 3u)), size);
    abort_status(b);            /* I/O space: no function decodes I/O */
    return ones(size);
}

static void bandit_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_bandit *b = p;
    if (offset >= MC_BANDIT_MEMORY) { mc_bandit_mem_write(b, b->base + offset, size, value); return; }
    if (offset >= MC_BANDIT_CONFIG_DATA) {
        unsigned device, function, reg;
        if (!decode(b, &device, &function, &reg)) { abort_status(b); return; }
        mc_bandit_config_write(b, device, function, reg + (offset & 3u), size, mc_pci_swap(value, size));
        return;
    }
    if (offset >= MC_BANDIT_CONFIG_ADDRESS) {
        const unsigned shift = 8 * (offset & 3u);
        const uint32_t v = mc_pci_swap(value, size), m = (size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu) << shift;
        b->config_address = (b->config_address & ~m) | ((v << shift) & m);
        return;
    }
    abort_status(b);
}

static void bandit_reset_op(void *p) { mc_bandit_reset(p); }

const mc_dev_ops mc_bandit_ops = {bandit_read, bandit_write, NULL, bandit_reset_op, NULL};
