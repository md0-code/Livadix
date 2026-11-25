/* Livadix — Cuda's link and pseudo-commands (see cuda.h and adbctl.h for
 * sources). Livadix's own code. The states follow the firmware's routines
 * (341S0788): receive $1274/$1488, send $1504/$154B/$162B, resynchronise
 * $15E1, end of packet $1609. */
#include "cuda.h"

#include <string.h>

enum {
    IDLE, RX, RX_SHIFT, TX_ATTN, TX_COLLIDE, TX_WAIT_TIP, TX_WAIT_ACK, TX_SHIFT, END_WAIT_TIP, END_CLOCKS,
    SYNC_CLOCKS, SYNC_WAIT_ACK, SYNC_CLOCKS2, SETTLE
};

static int tip(const mc_adbctl *c) { return !(c->via_pb & 0x20u); }
static uint8_t byteack(const mc_adbctl *c) { return (c->via_pb >> 4) & 1u; }

static void shift_idle(mc_adbctl *c) {
    (void)mc_via_shift8(c->via, 0xFF);
    mc_via_set_cb2(c->via, 1);
}

static void wait(mc_adbctl *c, uint8_t state, unsigned us) {
    c->link = state;
    c->link_at = mc_adbctl_now(c) + MC_ADBCTL_US(us);
}

#define T (mc_adbctl_timing_of(c))

/* Resynchronisation (TIP negated with BYTEACK low). */
static void resync(mc_adbctl *c) {
    c->reply_pending = 0;
    mc_adbctl_set_xcvr(c, 0);
    wait(c, SYNC_CLOCKS, T->attention);
}

/* A send given up (the host negated TIP): TREQ negated, 8 idle clocks once
 * TIP is negated. */
static void give_up(mc_adbctl *c) {
    c->reply_pending = 0;
    if (!byteack(c)) {
        resync(c);
        return;
    }
    mc_adbctl_set_xcvr(c, 1);
    wait(c, END_CLOCKS, T->attention);
}

void mc_cuda_link(mc_adbctl *c) {
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
            if (c->reply_pending) return;                   /* busy on the ADB */
            if (!tip(c) && !byteack(c)) { resync(c); continue; }
            if ((c->mem[0xA2] & 0x18u) != 0x18u && tip(c)) {
                c->rx_n = 0;
                c->ack = 0;
                c->link = RX;
                continue;
            }
            if (mc_adbctl_unsolicited(c)) continue;
            return;
        case RX:
            if (!tip(c)) {
                mc_adbctl_packet(c);
                c->link = IDLE;
                if (!byteack(c)) resync(c);
                continue;
            }
            if (byteack(c) != c->ack) {
                wait(c, RX_SHIFT, c->rx_n ? T->rx_byte : T->rx_first);
                continue;
            }
            return;
        case RX_SHIFT:
            if (!due) return;
            c->link_at = 0;
            if (!tip(c)) { c->link = RX; continue; }
            {
                const uint8_t b = mc_via_shift8(c->via, 0xFF);
                mc_via_set_cb2(c->via, 1);
                if (c->rx_n < MC_ADBCTL_MAX_PACKET) c->rx[c->rx_n++] = b;
            }
            c->ack = byteack(c);
            c->link = RX;
            continue;
        case TX_ATTN:
            if (!due) return;
            c->link_at = 0;
            shift_idle(c);
            c->link = tip(c) ? TX_COLLIDE : TX_WAIT_TIP;
            continue;
        case TX_COLLIDE:
            if (tip(c)) return;
            wait(c, TX_ATTN, T->attention);
            continue;
        case TX_WAIT_TIP:
            if (!tip(c)) {
                if (!byteack(c)) resync(c);
                return;
            }
            c->ack = 0;
            c->tx_i = 0;
            c->link = TX_WAIT_ACK;
            continue;
        case TX_WAIT_ACK:
            if (!tip(c)) { give_up(c); continue; }
            if (byteack(c) == c->ack) return;
            wait(c, TX_SHIFT, c->tx_i ? T->tx_byte : T->tx_first);
            continue;
        case TX_SHIFT: {
            if (!due) return;
            c->link_at = 0;
            if (!tip(c)) { give_up(c); continue; }
            int last;
            const uint8_t b = mc_adbctl_tx_byte(c, c->tx_i, &last);
            if (last) mc_adbctl_set_xcvr(c, 1);
            (void)mc_via_shift8(c->via, b);
            mc_via_set_cb2(c->via, 1);
            c->ack = byteack(c);
            c->tx_i++;
            if (last) {
                c->reply_pending = 0;
                c->link = END_WAIT_TIP;
            } else c->link = TX_WAIT_ACK;
            continue;
        }
        case END_WAIT_TIP:
            if (tip(c)) return;
            wait(c, END_CLOCKS, T->attention);
            continue;
        case END_CLOCKS:
            if (!due) return;
            shift_idle(c);
            wait(c, SETTLE, 90);
            continue;
        case SYNC_CLOCKS:
            if (!due) return;
            c->link_at = 0;
            shift_idle(c);
            c->link = SYNC_WAIT_ACK;
            continue;
        case SYNC_WAIT_ACK:
            if (!byteack(c)) return;
            mc_adbctl_set_xcvr(c, 1);
            wait(c, SYNC_CLOCKS2, T->attention);
            continue;
        case SYNC_CLOCKS2:
            if (!due) return;
            shift_idle(c);
            /* The firmware then starts over ($15FB-$1606): autopoll and the
             * one-second packets off ($A2 bits 7 and 4, the mode $C3), the
             * power-key packets off ($A3 bit 5, bit 4 set), as pseudo-command
             * $00 does; so a packet of its own never meets the host's next
             * send (the ROM's polled start-up send, $408064C6, does not look
             * for one). */
            c->mem[0xA2] &= 0x6Fu;
            c->mem[0xC3] = 0;
            c->mem[0xA3] = (uint8_t)((c->mem[0xA3] & ~0x20u) | 0x10u);
            wait(c, SETTLE, 90);
            continue;
        case SETTLE:
            if (!due) return;
            c->link_at = 0;
            c->link = IDLE;
            continue;
        default:
            c->link = IDLE;
            continue;
        }
    }
}

/* Data lengths of Cuda's pseudo-commands (341S0788's table at $12E4 and
 * each handler's length check): -1 refused, -2 two or more, -4 any. */
int mc_cuda_pseudo_length(const mc_adbctl *c, uint8_t cmd) {
    if(cmd==0x25 && c->iic_transfer)return -4;
    static const signed char len[0x25] = {
        0, 1, 2, 0, -1, -1, -1, 2, -2, 4, 0, 4, -2, 1, -4, -1,          /* $00-$0F */
        0, 0, 1, 1, 1, -1, 0, -1, -1, 2, 0, 1, -1, -1, -1, -1,          /* $10-$1F */
        -1, 1, -4, 1, 1                                                 /* $20-$24 */
    };
    return cmd < sizeof len ? len[cmd] : -1;
}

int mc_cuda_pseudo(mc_adbctl *c, uint8_t cmd, const uint8_t *d, unsigned n) {
    uint8_t *m = c->mem;
    uint8_t p[4] = {1, 0, cmd, 0};
    (void)n;
    switch (cmd) {
    case 0x0E:                      /* to the sound chip over IIC: sent without acknowledge */
        memcpy(c->tx, p, 3);
        c->tx_n = 3;
        c->tx_i = 0;
        c->stream = 0xFFFFu;
        c->reply_pending = 1;
        c->reply_at = mc_adbctl_now(c) + MC_ADBCTL_US(mc_adbctl_timing_of(c)->process - mc_adbctl_timing_of(c)->attention + 270u);
        return 1;
    case 0x10:                      /* was the clock set? 0 yes, $FF no */
        p[3] = (m[0xA1] & 0x10u) ? 0 : 0xFF;
        memcpy(c->tx, p, 4);
        c->tx_n = 4;
        goto send;
    case 0x12:                      /* Command-power: the full NMI ($A2 bit 5) */
        m[0xA2] = (uint8_t)(d[0] ? m[0xA2] | 0x20u : m[0xA2] & ~0x20u);
        break;
    case 0x21:                      /* power-key packets ($1B11: sub-commands 0-4) */
        switch (d[0]) {
        case 0: m[0xA3] &= (uint8_t)~0x20; m[0xA1] &= (uint8_t)~0x02; break;
        case 1: m[0xA3] |= 0x20u; m[0xA6] = 0; break;
        case 2:
            if (m[0xA3] & 0x20u) m[0xA1] |= 0x02u;
            else m[0xA1] &= (uint8_t)~0x02;
            break;
        case 3: m[0xA1] &= (uint8_t)~0x02; break;
        case 4:
            if ((m[0xA1] & 0x02u) && !(m[0xA3] & 0x20u)) {
                m[0xA3] |= 0x20u;
                m[0xA6] = 0x64;
            }
            break;
        default:
            p[0] = 2; p[1] = 2; p[2] = 1; p[3] = cmd;
            memcpy(c->tx, p, 4);
            c->tx_n = 4;
            goto send;
        }
        break;
    case 0x25:
    case 0x22: {                    /* IIC transfers to the board's devices. */
        if(c->iic_transfer){
            unsigned got=sizeof c->tx-3u;
            if(c->iic_transfer(c->iic_ctx,cmd,d,n,c->tx+3,&got)&&got<=sizeof c->tx-3u){
                memcpy(c->tx,p,3);c->tx_n=3u+got;
            }else{const uint8_t e[]={2,5,1,cmd};memcpy(c->tx,e,4);c->tx_n=4;}
            goto send;
        }
        if (c->iic_device) break;   /* or a device that acknowledges (adbctl.h) */
        const uint8_t e[4] = {2, 5, 1, cmd};
        memcpy(c->tx, e, 4);
        c->tx_n = 4;
        goto send;
    }
    case 0x23:                      /* port A bit 4's direction */
        break;
    case 0x24:
        m[0xA6] = d[0];
        break;
    default:
        return 0;
    }
    memcpy(c->tx, p, 3);
    c->tx_n = 3;
send:
    c->tx_i = 0;
    c->stream = 0xFFFFu;
    c->reply_pending = 1;
    c->reply_at = mc_adbctl_now(c) + MC_ADBCTL_US(mc_adbctl_timing_of(c)->process - mc_adbctl_timing_of(c)->attention);
    return 1;
}
