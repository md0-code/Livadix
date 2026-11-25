/* The host side of Egret and Cuda for the tests (adbctl_host.h): the
 * driver's sequences are those of the Quadra 605 ROM's Cuda driver (at
 * ROM offset $A9A1C: TIP and BYTEACK on VIA1 PB5/PB4, TREQ on PB3, ACR bit
 * 4 switching the shift register between modes 3 and 7) and of the Egret
 * driver the IIsi and Quadra 605 ROMs share (at ROM offset $14838: SysSes
 * PB5, ViaFull PB4, XcvrSes PB3, ViaFull dropped and a delay of TimeVIADB/8 + 25 VIA accesses (about 160 us, ROM offset $8542E) before
 * each next byte). */
#include "adbctl_host.h"

#include <string.h>

/* Every VIA access comes after the controller's own up to now (the
 * firmware level runs in slices: adbctl.h). */
static uint8_t rd(ac_host *h, unsigned reg) {
    mc_adbctl_sync(&h->ctl);
    return (uint8_t)mc_via_ops.read(&h->via, reg << 9, 1);
}
static void wr(ac_host *h, unsigned reg, uint8_t v) {
    mc_adbctl_sync(&h->ctl);
    mc_via_ops.write(&h->via, reg << 9, 1, v);
}
static void pb(ac_host *h, uint8_t mask, uint8_t value) {
    wr(h, MC_VIA_ORB, (uint8_t)((h->via.orb & ~mask) | (value & mask)));
}
static int xcvr(ac_host *h) { return !(rd(h, MC_VIA_ORB) & 0x08u); }     /* TREQ / XcvrSes asserted */
static int sr_flag(ac_host *h) { return (rd(h, MC_VIA_IFR) & MC_VIA_IRQ_SR) != 0; }
static void tick(ac_host *h) { hw_fake_step(&h->f, h->step); }
static void delay(ac_host *h, uint64_t periods) { hw_fake_step(&h->f, periods); }

static int wait_sr(ac_host *h, uint64_t timeout) {
    const uint64_t start = h->f.now, end = h->f.now + timeout;
    while (!sr_flag(h)) {
        if (h->f.now >= end) return 0;
        tick(h);
    }
    h->send_wait += h->f.now - start;
    return 1;
}

static void on_line(hw_fake *f, void *dev, int line, int state) {
    ac_host *h = f->user;
    if (dev == &h->via && line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&h->ctl, (uint8_t)state);
    else if (dev == &h->ctl && line >= 0 && line < 3) h->lines[line][state != 0]++;
}

/* A packet from the controller: its attention byte's interrupt is pending. */
static void receive(ac_host *h) {
    ac_packet *p = &h->log[h->nlog % AC_LOG];
    memset(p, 0, sizeof *p);
    p->attention = h->f.now;
    (void)rd(h, MC_VIA_SR);                                 /* the idle byte */
    if (h->kind == MC_ADBCTL_CUDA) pb(h, 0x20, 0);          /* TIP */
    else {
        delay(h, AC_US(160));                               /* the driver's delay, then SysSes */
        pb(h, 0x20, 0x20);
    }
    for (;;) {
        if (!wait_sr(h, AC_MS(20))) break;
        const uint8_t b = rd(h, MC_VIA_SR);
        if (p->n == 0) p->start = h->f.now;
        p->end = h->f.now;
        if (p->n < sizeof p->b) p->b[p->n] = b;
        p->n++;
        const int stop = (h->stream && p->n >= 3u + h->stream) || p->n >= sizeof p->b;
        const int last = !xcvr(h) || stop;
        if (h->kind == MC_ADBCTL_CUDA) {
            if (last) {
                pb(h, 0x30, 0x30);
                break;
            }
            pb(h, 0x10, (uint8_t)(h->via.orb ^ 0x10u));     /* BYTEACK toggles */
        } else {
            if (stop) {                                     /* SysSes dropped before the acknowledge: no more */
                pb(h, 0x20, 0);
                delay(h, AC_US(160));
                break;
            }
            pb(h, 0x10, 0x10);                              /* ViaFull: read */
            delay(h, AC_US(160));
            pb(h, 0x10, 0);
            if (last) {
                pb(h, 0x20, 0);
                delay(h, AC_US(160));                       /* the driver's turnaround */
                break;
            }
        }
    }
    h->stream = 0;
    if (h->kind == MC_ADBCTL_CUDA && wait_sr(h, AC_MS(1))) (void)rd(h, MC_VIA_SR);   /* the closing clocks */
    h->nlog++;
}

static void service(ac_host *h) {
    if (!sr_flag(h)) return;
    if (xcvr(h)) receive(h);
    else (void)rd(h, MC_VIA_SR);                            /* clocks of a resynchronisation */
}

void ac_resync(ac_host *h) {
    pb(h, 0x30, 0x20);                                      /* TIP negated, BYTEACK low */
    if (wait_sr(h, AC_MS(20))) (void)rd(h, MC_VIA_SR);
    pb(h, 0x10, 0x10);                                      /* BYTEACK high */
    if (wait_sr(h, AC_MS(20))) (void)rd(h, MC_VIA_SR);
}

void ac_run(ac_host *h, uint64_t periods) {
    const uint64_t end = h->f.now + periods;
    while (h->f.now < end) {
        tick(h);
        service(h);
    }
}

int ac_send(ac_host *h, const uint8_t *p, unsigned n) {
    h->send_bytes = n;
    for (int tries = 0; xcvr(h) || sr_flag(h); tries++) {      /* the controller first */
        if (tries > 200000) return 0;
        tick(h);
        service(h);
    }
    h->send_wait = 0;
    if (h->kind == MC_ADBCTL_CUDA) {
        wr(h, MC_VIA_ACR, 0x1C);
        wr(h, MC_VIA_SR, p[0]);
        pb(h, 0x30, 0x10);                                  /* BYTEACK high, TIP asserted */
        if (!wait_sr(h, AC_MS(20))) return 0;
        h->send_first = h->send_wait;
        if (xcvr(h)) {                                      /* collision: Cuda goes first */
            delay(h, AC_US(20));
            wr(h, MC_VIA_ACR, 0x0C);
            (void)rd(h, MC_VIA_SR);
            pb(h, 0x30, 0x30);
            h->collisions++;
            return 0;
        }
        for (unsigned i = 1; i < n; i++) {
            wr(h, MC_VIA_SR, p[i]);
            pb(h, 0x10, (uint8_t)(h->via.orb ^ 0x10u));
            if (!wait_sr(h, AC_MS(20))) return 0;
        }
        wr(h, MC_VIA_ACR, 0x0C);
        (void)rd(h, MC_VIA_SR);
        pb(h, 0x30, 0x30);
        h->sent = h->f.now;
    h->send_total = h->send_wait;
        return 1;
    }
    pb(h, 0x20, 0x20);                                      /* SysSes */
    wr(h, MC_VIA_ACR, 0x1C);
    wr(h, MC_VIA_SR, p[0]);
    pb(h, 0x10, 0x10);                                      /* ViaFull */
    if (!wait_sr(h, AC_MS(20))) return 0;
    h->send_first = h->send_wait;
    if (xcvr(h)) {                                          /* collision: Egret goes first */
        wr(h, MC_VIA_ACR, 0x0C);
        (void)rd(h, MC_VIA_SR);
        pb(h, 0x30, 0);
        h->collisions++;
        return 0;
    }
    for (unsigned i = 1; i < n; i++) {
        pb(h, 0x10, 0);
        delay(h, AC_US(160));
        wr(h, MC_VIA_SR, p[i]);
        pb(h, 0x10, 0x10);
        if (!wait_sr(h, AC_MS(20))) return 0;
    }
    wr(h, MC_VIA_ACR, 0x0C);
    (void)rd(h, MC_VIA_SR);
    pb(h, 0x30, 0);
    h->sent = h->f.now;
    h->send_total = h->send_wait;
    delay(h, AC_US(160));
    return 1;
}

const ac_packet *ac_next(ac_host *h, uint64_t timeout) {
    const uint64_t end = h->f.now + timeout;
    while (h->read == h->nlog) {
        if (h->f.now >= end) return NULL;
        tick(h);
        service(h);
    }
    return &h->log[h->read++ % AC_LOG];
}

const ac_packet *ac_command(ac_host *h, const uint8_t *p, unsigned n, uint64_t timeout) {
    for (int tries = 0; tries < 8 && !ac_send(h, p, n); tries++) ac_run(h, AC_MS(2));
    const uint64_t end = h->f.now + timeout;
    for (;;) {
        const uint64_t now = h->f.now;
        const ac_packet *r = ac_next(h, now < end ? end - now : 0);
        if (!r) return NULL;
        /* The reply names the command: [type, flags, command] or [2, error, type, command]. */
        if (r->n >= 3 && r->b[0] <= 1 && r->b[2] == p[1] && !(r->b[0] == 0 && (r->b[1] & 0x40u))) return r;
        if (r->n >= 4 && r->b[0] == 2 && r->b[3] == p[1]) return r;
    }
}

int ac_init(ac_host *h, mc_adbctl_kind kind, const uint8_t *image) {
    memset(h, 0, sizeof *h);
    hw_fake_init(&h->f);
    h->f.user = h;
    h->f.on_line = on_line;
    h->kind = kind;
    h->step = AC_US(5);
    mc_via_init(&h->via, &h->f.host, "VIA1", 9);
    mc_adb_bus_init(&h->adb);
    mc_adb_keyboard_init(&h->kbd);
    mc_adb_mouse_init(&h->mouse);
    mc_adb_attach(&h->adb, &h->kbd.dev);
    mc_adb_attach(&h->adb, &h->mouse.dev);
    mc_adbctl_init(&h->ctl, &h->f.host, kind == MC_ADBCTL_CUDA ? "Cuda" : "Egret", kind, &h->via, &h->adb);
    mc_via_ops.reset(&h->via);
    /* The ROM's setup: PB4/PB5 outputs at rest, the shift register in mode 3. */
    wr(h, MC_VIA_ORB, kind == MC_ADBCTL_CUDA ? 0x30 : 0x00);
    wr(h, MC_VIA_DDRB, 0x30);
    wr(h, MC_VIA_ACR, 0x0C);
    if (!image) {
        mc_adbctl_ops.reset(&h->ctl);
        ac_run(h, AC_MS(1));
        return 1;
    }
    if (!mc_adbctl_use_firmware(&h->ctl, image, MC_ADBCTL_FW_SIZE)) return 0;
    const uint64_t end = h->f.now + AC_MS(5000);
    while (h->f.now < end) {
        ac_run(h, AC_MS(10));
        if (h->ctl.line_state[MC_ADBCTL_LINE_POWER] && !h->ctl.line_state[MC_ADBCTL_LINE_RESET]) {
            ac_run(h, AC_MS(50));
            return 1;
        }
    }
    return 0;
}
