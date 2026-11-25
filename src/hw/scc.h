/* Livadix — Zilog Z85C30 Serial Communications Controller (SCC).
 *
 * Written from the Zilog SCC/ESCC User's Manual (1992; "SCC UM" below):
 * 2.3.4 Z85X30 register access (one register pointer for both channels, data
 * registers addressed directly), 2.3.7 and Table 2-7 reset values, 2.4.3-2.4.9
 * interrupts (IE/IP/IUS, the internal daisy chain, Table 2-8 priorities,
 * Table 2-9 vector modification, software acknowledge), 2.6 local loopback
 * and auto echo, 3.2 the baud-rate generator, 4.2 asynchronous mode and 5
 * (every write and read register). Macintosh wiring from the Guide to the
 * Macintosh Family Hardware, 2nd ed., chapter 10 ("Serial I/O Ports": DCD on
 * GPi, CTS and TRxC on HSKi, DTR on HSKo, the 3.672 MHz clock on PCLK and
 * RTxC) and Inside Macintosh III ("The Macintosh Hardware": PCLK and RTxC at
 * 3.672 MHz; register offsets aData 6, aCtl 2, bData 4, bCtl 0).
 *
 * Model: both channels, WR0-WR15 and RR0-RR15 through the shared pointer,
 * asynchronous mode with exact character timing (start, data, parity and
 * stop bits at the rate the clock mode, WR11 clock sources, WR14 and the
 * WR12/WR13 time constant give) scheduled in guest time, the 3-byte receive
 * FIFO with overrun, the transmit buffer and shift register, all six
 * interrupt sources with the daisy chain, external/status latching, local
 * loopback and auto echo. Synchronous and SDLC modes accept their registers
 * and consume transmitted bytes at the bit rate without sending frames; they
 * never receive. No DPLL decoding, no CRC.
 *
 * Register offsets (read/write `offset`): bit 1 selects channel A (1) or B
 * (0), bit 2 the data register (1) or the control register (0); bit 0 and
 * the bits above 2 are ignored. That is Apple's layout (Inside Macintosh III:
 * bCtl 0, aCtl 2, bData 4, aData 6), so the machine passes its offset from
 * the SCC base as is. On the Quadra 700 the SCC is at $5000C000 (Quadra 700
 * Developer Note, Figure 1-2); the ROM makes byte accesses at those offsets
 * from $5000C020.
 */
#ifndef MC_SCC_H
#define MC_SCC_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

/* Output lines (mc_hw_host.line). */
enum {
    MC_SCC_LINE_IRQ = 0,    /* /INT asserted: IPL 4 on the Quadra 700 (Developer Note, Table 2-7), level 2 on the Mac II family (Guide, ch. 10) */
    MC_SCC_LINE_WREQ = 1,   /* /W//REQA or /W//REQB asserted (request mode): VIA1 PA7 vSCCWrReq on Macs that wire it (Guide, ch. 4, Table 4-2) */
    MC_SCC_LINE_WREQ_A = 2, /* channel A's /W//REQ alone */
    MC_SCC_LINE_WREQ_B = 3, /* channel B's /W//REQ alone */
    MC_SCC_LINE_TX_READY_A = 4, MC_SCC_LINE_TX_READY_B = 5,
    MC_SCC_LINE_RX_READY_A = 6, MC_SCC_LINE_RX_READY_B = 7
};

enum { MC_SCC_A = 0, MC_SCC_B = 1 };

/* Modem outputs reported to the port host (mc_scc_port.outputs). */
enum {
    MC_SCC_OUT_DTR = 1,     /* /DTR asserted (WR5 D7): HSKo on the Macintosh */
    MC_SCC_OUT_RTS = 2,     /* /RTS asserted (WR5 D1): enables the TxD line driver on the Macintosh */
    MC_SCC_OUT_BREAK = 4    /* Send Break (WR5 D4): TxD held spacing */
};

/* What the shell connects to a port (`ports.a` = channel A, the modem port;
 * `ports.b` = channel B, the printer port). All optional. */
typedef struct mc_scc_port {
    void *ctx;
    /* A character finished leaving TxD (its last stop bit), masked to the
       programmed bits per character. Asynchronous mode only. */
    void (*transmit)(void *ctx, int channel, uint8_t byte);
    /* The modem outputs of a channel changed (MC_SCC_OUT_*). */
    void (*outputs)(void *ctx, int channel, unsigned outputs);
} mc_scc_port;

#define MC_SCC_LINE_QUEUE 1024u     /* bytes waiting on RxD (host side) */

typedef struct mc_scc_chan {
    uint8_t wr[16];             /* WR2 and WR9 live in channel A's copy */
    uint8_t rr0;                /* live RR0 status bits (D3 DCD, D5 CTS, D6 underrun/EOM, D7 break) */
    uint8_t rr0_latched, latch; /* RR0 frozen by an external/status interrupt */
    uint8_t rr1;                /* latched special conditions (D4 parity, D5 overrun) and all sent */
    /* Receive FIFO: data and per-character status (RR1 D4-D6 bits). */
    uint8_t fifo[3], fifo_st[3], fifo_n;
    uint8_t rx_first;           /* Rx interrupt on first character: armed */
    uint8_t ip;                 /* 1 Rx, 2 Tx, 4 ext/status (SCC UM 2.4.4.3) */
    uint8_t ius;                /* same bits: under service */
    uint8_t rx_special;         /* the Rx IP is a special condition */
    /* Transmitter. */
    uint8_t tx_buf, tx_full, tx_shift, tx_busy, tx_ip_armed;
    uint64_t tx_done;           /* when the character in the shift register leaves */
    /* Receiver: bytes on the line and the one being shifted in. */
    uint8_t line[MC_SCC_LINE_QUEUE];
    unsigned line_head, line_n;
    uint8_t rx_busy, rx_byte, rx_break;
    uint64_t rx_done;
    /* Inputs and clocks. */
    uint8_t dcd, cts, break_in, sync;
    uint32_t trxc_hz;           /* external clock on TRxC (HSKi); 0 = none */
    uint32_t rtxc_hz;
    uint8_t dpll_brg;           /* DPLL source: 1 BRG, 0 RTxC (WR14 commands) */
    uint64_t zc_next;           /* next BRG zero count (MC_NEVER when not wanted) */
    unsigned outputs;           /* last reported MC_SCC_OUT_* */
    uint64_t rem;               /* remainder of clock-to-C15M conversions */
} mc_scc_chan;

typedef struct mc_scc {
    mc_dev dev;
    mc_scc_chan ch[2];          /* [MC_SCC_A], [MC_SCC_B] */
    uint8_t ptr;                /* register pointer, shared by both channels (SCC UM 2.3) */
    uint32_t pclk_hz;
    int irq, wreq[2];
    uint8_t data_ready[2];      /* Tx buffer empty and Rx data available, SCC UM 5.3.1 */
    uint64_t armed;             /* deadline currently armed */
    mc_scc_port port;
} mc_scc;

/* The Macintosh's SCC clock on PCLK and RTxC: 3.672 MHz = C15M x 15/64
 * (Inside Macintosh III, "The Macintosh Hardware"; Guide, ch. 10). The
 * Quadra 700 keeps it on RTxC and runs PCLK faster (quadra700.c). */
#define MC_SCC_MAC_CLOCK_HZ 3672000u

/* A chip on `host`, after a hardware reset; clocks MC_SCC_MAC_CLOCK_HZ,
 * DCD and CTS asserted, no port connected. */
void mc_scc_init(mc_scc *scc, const mc_hw_host *host);
extern const mc_dev_ops mc_scc_ops;

/* Clocks: PCLK, and the RTxC pins of channel A and B (Hz). */
void mc_scc_set_clocks(mc_scc *scc, uint32_t pclk_hz, uint32_t rtxc_a_hz, uint32_t rtxc_b_hz);
/* An external clock on a channel's TRxC pin (HSKi on the Macintosh; MIDI
 * interfaces supply 0.5 or 1 MHz there); 0 = none. */
void mc_scc_set_trxc(mc_scc *scc, int channel, uint32_t hz);

/* Host side of a port. */
void mc_scc_set_port(mc_scc *scc, const mc_scc_port *port);
/* Bytes arriving on a channel's RxD, in order; each takes one character
 * time on the line before it reaches the receive FIFO. Returns how many
 * were queued (the rest did not fit: try again later). */
size_t mc_scc_receive(mc_scc *scc, int channel, const uint8_t *data, size_t count);
/* A DMA data-register cycle. Returns 1 when a byte transfers and 0
 * when the FIFO cannot accept/provide one. The shared register pointer
 * is unchanged, as for the direct data ports (SCC UM 2.3.4).
 * The PCI I/O controller can use TX_READY/RX_READY independently of
 * the external W/REQ pin's programmed function. */
int mc_scc_dma_read(mc_scc *scc, int channel, uint8_t *byte);
int mc_scc_dma_write(mc_scc *scc, int channel, uint8_t byte);
/* Room left on the line queue. */
size_t mc_scc_receive_room(const mc_scc *scc, int channel);
/* Modem inputs: /DCD (GPi on the Macintosh) and /CTS (HSKi) asserted. */
void mc_scc_set_dcd(mc_scc *scc, int channel, int asserted);
void mc_scc_set_cts(mc_scc *scc, int channel, int asserted);
/* A break on RxD begins (1) or ends (0): RR0 D7, an external/status
 * interrupt, and a null character in the FIFO when it ends (SCC UM 4.2.2). */
void mc_scc_set_break(mc_scc *scc, int channel, int on);
/* Current modem outputs of a channel (MC_SCC_OUT_*). */
unsigned mc_scc_outputs(const mc_scc *scc, int channel);
/* Duration of whole transmit bits at the programmed clock, rounded up in
 * C15M periods. MC_NEVER when that direction has no input clock. */
uint64_t mc_scc_tx_time(const mc_scc *scc, int channel, uint16_t bits);

/* A hardware interrupt acknowledge cycle (/INTACK): sets the IUS of the
 * highest pending source and returns the vector (WR2, modified when WR9 VIS
 * is set), or -1 when WR9 NV is set or nothing is pending. The Macintosh
 * does not wire /INTACK (autovectors); software reads RR2 of channel B. */
int mc_scc_iack(mc_scc *scc);

#endif
