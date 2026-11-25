/* test_appletalk: the file server's AppleTalk layers on a simulated cable
 * between Livadix's node and a second node standing for the Macintosh, with
 * the clock in the test's hands. Expected values from Inside AppleTalk,
 * 2nd ed.: chapter 2 and 3 (AARP probes, ten of them 1/5 s apart; answers
 * to requests and to probes for a held address; a probe answered makes the
 * prober try another address; node IDs never 0, $FE or $FF; the startup
 * range), chapter 4 (the extended header, the checksum, a bad checksum
 * dropped, network numbers equivalent through 0), chapter 6 (echo),
 * chapter 7 (LkUp and LkUp-Reply, wildcards, case per appendix D),
 * chapter 9 (XO: one execution, duplicates answered from the kept
 * response, a lost response packet sent again for the bitmap, TRel; ALO;
 * EOM; retries and their end), chapter 11 (GetStatus, OpenSess and its
 * reply, Command and its result, Write with WriteContinue, Tickle,
 * CloseSess, the 2-minute session timer, Attention, BadVersNum). Also DES
 * (FIPS 46-3 vectors) and the names (macroman.h).
 *
 * Livadix's own code. */
#include "../test.h"
#include "../../src/net/appletalk/asp.h"
#include "../../src/net/afp/des.h"
#include "../../src/net/afp/macroman.h"

#include <stdlib.h>
#include <string.h>

int mc_test_failures;
static int tests;
#define T() (tests++)

static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* ---- the cable ------------------------------------------------------------------ */

#define WIRE 512
static uint8_t wire[WIRE][1514];
static size_t wire_len[WIRE];
static int wire_from[WIRE];
static unsigned nwire;
static uint64_t now;
static int drop_next_resp_seq = -1;        /* drop the next TResp with this sequence number */
static unsigned frames_seen;

typedef struct station {
    mc_atalk *at;
    int id;
} station;
static station st[2];
static const uint8_t mac_a[6] = {0x02, 0x4C, 0x56, 0x44, 0x58, 0x03}, mac_b[6] = {0x08, 0x00, 0x07, 0x4D, 0x41, 0x43};

static void out(void *ctx, const uint8_t *f, size_t n) {
    const station *s = ctx;
    frames_seen++;
    /* an ATP TResp (SNAP DDP, type 3, function 10) with the chosen sequence number is lost */
    if (drop_next_resp_seq >= 0 && n > 22 + 13 + 2 && f[17] == 0x08 && f[22 + 12] == 3 && (f[35] & 0xC0) == 0x80 &&
        f[36] == drop_next_resp_seq) {
        drop_next_resp_seq = -1;
        return;
    }
    if (nwire == WIRE) return;
    memcpy(wire[nwire], f, n);
    wire_len[nwire] = n;
    wire_from[nwire] = s->id;
    nwire++;
}

/* Delivers what is on the cable (and what that sends) to the other station. */
static void pump(void) {
    for (unsigned k = 0; k < nwire; k++) {
        const int to = 1 - wire_from[k];
        mc_atalk_input(st[to].at, wire[k], wire_len[k], now);
    }
    nwire = 0;
}

static mc_atp *ticked[4];
static void run(uint64_t ms) {
    for (uint64_t t = 0; t < ms; t += 10) {
        now += 10;
        mc_atalk_tick(st[0].at, now);
        mc_atalk_tick(st[1].at, now);
        for (int i = 0; i < 4; i++)
            if (ticked[i]) mc_atp_tick(ticked[i], now);
        for (int i = 0; i < 4; i++) pump();
    }
}

static void cable(uint32_t seed_a, uint32_t seed_b) {
    for (int i = 0; i < 2; i++)
        if (st[i].at) mc_atalk_destroy(st[i].at);
    nwire = 0;
    st[0].id = 0;
    st[1].id = 1;
    st[0].at = mc_atalk_create(mac_a, seed_a, out, &st[0]);
    st[1].at = mc_atalk_create(mac_b, seed_b, out, &st[1]);
}

/* ---- DES, names ---------------------------------------------------------------------- */

static void test_des(void) {
    T();
    static const uint8_t key[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    static const uint8_t plain[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    static const uint8_t cipher[8] = {0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05};
    uint8_t got[8];
    mc_des_encrypt(key, plain, got);
    CHECK(!memcmp(got, cipher, 8));
    /* FIPS 81 / NBS validation: key 0101..01 (parity only), 95F8A5E5DD31D900 -> 8000000000000000 */
    static const uint8_t k2[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    static const uint8_t p2[8] = {0x95, 0xF8, 0xA5, 0xE5, 0xDD, 0x31, 0xD9, 0x00};
    mc_des_encrypt(k2, p2, got);
    CHECK(got[0] == 0x80 && !got[1] && !got[2] && !got[3] && !got[4] && !got[5] && !got[6] && !got[7]);
    /* parity bits of the key do not matter */
    static const uint8_t k3[8] = {0x12, 0x35, 0x56, 0x78, 0x9A, 0xBD, 0xDE, 0xF0};
    uint8_t a[8], b[8];
    mc_des_encrypt(k3, plain, a);
    static const uint8_t k4[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
    mc_des_encrypt(k4, plain, b);
    CHECK(!memcmp(a, b, 8));
}

static void test_names(void) {
    T();
    uint16_t w[64];
    uint8_t m[256];
    int exact;
    /* Mac Roman to Unicode: é ($8E), the Apple logo ($F0), $DB the currency sign */
    CHECK(mc_macroman_to_unicode(0x8E) == 0x00E9);
    CHECK(mc_macroman_to_unicode(0xF0) == 0xF8FF);
    CHECK(mc_macroman_to_unicode(0xDB) == 0x00A4);
    uint8_t c;
    CHECK(mc_unicode_to_macroman(0x20AC, &c) && c == 0xDB);
    CHECK(!mc_unicode_to_macroman(0x4E2D, &c));
    /* forbidden characters to private use and back */
    const uint8_t mac1[] = {'a', '/', 'b', '?', '*', '"', '<', '>', '|', '\\', 0x0D, '.'};
    CHECK_EQ(mc_mac_to_host_name(mac1, sizeof mac1, w, 64), sizeof mac1);
    CHECK(w[1] == 0xF022 && w[3] == 0xF025 && w[4] == 0xF021 && w[5] == 0xF020 && w[6] == 0xF023 && w[7] == 0xF024 && w[8] == 0xF027 &&
          w[9] == 0xF026 && w[10] == 0xF00D && w[11] == 0xF029);
    CHECK_EQ(mc_host_to_mac_name(w, m, &exact), sizeof mac1);
    CHECK(exact && !memcmp(m, mac1, sizeof mac1));
    /* a trailing space; a period inside stays */
    const uint8_t mac2[] = {'a', '.', 'b', ' '};
    mc_mac_to_host_name(mac2, 4, w, 64);
    CHECK(w[1] == '.' && w[3] == 0xF028);
    /* names that cannot be: ':' and NUL */
    const uint8_t bad[] = {'a', ':', 'b'};
    CHECK_EQ(mc_mac_to_host_name(bad, 3, w, 64), 0);
    /* Unicode without Mac Roman, and long names: not exact */
    const uint16_t han[] = {'x', 0x4E2D, 0};
    mc_host_to_mac_name(han, m, &exact);
    CHECK(!exact && m[1] == '_');
    uint16_t longname[40];
    for (int i = 0; i < 32; i++) longname[i] = 'a';
    longname[32] = 0;
    mc_host_to_mac_name(longname, m, &exact);
    CHECK(!exact);
    longname[31] = 0;
    mc_host_to_mac_name(longname, m, &exact);
    CHECK(exact);
    /* case-insensitive per Table D-2 */
    const uint8_t e1[] = {'C', 'a', 'f', 0x8E}, e2[] = {'c', 'A', 'F', 0x83};
    CHECK(mc_mac_name_equal(e1, 4, e2, 4));
    const uint8_t e3[] = {'c', 'a', 'f', 'e'};
    CHECK(!mc_mac_name_equal(e1, 4, e3, 4));
}

static void test_nbp_match(void) {
    T();
    const uint8_t eq[] = "=", wild[] = "L\xC5", wild2[] = "\xC5" "ix", name[] = "Livadix", srv[] = "AFPServer";
    CHECK(mc_nbp_match(eq, 1, name, 7));
    CHECK(mc_nbp_match(wild, 2, name, 7));
    CHECK(mc_nbp_match(wild2, 3, name, 7));
    CHECK(mc_nbp_match((const uint8_t *)"afpserver", 9, srv, 9));
    CHECK(!mc_nbp_match((const uint8_t *)"AFPServe", 8, srv, 9));
    CHECK(!mc_nbp_match((const uint8_t *)"X\xC5", 2, name, 4));
}

/* ---- AARP and DDP ----------------------------------------------------------------- */

static unsigned received;
static uint8_t last_data[600];
static size_t last_len;
static mc_atalk_addr last_from;
static void sink(void *ctx, const mc_atalk_addr *from, uint8_t type, const uint8_t *d, size_t n, uint64_t t) {
    (void)ctx; (void)type; (void)t;
    received++;
    memcpy(last_data, d, n);
    last_len = n;
    last_from = *from;
}

static void test_aarp(void) {
    T();
    cable(1, 2);
    now = 1000;
    mc_atalk_start(st[0].at, now);
    mc_atalk_start(st[1].at, now);
    /* ten probes 200 ms apart, then the address */
    run(1500);
    CHECK(!mc_atalk_ready(st[0].at));
    run(1000);
    CHECK(mc_atalk_ready(st[0].at) && mc_atalk_ready(st[1].at));
    mc_atalk_stats s;
    mc_atalk_get_stats(st[0].at, &s);
    CHECK_EQ(s.probes_sent, 10);
    const mc_atalk_addr a = mc_atalk_address(st[0].at), b = mc_atalk_address(st[1].at);
    CHECK(a.net >= 0xFF00 && a.net <= 0xFFFE && a.node >= 1 && a.node <= 0xFD);
    CHECK(b.net >= 0xFF00 && b.net <= 0xFFFE);
    /* a probe for the held address is answered, to the prober */
    uint8_t f[64];
    memset(f, 0, sizeof f);
    static const uint8_t multicast[6] = {0x09, 0x00, 0x07, 0xFF, 0xFF, 0xFF}, snap_aarp[5] = {0, 0, 0, 0x80, 0xF3};
    memcpy(f, multicast, 6);
    memcpy(f + 6, mac_b, 6);
    put16(f + 12, 3 + 5 + 28);
    f[14] = 0xAA; f[15] = 0xAA; f[16] = 3;
    memcpy(f + 17, snap_aarp, 5);
    uint8_t *p = f + 22;
    put16(p, 1); put16(p + 2, 0x809B); p[4] = 6; p[5] = 4; put16(p + 6, 3);
    memcpy(p + 8, mac_b, 6);
    put16(p + 15, a.net); p[17] = a.node;
    put16(p + 25, a.net); p[27] = a.node;
    nwire = 0;
    mc_atalk_input(st[0].at, f, 60, now);
    CHECK_EQ(nwire, 1);
    CHECK(!memcmp(wire[0], mac_b, 6) && get16(wire[0] + 22 + 6) == 2 && !memcmp(wire[0] + 22 + 8, mac_a, 6) &&
          get16(wire[0] + 22 + 15) == a.net && wire[0][22 + 17] == a.node);
    mc_atalk_get_stats(st[0].at, &s);
    CHECK_EQ(s.aarp_replies, 1);
    /* a request for it too */
    put16(p + 6, 1);
    put16(p + 15, b.net); p[17] = b.node;
    nwire = 0;
    mc_atalk_input(st[0].at, f, 60, now);
    CHECK(nwire == 1 && get16(wire[0] + 22 + 6) == 2);
    nwire = 0;
    /* a prober told the address is taken tries another */
    mc_atalk_start(st[1].at, now);
    run(50);
    const mc_atalk_addr tentative = mc_atalk_address(st[1].at);
    put16(p + 6, 2);
    memcpy(p + 8, mac_a, 6);
    memcpy(f + 6, mac_a, 6);
    put16(p + 15, tentative.net); p[17] = tentative.node;
    mc_atalk_input(st[1].at, f, 60, now);
    const mc_atalk_addr moved = mc_atalk_address(st[1].at);
    CHECK(moved.net != tentative.net || moved.node != tentative.node);
    run(2500);
    CHECK(mc_atalk_ready(st[1].at));
}

static void test_ddp(void) {
    T();
    cable(3, 4);
    now = 1000;
    mc_atalk_start(st[0].at, now);
    mc_atalk_start(st[1].at, now);
    run(2500);
    CHECK_EQ(mc_atalk_open(st[0].at, 0x80, sink, NULL), 0x80);
    CHECK_EQ(mc_atalk_open(st[0].at, 0x80, sink, NULL), 0);
    CHECK(mc_atalk_open(st[1].at, 0, sink, NULL) >= 128);
    CHECK_EQ(mc_atalk_open(st[1].at, 0x81, sink, NULL), 0x81);
    mc_atalk_addr to = mc_atalk_address(st[0].at);
    to.socket = 0x80;
    received = 0;
    const uint8_t hello[] = "hello";
    /* the first datagram waits for AARP, then goes */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    run(100);
    CHECK_EQ(received, 1);
    CHECK(last_len == 5 && !memcmp(last_data, "hello", 5));
    const mc_atalk_addr b = mc_atalk_address(st[1].at);
    CHECK(last_from.net == b.net && last_from.node == b.node && last_from.socket == 0x81);
    /* a datagram with a bad checksum is dropped */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    CHECK_EQ(nwire, 1);
    wire[0][22 + 13] ^= 1;
    pump();
    CHECK_EQ(received, 1);
    mc_atalk_stats s;
    mc_atalk_get_stats(st[0].at, &s);
    CHECK_EQ(s.bad_checksums, 1);
    /* checksum 0: none, accepted */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    wire[0][22 + 2] = wire[0][22 + 3] = 0;
    pump();
    CHECK_EQ(received, 2);
    /* destination network 0 matches */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    wire[0][22 + 4] = wire[0][22 + 5] = 0;
    wire[0][22 + 2] = wire[0][22 + 3] = 0;
    pump();
    CHECK_EQ(received, 3);
    /* another network number is not ours */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    wire[0][22 + 4] ^= 0x01;
    wire[0][22 + 5] ^= 0x01;
    wire[0][22 + 2] = wire[0][22 + 3] = 0;
    pump();
    CHECK_EQ(received, 3);
    /* the frame's 802.3 header: length, LLC and SNAP */
    CHECK(mc_atalk_send(st[1].at, 0x81, &to, 0x10, hello, 5, now));
    CHECK(get16(wire[0] + 12) == 3 + 5 + 13 + 5 && wire[0][14] == 0xAA && wire[0][15] == 0xAA && wire[0][16] == 3 && wire[0][17] == 0x08 &&
          wire[0][21] == 0x9B && wire_len[0] == 60);
    pump();
    /* AEP: the echoer answers */
    received = 0;
    mc_atalk_addr echo = mc_atalk_address(st[0].at);
    echo.socket = MC_SOCKET_ECHO;
    const uint8_t ping[] = {1, 'p', 'i', 'n', 'g'};
    CHECK(mc_atalk_send(st[1].at, 0x81, &echo, MC_DDP_AEP, ping, 5, now));
    run(20);
    CHECK_EQ(received, 1);
    CHECK(last_data[0] == 2 && !memcmp(last_data + 1, "ping", 4));
}

static void test_nbp(void) {
    T();
    cable(5, 6);
    now = 1000;
    mc_atalk_start(st[0].at, now);
    mc_atalk_start(st[1].at, now);
    run(2500);
    CHECK(mc_nbp_register(st[0].at, "Livadix", "AFPServer", 0x90));
    const uint8_t sock = mc_atalk_open(st[1].at, 0, sink, NULL);
    const mc_atalk_addr b = mc_atalk_address(st[1].at);
    uint8_t lk[64];
    size_t n = 0;
    lk[n++] = 0x21;            /* LkUp, one tuple */
    lk[n++] = 0x42;            /* NBP ID */
    put16(lk + n, b.net);
    lk[n + 2] = b.node;
    lk[n + 3] = sock;
    lk[n + 4] = 0;
    n += 5;
    lk[n++] = 1; lk[n++] = '=';
    lk[n++] = 9; memcpy(lk + n, "afpserver", 9); n += 9;
    lk[n++] = 1; lk[n++] = '*';
    const mc_atalk_addr bcast = {0, 0xFF, MC_SOCKET_NIS};
    received = 0;
    CHECK(mc_atalk_send(st[1].at, sock, &bcast, MC_DDP_NBP, lk, n, now));
    run(50);
    CHECK_EQ(received, 1);
    const mc_atalk_addr a = mc_atalk_address(st[0].at);
    CHECK(last_data[0] == 0x31 && last_data[1] == 0x42);
    CHECK(get16(last_data + 2) == a.net && last_data[4] == a.node && last_data[5] == 0x90);
    CHECK(last_data[7] == 7 && !memcmp(last_data + 8, "Livadix", 7) && last_data[15] == 9 && !memcmp(last_data + 16, "AFPServer", 9) &&
          last_data[25] == 1 && last_data[26] == '*');
    CHECK(last_from.socket == MC_SOCKET_NIS);
    /* another type, another zone: no reply */
    lk[n - 1] = 'Z';
    CHECK(mc_atalk_send(st[1].at, sock, &bcast, MC_DDP_NBP, lk, n, now));
    run(50);
    CHECK_EQ(received, 1);
    lk[n - 1] = '*';
    lk[2 + 5 + 2 + 1] = 'x';
    CHECK(mc_atalk_send(st[1].at, sock, &bcast, MC_DDP_NBP, lk, n, now));
    run(50);
    CHECK_EQ(received, 1);
    /* confirm: a directed LkUp for the exact name */
    n = 7;
    lk[n++] = 7; memcpy(lk + n, "LIVADIX", 7); n += 7;
    lk[n++] = 9; memcpy(lk + n, "AFPServer", 9); n += 9;
    lk[n++] = 0;
    mc_atalk_addr direct = a;
    direct.socket = MC_SOCKET_NIS;
    CHECK(mc_atalk_send(st[1].at, sock, &direct, MC_DDP_NBP, lk, n, now));
    run(50);
    CHECK_EQ(received, 2);
}

/* ---- ATP ------------------------------------------------------------------------- */

static unsigned executed;
static uint8_t big[2000];
static void responder(void *ctx, mc_atp *atp, const mc_atp_request *q, uint64_t t) {
    (void)ctx;
    executed++;
    const uint8_t user[4] = {9, 8, 7, 6};
    mc_atp_respond(atp, q, user, big, q->user[0] == 'B' ? sizeof big : 10, t);
}
static int done_status = 99;
static size_t done_len;
static uint8_t done_user[4], done_data[8 * 578];
static void done(void *ctx, int status, const uint8_t user[4], const uint8_t *d, size_t n, uint64_t t) {
    (void)ctx; (void)t;
    done_status = status;
    done_len = n;
    memcpy(done_user, user, 4);
    memcpy(done_data, d, n);
}

static void test_atp(void) {
    T();
    cable(11, 12);
    now = 1000;
    mc_atalk_start(st[0].at, now);
    mc_atalk_start(st[1].at, now);
    run(2500);
    for (size_t k = 0; k < sizeof big; k++) big[k] = (uint8_t)(k * 7);
    mc_atp *srv = mc_atp_open(st[0].at, 0, responder, NULL), *cli = mc_atp_open(st[1].at, 0, NULL, NULL);
    CHECK(srv && cli);
    ticked[0] = srv;
    ticked[1] = cli;
    mc_atalk_addr to = mc_atalk_address(st[0].at);
    to.socket = mc_atp_socket(srv);
    /* XO, eight packets asked, 2000 bytes: four packets, EOM on the fourth */
    executed = 0;
    done_status = 99;
    const uint8_t ub[4] = {'B', 0, 0, 0};
    CHECK(mc_atp_send_request(cli, &to, 1, ub, (const uint8_t *)"req", 3, 8, 500, 5, done, NULL, now) > 0);
    run(100);
    CHECK_EQ(done_status, 0);
    CHECK_EQ(done_len, 2000);
    CHECK(!memcmp(done_data, big, 2000));
    CHECK(done_user[0] == 9 && done_user[3] == 6);
    CHECK_EQ(executed, 1);
    /* a lost response packet: the retry asks for it alone, the request is not executed again */
    done_status = 99;
    drop_next_resp_seq = 2;
    CHECK(mc_atp_send_request(cli, &to, 1, ub, NULL, 0, 8, 500, 5, done, NULL, now) > 0);
    run(100);
    CHECK_EQ(done_status, 99);
    run(600);
    CHECK_EQ(done_status, 0);
    CHECK_EQ(done_len, 2000);
    CHECK(!memcmp(done_data, big, 2000));
    CHECK_EQ(executed, 2);
    /* the bitmap limits the response: two packets asked, 2000 bytes offered */
    done_status = 99;
    CHECK(mc_atp_send_request(cli, &to, 0, ub, NULL, 0, 2, 500, 5, done, NULL, now) > 0);
    run(100);
    CHECK_EQ(done_status, 0);
    CHECK_EQ(done_len, 2 * 578);
    /* no answer: the retries end */
    mc_atalk_addr nowhere = to;
    nowhere.socket = 0x7E;
    done_status = 99;
    CHECK(mc_atp_send_request(cli, &nowhere, 0, ub, NULL, 0, 1, 200, 2, done, NULL, now) > 0);
    run(1000);
    CHECK_EQ(done_status, -1);
    ticked[0] = ticked[1] = NULL;
    mc_atp_close(srv);
    mc_atp_close(cli);
}

/* ---- ASP --------------------------------------------------------------------------- */

static unsigned commands, sessions_open, sessions_closed;
static size_t srv_status(void *ctx, uint8_t *o, size_t cap) { (void)ctx; (void)cap; memcpy(o, "STATUS", 6); return 6; }
static void *srv_opened(void *ctx, unsigned id, const mc_atalk_addr *ws) { (void)ctx; (void)id; (void)ws; sessions_open++; return &commands; }
static void srv_closed(void *ctx, void *s) { (void)ctx; (void)s; sessions_closed++; }
static int32_t srv_command(void *ctx, void *s, const uint8_t *cmd, size_t n, uint8_t *reply, size_t *rn) {
    (void)ctx; (void)s;
    commands++;
    memcpy(reply, cmd, n);
    *rn = n;
    return -5018;
}
static uint8_t written[4624];
static size_t written_len;
static long srv_write_begin(void *ctx, void *s, const uint8_t *cmd, size_t n, int32_t *result, uint8_t *reply, size_t *rn) {
    (void)ctx; (void)s; (void)reply; (void)rn;
    if (n && cmd[0] == 0xEE) { *result = -5000; return -1; }
    return 1500;
}
static int32_t srv_write_end(void *ctx, void *s, const uint8_t *cmd, size_t n, const uint8_t *d, size_t dn, uint8_t *reply, size_t *rn) {
    (void)ctx; (void)s; (void)cmd; (void)n;
    memcpy(written, d, dn);
    written_len = dn;
    reply[0] = 0x55;
    *rn = 1;
    return 0;
}
static const mc_asp_server_ops sops = {srv_status, srv_opened, srv_closed, srv_command, srv_write_begin, srv_write_end};

/* The workstation's socket: it answers WriteContinue with data and counts tickles and attentions. */
static unsigned ws_tickles, ws_attentions, ws_closes;
static uint8_t ws_data[1500];
static void wss(void *ctx, mc_atp *atp, const mc_atp_request *q, uint64_t t) {
    (void)ctx;
    if (q->user[0] == 7) {
        CHECK(get16(q->data) == 1500);
        mc_atp_respond(atp, q, NULL, ws_data, sizeof ws_data, t);
    } else if (q->user[0] == 5) ws_tickles++;
    else if (q->user[0] == 8) {
        ws_attentions++;
        const uint8_t zero[4] = {0};
        mc_atp_respond(atp, q, zero, NULL, 0, t);
    } else if (q->user[0] == 1) ws_closes++;
}

static void test_asp(void) {
    T();
    cable(21, 22);
    now = 1000;
    mc_atalk_start(st[0].at, now);
    mc_atalk_start(st[1].at, now);
    run(2500);
    mc_asp *asp = mc_asp_create(st[0].at, &sops, NULL);
    mc_atp *ws = mc_atp_open(st[1].at, 0, wss, NULL);
    CHECK(asp && ws);
    mc_atalk_addr sls = mc_atalk_address(st[0].at);
    sls.socket = mc_asp_listening_socket(asp);
#define STEP(ms)                                  \
    do {                                          \
        for (uint64_t t_ = 0; t_ < (ms); t_ += 10) { \
            now += 10;                            \
            mc_atalk_tick(st[0].at, now);         \
            mc_atalk_tick(st[1].at, now);         \
            mc_asp_tick(asp, now);                \
            mc_atp_tick(ws, now);                 \
            for (int i_ = 0; i_ < 4; i_++) pump(); \
        }                                         \
    } while (0)
    /* GetStatus */
    const uint8_t gs[4] = {3, 0, 0, 0};
    done_status = 99;
    CHECK(mc_atp_send_request(ws, &sls, 0, gs, NULL, 0, 8, 500, 3, done, NULL, now));
    STEP(50);
    CHECK(done_status == 0 && done_len == 6 && !memcmp(done_data, "STATUS", 6));
    /* OpenSess with a bad version */
    uint8_t os[4] = {4, mc_atp_socket(ws), 0x02, 0x00};
    done_status = 99;
    mc_atp_send_request(ws, &sls, 1, os, NULL, 0, 1, 500, 3, done, NULL, now);
    STEP(50);
    CHECK(done_status == 0 && (int16_t)get16(done_user + 2) == MC_ASP_BAD_VERSION);
    /* OpenSess */
    os[2] = 1;
    done_status = 99;
    mc_atp_send_request(ws, &sls, 1, os, NULL, 0, 1, 500, 3, done, NULL, now);
    STEP(50);
    CHECK(done_status == 0 && get16(done_user + 2) == 0);
    const uint8_t sid = done_user[1];
    mc_atalk_addr sss = sls;
    sss.socket = done_user[0];
    CHECK_EQ(sessions_open, 1);
    CHECK_EQ(mc_asp_session_count(asp), 1);
    /* Command: the reply and the result */
    uint8_t cu[4] = {2, sid, 0, 1};
    done_status = 99;
    mc_atp_send_request(ws, &sss, 1, cu, (const uint8_t *)"\x12\x34", 2, 8, 500, 3, done, NULL, now);
    STEP(50);
    CHECK(done_status == 0 && done_len == 2 && done_data[0] == 0x12);
    CHECK((int32_t)((uint32_t)done_user[0] << 24 | (uint32_t)done_user[1] << 16 | (uint32_t)done_user[2] << 8 | done_user[3]) == -5018);
    CHECK_EQ(commands, 1);
    /* Write: WriteContinue fetches 1500 bytes from the workstation */
    for (size_t k = 0; k < sizeof ws_data; k++) ws_data[k] = (uint8_t)(k ^ 0x5A);
    uint8_t wu[4] = {6, sid, 0, 2};
    done_status = 99;
    mc_atp_send_request(ws, &sss, 1, wu, (const uint8_t *)"\x21", 1, 8, 500, 10, done, NULL, now);
    STEP(200);
    CHECK_EQ(done_status, 0);
    CHECK(done_len == 1 && done_data[0] == 0x55);
    CHECK(written_len == 1500 && !memcmp(written, ws_data, 1500));
    /* a write refused at once */
    wu[3] = 3;
    done_status = 99;
    mc_atp_send_request(ws, &sss, 1, wu, (const uint8_t *)"\xEE", 1, 8, 500, 3, done, NULL, now);
    STEP(50);
    CHECK(done_status == 0 && done_user[0] == 0xFF && done_user[3] == 0x78);   /* -5000 */
    /* Attention reaches the workstation */
    CHECK(mc_asp_attention(asp, &commands, 0x8000, now));
    STEP(50);
    CHECK_EQ(ws_attentions, 1);
    /* tickles every 30 s; with the workstation's tickles the session stays */
    const uint8_t tk[4] = {5, sid, 0, 0};
    for (int i = 0; i < 5; i++) {
        mc_atp_send_request(ws, &sls, 0, tk, NULL, 0, 1, 1000, 0, NULL, NULL, now);
        STEP(30000);
    }
    CHECK(ws_tickles >= 4);
    CHECK_EQ(mc_asp_session_count(asp), 1);
    /* silence for over 2 minutes: the server closes the session */
    STEP(125000);
    CHECK_EQ(mc_asp_session_count(asp), 0);
    CHECK_EQ(sessions_closed, 1);
    /* a new session closed by the workstation */
    done_status = 99;
    mc_atp_send_request(ws, &sls, 1, os, NULL, 0, 1, 500, 3, done, NULL, now);
    STEP(50);
    const uint8_t sid2 = done_user[1];
    CHECK_EQ(mc_asp_session_count(asp), 1);
    const uint8_t cs[4] = {1, sid2, 0, 0};
    done_status = 99;
    mc_atp_send_request(ws, &sss, 0, cs, NULL, 0, 1, 500, 3, done, NULL, now);
    STEP(50);
    CHECK_EQ(done_status, 0);
    CHECK_EQ(mc_asp_session_count(asp), 0);
    CHECK_EQ(sessions_closed, 2);
    /* the server closes a session: CloseSess to the workstation */
    mc_atp_send_request(ws, &sls, 1, os, NULL, 0, 1, 500, 3, done, NULL, now);
    STEP(50);
    mc_asp_close(asp, &commands, now);
    STEP(50);
    CHECK_EQ(ws_closes, 1);
    CHECK_EQ(mc_asp_session_count(asp), 0);
    mc_asp_destroy(asp, now);
    mc_atp_close(ws);
#undef STEP
}

int main(void) {
    test_des();
    test_names();
    test_nbp_match();
    test_aarp();
    test_ddp();
    test_nbp();
    test_atp();
    test_asp();
    printf("test_appletalk: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
