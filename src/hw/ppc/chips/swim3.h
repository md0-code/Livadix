/* Livadix — SWIM III, the floppy disk controller of the Power Macintosh 6100,
 * 7100 and 8100, with one or two Apple SuperDrives; sector by sector.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994), chapter
 * 2, "SWIM III Floppy Disk Drive Controller" (an extension of SWIM II: DMA
 * transfers, no need to mask interrupts during disk accesses, GCR and MFM
 * on 1.44 MB disks, the manual-inject drive), Figure 2-2 (its interrupt on
 * the pseudo-VIA2's flag bit 5); "Floppy Disk Drive Connection". The
 * drive's side as for the SWIM (swim.h: Apple 800K Double-Sided Drive ERS
 * 699-0452-A, Table 1, the status and command registers addressed by SEL,
 * CA2, CA1, CA0, commands latched on LSTRB; the SuperDrive's MFM
 * registers).
 *
 * The registers as ROM 9FEB69B3's floppy driver uses them (base $50F1 6000 from the PDM decoder table, register n at
 * +n x $200; ROM addresses at $FFC0 0000):
 *   2  error (read; the driver reads it to clear before each operation)
 *   3  parameter (written $95)†, kept
 *   4  phase: bits 2-0 the drive's CA2-CA0, bit 3 LSTRB (a command is a
 *      pulse of bit 3, $FFD90928-$FFD90964); read back as written (the
 *      driver's register test writes 5, 6, 7 and compares, $FFD90880)
 *   5  setup: bit 2 set for GCR ($4C), clear for MFM ($28) (the driver's
 *      table at $FFD908DC; MFM sector numbers start at 1, $FFD8FD6C); kept
 *   6  write: clears the mode bits written; read: the mode
 *   7  write: sets the mode bits written; read: bit 2 the drive's sense
 *      line (RD), others 0† ($FFD90918)
 *      Mode bits: 1 and 2 enable drive 1 and drive 2 ($FFD9096E), 3 GO
 *      (start the operation), 4 write, 5 the drive's SEL (head) line, 7
 *      seek; bit 0 set at the driver's start, bit 6 around its scans: kept†
 *   8  interrupt flags (read clears them): bit 1 seek done, 2 an address
 *      field (sector header) seen, 3 the sector transfer done; bits 0 and 4
 *      kept for errors† (the driver's table $FFD90B98 dispatches bits 0-4)
 *   9  steps to go (written before a seek in the direction the drive's
 *      DIRTN latch holds, $FFD907D0-$FFD907FA; reads what is left)
 *   10 the track under the head, 11 the sector, 12 kept (read with them
 *      after an address field, $FFD8F9FA-$FFD8FA06)
 *   13 the first sector to transfer, 14 how many (the driver gives 1)
 *   15 interrupt enables (the driver sets bit n for the operation and
 *      clears it in its handler)
 * An operation: the driver writes the interrupt bit to 15 and the mode
 * bits to 7 ($FFD90B04: $80 with bit 1 for a seek, $08 with bit 3 for a
 * transfer, $08 with bit 2 for an address field); 15 resets clear. Data
 * moves through the AMIC's floppy DMA channel: SWIM III asserts its DRQ
 * line while it has a byte to give or wants one (mc_swim3_dma_read /
 * _write are the channel's port). The interrupt goes to the pseudo-VIA2's
 * bit 5 and the AMIC floppy channel's (DMA flag 6, the driver's handler
 * $FFD8EB48).
 * Encodings: a disk gives address fields only in its own encoding (setup
 * bit 2), so the driver finds a GCR disk after its MFM tries. A read moves
 * MFM sectors as their 512 bytes, GCR ones as the sector number and the
 * data field's 703 nibbles as 6-bit values (12 tag and 512 data bytes in
 * 6-and-2 with their checksum; the driver decodes them, $FFD90004-
 * $FFD900B0). A write is the driver's stream ($FFD900F4-$FFD901F0): $99
 * escapes a code: $A1, $C2, $FB, $FE, $FC marks, $0F before an MFM data
 * field's 512 bytes (taken as they come), $04 its CRC, $08 the end; GCR
 * self-sync bytes, $D5 $AA, then 6-bit values: $0B (the $AD mark), the
 * sector number, the data field's 703 values, $27 $AA (the $DE $AA
 * bit-slip). Livadix keeps each sector's data from it†.
 * Timing (Livadix's choice†): a step 3 ms, an address field or a sector 2 ms.
 *
 * Output lines (1 = asserted): MC_SWIM3_LINE_IRQ, MC_SWIM3_LINE_DRQ.
 *
 * Livadix's own code. */
#ifndef MC_SWIM3_H
#define MC_SWIM3_H

#include "hw.h"
#include "disk.h"
#include "floppy_track.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_SWIM3_LINE_IRQ = 0, MC_SWIM3_LINE_DRQ = 1 };

#define MC_SWIM3_SIZE 0x2000u
#define MC_SWIM3_DRIVES 2

enum {
    MC_SWIM3_INT_SEEK = 0x02, MC_SWIM3_INT_HEADER = 0x04, MC_SWIM3_INT_DONE = 0x08
};

typedef struct mc_swim3_drive {
    mc_disk *disk;
    mc_floppy_format format;
    uint8_t protect, ejected;
    uint8_t track, dirtn, motor, eject_latch, mfm;
    uint8_t next_sector;        /* the address field coming round */
} mc_swim3_drive;

typedef struct mc_swim3 {
    mc_dev dev;
    uint8_t reg[16];            /* as written */
    uint8_t mode, intr, error;
    uint8_t irq, drq;
    unsigned drives;
    mc_swim3_drive drive[MC_SWIM3_DRIVES];
    /* The operation in progress. */
    uint8_t op;                 /* 0 none, 1 seek, 2 header, 3 read, 4 write */
    int op_drive;
    uint64_t due;
    uint8_t buf[36 * 512];
    uint32_t buf_n, buf_i;
    unsigned nsec;              /* the sectors of the operation */
    uint8_t wstream[8192];      /* a write's byte stream from the DMA */
    uint32_t wn, raw_left;
    uint8_t esc, raw_pending, wend;
    unsigned shift;             /* register n at offset n << shift (9: $200 steps) */
} mc_swim3;

extern const mc_dev_ops mc_swim3_ops;

void mc_swim3_init(mc_swim3 *s, const mc_hw_host *host, const char *name, unsigned drives);
/* The registers' spacing: register n at offset n << shift. 9 ($200 steps,
 * the PDM boards' $50F1 6000 window) unless set; Grand Central has them at
 * $10 steps (4: the swim3 node's registers at + $15000). */
void mc_swim3_set_stride(mc_swim3 *s, unsigned shift);
/* Puts a disk image in a drive (raw or DiskCopy 4.2 of 400, 720, 800 or
 * 1440 KiB). */
int mc_swim3_insert(mc_swim3 *s, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity);
mc_disk *mc_swim3_media(const mc_swim3 *s, unsigned drive, int *ejected);
mc_disk *mc_swim3_remove(mc_swim3 *s, unsigned drive);
/* The AMIC's floppy channel (mc_amic_port). */
int mc_swim3_dma_read(void *s, uint8_t *byte);
int mc_swim3_dma_write(void *s, uint8_t byte);

#endif
