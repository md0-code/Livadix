/* Livadix — the PowerBook Power Manager at protocol level, and the front that
 * dispatches to the firmware level (see pmu.h for sources and the
 * protocol; pmu_fw.c for the firmware level). Livadix's own code.
 *
 * The protocol level reproduces what the M50753 firmware does, as its
 * disassembly shows and the differential (test_pmu) checks: the same
 * replies to the same commands, its ADB transactions and autopoll on its
 * tick, its battery bookkeeping for the battery pmu.h describes, its
 * interrupt pulses, its sleep and wake. The PG&E and PMU99 run the same
 * command set in the new framing; what the firmware does not show for them
 * (commands the M50753 does not know, the PMU99's one-second interrupt, the
 * version byte) is Livadix's choice, marked †. */
#include "../core/nvram.h"
#include "pmu.h"
#include "pmu_fw.h"
#include "pmu_pge.h"
#include "config.h"
#include "romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- command lengths ---------------------------------------------------------------
 * The ROMs' send and receive count tables (PB 160 ROM $89DAC and $89EAC;
 * the PB 150's adds $61, $6F and $D2): the send count, or $FF when a count
 * byte follows; the receive count (see pmu.h), 0 none. Listed are the
 * entries that differ from the rule $FF / (bit 3 of the command ? $FF : 0). */
static const uint8_t SEND_FIXED[][2] = {
    {0x10, 1}, {0x11, 1}, {0x18, 0}, {0x19, 0}, {0x1F, 0},
    {0x21, 0}, {0x22, 2}, {0x23, 1}, {0x24, 1}, {0x28, 0},
    {0x30, 4}, {0x31, 20}, {0x38, 0}, {0x39, 0}, {0x3A, 2},
    {0x40, 1}, {0x41, 1}, {0x48, 0}, {0x49, 0},
    {0x50, 1}, {0x51, 0}, {0x52, 2}, {0x53, 2}, {0x55, 1}, {0x56, 3}, {0x57, 1},
    {0x58, 0}, {0x59, 1}, {0x5A, 0}, {0x5B, 0}, {0x5C, 0},
    {0x60, 2}, {0x61, 1}, {0x68, 0}, {0x69, 0}, {0x6A, 0}, {0x6B, 0}, {0x6C, 0}, {0x6D, 0}, {0x6F, 0},
    {0x70, 1}, {0x71, 1}, {0x72, 1}, {0x78, 0}, {0x79, 0}, {0x7E, 4}, {0x7F, 4},
    {0x80, 4}, {0x82, 0}, {0x88, 0}, {0x90, 1}, {0x91, 2}, {0x98, 0}, {0x99, 0},
    {0xA0, 2}, {0xA1, 2}, {0xA2, 2}, {0xA3, 4}, {0xA5, 0}, {0xA8, 1}, {0xA9, 1}, {0xAA, 3}, {0xAB, 2},
    {0xD0, 0}, {0xD8, 1}, {0xD9, 1}, {0xDC, 0}, {0xDD, 0},
    {0xE1, 4}, {0xE2, 0}, {0xE8, 3}, {0xEA, 0}, {0xEC, 0}, {0xEF, 0},
};
static const uint8_t RECV_FIXED[][2] = {
    {0x18, 2}, {0x19, 2}, {0x1F, 0}, {0x38, 5}, {0x39, 21}, {0x48, 2}, {0x49, 2},
    {0x58, 2}, {0x59, 0}, {0x5A, 3}, {0x5B, 3}, {0x68, 4}, {0x69, 4}, {0x6A, 3}, {0x6B, 9}, {0x6F, 2},
    {0x7E, 1}, {0x7F, 1}, {0x88, 6}, {0x98, 2}, {0x99, 2}, {0xA8, 2}, {0xA9, 0}, {0xAA, 0}, {0xAB, 0},
    {0xD8, 2}, {0xD9, 2}, {0xDC, 2}, {0xE2, 1}, {0xEA, 2}, {0xEF, 0},
};

static unsigned send_count(const mc_pmu *p, uint8_t cmd) {
    if (p->counts) return p->counts[cmd];
    if (p->nvram && cmd == 0x33) return 3;
    if (p->nvram && cmd == 0x3B) return 2;
    for (size_t i = 0; i < sizeof SEND_FIXED / sizeof *SEND_FIXED; i++)
        if (SEND_FIXED[i][0] == cmd) return SEND_FIXED[i][1];
    return 0xFF;
}
static unsigned recv_count(const mc_pmu *p, uint8_t cmd) {
    if (p->counts) return p->counts[256 + cmd];
    if (p->nvram && cmd == 0x3B) return 2;
    for (size_t i = 0; i < sizeof RECV_FIXED / sizeof *RECV_FIXED; i++)
        if (RECV_FIXED[i][0] == cmd) return RECV_FIXED[i][1];
    return (cmd & 0x08u) ? 0xFFu : 0u;
}

static int new_framing(const mc_pmu *p) { return p->model == MC_PMU_PGE || p->model == MC_PMU_99; }
static int has_clock(const mc_pmu *p) { return p->model != MC_PMU_PB140; }
static int portable(const mc_pmu *p) { return p->model == MC_PMU_PORTABLE; }
/* Where parameter RAM starts in the M50753's RAM ($E0/$E8 address it). */
static unsigned pram_base(const mc_pmu *p) { return portable(p) ? 0x2Fu : 0x30u; }

/* --- timing ------------------------------------------------------------------------ */

#define US(n) ((uint64_t)MC_C15M_HZ * (n) / 1000000u)
#define REPLY_TIMEOUT US(5000)          /* the firmware gives up a reply byte after ~5 ms */
#define COMMAND_TIMEOUT US(1000)        /* and a command's next byte after ~45 us (more lenient here) */
#define POWER_ON_TICKS 17u              /* reset to main power on: the power key's press, debounced */
#define BOOT_TICKS 4u                   /* main power on to the main loop */
#define FIRST_SECOND 256u               /* a cold start's first second, in ticks ($25 from 0) */
#define WAKE_RESET_TICKS 4u             /* the CPU's reset pulse on wake */
#define DOWN_TICKS 5u                   /* $7E/$7F's reply to power off: the next tick and four more */
/* Asleep or off, the firmware's loop runs from the slow clock (pmu_fw.h):
 * it starts one slow timer period after power-off (16 x 255 x 11 / 32768
 * s) and takes 136 cycles of 1/8192 s a pass; a key wakes it in two passes,
 * the power key (debounced down and up) in fifteen. */
#define SLOW_LOOP_START US(1369629)
#define SLOW_PASS US(16602)
#define BUSY_TIME US(300)               /* PB 160: $07 on the port while the tick's work runs */
#define SELF_TEST US(232000)            /* $EC: the firmware's ROM checksum and RAM tests */
/* The Portable's firmware (pmu.h): main power at the first tick of its 60
 * Hz after a reset, the processor at the second; $7F's way down three
 * ticks; the processor in reset for two ticks after the wake's power; its
 * busy byte $07 on the port while a tick's work runs (the second's work
 * with its two A-D conversions), and while an ADB transaction does. */
#define PORTABLE_DOWN_TICKS 3u
#define PORTABLE_WAKE_TICKS 2u
#define PORTABLE_BUSY US(40)
#define PORTABLE_SECOND_BUSY US(230)
#define PORTABLE_SECOND_PULSE US(4)
/* The firmware's ADB timings, measured on its 740 core (test_pmu): from the
 * command's last byte to the end of the bus command's stop bit (setup,
 * attention 787 us, sync, eight 100-us bits, stop); then a Talk's answer
 * (the device's Tlt, its frame of 100-us bits, the firmware's receive
 * code), the wait for an answer that does not come, a Listen's frame; a
 * SendReset holds the line for three 256-pass delays. */
#define ADB_COMMAND US(1950)
#define ADB_TALK_GAP US(400)
#define ADB_NO_ANSWER US(350)
#define ADB_LISTEN_GAP US(550)
#define ADB_BIT US(100)
#define ADB_RESET US(4750)

enum { H_IDLE, H_ACK_DUE, H_ACKED, H_REL_DUE };
enum { DIR_RX, DIR_TX };

static void line(mc_pmu *p, int which, int state) { mc_dev_line(&p->dev, which, state); }

static void set_ack(mc_pmu *p, uint8_t level) {
    if (level != p->ack) {
        p->ack = level;
        line(p, MC_PMU_LINE_ACK, level);
    }
}
static void set_data(mc_pmu *p, int value) {
    if (value != p->data_out) {
        p->data_out = value;
        line(p, MC_PMU_LINE_DATA, value);
    }
}
static void set_power(mc_pmu *p, uint8_t power) {
    if (power != p->power) {
        p->power = power;
        line(p, MC_PMU_LINE_POWER, power);
    }
}
static void set_reset(mc_pmu *p, uint8_t reset) {
    if (reset != p->reset) {
        p->reset = reset;
        line(p, MC_PMU_LINE_RESET, reset);
    }
}

/* Protocol state kept in the struct's generic fields. */
#define HS(p) ((p)->phase & 0x0Fu)
#define DIR(p) ((p)->phase >> 4)
static void set_hs(mc_pmu *p, unsigned hs) { p->phase = (uint8_t)((p->phase & 0xF0u) | hs); }
static void set_dir(mc_pmu *p, unsigned dir) { p->phase = (uint8_t)((p->phase & 0x0Fu) | dir << 4); }

static uint64_t now_of(const mc_pmu *p) { return mc_dev_now(&p->dev); }

static void reschedule(mc_pmu *p);

/* --- the clock and parameter RAM ------------------------------------------------- */

uint32_t mc_pmu_seconds(mc_pmu *p) {
    if (p->fw) {
        mc_pmu_fw_catch_up(p->fw, now_of(p));
        return mc_pmu_fw_seconds(p->fw);
    }
    if (p->pge) return mc_pmu_pge_seconds(p->pge);
    return p->seconds + (uint32_t)((now_of(p) - p->clock_base) / MC_C15M_HZ);
}

void mc_pmu_set_seconds(mc_pmu *p, uint32_t s) {
    if (p->fw) {
        mc_pmu_fw_catch_up(p->fw, now_of(p));
        mc_pmu_fw_set_seconds(p->fw, s);
        return;
    }
    if (p->pge) {
        mc_pmu_pge_set_seconds(p->pge, s);
        return;
    }
    p->seconds = s - (uint32_t)((now_of(p) - p->clock_base) / MC_C15M_HZ);
}

unsigned mc_pmu_pram_size(const mc_pmu *p) {
    return (p->model == MC_PMU_PB140 || p->model == MC_PMU_PB160 || p->model == MC_PMU_PORTABLE) ? 128u : 256u;
}

void mc_pmu_pram_get(mc_pmu *p, uint8_t *out) {
    if (p->fw) mc_pmu_fw_pram_get(p->fw, out);
    else if (p->pge) mc_pmu_pge_pram_get(p->pge, out);
    else memcpy(out, p->pram, mc_pmu_pram_size(p));
}

void mc_pmu_pram_put(mc_pmu *p, const uint8_t *in) {
    if (p->fw) mc_pmu_fw_pram_put(p->fw, in);
    else if (p->pge) mc_pmu_pge_pram_put(p->pge, in);
    else memcpy(p->pram, in, mc_pmu_pram_size(p));
}

/* PRAM byte n as the commands see it: on the PB 160 firmware the first four
 * bytes are the clock (big-endian seconds); the M50753 PMUs keep 128. */
static uint8_t pram_read(mc_pmu *p, unsigned n) {
    if (n >= mc_pmu_pram_size(p)) return 0;      /* the firmware's RAM and registers there† */
    if (p->model == MC_PMU_PB160 && n < 4) return (uint8_t)(mc_pmu_seconds(p) >> (24 - 8 * n));
    /* The PB 160 firmware counts the power key's hold in byte 8 (its RAM
     * $38), cleared every tick while the key is up. */
    if (p->model == MC_PMU_PB160 && n == 8) return 0;
    return p->pram[n];
}

static void pram_write(mc_pmu *p, unsigned n, uint8_t v) {
    if (n >= mc_pmu_pram_size(p)) return;
    if (p->model == MC_PMU_PB160 && n < 4) {
        uint32_t s = mc_pmu_seconds(p);
        const unsigned shift = 24 - 8 * n;
        s = (s & ~(0xFFu << shift)) | (uint32_t)v << shift;
        mc_pmu_set_seconds(p, s);
    } else {
        p->pram[n] = v;
    }
    p->dirty = 1;
}

/* --- interrupts --------------------------------------------------------------------- */

static void pulse_int(mc_pmu *p) {
    if (!p->int_level) return;
    p->int_level = 0;
    line(p, MC_PMU_LINE_INT, 0);
    p->due_int = now_of(p) + MC_PMU_INT_PULSE;
    p->misc |= 0x02u;               /* firmware $00 bit 1: pulsed, not yet answered */
}

/* --- ADB --------------------------------------------------------------------------- */

static void adb_start(mc_pmu *p) {
    const uint64_t now = now_of(p);
    p->adb_flags &= 0x05u;          /* the host's bit 0 and autopoll stay */
    p->adb_busy = 1;
    p->adb_reply_len = 0;
    /* The Portable's firmware shows its busy byte through the transaction. */
    if (portable(p) && DIR(p) == DIR_RX) set_data(p, 0x07);
    if ((p->adb_cmd & 0x0Fu) == 0) {
        /* SendReset. */
        mc_adb_reset(p->bus);
        p->adb_flags |= 0x10u;
        p->adb_busy = 2;
        p->due_adb = now + ADB_RESET;
        return;
    }
    /* The devices see the command at the end of its stop bit. */
    p->due_adb = now + ADB_COMMAND;
}

/* The bus command has gone out: the devices act on it, a Service Request
 * shows in the stop bit, and the data phase follows. */
static void adb_command_sent(mc_pmu *p) {
    const uint64_t now = now_of(p);
    const unsigned kind = (p->adb_cmd >> 2) & 3u;
    uint64_t t = 0;
    if (mc_adb_srq(p->bus, (p->adb_cmd >> 4) & 15u)) p->adb_flags |= 0x08u;
    if (kind == 2) {
        mc_adb_command(p->bus, p->adb_cmd, p->adb_out, p->adb_out_len, NULL);
        t = ADB_LISTEN_GAP + (uint64_t)(2 + 8 * p->adb_out_len) * ADB_BIT;
        /* The Portable's firmware then waits for a Talk's answer too
         * ($E98B): none, its bits 7 and 4. */
        if (portable(p)) p->adb_flags |= 0x90u;
    } else {
        uint8_t out[8];
        const unsigned n = mc_adb_command(p->bus, p->adb_cmd, NULL, 0, out);
        if (kind == 3) {
            if (n) {
                memcpy(p->adb_reply, out, n);
                p->adb_reply_len = n;
                t = ADB_TALK_GAP + (uint64_t)(2 + 8 * n) * ADB_BIT;
            } else {
                p->adb_flags |= portable(p) ? 0x90u : 0xB0u;   /* no answer: the firmware's bits 7, 5, 4 (the Portable's 7, 4) */
                t = ADB_NO_ANSWER;
            }
        }
    }
    p->adb_busy = 2;
    p->due_adb = now + t;
}

static void adb_done(mc_pmu *p) {
    p->adb_busy = 0;
    p->due_adb = MC_NEVER;
    if (p->adb_map && (p->adb_flags & 0x04u) && !p->adb_reply_len) {
        /* The next device of the new framing's autopoll map. */
        unsigned a = (p->adb_cmd >> 4) & 15u;
        for (unsigned k = 0; k < 16; k++) {
            a = (a + 1u) & 15u;
            if (p->adb_map & (1u << a)) break;
        }
        p->adb_cmd = (uint8_t)(a << 4 | 0x0Cu);
        p->adb_flags &= 0xF7u;
    }
    /* With autopoll on, only data or a Service Request is reported. */
    if (!(p->adb_flags & 0x04u) || p->adb_reply_len || (p->adb_flags & 0x08u)) p->ram01 |= 0x01u;
    if (p->ram01 & 0x07u) pulse_int(p);
    if (p->model == MC_PMU_PB160 || portable(p)) set_data(p, -1);
}

/* --- replies ------------------------------------------------------------------------ */

/* Queues the reply the firmware would send (first byte, data) in the
 * machine's framing. */
static void reply(mc_pmu *p, uint8_t first, const uint8_t *data, unsigned n) {
    p->out_len = p->out_pos = 0;
    if (!new_framing(p)) {
        p->out[p->out_len++] = first;
        p->out[p->out_len++] = (uint8_t)n;
        memcpy(p->out + p->out_len, data, n);
        p->out_len += n;
    } else {
        const unsigned r = recv_count(p, p->cmd);
        if (r == 0) return;
        if (r == 0xFF) {
            p->out[p->out_len++] = (uint8_t)n;
            memcpy(p->out + p->out_len, data, n);
            p->out_len += n;
        } else if (r == 1) {
            p->out[p->out_len++] = first;
        } else {
            for (unsigned i = 0; i < r - 1; i++) p->out[p->out_len++] = i < n ? data[i] : 0;
        }
    }
}

/* PG&E downloads in 83A21950 ($11F878) and 83C54F75 ($11EB08):
 * LCD command tables at $8576/$8561. $40 converts the slider value to
 * PWM with 8-bit arithmetic, $41 sets brightness independently. $4F
 * subcommands ($CBCB/$CAF9) operate on the raw calibration and PWM. */
static void pbx_lcd_contrast(mc_pmu *p, uint8_t pwm) {
    if (pwm > p->contrast_max) pwm = p->contrast_max;
    else if (pwm < p->contrast_min) pwm = p->contrast_min;
    if (pwm == p->contrast) return;
    p->contrast = pwm;
    line(p, MC_PMU_LINE_CONTRAST, pwm);
}

static void pbx_lcd_command(mc_pmu *p, uint8_t cmd, const uint8_t *data, unsigned n) {
    uint8_t value;
    switch (cmd) {
    case 0x40:
        if (n) pbx_lcd_contrast(p, (uint8_t)(2u * data[0] + p->contrast_min));
        break;
    case 0x41:
        if (n) p->brightness = data[0];
        break;
    case 0x48:
        value = (uint8_t)(p->contrast - p->contrast_min) >> 1;
        reply(p, cmd, &value, 1);
        break;
    case 0x49:
        reply(p, cmd, &p->brightness, 1);
        break;
    case 0x4F:
        if (!n) break;
        switch (data[0]) {
        case 0:
            if (n < 3) break;
            p->contrast_max = data[1];
            p->contrast_min = data[2];
            pbx_lcd_contrast(p, p->contrast);
            reply(p, cmd, data, 0);
            break;
        case 1: {
            const uint8_t limits[] = {p->contrast_max, p->contrast_min};
            reply(p, cmd, limits, sizeof limits);
            break;
        }
        case 2:
            if (n < 2) break;
            pbx_lcd_contrast(p, data[1]);
            reply(p, cmd, data, 0);
            break;
        case 3:
            reply(p, cmd, &p->contrast, 1);
            break;
        case 5:
            reply(p, cmd, &p->lcd_aux, 1);
            break;
        default:
            break;
        }
        break;
    }
}

static void pbx_lcd_reset(mc_pmu *p) {
    p->contrast = 0x80;
    p->brightness = 0x40;
    p->contrast_max = 0xFF;
    p->contrast_min = 0;
    p->lcd_aux = 0x80;
}

/* What happens once a reply has gone out (the firmware's code after its
 * send routine returns). */
enum {
    AFTER_NONE, AFTER_BATTERY, AFTER_INT_ADB, AFTER_INT_BATTERY, AFTER_SLEEP, AFTER_SHUTDOWN, AFTER_RESET,
    AFTER_PORTABLE_INT, AFTER_PORTABLE_ADB
};

static void enter_sleep(mc_pmu *p, int shutdown) {
    p->sleep_mode = 1;
    /* The Portable's sleep loop runs at once ($F0EA). */
    p->loop_start = now_of(p) + (portable(p) ? 0 : SLOW_LOOP_START);
    set_data(p, -1);
    /* The Portable's firmware lowers P3.1 too (pmu_fw.c): the processor
     * in reset. */
    if (portable(p)) set_reset(p, 1);
    set_power(p, shutdown ? MC_PMU_POWER_OFF : MC_PMU_POWER_SLEEP);
    p->planes_saved = p->planes;
    p->planes = portable(p) ? 0x9F : 0xD7;
    line(p, MC_PMU_LINE_PLANES, p->planes);
}

/* $7F and $7E: the firmware's way down takes a few ticks. */
static void going_down(mc_pmu *p, int shutdown) {
    p->sleep_mode = shutdown ? 7 : 6;
    p->down_ticks = portable(p) ? PORTABLE_DOWN_TICKS : DOWN_TICKS;
    p->adb_flags &= 0xFDu;
}

static void after_reply(mc_pmu *p, unsigned what) {
    switch (what) {
    case AFTER_BATTERY:
        if (!(p->misc & 0x02u)) {
            p->ram01 &= 0xF9u;
            p->battery_flags &= 0xDFu;
        }
        break;
    case AFTER_INT_ADB:
        p->ram01 &= 0xFEu;
        p->misc &= 0xFDu;
        break;
    case AFTER_INT_BATTERY:
        p->ram01 &= 0xF9u;
        p->battery_flags &= 0xDFu;
        break;
    case AFTER_SLEEP: going_down(p, 0); break;
    case AFTER_SHUTDOWN: going_down(p, 1); break;
    case AFTER_RESET:
        p->planes = 0x40;
        line(p, MC_PMU_LINE_PLANES, p->planes);
        break;
    case AFTER_PORTABLE_INT:
        p->ram01 &= 0xF7u;
        p->misc &= 0xFDu;
        break;
    case AFTER_PORTABLE_ADB:
        if (!(p->misc & 0x02u)) p->ram01 &= 0xFEu;
        break;
    default: break;
    }
}

/* --- commands ----------------------------------------------------------------------- */

static uint8_t analog(const mc_pmu *p, unsigned channel) {
    switch (channel) {
    case 0: return p->slider;
    case 1: return MC_PMU_BATTERY_LEVEL;
    case 5: return MC_PMU_TEMPERATURE;
    case 7: return MC_PMU_CHARGER_SENSE;
    default: return 0x80;
    }
}

/* The Portable's firmware (pmu-portable.bin): its command table at $EE11
 * by the command's high nibble, $78 and $28 apart ($E922-$E934). A command
 * it does not know is taken with its count and nothing more. */
/* The ports $E8 reads outside parameter RAM, as the firmware leaves them
 * on a running machine (pmu_fw.c): P0 the planes, P1 ($D1: the reset and
 * the planes' bit 0 high, P1.6 high, the charger's bit 5 while charging,
 * bit 7 as $50 sets it), IN ($AF). The PowerBook 100's ROM reads IN bit 5
 * at reset (ROM 96645F9C offset $3980E: clear, a start by the charger, it
 * shows the battery charging) and P1 ($39884). Other addresses read 0†. */
static uint8_t port_portable(const mc_pmu *p, unsigned a) {
    switch (a) {
    case 0xE0: return p->planes;
    case 0xE2: return (uint8_t)(0x51u | p->p1_out | (p->charge_state == 1 || p->charge_state == 2 ? 0x20u : 0));
    case 0xEE: return p->charger_start ? 0x8F : 0xAF;
    default: return 0;
    }
}

static void execute_portable(mc_pmu *p) {
    const uint8_t c = p->cmd;
    const uint8_t *d = p->in;
    const unsigned n = p->in_len;
    const int read = (c & 0x08u) != 0;
    uint8_t r[32];
    memset(r, 0, sizeof r);
    if (c == 0x78) {
        /* $F076: the interrupt's sources ($01: bit 0 ADB data, bit 1 the
         * battery, bit 3 a cold start); then bit 3 and the pulse's flag
         * clear. */
        r[0] = p->ram01;
        reply(p, c, r, 1);
        p->pending_action = (uint8_t)AFTER_PORTABLE_INT;
        return;
    }
    if (c == 0x28) {
        /* $EE87: the ADB transaction: command, flags, count, data. */
        r[0] = p->adb_cmd;
        r[1] = p->adb_flags;
        r[2] = (uint8_t)p->adb_reply_len;
        memcpy(r + 3, p->adb_reply, p->adb_reply_len);
        reply(p, c, r, 3 + p->adb_reply_len);
        p->pending_action = (uint8_t)AFTER_PORTABLE_ADB;
        return;
    }
    switch (c >> 4) {
    case 0x1:
        /* $EE33: P0, bit 6 the plane a byte's bit 7 does not invert; the
         * read with IN.2 (high) in bit 5. */
        if (read) {
            r[0] = (uint8_t)(((p->planes & 0x7Fu) ^ 0x3Fu) | 0x20u);
            reply(p, c, r, 1);
        } else {
            for (unsigned i = 0; i < n; i++) {
                const uint8_t b = d[i];
                if (b & 0x80u) p->planes = (uint8_t)((p->planes & ~b) | (b & 0x40u));
                else p->planes = (uint8_t)((p->planes | (b & 0x7Fu)) & ((b & 0x40u) ^ 0x7Fu));
            }
            if (n) line(p, MC_PMU_LINE_PLANES, p->planes);
        }
        break;
    case 0x2:
        if (c == 0x20 && n >= 3) {
            p->adb_map = 0;
            p->adb_cmd = d[0];
            p->adb_flags = (uint8_t)(d[1] | 0x02u);
            p->adb_out_len = d[2] > 8 ? 8u : d[2];
            if (p->adb_out_len > n - 3) p->adb_out_len = n - 3;
            memcpy(p->adb_out, d + 3, p->adb_out_len);
            p->ram01 &= 0xFEu;
        } else if (c == 0x21) {
            p->adb_flags = 0;
            p->ram01 &= 0xFEu;
        }
        break;
    case 0x3:
        if (c == 0x30 && n >= 4) {
            mc_pmu_set_seconds(p, (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
            p->dirty = 1;
        } else if (c == 0x38) {
            const uint32_t s = mc_pmu_seconds(p);
            r[0] = (uint8_t)(s >> 24); r[1] = (uint8_t)(s >> 16); r[2] = (uint8_t)(s >> 8); r[3] = (uint8_t)s;
            reply(p, c, r, 4);
        } else if (c == 0x31 && n >= 20) {
            for (unsigned i = 0; i < 16; i++) pram_write(p, 0x10 + i, d[i]);
            for (unsigned i = 0; i < 4; i++) pram_write(p, 0x08 + i, d[16 + i]);
        } else if (c == 0x39) {
            for (unsigned i = 0; i < 16; i++) r[i] = pram_read(p, 0x10 + i);
            for (unsigned i = 0; i < 4; i++) r[16 + i] = pram_read(p, 0x08 + i);
            reply(p, c, r, 20);
        } else if (!read && n >= 2) {
            for (unsigned i = 0; i < d[1] && 2 + i < n; i++) pram_write(p, (uint8_t)(d[0] + i), d[2 + i]);
        } else if (read && n >= 2) {
            p->out[0] = c;
            p->out[1] = d[1];
            for (unsigned i = 0; i < d[1]; i++) p->out[2 + i] = pram_read(p, (uint8_t)(d[0] + i));
            p->out_len = 2u + d[1];
        }
        break;
    case 0x4:
        if (read) {
            r[0] = p->contrast;
            reply(p, c, r, 1);
        } else if (n >= 1) {
            p->contrast = (uint8_t)(d[0] & 0x1Fu);
            line(p, MC_PMU_LINE_CONTRAST, p->contrast);
        }
        break;
    case 0x5:
        /* $F005: bit 0 the plane P0.3 (low on), bit 1 P1.7, bit 2 wake on
         * ring; read back with P4.3, P1.6 and IN.3 (all high) in bits 3-5. */
        if (read) {
            r[0] = (uint8_t)(((p->planes & 0x08u) ? 0 : 0x01u) | ((p->p1_out & 0x80u) ? 0x02u : 0) |
                             ((p->misc & 0x80u) ? 0x04u : 0));
            reply(p, c, r, 1);
        } else if (n >= 1) {
            p->misc = (uint8_t)((p->misc & 0x7Fu) | ((d[0] & 0x04u) ? 0x80u : 0));
            p->p1_out = (d[0] & 0x02u) ? 0x80u : 0;
            p->planes = (uint8_t)((d[0] & 0x01u) ? (p->planes & ~0x08u) : (p->planes | 0x08u));
            line(p, MC_PMU_LINE_PLANES, p->planes);
        }
        break;
    case 0x6:
        if (c == 0x68 || c == 0x69) {
            r[0] = p->battery_flags; r[1] = p->battery_level; r[2] = p->temperature;
            reply(p, c, r, 3);
            p->pending_action = (uint8_t)AFTER_BATTERY;
        }
        break;
    case 0x7:
        if (c == 0x7F && n >= 4) {
            const int ok = d[0] == 'M' && d[1] == 'A' && d[2] == 'T' && d[3] == 'T';
            reply(p, ok ? 0x70 : 0xAA, r, 0);
            if (ok) p->pending_action = (uint8_t)AFTER_SLEEP;
        }
        break;
    case 0x8:
        if (c == 0x80 && n >= 4) {
            memcpy(p->wake_time, d, 4);
            p->wake_enabled = 1;
        } else if (c == 0x82) {
            p->wake_enabled = 0;
        } else {
            memcpy(r, p->wake_time, 4);
            r[4] = p->wake_enabled;
            reply(p, c, r, 5);
        }
        break;
    case 0x9:
        /* $F188: bit 0 the modem's power (P3.3); its status with IN.2 (high:
         * bit 1 clear). */
        if (read) {
            r[0] = p->modem;
            reply(p, c, r, 1);
        } else if (n >= 1) {
            const uint8_t modem = d[0] & 1u;
            if (modem != p->modem) {
                p->modem = modem;
                line(p, MC_PMU_LINE_MODEM, modem);
            }
        }
        break;
    case 0xE:
        if (c == 0xEF) {
            reply(p, c, r, 0);
            p->pending_action = (uint8_t)AFTER_RESET;
        } else if (c == 0xEC) {
            reply(p, c, r, 1);
        } else if (c == 0xEA) {
            reply(p, c, p->version, 2);
        } else if (c == 0xE0) {
            const unsigned a = n >= 2 ? (unsigned)(d[0] << 8 | d[1]) : 0, b = pram_base(p);
            for (unsigned i = 2; i < n; i++) {
                if (a + i - 2 >= b && a + i - 2 < b + mc_pmu_pram_size(p)) pram_write(p, a + i - 2 - b, d[i]);
                else if (a + i - 2 == 0xE2u) p->p1_out = d[i] & 0x80u;
            }
        } else if (n >= 3) {
            const unsigned a = (unsigned)(d[0] << 8 | d[1]), b = pram_base(p);
            for (unsigned i = 0; i < d[2] && i < sizeof r; i++)
                r[i] = (a + i >= b && a + i < b + mc_pmu_pram_size(p)) ? pram_read(p, a + i - b) : port_portable(p, a + i);
            reply(p, c, r, d[2] < sizeof r ? d[2] : (unsigned)sizeof r);
        }
        break;
    default:
        break;
    }
}

static void execute(mc_pmu *p) {
    const uint8_t c = p->cmd;
    const uint8_t *d = p->in;
    const unsigned n = p->in_len;
    uint8_t r[32];
    p->pending_action = AFTER_NONE;
    p->out_len = p->out_pos = 0;
    if (getenv("LIVADIX_PMU_LOG")) {
        fprintf(stderr, "pmu %02X:", c);
        for (unsigned k = 0; k < n; k++) fprintf(stderr, " %02X", d[k]);
        fprintf(stderr, "\n");
    }
    if (portable(p)) {
        execute_portable(p);
        return;
    }
    const unsigned group = c >> 4;
    const int read = (c & 0x08u) != 0;

    switch (group) {
    case 0x1:
        if (read) {
            r[0] = (uint8_t)(((p->planes & 0x7Fu) ^ 0x77u) & 0xDFu);   /* bit 5: IN.2 low (it is high) */
            reply(p, c, r, 1);
        } else {
            for (unsigned i = 0; i < n; i++) {
                const uint8_t b = d[i];
                if (b & 0x80u) p->planes = (uint8_t)((p->planes & ~b) | (b & 0x08u));
                else p->planes = (uint8_t)((p->planes | (b & 0x7Fu)) & ~(b & 0x08u) & 0x7Fu);
            }
            if (n) line(p, MC_PMU_LINE_PLANES, p->planes);
        }
        break;
    case 0x2:
        if (c == 0x20 && n >= 4 && new_framing(p) && d[0] == 0 && (d[1] & 0x04u)) {
            /* The new framing's autopoll: command 0 with the autopoll flag
             * and the devices' map in two bytes (the 150's, 520's and
             * 190's ROMs and the Systems send $20 00 86 00 0C: the
             * keyboard and the mouse): the PMU talks register 0 to each
             * device of the map in turn and reports the answers†. */
            p->adb_map = (uint16_t)(d[2] << 8 | d[3]);
            unsigned a = 0;
            while (a < 16 && !(p->adb_map & (1u << a))) a++;
            p->adb_cmd = (uint8_t)((a & 15u) << 4 | 0x0Cu);
            p->adb_flags = (uint8_t)(d[1] | 0x02u);
            p->adb_out_len = 0;
            p->ram01 &= 0xFEu;
        } else if (c == 0x20 && n >= 3) {
            p->adb_map = 0;
            p->adb_cmd = d[0];
            p->adb_flags = (uint8_t)(d[1] | 0x02u);
            p->adb_out_len = d[2] > 8 ? 8u : d[2];
            if (p->adb_out_len > n - 3) p->adb_out_len = n - 3;
            memcpy(p->adb_out, d + 3, p->adb_out_len);
            p->ram01 &= 0xFEu;
        } else if (c == 0x21) {
            p->adb_flags = 0;
            p->ram01 &= 0xFEu;
        }
        break;
    case 0x3:
        if (p->nvram && c == 0x33 && n == 3) {
            const unsigned address = (unsigned)d[0] << 8 | d[1];
            if (address < p->nvram_size) {
                p->nvram[address] = d[2];
                p->dirty = 1;
            }
        } else if (p->nvram && c == 0x3B && n == 2) {
            const unsigned address = (unsigned)d[0] << 8 | d[1];
            r[0] = address < p->nvram_size ? p->nvram[address] : 0xFF;
            reply(p, c, r, 1);
        } else if (c == 0x30 && has_clock(p) && n >= 4) {
            mc_pmu_set_seconds(p, (uint32_t)d[0] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[2] << 8 | d[3]);
            p->dirty = 1;
        } else if (c == 0x38 && has_clock(p)) {
            const uint32_t s = mc_pmu_seconds(p);
            r[0] = (uint8_t)(s >> 24); r[1] = (uint8_t)(s >> 16); r[2] = (uint8_t)(s >> 8); r[3] = (uint8_t)s;
            reply(p, c, r, 4);
        } else if (c == 0x31 && n >= 20) {
            for (unsigned i = 0; i < 16; i++) pram_write(p, 0x10 + i, d[i]);
            for (unsigned i = 0; i < 4; i++) pram_write(p, 0x08 + i, d[16 + i]);
        } else if (c == 0x32 && n >= 2) {
            for (unsigned i = 0; i < d[1] && 2 + i < n; i++) pram_write(p, (uint8_t)(d[0] + i), d[2 + i]);
        } else if (c == 0x39) {
            for (unsigned i = 0; i < 16; i++) r[i] = pram_read(p, 0x10 + i);
            for (unsigned i = 0; i < 4; i++) r[16 + i] = pram_read(p, 0x08 + i);
            reply(p, c, r, 20);
        } else if (c == 0x3A && n >= 2) {
            uint8_t buf[256];
            for (unsigned i = 0; i < d[1]; i++) buf[i] = pram_read(p, (uint8_t)(d[0] + i));
            p->out_len = 0;
            if (!new_framing(p)) {
                p->out[0] = c;
                p->out[1] = d[1];
                memcpy(p->out + 2, buf, d[1]);
                p->out_len = 2u + d[1];
            } else {
                p->out[0] = d[1];
                memcpy(p->out + 1, buf, d[1]);
                p->out_len = 1u + d[1];
            }
        }
        break;
    case 0x4:
        if (c == 0x4D && p->bay_wired) {
            r[0] = p->bay_id;
            reply(p, c, r, 1);
        } else if (p->pbx_lcd && (c == 0x40 || c == 0x41 || c == 0x48 ||
                                  c == 0x49 || c == 0x4F)) {
            pbx_lcd_command(p, c, d, n);
        } else if (c == 0x49) {
            r[0] = analog(p, 0);
            reply(p, c, r, 1);
        } else if (read) {
            r[0] = p->contrast;
            reply(p, c, r, 1);
        } else if (n >= 1) {
            p->contrast = (uint8_t)(d[0] & 0x1Fu);
            line(p, MC_PMU_LINE_CONTRAST, p->contrast);
        }
        break;
    case 0x5:
        if (read) {
            /* Bit 0 the misc. plane, bit 2 the wake-on-ring bit; bits 3-5
             * inputs low on a running machine (P4.3, P1.6, IN.3 high). */
            r[0] = (uint8_t)(((p->planes & 0x40u) ? 0 : 0x01u) | ((p->misc & 0x80u) ? 0x04u : 0));
            reply(p, c, r, 1);
        } else if (n >= 1) {
            p->misc = (uint8_t)((p->misc & 0x7Fu) | ((d[0] & 0x04u) ? 0x80u : 0));
            p->planes = (uint8_t)((d[0] & 0x01u) ? (p->planes & ~0x40u) : (p->planes | 0x40u));
            line(p, MC_PMU_LINE_PLANES, p->planes);
        }
        break;
    case 0x6:
        if (c == 0x68 || c == 0x69) {
            r[0] = p->battery_flags; r[1] = p->battery_level; r[2] = p->temperature;
            reply(p, c, r, 3);
            p->pending_action = (uint8_t)AFTER_BATTERY;
        } else if (c == 0x60 && n >= 2) {
            p->threshold[0] = d[0];
            p->threshold[1] = d[1];
        } else if (c == 0x6A) {
            reply(p, c, p->threshold, 2);
        } else if (c == 0x6B && new_framing(p)) {
            /* The battery in eight bytes, as the Duo ROM's PMgr reads them
             * (ROM ECFA989B offset $89C2C): the flags (bits 0, 1, 3 as
             * $68's), bit 2 set when a level follows, the level doubled in
             * a word, the temperature; the rest 0†. */
            memset(r, 0, 8);
            r[0] = (uint8_t)((p->battery_flags & 0x0Bu) | 0x04u);
            r[1] = (uint8_t)(p->battery_level >> 7);
            r[2] = (uint8_t)(p->battery_level << 1);
            r[3] = p->temperature;
            reply(p, c, r, 8);
        } else if (c == 0x6F && p->smart_battery) {
            /* A smart battery's record, as Mac OS 9.0.4 takes it on the
             * PowerBook G3 Series (with zeros its Finder announces "No
             * reserve battery power remains" and sleeps): the record's
             * format (5: 16-bit values), the flags (bit 0 the power
             * adapter, bit 2 the battery fitted), then the charge, the
             * capacity, the current and the voltage as words. The left
             * battery is fitted and full on the adapter, the right bay
             * empty†. */
            memset(r, 0, 10);
            r[0] = 5;
            r[1] = 0x01;
            if (n >= 1 && d[0] == 1) {
                r[1] = 0x05;
                r[2] = r[4] = 0x0B; r[3] = r[5] = 0xB8;    /* 3000 of 3000 */
                r[8] = 0x2E; r[9] = 0xE0;                  /* 12000 */
            }
            reply(p, c, r, 10);
        } else if (c == 0x6F && recv_count(p, c) == 0xFF) {
            /* One battery's record, as ROM B6909089's battery code reads
             * it (offset $DD96E: the battery's number sent, byte 0 nonzero
             * when it is fitted, byte 1 the flags as $68's bits 0, 1 and
             * 3, byte 3 its level): the first battery fitted and full, the
             * second absent†. */
            memset(r, 0, 16);
            if (n >= 1 && d[0] == 1) {
                r[0] = 1;
                r[1] = (uint8_t)(p->battery_flags & 0x0Bu);
                r[3] = 200;
            }
            /* Without a battery's number, one byte: ROM 4D27039C's
             * one-second VBL task (offset $7B42) asks so into a six-byte
             * buffer on its stack and reads byte 0 (0: the kind its
             * $6A/$6B path serves); a longer reply overruns that buffer
             * into the VBL dispatcher's return address. */
            reply(p, c, r, n >= 1 ? 16 : 1);
        } else if (new_framing(p) && recv_count(p, c)) {
            /* Honor the ROM's receive table, even for commands without
             * bit 3 set. The 1400's table ($3D8B6) expects two data bytes
             * for $63; with no reply, the read clocks become new commands. */
            reply(p, c, r, 0);       /* PMU99/PG&E extensions: zeros† */
            memset(p->out, 0, sizeof p->out);
        }
        break;
    case 0x7:
        if (c == 0x78) {
            if (p->ram01 & 0x01u) {
                r[0] = (uint8_t)((p->ram01 << 4) | (p->adb_flags & 0x0Fu));
                r[1] = p->adb_cmd;
                memcpy(r + 2, p->adb_reply, p->adb_reply_len);
                reply(p, c, r, 2 + p->adb_reply_len);
                p->pending_action = (uint8_t)AFTER_INT_ADB;
            } else if (p->ram01 & 0x06u) {
                r[0] = (uint8_t)(p->ram01 << 4);
                r[1] = p->battery_flags; r[2] = p->battery_level; r[3] = p->temperature;
                reply(p, c, r, 4);
                p->pending_action = (uint8_t)AFTER_INT_BATTERY;
            } else if (p->ram01 & 0x08u) {
                r[0] = 0x80;                 /* PMU99: the one-second interrupt† */
                reply(p, c, r, 1);
                p->ram01 &= 0xF7u;
            } else if (new_framing(p)) {
                /* Nothing pending: an empty status (the Duo 270c ROM polls
                 * $78 at start, ROM 0024D346 offset $897FE)†. */
                r[0] = 0;
                reply(p, c, r, 1);
            }
        } else if ((c == 0x7E || c == 0x7F) && n >= 4) {
            const int ok = d[0] == 'M' && d[1] == 'A' && d[2] == 'T' && d[3] == 'T';
            reply(p, ok ? 0x70 : 0xAA, r, 0);
            if (c == 0x7E) p->pending_action = (uint8_t)AFTER_SHUTDOWN;
            else if (ok) p->pending_action = (uint8_t)AFTER_SLEEP;
        }
        break;
    case 0x8:
        if (p->model == MC_PMU_PB140) break;
        if (c == 0x80 && n >= 4) {
            memcpy(p->wake_time, d, 4);
            p->wake_enabled = 1;
        } else if (c == 0x82) {
            p->wake_enabled = 0;
        } else if (read) {
            memcpy(r, p->wake_time, 4);
            r[4] = p->wake_enabled;
            reply(p, c, r, 5);
        }
        break;
    case 0x9:
        if (read) {
            r[0] = (uint8_t)(p->modem | 0x02u);
            reply(p, c, r, 1);
        } else if (n >= 1 && p->model != MC_PMU_PB160) {
            const uint8_t modem = d[0] & 1u;
            if (modem != p->modem) {
                p->modem = modem;
                line(p, MC_PMU_LINE_MODEM, modem);
            }
        }
        break;
    case 0xD:
        if ((c == 0xD8 || (c == 0xD9 && new_framing(p))) && n >= 1 && d[0] < 8) {
            /* $D9: the new framing's A-D read, answered as $D8† */
            r[0] = analog(p, d[0]);
            reply(p, c, r, 1);
        } else if (new_framing(p) && recv_count(p, c)) {
            reply(p, c, r, 0);       /* PMU99/PG&E extensions: zeros† */
            memset(p->out, 0, sizeof p->out);
        }
        break;
    case 0xE:
        if (c == 0xEF) {
            reply(p, c, r, 0);
            p->pending_action = (uint8_t)AFTER_RESET;
        } else if (c == 0xEC) {
            r[0] = 0;
            reply(p, c, r, 1);
        } else if (c == 0xEA) {
            reply(p, c, p->version, 2);
        } else if (c == 0xE2 && new_framing(p)) {
            /* The PG&E's boot status: $62 once its main program runs (the
             * Duo ROMs then skip the download, ROM ECFA989B offset
             * $B29EA-$B2A10; the boot ROM alone answers $94). The PowerBook
             * 150's ROM asks the PMU99 the same (ROM FDA22562 offset
             * $B29EA). */
            reply(p, 0x62, r, 0);
        } else if (c == 0xE1 && new_framing(p)) {
            /* A download: the protocol level runs its program already†. */
        } else if (c == 0xE0) {
            const unsigned a = n >= 2 ? (unsigned)(d[0] << 8 | d[1]) : 0;
            const unsigned b = pram_base(p);
            for (unsigned i = 2; i < n; i++)
                if (a + i - 2 >= b && a + i - 2 < b + mc_pmu_pram_size(p)) pram_write(p, a + i - 2 - b, d[i]);
        } else if (n >= 3) {
            /* Read PMU memory: parameter RAM where it lives, else 0†. */
            const unsigned a = (unsigned)(d[0] << 8 | d[1]), b = pram_base(p);
            for (unsigned i = 0; i < d[2] && i < sizeof r; i++)
                r[i] = (a + i >= b && a + i < b + mc_pmu_pram_size(p)) ? pram_read(p, a + i - b) : 0;
            reply(p, c, r, d[2] < sizeof r ? d[2] : (unsigned)sizeof r);
        } else if (new_framing(p) && read) {
            /* Other reads ($EB, which System 7.5.3 sends on the Duos and
             * compares with a byte it then writes with $E3): one byte, 0† */
            r[0] = 0;
            reply(p, c, r, 1);
        }
        break;
    default:
        /* Not an M50753 command: the new framing answers the table's count
         * with zeros†. */
        if (new_framing(p) && recv_count(p, c)) {
            reply(p, c, r, 0);
            for (unsigned i = 0; i < p->out_len; i++) p->out[i] = 0;
        }
        break;
    }
}


/* --- the byte handshake ------------------------------------------------------------ */

static int blocked(const mc_pmu *p) {
    return p->adb_busy || p->power != MC_PMU_POWER_ON || p->sleep_mode || (portable(p) && p->due_busy != MC_NEVER);
}

static void finish_reply(mc_pmu *p) {
    const uint8_t what = p->pending_action;
    set_dir(p, DIR_RX);
    set_hs(p, H_IDLE);
    p->out_len = p->out_pos = 0;
    p->pending_action = AFTER_NONE;
    p->due_timeout = MC_NEVER;
    if (p->iface == MC_PMU_PARALLEL) set_data(p, -1);
    after_reply(p, what);
}

static void command_done(mc_pmu *p) {
    p->rx_stage = 0;
    p->due_timeout = MC_NEVER;
    execute(p);
    p->in_len = 0;
    p->in_need = 0;
    p->ready_at = now_of(p) + (p->cmd == 0xEC ? SELF_TEST : 0);
    if (p->out_len) {
        set_dir(p, DIR_TX);
        set_hs(p, H_IDLE);
        if (p->iface == MC_PMU_PARALLEL && p->req) {
            /* The first byte goes out at once; the Mac waits for /ACK. */
            set_data(p, p->out[0]);
            set_hs(p, H_ACK_DUE);
            p->due_ack = p->ready_at + MC_PMU_NEXT_DELAY;
        }
    } else {
        const uint8_t what = p->pending_action;
        p->pending_action = AFTER_NONE;
        after_reply(p, what);
    }
    /* An ADB command runs as soon as it has been taken. */
    if (p->cmd == 0x20 && (p->adb_flags & 0x02u) && !p->adb_busy) adb_start(p);
}

static void byte_in(mc_pmu *p, uint8_t b) {
    switch (p->rx_stage) {
    case 0:
        p->cmd = b;
        p->in_len = 0;
        if (!new_framing(p) || send_count(p, b) == 0xFF) {
            p->rx_stage = 1;
        } else {
            p->in_need = send_count(p, b);
            if (!p->in_need) {
                command_done(p);
                return;
            }
            p->rx_stage = 2;
        }
        break;
    case 1:
        p->in_need = b;
        if (!b) {
            command_done(p);
            return;
        }
        p->rx_stage = 2;
        break;
    default:
        if (p->in_len < sizeof p->in) p->in[p->in_len++] = b;
        if (p->in_len >= p->in_need) {
            command_done(p);
            return;
        }
        break;
    }
    p->due_timeout = now_of(p) + COMMAND_TIMEOUT;
}

/* The Mac pulled /REQ low with a byte for the PMU (or, shift register, to
 * receive one). */
static void take_request(mc_pmu *p) {
    const uint64_t now = now_of(p);
    if (p->iface == MC_PMU_SHIFT && !p->int_level) {
        /* The interrupt's pulse ends before the PMU clocks a byte on the
         * same line (VIA1 CB1)†. */
        p->due_int = MC_NEVER;
        p->int_level = 1;
        line(p, MC_PMU_LINE_INT, 1);
    }
    if (DIR(p) == DIR_RX) {
        if (HS(p) != H_IDLE || blocked(p)) return;
        p->rx_byte = p->iface == MC_PMU_PARALLEL ? p->port_in : (p->shift ? p->shift(p->shift_ctx, 0xFF) : 0xFF);
        set_hs(p, H_ACK_DUE);
        p->due_ack = now + MC_PMU_ACK_DELAY;
    } else if (p->iface == MC_PMU_PARALLEL) {
        if (HS(p) == H_ACKED) {
            set_hs(p, H_REL_DUE);
            p->due_ack = now + MC_PMU_RELEASE_DELAY;
        }
    } else if (HS(p) == H_IDLE) {
        if (p->shift) p->shift(p->shift_ctx, p->out[p->out_pos]);
        set_hs(p, H_ACK_DUE);
        p->due_ack = (now > p->ready_at ? now : p->ready_at) + MC_PMU_ACK_DELAY;
    }
}

void mc_pmu_set_req(mc_pmu *p, int level) {
    level = level != 0;
    if (p->fw) {
        p->req = (uint8_t)level;
        mc_pmu_fw_req(p->fw, level);
        return;
    }
    if (p->pge) {
        p->req = (uint8_t)level;
        mc_pmu_pge_req(p->pge, level);
        return;
    }
    if (level == p->req) return;
    p->req = (uint8_t)level;
    const uint64_t now = now_of(p);
    if (!level) {
        take_request(p);
    } else if (DIR(p) == DIR_RX || p->iface == MC_PMU_SHIFT) {
        if (HS(p) == H_ACKED) {
            set_hs(p, H_REL_DUE);
            p->due_ack = now + MC_PMU_RELEASE_DELAY;
        } else if (p->iface == MC_PMU_SHIFT && DIR(p) == DIR_RX && HS(p) == H_ACK_DUE) {
            /* The 1400 ROM can time out and release REQ before the PMU
             * acknowledges ($4003D83E). Cancel that unaccepted byte:
             * asserting ACK after REQ has gone high strands it low. */
            set_hs(p, H_IDLE);
            p->due_ack = MC_NEVER;
        }
    } else if (HS(p) == H_IDLE && p->out_pos < p->out_len) {
        /* Parallel reply: the next byte once the Mac has raised /REQ. */
        set_data(p, p->out[p->out_pos]);
        set_hs(p, H_ACK_DUE);
        p->due_ack = now + MC_PMU_NEXT_DELAY;
    }
    reschedule(p);
}

void mc_pmu_set_port(mc_pmu *p, uint8_t value) {
    p->port_in = value;
    if (p->fw) mc_pmu_fw_port(p->fw, value);
}

void mc_pmu_set_counts(mc_pmu *p, const uint8_t *counts) { p->counts = counts; }

void mc_pmu_set_nvram(mc_pmu *p, uint8_t *data, uint32_t size) {
    p->nvram = data;
    p->nvram_size = data ? size : 0;
}
void mc_pmu_set_pbx_lcd(mc_pmu *p) { p->pbx_lcd = 1; pbx_lcd_reset(p); }
void mc_pmu_set_smart_battery(mc_pmu *p) { p->smart_battery = 1; }

void mc_pmu_set_bay(mc_pmu *p, uint8_t id) { p->bay_wired = 1; p->bay_id = id; }

void mc_pmu_set_shift(mc_pmu *p, uint8_t (*shift)(void *ctx, uint8_t in), void *ctx) {
    p->shift = shift;
    p->shift_ctx = ctx;
}

static void ack_event(mc_pmu *p) {
    const uint64_t now = now_of(p);
    p->due_ack = MC_NEVER;
    if (HS(p) == H_ACK_DUE) {
        set_ack(p, 0);
        set_hs(p, H_ACKED);
        if (DIR(p) == DIR_TX) p->due_timeout = now + REPLY_TIMEOUT;
        return;
    }
    if (HS(p) != H_REL_DUE) return;
    set_ack(p, 1);
    set_hs(p, H_IDLE);
    if (DIR(p) == DIR_RX) {
        byte_in(p, p->rx_byte);
        return;
    }
    p->due_timeout = MC_NEVER;
    if (++p->out_pos >= p->out_len) {
        finish_reply(p);
        return;
    }
    if (p->iface == MC_PMU_PARALLEL && p->req) {
        set_data(p, p->out[p->out_pos]);
        set_hs(p, H_ACK_DUE);
        p->due_ack = now + MC_PMU_NEXT_DELAY;
    } else if (p->iface == MC_PMU_SHIFT && !p->req) {
        take_request(p);
    }
}

static void timeout_event(mc_pmu *p) {
    p->due_timeout = MC_NEVER;
    if (DIR(p) == DIR_TX) {
        /* The Mac did not take the reply: the firmware gives it up. */
        set_ack(p, 1);
        p->pending_action = AFTER_NONE;
        finish_reply(p);
    } else if (p->rx_stage) {
        p->rx_stage = 0;
        p->in_len = 0;
    }
}

/* --- the tick, sleep and wake ------------------------------------------------------- */

static void one_second(mc_pmu *p) {
    if (!(p->battery_flags & 0x01u)) {
        /* The first second: the charger appears (a change: bit 5 and a
         * battery interrupt), the default thresholds load. */
        p->battery_flags |= 0x21u;
        if (p->model != MC_PMU_PB140) p->battery_flags |= 0x80u;
        p->ram01 |= 0x02u;
        p->charge_seconds = 0;
    } else if (p->charge_seconds < 0xFFFFu) {
        p->charge_seconds++;
    }
    /* The PB 160 firmware charges four minutes before it looks at the
     * battery (states 0 and 1 of its charge machine), then trickles: the
     * charging bit through second 240. */
    if (p->model != MC_PMU_PB140) {
        if (p->charge_seconds <= 240) p->battery_flags |= 0x02u;
        else p->battery_flags &= 0xFDu;
    }
    if (p->model == MC_PMU_99 && !p->sleep_mode) p->ram01 |= 0x08u;
    if (p->wake_enabled && p->sleep_mode == 1 && p->power == MC_PMU_POWER_SLEEP) {
        const uint32_t s = mc_pmu_seconds(p);
        const uint32_t w = (uint32_t)p->wake_time[0] << 24 | (uint32_t)p->wake_time[1] << 16 |
                           (uint32_t)p->wake_time[2] << 8 | p->wake_time[3];
        if (s == w) mc_pmu_wake(p);
    }
}


/* The Portable's second ($EB97, every 60 ticks): the one-second pulse to
 * the VIA's CA2, the wakeup timer, then its battery work ($EA5E) for the
 * plugged-in battery pmu.h describes: the charger connected (P1.3 low,
 * flags bit 0; a change raises bit 5 and the battery interrupt), its
 * charge machine ($EAE9: charging, bit 1, until the level reads $D0 or
 * more, then one more second). */
static void second_portable(mc_pmu *p) {
    p->second_level = 0;
    line(p, MC_PMU_LINE_SECOND, 0);
    p->due_second = now_of(p) + PORTABLE_SECOND_PULSE;
    if (p->wake_enabled && p->sleep_mode == 1 && p->power == MC_PMU_POWER_SLEEP) {
        const uint32_t s = mc_pmu_seconds(p);
        const uint32_t w = (uint32_t)p->wake_time[0] << 24 | (uint32_t)p->wake_time[1] << 16 |
                           (uint32_t)p->wake_time[2] << 8 | p->wake_time[3];
        if (s == w) {
            p->wake_enabled = 0;
            mc_pmu_wake(p);
        }
    }
    uint8_t f = p->battery_flags;
    if (!(f & 0x01u)) {
        p->ram01 |= 0x02u;
        f |= 0x20u;
    }
    f = (uint8_t)((f & ~0x59u) | 0x01u);
    switch (p->charge_state) {
    case 0:
        f = (uint8_t)((f | 0x02u) & ~0x04u);
        p->charge_state = 1;
        p->charge_count = 0x0101u;
        break;
    case 1:
        if (p->battery_level >= 0xD0u) p->charge_state = 2;
        break;
    case 2:
        if ((p->charge_count & 0xFFu) == 1u && (p->charge_count >> 8) == 1u) {
            p->charge_state = 3;
            f &= 0xFDu;
        }
        p->charge_count = (uint16_t)(p->charge_count - 0x0101u);
        break;
    default:
        break;
    }
    p->battery_flags = f;
}

/* The Portable's tick: a falling edge of the board's 60.15 Hz on INT1. */
static void tick_portable(mc_pmu *p) {
    const uint64_t now = now_of(p);
    int second = 0;
    if (p->sleep_mode == 4) {
        /* Main power at the first tick after a reset. */
        p->planes = 0x48;
        line(p, MC_PMU_LINE_PLANES, p->planes);
        set_power(p, MC_PMU_POWER_ON);
        p->sleep_mode = 5;
    } else if (p->sleep_mode == 5 || (p->sleep_mode == 3 && p->down_ticks && --p->down_ticks == 0)) {
        /* The processor at the second (after a wake, two ticks of reset). */
        set_reset(p, 0);
        p->sleep_mode = 0;
        if (!p->req) take_request(p);
    }
    if (--p->seconds_ticks == 0) {
        p->seconds_ticks = 60;
        second_portable(p);
        second = 1;
    }
    if (p->sleep_mode == 6 && --p->down_ticks == 0) enter_sleep(p, 0);
    if (p->sleep_mode || p->power != MC_PMU_POWER_ON) return;
    const int idle = DIR(p) == DIR_RX && HS(p) == H_IDLE && p->rx_stage == 0 && p->req;
    if (idle && !p->adb_busy) {
        set_data(p, 0x07);
        p->due_busy = now + (second ? PORTABLE_SECOND_BUSY : PORTABLE_BUSY);
    }
    if (p->ram01 & 0x01u) pulse_int(p);
    else if ((p->adb_flags & 0x04u) && !p->adb_busy && idle) adb_start(p);
    else if (p->ram01 & 0x07u) pulse_int(p);
}

static void tick(mc_pmu *p) {
    if (portable(p)) {
        tick_portable(p);
        return;
    }
    const uint64_t now = now_of(p);
    p->due_tick = p->model == MC_PMU_PORTABLE ? MC_NEVER : now + MC_PMU_TICK;
    if (p->sleep_mode >= 4) {
        /* Not counting seconds before the main loop. */
    } else if (p->seconds_ticks == 0 || --p->seconds_ticks == 0) {
        p->seconds_ticks = p->sleep_mode ? 60u : MC_PMU_TICK_HZ;
        one_second(p);
    }
    if (p->reset && p->sleep_mode == 4) set_reset(p, 0);      /* the CPU's reset released before power */
    if ((p->sleep_mode == 6 || p->sleep_mode == 7) && --p->down_ticks == 0) enter_sleep(p, p->sleep_mode == 7);
    if (p->sleep_mode || p->power != MC_PMU_POWER_ON) return;
    const int idle = DIR(p) == DIR_RX && HS(p) == H_IDLE && p->rx_stage == 0 && p->req;
    if (p->model == MC_PMU_PB160 && idle && !p->adb_busy) {
        set_data(p, 0x07);
        p->due_busy = now + BUSY_TIME;
    }
    if (p->ram01 & 0x01u) pulse_int(p);
    else if ((p->adb_flags & 0x04u) && !p->adb_busy && idle) adb_start(p);
    else if (p->ram01 & 0x0Fu) pulse_int(p);
}

int mc_pmu_asleep(const mc_pmu *p) { return p->power != MC_PMU_POWER_ON; }

/* The Portable's reset button (pmu.h): at the protocol level the press
 * holds the processor in reset and drops the transfer under way; at the
 * release the firmware's main loop starts again and the processor goes at
 * its next tick (as after a power-up, tick_portable). */
int mc_pmu_reset_switch(mc_pmu *p, int pressed) {
    if (p->model != MC_PMU_PORTABLE) return 0;
    if (p->fw) {
        mc_pmu_fw_reset_switch(p->fw, pressed);
        return 1;
    }
    if (p->power != MC_PMU_POWER_ON || p->sleep_mode == 1) return 1;
    if (pressed) {
        set_reset(p, 1);
        p->sleep_mode = 8;              /* waits for the release */
        p->phase = 0;
        p->rx_stage = 0;
        p->in_len = p->in_need = 0;
        p->out_len = p->out_pos = 0;
        p->pending_action = AFTER_NONE;
        p->adb_busy = 0;
        p->due_ack = p->due_adb = p->due_busy = p->due_timeout = MC_NEVER;
        set_data(p, -1);
        if (!p->ack) { p->ack = 1; line(p, MC_PMU_LINE_ACK, 1); }
    } else if (p->sleep_mode == 8) {
        p->sleep_mode = 5;
    }
    reschedule(p);
    return 1;
}

int mc_pmu_wake(mc_pmu *p) {
    if (p->fw) return mc_pmu_fw_wake(p->fw);
    if (p->pge) return 0;           /* the PG&E's own keyboard scan sees the power key (pmu_pge.c) */
    if (p->sleep_mode != 1) return 0;
    /* The slow loop sees a key (asleep) in two passes, the power key (off)
     * in fifteen; then, a key released three ticks later, the CPU is
     * powered at a tick, in reset for four. */
    const uint64_t now = now_of(p);
    const uint64_t base = now > p->loop_start ? now : p->loop_start;
    uint64_t t = p->power == MC_PMU_POWER_SLEEP ? base + 2u * SLOW_PASS + 3u * MC_PMU_TICK : base + 15u * SLOW_PASS;
    uint64_t tick_at = p->due_tick;
    /* The Portable's firmware restores the planes once the loop has seen
     * the key ($F10F) and lets the processor go once it is up (pmu_fw.c:
     * three ticks of its own later) and two of the 60 Hz have passed. */
    if (portable(p)) tick_at = t = base + SLOW_PASS / 2u;
    while (tick_at < t) tick_at += MC_PMU_TICK;
    p->sleep_mode = 2;
    p->due_power = tick_at;
    reschedule(p);
    return 1;
}

static void power_event(mc_pmu *p) {
    const uint64_t now = now_of(p);
    p->due_power = MC_NEVER;
    switch (p->sleep_mode) {
    case 2:
        /* Waking: the CPU powered, in reset. */
        p->planes = (uint8_t)(p->planes_saved & 0x7Fu);
        line(p, MC_PMU_LINE_PLANES, p->planes);
        set_reset(p, 1);
        set_power(p, MC_PMU_POWER_ON);
        p->sleep_mode = 3;
        p->down_ticks = 0;
        p->due_power = portable(p) ? now + 3u * MC_PMU_TICK : now + WAKE_RESET_TICKS * MC_PMU_TICK;
        break;
    case 3:
        if (portable(p)) {
            /* The key is up: two ticks more (tick_portable). */
            p->down_ticks = PORTABLE_WAKE_TICKS;
            break;
        }
        set_reset(p, 0);
        p->sleep_mode = 0;
        if (!p->req) take_request(p);
        break;
    case 4:
        /* Power-up: main power on; the firmware enters its main loop four
         * ticks later. */
        p->planes = 0x40;
        line(p, MC_PMU_LINE_PLANES, p->planes);
        set_power(p, MC_PMU_POWER_ON);
        p->sleep_mode = 5;
        /* The PG&E answers from the processor's start: the Duo ROMs send
         * their first command within a millisecond of it (ROM ECFA989B
         * offset $476C6), before the M50753's four ticks† (pmu.h); the
         * PMU99 too (the PowerBook 150's ROM FDA22562, offset $476C6). */
        p->due_power = new_framing(p) ? now : now + BOOT_TICKS * MC_PMU_TICK;
        break;
    case 5:
        p->sleep_mode = 0;
        p->seconds_ticks = FIRST_SECOND;
        if (!p->req) take_request(p);
        break;
    default:
        break;
    }
}

/* --- the device --------------------------------------------------------------------- */

static void reschedule(mc_pmu *p) {
    if (p->level == MC_PMU_FIRMWARE) return;
    uint64_t t = p->due_tick;
    const uint64_t due[] = {p->due_ack, p->due_int, p->due_adb, p->due_busy, p->due_timeout, p->due_power, p->due_second};
    for (size_t i = 0; i < sizeof due / sizeof *due; i++)
        if (due[i] < t) t = due[i];
    mc_dev_schedule(&p->dev, t);
}

static void pmu_event(void *dev, uint64_t now) {
    mc_pmu *p = dev;
    if (p->fw) {
        mc_pmu_fw_event(p->fw, now);
        return;
    }
    if (p->pge) {
        mc_pmu_pge_event(p->pge, now);
        return;
    }
    if (p->due_int <= now) {
        p->due_int = MC_NEVER;
        p->int_level = 1;
        line(p, MC_PMU_LINE_INT, 1);
    }
    if (p->due_busy <= now) {
        p->due_busy = MC_NEVER;
        if (DIR(p) == DIR_RX && !(portable(p) && p->adb_busy)) set_data(p, -1);
        if (portable(p) && !p->req) take_request(p);
    }
    if (p->due_second <= now) {
        p->due_second = MC_NEVER;
        p->second_level = 1;
        line(p, MC_PMU_LINE_SECOND, 1);
    }
    if (p->due_power <= now) power_event(p);
    if (p->due_ack <= now) ack_event(p);
    if (p->due_timeout <= now) timeout_event(p);
    if (p->due_adb <= now) {
        if (p->adb_busy == 1) {
            adb_command_sent(p);
        } else {
            adb_done(p);
            if (!p->req) take_request(p);
        }
    }
    if (p->due_tick <= now) tick(p);
    reschedule(p);
}

static void pmu_reset(void *dev) {
    mc_pmu *p = dev;
    const uint64_t now = now_of(p);
    p->req = 1;
    p->ack = 1;
    p->int_level = 1;
    p->data_out = -1;
    p->phase = 0;
    p->rx_stage = 0;
    p->in_len = p->in_need = 0;
    p->out_len = p->out_pos = 0;
    p->pending_action = AFTER_NONE;
    p->ram01 = 0;
    p->adb_flags = 0;
    p->adb_busy = 0;
    p->misc = 0;
    p->sleep_mode = 0;
    p->due_ack = p->due_int = p->due_adb = p->due_busy = p->due_timeout = p->due_second = MC_NEVER;
    if (p->fw) {
        p->power = MC_PMU_POWER_OFF;
        p->reset = 0;
        mc_pmu_fw_reset(p->fw);
        return;
    }
    if (p->pge) {
        /* The PG&E's boot ROM turns the main power on (pmu_pge.h). */
        p->power = MC_PMU_POWER_OFF;
        p->reset = 0;
        mc_pmu_pge_reset(p->pge);
        return;
    }
    p->planes = 0xD7;
    p->contrast = 0;
    if (p->pbx_lcd) pbx_lcd_reset(p);
    p->battery_flags = 0;
    p->battery_level = MC_PMU_BATTERY_LEVEL;
    p->temperature = MC_PMU_TEMPERATURE;
    /* A cold start's thresholds, as each firmware sets them for the charger
     * sense of pmu.h. */
    p->threshold[0] = p->model == MC_PMU_PB160 ? 0x4E : 0x49;
    p->threshold[1] = 0x30;
    p->seconds_ticks = 0;
    if (p->model == MC_PMU_PB160) p->pram[9] = 0x19;    /* the firmware's hold limit in byte 9 ($39) */
    p->sleep_mode = 4;
    p->power = MC_PMU_POWER_OFF;
    p->reset = 1;
    line(p, MC_PMU_LINE_POWER, p->power);
    line(p, MC_PMU_LINE_RESET, p->reset);
    p->due_tick = portable(p) ? MC_NEVER : now + MC_PMU_TICK;
    p->due_power = portable(p) ? MC_NEVER : now + POWER_ON_TICKS * MC_PMU_TICK;
    if (portable(p)) {
        /* Its first second 256 ticks on ($24 from 0); the cold start's
         * flag in the interrupt's sources ($01 = 8, $E8A0). */
        p->seconds_ticks = FIRST_SECOND;
        p->ram01 = 0x08;
        /* The contrast its firmware drives from power-up (its PWM's
         * timer 1 latch: pmu_fw.c reports 4). */
        p->contrast = 4;
        line(p, MC_PMU_LINE_CONTRAST, p->contrast);
        p->charge_state = 0;
        p->p1_out = 0x80;
        p->planes = 0x9F;
        p->second_level = 1;
        p->due_second = MC_NEVER;
    }
    reschedule(p);
}

const mc_dev_ops mc_pmu_ops = {NULL, NULL, pmu_event, pmu_reset, NULL};

void mc_pmu_init(mc_pmu *p, const mc_hw_host *host, const char *name, unsigned model, unsigned iface,
                 mc_adb_bus *bus, const uint8_t *firmware, size_t firmware_size) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_pmu_ops;
    p->dev.host = host;
    p->dev.name = name;
    p->model = (uint8_t)model;
    p->iface = (uint8_t)iface;
    p->bus = bus;
    p->req = p->ack = p->int_level = 1;
    p->data_out = -1;
    p->slider = 0x80;
    p->clock_base = mc_dev_now(&p->dev);
    p->due_tick = p->due_ack = p->due_int = p->due_adb = p->due_busy = p->due_timeout = p->due_power = p->due_second = MC_NEVER;
    p->tick_in = p->second_level = 1;
    /* The firmware's version bytes ($E802-$E803 of each dump); the later
     * PMUs' are not known†. */
    if (model == MC_PMU_PB140) { p->version[0] = 0x02; p->version[1] = 0xB3; }
    else if (model == MC_PMU_PB160) { p->version[0] = 0x03; p->version[1] = 0xA2; }
    else if (model == MC_PMU_PORTABLE) { p->version[0] = 0x02; p->version[1] = 0xB5; }
    p->level = MC_PMU_PROTOCOL;
    if (firmware) mc_pmu_use_firmware(p, firmware, firmware_size);
}

void mc_pmu_free(mc_pmu *p) {
    if (p->fw) mc_pmu_fw_free(p->fw);
    p->fw = NULL;
    if (p->pge) mc_pmu_pge_free(p->pge);
    p->pge = NULL;
}

/* --- firmware ------------------------------------------------------------------- */

void mc_pmu_set_tick(mc_pmu *p, int level) {
    level = level != 0;
    if (level == p->tick_in) return;
    p->tick_in = (uint8_t)level;
    if (p->fw) {
        mc_pmu_fw_tick(p->fw, level);
        return;
    }
    if (p->model != MC_PMU_PORTABLE || p->pge || level) return;
    tick(p);
    reschedule(p);
}

void mc_pmu_set_slider(mc_pmu *p, uint8_t value) {
    p->slider = value;
    if (p->fw) p->fw->chip.analog[0] = value;
}

int mc_pmu_use_firmware(mc_pmu *p, const uint8_t *image, size_t size) {
    const mc_firmware *f = mc_firmware_identify(image, size);
    static const char *const FILES[] = {"pmu-pb140.bin", "pmu-pb160.bin", "pmu-pge-boot.bin", NULL, "pmu-portable.bin"};
    if (!f || p->model > MC_PMU_PORTABLE || !FILES[p->model] || strcmp(f->file, FILES[p->model])) return 0;
    if (p->model == MC_PMU_PGE) {
        if (!p->pge) p->pge = mc_pmu_pge_new(p, image);
        if (!p->pge) return 0;
    } else {
        if (!p->fw) p->fw = mc_pmu_fw_new(p, image);
        if (!p->fw) return 0;
    }
    p->level = MC_PMU_FIRMWARE;
    return 1;
}

int mc_pmu_input_key(mc_pmu *p, uint8_t code, int down) {
    return p->pge ? mc_pmu_pge_key(p->pge, code, down) : 0;
}

int mc_pmu_input_mouse(mc_pmu *p, int dx, int dy, int button) {
    if (!p->pge) return 0;
    mc_pmu_pge_mouse(p->pge, dx, dy, button);
    return 1;
}

void mc_pmu_pge_program(mc_pmu *p, const uint8_t *image, size_t size) {
    if (p->pge) mc_pmu_pge_load(p->pge, image, size);
}

int mc_pmu_select(mc_pmu *p, unsigned level, const mc_rom_index *roms, const char *const *keys) {
    if (level == MC_CONTROLLER_PROTOCOL || !roms || !keys) return level == MC_CONTROLLER_FIRMWARE ? -1 : 0;
    for (; *keys; keys++) {
        const mc_rom_file *file = mc_rom_index_firmware(roms, mc_firmware_find(*keys));
        FILE *f = file ? fopen(file->path, "rb") : NULL;
        if (!f) continue;
        uint8_t image[MC_M50753_ROM_SIZE + 1];
        const size_t n = fread(image, 1, sizeof image, f);
        fclose(f);
        if (mc_pmu_use_firmware(p, image, n)) return 1;
    }
    return level == MC_CONTROLLER_FIRMWARE ? -1 : 0;
}

/* --- persistence --------------------------------------------------------------------- */

static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

int mc_pmu_nvram_save(mc_pmu *p, const char *path, int64_t host_seconds) {
    uint8_t buf[8 + 1 + 8 + 256];
    memset(buf, 0, sizeof buf);
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t offset = (int64_t)mc_pmu_seconds(p) - host_seconds;
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    mc_pmu_pram_get(p, buf + 17);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int ok = fwrite(buf, 1, sizeof buf, f) == sizeof buf;
    fclose(f);
    if (ok) p->dirty = 0;
    return ok;
}

int mc_pmu_nvram_load(mc_pmu *p, const char *path, int64_t host_seconds) {
    uint8_t buf[8 + 1 + 8 + 256];
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    const int ok = fread(buf, 1, sizeof buf, f) == sizeof buf && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1;
    fclose(f);
    if (!ok) return 0;
    int64_t offset = 0;
    offset = mc_nvram_clock_offset(buf + 9);
    mc_pmu_pram_put(p, buf + 17);
    mc_pmu_set_seconds(p, (uint32_t)(host_seconds + offset));
    return 1;
}
