/* Livadix — NCR 5380 / 53C80 SCSI controller (ncr5380.h). Section numbers are
 * those of the NCR 5380/53C80 SCSI Interface Chip Design Manual (SP-1051,
 * March 1986). */
#include "ncr5380.h"

#include <string.h>

/* ---- derived bus state -------------------------------------------------- */

static int tmode(const mc_ncr5380 *c) { return (c->mode & MC_N80_MODE_TARGET) != 0; }

/* The Initiator Command Register as the pins see it: TEST MODE (bit 6 on
 * write, 6.2) disables every output driver except the RST latch. */
static uint8_t ic(const mc_ncr5380 *c) { return (c->icr & 0x40) ? (uint8_t)(c->icr & 0x80) : c->icr; }

static int bsy_chip(const mc_ncr5380 *c) {
    return (ic(c) & MC_N80_ICR_BSY) || (c->aip && (c->mode & MC_N80_MODE_ARBITRATE));
}
static int bsy_obs(const mc_ncr5380 *c) { return bsy_chip(c) || c->ext_bsy || c->bus->target >= 0; }
static int sel_obs(const mc_ncr5380 *c) { return (ic(c) & MC_N80_ICR_SEL) || c->ext_sel; }
/* ATN and ACK are driven by the chip only as an initiator (6.2 bits 4, 1). */
static int atn_chip(const mc_ncr5380 *c) { return (ic(c) & MC_N80_ICR_ATN) && !tmode(c); }
static int ack_chip(const mc_ncr5380 *c) { return (ic(c) & MC_N80_ICR_ACK) && !tmode(c); }

/* MSG, C/D, I/O (bits 2-0): driven by the chip from the Target Command
 * Register as a target, else by the target on the bus. */
static unsigned phase_lines(const mc_ncr5380 *c) {
    unsigned p;
    if (tmode(c)) p = c->tcr & 7;
    else p = (c->bus->target >= 0 && c->bus->phase <= MC_SCSI_MSG_IN) ? (unsigned)c->bus->phase : 0;
    if (c->ext_sel && c->ext_io) p |= 1;
    return p;
}
static int phase_match(const mc_ncr5380 *c) { return phase_lines(c) == (unsigned)(c->tcr & 7); }

static int req_obs(const mc_ncr5380 *c) {
    if (tmode(c)) return (c->tcr & MC_N80_TCR_REQ) != 0;
    return c->bus->target >= 0 && mc_scsi_bus_req(c->bus) && !c->ack_taken;
}

/* 6.2 bit 0: the Output Data Register drives the data lines as a target, or
 * as an initiator when I/O is false and the phase lines match the Target
 * Command Register. */
static int data_drive(const mc_ncr5380 *c) {
    if (!(ic(c) & MC_N80_ICR_DATA)) return 0;
    return tmode(c) || (!(phase_lines(c) & 1) && phase_match(c));
}

static int data_from_chip(const mc_ncr5380 *c) {
    return data_drive(c) || (c->aip && (c->mode & MC_N80_MODE_ARBITRATE)); /* arbitration asserts the ID (6.2 AIP) */
}

/* The byte on the data lines; *ext set when another device drives it. */
static uint8_t data_lines(const mc_ncr5380 *c, int *ext) {
    *ext = 0;
    if (data_from_chip(c)) return c->odr;
    if (c->ext_sel) { *ext = 1; return c->ext_data; }
    if (req_obs(c) && !tmode(c) && (phase_lines(c) & 1)) { *ext = 1; return mc_scsi_bus_peek(c->bus); }
    return 0;
}

static int popcount8(unsigned v) { int n = 0; for (; v; v >>= 1) n += (int)(v & 1); return n; }

/* ---- interrupts and lines ----------------------------------------------- */

static void report(mc_ncr5380 *c, int line, int state) {
    state = state != 0;
    if (c->lines[line] == state) return;
    c->lines[line] = state;
    mc_dev_line(&c->dev, line, state);
}

static void dma_stop(mc_ncr5380 *c) {
    c->dma_run = 0;
    c->sent = 0;
    c->drq = 0;
    c->eod = 0;
    c->last_byte = 0;
}

static void parity_check(mc_ncr5380 *c) {
    if (c->bad_parity && (c->mode & MC_N80_MODE_PARCHK)) {   /* 6.3 bit 5, 8.4 */
        c->parity_err = 1;
        if (c->mode & MC_N80_MODE_PARINT) c->irq = 1;
    }
}

/* Everything the chip does by itself after any change of its inputs. */
static void update(mc_ncr5380 *c) {
    mc_scsi_bus *bus = c->bus;
    /* Arbitration (6.3 bit 0, 7): starts when the bus is free and SEL is not
     * asserted; the chip then drives BSY and the ID. SEL asserted by another
     * device while AIP is set is a lost arbitration (6.2 bit 5). */
    if (c->mode & MC_N80_MODE_ARBITRATE) {
        if (!c->aip) {
            if (bus->phase == MC_SCSI_BUS_FREE && !c->ext_bsy && !c->ext_sel && c->odr) {
                int id = 7;
                while (id > 0 && !((c->odr >> id) & 1)) id--;
                if (mc_scsi_bus_arbitrate(bus, id)) { c->aip = 1; c->my_id = id; c->la = 0; }
            }
        } else if (c->ext_sel) {
            c->la = 1;
        }
    } else {
        c->aip = c->la = 0;
    }
    /* The owner of the bus that gave up arbitration and asserts neither BSY
     * nor SEL lets the bus go. */
    if (bus->phase == MC_SCSI_ARBITRATION && !(c->mode & MC_N80_MODE_ARBITRATE) && !(ic(c) & (MC_N80_ICR_BSY | MC_N80_ICR_SEL)))
        mc_scsi_bus_release(bus);
    /* Selection: SEL asserted with the ID bits on the data lines (the
     * target's and the chip's own) and BSY released (6.2 bits 3-2, 8.1). */
    if (!(ic(c) & MC_N80_ICR_SEL)) c->sel_tried = 0;
    if ((ic(c) & MC_N80_ICR_SEL) && !c->sel_tried && (ic(c) & MC_N80_ICR_DATA) && !bsy_chip(c) && c->my_id >= 0 &&
        bus->phase == MC_SCSI_ARBITRATION && bus->initiator == c->my_id) {
        unsigned m = c->odr & ~(1u << c->my_id);
        if (m) {
            int t = 7;
            while (t > 0 && !((m >> t) & 1)) t--;
            c->sel_tried = 1;
            mc_scsi_bus_select(bus, t, atn_chip(c));
        }
    }
    if (bus->target >= 0 && bus->atn != (atn_chip(c) != 0)) mc_scsi_bus_set_atn(bus, atn_chip(c));
    /* Loss of BSY (6.3 bit 2, 8.6): the lower bits of the Initiator Command
     * Register reset, the outputs go off and DMA MODE resets (6.7 bit 2). */
    int bsy = bsy_obs(c);
    int busy_cond = (c->mode & MC_N80_MODE_MONBSY) && !bsy;
    if (busy_cond && !c->busy_prev) {
        c->busy_err = 1;
        c->irq = 1;
        c->icr &= 0xE0;
        c->mode &= (uint8_t)~MC_N80_MODE_DMA;
        dma_stop(c);
        c->ack_out = c->ack_taken = 0;
        if (bus->phase == MC_SCSI_ARBITRATION && !(c->mode & MC_N80_MODE_ARBITRATE)) mc_scsi_bus_release(bus);
    }
    c->busy_prev = busy_cond;
    /* Selected or reselected by another device (8.1): SEL, the chip's ID in
     * the Select Enable Register and BSY false. */
    int sel_cond = c->ext_sel && (c->ext_data & c->ser) && !bsy;
    if (sel_cond && !c->sel_prev) {
        c->irq = 1;
        if (c->bad_parity && (c->mode & MC_N80_MODE_PARCHK)) c->parity_err = 1;   /* no parity interrupt needed */
    }
    c->sel_prev = sel_cond;
    /* REQ edge: a phase mismatch when REQ goes true in DMA mode interrupts
     * (8.5); a mismatch also keeps the chip from recognising REQ. */
    int req = req_obs(c);
    int match = phase_match(c);
    if (req && !c->req_prev && (c->mode & MC_N80_MODE_DMA) && !tmode(c) && !match) c->irq = 1;
    c->req_prev = req;
    /* DMA request (4.1, 10.2): a byte ready to take or to send. */
    if (c->dma_run && (c->mode & MC_N80_MODE_DMA) && !c->drq) {
        if (tmode(c)) {
            uint8_t b;
            if (c->dma_run == 1 && c->tgt_send) c->drq = 1;
            else if (c->dma_run == 3 && c->tgt_recv && c->tgt_recv(c->tgt_ctx, &b)) {
                c->idr = b;
                c->drq = 1;
                parity_check(c);
            }
        } else if (req && match) {
            int io = (phase_lines(c) & 1) != 0;
            if (c->dma_run == 1 && !io) {
                c->drq = 1;
            } else if (c->dma_run == 2 && io) {
                c->idr = mc_scsi_bus_peek(bus);     /* latched when REQ goes active (6.1.3) */
                c->drq = 1;
                parity_check(c);
            }
        } else if (c->dma_run == 1 && c->sent && !match) {
            /* Send: the last byte taken, the output register is empty and
             * the chip asks for the next one, which the target, now in
             * another phase, does not take (†: the Macintosh II ROMs wait
             * for DRQ or a phase match after a blind write, ROM 9779D2C4
             * at $40826B06; a real target is still in the data phase for
             * the first poll, Livadix's changes phase at once). */
            c->drq = 1;
        }
    }
    report(c, MC_NCR5380_LINE_IRQ, c->irq);
    report(c, MC_NCR5380_LINE_DRQ, c->drq);
    report(c, MC_NCR5380_LINE_READY, c->drq && (c->mode & MC_N80_MODE_BLOCK));
}

/* ---- the ACK handshake -------------------------------------------------- */

static void ack_rise(mc_ncr5380 *c) {
    mc_scsi_bus *bus = c->bus;
    if (bus->target < 0 || !mc_scsi_bus_req(bus) || c->ack_taken) return;
    int phase = bus->phase;
    if (phase & 1) {    /* DATA IN, STATUS, MSG IN: the target drives; a MSG IN byte keeps ACK held (10.1) */
        mc_scsi_bus_in(bus, phase == MC_SCSI_MSG_IN);
    } else {            /* the chip must drive the data lines (6.2 bit 0, 8.5) */
        if (!data_drive(c)) return;
        mc_scsi_bus_out(bus, c->odr);
    }
    c->ack_taken = 1;
    c->req_prev = 0;
}

static void ack_fall(mc_ncr5380 *c) {
    if (!c->ack_taken) return;
    c->ack_taken = 0;
    c->req_prev = 0;
    if (c->bus->ack_held) mc_scsi_bus_release_ack(c->bus);
}

static void sync_ack(mc_ncr5380 *c) {
    int eff = ack_chip(c) != 0;
    if (eff && !c->ack_out) { c->ack_out = 1; ack_rise(c); }
    else if (!eff && c->ack_out) { c->ack_out = 0; ack_fall(c); }
}

/* ---- reset -------------------------------------------------------------- */

/* All internal logic and control registers cleared; with keep_rst the
 * interrupt latch and ASSERT RST survive (9.2, 9.3). */
static void internal_reset(mc_ncr5380 *c, int keep_rst) {
    uint8_t rst = keep_rst ? (uint8_t)(c->icr & MC_N80_ICR_RST) : 0;
    int irq = keep_rst ? c->irq : 0;
    c->odr = c->mode = c->tcr = c->ser = c->idr = 0;
    c->icr = rst;
    c->aip = c->la = 0;
    c->eod = c->parity_err = c->busy_err = 0;
    c->irq = irq;
    c->drq = 0;
    c->last_byte = 0;
    c->dma_run = 0;
    c->ack_out = c->ack_taken = 0;
    c->req_prev = 0;
    c->sel_tried = c->sel_prev = c->busy_prev = 0;
    c->eop_pin = 0;
    c->my_id = -1;
}

void mc_ncr5380_reset(mc_ncr5380 *c) {
    internal_reset(c, 0);
    c->ext_sel = c->ext_io = c->ext_bsy = c->ext_atn = c->ext_ack = 0;
    c->ext_data = 0;
    update(c);
}

/* RST on the bus, from the chip or from outside: the targets reset, the
 * chip resets and interrupts. */
static void rst_event(mc_ncr5380 *c) {
    mc_scsi_bus_reset(c->bus);
    internal_reset(c, 1);
    c->irq = 1;
}

/* ---- registers ---------------------------------------------------------- */

static uint32_t reg_read(void *dev, uint32_t offset, unsigned size) {
    mc_ncr5380 *c = dev;
    (void)size;
    update(c);
    uint32_t v = 0;
    switch (offset & 7) {
    case MC_N80_CSD: {
        int ext;
        v = data_lines(c, &ext);
        if (ext && c->bad_parity) parity_check(c);      /* parity is checked at the start of the read (6.1.1) */
        break;
    }
    case MC_N80_ICR:
        v = (c->icr & 0x9F) | (c->aip ? MC_N80_ICR_AIP : 0) | (c->la ? MC_N80_ICR_LA : 0);
        break;
    case MC_N80_MODE: v = c->mode; break;
    case MC_N80_TCR: v = (c->tcr & 0x0F) | ((c->c80 && c->last_byte) ? MC_N80_TCR_LASTBYTE : 0); break;
    case MC_N80_CSBS: {
        int ext;
        uint8_t d = data_lines(c, &ext);
        /* This is the live DBP pin (6.5). A released bus is not a
         * driven zero byte and does not assert odd parity. */
        int dbp = (ext || data_from_chip(c)) && (popcount8(d) & 1) == 0;
        if (ext && c->bad_parity) dbp = !dbp;
        unsigned p = phase_lines(c);
        v = (dbp ? MC_N80_CSBS_DBP : 0) | (sel_obs(c) ? MC_N80_CSBS_SEL : 0) | ((p & 1) ? MC_N80_CSBS_IO : 0) |
            ((p & 2) ? MC_N80_CSBS_CD : 0) | ((p & 4) ? MC_N80_CSBS_MSG : 0) | (req_obs(c) ? MC_N80_CSBS_REQ : 0) |
            (bsy_obs(c) ? MC_N80_CSBS_BSY : 0) | ((ic(c) & MC_N80_ICR_RST) ? MC_N80_CSBS_RST : 0);
        break;
    }
    case MC_N80_BSR:
        v = ((ack_chip(c) || c->ext_ack) ? MC_N80_BSR_ACK : 0) | ((atn_chip(c) || c->ext_atn) ? MC_N80_BSR_ATN : 0) |
            (c->busy_err ? MC_N80_BSR_BUSYERR : 0) | (phase_match(c) ? MC_N80_BSR_PHASE : 0) |
            (c->irq ? MC_N80_BSR_IRQ : 0) | (c->parity_err ? MC_N80_BSR_PARITY : 0) | (c->drq ? MC_N80_BSR_DRQ : 0) |
            (c->eod ? MC_N80_BSR_EOD : 0);
        break;
    case MC_N80_IDR: v = c->idr; break;
    case MC_N80_RESET:         /* 6.9 */
        c->parity_err = c->busy_err = c->irq = 0;
        break;
    }
    update(c);
    return v;
}

static void reg_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr5380 *c = dev;
    uint8_t v = (uint8_t)value;
    (void)size;
    switch (offset & 7) {
    case MC_N80_CSD: c->odr = v; break;
    case MC_N80_ICR: {
        uint8_t old = c->icr;
        c->icr = v & 0xDF;
        if ((v & MC_N80_ICR_RST) && !(old & MC_N80_ICR_RST)) rst_event(c);
        sync_ack(c);
        break;
    }
    case MC_N80_MODE:
        if ((v & MC_N80_MODE_DMA) && !bsy_obs(c)) v &= (uint8_t)~MC_N80_MODE_DMA;   /* BSY must be active (6.3 bit 1) */
        c->mode = v;
        if (!(v & MC_N80_MODE_DMA)) dma_stop(c);
        sync_ack(c);
        break;
    case MC_N80_TCR: c->tcr = v & 0x0F; break;
    case MC_N80_CSBS: c->ser = v; break;
    case MC_N80_BSR:           /* Start DMA Send (6.8.1) */
        if (c->mode & MC_N80_MODE_DMA) { c->dma_run = 1; c->drq = 0; c->last_byte = 0; c->sent = 0; }
        break;
    case MC_N80_IDR:           /* Start DMA Target Receive (6.8.2): TARGETMODE required */
        if ((c->mode & MC_N80_MODE_DMA) && tmode(c)) { c->dma_run = 3; c->drq = 0; }
        break;
    case MC_N80_RESET:         /* Start DMA Initiator Receive (6.8.3): TARGETMODE false */
        if ((c->mode & MC_N80_MODE_DMA) && !tmode(c)) { c->dma_run = 2; c->drq = 0; }
        break;
    }
    update(c);
}

static void on_event(void *dev, uint64_t now) { (void)dev; (void)now; }
static void on_reset(void *dev) { mc_ncr5380_reset(dev); }

const mc_dev_ops mc_ncr5380_ops = {reg_read, reg_write, on_event, on_reset, NULL};

void mc_ncr5380_init(mc_ncr5380 *c, const mc_hw_host *host, mc_scsi_bus *bus, int c80) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_ncr5380_ops;
    c->dev.host = host;
    c->dev.name = c80 ? "ncr53c80" : "ncr5380";
    c->bus = bus;
    c->c80 = c80 != 0;
    for (int i = 0; i < 3; i++) c->lines[i] = -1;
    mc_ncr5380_reset(c);
}

/* ---- DACK cycles -------------------------------------------------------- */

static void end_of_cycle(mc_ncr5380 *c, int sent) {
    c->req_prev = 0;
    if (c->eop_pin) {           /* EOP with DACK and IOR/IOW (8.2, 10.5.1) */
        c->eod = 1;
        if (c->mode & MC_N80_MODE_EOPINT) c->irq = 1;
        c->dma_run = 0;
        if (sent) c->last_byte = 1;
    }
    update(c);
}

static int dack_read(mc_ncr5380 *c, uint8_t *b) {
    update(c);
    if (!c->drq || !(c->mode & MC_N80_MODE_DMA) || (c->dma_run != 2 && c->dma_run != 3)) return 0;
    *b = c->idr;
    c->drq = 0;
    if (c->dma_run == 2 && c->bus->target >= 0 && mc_scsi_bus_req(c->bus)) mc_scsi_bus_in(c->bus, 0);  /* the chip's ACK cycle */
    end_of_cycle(c, 0);
    return 1;
}

static int dack_write(mc_ncr5380 *c, uint8_t b) {
    update(c);
    if (!c->drq || !(c->mode & MC_N80_MODE_DMA) || c->dma_run != 1) return 0;
    c->odr = b;
    c->drq = 0;
    if (tmode(c)) {
        if (c->tgt_send) c->tgt_send(c->tgt_ctx, b);
    } else if (c->bus->target >= 0 && mc_scsi_bus_req(c->bus) && phase_match(c)) {
        mc_scsi_bus_out(c->bus, data_drive(c) ? b : 0);
        c->sent = 1;
    } else {
        c->sent = 0;            /* held in the output register: no further DRQ */
    }
    end_of_cycle(c, 1);
    return 1;
}

unsigned mc_ncr5380_dma_read(mc_ncr5380 *c, unsigned size, uint32_t *value) {
    uint32_t v = 0;
    unsigned n = 0;
    for (; n < size; n++) {
        uint8_t b;
        if (!dack_read(c, &b)) break;
        v = v << 8 | b;
    }
    *value = v;
    return n;
}

unsigned mc_ncr5380_dma_write(mc_ncr5380 *c, unsigned size, uint32_t value) {
    unsigned n = 0;
    for (; n < size; n++)
        if (!dack_write(c, (uint8_t)(value >> (8 * (size - 1 - n))))) break;
    return n;
}

void mc_ncr5380_set_eop(mc_ncr5380 *c, int state) { c->eop_pin = state != 0; }

/* ---- the other bus devices ---------------------------------------------- */

void mc_ncr5380_ext_select(mc_ncr5380 *c, uint8_t data, int reselect) {
    c->ext_sel = 1;
    c->ext_data = data;
    c->ext_io = reselect != 0;
    update(c);
}

void mc_ncr5380_ext_release(mc_ncr5380 *c) {
    c->ext_sel = c->ext_io = 0;
    c->ext_data = 0;
    update(c);
}

void mc_ncr5380_ext_bsy(mc_ncr5380 *c, int state) {
    c->ext_bsy = state != 0;
    update(c);
}

void mc_ncr5380_ext_signals(mc_ncr5380 *c, int atn, int ack) {
    c->ext_atn = atn != 0;
    c->ext_ack = ack != 0;
    update(c);
}

void mc_ncr5380_ext_reset(mc_ncr5380 *c) {
    rst_event(c);
    update(c);
}
