/* Livadix — a PCI function's configuration space (pci.h).
 *
 * Livadix's own code. */
#include "pci.h"

#include <string.h>

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

void mc_pci_config_init(mc_pci_config *c, uint16_t vendor, uint16_t device, uint8_t revision, uint32_t class_code, uint8_t header) {
    memset(c, 0, sizeof *c);
    put16(&c->reg[MC_PCI_VENDOR], vendor);
    put16(&c->reg[MC_PCI_DEVICE], device);
    c->reg[MC_PCI_REVISION] = revision;
    c->reg[MC_PCI_CLASS] = (uint8_t)class_code;
    c->reg[MC_PCI_CLASS + 1] = (uint8_t)(class_code >> 8);
    c->reg[MC_PCI_CLASS + 2] = (uint8_t)(class_code >> 16);
    c->reg[MC_PCI_HEADER] = header;
    /* PCI 2.0, 6.2.2: I/O space, memory space, bus master, memory write
     * and invalidate (bit 4), parity and SERR enables (bits 6, 8). */
    c->wmask[MC_PCI_COMMAND] = 0x57u;
    c->wmask[MC_PCI_COMMAND + 1] = 0x01u;
    c->w1c[MC_PCI_STATUS + 1] = 0xF9u;      /* 6.2.3: bits 15-11 and 8 */
    c->wmask[MC_PCI_CACHE_LINE] = 0xFFu;
    c->wmask[MC_PCI_LATENCY] = 0xFFu;
    c->wmask[MC_PCI_INT_LINE] = 0xFFu;
}

void mc_pci_config_bar(mc_pci_config *c, unsigned n, uint32_t size, int io) {
    if (n >= MC_PCI_BARS) return;
    const unsigned r = MC_PCI_BAR0 + 4u * n;
    c->bar_size[n] = size;
    c->bar_io[n] = (uint8_t)(io != 0);
    const uint32_t mask = size ? ~(size - 1u) : 0;
    for (unsigned i = 0; i < 4; i++) c->wmask[r + i] = (uint8_t)(mask >> (8 * i));
    if (size) {
        c->wmask[r] &= io ? 0xFCu : 0xF0u;
        c->reg[r] = (uint8_t)(io ? 1u : 0u);
    }
}

void mc_pci_config_rom(mc_pci_config *c, uint32_t size) {
    c->rom_size = size;
    const uint32_t mask = size ? ((~(size - 1u) & 0xFFFFF800u) | 1u) : 0;
    for (unsigned i = 0; i < 4; i++) {
        c->wmask[MC_PCI_ROM + i] = (uint8_t)(mask >> (8 * i));
        c->reg[MC_PCI_ROM + i] = 0;
    }
}

void mc_pci_config_writable(mc_pci_config *c, unsigned reg, unsigned bytes) {
    for (unsigned i = 0; i < bytes && reg + i < 256; i++) c->wmask[reg + i] = 0xFFu;
}

uint32_t mc_pci_config_read(const mc_pci_config *c, unsigned reg, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size && reg + i < 256; i++) v |= (uint32_t)c->reg[reg + i] << (8 * i);
    return v;
}

void mc_pci_config_write(mc_pci_config *c, unsigned reg, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size && reg + i < 256; i++) {
        const unsigned r = reg + i;
        const uint8_t b = (uint8_t)(value >> (8 * i));
        c->reg[r] = (uint8_t)((c->reg[r] & ~c->wmask[r]) | (b & c->wmask[r]));
        c->reg[r] &= (uint8_t)~(b & c->w1c[r]);
    }
}

uint32_t mc_pci_config_base(const mc_pci_config *c, unsigned n) {
    if (n >= MC_PCI_BARS) return 0;
    const uint32_t v = mc_pci_config_read(c, MC_PCI_BAR0 + 4u * n, 4);
    return v & (c->bar_io[n] ? ~3u : ~15u);
}

int mc_pci_config_hit(const mc_pci_config *c, unsigned n, uint32_t address, uint32_t *offset) {
    if (n >= MC_PCI_BARS || !c->bar_size[n] || c->bar_io[n]) return 0;
    if (!(c->reg[MC_PCI_COMMAND] & MC_PCI_CMD_MEMORY)) return 0;
    const uint32_t base = mc_pci_config_base(c, n);
    if (address - base >= c->bar_size[n]) return 0;
    if (offset) *offset = address - base;
    return 1;
}

int mc_pci_config_rom_hit(const mc_pci_config *c, uint32_t address, uint32_t *offset) {
    if (!c->rom_size || !(c->reg[MC_PCI_COMMAND] & MC_PCI_CMD_MEMORY) || !(c->reg[MC_PCI_ROM] & 1u)) return 0;
    const uint32_t base = mc_pci_config_read(c, MC_PCI_ROM, 4) & 0xFFFFF800u;
    if (address - base >= c->rom_size) return 0;
    if (offset) *offset = address - base;
    return 1;
}
