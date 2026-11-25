/* Livadix - MPC106 (Grackle), sources/contract in grackle.h. */
#include "grackle.h"
#include <string.h>

static uint32_t ones(unsigned size) { return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu; }
static void put32(uint8_t *p, uint32_t v) { for (unsigned k = 0; k < 4; ++k) p[k] = (uint8_t)(v >> (8u * k)); }
static void abort_cycle(mc_grackle *g) {
    ++g->aborts;
    g->cfg.reg[MC_PCI_STATUS + 1] |= (uint8_t)(MC_PCI_ST_MASTER_ABORT >> 8);
}
void mc_grackle_reset(mc_grackle *g) {
    /* MPC106UM Table 3-10: no BARs, read-only header fields. Map B and
     * 64-bit processor-bus ROM are board straps (Tables 3-31, 3-35). */
    mc_pci_config_init(&g->cfg, 0x1057, 0x0002, g->revision, 0x060000, 0);
    g->cfg.reg[MC_PCI_COMMAND] = 6;
    g->cfg.reg[MC_PCI_STATUS] = 0x80;
    g->cfg.reg[MC_PCI_CACHE_LINE] = 8;
    g->cfg.wmask[MC_PCI_COMMAND] = 0x46;
    g->cfg.wmask[MC_PCI_CACHE_LINE] = 0;
    g->cfg.wmask[MC_PCI_LATENCY] = 0;
    g->cfg.wmask[MC_PCI_INT_LINE] = 0;
    /* Only the registers the manual defines are writable. Reserved
     * bytes remain zero; firmware must preserve reserved bit values. */
    static const uint8_t spans[][2] = {
        {0x41,1},{0x70,4},{0x80,0x21},{0xA3,1},{0xA8,8},
        {0xB8,4},{0xC0,1},{0xC4,1},{0xE0,4},{0xE8,4},{0xF0,16}
    };
    for (unsigned k = 0; k < sizeof spans / sizeof *spans; ++k)
        mc_pci_config_writable(&g->cfg, spans[k][0], spans[k][1]);
    for (unsigned k = 0; k < 8; ++k) {
        g->cfg.wmask[MC_GRACKLE_EXT_START + k] = 3;
        g->cfg.wmask[MC_GRACKLE_EXT_END + k] = 3;
    }
    /* Table 3-10 reset values; Table 3-35 RCS0 is latched high for ROM
     * on the processor bus. PROC_TYPE starts at 601 until programmed. */
    g->cfg.reg[0x73] = 0xCD; g->cfg.reg[0xBA] = 4; g->cfg.reg[0xC0] = 1;
    put32(&g->cfg.reg[MC_GRACKLE_PICR1], 0xFF100010u);
    put32(&g->cfg.reg[MC_GRACKLE_PICR2], 0x000C060Cu);
    put32(&g->cfg.reg[0xE0], 0x0FFF0042u); put32(&g->cfg.reg[0xE8], 0x20);
    put32(&g->cfg.reg[MC_GRACKLE_MCCR1], 0xFF820000u);
    put32(&g->cfg.reg[0xF4], 3); put32(&g->cfg.reg[0xFC], 0x00100000u);
    g->cfg.wmask[MC_GRACKLE_PICR1 + 1] &= 0x3Fu; /* read-only MP_ID, Table 3-35 */
    g->cfg.wmask[MC_GRACKLE_PICR1 + 2] &= 0xEFu; /* read-only RCS0 strap */
    g->cfg.wmask[MC_GRACKLE_MCCR1 + 2] &= 0xDFu; /* read-only 8N64 strap */
    /* Error-detection/status bits clear on writes of one, ch. 9. */
    static const uint8_t errors[] = {0xC1,0xC3,0xC5,0xC7};
    for (unsigned k = 0; k < sizeof errors; ++k) g->cfg.w1c[errors[k]] = 0xFF;
    g->config_address = 0; g->aborts = 0;
}
void mc_grackle_init(mc_grackle *g, const mc_hw_host *host, const char *name, uint8_t revision, const uint32_t *sizes) {
    memset(g, 0, sizeof *g); g->dev.ops = &mc_grackle_ops; g->dev.host = host; g->dev.name = name;
    g->revision = revision;
    if (sizes) memcpy(g->bank_size, sizes, sizeof g->bank_size);
    mc_grackle_reset(g);
}
int mc_grackle_attach(mc_grackle *g, unsigned device, const mc_pci_function *f) {
    if (!device || device >= 32 || !f) return 0;
    g->fn[device] = *f; return 1;
}
uint32_t mc_grackle_config_read(mc_grackle *g, unsigned device, unsigned function, unsigned reg, unsigned size) {
    if (device >= 32 || function || reg + size > 256) { abort_cycle(g); return ones(size); }
    if (!device) return mc_pci_config_read(&g->cfg, reg, size);
    const mc_pci_function *f = &g->fn[device];
    if (!f->config_read) { abort_cycle(g); return ones(size); }
    return f->config_read(f->dev, reg, size);
}
void mc_grackle_config_write(mc_grackle *g, unsigned device, unsigned function, unsigned reg, unsigned size, uint32_t value) {
    if (device >= 32 || function || reg + size > 256) { abort_cycle(g); return; }
    if (!device) {
        mc_pci_config_write(&g->cfg, reg, size, value);
        if ((reg < 0xA1 && reg + size > 0x80) || (reg < 0xF4 && reg + size > 0xF0)) {
            mc_dev_line(&g->dev, MC_GRACKLE_LINE_MAP, 1); mc_dev_line(&g->dev, MC_GRACKLE_LINE_MAP, 0);
        }
    } else {
        const mc_pci_function *f = &g->fn[device];
        if (!f->config_write) { abort_cycle(g); return; }
        f->config_write(f->dev, reg, size, value);
    }
}
unsigned mc_grackle_windows(const mc_grackle *g, mc_mem_window *w, unsigned max) {
    if (!(mc_pci_config_read(&g->cfg, MC_GRACKLE_MCCR1, 4) & MC_GRACKLE_MEMGO)) return 0;
    uint32_t offset = 0;
    unsigned count = 0;
    for (unsigned bank = 0; bank < MC_GRACKLE_BANKS; ++bank) {
        const uint32_t capacity = g->bank_size[bank];
        /* MPC106UM 3.2.6.1: 2 extended bits, 8 address bits, 20 low bits. */
        const uint32_t start = ((uint32_t)(g->cfg.reg[MC_GRACKLE_EXT_START + bank] & 3u) << 28)
            | (uint32_t)g->cfg.reg[MC_GRACKLE_START + bank] << 20;
        const uint32_t end = ((uint32_t)(g->cfg.reg[MC_GRACKLE_EXT_END + bank] & 3u) << 28)
            | (uint32_t)g->cfg.reg[MC_GRACKLE_END + bank] << 20 | 0xFFFFFu;
        if (capacity && (g->cfg.reg[MC_GRACKLE_ENABLE] & (1u << bank)) && end >= start && count < max)
            w[count++] = (mc_mem_window){start, end - start + 1u, offset, capacity, bank};
        offset += capacity;
    }
    return count;
}
int mc_grackle_decode(const mc_grackle *g, uint32_t address, unsigned *bank, uint32_t *offset) {
    mc_mem_window w[MC_GRACKLE_BANKS];
    const unsigned count = mc_grackle_windows(g, w, MC_GRACKLE_BANKS);
    for (unsigned k = 0; k < count; ++k) if (address - w[k].base < w[k].size) {
        if (bank) *bank = w[k].bank;
        if (offset) *offset = w[k].offset + (address - w[k].base) % w[k].bank_size;
        return 1;
    }
    return 0;
}
int mc_grackle_mem_read(mc_grackle *g, uint32_t address, unsigned size, uint32_t *value) {
    if (!(g->cfg.reg[MC_PCI_COMMAND] & MC_PCI_CMD_MASTER)) { *value = ones(size); return 0; }
    for (unsigned k = 1; k < 32; ++k) {
        const mc_pci_function *f = &g->fn[k]; uint32_t v;
        if (f->mem_read && f->mem_read(f->dev, address, size, &v)) { *value = mc_pci_swap(v, size); return 1; }
    }
    abort_cycle(g); *value = ones(size); return 0;
}
int mc_grackle_mem_write(mc_grackle *g, uint32_t address, unsigned size, uint32_t value) {
    if (!(g->cfg.reg[MC_PCI_COMMAND] & MC_PCI_CMD_MASTER)) return 0;
    for (unsigned k = 1; k < 32; ++k) {
        const mc_pci_function *f = &g->fn[k];
        if (f->mem_write && f->mem_write(f->dev, address, size, mc_pci_swap(value, size))) return 1;
    }
    abort_cycle(g); return 0;
}
static int target(mc_grackle *g, unsigned *device, unsigned *function, unsigned *reg) {
    /* MPC106UM 3.2.1/ch. 7: mechanism-1, enable bit, bus/device/function. */
    const uint32_t a = g->config_address;
    if (!(a & 0x80000000u) || (a & 0x00FF0000u)) return 0;
    *device = (a >> 11) & 31u; *function = (a >> 8) & 7u; *reg = a & 0xFCu;
    return 1;
}
static uint32_t grackle_read(void *p, uint32_t offset, unsigned size) {
    mc_grackle *g = p;
    if (offset < MC_GRACKLE_CONFIG_DATA)
        return mc_pci_swap(g->config_address >> (8u * (offset & 3u)), size);
    if (offset < MC_GRACKLE_CONFIG_SIZE) {
        unsigned d, f, r;
        if (!target(g, &d, &f, &r)) { abort_cycle(g); return ones(size); }
        return mc_pci_swap(mc_grackle_config_read(g, d, f, r + (offset & 3u), size), size);
    }
    return ones(size);
}
static void grackle_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_grackle *g = p;
    if (offset < MC_GRACKLE_CONFIG_DATA) {
        const unsigned shift = 8u * (offset & 3u);
        const uint32_t mask = ones(size) << shift;
        g->config_address = (g->config_address & ~mask) | (mc_pci_swap(value, size) << shift);
    } else if (offset < MC_GRACKLE_CONFIG_SIZE) {
        unsigned d, f, r;
        if (!target(g, &d, &f, &r)) { abort_cycle(g); return; }
        mc_grackle_config_write(g, d, f, r + (offset & 3u), size, mc_pci_swap(value, size));
    }
}
static void reset(void *p) { mc_grackle_reset(p); }
const mc_dev_ops mc_grackle_ops = {grackle_read, grackle_write, NULL, reset, NULL};
