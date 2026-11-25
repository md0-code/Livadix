/* Livadix — the Macintosh SE/30's built-in video ("pseudo-slot video" in
 * slot $E).
 *
 * Guide to the Macintosh Family Hardware, 2nd ed. (1990):
 *   - chapter 12, "Video display in the Macintosh SE/30 computer": 512 x 342
 *     black-and-white pixels at 60.15 Hz, 64 KB of video RAM of its own
 *     holding two screen buffers, an 8192-byte declaration ROM, the video
 *     RAM in slot $E's space ($FE00 0000-$FEFF FFFF), no CLUT and no
 *     programmable timing;
 *   - chapter 3, "Video PALs in the Macintosh SE/30 computer" and Table 3-9
 *     (the declaration ROM at the top of the slot's 24-bit 1 MB window, as
 *     the Slot Manager wants it);
 *   - chapter 4, Table 4-5 (VIA1 PA6 vPage2: 0 the alternate screen buffer,
 *     1 the main one), Table 4-14 and its notes (VIA1 PB6 vSyncEnA: 0
 *     enables the slot $E interrupt the video asserts at each vertical
 *     blanking), Table 4-9 and Table 4-20 (slot $E's interrupt on VIA2
 *     PA5 and the slot interrupt on CA1).
 *
 * Observed from the declaration ROM video-se30.bin (CRC32 b74c3463): its format block (8 KB, all four byte lanes); its
 * video sResources (MinorBaseOS 0, MinorLength $D5C0, the 1-bit mode's
 * base offset $8040 and row bytes $40); its PrimaryInit, which makes VIA1
 * PA6 and PB6 outputs, sets both (main buffer, interrupt off) and fills
 * the buffers at $FEE0 8040 and $FEE0 0040 with a gray pattern; its
 * driver, which takes the main or alternate buffer at $FEE0 8040 and
 * $FEE0 0040 with PA6, and whose slot interrupt handler sets then clears
 * PB6 before running the slot's VBL tasks.
 *
 * Livadix's model (†): the 64 KB repeat through slot $E's space but for the
 * last 64 KB of each MB, where the 8 KB ROM repeats (the PrimaryInit's
 * $FEE0 0000, the 24-bit $E0 8040 -> $FE00 8040 and the ROM at both
 * $FE0F FFFF and $FEFF FFFF reach them); the vertical blanking comes every
 * 704 x 370 C15M periods, the classic Macintosh's frame (60.15 Hz); the
 * interrupt is latched at the blanking while PB6 is low and cleared by PB6
 * going high.
 *
 * Livadix's own code. */
#ifndef MC_SE30VIDEO_H
#define MC_SE30VIDEO_H

#include "../bus.h"
#include "../hw.h"

#include <stddef.h>
#include <stdint.h>

#define MC_SE30VIDEO_VRAM 0x10000u
#define MC_SE30VIDEO_ROM 0x2000u
#define MC_SE30VIDEO_BASE 0xFE000000u     /* slot $E's standard slot space */
#define MC_SE30VIDEO_MAIN 0x8040u          /* the screen buffers in the video RAM */
#define MC_SE30VIDEO_ALT 0x0040u
#define MC_SE30VIDEO_ROW_BYTES 64u
#define MC_SE30VIDEO_WIDTH 512u
#define MC_SE30VIDEO_HEIGHT 342u

enum { MC_SE30VIDEO_LINE_IRQ = 0 };

typedef struct mc_se30video {
    mc_dev dev;
    uint8_t vram[MC_SE30VIDEO_VRAM];
    uint8_t rom[MC_SE30VIDEO_ROM];
    int main_page;              /* VIA1 PA6 */
    int irq_enabled;            /* VIA1 PB6 low */
    int irq;                    /* the latched interrupt */
    uint64_t frame_start;       /* guest time of the current frame's start */
    uint32_t palette[2];
} mc_se30video;

/* `rom`: the 8192-byte declaration ROM. 0 when it is not that size. */
int mc_se30video_init(mc_se30video *v, const mc_hw_host *host, const char *name, const uint8_t *rom, size_t rom_size);
extern const mc_dev_ops mc_se30video_ops;
/* Maps the video RAM and the ROM through slot $E's space. */
void mc_se30video_map(mc_se30video *v, mc_bus *bus);
/* VIA1 PA6 and PB6 as the pins are. */
void mc_se30video_set_page(mc_se30video *v, int main_page);
void mc_se30video_set_sync_enable(mc_se30video *v, int pb6);

/* The screen: 512 x 342 x 1, the buffer PA6 selects. */
typedef struct mc_se30video_view {
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_se30video_view;
void mc_se30video_view_get(const mc_se30video *v, mc_se30video_view *view);

#endif
