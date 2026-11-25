/* Livadix - MPC106 (Grackle), map-B PCI bridge and memory controller.
 * Sources: Motorola MPC106UM/AD (1997), 3.1.2/Table 3-4 (map B),
 * 3.2.1.2 (BE configuration lanes), 3.2.2/Table 3-10 (registers/reset),
 * 3.2.3 (PCI header/status), 3.2.6.1-2/Tables 3-25..29 (bank bounds),
 * 3.2.6.4/Table 3-31 (MEMGO), 3.2.7/Table 3-35 (PICR1), ch. 7 (PCI).
 * Apple Power Macintosh G3 Computers Developer Note (1998), ch. 2,
 * "Grackle IC": processor/PCI bridge, system SDRAM and ROM controller.
 * Timing/cache hit latency and PCI masters' inbound transactions are not
 * represented by this component. No other emulator source was read. */
#ifndef MC_GRACKLE_H
#define MC_GRACKLE_H
#include "hw.h"
#include "pci.h"
#include "hammerhead.h" /* shared mc_mem_window description */

/* Map B, MPC106UM Table 3-4: offsets are from FEC00000. */
#define MC_GRACKLE_BASE 0xFEC00000u
#define MC_GRACKLE_CONFIG_DATA 0x00200000u
#define MC_GRACKLE_CONFIG_SIZE 0x00300000u
#define MC_GRACKLE_BANKS 8u
#define MC_GRACKLE_LINE_MAP 0
#define MC_GRACKLE_MEMGO 0x00080000u /* Table 3-31, bit 19 */
enum { MC_GRACKLE_START = 0x80, MC_GRACKLE_EXT_START = 0x88,
       MC_GRACKLE_END = 0x90, MC_GRACKLE_EXT_END = 0x98,
       MC_GRACKLE_ENABLE = 0xA0, MC_GRACKLE_PICR1 = 0xA8,
       MC_GRACKLE_PICR2 = 0xAC, MC_GRACKLE_MCCR1 = 0xF0 };

typedef struct mc_grackle {
    mc_dev dev;
    mc_pci_config cfg;
    mc_pci_function fn[32];
    uint32_t config_address, bank_size[MC_GRACKLE_BANKS];
    uint32_t aborts;
    uint8_t revision;
} mc_grackle;
extern const mc_dev_ops mc_grackle_ops;
/* Bank sizes are physical populated capacities, in physical RAM order.
 * Revision is a board input (Table 3-11), not an invented probe result. */
void mc_grackle_init(mc_grackle *, const mc_hw_host *, const char *, uint8_t revision, const uint32_t *bank_sizes);
void mc_grackle_reset(mc_grackle *);
int mc_grackle_attach(mc_grackle *, unsigned device, const mc_pci_function *);
uint32_t mc_grackle_config_read(mc_grackle *, unsigned device, unsigned function, unsigned reg, unsigned size);
void mc_grackle_config_write(mc_grackle *, unsigned device, unsigned function, unsigned reg, unsigned size, uint32_t value);
unsigned mc_grackle_windows(const mc_grackle *, mc_mem_window *, unsigned max);
int mc_grackle_decode(const mc_grackle *, uint32_t address, unsigned *bank, uint32_t *offset);
/* CPU numeric BE values; function interfaces carry PCI LE values. */
int mc_grackle_mem_read(mc_grackle *, uint32_t address, unsigned size, uint32_t *value);
int mc_grackle_mem_write(mc_grackle *, uint32_t address, unsigned size, uint32_t value);
#endif
