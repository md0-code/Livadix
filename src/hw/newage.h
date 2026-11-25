/* Livadix — New Age, the floppy disk controller of the Quadra 840AV and
 * Centris 660AV.
 *
 * Quadra 840AV and Centris 660AV Developer Note (Apple, 1993): chapter 2,
 * "New Age" (controls an Apple SuperDrive at all its densities, a command
 * set for MFM and Apple GCR, an interrupt on disk insertion, a 16-byte FIFO,
 * DMA), Table 2-10 (the PSC's FDC DMA channel, 16 bits, 16 bytes), chapter
 * 12 "New Age Floppy Disk Driver" (800K GCR and SuperDrive formats; one
 * error for a bad address mark; noNybErr for a time-out waiting for the
 * controller).
 *
 * What the ROM's driver (ROM 5BF10FD1 offsets $18E8B8-$1910BA) does with the
 * chip, the binding evidence: two byte registers at
 * $50F2 A101 and $50F2 A141 (stride $40, byte lane 1). $101 read is a main
 * status register and $101 write a data-rate select; $141 is the data FIFO
 * through which commands go in and results come out, one byte at a time
 * whenever the status says the controller is ready (bit 7) and which way
 * the byte goes (bit 6: to the processor), bit 4 marking a command in
 * progress: the protocol of the Intel 82077AA floppy controller (82077AA
 * data sheet, "Main Status Register", "Datarate Select Register", "FIFO",
 * "Command Set"). Its commands are the 82077AA's where the driver sends
 * them (SPECIFY $03, SENSE DRIVE STATUS $04, RECALIBRATE $07, SENSE
 * INTERRUPT STATUS $08, SEEK $0F, CONFIGURE $13, READ ID $0A/$4A, READ DATA
 * $06/$46, WRITE DATA $05/$45, FORMAT $0D/$4D, with bit 6 MFM and GCR
 * without it), and Apple's own beside them ($1A/$9A, $1B/$9B, $1C, $12, $32,
 * $3F, $52, $5C)†. A write of the data-rate register with bit 7 set resets
 * the chip.
 *
 * Interrupt (MC_NEWAGE_LINE_IRQ): the driver enables it as bit 5 of VIA2's
 * interrupt enable register ($50F0 3C00 writes $A0/$20).
 *
 * The same window's $080-$0FF hold status longwords the DMA serial driver
 * reads ($090, $0A0, $0B0, $0D0, $0E0; ROM offsets $1850B8, $186528); it
 * waits for bit 30 of $090 after resetting the transmitter ($1851D0)†:
 * read here as always set.
 *
 * Apple's side, as the driver uses it:
 *   - SENSE INTERRUPT STATUS answers ST0 alone, and the present cylinder
 *     after it only when ST0 has seek end (bit 5): the driver reads the
 *     second byte only then ($190F64) and one byte in its interrupt handler
 *     ($190E6A). With nothing to report it answers $80;
 *   - ST0 bits 7-6 = 11 report a disk inserted (bit 1 clear) or removed
 *     (bit 1 set) in the drive of bit 0; the handler posts the disk-inserted
 *     event ($190EC6-$190F24). A command's completion is reported before a
 *     change, and a change only while the controller is idle;
 *   - SENSE DRIVE STATUS (ST3): bits 3 and 2 a SuperDrive (bit 3 alone a
 *     2.88 MB drive, neither an 800K drive; $18E96E), bit 6 the disk
 *     writable (clear: locked, $190BB4), bit 7 a double-density disk, bit 1
 *     a high-density one, neither an extra-density one ($19071C: the format
 *     tables at $19047E-$1904A4);
 *   - READ ID in GCR returns the address field's format byte as N ($02 a
 *     400K disk, $22 an 800K one, $190898); in MFM the sectors come round
 *     as the disk turns, the driver counting up to the highest for 720K
 *     (9) or 1440K (18) ($190A2C); GCR sectors count from 0, MFM from 1;
 *   - READ DATA, WRITE DATA and FORMAT as the 82077AA's, the data (512
 *     bytes a sector, no GCR tags) or FORMAT's four ID bytes a sector
 *     through the PSC's FDC channel; their seven result bytes clear the
 *     interrupt as they are read ($190FC6). The driver's error map
 *     ($190C68) reads ST1 bit 1 as locked and bit 0 as no address mark;
 *   - Apple's commands with their drive byte: $9A motor on, $1A motor off,
 *     $9B (before each new look at a disk; an interrupt), $1B (no
 *     interrupt), $1C GCR mode and $5C MFM mode (an interrupt), $52 eject
 *     (an interrupt when the disk is out), $12 and $32 with one byte (no
 *     result; $12 3 for 2.88 MB disks).
 *
 * This model: one or two SuperDrives (MC_NEWAGE_DRIVES) over disk images
 * (400K, 800K, 720K, 1440K), sector by sector (the GCR and MFM encodings
 * are the controller's own business: the driver sees sectors only); a
 * motion or data command completes 1 ms of guest time after its last byte;
 * the data rate is not checked against the disk. */
#ifndef MC_NEWAGE_H
#define MC_NEWAGE_H

#include "hw.h"
#include "disk.h"
#include "floppy_track.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_NEWAGE_LINE_IRQ = 0, MC_NEWAGE_LINE_DRQ = 1 };

#define MC_NEWAGE_SIZE 0x200u

#define MC_NEWAGE_DRIVES 2

typedef struct mc_newage_drive {
    mc_disk *disk;
    mc_floppy_format format;
    uint8_t protect, ejected;   /* ejected: the driver took the disk out */
    uint8_t change;             /* 1 inserted, 2 removed: to report */
    uint8_t pcn;                /* present cylinder */
    uint8_t motor;
    unsigned turn;              /* READ ID: the sector coming round */
} mc_newage_drive;

typedef struct mc_newage {
    mc_dev dev;
    uint8_t dsr;                /* data-rate select (as written, bit 7 cleared) */
    uint8_t cmd[16];
    unsigned cmd_n, cmd_len;
    uint8_t res[16];
    unsigned res_n, res_i;
    uint8_t irq, drq;           /* lines */
    uint8_t int_pending;        /* a SENSE INTERRUPT STATUS has a status to report */
    uint8_t st0, pcn;
    uint8_t result_irq;         /* a result phase's interrupt (cleared by reading it) */
    uint8_t drives;             /* drives fitted (0: none) */
    /* The command executing: 0 none; else its first byte, done at `due`. */
    uint8_t exec[16];
    uint8_t busy;               /* executing (CB, no RQM) */
    uint8_t seeking;            /* a motion command runs: MSR drive-busy bits */
    uint8_t xfer;               /* data phase: 1 to memory, 2 from memory, 3 format IDs */
    uint32_t buf_n, buf_i;
    uint8_t buf[18 * 512];
    mc_newage_drive drive[MC_NEWAGE_DRIVES];
} mc_newage;

void mc_newage_init(mc_newage *n, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_newage_ops;

/* Fits `drives` SuperDrives (0-2), empty. */
void mc_newage_fit(mc_newage *n, unsigned drives);
/* Puts a disk image in a drive (raw or DiskCopy 4.2 of 400, 720, 800 or
 * 1440 KiB); the controller reports the insertion. */
int mc_newage_insert(mc_newage *n, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity);
/* The disk in a drive (NULL when empty); `ejected` set when the driver
 * ejected it. */
mc_disk *mc_newage_media(const mc_newage *n, unsigned drive, int *ejected);
/* Takes the disk out; returns it. */
mc_disk *mc_newage_remove(mc_newage *n, unsigned drive);
/* The PSC's FDC channel (mc_psc_port): a byte to memory, a byte from it. */
int mc_newage_dma_read(void *n, uint8_t *byte);
int mc_newage_dma_write(void *n, uint8_t byte);

#endif
