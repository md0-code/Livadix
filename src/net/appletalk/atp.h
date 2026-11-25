/* Livadix — ATP, the AppleTalk Transaction Protocol (Inside AppleTalk, 2nd ed.,
 * chapter 9) on one DDP socket of an AppleTalk node: the responder (at-
 * least-once and exactly-once: duplicate requests filtered and answered
 * from the response kept until TRel or the release timer, a response that
 * may come later than the request) and the requester (bitmap, retries,
 * multipacket responses with EOM, TRel after an XO response).
 *
 * Livadix's own code. */
#ifndef MC_ATP_H
#define MC_ATP_H

#include "atalk.h"

#define MC_ATP_DATA_MAX 578
#define MC_ATP_PACKETS 8

typedef struct mc_atp mc_atp;

/* A request received on the socket. */
typedef struct mc_atp_request {
    mc_atalk_addr from;
    uint16_t tid;
    uint8_t bitmap;             /* the response packets the requester has room for */
    uint8_t xo;
    uint8_t user[4];
    const uint8_t *data;
    size_t length;
} mc_atp_request;

typedef void (*mc_atp_request_fn)(void *ctx, mc_atp *atp, const mc_atp_request *request, uint64_t now);
/* A transaction this end requested has ended: status 0 with the response
 * (the first packet's user bytes; the data of packets 0..n-1 in order), or
 * -1 when the retries ran out, -2 when it was cancelled. */
typedef void (*mc_atp_done_fn)(void *ctx, int status, const uint8_t user[4], const uint8_t *data, size_t length, uint64_t now);

/* Opens ATP on a socket of the node (0: a dynamic socket). NULL when the
 * socket cannot be opened. */
mc_atp *mc_atp_open(mc_atalk *at, uint8_t socket, mc_atp_request_fn on_request, void *ctx);
void mc_atp_close(mc_atp *atp);
uint8_t mc_atp_socket(const mc_atp *atp);

/* The response to a request (its address, TID, XO flag and bitmap, as
 * received): data split into packets of up to 578 bytes, the user bytes in
 * the first and zero in the others, EOM on the last. More packets than the
 * bitmap allows are cut. An XO response is kept for duplicates. */
void mc_atp_respond(mc_atp *atp, const mc_atp_request *request, const uint8_t user[4], const uint8_t *data, size_t length,
                    uint64_t now);

/* Sends a request for up to `packets` response packets; retries every
 * timeout_ms, `retries` times (-1: until answered or cancelled). Returns a
 * handle (> 0), 0 when it cannot be sent. */
int mc_atp_send_request(mc_atp *atp, const mc_atalk_addr *to, int xo, const uint8_t user[4], const uint8_t *data, size_t length,
                        unsigned packets, unsigned timeout_ms, int retries, mc_atp_done_fn done, void *ctx, uint64_t now);
/* Ends a request without calling its done function. */
void mc_atp_cancel(mc_atp *atp, int handle);
/* Ends every request whose done context is ctx (a session going away). */
void mc_atp_cancel_ctx(mc_atp *atp, void *ctx);
/* Timers: retries, release timers. */
void mc_atp_tick(mc_atp *atp, uint64_t now);

#endif
