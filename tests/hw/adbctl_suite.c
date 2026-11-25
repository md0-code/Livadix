/* The protocol-level checks Egret and Cuda share (test_egret.c,
 * test_cuda.c). Expected packets are the firmware's (341S0851, 341S0788),
 * as the differential harness confirms them (test_adbctl_diff.c): reply
 * [1, 0, command, data], ADB [0, flags, command, data], errors [2, error,
 * type, command]. */
#include "adbctl_suite.h"

#include <stdlib.h>

static ac_host h;

const ac_packet *suite_cmd(ac_host *host, const uint8_t *p, unsigned n, unsigned stream) {
    host->stream = stream;
    return ac_command(host, p, n, AC_MS(100));
}

int suite_is(const ac_packet *r, const uint8_t *want, unsigned n) {
    if (!r) {
        fprintf(stderr, "  no reply\n");
        return 0;
    }
    if (r->n != n || memcmp(r->b, want, n)) {
        fprintf(stderr, "  got");
        for (unsigned i = 0; i < r->n; i++) fprintf(stderr, " %02X", r->b[i]);
        fprintf(stderr, ", want");
        for (unsigned i = 0; i < n; i++) fprintf(stderr, " %02X", want[i]);
        fprintf(stderr, "\n");
        return 0;
    }
    return 1;
}

static void clock_and_pram(mc_adbctl_kind kind) {
    CHECK(EXPECT(&h, 0, P(1, 0x09, 0x9A, 0xBC, 0xDE, 0xF0), P(1, 0, 0x09)));
    CHECK(EXPECT(&h, 0, P(1, 0x03), P(1, 0, 0x03, 0x9A, 0xBC, 0xDE, 0xF0)));
    CHECK_EQ(mc_adbctl_seconds(&h.ctl), 0x9ABCDEF0u);
    ac_run(&h, AC_MS(2100));
    CHECK_EQ(mc_adbctl_seconds(&h.ctl), 0x9ABCDEF2u);      /* the clock runs on guest time */
    h.ctl.dirty = 0;
    CHECK(EXPECT(&h, 0, P(1, 0x0C, 0x00, 0x20, 0xA1, 0xB2, 0xC3), P(1, 0, 0x0C)));
    CHECK(h.ctl.dirty);
    CHECK_EQ(mc_adbctl_pram(&h.ctl)[0x21], 0xB2);
    CHECK(EXPECT(&h, 3, P(1, 0x07, 0x00, 0x20), P(1, 0, 0x07, 0xA1, 0xB2, 0xC3)));
    CHECK(EXPECT(&h, 0, P(1, 0x07, 0x02, 0x00), P(2, 4, 1, 0x07)));      /* bad address */
    CHECK(EXPECT(&h, 0, P(1, 0x0C, 0x05, 0x00, 0x11), P(2, 4, 1, 0x0C)));
    /* Past the last byte: Cuda reads on ($200: nothing), Egret wraps into its 8 more bytes. */
    CHECK(EXPECT(&h, 0, P(1, 0x0C, 0x00, 0xFF, 0x5A), P(1, 0, 0x0C)));
    if (kind == MC_ADBCTL_CUDA) CHECK(EXPECT(&h, 2, P(1, 0x07, 0x00, 0xFF), P(1, 0, 0x07, 0x5A, 0x00)));
    else {
        CHECK(EXPECT(&h, 0, P(1, 0x0C, 0x01, 0x00, 0x66), P(1, 0, 0x0C)));
        CHECK(EXPECT(&h, 2, P(1, 0x07, 0x00, 0xFF), P(1, 0, 0x07, 0x5A, 0x66)));
        CHECK_EQ(h.ctl.mem[0xD7], 0x66);
    }
    /* The address space: the clock at $AB, the autopoll period at $A4. */
    CHECK(EXPECT(&h, 2, P(1, 0x02, 0x00, 0xA4), P(1, 0, 0x02, 0x0B, kind == MC_ADBCTL_EGRET ? 0x0F : 0x00)));
    CHECK(EXPECT(&h, 0, P(1, 0x08, 0x00, 0xA4, 0x14), P(1, 0, 0x08)));
    CHECK(EXPECT(&h, 0, P(1, 0x16), P(1, 0, 0x16, 0x14)));
    CHECK(EXPECT(&h, 0, P(1, 0x14, 0x0B), P(1, 0, 0x14)));
    CHECK(EXPECT(&h, 0, P(1, 0x08, 0x01, 0x40, 0x77), P(1, 0, 0x08)));
    CHECK(EXPECT(&h, 1, P(1, 0x07, 0x00, 0x40), P(1, 0, 0x07, 0x77)));
    {
        const ac_packet *r = suite_cmd(&h, P(1, 0x02, 0x00, 0xAB), 4, 4);
        CHECK(r && r->n == 7 && r->b[3] == 0x9A && r->b[4] == 0xBC);
    }
}

static void errors(void) {
    CHECK(EXPECT(&h, 0, P(9, 0x09), P(2, 1, 9, 9)));                     /* bad type */
    CHECK(EXPECT(&h, 0, P(1, 0x30), P(2, 2, 1, 0x30)));                  /* bad command */
    CHECK(EXPECT(&h, 0, P(1, 0x03, 0x00), P(2, 3, 1, 0x03)));            /* too long */
    CHECK(EXPECT(&h, 0, P(1, 0x09, 0x01), P(2, 3, 1, 0x09)));            /* too short */
    CHECK(EXPECT(&h, 0, P(0, 0x05), P(2, 2, 0, 0x05)));                  /* ADB 2-7 */
    CHECK(EXPECT(&h, 0, P(0, 0x2C, 0x01), P(2, 3, 0, 0x2C)));            /* Talk with data */
    CHECK(EXPECT(&h, 0, P(0, 0x2A, 0x01), P(2, 3, 0, 0x2A)));            /* Listen of 1 byte */
}

static void adb(void) {
    CHECK(EXPECT(&h, 0, P(0, 0x2F), P(0, 0, 0x2F, 0x62, 0x02)));
    CHECK(EXPECT(&h, 0, P(0, 0x3F), P(0, 0, 0x3F, 0x63, 0x01)));
    CHECK(EXPECT(&h, 0, P(0, 0x5F), P(0, 2, 0x5F)));                     /* timeout */
    CHECK(EXPECT(&h, 0, P(0, 0x2C), P(0, 2, 0x2C)));                     /* nothing to say */
    CHECK(EXPECT(&h, 0, P(0, 0x2A, 0xFF, 0xFB), P(0, 0, 0x2A)));         /* Listen: no flags */
    CHECK_EQ(h.kbd.leds, 3);
    CHECK(EXPECT(&h, 0, P(0, 0x3B, 0x68, 0xFE), P(0, 0, 0x3B)));         /* the mouse to 8 */
    CHECK(EXPECT(&h, 0, P(0, 0x8F), P(0, 0, 0x8F, 0x68, 0x01)));
    CHECK(EXPECT(&h, 0, P(0, 0x81), P(0, 0, 0x81)));                     /* Flush */
    CHECK(EXPECT(&h, 0, P(0, 0x00), P(0, 0, 0x00)));                     /* SendReset */
    CHECK(EXPECT(&h, 0, P(0, 0x3F), P(0, 0, 0x3F, 0x63, 0x01)));
    /* The keyboard has data: its Service Request on a Talk to the mouse. */
    mc_adb_keyboard_key(&h.kbd, 0x00, 1);
    CHECK(EXPECT(&h, 0, P(0, 0x3C), P(0, 3, 0x3C)));
    CHECK(EXPECT(&h, 0, P(0, 0x2C), P(0, 0, 0x2C, 0x00, 0xFF)));
    mc_adb_keyboard_key(&h.kbd, 0x00, 0);
    mc_adb_mouse_move(&h.mouse, 1, 1, 0);
    CHECK(EXPECT(&h, 0, P(0, 0x2C), P(0, 1, 0x2C, 0x80, 0xFF)));
    CHECK(EXPECT(&h, 0, P(0, 0x3C), P(0, 0, 0x3C, 0x81, 0x81)));
}

static const ac_packet *next_unsolicited(uint64_t timeout) { return ac_next(&h, timeout); }

static void autopoll(void) {
    CHECK(EXPECT(&h, 0, P(1, 0x1A), P(1, 0, 0x1A, 0x00, 0x0C)));          /* keyboard and mouse */
    CHECK(EXPECT(&h, 0, P(1, 0x01, 0x01), P(1, 0, 0x01)));
    ac_run(&h, AC_MS(40));
    CHECK_EQ(h.nlog, h.read);                                            /* idle devices: nothing */
    mc_adb_keyboard_key(&h.kbd, 0x0C, 1);
    mc_adbctl_poke(&h.ctl);
    const uint64_t t0 = h.f.now;
    const ac_packet *r = next_unsolicited(AC_MS(30));
    CHECK(suite_is(r, P(0, 0x40, 0x2C, 0x0C, 0xFF), 5));
    CHECK(r && r->attention - t0 < AC_MS(15));
    mc_adb_mouse_move(&h.mouse, -2, 3, 1);
    r = next_unsolicited(AC_MS(40));
    CHECK(suite_is(r, P(0, 0x40, 0x3C, 0x03, 0xFE), 5));
    mc_adb_keyboard_key(&h.kbd, 0x0C, 0);
    r = next_unsolicited(AC_MS(40));                                     /* found by the SRQ search */
    CHECK(suite_is(r, P(0, 0x40, 0x2C, 0x8C, 0xFF), 5));
    /* A host packet while autopoll runs. */
    CHECK(EXPECT(&h, 0, P(1, 0x16), P(1, 0, 0x16, 0x0B)));
    CHECK(EXPECT(&h, 0, P(1, 0x19, 0x00, 0x04), P(1, 0, 0x19)));
    CHECK(EXPECT(&h, 0, P(1, 0x1A), P(1, 0, 0x1A, 0x00, 0x04)));
    CHECK(EXPECT(&h, 0, P(1, 0x19, 0x00, 0x0C), P(1, 0, 0x19)));
    ac_run(&h, AC_MS(30));
    h.read = h.nlog;
    CHECK(EXPECT(&h, 0, P(1, 0x01, 0x00), P(1, 0, 0x01)));
    mc_adb_mouse_move(&h.mouse, 1, 0, 1);
    ac_run(&h, AC_MS(50));
    CHECK_EQ(h.nlog, h.read);                                            /* autopoll off */
    CHECK(EXPECT(&h, 0, P(0, 0x3C), P(0, 0, 0x3C, 0x00, 0x81)));
}

static void one_second(mc_adbctl_kind kind) {
    (void)kind;
    CHECK(EXPECT(&h, 0, P(1, 0x1B, 0x03), P(1, 0, 0x1B)));
    const unsigned n0 = h.nlog;
    ac_run(&h, h.ctl.next_tick - h.f.now + AC_MS(10));          /* just after a second's mark */
    ac_run(&h, AC_MS(2950));
    /* A second already passed sends one at once; the first after the mode
     * is set carries the whole time, the others one byte. */
    CHECK(h.nlog - n0 == 3 || h.nlog - n0 == 4);
    CHECK(h.log[n0 % AC_LOG].n == 7 && h.log[n0 % AC_LOG].b[0] == 1 && h.log[n0 % AC_LOG].b[2] == 3);
    CHECK(suite_is(&h.log[(h.nlog - 1) % AC_LOG], P(3), 1));
    CHECK(suite_is(&h.log[(h.nlog - 2) % AC_LOG], P(3), 1));
    h.read = h.nlog;
    CHECK(EXPECT(&h, 0, P(1, 0x1B, 0x02), P(1, 0, 0x1B)));
    const unsigned n1 = h.nlog;
    ac_run(&h, h.ctl.next_tick - h.f.now + AC_MS(10));
    ac_run(&h, AC_MS(1950));
    CHECK(h.nlog - n1 == 2 || h.nlog - n1 == 3);
    CHECK(suite_is(&h.log[(h.nlog - 1) % AC_LOG], P(1, 0, 3), 3));
    h.read = h.nlog;
    CHECK(EXPECT(&h, 0, P(1, 0x1B, 0x01), P(1, 0, 0x1B)));
    const unsigned n2 = h.nlog;
    ac_run(&h, h.ctl.next_tick - h.f.now + AC_MS(10));
    ac_run(&h, AC_MS(1950));
    CHECK(h.nlog - n2 == 2 || h.nlog - n2 == 3);
    CHECK(h.log[(h.nlog - 1) % AC_LOG].n == 7);
    h.read = h.nlog;
    CHECK(EXPECT(&h, 0, P(1, 0x00), P(1, 0, 0x00)));                    /* everything off */
    ac_run(&h, AC_MS(2050));
    CHECK_EQ(h.nlog, h.read);
    /* A host packet colliding with a tick: the tick first, the packet again. */
    CHECK(EXPECT(&h, 0, P(1, 0x1B, 0x03), P(1, 0, 0x1B)));
    const uint64_t next = h.ctl.next_tick;
    ac_run(&h, next - h.f.now - AC_US(150));
    const ac_packet *r = suite_cmd(&h, P(1, 0x16), 2, 0);
    CHECK(suite_is(r, P(1, 0, 0x16, 0x0B), 4));
    h.read = h.nlog;
    CHECK(EXPECT(&h, 0, P(1, 0x1B, 0x00), P(1, 0, 0x1B)));
}

static void persistence(mc_adbctl_kind kind) {
    const char *path = hw_work_path(kind == MC_ADBCTL_CUDA ? "cuda" : "egret", "pram.nvram");
    CHECK(EXPECT(&h, 0, P(1, 0x0C, 0x00, 0x08, 0x13, 0x57), P(1, 0, 0x0C)));
    CHECK(EXPECT(&h, 0, P(1, 0x09, 0xB0, 0x00, 0x00, 0x00), P(1, 0, 0x09)));
    /* Saved relative to a host time of $A0000000, reloaded at $A0000010. */
    CHECK(mc_adbctl_nvram_save(&h.ctl, path, 0xA0000000ll));
    CHECK(!h.ctl.dirty);
    static ac_host g;
    ac_init(&g, kind, NULL);
    CHECK(mc_adbctl_nvram_load(&g.ctl, path));
    mc_adbctl_set_clock(&g.ctl, 0xA0000010ll);
    CHECK_EQ(mc_adbctl_pram(&g.ctl)[0x08], 0x13);
    CHECK_EQ(mc_adbctl_pram(&g.ctl)[0x09], 0x57);
    const uint32_t s = mc_adbctl_seconds(&g.ctl);
    const uint32_t want = 0xB0000010u - (uint32_t)(h.f.now / MC_C15M_HZ);
    CHECK(s >= want - 1 && s <= want + 1);
    CHECK(EXPECT(&g, 2, P(1, 0x07, 0x00, 0x08), P(1, 0, 0x07, 0x13, 0x57)));
    CHECK(!mc_adbctl_nvram_load(&g.ctl, hw_work_path("cuda", "absent.nvram")));
}

static void power(mc_adbctl_kind kind) {
    /* Restart: no reply, the 68k's reset pulsed, the link usable again. */
    memset(h.lines, 0, sizeof h.lines);
    CHECK(ac_send(&h, P(1, 0x11), 2));
    ac_run(&h, AC_MS(100));
    CHECK(h.ctl.line_state[MC_ADBCTL_LINE_RESET]);
    ac_run(&h, AC_MS(400));
    CHECK(!h.ctl.line_state[MC_ADBCTL_LINE_RESET]);
    CHECK_EQ(h.lines[MC_ADBCTL_LINE_RESET][1], 1);
    CHECK_EQ(h.nlog, h.read);
    CHECK(EXPECT(&h, 0, P(1, 0x16), P(1, 0, 0x16, 0x0B)));
    /* Control-Command-power on the keyboard: a reset, unless refused once. */
    CHECK(EXPECT(&h, 0, P(1, 0x01, 0x01), P(1, 0, 0x01)));
    CHECK(EXPECT(&h, 0, P(1, 0x0D, 0x01), P(1, 0, 0x0D)));
    mc_adb_keyboard_key(&h.kbd, 0x36, 1);
    mc_adb_keyboard_key(&h.kbd, 0x37, 1);
    mc_adb_keyboard_key(&h.kbd, 0x7F, 1);
    memset(h.lines, 0, sizeof h.lines);
    ac_run(&h, AC_MS(100));
    CHECK_EQ(h.lines[MC_ADBCTL_LINE_RESET][1], 0);                     /* refused */
    CHECK(!(h.ctl.mem[0xA2] & 0x40));
    mc_adb_keyboard_key(&h.kbd, 0x7F, 0);
    mc_adb_keyboard_key(&h.kbd, 0x37, 0);
    mc_adb_keyboard_key(&h.kbd, 0x37, 1);
    mc_adb_keyboard_key(&h.kbd, 0x7F, 1);
    ac_run(&h, AC_MS(100));
    CHECK_EQ(h.lines[MC_ADBCTL_LINE_RESET][1], 1);                     /* now a reset */
    ac_run(&h, AC_MS(400));
    mc_adb_keyboard_key(&h.kbd, 0x7F, 0);
    mc_adb_keyboard_key(&h.kbd, 0x37, 0);
    mc_adb_keyboard_key(&h.kbd, 0x36, 0);
    h.read = h.nlog;
    /* Command-power: the NMI (Egret: once pseudo-command $1C enabled it). */
    CHECK(EXPECT(&h, 0, P(1, 0x01, 0x01), P(1, 0, 0x01)));
    if (kind == MC_ADBCTL_EGRET) CHECK(EXPECT(&h, 0, P(1, 0x1C, 0x01), P(1, 0, 0x1C)));
    memset(h.lines, 0, sizeof h.lines);
    mc_adb_keyboard_key(&h.kbd, 0x37, 1);
    mc_adb_keyboard_key(&h.kbd, 0x7F, 1);
    ac_run(&h, AC_MS(100));
    CHECK_EQ(h.lines[MC_ADBCTL_LINE_NMI][1], 1);
    CHECK(!h.ctl.line_state[MC_ADBCTL_LINE_NMI]);
    mc_adb_keyboard_key(&h.kbd, 0x7F, 0);
    mc_adb_keyboard_key(&h.kbd, 0x37, 0);
    ac_run(&h, AC_MS(100));
    CHECK(EXPECT(&h, 0, P(1, 0x01, 0x00), P(1, 0, 0x01)));
    h.read = h.nlog;
    /* Power down: no reply, the power line drops; the power key brings it back. */
    CHECK(ac_send(&h, P(1, 0x0A), 2));
    ac_run(&h, AC_MS(50));
    CHECK(!h.ctl.line_state[MC_ADBCTL_LINE_POWER]);
    CHECK_EQ(h.nlog, h.read);
    memset(h.lines, 0, sizeof h.lines);
    mc_adbctl_power_key(&h.ctl, 1);
    mc_adbctl_power_key(&h.ctl, 0);
    CHECK(h.ctl.line_state[MC_ADBCTL_LINE_POWER]);
    CHECK_EQ(h.lines[MC_ADBCTL_LINE_RESET][1], 1);
    ac_run(&h, AC_MS(400));
    CHECK(EXPECT(&h, 0, P(1, 0x16), P(1, 0, 0x16, 0x0B)));
}

ac_host *adbctl_suite_host(void) { return &h; }

void adbctl_suite(mc_adbctl_kind kind) {
    CHECK(ac_init(&h, kind, NULL));
    CHECK(h.ctl.line_state[MC_ADBCTL_LINE_POWER]);
    CHECK(!h.ctl.line_state[MC_ADBCTL_LINE_RESET]);
    clock_and_pram(kind);
    errors();
    adb();
    autopoll();
    one_second(kind);
    persistence(kind);
    power(kind);
}
