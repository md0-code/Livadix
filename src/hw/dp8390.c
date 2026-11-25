/* Livadix — National DP8390 NIC (dp8390.h). Section numbers are those of the
 * DP8390D/NS32490D datasheet (National, Local Area Network Databook). */
#include "dp8390.h"

#include "sonic.h"

#include <string.h>

/* The wire: 10 Mbit/s, 64 bits of preamble and start of frame delimiter
 * before each frame and the 9.6 us interframe gap after it (IEEE 802.3;
 * NIC DS 1.0). */
#define WIRE_BPS 10000000u
#define WIRE_EXTRA_BITS (64u + 96u)

static uint64_t now(const mc_dp8390 *n) { return mc_dev_now(&n->dev); }
static int running(const mc_dp8390 *n) { return (n->cr & (MC_DP8390_CR_STA | MC_DP8390_CR_STP)) == MC_DP8390_CR_STA; }

static void update_irq(mc_dp8390 *n) {
    const int irq = (n->isr & n->imr & 0x7F) != 0;
    if (irq != n->irq) { n->irq = irq; mc_dev_line(&n->dev, MC_DP8390_LINE_IRQ, irq); }
}

static uint64_t wire_time(size_t length) {
    const uint64_t bits = (uint64_t)length * 8u + WIRE_EXTRA_BITS;
    return (bits * MC_C15M_HZ + WIRE_BPS - 1) / WIRE_BPS;
}

/* The device's one deadline: the end of the transmission or of the frame
 * on the wire, whichever comes first. */
static void arm(mc_dp8390 *n) {
    uint64_t when = n->tx_end;
    if (n->wire_count && n->wire_end < when) when = n->wire_end;
    mc_dev_schedule(&n->dev, when);
}

/* ---- buffer memory (local DMA) ------------------------------------------------ */

static uint8_t *mem_at(mc_dp8390 *n, uint32_t a) { return &n->mem[a % n->mem_size]; }

/* A tally counter (11.0 "Tally Counters"): saturates at FFh; its MSB sets
 * ISR CNT. */
static void count(mc_dp8390 *n, unsigned which) {
    if (n->cntr[which] != 0xFF) n->cntr[which]++;
    if (n->cntr[which] & 0x80) n->isr |= MC_DP8390_ISR_CNT;
}

/* ---- reception (7.0, 8.0) ---------------------------------------------------- */

unsigned mc_dp8390_hash(const uint8_t a[6]) {
    /* The CRC generator over the destination address, bits in transmit
     * order (each byte's bit 0 first); its six most significant bits pick
     * the bit of MAR0-7, MAR0 bit 0 for 0 (8.0 "Multicast Address"). */
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned i = 0; i < 6; i++)
        for (unsigned b = 0; b < 8; b++) {
            const uint32_t in = (a[i] >> b & 1u) ^ (crc >> 31);
            crc <<= 1;
            if (in) crc ^= 0x04C11DB7u;
        }
    return crc >> 26;
}

static int accept(const mc_dp8390 *n, const uint8_t *dst, uint8_t *rsr) {
    if (dst[0] & 1) {
        static const uint8_t all[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        *rsr |= MC_DP8390_RSR_PHY;
        if (!memcmp(dst, all, 6)) return (n->rcr & MC_DP8390_RCR_AB) != 0;
        const unsigned h = mc_dp8390_hash(dst);
        return (n->rcr & MC_DP8390_RCR_PRO) || ((n->rcr & MC_DP8390_RCR_AM) && (n->mar[h >> 3] >> (h & 7) & 1u));
    }
    return (n->rcr & MC_DP8390_RCR_PRO) || !memcmp(dst, n->par, 6);
}

static void receive(mc_dp8390 *n, const mc_dp8390_frame *f) {
    if (!running(n) || (n->tcr & MC_DP8390_TCR_LB)) return;   /* stopped, or cut off in loopback (13.0) */
    uint8_t rsr = MC_DP8390_RSR_PRX;
    if (!accept(n, f->data, &rsr)) return;
    if (n->rcr & MC_DP8390_RCR_MON) {                       /* checked and counted, not buffered */
        n->rsr = rsr | MC_DP8390_RSR_MPA;
        count(n, 2);
        return;
    }
    const unsigned ring = n->pstop > n->pstart ? (unsigned)(n->pstop - n->pstart) : 0;
    if (!ring || n->curr < n->pstart || n->curr >= n->pstop) return;
    const unsigned total = 4u + f->length, pages = (total + 255u) / 256u;
    const unsigned bnry = n->bnry < n->pstart || n->bnry >= n->pstop ? n->pstart : n->bnry;
    const unsigned avail = (bnry + ring - n->curr) % ring;   /* pages up to, not into, BNRY */
    if (pages > avail) {
        /* Ring overflow (7.0 "Buffer Recovery"): the packet is lost. */
        n->rsr = rsr | MC_DP8390_RSR_MPA;
        n->isr |= MC_DP8390_ISR_OVW;
        count(n, 2);
        return;
    }
    const unsigned next = n->pstart + (n->curr - n->pstart + pages) % ring;
    uint32_t a = (uint32_t)n->curr << 8;
    const uint8_t header[4] = {rsr, (uint8_t)next, (uint8_t)f->length, (uint8_t)(f->length >> 8)};
    for (unsigned i = 0; i < total; i++) {
        *mem_at(n, a) = i < 4 ? header[i] : f->data[i - 4];
        if (++a >= (uint32_t)n->pstop << 8) a = (uint32_t)n->pstart << 8;
    }
    n->clda = (uint16_t)a;
    n->curr = (uint8_t)next;
    n->rsr = rsr;
    n->isr |= MC_DP8390_ISR_PRX;
}

static int wire_put(mc_dp8390 *n, const uint8_t *frame, size_t length) {
    if (n->wire_count >= MC_DP8390_WIRE || !length) return 0;
    if (length > MC_DP8390_FRAME_MAX) length = MC_DP8390_FRAME_MAX;
    mc_dp8390_frame *f = &n->wire[(n->wire_head + n->wire_count) % MC_DP8390_WIRE];
    memcpy(f->data, frame, length);
    if (length < 60) { memset(f->data + length, 0, 60 - length); length = 60; }   /* the minimum frame */
    const uint32_t crc = mc_sonic_crc32(f->data, length);
    for (unsigned i = 0; i < 4; i++) f->data[length + i] = (uint8_t)(crc >> (8 * i));
    f->length = (uint16_t)(length + 4);
    if (!n->wire_count) n->wire_end = now(n) + wire_time(f->length);
    n->wire_count++;
    arm(n);
    return 1;
}

int mc_dp8390_receive(mc_dp8390 *n, const uint8_t *frame, size_t length) { return wire_put(n, frame, length); }

static void wire_run(mc_dp8390 *n, uint64_t t) {
    while (n->wire_count && n->wire_end <= t) {
        const uint64_t end = n->wire_end;
        mc_dp8390_frame *f = &n->wire[n->wire_head];
        n->wire_head = (n->wire_head + 1) % MC_DP8390_WIRE;
        n->wire_count--;
        receive(n, f);
        if (n->wire_count) n->wire_end = end + wire_time(n->wire[n->wire_head].length);
    }
}

/* ---- transmission (9.0) -------------------------------------------------------- */

/* TBCR bytes from page TPSR go out with the FCS the NIC appends (unless
 * TCR CRC inhibits it; the network takes frames without it either way).
 * Without a cable the packet still completes, with carrier sense lost
 * (TSR CRS, 11.0 "Transmit Status Register"); in loopback it goes nowhere.
 * TXP stays set until the packet is done. */
static void transmit(mc_dp8390 *n) {
    size_t length = n->tbcr;
    if (length > MC_DP8390_FRAME_MAX) length = MC_DP8390_FRAME_MAX;
    const uint32_t base = (uint32_t)n->tpsr << 8;
    for (size_t i = 0; i < length; i++) n->tx[i] = *mem_at(n, base + (uint32_t)i);
    n->tsr = MC_DP8390_TSR_PTX;
    if (!(n->tcr & MC_DP8390_TCR_LB)) {
        if (n->port.transmit && length) n->port.transmit(n->port.ctx, n->tx, length);
        else if (!n->port.transmit) n->tsr |= 0x10;   /* CRS */
    }
    n->ncr = 0;
    n->tx_end = now(n) + wire_time((length < 60 ? 60 : length) + 4);
    arm(n);
}

static void transmit_done(mc_dp8390 *n) {
    n->tx_end = MC_NEVER;
    n->cr &= (uint8_t)~MC_DP8390_CR_TXP;
    n->isr |= MC_DP8390_ISR_PTX;
}

/* ---- remote DMA (10.0) ------------------------------------------------------- */

static uint8_t remote_read(mc_dp8390 *n) {
    if (!n->rbcr) return 0xFF;
    const uint8_t v = *mem_at(n, n->crda);
    if (++n->crda == (uint16_t)(n->pstop << 8)) n->crda = (uint16_t)(n->pstart << 8);
    if (!--n->rbcr) n->isr |= MC_DP8390_ISR_RDC;
    return v;
}
static void remote_write(mc_dp8390 *n, uint8_t v) {
    if (!n->rbcr) return;
    *mem_at(n, n->crda) = v;
    n->crda++;
    if (!--n->rbcr) n->isr |= MC_DP8390_ISR_RDC;
}

/* ---- registers (11.0) ------------------------------------------------------------ */

static void write_cr(mc_dp8390 *n, uint8_t v) {
    uint8_t state = n->cr & (MC_DP8390_CR_STA | MC_DP8390_CR_STP | MC_DP8390_CR_TXP);
    if (v & MC_DP8390_CR_STP) {
        /* Stopped: nothing is in flight here, so the reset state comes at
         * once (ISR RST, 11.0 "Interrupt Status Register"). */
        state = (uint8_t)((state & MC_DP8390_CR_TXP) | MC_DP8390_CR_STP);
        n->isr |= MC_DP8390_ISR_RST;
    } else if (v & MC_DP8390_CR_STA) {
        state = (uint8_t)((state & MC_DP8390_CR_TXP) | MC_DP8390_CR_STA);
        n->isr &= (uint8_t)~MC_DP8390_ISR_RST;
    }
    n->cr = (uint8_t)((v & (MC_DP8390_CR_PS | MC_DP8390_CR_RD)) | state);
    if ((v & MC_DP8390_CR_TXP) && running(n) && n->tx_end == MC_NEVER) {
        n->cr |= MC_DP8390_CR_TXP;
        transmit(n);
    }
    /* Send Packet (RD 011): the remote DMA reads the packet at BNRY, its
     * byte count from the header (10.0 "Send Packet Command"). */
    if ((v & MC_DP8390_CR_RD) == 0x18) {
        n->crda = (uint16_t)(n->bnry << 8);
        n->rbcr = (uint16_t)(*mem_at(n, n->crda + 2u) | *mem_at(n, n->crda + 3u) << 8);
    }
}

uint8_t mc_dp8390_read(mc_dp8390 *n, unsigned r) {
    if (r == 16) { const uint8_t v = remote_read(n); update_irq(n); return v; }
    r &= 15;
    if (r == 0) return n->cr;
    switch (n->cr >> 6) {
    case 0:
        switch (r) {
        case 1: return (uint8_t)n->clda;
        case 2: return (uint8_t)(n->clda >> 8);
        case 3: return n->bnry;
        case 4: return n->tsr;
        case 5: return n->ncr;
        case 6: return 0;                                   /* FIFO: not modelled */
        case 7: return n->isr;
        case 8: return (uint8_t)n->crda;
        case 9: return (uint8_t)(n->crda >> 8);
        case 12: return n->rsr;
        case 13: case 14: case 15: {                        /* the counters clear when read */
            const uint8_t v = n->cntr[r - 13];
            n->cntr[r - 13] = 0;
            return v;
        }
        default: return 0xFF;
        }
    case 1:
        if (r <= 6) return n->par[r - 1];
        if (r == 7) return n->curr;
        return n->mar[r - 8];
    case 2:
        switch (r) {
        case 1: return n->pstart;
        case 2: return n->pstop;
        case 3: return n->bnry;                             /* remote next packet pointer */
        case 4: return n->tpsr;
        case 5: return n->curr;                             /* local next packet pointer */
        case 12: return (uint8_t)(n->rcr | 0xC0);
        case 13: return (uint8_t)(n->tcr | 0xE0);
        case 14: return (uint8_t)(n->dcr | 0x80);
        case 15: return (uint8_t)(n->imr | 0x80);
        default: return 0xFF;
        }
    default: return 0xFF;
    }
}

void mc_dp8390_write(mc_dp8390 *n, unsigned r, uint8_t v) {
    if (r == 16) { remote_write(n, v); update_irq(n); return; }
    r &= 15;
    if (r == 0) write_cr(n, v);
    else if ((n->cr >> 6) == 0) {
        switch (r) {
        case 1: n->pstart = v; break;
        case 2: n->pstop = v; break;
        case 3: n->bnry = v; break;
        case 4: n->tpsr = v; break;
        case 5: n->tbcr = (uint16_t)((n->tbcr & 0xFF00) | v); break;
        case 6: n->tbcr = (uint16_t)((n->tbcr & 0x00FF) | v << 8); break;
        case 7: n->isr &= (uint8_t)~(v & 0x7F); break;      /* RST is cleared by STA alone */
        case 8: n->rsar = (uint16_t)((n->rsar & 0xFF00) | v); n->crda = n->rsar; break;
        case 9: n->rsar = (uint16_t)((n->rsar & 0x00FF) | v << 8); n->crda = n->rsar; break;
        case 10: n->rbcr = (uint16_t)((n->rbcr & 0xFF00) | v); break;
        case 11: n->rbcr = (uint16_t)((n->rbcr & 0x00FF) | v << 8); break;
        case 12: n->rcr = v & 0x3F; break;
        case 13: n->tcr = v & 0x1F; break;
        case 14: n->dcr = v & 0x7F; break;
        case 15: n->imr = v & 0x7F; break;
        }
    } else if ((n->cr >> 6) == 1) {
        if (r <= 6) n->par[r - 1] = v;
        else if (r == 7) n->curr = v;
        else n->mar[r - 8] = v;
    }
    update_irq(n);
}

static uint32_t nic_read(void *dev, uint32_t offset, unsigned size) {
    (void)size;
    return mc_dp8390_read(dev, offset & 15);
}
static void nic_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)size;
    mc_dp8390_write(dev, offset & 15, (uint8_t)value);
}

static void nic_event(void *dev, uint64_t t) {
    mc_dp8390 *n = dev;
    if (n->tx_end <= t) transmit_done(n);
    wire_run(n, t);
    arm(n);
    update_irq(n);
}

/* Hardware reset (11.0, "Command Register" and "Interrupt Status
 * Register"): stopped with the remote DMA aborted (CR 21h), ISR RST, every
 * interrupt masked. */
static void nic_reset(void *dev) {
    mc_dp8390 *n = dev;
    n->cr = 0x21;
    n->isr = MC_DP8390_ISR_RST;
    n->imr = 0;
    n->tx_end = MC_NEVER;
    n->wire_count = 0;
    n->wire_head = 0;
    arm(n);
    update_irq(n);
}

const mc_dev_ops mc_dp8390_ops = {nic_read, nic_write, nic_event, nic_reset};

void mc_dp8390_init(mc_dp8390 *n, const mc_hw_host *host, uint8_t *mem, uint32_t mem_size) {
    memset(n, 0, sizeof *n);
    n->dev.ops = &mc_dp8390_ops;
    n->dev.host = host;
    n->dev.name = "DP8390";
    n->mem = mem;
    n->mem_size = mem_size;
    n->tx_end = MC_NEVER;
    n->cr = 0x21;
    n->isr = MC_DP8390_ISR_RST;
}

void mc_dp8390_set_port(mc_dp8390 *n, const mc_dp8390_port *port) {
    if (port) n->port = *port;
    else memset(&n->port, 0, sizeof n->port);
}
