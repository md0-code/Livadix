/* Livadix — Egret's link and pseudo-commands (see egret.h and adbctl.h for
 * sources). Livadix's own code. The states follow the firmware's routines
 * (341S0851): receive $12C2/$14C8, send $1549/$1593/$164D. */
#include "egret.h"

#include <string.h>

enum {
    IDLE, RX, RX_SHIFT, RX_WAIT_EMPTY, TX_ATTN, TX_SES_LOW, TX_SES_HIGH, TX_WAIT_EMPTY, TX_SHIFT, TX_WAIT_FULL,
    TX_WAIT_EMPTY2, END_WAIT_SES, ABORTED
};

static int sysses(const mc_adbctl *c) { return (c->via_pb & 0x20u) != 0; }
static int viafull(const mc_adbctl *c) { return (c->via_pb & 0x10u) != 0; }

static void wait(mc_adbctl *c, uint8_t state, unsigned us) {
    c->link = state;
    c->link_at = mc_adbctl_now(c) + MC_ADBCTL_US(us);
}

#define T (mc_adbctl_timing_of(c))

/* The host ended Egret's packet early by negating SysSes (the IIsi ROM's
 * receive into a buffer of fixed length, 36B7FB6C at $408149C4-$408149D0,
 * negates it and waits for XcvrSes negated with interrupts masked): Egret
 * negates XcvrSes and starts nothing new for its attention time, the
 * firmware's turnaround back to its main loop†, so the host sees the line
 * high before a waiting packet asserts it again. */
static void give_up(mc_adbctl *c) {
    c->reply_pending = 0;
    mc_adbctl_set_xcvr(c, 1);
    c->link = ABORTED;
    c->link_at = mc_adbctl_now(c) + MC_ADBCTL_US(mc_adbctl_timing_of(c)->attention);
}

void mc_egret_link(mc_adbctl *c) {
    for (int guard = 0; guard < 64; guard++) {
        const uint64_t now = mc_adbctl_now(c);
        const int due = c->link_at && now >= c->link_at;
        switch (c->link) {
        case IDLE:
            c->link_at = 0;
            if (c->reply_pending && now >= c->reply_at) {
                mc_adbctl_set_xcvr(c, 0);
                wait(c, TX_ATTN, T->attention);
                continue;
            }
            if (c->reply_pending) return;
            if ((c->mem[0xA2] & 0x18u) != 0x18u && sysses(c)) {
                c->rx_n = 0;
                c->link = RX;
                continue;
            }
            if (mc_adbctl_unsolicited(c)) continue;
            return;
        case RX:
            if (viafull(c)) {
                wait(c, RX_SHIFT, c->rx_n ? T->rx_byte : T->rx_first);
                continue;
            }
            if (!sysses(c)) {
                mc_adbctl_packet(c);
                c->link = IDLE;
                continue;
            }
            return;
        case RX_SHIFT:
            if (!due) return;
            c->link_at = 0;
            {
                const uint8_t b = mc_via_shift8(c->via, 0xFF);
                mc_via_set_cb2(c->via, 1);
                if (c->rx_n < MC_ADBCTL_MAX_PACKET) c->rx[c->rx_n++] = b;
            }
            c->link = RX_WAIT_EMPTY;
            continue;
        case RX_WAIT_EMPTY:
            if (viafull(c)) return;
            c->link = RX;
            continue;
        case TX_ATTN:
            if (!due) return;
            c->link_at = 0;
            (void)mc_via_shift8(c->via, 0xFF);
            mc_via_set_cb2(c->via, 1);
            c->link = TX_SES_LOW;
            continue;
        case TX_SES_LOW:
            if (sysses(c)) return;
            c->link = TX_SES_HIGH;
            continue;
        case TX_SES_HIGH:
            if (!sysses(c)) return;
            c->tx_i = 0;
            c->link = TX_WAIT_EMPTY;
            continue;
        case TX_WAIT_EMPTY:
            if (!sysses(c)) { give_up(c); continue; }
            if (viafull(c)) return;
            wait(c, TX_SHIFT, c->tx_i ? T->tx_byte : T->tx_first);
            continue;
        case TX_SHIFT: {
            if (!due) return;
            c->link_at = 0;
            int last;
            const uint8_t b = mc_adbctl_tx_byte(c, c->tx_i, &last);
            if (last) mc_adbctl_set_xcvr(c, 1);
            (void)mc_via_shift8(c->via, b);
            mc_via_set_cb2(c->via, 1);
            c->tx_i++;
            c->ack = (uint8_t)last;
            c->link = TX_WAIT_FULL;
            continue;
        }
        case TX_WAIT_FULL:
            if (!sysses(c)) { give_up(c); continue; }
            if (!viafull(c)) return;
            c->link = TX_WAIT_EMPTY2;
            continue;
        case TX_WAIT_EMPTY2:
            if (!sysses(c)) { give_up(c); continue; }
            if (viafull(c)) return;
            if (c->ack) {
                c->reply_pending = 0;
                c->link = END_WAIT_SES;
            } else c->link = TX_WAIT_EMPTY;
            continue;
        case END_WAIT_SES:
            if (sysses(c)) return;
            c->link = IDLE;
            continue;
        case ABORTED:
            if (!due) return;
            c->link = IDLE;
            continue;
        default:
            c->link = IDLE;
            continue;
        }
    }
}

/* Data lengths of Egret's pseudo-commands (341S0851's table at $132D and
 * each handler's length check): -1 refused, -2 two or more, -3 one to four. */
int mc_egret_pseudo_length(const mc_adbctl *c, uint8_t cmd) {
    static const signed char len[0x20] = {
        0, 1, 2, 0, 0, 0, 0, 2, -2, 4, 0, 4, -2, 1, -3, 0,             /* $00-$0F */
        0, 0, 1, 1, 1, 0, 0, 1, 0, 2, 0, 1, 1, 1, 1, 0                  /* $10-$1F */
    };
    if (c->caboose && (cmd == 0x20 || cmd == 0x21)) return cmd == 0x20 ? 2 : 1;
    return cmd < sizeof len ? len[cmd] : -1;
}

static int answer(mc_adbctl *c, const uint8_t *p, unsigned n) {
    memcpy(c->tx, p, n);
    c->tx_n = n;
    c->tx_i = 0;
    c->stream = 0xFFFFu;
    c->reply_pending = 1;
    c->reply_at = mc_adbctl_now(c) + MC_ADBCTL_US(mc_adbctl_timing_of(c)->process - mc_adbctl_timing_of(c)->attention);
    return 1;
}

int mc_egret_pseudo(mc_adbctl *c, uint8_t cmd, const uint8_t *d, unsigned n) {
    uint8_t *m = c->mem;
    uint8_t p[5] = {1, 0, cmd, 0, 0};
    const uint8_t refused[4] = {2, 2, 1, cmd};
    (void)n;
    switch (cmd) {
    /* Constants the firmware answers ($170A, $1726, $1743, $1A23). */
    case 0x04: p[3] = 0x10; p[4] = 0x00; return answer(c, p, 5);
    case 0x05: p[3] = 0x0F; p[4] = 0x00; return answer(c, p, 5);
    case 0x06: p[3] = 0x0F; p[4] = 0x5A; return answer(c, p, 5);
    case 0x15: p[3] = 0x01; p[4] = 0x08; return answer(c, p, 5);
    case 0x0A:                      /* power down: only with the strap high */
        if (!c->strap) return answer(c, p, 3);
        return 0;
    case 0x0B:
    case 0x13:
        if (!c->strap) return answer(c, refused, 4);
        return 0;
    case 0x0E:                      /* to the sound chip: bit by bit on PB6/PB7 */
        return answer(c, p, 3);
    case 0x20:                      /* Caboose: kept (adbctl.h) */
    case 0x21:
        if (!c->caboose) return 0;
        memcpy(c->caboose_cmd[cmd - 0x20], d, n);
        return answer(c, p, 3);
    case 0x0F: {                    /* self-test: RAM, the ADB line, the ROM sum: all pass */
        const uint8_t r[5] = {1, 0, 0x0F, 0, 0};
        return answer(c, r, 5);
    }
    case 0x10:                      /* the soft power latch: $FF on */
        if (c->strap) return answer(c, refused, 4);
        p[3] = c->line_state[MC_ADBCTL_LINE_POWER] ? 0xFF : 0;
        return answer(c, p, 4);
    case 0x12:                      /* port A bit 1 */
        if (c->strap) return answer(c, refused, 4);
        return answer(c, p, 3);
    case 0x17: m[0xA5] = d[0]; return answer(c, p, 3);
    case 0x18: p[3] = m[0xA5]; return answer(c, p, 4);
    case 0x1C: m[0xA2] = (uint8_t)(d[0] ? m[0xA2] | 0x04u : m[0xA2] & ~0x04u); return answer(c, p, 3);
    case 0x1D: m[0xA1] = (uint8_t)(d[0] ? m[0xA1] | 0x40u : m[0xA1] & ~0x40u); return answer(c, p, 3);
    case 0x1E: m[0xA6] = d[0] ? d[0] : 0xFF; return answer(c, p, 3);
    case 0x1F: p[3] = m[0xA6]; return answer(c, p, 4);
    default:
        return 0;
    }
}
