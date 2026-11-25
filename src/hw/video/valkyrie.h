/* Livadix — Valkyrie, the built-in video of the Quadra 630 / LC 630 and the LC
 * 580 families: its display memory controller, the CLUT/DAC and the monitor
 * sense lines.
 *
 * Sources: Macintosh LC 630 and Quadra 630 Developer Note (1994): chapter 2,
 * "Valkyrie IC" (display memory controller, CLUT for the 4- and 8-bit modes
 * and gamma in the 16-bit mode, 1, 2, 4, 8 or 16 bits per pixel, no 24-bit
 * mode), "Display RAM" (1 MB of DRAM, dedicated address space from $F900
 * 0000); chapter 3, Table 3-9 (maximum depths per monitor), Table 3-11 (the
 * monitor sense codes, standard and extended), Tables 3-13 and 3-14 (video
 * timing: frame rates); chapter 5, Table 5-1 (the depths the video driver
 * offers). Apple Technical Note HW 30 "Sense Lines" for the extended sense
 * protocol.
 *
 * The register layout is what ROMs 06684214 and 064DC91D program
 * (the driver .Display_Video_Apple_
 * Valkyrie at ROM offset $FD154, its PrimaryInit at $F9CE2): byte registers
 * every 4 bytes from $50F2A000 (the VIA1 base + $2A000):
 *   +$00 control: bits 0-3 the display mode (the ROM's timing records at
 *        ROM offset $F5B50: 2 the 12-inch RGB's 512 x 384, 6 the 14-inch
 *        RGB's 640 x 480, 9 the 16-inch RGB's 832 x 624, $B VGA's 640 x
 *        480, $C and $D SVGA's 800 x 600 at 60 and 72 Hz), bit 7 set
 *        while the ROM sets a mode up (video off), bits 5-6 the sync
 *        controls (the driver's SetSync);
 *   +$04 the depth: 0-4 for 1, 2, 4, 8 and 16 bits per pixel;
 *   +$0C configuration: bit 0 set by the ROM when the full 1 MB of display
 *        memory answers;
 *   +$10 a write with bit 0 set clears the VBL flag;
 *   +$14 bit 0, the VBL flag (set at each vertical blanking);
 *   +$18 the VBL interrupt enable, written as the VIA's IER: bit 7 set or
 *        clear, bit 0 the enable;
 *   +$1C the sense lines: bits 0-2 written drive lines 2-0 low (bit 2 line
 *        2), bits 4-6 read the levels of lines 0-2.
 * The CLUT/DAC at $50F24000 (the decoder's VDAC base): +$00 the address
 * register, +$04 the data register (red, green, blue in turn, the address
 * advancing after blue), +$0C a control register ($EB at start).
 *
 * Display memory: the frame buffer starts $1000 bytes into the 1 MB (the
 * ROM's ScrnBase is $F900 1000; its PrimaryInit tests the memory $1000
 * below it); a row is the width times the depth in bits / 8.
 *
 * Lines: MC_VALKYRIE_LINE_VBL (1 = the VBL interrupt is asserted). */
#ifndef MC_VALKYRIE_H
#define MC_VALKYRIE_H

#include "../hw.h"

#include <stdint.h>

enum { MC_VALKYRIE_LINE_VBL };

#define MC_VALKYRIE_VRAM (1u << 20)
#define MC_VALKYRIE_FB_OFFSET 0x1000u   /* the frame buffer's start in display memory (ScrnBase $F900 1000) */
#define MC_VALKYRIE_CLUT_SIZE 0x1000u
#define MC_VALKYRIE_REGS_SIZE 0x1000u

/* The monitors the Developer Note lists (Table 3-11). */
typedef enum mc_valkyrie_monitor {
    MC_VALKYRIE_NONE,
    MC_VALKYRIE_RGB12,      /* 12-inch RGB, 512 x 384 */
    MC_VALKYRIE_RGB14,      /* 14-inch RGB, 640 x 480 */
    MC_VALKYRIE_VGA,        /* VGA and SVGA: 640 x 480, 800 x 600 */
    MC_VALKYRIE_RGB16,      /* 16-inch RGB, 832 x 624 */
    MC_VALKYRIE_MONITORS
} mc_valkyrie_monitor;

typedef struct mc_valkyrie {
    mc_dev dev;
    uint8_t *vram;
    mc_valkyrie_monitor monitor;
    uint8_t reg[16];            /* $00-$3C, one byte every 4 */
    uint8_t vbl_flag, irq;
    uint8_t ar;                /* later Valkyrie-AR/AV2 display timings */
    uint8_t clut_index, clut_component, clut_control;
    uint8_t clut_rgb[3];
    uint32_t palette[256];
    uint64_t frame_length, next_vbl;
} mc_valkyrie;

int mc_valkyrie_init(mc_valkyrie *v, const mc_hw_host *host, const char *name, mc_valkyrie_monitor monitor);
/* Performa 6400 Developer Note, chapter 2 and Tables 3-10/3-14:
 * the AR variant also has 1024x768 timings and 800x600 at 16 bits.
 * ROM 6F5724C0 programs its CLUT index/data/control at +$00/+$08/+$18
 * (native driver's palette writes observed at $F1304008). */
int mc_valkyrie_ar_init(mc_valkyrie *v, const mc_hw_host *host, const char *name, mc_valkyrie_monitor monitor);
void mc_valkyrie_free(mc_valkyrie *v);
extern const mc_dev_ops mc_valkyrie_ops;        /* the registers, offsets from $50F2A000 */
extern const mc_dev_ops mc_valkyrie_clut_ops;   /* the CLUT/DAC, offsets from $50F24000 */

/* The monitor for a screen size (video.resolution): NONE when the Developer
 * Note lists no monitor of that size. */
mc_valkyrie_monitor mc_valkyrie_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth Valkyrie drives the monitor at (Table 3-9; Table 5-1). */
uint32_t mc_valkyrie_max_depth(mc_valkyrie_monitor monitor);
/* The levels on the three sense lines (bit n = line n) given what Valkyrie
 * drives (HW 30). */
uint8_t mc_valkyrie_sense(const mc_valkyrie *v);

/* What the display shows; 0 when video is off. depth 1-8 index the palette,
 * 16 is x-R5-G5-B5. */
typedef struct mc_valkyrie_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_valkyrie_view;
int mc_valkyrie_view_get(const mc_valkyrie *v, mc_valkyrie_view *view);

#endif
