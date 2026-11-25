/* Livadix — the file server as a host on the segment (share.h).
 *
 * Livadix's own code. */
#include "share.h"
#include "appletalk/asp.h"
#include "net.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A locally administered address beside the NAT's 02:4C:56:44:58:02. */
const uint8_t mc_share_mac[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x03};

struct mc_share {
    mc_net *net;
    mc_atalk *at;
    mc_asp *asp;
    mc_afp *afp;
    int started, registered;
    int log;
};

static void out(void *ctx, const uint8_t *frame, size_t length) {
    mc_share *s = ctx;
    if (s->net) mc_net_emit(s->net, s, frame, length);
}

static void log_line(void *ctx, const char *line) {
    const mc_share *s = ctx;
    if (s && s->log) fprintf(stderr, "network: %s\n", line);
}

static size_t status(void *ctx, uint8_t *o, size_t capacity) { return mc_afp_status(((mc_share *)ctx)->afp, o, capacity); }
static void *opened(void *ctx, unsigned id, const mc_atalk_addr *ws) {
    mc_share *s = ctx;
    if (s->log) fprintf(stderr, "network: AppleShare session %u from %u.%u\n", id, ws->net, ws->node);
    return mc_afp_session_open(s->afp);
}
static void closed(void *ctx, void *session) {
    mc_share *s = ctx;
    if (s->log) fprintf(stderr, "network: AppleShare session closed\n");
    mc_afp_session_close(s->afp, session);
}
static int32_t command(void *ctx, void *session, const uint8_t *cmd, size_t length, uint8_t *reply, size_t *reply_length) {
    mc_share *s = ctx;
    const int32_t r = mc_afp_command(s->afp, session, cmd, length, reply, reply_length);
    if (s->log > 1) fprintf(stderr, "network: AFP %u -> %ld (%zu bytes)\n", length ? cmd[0] : 0, (long)r, *reply_length);
    return r;
}
static long write_begin(void *ctx, void *session, const uint8_t *cmd, size_t length, int32_t *result, uint8_t *reply, size_t *reply_length) {
    return mc_afp_write_begin(((mc_share *)ctx)->afp, session, cmd, length, result, reply, reply_length);
}
static int32_t write_end(void *ctx, void *session, const uint8_t *cmd, size_t length, const uint8_t *data, size_t data_length, uint8_t *reply,
                         size_t *reply_length) {
    mc_share *s = ctx;
    const int32_t r = mc_afp_write_end(s->afp, session, cmd, length, data, data_length, reply, reply_length);
    if (s->log > 1) fprintf(stderr, "network: AFP write %u -> %ld (%zu bytes)\n", length ? cmd[0] : 0, (long)r, data_length);
    return r;
}
static const mc_asp_server_ops asp_ops = {status, opened, closed, command, write_begin, write_end};

mc_share *mc_share_create(const mc_afp_config *config, char *error, size_t capacity) {
    mc_share *s = calloc(1, sizeof *s);
    if (!s) {
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    const char *env = getenv("LIVADIX_NET_LOG");
    s->log = env ? (atoi(env) > 1 ? atoi(env) : 1) : 0;
    mc_afp_config c = *config;
    if (!c.log && s->log) {
        c.log = log_line;
        c.log_ctx = s;
    }
    s->afp = mc_afp_create(&c, error, capacity);
    s->at = s->afp ? mc_atalk_create(mc_share_mac, (uint32_t)mc_clock_ms(), out, s) : NULL;
    s->asp = s->at ? mc_asp_create(s->at, &asp_ops, s) : NULL;
    if (!s->asp) {
        if (s->afp && capacity && !error[0]) snprintf(error, capacity, "Cannot start the file server");
        mc_share_destroy(s);
        return NULL;
    }
    if (s->log) mc_atalk_set_log(s->at, log_line, s);
    if (s->log > 1) mc_asp_set_log(s->asp, log_line, s);
    if (s->log > 2) mc_atalk_set_trace(s->at, 1);
    size_t n;
    const char *name = mc_afp_server_name(s->afp, &n);
    char object[33];
    memcpy(object, name, n);
    object[n] = 0;
    mc_nbp_register(s->at, object, "AFPServer", mc_asp_listening_socket(s->asp));
    return s;
}

void mc_share_destroy(mc_share *s) {
    if (!s) return;
    if (s->asp) mc_asp_destroy(s->asp, mc_clock_ms());
    mc_atalk_destroy(s->at);
    mc_afp_destroy(s->afp);
    free(s);
}

mc_afp *mc_share_afp(mc_share *s) { return s->afp; }

static void host_input(void *self, const uint8_t *frame, size_t length) {
    mc_share *s = self;
    mc_atalk_input(s->at, frame, length, mc_clock_ms());
}

static void host_poll(void *self, int timeout_ms) {
    (void)timeout_ms;
    mc_share *s = self;
    const uint64_t now = mc_clock_ms();
    if (!s->started) {
        s->started = 1;
        mc_atalk_start(s->at, now);
    }
    mc_atalk_tick(s->at, now);
    mc_asp_tick(s->asp, now);
    mc_afp_poll(s->afp);
}

static void host_destroy(void *self) { mc_share_destroy(self); }

static const mc_net_host_ops share_ops = {host_input, host_poll, NULL, host_destroy};

int mc_share_attach(mc_net *net, mc_share *s) {
    s->net = net;
    return mc_net_attach_host(net, &share_ops, s);
}
