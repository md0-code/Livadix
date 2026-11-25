/* Livadix — Sonora, the integrated controller of the Macintosh LC III family
 * and its relatives with built-in screens (Color Classic II, LC 550,
 * Performa 275/550/560, Macintosh TV): memory controller, pseudo-VIA2,
 * built-in video and its CLUT DAC.
 *
 * Sources: Macintosh LC III Developer Note (Apple, 1993), chapter 1:
 * "Sonora Integrated Controller" (timing, address decode and memory
 * mapping, video generation, sound control, GLU and VIA functions: "a
 * full-function VIA1, a pseudo-VIA2 similar to that of the V8 gate
 * array"), Table 1-1 (the memory map: RAM from 0, ROM at $4080 0000, I/O
 * at $50F0 0000, VRAM at $60B0 0000, the PDS as slot $E), Table 1-2 (the
 * RAM SIMM configurations, expansion RAM contiguous from $0040 0000),
 * "Video Interface", "VRAM" (512 KB soldered, a SIMM socket for 256 KB
 * more), Table 1-4 (video modes), Table 1-6 (the monitor sense codes,
 * standard and extended), Figures 1-3 to 1-9 (the video timing of each
 * mode). Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (the
 * VIA2 functions the pseudo-VIA2 takes over: slot interrupts active low on
 * port A, the interrupt flag and enable registers with the 6522's set and
 * clear convention). Apple Technical Note HW 30 (the extended sense
 * protocol: drive one line low, read the other two).
 *
 * Register offsets and bit meanings are read from the LC III ROMs
 * ECBBC41C and EC904829 and cited where they are
 * used in sonora.c. Three windows of the I/O block:
 *
 *   $50F2 6000  pseudo-VIA2, byte registers repeating every $20:
 *     $00 port B (written $8F at start; stored)†
 *     $01 RAM configuration: bits 2-0 the SIMM's size, 2^(n-1) MB (0 none,
 *         7 while the ROM sizes memory), bits 5-3 a second SIMM (unused),
 *         bit 6 the 4 MB on the board, bit 7 kept
 *     $02 slot interrupt flags (active low): bit 6 the built-in video's
 *         vertical blanking, bit 5 the PDS (slot $E)
 *     $03 interrupt flags: 0 SCSI DRQ, 1 any slot, 3 SCSI IRQ, 4 sound;
 *         bit 7 any enabled (reads)
 *     $04 VRAM configuration (the ROM's sizing writes 6, then 4 for 512 KB,
 *         6 for 768 KB)
 *     $05 bus speed (2 at 25 MHz, 3 at 33 MHz; stored)†
 *     $10 monitor parameters (written $40 at start; stored)†
 *     $12 slot interrupt enables, $13 interrupt enables (bit 7 set: set the
 *         bits written, clear: clear them)
 *   $50F2 8000  video:
 *     $00 bit 7 set holds the video off while the dot clock is changed
 *         (the driver sets it, programs the clock through Egret and
 *         writes the mode with bit 7 clear, ROM ECBBC41C offset $FDB30);
 *         bits 6-0 the timing of the mode (the sResource's video
 *         parameters: 6 on the 13-inch monitor)
 *     $01 pixel depth: 0-4 for 1, 2, 4, 8, 16 bits
 *     $02 sense lines: bits 6-4 read the lines, bits 2-0 drive them (0 =
 *         pulled low)
 *   $50F1 8000  configuration: the Color Classic II ROM writes $90 to $00
 *         and $B7 to $01 at start on the boards whose ProductInfo record
 *         has feature bit 27 (ROM EDE66CBD offset $4977); stored†
 *   $50F2 4000  CLUT DAC: $00 address, $01 data (red, green, blue, then the
 *         next entry), $02 control (bits 2-0 the depth, as $28001)
 *
 * Output lines (mc_hw_host line, 1 = asserted):
 *   MC_SONORA_LINE_IRQ   the pseudo-VIA2's interrupt (level 2)
 *   MC_SONORA_LINE_TICK  the 60.15 Hz tick to VIA1 CA1 (square wave)
 *
 * Livadix's own code. */
#ifndef MC_SONORA_H
#define MC_SONORA_H

#include "../hw.h"

#include <stdint.h>

enum { MC_SONORA_LINE_IRQ = 0, MC_SONORA_LINE_TICK = 1 };

/* Interrupt flag bits ($03). */
enum {
    MC_SONORA_IRQ_SCSI_DRQ = 0x01, MC_SONORA_IRQ_SLOT = 0x02, MC_SONORA_IRQ_SCSI = 0x08, MC_SONORA_IRQ_SOUND = 0x10
};

/* Pseudo-VIA2 registers. */
enum {
    MC_SONORA_DATAB = 0x00, MC_SONORA_RAMCFG = 0x01, MC_SONORA_SIFR = 0x02, MC_SONORA_IFR = 0x03,
    MC_SONORA_VRAMCFG = 0x04, MC_SONORA_SPEED = 0x05, MC_SONORA_MONP = 0x10, MC_SONORA_SIER = 0x12,
    MC_SONORA_IER = 0x13
};

/* Monitors the built-in video drives (LC III Developer Note, Table 1-6). */
typedef enum mc_sonora_monitor {
    MC_SONORA_NONE,
    MC_SONORA_RGB12,        /* 12-inch RGB, 512 x 384 (sense 2) */
    MC_SONORA_HIRES13,      /* 13-inch RGB, 640 x 480 (sense 6) */
    MC_SONORA_PORTRAIT,     /* portrait, 640 x 870 (sense 1) */
    MC_SONORA_VGA,          /* VGA, 640 x 480 (extended $17) */
    MC_SONORA_RGB16,        /* 16-inch RGB, 832 x 624 (extended $2D) */
    MC_SONORA_MONITORS
} mc_sonora_monitor;

#define MC_SONORA_VRAM_MAX 0xC0000u

typedef struct mc_sonora {
    mc_dev dev;
    /* Pseudo-VIA2. */
    uint8_t datab, ramcfg, vramcfg, speed, monp;
    uint8_t ifr, ier;           /* bits 0-6 */
    uint8_t slots;              /* port A sources asserted (bit = asserted) */
    uint8_t sier;
    uint8_t irq;
    /* Video. */
    uint8_t vctl, vdepth, vsense;
    mc_sonora_monitor monitor;
    uint64_t frame, blank;      /* C15M periods per frame and of its blanking */
    uint64_t frame_start;
    uint8_t vbl;
    uint64_t tick_start;
    uint8_t tick;
    /* CLUT DAC. */
    uint8_t dac_addr, dac_rgb, dac_ctl;
    uint8_t clut[256][3];
    uint32_t palette[256];
    uint32_t view_palette[256];     /* by pixel value at the current depth */
    /* The configuration bytes at $50F1 8000 (the Color Classic II ROM's
     * boards: $90 and $B7 at start; stored)†. */
    uint8_t cfg[2];
    /* VRAM: 512 KB, or 768 KB with the SIMM. */
    uint8_t *vram;
    uint32_t vram_size;
    /* The board remaps RAM after a write of $01. */
    void (*remap)(void *ctx);
    void *remap_ctx;
} mc_sonora;

extern const mc_dev_ops mc_sonora_ops;          /* the pseudo-VIA2, $50F2 6000 */
extern const mc_dev_ops mc_sonora_video_ops;    /* the video registers, $50F2 8000 */
extern const mc_dev_ops mc_sonora_dac_ops;      /* the CLUT DAC, $50F2 4000 */
extern const mc_dev_ops mc_sonora_cfg_ops;      /* the configuration bytes, $50F1 8000 */

/* `vram`: the board's VRAM (vram_size bytes, 512 or 768 KB). */
void mc_sonora_init(mc_sonora *s, const mc_hw_host *host, const char *name, mc_sonora_monitor monitor, uint8_t *vram,
                    uint32_t vram_size, void (*remap)(void *ctx), void *remap_ctx);
/* The monitor of a screen size (NONE when the built-in video drives none). */
mc_sonora_monitor mc_sonora_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth Sonora shows on that monitor with `vram_size` bytes. */
uint32_t mc_sonora_max_depth(mc_sonora_monitor monitor, uint32_t vram_size);
/* An interrupt source (MC_SONORA_IRQ_* other than SLOT) changed. */
void mc_sonora_set_irq(mc_sonora *s, uint8_t bit, int asserted);
/* The slot interrupt lines: bit 5 the PDS (slot $E). */
void mc_sonora_set_slots(mc_sonora *s, uint8_t asserted, uint8_t mask);
/* The levels on the sense lines given what Sonora drives. */
uint8_t mc_sonora_sense(const mc_sonora *s);

/* The RAM layout the configuration register selects: the board's RAM
 * (board_size, usually 4 MB) and the SIMM (simm_size, one or two banks).
 * Calls map(ctx, base, window, offset, size) for each bank: `window` bytes
 * from `base` show the bank's `size` bytes from `offset` in the RAM,
 * repeating. While the ROM sizes memory (SIMM size code 7) the banks sit
 * at 16 MB boundaries: the board's from 0, the SIMM's two banks from
 * $0100 0000 and $0200 0000; otherwise the board's RAM from 0 and the
 * SIMM right after it. */
void mc_sonora_ram_layout(const mc_sonora *s, uint32_t board_size, uint32_t simm_size,
                          void (*map)(void *ctx, uint32_t base, uint32_t window, uint32_t offset, uint32_t size), void *ctx);

typedef struct mc_sonora_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_sonora_view;
/* 0 when the video is off or no monitor is connected. */
int mc_sonora_view_get(mc_sonora *s, mc_sonora_view *view);

#endif
