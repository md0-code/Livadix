/* Livadix — the Apple Desktop Bus at the bit-cell level (see adb_wire.h for
 * sources). Livadix's own code. */
#include "adb_wire.h"
#include "hw.h"

#include <string.h>

enum { IDLE, COMMAND, LISTEN };

#define T_RESET MC_ADB_US(2000)
#define T_ATTENTION MC_ADB_US(560)
#define T_ONE MC_ADB_US(50)
#define T_SRQ MC_ADB_US(300)
#define T_STOP_TO_START MC_ADB_US(200)
#define T_LISTEN_END MC_ADB_US(400)

void mc_adb_wire_init(mc_adb_wire *w, mc_adb_bus *bus) {
    memset(w, 0, sizeof *w);
    w->bus = bus;
}

static void device_low(mc_adb_wire *w, uint64_t start, uint64_t end) {
    if (w->nlow < MC_ADB_WIRE_LOWS) {
        w->low_start[w->nlow] = start;
        w->low_end[w->nlow] = end;
        w->nlow++;
    }
}

/* A device's answer: a 1 start bit, the bytes, a 0 stop bit. */
static void answer(mc_adb_wire *w, uint64_t t, const uint8_t *data, unsigned n) {
    const uint64_t cell = MC_ADB_US(100), one = MC_ADB_US(35), zero = MC_ADB_US(65);
    device_low(w, t, t + one);
    t += cell;
    for (unsigned i = 0; i < n * 8u; i++) {
        const int bit = (data[i / 8u] >> (7u - i % 8u)) & 1;
        device_low(w, t, t + (bit ? one : zero));
        t += cell;
    }
    device_low(w, t, t + zero);
    w->answers++;
}

static void commit_listen(mc_adb_wire *w) {
    const unsigned n = w->nbits ? (w->nbits - 1u) / 8u : 0;
    if (n) mc_adb_command(w->bus, w->cmd, w->data, n > 8 ? 8 : n, NULL);
    w->state = IDLE;
    w->nbits = 0;
}

static void command_done(mc_adb_wire *w, uint64_t t) {
    uint64_t end = t;
    w->commands++;
    if (mc_adb_srq(w->bus, w->cmd >> 4)) {
        end = w->fall + T_SRQ;
        if (end > t) device_low(w, t, end);
        else end = t;
    }
    const unsigned kind = (w->cmd >> 2) & 3u;
    if (kind == 3 && (w->cmd & 0x0Fu) >= 0x0Cu) {
        uint8_t out[8];
        const unsigned n = mc_adb_command(w->bus, w->cmd, NULL, 0, out);
        if (n) answer(w, end + T_STOP_TO_START, out, n);
        w->state = IDLE;
    } else if (kind == 2) {
        w->state = LISTEN;
        w->nbits = 0;
        memset(w->data, 0, sizeof w->data);
    } else {
        mc_adb_command(w->bus, w->cmd, NULL, 0, NULL);
        w->state = IDLE;
    }
}

void mc_adb_wire_host(mc_adb_wire *w, int low, uint64_t t) {
    low = low != 0;
    if (low == w->host_low) return;
    w->host_low = (uint8_t)low;
    if (low) {
        if (w->state == LISTEN && w->nbits > 1 && t - w->rise > T_LISTEN_END) commit_listen(w);
        w->fall = t;
        return;
    }
    w->rise = t;
    const uint64_t d = t - w->fall;
    if (d >= T_RESET) {
        mc_adb_reset(w->bus);
        w->state = IDLE;
        w->nlow = 0;
    } else if (d >= T_ATTENTION) {
        if (w->state == LISTEN) commit_listen(w);
        w->state = COMMAND;
        w->bits = 0;
        w->cmd = 0;
        w->nlow = 0;
    } else {
        const unsigned bit = d < T_ONE;
        if (w->state == COMMAND) {
            if (w->bits < 8) {
                w->cmd = (uint8_t)(w->cmd << 1 | bit);
                w->bits++;
            } else {
                command_done(w, t);
            }
        } else if (w->state == LISTEN) {
            if (w->nbits == 0) w->nbits = 1;     /* the start bit */
            else {
                const unsigned i = w->nbits - 1u;
                if (i < 64u && bit) w->data[i / 8u] |= (uint8_t)(0x80u >> (i % 8u));
                w->nbits++;
            }
        }
    }
}

int mc_adb_wire_level(mc_adb_wire *w, uint64_t t) {
    if (w->host_low) return 0;
    if (w->state == LISTEN && w->nbits > 1 && t - w->rise > T_LISTEN_END) commit_listen(w);
    for (unsigned i = 0; i < w->nlow; i++)
        if (t >= w->low_start[i] && t < w->low_end[i]) return 0;
    return 1;
}
