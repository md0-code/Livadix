/* Livadix — a PCI function's configuration space and the interface between a
 * host bridge (bandit.h) and the functions on its bus.
 *
 * Sources: PCI Local Bus Specification, Revision 2.0 (PCI Special Interest
 * Group, 1993), chapter 6 "Configuration Space": 6.1 the 256-byte space of
 * each function, 6.2 the predefined header (vendor ID, device ID, command,
 * status, revision ID, class code, cache line size, latency timer, header
 * type, base address registers, interrupt line and pin), 6.2.2 the command
 * register (bit 0 I/O space, bit 1 memory space, bit 2 bus master), 6.2.3
 * status bits cleared by writing 1 (bits 15-11: parity error, signalled
 * system error, received master abort, received target abort, signalled
 * target abort), 6.2.5.1 base address registers (bit 0 1 for I/O space;
 * the size found by writing all ones and reading back the bits that stay
 * 0); 3.2.2 a configuration read nobody claims ends in master abort, which
 * a host bridge returns as all ones. Designing PCI Cards and Drivers for
 * Power Macintosh Computers (Apple, 1995), chapter 3 "Data Flow", Figure
 * 3-1: the byte the processor writes at address n travels on PCI byte lane
 * n (address invariance), so a function sees each access as bytes at PCI
 * addresses; values in this interface are those bytes as a little-endian
 * number (the byte at the lowest address in bits 7-0).
 *
 * Livadix's own code. */
#ifndef MC_PCI_H
#define MC_PCI_H

#include <stdint.h>

#define MC_PCI_BARS 6u

/* Header registers (PCI 2.0, 6.2, Figure 6-1). */
enum {
    MC_PCI_VENDOR = 0x00, MC_PCI_DEVICE = 0x02, MC_PCI_COMMAND = 0x04, MC_PCI_STATUS = 0x06,
    MC_PCI_REVISION = 0x08, MC_PCI_CLASS = 0x09, MC_PCI_CACHE_LINE = 0x0C, MC_PCI_LATENCY = 0x0D,
    MC_PCI_HEADER = 0x0E, MC_PCI_BAR0 = 0x10, MC_PCI_ROM = 0x30, MC_PCI_INT_LINE = 0x3C, MC_PCI_INT_PIN = 0x3D
};
enum { MC_PCI_CMD_IO = 0x0001, MC_PCI_CMD_MEMORY = 0x0002, MC_PCI_CMD_MASTER = 0x0004 };
enum { MC_PCI_ST_MASTER_ABORT = 0x2000, MC_PCI_ST_TARGET_ABORT = 0x1000 };

typedef struct mc_pci_config {
    uint8_t reg[256];
    uint8_t wmask[256];             /* bits a configuration write sets */
    uint8_t w1c[256];               /* bits a 1 written clears */
    uint32_t bar_size[MC_PCI_BARS]; /* 0: no BAR; else a power of two */
    uint8_t bar_io[MC_PCI_BARS];
    uint32_t rom_size;              /* 0: no expansion ROM; else a power of two (2 KiB or more) */
} mc_pci_config;

/* Vendor, device, revision, class code (24 bits), header type; the command
 * register's memory, I/O and master bits, the latency timer, the cache
 * line size and the interrupt line writable, everything else read only. */
void mc_pci_config_init(mc_pci_config *c, uint16_t vendor, uint16_t device, uint8_t revision, uint32_t class_code, uint8_t header);
/* Gives BAR `n` (0-5) a size (a power of two, 16 bytes or more) in memory
 * space, or in I/O space (4 bytes or more). */
void mc_pci_config_bar(mc_pci_config *c, unsigned n, uint32_t size, int io);
/* Gives the Expansion ROM Base Address register ($30; PCI 2.0, 6.2.5.2) a
 * size (a power of two, 2 KiB or more). */
void mc_pci_config_rom(mc_pci_config *c, uint32_t size);
/* Makes `bytes` bytes from `reg` writable (a device-specific register). */
void mc_pci_config_writable(mc_pci_config *c, unsigned reg, unsigned bytes);
/* A configuration read or write of 1, 2 or 4 bytes at byte offset `reg`
 * (little-endian value). */
uint32_t mc_pci_config_read(const mc_pci_config *c, unsigned reg, unsigned size);
void mc_pci_config_write(mc_pci_config *c, unsigned reg, unsigned size, uint32_t value);
/* The base the system assigned to BAR n (its type bits masked). */
uint32_t mc_pci_config_base(const mc_pci_config *c, unsigned n);
/* 1 when `address` falls in memory BAR n and memory space is enabled;
 * *offset its offset in the BAR. */
int mc_pci_config_hit(const mc_pci_config *c, unsigned n, uint32_t address, uint32_t *offset);
/* 1 when `address` falls in the expansion ROM BAR and both memory space
 * and the ROM's address decode (bit 0) are enabled; *offset its offset. */
int mc_pci_config_rom_hit(const mc_pci_config *c, uint32_t address, uint32_t *offset);

/* A function on a bridge's bus, as the bridge reaches it. */
typedef struct mc_pci_function {
    void *dev;
    uint32_t (*config_read)(void *dev, unsigned reg, unsigned size);
    void (*config_write)(void *dev, unsigned reg, unsigned size, uint32_t value);
    /* A memory cycle at PCI address `address` (aligned, 1, 2 or 4 bytes):
     * 1 when the function claims it (asserts DEVSEL), 0 when not. NULL: the
     * function decodes no memory. */
    int (*mem_read)(void *dev, uint32_t address, unsigned size, uint32_t *value);
    int (*mem_write)(void *dev, uint32_t address, unsigned size, uint32_t value);
} mc_pci_function;

/* Byte order between the processor's big-endian numeric values and the
 * PCI bus's little-endian ones for an access of `size` bytes. */
static inline uint32_t mc_pci_swap(uint32_t v, unsigned size) {
    if (size == 2) return ((v & 0xFFu) << 8) | ((v >> 8) & 0xFFu);
    if (size == 4) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    return v & 0xFFu;
}

#endif
