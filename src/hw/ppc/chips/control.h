/* Livadix — Control (343S1154), the built-in video controller of the 7500,
 * 8500, 7600 and 8600: addressing and control of the VRAM frame buffer and
 * the monitor's timing, a function on Chaos's video bus (bandit.h), its
 * colours through RaDACal (radacal.h).
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * "Video Subsystem ICs" (Control, addressing and control for the video
 * subsystem), "Video Frame Buffer" (four VRAM SIMM slots of 1 MB: 2 or
 * 4 MB; 24 bpp up to 17-inch monitors with 2 MB, 16 bpp on a 21-inch;
 * 24 bpp on all with 4 MB), "Video Bus" (Control and Chaos, synchronous
 * with the system bus), Table 3-6 (monitor sense codes); Power Macintosh
 * 7300/7600/8600/9600 Developer Note (1996) (the 7600 and 8600 keep it).
 *
 * As ROM 9630C68B uses it (ROM file offsets): Open
 * Firmware's control FCode (file $346634): "AAPL,343S1154", a display; its
 * reg property BAR $14 (the registers, $1000 bytes) and BAR $18 (a 64 MB
 * aperture, the frame buffer at + $80 0000); the chaos node writes its
 * configuration register $10 ($02) and BAR $14 before the probe; the
 * function's IDs, device $0003 class $038000 (the record before the FCode,
 * file $346624). Registers little-endian at $10 steps (the display driver,
 * file $26BD50, and the FCode):
 *  $010-$100  timing counters (12 bits): Open Firmware writes its tables'
 *        16 values from $010 (the field at $010 + $10 i, file $346ABF; 640 x
 *        480 at 67 Hz and at 60 Hz), the display driver the same 16;
 *        vertical counts in half lines ($020 the active end, $030 its
 *        start, $070 the total), horizontal in pixel pairs ($0A0 the active
 *        end, $0B0 its start, $0D0 the total)
 *  $120  a serial interface to the pixel clock generator (bits toggled)
 *  $130  the frame buffer's start in VRAM (the driver writes $200)
 *  $140  the row stride: (width x bytes per pixel + $20) x 1 or 2
 *  $150  the monitor sense lines (the FCode's sense word, file $3468FB):
 *        a write's bits 5-3 release (1) or pull low (0) lines 2-0, a read's
 *        bits 8-6 are the lines; $38 reads the sense code, then pulling one
 *        line at a time gives the extended code (lines 1-0 with line 2
 *        low its bits 5-4, lines 2 and 0 with line 1 low bits 3-2, lines
 *        2-1 with line 0 low bits 1-0); codes 6 and 7 are extended (13"
 *        Hi-Res $2B, VGA $17 in its table, file $3468BB)
 *  $160, $170, $180  pixel format and mode (the FCode writes $170 = 2
 *        for 8 bits per pixel; $160 bits 6, 3 and 2 the VRAM arrangement the
 *        driver tries while sizing VRAM by aliasing)
 *  $190  interrupt control (the driver writes 4, then $C); $1A0 bit 2 the
 *        vertical blanking, polled.
 *
 * Livadix's model: the registers are kept as written (12-bit timing, 32-bit
 * others); the VRAM (2 or 4 MB) repeats through the aperture from
 * + $80 0000 (bytes in address order); the sense lines from the monitor
 * the board connects; the picture: width from $0A0 and $0B0, height from
 * $020 and $030 (a start $60 0000 further with $160 bit 6 clear, the
 * 8500's arrangement†), the row stride from $140 and the depth from
 * it (bytes per pixel the one of 1, 2, 4 that fits (stride / k - $20) /
 * width for k = 1 or 2)†, from VRAM + $130. The vertical blanking sets $1A0
 * bit 2 for its first 1/50 of each 60 Hz† frame and raises
 * MC_CONTROL_LINE_VBL while $190 bit 2 is set; a write with bit 3 drops
 * the line†. Monitor sense and the clock's serial protocol are not
 * modelled: kept as written†.
 *
 * Livadix's own code. */
#ifndef MC_CONTROL_H
#define MC_CONTROL_H

#include "hw.h"
#include "pci.h"

#include <stdint.h>

enum { MC_CONTROL_LINE_VBL = 0 };

#define MC_CONTROL_DEVICE 11u           /* on Chaos's bus */
#define MC_CONTROL_REGS 0x1000u
#define MC_CONTROL_APERTURE 0x04000000u
#define MC_CONTROL_FB 0x00800000u
enum { MC_CONTROL_SENSE = 0x150, MC_CONTROL_MODE = 0x160, MC_CONTROL_START = 0x130, MC_CONTROL_STRIDE = 0x140, MC_CONTROL_PIXEL = 0x170,
       MC_CONTROL_INT = 0x190, MC_CONTROL_STATUS = 0x1A0 };

typedef struct mc_control_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
} mc_control_view;

typedef struct mc_control_chip {
    mc_dev dev;
    mc_pci_config cfg;
    uint32_t reg[MC_CONTROL_REGS / 16];
    uint8_t *vram;
    uint32_t vram_size;
    int vbl, irq, irq_ack;
    uint64_t frame;
    uint8_t sense, sense_ext;       /* the monitor's sense lines and extended code */
} mc_control_chip;

extern const mc_dev_ops mc_control_ops;     /* the registers (little-endian values) */

/* A controller with `vram_size` bytes (2 or 4 MB); 0 on failure. */
int mc_control_init(mc_control_chip *c, const mc_hw_host *host, const char *name, uint32_t vram_size);
void mc_control_free(mc_control_chip *c);
void mc_control_reset(mc_control_chip *c);
/* The function for Chaos (mc_bandit_attach at MC_CONTROL_DEVICE). */
mc_pci_function mc_control_function(mc_control_chip *c);
/* The monitor on the connector: its sense code (0-7) and, for codes 6
 * and 7, its extended code (6 bits; control.c). */
void mc_control_set_monitor(mc_control_chip *c, uint8_t sense, uint8_t sense_ext);
/* 1 while the function answers its BARs (control.c). */
int mc_control_decoding(const mc_control_chip *c);
/* What the monitor shows; 0 when the registers describe no picture. */
int mc_control_view_get(const mc_control_chip *c, mc_control_view *v);

#endif
