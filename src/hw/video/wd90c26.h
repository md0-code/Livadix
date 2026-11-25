/* Livadix — the color LCD video of the PowerBook 165c and 180c: Pangola, an
 * Apple bus interface, and the Western Digital WD90C26 flat-panel VGA
 * controller, seen as a NuBus video card in slot $C.
 *
 * Sources: Macintosh PowerBook 165c Developer Note (Apple), chapter 1,
 * "Color LCD Interface" (the WD90C26 controller and WD90C55 driver behind
 * Pangola, a 256-entry CLUT and RAMDAC, 512 KB of DRAM, a declaration ROM;
 * "The frame buffer appears to the 68030 as a continuous RAM array of
 * 512 KB beginning at $FC04 0000"; Pangola packs and unpacks the 1-, 2- and
 * 4-bit modes into the controller's byte per pixel, so the processor sees
 * QuickDraw's packed pixels), Figure 1-2 (slot $C the internal video),
 * "LCD Screen" (640 x 400 on the 165c). The VGA register set (sequencer,
 * CRT controller, graphics controller and attribute registers through
 * their index and data ports; the DAC's write index $3C8 and data $3C9
 * taking red, green and blue, six bits each; input status 1 at $3DA) is
 * the IBM VGA's, which the WD90C26 keeps.
 *
 * Observed from the declaration ROM video-pb180c.bin ("PowerBook Video ROM
 * 1.1", its driver .Display_Video_Apple_WD): the VGA
 * ports at slot offset $40 0000 + port, Pangola's control word at
 * $80 0000, the sResources $80 (640 x 400) and $81 (640 x 480) with 1, 2,
 * 4 and 8 bits (rows of 80, 160, 320 and 640 bytes) and the minor base
 * offset $8 0000 (the screen at $FC08 0000, 256 KB into the frame
 * buffer); the driver waits for the vertical retrace in bit 3 of $3DA.
 * Pangola's control word (the driver's mode table at ROM offset $149A,
 * PrimaryInit at $85C-$87C): bits 6-5 the depth (3 1 bit, 2 2 bits, 1 4
 * bits, 0 8 bits), bit 3 the display on (set last), bits 2-0 a reset
 * sequence $3, $7, $6 at start†; the word at $84 0000 is written $C8 and
 * kept†.
 *
 * Livadix's own code. */
#ifndef MC_WD90C26_H
#define MC_WD90C26_H

#include "../hw.h"

#include <stdint.h>

#define MC_WD_VRAM_OFFSET 0x040000u     /* the frame buffer in the slot */
#define MC_WD_VRAM_SIZE 0x80000u
#define MC_WD_PORTS 0x400000u           /* the VGA ports: + port number */
#define MC_WD_PANGOLA 0x800000u         /* Pangola's control word */
#define MC_WD_SCREEN 0x40000u           /* the screen in the frame buffer */
#define MC_PANGOLA_ON 0x0008u

typedef struct mc_wd90c26 {
    mc_dev dev;
    uint32_t height;                    /* the panel: 400 or 480 lines */
    uint8_t *vram;
    uint8_t seq_index, seq[32];
    uint8_t crtc_index, crtc[128];
    uint8_t gc_index, gc[64];
    uint8_t attr_index, attr[32], attr_flip;
    uint8_t misc, feature;
    uint8_t dac_write, dac_read, dac_step, dac_mask;
    uint8_t dac[256][3];
    uint16_t pangola;
    uint8_t other[16];                  /* other ports ($46E8, $102): kept† */
    uint64_t frame_start;
    uint32_t palette[256];
} mc_wd90c26;

typedef struct mc_wd90c26_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_wd90c26_view;

void mc_wd90c26_init(mc_wd90c26 *w, const mc_hw_host *host, const char *name, uint8_t *vram, uint32_t height);
/* The slot's registers: offsets within slot $C's 16 MB (the VGA ports and
 * Pangola; the frame buffer and the declaration ROM are memory). */
extern const mc_dev_ops mc_wd90c26_ops;
int mc_wd90c26_view_get(mc_wd90c26 *w, mc_wd90c26_view *v);

#endif
