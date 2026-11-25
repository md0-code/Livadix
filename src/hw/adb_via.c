/* Livadix — the VIA ADB transceiver (see adb_via.h for sources). Livadix's own
 * code. */
#include "adb_via.h"

#include <string.h>

enum { ACT_NONE, ACT_COMMAND, ACT_BYTE, ACT_POLL };

static void set_int(mc_adb_via *x, uint8_t level) {
    if (level != x->int_level) {
        x->int_level = level;
        mc_dev_line(&x->dev, MC_ADB_VIA_LINE_INT, level);
    }
}

static void arm(mc_adb_via *x, uint8_t action, uint64_t delay) {
    x->action = action;
    mc_dev_schedule(&x->dev, action == ACT_NONE ? MC_NEVER : mc_dev_now(&x->dev) + delay);
}

static int is_talk(uint8_t cmd) { return ((cmd >> 2) & 3u) == 3u; }
static int is_listen(uint8_t cmd) { return ((cmd >> 2) & 3u) == 2u; }

static void run_talk(mc_adb_via *x) {
    x->n = mc_adb_command(x->bus, x->cmd, NULL, 0, x->data);
    x->srq = (uint8_t)mc_adb_srq(x->bus, x->cmd >> 4);
    x->index = 0;
    x->talk_due = 0;
}

/* The command byte goes out (state 0). */
static void do_command(mc_adb_via *x) {
    if (x->held) {
        /* An answer from polling is waiting: it goes to the host instead. */
        set_int(x, 0);
        x->shift(x->shift_ctx, 0xFF);
        x->cmd = x->poll_cmd;
        memcpy(x->data, x->held_data, sizeof x->data);
        x->n = x->held_n;
        x->srq = x->held_srq;
        x->index = 0;
        x->held = 0;
        x->talk_due = 0;
        x->listening = 0;
        return;
    }
    set_int(x, 1);
    x->cmd = x->shift(x->shift_ctx, 0xFF);
    x->listening = 0;
    x->talk_due = 0;
    x->n = 0;
    x->index = 0;
    if (is_talk(x->cmd)) x->talk_due = 1;
    else if (is_listen(x->cmd)) x->listening = 1;
    else mc_adb_command(x->bus, x->cmd, NULL, 0, NULL);     /* SendReset, Flush */
}

/* One data byte in state 1 or 2. */
static void do_byte(mc_adb_via *x) {
    if (x->listening) {
        const uint8_t byte = x->shift(x->shift_ctx, 0xFF);
        if (x->index < 8) x->data[x->index] = byte;
        x->index++;
        set_int(x, x->index == 2 && mc_adb_srq(x->bus, x->cmd >> 4) ? 0 : 1);
        return;
    }
    if (x->talk_due) run_talk(x);
    uint8_t byte = 0xFF, level = 1;
    if (x->index == 0) level = x->n ? 1 : 0;                /* low: timeout */
    else if (x->index == 1) level = x->srq ? 0 : 1;         /* low: a Service Request */
    else level = x->index < x->n ? 1 : 0;                   /* low: the answer ended */
    if (x->index < x->n) byte = x->data[x->index];
    x->index++;
    set_int(x, level);
    x->shift(x->shift_ctx, byte);
}

/* Polling in state 3: the last Talk again. */
static void do_poll(mc_adb_via *x) {
    if (!x->poll_valid || x->held) return;
    x->held_n = mc_adb_command(x->bus, x->poll_cmd, NULL, 0, x->held_data);
    x->held_srq = (uint8_t)mc_adb_srq(x->bus, x->poll_cmd >> 4);
    if (x->held_n || x->held_srq) {
        x->held = 1;
        set_int(x, 1);
        x->shift(x->shift_ctx, 0xFF);   /* tells the host */
        return;
    }
    arm(x, ACT_POLL, MC_ADB_POLL_TIME);
}

static void finish_transaction(mc_adb_via *x) {
    if (x->listening) {
        mc_adb_command(x->bus, x->cmd, x->data, x->index > 8 ? 8 : x->index, NULL);
        x->listening = 0;
    }
    if (is_talk(x->cmd)) {
        x->poll_cmd = x->cmd;
        x->poll_valid = 1;
    }
}

void mc_adb_via_state(mc_adb_via *x, unsigned st) {
    st &= 3u;
    if (st == x->st) return;
    const unsigned old = x->st;
    x->st = (uint8_t)st;
    switch (st) {
    case 0:
        if (old != 0) finish_transaction(x);
        arm(x, ACT_COMMAND, MC_ADB_COMMAND_TIME);
        break;
    case 1:
    case 2:
        if (old == 3 && x->held) {
            /* The host reads what polling found. */
            x->cmd = x->poll_cmd;
            memcpy(x->data, x->held_data, sizeof x->data);
            x->n = x->held_n;
            x->srq = x->held_srq;
            x->index = 0;
            x->held = 0;
            x->talk_due = 0;
            x->listening = 0;
        }
        /* A Talk answer starts after the device's response time. */
        arm(x, ACT_BYTE, x->index == 0 && !x->listening ? MC_ADB_BYTE_TIME + MC_ADB_BYTE_TIME / 2u : MC_ADB_BYTE_TIME);
        break;
    case 3:
        finish_transaction(x);
        set_int(x, 1);
        arm(x, x->held ? ACT_NONE : ACT_POLL, MC_ADB_BYTE_TIME * 4u);
        break;
    }
}

void mc_adb_via_poke(mc_adb_via *x) {
    if (x->st == 3 && x->action == ACT_POLL) {
        const uint64_t soon = mc_dev_now(&x->dev) + MC_ADB_BYTE_TIME * 4u;
        mc_dev_schedule(&x->dev, soon);
    }
}

static void adbv_event(void *dev, uint64_t now) {
    (void)now;
    mc_adb_via *x = dev;
    const uint8_t action = x->action;
    x->action = ACT_NONE;
    switch (action) {
    case ACT_COMMAND: if (x->st == 0) do_command(x); break;
    case ACT_BYTE: if (x->st == 1 || x->st == 2) do_byte(x); break;
    case ACT_POLL: if (x->st == 3) do_poll(x); break;
    default: break;
    }
}

static void adbv_reset(void *dev) {
    mc_adb_via *x = dev;
    x->st = 3;
    x->held = 0;
    x->poll_valid = 0;
    x->listening = 0;
    x->talk_due = 0;
    x->int_level = 0;
    set_int(x, 1);
    arm(x, ACT_NONE, 0);
}

const mc_dev_ops mc_adb_via_ops = {NULL, NULL, adbv_event, adbv_reset};

void mc_adb_via_init(mc_adb_via *x, const mc_hw_host *host, const char *name, mc_adb_bus *bus,
                     uint8_t (*shift)(void *ctx, uint8_t in), void *shift_ctx) {
    memset(x, 0, sizeof *x);
    x->dev.ops = &mc_adb_via_ops;
    x->dev.host = host;
    x->dev.name = name;
    x->bus = bus;
    x->shift = shift;
    x->shift_ctx = shift_ctx;
    x->st = 3;
    x->int_level = 1;
}
