/* Livadix — DJMEMC, the memory controller of the Centris/Quadra 610, 650 and
 * Quadra 800 (the Developer Note calls it MEMC): the RAM banks' decoding.
 *
 * Macintosh Developer Note Number 3 (Centris 610, Centris 650 and Quadra
 * 800), chapter 2, "MEMC IC" (control and timing of ROM, RAM and VRAM; the
 * frame buffer controller), Figure 2-4 (MEMC at $5000 E000), chapter 4,
 * "Support for Memory on 64 MB Boundaries" (one to ten banks, each sized by
 * the ROM at start, which stores the bank layout in the MEMC's registers;
 * the MEMC then decodes the installed banks to contiguous addresses).
 *
 * The register layout is as ROMs F1A6F343 and F1ACAD13 program it
 * (ROM offsets $A0140-$A05C6):
 * longword registers at $5000 E000 + 4n; $00 the configuration (bits 0-3
 * interleave of the bank pairs, bit 4, bit 5 set on the 20 MHz board), $04-
 * $28 the ten banks' start addresses in 4 MB units in bits 0-7 (bit 8 is
 * written set and kept), $2C the end of RAM in 4 MB units, $30 and $34
 * timing values chosen by the CPU clock. To size the banks the ROM puts
 * bank n at n x 64 MB with the end at $FFFFFFFF and looks for each bank's
 * contents repeating through its 64 MB window; then it packs the banks.
 * Bank n answers from its start up to the next bank's start (the last up
 * to the end register), repeating its contents; an empty bank and the
 * space above the end float (reads 0, writes are lost). Every other
 * register reads back what was written. */
#ifndef MC_DJMEMC_H
#define MC_DJMEMC_H

#include "../bus.h"
#include "../hw.h"

#include <stdint.h>

#define MC_DJMEMC_BANKS 10
#define MC_DJMEMC_REGS 64
#define MC_DJMEMC_UNIT (4u << 20)       /* the bank registers' unit */

typedef struct mc_djmemc {
    mc_dev dev;
    uint32_t reg[MC_DJMEMC_REGS];
    uint32_t bank_size[MC_DJMEMC_BANKS];   /* the populated banks (bytes, a power of two, or 0) */
    void (*remap)(void *ctx);               /* the decoding changed */
    void *ctx;
} mc_djmemc;

/* `bank_size`: the RAM of each bank, in order through hw->ram. */
void mc_djmemc_init(mc_djmemc *m, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_DJMEMC_BANKS],
                    void (*remap)(void *ctx), void *ctx);
extern const mc_dev_ops mc_djmemc_ops;
/* Maps the RAM banks into [0, $40000000) as the registers decode them. */
void mc_djmemc_map(const mc_djmemc *m, mc_bus *bus, uint8_t *ram);
/* Splits `ram_mb` into banks as the board's sockets hold it: `onboard_mb`
 * (4 or 8) soldered in banks 0 and 1 (4 MB each), the rest in up to
 * `simm_banks` SIMM banks from bank 2, of 16 MB, then 4 MB (72-pin SIMMs:
 * one or two banks each). 0 when it does not fit. */
int mc_djmemc_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_DJMEMC_BANKS]);

#endif
