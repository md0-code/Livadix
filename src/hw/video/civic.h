/* Livadix — Civic, the video controller of the Quadra 840AV and Centris 660AV,
 * with the Sebastian color palette and DAC and the dot-clock generator.
 *
 * Quadra 840AV and Centris 660AV Developer Note (Apple, 1993), chapter 2:
 * "Cyclone Integrated Video Interfaces Controller" (1 or 2 MB of VRAM, 32-
 * or 64-bit paths, vertical blanking and video-in interrupts), "Sebastian"
 * (the CLUT and DAC), "Endeavor" (the 840AV's dot clock; Clifton Plus on the
 * 660AV), "Video RAM Usage" (two 512 KB banks soldered; the 840AV takes two
 * more), Table 2-13 (monitors and depths with 1 and 2 MB of VRAM), Table
 * 2-14 (timings); chapter 11 (the video driver). Apple Technical Note HW 30
 * "Sense Lines" (the monitor codes, standard and extended).
 *
 * The register layout is what ROM 5BF10FD1 programs (the driver's field table at ROM offset $1FFAD0 and
 * its copy at $1E4B4C: 59 fields, each an offset and a width):
 * Civic's registers are one bit each, the bit 0 of a longword from
 * $5003 6000; a field of n bits is n consecutive registers, bit 0 first.
 * Fields this model gives meaning to (offset: bits):
 *   $000: 1  the vertical blanking interrupt's status (read; the driver's
 *            VBL handler tests it, ROM offset $1E6F68);
 *   $004: 1  video on (written last when a mode is set, $1E6006);
 *   $020: 3  3 for the ROM's start screen (1 bit), 0 from the System's driver;
 *   $040: 1  the second MB of VRAM in use (the ROM writes it set, tests the
 *            last longwords of 2 MB and clears it when they do not hold,
 *            $1FF378);
 *   $05C, $060, $064: 1 each, drive sense lines 0, 1, 2 low. Line 0
 *            is active low, the others active high: 001 releases all,
 *            101/011/000 drive 2/1/0 (register order $064/$060/$05C;
 *            ROM offsets $1FF3BE and $1FEEFC, HW 30 extended sense);
 *   $080: 3  the sense lines' levels (read; bit n line n);
 *   $110: 1  the VBL interrupt enabled; $120: 1 written 0 then 1 clears the
 *            VBL status ($1E6F78);
 *   $140: 12 the current half-line of the frame (read, the driver waits on
 *            it);
 *   $300: 12 the horizontal total in units, less one: a unit is two dots, one at 16 and
 *            32 bits per pixel with the second MB in use ($040; the 840AV's
 *            640 x 480: 431 at 8 bits, 863 at 16; the 660AV's: 431 at both);
 *   $380, $3C0: 12 each, the first and last active horizontal units;
 *   $480: 12 the vertical total in half-lines;
 *   $580, $640: 12 each, the first and last active half-lines;
 *   $08C: 8  the row length in 32-byte units and $0C0: 9 the screen's start in
 *            VRAM in 32-byte units (QuickDraw's ScreenRow and ScrnBase: 1024 and
 *            $800 at 640 x 480 x 8, 2048 and $800 at 16 bits, 2560 and $600 at
 *            32 bits, 1152 and $500 at 1152 x 870 x 8);
 * The others (sync and equalisation positions, the VRAM transfer timing,
 * the video-in window) are kept and read back.
 * VRAM: 1 MB (660AV) or 2 MB (840AV, expanded) at $5010 0000; the screen
 * starts at its first byte. Above the VRAM fitted the bus floats.
 * Sebastian at $50F3 0800: +$00 the CLUT address, +$10 the data (red,
 * green, blue and a fourth byte for each entry, the address advancing after
 * it), +$20 the pixel format (bits 0-2 log2 of the bits per pixel: $10 the
 * start screen's 1 bit, $13 8 bits, $54 16 bits, $45 32 bits; bit 6 set
 * while the CLUT is written); a pixel of d < 8 bits indexes entry
 * (p << (8 - d)) | the low bits set (the ROM puts white at $7F and black at
 * $FF for its 1-bit start screen, $6C38). The dot-clock generator at
 * $50F2 E000 (+$00, +$10, +$20) is kept.
 *
 * Lines: MC_CIVIC_LINE_VBL (1 = the VBL interrupt is asserted). */
#ifndef MC_CIVIC_H
#define MC_CIVIC_H

#include "../hw.h"

#include <stdint.h>

enum { MC_CIVIC_LINE_VBL };

#define MC_CIVIC_SIZE 0x800u            /* the registers, from $5003 6000 */
#define MC_CIVIC_SEBASTIAN_SIZE 0x800u  /* from $50F3 0800 */
#define MC_CIVIC_CLOCK_SIZE 0x1000u     /* from $50F2 E000 */
#define MC_CIVIC_VRAM_WINDOW (2u << 20) /* from $5010 0000 */

typedef enum mc_civic_monitor {
    MC_CIVIC_NONE,
    MC_CIVIC_RGB12,         /* 12-inch RGB, 512 x 384 */
    MC_CIVIC_RGB13,         /* 13- or 14-inch RGB, 640 x 480 */
    MC_CIVIC_PORTRAIT,      /* Portrait, 640 x 870 */
    MC_CIVIC_RGB16,         /* 16-inch RGB, 832 x 624 */
    MC_CIVIC_RGB19,         /* 19-inch RGB, 1024 x 768 */
    MC_CIVIC_RGB21,         /* 21-inch RGB, 1152 x 870 */
    MC_CIVIC_VGA,           /* VGA, 640 x 480 */
    MC_CIVIC_SVGA,          /* Super VGA, 800 x 600 */
    MC_CIVIC_MONITORS
} mc_civic_monitor;

typedef struct mc_civic {
    mc_dev dev;
    uint8_t *vram;
    uint32_t vram_size;             /* 1 or 2 MB */
    mc_civic_monitor monitor;
    uint8_t bit[MC_CIVIC_SIZE / 4];
    uint8_t vbl, irq;
    uint64_t frame_start, frame_length;
    /* Sebastian */
    uint8_t clut_address, clut_byte, clut_format;
    uint8_t clut_rgb[4];
    uint32_t clut[256];
    uint32_t palette[256];          /* the view's, by the depth's pixel values */
    uint8_t clock[3];
} mc_civic;

int mc_civic_init(mc_civic *c, const mc_hw_host *host, const char *name, uint32_t vram_size, mc_civic_monitor monitor);
void mc_civic_free(mc_civic *c);
extern const mc_dev_ops mc_civic_ops;           /* the registers, offsets from $5003 6000 */
extern const mc_dev_ops mc_civic_sebastian_ops; /* Sebastian, offsets from $50F3 0800 */
extern const mc_dev_ops mc_civic_clock_ops;     /* the dot clock, offsets from $50F2 E000 */

/* The monitor for a screen size (video.resolution); NONE when none of the
 * Developer Note's monitors has it (Table 2-13). */
mc_civic_monitor mc_civic_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth Civic drives the monitor at with `vram` bytes (Table
 * 2-13, graphics only). */
uint32_t mc_civic_max_depth(mc_civic_monitor monitor, uint32_t vram);
/* A field's value (offset of its first register, its width). */
uint32_t mc_civic_field(const mc_civic *c, uint32_t offset, unsigned bits);

/* What the display shows; 0 when video is off. depth 1-8 index the palette,
 * 16 is x-R5-G5-B5, 32 x-R8-G8-B8. */
typedef struct mc_civic_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_civic_view;
int mc_civic_view_get(mc_civic *c, mc_civic_view *view);

#endif
