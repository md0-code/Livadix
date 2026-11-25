/* Livadix — NCR 53C96 SCSI controller, the initiator of
 * the Quadra family's SCSI bus.
 *
 * Written from the NCR 53C94/53C95/53C96 Advanced SCSI Controller data sheet
 * (NCR, February 1990): Table 7 register set, the transfer counter, FIFO,
 * command register (two deep), status, interrupt, sequence step, FIFO flags,
 * configuration 1-3, clock conversion, time-out and synchronous registers;
 * Tables 8-11 (initiator selection sequences and their sequence steps);
 * Table 19-23 command set: miscellaneous (NOP, flush FIFO, reset chip, reset
 * SCSI bus), disconnected (select with and without ATN, with ATN and stop,
 * with ATN3, enable/disable selection, reselect), initiator (transfer
 * information, initiator command complete sequence, message accepted,
 * transfer pad, set/reset ATN); DMA and non-DMA forms (bit 7).
 *
 * The chip drives an mc_scsi_bus (scsi_bus.h) as initiator. Target-role
 * commands answer with an illegal-command interrupt, as the data sheet
 * specifies when the chip is not in the target state: nothing on a Macintosh
 * selects the Mac. Synchronous transfer registers are kept; transfers are
 * asynchronous (the targets answer at once). Parity is never in error.
 *
 * Registers: offsets 0-15 (the register number; the machine maps its
 * stride, $10 on the Quadra 700: the ROM's registers at $50F0F000 + $10 n).
 * Interrupt latency: a command's interrupt
 * arrives `latency` C15M periods after it completes on the bus; a selection
 * that nobody answers ends after the time-out the ROM programmed:
 * RV x 8192 x CCF clock periods (data sheet, "Time-Out").
 *
 * Output lines (mc_hw_host line):
 *   MC_NCR53C96_LINE_IRQ  INT (1 = asserted): to VIA2 CB2 on the Quadra 700
 *   MC_NCR53C96_LINE_DRQ  DREQ (1 = asserted): to VIA2 CA2 and DAFB's Turbo
 *                         SCSI status bit 9 on the Quadra 700 (System 7.1's
 *                         and Mac OS 8's SCSI Managers work from them)
 *
 * DMA: the Quadra 700 has no DMA controller on this chip; the CPU moves the
 * data itself through a window where the board's glue turns each access into
 * DACK cycles (pseudo-DMA at +$F100: the ROM moves its data through
 * $50F0F100 and its images). The machine maps that
 * window to mc_ncr53c96_dma_read / mc_ncr53c96_dma_write. */
#ifndef MC_NCR53C96_H
#define MC_NCR53C96_H

#include "hw.h"
#include "scsi_bus.h"

#include <stdint.h>

enum { MC_NCR53C96_LINE_IRQ = 0, MC_NCR53C96_LINE_DRQ = 1 };

/* Register numbers (data sheet Table 7): read / write. */
enum {
    MC_NCR_TC_LO = 0,       /* transfer counter / count LSB */
    MC_NCR_TC_HI = 1,
    MC_NCR_FIFO = 2,
    MC_NCR_COMMAND = 3,
    MC_NCR_STATUS = 4,      /* write: destination ID */
    MC_NCR_INTR = 5,        /* write: select/reselect time-out */
    MC_NCR_SEQ = 6,         /* write: synchronous period */
    MC_NCR_FLAGS = 7,       /* write: synchronous offset */
    MC_NCR_CONFIG1 = 8,
    MC_NCR_CLOCK = 9,       /* write only: clock conversion factor */
    MC_NCR_TEST = 10,       /* write only */
    MC_NCR_CONFIG2 = 11,
    MC_NCR_CONFIG3 = 12,
    MC_NCR_FIFO_BOTTOM = 15 /* write only: reserve FIFO byte */
};

typedef struct mc_ncr53c96 {
    mc_dev dev;
    mc_scsi_bus *bus;
    uint32_t clock_hz;          /* CLK input */
    uint32_t latency;           /* C15M periods from bus completion to INT */
    /* Registers. */
    uint8_t fifo[16];
    unsigned fifo_n;
    uint32_t tc;                /* transfer counter (0-65536) */
    uint16_t tc_reg;            /* transfer count register */
    uint8_t cmd_queue[2];
    unsigned cmd_n;
    uint8_t command;            /* the last command: what reads of register 3 return */
    uint8_t status, intr, seq;  /* as presented */
    uint8_t phase_latched;
    uint8_t stack_status[2], stack_intr[2], stack_seq[2];
    unsigned stacked;           /* interrupts waiting behind the presented one */
    uint8_t dest, timeout, sync_period, sync_offset, config1, ccf, test, config2, config3;
    /* Sequencer. */
    int state;                  /* 0 disconnected, 1 initiator */
    int busy;                   /* a command is executing */
    int dma;                    /* DMA transfer in progress: 0, 1 host writes (to SCSI), 2 host reads (from SCSI) */
    int dma_phase;              /* the information phase the DMA transfer serves (-1: none yet) */
    int dma_select;             /* a DMA selection is taking its message and command bytes */
    unsigned sel_msgs;          /* message bytes it sent */
    int drain;                  /* a DMA input ended with bytes left in the FIFO for DACK */
    int pending;                /* a completion waits for its deadline */
    uint8_t pend_intr, pend_seq;
    int irq, drq;               /* line states */
} mc_ncr53c96;

extern const mc_dev_ops mc_ncr53c96_ops;

/* A chip on `bus`, clocked at `clock_hz` (25 MHz on the Quadra 700: the ROM
 * writes clock conversion factor 5, the data sheet's 20-25 MHz), reset. */
void mc_ncr53c96_init(mc_ncr53c96 *c, const mc_hw_host *host, mc_scsi_bus *bus, uint32_t clock_hz);
/* Hardware reset (the reset op). */
void mc_ncr53c96_reset(mc_ncr53c96 *c);

/* Pseudo-DMA: one CPU access to the DMA window, size 1, 2 or 4 bytes
 * (big-endian, as the 68040 sees memory). Each byte is one DACK cycle: a
 * read takes the next byte from the FIFO (refilled from the bus), a write
 * puts one into the FIFO (sent to the bus); either decrements the transfer
 * counter. Returns the bytes moved: fewer than `size` when DREQ was not
 * asserted for the rest (no DMA command running, counter exhausted, or the
 * target left the phase). The real glue holds the CPU's cycle until DREQ
 * and ends it with a bus error after a time-out†; the machine decides which
 * of the two to model for a short count. */
unsigned mc_ncr53c96_dma_read(mc_ncr53c96 *c, unsigned size, uint32_t *value);
unsigned mc_ncr53c96_dma_write(mc_ncr53c96 *c, unsigned size, uint32_t value);

#endif
