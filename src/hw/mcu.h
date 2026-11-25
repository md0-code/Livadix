/* Livadix — the Quadra 700/900 Memory Control Unit (MCU): RAM bank decoding.
 *
 * The Macintosh Quadra 700 Developer Note ("Memory Control Unit", "RAM
 * control") describes the chip only as holding "registers that the system
 * software uses to set the starting address of each bank of memory"; the
 * register layout here is as the Quadra 700 ROM programs it: longword registers at $5000 E000 + 4n, each
 * holding one configuration bit in bit 0, latched by a write to $A0; bits 0-5
 * give the start of bank 2 in 4 MB units (the ROM sizes the banks in two
 * 64 MB windows at 0 and $0400 0000, then sets bank 2 right above bank 1).
 *
 * The Quadra 900 and 950 have four banks of SIMMs (A-D) and no soldered
 * RAM (mc_mcu_set_banks). As their ROMs program it, bits 0-5, 6-11 and 12-17 give the starts of banks B, C
 * and D in 4 MB units: $138B0810 at reset puts them at 64, 128 and 192 MB
 * to size each bank in its own 64 MB window. */
#ifndef MC_MCU_H
#define MC_MCU_H

#include "bus.h"
#include "hw.h"

#include <stdint.h>

#define MC_MCU_REGS 64

typedef struct mc_mcu {
    mc_dev dev;
    uint32_t reg[MC_MCU_REGS];
    uint32_t ram_size;
    uint32_t bank_b;            /* bank 2's start in 4 MB units (bits 0-5, latched by a write to $A0) */
    uint32_t start[3];          /* four-bank boards: banks B-D's starts in 4 MB units */
    uint32_t banks[4];          /* four-bank boards: each bank's size in bytes (none: the Quadra 700's layout) */
    void (*remap)(void *ctx);   /* the decoding changed */
    void *ctx;
} mc_mcu;

void mc_mcu_init(mc_mcu *mcu, const mc_hw_host *host, const char *name, uint32_t ram_size, void (*remap)(void *ctx), void *ctx);
extern const mc_dev_ops mc_mcu_ops;
/* A board with four banks of the sizes given (bytes, in RAM order; a bank
 * smaller than its window repeats through it). */
void mc_mcu_set_banks(mc_mcu *mcu, const uint32_t sizes[4]);
/* Maps the RAM banks into [0, $40000000) as the registers decode them. */
void mc_mcu_map(const mc_mcu *mcu, mc_bus *bus, uint8_t *ram, uint32_t ram_size);

#endif
