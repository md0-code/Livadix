/* Livadix — Zilog Z85C30 SCC (scc.h). Section numbers are those of the Zilog
 * SCC/ESCC User's Manual (1992). */
#include "scc.h"

#include <string.h>

/* WR0 command codes, D5-D3 (5.2.1). */
enum {
    CMD_NULL = 0, CMD_POINT_HIGH = 1, CMD_RESET_EXT = 2, CMD_SEND_ABORT = 3,
    CMD_INT_NEXT_RX = 4, CMD_RESET_TX_IP = 5, CMD_ERROR_RESET = 6, CMD_RESET_IUS = 7
};
/* Interrupt pending/under service bits per channel (2.4.4). */
enum { IP_RX = 1, IP_TX = 2, IP_EXT = 4 };
/* RR0 bits (5.3.1). */
enum {
    RR0_RX_AVAIL = 0x01, RR0_ZERO_COUNT = 0x02, RR0_TX_EMPTY = 0x04, RR0_DCD = 0x08,
    RR0_SYNC = 0x10, RR0_CTS = 0x20, RR0_TX_UNDERRUN = 0x40, RR0_BREAK = 0x80
};
/* RR1 bits (5.3.2). */
enum { RR1_ALL_SENT = 0x01, RR1_RESIDUE = 0x06, RR1_PARITY = 0x10, RR1_OVERRUN = 0x20, RR1_FRAMING = 0x40 };

static void update(mc_scc *s);

/* ---- clocks and character timing ------------------------------------------------ */

static int async_mode(const mc_scc_chan *c) { return (c->wr[4] & 0x0C) != 0; }   /* WR4 D3-D2: stop bits; 00 = synchronous (5.2.5) */
static unsigned clock_factor(const mc_scc_chan *c) {
    static const unsigned f[4] = {1, 16, 32, 64};   /* WR4 D7-D6 (5.2.5) */
    return f[c->wr[4] >> 6];
}
/* Data bits per character: WR3 D7-D6 receive (Table 5-4), WR5 D6-D5 transmit
 * (Table 5-5): 00 five (or fewer), 01 seven, 10 six, 11 eight. */
static unsigned rx_bits(const mc_scc_chan *c) { static const unsigned b[4] = {5, 7, 6, 8}; return b[c->wr[3] >> 6]; }
static unsigned tx_bits(const mc_scc_chan *c) { static const unsigned b[4] = {5, 7, 6, 8}; return b[(c->wr[5] >> 5) & 3]; }
/* Half-bit periods of a character on the line: start, data, parity, stop
 * (WR4 D3-D2: 01 one, 10 one and a half, 11 two) in asynchronous mode
 * (4.2); the data bits alone in the synchronous modes. */
static unsigned half_bits(const mc_scc_chan *c, unsigned data_bits) {
    if (!async_mode(c)) return 2 * data_bits;
    static const unsigned stop[4] = {0, 2, 3, 4};
    return 2 * (1 + data_bits + (c->wr[4] & 1)) + stop[(c->wr[4] >> 2) & 3];
}

/* The bit clock of one direction: `src` is the WR11 source field (5.2.13:
 * 00 RTxC pin, 01 TRxC pin, 10 BRG output, 11 DPLL output). Sets the clock
 * frequency in Hz and the input cycles per bit; returns 0 without a clock. */
static int bit_clock(const mc_scc *s, const mc_scc_chan *c, unsigned src, uint64_t *hz, uint64_t *cycles) {
    unsigned tc = c->wr[12] | (unsigned)c->wr[13] << 8;
    /* The BRG output toggles at each zero count of a counter reloaded with
     * the time constant, the load taking one more clock: one output period
     * is 2 x (TC + 2) input clocks (3.2; WR14 D1 selects PCLK or RTxC). */
    uint64_t brg_hz = (c->wr[14] & 2) ? s->pclk_hz : c->rtxc_hz;
    uint64_t brg_cycles = 2ull * (tc + 2);
    int brg_on = (c->wr[14] & 1) != 0;
    switch (src) {
    case 0: *hz = c->rtxc_hz; *cycles = clock_factor(c); break;
    case 1: *hz = c->trxc_hz; *cycles = clock_factor(c); break;
    case 2:
        if (!brg_on) return 0;
        *hz = brg_hz; *cycles = brg_cycles * clock_factor(c);
        break;
    default:
        /* The DPLL samples at 32 times the bit rate from the BRG or RTxC
         * (3.4); it is used with the x1 clock mode. */
        if (c->dpll_brg) {
            if (!brg_on) return 0;
            *hz = brg_hz; *cycles = brg_cycles * 32;
        } else {
            *hz = c->rtxc_hz; *cycles = 32;
        }
        break;
    }
    return *hz != 0;
}

/* Guest time (C15M periods) of `half` half-bits at the clock of `src`;
 * MC_NEVER without a clock. The fractional period is carried in `rem`. */
static uint64_t line_time(const mc_scc *s, const mc_scc_chan *c, unsigned src, unsigned half, uint64_t *rem) {
    uint64_t hz, cycles;
    if (!bit_clock(s, c, src, &hz, &cycles)) return MC_NEVER;
    uint64_t num = cycles * half * (uint64_t)MC_C15M_HZ + *rem;
    uint64_t den = 2 * hz;
    *rem = num % den;
    uint64_t t = num / den;
    return t ? t : 1;
}
uint64_t mc_scc_tx_time(const mc_scc *s, int channel, uint16_t bits) {
    if (channel < 0 || channel >= 2) return MC_NEVER;
    const mc_scc_chan *c = &s->ch[channel];
    uint64_t hz, cycles;
    if (!bit_clock(s, c, (c->wr[11] >> 3) & 3u, &hz, &cycles)) return MC_NEVER;
    return (cycles * bits * MC_C15M_HZ + hz - 1u) / hz;
}

static uint64_t now(const mc_scc *s) { return mc_dev_now(&s->dev); }

/* ---- external/status (2.4.9) ------------------------------------------------------ */

static uint8_t live_rr0(const mc_scc_chan *c) {
    uint8_t v = c->rr0 & (RR0_TX_UNDERRUN);
    if (c->dcd) v |= RR0_DCD;
    if (c->cts) v |= RR0_CTS;
    /* Sync/Hunt: the /SYNC pin in asynchronous mode; in the synchronous
     * modes the receiver's Hunt state, which it never leaves here: Livadix
     * receives no flag or sync character (2.4.9.5, Figure 5-19 bit 4). */
    if (async_mode(c) ? c->sync : 1) v |= RR0_SYNC;
    if (c->break_in) v |= RR0_BREAK;
    return v;
}
/* A source of an external/status interrupt changed: when its WR15 enable is
 * set the latches close on the current status (all together), and with the
 * WR1 D0 master enable the IP is set. `bit` is the RR0 bit (RR0_ZERO_COUNT
 * for the BRG). Break closes the latches on both edges even when they are
 * already closed. */
static void ext_change(mc_scc *s, mc_scc_chan *c, uint8_t bit) {
    if (!(c->wr[15] & bit)) return;
    if (c->latch && bit != RR0_BREAK) return;
    c->latch = 1;
    c->rr0_latched = live_rr0(c);
    if (c->wr[1] & 1) c->ip |= IP_EXT;
    (void)s;
}
static void set_input(mc_scc *s, mc_scc_chan *c, uint8_t *pin, int value, uint8_t bit) {
    value = value != 0;
    if (*pin == value) return;
    *pin = (uint8_t)value;
    ext_change(s, c, bit);
}

/* ---- receive FIFO (2.4.7, 4.2.2) ---------------------------------------------------- */

static int exit_special(const mc_scc_chan *c) {
    if (!c->fifo_n) return 0;
    uint8_t st = c->fifo_st[0];
    if (!(c->wr[1] & 4)) st &= (uint8_t)~RR1_PARITY;   /* parity is special only when WR1 D2 */
    return (st & (RR1_OVERRUN | RR1_FRAMING | RR1_PARITY)) != 0;
}
/* A character assembled by the receiver enters the FIFO. With the FIFO full
 * the newest character overwrites the last entry and is flagged overrun
 * (5.3.2, RR1 D5). */
static void rx_push(mc_scc_chan *c, uint8_t byte, uint8_t status) {
    if (c->fifo_n < 3) {
        c->fifo[c->fifo_n] = byte;
        c->fifo_st[c->fifo_n++] = status;
    } else {
        c->fifo[2] = byte;
        c->fifo_st[2] = status | RR1_OVERRUN;
    }
}
/* A received character as the FIFO holds it: right-justified with the unused
 * high bits set to 1; with parity and fewer than eight bits the parity bit is
 * assembled above the data (4.2.2). The line carries characters with correct
 * parity. */
static uint8_t rx_format(const mc_scc_chan *c, uint8_t data) {
    unsigned n = rx_bits(c);
    if (n >= 8) return data;
    unsigned v = data & ((1u << n) - 1);
    unsigned top = n;
    if (c->wr[4] & 1) {
        unsigned ones = 0;
        for (unsigned i = 0; i < n; i++) ones += (v >> i) & 1;
        unsigned parity = (c->wr[4] & 2) ? (ones & 1) : !(ones & 1);   /* WR4 D1: even (1) or odd */
        v |= parity << n;
        top++;
    }
    if (top < 8) v |= 0xFFu << top;
    return (uint8_t)v;
}
/* Whether the receiver takes characters: WR3 D0 Rx enable, and with Auto
 * Enables (WR3 D5) /DCD asserted, except in local loopback (2.6.1). */
static int rx_enabled(const mc_scc_chan *c) {
    if (!(c->wr[3] & 1) || !async_mode(c)) return 0;
    if ((c->wr[3] & 0x20) && !c->dcd && !(c->wr[14] & 0x10)) return 0;
    return 1;
}
static unsigned rx_src(const mc_scc_chan *c) { return (c->wr[11] >> 5) & 3; }
static unsigned tx_src(const mc_scc_chan *c) { return (c->wr[11] >> 3) & 3; }

/* Starts shifting the next byte waiting on RxD. */
static void rx_start(mc_scc *s, mc_scc_chan *c) {
    if (c->rx_busy || !c->line_n) return;
    if (c->wr[14] & 0x10 && !(c->wr[14] & 0x08)) return;   /* local loopback: RxD is not the receiver's input */
    uint64_t t = line_time(s, c, rx_src(c), half_bits(c, rx_bits(c)), &c->rem);
    if (t == MC_NEVER) return;
    c->rx_byte = c->line[c->line_head];
    c->line_head = (c->line_head + 1) % MC_SCC_LINE_QUEUE;
    c->line_n--;
    c->rx_busy = 1;
    c->rx_done = now(s) + t;
}
static void deliver(mc_scc *s, int ch, uint8_t byte);
static void rx_finish(mc_scc *s, int ch) {
    mc_scc_chan *c = &s->ch[ch];
    c->rx_busy = 0;
    uint8_t byte = c->rx_byte;
    /* Auto echo: RxD is routed to TxD (2.6.2). */
    if (c->wr[14] & 0x08) deliver(s, ch, byte);
    if (rx_enabled(c)) rx_push(c, rx_format(c, byte), 0);
    rx_start(s, c);
}

/* ---- transmitter (2.4.8, 4.2.1) ------------------------------------------------------ */

static void deliver(mc_scc *s, int ch, uint8_t byte) {
    if (s->port.transmit) s->port.transmit(s->port.ctx, ch, byte);
}
/* Whether the transmitter may start a character: WR5 D3 Tx enable, and with
 * Auto Enables /CTS asserted, except in local loopback (2.6.1, 5.2.4). */
static int tx_enabled(const mc_scc_chan *c) {
    if (!(c->wr[5] & 8)) return 0;
    if ((c->wr[3] & 0x20) && !c->cts && !(c->wr[14] & 0x10)) return 0;
    return 1;
}
/* Moves the transmit buffer into the shift register when it is free. The
 * buffer going empty sets the Tx IP when WR1 D1 is set (2.4.8: on the
 * NMOS/CMOS SCC the IP is set when the buffer becomes empty). */
static void tx_load(mc_scc *s, mc_scc_chan *c) {
    if (c->tx_busy || !c->tx_full || !tx_enabled(c)) return;
    uint64_t t = line_time(s, c, tx_src(c), half_bits(c, tx_bits(c)), &c->rem);
    if (t == MC_NEVER) return;
    c->tx_shift = c->tx_buf;
    c->tx_full = 0;
    c->tx_busy = 1;
    c->tx_done = now(s) + t;
    if (c->wr[1] & 2) c->ip |= IP_TX;
}
static void tx_finish(mc_scc *s, int ch) {
    mc_scc_chan *c = &s->ch[ch];
    c->tx_busy = 0;
    if (async_mode(c)) {
        unsigned n = tx_bits(c);
        uint8_t byte = n >= 8 ? c->tx_shift : (uint8_t)(c->tx_shift & ((1u << n) - 1));
        /* Local loopback: the transmitter feeds the receiver and TxD stays
         * connected; auto echo disconnects the transmitter (2.6). */
        if (!(c->wr[14] & 0x08)) deliver(s, ch, byte);
        if ((c->wr[14] & 0x18) == 0x10 && rx_enabled(c)) rx_push(c, rx_format(c, c->tx_shift), 0);
    }
    tx_load(s, c);
    if (!c->tx_busy && !async_mode(c) && !(c->rr0 & RR0_TX_UNDERRUN)) {
        /* Synchronous modes: the transmitter underruns (2.4.9.2). */
        c->rr0 |= RR0_TX_UNDERRUN;
        ext_change(s, c, RR0_TX_UNDERRUN);
    }
}

/* ---- the BRG zero count (2.4.9) --------------------------------------------------------- */

static void zc_arm(mc_scc *s, mc_scc_chan *c) {
    c->zc_next = MC_NEVER;
    if (!(c->wr[14] & 1) || !(c->wr[15] & RR0_ZERO_COUNT) || c->latch) return;
    uint64_t hz = (c->wr[14] & 2) ? s->pclk_hz : c->rtxc_hz;
    if (!hz) return;
    uint64_t tc = c->wr[12] | (unsigned)c->wr[13] << 8;
    uint64_t t = ((tc + 2) * MC_C15M_HZ + hz - 1) / hz;
    c->zc_next = now(s) + (t ? t : 1);
}

/* ---- interrupts (2.4.4-2.4.6) ------------------------------------------------------------- */

static unsigned rx_ip(const mc_scc_chan *c) {
    switch ((c->wr[1] >> 3) & 3) {   /* WR1 D4-D3 (Figure 2-14) */
    case 1: return (c->fifo_n && c->rx_first) || exit_special(c);
    case 2: return c->fifo_n != 0;
    case 3: return exit_special(c);
    default: return 0;
    }
}
static unsigned pending(const mc_scc_chan *c) {
    unsigned ip = c->ip & (IP_TX | IP_EXT);
    if (rx_ip(c)) ip |= IP_RX;
    /* An IP whose enable was cleared no longer requests. */
    if (!(c->wr[1] & 2)) ip &= ~(unsigned)IP_TX;
    if (!(c->wr[1] & 1)) ip &= ~(unsigned)IP_EXT;
    return ip;
}
/* Sources in priority order (Table 2-8): A Rx, A Tx, A Ext, B Rx, B Tx,
 * B Ext. Returns the index (0-5) of the highest pending source not masked by
 * an IUS at or above it, or -1. */
static int highest(const mc_scc *s) {
    static const unsigned order[3] = {IP_RX, IP_TX, IP_EXT};
    for (int i = 0; i < 6; i++) {
        const mc_scc_chan *c = &s->ch[i / 3];
        unsigned bit = order[i % 3];
        if (c->ius & bit) return -1;
        if (pending(c) & bit) return i;
    }
    return -1;
}
/* The status code of Table 2-9 for a source index, or 3 (channel B special
 * receive condition) when nothing is pending (5.3.3). */
static unsigned status_code(const mc_scc *s, int src) {
    if (src < 0) return 3;
    const mc_scc_chan *c = &s->ch[src / 3];
    unsigned base = src < 3 ? 4 : 0;
    switch (src % 3) {
    case 0: return base | (exit_special(c) ? 3 : 2);
    case 1: return base | 0;
    default: return base | 1;
    }
}
/* WR2 with the status in V3-V1 (status low) or V4-V6 (status high: WR9 D4),
 * where the code's high bit goes to V3 or V4 (Table 2-9). */
static uint8_t modified_vector(const mc_scc *s, unsigned code) {
    uint8_t v = s->ch[MC_SCC_A].wr[2];
    if (s->ch[MC_SCC_A].wr[9] & 0x10) {
        unsigned hi = ((code & 4) ? 0x10 : 0) | ((code & 2) ? 0x20 : 0) | ((code & 1) ? 0x40 : 0);
        return (uint8_t)((v & 0x8F) | hi);
    }
    return (uint8_t)((v & 0xF1) | code << 1);
}
static void acknowledge(mc_scc *s, int src) {
    static const unsigned order[3] = {IP_RX, IP_TX, IP_EXT};
    if (src >= 0) s->ch[src / 3].ius |= (uint8_t)order[src % 3];
}

/* Recomputes the output lines and the deadline. */
static void update(mc_scc *s) {
    int irq = (s->ch[MC_SCC_A].wr[9] & 8) && highest(s) >= 0;   /* WR9 D3 MIE */
    if (irq != s->irq) { s->irq = irq; mc_dev_line(&s->dev, MC_SCC_LINE_IRQ, irq); }
    int before = s->wreq[0] | s->wreq[1];
    for (int i = 0; i < 2; i++) {
        mc_scc_chan *c = &s->ch[i];
        int w = 0;
        /* WR1 D7 enable, D6 request function, D5 on receive or transmit
           (5.2.2). The wait function only holds an access that would find
           no data: never here, accesses complete at once. */
        if ((c->wr[1] & 0xC0) == 0xC0)
            w = (c->wr[1] & 0x20) ? c->fifo_n != 0 : !c->tx_full;
        if (w != s->wreq[i]) { s->wreq[i] = w; mc_dev_line(&s->dev, i == MC_SCC_A ? MC_SCC_LINE_WREQ_A : MC_SCC_LINE_WREQ_B, w); }
    }
    if ((s->wreq[0] | s->wreq[1]) != before) mc_dev_line(&s->dev, MC_SCC_LINE_WREQ, s->wreq[0] | s->wreq[1]);
    /* The FIFO conditions also feed the integrated PCI I/O controller.
     * WR1 configures the external W/REQ pin separately (SCC UM 5.2.2,
     * 5.3.1). Update the saved state before notifying a DMA requester. */
    for (int ch = 0; ch < 2; ch++) {
        const mc_scc_chan *c = &s->ch[ch];
        const uint8_t ready = (uint8_t)((!c->tx_full ? 1u : 0u) | (c->fifo_n ? 2u : 0u));
        const uint8_t changed = ready ^ s->data_ready[ch];
        s->data_ready[ch] = ready;
        if (changed & 1u) mc_dev_line(&s->dev, ch == MC_SCC_A ? MC_SCC_LINE_TX_READY_A : MC_SCC_LINE_TX_READY_B, ready & 1u);
        if (changed & 2u) mc_dev_line(&s->dev, ch == MC_SCC_A ? MC_SCC_LINE_RX_READY_A : MC_SCC_LINE_RX_READY_B, (ready >> 1) & 1u);
    }
    /* Modem outputs. */
    for (int i = 0; i < 2; i++) {
        mc_scc_chan *c = &s->ch[i];
        unsigned out = ((c->wr[5] & 0x80) ? MC_SCC_OUT_DTR : 0) | ((c->wr[5] & 2) ? MC_SCC_OUT_RTS : 0) |
                       ((c->wr[5] & 0x10) ? MC_SCC_OUT_BREAK : 0);
        if (out != c->outputs) {
            c->outputs = out;
            if (s->port.outputs) s->port.outputs(s->port.ctx, i, out);
        }
    }
    /* The next deadline. */
    uint64_t next = MC_NEVER;
    for (int i = 0; i < 2; i++) {
        mc_scc_chan *c = &s->ch[i];
        if (c->tx_busy && c->tx_done < next) next = c->tx_done;
        if (c->rx_busy && c->rx_done < next) next = c->rx_done;
        if (c->zc_next < next) next = c->zc_next;
    }
    if (next != s->armed) { s->armed = next; mc_dev_schedule(&s->dev, next); }
}

/* ---- resets (2.3.7, Table 2-7) ---------------------------------------------------------- */

static void channel_reset(mc_scc *s, mc_scc_chan *c) {
    c->wr[0] = 0;
    c->wr[1] &= 0x24;
    c->wr[3] &= 0xFE;
    c->wr[4] |= 0x04;
    c->wr[5] &= 0x61;
    c->wr[10] &= 0x61;
    c->wr[14] = (uint8_t)((c->wr[14] & 0xC3) | 0x20);
    c->wr[15] = 0xF8;
    c->rr0 = RR0_TX_UNDERRUN;
    c->latch = 0;
    c->rr1 = RR1_ALL_SENT;
    c->fifo_n = 0;
    c->ip = 0;
    c->ius = 0;
    c->rx_first = 0;
    c->tx_full = 0;
    c->tx_busy = 0;
    c->rx_busy = 0;
    c->zc_next = MC_NEVER;
    s->ch[MC_SCC_A].wr[9] &= (uint8_t)~0x20;   /* WR9 D5 (Table 2-7) */
}
static void hardware_reset(mc_scc *s) {
    for (int i = 0; i < 2; i++) {
        mc_scc_chan *c = &s->ch[i];
        channel_reset(s, c);
        c->wr[10] = 0;
        c->wr[11] = 0x08;
        c->wr[14] = (uint8_t)((c->wr[14] & 0xC0) | 0x30);
    }
    s->ch[MC_SCC_A].wr[9] = (uint8_t)((s->ch[MC_SCC_A].wr[9] & 0x03) | 0xC0);
    s->ptr = 0;
}

/* ---- register access ------------------------------------------------------------------------ */

static void write_wr(mc_scc *s, int ch, unsigned reg, uint8_t v) {
    mc_scc_chan *c = &s->ch[ch];
    switch (reg) {
    case 0: {
        unsigned cmd = (v >> 3) & 7;
        s->ptr = v & 7;
        switch (cmd) {
        case CMD_POINT_HIGH: s->ptr |= 8; break;
        case CMD_RESET_EXT:
            /* Opens the latches and clears the IP (2.4.9); a zero count
               may close them again. */
            c->latch = 0;
            c->ip &= (uint8_t)~IP_EXT;
            zc_arm(s, c);
            break;
        case CMD_SEND_ABORT:
            c->rr0 |= RR0_TX_UNDERRUN;   /* 2.4.9.2 */
            break;
        case CMD_INT_NEXT_RX: c->rx_first = 1; break;
        case CMD_RESET_TX_IP: c->ip &= (uint8_t)~IP_TX; break;
        case CMD_ERROR_RESET: c->rr1 &= (uint8_t)~(RR1_PARITY | RR1_OVERRUN | RR1_FRAMING); break;
        case CMD_RESET_IUS:
            /* Reset Highest IUS: the highest-priority IUS bit of the chip (2.4.4.4). */
            for (int i = 0; i < 6; i++) {
                static const unsigned order[3] = {IP_RX, IP_TX, IP_EXT};
                mc_scc_chan *x = &s->ch[i / 3];
                if (x->ius & order[i % 3]) { x->ius &= (uint8_t)~order[i % 3]; break; }
            }
            break;
        default: break;
        }
        /* D7-D6: 11 resets the Tx Underrun/EOM latch (RR0 D6 reads 0 until
           the next underrun); the CRC resets have no effect here. In
           asynchronous mode the bit is always set (2.4.9.2). */
        if ((v >> 6) == 3 && !async_mode(c)) c->rr0 &= (uint8_t)~RR0_TX_UNDERRUN;
        c->wr[0] = v;
        return;
    }
    case 2: s->ch[MC_SCC_A].wr[2] = v; break;   /* one vector register for both channels (5.2.3) */
    case 8:
        c->tx_buf = v;
        c->tx_full = 1;
        c->ip &= (uint8_t)~IP_TX;   /* writing the buffer clears the Tx IP (2.4.8) */
        c->rr1 &= (uint8_t)~RR1_ALL_SENT;
        tx_load(s, c);
        break;
    case 9: {
        unsigned reset = v >> 6;   /* 5.2.11: 01 channel B, 10 channel A, 11 hardware */
        if (reset == 3) {
            hardware_reset(s);
            break;
        }
        if (reset == 1) channel_reset(s, &s->ch[MC_SCC_B]);
        if (reset == 2) channel_reset(s, &s->ch[MC_SCC_A]);
        s->ch[MC_SCC_A].wr[9] = v & 0x3F;
        break;
    }
    case 14: {
        unsigned dpll = v >> 5;   /* 5.2.16: DPLL commands */
        if (dpll == 4) c->dpll_brg = 1;
        if (dpll == 5) c->dpll_brg = 0;
        c->wr[14] = v;
        zc_arm(s, c);
        tx_load(s, c);
        break;
    }
    case 1:
        if (((v >> 3) & 3) == 1 && ((c->wr[1] >> 3) & 3) != 1) c->rx_first = 1;   /* first character after the mode is selected */
        c->wr[1] = v;
        break;
    case 15: c->wr[15] = v; zc_arm(s, c); break;
    case 3:
        c->wr[3] = v;
        if (!(v & 1)) c->rx_busy = 0;
        rx_start(s, c);
        /* Enter Hunt Mode closes the latches in the synchronous modes (2.4.9.5). */
        if ((v & 0x10) && !async_mode(c)) ext_change(s, c, RR0_SYNC);
        break;
    case 5:
        c->wr[5] = v;
        tx_load(s, c);
        break;
    case 12: case 13: c->wr[reg] = v; break;
    default: c->wr[reg] = v; break;
    }
}

/* RR3's order within a channel: D0 Ext/Status, D1 Tx, D2 Rx (Figure 2-12). */
static unsigned rr3_bits(unsigned ip) {
    return ((ip & IP_EXT) ? 1u : 0) | ((ip & IP_TX) ? 2u : 0) | ((ip & IP_RX) ? 4u : 0);
}

static uint8_t read_rr(mc_scc *s, int ch, unsigned reg) {
    mc_scc_chan *c = &s->ch[ch];
    /* The Z85C30 aliases (Table 5-3, WR15 D2 = 0): RR4-RR7 read RR0-RR3,
       RR9 RR13, RR11 RR15, RR14 RR10. */
    switch (reg) {
    case 4: case 5: case 6: case 7: reg -= 4; break;
    case 9: reg = 13; break;
    case 11: reg = 15; break;
    case 14: reg = 10; break;
    default: break;
    }
    switch (reg) {
    case 0: {
        uint8_t live = live_rr0(c);
        uint8_t latched = c->wr[15] & (RR0_DCD | RR0_SYNC | RR0_CTS | RR0_TX_UNDERRUN | RR0_BREAK);
        uint8_t v = c->latch ? (uint8_t)((live & ~latched) | (c->rr0_latched & latched)) : live;
        if (c->fifo_n) v |= RR0_RX_AVAIL;
        if (!c->tx_full) v |= RR0_TX_EMPTY;
        return v;
    }
    case 1: {
        uint8_t v = (uint8_t)(c->rr1 & (RR1_PARITY | RR1_OVERRUN)) | RR1_RESIDUE;
        if (c->fifo_n) v |= c->fifo_st[0] & (RR1_PARITY | RR1_OVERRUN | RR1_FRAMING);
        if (!c->tx_full && !c->tx_busy) v |= RR1_ALL_SENT;
        return v;
    }
    case 2:
        if (ch == MC_SCC_A) return s->ch[MC_SCC_A].wr[2];
        {
            /* Channel B: the vector with the status of the highest pending
               interrupt, whatever VIS (2.4.6.1); with WR9 D5 the read is a
               software acknowledge (2.4.6.3). */
            int src = highest(s);
            uint8_t v = modified_vector(s, status_code(s, src));
            if (s->ch[MC_SCC_A].wr[9] & 0x20) acknowledge(s, src);
            return v;
        }
    case 3:
        if (ch == MC_SCC_B) return 0;   /* 5.3.4: always 0 in channel B */
        return (uint8_t)(rr3_bits(pending(&s->ch[MC_SCC_B])) | rr3_bits(pending(&s->ch[MC_SCC_A])) << 3);
    case 8: {
        if (!c->fifo_n) return c->fifo[0];
        uint8_t v = c->fifo[0];
        c->rr1 |= c->fifo_st[0] & (RR1_PARITY | RR1_OVERRUN);   /* latched until Error Reset (5.3.2) */
        c->fifo[0] = c->fifo[1]; c->fifo_st[0] = c->fifo_st[1];
        c->fifo[1] = c->fifo[2]; c->fifo_st[1] = c->fifo_st[2];
        c->fifo_n--;
        if (c->fifo_n == 0) c->fifo[0] = v;
        c->rx_first = 0;
        return v;
    }
    case 10: return 0;   /* 5.3.11: loop and clock-missing status, all clear in asynchronous mode */
    case 12: return c->wr[12];
    case 13: return c->wr[13];
    case 15: return c->wr[15] & 0xFA;   /* 5.3.16: D0 and D2 read 0 */
    default: return 0;
    }
}

static uint32_t scc_read(void *dev, uint32_t offset, unsigned size) {
    mc_scc *s = dev;
    (void)size;
    int ch = (offset & 2) ? MC_SCC_A : MC_SCC_B;
    uint8_t v;
    if (offset & 4) {
        v = read_rr(s, ch, 8);   /* data register: direct, pointer untouched (2.3.4) */
    } else {
        v = read_rr(s, ch, s->ptr);
        s->ptr = 0;
    }
    update(s);
    return v;
}
int mc_scc_dma_read(mc_scc *s, int channel, uint8_t *byte) {
    if (channel < 0 || channel >= 2 || !s->ch[channel].fifo_n) return 0;
    *byte = read_rr(s, channel, 8);
    update(s);
    return 1;
}

int mc_scc_dma_write(mc_scc *s, int channel, uint8_t byte) {
    if (channel < 0 || channel >= 2 || s->ch[channel].tx_full) return 0;
    write_wr(s, channel, 8, byte);
    update(s);
    return 1;
}

static void scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_scc *s = dev;
    (void)size;
    int ch = (offset & 2) ? MC_SCC_A : MC_SCC_B;
    if (offset & 4) {
        write_wr(s, ch, 8, (uint8_t)value);
    } else {
        unsigned reg = s->ptr;
        s->ptr = 0;
        write_wr(s, ch, reg, (uint8_t)value);   /* WR0 sets the pointer itself */
    }
    update(s);
}
static void scc_event(void *dev, uint64_t t) {
    mc_scc *s = dev;
    s->armed = MC_NEVER;
    for (int i = 0; i < 2; i++) {
        mc_scc_chan *c = &s->ch[i];
        if (c->tx_busy && c->tx_done <= t) tx_finish(s, i);
        if (c->rx_busy && c->rx_done <= t) rx_finish(s, i);
        if (c->zc_next <= t) {
            ext_change(s, c, RR0_ZERO_COUNT);
            zc_arm(s, c);
        }
    }
    update(s);
}
static void scc_reset(void *dev) {
    mc_scc *s = dev;
    hardware_reset(s);
    update(s);
}

/* A control read with the register pointer at 0 reads RR0 and changes
 * nothing: its bits move at the chip's events (a character sent or received,
 * a zero count) and at the host's modem lines, which change between run
 * slices. */
static int scc_stable(void *dev, uint32_t offset, unsigned size) {
    const mc_scc *s = dev;
    (void)size;
    return !(offset & 4) && s->ptr == 0;
}

const mc_dev_ops mc_scc_ops = {scc_read, scc_write, scc_event, scc_reset, scc_stable};

void mc_scc_init(mc_scc *s, const mc_hw_host *host) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_scc_ops;
    s->dev.host = host;
    s->dev.name = "SCC";
    s->armed = MC_NEVER;
    s->pclk_hz = MC_SCC_MAC_CLOCK_HZ;
    for (int i = 0; i < 2; i++) {
        s->ch[i].rtxc_hz = MC_SCC_MAC_CLOCK_HZ;
        s->ch[i].dcd = 1;
        s->ch[i].cts = 1;
        s->ch[i].zc_next = MC_NEVER;
    }
    hardware_reset(s);
}

void mc_scc_set_clocks(mc_scc *s, uint32_t pclk_hz, uint32_t rtxc_a_hz, uint32_t rtxc_b_hz) {
    s->pclk_hz = pclk_hz;
    s->ch[MC_SCC_A].rtxc_hz = rtxc_a_hz;
    s->ch[MC_SCC_B].rtxc_hz = rtxc_b_hz;
}
void mc_scc_set_trxc(mc_scc *s, int channel, uint32_t hz) { s->ch[channel & 1].trxc_hz = hz; }
void mc_scc_set_port(mc_scc *s, const mc_scc_port *port) {
    if (port) s->port = *port;
    else memset(&s->port, 0, sizeof s->port);
}
size_t mc_scc_receive_room(const mc_scc *s, int channel) { return MC_SCC_LINE_QUEUE - s->ch[channel & 1].line_n; }
size_t mc_scc_receive(mc_scc *s, int channel, const uint8_t *data, size_t count) {
    mc_scc_chan *c = &s->ch[channel & 1];
    size_t n = 0;
    while (n < count && c->line_n < MC_SCC_LINE_QUEUE) {
        c->line[(c->line_head + c->line_n) % MC_SCC_LINE_QUEUE] = data[n++];
        c->line_n++;
    }
    rx_start(s, c);
    update(s);
    return n;
}
void mc_scc_set_dcd(mc_scc *s, int channel, int asserted) {
    mc_scc_chan *c = &s->ch[channel & 1];
    set_input(s, c, &c->dcd, asserted, RR0_DCD);
    update(s);
}
void mc_scc_set_cts(mc_scc *s, int channel, int asserted) {
    mc_scc_chan *c = &s->ch[channel & 1];
    set_input(s, c, &c->cts, asserted, RR0_CTS);
    tx_load(s, c);
    update(s);
}
void mc_scc_set_break(mc_scc *s, int channel, int on) {
    mc_scc_chan *c = &s->ch[channel & 1];
    on = on != 0;
    if (c->break_in == on) return;
    c->break_in = (uint8_t)on;
    ext_change(s, c, RR0_BREAK);
    /* The end of a break leaves a single null character in the FIFO (4.2.2). */
    if (!on && rx_enabled(c)) rx_push(c, 0, 0);
    update(s);
}
unsigned mc_scc_outputs(const mc_scc *s, int channel) { return s->ch[channel & 1].outputs; }

int mc_scc_iack(mc_scc *s) {
    if (!(s->ch[MC_SCC_A].wr[9] & 8)) return -1;
    int src = highest(s);
    if (src < 0) return -1;
    acknowledge(s, src);
    uint8_t wr9 = s->ch[MC_SCC_A].wr[9];
    int v = (wr9 & 2) ? -1 : (wr9 & 1) ? modified_vector(s, status_code(s, src)) : s->ch[MC_SCC_A].wr[2];
    update(s);
    return v;
}
