/* Livadix — DAFB, the Quadra 700/900 built-in video (frame buffer controller,
 * Swatch timing, CLUT/DAC, clock generator, monitor sense).
 *
 * Sources: the Macintosh Quadra 700 Developer Note ("Video frame-buffer
 * controller IC: DAFB": VRAM in up to four 512 KB banks, Table 1-1 depths
 * per VRAM; registers and VRAM in the NuBus slot $9 space; the video
 * interrupt on VIA2 PA6 and /SLOTIRQ, Table 2-6), the DAFB II specification
 * (Apple drawing 343S0128-A, section 6.4 "Frame buffer memory map": VRAM at
 * $F900 0000-$F91F FFFF, control at $F980 0000, TEA for the rest of slot 9
 * and all of super slot 9; 6.6.1 the DAFB registers: video base, row words,
 * clock configuration, DAFB configuration, block write, page mode, sense
 * lines, reset, Turbo SCSI channel configuration, test, colour registers;
 * 6.6.2 Swatch: mode, interrupt mask, status and clears, cursor and animate
 * lines, the horizontal and vertical timing parameters, current line; 6.6.3
 * the CLUT/DAC at $F980 0200 on byte lane 3 with register select on
 * address bits 5-4; 6.6.4 the clock generator at $F980 0300 on data bits
 * 3-0) and Apple Technical Note HW 30 "Sense Lines" (Tables S-1 and S-2:
 * the monitor codes and the type-7 extended codes).
 *
 * Lines: MC_DAFB_LINE_VBL (1 = the video interrupt is asserted). */
#ifndef MC_DAFB_H
#define MC_DAFB_H

#include "../hw.h"

#include <stdint.h>

enum { MC_DAFB_LINE_VBL };

/* Monitors (HW 30, Table S-1/S-2). */
typedef enum mc_dafb_monitor {
    MC_MONITOR_NONE,
    MC_MONITOR_RGB12,       /* 512 x 384 */
    MC_MONITOR_HIRES13,     /* 640 x 480 */
    MC_MONITOR_PORTRAIT,    /* 640 x 870 colour */
    MC_MONITOR_RGB16,       /* 832 x 624 (extended) */
    MC_MONITOR_RGB19,       /* 1024 x 768 (extended) */
    MC_MONITOR_RGB21,       /* 1152 x 870 */
    MC_MONITOR_VGA,         /* 640 x 480, 800 x 600 (extended) */
    MC_MONITOR_COUNT
} mc_dafb_monitor;

typedef struct mc_dafb {
    mc_dev dev;
    uint8_t *vram;
    uint32_t vram_size;
    mc_dafb_monitor monitor;
    uint16_t reg[64];           /* DAFB registers, $F980 0000 + 4n */
    uint16_t swatch[32];        /* Swatch registers, $F980 0100 + 4n */
    /* CLUT/DAC. */
    uint8_t clut_index, clut_component, clut_config;
    /* The Centris/Quadra 610-800 and LC 475 boards' DAC: its configuration
     * registers ($F980 0220) indexed by the address register (low four
     * bits), register 0 the configuration the picture follows. Their ROMs
     * take a DAC that reads back register 0 after register 1 was written
     * (ROM F1ACAD13 offset $F8F82) as the one that drives 16 bits, and only
     * then offer the 16-bit modes (their second sResource table, $F9CC4). */
    uint8_t antelope;
    uint8_t dac_reg[16];
    uint8_t clut_rgb[3];
    uint32_t palette[256];
    uint8_t clock[16];          /* clock generator registers */
    /* VBL. */
    uint64_t frame_start, frame_length;
    uint8_t vbl_status, irq, pending_bits;
    uint64_t serial;
    /* The Turbo SCSI channels' DREQ inputs (the 53C96 lines the board wires
     * in), read back in bit 9 of the channel's configuration register. */
    uint8_t scsi_drq[2];
} mc_dafb;

/* `vram_size`: 1 or 2 MB (two or four banks). */
int mc_dafb_init(mc_dafb *d, const mc_hw_host *host, const char *name, uint32_t vram_size, mc_dafb_monitor monitor);
void mc_dafb_free(mc_dafb *d);
extern const mc_dev_ops mc_dafb_ops;     /* the control space, offsets from $F980 0000 */
/* The monitor for a screen size (video.resolution); MC_MONITOR_NONE when no
 * Apple monitor the DAFB drives has it. */
mc_dafb_monitor mc_dafb_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth the DAFB drives a monitor at with four banks of VRAM
 * (Developer Note, Table 1-1: 24 bits, as 32, on the 12" and 13"; 8 on the
 * portrait and two-page displays, and the 16" here†); 0 for none. */
uint32_t mc_dafb_max_depth(mc_dafb_monitor monitor);

/* What the display shows now; 0 when video is off. depth 1-8 index the
 * palette, 16 is x-R5-G5-B5 and 32 x-R8-G8-B8. */
typedef struct mc_dafb_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_dafb_view;
int mc_dafb_view_get(mc_dafb *d, mc_dafb_view *view);

/* The levels on the three sense lines given what the DAFB drives. */
uint8_t mc_dafb_sense(const mc_dafb *d);

#endif
