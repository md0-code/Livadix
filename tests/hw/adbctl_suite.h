/* The protocol-level checks Egret and Cuda share (adbctl_suite.c). */
#ifndef MC_ADBCTL_SUITE_H
#define MC_ADBCTL_SUITE_H

#include "adbctl_host.h"

/* A packet as a byte array, and a command whose reply must be `want`. */
#define P(...) ((const uint8_t[]){__VA_ARGS__})
#define EXPECT(host, stream, req, want) \
    suite_is(suite_cmd((host), (req), (unsigned)sizeof(req), (stream)), (want), (unsigned)sizeof(want))

const ac_packet *suite_cmd(ac_host *host, const uint8_t *p, unsigned n, unsigned stream);
int suite_is(const ac_packet *r, const uint8_t *want, unsigned n);
/* Runs the shared checks on a fresh host of `kind`; the host stays up. */
void adbctl_suite(mc_adbctl_kind kind);
ac_host *adbctl_suite_host(void);

#endif
