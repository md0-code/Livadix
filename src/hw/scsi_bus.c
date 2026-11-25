#include <stdio.h>
#include <stdlib.h>
/* Livadix — the SCSI bus (scsi_bus.h). SCSI-2 (X3.131-1994) sections cited
 * per rule. */
#include "scsi_bus.h"

#include <stdlib.h>
#include <string.h>

void mc_scsi_bus_init(mc_scsi_bus *bus) {
    memset(bus, 0, sizeof *bus);
    bus->buffer = malloc(MC_SCSI_CHUNK);
    bus->phase = MC_SCSI_BUS_FREE;
    bus->initiator = bus->target = -1;
    bus->resume = -1;
}

void mc_scsi_bus_free(mc_scsi_bus *bus) {
    free(bus->buffer);
    bus->buffer = NULL;
}

void mc_scsi_bus_attach(mc_scsi_bus *bus, int id, mc_scsi_target *target) {
    if (id >= 0 && id < 8) bus->dev[id] = target;
}

static int info_phase(int phase) { return phase <= MC_SCSI_MSG_IN && phase != 4 && phase != 5; }

static void go_free(mc_scsi_bus *bus) {
    bus->phase = MC_SCSI_BUS_FREE;
    bus->initiator = bus->target = -1;
    bus->atn = bus->ack_held = 0;
    bus->resume = -1;
}

int mc_scsi_bus_arbitrate(mc_scsi_bus *bus, int id) {
    if (bus->phase != MC_SCSI_BUS_FREE || id < 0 || id > 7) return 0;
    bus->phase = MC_SCSI_ARBITRATION;
    bus->initiator = id;
    return 1;
}

int mc_scsi_bus_select(mc_scsi_bus *bus, int target, int atn) {
    if (bus->phase != MC_SCSI_ARBITRATION || target < 0 || target > 7 || target == bus->initiator || !bus->dev[target]) {
        go_free(bus);
        return 0;
    }
    bus->target = target;
    bus->atn = atn != 0;
    bus->ack_held = 0;
    bus->lun = 0;
    bus->resume = -1;
    bus->command_done = 0;
    bus->msg_out_n = 0;
    bus->cdb_n = bus->cdb_need = 0;
    bus->direction = MC_SCSI_XFER_NONE;
    bus->total = bus->done = 0;
    /* 5.1.3: a target selected with ATN goes to MESSAGE OUT (5.2.1). */
    bus->phase = atn ? MC_SCSI_MSG_OUT : MC_SCSI_COMMAND;
    return 1;
}

void mc_scsi_bus_reset(mc_scsi_bus *bus) {
    for (int i = 0; i < 8; i++)
        if (bus->dev[i] && bus->dev[i]->reset) bus->dev[i]->reset(bus->dev[i]);
    go_free(bus);
}

void mc_scsi_bus_release(mc_scsi_bus *bus) { go_free(bus); }

int mc_scsi_bus_phase(const mc_scsi_bus *bus) { return bus->phase; }

int mc_scsi_bus_req(const mc_scsi_bus *bus) {
    return bus->target >= 0 && info_phase(bus->phase) && !bus->ack_held;
}

void mc_scsi_bus_set_atn(mc_scsi_bus *bus, int atn) { bus->atn = atn != 0; }

/* The command length by group code (SCSI-2 7.2.1, Table 18 and the 53C96's
 * reading of it): groups 0, 6 six bytes; 1, 2, 7 ten; 5 twelve; 3 and 4
 * reserved, treated as six. */
static unsigned cdb_length(uint8_t op) {
    switch (op >> 5) {
    case 1: case 2: case 7: return 10;
    case 5: return 12;
    default: return 6;
    }
}

static void enter_status(mc_scsi_bus *bus) {
    mc_scsi_target *t = bus->dev[bus->target];
    if (bus->direction != MC_SCSI_XFER_NONE && t->finish) bus->status = t->finish(t);
    bus->direction = MC_SCSI_XFER_NONE;
    bus->phase = MC_SCSI_STATUS;
}

static void load_chunk(mc_scsi_bus *bus) {
    mc_scsi_target *t = bus->dev[bus->target];
    uint32_t n = bus->total - bus->done;
    if (n > MC_SCSI_CHUNK) n = MC_SCSI_CHUNK;
    bus->chunk_base = bus->done;
    bus->chunk_len = n;
    if (bus->direction == MC_SCSI_XFER_IN && t->data_in(t, bus->buffer, bus->done, n)) enter_status(bus);
}

static void execute(mc_scsi_bus *bus) {
    mc_scsi_target *t = bus->dev[bus->target];
    int direction = MC_SCSI_XFER_NONE;
    uint32_t bytes = 0;
    bus->command_done = 1;
    bus->status = t->command(t, bus->lun, bus->cdb, bus->cdb_n, &direction, &bytes);
    if (getenv("LIVADIX_SCSI_LOG")) { fprintf(stderr, "scsi %d:", bus->target); for (unsigned k = 0; k < bus->cdb_n; k++) fprintf(stderr, " %02X", bus->cdb[k]); fprintf(stderr, " -> st %02X dir %d bytes %u\n", bus->status, direction, (unsigned)bytes); }
    bus->direction = bytes ? direction : MC_SCSI_XFER_NONE;
    bus->total = bytes;
    bus->done = 0;
    if (bus->direction == MC_SCSI_XFER_NONE) {
        bus->phase = MC_SCSI_STATUS;
    } else {
        bus->phase = direction == MC_SCSI_XFER_IN ? MC_SCSI_DATA_IN : MC_SCSI_DATA_OUT;
        load_chunk(bus);
    }
}

/* After a byte: ATN makes the target go to MESSAGE OUT (5.2.1), coming back
 * to the phase it was about to enter. */
static void check_atn(mc_scsi_bus *bus) {
    if (bus->atn && bus->target >= 0 && bus->phase != MC_SCSI_MSG_OUT && info_phase(bus->phase)) {
        bus->resume = bus->phase;
        bus->phase = MC_SCSI_MSG_OUT;
    }
}

static void queue_msg_in(mc_scsi_bus *bus, uint8_t msg, int then_free) {
    bus->msg_in[0] = msg;
    bus->msg_in_n = 1;
    bus->msg_in_pos = 0;
    bus->free_after_msg = then_free;
    bus->phase = MC_SCSI_MSG_IN;
}

/* The phase after MESSAGE OUT: back where the target was, or COMMAND when
 * the CDB has not come yet. */
static void after_msg_out(mc_scsi_bus *bus) {
    int next = bus->resume >= 0 ? bus->resume : (bus->command_done ? MC_SCSI_STATUS : MC_SCSI_COMMAND);
    bus->resume = -1;
    bus->phase = next;
}

/* A complete message from the initiator (5.6). */
static void message_out(mc_scsi_bus *bus) {
    const uint8_t *m = bus->msg_out;
    mc_scsi_target *t = bus->dev[bus->target];
    bus->msg_out_n = 0;
    if (m[0] & MC_SCSI_MSG_IDENTIFY) {      /* 5.6.7: LUNTRN 0, LUN in bits 2-0 */
        bus->lun = m[0] & 7;
    } else if (m[0] == MC_SCSI_MSG_ABORT) { /* 5.6.1: clear the I/O process, go BUS FREE */
        go_free(bus);
        return;
    } else if (m[0] == MC_SCSI_MSG_DEVICE_RESET) { /* 5.6.2 */
        if (t->reset) t->reset(t);
        go_free(bus);
        return;
    } else if (m[0] == MC_SCSI_MSG_NOP || m[0] == MC_SCSI_MSG_REJECT) {
        /* nothing to do */
    } else {
        /* Anything else (extended messages such as SDTR, disconnect
         * privileges, tags) is not supported: MESSAGE REJECT (5.6.13), then
         * back to the phase the target was in. */
        if (!bus->atn) {
            int back = bus->resume;
            queue_msg_in(bus, MC_SCSI_MSG_REJECT, 0);
            bus->resume = back;
            return;
        }
    }
    if (!bus->atn) after_msg_out(bus);
}

/* Length of the message whose first bytes are in msg_out (5.6: one-byte,
 * two-byte ($20-$2F) and extended ($01, length in the second byte). */
static int message_complete(const mc_scsi_bus *bus) {
    const uint8_t *m = bus->msg_out;
    unsigned n = bus->msg_out_n;
    if (m[0] == MC_SCSI_MSG_EXTENDED) return n >= 2 && n >= 2u + (m[1] ? m[1] : 256u);
    if (m[0] >= 0x20 && m[0] <= 0x2F) return n >= 2;
    return 1;
}

void mc_scsi_bus_out(mc_scsi_bus *bus, uint8_t byte) {
    if (bus->target < 0) return;
    switch (bus->phase) {
    case MC_SCSI_MSG_OUT:
        if (bus->msg_out_n < sizeof bus->msg_out) bus->msg_out[bus->msg_out_n] = byte;
        bus->msg_out_n++;
        if (message_complete(bus)) message_out(bus);
        break;
    case MC_SCSI_COMMAND:
        if (bus->cdb_n == 0) bus->cdb_need = cdb_length(byte);
        bus->cdb[bus->cdb_n++] = byte;
        if (bus->cdb_n == bus->cdb_need) execute(bus);
        check_atn(bus);
        break;
    case MC_SCSI_DATA_OUT: {
        mc_scsi_target *t = bus->dev[bus->target];
        bus->buffer[bus->done - bus->chunk_base] = byte;
        bus->done++;
        if (bus->done == bus->chunk_base + bus->chunk_len) {
            if (t->data_out(t, bus->buffer, bus->chunk_base, bus->chunk_len) || bus->done == bus->total) enter_status(bus);
            else load_chunk(bus);
        }
        check_atn(bus);
        break;
    }
    default:
        break;  /* not an output phase: the byte goes nowhere */
    }
}

uint8_t mc_scsi_bus_peek(const mc_scsi_bus *bus) {
    if (bus->target < 0) return 0;
    switch (bus->phase) {
    case MC_SCSI_DATA_IN: return bus->buffer[bus->done - bus->chunk_base];
    case MC_SCSI_STATUS: return bus->status;
    case MC_SCSI_MSG_IN: return bus->msg_in[bus->msg_in_pos < bus->msg_in_n ? bus->msg_in_pos : 0];
    default: return 0;
    }
}

static void after_msg_in(mc_scsi_bus *bus) {
    bus->ack_held = 0;
    if (bus->msg_in_pos < bus->msg_in_n) return;
    if (bus->atn) {     /* the initiator rejects the message (5.6.13) */
        bus->phase = MC_SCSI_MSG_OUT;
        if (bus->free_after_msg) bus->resume = -1;
        return;
    }
    if (bus->free_after_msg) {  /* COMMAND COMPLETE: the target releases BSY (5.6.5) */
        go_free(bus);
        return;
    }
    after_msg_out(bus);
}

uint8_t mc_scsi_bus_in(mc_scsi_bus *bus, int hold_ack) {
    uint8_t byte = mc_scsi_bus_peek(bus);
    if (!mc_scsi_bus_req(bus)) return byte;
    switch (bus->phase) {
    case MC_SCSI_DATA_IN:
        bus->done++;
        if (bus->done == bus->total) enter_status(bus);
        else if (bus->done == bus->chunk_base + bus->chunk_len) load_chunk(bus);
        check_atn(bus);
        break;
    case MC_SCSI_STATUS:
        queue_msg_in(bus, MC_SCSI_MSG_COMMAND_COMPLETE, 1);
        check_atn(bus);
        break;
    case MC_SCSI_MSG_IN:
        bus->msg_in_pos++;
        if (hold_ack) bus->ack_held = 1;
        else after_msg_in(bus);
        break;
    default:
        break;
    }
    return byte;
}

void mc_scsi_bus_release_ack(mc_scsi_bus *bus) {
    if (bus->ack_held) after_msg_in(bus);
}
