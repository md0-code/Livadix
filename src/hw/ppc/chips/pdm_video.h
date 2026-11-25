/* Livadix — the built-in video of the Power Macintosh 6100, 7100 and 8100:
 * the AMIC's video timing and monitor sense registers and the data path
 * chips' frame fetch from main RAM. Its CLUT DAC is Ariel II (ariel.h).
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994): chapter
 * 2, "Apple Memory-Mapped I/O Controller" (monitor support), "Data Path
 * Chips" (FIFO buffering of the video data fetched from RAM in eight-beat
 * bursts, its timing to Ariel II), Table 2-4 (video refresh on the bus);
 * chapter 3, "Built-in Interface for Video Monitors" (system RAM as the
 * frame buffer, monitors up to 16 inches), Table 3-8 (the monitors and
 * depths: 12-inch 512 x 384 and 14-inch 640 x 480 to 16 bits, 15-inch
 * portrait 640 x 870, 16-inch 832 x 624 and VGA 640 x 480 to 8 bits),
 * Table 3-9 (the monitor ID sense lines), Table 3-10 (each monitor's dot
 * clock, dots per line, lines per frame and vertical blanking lines).
 * Apple Technical Note HW 30 (the standard and extended sense codes).
 *
 * The registers as ROM 9FEB69B3 drives them at $50F2 8000 (ROM addresses at $FFC0 0000). The ROM's built-in video
 * driver is the LC III's family (".Display_Video_Apple_Sonora", "Built-In
 * DRAM Video", $FFEF17ED) and the PDM's code uses the same three
 * registers as Sonora's video (Livadix's sonora.h):
 *   +$00 control: bit 7 set holds the video off (the POST writes $9F at
 *        $FFF035D8, the start-up chime sets bit 7 at $FFCE59E2, the
 *        driver clears it to run the sense lines and sets it again with
 *        no monitor); bits 6-0 the timing, kept†
 *   +$01 pixel depth: 0-4 for 1, 2, 4, 8, 16 bits† (the POST writes 0,
 *        $FFF035F0)
 *   +$02 sense lines: bits 2-0 drive the lines (0 pulls one low), bits
 *        6-4 read them (the memory set-up drives 7, 3, 5, 6 and builds the
 *        extended code, $FFCCDBBE-$FFCCDC34; $3F is no monitor)
 * The registers repeat every 4 bytes through the window†.
 * The vertical blanking: an output line asserted through the blanking
 * lines of each frame (the AMIC's pseudo-VIA2 slot flag bit 6, "VBL", the
 * Developer Note's Figure 2-2; amic.h). With no monitor the timing runs at
 * the 13-inch monitor's rate†.
 * The frame buffer: in RAM at the address the machine gives (the ROM sets
 * aside $97000 bytes below its DMA area for the video when a monitor
 * answers, $FFCCDCB6; where the DP chips fetch from is not programmed in
 * these registers†), rows of width x depth / 8 bytes†.
 *
 * Output line (mc_hw_host line, 1 = asserted): MC_PDM_VIDEO_LINE_VBL.
 *
 * Livadix's own code. */
#ifndef MC_PDM_VIDEO_H
#define MC_PDM_VIDEO_H

#include "hw.h"

#include <stdint.h>

enum { MC_PDM_VIDEO_LINE_VBL = 0 };

#define MC_PDM_VIDEO_SIZE 0x1000u
#define MC_PDM_VIDEO_FB_SIZE 0x97000u   /* the ROM's reservation */

typedef enum mc_pdm_monitor {
    MC_PDM_MONITOR_NONE,
    MC_PDM_MONITOR_RGB12,       /* 12-inch RGB, 512 x 384 (sense 2) */
    MC_PDM_MONITOR_RGB13,       /* 13/14-inch RGB, 640 x 480 (sense 6) */
    MC_PDM_MONITOR_PORTRAIT,    /* 15-inch portrait, 640 x 870 (sense 1) */
    MC_PDM_MONITOR_RGB16,       /* 16-inch RGB, 832 x 624 (extended $2D) */
    MC_PDM_MONITOR_VGA,         /* VGA, 640 x 480 (extended $17) */
    MC_PDM_MONITORS
} mc_pdm_monitor;

typedef struct mc_pdm_video {
    mc_dev dev;
    uint8_t ctl, depth, sense;  /* the three registers as written */
    mc_pdm_monitor monitor;
    uint64_t frame, blank;      /* C15M periods per frame, and of its blanking */
    uint64_t frame_start;
    uint8_t vbl;
    uint32_t base;              /* the frame buffer's physical address */
} mc_pdm_video;

extern const mc_dev_ops mc_pdm_video_ops;

void mc_pdm_video_init(mc_pdm_video *v, const mc_hw_host *host, const char *name, mc_pdm_monitor monitor);
/* The monitor of a screen size (NONE when the built-in video drives none). */
mc_pdm_monitor mc_pdm_video_monitor_for(uint32_t width, uint32_t height);
/* The deepest depth Table 3-8 gives that monitor (0 for none). */
uint32_t mc_pdm_video_max_depth(mc_pdm_monitor monitor);
/* The levels the three sense lines read (bits 2-0) as now driven. */
uint8_t mc_pdm_video_sense(const mc_pdm_video *v);
/* Where the frame buffer is (physical). */
void mc_pdm_video_set_base(mc_pdm_video *v, uint32_t base);

typedef struct mc_pdm_video_view {
    uint32_t width, height, depth, row_bytes;
    uint32_t base;              /* physical address of the first row */
} mc_pdm_video_view;
/* 0 when the video is held off, no monitor is connected or the depth is
 * not one Table 3-8 offers on it. */
int mc_pdm_video_view_get(mc_pdm_video *v, mc_pdm_video_view *view);

#endif
