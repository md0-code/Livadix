/* Livadix — the AppleTalk node (atalk.h).
 *
 * Sources: Inside AppleTalk, 2nd ed. (Apple, 1990): chapter 2 (AARP:
 * request, response and probe; gleaning; AMT aging by probes), chapter 3
 * (ELAP: 802.2 type 1 with SNAP, discriminators $080007809B and
 * $00000080F3, the broadcast multicast $090007FFFFFF, node IDs 0, $FE and
 * $FF never chosen, probes every 1/5 s, 10 of them), chapter 4 (DDP: the
 * extended header, the checksum, startup range, routing on a nonrouter
 * node), chapter 6 (AEP), chapter 7 (NBP packets and tuples), appendix C
 * (DDP types, sockets) and appendix D (Table D-2, the case mapping).
 *
 * Livadix's own code. */
#include "atalk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_MAX 1514
#define ELAP_HEADER 22          /* 802.3 (14) + 802.2 (3) + SNAP (5) */
#define DDP_HEADER 13
#define AMT_SIZE 64
#define PENDING 16
#define NAMES 8
#define PROBES 10
#define PROBE_MS 200
#define REQUEST_MS 250
#define REQUESTS 8

static const uint8_t broadcast_mac[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
static const uint8_t snap_ddp[5] = {0x08, 0x00, 0x07, 0x80, 0x9B};
static const uint8_t snap_aarp[5] = {0x00, 0x00, 0x00, 0x80, 0xF3};

typedef struct amt_entry {
    uint16_t net;
    uint8_t node, used;
    uint8_t mac[6];
    uint64_t when;
} amt_entry;

/* A datagram waiting for its destination's hardware address. */
typedef struct pending {
    int used;
    uint16_t net;
    uint8_t node;
    unsigned tries;
    uint64_t next;
    size_t length;
    uint8_t frame[FRAME_MAX];
} pending;

typedef struct socket_entry {
    mc_atalk_receive_fn receive;
    void *ctx;
} socket_entry;

typedef struct name_entry {
    int used;
    uint8_t socket, enumerator;
    uint8_t object[MC_NBP_NAME_MAX + 1], type[MC_NBP_NAME_MAX + 1];   /* Pascal strings */
} name_entry;

struct mc_atalk {
    uint8_t mac[6];
    uint32_t seed;
    mc_atalk_output_fn output;
    void *output_ctx;
    mc_atalk_log_fn log;
    void *log_ctx;
    /* address: probing (probes > 0) or held (ready) */
    uint16_t net;
    uint8_t node;
    int ready, probing;
    unsigned probes;
    uint64_t next_probe;
    amt_entry amt[AMT_SIZE];
    pending queue[PENDING];
    socket_entry sockets[256];
    name_entry names[NAMES];
    uint8_t next_enumerator;
    mc_atalk_stats stats;
    int trace;
};

static void logf_(mc_atalk *at, const char *format, ...) {
    if (!at->log) return;
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    at->log(at->log_ctx, line);
}

/* Table D-2: lower case to upper case in AppleTalk names. */
static uint8_t upper(uint8_t c) {
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 32);
    switch (c) {
    case 0x88: return 0xCB;
    case 0x8A: return 0x80;
    case 0x8B: return 0xCC;
    case 0x8C: return 0x81;
    case 0x8D: return 0x82;
    case 0x8E: return 0x83;
    case 0x96: return 0x84;
    case 0x9A: return 0x85;
    case 0x9B: return 0xCD;
    case 0x9F: return 0x86;
    case 0xBE: return 0xAE;
    case 0xBF: return 0xAF;
    case 0xCF: return 0xCE;
    default: return c;
    }
}

uint16_t mc_ddp_checksum(const uint8_t *d, size_t n) {
    uint16_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum = (uint16_t)(sum + d[i]);
        sum = (uint16_t)(sum << 1 | sum >> 15);
    }
    return sum ? sum : 0xFFFF;
}

int mc_nbp_match(const uint8_t *p, size_t plen, const uint8_t *s, size_t slen) {
    if (plen == 1 && p[0] == '=') return 1;
    for (size_t k = 0; k < plen; k++) {
        if (p[k] != 0xC5) continue;
        /* prefix ≈ suffix */
        const size_t suffix = plen - k - 1;
        if (k + suffix > slen) return 0;
        for (size_t i = 0; i < k; i++)
            if (upper(p[i]) != upper(s[i])) return 0;
        for (size_t i = 0; i < suffix; i++)
            if (upper(p[k + 1 + i]) != upper(s[slen - suffix + i])) return 0;
        return 1;
    }
    if (plen != slen) return 0;
    for (size_t i = 0; i < plen; i++)
        if (upper(p[i]) != upper(s[i])) return 0;
    return 1;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }

/* An ELAP frame's link headers; returns where its data goes. */
static uint8_t *elap(mc_atalk *at, uint8_t *f, const uint8_t dst[6], const uint8_t snap[5]) {
    memcpy(f, dst, 6);
    memcpy(f + 6, at->mac, 6);
    f[14] = 0xAA;
    f[15] = 0xAA;
    f[16] = 0x03;
    memcpy(f + 17, snap, 5);
    return f + ELAP_HEADER;
}

/* Sets the 802.3 length, pads to the 60-byte minimum and sends. */
static void transmit(mc_atalk *at, uint8_t *f, size_t length) {
    put16(f + 12, (unsigned)(length - 14));
    while (length < 60) f[length++] = 0;
    at->output(at->output_ctx, f, length);
}

/* ---- AARP ------------------------------------------------------------------ */

static void aarp_send(mc_atalk *at, unsigned function, const uint8_t dst_mac[6], const uint8_t target_mac[6], uint16_t target_net,
                      uint8_t target_node, uint16_t src_net, uint8_t src_node) {
    uint8_t f[64];
    uint8_t *p = elap(at, f, dst_mac, snap_aarp);
    put16(p, 1);                    /* Ethernet */
    put16(p + 2, 0x809B);           /* AppleTalk */
    p[4] = 6;
    p[5] = 4;
    put16(p + 6, function);
    memcpy(p + 8, at->mac, 6);
    p[14] = 0;
    put16(p + 15, src_net);
    p[17] = src_node;
    if (target_mac) memcpy(p + 18, target_mac, 6);
    else memset(p + 18, 0, 6);
    p[24] = 0;
    put16(p + 25, target_net);
    p[27] = target_node;
    transmit(at, f, ELAP_HEADER + 28);
}

static amt_entry *amt_find(mc_atalk *at, uint16_t net, uint8_t node) {
    for (unsigned k = 0; k < AMT_SIZE; k++)
        if (at->amt[k].used && at->amt[k].net == net && at->amt[k].node == node) return &at->amt[k];
    return NULL;
}

static void amt_forget(mc_atalk *at, uint16_t net, uint8_t node) {
    amt_entry *e = amt_find(at, net, node);
    if (e) e->used = 0;
}

static void flush_pending(mc_atalk *at, uint16_t net, uint8_t node, const uint8_t mac[6]) {
    for (unsigned k = 0; k < PENDING; k++) {
        pending *q = &at->queue[k];
        if (!q->used || q->net != net || q->node != node) continue;
        memcpy(q->frame, mac, 6);
        at->output(at->output_ctx, q->frame, q->length);
        q->used = 0;
    }
}

static void amt_learn(mc_atalk *at, uint16_t net, uint8_t node, const uint8_t mac[6], uint64_t now) {
    if (node == 0 || node >= 0xFE) return;
    amt_entry *e = amt_find(at, net, node);
    if (!e) {
        amt_entry *oldest = &at->amt[0];
        for (unsigned k = 0; k < AMT_SIZE && !e; k++) {
            if (!at->amt[k].used) e = &at->amt[k];
            else if (at->amt[k].when < oldest->when) oldest = &at->amt[k];
        }
        if (!e) e = oldest;
        e->used = 1;
        e->net = net;
        e->node = node;
    }
    memcpy(e->mac, mac, 6);
    e->when = now;
    flush_pending(at, net, node, mac);
}

/* A provisional address in the startup range, from the seed. */
static void pick_address(mc_atalk *at) {
    at->seed = at->seed * 1103515245u + 12345u;
    at->net = (uint16_t)(0xFF00 + (at->seed >> 16) % 0xFF);
    at->seed = at->seed * 1103515245u + 12345u;
    at->node = (uint8_t)(1 + (at->seed >> 16) % 0xFD);
}

void mc_atalk_start(mc_atalk *at, uint64_t now) {
    at->ready = 0;
    at->probing = 1;
    at->probes = 0;
    at->next_probe = now;
    mc_atalk_tick(at, now);
}

static int is_us(const mc_atalk *at, uint16_t net, uint8_t node) { return net == at->net && node == at->node; }

static void aarp_input(mc_atalk *at, const uint8_t *src_mac, const uint8_t *p, size_t n, uint64_t now) {
    if (n < 28 || get16(p) != 1 || get16(p + 2) != 0x809B || p[4] != 6 || p[5] != 4) return;
    const unsigned function = get16(p + 6);
    const uint8_t *smac = p + 8;
    const uint16_t snet = (uint16_t)get16(p + 15), tnet = (uint16_t)get16(p + 25);
    const uint8_t snode = p[17], tnode = p[27];
    (void)src_mac;
    if (function == 3) {                    /* Probe: never gleaned; a mapping to that address is stale */
        logf_(at, "AARP: %02X:%02X:%02X:%02X:%02X:%02X probes for %u.%u", smac[0], smac[1], smac[2], smac[3], smac[4], smac[5], tnet,
              tnode);
        amt_forget(at, tnet, tnode);
        if (is_us(at, tnet, tnode)) {
            if (at->ready) {
                aarp_send(at, 2, smac, smac, tnet, tnode, at->net, at->node);
                at->stats.aarp_replies++;
            } else if (memcmp(smac, at->mac, 6)) {
                logf_(at, "AARP: another node probes for %u.%u; trying another address", tnet, tnode);
                pick_address(at);
                mc_atalk_start(at, now);
            }
        }
        return;
    }
    if (function == 1) {                    /* Request: glean the sender, answer for us */
        if (snode) amt_learn(at, snet, snode, smac, now);
        if (at->ready && is_us(at, tnet, tnode)) {
            aarp_send(at, 2, smac, smac, snet, snode, at->net, at->node);
            at->stats.aarp_replies++;
        }
        return;
    }
    if (function == 2) {                    /* Response */
        if (at->probing && is_us(at, snet, snode)) {
            logf_(at, "AARP: %u.%u is taken; trying another address", snet, snode);
            pick_address(at);
            mc_atalk_start(at, now);
            return;
        }
        amt_learn(at, snet, snode, smac, now);
    }
}

/* ---- DDP ------------------------------------------------------------------- */

uint8_t mc_atalk_open(mc_atalk *at, uint8_t socket, mc_atalk_receive_fn receive, void *ctx) {
    if (!socket) {
        for (unsigned s = 128; s < 255 && !socket; s++)
            if (!at->sockets[s].receive) socket = (uint8_t)s;
        if (!socket) return 0;
    }
    if (socket == 255 || at->sockets[socket].receive) return 0;
    at->sockets[socket].receive = receive;
    at->sockets[socket].ctx = ctx;
    return socket;
}

void mc_atalk_close(mc_atalk *at, uint8_t socket) {
    at->sockets[socket].receive = NULL;
    at->sockets[socket].ctx = NULL;
}

int mc_atalk_send(mc_atalk *at, uint8_t from_socket, const mc_atalk_addr *to, uint8_t type, const uint8_t *data, size_t length,
                  uint64_t now) {
    if (!at->ready || length > MC_DDP_DATA_MAX) return 0;
    uint8_t f[FRAME_MAX];
    uint8_t *d = elap(at, f, broadcast_mac, snap_ddp);
    put16(d, (unsigned)(DDP_HEADER + length));
    put16(d + 4, to->net);
    put16(d + 6, at->net);
    d[8] = to->node;
    d[9] = at->node;
    d[10] = to->socket;
    d[11] = from_socket;
    d[12] = type;
    memcpy(d + DDP_HEADER, data, length);
    put16(d + 2, mc_ddp_checksum(d + 4, DDP_HEADER - 4 + length));
    if (at->trace)
        logf_(at, "DDP out to %u.%u:%u from :%u type %u, %u bytes: %02X %02X %02X %02X %02X %02X %02X %02X", to->net, to->node, to->socket,
              from_socket, type, (unsigned)length, length > 0 ? data[0] : 0, length > 1 ? data[1] : 0, length > 2 ? data[2] : 0,
              length > 3 ? data[3] : 0, length > 4 ? data[4] : 0, length > 5 ? data[5] : 0, length > 6 ? data[6] : 0,
              length > 7 ? data[7] : 0);
    size_t total = ELAP_HEADER + DDP_HEADER + length;
    put16(f + 12, (unsigned)(total - 14));
    while (total < 60) f[total++] = 0;
    at->stats.datagrams_out++;
    if (to->node == 0xFF) {             /* broadcasts go to the AppleTalk multicast */
        at->output(at->output_ctx, f, total);
        return 1;
    }
    if (is_us(at, to->net, to->node)) return 0;
    const amt_entry *e = amt_find(at, to->net, to->node);
    if (e) {
        memcpy(f, e->mac, 6);
        at->output(at->output_ctx, f, total);
        return 1;
    }
    /* Resolve first: the datagram waits, the request goes out. */
    pending *q = NULL;
    for (unsigned k = 0; k < PENDING && !q; k++)
        if (!at->queue[k].used) q = &at->queue[k];
    if (!q) {
        at->stats.unresolved_drops++;
        return 0;
    }
    q->used = 1;
    q->net = to->net;
    q->node = to->node;
    q->tries = 1;
    q->next = now + REQUEST_MS;
    q->length = total;
    memcpy(q->frame, f, total);
    aarp_send(at, 1, broadcast_mac, NULL, to->net, to->node, at->net, at->node);
    return 1;
}

static void nbp_input(mc_atalk *at, const mc_atalk_addr *from, const uint8_t *d, size_t n, uint64_t now);

static void aep_input(mc_atalk *at, const mc_atalk_addr *from, const uint8_t *d, size_t n, uint64_t now) {
    if (n < 1 || d[0] != 1) return;
    uint8_t reply[MC_DDP_DATA_MAX];
    memcpy(reply, d, n);
    reply[0] = 2;
    mc_atalk_send(at, MC_SOCKET_ECHO, from, MC_DDP_AEP, reply, n, now);
}

static void ddp_input(mc_atalk *at, const uint8_t *src_mac, const uint8_t *d, size_t n, uint64_t now) {
    if (n < DDP_HEADER) return;
    const size_t length = get16(d) & 0x3FF;
    if (length < DDP_HEADER || length > n) return;
    n = length;
    const unsigned sum = get16(d + 2);
    if (sum && mc_ddp_checksum(d + 4, n - 4) != sum) {
        at->stats.bad_checksums++;
        logf_(at, "DDP: bad checksum from %u.%u", get16(d + 6), d[9]);
        return;
    }
    const uint16_t dnet = (uint16_t)get16(d + 4);
    const mc_atalk_addr from = {(uint16_t)get16(d + 6), d[9], d[11]};
    const uint8_t dnode = d[8], dsocket = d[10], type = d[12];
    if (!at->ready) return;
    /* Network equivalence (zero matches anything); node: ours or broadcast. */
    if (dnet && dnet != at->net && dnode != 0xFF) return;
    if (dnode != at->node && dnode != 0xFF) return;
    if (from.node && from.node < 0xFE) amt_learn(at, from.net, from.node, src_mac, now);
    if (at->trace)
        logf_(at, "DDP in from %u.%u:%u to %u.%u:%u type %u, %u bytes: %02X %02X %02X %02X %02X %02X %02X %02X", from.net, from.node,
              from.socket, dnet, dnode, dsocket, type, (unsigned)(n - DDP_HEADER), n > 13 ? d[13] : 0, n > 14 ? d[14] : 0,
              n > 15 ? d[15] : 0, n > 16 ? d[16] : 0, n > 17 ? d[17] : 0, n > 18 ? d[18] : 0, n > 19 ? d[19] : 0, n > 20 ? d[20] : 0);
    at->stats.datagrams_in++;
    const uint8_t *data = d + DDP_HEADER;
    const size_t dlen = n - DDP_HEADER;
    if (dsocket == MC_SOCKET_NIS && type == MC_DDP_NBP) nbp_input(at, &from, data, dlen, now);
    else if (dsocket == MC_SOCKET_ECHO && type == MC_DDP_AEP) aep_input(at, &from, data, dlen, now);
    else if (at->sockets[dsocket].receive) at->sockets[dsocket].receive(at->sockets[dsocket].ctx, &from, type, data, dlen, now);
}

/* ---- NBP ------------------------------------------------------------------- */

static int set_pstring(uint8_t *dst, const char *s) {
    const size_t n = strlen(s);
    if (!n || n > MC_NBP_NAME_MAX) return 0;
    dst[0] = (uint8_t)n;
    memcpy(dst + 1, s, n);
    return 1;
}

int mc_nbp_register(mc_atalk *at, const char *object, const char *type, uint8_t socket) {
    for (unsigned k = 0; k < NAMES; k++) {
        name_entry *e = &at->names[k];
        if (e->used) continue;
        if (!set_pstring(e->object, object) || !set_pstring(e->type, type)) return 0;
        e->used = 1;
        e->socket = socket;
        e->enumerator = at->next_enumerator++;
        return 1;
    }
    return 0;
}

void mc_nbp_remove(mc_atalk *at, uint8_t socket) {
    for (unsigned k = 0; k < NAMES; k++)
        if (at->names[k].socket == socket) at->names[k].used = 0;
}

/* LkUp (and its directed form, confirm): one tuple, the requester's
 * address and the name sought. Replies list the matching names of ours. */
static void nbp_input(mc_atalk *at, const mc_atalk_addr *from, const uint8_t *d, size_t n, uint64_t now) {
    (void)from;
    if (n < 2 + 5 + 3) return;
    const unsigned function = d[0] >> 4, id = d[1];
    if (function != 2) return;          /* LkUp only: BrRq and FwdReq are for routers */
    const uint8_t *t = d + 2;
    const mc_atalk_addr reply_to = {(uint16_t)get16(t), t[2], t[3]};
    size_t at_ = 5;
    const uint8_t *obj = t + at_ + 1;
    const size_t olen = t[at_];
    at_ += 1 + olen;
    if (2 + at_ >= n) return;
    const uint8_t *typ = t + at_ + 1;
    const size_t tlen = t[at_];
    at_ += 1 + tlen;
    if (2 + at_ >= n) return;
    const uint8_t *zone = t + at_ + 1;
    const size_t zlen = t[at_];
    if (2 + at_ + 1 + zlen > n) return;
    if (!(zlen == 0 || (zlen == 1 && zone[0] == '*'))) return;
    uint8_t reply[MC_DDP_DATA_MAX];
    size_t r = 2;
    unsigned count = 0;
    const mc_atalk_addr us = mc_atalk_address(at);
    for (unsigned k = 0; k < NAMES; k++) {
        const name_entry *e = &at->names[k];
        if (!e->used || !mc_nbp_match(obj, olen, e->object + 1, e->object[0]) || !mc_nbp_match(typ, tlen, e->type + 1, e->type[0]))
            continue;
        if (r + 5 + 2 + e->object[0] + e->type[0] + 2 > sizeof reply || count == 15) break;
        put16(reply + r, us.net);
        reply[r + 2] = us.node;
        reply[r + 3] = e->socket;
        reply[r + 4] = e->enumerator;
        r += 5;
        memcpy(reply + r, e->object, 1u + e->object[0]);
        r += 1u + e->object[0];
        memcpy(reply + r, e->type, 1u + e->type[0]);
        r += 1u + e->type[0];
        reply[r++] = 1;
        reply[r++] = '*';
        count++;
    }
    if (!count) return;
    reply[0] = (uint8_t)(3 << 4 | count);
    reply[1] = (uint8_t)id;
    at->stats.lookups_answered++;
    mc_atalk_send(at, MC_SOCKET_NIS, &reply_to, MC_DDP_NBP, reply, r, now);
}

/* ---- the node -------------------------------------------------------------- */

mc_atalk *mc_atalk_create(const uint8_t mac[6], uint32_t seed, mc_atalk_output_fn output, void *output_ctx) {
    mc_atalk *at = calloc(1, sizeof *at);
    if (!at) return NULL;
    memcpy(at->mac, mac, 6);
    at->seed = seed ^ (uint32_t)mac[5] << 8 ^ mac[4];
    at->output = output;
    at->output_ctx = output_ctx;
    at->next_enumerator = 1;
    pick_address(at);
    return at;
}

void mc_atalk_destroy(mc_atalk *at) { free(at); }

void mc_atalk_set_trace(mc_atalk *at, int on) { at->trace = on; }

void mc_atalk_set_log(mc_atalk *at, mc_atalk_log_fn log, void *ctx) {
    at->log = log;
    at->log_ctx = ctx;
}

int mc_atalk_ready(const mc_atalk *at) { return at->ready; }

mc_atalk_addr mc_atalk_address(const mc_atalk *at) {
    const mc_atalk_addr a = {at->net, at->node, 0};
    return a;
}

void mc_atalk_get_stats(const mc_atalk *at, mc_atalk_stats *stats) { *stats = at->stats; }

void mc_atalk_input(mc_atalk *at, const uint8_t *f, size_t n, uint64_t now) {
    if (n < ELAP_HEADER) return;
    /* Ours, the AppleTalk broadcast and the zone multicasts (09:00:07:..). */
    if (memcmp(f, at->mac, 6) && !(f[0] == 0x09 && f[1] == 0x00 && f[2] == 0x07)) return;
    if (!memcmp(f + 6, at->mac, 6)) return;
    const size_t len = get16(f + 12);
    if (len > 1500 || 14 + len > n || len < 8) return;
    if (f[14] != 0xAA || f[15] != 0xAA || f[16] != 0x03) return;
    const uint8_t *p = f + ELAP_HEADER;
    const size_t m = 14 + len - ELAP_HEADER;
    if (!memcmp(f + 17, snap_aarp, 5)) aarp_input(at, f + 6, p, m, now);
    else if (!memcmp(f + 17, snap_ddp, 5)) ddp_input(at, f + 6, p, m, now);
}

void mc_atalk_tick(mc_atalk *at, uint64_t now) {
    if (at->probing && now >= at->next_probe) {
        if (at->probes == PROBES) {
            at->probing = 0;
            at->ready = 1;
            logf_(at, "AppleTalk: node %u.%u", at->net, at->node);
        } else {
            aarp_send(at, 3, broadcast_mac, NULL, at->net, at->node, at->net, at->node);
            at->probes++;
            at->stats.probes_sent++;
            at->next_probe = now + PROBE_MS;
        }
    }
    for (unsigned k = 0; k < PENDING; k++) {
        pending *q = &at->queue[k];
        if (!q->used || now < q->next) continue;
        if (q->tries == REQUESTS) {
            q->used = 0;
            at->stats.unresolved_drops++;
            logf_(at, "AARP: no answer from %u.%u", q->net, q->node);
            continue;
        }
        q->tries++;
        q->next = now + REQUEST_MS;
        aarp_send(at, 1, broadcast_mac, NULL, q->net, q->node, at->net, at->node);
    }
}
