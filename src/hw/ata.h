/* Livadix — the ATA (IDE) controller with its two devices: the task file of ATA-2 (X3.279-1996) with a fixed disk on the virtual
 * disk (storage/vdisk.h) and an ATAPI CD-ROM (SFF-8020i) on the SCSI CD-ROM
 * command logic (scsi_target.h), as the Quadra 630 / LC 580 (F108) and the
 * PowerBook 150 and 190 use them.
 *
 * Registers (ATA-2 clause 7, Table 1 task file; the register selectors of the
 * ATA Manager's ATA_RegAccess, Apple's ATA Device Software Guide Table 3-3):
 *   0 data (16 bits), 1 error / features, 2 sector count, 3 sector number,
 *   4 cylinder low, 5 cylinder high, 6 drive/head, 7 status / command,
 *   14 alternate status / device control, 15 drive address.
 * ops->read/write take a byte offset; the register is (offset >> reg_shift)
 * & 15, with reg_shift 2 for the F108's register stride of 4†. Byte accesses
 * address one register; the data register takes 16-bit accesses (a 32-bit
 * access is two words, high word first).
 *
 * Data register lanes: the ATA Manager sends bytes "in the LSB-MSB format
 * compatible with PC clones" unless ByteSwap is set (Guide, Table 3-1), so a
 * word moved between the register and big-endian memory must carry the first
 * byte of the sector in bits 15..8 of the 68k word. `lane_swap` (default 1,
 * set at init) gives the register that view; with 0 a 16-bit access sees the
 * ATA word (first byte in bits 7..0). mc_ata_data_read16/write16 are the
 * ATA word, whatever the wiring.
 *
 * Output pin: INTRQ is the mc_hw_host line MC_ATA_LINE_INTRQ (state 1 =
 * asserted), gated by nIEN (device control bit 1: with nIEN set the pin is
 * negated, ATA-2 7.2.7), cleared by a status read or a command write.
 *
 * Time: guest time, C15M periods. A command sets BSY at once; the data or the
 * completion follows after the delays MC_ATA_T_* below (Livadix's choices: ATA
 * gives maxima, not typical values); the machine schedules the one deadline
 * the chip arms and calls ops->event.
 *
 * Per-machine wiring (5.2h's machines † until their ROMs are read):
 *   Quadra 630 / LC 580 (F108, asic/f108.h; ROMs 06684214 and 064DC91D):
 *     task file at $50F1A000, register stride 4 (reg_shift 2), alternate
 *     status / device control at register 14 (+$38); the data register
 *     byte-swapped (lane_swap 1: the Developer Note's DD0-7 on IOD24-31);
 *     INTRQ in bit 5 of the F108's interrupt byte at $50F1A101, which the
 *     ATA Manager polls and the VIA2 slot handler (level 2, VIA2 CA1)
 *     dispatches as slot $D. The 53C96 is the external SCSI bus; the ROMs'
 *     ATA Manager 1.0 drives the master device only and has no packet
 *     protocol, so no ATAPI device is fitted there.
 *   PowerBook 150 (ROM FDA22562): task file at $50F1A000, stride 4, as
 *     the F108's (the ATA Manager waits on register 14, ROM offset
 *     $8D0A80); INTRQ not wired (the ROM polls)†; one hard disk.
 *   PowerBook 190 (ROM 4D27039C): task file at $50F1A000, stride 4; an
 *     interrupt status word at +$D4 (bit 1 INTRQ) and INTRQ as slot $C's
 *     interrupt on VIA2 (machines/powerbook500.c); one hard disk. The
 *     expansion bay's ATA bus is a second chip, its task file from +$80
 *     (+$D4 bit 0 its INTRQ): an ATAPI CD-ROM drive as its master when
 *     the bay holds the CD-ROM module.
 *   The ATA Manager addresses devices by ataPBDeviceID: devNum 0 or 1 (the
 *     DEV bit of the drive/head register), busNum the bus (Guide, p. 3-24).
 *
 * Commands (ATA-2 clause 8 and 9 unless noted): READ SECTOR(S) 20h/21h, WRITE
 * SECTOR(S) 30h/31h, READ VERIFY SECTOR(S) 40h/41h, READ MULTIPLE C4h, WRITE
 * MULTIPLE C5h, SET MULTIPLE MODE C6h, INITIALIZE DEVICE PARAMETERS 91h,
 * IDENTIFY DEVICE ECh, SET FEATURES EFh, IDLE E3h, IDLE IMMEDIATE E1h, STANDBY
 * E2h, STANDBY IMMEDIATE E0h, CHECK POWER MODE E5h, SLEEP E6h, EXECUTE DEVICE
 * DIAGNOSTIC 90h, RECALIBRATE 1xh, SEEK 7xh, READ BUFFER E4h, WRITE BUFFER E8h;
 * FLUSH CACHE E7h (ATA-3 draft X3T13/2008D†). ATAPI (SFF-8020i): PACKET A0h,
 * IDENTIFY PACKET DEVICE A1h, DEVICE RESET 08h. DMA commands, long and 48-bit
 * commands abort. */
#ifndef MC_ATA_H
#define MC_ATA_H

#include "hw.h"
#include "scsi_target.h"
#include "vdisk.h"

#include <stdint.h>

enum { MC_ATA_LINE_INTRQ = 0 };

/* Task file registers (offset >> reg_shift). */
enum {
    MC_ATA_DATA = 0, MC_ATA_ERROR = 1, MC_ATA_FEATURES = 1, MC_ATA_COUNT = 2, MC_ATA_SECTOR = 3, MC_ATA_CYL_LO = 4,
    MC_ATA_CYL_HI = 5, MC_ATA_DRIVE = 6, MC_ATA_STATUS = 7, MC_ATA_COMMAND = 7, MC_ATA_ALT_STATUS = 14,
    MC_ATA_DEV_CONTROL = 14, MC_ATA_DRIVE_ADDRESS = 15
};

/* Status register bits (ATA-2 7.2.13). */
enum {
    MC_ATA_ST_ERR = 0x01, MC_ATA_ST_IDX = 0x02, MC_ATA_ST_CORR = 0x04, MC_ATA_ST_DRQ = 0x08, MC_ATA_ST_DSC = 0x10,
    MC_ATA_ST_DWF = 0x20, MC_ATA_ST_DRDY = 0x40, MC_ATA_ST_BSY = 0x80
};
/* Error register bits (ATA-2 7.2.2); ATAPI error register (SFF-8020i 5.x): sense key in bits 7..4, ABRT bit 2. */
enum {
    MC_ATA_ER_AMNF = 0x01, MC_ATA_ER_TK0NF = 0x02, MC_ATA_ER_ABRT = 0x04, MC_ATA_ER_MCR = 0x08, MC_ATA_ER_IDNF = 0x10,
    MC_ATA_ER_MC = 0x20, MC_ATA_ER_UNC = 0x40, MC_ATA_ER_BBK = 0x80
};
/* Device control bits (ATA-2 7.2.7). */
enum { MC_ATA_DC_NIEN = 0x02, MC_ATA_DC_SRST = 0x04 };
/* ATAPI interrupt reason (the sector count register; SFF-8020i): CoD, IO, REL. */
enum { MC_ATAPI_COD = 0x01, MC_ATAPI_IO = 0x02, MC_ATAPI_REL = 0x04 };

/* Times in C15M periods (15.6672 MHz): 1 us = 15.67 periods. Livadix's choices. */
#define MC_ATA_T_CMD        314u        /* 20 us: command accepted to its first action */
#define MC_ATA_T_FIRST      3134u       /* 200 us: first sector of a read */
#define MC_ATA_T_SECTOR     314u        /* 20 us: each further block */
#define MC_ATA_T_WRITE      784u        /* 50 us: a block written */
#define MC_ATA_T_RESET      31334u      /* 2 ms: soft reset to the signature */
#define MC_ATA_T_SPINUP     78336u      /* 5 ms: standby to active */
#define MC_ATA_T_PACKET     784u        /* 50 us: PACKET to the request for the command packet */
#define MC_ATA_T_EXEC       3134u       /* 200 us: an ATAPI command executed */

#define MC_ATA_SECTOR_BYTES 512u
#define MC_ATA_MAX_MULTIPLE 16u
#define MC_ATA_BUFFER_BYTES 65536u

typedef enum { MC_ATA_NONE, MC_ATA_DISK, MC_ATA_PACKET } mc_ata_kind;

typedef struct mc_ata mc_ata;
typedef void (*mc_ata_eject_fn)(void *ctx, unsigned dev, mc_vdisk *media);

typedef struct mc_ata_unit {
    mc_ata_kind kind;
    mc_vdisk *media;
    /* ATA disk geometry. */
    uint32_t cylinders, heads, sectors;         /* the default translation */
    uint32_t cur_cylinders, cur_heads, cur_sectors;     /* after INITIALIZE DEVICE PARAMETERS */
    uint8_t multiple;                           /* SET MULTIPLE MODE count, 0 none */
    uint8_t write_cache, look_ahead;            /* SET FEATURES 02h/82h, 55h/AAh */
    uint8_t pio_mode;                           /* SET FEATURES 03h: the PIO transfer mode set */
    uint8_t power;                              /* CHECK POWER MODE: 0x00 standby, 0x80 idle, 0xFF active */
    uint8_t sleeping;
    /* Registers as the host reads them. */
    uint8_t status, error, count, sector, cyl_lo, cyl_hi;
    /* ATAPI. */
    mc_scsi_unit scsi;
    char serial[21];
} mc_ata_unit;

struct mc_ata {
    mc_dev dev;
    unsigned reg_shift;
    int lane_swap;
    uint8_t features, devctl, drive;
    unsigned selected;                          /* the DEV bit */
    mc_ata_unit unit[2];
    mc_ata_eject_fn on_eject;
    void *eject_ctx;
    /* The command in progress. */
    int phase;
    unsigned cur;
    uint8_t cmd;
    uint32_t sectors_left;                      /* ATA: sectors still to transfer after this block */
    uint64_t lba;                               /* ATA: first sector of the next block */
    uint32_t block_sectors;                     /* ATA: sectors per DRQ block (1 or the multiple count) */
    uint32_t pos, len;                          /* bytes moved / in the current DRQ block */
    uint32_t pkt_limit;                         /* ATAPI byte count limit of the command */
    uint32_t pkt_total, pkt_done;               /* ATAPI data phase bytes: all / moved to or from the host */
    uint32_t pkt_fetched;                       /* ATAPI: bytes taken from the target (data in) */
    int pkt_dir;                                /* MC_SCSI_XFER_* */
    uint8_t pkt_status;
    uint8_t stage[4096];                        /* ATAPI data in: bytes fetched from the target, not yet handed out */
    uint32_t stage_pos, stage_len;
    uint8_t packet[16];
    uint64_t deadline;
    int intrq, pin;
    uint8_t buffer[MC_ATA_BUFFER_BYTES];
    uint8_t sector_buffer[MC_ATA_SECTOR_BYTES]; /* READ BUFFER / WRITE BUFFER */
};

/* The chip with no device; `reg_shift` is the register spacing (2: stride 4). */
void mc_ata_init(mc_ata *a, const mc_hw_host *host, unsigned reg_shift);
/* The machine's hard reset (power on): both devices reset to their signature. */
void mc_ata_reset(mc_ata *a);
/* A hard disk as device 0 or 1 (a NULL medium leaves it absent). */
void mc_ata_attach_disk(mc_ata *a, unsigned dev, mc_vdisk *media);
/* An ATAPI CD-ROM drive (empty) as device 0 or 1. */
void mc_ata_attach_cd(mc_ata *a, unsigned dev);
/* Inserts a disc (the drive reports a unit attention to the next command);
 * returns 0 when a disc is already in. */
int mc_ata_cd_insert(mc_ata *a, unsigned dev, mc_vdisk *media);
/* Takes the disc out from the host side (NULL when empty). */
mc_vdisk *mc_ata_cd_remove(mc_ata *a, unsigned dev);
/* The guest ejected a disc (START STOP UNIT): `media` is no longer the drive's; the machine closes it. */
void mc_ata_on_eject(mc_ata *a, mc_ata_eject_fn fn, void *ctx);

/* The task file as the ATA word-wide data register and the byte registers. */
uint32_t mc_ata_reg_read(mc_ata *a, unsigned reg, unsigned size);
void mc_ata_reg_write(mc_ata *a, unsigned reg, unsigned size, uint32_t value);
uint16_t mc_ata_data_read16(mc_ata *a);
void mc_ata_data_write16(mc_ata *a, uint16_t word);

/* The machine's view (the mc_dev_ops of the chip; a.dev.ops is set by init). */
extern const mc_dev_ops mc_ata_ops;

#endif
