/* Livadix — the Am79C940 MACE (see am79c940.h). Livadix's own code. */
#include "am79c940.h"
#include "sonic.h"              /* mc_sonic_crc32: the Ethernet FCS */

#include <string.h>

enum {
    R_RCVFIFO = 0, R_XMTFIFO = 1, R_XMTFC = 2, R_XMTFS = 3, R_XMTRC = 4, R_RCVFC = 5, R_RCVFS = 6,
    R_FIFOFC = 7, R_IR = 8, R_IMR = 9, R_PR = 10, R_BIUCC = 11, R_FIFOCC = 12, R_MACCC = 13,
    R_PLSCC = 14, R_PHYCC = 15, R_CHIPID_LO = 16, R_CHIPID_HI = 17, R_IAC = 18, R_LADRF = 20,
    R_PADR = 21, R_MPC = 24, R_RNTPC = 26, R_RCVCC = 27, R_UTR = 29
};

#define IR_XMTINT 0x01u
#define IR_RCVINT 0x02u
#define XMTFS_XMTSV 0x80u
#define RCVFC_ASTRPRCV 0x01u
#define BIUCC_SWRST 0x01u
#define MACCC_ENRCV 0x01u
#define MACCC_ENXMT 0x02u
#define MACCC_DRCVBC 0x04u
#define MACCC_PROM 0x80u
#define IAC_LOGADDR 0x02u
#define IAC_PHYADDR 0x04u
#define UTR_LOOP 0x06u

static void update_irq(mc_am79c940 *m) {
    const uint8_t tx = (m->reg[R_XMTFS] & XMTFS_XMTSV) != 0;
    if (tx != m->tx_status) {
        m->tx_status = tx;
        mc_dev_line(&m->dev, MC_AM79C940_LINE_TX_STATUS, tx);
    }
    const uint8_t on = (m->reg[R_IR] & (uint8_t)~m->reg[R_IMR]) != 0;
    if (on == m->irq) return;
    m->irq = on;
    mc_dev_line(&m->dev, MC_AM79C940_LINE_IRQ, on);
}

/* The data sheet's reset values ("Register Summary"). */
static void reset(mc_am79c940 *m) {
    memset(m->reg, 0, sizeof m->reg);
    m->reg[R_BIUCC] = 0x00;
    m->reg[R_FIFOCC] = 0x0F;
    m->reg[R_CHIPID_LO] = 0x40;
    /* AMD Am79C940, CHIPID: $x940, x the silicon revision. Apple's
     * original PCI MACE driver (9.0.4, $0BB5B510-$0BB5B54C) accepts
     * revision 0 or 1; revision 3 prevents port registration. */
    m->reg[R_CHIPID_HI] = 0x09;
    m->padr_i = m->ladrf_i = m->rcvfs_i = 0;
    update_irq(m);
}

/* The frame to the receiver: filter, status bytes, the DMA. */
static int deliver(mc_am79c940 *m, const uint8_t *frame, size_t length) {
    if (!(m->reg[R_MACCC] & MACCC_ENRCV) || length < 14) return 1;
    const uint8_t *dst = frame;
    int ok = (m->reg[R_MACCC] & MACCC_PROM) || !memcmp(dst, m->padr, 6);
    if (!ok && (dst[0] & 1u)) {
        static const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (!memcmp(dst, bcast, 6)) ok = !(m->reg[R_MACCC] & MACCC_DRCVBC);
        else for (unsigned i = 0; i < 8; i++) ok |= m->ladrf[i] != 0;
    }
    if (!ok) return 1;
    uint8_t buf[1536 + 4];
    if (length > 1514) length = 1514;
    memcpy(buf, frame, length);
    size_t n = length;
    while (n < 60) buf[n++] = 0;        /* the minimum frame (the sender's pad) */
    const uint32_t fcs = mc_sonic_crc32(buf, n);
    buf[n++] = (uint8_t)fcs; buf[n++] = (uint8_t)(fcs >> 8); buf[n++] = (uint8_t)(fcs >> 16); buf[n++] = (uint8_t)(fcs >> 24);
    const unsigned field = (unsigned)buf[12] << 8 | buf[13];
    if ((m->reg[R_RCVFC] & RCVFC_ASTRPRCV) && field < 46) n = 14 + field;
    const uint8_t status[4] = {(uint8_t)n, (uint8_t)((n >> 8) & 0x0Fu), 0, 0};
    if (!m->port.receive || !m->port.receive(m->port.ctx, buf, n, status)) return 0;
    memcpy(m->rcvfs, status, sizeof status);
    return 1;
}

void mc_am79c940_transmit(mc_am79c940 *m, const uint8_t *frame, size_t length) {
    if (!(m->reg[R_MACCC] & MACCC_ENXMT)) return;
    if (m->reg[R_UTR] & UTR_LOOP) deliver(m, frame, length);
    else if (m->port.transmit) m->port.transmit(m->port.ctx, frame, length);
    m->reg[R_XMTFS] = XMTFS_XMTSV;
    m->reg[R_XMTRC] = 0;
    m->reg[R_IR] |= IR_XMTINT;
    update_irq(m);
}

int mc_am79c940_receive(mc_am79c940 *m, const uint8_t *frame, size_t length) {
    if (m->reg[R_UTR] & UTR_LOOP) return 1;     /* the line is not listened to */
    return deliver(m, frame, length);
}

static uint32_t am_read(void *dev, uint32_t offset, unsigned size) {
    mc_am79c940 *m = dev;
    (void)size;
    const unsigned r = (offset >> 4) & 31u;
    uint8_t v = m->reg[r];
    switch (r) {
    case R_RCVFIFO: return 0;
    case R_XMTFS:
        m->reg[R_XMTFS] = 0;
        update_irq(m);
        return v;
    case R_RCVFS:
        v = m->rcvfs[m->rcvfs_i];
        m->rcvfs_i = (m->rcvfs_i + 1) & 3u;
        return v;
    case R_IR:
        m->reg[R_IR] = 0;
        update_irq(m);
        return v;
    case R_PR: return (uint8_t)(m->reg[R_XMTFS] & XMTFS_XMTSV);
    case R_IAC: return (uint8_t)(v & 0x7Fu);    /* the address change is done at once */
    case R_LADRF:
        v = m->ladrf[m->ladrf_i];
        m->ladrf_i = (m->ladrf_i + 1) & 7u;
        return v;
    case R_PADR:
        v = m->padr[m->padr_i];
        m->padr_i = (m->padr_i + 1) % 6u;
        return v;
    case R_MPC: case R_RNTPC: case R_RCVCC:
        m->reg[r] = 0;
        return v;
    default: return v;
    }
}

static void am_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_am79c940 *m = dev;
    (void)size;
    const unsigned r = (offset >> 4) & 31u;
    const uint8_t v = (uint8_t)value;
    switch (r) {
    case R_RCVFIFO: case R_XMTFIFO: case R_XMTFS: case R_XMTRC: case R_RCVFS: case R_FIFOFC: case R_IR: case R_PR:
    case R_CHIPID_LO: case R_CHIPID_HI:
        break;                  /* read-only (no programmed-I/O path) */
    case R_BIUCC:
        if (v & BIUCC_SWRST) { reset(m); break; }
        m->reg[r] = v;
        break;
    case R_FIFOCC:
        m->reg[r] = (uint8_t)(v & ~0x0Cu);       /* XMTFWU, RCVFWU: done at once */
        break;
    case R_IMR:
        m->reg[r] = v;
        update_irq(m);
        break;
    case R_IAC:
        m->reg[r] = (uint8_t)(v & 0x7Fu);
        if (v & IAC_PHYADDR) m->padr_i = 0;
        if (v & IAC_LOGADDR) m->ladrf_i = 0;
        break;
    case R_LADRF:
        m->ladrf[m->ladrf_i] = v;
        m->ladrf_i = (m->ladrf_i + 1) & 7u;
        break;
    case R_PADR:
        m->padr[m->padr_i] = v;
        m->padr_i = (m->padr_i + 1) % 6u;
        break;
    default:
        m->reg[r] = v;
        break;
    }
}

static void am_reset(void *dev) { reset(dev); }

const mc_dev_ops mc_am79c940_ops = {am_read, am_write, NULL, am_reset};

void mc_am79c940_init(mc_am79c940 *m, const mc_hw_host *host, const char *name) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_am79c940_ops;
    m->dev.host = host;
    m->dev.name = name;
    reset(m);
}

void mc_am79c940_set_port(mc_am79c940 *m, const mc_am79c940_port *port) {
    if (port) m->port = *port;
    else memset(&m->port, 0, sizeof m->port);
}
