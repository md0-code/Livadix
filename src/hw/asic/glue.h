/* Livadix — the GLUE of the Macintosh II, II FDHD, IIx, IIcx and SE/30: the
 * physical address decode of the board.
 *
 * Guide to the Macintosh Family Hardware, 2nd ed. (1990):
 *   - chapter 3, "GLUE in the Macintosh SE/30, Macintosh II, and Macintosh
 *     IIx computers" (address decode, device selects, /DSACK, the RAM's
 *     strobes, the E clock, the bus error on a transfer that does not
 *     complete, the OR of the slot interrupts, the interrupt priorities);
 *   - "Address map for the Macintosh II, Macintosh IIx, and Macintosh IIcx
 *     computers" and "Address map for the Macintosh SE/30 computer",
 *     Figures 3-5 to 3-7, Tables 3-8 and 3-10: RAM from $0000 0000, the ROM
 *     at $4000 0000, the I/O space at $5000 0000 (the devices in
 *     $5000 0000-$5001 FFFF; the space up to $5FFF FFFF beyond
 *     $5100 0000 undecoded, no /DSACK), NuBus super slot space
 *     $6000 0000-$EFFF FFFF and standard slot space $F100 0000-
 *     $FFFF FFFF, $F000 0000-$F0FF FFFF a bus error for the processor; the
 *     ROM overlay map (ROM through $0000 0000-$3FFF FFFF) while VIA1 PA4
 *     vOverlay is high; the 24-bit map Table 3-8 gives (amu.h);
 *   - chapter 4, Tables 4-9 and 4-10: VIA2 PA7/PA6 v2RAM1/v2RAM0, the size
 *     of the RAM ICs in bank A (256 Kbit, 1, 4 or 16 Mbit), which "determine
 *     the physical address at which the GLUE IC stops selecting bank A and
 *     starts selecting bank B".
 *
 * The I/O block (Figure 3-7, its device boundaries; the ROMs' own
 * addresses): VIA1 +$0 0000, VIA2 +$0 2000, SCC
 * +$0 4000, SCSI pseudo-DMA with the GLUE's DRQ handshake +$0 6000, SCSI
 * normal mode +$1 0000 (the 5380's registers every $10), SCSI pseudo-DMA
 * without the handshake +$1 2000, sound +$1 4000, IWM or SWIM +$1 6000.
 * The ROMs address it at $50F0 0000 (VIA1 $50F0 0000, VIA2 $50F0 2000, SCC
 * $50F0 4000, SCSIHsk $50F0 6000, SCSIBase $50F1 0000, SCSIDMA
 * $50F1 2000, sound $50F1 4000, IWM $50F1 6000), and in 24-bit mode
 * through $F0 0000 -> $5000 0000.
 *
 * Livadix's model (†: not in the documents):
 *   - RAM: the size bits make bank A's window W = 1, 4, 16 or 64 MB
 *     (the ROM's own table, ROM offset $36BA); the GLUE decodes the RAM
 *     space modulo 2W, bank A in its first half, bank B in its second, each
 *     repeating its contents (a bank of smaller RAM ICs than the bits say
 *     ignores the high row and column bits). The ROM sizes the RAM exactly
 *     that way (ROM offset $35A4: bank A's ICs from where a write at 32, 8
 *     and 2 MB shows at 0, then the total from the first address of a list
 *     that shows at an earlier one). An empty bank B floats (the cycle
 *     completes, reads 0)†. Bank A holds the larger SIMMs.
 *   - The I/O block repeats every $2 0000 through $5000 0000-$50FF FFFF
 *     (A23-A17 ignored: the ROMs' $50F0 0000 image and the 24-bit $F0 0000
 *     -> $5000 0000 both reach it)†; an address of the block no device
 *     decodes completes and reads 0 ("The GLUE responds to any I/O device
 *     address with a /DSACK0 signal")†.
 *   - The reset overlay: from reset the ROM answers through the RAM space
 *     until VIA1 PA4 is driven low (the pin floats high while it is an
 *     input, so the overlay is on after reset).
 *
 * Livadix's own code. */
#ifndef MC_GLUE_H
#define MC_GLUE_H

#include "../bus.h"

#include <stdint.h>

#define MC_GLUE_RAM_SPACE 0x40000000u     /* $0000 0000-$3FFF FFFF */
#define MC_GLUE_ROM 0x40000000u
#define MC_GLUE_ROM_SPACE 0x10000000u     /* $4000 0000-$4FFF FFFF */
#define MC_GLUE_IO 0x50000000u
#define MC_GLUE_IO_SIZE 0x01000000u       /* the decoded I/O space, $5000 0000-$50FF FFFF */
#define MC_GLUE_IO_IMAGE 0x20000u         /* the block repeats every $2 0000 (†) */
#define MC_GLUE_UNDECODED 0x51000000u     /* $5100 0000-$5FFF FFFF: no /DSACK, a bus error */
#define MC_GLUE_NUBUS_LOCAL 0xF0000000u   /* $F000 0000-$F0FF FFFF: a bus error (Table 3-10) */

/* Offsets in the I/O block. */
enum {
    MC_GLUE_VIA1 = 0x00000,
    MC_GLUE_VIA2 = 0x02000,
    MC_GLUE_SCC = 0x04000,
    MC_GLUE_SCSI_DRQ = 0x06000,      /* pseudo-DMA with the DRQ handshake (SCSIHsk) */
    MC_GLUE_SCSI = 0x10000,          /* normal mode: the 5380's registers (SCSIBase) */
    MC_GLUE_SCSI_BLIND = 0x12000,    /* pseudo-DMA without the handshake (SCSIDMA) */
    MC_GLUE_SOUND = 0x14000,
    MC_GLUE_IWM = 0x16000
};

typedef struct mc_glue {
    uint32_t bank_a, bank_b;    /* populated sizes in bytes (bank B may be 0) */
    uint8_t size_bits;          /* VIA2 PA7/PA6 as the pins drive them, bits 1-0 */
    int overlay;                /* the ROM overlay map */
} mc_glue;

/* The RAM sizes (MiB) four SIMMs per bank give, at least Livadix's 4 MiB,
 * 0-terminated; mc_glue_banks gives each bank's share. */
extern const uint16_t mc_glue_ram_mb[];
int mc_glue_banks(uint32_t ram_mb, uint32_t *bank_a, uint32_t *bank_b);

void mc_glue_init(mc_glue *g, uint32_t bank_a, uint32_t bank_b);
/* Bank A's window for the size bits (Table 4-10: 256 Kbit, 1, 4, 16 Mbit
 * ICs, four 8-chip SIMMs). */
static inline uint32_t mc_glue_window(const mc_glue *g) { return 0x100000u << (2u * (g->size_bits & 3u)); }
/* The RAM space as the GLUE decodes it now: the overlay's ROM, else the
 * banks (hw: ram holds bank A then bank B). */
void mc_glue_map_ram(const mc_glue *g, mc_bus *bus, uint8_t *ram, const uint8_t *rom, uint32_t rom_size);
/* What never changes: the ROM's own space, the undecoded I/O space and
 * the NuBus range the processor cannot reach. */
void mc_glue_map_fixed(const mc_glue *g, mc_bus *bus, const uint8_t *rom, uint32_t rom_size);
/* VIA2 PA7/PA6 changed (the levels the pins have): 1 when the window
 * changed (the RAM must be mapped again). */
int mc_glue_set_size_bits(mc_glue *g, uint8_t pa);

#endif
