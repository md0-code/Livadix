/* The host side of Egret and Cuda for the tests: a VIA1, the controller,
 * an ADB keyboard and mouse, and a polling driver that works the VIA's
 * registers the way the Mac ROMs' Egret and Cuda drivers do (the IIsi and
 * Quadra 605 ROMs: egret.h, cuda.h). Time is the fake host's (hw_test.h),
 * advanced in small steps while the driver polls. */
#ifndef MC_ADBCTL_HOST_H
#define MC_ADBCTL_HOST_H

#include "hw_test.h"
#include "../../src/hw/adb.h"
#include "../../src/hw/adbctl.h"
#include "../../src/hw/via.h"

#define AC_LOG 256

typedef struct ac_packet {
    uint8_t b[48];
    unsigned n;
    uint64_t attention;         /* the controller's idle byte's interrupt (C15M) */
    uint64_t start, end;        /* first byte's interrupt, last byte's interrupt */
} ac_packet;

typedef struct ac_host {
    hw_fake f;
    mc_via via;
    mc_adb_bus adb;
    mc_adb_keyboard kbd;
    mc_adb_mouse mouse;
    mc_adbctl ctl;
    mc_adbctl_kind kind;
    uint64_t step;              /* the driver's polling period */
    unsigned stream;            /* bytes to read of a streaming reply (after the header) */
    ac_packet log[AC_LOG];      /* every packet received, in order */
    unsigned nlog, read;        /* packets logged, packets consumed by ac_next */
    unsigned collisions;
    uint64_t sent;              /* when the last packet sent ended (TIP/SysSes negated) */
    uint64_t send_wait;         /* the last send: time spent waiting for the controller's clocks */
    unsigned send_bytes;
    uint64_t send_first;        /* of it, for the first byte */
    uint64_t send_total;
    uint8_t lines[3][2];        /* line changes seen: [line][state] counts */
} ac_host;

/* A host with a controller of `kind`, at the protocol level (image NULL)
 * or running the firmware image; brings it up: powered, reset released,
 * the link idle. Returns 0 when the firmware never came up. */
int ac_init(ac_host *h, mc_adbctl_kind kind, const uint8_t *image);
/* Cuda: the host's resynchronisation as the Mac ROMs do it (TIP negated
 * with BYTEACK low, the controller's clocks, BYTEACK high, its clocks;
 * the Power Macintosh ROM's $4080C834-$4080C89C). */
void ac_resync(ac_host *h);
/* Advances time by `periods`, receiving whatever the controller sends. */
void ac_run(ac_host *h, uint64_t periods);
/* Sends a packet; returns 1, or 0 when the controller's own packet
 * collided with it (that one is then received and logged). */
int ac_send(ac_host *h, const uint8_t *p, unsigned n);
/* The next packet not consumed yet, waiting up to `timeout`; NULL if none. */
const ac_packet *ac_next(ac_host *h, uint64_t timeout);
/* Sends and returns the reply: the next packet that answers this command
 * (others are skipped and stay in the log). Retries a collided send. */
const ac_packet *ac_command(ac_host *h, const uint8_t *p, unsigned n, uint64_t timeout);

#define AC_MS(n) ((uint64_t)MC_C15M_HZ / 1000u * (n))
#define AC_US(n) ((uint64_t)MC_C15M_HZ / 1000000u * (n))

#endif
