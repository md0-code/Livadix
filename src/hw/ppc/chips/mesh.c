/* Livadix — MESH, the fast SCSI controller (mesh.h).
 *
 * Livadix's own code. */
#include "mesh.h"
#include "dbdma.h"

#include <string.h>

#define SELECT_UNIT (MC_C15M_HZ / 100u)    /* 12.5.16: 10 ms per count */

static uint8_t int_bits(const mc_mesh *m) {
    return (uint8_t)((m->error ? MC_MESH_INT_ERROR : 0) | (m->exception ? MC_MESH_INT_EXCEPTION : 0) | (m->done ? MC_MESH_INT_DONE : 0));
}

static void update(mc_mesh *m) {
    /* AppleMesh initCP: s5/s6/s7 are the unmasked, active-low
     * CmdDone/Exception/Error signals. They drive descriptor waits and
     * branches even while the processor interrupt is masked. */
    const uint8_t status = (uint8_t)(~(int_bits(m) << 5) & 0xE0u);
    if (status != m->dma_status) {
        m->dma_status = status;
        mc_dev_line(&m->dev, MC_MESH_LINE_DMA_STATUS, status);
    }
    const int irq = (int_bits(m) & m->mask) != 0;
    if (irq != m->irq) {
        m->irq = irq;
        mc_dev_line(&m->dev, MC_MESH_LINE_IRQ, irq);
    }
}

static void set_drq(mc_mesh *m, int drq) {
    if (drq != m->drq) {
        m->drq = drq;
        mc_dev_line(&m->dev, MC_MESH_LINE_DRQ, drq);
    }
}

/* The completion of a command: its bits arrive after the latency. */
static void finish(mc_mesh *m, uint8_t done, uint8_t exception, uint8_t error, uint64_t delay) {
    /* A terminal exception ends the command too (CHRP I/O 12.5.14.1).
     * AppleMesh waits on CmdDone before testing Exception/Error. */
    m->pend_done |= (uint8_t)(done || exception || error);
    m->pend_exception |= exception;
    m->pend_error |= error;
    m->pending = 1;
    if (done || exception) { m->command = 0; set_drq(m, 0); }
    mc_dev_schedule(&m->dev, mc_dev_now(&m->dev) + delay);
}

static void deliver(mc_mesh *m) {
    if (!m->pending) return;
    m->pending = 0;
    m->done |= m->pend_done;
    m->exception |= m->pend_exception;
    m->error |= m->pend_error;
    m->pend_done = m->pend_exception = m->pend_error = 0;
    update(m);
}

static void mesh_event(void *p, uint64_t now) {
    (void)now;
    deliver(p);
}

static int expected_phase(int cmd) {
    switch (cmd) {
    case MC_MESH_CMD_COMMAND: return MC_SCSI_COMMAND;
    case MC_MESH_CMD_STATUS: return MC_SCSI_STATUS;
    case MC_MESH_CMD_DATA_OUT: return MC_SCSI_DATA_OUT;
    case MC_MESH_CMD_DATA_IN: return MC_SCSI_DATA_IN;
    case MC_MESH_CMD_MSG_OUT: return MC_SCSI_MSG_OUT;
    case MC_MESH_CMD_MSG_IN: return MC_SCSI_MSG_IN;
    default: return -1;
    }
}

static int output_command(int cmd) {
    return cmd == MC_MESH_CMD_COMMAND || cmd == MC_MESH_CMD_DATA_OUT || cmd == MC_MESH_CMD_MSG_OUT;
}

static int in_phase(const mc_mesh *m) {
    return mc_scsi_bus_phase(m->bus) == expected_phase(m->command) && mc_scsi_bus_req(m->bus);
}

static void phase_mismatch(mc_mesh *m) {
    finish(m, 0, MC_MESH_EXC_PHASE, 0, m->latency);
}

/* Moves programmed-I/O bytes between the FIFO and the bus and ends the
 * command when its count and, for an output, the FIFO are exhausted
 * (12.5.1, 12.5.7). */
static void pump(mc_mesh *m) {
    if (!m->command || m->pending) return;
    if (m->sequence & MC_MESH_SEQ_DMA) return;
    if (output_command(m->command)) {
        while (m->fifo_n && in_phase(m)) {
            mc_scsi_bus_out(m->bus, m->fifo[0]);
            memmove(m->fifo, m->fifo + 1, --m->fifo_n);
        }
        if (!m->tc && !m->fifo_n) finish(m, 1, 0, 0, m->latency);
        else if (m->fifo_n && !in_phase(m)) phase_mismatch(m);
    } else {
        while (m->tc && m->fifo_n < 16 && in_phase(m)) {
            const int last = m->tc == 1 && m->command == MC_MESH_CMD_MSG_IN;
            m->fifo[m->fifo_n++] = mc_scsi_bus_in(m->bus, last);
            m->tc--;
            if (!m->tc) m->ack_held = 1;            /* Table 290: ACK stays asserted after an input */
        }
        /* An input command is done when its count is: the native SIM
         * issues Status with a count of 1 and waits for CmdDone before it
         * reads the byte from the FIFO (ROM 9630C68B's SCSI SIM)†. */
        if (!m->tc) finish(m, 1, 0, 0, m->latency);
        /* PhaseMM ends a transfer even while its final bytes remain in
         * the FIFO (CHRP I/O 12.5.8). Open Firmware waits for this exception
         * before draining an input whose count exceeds the target's reply. */
        else if (!in_phase(m)) phase_mismatch(m);
    }
}

void mc_mesh_reset(mc_mesh *m) {
    m->tc = 0;
    m->fifo_n = 0;
    m->sequence = m->exception = m->error = m->mask = 0;
    m->source = m->dest = m->sync = m->sel_timeout = m->status1_out = 0;
    m->command = 0;
    m->ack_held = 0;
    m->owner = 0;
    m->pending = 0;
    m->pend_done = m->pend_exception = m->pend_error = 0;
    m->dma_status = 0;             /* redrive pins after the DMA controller resets */
    m->done = 1;                    /* 12.5.11: CmdDone set after a reset, the mask 0 */
    if (m->dev.host) {
        mc_dev_schedule(&m->dev, MC_NEVER);
        set_drq(m, 0);
        update(m);
    }
}

void mc_mesh_init(mc_mesh *m, const mc_hw_host *host, const char *name, mc_scsi_bus *bus, uint8_t id_version) {
    memset(m, 0, sizeof *m);
    m->dev.ops = &mc_mesh_ops;
    m->dev.host = host;
    m->dev.name = name;
    m->bus = bus;
    m->id_version = id_version;
    m->latency = 32;                /* about 2 us† */
    mc_mesh_reset(m);
}

static void command(mc_mesh *m, uint8_t v) {
    deliver(m);
    const int cmd = v & 0x0F;
    if ((m->exception || m->error) && cmd != MC_MESH_CMD_RESET && cmd != MC_MESH_CMD_FLUSH) {
        m->error |= MC_MESH_ERR_SEQ;  /* 12.5.9 SeqErr: a command while an interrupt is pending */
        update(m);
        return;
    }
    m->sequence = v;
    m->done = 0;
    update(m);
    if (m->sequence & MC_MESH_SEQ_TMODE) return;      /* target mode: not modelled */
    const int atn = (v & MC_MESH_SEQ_ATN) != 0;
    if (cmd >= MC_MESH_CMD_SELECT && cmd <= MC_MESH_CMD_MSG_IN) mc_scsi_bus_set_atn(m->bus, atn);
    if (m->ack_held) {                                  /* Table 290: ACK negated by the next command */
        m->ack_held = 0;
        mc_scsi_bus_release_ack(m->bus);
    }
    switch (cmd) {
    case MC_MESH_CMD_ARBITRATE:
        m->owner = mc_scsi_bus_arbitrate(m->bus, m->source & 7);
        finish(m, m->owner ? 1 : 0, m->owner ? 0 : MC_MESH_EXC_ARBLOST, 0, m->latency);
        break;
    case MC_MESH_CMD_SELECT:
        if (mc_scsi_bus_select(m->bus, m->dest & 7, atn)) {
            finish(m, 1, 0, 0, m->latency);
        } else {
            m->owner = 0;
            finish(m, 0, MC_MESH_EXC_SELTO, 0, (uint64_t)(m->sel_timeout ? m->sel_timeout : 256u) * SELECT_UNIT);
        }
        break;
    case MC_MESH_CMD_COMMAND: case MC_MESH_CMD_STATUS: case MC_MESH_CMD_DATA_OUT:
    case MC_MESH_CMD_DATA_IN: case MC_MESH_CMD_MSG_OUT: case MC_MESH_CMD_MSG_IN:
        if (!m->tc) m->tc = 65536;                    /* 12.5.1: a count of 0 is 64 KB */
        m->command = cmd;
        if (mc_scsi_bus_phase(m->bus) != expected_phase(cmd)) {
            phase_mismatch(m);
            break;
        }
        if (v & MC_MESH_SEQ_DMA) set_drq(m, 1);
        else pump(m);
        break;
    case MC_MESH_CMD_BUS_FREE:
        if (mc_scsi_bus_phase(m->bus) == MC_SCSI_BUS_FREE) { m->owner = 0; finish(m, 1, 0, 0, m->latency); }
        else if (mc_scsi_bus_req(m->bus)) phase_mismatch(m);
        else { m->owner = 0; finish(m, 1, 0, 0, m->latency); }
        break;
    case MC_MESH_CMD_RESET:                            /* RstMESH: the controller, not the bus */
        mc_mesh_reset(m);
        break;
    case MC_MESH_CMD_FLUSH:
        m->fifo_n = 0;
        break;
    default:                                           /* parity and reselection enables: no interrupt */
        break;
    }
}

static uint32_t mesh_read(void *p, uint32_t offset, unsigned size) {
    mc_mesh *m = p;
    (void)size;
    switch (offset & 15u) {
    case MC_MESH_COUNT0: return m->tc & 0xFFu;
    case MC_MESH_COUNT1: return (m->tc >> 8) & 0xFFu;
    case MC_MESH_FIFO: {
        if (!m->fifo_n) return 0;
        const uint8_t b = m->fifo[0];
        memmove(m->fifo, m->fifo + 1, --m->fifo_n);
        pump(m);
        return b;
    }
    case MC_MESH_SEQUENCE: return m->sequence;
    case MC_MESH_STATUS0: {
        const int ph = mc_scsi_bus_phase(m->bus);
        uint8_t v = 0;
        if (ph <= MC_SCSI_MSG_IN) v |= (uint8_t)ph;
        /* While ACK is held after an input command the target cannot raise
         * REQ for the next byte (the native SIM waits for REQ to drop after
         * Status before it issues MsgIn, its RAM copy at $001D7904). */
        if (mc_scsi_bus_req(m->bus) && !m->ack_held) v |= 0x20;
        if (m->ack_held) v |= 0x10;
        if (m->bus->atn) v |= 0x08;
        return v;
    }
    case MC_MESH_STATUS1: {
        const int ph = mc_scsi_bus_phase(m->bus);
        uint8_t v = m->status1_out & 0x80u;
        if (ph != MC_SCSI_BUS_FREE) v |= 0x40;
        return v;
    }
    case MC_MESH_FIFO_COUNT: return m->fifo_n;
    case MC_MESH_EXCEPTION: return m->exception;
    case MC_MESH_ERROR: return m->error;
    case MC_MESH_INT_MASK: return m->mask;
    case MC_MESH_INTERRUPT: return int_bits(m);
    case MC_MESH_SOURCE_ID: return m->source;
    case MC_MESH_DEST_ID: return m->dest;
    case MC_MESH_SYNC: return m->sync;
    case MC_MESH_ID: return m->id_version;
    default: return m->sel_timeout;
    }
}

static void mesh_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_mesh *m = p;
    const uint8_t v = (uint8_t)value;
    (void)size;
    switch (offset & 15u) {
    case MC_MESH_COUNT0: m->tc = (m->tc & 0xFF00u) | v; break;
    case MC_MESH_COUNT1: m->tc = (m->tc & 0x00FFu) | (uint32_t)v << 8; break;
    case MC_MESH_FIFO:
        if (m->fifo_n < 16) m->fifo[m->fifo_n++] = v;
        if (m->tc) m->tc--;
        pump(m);
        break;
    case MC_MESH_SEQUENCE: command(m, v); break;
    case MC_MESH_STATUS1:
        if ((v & 0x80u) && !(m->status1_out & 0x80u)) {
            mc_scsi_bus_reset(m->bus);              /* 12.5.6: RST asserted through bus status 1 */
            m->owner = 0;
            m->command = 0;
            m->ack_held = 0;
            m->error |= MC_MESH_ERR_RESET;
            update(m);
        }
        m->status1_out = v;
        break;
    case MC_MESH_EXCEPTION: m->exception &= (uint8_t)~v; update(m); break;
    case MC_MESH_ERROR: m->error &= (uint8_t)~v; update(m); break;
    case MC_MESH_INT_MASK: m->mask = v & 7u; update(m); break;
    case MC_MESH_INTERRUPT:
        if (v & MC_MESH_INT_DONE) m->done = 0;
        if (v & MC_MESH_INT_EXCEPTION) m->exception = 0;
        if (v & MC_MESH_INT_ERROR) m->error = 0;
        update(m);
        break;
    case MC_MESH_SOURCE_ID: m->source = v; break;
    case MC_MESH_DEST_ID: m->dest = v; break;
    case MC_MESH_SYNC: m->sync = v; break;
    case MC_MESH_SEL_TIMEOUT: m->sel_timeout = v; break;
    default: break;
    }
}

int mc_mesh_dma_read(void *p, uint8_t *byte) {
    mc_mesh *m = p;
    if (!m->command || !(m->sequence & MC_MESH_SEQ_DMA) || output_command(m->command) || m->pending) return MC_DBDMA_NONE;
    if (!m->tc) return MC_DBDMA_NONE;
    if (!in_phase(m)) { phase_mismatch(m); return MC_DBDMA_END; }
    const int last = m->tc == 1 && m->command == MC_MESH_CMD_MSG_IN;
    *byte = mc_scsi_bus_in(m->bus, last);
    if (!--m->tc) {
        m->ack_held = 1;                            /* Table 290: ACK stays asserted after an input */
        finish(m, 1, 0, 0, m->latency);
    }
    return MC_DBDMA_BYTE;
}

int mc_mesh_dma_write(void *p, uint8_t byte) {
    mc_mesh *m = p;
    if (!m->command || !(m->sequence & MC_MESH_SEQ_DMA) || !output_command(m->command) || m->pending || !m->tc) return 0;
    if (!in_phase(m)) { phase_mismatch(m); return 0; }
    mc_scsi_bus_out(m->bus, byte);
    if (!--m->tc) finish(m, 1, 0, 0, m->latency);
    return 1;
}

static void mesh_reset_op(void *p) { mc_mesh_reset(p); }

const mc_dev_ops mc_mesh_ops = {mesh_read, mesh_write, mesh_event, mesh_reset_op, NULL};
