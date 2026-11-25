/* Livadix — the server end of ASP (asp.h).
 *
 * Sources: Inside AppleTalk, 2nd ed. (Apple, 1990), chapter 11: "Packet
 * formats and algorithms" (Figures 11-10 to 11-15: OpenSess and its reply,
 * CloseSess, GetStatus, Command and CmdReply, Write, WriteContinue and its
 * reply, Tickle, Attention), "Maintaining the session" (tickles every 30 s,
 * the 2-minute session maintenance timer), "Timeouts and retry counts";
 * appendix C (SPFunction values 1-8, SPError values, ASP version $0100).
 *
 * Livadix's own code. */
#include "asp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SP_CLOSE = 1, SP_COMMAND = 2, SP_STATUS = 3, SP_OPEN = 4, SP_TICKLE = 5, SP_WRITE = 6, SP_WRITE_CONTINUE = 7, SP_ATTENTION = 8 };
#define TICKLE_MS 30000
#define SESSION_MS 120000
#define VERSION 0x0100

typedef struct session {
    struct mc_asp *asp;
    int used;
    uint8_t id;
    mc_atalk_addr wss;
    uint64_t heard;
    void *client;
    int tickle;                          /* the tickle transaction's handle */
    /* a write waiting for its data */
    int writing;
    mc_atp_request write;
    uint8_t write_cmd[MC_ATP_DATA_MAX];
} session;

struct mc_asp {
    mc_atalk *at;
    const mc_asp_server_ops *ops;
    void *ctx;
    mc_atp *sls, *sss;
    uint8_t next_id;
    session sessions[MC_ASP_SESSIONS];
    uint8_t reply[MC_ASP_QUANTUM];
    void (*log)(void *ctx, const char *line);
    void *log_ctx;
};

static void trace(mc_asp *asp, const char *where, const mc_atp_request *q) {
    if (!asp->log) return;
    char line[160];
    snprintf(line, sizeof line, "ASP %s: function %u session %u seq %u from %u.%u:%u tid %u bitmap %02X%s, %u bytes", where, q->user[0],
             q->user[1], (unsigned)(q->user[2] << 8 | q->user[3]), q->from.net, q->from.node, q->from.socket, q->tid, q->bitmap,
             q->xo ? " XO" : "", (unsigned)q->length);
    asp->log(asp->log_ctx, line);
}

void mc_asp_set_log(mc_asp *asp, void (*log)(void *ctx, const char *line), void *ctx) {
    asp->log = log;
    asp->log_ctx = ctx;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

static session *by_id(mc_asp *asp, unsigned id, const mc_atalk_addr *from) {
    for (unsigned k = 0; k < MC_ASP_SESSIONS; k++) {
        session *s = &asp->sessions[k];
        if (s->used && s->id == id && s->wss.node == from->node && (!s->wss.net || !from->net || s->wss.net == from->net)) return s;
    }
    return NULL;
}

static session *by_client(mc_asp *asp, void *client) {
    for (unsigned k = 0; k < MC_ASP_SESSIONS; k++)
        if (asp->sessions[k].used && asp->sessions[k].client == client) return &asp->sessions[k];
    return NULL;
}

static void end_session(mc_asp *asp, session *s) {
    mc_atp_cancel_ctx(asp->sls, s);
    mc_atp_cancel_ctx(asp->sss, s);
    s->used = 0;
    if (asp->ops->closed) asp->ops->closed(asp->ctx, s->client);
    s->client = NULL;
}

static void ignore_done(void *ctx, int status, const uint8_t user[4], const uint8_t *data, size_t length, uint64_t now) {
    (void)ctx; (void)status; (void)user; (void)data; (void)length; (void)now;
}

/* ---- the listening socket ------------------------------------------------ */

static void on_sls(void *ctx, mc_atp *atp, const mc_atp_request *q, uint64_t now) {
    mc_asp *asp = ctx;
    const unsigned function = q->user[0];
    trace(asp, "SLS", q);
    if (function == SP_STATUS) {
        const size_t n = asp->ops->status(asp->ctx, asp->reply, sizeof asp->reply);
        const uint8_t zero[4] = {0};
        mc_atp_respond(atp, q, zero, asp->reply, n, now);
        return;
    }
    if (function == SP_TICKLE) {
        session *s = by_id(asp, q->user[1], &q->from);
        if (s) s->heard = now;
        return;
    }
    if (function != SP_OPEN) return;
    uint8_t user[4] = {0};
    int error = 0;
    session *s = NULL;
    if (get16(q->user + 2) != VERSION) error = MC_ASP_BAD_VERSION;
    else {
        for (unsigned k = 0; k < MC_ASP_SESSIONS && !s; k++)
            if (!asp->sessions[k].used) s = &asp->sessions[k];
        if (!s) error = MC_ASP_SERVER_BUSY;
    }
    if (s) {
        /* A session ID no open session has. */
        uint8_t id;
        for (;;) {
            id = ++asp->next_id;
            if (!id) continue;
            int busy = 0;
            for (unsigned k = 0; k < MC_ASP_SESSIONS; k++)
                if (asp->sessions[k].used && asp->sessions[k].id == id) busy = 1;
            if (!busy) break;
        }
        memset(s, 0, sizeof *s);
        s->asp = asp;
        s->id = id;
        s->wss = q->from;
        s->wss.socket = q->user[1];
        s->heard = now;
        s->client = asp->ops->opened(asp->ctx, id, &s->wss);
        if (!s->client) error = MC_ASP_SERVER_BUSY;
        else {
            s->used = 1;
            const uint8_t tickle[4] = {SP_TICKLE, id, 0, 0};
            s->tickle = mc_atp_send_request(asp->sls, &s->wss, 0, tickle, NULL, 0, 1, TICKLE_MS, -1, ignore_done, s, now);
            user[0] = mc_atp_socket(asp->sss);
            user[1] = id;
        }
    }
    put16(user + 2, (unsigned)(error & 0xFFFF));
    mc_atp_respond(atp, q, user, NULL, 0, now);
}

/* ---- the session socket ---------------------------------------------------- */

static void write_data(void *ctx, int status, const uint8_t user[4], const uint8_t *data, size_t length, uint64_t now);

static void on_sss(void *ctx, mc_atp *atp, const mc_atp_request *q, uint64_t now) {
    mc_asp *asp = ctx;
    const unsigned function = q->user[0];
    trace(asp, "SSS", q);
    session *s = by_id(asp, q->user[1], &q->from);
    if (!s) return;
    s->heard = now;
    if (function == SP_TICKLE) return;
    if (function == SP_CLOSE) {
        const uint8_t zero[4] = {0};
        mc_atp_respond(atp, q, zero, NULL, 0, now);
        end_session(asp, s);
        return;
    }
    if (function == SP_COMMAND) {
        size_t n = 0;
        const int32_t result = asp->ops->command(asp->ctx, s->client, q->data, q->length, asp->reply, &n);
        uint8_t user[4];
        put32(user, (uint32_t)result);
        mc_atp_respond(atp, q, user, asp->reply, n, now);
        return;
    }
    if (function != SP_WRITE || s->writing) return;
    size_t n = 0;
    int32_t result = 0;
    const long want = asp->ops->write_begin(asp->ctx, s->client, q->data, q->length, &result, asp->reply, &n);
    if (want < 0) {
        uint8_t user[4];
        put32(user, (uint32_t)result);
        mc_atp_respond(atp, q, user, asp->reply, n, now);
        return;
    }
    /* WriteContinue to the workstation for the data. */
    s->writing = 1;
    s->write = *q;
    memcpy(s->write_cmd, q->data, q->length);
    s->write.data = s->write_cmd;
    const size_t size = (size_t)want > MC_ASP_QUANTUM ? MC_ASP_QUANTUM : (size_t)want;
    unsigned packets = (unsigned)((size + MC_ATP_DATA_MAX - 1) / MC_ATP_DATA_MAX);
    if (!packets) packets = 1;
    uint8_t user[4] = {SP_WRITE_CONTINUE, s->id, q->user[2], q->user[3]};
    uint8_t buffer[2];
    put16(buffer, (unsigned)size);
    if (!mc_atp_send_request(asp->sss, &s->wss, 1, user, buffer, 2, packets, 2000, -1, write_data, s, now)) {
        s->writing = 0;
        uint8_t err[4];
        put32(err, (uint32_t)-1);
        mc_atp_respond(atp, q, err, NULL, 0, now);
    }
}

static void write_data(void *ctx, int status, const uint8_t user[4], const uint8_t *data, size_t length, uint64_t now) {
    (void)user;
    session *s = ctx;
    mc_asp *asp = s->asp;
    s->writing = 0;
    if (status != 0 || !s->used) return;
    s->heard = now;
    size_t n = 0;
    const int32_t result = asp->ops->write_end(asp->ctx, s->client, s->write.data, s->write.length, data, length, asp->reply, &n);
    uint8_t ruser[4];
    put32(ruser, (uint32_t)result);
    mc_atp_respond(asp->sss, &s->write, ruser, asp->reply, n, now);
}

mc_asp *mc_asp_create(mc_atalk *at, const mc_asp_server_ops *ops, void *ctx) {
    mc_asp *asp = calloc(1, sizeof *asp);
    if (!asp) return NULL;
    asp->at = at;
    asp->ops = ops;
    asp->ctx = ctx;
    asp->sls = mc_atp_open(at, 0, on_sls, asp);
    asp->sss = mc_atp_open(at, 0, on_sss, asp);
    if (!asp->sls || !asp->sss) {
        mc_atp_close(asp->sls);
        mc_atp_close(asp->sss);
        free(asp);
        return NULL;
    }
    return asp;
}

void mc_asp_destroy(mc_asp *asp, uint64_t now) {
    if (!asp) return;
    for (unsigned k = 0; k < MC_ASP_SESSIONS; k++)
        if (asp->sessions[k].used) mc_asp_close(asp, asp->sessions[k].client, now);
    mc_atp_close(asp->sls);
    mc_atp_close(asp->sss);
    free(asp);
}

uint8_t mc_asp_listening_socket(const mc_asp *asp) { return mc_atp_socket(asp->sls); }

unsigned mc_asp_session_count(const mc_asp *asp) {
    unsigned n = 0;
    for (unsigned k = 0; k < MC_ASP_SESSIONS; k++) n += asp->sessions[k].used ? 1u : 0u;
    return n;
}

void mc_asp_tick(mc_asp *asp, uint64_t now) {
    mc_atp_tick(asp->sls, now);
    mc_atp_tick(asp->sss, now);
    for (unsigned k = 0; k < MC_ASP_SESSIONS; k++) {
        session *s = &asp->sessions[k];
        if (s->used && now - s->heard > SESSION_MS) end_session(asp, s);
    }
}

int mc_asp_attention(mc_asp *asp, void *client, uint16_t code, uint64_t now) {
    session *s = by_client(asp, client);
    if (!s || !code) return 0;
    uint8_t user[4] = {SP_ATTENTION, s->id, 0, 0};
    put16(user + 2, code);
    return mc_atp_send_request(asp->sls, &s->wss, 0, user, NULL, 0, 1, 2000, 10, ignore_done, NULL, now) != 0;
}

void mc_asp_close(mc_asp *asp, void *client, uint64_t now) {
    session *s = by_client(asp, client);
    if (!s) return;
    const uint8_t user[4] = {SP_CLOSE, s->id, 0, 0};
    const mc_atalk_addr wss = s->wss;
    end_session(asp, s);
    mc_atp_send_request(asp->sls, &wss, 0, user, NULL, 0, 1, 1000, 3, ignore_done, NULL, now);
}
