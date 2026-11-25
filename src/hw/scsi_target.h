/* Livadix — SCSI targets: the direct-access disk (scsi_disk.c) and the CD-ROM
 * (scsi_cd.c) on a virtual disk (storage/vdisk.h), per SCSI-2 (X3.131-1994).
 *
 * Commands (SCSI-2 section numbers): TEST UNIT READY (8.2.16), REZERO UNIT
 * (9.2.13), REQUEST SENSE (8.2.14), FORMAT UNIT (9.2.1, no-op), READ(6)
 * (9.2.5), WRITE(6) (9.2.20), SEEK(6)/SEEK(10) (9.2.15), INQUIRY (8.2.5),
 * MODE SELECT(6)/(10) (8.2.8, 8.2.9), RESERVE/RELEASE (9.2.12, 9.2.11),
 * MODE SENSE(6)/(10) (8.2.10, 8.2.11), START STOP UNIT (9.2.17), SEND
 * DIAGNOSTIC (8.2.15), PREVENT ALLOW MEDIUM REMOVAL (9.2.4), READ CAPACITY
 * (9.2.7), READ(10) (9.2.6), WRITE(10) (9.2.21), WRITE AND VERIFY (9.2.22),
 * VERIFY (9.2.19), SYNCHRONIZE CACHE (9.2.18), READ(12)/WRITE(12) (the
 * twelve-byte forms of chapters 14 and 16). CD-ROM: READ TOC (14.2.11), READ
 * SUB-CHANNEL (14.2.10), PLAY AUDIO(10), (12), MSF and TRACK INDEX (14.2.2-
 * 14.2.5), PAUSE/RESUME (14.2.1), STOP PLAY/SCAN (MMC), the
 * block length switch through the mode parameter block descriptor (8.3.3,
 * Table 93) between 512 bytes, the default the Quadra 700 ROM boots from
 * (scsi_cd.c), and 2048, read-only, eject. Mode pages (8.3.3, 9.3.3, 14.3.3):
 * 01h read-write error recovery (9.3.3.6), 03h format device (9.3.3.3), 04h
 * rigid disk drive geometry (9.3.3.7), 08h caching (9.3.3.1), CD 0Dh CD-ROM
 * device parameters (14.3.3.2) and 0Eh CD-ROM audio control (14.3.3.1); 3Fh
 * returns them all.
 *
 * INQUIRY reports vendor "LIVADIX" and products "VIRTUAL DISK" or "VIRTUAL
 * CD-ROM" (Livadix's own strings), ANSI version 2. Fixed-format sense data
 * (8.2.14, 18 bytes). Unit attention: a CD reports 28h (not ready to ready
 * transition) after a disc is inserted; no target reports power-on or reset
 * attention, so the ROM's first command succeeds. */
#ifndef MC_SCSI_TARGET_H
#define MC_SCSI_TARGET_H

#include "scsi_bus.h"
#include "vdisk.h"

#include <stdint.h>

/* Sense keys (SCSI-2 8.2.14.3, Table 69). */
enum {
    MC_SENSE_NONE = 0, MC_SENSE_NOT_READY = 2, MC_SENSE_MEDIUM_ERROR = 3, MC_SENSE_HARDWARE_ERROR = 4,
    MC_SENSE_ILLEGAL_REQUEST = 5, MC_SENSE_UNIT_ATTENTION = 6, MC_SENSE_DATA_PROTECT = 7
};

/* The CD-ROM drive's block length at power-on, reset and insertion when the
 * disc does not say otherwise (scsi_cd.c). */
#define MC_SCSI_CD_BLOCK 512u

/* What the data phase of the command in progress carries. */
enum { MC_UNIT_NONE, MC_UNIT_READ, MC_UNIT_WRITE, MC_UNIT_RESPONSE, MC_UNIT_PARAMS, MC_UNIT_DISCARD };

typedef struct mc_scsi_unit mc_scsi_unit;
/* The guest ejected the medium: `media` is no longer the unit's; the
 * machine closes it (and its images). */
typedef void (*mc_scsi_eject_fn)(void *ctx, mc_scsi_unit *unit, mc_vdisk *media);

struct mc_scsi_unit {
    mc_scsi_target target;      /* first: what the bus calls */
    int cd;                     /* peripheral device type 5, removable, read-only */
    mc_vdisk *media;            /* NULL: no medium */
    uint32_t block;             /* logical block length: 512, or 2048/512 on a CD */
    uint32_t cd_block;          /* CD only: the length at power-on, reset and insertion (scsi_cd.c) */
    int prevent;                /* PREVENT ALLOW MEDIUM REMOVAL */
    uint8_t ua_asc;             /* pending unit attention (additional sense code), 0 none */
    uint8_t key, asc, ascq, info_valid;
    uint32_t info;
    /* The command in progress. */
    uint8_t status;
    int mode;                   /* MC_UNIT_* */
    uint64_t lba;
    uint32_t length;
    uint8_t data[512];          /* responses and parameter lists */
    mc_scsi_eject_fn on_eject;
    void *eject_ctx;
    /* CD only: commands beyond the common set (READ TOC). Returns -1 when
     * the opcode is not one of them. */
    int (*extra)(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes);
    int apple_cd;               /* CD only: Apple identification mode page $30 */
    int read_cd;                /* CD only: READ CD (BEh) of an ATAPI drive (SFF-8020i), scsi_cd.c */
    uint8_t volume[2];          /* CD only: the audio control page's port 0 and 1 volumes */
    int audio_reported;         /* CD only: a completed play's status was reported (SCSI-2 14.2.10) */
};

/* A disk unit on `media` (512-byte blocks; may be NULL until attached). */
void mc_scsi_disk_init(mc_scsi_unit *u, mc_vdisk *media);
/* An empty generic CD-ROM drive (512-byte blocks until configured). */
void mc_scsi_cd_init(mc_scsi_unit *u);
/* The internal Pippin drive: native optical sectors and mode page $30. */
void mc_scsi_cd_init_pippin(mc_scsi_unit *u);
/* Inserts a disc (the vdisk of a CD image); the drive reports a unit
 * attention. Returns 0 when a disc is already in. */
int mc_scsi_cd_insert(mc_scsi_unit *u, mc_vdisk *media);
/* Removes the disc from the host side (returns it; NULL when empty). */
mc_vdisk *mc_scsi_unit_remove(mc_scsi_unit *u);
void mc_scsi_unit_on_eject(mc_scsi_unit *u, mc_scsi_eject_fn fn, void *ctx);
/* The unit as the bus sees it (mc_scsi_bus_attach). */
static inline mc_scsi_target *mc_scsi_unit_target(mc_scsi_unit *u) { return &u->target; }

/* Shared between scsi_disk.c and scsi_cd.c. */
void mc_scsi_unit_sense(mc_scsi_unit *u, uint8_t key, uint8_t asc, uint8_t ascq);
uint32_t mc_scsi_unit_capacity(const mc_scsi_unit *u);  /* logical blocks */
/* A READ of `count` logical blocks from `lba` (the checks of READ(10)). */
uint8_t mc_scsi_unit_read(mc_scsi_unit *u, uint64_t lba, uint32_t count, int *direction, uint32_t *bytes);
void mc_scsi_unit_init_common(mc_scsi_unit *u);

#endif
