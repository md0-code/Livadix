/* Livadix — the user-mode NAT (nat.h). RFC sections are cited where a rule
 * comes from.
 *
 * Livadix's own code. */
#include "nat.h"
#include "sys.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <icmpapi.h>
#else
#include <netinet/tcp.h>
#include <poll.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
/* The virtual hosts' Ethernet address: locally administered (IEEE 802). */
static const uint8_t router_mac[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x02};
static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

enum { ETH_IP = 0x0800, ETH_ARP = 0x0806 };
enum { PROTO_ICMP = 1, PROTO_TCP = 6, PROTO_UDP = 17 };
enum { TCP_FIN = 0x01, TCP_SYN = 0x02, TCP_RST = 0x04, TCP_PSH = 0x08, TCP_ACK = 0x10 };
/* TCP states, RFC 9293 3.3.2 (LISTEN is the forward's host socket). */
enum { T_FREE, T_CONNECTING, T_SYN_SENT, T_SYN_RCVD, T_ESTABLISHED, T_FIN_WAIT_1, T_FIN_WAIT_2, T_CLOSE_WAIT,
       T_CLOSING, T_LAST_ACK, T_TIME_WAIT };

#define MAX_UDP 64
#define MAX_TCP 128
#define MAX_FWD_UDP 64
#define TCP_BUF 65537u          /* a full DNS message plus its two-byte TCP length (RFC 1035 4.2.2) */
#define OOO_MAX 16              /* out-of-order segments held per connection */
#define OUR_MSS 1460            /* an Ethernet's 1500-byte MTU less 40 bytes of headers */
#define DEFAULT_MSS 536         /* RFC 9293 3.7.1: without the option */
#define RTO_INITIAL 1000        /* RFC 6298 2.1 */
#define RTO_MIN 200             /* below RFC 6298's 1 s: the Macintosh is on the same host */
#define RTO_MAX 30000
#define RETRIES 12              /* retransmissions before the connection is given up */
#define SYN_RETRIES 5
#define TIME_WAIT_MS 2000       /* a short 2 MSL: the link has no delay */
#define UDP_IDLE_MS 120000
#define UDP_MAX_PAYLOAD 65507u   /* IPv4's 65535 bytes less IP and UDP headers */
#define IP_PAYLOAD_MAX 65515u
#define NAT_MTU 1500u
#define REASSEMBLIES 8
#define REASSEMBLY_MS 60000u    /* bounded reassembly lifetime (RFC 1122 3.3.2) */
#define FWD_PORT_FIRST 49152

#define SEQ_LT(a, b) ((int32_t)((a) - (b)) < 0)
#define SEQ_LEQ(a, b) ((int32_t)((a) - (b)) <= 0)
#define SEQ_GT(a, b) ((int32_t)((a) - (b)) > 0)
#define SEQ_GEQ(a, b) ((int32_t)((a) - (b)) >= 0)

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* The Internet checksum (RFC 1071). */
static uint32_t sum_add(uint32_t sum, const uint8_t *p, size_t n) {
    for (; n > 1; n -= 2, p += 2) sum += (uint32_t)(p[0] << 8 | p[1]);
    if (n) sum += (uint32_t)p[0] << 8;
    return sum;
}
static uint16_t sum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- state ------------------------------------------------------------------- */

typedef struct udp_bind {
    int used;
    uint16_t guest_port;
    mc_socket s;
    uint64_t last;
} udp_bind;

/* A client of a UDP forward: datagrams to the Macintosh come from
 * router:nat_port, its replies to that port go back to the client. */
typedef struct udp_fwd {
    int used;
    unsigned forward;
    struct sockaddr_in client;
    uint16_t nat_port;
    uint64_t last;
} udp_fwd;

typedef struct segment {
    uint32_t seq;
    uint16_t len;
    uint8_t fin;
    uint8_t *data;
} segment;

typedef struct tcb {
    int state;
    int forwarded;              /* opened by a host connection to a forward */
    int dns, dns_pending;        /* local DNS TCP endpoint, at most one resolver job at a time */
    uint32_t cookie;            /* asynchronous replies cannot reach a reused connection */
    uint64_t dns_deadline;
    uint32_t remote;            /* the far end as the Macintosh sees it */
    uint16_t guest_port, remote_port;
    mc_socket s;
    /* Sending to the Macintosh (RFC 9293 3.3.1): tx holds the bytes from
     * tx_seq on, sent or not. */
    uint32_t iss, snd_una, snd_nxt, snd_max, snd_wnd, snd_wl1, snd_wl2;
    uint32_t tx_seq;
    uint8_t *tx;
    uint32_t tx_len;
    uint16_t mss;
    /* Receiving from the Macintosh: rx holds bytes not yet written to the host. */
    uint32_t irs, rcv_nxt, adv_wnd;
    uint8_t *rx;
    uint32_t rx_len;
    segment ooo[OOO_MAX];
    int host_eof, fin_sent, guest_fin, host_shut;
    uint32_t fin_seq;
    /* Timers (wall time, ms; 0: off). */
    uint64_t rto_at, wait_until;
    uint32_t rto, srtt, rttvar;
    int rtt_valid, timing;
    uint32_t rtt_seq;
    uint64_t rtt_start;
    unsigned backoff, dupacks;
} tcb;

typedef struct job {
    struct job *next;
    int ping;                   /* 0: resolve `name`, 1: echo to `addr` */
    char name[256];
    uint32_t addr;
    uint32_t guest_dst;         /* the echo's destination as the Macintosh addressed it */
    uint8_t *packet;            /* the DNS query or the ICMP echo request */
    size_t length;
    uint16_t guest_port;
    uint32_t tcp_cookie;        /* zero: UDP; otherwise the DNS TCP connection */
    /* Results. */
    uint32_t answers[8];
    unsigned count;
    int ok;
} job;

typedef struct ip_reassembly {
    uint32_t src, dst, received, last, highest;
    uint16_t id;
    uint8_t proto, first_ihl;
    uint64_t deadline;
    uint8_t *data, *seen;
} ip_reassembly;

struct mc_nat {
    mc_nat_config c;
    uint8_t guest_mac[6];
    int guest_mac_known;
    mc_socket wake;
    struct sockaddr_in wake_addr;
    mc_socket listener[MC_NAT_FORWARDS];
    uint16_t listen_port[MC_NAT_FORWARDS];
    udp_bind udp[MAX_UDP];
    udp_fwd ufwd[MAX_FWD_UDP];
    tcb tcp[MAX_TCP];
    uint32_t tcp_cookie;
    ip_reassembly fragments[REASSEMBLIES];
    uint16_t next_port;
    uint16_t ip_id;
    uint32_t iss_seed;
    mc_nat_stats stats;
    uint8_t frame[14 + 65535];
    /* The worker thread: getaddrinfo and ICMP echo. */
    mc_thread worker;
    int worker_running, stopping;
    mc_mutex lock;
    mc_cond cond;
    job *todo, *done;
};

static uint64_t now_ms(mc_nat *n) { return n->c.clock ? n->c.clock(n->c.clock_ctx) : mc_clock_ms(); }

static void nat_log(mc_nat *n, const char *fmt, ...) {
    if (!n->c.log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    n->c.log(n->c.log_ctx, line);
}
static const char *dotted(uint32_t a, char *buf) {
    snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16 & 255), (unsigned)(a >> 8 & 255), (unsigned)(a & 255));
    return buf;
}

static int is_local(const mc_nat *n, uint32_t a) { return (a & 0xFFFFFF00u) == n->c.network; }
/* The host address a destination of the Macintosh reaches. */
static uint32_t to_host(const mc_nat *n, uint32_t a) { return a == n->c.router ? n->c.host_loopback : a; }
/* The address the Macintosh sees for a host peer. */
static uint32_t from_host(const mc_nat *n, uint32_t a) { return (a >> 24) == 127 ? n->c.router : a; }

static void sin_set(struct sockaddr_in *sa, uint32_t addr, uint16_t port) {
    memset(sa, 0, sizeof *sa);
    sa->sin_family = AF_INET;
    sa->sin_addr.s_addr = htonl(addr);
    sa->sin_port = htons(port);
}

/* ---- frames to the Macintosh ------------------------------------------------- */

#define ETH 14
#define IPH 20

static void output(mc_nat *n, size_t length) {
    if (n->c.output) n->c.output(n->c.output_ctx, n->frame, length);
}

/* The transport header and payload are at frame + ETH + IPH; this writes
 * the Ethernet and IP headers (RFC 791 3.1) and the transport checksum. */
static void send_ip(mc_nat *n, uint32_t src, uint32_t dst, uint8_t proto, size_t length, int broadcast) {
    uint8_t *f = n->frame;
    memcpy(f, broadcast || !n->guest_mac_known ? broadcast_mac : n->guest_mac, 6);
    memcpy(f + 6, router_mac, 6);
    put16(f + 12, ETH_IP);
    uint8_t *ip = f + ETH, *t = ip + IPH;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, (uint32_t)(IPH + length));
    put16(ip + 4, n->ip_id++);
    put16(ip + 6, 0);
    ip[8] = 64;
    ip[9] = proto;
    put16(ip + 10, 0);
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, sum_fold(sum_add(0, ip, IPH)));
    if (proto == PROTO_UDP || proto == PROTO_TCP) {
        const unsigned at = proto == PROTO_UDP ? 6 : 16;
        put16(t + at, 0);
        uint32_t sum = sum_add(0, ip + 12, 8) + proto + (uint32_t)length;
        uint16_t c = sum_fold(sum_add(sum, t, length));
        if (proto == PROTO_UDP && c == 0) c = 0xFFFF;   /* RFC 768: an all-zero sum is sent as ones */
        put16(t + at, c);
    } else if (proto == PROTO_ICMP) {
        put16(t + 2, 0);
        put16(t + 2, sum_fold(sum_add(0, t, length)));
    }
    if (IPH + length <= NAT_MTU) output(n, ETH + IPH + length);
    else {
        /* RFC 791 3.1/3.2: every non-final fragment has an eight-byte
         * multiple of payload, a shared ID and its own header checksum. */
        uint8_t fragment[ETH + NAT_MTU];
        for (size_t off = 0; off < length;) {
            size_t part = length - off;
            if (part > NAT_MTU - IPH) part = (NAT_MTU - IPH) & ~7u;
            memcpy(fragment, f, ETH + IPH);
            memcpy(fragment + ETH + IPH, t + off, part);
            uint8_t *header = fragment + ETH;
            put16(header + 2, (uint32_t)(IPH + part));
            put16(header + 6, (uint32_t)(off / 8) | (off + part < length ? 0x2000u : 0u));
            put16(header + 10, 0);
            put16(header + 10, sum_fold(sum_add(0, header, IPH)));
            if (n->c.output) n->c.output(n->c.output_ctx, fragment, ETH + IPH + part);
            off += part;
        }
    }
}

static void send_udp(mc_nat *n, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *data, size_t len,
                     int broadcast) {
    if (len > UDP_MAX_PAYLOAD) return;
    uint8_t *u = n->frame + ETH + IPH;
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint32_t)(8 + len));
    if (data != u + 8) memmove(u + 8, data, len);
    send_ip(n, src, dst, PROTO_UDP, 8 + len, broadcast);
}

/* ---- the worker: getaddrinfo and ICMP echo ------------------------------------- */

static void run_job(job *j) {
    if (!j->ping) {
        struct addrinfo hints, *res = NULL;
        memset(&hints, 0, sizeof hints);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(j->name, NULL, &hints, &res) == 0) {
            for (struct addrinfo *a = res; a && j->count < 8; a = a->ai_next) {
                if (a->ai_family != AF_INET) continue;
                const uint32_t v = ntohl(((struct sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
                unsigned k;
                for (k = 0; k < j->count && j->answers[k] != v; k++) {}
                if (k == j->count) j->answers[j->count++] = v;
            }
            freeaddrinfo(res);
        }
        j->ok = 1;
        return;
    }
#ifdef _WIN32
    /* ICMP echo through the host's service (no raw socket, no rights). */
    HANDLE h = IcmpCreateFile();
    if (h == INVALID_HANDLE_VALUE) return;
    const size_t data = j->length > 8 ? j->length - 8 : 0;
    const DWORD size = (DWORD)(sizeof(ICMP_ECHO_REPLY) + data + 8 + 64);
    void *reply = malloc(size);
    if (reply && IcmpSendEcho(h, htonl(j->addr), j->packet + 8, (WORD)data, NULL, reply, size, 2000) > 0) {
        const ICMP_ECHO_REPLY *r = reply;
        j->ok = r->Status == IP_SUCCESS;
    }
    free(reply);
    IcmpCloseHandle(h);
#else
    /* ICMP echo through an unprivileged ICMP datagram socket (the kernel
     * sets the identifier and the checksum; icmp(7), ping_group_range). A
     * host that does not allow one to be opened drops the echo. */
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
    if (s < 0) return;
    struct sockaddr_in to;
    sin_set(&to, j->addr, 0);
    struct pollfd wait = {s, POLLIN, 0};
    uint8_t reply[64];                            /* only the type is read */
    if (sendto(s, j->packet, j->length, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)j->length && poll(&wait, 1, 2000) > 0) {
        const ssize_t got = recv(s, reply, sizeof reply, MSG_DONTWAIT);
        j->ok = got >= 1 && reply[0] == 0;     /* an echo reply */
    }
    close(s);
#endif
}

static MC_THREAD_RETURN worker_main(void *arg) {
    mc_nat *n = arg;
    mc_mutex_lock(&n->lock);
    for (;;) {
        while (!n->todo && !n->stopping) mc_cond_wait(&n->cond, &n->lock);
        if (n->stopping) break;
        job *j = n->todo;
        n->todo = j->next;
        mc_mutex_unlock(&n->lock);
        run_job(j);
        mc_mutex_lock(&n->lock);
        j->next = n->done;
        n->done = j;
        mc_mutex_unlock(&n->lock);
        mc_nat_wake(n);
        mc_mutex_lock(&n->lock);
    }
    mc_mutex_unlock(&n->lock);
    return 0;
}

static void submit(mc_nat *n, job *j) {
    mc_mutex_lock(&n->lock);
    job **p = &n->todo;
    while (*p) p = &(*p)->next;
    j->next = NULL;
    *p = j;
    mc_cond_signal(&n->cond);
    mc_mutex_unlock(&n->lock);
}

static void free_job(job *j) {
    free(j->packet);
    free(j);
}

/* ---- ARP (RFC 826) -------------------------------------------------------------- */

static void arp_input(mc_nat *n, const uint8_t *f, size_t len) {
    if (len < ETH + 28) return;
    const uint8_t *a = f + ETH;
    if (get16(a) != 1 || get16(a + 2) != ETH_IP || a[4] != 6 || a[5] != 4 || get16(a + 6) != 1) return;
    const uint32_t tpa = get32(a + 24);
    if (tpa != n->c.router && tpa != n->c.dns) return;
    uint8_t *o = n->frame;
    memcpy(o, a + 8, 6);
    memcpy(o + 6, router_mac, 6);
    put16(o + 12, ETH_ARP);
    uint8_t *r = o + ETH;
    put16(r, 1);
    put16(r + 2, ETH_IP);
    r[4] = 6;
    r[5] = 4;
    put16(r + 6, 2);
    memcpy(r + 8, router_mac, 6);
    put32(r + 14, tpa);
    memcpy(r + 18, a + 8, 6);
    memcpy(r + 24, a + 14, 4);
    n->stats.arp_replies++;
    output(n, ETH + 28);
}

/* ---- DHCP (RFC 2131, 2132) and BOOTP (RFC 951, 1497) ---------------------------- */

#define BOOTP_MIN 300           /* RFC 951: the 64-byte vendor area makes a 300-byte message */
static const uint8_t dhcp_cookie[4] = {99, 130, 83, 99};
enum { DHCPDISCOVER = 1, DHCPOFFER, DHCPREQUEST, DHCPDECLINE, DHCPACK, DHCPNAK, DHCPRELEASE, DHCPINFORM };

static const uint8_t *dhcp_option(const uint8_t *b, size_t len, uint8_t code, uint8_t *olen) {
    if (len < 240 || memcmp(b + 236, dhcp_cookie, 4)) return NULL;
    for (size_t i = 240; i < len;) {
        const uint8_t c = b[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len || i + 2 + b[i + 1] > len) break;
        if (c == code) { *olen = b[i + 1]; return b + i + 2; }
        i += 2u + b[i + 1];
    }
    return NULL;
}

static uint8_t *put_option(uint8_t *p, uint8_t code, uint8_t len, const void *data) {
    p[0] = code;
    p[1] = len;
    memcpy(p + 2, data, len);
    return p + 2 + len;
}
static uint8_t *put_option32(uint8_t *p, uint8_t code, uint32_t v) {
    uint8_t b[4];
    put32(b, v);
    return put_option(p, code, 4, b);
}

static void dhcp_input(mc_nat *n, const uint8_t *b, size_t len) {
    if (len < 236 || b[0] != 1 || b[1] != 1 || b[2] != 6) return;
    uint8_t olen = 0;
    const uint8_t *type = dhcp_option(b, len, 53, &olen);
    const int dhcp = type && olen == 1;
    int reply_type = 0;
    const uint32_t ciaddr = get32(b + 12);
    uint32_t yiaddr = n->c.guest;
    char t1[16];
    memcpy(n->guest_mac, b + 28, 6);   /* the client asking for the Macintosh's address */
    n->guest_mac_known = 1;
    if (dhcp) {
        const uint8_t *sid = dhcp_option(b, len, 54, &olen);
        if (sid && olen == 4 && get32(sid) != n->c.router) return;   /* for another server */
        switch (*type) {
        case DHCPDISCOVER: reply_type = DHCPOFFER; break;
        case DHCPREQUEST: {
            const uint8_t *req = dhcp_option(b, len, 50, &olen);
            const uint32_t want = req && olen == 4 ? get32(req) : ciaddr;
            reply_type = want == n->c.guest ? DHCPACK : DHCPNAK;
            break;
        }
        case DHCPINFORM: reply_type = DHCPACK; yiaddr = 0; break;
        case DHCPDECLINE: nat_log(n, "dhcp: the Macintosh declined %s", dotted(n->c.guest, t1)); return;
        default: return;   /* RELEASE */
        }
    }
    uint8_t *u = n->frame + ETH + IPH, *r = u + 8;
    memset(r, 0, 548);
    r[0] = 2;   /* BOOTREPLY */
    r[1] = 1;
    r[2] = 6;
    memcpy(r + 4, b + 4, 4);         /* xid */
    memcpy(r + 10, b + 10, 2);       /* flags */
    memcpy(r + 12, b + 12, 4);       /* ciaddr */
    if (reply_type != DHCPNAK) put32(r + 16, yiaddr);
    put32(r + 20, n->c.router);      /* siaddr */
    memcpy(r + 24, b + 24, 4);       /* giaddr */
    memcpy(r + 28, b + 28, 16);      /* chaddr */
    memcpy(r + 236, dhcp_cookie, 4);
    uint8_t *p = r + 240;
    if (dhcp) {
        const uint8_t t = (uint8_t)reply_type;
        p = put_option(p, 53, 1, &t);
        p = put_option32(p, 54, n->c.router);
    }
    if (reply_type != DHCPNAK) {
        p = put_option32(p, 1, 0xFFFFFF00u);
        p = put_option32(p, 3, n->c.router);
        p = put_option32(p, 6, n->c.dns);
        p = put_option32(p, 28, n->c.network | 0xFF);
        if (dhcp && yiaddr) {
            p = put_option32(p, 51, 86400);
            p = put_option32(p, 58, 43200);
            p = put_option32(p, 59, 75600);
        }
    }
    *p++ = 255;
    size_t size = (size_t)(p - r);
    if (size < BOOTP_MIN) size = BOOTP_MIN;
    /* RFC 2131 4.1: to ciaddr when the client has one, else broadcast (the
     * client has no address to receive a unicast at). */
    const int bcast = ciaddr == 0 || reply_type == DHCPNAK;
    if (dhcp) {
        if (reply_type == DHCPOFFER) n->stats.dhcp_offers++;
        else if (reply_type == DHCPACK) n->stats.dhcp_acks++;
        if (reply_type == DHCPNAK) nat_log(n, "dhcp: nak (only %s is given out)", dotted(n->c.guest, t1));
        else nat_log(n, "dhcp: %s %s", reply_type == DHCPOFFER ? "offer" : "ack", yiaddr ? dotted(yiaddr, t1) : "(inform)");
    } else {
        n->stats.bootp_replies++;
        nat_log(n, "bootp: reply %s", dotted(yiaddr, t1));
    }
    send_udp(n, n->c.router, 67, bcast ? 0xFFFFFFFFu : ciaddr, 68, r, size, bcast);
}

/* ---- DNS (RFC 1035) --------------------------------------------------------------- */

/* The first question's name (dotted), and the offset after its class. */
static size_t dns_question(const uint8_t *q, size_t len, char *name, size_t cap, uint16_t *qtype) {
    size_t i = 12, o = 0;
    while (i < len && q[i]) {
        const unsigned l = q[i];
        if (l > 63 || i + 1 + l > len || o + l + 2 > cap) return 0;
        if (o) name[o++] = '.';
        memcpy(name + o, q + i + 1, l);
        o += l;
        i += 1 + l;
    }
    if (i + 5 > len) return 0;
    name[o] = 0;
    *qtype = get16(q + i + 1);
    return i + 5;
}

static void tcp_output(mc_nat *n, tcb *t, unsigned limit, int probe);

static tcb *dns_connection(mc_nat *n, uint32_t cookie) {
    for (unsigned k = 0; k < MAX_TCP; k++)
        if (n->tcp[k].state && n->tcp[k].dns && n->tcp[k].cookie == cookie) return &n->tcp[k];
    return NULL;
}

static void dns_reply(mc_nat *n, const uint8_t *q, size_t qlen, uint16_t guest_port, uint32_t cookie,
                      unsigned rcode, const uint32_t *answers, unsigned count) {
    uint8_t reply[512];
    uint8_t *r = reply;
    char name[256];
    uint16_t qtype = 0;
    size_t end = dns_question(q, qlen, name, sizeof name, &qtype);
    if (!end) end = 12;
    if (end + 16u * count > sizeof reply) count = 0;
    memcpy(r, q, end);
    put16(r + 2, 0x8000u | (get16(q + 2) & 0x7900u) | 0x0080u | rcode);   /* QR, opcode, RD; RA */
    put16(r + 4, end > 12 ? 1 : 0);
    put16(r + 6, count);
    put16(r + 8, 0);
    put16(r + 10, 0);
    uint8_t *p = r + end;
    for (unsigned k = 0; k < count; k++, p += 16) {
        put16(p, 0xC00C);            /* the question's name */
        put16(p + 2, 1);             /* A */
        put16(p + 4, 1);             /* IN */
        put32(p + 6, 300);
        put16(p + 10, 4);
        put32(p + 12, from_host(n, answers[k]));
    }
    if (count) n->stats.dns_answers++;
    const uint32_t length = (uint32_t)(p - r);
    if (!cookie) send_udp(n, n->c.dns, 53, n->c.guest, guest_port, r, length, 0);
    else {
        tcb *t = dns_connection(n, cookie);
        if (!t) return;
        put16(t->tx + t->tx_len, length);
        memcpy(t->tx + t->tx_len + 2, r, length);
        t->tx_len += length + 2;
        t->dns_pending = 0;
        t->dns_deadline = now_ms(n) + UDP_IDLE_MS;
        tcp_output(n, t, 64, 0);
    }
}

static void dns_input(mc_nat *n, uint16_t guest_port, uint32_t cookie, const uint8_t *q, size_t len) {
    if (len < 12 || (q[2] & 0x80)) {
        if (cookie) { tcb *t = dns_connection(n, cookie); if (t) t->dns_pending = 0; }
        return;
    }
    n->stats.dns_queries++;
    char name[256];
    uint16_t qtype = 0;
    const size_t end = dns_question(q, len, name, sizeof name, &qtype);
    if (get16(q + 4) != 1 || !end || ((q[2] >> 3) & 15) != 0) { dns_reply(n, q, len, guest_port, cookie, 4, NULL, 0); return; }
    if (qtype == 28) { dns_reply(n, q, len, guest_port, cookie, 0, NULL, 0); return; }   /* AAAA: none */
    if (qtype != 1 || get16(q + end - 2) != 1) { dns_reply(n, q, len, guest_port, cookie, 4, NULL, 0); return; }
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->packet = malloc(len))) {
        free(j);
        dns_reply(n, q, len, guest_port, cookie, 2, NULL, 0);
        return;
    }
    j->tcp_cookie = cookie;
    snprintf(j->name, sizeof j->name, "%s", name);
    memcpy(j->packet, q, len);
    j->length = len;
    j->guest_port = guest_port;
    submit(n, j);
}

/* ---- ICMP echo (RFC 792) ---------------------------------------------------------------- */

static void icmp_echo_reply(mc_nat *n, uint32_t from, const uint8_t *req, size_t len) {
    uint8_t *t = n->frame + ETH + IPH;
    if (len > IP_PAYLOAD_MAX) return;
    memmove(t, req, len);
    t[0] = 0;   /* echo reply */
    t[1] = 0;
    n->stats.icmp_replies++;
    send_ip(n, from, n->c.guest, PROTO_ICMP, len, 0);
}

static void icmp_input(mc_nat *n, uint32_t dst, const uint8_t *t, size_t len) {
    if (len < 8 || t[0] != 8) return;
    if (dst == n->c.router || dst == n->c.dns) { icmp_echo_reply(n, dst, t, len); return; }
    if (is_local(n, dst) || dst == 0xFFFFFFFFu) return;
    job *j = calloc(1, sizeof *j);
    if (!j || !(j->packet = malloc(len))) { free(j); return; }
    j->ping = 1;
    j->addr = to_host(n, dst);
    j->guest_dst = dst;
    memcpy(j->packet, t, len);
    j->length = len;
    submit(n, j);
}

static void finish_jobs(mc_nat *n) {
    mc_mutex_lock(&n->lock);
    job *j = n->done;
    n->done = NULL;
    mc_mutex_unlock(&n->lock);
    while (j) {
        job *next = j->next;
        if (j->ping) {
            if (j->ok) icmp_echo_reply(n, j->guest_dst, j->packet, j->length);
        } else {
            char t1[16];
            nat_log(n, "dns: %s: %u answer(s)%s%s", j->name, j->count, j->count ? ", " : "", j->count ? dotted(j->answers[0], t1) : "");
            dns_reply(n, j->packet, j->length, j->guest_port, j->tcp_cookie, j->count ? 0 : 3, j->answers, j->count);
        }
        free_job(j);
        j = next;
    }
}

/* ---- UDP (RFC 768) ----------------------------------------------------------------- */

static void udp_socket_options(mc_socket s) {
    mc_set_nonblocking(s);
#ifdef _WIN32
    /* An ICMP port unreachable would otherwise fail the next recvfrom. */
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &off, sizeof off, NULL, 0, &got, NULL, NULL);   /* SIO_UDP_CONNRESET */
#endif
}

static udp_bind *udp_for(mc_nat *n, uint16_t guest_port) {
    udp_bind *free_slot = NULL;
    for (unsigned k = 0; k < MAX_UDP; k++) {
        if (n->udp[k].used && n->udp[k].guest_port == guest_port) return &n->udp[k];
        if (!n->udp[k].used && !free_slot) free_slot = &n->udp[k];
    }
    if (!free_slot) {
        /* The longest idle one makes room. */
        free_slot = &n->udp[0];
        for (unsigned k = 1; k < MAX_UDP; k++) if (n->udp[k].last < free_slot->last) free_slot = &n->udp[k];
        mc_close_socket(free_slot->s);
        free_slot->used = 0;
    }
    mc_socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == MC_BAD_SOCKET) return NULL;
    struct sockaddr_in sa;
    sin_set(&sa, 0, 0);
    if (bind(s, (struct sockaddr *)&sa, sizeof sa) != 0) { mc_close_socket(s); return NULL; }
    udp_socket_options(s);
    free_slot->used = 1;
    free_slot->guest_port = guest_port;
    free_slot->s = s;
    return free_slot;
}

static void udp_input(mc_nat *n, uint32_t src, uint32_t dst, const uint8_t *u, size_t len) {
    if (len < 8) return;
    const uint16_t sport = get16(u), dport = get16(u + 2);
    size_t ulen = get16(u + 4);
    if (ulen < 8 || ulen > len) return;
    const uint8_t *data = u + 8;
    ulen -= 8;
    if (dport == 67) { dhcp_input(n, data, ulen); return; }
    if (dst == n->c.dns && dport == 53) { dns_input(n, sport, 0, data, ulen); return; }
    if (src != n->c.guest) return;
    /* A reply to a forward's client. */
    if (dst == n->c.router)
        for (unsigned k = 0; k < MAX_FWD_UDP; k++) {
            udp_fwd *f = &n->ufwd[k];
            if (f->used && f->nat_port == dport && n->c.forward[f->forward].guest_port == sport) {
                sendto(n->listener[f->forward], (const char *)data, (int)ulen, 0, (struct sockaddr *)&f->client, sizeof f->client);
                f->last = now_ms(n);
                n->stats.udp_out++;
                return;
            }
        }
    if (is_local(n, dst) && dst != n->c.router) return;
    if (dst == 0xFFFFFFFFu || (dst >> 28) == 14) return;   /* broadcast, multicast */
    udp_bind *b = udp_for(n, sport);
    if (!b) return;
    b->last = now_ms(n);
    struct sockaddr_in to;
    sin_set(&to, to_host(n, dst), dport);
    sendto(b->s, (const char *)data, (int)ulen, 0, (struct sockaddr *)&to, sizeof to);
    n->stats.udp_out++;
}

static void udp_receive(mc_nat *n, udp_bind *b) {
    uint8_t *data = n->frame + ETH + IPH + 8;
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int got = recvfrom(b->s, (char *)data, UDP_MAX_PAYLOAD, 0, (struct sockaddr *)&from, &fl);
        if (got < 0) break;
        b->last = now_ms(n);
        n->stats.udp_in++;
        send_udp(n, from_host(n, ntohl(from.sin_addr.s_addr)), ntohs(from.sin_port), n->c.guest, b->guest_port, data, (size_t)got, 0);
    }
}

static uint16_t new_nat_port(mc_nat *n);

static void udp_forward_receive(mc_nat *n, unsigned fi) {
    uint8_t *data = n->frame + ETH + IPH + 8;
    for (int k = 0; k < 64; k++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        const int got = recvfrom(n->listener[fi], (char *)data, UDP_MAX_PAYLOAD, 0, (struct sockaddr *)&from, &fl);
        if (got < 0) break;
        udp_fwd *f = NULL, *spare = NULL;
        for (unsigned j = 0; j < MAX_FWD_UDP; j++) {
            udp_fwd *e = &n->ufwd[j];
            if (e->used && e->forward == fi && e->client.sin_addr.s_addr == from.sin_addr.s_addr && e->client.sin_port == from.sin_port) f = e;
            if (!e->used && !spare) spare = e;
        }
        if (!f) {
            if (!spare) {
                spare = &n->ufwd[0];
                for (unsigned j = 1; j < MAX_FWD_UDP; j++) if (n->ufwd[j].last < spare->last) spare = &n->ufwd[j];
            }
            f = spare;
            f->used = 1;
            f->forward = fi;
            f->client = from;
            f->nat_port = new_nat_port(n);
        }
        f->last = now_ms(n);
        n->stats.udp_in++;
        send_udp(n, n->c.router, f->nat_port, n->c.guest, n->c.forward[fi].guest_port, data, (size_t)got, 0);
    }
}

/* ---- TCP (RFC 793, RFC 9293) ------------------------------------------------------- */

static uint32_t our_window(const tcb *t) {
    const uint32_t room = TCP_BUF - t->rx_len;
    return room > 65535u ? 65535u : room;
}

static void tcp_segment(mc_nat *n, tcb *t, uint32_t seq, uint8_t flags, const uint8_t *data, size_t len) {
    uint8_t *h = n->frame + ETH + IPH;
    const int syn = (flags & TCP_SYN) != 0;
    const unsigned hl = syn ? 24u : 20u;
    put16(h, t->remote_port);
    put16(h + 2, t->guest_port);
    put32(h + 4, seq);
    put32(h + 8, (flags & TCP_ACK) ? t->rcv_nxt : 0);
    h[12] = (uint8_t)((hl / 4) << 4);
    h[13] = flags;
    const uint32_t wnd = our_window(t);
    put16(h + 14, wnd > 65535 ? 65535 : wnd);
    put16(h + 16, 0);
    put16(h + 18, 0);
    if (syn) { h[20] = 2; h[21] = 4; put16(h + 22, OUR_MSS); }   /* the MSS option (RFC 9293 3.7.1) */
    if (len) memcpy(h + hl, data, len);
    t->adv_wnd = wnd;
    send_ip(n, t->remote, n->c.guest, PROTO_TCP, hl + len, 0);
}

/* A reset for a segment that has no connection (RFC 9293 3.10.7.1). */
static void tcp_reset_for(mc_nat *n, uint32_t remote, uint16_t rport, uint16_t gport, uint32_t seq, uint32_t ack, int has_ack,
                          size_t seglen) {
    tcb t;
    memset(&t, 0, sizeof t);
    t.remote = remote;
    t.remote_port = rport;
    t.guest_port = gport;
    t.rx_len = TCP_BUF;   /* window 0 */
    if (has_ack) tcp_segment(n, &t, ack, TCP_RST, NULL, 0);
    else {
        t.rcv_nxt = seq + (uint32_t)seglen;
        tcp_segment(n, &t, 0, TCP_RST | TCP_ACK, NULL, 0);
    }
}

static void tcp_free(mc_nat *n, tcb *t, int abort_host) {
    char t1[16];
    if (t->s != MC_BAD_SOCKET) {
        if (abort_host) {
            struct linger lg = {1, 0};   /* a reset to the host's peer */
            setsockopt(t->s, SOL_SOCKET, SO_LINGER, (const char *)&lg, sizeof lg);
        }
        mc_close_socket(t->s);
    }
    for (unsigned k = 0; k < OOO_MAX; k++) free(t->ooo[k].data);
    free(t->tx);
    free(t->rx);
    nat_log(n, "tcp: %s:%u closed", dotted(t->remote, t1), (unsigned)t->remote_port);
    n->stats.tcp_closed++;
    memset(t, 0, sizeof *t);
    t->s = MC_BAD_SOCKET;
}

static void tcp_abort(mc_nat *n, tcb *t) {
    tcp_segment(n, t, t->snd_nxt, TCP_RST | TCP_ACK, NULL, 0);
    tcp_free(n, t, 1);
}

static void rto_arm(mc_nat *n, tcb *t) {
    uint32_t rto = t->rto << (t->backoff > 8 ? 8 : t->backoff);
    if (rto > RTO_MAX) rto = RTO_MAX;
    t->rto_at = now_ms(n) + rto;
}

/* RFC 6298 2.2, 2.3. */
static void rtt_sample(tcb *t, uint32_t r) {
    if (!t->rtt_valid) {
        t->srtt = r;
        t->rttvar = r / 2;
        t->rtt_valid = 1;
    } else {
        const uint32_t d = t->srtt > r ? t->srtt - r : r - t->srtt;
        t->rttvar = (3 * t->rttvar + d) / 4;
        t->srtt = (7 * t->srtt + r) / 8;
    }
    uint32_t rto = t->srtt + (4 * t->rttvar > 10 ? 4 * t->rttvar : 10);
    if (rto < RTO_MIN) rto = RTO_MIN;
    if (rto > RTO_MAX) rto = RTO_MAX;
    t->rto = rto;
}

/* Sends what the window allows (RFC 9293 3.8.6), then the FIN once the
 * host is done and every byte is out. `limit` caps the segments sent. */
static void tcp_output(mc_nat *n, tcb *t, unsigned limit, int probe) {
    if (t->state < T_ESTABLISHED || t->state == T_TIME_WAIT) return;
    unsigned sent = 0;
    for (; sent < limit; sent++) {
        const uint32_t off = t->snd_nxt - t->tx_seq;
        if (SEQ_LT(t->snd_nxt, t->tx_seq) || off >= t->tx_len) break;
        const uint32_t inflight = t->snd_nxt - t->snd_una;
        uint32_t room = t->snd_wnd > inflight ? t->snd_wnd - inflight : 0;
        if (probe && !room && !inflight) room = 1;   /* a zero-window probe (RFC 9293 3.8.6.1) */
        if (!room) break;
        uint32_t len = t->tx_len - off;
        if (len > room) len = room;
        if (len > t->mss) len = t->mss;
        const int last = off + len == t->tx_len;
        tcp_segment(n, t, t->snd_nxt, (uint8_t)(TCP_ACK | (last ? TCP_PSH : 0)), t->tx + off, len);
        if (!t->timing && t->snd_nxt == t->snd_max) { t->timing = 1; t->rtt_seq = t->snd_nxt; t->rtt_start = now_ms(n); }
        t->snd_nxt += len;
        if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        if (!t->rto_at) rto_arm(n, t);
    }
    if (sent < limit && t->host_eof && !t->fin_sent && t->snd_nxt == t->tx_seq + t->tx_len &&
        t->state != T_FIN_WAIT_2 && t->state != T_TIME_WAIT) {
        tcp_segment(n, t, t->snd_nxt, TCP_FIN | TCP_ACK, NULL, 0);
        t->fin_seq = t->snd_nxt;
        t->fin_sent = 1;
        t->snd_nxt++;
        if (SEQ_GT(t->snd_nxt, t->snd_max)) t->snd_max = t->snd_nxt;
        if (t->state == T_ESTABLISHED) t->state = T_FIN_WAIT_1;
        else if (t->state == T_CLOSE_WAIT) t->state = T_LAST_ACK;
        if (!t->rto_at) rto_arm(n, t);
    }
    /* A zero window with data waiting: probe it on the retransmission
       timer (RFC 9293 3.8.6.1). */
    if (!t->rto_at && t->snd_una == t->snd_max && t->snd_wnd == 0 && t->tx_len > t->snd_nxt - t->tx_seq) rto_arm(n, t);
}

static tcb *tcp_new(mc_nat *n) {
    for (unsigned k = 0; k < MAX_TCP; k++) {
        tcb *t = &n->tcp[k];
        if (t->state != T_FREE) continue;
        memset(t, 0, sizeof *t);
        t->s = MC_BAD_SOCKET;
        if (++n->tcp_cookie == 0) ++n->tcp_cookie;
        t->cookie = n->tcp_cookie;
        t->tx = malloc(TCP_BUF);
        t->rx = malloc(TCP_BUF);
        if (!t->tx || !t->rx) { free(t->tx); free(t->rx); t->tx = t->rx = NULL; return NULL; }
        n->iss_seed = n->iss_seed * 1103515245u + 12345u + (uint32_t)now_ms(n);
        t->iss = n->iss_seed;
        t->rto = RTO_INITIAL;
        t->mss = DEFAULT_MSS;
        return t;
    }
    return NULL;
}

static uint16_t new_nat_port(mc_nat *n) {
    for (;;) {
        const uint16_t p = n->next_port;
        n->next_port = (uint16_t)(n->next_port == 65535 ? FWD_PORT_FIRST : n->next_port + 1);
        int used = 0;
        for (unsigned k = 0; k < MAX_TCP && !used; k++) used = n->tcp[k].state && n->tcp[k].remote_port == p && n->tcp[k].remote == n->c.router;
        for (unsigned k = 0; k < MAX_FWD_UDP && !used; k++) used = n->ufwd[k].used && n->ufwd[k].nat_port == p;
        if (!used) return p;
    }
}

static void parse_mss(tcb *t, const uint8_t *h, unsigned hl) {
    for (unsigned i = 20; i < hl;) {
        const uint8_t kind = h[i];
        if (kind == 0) break;
        if (kind == 1) { i++; continue; }
        if (i + 1 >= hl || h[i + 1] < 2 || i + h[i + 1] > hl) break;
        if (kind == 2 && h[i + 1] == 4) {
            uint16_t m = get16(h + i + 2);
            if (m > OUR_MSS) m = OUR_MSS;
            if (m >= 64) t->mss = m;
        }
        i += h[i + 1];
    }
}

/* A SYN from the Macintosh: the host connects first; the SYN-ACK follows
 * when that succeeds (RFC 9293 3.10.7.2, the LISTEN case). */
static void tcp_open(mc_nat *n, uint32_t dst, uint16_t dport, uint16_t sport, const uint8_t *h, unsigned hl) {
    char t1[16];
    const uint32_t seq = get32(h + 4);
    const int local_dns = dst == n->c.dns && dport == 53;
    if ((!local_dns && is_local(n, dst) && dst != n->c.router) || dst == 0xFFFFFFFFu || (dst >> 28) >= 14) {
        tcp_reset_for(n, dst, dport, sport, seq, 0, 0, 1);
        return;
    }
    tcb *t = tcp_new(n);
    if (!t) { tcp_reset_for(n, dst, dport, sport, seq, 0, 0, 1); return; }
    t->remote = dst;
    t->remote_port = dport;
    t->guest_port = sport;
    t->irs = seq;
    t->rcv_nxt = seq + 1;
    t->snd_wnd = get16(h + 14);
    parse_mss(t, h, hl);
    t->snd_una = t->snd_nxt = t->snd_max = t->iss;
    t->tx_seq = t->iss + 1;
    if (local_dns) {
        t->dns = 1;
        t->dns_deadline = now_ms(n) + UDP_IDLE_MS;
        t->state = T_SYN_RCVD;
        tcp_segment(n, t, t->iss, TCP_SYN | TCP_ACK, NULL, 0);
        t->snd_nxt = t->snd_max = t->iss + 1;
        rto_arm(n, t);
        return;
    }
    t->s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->s == MC_BAD_SOCKET) { tcp_reset_for(n, dst, dport, sport, seq, 0, 0, 1); tcp_free(n, t, 0); return; }
    mc_set_nonblocking(t->s);
    int one = 1;
    setsockopt(t->s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    struct sockaddr_in to;
    sin_set(&to, to_host(n, dst), dport);
    t->state = T_CONNECTING;
    if (connect(t->s, (struct sockaddr *)&to, sizeof to) != 0 && !mc_would_block(mc_socket_error())) {
        n->stats.tcp_refused++;
        tcp_reset_for(n, dst, dport, sport, seq, 0, 0, 1);
        tcp_free(n, t, 0);
        return;
    }
    nat_log(n, "tcp: connecting to %s:%u for port %u", dotted(dst, t1), (unsigned)dport, (unsigned)sport);
}

static void ooo_merge(mc_nat *n, tcb *t);
static void guest_fin(mc_nat *n, tcb *t);

/* Data and FIN of a segment, in sequence order (RFC 9293 3.10.7.4,
 * "seventh, process the segment text"). */
static void tcp_data(mc_nat *n, tcb *t, uint32_t seq, const uint8_t *data, uint32_t len, int fin) {
    if (t->guest_fin) return;
    if (SEQ_LT(seq, t->rcv_nxt)) {
        const uint32_t d = t->rcv_nxt - seq;
        if (d > len) return;   /* all old (a FIN already taken too) */
        data += d;
        len -= d;
        seq = t->rcv_nxt;
    }
    const uint32_t wnd = our_window(t);
    if (len > wnd - (seq - t->rcv_nxt) || SEQ_GEQ(seq, t->rcv_nxt + wnd)) {
        if (SEQ_GEQ(seq, t->rcv_nxt + wnd)) return;
        len = wnd - (seq - t->rcv_nxt);
        fin = 0;
    }
    if (seq != t->rcv_nxt) {
        /* Ahead of what is expected: held until the gap is filled. */
        for (unsigned k = 0; k < OOO_MAX; k++) {
            segment *s = &t->ooo[k];
            if (s->data && s->seq == seq && s->len >= len) return;
        }
        for (unsigned k = 0; k < OOO_MAX; k++) {
            segment *s = &t->ooo[k];
            if (s->data) continue;
            s->data = malloc(len ? len : 1);
            if (!s->data) return;
            memcpy(s->data, data, len);
            s->seq = seq;
            s->len = (uint16_t)len;
            s->fin = (uint8_t)fin;
            return;
        }
        return;
    }
    memcpy(t->rx + t->rx_len, data, len);
    t->rx_len += len;
    t->rcv_nxt += len;
    n->stats.tcp_bytes_out += len;
    if (fin) guest_fin(n, t);
    else ooo_merge(n, t);
}

static void ooo_merge(mc_nat *n, tcb *t) {
    for (int again = 1; again && !t->guest_fin;) {
        again = 0;
        for (unsigned k = 0; k < OOO_MAX; k++) {
            segment *s = &t->ooo[k];
            if (!s->data) continue;
            if (SEQ_GT(s->seq, t->rcv_nxt)) continue;
            const uint32_t d = t->rcv_nxt - s->seq;
            if (d <= s->len) {
                uint32_t len = s->len - d;
                if (len > our_window(t)) len = our_window(t);
                memcpy(t->rx + t->rx_len, s->data + d, len);
                t->rx_len += len;
                t->rcv_nxt += len;
                if (s->fin && d + len == s->len) guest_fin(n, t);
                again = 1;
            }
            free(s->data);
            s->data = NULL;
        }
    }
}

static void guest_fin(mc_nat *n, tcb *t) {
    char t1[16];
    (void)t1;
    if (!t->guest_fin) t->rcv_nxt++;
    t->guest_fin = 1;
    switch (t->state) {
    case T_SYN_RCVD:
    case T_ESTABLISHED: t->state = T_CLOSE_WAIT; break;
    case T_FIN_WAIT_1: t->state = T_CLOSING; break;
    case T_FIN_WAIT_2:
        t->state = T_TIME_WAIT;
        t->wait_until = now_ms(n) + TIME_WAIT_MS;
        break;
    default: break;
    }
}

/* The FIN we sent was acknowledged. */
static int fin_acked(mc_nat *n, tcb *t) {
    switch (t->state) {
    case T_FIN_WAIT_1: t->state = T_FIN_WAIT_2; break;
    case T_CLOSING:
        t->state = T_TIME_WAIT;
        t->wait_until = now_ms(n) + TIME_WAIT_MS;
        break;
    case T_LAST_ACK: tcp_free(n, t, 0); return 0;
    default: break;
    }
    return 1;
}

static tcb *tcp_find(mc_nat *n, uint32_t remote, uint16_t rport, uint16_t gport) {
    for (unsigned k = 0; k < MAX_TCP; k++) {
        tcb *t = &n->tcp[k];
        if (t->state && t->remote == remote && t->remote_port == rport && t->guest_port == gport) return t;
    }
    return NULL;
}

static void dns_tcp_pump(mc_nat *n, tcb *t);

static void tcp_input(mc_nat *n, uint32_t src, uint32_t dst, const uint8_t *h, size_t len) {
    if (len < 20 || src != n->c.guest) return;
    const unsigned hl = (unsigned)(h[12] >> 4) * 4u;
    if (hl < 20 || hl > len) return;
    const uint16_t sport = get16(h), dport = get16(h + 2);
    const uint32_t seq = get32(h + 4), ack = get32(h + 8);
    const uint8_t flags = h[13];
    const uint32_t wnd = get16(h + 14);
    const uint8_t *data = h + hl;
    const uint32_t dlen = (uint32_t)(len - hl);
    tcb *t = tcp_find(n, dst, dport, sport);
    if (t && t->state == T_TIME_WAIT && (flags & TCP_SYN) && !(flags & TCP_ACK) && SEQ_GT(seq, t->rcv_nxt)) {
        tcp_free(n, t, 0);   /* a new connection on the old ports (RFC 9293 3.10.7.4) */
        t = NULL;
    }
    if (!t) {
        if (flags & TCP_RST) return;
        if ((flags & TCP_SYN) && !(flags & TCP_ACK)) tcp_open(n, dst, dport, sport, h, hl);
        else tcp_reset_for(n, dst, dport, sport, seq, ack, (flags & TCP_ACK) != 0, dlen + ((flags & TCP_SYN) ? 1u : 0u) + ((flags & TCP_FIN) ? 1u : 0u));
        return;
    }
    if (t->state == T_CONNECTING) return;   /* the SYN-ACK waits for the host's connect */
    if (t->state == T_SYN_SENT) {
        /* Our SYN to the Macintosh, for a forward (RFC 9293 3.10.7.3). */
        if ((flags & TCP_ACK) && ack != t->iss + 1) {
            if (!(flags & TCP_RST)) tcp_reset_for(n, dst, dport, sport, seq, ack, 1, 0);
            return;
        }
        if (flags & TCP_RST) { if (flags & TCP_ACK) tcp_free(n, t, 1); return; }
        if (!(flags & TCP_SYN) || !(flags & TCP_ACK)) return;
        t->irs = seq;
        t->rcv_nxt = seq + 1;
        t->snd_una = t->snd_nxt = t->snd_max = t->iss + 1;
        t->snd_wnd = wnd;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        parse_mss(t, h, hl);
        t->state = T_ESTABLISHED;
        t->rto_at = 0;
        t->backoff = 0;
        n->stats.tcp_opened++;
        tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0);
        tcp_output(n, t, 64, 0);
        return;
    }
    /* Synchronized states: the acceptability test (RFC 9293 3.10.7.4,
     * "first, check sequence number"). */
    const uint32_t seglen = dlen + ((flags & TCP_SYN) ? 1u : 0u) + ((flags & TCP_FIN) ? 1u : 0u);
    const uint32_t rwnd = our_window(t);
    int acceptable;
    if (!seglen) acceptable = rwnd ? SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + rwnd) : seq == t->rcv_nxt;
    else acceptable = rwnd && ((SEQ_GEQ(seq, t->rcv_nxt) && SEQ_LT(seq, t->rcv_nxt + rwnd)) ||
                               (SEQ_GEQ(seq + seglen - 1, t->rcv_nxt) && SEQ_LT(seq + seglen - 1, t->rcv_nxt + rwnd)));
    if (t->state == T_SYN_RCVD && (flags & TCP_SYN) && !(flags & TCP_ACK) && seq == t->irs) {
        tcp_segment(n, t, t->iss, TCP_SYN | TCP_ACK, NULL, 0);   /* the Macintosh repeated its SYN */
        return;
    }
    if (!acceptable) {
        if (!(flags & TCP_RST)) tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0);
        return;
    }
    if (flags & TCP_RST) { tcp_free(n, t, 1); return; }   /* second, the RST bit */
    if (flags & TCP_SYN) { tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0); return; }   /* challenge ACK (RFC 5961, in 9293) */
    if (!(flags & TCP_ACK)) return;
    /* Fifth, the ACK field. */
    if (t->state == T_SYN_RCVD) {
        if (ack != t->iss + 1) { tcp_reset_for(n, dst, dport, sport, seq, ack, 1, 0); return; }
        t->state = T_ESTABLISHED;
        t->snd_una = t->snd_nxt = t->snd_max = t->iss + 1;
        t->snd_wnd = wnd;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
        t->rto_at = 0;
        t->backoff = 0;
        n->stats.tcp_opened++;
    }
    if (SEQ_GT(ack, t->snd_max)) { tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0); return; }
    if (SEQ_GT(ack, t->snd_una)) {
        if (t->timing && SEQ_GT(ack, t->rtt_seq)) {
            if (!t->backoff) rtt_sample(t, (uint32_t)(now_ms(n) - t->rtt_start));
            t->timing = 0;
        }
        /* The bytes acknowledged leave the buffer. */
        if (SEQ_GT(ack, t->tx_seq)) {
            uint32_t d = ack - t->tx_seq;
            if (d > t->tx_len) d = t->tx_len;
            memmove(t->tx, t->tx + d, t->tx_len - d);
            t->tx_len -= d;
            t->tx_seq += d;
        }
        t->snd_una = ack;
        if (SEQ_LT(t->snd_nxt, t->snd_una)) t->snd_nxt = t->snd_una;
        t->backoff = 0;
        t->dupacks = 0;
        t->rto_at = 0;
        if (t->snd_una != t->snd_max) rto_arm(n, t);
        if (t->fin_sent && SEQ_GT(ack, t->fin_seq) && !fin_acked(n, t)) return;
    } else if (ack == t->snd_una && !dlen && !(flags & TCP_FIN) && t->snd_una != t->snd_max && wnd == t->snd_wnd) {
        /* Fast retransmit on the third duplicate ACK (RFC 5681 3.2). */
        if (++t->dupacks == 3) {
            t->snd_nxt = t->snd_una;
            t->timing = 0;
            n->stats.tcp_retransmits++;
            tcp_output(n, t, 1, 0);
            t->snd_nxt = t->snd_max;
        }
    }
    /* The send window (RFC 9293 3.10.7.4, SND.WL1/WL2). */
    if (SEQ_LT(t->snd_wl1, seq) || (t->snd_wl1 == seq && SEQ_LEQ(t->snd_wl2, ack))) {
        t->snd_wnd = wnd;
        t->snd_wl1 = seq;
        t->snd_wl2 = ack;
    }
    /* Seventh and eighth: the text and the FIN. */
    if (dlen || (flags & TCP_FIN)) {
        tcp_data(n, t, seq, data, dlen, (flags & TCP_FIN) != 0);
        tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0);
    }
    if (t->state != T_FREE) {
        if (t->dns) { t->dns_deadline = now_ms(n) + UDP_IDLE_MS; dns_tcp_pump(n, t); }
        else tcp_output(n, t, 64, 0);
    }
}

/* The host's side of a connection: connect done, bytes in, bytes out. */
static void tcp_host(mc_nat *n, tcb *t, int readable, int writable, int failed) {
    char t1[16];
    if (t->state == T_CONNECTING) {
        if (!writable && !failed) return;
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(t->s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (failed || err) {
            nat_log(n, "tcp: %s:%u refused", dotted(t->remote, t1), (unsigned)t->remote_port);
            n->stats.tcp_refused++;
            tcp_reset_for(n, t->remote, t->remote_port, t->guest_port, t->irs, 0, 0, 1);
            tcp_free(n, t, 0);
            return;
        }
        t->state = T_SYN_RCVD;
        tcp_segment(n, t, t->iss, TCP_SYN | TCP_ACK, NULL, 0);
        t->snd_nxt = t->snd_max = t->iss + 1;
        rto_arm(n, t);
        return;
    }
    if (readable && !t->host_eof && t->tx_len < TCP_BUF) {
        const int got = recv(t->s, (char *)t->tx + t->tx_len, (int)(TCP_BUF - t->tx_len), 0);
        if (got > 0) {
            t->tx_len += (uint32_t)got;
            n->stats.tcp_bytes_in += (uint32_t)got;
        } else if (got == 0) t->host_eof = 1;
        else if (!mc_would_block(mc_socket_error())) { tcp_abort(n, t); return; }
        tcp_output(n, t, 64, 0);
    }
    if (writable && t->rx_len) {
        const int put = send(t->s, (const char *)t->rx, (int)t->rx_len, 0);
        if (put > 0) {
            memmove(t->rx, t->rx + put, t->rx_len - (uint32_t)put);
            t->rx_len -= (uint32_t)put;
            ooo_merge(n, t);
            /* A window update once enough room opened (RFC 9293 3.8.6.2.2). */
            const uint32_t w = our_window(t);
            if (t->state >= T_ESTABLISHED && (t->adv_wnd < t->mss || w - t->adv_wnd >= TCP_BUF / 2))
                tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0);
        } else if (put < 0 && !mc_would_block(mc_socket_error())) { tcp_abort(n, t); return; }
    }
    if (t->guest_fin && !t->rx_len && !t->host_shut) {
        shutdown(t->s, 1);   /* SD_SEND, SHUT_WR */
        t->host_shut = 1;
    }
}

static void tcp_accept(mc_nat *n, unsigned fi) {
    char t1[16];
    for (int k = 0; k < 16; k++) {
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        mc_socket s = accept(n->listener[fi], (struct sockaddr *)&from, &fl);
        if (s == MC_BAD_SOCKET) return;
        tcb *t = tcp_new(n);
        if (!t) { mc_close_socket(s); continue; }
        mc_set_nonblocking(s);
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        t->s = s;
        t->forwarded = 1;
        t->remote = n->c.router;
        t->remote_port = new_nat_port(n);
        t->guest_port = n->c.forward[fi].guest_port;
        t->state = T_SYN_SENT;
        t->snd_una = t->iss;
        t->snd_nxt = t->snd_max = t->iss + 1;
        t->tx_seq = t->iss + 1;
        tcp_segment(n, t, t->iss, TCP_SYN, NULL, 0);
        rto_arm(n, t);
        nat_log(n, "tcp: forward from %s:%u to port %u", dotted(ntohl(from.sin_addr.s_addr), t1), (unsigned)ntohs(from.sin_port),
                (unsigned)t->guest_port);
    }
}

/* DNS over TCP has two length bytes, independent of segment boundaries;
 * keep the connection for subsequent/pipelined queries (RFC 1035 4.2.2,
 * RFC 7766 6.2.1). Resolver jobs run serially per connection. */
static void dns_tcp_pump(mc_nat *n, tcb *t) {
    if (t->state != T_ESTABLISHED && t->state != T_CLOSE_WAIT) return;
    while (!t->dns_pending && !t->host_eof && t->rx_len >= 2 && t->tx_len <= TCP_BUF - 514u) {
        const uint32_t length = get16(t->rx);
        if (t->rx_len < length + 2) break;
        t->dns_pending = 1;
        dns_input(n, t->guest_port, t->cookie, t->rx + 2, length);
        t->rx_len -= length + 2;
        memmove(t->rx, t->rx + length + 2, t->rx_len);
        ooo_merge(n, t);
        tcp_segment(n, t, t->snd_nxt, TCP_ACK, NULL, 0);
        t->dns_deadline = now_ms(n) + UDP_IDLE_MS;
    }
    if (!t->dns_pending && ((t->guest_fin && t->rx_len < 2) || now_ms(n) >= t->dns_deadline)) t->host_eof = 1;
    tcp_output(n, t, 64, 0);
}

static void tcp_timers(mc_nat *n, tcb *t, uint64_t now) {
    if (t->state == T_TIME_WAIT) {
        if (now >= t->wait_until) tcp_free(n, t, 0);
        return;
    }
    if (!t->rto_at || now < t->rto_at) return;
    t->rto_at = 0;
    const unsigned limit = (t->state == T_SYN_SENT || t->state == T_SYN_RCVD) ? SYN_RETRIES : RETRIES;
    if (t->backoff >= limit) {
        char t1[16];
        nat_log(n, "tcp: %s:%u timed out", dotted(t->remote, t1), (unsigned)t->remote_port);
        if (t->state == T_SYN_SENT) tcp_free(n, t, 1);
        else tcp_abort(n, t);
        return;
    }
    t->backoff++;
    t->timing = 0;
    n->stats.tcp_retransmits++;
    if (t->state == T_SYN_SENT) tcp_segment(n, t, t->iss, TCP_SYN, NULL, 0);
    else if (t->state == T_SYN_RCVD) tcp_segment(n, t, t->iss, TCP_SYN | TCP_ACK, NULL, 0);
    else if (t->snd_una == t->snd_max && t->snd_wnd == 0 && t->tx_len) tcp_output(n, t, 1, 1);
    else {
        /* Retransmission from the oldest unacknowledged byte (RFC 6298 5.4). */
        t->snd_nxt = t->snd_una;
        if (t->fin_sent && SEQ_LEQ(t->snd_una, t->fin_seq)) t->fin_sent = 0;
        tcp_output(n, t, 1, 0);
    }
    rto_arm(n, t);
}

/* ---- the Macintosh's frames ------------------------------------------------------------- */

static void ip_dispatch(mc_nat *n, uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *p, size_t len) {
    switch (proto) {
    case PROTO_ICMP: icmp_input(n, dst, p, len); break;
    case PROTO_UDP: udp_input(n, src, dst, p, len); break;
    case PROTO_TCP: tcp_input(n, src, dst, p, len); break;
    default: break;
    }
}

static void fragment_clear(ip_reassembly *r) {
    free(r->data);
    free(r->seen);
    memset(r, 0, sizeof *r);
}

/* RFC 791 3.2: key by source, destination, protocol and ID. Store byte
 * coverage to accept reordered and duplicate fragments. Conflicting
 * overlaps/lengths discard the datagram; incomplete sets expire. */
static void ip_fragment(mc_nat *n, const uint8_t *ip, unsigned ihl, size_t total) {
    const uint16_t flags = get16(ip + 6);
    const uint32_t off = (uint32_t)(flags & 0x1FFFu) * 8u;
    const uint32_t length = (uint32_t)(total - ihl), end = off + length;
    const int more = (flags & 0x2000u) != 0;
    if (!length || (more && (length & 7u)) || end > IP_PAYLOAD_MAX || (flags & 0xC000u)) return;
    const uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    const uint16_t id = get16(ip + 4);
    ip_reassembly *r = NULL, *slot = NULL;
    const uint64_t now = now_ms(n);
    for (unsigned k = 0; k < REASSEMBLIES; k++) {
        ip_reassembly *q = &n->fragments[k];
        if (q->data && now >= q->deadline) fragment_clear(q);
        if (q->data && q->src == src && q->dst == dst && q->id == id && q->proto == ip[9]) { r = q; break; }
        if (!q->data) slot = q;
    }
    if (!r) {
        if (!slot) return;
        r = slot;
        r->data = malloc(IP_PAYLOAD_MAX);
        r->seen = calloc((IP_PAYLOAD_MAX + 7u) / 8u, 1);
        if (!r->data || !r->seen) { fragment_clear(r); return; }
        r->src = src; r->dst = dst; r->id = id; r->proto = ip[9];
        r->deadline = now + REASSEMBLY_MS;
    }
    if (!off) r->first_ihl = (uint8_t)ihl;
    if ((r->first_ihl && (end > 65535u - r->first_ihl || r->highest > 65535u - r->first_ihl)) ||
        (r->last && (end > r->last || (!more && end != r->last))) ||
        (!more && r->highest > end)) { fragment_clear(r); return; }
    if (!more) r->last = end;
    if (end > r->highest) r->highest = end;
    const uint8_t *data = ip + ihl;
    for (uint32_t i = 0; i < length; i++) {
        const uint32_t at = off + i;
        const uint8_t mask = (uint8_t)(1u << (at & 7u));
        if (r->seen[at >> 3] & mask) {
            if (r->data[at] != data[i]) { fragment_clear(r); return; }
        } else {
            r->seen[at >> 3] |= mask;
            r->data[at] = data[i];
            r->received++;
        }
    }
    if (r->first_ihl && r->last && r->received == r->last) {
        ip_dispatch(n, src, dst, r->proto, r->data, r->last);
        fragment_clear(r);
    }
}

void mc_nat_input(mc_nat *n, const uint8_t *f, size_t len) {
    if (len < ETH) return;
    const uint16_t type = get16(f + 12);
    /* Other stations share the segment (net.h): the Macintosh's address is
       learnt from what it sends as 10.0.2.15 (and from DHCP's chaddr). */
    const int from_guest = (type == ETH_ARP && len >= ETH + 28 && get32(f + ETH + 14) == n->c.guest) ||
                           (type == ETH_IP && len >= ETH + IPH && get32(f + ETH + 12) == n->c.guest);
    if (from_guest && !(f[6] & 1)) { memcpy(n->guest_mac, f + 6, 6); n->guest_mac_known = 1; }
    if (type == ETH_ARP) { arp_input(n, f, len); return; }
    if (type != ETH_IP || len < ETH + IPH) return;
    if (memcmp(f, router_mac, 6) && memcmp(f, broadcast_mac, 6)) return;
    const uint8_t *ip = f + ETH;
    const unsigned ihl = (unsigned)(ip[0] & 15) * 4u;
    if ((ip[0] >> 4) != 4 || ihl < IPH) return;
    size_t total = get16(ip + 2);
    if (total < ihl || total > len - ETH) return;
    if (sum_fold(sum_add(0, ip, ihl))) return;
    if (get16(ip + 6) & 0x3FFF) { ip_fragment(n, ip, ihl, total); return; }
    const uint32_t src = get32(ip + 12), dst = get32(ip + 16);
    const uint8_t *p = ip + ihl;
    const size_t plen = total - ihl;
    ip_dispatch(n, src, dst, ip[9], p, plen);
}

/* ---- polling ------------------------------------------------------------------------------- */

void mc_nat_wake(mc_nat *n) {
    const char b = 0;
    sendto(n->wake, &b, 1, 0, (const struct sockaddr *)&n->wake_addr, sizeof n->wake_addr);
}

void mc_nat_poll(mc_nat *n, int timeout_ms) {
    uint64_t now = now_ms(n);
    /* The nearest timer bounds the wait. */
    for (unsigned k = 0; k < MAX_TCP; k++) {
        const tcb *t = &n->tcp[k];
        uint64_t due = 0;
        if (t->state == T_TIME_WAIT) due = t->wait_until;
        else if (t->state && t->rto_at) due = t->rto_at;
        if (due) {
            const int64_t left = due > now ? (int64_t)(due - now) : 0;
            if (left < timeout_ms) timeout_ms = (int)left;
        }
    }
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    mc_socket top = n->wake;
#define WATCH(set, s) do { FD_SET((s), (set)); if ((s) > top) top = (s); } while (0)
    WATCH(&rs, n->wake);
    for (unsigned k = 0; k < n->c.forwards; k++) if (n->listener[k] != MC_BAD_SOCKET) WATCH(&rs, n->listener[k]);
    for (unsigned k = 0; k < MAX_UDP; k++) if (n->udp[k].used) WATCH(&rs, n->udp[k].s);
    for (unsigned k = 0; k < MAX_TCP; k++) {
        const tcb *t = &n->tcp[k];
        if (!t->state || t->s == MC_BAD_SOCKET) continue;
        if (t->state == T_CONNECTING) { WATCH(&ws, t->s); WATCH(&es, t->s); continue; }
        if (t->state == T_SYN_SENT) continue;
        if (!t->host_eof && t->tx_len < TCP_BUF) WATCH(&rs, t->s);
        if (t->rx_len) WATCH(&ws, t->s);
    }
#undef WATCH
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    const int ready = select((int)(top + 1), &rs, &ws, &es, &tv);
    if (ready > 0) {
        if (FD_ISSET(n->wake, &rs)) {
            char b[64];
            while (recv(n->wake, b, sizeof b, 0) > 0) {}
        }
        for (unsigned k = 0; k < n->c.forwards; k++) {
            if (n->listener[k] == MC_BAD_SOCKET || !FD_ISSET(n->listener[k], &rs)) continue;
            if (n->c.forward[k].udp) udp_forward_receive(n, k);
            else tcp_accept(n, k);
        }
        for (unsigned k = 0; k < MAX_UDP; k++)
            if (n->udp[k].used && FD_ISSET(n->udp[k].s, &rs)) udp_receive(n, &n->udp[k]);
        for (unsigned k = 0; k < MAX_TCP; k++) {
            tcb *t = &n->tcp[k];
            if (!t->state || t->s == MC_BAD_SOCKET) continue;
            const int r = FD_ISSET(t->s, &rs) != 0, w = FD_ISSET(t->s, &ws) != 0, e = FD_ISSET(t->s, &es) != 0;
            if (r || w || e) tcp_host(n, t, r, w, e);
        }
    }
    finish_jobs(n);
    now = now_ms(n);
    for (unsigned k = 0; k < MAX_TCP; k++) {
        tcb *t = &n->tcp[k];
        if (t->state && t->dns) dns_tcp_pump(n, t);
        if (t->state) tcp_timers(n, t, now);
    }
    for (unsigned k = 0; k < REASSEMBLIES; k++)
        if (n->fragments[k].data && now >= n->fragments[k].deadline) fragment_clear(&n->fragments[k]);
    for (unsigned k = 0; k < MAX_UDP; k++)
        if (n->udp[k].used && now - n->udp[k].last > UDP_IDLE_MS) { mc_close_socket(n->udp[k].s); n->udp[k].used = 0; }
    for (unsigned k = 0; k < MAX_FWD_UDP; k++)
        if (n->ufwd[k].used && now - n->ufwd[k].last > UDP_IDLE_MS) n->ufwd[k].used = 0;
}

/* ---- life ---------------------------------------------------------------------------------- */

mc_nat *mc_nat_create(const mc_nat_config *config, char *error, size_t capacity) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { snprintf(error, capacity, "Winsock is unavailable"); return NULL; }
#endif
    mc_nat *n = calloc(1, sizeof *n);
    if (!n) {
#ifdef _WIN32
        WSACleanup();
#endif
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    n->c = *config;
    if (!n->c.network) n->c.network = IP4(10, 0, 2, 0);
    if (!n->c.router) n->c.router = n->c.network | 2;
    if (!n->c.dns) n->c.dns = n->c.network | 3;
    if (!n->c.guest) n->c.guest = n->c.network | 15;
    if (!n->c.host_loopback) n->c.host_loopback = IP4(127, 0, 0, 1);
    n->next_port = FWD_PORT_FIRST;
    n->iss_seed = (uint32_t)mc_clock_ms() * 2654435761u;
    for (unsigned k = 0; k < MC_NAT_FORWARDS; k++) n->listener[k] = MC_BAD_SOCKET;
    for (unsigned k = 0; k < MAX_TCP; k++) n->tcp[k].s = MC_BAD_SOCKET;
    mc_mutex_init(&n->lock);
    mc_cond_init(&n->cond);
    /* The wake socket: a datagram to itself ends select(). */
    n->wake = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sin_set(&n->wake_addr, IP4(127, 0, 0, 1), 0);
    socklen_t al = sizeof n->wake_addr;
    if (n->wake == MC_BAD_SOCKET || bind(n->wake, (struct sockaddr *)&n->wake_addr, sizeof n->wake_addr) != 0 ||
        getsockname(n->wake, (struct sockaddr *)&n->wake_addr, &al) != 0) {
        snprintf(error, capacity, "Cannot make the network's sockets");
        mc_nat_destroy(n);
        return NULL;
    }
    udp_socket_options(n->wake);
    for (unsigned k = 0; k < n->c.forwards && k < MC_NAT_FORWARDS; k++) {
        const mc_nat_forward *f = &n->c.forward[k];
        mc_socket s = socket(AF_INET, f->udp ? SOCK_DGRAM : SOCK_STREAM, f->udp ? IPPROTO_UDP : IPPROTO_TCP);
        struct sockaddr_in sa;
        sin_set(&sa, f->bind ? f->bind : IP4(127, 0, 0, 1), f->host_port);
        socklen_t sl = sizeof sa;
        if (s == MC_BAD_SOCKET || bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || (!f->udp && listen(s, 8) != 0) ||
            getsockname(s, (struct sockaddr *)&sa, &sl) != 0) {
            char t1[16];
            snprintf(error, capacity, "network.forward: cannot listen on %s port %u of %s", f->udp ? "UDP" : "TCP",
                     (unsigned)f->host_port, dotted(f->bind ? f->bind : IP4(127, 0, 0, 1), t1));
            if (s != MC_BAD_SOCKET) mc_close_socket(s);
            mc_nat_destroy(n);
            return NULL;
        }
        if (f->udp) udp_socket_options(s);
        else mc_set_nonblocking(s);
        n->listener[k] = s;
        n->listen_port[k] = ntohs(sa.sin_port);
    }
    if (!mc_thread_start(&n->worker, worker_main, n)) {
        snprintf(error, capacity, "Cannot start the network's resolver");
        mc_nat_destroy(n);
        return NULL;
    }
    n->worker_running = 1;
    return n;
}

void mc_nat_destroy(mc_nat *n) {
    if (!n) return;
    if (n->worker_running) {
        mc_mutex_lock(&n->lock);
        n->stopping = 1;
        mc_cond_signal(&n->cond);
        mc_mutex_unlock(&n->lock);
        mc_thread_join(n->worker);
    }
    for (job *j = n->todo; j;) { job *next = j->next; free_job(j); j = next; }
    for (job *j = n->done; j;) { job *next = j->next; free_job(j); j = next; }
    for (unsigned k = 0; k < MAX_TCP; k++)
        if (n->tcp[k].state) tcp_free(n, &n->tcp[k], 1);
    for (unsigned k = 0; k < MAX_UDP; k++)
        if (n->udp[k].used) mc_close_socket(n->udp[k].s);
    for (unsigned k = 0; k < MC_NAT_FORWARDS; k++)
        if (n->listener[k] != MC_BAD_SOCKET) mc_close_socket(n->listener[k]);
    if (n->wake != MC_BAD_SOCKET && n->wake) mc_close_socket(n->wake);
    mc_cond_free(&n->cond);
    mc_mutex_free(&n->lock);
    for (unsigned k = 0; k < REASSEMBLIES; k++) fragment_clear(&n->fragments[k]);
    free(n);
#ifdef _WIN32
    WSACleanup();
#endif
}

void mc_nat_get_stats(mc_nat *n, mc_nat_stats *stats) { *stats = n->stats; }

uint16_t mc_nat_forward_port(mc_nat *n, unsigned index) {
    return index < n->c.forwards && index < MC_NAT_FORWARDS ? n->listen_port[index] : 0;
}
