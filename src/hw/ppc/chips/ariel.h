/* Livadix — Ariel II, the color lookup table and video DAC of the Power
 * Macintosh 6100, 7100 and 8100's built-in video.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994), chapter
 * 2, "Ariel II Video Chip" (a CLUT and DAC driving the AudioVision
 * monitor; the data path chips supply its video data) and Figure 2-1.
 *
 * Its registers as ROM 9FEB69B3 drives them at $50F2 4000 (the decoder table at $FFC154F0 names the address; ROM
 * addresses at $FFC0 0000): the POST's video set-up ($FFF03604-$FFF03664)
 * writes 8 to +$02, then the address $7F to +$00 and three bytes $FF to
 * +$01, the address $FF and three bytes 0: the address register at +$00,
 * the color data at +$01 (red, green, blue, then the next entry), a control
 * register at +$02. The same layout as the LC III's CLUT DAC, whose video
 * driver family the PDM ROM's built-in video driver belongs to
 * (".Display_Video_Apple_Sonora", "Built-In DRAM Video", $FFEF17ED;
 * Livadix's sonora.h): control bits 2-0 hold the pixel depth there†; bit 3,
 * which the POST sets, is kept†. +$03 is kept and read back†.
 * Reads of +$01 return the entries' bytes in the same order.
 *
 * Livadix's own code. */
#ifndef MC_ARIEL_H
#define MC_ARIEL_H

#include "hw.h"

#include <stdint.h>

#define MC_ARIEL_SIZE 0x1000u       /* the registers repeat every 4 bytes */

typedef struct mc_ariel {
    mc_dev dev;
    uint8_t addr, rgb, ctl, reg3;
    uint8_t clut[256][3];
    uint32_t palette[256];      /* $FFrrggbb by entry */
} mc_ariel;

extern const mc_dev_ops mc_ariel_ops;
void mc_ariel_init(mc_ariel *a, const mc_hw_host *host, const char *name);
/* The 256 entries as $FFrrggbb. */
const uint32_t *mc_ariel_palette(const mc_ariel *a);
/* The colors of the 2^depth pixel values (depth 1-8): a pixel of d bits
 * reaches the CLUT in its top bits with the others set, as on the LC III's
 * DAC (sonora.c)†. */
void mc_ariel_view_palette(const mc_ariel *a, unsigned depth, uint32_t *out);

#endif
