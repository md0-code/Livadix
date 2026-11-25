/* Livadix — the GSC, the gray-scale LCD controller of the PowerBook 160, 165
 * and 180 and of the PowerBook Duos with a gray-scale screen (Duo 210,
 * 230, 250, 280).
 *
 * Sources: no GSC document was available; everything is observed from
 * ROM E33B2724: the decoder record of decoder kind
 * 12 (ROM offset $CD014, entry 14: the GSC at $50F2 0000), the video
 * record of the PowerBook 180's ProductInfo ($CD288: the screen at
 * $6004 0000), the built-in video's modes (vpBlocks at $F6B78-$F7C62:
 * 640 x 400 at 1, 2 and 4 bits, rows of 80, 160 and 320 bytes) and the
 * driver .Display_Video_Apple_DBLite ($FCED5; its mode change at
 * $FD276):
 *   - register 1, bits 2-0: the panel's type (read; it selects the
 *     timing values below);
 *   - register 4: bits 2-0 the depth (0 1 bit, 1 2 bits, 2 4 bits),
 *     bit 5 the display on (cleared while the depth changes), bits 6
 *     and 7 kept†;
 *   - registers 8 and $A: two timing values per panel type and depth
 *     from the driver's tables ($FD2E2, $FD2FA), kept†;
 *   - registers 3, 5-7, 9, $1D-$1F: written once by the ROM's start
 *     ($10, $64, $80, $80, $05, $00, $00, $03), kept†.
 * The panel's type also sets its height, as each ROM's driver reads it:
 * ROM FDA22562 (the PowerBook 150) takes 640 x 480 for type 4 (the
 * cursor's pin rectangle, $834, then reads 480 x 640) and offers 1 and 2
 * bits there (the mode check at ROM offset $FBED2); ROM 4D27039C (the
 * PowerBook 190) takes 640 x 480 for type 3, the one its driver gives a
 * backlight (the table it copies to $86EA); the board sets `height`.
 * The panel shows each pixel's value as a gray level, 0 white and the
 * largest value black (the driver loads no color table); the contrast
 * the PMU drives is not modelled.
 *
 * Livadix's own code. */
#ifndef MC_GSC_H
#define MC_GSC_H

#include "../hw.h"

#include <stdint.h>

#define MC_GSC_NREG 32u
#define MC_GSC_WIDTH 640u
#define MC_GSC_HEIGHT 400u

enum { MC_GSC_PANEL = 0x01, MC_GSC_CONTROL = 0x04 };

typedef struct mc_gsc {
    mc_dev dev;
    uint8_t reg[MC_GSC_NREG];
    uint8_t panel;              /* the panel's type (register 1) */
    uint32_t height;            /* the panel's lines (0: 400) */
    const uint8_t *screen;      /* the panel's frame buffer */
    uint32_t palette[16];
} mc_gsc;

typedef struct mc_gsc_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_gsc_view;

void mc_gsc_init(mc_gsc *g, const mc_hw_host *host, const char *name, const uint8_t *screen, uint8_t panel);
extern const mc_dev_ops mc_gsc_ops;
/* What the panel shows; 0 while the display is off. */
int mc_gsc_view_get(mc_gsc *g, mc_gsc_view *v);

#endif
