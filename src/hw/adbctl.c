/* Livadix — Egret and Cuda: the parts both share (see adbctl.h for sources).
 * Livadix's own code.
 *
 * The protocol level keeps the controller's state at the addresses the
 * firmware keeps it (observed in 341S0851 and 341S0788),
 * so the address-space pseudo-commands read and write the same things:
 *
 *   $A1  bit 7 restart, bit 4 clock set (Cuda), bit 1 power-key packet
 *        sent (Cuda), keys held: bit 0 Control, 2 Command, 5 power,
 *        6 Option (Cuda)
 *   $A2  bit 7 autopoll, bit 6 refuse the next keyboard reset, bit 5 full
 *        NMI (Cuda), bit 4 one-second packets, bit 3 a second passed, bit 2
 *        NMI enabled (Egret), bit 0 send the whole time next
 *   $A3  bit 6 wake-up time reached, bit 5 power-key packets on (Cuda),
 *        bit 2 the flag of pseudo-command $13
 *   $A4  autopoll period (ms)          $A5  ADB device delay (Egret)
 *   $A6  power-key timeout (Cuda) or the byte of pseudo-command $1E (Egret)
 *   $AB-$AE  the clock, $AF-$B2  the wake-up time (big-endian)
 *   $B3, $B4  the device autopoll asks first, the one before it
 *   $C1-$C3 (Cuda), $C9-$CB (Egret): autopoll device mask (addresses 15-8,
 *        7-0) and the one-second packet mode
 *   $100-$1FF  parameter RAM; Egret's 8 more bytes at $D7-$DE */
#include "../core/nvram.h"
#include "adbctl.h"
#include "../core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { A1 = 0xA1, A2 = 0xA2, A3 = 0xA3, RATE = 0xA4, DELAY = 0xA5, A6 = 0xA6, CLOCK = 0xAB, ALARM = 0xAF,
       POLL = 0xB3, PREV = 0xB4 };

static unsigned mask_hi(const mc_adbctl *c) { return c->kind == MC_ADBCTL_CUDA ? 0xC1u : 0xC9u; }
static unsigned tick_mode(const mc_adbctl *c) { return c->kind == MC_ADBCTL_CUDA ? 0xC3u : 0xCBu; }

uint64_t mc_adbctl_now(const mc_adbctl *c) { return mc_dev_now(&c->dev); }

/* Measured on the firmware (adbctl.h): Egret 341S0851 (also 341S0850,
 * 344S0100), Cuda 341S0788 (also 341S0417), Cuda 341S0789 and 341S0060. */
static const mc_adbctl_timing TIMING[3] = {
    {170, 100, 185, 60, 165, 167, 250, 0},
    {170, 125, 220, 60, 155, 150, 280, 70},
    {170, 125, 220, 60, 210, 205, 280, 70},
};
const mc_adbctl_timing *mc_adbctl_timing_of(const mc_adbctl *c) {
    return c->timing ? c->timing : &TIMING[c->kind == MC_ADBCTL_CUDA];
}

void mc_adbctl_set_part(mc_adbctl *c, const char *part) {
    c->timing = part && (strstr(part, "341S0789") || strstr(part, "341S0060")) ? &TIMING[2] : NULL;
}

/* The delay of a reply's idle clocks after the host's packet, less the
 * link's own wait for them. */
static uint64_t processing(const mc_adbctl *c) {
    const mc_adbctl_timing *t = mc_adbctl_timing_of(c);
    return MC_ADBCTL_US(t->process - t->attention);
}

/* --- lines ------------------------------------------------------------------- */

void mc_adbctl_set_line(mc_adbctl *c, int line, int state) {
    state = state != 0;
    if (c->line_state[line] == state) return;
    c->line_state[line] = (uint8_t)state;
    /* Cuda's /IRQ follows the system's power (341S0060 reads it as its
     * power sense with BIH; 341S0788's /IRQ handler is a power-fail restart). */
    if (line == MC_ADBCTL_LINE_POWER && c->firmware && c->kind == MC_ADBCTL_CUDA) mc_adbctl_mcu_set_irq(c->mcu, state);
    mc_dev_line(&c->dev, line, state);
}

void mc_adbctl_set_xcvr(mc_adbctl *c, int level) {
    level = level != 0;
    if (c->xcvr == level) return;
    c->xcvr = (uint8_t)level;
    if (c->via) mc_via_set_pb(c->via, level ? 0x08u : 0, 0x08u);
}

/* --- the clock ----------------------------------------------------------------- */

static uint32_t clock_now(const mc_adbctl *c) {
    return c->seconds + (uint32_t)((mc_adbctl_now(c) - c->base) / MC_C15M_HZ);
}

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void clock_to_mem(mc_adbctl *c) { put32(c->mem + CLOCK, clock_now(c)); }

static void clock_set(mc_adbctl *c, uint32_t s) {
    /* The second keeps its phase: the counter changes, its ticks do not. */
    c->seconds = s - (uint32_t)((mc_adbctl_now(c) - c->base) / MC_C15M_HZ);
    put32(c->mem + CLOCK, s);
    c->dirty = 1;
}

uint8_t *mc_adbctl_pram(mc_adbctl *c) { return c->firmware ? c->mcu->ram + 0x100 : c->mem + 0x100; }

uint32_t mc_adbctl_seconds(mc_adbctl *c) {
    if (c->firmware) {
        mc_adbctl_sync(c);
        return get32(c->mcu->ram + CLOCK);
    }
    return clock_now(c);
}

void mc_adbctl_set_seconds(mc_adbctl *c, uint32_t s) {
    if (c->firmware) put32(c->mcu->ram + CLOCK, s);
    clock_set(c, s);
}

/* --- the address space (pseudo-commands $02, $08, $07, $0C) ------------------ */

static uint8_t mem_read(mc_adbctl *c, uint16_t a) {
    a &= 0x1FFFu;
    if (a >= CLOCK && a < CLOCK + 4) clock_to_mem(c);
    return a < 0x200u ? c->mem[a] : 0;      /* no firmware image at this level */
}

static void mem_write(mc_adbctl *c, uint16_t a, uint8_t v) {
    a &= 0x1FFFu;
    if (a >= 0x200u) return;
    if (a >= CLOCK && a < CLOCK + 4) {
        clock_to_mem(c);
        c->mem[a] = v;
        clock_set(c, get32(c->mem + CLOCK));
        return;
    }
    if (a >= 0x100u && c->mem[a] != v) c->dirty = 1;
    c->mem[a] = v;
}

/* Parameter RAM addresses of pseudo-commands $07 and $0C: Cuda takes
 * $00nn ($100 + nn); Egret also $01nn below $0108 (its 8 bytes at $D7).
 * Returns the 6805 address or $FFFF. */
static uint16_t pram_address(const mc_adbctl *c, uint8_t hi, uint8_t lo) {
    if (hi == 0) return (uint16_t)(0x100u + lo);
    if (c->kind == MC_ADBCTL_EGRET && hi == 1 && lo < 8) return (uint16_t)(0xD7u + lo);
    return 0xFFFFu;
}

/* The next address of a parameter RAM stream (Egret wraps its counter into
 * the 8 extra bytes, then back; Cuda runs on past $1FF). */
static uint16_t pram_next(const mc_adbctl *c, uint16_t a) {
    if (c->kind == MC_ADBCTL_EGRET) {
        if (a == 0x1FFu) return 0xD7u;
        if (a >= 0xD7u && a < 0xDEu) return (uint16_t)(a + 1u);
        if (a == 0xDEu) return 0x100u;
    }
    return (uint16_t)(a + 1u);
}

/* --- replies --------------------------------------------------------------------- */

static void reply(mc_adbctl *c, const uint8_t *bytes, unsigned n, uint64_t delay) {
    memcpy(c->tx, bytes, n);
    c->tx_n = n;
    c->tx_i = 0;
    c->stream = 0xFFFFu;
    c->reply_pending = 1;
    c->reply_at = mc_adbctl_now(c) + delay;
}

static void reply3(mc_adbctl *c, uint8_t type, uint8_t flags, uint8_t cmd, uint64_t delay) {
    const uint8_t p[3] = {type, flags, cmd};
    reply(c, p, 3, delay);
}

static void error(mc_adbctl *c, uint8_t code, uint8_t type, uint8_t cmd) {
    const uint8_t p[4] = {2, code, type, cmd};
    reply(c, p, 4, processing(c));
}

uint8_t mc_adbctl_tx_byte(mc_adbctl *c, unsigned i, int *last) {
    if (i < c->tx_n) {
        *last = c->stream == 0xFFFFu && i + 1u == c->tx_n;
        return c->tx[i];
    }
    *last = 0;
    if (c->stream == 0xFFFFu) return 0;
    /* Streams: one byte per index past the header, in order. */
    const uint16_t a = c->stream;
    c->stream = c->stream_kind == 2 ? pram_next(c, a) : (uint16_t)((a + 1u) & 0x1FFFu);
    return mem_read(c, a);
}

/* --- ADB --------------------------------------------------------------------------- */

/* ADB time (Guide, Table 8-14): attention 800 us, sync 65 us, 8 bits, stop
 * 65 us; the stop-to-start time; a start bit, the data, a stop bit. */
static uint64_t us(unsigned n) { return (uint64_t)MC_C15M_HZ / 1000000u * n + (uint64_t)MC_C15M_HZ % 1000000u * n / 1000000u; }
static uint64_t adb_time(const mc_adbctl *c, unsigned bytes, int answered) {
    const mc_adbctl_timing *tm = mc_adbctl_timing_of(c);
    uint64_t t = us(800 + 65 + 800 + 65 + 200);
    t += answered ? us(100 + 65 + tm->adb_answer) + (uint64_t)bytes * us(800) : us(150u + tm->adb_timeout);
    return t;
}

/* The keyboard's key codes as the firmware watches them (Talk R0 of
 * address 2): Control $36, Command $37, the power key ($7F7F), Option $3A
 * (Cuda); any other key clears the combination. Returns 1 when the data is
 * to be kept from the host (Egret's NMI combination). */
static int key_hook(mc_adbctl *c, const uint8_t *data, unsigned n) {
    if (n < 2) return 0;
    const uint8_t keep = c->kind == MC_ADBCTL_CUDA ? 0x65u : 0x25u;
    for (unsigned i = 0; i < 2; i++) {
        const uint8_t k = data[i] & 0x7Fu, up = data[i] & 0x80u;
        uint8_t bit = 0;
        if (k == 0x36) bit = 0x01;
        else if (k == 0x37) bit = 0x04;
        else if (k == 0x3A && c->kind == MC_ADBCTL_CUDA) bit = 0x40;
        else if (k == 0x7F) {
            if ((data[0] & 0x7Fu) != 0x7F || (data[1] & 0x7Fu) != 0x7F) continue;
            bit = 0x20;
        }
        if (!bit) {
            c->mem[A1] &= (uint8_t)~keep;
            continue;
        }
        c->mem[A1] = (uint8_t)(up ? c->mem[A1] & ~bit : c->mem[A1] | bit);
        if (k == 0x7F) break;
    }
    const uint8_t held = c->mem[A1] & keep & 0x25u;
    if (held == 0x25u) {                                     /* Control-Command-power: reset */
        if (c->mem[A2] & 0x40u) {
            c->mem[A2] &= (uint8_t)~0x40;
            c->mem[A1] &= (uint8_t)~0x04;
        } else {
            c->mem[A1] |= 0x80u;
            c->restart_due = 1;
        }
    } else if (held == 0x24u) {                              /* Command-power: NMI */
        if (c->kind == MC_ADBCTL_EGRET) {
            if (c->mem[A2] & 0x04u) {
                c->mem[A1] &= 0xC0u;
                c->nmi_until = mc_adbctl_now(c) + us(1000);
                mc_adbctl_set_line(c, MC_ADBCTL_LINE_NMI, 1);
                return 1;
            }
        } else {
            c->mem[A1] &= (uint8_t)~0x20;
            c->nmi_until = mc_adbctl_now(c) + us(1000);
            mc_adbctl_set_line(c, MC_ADBCTL_LINE_NMI, 1);
        }
    }
    return 0;
}

/* A Talk: the answer in out[], the flags ($02 timeout, $01 Service Request). */
static unsigned adb_talk(mc_adbctl *c, uint8_t cmd, uint8_t *out, uint8_t *flags) {
    const unsigned n = mc_adb_command(c->adb, cmd, NULL, 0, out);
    *flags = 0;
    if (mc_adb_srq(c->adb, cmd >> 4)) *flags |= 0x01u;
    if (!n) *flags |= 0x02u;
    if (n && cmd == 0x2C && key_hook(c, out, n)) return 0;
    return n;
}

static int in_mask(const mc_adbctl *c, unsigned address) {
    const unsigned m = (unsigned)c->mem[mask_hi(c)] << 8 | c->mem[mask_hi(c) + 1u];
    return (m >> (address & 15u)) & 1u;
}

/* A host command to an address in the autopoll mask makes it the first
 * one polled (the one before moves to $B4). */
static void poll_follow(mc_adbctl *c, uint8_t cmd) {
    const uint8_t a = cmd & 0xF0u;
    if (!in_mask(c, a >> 4) || a == c->mem[POLL]) return;
    c->mem[PREV] = c->mem[POLL];
    c->mem[POLL] = a;
}

static void adb_packet(mc_adbctl *c, uint8_t cmd, const uint8_t *data, unsigned n) {
    const unsigned op = cmd & 0x0Fu;
    if (op == 0) {                                          /* SendReset */
        if (n) { error(c, 3, 0, cmd); return; }
        mc_adb_reset(c->adb);
        c->mem[POLL] = 0x30;
        c->mem[PREV] = 0x20;
        reply3(c, 0, 0, cmd, processing(c) + us(3300));
    } else if (op == 1) {                                   /* Flush */
        if (n) { error(c, 3, 0, cmd); return; }
        poll_follow(c, cmd);
        mc_adb_command(c->adb, cmd, NULL, 0, NULL);
        reply3(c, 0, 0, cmd, processing(c) + adb_time(c, 0, 0));
    } else if (op < 8) {
        error(c, 2, 0, cmd);
    } else if (op < 12) {                                   /* Listen */
        if (n < 2) { error(c, 3, 0, cmd); return; }
        if (n > 8) n = 8;
        poll_follow(c, cmd);
        mc_adb_command(c->adb, cmd, data, n, NULL);
        reply3(c, 0, 0, cmd, processing(c) + adb_time(c, n, 1));
    } else {                                                /* Talk */
        if (n) { error(c, 3, 0, cmd); return; }
        poll_follow(c, cmd);
        uint8_t p[3 + 8], flags;
        const unsigned got = adb_talk(c, cmd, p + 3, &flags);
        p[0] = 0;
        p[1] = flags;
        p[2] = cmd;
        reply(c, p, 3 + got, processing(c) + adb_time(c, got, got != 0));
    }
}

/* --- pseudo-commands ----------------------------------------------------------- */

static void pseudo_packet(mc_adbctl *c, uint8_t cmd, const uint8_t *d, unsigned n) {
    const int want = c->kind == MC_ADBCTL_CUDA ? mc_cuda_pseudo_length(c, cmd) : mc_egret_pseudo_length(c, cmd);
    if (want == -1) { error(c, 2, 1, cmd); return; }
    if ((want >= 0 && n != (unsigned)want) || (want == -2 && n < 2) || (want == -3 && (n < 1 || n > 4))) {
        error(c, 3, 1, cmd);
        return;
    }
    if ((c->kind == MC_ADBCTL_CUDA ? mc_cuda_pseudo : mc_egret_pseudo)(c, cmd, d, n)) return;
    uint8_t p[8] = {1, 0, cmd};
    switch (cmd) {
    case 0x00:                                              /* autopoll and one-second packets off */
        c->mem[A2] &= (uint8_t)~0x90;
        c->mem[tick_mode(c)] = 0;
        if (c->kind == MC_ADBCTL_CUDA) c->mem[A3] = (uint8_t)((c->mem[A3] & ~0x20u) | 0x10u);
        break;
    case 0x01:                                              /* autopoll on/off */
        c->mem[A2] &= (uint8_t)~0x80;
        if (d[0]) {
            c->mem[A2] |= 0x80u;
            c->mem[POLL] = 0x30;
            c->mem[PREV] = 0x20;
            c->next_poll = mc_adbctl_now(c);
        }
        break;
    case 0x02:                                              /* read the address space */
        reply(c, p, 3, processing(c));
        c->stream = (uint16_t)((d[0] << 8 | d[1]) & 0x1FFFu);
        c->stream_kind = 1;
        return;
    case 0x03:                                              /* the clock */
        clock_to_mem(c);
        memcpy(p + 3, c->mem + CLOCK, 4);
        c->mem[A2] &= (uint8_t)~0x01;
        reply(c, p, 7, processing(c));
        return;
    case 0x07: {                                            /* read parameter RAM */
        const uint16_t a = pram_address(c, d[0], d[1]);
        if (a == 0xFFFFu) { error(c, 4, 1, cmd); return; }
        reply(c, p, 3, processing(c));
        c->stream = a;
        c->stream_kind = 2;
        return;
    }
    case 0x08: {                                            /* write the address space */
        uint16_t a = (uint16_t)(d[0] << 8 | d[1]);
        for (unsigned i = 2; i < n; i++, a++) mem_write(c, a, d[i]);
        break;
    }
    case 0x09:                                              /* set the clock */
        clock_set(c, get32(d));
        if (c->kind == MC_ADBCTL_CUDA) c->mem[A1] |= 0x10u;
        break;
    case 0x0A:                                              /* power down */
        c->mem[A3] &= (uint8_t)~0x04;
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_POWER, 0);
        return;                                             /* the host is off: no reply */
    case 0x0B:                                              /* wake-up time */
        memcpy(c->mem + ALARM, d, 4);
        break;
    case 0x0C: {                                            /* write parameter RAM */
        uint16_t a = pram_address(c, d[0], d[1]);
        if (a == 0xFFFFu) { error(c, 4, 1, cmd); return; }
        for (unsigned i = 2; i < n; i++, a = pram_next(c, a)) mem_write(c, a, d[i]);
        break;
    }
    case 0x0D:                                              /* refuse the next keyboard reset */
        c->mem[A2] = (uint8_t)(d[0] ? c->mem[A2] | 0x40u : c->mem[A2] & ~0x40u);
        break;
    case 0x11:                                              /* restart: no reply */
        c->mem[A1] &= (uint8_t)~0x80;
        c->restart_due = 1;
        return;
    case 0x13:
        c->mem[A3] = (uint8_t)(d[0] ? c->mem[A3] | 0x04u : c->mem[A3] & ~0x04u);
        break;
    case 0x14: c->mem[RATE] = d[0]; break;                  /* autopoll period */
    case 0x16: p[3] = c->mem[RATE]; reply(c, p, 4, processing(c)); return;
    case 0x19:                                              /* autopoll device mask */
        c->mem[mask_hi(c)] = d[0];
        c->mem[mask_hi(c) + 1u] = d[1];
        break;
    case 0x1A:
        p[3] = c->mem[mask_hi(c)];
        p[4] = c->mem[mask_hi(c) + 1u];
        reply(c, p, 5, processing(c));
        return;
    case 0x1B:                                              /* one-second packets: mode 1 the time, 2 short, 3 a byte */
        c->mem[A2] &= (uint8_t)~0x10;
        c->mem[tick_mode(c)] = 0;
        if (d[0] && d[0] < 4) {
            c->mem[A2] |= 0x11u;
            c->mem[tick_mode(c)] = d[0];
        }
        break;
    default:
        break;
    }
    reply(c, p, 3, processing(c));
}

void mc_adbctl_packet(mc_adbctl *c) {
    c->reply_pending = 0;
    if (c->rx_n < 2) return;                                /* the host gave up */
    const uint8_t type = c->rx[0], cmd = c->rx[1];
    const uint8_t *data = c->rx + 2;
    const unsigned n = c->rx_n - 2u;
    if (type == 0) adb_packet(c, cmd, data, n);
    else if (type == 1) pseudo_packet(c, cmd, data, n);
    else error(c, 1, type, cmd);
}

/* --- unsolicited packets ------------------------------------------------------------ */

/* Autopoll's period after a poll (Cuda: the core timer's 0.488 ms ticks,
 * 2 x rate - 3 of them, 2 x rate - 8 after a packet; Egret: its loop of
 * about 1 ms per count, rate - 2)†. */
static uint64_t poll_period(const mc_adbctl *c, int had_data) {
    const unsigned rate = c->mem[RATE] ? c->mem[RATE] : 1u;
    if (c->kind == MC_ADBCTL_CUDA) {
        int ticks = 2 * (int)rate - (had_data && rate > 8 ? 8 : 3);
        if (ticks < 1) ticks = 1;
        return (uint64_t)ticks * MC_C15M_HZ / 2048u;
    }
    return (uint64_t)(rate > 2 ? rate - 2u : 1u) * (MC_C15M_HZ / 1000u);
}

/* One autopoll: Talk R0 to the device asked first; after a Service Request,
 * the one before it, then every other address in the mask going down,
 * until one answers, which becomes the first. */
static int autopoll(mc_adbctl *c) {
    uint8_t p[3 + 8], flags = 0;
    const uint64_t now = mc_adbctl_now(c);
    uint64_t spent = 0;
    unsigned got = 0;
    uint8_t cmd = (uint8_t)(c->mem[POLL] | 0x0Cu);
    if (!c->search) {
        got = adb_talk(c, cmd, p + 3, &flags);
        spent += adb_time(c, got, got != 0);
        if (!got && !(flags & 1u)) {
            c->next_poll = now + spent + poll_period(c, 0);
            return 0;
        }
        c->search = flags & 1u;
    } else {
        c->search = 0;
        const uint8_t first = c->mem[POLL];
        uint8_t a = c->mem[PREV];
        for (unsigned tries = 0; tries < 16; tries++) {
            if (a != first && (tries == 0 || in_mask(c, a >> 4))) {
                cmd = (uint8_t)(a | 0x0Cu);
                got = adb_talk(c, cmd, p + 3, &flags);
                spent += adb_time(c, got, got != 0);
                if (got) {
                    c->mem[PREV] = first;
                    c->mem[POLL] = a;
                    break;
                }
            }
            a = (uint8_t)((a - 0x10u) & 0xF0u);
        }
        if (!got) {
            c->next_poll = now + spent + poll_period(c, 0);
            return 0;
        }
        c->search = flags & 1u;
    }
    if (!got) {
        c->next_poll = now + spent;                         /* look for the requester at once */
        return 0;
    }
    p[0] = 0;
    p[1] = (uint8_t)(0x40u | flags);
    p[2] = cmd;
    reply(c, p, 3 + got, spent);
    c->next_poll = c->search ? now + spent : now + spent + poll_period(c, 1);
    return 1;
}

static int tick_packet(mc_adbctl *c) {
    const uint8_t mode = c->mem[tick_mode(c)];
    c->mem[A2] &= (uint8_t)~0x08;
    if ((c->mem[A2] & 0x01u) || mode == 1) {                /* the whole time */
        uint8_t p[7] = {1, 0, 3};
        clock_to_mem(c);
        memcpy(p + 3, c->mem + CLOCK, 4);
        c->mem[A2] &= (uint8_t)~0x01;
        reply(c, p, 7, 0);
    } else if (mode == 2) {
        reply3(c, 1, 0, 3, 0);
    } else {
        const uint8_t p[1] = {3};
        reply(c, p, 1, 0);
    }
    return 1;
}

int mc_adbctl_unsolicited(mc_adbctl *c) {
    const uint64_t now = mc_adbctl_now(c);
    if (c->post_n) {
        reply(c, c->post, c->post_n, 0);
        c->post_n = 0;
        return 1;
    }
    if ((c->mem[A2] & 0x18u) == 0x18u) return tick_packet(c);
    if (c->kind == MC_ADBCTL_CUDA && c->power_key == 1 && (c->mem[A3] & 0x20u) && !(c->mem[A1] & 0x02u)) {
        /* Cuda's power-key packet (341S0788 at $1661-$1735). */
        const uint8_t p[3] = {4, 4, 4};
        c->power_key = 2;
        c->mem[A1] |= 0x02u;
        c->mem[A6] = 0x64;
        reply(c, p, 3, 0);
        return 1;
    }
    if ((c->mem[A2] & 0x80u) && now >= c->next_poll) return autopoll(c);
    return 0;
}

uint64_t mc_adbctl_own_deadline(const mc_adbctl *c) {
    uint64_t t = MC_NEVER;
    if (c->mem[A2] & 0x10u && c->next_tick < t) t = c->next_tick;
    if (c->link == 0) {                     /* the link's own states wait for the host */
        if (c->reply_pending) {
            if (c->reply_at < t) t = c->reply_at;
        } else if (c->mem[A2] & 0x80u && c->next_poll < t) t = c->next_poll;
    }
    if (c->reset_until && c->reset_until < t) t = c->reset_until;
    if (c->nmi_until && c->nmi_until < t) t = c->nmi_until;
    if (c->mem[A6] && c->kind == MC_ADBCTL_CUDA && c->mem[A3] & 0x20u && c->next_rti < t) t = c->next_rti;
    return t;
}

/* --- the device ---------------------------------------------------------------------- */

static void link(mc_adbctl *c) {
    if (c->kind == MC_ADBCTL_CUDA) mc_cuda_link(c);
    else mc_egret_link(c);
}

void mc_adbctl_reschedule(mc_adbctl *c) {
    if (c->firmware) {
        mc_dev_schedule(&c->dev, mc_adbctl_now(c) + c->slice);
        return;
    }
    uint64_t t = mc_adbctl_own_deadline(c);
    if (c->link_at && c->link_at < t) t = c->link_at;
    mc_dev_schedule(&c->dev, t);
}

/* Cold start of the protocol level: the firmware's reset values. */
static void protocol_reset(mc_adbctl *c) {
    memset(c->mem + 0x90, 0, 0x100 - 0x90);
    c->mem[RATE] = 0x0B;
    c->mem[mask_hi(c) + 1u] = 0x0C;
    if (c->kind == MC_ADBCTL_EGRET) {
        c->mem[DELAY] = 0x0F;
        c->mem[A6] = 0x02;
    }
    clock_to_mem(c);
    c->link = 0;
    c->link_at = 0;
    c->reply_pending = 0;
    c->search = 0;
    c->restart_due = 0;
    c->stream = 0xFFFFu;
}

static void restart(mc_adbctl *c, uint64_t now) {
    /* The firmware's restart: autopoll and packets off, the 68k reset. */
    const uint8_t a1 = c->mem[A1] & 0x10u;
    protocol_reset(c);
    c->mem[A1] = a1;
    c->reset_until = now + us(250000);
    mc_adbctl_set_line(c, MC_ADBCTL_LINE_RESET, 1);
    mc_adbctl_set_xcvr(c, 1);
}

static void protocol_event(mc_adbctl *c, uint64_t now) {
    if (c->reset_until && now >= c->reset_until) {
        c->reset_until = 0;
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_RESET, 0);
    }
    if (c->nmi_until && now >= c->nmi_until) {
        c->nmi_until = 0;
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_NMI, 0);
    }
    while (now >= c->next_tick) {
        if (c->mem[A2] & 0x08u) c->mem[A2] |= 0x01u;       /* a tick not sent: the whole time next */
        c->mem[A2] |= 0x08u;
        c->next_tick += MC_C15M_HZ;
    }
    if (c->kind == MC_ADBCTL_CUDA && c->mem[A6] && (c->mem[A3] & 0x20u) && now >= c->next_rti) {
        if (--c->mem[A6] == 0) c->mem[A3] &= (uint8_t)~0x20;
    }
    while (now >= c->next_rti) c->next_rti += (uint64_t)MC_C15M_HZ / 16u;    /* 2^17 bus cycles, 62.5 ms† */
    link(c);
    if (c->restart_due && c->link == 0) restart(c, now);
}

static void link_and_restart(mc_adbctl *c) {
    link(c);
    if (c->restart_due && c->link == 0) restart(c, mc_adbctl_now(c));
}

/* The firmware runs in slices up to the CPU's time. An edge it makes on
 * the VIA's lines is seen by the CPU at once, so after one the firmware
 * runs on for HOLD more: its own next instructions (its sampling of CB2
 * 2.4 us after the shift register's last clock) come before the CPU's
 * reaction, as the CPU's interrupt latency guarantees on the real machine. */
#define HOLD (MC_C15M_HZ / 1000000u * 4u)

/* --- firmware level --------------------------------------------------------------- */

/* Port wiring (both kinds, from the firmware: 341S0851, 341S0788):
 * PA0 soft power (driven high on, low off), PA2 the power key (low held),
 * PA3 /RESET (with PC3 while driven), PA6/PA7 the ADB line in/out, PA1,
 * PA4, PA5 straps (PA5 Egret's model strap†); PB0 the system's power, PB1
 * TREQ/XcvrSes to VIA PB3, PB2 from VIA PB4, PB3 from VIA PB5, PB4 CB1,
 * PB5 CB2; PB6/PB7 the IIC lines (no device: pulled up); PC0-PC2 the
 * IPL lines (all low: NMI). */
/* The IIC bus on port B, open drain (the firmware drives a line low by
 * setting its DDR bit, the latch bit 0): SCL on PB7, SDA on PB6 (341S0417's
 * byte transfer at $198D-$19A9, the acknowledge read at $19ED-$19F5, with
 * clock stretching waits on PB7). With a device on the bus (iic_device) it
 * acknowledges each byte: after a start (SDA falling while SCL is high) it
 * pulls SDA low from the eighth clock's falling edge to the ninth's, as an
 * IIC receiver does; a stop (SDA rising while SCL is high) ends it. */
static void iic_master(mc_adbctl *c, int scl, int sda) {
    if (scl && c->iic_scl && sda != c->iic_sda) {
        c->iic_active = (uint8_t)!sda;
        c->iic_bits = 0;
        c->iic_ack = 0;
    } else if (c->iic_active && scl != c->iic_scl) {
        if (scl) {
            if (c->iic_bits < 9) c->iic_bits++;
        } else if (c->iic_bits == 8) {
            c->iic_ack = 1;
        } else if (c->iic_bits == 9) {
            c->iic_ack = 0;
            c->iic_bits = 0;
        }
    }
    c->iic_scl = (uint8_t)scl;
    c->iic_sda = (uint8_t)sda;
}

static uint8_t fw_pins(void *ctx, unsigned port, uint64_t now) {
    mc_adbctl *c = ctx;
    switch (port) {
    case 0: {
        uint8_t a = 0xFF;
        if (c->power_key || c->power_boot) a &= (uint8_t)~0x04;
        if (!c->strap) a &= (uint8_t)~0x20;
        if (!mc_adb_wire_level(&c->wire, now)) a &= (uint8_t)~0x40;
        return a;
    }
    case 1: {
        uint8_t b = 0x10u | 0x02u;
        if (!c->iic_device) b |= 0xC0u;
        else {
            if (c->iic_scl) b |= 0x80u;
            if (c->iic_sda && !c->iic_ack) b |= 0x40u;
        }
        if (c->line_state[MC_ADBCTL_LINE_POWER]) b |= 0x01u;
        if (c->via_pb & 0x10u) b |= 0x04u;
        if (c->via_pb & 0x20u) b |= 0x08u;
        /* CB2: what the VIA drives in its shift-out modes, else pulled up. */
        if (!c->via || mc_via_sr_mode(c->via) < 4 || c->via->cb2_out) b |= 0x20u;
        return b;
    }
    default:
        return 0xFF;
    }
}

static void fw_outputs(void *ctx, unsigned port, uint8_t levels, uint8_t driven, uint64_t now) {
    mc_adbctl *c = ctx;
    mc_adbctl_mcu *m = c->mcu;
    if (port == 0) {
        if (driven & 0x01u) {
            if ((levels & 1u) && !c->line_state[MC_ADBCTL_LINE_POWER]) {
                mc_adbctl_set_line(c, MC_ADBCTL_LINE_POWER, 1);
                /* Cuda's battery-backed state also says the clock was
                 * set (A1 bit 4, answered by pseudo-command $10): Livadix's
                 * clock comes from the host or emulation.nvram. */
                if (m->keep_ram && c->kind == MC_ADBCTL_CUDA) m->ram[A1] |= 0x10u;
                c->keep_ram = 0;
                m->keep_ram = 0;
            } else if (!(levels & 1u)) mc_adbctl_set_line(c, MC_ADBCTL_LINE_POWER, 0);
        }
        mc_adb_wire_host(&c->wire, (driven & 0x80u) && (levels & 0x80u), now);
    } else if (port == 1) {
        if (c->iic_device) iic_master(c, (levels & 0x80u) != 0, (levels & 0x40u) != 0);
        c->hold = now + HOLD;
        mc_adbctl_set_xcvr(c, levels & 0x02u);
        if (c->via) {
            if (driven & 0x20u) mc_via_set_cb2(c->via, levels & 0x20u);
            else mc_via_set_cb2(c->via, 1);
            mc_via_set_cb1(c->via, levels & 0x10u);
        }
    }
    if (port == 0 || port == 2) {
        const int reset = !(m->latch[0] & m->ddr[0] & 0x08u) || ((m->ddr[2] & 0x08u) && !(m->latch[2] & 0x08u));
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_RESET, reset);
        const int nmi = (m->ddr[2] & 0x07u) == 0x07u && !(m->latch[2] & 0x07u);
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_NMI, nmi);
    }
}


static void fw_run(mc_adbctl *c, uint64_t until) {
    mc_adbctl_mcu_run(c->mcu, until);
    while (c->hold > mc_adbctl_mcu_now(c->mcu)) mc_adbctl_mcu_run(c->mcu, c->hold);
}

void mc_adbctl_sync(mc_adbctl *c) {
    if (c->firmware) fw_run(c, mc_adbctl_now(c));
}

int mc_adbctl_use_firmware(mc_adbctl *c, const uint8_t *image, size_t size) {
    mc_adbctl_kind kind;
    if (!mc_adbctl_firmware_name(image, size, &kind) || kind != c->kind) return 0;
    if (!c->mcu) c->mcu = calloc(1, sizeof *c->mcu);
    if (!c->mcu) return 0;
    const uint64_t now = mc_adbctl_now(c);
    mc_adbctl_mcu_init(c->mcu, image, now, c, fw_pins, fw_outputs);
    mc_adbctl_mcu_set_irq(c->mcu, c->kind != MC_ADBCTL_CUDA);   /* Cuda: the power, off */
    /* The battery-backed RAM: the parameter RAM and the clock survive the
     * firmware's cold start (its RAM clear) until it first powers up. */
    memcpy(c->mcu->ram + 0x100, c->mem + 0x100, 0x100);
    put32(c->mcu->ram + CLOCK, clock_now(c));
    c->mcu->keep_ram = 1;
    c->keep_ram = 1;
    mc_adb_wire_init(&c->wire, c->adb);
    c->firmware = 1;
    c->slice = MC_C15M_HZ / 20000u;     /* 50 us */
    c->line_state[MC_ADBCTL_LINE_POWER] = 0;
    mc_dev_line(&c->dev, MC_ADBCTL_LINE_POWER, 0);
    mc_adbctl_set_line(c, MC_ADBCTL_LINE_RESET, 1);
    c->power_boot = 1;                  /* pressed until the power comes on */
    mc_adbctl_reschedule(c);
    return 1;
}

/* --- interface ---------------------------------------------------------------------- */

void mc_adbctl_via_pb(mc_adbctl *c, uint8_t pb) {
    if (c->firmware) {
        mc_adbctl_sync(c);
        c->via_pb = pb;
        return;
    }
    c->via_pb = pb;
    link_and_restart(c);
    mc_adbctl_reschedule(c);
}

void mc_adbctl_poke(mc_adbctl *c) {
    if (c->firmware) return;
    const uint64_t soon = mc_adbctl_now(c) + us(1000);
    if ((c->mem[A2] & 0x80u) && c->next_poll > soon) {
        c->next_poll = soon;
        mc_adbctl_reschedule(c);
    }
}

void mc_adbctl_post(mc_adbctl *c, const uint8_t *p, unsigned n) {
    if (c->firmware || n == 0 || n > sizeof c->post) return;
    memcpy(c->post, p, n);
    c->post_n = (uint8_t)n;
    link(c);
    mc_adbctl_reschedule(c);
}

void mc_adbctl_power_key(mc_adbctl *c, int down) {
    if (c->firmware) {
        mc_adbctl_sync(c);
        c->power_key = (uint8_t)(down != 0);
        return;
    }
    if (!down) {
        c->power_key = 0;
        return;
    }
    if (!c->line_state[MC_ADBCTL_LINE_POWER]) {
        mc_adbctl_set_line(c, MC_ADBCTL_LINE_POWER, 1);
        restart(c, mc_adbctl_now(c));
        return;
    }
    if (!c->power_key) c->power_key = 1;
    link(c);
    mc_adbctl_reschedule(c);
}

static void adbctl_event(void *dev, uint64_t now) {
    mc_adbctl *c = dev;
    if (c->firmware) {
        fw_run(c, now);
        if (c->line_state[MC_ADBCTL_LINE_POWER] && c->power_boot && c->keep_ram == 0) {
            /* The start's power key held key_hold longer (the board's choice). */
            if (c->key_hold && !c->key_release) c->key_release = now + c->key_hold;
            if (!c->key_release || now >= c->key_release) c->power_boot = 0;
        }
        mc_adbctl_reschedule(c);
        return;
    }
    protocol_event(c, now);
    mc_adbctl_reschedule(c);
}

static void adbctl_reset(void *dev) {
    mc_adbctl *c = dev;
    if (c->firmware) {
        mc_adbctl_reschedule(c);
        return;
    }
    const uint64_t now = mc_adbctl_now(c);
    protocol_reset(c);
    c->next_tick = now + MC_C15M_HZ;
    c->next_rti = now;
    c->xcvr = 0;
    mc_adbctl_set_xcvr(c, 1);
    mc_adbctl_set_line(c, MC_ADBCTL_LINE_POWER, 1);
    mc_adbctl_set_line(c, MC_ADBCTL_LINE_RESET, 0);
    mc_adbctl_reschedule(c);
}

const mc_dev_ops mc_adbctl_ops = {NULL, NULL, adbctl_event, adbctl_reset, NULL};

void mc_adbctl_init(mc_adbctl *c, const mc_hw_host *host, const char *name, mc_adbctl_kind kind,
                    mc_via *via1, mc_adb_bus *adb) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_adbctl_ops;
    c->dev.host = host;
    c->dev.name = name;
    c->kind = kind;
    c->via = via1;
    c->adb = adb;
    c->iic_scl = c->iic_sda = 1;        /* the IIC lines pulled up */
    c->via_pb = 0xFF;
    c->xcvr = 1;
    c->strap = 1;
    c->stream = 0xFFFFu;
    /* The firmware's clock at its cold start ($630BD178 written by its reset
     * code) until the machine sets it from emulation.nvram or the host. */
    c->seconds = 0x630BD178u;
    c->base = mc_adbctl_now(c);
    protocol_reset(c);
    c->next_tick = c->base + MC_C15M_HZ;
}

/* --- firmware identification -------------------------------------------------------- */

/* docs/NOTES.md, "Controller firmware": the parts are romdb.c's; the kind
 * follows the file's standard name. */
const char *mc_adbctl_firmware_name(const uint8_t *image, size_t size, mc_adbctl_kind *kind) {
    if (size != MC_ADBCTL_FW_SIZE) return NULL;
    const mc_firmware *fw = mc_firmware_identify(image, size);
    if (!fw) return NULL;
    const int egret = !strncmp(fw->file, "egret-", 6), cuda = !strncmp(fw->file, "cuda-", 5);
    if (!egret && !cuda) return NULL;
    if (kind) *kind = egret ? MC_ADBCTL_EGRET : MC_ADBCTL_CUDA;
    return fw->part;
}

int mc_adbctl_select(mc_adbctl *c, mc_adbctl_level level, const mc_rom_index *roms, const char *const *keys) {
    if (level == MC_ADBCTL_PROTOCOL || !roms || !keys) return level == MC_ADBCTL_FIRMWARE ? -1 : 0;
    for (; *keys; keys++) {
        const mc_rom_file *file = mc_rom_index_firmware(roms, mc_firmware_find(*keys));
        FILE *f = file ? fopen(file->path, "rb") : NULL;
        if (!f) continue;
        uint8_t image[MC_ADBCTL_FW_SIZE + 1];
        const size_t n = fread(image, 1, sizeof image, f);
        fclose(f);
        if (mc_adbctl_use_firmware(c, image, n)) return 1;
    }
    return level == MC_ADBCTL_FIRMWARE ? -1 : 0;
}

/* --- persistence ------------------------------------------------------------------- */

static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

int mc_adbctl_nvram_load(mc_adbctl *c, const char *path) {
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    uint8_t buf[8 + 1 + 8 + 256];
    int ok = 0;
    if (f && fread(buf, 1, sizeof buf, f) == sizeof buf && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1) {
        int64_t offset = 0;
        offset = mc_nvram_clock_offset(buf + 9);
        memcpy(mc_adbctl_pram(c), buf + 17, 256);
        if (c->firmware) memcpy(c->mem + 0x100, buf + 17, 256);
        c->seconds = (uint32_t)offset;      /* the host's time is added by set_clock */
        c->base = mc_adbctl_now(c);
        ok = 1;
    }
    if (f) fclose(f);
    return ok;
}

void mc_adbctl_set_clock(mc_adbctl *c, int64_t host_seconds) {
    mc_adbctl_set_seconds(c, (uint32_t)((int64_t)clock_now(c) + host_seconds));
}

int mc_adbctl_nvram_save(mc_adbctl *c, const char *path, int64_t host_start) {
    if (!path || !path[0]) return 0;
    uint8_t buf[8 + 1 + 8 + 256];
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t offset = (int64_t)mc_adbctl_seconds(c) - host_start - (int64_t)(mc_adbctl_now(c) / MC_C15M_HZ);
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    memcpy(buf + 17, mc_adbctl_pram(c), 256);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    fclose(f);
    c->dirty = 0;
    return ok;
}
