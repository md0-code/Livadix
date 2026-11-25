/* Livadix — National DP83932 SONIC (sonic.h). Section numbers are those of the
 * DP83932B datasheet (National, Local Area Network Databook, 1992). */
#include "sonic.h"

#include <string.h>

const uint8_t mc_sonic_default_address[6] = {0x08, 0x00, 0x07, 0x4D, 0x41, 0x43};

/* The general purpose timer counts at half the transmit clock (4.3.12): the
 * 10 MHz TXC of the 802.3 ENDEC gives 5 MHz. */
#define TIMER_HZ 5000000u
/* The wire: 10 Mbit/s (1.1); each frame comes with 64 bits of preamble and
 * start of frame delimiter (2.1) and is followed by the 9.6 us interframe
 * gap (1.2.2), 96 bits. */
#define WIRE_BPS 10000000u
#define WIRE_EXTRA_BITS (64u + 96u)

static int in_reset(const mc_sonic *s) { return (s->reg[MC_SONIC_CR] & MC_SONIC_CR_RST) != 0; }
static uint64_t now(const mc_sonic *s) { return mc_dev_now(&s->dev); }
static int wide(const mc_sonic *s) { return (s->reg[MC_SONIC_DCR] & MC_SONIC_DCR_DW) != 0; }

static void update_irq(mc_sonic *s) {
    int irq = (s->reg[MC_SONIC_ISR] & s->reg[MC_SONIC_IMR]) != 0;
    if (irq != s->irq) { s->irq = irq; mc_dev_line(&s->dev, MC_SONIC_LINE_IRQ, irq); }
}

/* ---- general purpose timer (4.3.12) and the device's one deadline -------- */

static uint64_t ticks_since(const mc_sonic *s, uint64_t t) {
    return t <= s->timer_base ? 0 : (t - s->timer_base) * TIMER_HZ / MC_C15M_HZ;
}
static uint32_t timer_now(const mc_sonic *s) {
    if (!s->timer_running) return s->timer_value;
    return s->timer_value - (uint32_t)ticks_since(s, now(s));
}
/* The deadline: the timer's next rollover (from 0 to FFFFFFFFh after value
 * + 1 ticks; each sets TC in the ISR) or the end of the frame on the wire,
 * whichever comes first. */
static void arm(mc_sonic *s) {
    uint64_t when = MC_NEVER;
    if (s->timer_running) {
        uint64_t ticks = (uint64_t)s->timer_value + 1;
        when = s->timer_base + (ticks * MC_C15M_HZ + TIMER_HZ - 1) / TIMER_HZ;
    }
    if (s->wire_count && s->wire_end < when) when = s->wire_end;
    mc_dev_schedule(&s->dev, when);
}
static void timer_rebase(mc_sonic *s) {
    s->timer_value = timer_now(s);
    s->timer_base = now(s);
}
static void timer_set_running(mc_sonic *s, int run) {
    timer_rebase(s);
    s->timer_running = run;
    arm(s);
}

/* ---- memory as bus master (1.3, 3.3) --------------------------------------- */

/* A descriptor field: 16-bit words in 16-bit mode, the low half of a long
 * word in 32-bit mode (DCR DW, 4.3.2; 3.3 descriptor data alignment); the
 * bus is big-endian (BMODE = 1, 1.3), so the low half is the long word's
 * second 16 bits. */
static uint32_t field(const mc_sonic *s, uint32_t base, unsigned index) {
    return wide(s) ? base + index * 4 + 2 : base + index * 2;
}
static unsigned field_size(const mc_sonic *s) { return wide(s) ? 4u : 2u; }
static int rd(mc_sonic *s, uint32_t a, uint16_t *v) {
    uint8_t b[2];
    if (!mc_dev_dma(&s->dev, a, b, 2, 0)) return 0;
    *v = (uint16_t)(b[0] << 8 | b[1]);
    return 1;
}
static int wr(mc_sonic *s, uint32_t a, uint16_t v) {
    uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    return mc_dev_dma(&s->dev, a, b, 2, 1);
}
static uint32_t upper(const mc_sonic *s, unsigned reg) { return (uint32_t)s->reg[reg] << 16; }
static uint32_t pair(const mc_sonic *s, unsigned lo, unsigned hi) { return (uint32_t)s->reg[hi] << 16 | s->reg[lo]; }
static void set_pair(mc_sonic *s, unsigned lo, unsigned hi, uint32_t v) {
    s->reg[lo] = (uint16_t)v;
    s->reg[hi] = (uint16_t)(v >> 16);
}

/* ---- CAM (4.1) --------------------------------------------------------------- */

/* Load CAM: CDC descriptors (entry pointer, CAP0, CAP1, CAP2) from the CDA
 * at URRA:CDP, then the CAM enable field (4.1.1). */
static void load_cam(mc_sonic *s) {
    if (s->dev.host->dma) {
        unsigned count = s->reg[MC_SONIC_CDC] & 0x1F;
        uint32_t base = upper(s, MC_SONIC_URRA);
        while (count--) {
            uint32_t d = base | s->reg[MC_SONIC_CDP];
            uint16_t ep = 0, c0 = 0, c1 = 0, c2 = 0;
            if (!rd(s, field(s, d, 0), &ep) || !rd(s, field(s, d, 1), &c0) || !rd(s, field(s, d, 2), &c1) ||
                !rd(s, field(s, d, 3), &c2)) {
                s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_BR;
                break;
            }
            s->cam[ep & 15][0] = c0;
            s->cam[ep & 15][1] = c1;
            s->cam[ep & 15][2] = c2;
            s->reg[MC_SONIC_CDP] = (uint16_t)(s->reg[MC_SONIC_CDP] + 4 * field_size(s));
            s->reg[MC_SONIC_CDC] = (uint16_t)(s->reg[MC_SONIC_CDC] - 1);
        }
        uint16_t ce;
        if (rd(s, field(s, base | s->reg[MC_SONIC_CDP], 0), &ce)) {
            s->reg[MC_SONIC_CE] = ce;
            s->reg[MC_SONIC_CDP] = (uint16_t)(s->reg[MC_SONIC_CDP] + field_size(s));
        }
    }
    s->reg[MC_SONIC_CR] &= (uint16_t)~MC_SONIC_CR_LCAM;
    s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_LCD;
}

/* An address in the CAM, in an entry CE enables. CAP0 holds the address's
 * first two bytes, the first in its low half (4.3.10: 10:20:30:40:50:60
 * loads CAP0 2010h, CAP1 4030h, CAP2 6050h). */
static int cam_match(const mc_sonic *s, const uint8_t *a) {
    const uint16_t c0 = (uint16_t)(a[1] << 8 | a[0]), c1 = (uint16_t)(a[3] << 8 | a[2]), c2 = (uint16_t)(a[5] << 8 | a[4]);
    for (unsigned e = 0; e < 16; e++)
        if ((s->reg[MC_SONIC_CE] >> e & 1) && s->cam[e][0] == c0 && s->cam[e][1] == c1 && s->cam[e][2] == c2) return 1;
    return 0;
}

/* ---- the receive resource area (3.4.1, 3.4.7) ---------------------------------- */

/* Reads the resource descriptor at URRA:RRP (buffer pointer low, high, word
 * count low, high) into CRBA0/1 and RBWC0/1; RRP steps and wraps from REA to
 * RSA. RBE once RRP has reached RWP (the last descriptor read, 3.4.7). */
static void read_rra(mc_sonic *s) {
    if (!s->dev.host->dma) return;
    uint32_t d = upper(s, MC_SONIC_URRA) | s->reg[MC_SONIC_RRP];
    static const unsigned to[4] = {MC_SONIC_CRBA0, MC_SONIC_CRBA1, MC_SONIC_RBWC0, MC_SONIC_RBWC1};
    for (unsigned i = 0; i < 4; i++) {
        uint16_t v;
        if (rd(s, field(s, d, i), &v)) s->reg[to[i]] = v;
        else s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_BR;
    }
    uint16_t next = (uint16_t)(s->reg[MC_SONIC_RRP] + 4 * field_size(s));
    if (next == s->reg[MC_SONIC_REA]) next = s->reg[MC_SONIC_RSA];
    s->reg[MC_SONIC_RRP] = next;
    if (next == s->reg[MC_SONIC_RWP]) s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_RBE;
    s->rx_no_buffer = 0;
}

/* The next RBA once the current one is used (3.4.6.1): its sequence number
 * steps and the packet number restarts (4.3.9). With RBE still set there is
 * none until the system adds descriptors and clears RBE (3.4.7, ISR note 2). */
static void next_rba(mc_sonic *s) {
    if ((s->reg[MC_SONIC_ISR] & MC_SONIC_INT_RBE) || s->reg[MC_SONIC_RRP] == s->reg[MC_SONIC_RWP]) {
        s->rx_no_buffer = 1;
        return;
    }
    read_rra(s);
    s->reg[MC_SONIC_RSC] = (uint16_t)((s->reg[MC_SONIC_RSC] + 0x100) & 0xFF00);
}

/* ---- tally counters (4.3.11) ----------------------------------------------------- */

static void count_missed(mc_sonic *s) {
    if (in_reset(s)) return;   /* halted while RST is set */
    s->reg[MC_SONIC_MPT] = (uint16_t)(s->reg[MC_SONIC_MPT] + 1);
    if (!s->reg[MC_SONIC_MPT]) s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_MP;
}

/* ---- reception (3.4.5, 3.4.6) ------------------------------------------------------- */

/* Whether the address filter takes a frame (1.2.1, 4.3.3, 4.3.7), and its
 * MC and BC status. */
static int accept(const mc_sonic *s, const uint8_t *dst, uint16_t *status) {
    const uint16_t rcr = s->reg[MC_SONIC_RCR];
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    /* RJCM inverts the CAM's verdict only, not BRD, PRO or AMC (4.3.7). */
    const int cam = cam_match(s, dst) != ((s->reg[MC_SONIC_DCR2] & MC_SONIC_DCR2_RJCM) != 0);
    *status = 0;
    if (!memcmp(dst, broadcast, 6)) {
        *status = MC_SONIC_RCR_BC;
        return cam || (rcr & (MC_SONIC_RCR_BRD | MC_SONIC_RCR_AMC));
    }
    if (dst[0] & 1) {
        *status = MC_SONIC_RCR_MC;
        return cam || (rcr & MC_SONIC_RCR_AMC);
    }
    return cam || (rcr & MC_SONIC_RCR_PRO);
}

/* The end of a frame on the wire: filtered, then buffered into the RBA at
 * CRBA with its descriptor at URDA:CRDA. */
static void receive(mc_sonic *s, const mc_sonic_frame *f) {
    uint16_t status;
    if (in_reset(s) || !accept(s, f->data, &status)) return;
    if (!(s->reg[MC_SONIC_CR] & MC_SONIC_CR_RXEN) || !s->dev.host->dma) { count_missed(s); return; }
    const unsigned fs = field_size(s);
    /* 3.4.5: after a descriptor with EOL, the link field (CRDA holds its
     * address) is read again; still EOL, reception ceases. */
    if (s->rx_eol) {
        uint16_t link = 1;
        const uint32_t la = upper(s, MC_SONIC_URDA) | s->reg[MC_SONIC_CRDA];
        if (!rd(s, field(s, la, 0), &link) || (link & 1)) { count_missed(s); return; }
        wr(s, field(s, la, 1), 0);   /* RXpkt.in_use of the descriptor it kept */
        s->reg[MC_SONIC_CRDA] = link;
        s->rx_eol = 0;
    }
    if (s->rx_no_buffer) { count_missed(s); return; }
    /* The packet in whole words (16-bit mode) or long words (32-bit mode),
     * the last filled with FFh (1.4.1); RBWC counts 16-bit words. */
    const unsigned unit = wide(s) ? 4u : 2u;
    const uint32_t padded = (f->length + unit - 1) / unit * unit, words = padded / 2;
    uint32_t rbwc = pair(s, MC_SONIC_RBWC0, MC_SONIC_RBWC1);
    const uint32_t crba = pair(s, MC_SONIC_CRBA0, MC_SONIC_CRBA1);
    s->reg[MC_SONIC_TRBA0] = s->reg[MC_SONIC_CRBA0];
    s->reg[MC_SONIC_TRBA1] = s->reg[MC_SONIC_CRBA1];
    s->reg[MC_SONIC_TBWC0] = s->reg[MC_SONIC_RBWC0];
    s->reg[MC_SONIC_TBWC1] = s->reg[MC_SONIC_RBWC1];
    if (words > rbwc) {
        /* RBA limit exceeded: truncated, no descriptor, the space is not
           used again; the next RBA is fetched (3.4.7). */
        s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_RBAE;
        next_rba(s);
        return;
    }
    uint8_t buf[MC_SONIC_FRAME_MAX + 8];
    memcpy(buf, f->data, f->length);
    memset(buf + f->length, 0xFF, padded - f->length);
    if (!mc_dev_dma(&s->dev, crba, buf, padded, 1)) s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_BR;
    rbwc -= words;
    set_pair(s, MC_SONIC_CRBA0, MC_SONIC_CRBA1, crba + padded);
    set_pair(s, MC_SONIC_RBWC0, MC_SONIC_RBWC1, rbwc);
    /* The status: the configuration bits with this packet's (4.3.3; bits
     * 8-6 and 3-0 cleared at each reception). */
    status |= MC_SONIC_RCR_PRX | MC_SONIC_RCR_CRS;
    if (f->loopback) status |= MC_SONIC_RCR_LBK;
    const uint16_t eobc = (uint16_t)(s->reg[MC_SONIC_EOBC] & (wide(s) ? 0xFFFEu : 0xFFFFu));
    const int last = rbwc < eobc;
    if (last) status |= MC_SONIC_RCR_LPKT;
    s->reg[MC_SONIC_RCR] = (uint16_t)((s->reg[MC_SONIC_RCR] & 0xFE00) | status);
    /* The descriptor: status, byte count, packet pointer, sequence numbers,
     * then the link is read (3.4.6.1). */
    const uint32_t d = upper(s, MC_SONIC_URDA) | s->reg[MC_SONIC_CRDA];
    const uint16_t fields[5] = {s->reg[MC_SONIC_RCR], f->length, (uint16_t)crba, (uint16_t)(crba >> 16), s->reg[MC_SONIC_RSC]};
    for (unsigned i = 0; i < 5; i++)
        if (!wr(s, field(s, d, i), fields[i])) s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_BR;
    s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_PKTRX;
    s->reg[MC_SONIC_RSC] = (uint16_t)((s->reg[MC_SONIC_RSC] & 0xFF00) | ((s->reg[MC_SONIC_RSC] + 1) & 0xFF));
    uint16_t link = 1;
    rd(s, field(s, d, 5), &link);
    if (link & 1) {
        /* No more descriptors: RDE, the descriptor stays the SONIC's and CRDA
           holds its link field's address (3.4.6.1, 3.4.7). */
        s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_RDE;
        s->reg[MC_SONIC_CRDA] = (uint16_t)(d + 5 * fs);
        s->rx_eol = 1;
    } else {
        wr(s, field(s, d, 6), 0);
        s->reg[MC_SONIC_CRDA] = link;
    }
    if (last) next_rba(s);
}

/* ---- the wire ------------------------------------------------------------------------ */

static uint64_t wire_time(size_t length) {
    const uint64_t bits = (uint64_t)length * 8u + WIRE_EXTRA_BITS;
    return (bits * MC_C15M_HZ + WIRE_BPS - 1) / WIRE_BPS;
}

static int wire_put(mc_sonic *s, const uint8_t *frame, size_t length, int loopback) {
    if (s->wire_count >= MC_SONIC_WIRE || !length) return 0;
    if (length > MC_SONIC_FRAME_MAX) length = MC_SONIC_FRAME_MAX;
    mc_sonic_frame *f = &s->wire[(s->wire_head + s->wire_count) % MC_SONIC_WIRE];
    memcpy(f->data, frame, length);
    if (length < 60) { memset(f->data + length, 0, 60 - length); length = 60; }   /* the minimum frame, 64 bytes with the FCS */
    const uint32_t crc = mc_sonic_crc32(f->data, length);
    for (unsigned i = 0; i < 4; i++) f->data[length + i] = (uint8_t)(crc >> (8 * i));
    f->length = (uint16_t)(length + 4);
    f->loopback = (uint8_t)loopback;
    if (!s->wire_count) s->wire_end = now(s) + wire_time(f->length);
    s->wire_count++;
    arm(s);
    return 1;
}

int mc_sonic_receive(mc_sonic *s, const uint8_t *frame, size_t length) {
    /* In MAC and ENDEC loopback the SONIC is cut off from the network (1.7). */
    const unsigned lb = (s->reg[MC_SONIC_RCR] & MC_SONIC_RCR_LB) >> 9;
    if (lb == 1 || lb == 2) return 1;
    return wire_put(s, frame, length, 0);
}

/* Frames whose last bit has arrived by `t`. */
static void wire_run(mc_sonic *s, uint64_t t) {
    while (s->wire_count && s->wire_end <= t) {
        const uint64_t end = s->wire_end;
        mc_sonic_frame *f = &s->wire[s->wire_head];
        s->wire_head = (s->wire_head + 1) % MC_SONIC_WIRE;
        s->wire_count--;
        receive(s, f);
        if (s->wire_count) {
            const mc_sonic_frame *n = &s->wire[s->wire_head];
            s->wire_end = end + wire_time(n->length);   /* back to back */
        }
    }
}

uint32_t mc_sonic_crc32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- transmission (3.5) ---------------------------------------------------------------- */

/* Transmit Packets: walks the TDA from UTDA:CTDA. Each TXpkt is status,
 * config, pkt_size, frag_count, then frag_ptr0, frag_ptr1, frag_size per
 * fragment, then the link (bit 0 EOL) (3.5.1); the fragments are gathered
 * at any byte alignment (3.5.2). With the network the packet goes out
 * (PTX); without it it completes with no carrier (NCRS, CRSL; 4.3.4). A
 * size that is not the fragments' sum aborts with BCM and TXER. A source
 * address not in the CAM sets PMB unless the CRC is inhibited. After each
 * packet CTDA takes its link (3.5.3.1); after an abort it stays on the
 * packet. TXDN at the end of the list, after HTX or an abort (4.3.6).
 * HTX is not cleared: it reads back as written until a reset (4.3.1; the
 * Quadra 700's driver halts the transmitter and waits for CR to read ST,
 * RXDIS and HTX, observed). */
static void transmit(mc_sonic *s) {
    uint16_t isr = MC_SONIC_INT_TXDN;
    if (s->dev.host->dma) {
        for (unsigned guard = 0; guard < 65536; guard++) {
            uint32_t d = upper(s, MC_SONIC_UTDA) | (s->reg[MC_SONIC_CTDA] & 0xFFFEu);
            uint16_t config = 0, size = 0, count = 0, link = 1;
            if (!rd(s, field(s, d, 1), &config) || !rd(s, field(s, d, 2), &size) || !rd(s, field(s, d, 3), &count)) {
                isr |= MC_SONIC_INT_BR;
                break;
            }
            s->reg[MC_SONIC_TTDA] = s->reg[MC_SONIC_CTDA];
            s->reg[MC_SONIC_TCR] = (uint16_t)((s->reg[MC_SONIC_TCR] & 0x0FFF) | (config & 0xF000));
            s->reg[MC_SONIC_TPS] = size;
            s->reg[MC_SONIC_TFC] = count;
            if (config & MC_SONIC_TCR_PINTR) isr |= MC_SONIC_INT_PINT;
            uint32_t sum = 0;
            for (unsigned f = 0; f < count; f++) {
                uint16_t p0 = 0, p1 = 0, fs = 0;
                rd(s, field(s, d, 4 + f * 3), &p0);
                rd(s, field(s, d, 4 + f * 3 + 1), &p1);
                rd(s, field(s, d, 4 + f * 3 + 2), &fs);
                s->reg[MC_SONIC_TSA0] = p0;
                s->reg[MC_SONIC_TSA1] = p1;
                s->reg[MC_SONIC_TFS] = fs;
                if (sum + fs <= sizeof s->tx && !mc_dev_dma(&s->dev, (uint32_t)p1 << 16 | p0, s->tx + sum, fs, 0))
                    isr |= MC_SONIC_INT_BR;
                sum += fs;
            }
            uint16_t status;
            if (sum != size) status = MC_SONIC_TCR_BCM;
            else {
                status = MC_SONIC_TCR_PTX;
                const unsigned lb = (s->reg[MC_SONIC_RCR] & MC_SONIC_RCR_LB) >> 9;
                const size_t n = sum < sizeof s->tx ? sum : sizeof s->tx;
                /* No carrier without the network (no transceiver beyond the
                   ENDEC), and always in MAC loopback (4.3.4). */
                if (lb == 1 || (!s->port.transmit && lb != 2)) status |= MC_SONIC_TCR_NCRS | MC_SONIC_TCR_CRSL;
                if (!(config & MC_SONIC_TCR_CRCI) && n >= 12 && !cam_match(s, s->tx + 6)) status |= MC_SONIC_TCR_PMB;
                if (s->port.transmit && (lb == 0 || lb == 3)) s->port.transmit(s->port.ctx, s->tx, n);
                if (lb) wire_put(s, s->tx, n, 1);
            }
            s->reg[MC_SONIC_TCR] = (uint16_t)((s->reg[MC_SONIC_TCR] & 0xF000) | status);
            wr(s, field(s, d, 0), status);
            rd(s, field(s, d, 4 + count * 3u), &link);
            if (sum != size) { isr |= MC_SONIC_INT_TXER; break; }
            if (s->reg[MC_SONIC_CR] & MC_SONIC_CR_HTX) break;
            s->reg[MC_SONIC_CTDA] = link;
            if (link & 1) break;
        }
    }
    s->reg[MC_SONIC_CR] &= (uint16_t)~MC_SONIC_CR_TXP;   /* HTX stays as written (4.3.1) */
    s->reg[MC_SONIC_ISR] |= isr;
}

/* ---- resets (4.3) --------------------------------------------------------------- */

static void software_reset(mc_sonic *s) {
    /* CR: bits 9, 8, 1, 0 cleared, 7 and 2 set (4.3.1); the timer halts
       (4.3.12). DCR, RCR, TCR, IMR, ISR, DCR2 are unaffected. */
    timer_rebase(s);
    s->timer_running = 0;
    s->reg[MC_SONIC_CR] = (uint16_t)((s->reg[MC_SONIC_CR] & ~0x0303u & ~(unsigned)MC_SONIC_CR_ST) |
                                     MC_SONIC_CR_RST | MC_SONIC_CR_RXDIS | MC_SONIC_CR_STP);
    s->rx_eol = 0;
    arm(s);
}
static void hardware_reset(mc_sonic *s) {
    timer_rebase(s);
    s->timer_running = 0;
    s->reg[MC_SONIC_CR] = MC_SONIC_CR_RST | MC_SONIC_CR_STP | MC_SONIC_CR_RXDIS;   /* 4.3.1: bits 7, 4, 2 */
    s->reg[MC_SONIC_DCR] &= (uint16_t)~0xA000;      /* 4.3.2: bits 15 and 13 cleared */
    s->reg[MC_SONIC_RCR] &= (uint16_t)~0x6600;      /* 4.3.3: RNT, BRO, LB1, LB0 cleared */
    s->reg[MC_SONIC_TCR] = (uint16_t)((s->reg[MC_SONIC_TCR] & 0xF000) | 0x0102);   /* 4.3.4: bits 8 and 1 set */
    s->reg[MC_SONIC_IMR] = 0;                       /* 4.3.5 */
    s->reg[MC_SONIC_ISR] = 0;                       /* 4.3.6 */
    s->reg[MC_SONIC_DCR2] = 0;                      /* 4.3.7 */
    s->reg[MC_SONIC_EOBC] = 0x02F8;                 /* 4.3.9: 760 words */
    s->reg[MC_SONIC_RSC] = 0;                       /* 4.3.9 */
    s->reg[MC_SONIC_CE] = 0;                        /* 4.3.10 */
    s->reg[MC_SONIC_SR] = MC_SONIC_REVISION;        /* 4.3.13 */
    s->rx_eol = 0;
    s->rx_no_buffer = 0;
    s->wire_count = 0;
    arm(s);
    update_irq(s);
}

/* ---- registers ------------------------------------------------------------------------ */

uint16_t mc_sonic_reg(mc_sonic *s, unsigned r) {
    r &= 0x3F;
    switch (r) {
    case MC_SONIC_WT0: return (uint16_t)timer_now(s);
    case MC_SONIC_WT1: return (uint16_t)(timer_now(s) >> 16);
    case MC_SONIC_CAP0: return s->cam[s->reg[MC_SONIC_CEP] & 15][0];   /* 4.3.10: valid in reset mode */
    case MC_SONIC_CAP1: return s->cam[s->reg[MC_SONIC_CEP] & 15][1];
    case MC_SONIC_CAP2: return s->cam[s->reg[MC_SONIC_CEP] & 15][2];
    case MC_SONIC_TPS: return (uint16_t)~s->reg[r];   /* Table 4-2, note 1: reads the inversion */
    default: return s->reg[r];
    }
}

static void write_reg(mc_sonic *s, unsigned r, uint16_t v) {
    switch (r) {
    case MC_SONIC_CR: {
        v &= 0x03BF;   /* bits 15-10 and 6 must be 0 */
        if (in_reset(s)) {
            /* In reset mode a write clears RST (when written 0); the other
               command bits wait for the next write (4.3.1). Setting RST
               again keeps the chip in reset. */
            if (!(v & MC_SONIC_CR_RST)) s->reg[MC_SONIC_CR] &= (uint16_t)~MC_SONIC_CR_RST;
            if (v & MC_SONIC_CR_RST) software_reset(s);
            break;
        }
        if (v & MC_SONIC_CR_RST) { software_reset(s); break; }
        uint16_t cr = s->reg[MC_SONIC_CR];
        if ((v & MC_SONIC_CR_ST) && (v & MC_SONIC_CR_STP)) v &= (uint16_t)~MC_SONIC_CR_ST;   /* both: stop */
        if (v & MC_SONIC_CR_ST) { cr = (uint16_t)((cr | MC_SONIC_CR_ST) & ~MC_SONIC_CR_STP); s->reg[MC_SONIC_CR] = cr; timer_set_running(s, 1); }
        if (v & MC_SONIC_CR_STP) { cr = (uint16_t)((cr | MC_SONIC_CR_STP) & ~MC_SONIC_CR_ST); s->reg[MC_SONIC_CR] = cr; timer_set_running(s, 0); }
        if (v & MC_SONIC_CR_RXEN) cr = (uint16_t)((cr | MC_SONIC_CR_RXEN) & ~MC_SONIC_CR_RXDIS);
        if (v & MC_SONIC_CR_RXDIS) cr = (uint16_t)((cr | MC_SONIC_CR_RXDIS) & ~MC_SONIC_CR_RXEN);
        s->reg[MC_SONIC_CR] = cr | (v & (MC_SONIC_CR_TXP | MC_SONIC_CR_HTX | MC_SONIC_CR_RRRA | MC_SONIC_CR_LCAM));
        if (v & MC_SONIC_CR_LCAM) load_cam(s);
        if (v & MC_SONIC_CR_RRRA) {
            read_rra(s);   /* the RBA sequence number does not step (3.4.3) */
            s->reg[MC_SONIC_CR] &= (uint16_t)~MC_SONIC_CR_RRRA;
        }
        if (v & MC_SONIC_CR_TXP) transmit(s);
        break;
    }
    case MC_SONIC_DCR: case MC_SONIC_DCR2:
        if (in_reset(s)) s->reg[r] = (r == MC_SONIC_DCR) ? (uint16_t)(v & 0xBFFF) : v;   /* Table 4-1, note 3 */
        break;
    case MC_SONIC_RCR: s->reg[r] = (uint16_t)((s->reg[r] & 0x01FF) | (v & 0xFE00)); break;   /* bits 8-0 are status */
    case MC_SONIC_TCR: s->reg[r] = (uint16_t)((s->reg[r] & 0x07FF) | (v & 0xF000)); break;   /* bits 10-0 are status, 11 must be 0 */
    case MC_SONIC_IMR: s->reg[r] = v & 0x7FFF; break;
    case MC_SONIC_ISR: {
        /* 4.3.6: a 1 clears. Clearing RBE after adding resources makes the
           SONIC read the RRA again (note 3). */
        const int rbe = (s->reg[r] & v & MC_SONIC_INT_RBE) != 0;
        s->reg[r] &= (uint16_t)~v;
        if (rbe && s->reg[MC_SONIC_RRP] != s->reg[MC_SONIC_RWP]) {
            const int none = s->rx_no_buffer;
            read_rra(s);
            if (none) s->reg[MC_SONIC_RSC] = (uint16_t)((s->reg[MC_SONIC_RSC] + 0x100) & 0xFF00);
        }
        break;
    }
    case MC_SONIC_CE: if (in_reset(s)) s->reg[r] = v; break;   /* Table 4-1, note 2 */
    case MC_SONIC_CAP0: case MC_SONIC_CAP1: case MC_SONIC_CAP2: case MC_SONIC_SR: case MC_SONIC_MDT: break;   /* read only */
    case MC_SONIC_CRCT: case MC_SONIC_FAET: case MC_SONIC_MPT: s->reg[r] = (uint16_t)~v; break;   /* note 4: inverted */
    case MC_SONIC_WT0: timer_rebase(s); s->timer_value = (s->timer_value & 0xFFFF0000u) | v; arm(s); break;
    case MC_SONIC_WT1: timer_rebase(s); s->timer_value = (s->timer_value & 0xFFFFu) | (uint32_t)v << 16; arm(s); break;
    case MC_SONIC_CEP: s->reg[r] = v & 15; break;
    case MC_SONIC_CDC: s->reg[r] = v & 0x1F; break;
    case MC_SONIC_RSA: case MC_SONIC_REA: case MC_SONIC_RRP: case MC_SONIC_RWP: case MC_SONIC_CDP:
        s->reg[r] = v & 0xFFFE; break;   /* bit 0 unused, reads 0 (4.3.9, 4.3.10) */
    case MC_SONIC_CRDA:
        s->reg[r] = v;
        s->rx_eol = 0;   /* a new list (3.4.7, way 2) */
        break;
    default: s->reg[r] = v; break;
    }
}

static uint32_t sonic_read(void *dev, uint32_t offset, unsigned size) {
    mc_sonic *s = dev;
    uint16_t v = mc_sonic_reg(s, (offset >> s->reg_shift) & 0x3F);
    if (size == 1) return (offset & 1) ? (v & 0xFF) : (uint32_t)(v >> 8);
    return v;
}
static void sonic_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_sonic *s = dev;
    unsigned r = (offset >> s->reg_shift) & 0x3F;
    uint16_t v = (uint16_t)value;
    if (size == 1) {
        uint16_t old = s->reg[r];
        v = (offset & 1) ? (uint16_t)((old & 0xFF00) | (value & 0xFF)) : (uint16_t)((old & 0x00FF) | (value & 0xFF) << 8);
    }
    write_reg(s, r, v);
    update_irq(s);
}
static void sonic_event(void *dev, uint64_t t) {
    mc_sonic *s = dev;
    wire_run(s, t);
    /* A rollover from 0 to FFFFFFFFh: the count goes on (4.3.12). */
    if (s->timer_running && ticks_since(s, t) > s->timer_value) {
        s->reg[MC_SONIC_ISR] |= MC_SONIC_INT_TC;
        timer_rebase(s);
    }
    arm(s);
    update_irq(s);
}
static void sonic_reset(void *dev) { hardware_reset(dev); }

const mc_dev_ops mc_sonic_ops = {sonic_read, sonic_write, sonic_event, sonic_reset};

void mc_sonic_init(mc_sonic *s, const mc_hw_host *host, unsigned reg_shift) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_sonic_ops;
    s->dev.host = host;
    s->dev.name = "SONIC";
    s->reg_shift = reg_shift;
    hardware_reset(s);
}
void mc_sonic_set_port(mc_sonic *s, const mc_sonic_port *port) {
    if (port) s->port = *port;
    else memset(&s->port, 0, sizeof s->port);
}

/* ---- the address PROM ------------------------------------------------------------------- */

static uint8_t reverse8(uint8_t b) {
    b = (uint8_t)((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = (uint8_t)((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return (uint8_t)((b & 0xAA) >> 1 | (b & 0x55) << 1);
}
static uint32_t prom_read(void *dev, uint32_t offset, unsigned size) {
    mc_sonic_prom *p = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | p->bytes[((offset + i) >> p->shift) & 15];
    return v;
}
static void prom_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { (void)dev; (void)offset; (void)size; (void)value; }
static void prom_event(void *dev, uint64_t t) { (void)dev; (void)t; }
static void prom_reset(void *dev) { (void)dev; }
const mc_dev_ops mc_sonic_prom_ops = {prom_read, prom_write, prom_event, prom_reset};

void mc_sonic_prom_init(mc_sonic_prom *p, const mc_hw_host *host, const uint8_t address[6], unsigned shift, int bit_reversed) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_sonic_prom_ops;
    p->dev.host = host;
    p->dev.name = "Ethernet PROM";
    p->shift = shift;
    if (!address) address = mc_sonic_default_address;
    for (int i = 0; i < 6; i++) p->bytes[i] = bit_reversed ? reverse8(address[i]) : address[i];
    /* A check byte: the eight bytes XOR to FFh (byte 6 is 0†). */
    uint8_t x = 0;
    for (int i = 0; i < 7; i++) x ^= p->bytes[i];
    p->bytes[7] = (uint8_t)(x ^ 0xFF);
}
