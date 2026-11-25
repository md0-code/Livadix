/* Livadix — the Macintosh IIci's RAM-Based Video controller (RBV), and the
 * Bt478 CLUT DAC beside it.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990):
 * chapter 3, "RAM-Based Video controller (RBV)" (the GLUE's clocks and
 * interrupt priority, the VIA2 emulation registers on their own 8-bit bus,
 * the video: monitor ID lines, timing, bursts of eight longwords from bank
 * A, pixels of 1, 2, 4 or 8 bits to the CLUT DAC) and Figure 3-8 (RBV at
 * $5002 6000, VDAC at $5002 4000); chapter 4, "VIA2 functions in the
 * Macintosh IIci computer", Table 4-9 (port A: the slot interrupts, bit 6
 * v2IRQ0 the built-in video's, bit 7 reserved), Table 4-15 (port B: bit 7
 * /Par.Test, 6 v2SNDEXT, 5-4 v2TM0A/v2TM1A, 3 vFC3 = /CFLUSH, 2
 * v2PowerOff, 1 v2BusLk, 0 /CEnable), Table 4-16, Table 4-27 (the
 * interrupt flags: 4 ASC, 3 SCSI IRQ, 2 /EXP.IRQ, 1 slot, 0 SCSI DRQ; no
 * timers) and "Auxiliary Control register" (none on the RBV); chapter 11,
 * "Handshaking for SCSI data transfers" (the 5380's IRQ and DRQ in the
 * RBV's interrupt flags); chapter 12, "Video circuits in the Macintosh
 * IIci" (Table 12-3 screen sizes, 8 bits not on the portrait display;
 * Tables 12-5 and 12-6 timing; Figure 12-4 MONID1-3 = SENSE0-2; Table
 * 12-8 the sense codes: 001 portrait, 110 standard 640 x 480; the screen
 * buffer from physical 0 in bank A).
 *
 * Register offsets and bit meanings beyond those tables are read from the
 * IIci ROM 368CADFE, each cited where it is used in
 * rbv.c. The registers (byte wide):
 *   $00 RvDataB   port B (Table 4-15)
 *   $01 RvExp     stored†
 *   $02 RvSIFR    slot interrupt flags (port A, Table 4-9)
 *   $03 RvIFR     interrupt flags (Table 4-27), bit 7 any enabled one
 *   $10 RvMonP    monitor parameters: depth, sense lines, video off
 *   $11 RvChpT    chip test, stored†
 *   $12 RvSIER    slot interrupt enables
 *   $13 RvIER     interrupt enables (bit 7 set: set the bits written,
 *                 clear: clear them, as the 6522's IER)
 * The register block repeats through the RBV's window.
 *
 * Bt478 (Brooktree; the IIci's CLUT DAC, Guide, "CLUT DAC in the Macintosh
 * IIci"): its registers as the ROM's video code addresses them (rbv.c).
 *
 * Output lines (mc_hw_host line, 1 = asserted):
 *   MC_RBV_LINE_IRQ   the level-2 interrupt to the CPU
 *   MC_RBV_LINE_TICK  the 60.15 Hz tick to VIA1 CA1 (a square wave; state
 *                     is the level)
 *   MC_RBV_LINE_PB    port B as written (state = the byte), reported on
 *                     every write of $00: bit 2 v2PowerOff, low switches
 *                     the power off (Guide, Table 4-15)
 * Inputs: mc_rbv_set_irq (the ASC, the 5380's IRQ and DRQ, /EXP.IRQ),
 * mc_rbv_set_slots (the NuBus slots' lines).
 *
 * Livadix's own code. */
#ifndef MC_RBV_H
#define MC_RBV_H

#include "../hw.h"

#include <stdint.h>

enum { MC_RBV_LINE_IRQ = 0, MC_RBV_LINE_TICK = 1, MC_RBV_LINE_PB = 2 };

/* Interrupt flag bits (Guide, Table 4-27). */
enum {
    MC_RBV_IRQ_SCSI_DRQ = 0x01, MC_RBV_IRQ_SLOT = 0x02, MC_RBV_IRQ_EXP = 0x04,
    MC_RBV_IRQ_SCSI = 0x08, MC_RBV_IRQ_ASC = 0x10
};

/* Registers. */
enum {
    MC_RBV_DATAB = 0x00, MC_RBV_EXP = 0x01, MC_RBV_SIFR = 0x02, MC_RBV_IFR = 0x03,
    MC_RBV_MONP = 0x10, MC_RBV_CHPT = 0x11, MC_RBV_SIER = 0x12, MC_RBV_IER = 0x13
};

/* The monitors by sense code (Guide, Table 12-8; Apple Technical Note HW 30,
 * Table S-1). */
enum {
    MC_RBV_SENSE_PORTRAIT = 1,  /* 640 x 870 */
    MC_RBV_SENSE_RGB12 = 2,     /* 512 x 384 */
    MC_RBV_SENSE_HIRES = 6,     /* 640 x 480 */
    MC_RBV_SENSE_NONE = 7
};

#define MC_RBV_SIZE 0x2000u

typedef struct mc_rbv {
    mc_dev dev;
    /* VIA2 emulation. */
    uint8_t datab, datab_in;    /* port B as written; the levels of its inputs */
    uint8_t exp, chpt;
    uint8_t ifr, ier;           /* bits 0-6 */
    uint8_t slots;              /* port A sources asserted (bit = asserted) */
    uint8_t sier;
    uint8_t irq;                /* the line as last reported */
    /* Video. */
    uint8_t monp;
    uint8_t sense;              /* the monitor's sense code (MC_RBV_SENSE_*) */
    uint64_t frame, blank;      /* C15M periods per frame, and of its vertical blanking */
    uint64_t frame_start;       /* when the current frame began */
    uint8_t vbl;                /* in the vertical blanking interval (the video's line asserted) */
    uint64_t tick_start;        /* the 60.15 Hz tick's phase */
    uint8_t tick;               /* its level */
    uint64_t next;              /* the scheduled deadline */
    /* The Bt478. */
    uint8_t dac_addr, dac_rgb, dac_rd_addr, dac_rd_rgb, dac_mask;
    uint8_t clut[256][3];
    uint32_t palette[256];
    uint32_t view_palette[256];     /* the palette by pixel value at the current depth */
} mc_rbv;

extern const mc_dev_ops mc_rbv_ops;        /* the RBV's registers */
extern const mc_dev_ops mc_rbv_vdac_ops;   /* the Bt478 */

/* A RBV with a monitor of sense code `sense` connected. */
void mc_rbv_init(mc_rbv *r, const mc_hw_host *host, const char *name, uint8_t sense);
/* The sense code of the Apple monitor of a screen size (MC_RBV_SENSE_NONE
 * when the RBV drives no monitor of that size). */
uint8_t mc_rbv_sense_for(uint32_t width, uint32_t height);
/* The deepest depth the RBV shows on that monitor. */
uint32_t mc_rbv_max_depth(uint8_t sense);
/* An interrupt source (MC_RBV_IRQ_* other than SLOT) changed. */
void mc_rbv_set_irq(mc_rbv *r, uint8_t bit, int asserted);
/* The slot interrupt lines: port A bits asserted (bit 6 the built-in
 * video's, bits 3-5 slots $C-$E). */
void mc_rbv_set_slots(mc_rbv *r, uint8_t asserted);
/* Port B inputs (the sound jack, the NuBus transfer acknowledges). */
void mc_rbv_set_port_b(mc_rbv *r, uint8_t levels, uint8_t mask);

/* The picture: the screen buffer from `bank_a` (physical 0). */
typedef struct mc_rbv_view {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
} mc_rbv_view;
/* 0 when the video is off or no monitor is connected. */
int mc_rbv_view_get(mc_rbv *r, const uint8_t *bank_a, uint32_t bank_a_size, mc_rbv_view *view);

#endif
