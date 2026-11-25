/* Livadix — NCR 5380 / 53C80 SCSI controller, the SCSI
 * chip of the Macintosh Plus and of every 68k Macintosh up to the 030
 * PowerBooks (the IIfx and Quadras excepted).
 *
 * Written from the NCR 5380/53C80 SCSI Interface Chip Design Manual (NCR,
 * SP-1051, March 1986; "the manual"): 3 and 4 (signals), 6 (the eight read
 * and eight write registers, bit by bit), 7 (on-chip hardware support:
 * arbitration, bus-free filter), 8 (interrupts 8.1-8.6), 9 (reset
 * conditions), 10 (programmed I/O, normal DMA, block mode DMA, pseudo DMA,
 * halting a DMA operation) and the 53C80 appendix (Last Byte Sent, bit 7 of
 * the Target Command Register). The Macintosh side (pseudo-DMA, blind
 * transfers, the glue's handshake, IRQ and DRQ wiring) from the Guide to the
 * Macintosh Family Hardware, 2nd ed. (1990), chapter 11 "SCSI Ports", "SCSI
 * data transfers" and "Handshaking for SCSI data transfers", and the
 * Macintosh IIsi Developer Note (address map).
 *
 * The chip is an initiator on an mc_scsi_bus (scsi_bus.h). The bus is
 * byte-level, so the REQ/ACK handshake is modelled per byte: ACK asserted
 * (ICR bit 4, or the DMA logic) takes the byte on the bus, REQ is false
 * until ACK is released, and a MSG IN byte keeps the target waiting while
 * ACK stays asserted (the target then goes to MSG OUT when ATN is asserted,
 * or BUS FREE after COMMAND COMPLETE). Arbitration needs a bus free of every
 * target (the bus-free filter and the 2.2 us arbitration delay take no
 * time here: the manual leaves the delay to software). Selection happens
 * when the chip, owner of the bus after arbitration, asserts SEL with the
 * target's ID bit on the data bus and releases BSY (the target answering
 * asserts BSY). Nothing on Livadix's bus arbitrates, selects or resets the chip
 * by itself: the mc_ncr5380_ext_* functions play the other bus devices for
 * tests and for the machines' own needs (a lost arbitration, another device
 * selecting or reselecting the chip, RST from the bus).
 *
 * Target role: as far as the manual defines it for the registers: the phase
 * lines and REQ come from the Target Command Register, ATN and ACK only
 * from the outside, Start DMA Target Receive needs TARGETMODE. Livadix's bus has
 * no initiator, so a target-mode DMA transfer moves its bytes through the two
 * hooks tgt_send / tgt_recv (NULL: nobody acknowledges, DRQ stays false).
 *
 * Times: everything the chip does completes at once (the chip is clockless
 * and the bus has no time); there are no scheduled events.
 *
 * Registers: offsets 0-7 are the register numbers A2-A0 (the machine maps its
 * stride and read/write addressing, see below); the read and write meanings
 * are those of the manual's register summary.
 *
 * Output lines (mc_hw_host line, 1 = asserted):
 *   MC_NCR5380_LINE_IRQ    IRQ (pin 23)
 *   MC_NCR5380_LINE_DRQ    DRQ (pin 22)
 *   MC_NCR5380_LINE_READY  READY (pin 25), block mode only (Mode bit 7):
 *                          follows the chip's readiness for the next byte
 *
 * Pseudo-DMA (manual 10.4; Guide, "SCSI data transfers"): the driver sets
 * the Mode register's DMA MODE bit, writes one of the Start DMA registers and
 * then moves the data with CPU accesses to an address where the machine's
 * glue turns each access into a DACK cycle (IOR or IOW with DACK instead of
 * CS: the Input / Output Data Register). The machine maps that window to
 * mc_ncr5380_dma_read / mc_ncr5380_dma_write. The glue holds the CPU's
 * cycle until DRQ (the "hardware handshaking" of all models after the Plus)
 * and ends it with a bus error when DRQ does not come in time; the Plus has
 * no handshake. The "blind transfer" is the driver reading or writing a whole
 * block through that window after checking DRQ only for the first byte.
 * Block mode keeps DACK asserted over the block; it changes nothing in this
 * byte-level model but the READY line.
 *
 * Wiring per machine (for the 5.2 families; † = unconfirmed in the documents):
 *   - Macintosh SE: $580000 selects a SCSI read and $580001 a SCSI write
 *     (Guide, "Memory-mapped device selection"); the Plus has no DRQ
 *     handshake and neither IRQ nor DRQ reaches the CPU: the driver polls the
 *     Bus and Status register.
 *   - Models with two VIAs (Mac II family, SE/30, IIx, IIcx; 5.2i,
 *     machines/macii.c): IRQ goes to VIA2 and so does DRQ, which also goes
 *     to the glue for the handshake (Guide, "Handshaking for SCSI data
 *     transfers"); IRQ on CB2 and DRQ on CA2 (Guide, Table 4-20), both
 *     inverted for the PCR of 0 (negative edges) the ROMs leave there†.
 *     Registers at $5001 0000 + n x $10, read and write at the same
 *     address (the ROMs use the image at $50F1 0000); pseudo-DMA with DRQ
 *     at $5000 6000 (SCSIHsk), without at $5001 2000 (SCSIDMA), long-word
 *     blind transfers as four DACK cycles (ROM 9779D2C4 at $40826B9C).
 *   - IIci (confirmed, 5.2d: Guide, Figure 3-8 and chapter 11; the IIci ROM
 *     368CADFE's decoder table at ROM offset $348C and its accesses): IRQ
 *     and DRQ in the RBV's Interrupt Flags register, bits 3 and 0; the MDU
 *     is the glue. Registers at $5001 0000 + n x $10 (read and write at the
 *     same address; the ROM uses the image at $50F1 0000), pseudo-DMA with
 *     DRQ at $5000 6000, without DRQ at $5001 2000 (machines/iici.c).
 *   - IIsi Developer Note (address map): normal mode registers at
 *     $5001 0000-$5001 1FFF†, pseudo-DMA without DRQ check at
 *     $5001 2000†, pseudo-DMA with DRQ at $5000 6000-$5000 7FFF†; the Combo
 *     chip puts SCSI and SCC together and is software-compatible with the
 *     53C80.
 *   - IIfx: the SCSI DMA IC (Guide, "SCSI DMA in the Macintosh IIfx
 *     computer") puts its DMA handshake on top of this chip in
 *     machines/iifx.c.
 */
#ifndef MC_NCR5380_H
#define MC_NCR5380_H

#include "hw.h"
#include "scsi_bus.h"

#include <stdint.h>

enum { MC_NCR5380_LINE_IRQ = 0, MC_NCR5380_LINE_DRQ = 1, MC_NCR5380_LINE_READY = 2 };

/* Register numbers (manual 6.0, register summary): read / write. */
enum {
    MC_N80_CSD = 0,         /* read: Current SCSI Data; write: Output Data */
    MC_N80_ICR = 1,         /* Initiator Command */
    MC_N80_MODE = 2,
    MC_N80_TCR = 3,         /* Target Command */
    MC_N80_CSBS = 4,        /* read: Current SCSI Bus Status; write: Select Enable */
    MC_N80_BSR = 5,         /* read: Bus and Status; write: Start DMA Send */
    MC_N80_IDR = 6,         /* read: Input Data; write: Start DMA Target Receive */
    MC_N80_RESET = 7        /* read: Reset Parity/Interrupt; write: Start DMA Initiator Receive */
};

/* Initiator Command Register (6.2). */
enum {
    MC_N80_ICR_DATA = 0x01, MC_N80_ICR_ATN = 0x02, MC_N80_ICR_SEL = 0x04, MC_N80_ICR_BSY = 0x08,
    MC_N80_ICR_ACK = 0x10, MC_N80_ICR_LA = 0x20 /* read */, MC_N80_ICR_AIP = 0x40 /* read */,
    MC_N80_ICR_TEST = 0x40 /* write */, MC_N80_ICR_RST = 0x80
};
/* Mode Register (6.3). */
enum {
    MC_N80_MODE_ARBITRATE = 0x01, MC_N80_MODE_DMA = 0x02, MC_N80_MODE_MONBSY = 0x04,
    MC_N80_MODE_EOPINT = 0x08, MC_N80_MODE_PARINT = 0x10, MC_N80_MODE_PARCHK = 0x20,
    MC_N80_MODE_TARGET = 0x40, MC_N80_MODE_BLOCK = 0x80
};
/* Target Command Register (6.4): bits 2-0 are the phase (I/O, C/D, MSG). */
enum { MC_N80_TCR_REQ = 0x08, MC_N80_TCR_LASTBYTE = 0x80 /* 53C80, read */ };
/* Current SCSI Bus Status Register (6.5). */
enum {
    MC_N80_CSBS_DBP = 0x01, MC_N80_CSBS_SEL = 0x02, MC_N80_CSBS_IO = 0x04, MC_N80_CSBS_CD = 0x08,
    MC_N80_CSBS_MSG = 0x10, MC_N80_CSBS_REQ = 0x20, MC_N80_CSBS_BSY = 0x40, MC_N80_CSBS_RST = 0x80
};
/* Bus and Status Register (6.7). */
enum {
    MC_N80_BSR_ACK = 0x01, MC_N80_BSR_ATN = 0x02, MC_N80_BSR_BUSYERR = 0x04, MC_N80_BSR_PHASE = 0x08,
    MC_N80_BSR_IRQ = 0x10, MC_N80_BSR_PARITY = 0x20, MC_N80_BSR_DRQ = 0x40, MC_N80_BSR_EOD = 0x80
};

typedef struct mc_ncr5380 {
    mc_dev dev;
    mc_scsi_bus *bus;
    int c80;                    /* a 53C80: Target Command bit 7 is Last Byte Sent */
    /* Registers. */
    uint8_t odr, icr, mode, tcr, ser, idr;
    int aip, la;                /* arbitration in progress, lost arbitration */
    int eod, parity_err, busy_err, irq;     /* Bus and Status latches */
    int drq;
    int last_byte;              /* 53C80: the last DMA byte has reached the bus */
    /* Internal state. */
    int dma_run;                /* 0 none, 1 send, 2 initiator receive, 3 target receive */
    int sent;                   /* send: a byte of this transfer reached the bus */
    int ack_out, ack_taken;     /* our ACK asserted; its byte was taken (REQ is false until ACK drops) */
    int req_prev;               /* REQ as seen at the last update (edge detection) */
    int my_id, sel_tried;
    int sel_prev, busy_prev;
    int eop_pin;
    int lines[3];               /* last reported IRQ, DRQ, READY (-1: never) */
    /* The other devices of the bus (mc_ncr5380_ext_*). */
    int ext_sel, ext_io, ext_bsy, ext_atn, ext_ack;
    uint8_t ext_data;
    /* Parity: 1 makes every byte the chip receives from the bus arrive with
     * bad parity (a test and fault-injection input). */
    int bad_parity;
    /* Target-role DMA: the bytes the missing initiator would take / give.
     * tgt_recv returns 1 and stores the byte when one is available. */
    void *tgt_ctx;
    void (*tgt_send)(void *ctx, uint8_t byte);
    int (*tgt_recv)(void *ctx, uint8_t *byte);
} mc_ncr5380;

extern const mc_dev_ops mc_ncr5380_ops;

/* A chip on `bus` (c80: a 53C80), hardware reset. */
void mc_ncr5380_init(mc_ncr5380 *c, const mc_hw_host *host, mc_scsi_bus *bus, int c80);
/* RESET pin (manual 9.1): clears every register, no SCSI bus reset. */
void mc_ncr5380_reset(mc_ncr5380 *c);

/* One DACK cycle moves one byte (the Input or Output Data Register through
 * IOR/IOW with DACK). A pseudo-DMA access of `size` bytes (1, 2 or 4,
 * big-endian) is that many cycles. Returns the bytes moved: fewer than
 * `size` when DRQ was not asserted for the rest (no DMA running, count
 * ended by EOP, phase changed); the machine decides between waiting and a
 * bus error for a short count (the glue's time-out). */
unsigned mc_ncr5380_dma_read(mc_ncr5380 *c, unsigned size, uint32_t *value);
unsigned mc_ncr5380_dma_write(mc_ncr5380 *c, unsigned size, uint32_t value);
/* EOP pin (27): while asserted, the next DACK cycle ends the DMA transfer
 * (END OF DMA bit, interrupt when enabled, no more bytes requested; DMA MODE
 * stays set; manual 8.2, 10.5.1). */
void mc_ncr5380_set_eop(mc_ncr5380 *c, int state);
/* DRQ pin. */
static inline int mc_ncr5380_drq(const mc_ncr5380 *c) { return c->drq; }

/* The other bus devices. A device selecting (SEL asserted, BSY false) with
 * `data` on the data lines; `reselect` also asserts I/O (8.1). */
void mc_ncr5380_ext_select(mc_ncr5380 *c, uint8_t data, int reselect);
/* The device lets go of SEL and the data lines. */
void mc_ncr5380_ext_release(mc_ncr5380 *c);
/* Another device asserts BSY (the bus is not free). */
void mc_ncr5380_ext_bsy(mc_ncr5380 *c, int state);
/* ATN and ACK from an initiator (target role). */
void mc_ncr5380_ext_signals(mc_ncr5380 *c, int atn, int ack);
/* A device asserts RST (8.3, 9.2): the targets reset, the chip resets
 * except the interrupt latch and ASSERT RST, and interrupts. */
void mc_ncr5380_ext_reset(mc_ncr5380 *c);

#endif
