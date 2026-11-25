/* Livadix — Platinum (343S1184), the Power Macintosh 7200's memory and video
 * controller: the RAM banks, ROM and second-level cache, and the built-in
 * video's VRAM and timing, its registers at $F800 0000.
 *
 * Sources: Power Macintosh 7200 Developer Note (Apple, 1995), chapter 2
 * "Platinum Memory and Video Controller IC" (memory and cache up to 1 GB;
 * unlike Hammerhead no interleaving: banks linear; a bank base register per
 * bank), chapter 4 "RAM DIMMs" (banks of 4 MB to 128 MB), the block diagram
 * (the video's DAC, DACula, on Grand Central's bus); Designing PCI Cards and
 * Drivers for Power Macintosh Computers (Apple, 1995), Table 2-1.
 *
 * As ROM 9630C68B uses it (ROM at $FFC0 0000): the
 * registers are words at $10 steps, the value in the low bits.
 *  $000  identification: upper halfword $3001 ($FFF03104); bits 11-8 and
 *        15, 6 the bus clock code and table as on Hammerhead
 *  $020, $040, $050, $150, $1B0  timing, written from tables by the clock
 *        code ($FFF04400, $FFF043A8)
 *  $030  bit 5 the L2 cache present, bits 4-3 its size (0 512 KB, 1 256 KB,
 *        2 1 MB, 3 256 KB), bit 2 the tag-write mode, bit 0 enable
 *        ($FFF03C60); the cache's tags at $F810 0000, zeroed at $20 steps
 *        while bit 2 is set
 *  $060 + $10 n, banks n = 0-7: bits 30-21 the base address (2 MB units),
 *        bit 31 a mode POST sets to size a bank whose first sizing is not a
 *        power of two ($FFF04524: bank n at n x 128 MB before sizing;
 *        $FFF0454C: sized within 64 MB, then packed from 0)
 *  video (the display driver, ROM file $27AB60; its 640 x 480 and 1152 x
 *        870 set-ups traced): $100 the frame buffer's address ($F100 1010
 *        at 640 x 480; the 68k world's ScrnBase is $10 beyond it, as on
 *        Control), $120 the row stride (width x bytes per pixel + $20: $A20
 *        at 640 x 480 x 32, $920 at 1152 x 870 x 16), $290-$320 the
 *        horizontal timing in pixel pairs ($300 the active start, $310 its
 *        end), $330-$390 the vertical in half lines ($370 the active start,
 *        $380 its end), $140, $160, $180, $200, $210, $260, $3A0, $4A0
 *        written (the depth's bytes per pixel in $4A0†); $220 bit 2
 *        polled for the vertical blanking, after two reads of $230
 *        ($FFE831F0-$FFE83208); $170 the monitor sense lines: a write's
 *        bits 2-0 release (1) or pull low (0) lines 2-0, a read's bits 2-0
 *        the lines inverted (the driver writes 7, 3, 5 and 6 and takes
 *        NOT the value read back, RAM copy of file $2829F0); VRAM at
 *        $F100 0000, its size found by
 *        aliasing ("kevnsean" at $F100 0000, "Rush2112" at $F120 0000).
 *
 * Livadix's model: identification and cache are the board's; banks decode as
 * on Hammerhead (mc_mem_bank_windows, 128 MB windows); the VRAM (1, 2 or
 * 4 MB) repeats through its 4 MB window (mc_platinum_vram_ops); the
 * vertical blanking sets $220 bit 2 and raises MC_PLATINUM_LINE_VBL each
 * frame (60 Hz†, blanking 1/50 of a frame†), both drop at its end; a
 * read of $230 acknowledges them early†. OF 1.0.5 quit waits for GC
 * events to drop after clearing them ($FF815884-$FF8158C0). The video registers are
 * kept as written; the picture: width and height from the active timing,
 * the depth from the stride and the width†, from $100 + $10.
 *
 * Output lines: MC_PLATINUM_LINE_MAP (a pulse: a bank register changed),
 * MC_PLATINUM_LINE_VBL (Grand Central's source 30, the display).
 *
 * Livadix's own code. */
#ifndef MC_PLATINUM_H
#define MC_PLATINUM_H

#include "hw.h"
#include "hammerhead.h"

#include <stdint.h>

enum { MC_PLATINUM_LINE_MAP = 0, MC_PLATINUM_LINE_VBL = 1 };

#define MC_PLATINUM_SIZE 0x1000u
#define MC_PLATINUM_BANKS 8u
#define MC_PLATINUM_BANK_WINDOW 0x08000000u    /* 128 MB */
#define MC_PLATINUM_VRAM_WINDOW 0x00400000u
#define MC_PLATINUM_ID_VALUE 0x3001u

enum { MC_PLATINUM_ID = 0x000, MC_PLATINUM_CACHE = 0x030, MC_PLATINUM_BANK = 0x060, MC_PLATINUM_SENSE = 0x170,
       MC_PLATINUM_VBL_STATUS = 0x220, MC_PLATINUM_VBL_ACK = 0x230 };
enum { MC_PLATINUM_L2_512K, MC_PLATINUM_L2_256K, MC_PLATINUM_L2_1M, MC_PLATINUM_L2_NONE = 4 };

typedef struct mc_platinum {
    mc_dev dev;
    uint32_t id;
    uint8_t l2;
    uint32_t reg[MC_PLATINUM_SIZE / 16];
    uint32_t bank_size[MC_PLATINUM_BANKS];
    uint8_t *vram;
    uint32_t vram_size;
    int vbl, blank;                /* acknowledged indication and scan phase */
    uint64_t frame;                 /* C15M periods per frame */
    uint8_t sense, sense_ext;       /* the monitor's sense code and extended code */
} mc_platinum;

extern const mc_dev_ops mc_platinum_ops;        /* offsets from $F800 0000 */
extern const mc_dev_ops mc_platinum_vram_ops;   /* offsets from $F100 0000 */

/* `id_low` the identification's low halfword (the upper is $3001), the
 * cache, the banks' sizes (MC_PLATINUM_BANKS entries), the VRAM size (1, 2
 * or 4 MB). 0 on a bad size or no memory. */
int mc_platinum_init(mc_platinum *p, const mc_hw_host *host, const char *name, uint16_t id_low, unsigned l2, const uint32_t *bank_size, uint32_t vram_size);
void mc_platinum_free(mc_platinum *p);
void mc_platinum_reset(mc_platinum *p);
uint32_t mc_platinum_base(const mc_platinum *p, unsigned n);
/* What the monitor shows (platinum.c); 0 when the registers describe no
 * picture. */
typedef struct mc_platinum_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
} mc_platinum_view;
int mc_platinum_view_get(const mc_platinum *p, mc_platinum_view *v);
/* The monitor on the connector: its sense code (0-7) and, for codes 6 and
 * 7, its extended code (as Control's, control.h). */
void mc_platinum_set_monitor(mc_platinum *p, uint8_t sense, uint8_t sense_ext);
unsigned mc_platinum_windows(const mc_platinum *p, mc_mem_window *w, unsigned max);

#endif
