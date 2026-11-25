/* Livadix — MEMCjr, the memory controller of the LC 475, LC 575, Quadra 605
 * and Performa 475/476/575/577/578: the RAM banks' decoding.
 *
 * Macintosh LC 575 Developer Note (Apple, 1994), chapter 2, "MEMCjr IC"
 * (control and timing of ROM, RAM and VRAM, bus arbitration, a frame
 * buffer controller compatible with the DAFB), "RAM Addresses" (the first
 * 1 GB is RAM: 4 to 36 MB, sized by the ROM at start); chapter 1, "RAM
 * Expansion" and Figure 1-4 (4 MB on the board and one 72-pin SIMM socket:
 * 1, 2, 4, 8, 16 or 32 MB, single or double sided; 4, 5, 6, 8, 12, 20 and
 * 36 MB in all); chapter 3, "New Memory Controller" (a different
 * programming interface from the MEMC's; 1 and 2 MB SIMMs supported).
 *
 * The register layout as ROM FF7439EE programs it (ROM offsets $4952, $A014A-$A0182, $A0572-$A0636):
 * longword registers at $5000 E000 + 4n with a six-bit data path (the ROM
 * writes every value v as v ror 6 to +$7C, then v to the register, and
 * reads a register, then +$7C, and joins the two six-bit parts): +$7C is
 * the window on a register's upper six bits, latched into the register
 * written next and loaded from the register read; a register holds 12
 * bits. The frame buffer controller's registers at $F980 0000 are reached
 * the same way (the video driver's accessor at ROM offset $F8446 joins
 * the register's low six bits with the window's). +$04-$14 the five banks'
 * starts in 1 MB units in bits 0-9 (the RAM info record at ROM offset
 * $A88C4: unit $10 0000), bit 10 marking a bank the ROM found empty (ROM
 * offset $A0598); +$2C the last MB of RAM; +$30 and +$34 timing values
 * chosen by the CPU clock (ROM offset $4958). To size the banks the ROM puts bank n at n x 64 MB with the
 * last MB at $3FF and looks for each bank's contents repeating through its
 * window; then it packs the banks. Bank n answers from its start up to the
 * next bank's start (the last up to the end), repeating its contents; an
 * empty or marked bank and the space above the end float (reads 0, writes
 * are lost). Every other register reads back what was written. */
#ifndef MC_MEMCJR_H
#define MC_MEMCJR_H

#include "../bus.h"
#include "../hw.h"

#include <stdint.h>

#define MC_MEMCJR_BANKS 5
#define MC_MEMCJR_REGS 32
#define MC_MEMCJR_UNIT (1u << 20)       /* the bank registers' unit */

typedef struct mc_memcjr {
    mc_dev dev;
    uint32_t reg[MC_MEMCJR_REGS];       /* 12 bits: the window's six above the six written */
    uint32_t window;                    /* +$7C as written; its bits 0-5 are the upper six */
    uint32_t bank_size[MC_MEMCJR_BANKS];   /* the populated banks (bytes, a power of two, or 0) */
    void (*remap)(void *ctx);           /* the decoding changed */
    void *ctx;
} mc_memcjr;

/* `bank_size`: the RAM of each bank, in order through hw->ram. */
void mc_memcjr_init(mc_memcjr *m, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_MEMCJR_BANKS],
                    void (*remap)(void *ctx), void *ctx);
extern const mc_dev_ops mc_memcjr_ops;
/* A 12-bit register value from the six bits written (`low`) and the
 * window's six: what a write of `low` stores, here or in the frame buffer
 * controller. A read of such a register loads its upper six bits into the
 * window (mc_memcjr_load). */
uint32_t mc_memcjr_compose(const mc_memcjr *m, uint32_t low);
static inline void mc_memcjr_load(mc_memcjr *m, uint32_t value) { m->window = value >> 6 & 0x3Fu; }
/* Maps the RAM banks into [0, $40000000) as the registers decode them. */
void mc_memcjr_map(const mc_memcjr *m, mc_bus *bus, uint8_t *ram);
/* Splits `ram_mb` into banks: the 4 MB on the board in bank 0, the SIMM's
 * one or two sides in banks 1 and 2 (1, 4 or 16 MB each)†. 0 when it is
 * not one of the Developer Note's sizes. */
int mc_memcjr_layout(uint32_t ram_mb, uint32_t bank_size[MC_MEMCJR_BANKS]);

#endif
