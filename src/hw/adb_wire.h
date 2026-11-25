/* Livadix — the Apple Desktop Bus at the bit-cell level, for a host that
 * drives the line itself (the Egret and Cuda firmware, adbctl_mcu.c).
 *
 * From the Guide to the Macintosh Family Hardware, 2nd ed., chapter 8
 * "Apple Desktop Bus" (Figures 8-3 to 8-6, Table 8-14): an open-collector
 * line, high when idle; bit cells of about 100 us, a 0 low for about 65 us
 * then high, a 1 low for about 35 us then high; a command is an attention
 * low of 800 us, a sync high of 65 us, the command byte and a 0 stop bit; a
 * line held low for 3 ms resets every device; a device answers a Talk
 * after a stop-to-start time of 140-260 us with a 1 start bit, 2 to 8
 * bytes and a 0 stop bit, and a host's Listen data has the same frame; a
 * device that wants service holds the command's stop bit low for 300 us
 * (the Service Request).
 *
 * The host's level changes come in with their times (C15M periods); the
 * devices (adb.h) answer as a waveform the host samples with
 * mc_adb_wire_level. Pulses are classified by their low time: 2 ms or
 * more a reset, 560 us or more an attention, below 50 us a 1, else a 0. */
#ifndef MC_ADB_WIRE_H
#define MC_ADB_WIRE_H

#include "adb.h"

#include <stdint.h>

#define MC_ADB_WIRE_LOWS 80

typedef struct mc_adb_wire {
    mc_adb_bus *bus;
    uint8_t host_low;
    uint64_t fall, rise;        /* the host's last edges */
    uint8_t state;              /* see adb_wire.c */
    uint8_t cmd;
    unsigned bits;
    uint8_t data[8];
    unsigned nbits;             /* Listen data bits (after the start bit) */
    /* What the devices drive: low intervals [start, end) in C15M periods. */
    uint64_t low_start[MC_ADB_WIRE_LOWS], low_end[MC_ADB_WIRE_LOWS];
    unsigned nlow;
    unsigned commands, answers; /* counters for traces */
} mc_adb_wire;

void mc_adb_wire_init(mc_adb_wire *w, mc_adb_bus *bus);
/* The host drives the line low (1) or releases it (0) at time `t`. */
void mc_adb_wire_host(mc_adb_wire *w, int low, uint64_t t);
/* The line's level at time `t` (1 = high): low when the host or a device
 * holds it low. A Listen whose data ended is delivered here. */
int mc_adb_wire_level(mc_adb_wire *w, uint64_t t);

/* Timing in C15M periods (Guide, Table 8-14). */
#define MC_ADB_US(us) ((uint64_t)MC_C15M_HZ / 1000000u * (us) + (uint64_t)MC_C15M_HZ % 1000000u * (us) / 1000000u)

#endif
