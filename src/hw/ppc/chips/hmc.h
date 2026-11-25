/* Livadix — the HMC (high-speed memory controller) of the Power Macintosh
 * 6100, 7100 and 8100 (the "PDM" boards).
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994;
 * Macintosh Developer Note Number 8), chapter 2: "Cache Memory" (the L2
 * cache SIMM: the HMC reads two pins of its connector at start-up to learn
 * its size; no SIMM, pull-ups, external cache disabled), "High-Speed Memory
 * Controller" (all memory operations; single-beat and four-beat burst
 * transfers; misaligned reads as double words; address-only transactions;
 * the address bus arbitration; no pipelining, snooping, TEA recovery or
 * little-endian mode), Table 2-4 (bus arbitration), Table 2-5 (the physical
 * map: RAM $0000 0000-$0FFF FFFF, RAM aliases to $3FFF FFFF, ROM $4000
 * 0000-$4FFF FFFF, I/O $5000 0000-$5FFF FFFF, the ROM alias $FF00 0000-$FFFF
 * FFFF), "RAM Access". Enhanced Power Macintosh Developer Note (Apple,
 * 1994), appendix A: "RAM SIMMs", Table A-1, "Discontinuous Physical
 * Addressing" (each SIMM a 64 MB bank whose memory starts 16 MB into it;
 * the 8 MB soldered to the board in the low half of the first 16 MB),
 * Table A-2 (the 8100's map).
 *
 * The register and the banks as ROM 9FEB69B3 (pm-6100.rom) drives them
 * (ROM addresses at $FFC0 0000):
 *   - one serial configuration register of 35 bits at $50F4 0000: a write
 *     to +$08 starts it over at bit 0; each byte written at +$00 shifts its
 *     bit 0 into the next bit (bits 0-31, then 32-34); each byte read at
 *     +$00 returns the next bit in bit 0 ($FFF03838: the ROM writes the 35
 *     bits and reads them back at once, $FFF0387C);
 *   - bits 1-0 read the cache SIMM's size pins whatever was written: 0 no
 *     cache, n a cache of 64 KB << n (the ROM, $FFF032DC: 1 is 128 KB, 2
 *     256 KB); the other bits read back as written. The ROM sets bit 27
 *     when a cache is present ($FFF032F0), bits 21-16 from the bus clock
 *     ($FFF038F8), bit 31 for 8 MB on the board (cleared for 4 MB,
 *     $FFF03CB8), bits 30-29 the SIMM banks' size code (0: 2 MB banks or
 *     none, 1: 8 MB, 2: 32 MB, 3: 128 MB, $FFF03D24), bit 28 on an AMIC
 *     whose SCSI DMA control lacks bits 3-2 ($FFF032B8), bit 25 around its
 *     test of the CPU bus at $5000 0000 ($FFF03A0C); Livadix keeps them all;
 *   - the RAM banks, as the ROM sizes them ($FFF03060-$FFF03198 with
 *     $FFF03B00): it writes a signature at the top of each window and looks
 *     for the address where it reads back, in 2 MB steps, then for where a
 *     smaller bank repeats; so a bank appears from its window's start and
 *     repeats through the window, and an empty window holds nothing. On the
 *     7100 and 8100 (CPU ID $3012, $3013: cpuid.h) the windows are the 8 MB
 *     on the board at 0 and eight SIMM banks of 32 MB at $0100 0000 + n x
 *     $0400 0000 (Table A-2); on the 6100 ($3010) the board's 8 MB at 0 and
 *     two banks of 128 MB at $1000 0000 and $0800 0000 (sized in that
 *     order), which, once the ROM writes their size code, follow the
 *     board's RAM contiguously (the ROM rewrites its table so,
 *     $FFF03D04, and its nanokernel puts its data at the top of that
 *     span)†;
 *   - the ROM at $4000 0000-$4FFF FFFF and $FF00 0000-$FFFF FFFF, repeating
 *     every 4 MB (the ROM runs from $4030 xxxx after its first jump and its
 *     68k code addresses it at $4080 0000).
 * Not modelled: the arbitration, the cache itself (the ROM sees its size
 * only), parity, refresh, the timing bits' effect.
 *
 * Livadix's own code. */
#ifndef MC_HMC_H
#define MC_HMC_H

#include "hw.h"

#include <stdint.h>

#define MC_HMC_SIZE 0x10u              /* $50F4 0000: +$00 data bit, +$08 restart */
#define MC_HMC_BITS 35u
#define MC_HMC_BANKS 9u                /* the board's RAM and eight SIMM banks */
#define MC_HMC_ROM_SIZE 0x400000u      /* 4 MB (Developer Note, "Read-Only Memory") */

typedef enum mc_hmc_board {
    MC_HMC_BOARD_6100,          /* two SIMM banks of 128 MB windows */
    MC_HMC_BOARD_7100_8100      /* eight SIMM banks of 32 MB windows */
} mc_hmc_board;

typedef struct mc_hmc_bank {
    uint32_t window;            /* physical start of the bank's window */
    uint32_t window_size;
    uint32_t size;              /* installed bytes (0: empty) */
    uint32_t offset;            /* where the bank starts in the machine's RAM buffer */
} mc_hmc_bank;

typedef struct mc_hmc {
    mc_dev dev;
    uint64_t config;            /* the 35 bits as written */
    unsigned bit;               /* the next bit shifted in or out */
    uint8_t cache_code;         /* the cache SIMM's size pins (0: none) */
    mc_hmc_bank bank[MC_HMC_BANKS];
    uint32_t ram_size;          /* the banks' total */
    mc_hmc_board board;
    void (*remap)(void *ctx);   /* the board maps RAM again after a decode change */
    void *remap_ctx;
} mc_hmc;

extern const mc_dev_ops mc_hmc_ops;

/* An HMC on `board` with `board_ram` bytes soldered (4 or 8 MB), SIMM banks
 * of `simm_bank[i]` bytes (n = 2 on the 6100, up to 8 on the 7100 and
 * 8100; 0 for an empty bank; powers of two of 2 MB or more, at most the
 * window) and a cache SIMM of `cache_kb` KB (0, 128, 256, 512 or 1024). The banks
 * are packed in the machine's RAM buffer in order: board RAM first. */
void mc_hmc_init(mc_hmc *h, const mc_hw_host *host, const char *name, mc_hmc_board board, uint32_t board_ram,
                 const uint32_t *simm_bank, unsigned n, uint32_t cache_kb);

/* Called when the decode changes (the 6100's banks move, hmc.c). */
void mc_hmc_set_remap(mc_hmc *h, void (*remap)(void *ctx), void *ctx);
/* The RAM buffer offset of physical `address`, or -1 when no RAM answers
 * there (an empty window, between the windows, or outside the RAM space).
 * Banks repeat through their windows. (Table 2-5's "RAM aliases" range
 * holds the 8100's upper SIMM windows, Table A-2; no other alias is
 * modelled.) */
int64_t mc_hmc_ram_offset(const mc_hmc *h, uint32_t address);
/* The ROM offset of physical `address`, or -1 outside the ROM's ranges. */
int64_t mc_hmc_rom_offset(uint32_t address);
/* The 35 configuration bits (bits 1-0 as the ROM reads them). */
uint64_t mc_hmc_config(const mc_hmc *h);

#endif
