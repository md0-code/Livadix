/* test_nat: the user-mode NAT (src/net/nat.h) driven as the Macintosh would
 * drive it, frame by frame, against servers on the host's loopback
 * interface (no Internet needed): ARP (RFC 826); DHCP DISCOVER/OFFER,
 * REQUEST/ACK and NAK (RFC 2131, options RFC 2132) and BOOTP (RFC 951,
 * vendor area RFC 1497); DNS A, AAAA and other queries (RFC 1035); UDP both
 * ways (RFC 768); TCP (RFC 9293): the handshake with the MSS option, data
 * both ways within the Macintosh's window, out-of-order segments held and
 * merged, retransmission on the timer (RFC 6298), FIN from either side,
 * RST for a refused connection; ICMP echo to the router and through the
 * host (RFC 792); TCP and UDP port forwards. The NAT's clock runs ahead
 * when a test needs its timers to fire. Then the segment (net.h) on its
 * thread: the Macintosh's AppleTalk multicast reaching another host, that
 * host's frames reaching the Macintosh, the NAT answering through it.
 *
 * Livadix's own code. */
#include "../test.h"
#include "../../src/net/nat.h"
#include "../../src/net/net.h"
#include "../../src/net/sys.h"

#include <stdlib.h>
#include <string.h>

int mc_test_failures;
static int tests;
#define T() (tests++)

#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
static const uint32_t GUEST = IP4(10, 0, 2, 15), ROUTER = IP4(10, 0, 2, 2), DNS = IP4(10, 0, 2, 3);
static const uint8_t guest_mac[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x01};
static const uint8_t router_mac[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x02};
static const uint8_t bcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t sum_add(uint32_t sum, const uint8_t *p, size_t n) {
    for (; n > 1; n -= 2, p += 2) sum += (uint32_t)(p[0] << 8 | p[1]);
    if (n) sum += (uint32_t)p[0] << 8;
    return sum;
}
static uint16_t sum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

/* ---- the NAT and what it sends ---------------------------------------------------- */

static mc_nat *nat;
static int64_t skew;   /* ms the NAT's clock runs ahead of the host's */
static uint64_t clock_fn(void *ctx) { (void)ctx; return mc_clock_ms() + (uint64_t)skew; }

#define FRAMES 256
static uint8_t frames[FRAMES][1600];
static size_t lengths[FRAMES];
static unsigned nframes;
static void collect(void *ctx, const uint8_t *f, size_t len) {
    (void)ctx;
    if (nframes < FRAMES) { memcpy(frames[nframes], f, len); lengths[nframes] = len; }
    nframes++;
}
static void drop(unsigned k) {
    memmove(frames[k], frames[k + 1], (size_t)(nframes - k - 1) * sizeof frames[0]);
    memmove(lengths + k, lengths + k + 1, (size_t)(nframes - k - 1) * sizeof lengths[0]);
    nframes--;
}
static void logline(void *ctx, const char *line) { (void)ctx; if (getenv("LIVADIX_NET_LOG")) printf("  nat: %s\n", line); }

static mc_nat *make_nat(const mc_nat_forward *fw, unsigned nfw) {
    mc_nat_config c;
    memset(&c, 0, sizeof c);
    c.clock = clock_fn;
    c.output = collect;
    c.log = logline;
    for (unsigned k = 0; k < nfw; k++) c.forward[k] = fw[k];
    c.forwards = nfw;
    char error[256];
    mc_nat *n = mc_nat_create(&c, error, sizeof error);
    if (!n) fprintf(stderr, "mc_nat_create: %s\n", error);
    nframes = 0;
    return n;
}

/* A frame's IP protocol and transport header, or NULL. */
static const uint8_t *ip_of(const uint8_t *f, uint8_t proto) {
    if (get16(f + 12) != 0x0800 || f[14 + 9] != proto) return NULL;
    return f + 14 + (f[14] & 15) * 4;
}

typedef int (*match_fn)(const uint8_t *f, size_t len, const void *arg);
/* Polls the NAT until a frame matches (up to `ms` of host time); returns its
 * index (the frame stays queued) or -1. */
static int wait_for(match_fn m, const void *arg, int ms) {
    const uint64_t end = mc_clock_ms() + (uint64_t)ms;
    for (;;) {
        for (unsigned k = 0; k < nframes && k < FRAMES; k++)
            if (m(frames[k], lengths[k], arg)) return (int)k;
        if (mc_clock_ms() > end) return -1;
        mc_nat_poll(nat, 5);
    }
}

/* ---- the Macintosh's frames ---------------------------------------------------------- */

static uint8_t out[1600];
static uint16_t ip_id = 1;

static void send_ip(const uint8_t *dmac, uint32_t src, uint32_t dst, uint8_t proto, const uint8_t *payload, size_t len) {
    memcpy(out, dmac, 6);
    memcpy(out + 6, guest_mac, 6);
    put16(out + 12, 0x0800);
    uint8_t *ip = out + 14;
    memset(ip, 0, 20);
    ip[0] = 0x45;
    put16(ip + 2, (uint32_t)(20 + len));
    put16(ip + 4, ip_id++);
    ip[8] = 64;
    ip[9] = proto;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, sum_fold(sum_add(0, ip, 20)));
    memcpy(ip + 20, payload, len);
    uint8_t *t = ip + 20;
    if (proto == 6 || proto == 17) {
        const unsigned at = proto == 17 ? 6 : 16;
        put16(t + at, 0);
        const uint32_t sum = sum_add(0, ip + 12, 8) + proto + (uint32_t)len;
        put16(t + at, sum_fold(sum_add(sum, t, len)));
    } else if (proto == 1) {
        put16(t + 2, 0);
        put16(t + 2, sum_fold(sum_add(0, t, len)));
    }
    mc_nat_input(nat, out, 14 + 20 + len);
}
static void send_udp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const void *data, size_t len, int bcast) {
    uint8_t u[1500];
    put16(u, sport);
    put16(u + 2, dport);
    put16(u + 4, (uint32_t)(8 + len));
    memcpy(u + 8, data, len);
    send_ip(bcast ? bcast_mac : router_mac, src, dst, 17, u, 8 + len);
}

/* The transport checksum of a frame the NAT sent is right. */
static int checksum_ok(const uint8_t *f) {
    const uint8_t *ip = f + 14;
    if (sum_fold(sum_add(0, ip, 20)) != 0) return 0;
    const size_t len = get16(ip + 2) - 20u;
    const uint8_t proto = ip[9];
    if (proto == 1) return sum_fold(sum_add(0, ip + 20, len)) == 0;
    const uint32_t sum = sum_add(0, ip + 12, 8) + proto + (uint32_t)len;
    return sum_fold(sum_add(sum, ip + 20, len)) == 0;
}

/* ---- ARP --------------------------------------------------------------------------------- */

static int is_arp_reply(const uint8_t *f, size_t len, const void *arg) {
    (void)arg;
    return len >= 42 && get16(f + 12) == 0x0806 && get16(f + 20) == 2;
}

static void arp_request(uint32_t target) {
    memcpy(out, bcast_mac, 6);
    memcpy(out + 6, guest_mac, 6);
    put16(out + 12, 0x0806);
    uint8_t *a = out + 14;
    put16(a, 1); put16(a + 2, 0x0800); a[4] = 6; a[5] = 4; put16(a + 6, 1);
    memcpy(a + 8, guest_mac, 6); put32(a + 14, GUEST);
    memset(a + 18, 0, 6); put32(a + 24, target);
    mc_nat_input(nat, out, 42);
}

static void test_arp(void) {
    nframes = 0;
    arp_request(ROUTER);
    int k = wait_for(is_arp_reply, NULL, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *a = frames[k] + 14;
        CHECK(!memcmp(frames[k], guest_mac, 6)); T();
        CHECK(!memcmp(a + 8, router_mac, 6)); T();
        CHECK_EQ(get32(a + 14), ROUTER); T();
        CHECK(!memcmp(a + 18, guest_mac, 6)); T();
        CHECK_EQ(get32(a + 24), GUEST); T();
    }
    nframes = 0;
    arp_request(DNS);
    CHECK(wait_for(is_arp_reply, NULL, 100) >= 0); T();
    /* Nobody else answers: not the Macintosh's own address (a DHCP client
       probes it), not another host of the network. */
    nframes = 0;
    arp_request(GUEST);
    arp_request(IP4(10, 0, 2, 77));
    CHECK(wait_for(is_arp_reply, NULL, 50) < 0); T();
}

/* ---- DHCP and BOOTP ------------------------------------------------------------------------ */

static int is_bootp_reply(const uint8_t *f, size_t len, const void *arg) {
    (void)arg;
    const uint8_t *u = ip_of(f, 17);
    return u && len > 42 + 240 && get16(u) == 67 && get16(u + 2) == 68 && u[8] == 2;
}

/* An option of a reply, or NULL. */
static const uint8_t *option(const uint8_t *b, size_t len, uint8_t code) {
    for (size_t i = 240; i < len;) {
        if (b[i] == 255) break;
        if (b[i] == 0) { i++; continue; }
        if (b[i] == code) return b + i;
        i += 2u + b[i + 1];
    }
    return NULL;
}

static void bootp_request(int type, uint32_t requested, uint32_t server) {
    uint8_t b[300];
    memset(b, 0, sizeof b);
    b[0] = 1; b[1] = 1; b[2] = 6;
    put32(b + 4, 0x12345678);
    put16(b + 10, 0x8000);   /* broadcast */
    memcpy(b + 28, guest_mac, 6);
    b[236] = 99; b[237] = 130; b[238] = 83; b[239] = 99;
    uint8_t *p = b + 240;
    if (type) { *p++ = 53; *p++ = 1; *p++ = (uint8_t)type; }
    if (requested) { *p++ = 50; *p++ = 4; put32(p, requested); p += 4; }
    if (server) { *p++ = 54; *p++ = 4; put32(p, server); p += 4; }
    *p++ = 255;
    send_udp(0, 68, 0xFFFFFFFFu, 67, b, sizeof b, 1);
}

static void test_dhcp(void) {
    /* DISCOVER: an OFFER of 10.0.2.15, broadcast, with the mask, router,
       DNS server, the server's identifier and a day's lease (RFC 2131 4.3.1). */
    nframes = 0;
    bootp_request(1, 0, 0);
    int k = wait_for(is_bootp_reply, NULL, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *f = frames[k], *b = ip_of(f, 17) + 8;
        const size_t len = get16(ip_of(f, 17) + 4) - 8u;
        CHECK(!memcmp(f, bcast_mac, 6)); T();
        CHECK_EQ(get32(f + 14 + 16), 0xFFFFFFFFu); T();
        CHECK(checksum_ok(f)); T();
        CHECK(len >= 300); T();
        CHECK_EQ(get32(b + 4), 0x12345678); T();
        CHECK_EQ(get32(b + 16), GUEST); T();
        CHECK(!memcmp(b + 28, guest_mac, 6)); T();
        const uint8_t *o = option(b, len, 53);
        CHECK(o && o[2] == 2); T();
        o = option(b, len, 1); CHECK(o && get32(o + 2) == 0xFFFFFF00u); T();
        o = option(b, len, 3); CHECK(o && get32(o + 2) == ROUTER); T();
        o = option(b, len, 6); CHECK(o && get32(o + 2) == DNS); T();
        o = option(b, len, 54); CHECK(o && get32(o + 2) == ROUTER); T();
        o = option(b, len, 51); CHECK(o && get32(o + 2) == 86400); T();
    }
    /* REQUEST of the offered address: ACK. */
    nframes = 0;
    bootp_request(3, GUEST, ROUTER);
    k = wait_for(is_bootp_reply, NULL, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *b = ip_of(frames[k], 17) + 8;
        const uint8_t *o = option(b, 300, 53);
        CHECK(o && o[2] == 5); T();
        CHECK_EQ(get32(b + 16), GUEST); T();
    }
    /* REQUEST of another address: NAK (4.3.2). */
    nframes = 0;
    bootp_request(3, IP4(10, 0, 2, 99), 0);
    k = wait_for(is_bootp_reply, NULL, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *o = option(ip_of(frames[k], 17) + 8, 300, 53);
        CHECK(o && o[2] == 6); T();
    }
    /* A REQUEST for another server is not ours to answer. */
    nframes = 0;
    bootp_request(3, GUEST, IP4(10, 0, 2, 9));
    CHECK(wait_for(is_bootp_reply, NULL, 30) < 0); T();
    /* BOOTP (no message type): a BOOTREPLY with the address and the
       RFC 1497 vendor extensions, no DHCP options. */
    nframes = 0;
    bootp_request(0, 0, 0);
    k = wait_for(is_bootp_reply, NULL, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *b = ip_of(frames[k], 17) + 8;
        CHECK_EQ(get32(b + 16), GUEST); T();
        CHECK_EQ(get32(b + 20), ROUTER); T();
        CHECK(option(b, 300, 53) == NULL); T();
        const uint8_t *o = option(b, 300, 3);
        CHECK(o && get32(o + 2) == ROUTER); T();
        o = option(b, 300, 6);
        CHECK(o && get32(o + 2) == DNS); T();
    }
}

/* ---- DNS ------------------------------------------------------------------------------------ */

static int is_dns_reply(const uint8_t *f, size_t len, const void *arg) {
    const uint8_t *u = ip_of(f, 17);
    (void)len;
    return u && get16(u) == 53 && get16(u + 2) == 5353 && get16(u + 8) == *(const uint16_t *)arg;
}

static void dns_query(uint16_t id, const char *name, uint16_t qtype) {
    uint8_t q[300];
    memset(q, 0, 12);
    put16(q, id);
    put16(q + 2, 0x0100);   /* RD */
    put16(q + 4, 1);
    size_t i = 12;
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        const size_t l = dot ? (size_t)(dot - p) : strlen(p);
        q[i++] = (uint8_t)l;
        memcpy(q + i, p, l);
        i += l;
        p += l + (dot ? 1 : 0);
    }
    q[i++] = 0;
    put16(q + i, qtype); put16(q + i + 2, 1);
    i += 4;
    send_udp(GUEST, 5353, DNS, 53, q, i, 0);
}

static void test_dns(void) {
    /* localhost from the host's resolver: 127.0.0.1, which the Macintosh
       reaches as the router. */
    nframes = 0;
    uint16_t id = 0x4242;
    dns_query(id, "localhost", 1);
    int k = wait_for(is_dns_reply, &(uint16_t){0x4242}, 5000);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *d = ip_of(frames[k], 17) + 8;
        CHECK(checksum_ok(frames[k])); T();
        CHECK_EQ(get16(d + 2) & 0x800F, 0x8000); T();   /* QR, NOERROR */
        CHECK_EQ(get16(d + 4), 1); T();
        CHECK(get16(d + 6) >= 1); T();
        const uint8_t *a = d + 12 + 11 + 4;   /* after "localhost" */
        CHECK_EQ(get16(a + 2), 1); T();
        CHECK_EQ(get16(a + 10), 4); T();
        CHECK_EQ(get32(a + 12), ROUTER); T();
    }
    /* AAAA: no record, no error; MX: not implemented. */
    nframes = 0;
    id = 0x4343;
    dns_query(id, "localhost", 28);
    k = wait_for(is_dns_reply, &(uint16_t){0x4343}, 1000);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *d = ip_of(frames[k], 17) + 8;
        CHECK_EQ(get16(d + 2) & 15, 0); T();
        CHECK_EQ(get16(d + 6), 0); T();
    }
    nframes = 0;
    dns_query(0x4444, "localhost", 15);
    k = wait_for(is_dns_reply, &(uint16_t){0x4444}, 1000);
    CHECK(k >= 0); T();
    if (k >= 0) { CHECK_EQ(get16(ip_of(frames[k], 17) + 8 + 2) & 15, 4); T(); }
}

/* ---- UDP --------------------------------------------------------------------------------------- */

static int is_udp_to(const uint8_t *f, size_t len, const void *arg) {
    const uint8_t *u = ip_of(f, 17);
    (void)len;
    return u && get16(u + 2) == *(const uint16_t *)arg;
}

static mc_socket udp_server(uint16_t *port) {
    mc_socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(IP4(127, 0, 0, 1));
    bind(s, (struct sockaddr *)&sa, sizeof sa);
    socklen_t l = sizeof sa;
    getsockname(s, (struct sockaddr *)&sa, &l);
    *port = ntohs(sa.sin_port);
    mc_set_nonblocking(s);
    return s;
}

static void test_udp(void) {
    uint16_t port;
    mc_socket s = udp_server(&port);
    nframes = 0;
    send_udp(GUEST, 1234, ROUTER, port, "ping", 4, 0);
    char buf[64];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int got = -1;
    for (int k = 0; k < 200 && got < 0; k++) {
        mc_nat_poll(nat, 5);
        got = recvfrom(s, buf, sizeof buf, 0, (struct sockaddr *)&from, &fl);
    }
    CHECK_EQ(got, 4); T();
    CHECK(got == 4 && !memcmp(buf, "ping", 4)); T();
    sendto(s, "pong!", 5, 0, (struct sockaddr *)&from, fl);
    const uint16_t gp = 1234;
    const int k = wait_for(is_udp_to, &gp, 1000);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *f = frames[k], *u = ip_of(f, 17);
        CHECK_EQ(get32(f + 14 + 12), ROUTER); T();   /* the host's 127.0.0.1 is the router */
        CHECK_EQ(get32(f + 14 + 16), GUEST); T();
        CHECK_EQ(get16(u), port); T();
        CHECK_EQ(get16(u + 4), 13); T();
        CHECK(!memcmp(u + 8, "pong!", 5)); T();
        CHECK(checksum_ok(f)); T();
    }
    mc_close_socket(s);
}

/* A real UDP exchange larger than the Ethernet MTU, with guest fragments
 * sent out of order. The reply must reassemble to the same bytes and have
 * a valid complete UDP checksum (RFC 791/768). */
static void test_fragments(void) {
    uint16_t port;
    mc_socket server = udp_server(&port);
    uint8_t datagram[6008], received[6008], covered[6008] = {0};
    put16(datagram, 1235); put16(datagram + 2, port);
    put16(datagram + 4, sizeof datagram); put16(datagram + 6, 0);
    for (unsigned i = 8; i < sizeof datagram; i++) datagram[i] = (uint8_t)(i * 29u);
    nframes = 0;
    for (int part = 4; part >= 0; part--) {
        const unsigned off = (unsigned)part * 1480u;
        const unsigned count = sizeof datagram - off > 1480 ? 1480 : sizeof datagram - off;
        memcpy(out, router_mac, 6); memcpy(out + 6, guest_mac, 6); put16(out + 12, 0x0800);
        uint8_t *ip = out + 14;
        memset(ip, 0, 20); ip[0] = 0x45; ip[8] = 64; ip[9] = 17;
        put16(ip + 2, 20 + count); put16(ip + 4, 0x6161);
        put16(ip + 6, off / 8u | (part < 4 ? 0x2000u : 0u));
        put32(ip + 12, GUEST); put32(ip + 16, ROUTER);
        put16(ip + 10, sum_fold(sum_add(0, ip, 20)));
        memcpy(ip + 20, datagram + off, count);
        mc_nat_input(nat, out, 34 + count);
        if (part == 3) mc_nat_input(nat, out, 34 + count); /* harmless duplicate */
    }
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    int got = -1;
    for (unsigned k = 0; k < 200 && got < 0; k++) {
        mc_nat_poll(nat, 5);
        got = recvfrom(server, (char *)received, sizeof received, 0, (struct sockaddr *)&from, &fl);
    }
    CHECK_EQ(got, sizeof datagram - 8); T();
    CHECK(got == sizeof datagram - 8 && !memcmp(received, datagram + 8, (size_t)got)); T();
    if (got >= 0) sendto(server, (const char *)received, got, 0, (struct sockaddr *)&from, fl);
    unsigned have = 0, fragments = 0;
    uint16_t reply_id = 0;
    const uint64_t until = mc_clock_ms() + 1000;
    while (have < sizeof datagram && mc_clock_ms() < until) {
        mc_nat_poll(nat, 5);
        for (unsigned k = 0; k < nframes && k < FRAMES; k++) {
            const uint8_t *ip = frames[k] + 14;
            if (get16(frames[k] + 12) != 0x0800 || ip[9] != 17) continue;
            const unsigned off = (get16(ip + 6) & 0x1FFFu) * 8u, count = get16(ip + 2) - 20u;
            CHECK(sum_fold(sum_add(0, ip, 20)) == 0 && lengths[k] <= 1514); T();
            if (!fragments) reply_id = get16(ip + 4);
            CHECK_EQ(get16(ip + 4), reply_id); T();
            CHECK(!(get16(ip + 6) & 0x2000u) || !(count & 7u)); T();
            CHECK(off + count <= sizeof received);
            if (off + count <= sizeof received)
                for (unsigned i = 0; i < count; i++) {
                    received[off + i] = ip[20 + i];
                    if (!covered[off + i]) { covered[off + i] = 1; have++; }
                }
            fragments++;
        }
        nframes = 0;
    }
    CHECK_EQ(have, sizeof datagram); T();
    CHECK(fragments > 1); T();
    CHECK(!memcmp(received + 8, datagram + 8, sizeof datagram - 8)); T();
    uint8_t addresses[8]; put32(addresses, ROUTER); put32(addresses + 4, GUEST);
    CHECK_EQ(sum_fold(sum_add(sum_add(0, addresses, 8) + 17 + sizeof received, received, sizeof received)), 0); T();
    mc_close_socket(server);
}

/* ---- TCP ---------------------------------------------------------------------------------------- */

typedef struct seg {
    uint32_t seq, ack;
    uint8_t flags;
    uint16_t window, mss;
    const uint8_t *data;
    size_t len;
    uint16_t sport, dport;
} seg;

static int parse_tcp(const uint8_t *f, seg *s) {
    const uint8_t *h = ip_of(f, 6);
    if (!h) return 0;
    const unsigned hl = (unsigned)(h[12] >> 4) * 4u;
    s->sport = get16(h);
    s->dport = get16(h + 2);
    s->seq = get32(h + 4);
    s->ack = get32(h + 8);
    s->flags = h[13];
    s->window = get16(h + 14);
    s->mss = 0;
    if (hl >= 24 && h[20] == 2 && h[21] == 4) s->mss = get16(h + 22);
    s->data = h + hl;
    s->len = get16(f + 14 + 2) - 20u - hl;
    return 1;
}

typedef struct conn {
    uint16_t gport, rport;
    uint32_t remote;
    uint32_t snd, rcv;          /* the Macintosh's next sequence number and the next it expects */
    uint16_t window;
} conn;

static void tcp_send(conn *c, uint32_t seq, uint8_t flags, const void *data, size_t len, uint16_t mss) {
    uint8_t h[1500];
    memset(h, 0, 24);
    put16(h, c->gport);
    put16(h + 2, c->rport);
    put32(h + 4, seq);
    put32(h + 8, (flags & 0x10) ? c->rcv : 0);
    const unsigned hl = mss ? 24 : 20;
    h[12] = (uint8_t)((hl / 4) << 4);
    h[13] = flags;
    put16(h + 14, c->window);
    if (mss) { h[20] = 2; h[21] = 4; put16(h + 22, mss); }
    memcpy(h + hl, data, len);
    send_ip(router_mac, GUEST, c->remote, 6, h, hl + len);
}

typedef struct want { const conn *c; uint8_t flags; } want;
/* A segment of the connection with these flags among others. */
static int is_seg(const uint8_t *f, size_t len, const void *arg) {
    const want *w = arg;
    seg s;
    (void)len;
    return parse_tcp(f, &s) && s.dport == w->c->gport && s.sport == w->c->rport && (s.flags & w->flags) == w->flags;
}
static int next_seg(const conn *c, uint8_t flags, seg *s, int ms) {
    want w = {c, flags};
    const int k = wait_for(is_seg, &w, ms);
    if (k < 0) return 0;
    static uint8_t keep[1600];
    memcpy(keep, frames[k], lengths[k]);
    drop((unsigned)k);
    parse_tcp(keep, s);
    return 1;
}

static mc_socket tcp_server(uint16_t *port) {
    mc_socket s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(IP4(127, 0, 0, 1));
    bind(s, (struct sockaddr *)&sa, sizeof sa);
    listen(s, 4);
    socklen_t l = sizeof sa;
    getsockname(s, (struct sockaddr *)&sa, &l);
    *port = ntohs(sa.sin_port);
    mc_set_nonblocking(s);
    return s;
}
static mc_socket accept_poll(mc_socket l) {
    for (int k = 0; k < 400; k++) {
        mc_socket s = accept(l, NULL, NULL);
        if (s != MC_BAD_SOCKET) { mc_set_nonblocking(s); return s; }
        mc_nat_poll(nat, 5);
    }
    return MC_BAD_SOCKET;
}
/* Reads from a host socket while the NAT runs, up to `limit` bytes. */
static size_t read_poll(mc_socket s, uint8_t *buf, size_t limit, int *eof) {
    size_t got = 0;
    *eof = 0;
    for (int k = 0; k < 400 && got < limit && !*eof; k++) {
        mc_nat_poll(nat, 5);
        const int r = recv(s, (char *)buf + got, (int)(limit - got), 0);
        if (r > 0) got += (size_t)r;
        else if (r == 0) *eof = 1;
    }
    return got;
}

/* The handshake from the Macintosh's side. */
static int tcp_connect(conn *c, mc_socket listener, mc_socket *accepted) {
    seg s;
    c->snd = 1000;
    tcp_send(c, c->snd, 0x02, NULL, 0, 536);
    if (!next_seg(c, 0x12, &s, 2000)) return 0;
    CHECK_EQ(s.ack, 1001); T();
    CHECK_EQ(s.mss, 1460); T();
    c->snd = 1001;
    c->rcv = s.seq + 1;
    tcp_send(c, c->snd, 0x10, NULL, 0, 0);
    *accepted = listener == MC_BAD_SOCKET ? MC_BAD_SOCKET : accept_poll(listener);
    return 1;
}

static void test_dns_tcp(void) {
    conn c = {5354, 53, DNS, 0, 0, 8192};
    mc_socket accepted;
    nframes = 0;
    CHECK(tcp_connect(&c, MC_BAD_SOCKET, &accepted)); T();
    /* Split length prefix and two pipelined requests on one connection. */
    static const uint8_t question[] = {0,0, 0x51,0x51, 1,0, 0,1, 0,0, 0,0, 0,0,
                                      9,'l','o','c','a','l','h','o','s','t',0, 0,1,0,1};
    uint8_t requests[sizeof question * 2];
    memcpy(requests, question, sizeof question); memcpy(requests + sizeof question, question, sizeof question);
    put16(requests, sizeof question - 2); put16(requests + sizeof question, sizeof question - 2);
    put16(requests + sizeof question + 2, 0x5252);
    tcp_send(&c, c.snd++, 0x18, requests, 1, 0);
    tcp_send(&c, c.snd, 0x18, requests + 1, sizeof requests - 1, 0);
    c.snd += sizeof requests - 1;
    uint8_t reply[1024]; size_t have = 0;
    unsigned answers = 0;
    const uint64_t until = mc_clock_ms() + 3000;
    seg packet;
    while (answers < 2 && mc_clock_ms() < until) {
        if (!next_seg(&c, 0x10, &packet, 100)) continue;
        if (!packet.len || packet.seq != c.rcv) continue;
        CHECK(have + packet.len <= sizeof reply);
        if (have + packet.len > sizeof reply) break;
        memcpy(reply + have, packet.data, packet.len); have += packet.len;
        c.rcv += (uint32_t)packet.len;
        tcp_send(&c, c.snd, 0x10, NULL, 0, 0);
        while (have >= 2 && have >= 2u + get16(reply)) {
            const unsigned length = get16(reply);
            CHECK(length >= 43); T();
            CHECK_EQ(get16(reply + 2), answers ? 0x5252 : 0x5151); T();
            CHECK_EQ(get16(reply + 4) & 0x800F, 0x8000); T();
            CHECK(get16(reply + 8) > 0); T();
            if (length >= 43) { CHECK_EQ(get32(reply + 2 + 27 + 12), ROUTER); T(); }
            have -= length + 2; memmove(reply, reply + length + 2, have); answers++;
        }
    }
    CHECK_EQ(answers, 2); T();
    tcp_send(&c, c.snd++, 0x11, NULL, 0, 0);
    CHECK(next_seg(&c, 0x01, &packet, 1000)); T();
    c.rcv = packet.seq + 1; tcp_send(&c, c.snd, 0x10, NULL, 0, 0);
}

static void test_tcp(void) {
    uint16_t port;
    mc_socket l = tcp_server(&port), a = MC_BAD_SOCKET;
    conn c = {4000, port, ROUTER, 0, 0, 1072};
    nframes = 0;
    CHECK(tcp_connect(&c, l, &a)); T();
    CHECK(a != MC_BAD_SOCKET); T();
    /* Out of order: "world" first is held (a duplicate ACK of 1001); then
       "hello" fills the gap and both are acknowledged and delivered. */
    seg s;
    tcp_send(&c, 1006, 0x18, "world", 5, 0);
    CHECK(next_seg(&c, 0x10, &s, 1000)); T();
    CHECK_EQ(s.ack, 1001); T();
    tcp_send(&c, 1001, 0x18, "hello", 5, 0);
    CHECK(next_seg(&c, 0x10, &s, 1000)); T();
    CHECK_EQ(s.ack, 1011); T();
    c.snd = 1011;
    uint8_t buf[4096];
    int eof;
    size_t got = read_poll(a, buf, 10, &eof);
    CHECK_EQ(got, 10); T();
    CHECK(!memcmp(buf, "helloworld", 10)); T();
    /* 3000 bytes from the host: segments of at most 536 bytes (the
       Macintosh's MSS), never more than its window (1072) unacknowledged. */
    uint8_t big[3000];
    for (int k = 0; k < 3000; k++) big[k] = (uint8_t)(k * 7 + 3);
    send(a, (const char *)big, 3000, 0);
    uint8_t recvd[3000];
    size_t have = 0;
    while (have < 3000) {
        uint32_t inflight = 0;
        seg first;
        int n = 0;
        while (next_seg(&c, 0x10, &s, n ? 100 : 2000)) {
            if (!s.len) continue;
            CHECK(s.len <= 536);
            CHECK_EQ(s.seq, c.rcv + inflight);
            memcpy(recvd + have + inflight, s.data, s.len);
            if (!n) first = s;
            inflight += (uint32_t)s.len;
            n++;
        }
        CHECK(n > 0);
        CHECK(inflight <= 1072);
        if (!n) break;
        (void)first;
        have += inflight;
        c.rcv += inflight;
        tcp_send(&c, c.snd, 0x10, NULL, 0, 0);
    }
    CHECK_EQ(have, 3000); T();
    CHECK(!memcmp(recvd, big, 3000)); T();
    /* Retransmission: a segment left unacknowledged comes again once the
       timer runs out (RFC 6298), with the same sequence number. */
    send(a, "again", 5, 0);
    CHECK(next_seg(&c, 0x18, &s, 1000)); T();
    const uint32_t seq = s.seq;
    CHECK(!next_seg(&c, 0x18, &s, 100)); T();
    skew += 1500;
    CHECK(next_seg(&c, 0x18, &s, 1000)); T();
    CHECK_EQ(s.seq, seq); T();
    CHECK_EQ(s.len, 5); T();
    c.rcv += 5;
    tcp_send(&c, c.snd, 0x10, NULL, 0, 0);
    /* The host closes: a FIN after the data; the Macintosh acknowledges it
       and closes too; the NAT acknowledges the Macintosh's FIN. */
    mc_close_socket(a);
    CHECK(next_seg(&c, 0x01, &s, 1000)); T();
    CHECK_EQ(s.seq, c.rcv); T();
    c.rcv++;
    tcp_send(&c, c.snd, 0x11, NULL, 0, 0);
    CHECK(next_seg(&c, 0x10, &s, 1000)); T();
    CHECK_EQ(s.ack, c.snd + 1); T();

    /* A second connection closed by the Macintosh first: the host reads the
       end of the stream; then its close brings the NAT's FIN. */
    nframes = 0;
    conn d = {4001, port, ROUTER, 0, 0, 8192};
    CHECK(tcp_connect(&d, l, &a)); T();
    tcp_send(&d, d.snd, 0x19, "bye", 3, 0);
    CHECK(next_seg(&d, 0x10, &s, 1000)); T();
    CHECK_EQ(s.ack, d.snd + 4); T();
    d.snd += 4;
    got = read_poll(a, buf, 100, &eof);
    CHECK_EQ(got, 3); T();
    if (!eof) read_poll(a, buf, 100, &eof);
    CHECK(eof); T();
    mc_close_socket(a);
    CHECK(next_seg(&d, 0x01, &s, 1000)); T();
    d.rcv = s.seq + 1;
    mc_nat_stats st;
    mc_nat_get_stats(nat, &st);
    const unsigned closed = st.tcp_closed;
    tcp_send(&d, d.snd, 0x10, NULL, 0, 0);
    mc_nat_get_stats(nat, &st);
    CHECK_EQ(st.tcp_closed, closed + 1); T();   /* LAST-ACK to CLOSED */
    mc_close_socket(l);

    /* Refused: a SYN to a port nobody listens on gets RST, ACK. */
    uint16_t dead;
    mc_socket z = tcp_server(&dead);
    mc_close_socket(z);
    nframes = 0;
    conn e = {4002, dead, ROUTER, 0, 0, 8192};
    tcp_send(&e, 777, 0x02, NULL, 0, 536);
    CHECK(next_seg(&e, 0x04, &s, 3000)); T();
    CHECK_EQ(s.flags & 0x14, 0x14); T();
    CHECK_EQ(s.ack, 778); T();
    /* A segment of no connection: RST at its acknowledgment number. */
    nframes = 0;
    e.rcv = 5555;
    tcp_send(&e, 900, 0x10, NULL, 0, 0);
    CHECK(next_seg(&e, 0x04, &s, 1000)); T();
    CHECK_EQ(s.seq, 5555); T();
}

/* ---- ICMP ----------------------------------------------------------------------------------------- */

static int is_echo_reply(const uint8_t *f, size_t len, const void *arg) {
    const uint8_t *t = ip_of(f, 1);
    (void)len;
    return t && t[0] == 0 && get16(t + 4) == 0x77 && get16(t + 6) == *(const uint16_t *)arg;
}

static void echo(uint32_t dst, uint16_t seq) {
    uint8_t t[40];
    memset(t, 0, sizeof t);
    t[0] = 8;
    put16(t + 4, 0x77);
    put16(t + 6, seq);
    for (int k = 8; k < 40; k++) t[k] = (uint8_t)k;
    send_ip(router_mac, GUEST, dst, 1, t, sizeof t);
}

static void test_icmp(void) {
    nframes = 0;
    echo(ROUTER, 1);
    int k = wait_for(is_echo_reply, &(uint16_t){1}, 100);
    CHECK(k >= 0); T();
    if (k >= 0) {
        CHECK_EQ(get32(frames[k] + 14 + 12), ROUTER); T();
        CHECK(checksum_ok(frames[k])); T();
        CHECK(!memcmp(ip_of(frames[k], 1) + 8, out + 14 + 20 + 8, 32)); T();
    }
#ifdef _WIN32
    /* Through the host's echo service: its own loopback answers. */
    nframes = 0;
    echo(IP4(127, 0, 0, 1), 2);
    k = wait_for(is_echo_reply, &(uint16_t){2}, 3000);
    CHECK(k >= 0); T();
    if (k >= 0) { CHECK_EQ(get32(frames[k] + 14 + 12), IP4(127, 0, 0, 1)); T(); }
#endif
}

/* ---- port forwards -------------------------------------------------------------------------------- */

static void test_forward(void) {
    mc_nat_destroy(nat);
    const mc_nat_forward fw[2] = {{0, 0, 0, 80}, {1, 0, 0, 53}};
    nat = make_nat(fw, 2);
    CHECK(nat != NULL); T();
    if (!nat) return;
    const uint16_t tport = mc_nat_forward_port(nat, 0), uport = mc_nat_forward_port(nat, 1);
    CHECK(tport != 0 && uport != 0); T();
    /* TCP: a host client's connection becomes the NAT's SYN to port 80. */
    mc_socket cl = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    mc_set_nonblocking(cl);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(IP4(127, 0, 0, 1));
    sa.sin_port = htons(tport);
    connect(cl, (struct sockaddr *)&sa, sizeof sa);
    conn c = {80, 0, ROUTER, 0, 0, 8192};
    seg s;
    memset(&s, 0, sizeof s);
    int k = -1;
    for (int t = 0; t < 400 && k < 0; t++) {
        mc_nat_poll(nat, 5);
        for (unsigned j = 0; j < nframes && k < 0; j++)
            if (parse_tcp(frames[j], &s) && s.dport == 80 && (s.flags & 0x02)) k = (int)j;
    }
    CHECK(k >= 0); T();
    if (k < 0) { mc_close_socket(cl); return; }
    drop((unsigned)k);
    CHECK_EQ(s.sport, 49152); T();
    CHECK_EQ(s.mss, 1460); T();
    c.rport = s.sport;
    c.rcv = s.seq + 1;
    c.snd = 5000;
    tcp_send(&c, c.snd, 0x12, NULL, 0, 1460);   /* SYN, ACK */
    c.snd++;
    CHECK(next_seg(&c, 0x10, &s, 1000)); T();
    CHECK_EQ(s.ack, c.snd); T();
    send(cl, "GET", 3, 0);
    CHECK(next_seg(&c, 0x18, &s, 2000)); T();
    CHECK(s.len == 3 && !memcmp(s.data, "GET", 3)); T();
    c.rcv += 3;
    tcp_send(&c, c.snd, 0x18, "OK", 2, 0);
    c.snd += 2;
    uint8_t buf[16];
    int eof;
    CHECK_EQ(read_poll(cl, buf, 2, &eof), 2); T();
    CHECK(!memcmp(buf, "OK", 2)); T();
    mc_close_socket(cl);
    /* UDP: a host client's datagram arrives from router:NATPORT; the reply
       goes back to the client. */
    mc_socket u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    mc_set_nonblocking(u);
    sa.sin_port = htons(uport);
    sendto(u, "query", 5, 0, (struct sockaddr *)&sa, sizeof sa);
    const uint16_t gp = 53;
    nframes = 0;
    k = wait_for(is_udp_to, &gp, 1000);
    CHECK(k >= 0); T();
    if (k >= 0) {
        const uint8_t *ud = ip_of(frames[k], 17);
        CHECK(!memcmp(ud + 8, "query", 5)); T();
        const uint16_t from = get16(ud);
        send_udp(GUEST, 53, ROUTER, from, "answer", 6, 0);
        char rb[16];
        int got = -1;
        for (int t = 0; t < 200 && got < 0; t++) {
            mc_nat_poll(nat, 5);
            got = recv(u, rb, sizeof rb, 0);
        }
        CHECK_EQ(got, 6); T();
        CHECK(got == 6 && !memcmp(rb, "answer", 6)); T();
    }
    mc_close_socket(u);
}

/* ---- the segment (net.h) ------------------------------------------------------------------------ */

typedef struct probe_host {
    mc_net *net;
    volatile int seen_multicast, polls;
    int emitted;
} probe_host;
static void probe_input(void *self, const uint8_t *f, size_t len) {
    probe_host *h = self;
    static const uint8_t appletalk[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
    if (len >= 14 && !memcmp(f, appletalk, 6)) h->seen_multicast = 1;
}
static void probe_poll(void *self, int timeout_ms) {
    probe_host *h = self;
    (void)timeout_ms;
    h->polls++;
    if (!h->emitted) {
        /* An AppleTalk host's broadcast (802.3 with SNAP, an AARP probe's
           shape): every station sees it. */
        uint8_t f[60];
        memset(f, 0, sizeof f);
        memcpy(f, bcast_mac, 6);
        static const uint8_t mine[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x10};
        memcpy(f + 6, mine, 6);
        put16(f + 12, 46);
        f[14] = 0xAA; f[15] = 0xAA; f[16] = 0x03;
        mc_net_emit(h->net, h, f, sizeof f);
        h->emitted = 1;
    }
}
static void probe_destroy(void *self) { (void)self; }
static const mc_net_host_ops probe_ops = {probe_input, probe_poll, NULL, probe_destroy};

static void test_segment(void) {
    char error[256];
    mc_net *net = mc_net_create(error, sizeof error);
    CHECK(net != NULL); T();
    if (!net) return;
    mc_nat_config c;
    memset(&c, 0, sizeof c);
    mc_net_close(net);
    net = mc_net_open(&c, error, sizeof error);   /* the NAT on its segment, started */
    CHECK(net != NULL); T();
    if (!net) return;
    mc_net_close(net);
    /* A segment with a host that never waits: the thread waits on its own. */
    net = mc_net_create(error, sizeof error);
    static probe_host probe;
    memset(&probe, 0, sizeof probe);
    probe.net = net;
    CHECK(mc_net_attach_host(net, &probe_ops, &probe)); T();
    CHECK(mc_net_start(net, error, sizeof error)); T();
    CHECK(!mc_net_attach_host(net, &probe_ops, &probe)); T();   /* not once started */
    /* The Macintosh's AppleTalk multicast reaches the host. */
    uint8_t f[60];
    memset(f, 0, sizeof f);
    static const uint8_t appletalk[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF};
    memcpy(f, appletalk, 6);
    memcpy(f + 6, guest_mac, 6);
    put16(f + 12, 46);
    mc_net_send(net, f, sizeof f);
    for (int k = 0; k < 200 && !probe.seen_multicast; k++) {
        const uint64_t t = mc_clock_ms();
        while (mc_clock_ms() - t < 5) {}
    }
    CHECK(probe.seen_multicast); T();
    /* The host's broadcast is the Macintosh's to take. */
    uint8_t got[1514];
    size_t len = 0;
    int have = 0;
    for (int k = 0; k < 400 && !have; k++) {
        have = mc_net_next(net, got, &len);
        if (!have) { const uint64_t t = mc_clock_ms(); while (mc_clock_ms() - t < 5) {} }
    }
    CHECK(have && len == 60 && got[14] == 0xAA); T();
    mc_net_pop(net);
    CHECK(!mc_net_next(net, got, &len)); T();
    mc_net_close(net);
}

int main(void) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    nat = make_nat(NULL, 0);
    if (!nat) return 1;
    test_arp();
    test_dhcp();
    test_dns();
    test_udp();
    test_fragments();
    test_dns_tcp();
    test_tcp();
    test_icmp();
    test_forward();
    mc_nat_destroy(nat);
    test_segment();
    printf("test_nat: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
