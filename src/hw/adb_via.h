/* Livadix — the VIA-connected ADB transceiver of the Macintosh II family and
 * the Quadra 700/900.
 *
 * From the Guide to the Macintosh Family Hardware, 2nd ed., chapter 8
 * ("Apple Desktop Bus", "ADB transaction states", Table 8-12) and chapter 4:
 * VIA1 PB4/PB5 are ST0/ST1 (0 start a new command, 1 even data byte, 2 odd
 * data byte, 3 idle), PB3 is /ADB.INT, the shift register exchanges one byte
 * per state with the transceiver clocking CB1; in state 0 the command goes
 * out, then the host alternates 1 and 2 for the data bytes (out for Listen,
 * in for Talk); in state 3 the transceiver repeats the last Talk every 11
 * ms; a Service Request reaches the host as /ADB.INT low; the default state
 * at reset is 3. Timing: bit cells of 100 us, attention 800 us (Table 8-14).
 *
 * How /ADB.INT accompanies each byte follows the Quadra 700 ROM's ADB
 * driver, which defines what it expects of the chip: low with the first byte of a Talk answer means the
 * device timed out; low with the second byte means another device
 * requests service; low with a later byte means the answer has ended (that
 * byte carries nothing); low as the command byte goes out means the
 * transceiver holds an answer from its automatic polling, which the host
 * then reads instead of executing its command; in state 3, a byte shifted in
 * tells the host the polling found data or a Service Request.
 *
 * Lines: MC_ADB_VIA_LINE_INT, the level on /ADB.INT (1 = not asserted). */
#ifndef MC_ADB_VIA_H
#define MC_ADB_VIA_H

#include "adb.h"
#include "hw.h"

#include <stdint.h>

enum { MC_ADB_VIA_LINE_INT };

typedef struct mc_adb_via {
    mc_dev dev;
    mc_adb_bus *bus;
    /* Clocks 8 bits through the VIA's shift register on CB1 (mc_via_shift8). */
    uint8_t (*shift)(void *ctx, uint8_t in);
    void *shift_ctx;
    uint8_t st;                 /* the state the host set */
    uint8_t int_level;
    uint8_t action;             /* what the pending deadline does */
    /* The current transaction. */
    uint8_t cmd;
    uint8_t talk_due;           /* a Talk not executed yet */
    uint8_t listening;
    uint8_t data[8];
    unsigned n, index;          /* bytes of the answer (0: timeout) and bytes delivered */
    uint8_t srq;
    /* Automatic polling in state 3. */
    uint8_t poll_cmd, poll_valid;
    uint8_t held;               /* an answer from polling waits for the host */
    uint8_t held_data[8];
    unsigned held_n;
    uint8_t held_srq;
} mc_adb_via;

void mc_adb_via_init(mc_adb_via *x, const mc_hw_host *host, const char *name, mc_adb_bus *bus,
                     uint8_t (*shift)(void *ctx, uint8_t in), void *shift_ctx);
extern const mc_dev_ops mc_adb_via_ops;
/* The host changed ST1/ST0 (VIA1 PB5/PB4). */
void mc_adb_via_state(mc_adb_via *x, unsigned st);
/* A device has new data: polling should look soon. */
void mc_adb_via_poke(mc_adb_via *x);

/* Timing in C15M periods (Guide, Table 8-14: 100 us bit cells). */
#define MC_ADB_COMMAND_TIME (MC_C15M_HZ / 1000u * 16u / 10u)     /* attention, sync, 8 bits, stop: 1.6 ms */
#define MC_ADB_BYTE_TIME (MC_C15M_HZ / 1250u)                    /* 8 bits: 0.8 ms */
#define MC_ADB_POLL_TIME (MC_C15M_HZ / 1000u * 11u)              /* polling period: 11 ms */

#endif
