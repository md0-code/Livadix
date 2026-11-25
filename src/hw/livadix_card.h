/* Livadix — the Livadix Display Card: Livadix's own NuBus
 * video card, which offers every resolution and depth of
 * docs/NOTES.md in 2 MiB of VRAM.
 *
 * Standard slot space of its slot s ($Fs00 0000 + offset):
 *   $00 0000  VRAM (MC_LIVADIX_CARD_VRAM bytes; the window up to $7F FFFF is
 *             kept for more VRAM)
 *   $C0 0000  the registers below, one 4 KiB page
 *   $FF 0000  the declaration ROM, right-aligned to $FF FFFF, all four
 *             byte lanes (Designing Cards and Drivers for the Macintosh
 *             Family, 3rd ed., chapter 8 "The format block", Table 8-2)
 * Everything else in the slot bus-errors.
 *
 * Registers (longwords; a byte or word access takes its lane, as on the
 * 32-bit NuBus):
 *   $000 ID        'LVDX' (read-only)
 *   $004 VERSION   1
 *   $008 VRAM      VRAM size in bytes
 *   $00C MONITOR   the resolution the display was set up with (an index in
 *                  the RES table): the configuration's video.resolution,
 *                  what a real card learns from its monitor's sense lines
 *   $010 MODES     entries in the RES table
 *   $014 REFRESH   vertical refresh in mHz
 *   $018 INIT_DEPTH   the configured start depth (video.depth), which the
 *                  PCI card's driver starts in
 *   $020 WIDTH, $024 HEIGHT   the displayed size in pixels
 *   $028 DEPTH     bits per pixel: 1, 2, 4 and 8 index the CLUT; 16 is
 *                  x-R5-G5-B5 and 32 x-R8-G8-B8, each component taken
 *                  through the CLUT's own channel (an 8-bit index: 5-bit
 *                  components c as c << 3 | c >> 2)
 *   $02C STRIDE    bytes from one row to the next
 *   $030 BASE      offset of the first row in VRAM
 *   $040 CONTROL   bit 0 the display is on, bit 1 VBL interrupts enabled
 *   $044 STATUS    bit 0 a vertical blank happened; writing 1 clears it
 *   $050 CLUT_INDEX   the CLUT entry the data register reaches
 *   $054 CLUT_DATA    red, green, blue of that entry, one byte per access
 *                     in that order; the index steps after blue
 *   $058 GAMMA_INDEX, $05C GAMMA_DATA   the same for the gamma tables, one
 *                  entry per channel, applied after the CLUT
 *                  (the CLUT and the gamma tables reset to linear ramps)
 *   $100 RES[n]    width << 16 | height of resolution n (read-only)
 * The blitter: an engine on the card
 * that fills, inverts and copies rectangles of pixels in VRAM.
 *   $200 BLT_CAPS  bit 0 the blitter is there (0 with video.acceleration
 *                  false: the card's software then leaves QuickDraw alone),
 *                  bits 8-15 its version (1) (read-only)
 *   $204 BLT_STATUS   bit 0 busy: an operation is under way
 *   $208 BLT_OP    1 fill (each pixel the pattern's: FG where its bit is 1,
 *                  BG where 0), 2 invert (each pixel XORed with FG where
 *                  the pattern's bit is 1), 3 copy (the source rectangle to
 *                  the destination, overlapping or not, as if through a
 *                  buffer)
 *   $20C BLT_DEPTH bits per pixel, 1, 2, 4, 8, 16 or 32
 *   $210 BLT_DST, $214 BLT_DST_STRIDE   the destination pixel map: the
 *                  VRAM offset of its first row and its row bytes
 *   $218 BLT_SRC, $21C BLT_SRC_STRIDE   the source pixel map (copy)
 *   $220 BLT_DX, $224 BLT_DY   the destination rectangle's top-left pixel
 *   $228 BLT_W, $22C BLT_H     its size in pixels
 *   $230 BLT_SX, $234 BLT_SY   the source rectangle's top-left pixel
 *   $238 BLT_FG, $23C BLT_BG   pixel values
 *   $240 BLT_PAT0, $244 BLT_PAT1   an 8x8 pattern, one byte per row from
 *                  the high byte of PAT0, the leftmost pixel bit 7
 *   $248 BLT_PATX, $24C BLT_PATY   pixel (x, y) takes the pattern's row
 *                  (y + PATY) & 7 and column (x + PATX) & 7
 *   $250 BLT_GO    a write starts the operation; one outside VRAM or with
 *                  an unknown operation or depth does nothing
 *   $254 BLT_COUNT operations done (read-only)
 *   $258 BLT_CLIPS the clip list's length, 0-128: 0 draws the whole
 *                  destination rectangle, n only where it meets the first
 *                  n clip rectangles (which must not overlap)
 *   $400 CLIP[n]   two longs per clip rectangle, in destination pixels:
 *                  top << 16 | left, then bottom << 16 | right
 * The pixels change at once (a copy reads its whole source before writing,
 * so the clip list's order does not matter); BLT_STATUS stays busy for the
 * time the engine takes (MC_LIVADIX_BLT_*), as software polling it sees.
 * The vertical blank comes at the refresh rate in guest time; with CONTROL
 * bit 1 set it asserts the slot's interrupt line (MC_LIVADIX_CARD_LINE_IRQ)
 * until STATUS is cleared.
 *
 * Livadix's own design. */
#ifndef MC_LIVADIX_CARD_H
#define MC_LIVADIX_CARD_H

#include "hw.h"
#include "nubus.h"
#include "ppc/chips/pci.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_LIVADIX_CARD_LINE_IRQ };

#define MC_LIVADIX_CARD_VRAM (2u << 20)
#define MC_LIVADIX_CARD_VRAM_OFFSET 0x000000u
/* Color QuickDraw's bit-shifted CopyBits loop (rMask0, Quadra 700 ROM
 * $4082C962 and $4082CE12: BFEXTU (A4){D6:32},D0 with D6 in -31..0) reads up
 * to four bytes below row 0 when copying from its first longword to a pixel
 * further right in a destination longword: the screen starts 64 bytes into
 * VRAM (MinorBaseOS in livadixcard.yaml) so those bytes lie in mapped VRAM. */
#define MC_LIVADIX_CARD_FB_OFFSET 0x000040u
#define MC_LIVADIX_CARD_REGS_OFFSET 0xC00000u
#define MC_LIVADIX_CARD_ROM_OFFSET 0xFF0000u
#define MC_LIVADIX_CARD_ROM_WINDOW 0x10000u
#define MC_LIVADIX_CARD_REFRESH_MHZ 60000u
#define MC_LIVADIX_CARD_ID 0x4C564458u            /* 'LVDX' */
/* The BoardId of its declaration ROM (src/hw/guest/declrom/livadixcard.yaml;
 * not assigned by Apple†), which the Slot Manager keeps in the slot's PRAM,
 * and the PCI card's vendor and device IDs (not assigned by the PCI SIG†). */
#define MC_LIVADIX_CARD_BOARD_ID 0x4D43u
#define MC_LIVADIX_PCI_VENDOR 0x4D43u
#define MC_LIVADIX_PCI_DEVICE 0x0001u
#define MC_LIVADIX_PCI_CLASS 0x038000u
#define MC_LIVADIX_PCI_SLOT 13u                   /* slot A1 on Bandit 1 */

enum {
    MC_LIVADIX_REG_ID = 0x000, MC_LIVADIX_REG_VERSION = 0x004, MC_LIVADIX_REG_VRAM = 0x008, MC_LIVADIX_REG_MONITOR = 0x00C,
    MC_LIVADIX_REG_MODES = 0x010, MC_LIVADIX_REG_REFRESH = 0x014, MC_LIVADIX_REG_INIT_DEPTH = 0x018,
    MC_LIVADIX_REG_WIDTH = 0x020, MC_LIVADIX_REG_HEIGHT = 0x024, MC_LIVADIX_REG_DEPTH = 0x028, MC_LIVADIX_REG_STRIDE = 0x02C,
    MC_LIVADIX_REG_BASE = 0x030, MC_LIVADIX_REG_CONTROL = 0x040, MC_LIVADIX_REG_STATUS = 0x044,
    MC_LIVADIX_REG_CLUT_INDEX = 0x050, MC_LIVADIX_REG_CLUT_DATA = 0x054, MC_LIVADIX_REG_GAMMA_INDEX = 0x058,
    MC_LIVADIX_REG_GAMMA_DATA = 0x05C, MC_LIVADIX_REG_RES = 0x100,
    MC_LIVADIX_REG_BLT_CAPS = 0x200, MC_LIVADIX_REG_BLT_STATUS = 0x204, MC_LIVADIX_REG_BLT_OP = 0x208, MC_LIVADIX_REG_BLT_DEPTH = 0x20C,
    MC_LIVADIX_REG_BLT_DST = 0x210, MC_LIVADIX_REG_BLT_DST_STRIDE = 0x214, MC_LIVADIX_REG_BLT_SRC = 0x218,
    MC_LIVADIX_REG_BLT_SRC_STRIDE = 0x21C, MC_LIVADIX_REG_BLT_DX = 0x220, MC_LIVADIX_REG_BLT_DY = 0x224, MC_LIVADIX_REG_BLT_W = 0x228,
    MC_LIVADIX_REG_BLT_H = 0x22C, MC_LIVADIX_REG_BLT_SX = 0x230, MC_LIVADIX_REG_BLT_SY = 0x234, MC_LIVADIX_REG_BLT_FG = 0x238,
    MC_LIVADIX_REG_BLT_BG = 0x23C, MC_LIVADIX_REG_BLT_PAT0 = 0x240, MC_LIVADIX_REG_BLT_PAT1 = 0x244, MC_LIVADIX_REG_BLT_PATX = 0x248,
    MC_LIVADIX_REG_BLT_PATY = 0x24C, MC_LIVADIX_REG_BLT_GO = 0x250, MC_LIVADIX_REG_BLT_COUNT = 0x254,
    MC_LIVADIX_REG_BLT_CLIPS = 0x258, MC_LIVADIX_REG_CLIP = 0x400
};
#define MC_LIVADIX_BLT_CLIPS 128
enum { MC_LIVADIX_BLT_FILL = 1, MC_LIVADIX_BLT_INVERT = 2, MC_LIVADIX_BLT_COPY = 3 };
/* The blitter's time (Livadix's own figures): a start of
 * 1 us, then VRAM at 64 bits per 30 ns cycle: a fill writes 8 bytes a
 * cycle (266 MB/s), an inversion reads and writes them (133 MB/s), a copy
 * too. */
#define MC_LIVADIX_BLT_START_NS 1000u
#define MC_LIVADIX_BLT_CYCLE_NS 30u
#define MC_LIVADIX_BLT_CLIP_NS 100u       /* each clip rectangle met */
enum { MC_LIVADIX_CONTROL_DISPLAY = 1, MC_LIVADIX_CONTROL_VBL_IRQ = 2 };
enum { MC_LIVADIX_STATUS_VBL = 1 };

/* The resolutions, in the order of docs/NOTES.md (the declaration
 * ROM's video sResources are 128 + n in the same order). */
#define MC_LIVADIX_CARD_MODES 12
extern const uint16_t mc_livadix_card_modes[MC_LIVADIX_CARD_MODES][2];

/* The blitter, shared by the NuBus and PCI flavours of the card: its
 * registers (BLT_* in order from BLT_OP), clip list and when the current
 * operation finishes (guest time). */
typedef struct mc_livadix_blitter {
    int accel;
    uint32_t reg[20];
    uint32_t count, clips;
    uint32_t clip[MC_LIVADIX_BLT_CLIPS][2];
    uint64_t done;
    uint8_t *scratch;                   /* a copy's source */
    size_t scratch_size;
} mc_livadix_blitter;

void mc_livadix_blitter_init(mc_livadix_blitter *b, int accel);
void mc_livadix_blitter_free(mc_livadix_blitter *b);
void mc_livadix_blitter_reset(mc_livadix_blitter *b);
int mc_livadix_blitter_reg(uint32_t r);
uint32_t mc_livadix_blitter_read(const mc_livadix_blitter *b, uint32_t r, uint64_t now);
int mc_livadix_blitter_write(mc_livadix_blitter *b, uint32_t r, uint32_t v, uint8_t *vram, uint32_t vram_size, uint64_t now);

typedef struct mc_livadix_card {
    mc_dev dev;
    mc_pci_config cfg;                  /* the PCI configuration space (BAR0 VRAM, BAR1 registers, ROM) */
    uint8_t *vram;
    uint32_t vram_size;
    uint8_t *rom;                       /* the ROM window, the image right-aligned */
    size_t rom_size;
    uint32_t monitor, init_depth;
    uint32_t width, height, depth, stride, base, control, status;
    uint8_t clut_index, clut_part, gamma_index, gamma_part;
    uint8_t clut[256][3], gamma[3][256];
    uint8_t irq;
    uint64_t period, next_vbl;
    uint64_t vbl_count;                 /* vertical blanks so far */
    /* What the display shows: the CLUT through the gamma tables, and the
     * direct-mode pixels converted when that is not the identity. */
    int dirty, identity;
    uint32_t palette[256];
    uint8_t lut[3][256];
    uint32_t *direct;
    mc_livadix_blitter blt;
} mc_livadix_card;

/* `monitor`: an index in mc_livadix_card_modes; `rom`: the declaration ROM
 * or PCI expansion ROM image (copied); `accel`: the blitter is there
 * (video.acceleration). 0 when out of memory or the ROM is larger than
 * its window. */
int mc_livadix_card_init(mc_livadix_card *c, const mc_hw_host *host, const char *name, uint32_t vram_size, unsigned monitor,
                      const uint8_t *rom, size_t rom_size, int accel);
void mc_livadix_card_free(mc_livadix_card *c);
/* The initial depth the configuration asks for (1, 2, 4, 8, 16 or 32; 8
 * by default). */
void mc_livadix_card_set_depth(mc_livadix_card *c, uint32_t depth);
extern const mc_dev_ops mc_livadix_card_ops;      /* the register page */
/* Puts the card in NuBus slot `slot`: its VRAM, registers and ROM. */
int mc_livadix_card_attach(mc_livadix_card *c, mc_nubus *nubus, unsigned slot);
/* The PCI function for a Bandit slot (BAR0 VRAM, BAR1 registers, $30 the
 * FCode expansion ROM), and 1 while its memory BARs are decoded. */
mc_pci_function mc_livadix_card_function(mc_livadix_card *c);
int mc_livadix_card_pci_decoding(const mc_livadix_card *c);
/* The index of a resolution in mc_livadix_card_modes, -1 when absent. */
int mc_livadix_card_mode(uint32_t width, uint32_t height);

/* What the display shows now; 0 when it is off or the registers describe
 * no picture inside VRAM. depth 1-8 index `palette`; 16 is x-R5-G5-B5 and
 * 32 x-R8-G8-B8, big-endian. */
typedef struct mc_livadix_card_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_livadix_card_view;
int mc_livadix_card_view_get(mc_livadix_card *c, mc_livadix_card_view *view);

#endif
