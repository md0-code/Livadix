/* Livadix — the SCSI bus.
 *
 * A byte-level model of one parallel SCSI bus with up to eight IDs, written
 * from the SCSI-2 standard (X3.131-1994): bus phases (5.1, Table 5: MSG, C/D
 * and I/O encode the information transfer phases), arbitration (5.1.2: the
 * highest ID wins), selection with or without ATN (5.1.3), the information
 * transfer phases and the REQ/ACK handshake (5.1.5-5.1.9), the attention
 * condition (5.2.1) and the reset condition (5.2.2), and the message system
 * of 5.6 as far as a target without disconnection needs it (IDENTIFY,
 * COMMAND COMPLETE, ABORT, BUS DEVICE RESET, MESSAGE REJECT, NO OPERATION).
 *
 * Initiator controllers (53C96, later the 5380) drive the bus through the
 * functions below; targets (scsi_disk.c, scsi_cd.c) implement
 * mc_scsi_target. Targets answer at once: a byte is always ready when the
 * phase is an information phase, so REQ is asserted whenever a target is
 * connected. The bus has no notion of time; controllers apply their own
 * timing (selection time-out, interrupt latency) in guest time.
 *
 * Phase values are the SCSI-2 MSG/C-D/I-O encoding (bit 2 MSG, bit 1 C/D,
 * bit 0 I/O), the same three bits the 53C96 status register reports. */
#ifndef MC_SCSI_BUS_H
#define MC_SCSI_BUS_H

#include <stdint.h>

enum {
    MC_SCSI_DATA_OUT = 0,
    MC_SCSI_DATA_IN = 1,
    MC_SCSI_COMMAND = 2,
    MC_SCSI_STATUS = 3,
    MC_SCSI_MSG_OUT = 6,
    MC_SCSI_MSG_IN = 7,
    MC_SCSI_BUS_FREE = 8,       /* not an information phase: BSY false */
    MC_SCSI_ARBITRATION = 9     /* an initiator owns the bus, nobody selected yet */
};

/* Status bytes (SCSI-2 7.3, Table 27). */
enum { MC_SCSI_GOOD = 0x00, MC_SCSI_CHECK_CONDITION = 0x02, MC_SCSI_BUSY = 0x08 };
/* Messages (SCSI-2 5.6, Table 10). */
enum {
    MC_SCSI_MSG_COMMAND_COMPLETE = 0x00, MC_SCSI_MSG_EXTENDED = 0x01, MC_SCSI_MSG_ABORT = 0x06,
    MC_SCSI_MSG_REJECT = 0x07, MC_SCSI_MSG_NOP = 0x08, MC_SCSI_MSG_DEVICE_RESET = 0x0C,
    MC_SCSI_MSG_IDENTIFY = 0x80
};

/* The data phase a command asks for. */
enum { MC_SCSI_XFER_NONE, MC_SCSI_XFER_IN, MC_SCSI_XFER_OUT };

typedef struct mc_scsi_target mc_scsi_target;
struct mc_scsi_target {
    /* A complete CDB arrived for logical unit `lun`. The target sets
     * *direction and *length (bytes) of the data phase it wants and returns
     * the status byte to send if there is no data phase (or after it). */
    uint8_t (*command)(mc_scsi_target *t, unsigned lun, const uint8_t *cdb, unsigned length,
                       int *direction, uint32_t *bytes);
    /* Data in: fill `count` bytes at byte `offset` of the transfer. Data out:
     * take them. Calls arrive in order, in chunks of at most
     * MC_SCSI_CHUNK bytes. A nonzero return ends the data phase early; the
     * status the target returns from `finish` then reports it. */
    int (*data_in)(mc_scsi_target *t, uint8_t *buffer, uint32_t offset, uint32_t count);
    int (*data_out)(mc_scsi_target *t, const uint8_t *buffer, uint32_t offset, uint32_t count);
    /* The data phase is over (or there was none): the status byte. */
    uint8_t (*finish)(mc_scsi_target *t);
    /* Reset condition or BUS DEVICE RESET. */
    void (*reset)(mc_scsi_target *t);
};

#define MC_SCSI_CHUNK 65536u

typedef struct mc_scsi_bus {
    mc_scsi_target *dev[8];
    int phase;                  /* MC_SCSI_* */
    int initiator, target;      /* IDs on the bus, -1 when none */
    int atn;                    /* the initiator asserts ATN */
    int ack_held;               /* a MSG IN byte was taken with ACK still asserted */
    unsigned lun;
    int resume;                 /* phase to return to after MSG OUT, or -1 */
    int command_done;           /* the CDB has been executed */
    uint8_t msg_out[8];
    unsigned msg_out_n;
    uint8_t cdb[16];
    unsigned cdb_n, cdb_need;
    int direction;
    uint32_t total, done;       /* data phase: bytes asked, bytes moved */
    uint32_t chunk_base, chunk_len;
    uint8_t status;
    uint8_t msg_in[4];
    unsigned msg_in_n, msg_in_pos;
    int free_after_msg;         /* the target goes BUS FREE after the MSG IN byte(s) */
    uint8_t *buffer;            /* MC_SCSI_CHUNK bytes */
} mc_scsi_bus;

void mc_scsi_bus_init(mc_scsi_bus *bus);
void mc_scsi_bus_free(mc_scsi_bus *bus);
/* Attach (or with NULL detach) a target at an ID 0-7. */
void mc_scsi_bus_attach(mc_scsi_bus *bus, int id, mc_scsi_target *target);

/* Arbitration (SCSI-2 5.1.2): succeeds when the bus is free. Among several
 * initiators arbitrating at once the highest ID wins; this bus serves one
 * at a time, so the first caller on a free bus wins. Returns 1 when `id`
 * now owns the bus. */
int mc_scsi_bus_arbitrate(mc_scsi_bus *bus, int id);
/* Selection (5.1.3) of `target` by the owner, with ATN asserted or not.
 * Returns 1 when a target answered (it is now connected: MSG OUT with ATN,
 * else COMMAND), 0 when nothing answers (the caller times out and the bus is
 * free again). */
int mc_scsi_bus_select(mc_scsi_bus *bus, int target, int atn);
/* The reset condition (5.2.2): every target resets, the bus goes free. */
void mc_scsi_bus_reset(mc_scsi_bus *bus);
/* The initiator lets go of the bus (its controller was reset): the target
 * abandons the I/O process and the bus goes free. */
void mc_scsi_bus_release(mc_scsi_bus *bus);

int mc_scsi_bus_phase(const mc_scsi_bus *bus);
/* REQ: the target is waiting for a byte transfer (connected, in an
 * information phase, and no MSG IN byte is held with ACK). */
int mc_scsi_bus_req(const mc_scsi_bus *bus);
/* ATN (5.2.1). The target notices it at the end of the current byte. */
void mc_scsi_bus_set_atn(mc_scsi_bus *bus, int atn);

/* Input phases (DATA IN, STATUS, MSG IN): the byte on the data lines. */
uint8_t mc_scsi_bus_peek(const mc_scsi_bus *bus);
/* The initiator takes the byte. hold_ack: for MSG IN, keep ACK asserted so
 * the target waits (mc_scsi_bus_release_ack completes the handshake).
 * Returns the byte. */
uint8_t mc_scsi_bus_in(mc_scsi_bus *bus, int hold_ack);
/* Completes a held MSG IN handshake: the target proceeds (MSG OUT when ATN is
 * asserted, else the next phase or BUS FREE). */
void mc_scsi_bus_release_ack(mc_scsi_bus *bus);
/* Output phases (DATA OUT, COMMAND, MSG OUT): the initiator sends a byte. */
void mc_scsi_bus_out(mc_scsi_bus *bus, uint8_t byte);

#endif
