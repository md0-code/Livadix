/* Livadix — ATP (atp.h).
 *
 * Sources: Inside AppleTalk, 2nd ed. (Apple, 1990), chapter 9: the packet
 * format (Figure 9-7: function code 01 TReq, 10 TResp, 11 TRel; XO, EOM,
 * STS; the TRel timer indicator 30 s to 8 min), bitmaps and sequence
 * numbers, the requester and responder state models (TCB, RspCB, the
 * release timer restarted by each response sent), STS answered by sending
 * the request again. DDP type 3 (appendix C).
 *
 * Livadix's own code. */
#include "atp.h"

#include <stdlib.h>
#include <string.h>

#define HEADER 8
#define RSPCBS 64
#define TCBS 32

enum { F_TREQ = 0x40, F_TRESP = 0x80, F_TREL = 0xC0, F_XO = 0x20, F_EOM = 0x10, F_STS = 0x08 };

/* A responder's record of an XO transaction. */
typedef struct rspcb {
    int used, answered;
    mc_atalk_addr from;
    uint16_t tid;
    unsigned release_ms;
    uint64_t expires;
    unsigned count;                         /* packets of the response */
    uint16_t length[MC_ATP_PACKETS];
    uint8_t packet[MC_ATP_PACKETS][HEADER + MC_ATP_DATA_MAX];
} rspcb;

/* A requester's transaction. */
typedef struct tcb {
    int handle;
    mc_atalk_addr to;
    uint16_t tid;
    int xo, retries;
    unsigned timeout_ms;
    uint64_t next;
    uint8_t bitmap;                         /* still missing */
    uint8_t user[4];
    size_t req_length;
    uint8_t request[MC_ATP_DATA_MAX];
    uint8_t got_user[4];
    uint16_t length[MC_ATP_PACKETS];
    uint8_t data[MC_ATP_PACKETS][MC_ATP_DATA_MAX];
    mc_atp_done_fn done;
    void *ctx;
} tcb;

struct mc_atp {
    mc_atalk *at;
    uint8_t socket;
    mc_atp_request_fn on_request;
    void *ctx;
    uint16_t next_tid;
    int next_handle;
    rspcb rsp[RSPCBS];
    tcb req[TCBS];
};

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static int same(const mc_atalk_addr *a, const mc_atalk_addr *b) {
    return a->net == b->net && a->node == b->node && a->socket == b->socket;
}

static const unsigned release_times[5] = {30000, 60000, 120000, 240000, 480000};

static rspcb *rsp_find(mc_atp *atp, const mc_atalk_addr *from, uint16_t tid) {
    for (unsigned k = 0; k < RSPCBS; k++)
        if (atp->rsp[k].used && atp->rsp[k].tid == tid && same(&atp->rsp[k].from, from)) return &atp->rsp[k];
    return NULL;
}

static void send_packets(mc_atp *atp, const rspcb *r, uint8_t bitmap, uint64_t now) {
    for (unsigned k = 0; k < r->count; k++)
        if (bitmap & 1u << k) mc_atalk_send(atp->at, atp->socket, &r->from, MC_DDP_ATP, r->packet[k], r->length[k], now);
}

static void finish(mc_atp *atp, tcb *t, int status, uint64_t now) {
    uint8_t data[MC_ATP_PACKETS * MC_ATP_DATA_MAX];
    size_t n = 0;
    if (status == 0) {
        for (unsigned k = 0; k < MC_ATP_PACKETS && t->length[k] != 0xFFFF; k++) {
            memcpy(data + n, t->data[k], t->length[k]);
            n += t->length[k];
        }
        if (t->xo) {
            uint8_t rel[HEADER] = {F_TREL, 0, 0, 0, 0, 0, 0, 0};
            put16(rel + 2, t->tid);
            mc_atalk_send(atp->at, atp->socket, &t->to, MC_DDP_ATP, rel, HEADER, now);
        }
    }
    const mc_atp_done_fn done = t->done;
    void *ctx = t->ctx;
    uint8_t user[4];
    memcpy(user, t->got_user, 4);
    t->handle = 0;
    if (done) done(ctx, status, user, data, n, now);
}

static void send_request(mc_atp *atp, tcb *t, uint64_t now) {
    uint8_t p[HEADER + MC_ATP_DATA_MAX];
    p[0] = (uint8_t)(F_TREQ | (t->xo ? F_XO : 0));     /* release timer: 30 s */
    p[1] = t->bitmap;
    put16(p + 2, t->tid);
    memcpy(p + 4, t->user, 4);
    memcpy(p + HEADER, t->request, t->req_length);
    mc_atalk_send(atp->at, atp->socket, &t->to, MC_DDP_ATP, p, HEADER + t->req_length, now);
    t->next = now + t->timeout_ms;
}

static void receive(void *ctx, const mc_atalk_addr *from, uint8_t type, const uint8_t *d, size_t n, uint64_t now) {
    mc_atp *atp = ctx;
    if (type != MC_DDP_ATP || n < HEADER || n > HEADER + MC_ATP_DATA_MAX) return;
    const unsigned control = d[0], function = control & 0xC0;
    const uint16_t tid = (uint16_t)get16(d + 2);
    if (function == F_TREQ) {
        rspcb *r = (control & F_XO) ? rsp_find(atp, from, tid) : NULL;
        if (r) {                                /* a duplicate: the response again, or nothing yet */
            if (r->answered) {
                send_packets(atp, r, d[1], now);
                r->expires = now + r->release_ms;
            }
            return;
        }
        if (control & F_XO) {
            for (unsigned k = 0; k < RSPCBS && !r; k++)
                if (!atp->rsp[k].used) r = &atp->rsp[k];
            if (!r) {                           /* the oldest gives way */
                r = &atp->rsp[0];
                for (unsigned k = 1; k < RSPCBS; k++)
                    if (atp->rsp[k].expires < r->expires) r = &atp->rsp[k];
            }
            memset(r, 0, sizeof *r - sizeof r->packet);
            r->used = 1;
            r->from = *from;
            r->tid = tid;
            r->release_ms = release_times[(control & 7) < 5 ? control & 7 : 0];
            r->expires = now + r->release_ms;
        }
        mc_atp_request req;
        req.from = *from;
        req.tid = tid;
        req.bitmap = d[1];
        req.xo = (control & F_XO) ? 1 : 0;
        memcpy(req.user, d + 4, 4);
        req.data = d + HEADER;
        req.length = n - HEADER;
        if (atp->on_request) atp->on_request(atp->ctx, atp, &req, now);
        return;
    }
    if (function == F_TREL) {
        rspcb *r = rsp_find(atp, from, tid);
        if (r) r->used = 0;
        return;
    }
    if (function != F_TRESP) return;
    for (unsigned k = 0; k < TCBS; k++) {
        tcb *t = &atp->req[k];
        if (!t->handle || t->tid != tid || t->to.node != from->node || (t->to.net && from->net && t->to.net != from->net)) continue;
        const unsigned seq = d[1];
        if (seq >= MC_ATP_PACKETS || !(t->bitmap & 1u << seq)) return;
        t->bitmap &= (uint8_t)~(1u << seq);
        t->length[seq] = (uint16_t)(n - HEADER);
        memcpy(t->data[seq], d + HEADER, n - HEADER);
        if (seq == 0) memcpy(t->got_user, d + 4, 4);
        if (control & F_EOM) {
            t->bitmap &= (uint8_t)((1u << (seq + 1)) - 1);
            for (unsigned s = seq + 1; s < MC_ATP_PACKETS; s++) t->length[s] = 0xFFFF;
        }
        if (!t->bitmap) finish(atp, t, 0, now);
        else if (control & F_STS) send_request(atp, t, now);
        return;
    }
}

mc_atp *mc_atp_open(mc_atalk *at, uint8_t socket, mc_atp_request_fn on_request, void *ctx) {
    mc_atp *atp = calloc(1, sizeof *atp);
    if (!atp) return NULL;
    atp->at = at;
    atp->on_request = on_request;
    atp->ctx = ctx;
    atp->next_handle = 1;
    atp->socket = mc_atalk_open(at, socket, receive, atp);
    if (!atp->socket) {
        free(atp);
        return NULL;
    }
    return atp;
}

void mc_atp_close(mc_atp *atp) {
    if (!atp) return;
    mc_atalk_close(atp->at, atp->socket);
    free(atp);
}

uint8_t mc_atp_socket(const mc_atp *atp) { return atp->socket; }

void mc_atp_respond(mc_atp *atp, const mc_atp_request *q, const uint8_t user[4], const uint8_t *data, size_t length, uint64_t now) {
    rspcb local, *r = q->xo ? rsp_find(atp, &q->from, q->tid) : NULL;
    if (!r) {
        r = &local;
        memset(r, 0, sizeof *r - sizeof r->packet);
        r->from = q->from;
        r->tid = q->tid;
    }
    /* As many packets as the data needs (at least one), no more than the bitmap's highest. */
    unsigned most = 0;
    for (unsigned k = 0; k < MC_ATP_PACKETS; k++)
        if (q->bitmap & 1u << k) most = k + 1;
    unsigned count = (unsigned)((length + MC_ATP_DATA_MAX - 1) / MC_ATP_DATA_MAX);
    if (!count) count = 1;
    if (count > most) count = most;
    if (!count) {
        if (r != &local) r->used = 0;
        return;
    }
    size_t off = 0;
    for (unsigned k = 0; k < count; k++) {
        uint8_t *p = r->packet[k];
        size_t chunk = length - off < MC_ATP_DATA_MAX ? length - off : MC_ATP_DATA_MAX;
        p[0] = (uint8_t)(F_TRESP | (k == count - 1 ? F_EOM : 0));
        p[1] = (uint8_t)k;
        put16(p + 2, q->tid);
        if (k == 0 && user) memcpy(p + 4, user, 4);
        else memset(p + 4, 0, 4);
        memcpy(p + HEADER, data + off, chunk);
        r->length[k] = (uint16_t)(HEADER + chunk);
        off += chunk;
    }
    r->count = count;
    r->answered = 1;
    r->expires = now + r->release_ms;
    send_packets(atp, r, q->bitmap, now);
}

int mc_atp_send_request(mc_atp *atp, const mc_atalk_addr *to, int xo, const uint8_t user[4], const uint8_t *data, size_t length,
                        unsigned packets, unsigned timeout_ms, int retries, mc_atp_done_fn done, void *ctx, uint64_t now) {
    if (length > MC_ATP_DATA_MAX || packets < 1 || packets > MC_ATP_PACKETS) return 0;
    tcb *t = NULL;
    for (unsigned k = 0; k < TCBS && !t; k++)
        if (!atp->req[k].handle) t = &atp->req[k];
    if (!t) return 0;
    memset(t, 0, sizeof *t - sizeof t->data);
    t->handle = atp->next_handle++;
    if (atp->next_handle <= 0) atp->next_handle = 1;
    t->to = *to;
    /* A TID no transaction of ours uses. */
    for (;;) {
        const uint16_t tid = atp->next_tid++;
        int busy = 0;
        for (unsigned k = 0; k < TCBS; k++)
            if (&atp->req[k] != t && atp->req[k].handle && atp->req[k].tid == tid) busy = 1;
        if (!busy) {
            t->tid = tid;
            break;
        }
    }
    t->xo = xo;
    t->retries = retries;
    t->timeout_ms = timeout_ms ? timeout_ms : 2000;
    t->bitmap = (uint8_t)((1u << packets) - 1);
    if (user) memcpy(t->user, user, 4);
    t->req_length = length;
    memcpy(t->request, data, length);
    for (unsigned k = 0; k < MC_ATP_PACKETS; k++) t->length[k] = k < packets ? 0 : 0xFFFF;
    t->done = done;
    t->ctx = ctx;
    send_request(atp, t, now);
    return t->handle;
}

void mc_atp_cancel(mc_atp *atp, int handle) {
    for (unsigned k = 0; k < TCBS; k++)
        if (handle && atp->req[k].handle == handle) atp->req[k].handle = 0;
}

void mc_atp_cancel_ctx(mc_atp *atp, void *ctx) {
    for (unsigned k = 0; k < TCBS; k++)
        if (atp->req[k].handle && atp->req[k].ctx == ctx) atp->req[k].handle = 0;
}

void mc_atp_tick(mc_atp *atp, uint64_t now) {
    for (unsigned k = 0; k < RSPCBS; k++)
        if (atp->rsp[k].used && now >= atp->rsp[k].expires) atp->rsp[k].used = 0;
    for (unsigned k = 0; k < TCBS; k++) {
        tcb *t = &atp->req[k];
        if (!t->handle || now < t->next) continue;
        if (t->retries == 0) {
            finish(atp, t, -1, now);
            continue;
        }
        if (t->retries > 0) t->retries--;
        send_request(atp, t, now);
    }
}
