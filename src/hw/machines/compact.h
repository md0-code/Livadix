/* Livadix — the compact Macintosh board's parts that the SE family's board
 * (machines/se.c) shares with the 128K, 512K, 512Ke and Plus
 * (machines/compact.c): the board state, the byte-lane ports, the SCSI
 * window, the 68000's wait states, PRAM, storage, sound, serial ports and
 * the framebuffer (mc_compact_ops). Livadix's own code. */
#ifndef MC_MACHINES_COMPACT_H
#define MC_MACHINES_COMPACT_H

#include "../hw_machine.h"
#include "../m0110.h"
#include "../ncr5380.h"
#include "../quadmouse.h"
#include "../rtc.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../storage/vdisk.h"
#include "../video/compactvideo.h"

typedef struct compact_board {
    const char *model;
    uint8_t drive;              /* mc_drive_type */
    uint8_t keyboard;           /* MC_M0110, MC_M0110A */
    uint8_t scsi;               /* the Plus's 5380 */
} compact_board;

/* A chip on one byte lane of the 16-bit bus: a byte access on the other
 * lane finds nothing; a word access is one cycle of the chip (its byte on
 * both halves of a read). */
typedef struct lane_port {
    struct compact *m;
    void *dev;
    const mc_dev_ops *ops;
    int lane;                   /* 0 the high byte (even addresses), 1 the low byte (odd), -1 both */
    uint8_t reads, writes;      /* the cycles it answers (the SCC's read and write spaces) */
} lane_port;

typedef struct compact {
    mc_hw *hw;
    const compact_board *board;
    uint32_t ram_size;
    uint8_t overlay;
    uint8_t video_slots;        /* RAM cycles per video fetch in a visible line (0: the Plus's 2) */
    mc_via via;
    mc_rtc rtc;
    mc_scc scc;
    mc_swim iwm;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_compactvideo video;
    mc_m0110 keyboard;
    mc_quadmouse mouse;
    lane_port via_port, scc_read_port, scc_write_port, iwm_port, scsi_port;
    uint8_t via_irq, scc_irq;
    mc_serial_host serial;
    uint64_t fb_serial;
    char nvram[1024];
    int64_t host_start;
} compact;

/* The board operations of compact.c; a board built around `compact` (as
 * its first member) calls those it shares. */
extern const mc_board_ops mc_compact_ops;
extern const mc_dev_ops mc_compact_lane_ops, mc_compact_scsi_ops;
/* Claims `region` for `dev` through a lane port (its reads and writes as
 * the region's name says, its cycles' time as wait states). */
void mc_compact_attach(compact *m, lane_port *p, const char *region, void *dev, const mc_dev_ops *ops, int lane,
                       const mc_dev_ops *port_ops);
/* The 68000's wait states (m68k_bus.wait): the video's RAM cycles, the
 * VIA's E clock. */
unsigned mc_compact_wait(void *board, uint32_t a, unsigned fc, unsigned kind, uint64_t cycle);
/* Maps the RAM's decode into `size` bytes from `base` (2.5 MB as two rows). */
void mc_compact_map_ram(compact *m, uint32_t base, uint32_t size);
/* PRAM and the clock from the machine's PRAM file. */
void mc_compact_load_nvram(compact *m);
/* The SCSI IDs of the disks, the CD-ROM drive at 3. */
#define MC_COMPACT_CD_ID 3

#endif
