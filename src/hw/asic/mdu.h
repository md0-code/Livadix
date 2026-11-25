/* Livadix — the Macintosh IIci's Memory Decode Unit (MDU): the physical
 * address decode of the board.
 *
 * Guide to the Macintosh Family Hardware, 2nd ed. (1990), chapter 3,
 * "Memory Decode Unit (MDU)" (separate addresses and strobes for RAM banks
 * A and B, device selects for the RBV, the video CLUT DAC and the NuChip30,
 * the GLUE's decoding, bus error on a transfer that does not complete, the
 * SCSI handshake) and Figure 3-8 (the IIci address map); chapter 5, "Video
 * RAM in the Macintosh IIci computer" (bank A at physical $0000 0000, bank B
 * at $0400 0000; the 68030's MMU joins them into one logical RAM) and
 * Figure 5-11 (four SIMMs per bank: 256 KB or 1 MB SIMMs; Livadix adds the
 * 4 MB and 16 MB SIMMs of the IIci's 128 MB maximum); chapter 4, Table 4-8
 * (CPU.ID2 is 1 on a IIci with the optional parity feature).
 *
 * The physical map the MDU decodes (Figure 3-8):
 *   $0000 0000-$03FF FFFF  RAM bank A (holds the screen buffer from 0)
 *   $0400 0000-$07FF FFFF  RAM bank B
 *   $4000 0000-$4FFF FFFF  ROM, repeated
 *   $5000 0000-$50FF FFFF  I/O: the $4 0000 block from $5000 0000 repeated
 *                          ("Reserved (repeated images of $5000 0000-
 *                          $5003 FFFF)"): VIA1 +$0 0000, SCC +$0 4000, SCSI
 *                          pseudo-DMA with DRQ +$0 6000, SCSI normal mode
 *                          +$1 0000, SCSI pseudo-DMA without DRQ +$1 2000,
 *                          sound +$1 4000, SWIM +$1 6000, VDAC +$2 4000,
 *                          RBV +$2 6000
 *   $5100 0000-$51FF FFFF  RBV ("MDU provides chip select only")
 *   $5200 0000-$52FF FFFF  the cache card's data and tag RAM
 *   $5300 0000-$5FFF FFFF  expansion I/O and factory test space
 *   $6000 0000-$FFFF FFFF  NuBus super slot and standard slot spaces
 *
 * Observed from the IIci ROM 368CADFE: the ROM's
 * decoder table (ROM offset $348C) names the devices in the $5000 0000
 * block's image at $50F0 0000 (VIA1 $50F0 0000, SCC $50F0 4000, SCSI
 * $50F1 0000, pseudo-DMA $50F1 2000 and $50F0 6000, sound $50F1 4000, SWIM
 * $50F1 6000, VDAC $50F2 4000, RBV $50F2 6000).
 *
 * Livadix's model (†: not in the documents, see the observations): each bank
 * repeats its contents through its 64 MB window (the ROM finds a bank's
 * size by where its contents repeat); the RAM area beyond bank B's window
 * and an empty bank B float (the cycle completes, reads 0)†; the reset
 * overlay (ROM at 0) ends at the first access to the ROM's own range, as on
 * the Quadra 700's MCU†; the cache card's and the expansion spaces bus-error
 * when nothing answers† (no cache card is fitted); the parity option (9-bit
 * SIMMs and the Parity Generator and Checker) is not fitted: CPU.ID2 reads
 * 0 and VIA1 PB7 (/Par.Err) stays 1†.
 *
 * The MDU has no registers the ROM programs: this module is the decode as
 * mappings of the physical bus. Livadix's own code. */
#ifndef MC_MDU_H
#define MC_MDU_H

#include "../bus.h"

#include <stdint.h>

#define MC_MDU_BANK_A 0x00000000u
#define MC_MDU_BANK_B 0x04000000u
#define MC_MDU_WINDOW 0x04000000u       /* each bank's window: 64 MB */
#define MC_MDU_ROM 0x40000000u
#define MC_MDU_IO 0x50000000u
#define MC_MDU_IO_SIZE 0x01000000u      /* the repeated images, $5000 0000-$50FF FFFF */
#define MC_MDU_IO_IMAGE 0x40000u
#define MC_MDU_RBV 0x51000000u
#define MC_MDU_CACHE 0x52000000u

/* Offsets in the I/O block (Guide, Figure 3-8). */
enum {
    MC_MDU_VIA1 = 0x00000,
    MC_MDU_SCC = 0x04000,
    MC_MDU_SCSI_DRQ = 0x06000,      /* pseudo-DMA with DRQ (the MDU's handshake) */
    MC_MDU_SCSI = 0x10000,          /* normal mode: the 5380's registers */
    MC_MDU_SCSI_BLIND = 0x12000,    /* pseudo-DMA without DRQ */
    MC_MDU_SOUND = 0x14000,
    MC_MDU_SWIM = 0x16000,
    MC_MDU_VDAC = 0x24000,
    MC_MDU_RBV_IO = 0x26000
};

typedef struct mc_mdu {
    uint32_t bank_a, bank_b;    /* populated sizes in bytes (bank B may be 0) */
    int overlay;                /* the ROM answers at 0 (from reset) */
} mc_mdu;

/* The bank sizes for emulation.ram_mb: 0 when no population of four
 * equal SIMMs per bank (256 KB, 1 MB, 4 MB or 16 MB) gives it. Bank A is
 * always populated; the smaller SIMMs go there (Guide, Figure 5-11 note:
 * "Bank A needs only 1 MB for video"). */
int mc_mdu_banks(uint32_t ram_mb, uint32_t *bank_a, uint32_t *bank_b);
/* The legal emulation.ram_mb values, 0-terminated. */
extern const uint16_t mc_mdu_ram_mb[];

void mc_mdu_init(mc_mdu *m, uint32_t bank_a, uint32_t bank_b);
/* Maps the RAM area [0, $4000 0000): the ROM at 0 while the overlay is on,
 * else the banks in their windows (ram: bank A, then bank B). */
void mc_mdu_map_ram(const mc_mdu *m, mc_bus *bus, uint8_t *ram, const uint8_t *rom, uint32_t rom_size);
/* Maps the ROM through $4000 0000-$4FFF FFFF and the spaces where nothing
 * answers ($5200 0000-$5FFF FFFF without a cache card). */
void mc_mdu_map_fixed(const mc_mdu *m, mc_bus *bus, const uint8_t *rom, uint32_t rom_size);

#endif
