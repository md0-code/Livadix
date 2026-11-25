/* Livadix — NCR 53C96 SCSI controller (ncr53c96.h). Section and table names are
 * those of the NCR 53C94/53C95/53C96 data sheet (February 1990). */
#include "ncr53c96.h"

#include <string.h>

/* Status register (Figure 8). */
#define ST_INT 0x80
#define ST_GE 0x40
#define ST_PE 0x20
#define ST_TC 0x10
#define ST_VGC 0x08
/* Interrupt register (Figure 9). */
#define IN_RESET 0x80
#define IN_ILLEGAL 0x40
#define IN_DISCONNECT 0x20
#define IN_BUS_SERVICE 0x10
#define IN_FUNCTION_COMPLETE 0x08

enum { DISCONNECTED, INITIATOR };
enum { DMA_NONE, DMA_TO_SCSI, DMA_FROM_SCSI };

/* Interrupt latency default: a few microseconds (Livadix's choice; the data
 * sheet gives 1.5 to 3.5 CLK cycles for the disconnect interrupt and nothing
 * for the others). */
#define DEFAULT_LATENCY 64u

static void set_line(mc_ncr53c96 *c, int line, int *state, int value) {
    if (*state != value) {
        *state = value;
        mc_dev_line(&c->dev, line, value);
    }
}

static int phase(const mc_ncr53c96 *c) { return mc_scsi_bus_phase(c->bus); }
static int connected(const mc_ncr53c96 *c) { return c->bus->target >= 0 && phase(c) <= MC_SCSI_MSG_IN; }
static uint32_t remaining(const mc_ncr53c96 *c) { return c->tc; }

static void update_lines(mc_ncr53c96 *c) {
    int drq = 0;
    if (c->dma == DMA_TO_SCSI) drq = c->tc > 0 && c->fifo_n < 16;
    else if (c->dma == DMA_FROM_SCSI || c->drain) drq = c->fifo_n > 0;
    if (!c->fifo_n) c->drain = 0;
    set_line(c, MC_NCR53C96_LINE_IRQ, &c->irq, (c->status & ST_INT) != 0);
    set_line(c, MC_NCR53C96_LINE_DRQ, &c->drq, drq);
}

static void push(mc_ncr53c96 *c, uint8_t b) {
    if (c->fifo_n == 16) {      /* the top of the FIFO is overwritten: gross error (Figure 8, bit 6) */
        c->fifo[15] = b;
        c->status |= ST_GE;
        return;
    }
    c->fifo[c->fifo_n++] = b;
}

static uint8_t pop(mc_ncr53c96 *c) {
    if (!c->fifo_n) return c->fifo[0];   /* empty: reads see the bottom register (FIFO Register) */
    uint8_t b = c->fifo[0];
    memmove(c->fifo, c->fifo + 1, --c->fifo_n);
    if (!c->fifo_n) c->fifo[0] = b;
    return b;
}

static void count_down(mc_ncr53c96 *c) {
    if (c->tc && --c->tc == 0) c->status |= ST_TC;
}

/* Presents an interrupt, or stacks it behind the one presented (Command
 * Register: "the second interrupt is stacked behind the first"). */
static void raise(mc_ncr53c96 *c, uint8_t intr, uint8_t seq) {
    if (c->status & ST_INT) {
        if (c->stacked < 2) {
            c->stack_intr[c->stacked] = intr;
            c->stack_seq[c->stacked] = seq;
            c->stack_status[c->stacked] = (uint8_t)(phase(c) & 7);
            c->stacked++;
        }
        return;
    }
    c->intr = intr;
    c->seq = seq;
    c->status |= ST_INT;
    c->phase_latched = (uint8_t)(phase(c) & 7);
}

static void start_next(mc_ncr53c96 *c);

/* The command finished on the bus; its interrupt comes after `delay`. */
static void complete_after(mc_ncr53c96 *c, uint8_t intr, uint8_t seq, uint64_t delay) {
    c->dma = DMA_NONE;
    c->pending = 1;
    c->pend_intr = intr;
    c->pend_seq = seq;
    mc_dev_schedule(&c->dev, mc_dev_now(&c->dev) + delay);
    update_lines(c);
}

static void complete(mc_ncr53c96 *c, uint8_t intr, uint8_t seq) { complete_after(c, intr, seq, c->latency); }

/* A command that ends without an interrupt. */
static void quiet(mc_ncr53c96 *c) {
    c->busy = 0;
    c->dma = DMA_NONE;
    update_lines(c);
}

/* The bus after a transfer: the target went away (disconnect interrupt) or
 * requests the next phase (bus service). */
static void end_transfer(mc_ncr53c96 *c) {
    if (!connected(c)) {
        c->state = DISCONNECTED;
        complete(c, IN_DISCONNECT, 0);
    } else {
        complete(c, IN_BUS_SERVICE, 0);
    }
}

/* Selection time-out (Time-Out register): RV x 8192 x CCF CLK periods. */
static uint64_t timeout_periods(const mc_ncr53c96 *c) {
    uint64_t rv = c->timeout ? c->timeout : 256;
    uint64_t ccf = (c->ccf & 7) ? (c->ccf & 7) : 8;
    return rv * 8192u * ccf * MC_C15M_HZ / (c->clock_hz ? c->clock_hz : 25000000u);
}

/* Sends FIFO bytes while the target stays in `ph`; in MESSAGE OUT, ATN is
 * removed before the last byte (Transfer Information). `last_releases`:
 * the FIFO's last byte is the message's last. */
static void send_fifo(mc_ncr53c96 *c, int ph, int last_releases) {
    while (c->fifo_n && phase(c) == ph && mc_scsi_bus_req(c->bus)) {
        if (ph == MC_SCSI_MSG_OUT && c->fifo_n == 1 && last_releases) mc_scsi_bus_set_atn(c->bus, 0);
        mc_scsi_bus_out(c->bus, pop(c));
    }
}

/* Tables 8-11: the selection sequences. */
static void select_now(mc_ncr53c96 *c, uint8_t code) {
    int atn = code != 0x41;
    if (!mc_scsi_bus_arbitrate(c->bus, c->config1 & 7) || !mc_scsi_bus_select(c->bus, c->dest & 7, atn)) {
        c->state = DISCONNECTED;
        complete_after(c, IN_DISCONNECT, 0, timeout_periods(c));
        return;
    }
    c->state = INITIATOR;
    uint8_t seq = 0;
    if (atn) {
        unsigned messages = code == 0x46 ? 3 : 1;
        if (phase(c) != MC_SCSI_MSG_OUT) {          /* target did not assert message out */
            complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, 0);
            return;
        }
        for (unsigned i = 0; i < messages && phase(c) == MC_SCSI_MSG_OUT && c->fifo_n; i++) {
            if (code != 0x43 && i + 1 == messages) mc_scsi_bus_set_atn(c->bus, 0);
            mc_scsi_bus_out(c->bus, pop(c));
        }
        if (code == 0x43) {                         /* with ATN and stop: ATN stays asserted */
            complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, 1);
            return;
        }
    }
    seq = 2;
    if (phase(c) != MC_SCSI_COMMAND) {              /* target did not assert command phase */
        if (!connected(c)) {
            c->state = DISCONNECTED;
            complete(c, IN_DISCONNECT, seq);
        } else {
            complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, seq);
        }
        return;
    }
    send_fifo(c, MC_SCSI_COMMAND, 0);
    /* Complete when the target left command phase with the FIFO empty;
     * stopped during command transfer otherwise. */
    seq = (phase(c) != MC_SCSI_COMMAND && !c->fifo_n) ? 4 : 3;
    if (!connected(c)) {
        c->state = DISCONNECTED;
        complete(c, IN_DISCONNECT, seq);
        return;
    }
    complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, seq);
}

/* A DMA selection (Select ... with the DMA bit): the chip arbitrates and
 * selects at once and takes the message and command bytes by DMA as the
 * target asks for them (data sheet, "DMA Interface": the counter holds the
 * message and command bytes). The Quadra 700 ROM relies on it: it starts a
 * DMA selection with a count of 1 and waits for a nonzero sequence step and
 * the command phase before it moves any byte (ROM offset $98DC8-$98E3E).
 * While the bytes flow the sequence step reads the selection's progress: 2
 * selected, 3 in the command phase. */
static void select_dma_progress(mc_ncr53c96 *c) {
    const uint8_t code = (uint8_t)c->dma_select;
    const unsigned messages = code == 0x46 ? 3 : 1;
    while (c->fifo_n && connected(c) && mc_scsi_bus_req(c->bus)) {
        const int ph = phase(c);
        if (ph == MC_SCSI_MSG_OUT) {
            c->sel_msgs++;
            if (code != 0x43 && c->sel_msgs == messages) mc_scsi_bus_set_atn(c->bus, 0);
            mc_scsi_bus_out(c->bus, pop(c));
            if (code == 0x43) {                     /* with ATN and stop: ATN stays asserted */
                c->dma_select = 0;
                complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, 1);
                return;
            }
        } else if (ph == MC_SCSI_COMMAND) {
            mc_scsi_bus_out(c->bus, pop(c));
            c->seq = 3;
        } else break;
    }
    const int ph = phase(c);
    if (!connected(c)) {
        c->dma_select = 0;
        c->state = DISCONNECTED;
        complete(c, IN_DISCONNECT, c->seq);
    } else if (ph != MC_SCSI_MSG_OUT && ph != MC_SCSI_COMMAND) {
        c->dma_select = 0;
        complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, (uint8_t)(c->fifo_n ? 3 : c->seq >= 3 ? 4 : 2));
    } else if (c->tc == 0 && !c->fifo_n) {         /* the count ran out with the target still asking */
        c->dma_select = 0;
        complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, (uint8_t)(c->seq >= 3 ? 3 : 2));
    }
}

static void select_dma(mc_ncr53c96 *c, uint8_t code) {
    const int atn = code != 0x41;
    c->seq = 0;
    if (!mc_scsi_bus_arbitrate(c->bus, c->config1 & 7) || !mc_scsi_bus_select(c->bus, c->dest & 7, atn)) {
        c->state = DISCONNECTED;
        complete_after(c, IN_DISCONNECT, 0, timeout_periods(c));
        return;
    }
    c->state = INITIATOR;
    c->seq = 2;
    c->sel_msgs = 0;
    c->dma_select = code;
    c->dma = DMA_TO_SCSI;
    c->dma_phase = -1;
    if (atn && phase(c) != MC_SCSI_MSG_OUT) {       /* target did not assert message out */
        c->dma_select = 0;
        complete(c, IN_BUS_SERVICE | IN_FUNCTION_COMPLETE, 0);
        return;
    }
    select_dma_progress(c);                         /* bytes already in the FIFO go first */
    update_lines(c);
}

/* DMA from the SCSI bus: bytes come off the bus into the FIFO while it has
 * room and the counter asks for more; the counter counts the bytes taken
 * from the bus (data sheet, "Transfer Counter": it decrements with each
 * byte transferred on the SCSI bus), so it can end with the bytes still in
 * the FIFO for the DACK reads (the Quadra 700 ROM waits for the counter's
 * status bit before it reads each group of 16 bytes, ROM offset $992D2).
 * Message in and status bytes come one at a time; the last byte of a
 * message in leaves ACK asserted (Initiator Commands). */
static void fill(mc_ncr53c96 *c, int ph) {
    while (c->fifo_n < 16 && remaining(c) && phase(c) == ph && mc_scsi_bus_req(c->bus)) {
        if ((ph == MC_SCSI_MSG_IN || ph == MC_SCSI_STATUS) && c->fifo_n) break;
        int hold = ph == MC_SCSI_MSG_IN && remaining(c) == 1;
        push(c, mc_scsi_bus_in(c->bus, hold));
        count_down(c);
    }
}

static void dma_in_check(mc_ncr53c96 *c, int ph) {
    if (c->dma != DMA_FROM_SCSI) return;
    if (c->tc == 0) {
        complete(c, ph == MC_SCSI_MSG_IN ? IN_FUNCTION_COMPLETE : IN_BUS_SERVICE, 0);
        c->drain = 1;               /* DREQ stays while the FIFO holds bytes */
        update_lines(c);
    } else if (!c->fifo_n && (phase(c) != ph || !mc_scsi_bus_req(c->bus))) {
        end_transfer(c);
    }
}

static void transfer_information(mc_ncr53c96 *c, int dma, int pad) {
    if (!connected(c)) {
        c->state = DISCONNECTED;
        complete(c, IN_DISCONNECT, 0);
        return;
    }
    int ph = phase(c);
    int out = ph == MC_SCSI_DATA_OUT || ph == MC_SCSI_COMMAND || ph == MC_SCSI_MSG_OUT;
    if (pad) {      /* Transfer Pad: null bytes out, or bytes in discarded, until the counter ends */
        while (c->tc && phase(c) == ph && mc_scsi_bus_req(c->bus)) {
            if (out) {
                if (ph == MC_SCSI_MSG_OUT && c->tc == 1) mc_scsi_bus_set_atn(c->bus, 0);
                mc_scsi_bus_out(c->bus, 0);
            } else {
                mc_scsi_bus_in(c->bus, 0);
            }
            count_down(c);
        }
        end_transfer(c);
        return;
    }
    if (out) {
        if (!dma) {
            send_fifo(c, ph, 1);
            end_transfer(c);
            return;
        }
        c->dma = DMA_TO_SCSI;
        c->dma_phase = ph;
        send_fifo(c, ph, 0);
        update_lines(c);
        return;
    }
    if (!dma) {     /* non-DMA input: one byte, then an interrupt */
        if (!mc_scsi_bus_req(c->bus)) { end_transfer(c); return; }
        int msg = ph == MC_SCSI_MSG_IN;
        push(c, mc_scsi_bus_in(c->bus, msg));
        if (msg) complete(c, IN_FUNCTION_COMPLETE, 0);
        else end_transfer(c);
        return;
    }
    c->dma = DMA_FROM_SCSI;
    c->dma_phase = ph;
    fill(c, ph);
    update_lines(c);
    dma_in_check(c, ph);
}

/* Initiator command complete sequence: status byte and message byte into
 * the FIFO, ACK held on the message. */
static void command_complete_steps(mc_ncr53c96 *c) {
    if (!connected(c) || phase(c) != MC_SCSI_STATUS) { end_transfer(c); return; }
    push(c, mc_scsi_bus_in(c->bus, 0));
    if (!connected(c) || phase(c) != MC_SCSI_MSG_IN) { end_transfer(c); return; }
    push(c, mc_scsi_bus_in(c->bus, 1));
    complete(c, IN_FUNCTION_COMPLETE, 0);
}

static void illegal(mc_ncr53c96 *c) {
    c->command = 0;
    complete(c, IN_ILLEGAL, 0);
}

static void execute(mc_ncr53c96 *c, uint8_t cmd) {
    int dma = (cmd & 0x80) != 0;
    uint8_t code = cmd & 0x7F;
    if (dma) {      /* any DMA command loads the counter from the count register; zero is 65536 */
        c->tc = c->tc_reg ? c->tc_reg : 65536u;
        c->status &= (uint8_t)~ST_TC;
    }
    if (code >= 0x10 && code <= 0x1B && c->state != INITIATOR) { illegal(c); return; }
    if (code >= 0x40 && code <= 0x46 && c->state != DISCONNECTED) { illegal(c); return; }
    switch (code) {
    case 0x00: quiet(c); break;                         /* NOP */
    case 0x01: c->fifo_n = 0; c->fifo[0] = 0; quiet(c); break;  /* flush FIFO */
    case 0x10: transfer_information(c, dma, 0); break;
    case 0x11: command_complete_steps(c); break;
    case 0x12:                                          /* message accepted: release ACK */
        mc_scsi_bus_release_ack(c->bus);
        end_transfer(c);
        break;
    case 0x18: transfer_information(c, 1, 1); break;    /* transfer pad (uses the counter) */
    case 0x1A: mc_scsi_bus_set_atn(c->bus, 1); quiet(c); break;
    case 0x1B: mc_scsi_bus_set_atn(c->bus, 0); quiet(c); break;
    case 0x40:      /* reselect: no initiator on the bus answers a Macintosh's reselection */
        if (mc_scsi_bus_arbitrate(c->bus, c->config1 & 7)) mc_scsi_bus_release(c->bus);
        complete_after(c, IN_DISCONNECT, 0, timeout_periods(c));
        break;
    case 0x41: case 0x42: case 0x43: case 0x46:
        if (dma) select_dma(c, code);
        else select_now(c, code);
        break;
    case 0x44: quiet(c); break;                         /* enable selection/reselection */
    case 0x45: complete(c, IN_FUNCTION_COMPLETE, 0); break; /* disable selection/reselection */
    default: illegal(c); break;                         /* target commands and unused codes */
    }
}

static void start_next(mc_ncr53c96 *c) {
    while (!c->busy && c->cmd_n) {
        uint8_t cmd = c->cmd_queue[0];
        c->cmd_queue[0] = c->cmd_queue[1];
        c->cmd_n--;
        c->busy = 1;
        execute(c, cmd);
    }
}

/* Reset chip / hardware reset (Reset Chip; reset values from each register's
 * description): FIFO, command register, status, interrupt and sequence step
 * cleared; config 1 cleared except the bus ID; config 2 and 3 cleared; clock
 * conversion 2; synchronous period 5, offset 0; the count, destination and
 * time-out registers keep their values. */
void mc_ncr53c96_reset(mc_ncr53c96 *c) {
    if (c->state == INITIATOR && c->bus) mc_scsi_bus_release(c->bus);
    c->fifo_n = 0;
    c->fifo[0] = 0;
    c->cmd_n = 0;
    c->command = 0;
    c->status = c->intr = c->seq = 0;
    c->stacked = 0;
    c->config1 &= 7;
    c->config2 = c->config3 = 0;
    c->ccf = 2;
    c->sync_period = 5;
    c->sync_offset = 0;
    c->test = 0;
    c->state = DISCONNECTED;
    c->busy = 0;
    c->dma = DMA_NONE;
    c->dma_select = 0;
    c->drain = 0;
    c->pending = 0;
    mc_dev_schedule(&c->dev, MC_NEVER);
    update_lines(c);
}

/* Reset SCSI bus: the reset condition on the bus; the chip sees it on RSTI
 * and interrupts unless config 1 bit 6 disables the report. RST lasts
 * 130 CLK periods x CCF (T2). */
static void reset_bus(mc_ncr53c96 *c) {
    mc_scsi_bus_reset(c->bus);
    c->state = DISCONNECTED;
    c->busy = 0;
    c->dma = DMA_NONE;
    c->cmd_n = 0;
    uint64_t ccf = (c->ccf & 7) ? (c->ccf & 7) : 8;
    uint64_t t2 = 130u * ccf * MC_C15M_HZ / (c->clock_hz ? c->clock_hz : 25000000u) + 1;
    if (!(c->config1 & 0x40)) {
        c->busy = 1;
        complete_after(c, IN_RESET, 0, t2);
    } else {
        update_lines(c);
    }
}

static uint8_t read_reg(mc_ncr53c96 *c, unsigned reg) {
    switch (reg) {
    case MC_NCR_TC_LO: return (uint8_t)c->tc;
    case MC_NCR_TC_HI: return (uint8_t)(c->tc >> 8);
    case MC_NCR_FIFO: {
        uint8_t b = pop(c);
        if (c->dma == DMA_FROM_SCSI) {
            /* Read by the CPU, not by DACK cycles (the Quadra 700 ROM's SCSI
             * Manager empties the FIFO so when a target ends its data before
             * the counter does, a READ TOC's 20 bytes of 32): the transfer
             * ends as after a DACK cycle, the counter at zero or the target
             * in its next phase. */
            fill(c, c->dma_phase);
            update_lines(c);
            dma_in_check(c, c->dma_phase);
        }
        return b;
    }
    case MC_NCR_COMMAND: return c->command;
    case MC_NCR_STATUS: {
        uint8_t ph = (c->config2 & 0x40) && (c->status & ST_INT) ? c->phase_latched : (uint8_t)(connected(c) ? phase(c) & 7 : 0);
        return (uint8_t)((c->status & 0xF8) | ph);
    }
    case MC_NCR_INTR: {
        uint8_t v = c->intr;
        if (c->status & ST_INT) {   /* reading clears status (but TC), interrupt and sequence step */
            c->status &= ST_TC;
            c->intr = 0;
            c->seq = 0;
            if (c->stacked) {
                uint8_t intr = c->stack_intr[0], seq = c->stack_seq[0], ph = c->stack_status[0];
                c->stack_intr[0] = c->stack_intr[1];
                c->stack_seq[0] = c->stack_seq[1];
                c->stack_status[0] = c->stack_status[1];
                c->stacked--;
                c->intr = intr;
                c->seq = seq;
                c->phase_latched = ph;
                c->status |= ST_INT;
            }
            update_lines(c);
        }
        return v;
    }
    /* Sequence step: bits 2-0; bit 3 (synchronous offset maximum, active
     * low) reads 1: transfers are asynchronous†. */
    case MC_NCR_SEQ: return (uint8_t)(0x08 | (c->seq & 7));
    case MC_NCR_FLAGS: return (uint8_t)((c->seq & 7) << 5 | (c->fifo_n & 0x1F));
    case MC_NCR_CONFIG1: return c->config1;
    case MC_NCR_CONFIG2: return c->config2;
    case MC_NCR_CONFIG3: return c->config3;
    default: return 0;      /* NCR reserved */
    }
}

static void write_reg(mc_ncr53c96 *c, unsigned reg, uint8_t v) {
    switch (reg) {
    case MC_NCR_TC_LO: c->tc_reg = (uint16_t)((c->tc_reg & 0xFF00) | v); break;
    case MC_NCR_TC_HI: c->tc_reg = (uint16_t)((c->tc_reg & 0x00FF) | v << 8); break;
    case MC_NCR_FIFO:
        push(c, v);
        if (c->dma_select) select_dma_progress(c);
        else if (c->dma == DMA_TO_SCSI && c->dma_phase >= 0) send_fifo(c, c->dma_phase, 0);
        break;
    case MC_NCR_COMMAND: {
        uint8_t code = v & 0x7F;
        c->command = v;
        if (code == 0x02) { mc_ncr53c96_reset(c); c->command = v; break; }  /* executes at once */
        if (code == 0x03) { reset_bus(c); break; }
        if (c->cmd_n == 2) {    /* the top of the command register overwritten: gross error */
            c->status |= ST_GE;
            c->cmd_queue[1] = v;
        } else {
            c->cmd_queue[c->cmd_n++] = v;
        }
        start_next(c);
        break;
    }
    case MC_NCR_STATUS: c->dest = v; break;
    case MC_NCR_INTR: c->timeout = v; break;
    case MC_NCR_SEQ: c->sync_period = v & 0x1F; break;
    case MC_NCR_FLAGS: c->sync_offset = v & 0x0F; break;
    case MC_NCR_CONFIG1: c->config1 = v; break;
    case MC_NCR_CLOCK: c->ccf = v & 7; break;
    case MC_NCR_TEST: c->test = v; break;
    case MC_NCR_CONFIG2: c->config2 = v; break;
    case MC_NCR_CONFIG3: c->config3 = v; break;
    default: break;
    }
}

static uint32_t ncr_read(void *dev, uint32_t offset, unsigned size) {
    (void)size;
    return read_reg(dev, offset & 15);
}

static void ncr_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)size;
    write_reg(dev, offset & 15, (uint8_t)value);
}

static void ncr_event(void *dev, uint64_t now) {
    mc_ncr53c96 *c = dev;
    (void)now;
    if (!c->pending) return;
    c->pending = 0;
    raise(c, c->pend_intr, c->pend_seq);
    c->busy = 0;
    update_lines(c);
    start_next(c);
}

static void ncr_reset_op(void *dev) { mc_ncr53c96_reset(dev); }

/* Registers whose read changes nothing (all but the FIFO and the interrupt
 * register): the chip's state moves only at its own events (a command's
 * completion), at writes and at DMA cycles, so the value holds until then. */
static int ncr_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)size;
    const unsigned reg = offset & 15u;
    return reg != MC_NCR_FIFO && reg != MC_NCR_INTR;
}

const mc_dev_ops mc_ncr53c96_ops = {ncr_read, ncr_write, ncr_event, ncr_reset_op, ncr_stable};

void mc_ncr53c96_init(mc_ncr53c96 *c, const mc_hw_host *host, mc_scsi_bus *bus, uint32_t clock_hz) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_ncr53c96_ops;
    c->dev.host = host;
    c->dev.name = "53C96";
    c->bus = bus;
    c->clock_hz = clock_hz;
    c->latency = DEFAULT_LATENCY;
    c->config1 = 7;     /* the bus ID is unpredictable after power-up; the ROM sets it */
    mc_ncr53c96_reset(c);
}

/* One DACK cycle reading from the chip. */
static int dma_read_byte(mc_ncr53c96 *c, uint8_t *b) {
    if (c->dma != DMA_FROM_SCSI && !c->drain) return 0;
    if (!c->fifo_n && c->dma == DMA_FROM_SCSI) fill(c, c->dma_phase);
    if (!c->fifo_n) return 0;
    *b = pop(c);
    if (c->dma == DMA_FROM_SCSI) {
        fill(c, c->dma_phase);
        dma_in_check(c, c->dma_phase);
    }
    return 1;
}

/* One DACK cycle writing into the chip. */
static int dma_write_byte(mc_ncr53c96 *c, uint8_t b) {
    if (c->dma != DMA_TO_SCSI || c->tc == 0) return 0;
    push(c, b);
    count_down(c);
    if (c->dma_select) {        /* a DMA selection's message and command bytes */
        select_dma_progress(c);
        return 1;
    }
    int ph = c->dma_phase;
    while (c->fifo_n && phase(c) == ph && mc_scsi_bus_req(c->bus)) {
        if (ph == MC_SCSI_MSG_OUT && c->tc == 0 && c->fifo_n == 1) mc_scsi_bus_set_atn(c->bus, 0);
        mc_scsi_bus_out(c->bus, pop(c));
    }
    if (phase(c) != ph || !connected(c)) end_transfer(c);
    else if (c->tc == 0 && !c->fifo_n) complete(c, IN_BUS_SERVICE, 0);
    return 1;
}

unsigned mc_ncr53c96_dma_read(mc_ncr53c96 *c, unsigned size, uint32_t *value) {
    uint32_t v = 0;
    unsigned n = 0;
    for (; n < size; n++) {
        uint8_t b;
        if (!dma_read_byte(c, &b)) break;
        v = v << 8 | b;
    }
    if (n < size) v <<= 8 * (size - n);
    *value = v;
    update_lines(c);
    return n;
}

unsigned mc_ncr53c96_dma_write(mc_ncr53c96 *c, unsigned size, uint32_t value) {
    unsigned n = 0;
    for (; n < size; n++)
        if (!dma_write_byte(c, (uint8_t)(value >> 8 * (size - 1 - n)))) break;
    update_lines(c);
    return n;
}
