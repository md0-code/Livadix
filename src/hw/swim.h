/* SWIM floppy disk controller (Sander-Wozniak Integrated Machine) with up to
 * two Apple 3.5-inch drives (SuperDrive: GCR 400K/800K and MFM 720K/1440K).
 *
 * The SWIM holds two register sets: the IWM (sixteen state addresses that
 * set eight latches: PHASE0-3 = CA0, CA1, CA2, LSTRB; MotorOn = drive
 * enable; drive select; L6 = Q6; L7 = Q7) and the ISM (sixteen registers:
 * writes at 0-7, reads at 8-15). Software switches IWM -> ISM by writing the
 * IWM mode register four times with bit 6 = 1, 0, 1, 1, and back by clearing
 * ISM mode bit 6.
 *
 * Machine wiring (Quadra 700, the Developer Note's
 * address map: SWIM at $5001E000-$5001FFFF):
 *   - read/write ops: the register index is (offset >> 9) & 15, the IWM's
 *     state addresses at stride $200 (ph0L at +$0000 ... q7H at +$1E00); byte
 *     accesses, the value in bits 0-7.
 *   - mc_swim_set_headsel: VIA1 PA5 (vHeadSel), the drive's SEL line, on
 *     every change of that output.
 *   - No interrupt line; no scheduled events (TACH, index, rotation and
 *     transfer timing are computed from guest time on access).
 *   - Media: mc_swim_insert / mc_swim_remove / mc_swim_media; mc_swim_flush
 *     before closing images.
 *
 * Chip variants (mc_chip):
 *   - SWIM: as above (Quadra 700, 900/950, IIfx through the IOP, SE/30, IIci).
 *   - SWIM2 (mc_swim2_init): the 68040 Quadras 610/650/800, LC 475/Quadra 605
 *     and the 630/580 family. No SWIM2 register document is among the sources
 *     (the Centris 610/650 note has no floppy chapter, the 800, LC 475 and 630
 *     notes are not available), so the register model is the SWIM's with the
 *     same IWM -> ISM switch; the Mac OS drives GCR 400/800K disks through
 *     the IWM register set and MFM through the ISM, as on the Quadra 700
 *     (the driver of the 840AV/660AV uses a different chip, "New Age", with a
 *     command set and DMA: not a SWIM2, not modelled here). Differences a
 *     SWIM2 may have (error-register CRC writes, registers above $1000)
 *     are unconfirmed (dagger) and not implemented.
 *   - IWM (mc_iwm_init): the Mac II (v1 ROM) and the earlier Macintoshes: the
 *     IWM register set only, no ISM (the mode sequence 1,0,1,1 does nothing),
 *     GCR 400/800K only; the drives are mc_drive_type 800K (699-0326: REVISED
 *     1, double-sided, not MFM-capable) or 400K (699-0285: REVISED 0,
 *     /SINGLE SIDE low, head 0 only). The IWM's bit-cell and clock-divide
 *     mode bits are not modelled (bytes arrive at the media's rate whatever
 *     they say). The 400K drive's motor follows the speed control line
 *     (mc_swim_set_speed, or its source read at each TACH read: the 128K's
 *     and 512K's disk-speed PWM) on its TACH; the bytes still pass at the media's rate, so the synthesized
 *     tracks read whatever the speed (dagger).
 *
 * Mac II wiring (Guide to the Macintosh Family Hardware, 2nd ed., ch. 2
 * Table 2-1, ch. 3 "Macintosh II address map" and ch. 9 "Floppy Disk
 * Interfaces"): one or two internal 800K drives, no external connector; the
 * IWM at $50016000-$50017FFF, register index (offset >> 9) & 15 (A12-A9),
 * on data lines D31-D24, so a byte access at the register's even long
 * address (dagger for the byte lane); the drive enables /ENBL1 and /ENBL2 come
 * from the IWM's own enable and drive-select latches (no VIA drive-select
 * pin, so mc_swim_set_headsel from VIA1 PA5, vHeadSel, is the only VIA
 * line); wait states: two for the IWM space. The IWM clock is C16M
 * (15.6672 MHz, mc_dev_now time). Other IWM machines (Plus, SE, SE/30's
 * external port) differ: the SE selects the internal drive with VIA1 PA4
 * (vDriveSel) through the BBU/GLU (dagger for 5.2 to wire).
 *
 * To wire a machine: register the device at the base address over its
 * $2000 window with mc_swim_ops (or call mc_swim_access), call
 * mc_swim_set_headsel on every VIA1 PA5 change, insert images with
 * mc_swim_insert, mc_swim_flush before closing, and mc_swim_remove to eject.
 *
 * Bytes move a whole byte at a time at the media's rate: the read data
 * register and the ISM FIFO see the track byte under the head at guest time;
 * writes land at successive track positions and, when a write ends, the
 * track is decoded back into sectors and written to the image
 * (storage/floppy_track.c, src/core/disk.c). */
#ifndef MC_SWIM_H
#define MC_SWIM_H

#include "hw.h"
#include "disk.h"
#include "floppy_track.h"

#include <stddef.h>
#include <stdint.h>

#define MC_SWIM_DRIVES 2

/* The chip a mc_swim stands for: the register models are the same IWM and
 * ISM sets; the IWM chip has no ISM (the mode-register sequence does
 * nothing). */
typedef enum { MC_CHIP_SWIM, MC_CHIP_SWIM2, MC_CHIP_IWM } mc_chip;
/* The drive on a connector: the SuperDrive (GCR 400/800K and MFM 720K/1.44M),
 * the 800K double-sided GCR drive (699-0326) or the 400K single-sided GCR
 * drive (699-0285). */
typedef enum { MC_DRIVE_SUPERDRIVE, MC_DRIVE_800K, MC_DRIVE_400K } mc_drive_type;

typedef struct mc_swim_drive {
    uint8_t fitted;
    uint8_t type;                       /* mc_drive_type */
    mc_disk *disk;
    mc_floppy_format format;
    uint8_t inserted, protect, ejected;
    uint8_t track, dirtn, motor, mfm_mode, eject_latch, enabled;
    uint64_t step_done;                 /* the head has moved: /READY (C15M time) */
    uint64_t step_ack;                  /* /STEP returns high (C15M time) */
    uint64_t last_read;                 /* IWM: absolute byte index + 1 of the last byte read */
    /* The 400K drive under the speed control line: rpm x 16 (0: its zone
     * speed), and its turn (rpm x 16 x C15M periods; a revolution is
     * 16 x 60 x C15M of them) at rot_time. */
    uint32_t pwm_speed;
    uint64_t rot_time, rot_phase;
    /* One track side cached as bytes; `blank` when the drive's mode does
     * not match the media (or the side does not exist). */
    uint8_t cache_valid, cache_track, cache_side, cache_blank, dirty;
    mc_floppy_track cache;
} mc_swim_drive;

typedef struct mc_swim {
    mc_dev dev;
    unsigned drives;
    uint8_t chip;                       /* mc_chip */
    uint8_t ism;                        /* ISM register set selected */
    uint8_t phases;                     /* PHASE0-3: CA0, CA1, CA2, LSTRB */
    uint8_t latch;                      /* IWM latches: bit 4 MotorOn, 5 drive select, 6 L6, 7 L7 */
    uint8_t iwm_mode;
    uint8_t mode_seq, mode_count;       /* bit 6 of the last IWM mode writes */
    uint64_t timer_end;                 /* IWM enable timer after MotorOn goes off */
    uint8_t headsel;                    /* VIA1 PA5 */
    uint8_t upper;                      /* the SE's VIA1 PA4 low: /ENBL1 is the upper internal drive (mc_swim_set_upper) */
    /* ISM */
    uint8_t ism_mode, setup, phase_dir, error, param[16], param_index, iwm_config;
    /* The transfer in progress: 0 none, 1 IWM write, 2 ISM read, 3 ISM write. */
    uint8_t xfer, xfer_side;
    int xfer_drive;
    uint32_t wr_pos;                    /* writes: next track position */
    /* IWM write */
    uint64_t wr_shift_end;
    uint8_t wr_buffered, wr_byte, wr_underrun;
    /* ISM read stream: the byte at absolute index rd_first is the first one
     * delivered (rd_pos0 on the track). */
    uint64_t rd_first, rd_taken, rd_frozen;
    uint32_t rd_pos0;
    /* ISM write FIFO */
    uint8_t fifo[2], fifo_flags[2], fifo_n;
    uint64_t wr_next;
    uint16_t crc;
    uint8_t crc_prev_mark;
    /* The speed control line's level now (rpm x 16), when a board has one. */
    uint32_t (*speed)(void *ctx);
    void *speed_ctx;
    mc_swim_drive drive[MC_SWIM_DRIVES];
} mc_swim;

extern const mc_dev_ops mc_swim_ops;

/* A SWIM after reset with `drives` (1 or 2) SuperDrives fitted, empty. */
void mc_swim_init(mc_swim *s, const mc_hw_host *host, unsigned drives);
/* A SWIM2 (same register model; see the header comment) with SuperDrives. */
void mc_swim2_init(mc_swim *s, const mc_hw_host *host, unsigned drives);
/* An IWM alone (no ISM, no MFM) with `drives` drives of `type` (800K or 400K). */
void mc_iwm_init(mc_swim *s, const mc_hw_host *host, unsigned drives, mc_drive_type type);
/* Register access by index (0-15), as the ops do after decoding the offset;
 * returns the byte read (writes return 0). */
uint8_t mc_swim_access(mc_swim *s, unsigned reg, int write, uint8_t value);
/* VIA1 PA5: the drive's SEL (head select) line. */
void mc_swim_set_headsel(mc_swim *s, int level);
/* The disk-speed PWM's speed for the 400K drives (rpm x 16; Guide to the
 * Macintosh Family Hardware, chapter 13): their TACH follows it. Other
 * drives control their own speed ([ERS] 3.4) and ignore it. */
void mc_swim_set_speed(mc_swim *s, uint32_t rpm16);
/* Where the speed control line comes from: read at each TACH read. */
void mc_swim_speed_source(mc_swim *s, uint32_t (*speed)(void *ctx), void *ctx);
/* The SE's and Classic's VIA1 PA4 (vDriveSel, Guide to the Macintosh
 * Family Hardware, Table 4-3): low, /ENBL1 enables the upper internal
 * drive, which Livadix does not fit (drive 0 is the lower one); high, the
 * lower. */
void mc_swim_set_upper(mc_swim *s, int upper);

/* Puts a disk image in a drive: raw or DiskCopy 4.2, 400, 720, 800 or 1440
 * KiB of 512-byte sectors (1680 KiB refused). The swim keeps the pointer;
 * the caller still owns the image. `write_protect` locks the disk (a
 * read-only image is always locked). Returns 0 with a message on refusal. */
int mc_swim_insert(mc_swim *s, unsigned drive, mc_disk *disk, int write_protect, char *error, size_t capacity);
/* The disk in a drive (NULL when empty); `ejected` set when the guest
 * ejected it (the image stays attached until removed). */
mc_disk *mc_swim_media(const mc_swim *s, unsigned drive, int *ejected);
/* Takes the disk out (pending writes are written first); returns it. */
mc_disk *mc_swim_remove(mc_swim *s, unsigned drive);
/* Writes every pending track back to its image. */
void mc_swim_flush(mc_swim *s);

#endif
