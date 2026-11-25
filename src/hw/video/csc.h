/* Livadix — the CSC, the color LCD controller of the PowerBook Duo 270c and
 * 280c (a color screen of 640 x 480).
 *
 * Sources: no CSC document was available; everything is observed from
 * ROMs 0024D346 and 015621D7: the decoder record of
 * decoder kind 10 (ROM 0024D346 offset $A8494, entry 14: the controller at
 * $50F2 0000, where the Duo 210's GSC answers), the video record of the
 * Duo 270c's ProductInfo (the screen at $6000 0000), and the driver
 * .Display_Video_Apple_CSC (ROM 0024D346 offset $FCDCF) as it programs the
 * registers (byte wide, at their offsets):
 *   - $00: nonzero on a CSC: ROM 4D27039C tells the PowerBook 190cs's
 *     CSC from the 190's GSC by it (offset $FB56; the GSC's register 0
 *     reads 0); the value is †, the board sets `id`;
 *   - $01, $02: read at start, bits 0-2 the panel's type: 0 the color
 *     panel, 4 the Duo 280's grayscale one (ROM 015621D7's driver,
 *     $F8F0A-$F8F44: the type selects the video sResource and the box
 *     flag, table $F9214); with type 4 the driver loads no CLUT ($40/$42)
 *     and writes $0C, $0E, $10 (04, $0A, 05 at 4 bits; kept†): Livadix shows
 *     a pixel's value as its gray level, 0 white†;
 *   - $12: the depth (bits 1-0: 1, 2, 4 or 8 bits; the driver ends its
 *     start at 3 for the 640 x 480 x 8 screen);
 *   - $14: bit 0 the frame interrupt's enable, bit 1 its clear, bits 2-3
 *     the display on ($0C, $0E, $0F at start)†;
 *   - $40: the CLUT's address, $42 its data (red, green, blue, eight bits
 *     each, the address stepping after blue), $44 the pixel mask ($FF);
 *   - $03-$10, $16-$3C: timing written once and kept†;
 *   - past the registers ($50 on) zeros, writes ignored†: the driver's
 *     CLUT path for the gray panel (ROM B6909089 offset $1FCAAE, its VBL
 *     wait at $1FD236 entered with A3 at the CLUT data register) takes
 *     the longword at $52 for its globals' controller pointer and sets
 *     bit 1 of the byte 20 past it; registers repeating there gave it
 *     $0000 0700 and a low-memory vector ($714) the PowerBook 540's Shut
 *     Down then jumped through (with zeros the bit lands in the high
 *     byte of the zero-divide vector, $14).
 *
 * Livadix's own code. */
#ifndef MC_CSC_H
#define MC_CSC_H

#include "../hw.h"

#include <stdint.h>

#define MC_CSC_NREG 0x50u
#define MC_CSC_PANEL_GRAY 4u
#define MC_CSC_WIDTH 640u
#define MC_CSC_HEIGHT 480u

enum { MC_CSC_DEPTH = 0x12, MC_CSC_CONTROL = 0x14, MC_CSC_CLUT_ADDR = 0x40, MC_CSC_CLUT_DATA = 0x42, MC_CSC_MASK = 0x44 };

typedef struct mc_csc {
    mc_dev dev;
    uint8_t reg[MC_CSC_NREG];
    uint8_t panel;
    uint8_t id;                 /* what $00 reads when nonzero (the board sets it) */
    uint8_t clut[256][3];
    uint8_t clut_step;
    const uint8_t *screen;
    uint32_t palette[256];
} mc_csc;

typedef struct mc_csc_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_csc_view;

void mc_csc_init(mc_csc *c, const mc_hw_host *host, const char *name, const uint8_t *screen, uint8_t panel);
extern const mc_dev_ops mc_csc_ops;
int mc_csc_view_get(mc_csc *c, mc_csc_view *v);

#endif
