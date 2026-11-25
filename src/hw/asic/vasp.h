/* Livadix — VASP, the integrated controller of the Macintosh IIvx, IIvi and
 * Performa 600: pseudo-VIA2, built-in video and its CLUT DAC, the VRAM
 * configuration.
 *
 * Sources: Macintosh IIvx Developer Note (Apple, 1993), chapter 1: "VASP
 * Integrated Controller" (timing, video generation, memory mapping, sound,
 * clock generation, GLU functions: "a full-function VIA1, a pseudo-VIA2
 * similar to that of the V8 gate array"), Table 1-2 (the memory map: RAM
 * from 0, ROM at $4000 0000, I/O at $50F0 0000, VRAM at $60B0 0000, NuBus
 * $8000 0000-$EFFF FFFF), "Video", "VRAM" (two VRAM SIMMs: 512 KB or 1
 * MB), "Monitors Supported", Table 1-3 (12-inch RGB 512 x 384 to 16 bits,
 * 13-inch RGB and VGA 640 x 480 to 8 bits, 16 with 1 MB), Figures 1-2 to
 * 1-4 (timing); "RAM Cache" (a 32 KB cache without snooping, the IIvx
 * only). Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (the
 * VIA2 functions: active-low slot lines, the 6522's IFR/IER conventions).
 *
 * Register offsets and bit meanings are read from the IIvx ROM 4957EB49
 * and cited where they are used in vasp.c. Two
 * windows of the I/O block:
 *
 *   $50F2 6000  pseudo-VIA2, byte registers repeating every $20:
 *     $00 port B (written $8F at start, then $87; the cache's control
 *         lines on the IIvx; stored)†
 *     $01 configuration: bit 1 the VRAM's 512 KB layout (the ROM sets it,
 *         writes a pattern and clears it when the pattern does not appear
 *         512 KB higher, ROM offset $468E8), bit 2 set at start, bit 6 set
 *         after reset; stored
 *     $02 slot interrupt flags (active low): bits 3-5 NuBus slots $C-$E,
 *         bit 6 the built-in video's vertical blanking
 *     $03 interrupt flags: 0 SCSI DRQ, 1 any slot, 3 SCSI IRQ, 4 sound;
 *         bit 7 any enabled (reads)
 *     $10 monitor parameters: bits 2-0 the depth (log2 of the bits per
 *         pixel), bits 5-3 the sense code (read), bit 6 video off (the
 *         video PrimaryInit, ROM offset $FE972-$FEA56)
 *     $11 stored (the ROM's register test)
 *     $12 slot interrupt enables, $13 interrupt enables (bit 7 set: set the
 *         bits written, clear: clear them)
 *   $50F2 4000  CLUT DAC: $00 address, $01 data (red, green, blue, then the
 *         next entry), $02 control (the PrimaryInit writes $0B)
 *
 * The screen: VRAM from $60B0 0000 with rows of 1024 bytes (512 KB) or
 * 2048 bytes (1 MB) at every depth (the ROM's video sResources $82-$8E).
 *
 * Output lines (mc_hw_host line, 1 = asserted):
 *   MC_VASP_LINE_IRQ   the pseudo-VIA2's interrupt (level 2)
 *   MC_VASP_LINE_TICK  the 60.15 Hz tick to VIA1 CA1 (square wave)
 *
 * Livadix's own code. */
#ifndef MC_VASP_H
#define MC_VASP_H

#include "../hw.h"

#include <stdint.h>

enum { MC_VASP_LINE_IRQ = 0, MC_VASP_LINE_TICK = 1 };

/* Interrupt flag bits ($03). */
enum { MC_VASP_IRQ_SCSI_DRQ = 0x01, MC_VASP_IRQ_SLOT = 0x02, MC_VASP_IRQ_SCSI = 0x08, MC_VASP_IRQ_SOUND = 0x10 };

/* Pseudo-VIA2 registers. */
enum {
    MC_VASP_DATAB = 0x00, MC_VASP_CONFIG = 0x01, MC_VASP_SIFR = 0x02, MC_VASP_IFR = 0x03, MC_VASP_MONP = 0x10,
    MC_VASP_TEST = 0x11, MC_VASP_SIER = 0x12, MC_VASP_IER = 0x13
};

/* Monitors (Developer Note, Table 1-3; their standard sense codes). */
typedef enum mc_vasp_monitor {
    MC_VASP_NONE,
    MC_VASP_RGB12,          /* 12-inch RGB, 512 x 384 (sense 2) */
    MC_VASP_HIRES13,        /* 13-inch RGB, 640 x 480 (sense 6) */
    MC_VASP_MONITORS
} mc_vasp_monitor;

#define MC_VASP_VRAM_STD 0x80000u       /* 512 KB */
#define MC_VASP_VRAM_MAX 0x100000u      /* 1 MB */

typedef struct mc_vasp {
    mc_dev dev;
    uint8_t datab, config, monp, test;
    uint8_t ifr, ier;           /* bits 0-6 */
    uint8_t slots;              /* port A sources asserted (bit = asserted) */
    uint8_t sier;
    uint8_t irq;
    mc_vasp_monitor monitor;
    uint64_t frame, blank, frame_start;
    uint8_t vbl;
    uint64_t tick_start;
    uint8_t tick;
    uint8_t dac_addr, dac_rgb, dac_ctl;
    uint8_t clut[256][3];
    uint32_t palette[256];
    uint32_t view_palette[256];
    uint8_t *vram;
    uint32_t vram_size;
} mc_vasp;

extern const mc_dev_ops mc_vasp_ops;        /* the pseudo-VIA2, $50F2 6000 */
extern const mc_dev_ops mc_vasp_dac_ops;    /* the CLUT DAC, $50F2 4000 */

void mc_vasp_init(mc_vasp *v, const mc_hw_host *host, const char *name, mc_vasp_monitor monitor, uint8_t *vram,
                  uint32_t vram_size);
/* The monitor of a screen size (NONE when the built-in video drives none). */
mc_vasp_monitor mc_vasp_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth VASP shows on that monitor with `vram_size` bytes. */
uint32_t mc_vasp_max_depth(mc_vasp_monitor monitor, uint32_t vram_size);
/* An interrupt source (MC_VASP_IRQ_* other than SLOT) changed. */
void mc_vasp_set_irq(mc_vasp *v, uint8_t bit, int asserted);
/* The slot interrupt lines: bits 3-5 slots $C-$E. */
void mc_vasp_set_slots(mc_vasp *v, uint8_t asserted, uint8_t mask);

typedef struct mc_vasp_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_vasp_view;
/* 0 when the video is off or no monitor is connected. */
int mc_vasp_view_get(mc_vasp *v, mc_vasp_view *view);

#endif
