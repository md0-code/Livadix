/* Livadix — Apple's I/O Processor (IOP, formerly PIC): a 65C02 with 32 KiB
 * of RAM shared with the host, a timer, a port, two DMA channels and one
 * chip it fronts, the SCC (the SCC IOP) or the SWIM (the SWIM IOP), on the
 * Macintosh IIfx and the Quadra 900/950.
 *
 * Sources: the IOP SWIM Driver ERS (Apple, Rev 1.0 A1, January 1990: the
 * two IOPs, the shared memory interface, the SWIM IOP's message kinds and
 * layouts, ISM mode only with the drive's HeadSelect line driven from the
 * ISM register set) and observation of the IIfx and Quadra 950 ROMs (the
 * IOP Manager's download, message and interrupt code, the 'iopc' kernels
 * it downloads). Unconfirmed details carry †.
 *
 * Host interface (offsets from the IOP's base; byte registers, every wider
 * access made as successive byte cycles):
 *   +$00/$01  address register, high byte; +$02/$03 low byte. The IOP
 *             Manager writes both as one word at +$01 and reads it back.
 *   +$04-$07  control/status: bit 0 bypass (reads the IOP's $F030 bit 0†),
 *             bit 1 auto-increment of the address after each data access,
 *             bit 2 run (0 holds the 65C02 in reset; 0 -> 1 starts it from
 *             its RESET vector), bit 3 written 1 interrupts the IOP (its
 *             $F034 bit 4; reads 0†), bits 4 and 5 the IOP's interrupts to
 *             the host (INT0: a host-to-IOP message completed, INT1: an
 *             IOP-to-host message posted; written 1 clears), bit 6 the
 *             fronted chip's interrupt while in bypass, bit 7 reads 0†.
 *   +$08-$0B  data: the shared RAM byte at the address (A14-A0).
 *   +$0C-$0F  reads 0, writes ignored†; +$10-$1F mirror +$00-$0F†.
 *   +$20...   the fronted chip in bypass mode, at the offsets the Mac ROM
 *             uses for that chip (SCC: bCtl +0, aCtl +2, bData +4, aData
 *             +6; SWIM: register n at +n*$200); outside bypass reads 0 and
 *             writes are ignored†.
 *
 * The IOP's own map (from the kernels' accesses):
 *   $0000-$7FFF  RAM; the vectors at $7FFA-$7FFF. $8000-$FFFF repeat it†,
 *                apart from the I/O page:
 *   $F010-$F013  timer: counter low/latch low, counter high/latch high and
 *                load, latch low, latch high; a 16-bit down-counter that
 *                reloads from the latch, flag $F034 bit 5 on each underflow
 *                (cleared by loading it)†; one count per 65C02 cycle†.
 *   $F020-$F024  DMA channel 1, $F028-$F02C channel 2: control (bit 0
 *                enable, bit 2 chip to RAM; bit 7 kept: the SWIM kernel
 *                sets it reading†), address low/high, count low/high. An
 *                enabled channel moves a byte each time the chip asks (the
 *                SWIM's ISM handshake bit 7, through its data registers
 *                0/8; the SCC's /W//REQ, channel 1 SCC A†, 2 SCC B†),
 *                counting the address up and the count down; at a count of
 *                0 it stops and sets $F034 bit 1 (channel 1) or 2.
 *   $F030        bit 0 bypass: the host reaches the chip directly and the
 *                chip's interrupt goes to the host; other bits kept† (the
 *                SCC kernel sets bits 3-4 and 5-6 per channel).
 *   $F031        configuration ($23 SCC kernel, $32 SWIM kernel): kept†.
 *   $F032        port: bits 0-1 outputs (bit 1 pulls the ADB line low on
 *                the SWIM IOP), bit 2 input (the ADB line's level, 1 high:
 *                the Quadra 950 ROM's ADB Manager talks to its devices so),
 *                through `port`.
 *   $F033        interrupt enables; $F034 interrupt flags (written 1 clears
 *                a latched one; reads the enabled ones†): bit 3 the chip
 *                (level, outside bypass), bit 4 the host (control bit 3),
 *                bit 5 the timer, bits 1 and 2 the DMA channels; bit 0
 *                never set†.
 *   $F035        interrupts to the host: bit 2 sets INT0, bit 3 INT1 (0
 *                leaves them); reads them back.
 *   $F040-$F04F  the chip: SCC $F040 bCtl, $F041 aCtl, $F042 bData†,
 *                $F043 aData†; SWIM registers 0-15 (the ISM set). On the
 *                SWIM IOP the drive's SEL follows ISM mode bit 5 (HDSEL).
 *   Other I/O addresses keep what is written†.
 *
 * Messages (shared RAM, IOP Manager as observed; the SWIM ERS's message
 * numbers): page $02 carries host-to-IOP messages, page $03 IOP-to-host
 * ones. $0200/$0300 hold the highest message number (7 SCC, 3 SWIM, set by
 * the kernel), $0201-$0207/$0301-$0307 each message's state (0 idle, 1 new,
 * 2 being processed, 3 completed), message n's 32 bytes at $0200/$0300 +
 * n*$20. The host posts with state 1 and control bit 3; the kernel answers
 * in place, sets state 3 and INT0. The kernel posts with state 1 and INT1;
 * the host replies with state 3 and control bit 3. $031F is the kernel's
 * "alive" byte (the host clears it before starting the 65C02 and waits for
 * $FF); $021F is the patch semaphore.
 *
 * Machine wiring (for 5.2j; base addresses from each ROM's base-address
 * table, the IOP entries naming the chip window at +$20):
 *   - IIfx: the SCC IOP at $50F04000, the SWIM IOP at $50F12000; their
 *     interrupts through the OSS† (the SCC IOP at level 4).
 *   - Quadra 900/950: the SCC IOP at $50F0C000, the SWIM IOP at
 *     $50F1E000; their interrupts through the VIAs†.
 *   - mc_iop_init, then the chip (mc_scc_init / mc_swim_init) on
 *     mc_iop_chip_host(iop); register the IOP (not the chip) with the
 *     scheduler and with mc_iop_ops over $2000 bytes as an 8-bit port
 *     (MC_WIN_PORT8); wire MC_IOP_LINE_IRQ to the interrupt input and the
 *     SWIM IOP's `port` to the ADB wire (mc_adb_wire_host from bit 1, bit
 *     2 from mc_adb_wire_level; done in machines/quadra900.c, with the
 *     SWIM IOP's interrupt on VIA2 CA2 and the SCC IOP's at level 4); the SCC's serial ports and the SWIM's
 *     media as on the other machines.
 *
 * Clock: the 65C02 runs at C15M/8 (1.9584 MHz)†, the rate at which the
 * SWIM kernel's ADB delay loops give an 800 us attention and a 3 ms reset.
 * The IOP keeps its own time and runs up to the guest time on every host
 * access and every scheduled slice (MC_IOP_SLICE). */
#ifndef MC_IOP_H
#define MC_IOP_H

#include "hw.h"
#include "cpu6502.h"

#include <stdint.h>

#define MC_IOP_RAM_SIZE 0x8000u
#define MC_IOP_CYCLE 8u                 /* C15M periods per 65C02 cycle† */
#define MC_IOP_SLICE 1024u              /* C15M periods run per scheduled event */

/* Host interface offsets. */
enum {
    MC_IOP_ADDR_HI = 0x01, MC_IOP_ADDR_LO = 0x02, MC_IOP_CTL = 0x04, MC_IOP_DATA = 0x08,
    MC_IOP_CHIP = 0x20
};

/* Control/status bits. */
enum {
    MC_IOP_BYPASS = 0x01, MC_IOP_INC = 0x02, MC_IOP_RUN = 0x04, MC_IOP_GEN_INT = 0x08,
    MC_IOP_INT0 = 0x10, MC_IOP_INT1 = 0x20, MC_IOP_CHIP_INT = 0x40
};

/* Output lines (mc_hw_host.line). */
enum { MC_IOP_LINE_IRQ = 0 };

typedef enum { MC_IOP_SCC = 0, MC_IOP_SWIM = 1 } mc_iop_kind;

/* What the board connects to the port at $F032. Both optional. */
typedef struct mc_iop_port {
    void *ctx;
    /* Output bits 0-1 changed (`value` = the port's output byte) at guest time t. */
    void (*output)(void *ctx, uint8_t value, uint64_t t);
    /* The input bits (bit 2) at guest time t. */
    uint8_t (*input)(void *ctx, uint64_t t);
} mc_iop_port;

typedef struct mc_iop {
    mc_dev dev;
    mc_cpu6502 cpu;
    uint8_t kind;                   /* mc_iop_kind */
    uint8_t ram[MC_IOP_RAM_SIZE];
    /* Host side. */
    uint16_t addr;
    uint8_t ctl;                    /* bits 1 and 2 as written */
    uint8_t int0, int1;             /* interrupts to the host */
    int irq_out;
    /* IOP side. */
    uint8_t io[0x100];              /* $F000-$F0FF as written */
    uint8_t ie, flags;              /* $F033; the latched flags of $F034 (bits 4, 5) */
    uint8_t chip_irq;               /* the fronted chip's interrupt output */
    uint8_t wreq[2];                /* the SCC's /W//REQ A and B (DMA requests) */
    uint16_t timer, latch;          /* counter as of timer_at, latch */
    uint64_t timer_at;              /* IOP time the counter is valid at */
    uint64_t timer_next;            /* IOP time of the next underflow */
    uint64_t time;                  /* the IOP's own time (C15M) */
    uint64_t steps;                 /* 65C02 instructions executed */
    void *chip;
    const mc_dev_ops *chip_ops;
    mc_hw_host chip_host;           /* the chip's host (mc_iop_chip_host) */
    uint64_t chip_deadline;         /* the chip's armed deadline */
    uint64_t chip_time;             /* the last time the chip was given */
    uint8_t in_run;
    mc_iop_port port;
} mc_iop;

extern const mc_dev_ops mc_iop_ops;

/* An IOP on `host` after reset (the 65C02 held, RAM cleared), fronting
 * `chip` (an mc_scc or an mc_swim, by `kind`) reached through `chip_ops`.
 * The chip is not touched: initialise it afterwards on mc_iop_chip_host. */
void mc_iop_init(mc_iop *iop, const mc_hw_host *host, mc_iop_kind kind, void *chip, const mc_dev_ops *chip_ops);
/* The host the fronted chip is initialised on: the chip runs on the IOP's
 * clock (its time is the 65C02's while the IOP runs, and never goes back;
 * the IOP delivers its deadline), its interrupt goes to the IOP (the SCC's
 * MC_SCC_LINE_IRQ) and so do its /W//REQ lines (the DMA requests); its
 * other lines are not wired. The chip is not registered with the
 * machine's scheduler. */
const mc_hw_host *mc_iop_chip_host(mc_iop *iop);
/* The board's port connections. */
void mc_iop_set_port(mc_iop *iop, const mc_iop_port *port);
/* The fronted chip's interrupt output (1 = asserted). */
void mc_iop_set_chip_irq(mc_iop *iop, int asserted);
/* Runs the 65C02 up to guest time `to` (C15M). */
void mc_iop_run(mc_iop *iop, uint64_t to);

#endif
