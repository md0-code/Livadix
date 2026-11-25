/* Livadix — the M0110 and M0110A keyboards (m0110.h). Livadix's own code. */
#include "m0110.h"

#include <string.h>

/* Times in C15M periods. The keyboard clocks the Macintosh's command out
 * at about 330 us a bit and its reply in at about 160 us a bit (Guide,
 * chapter 14); the reply follows the command after a short pause, and
 * Inquiry waits a quarter of a second for a key before it answers Null. */
#define US(n) ((uint64_t)(n) * MC_C15M_HZ / 1000000u)
#define COMMAND_TIME US(2640)
#define REPLY_TIME US(1300)
#define RETRY_TIME US(500)
#define INQUIRY_TIME US(250000)
#define GIVE_UP US(1000000)

enum { IDLE, SEND, WAIT, REPLY };

static void arm(mc_m0110 *k, int state, uint64_t when) {
    k->state = (uint8_t)state;
    mc_dev_schedule(&k->dev, when);
}

static int pop(mc_m0110 *k, uint8_t *byte) {
    if (!k->count) return 0;
    *byte = k->queue[k->head];
    k->head = (k->head + 1) % MC_M0110_QUEUE;
    k->count--;
    return 1;
}

static void reply(mc_m0110 *k, uint8_t byte, uint64_t now) {
    k->reply = byte;
    k->inquiry_end = now + GIVE_UP;     /* reused: the time the reply is dropped */
    arm(k, REPLY, now + REPLY_TIME);
}

uint8_t mc_m0110_model_byte(const mc_m0110 *k) {
    /* Model 1, the M0110; the M0110A's 5 (†: the model number of the
     * M0110A is not in the sources at hand). */
    return k->model == MC_M0110A ? 0x0B : 0x03;
}

static void command(mc_m0110 *k, uint8_t c, uint64_t now) {
    uint8_t byte;
    k->last_command = c;
    switch (c) {
    case MC_M0110_INQUIRY:
        if (pop(k, &byte)) reply(k, byte, now);
        else {
            k->inquiry_end = now + INQUIRY_TIME;
            arm(k, WAIT, k->inquiry_end);
        }
        break;
    case MC_M0110_INSTANT:
        reply(k, pop(k, &byte) ? byte : MC_M0110_NULL, now);
        break;
    case MC_M0110_MODEL:
        /* The keyboard resets: transitions not yet sent are lost. */
        k->head = k->count = 0;
        reply(k, mc_m0110_model_byte(k), now);
        break;
    case MC_M0110_TEST:
        reply(k, MC_M0110_ACK, now);
        break;
    default:
        reply(k, MC_M0110_NAK, now);
        break;
    }
}

static void m0110_event(void *dev, uint64_t now) {
    mc_m0110 *k = dev;
    const unsigned mode = mc_via_sr_mode(k->via);
    switch (k->state) {
    case SEND:
        k->state = IDLE;
        /* The command is shifted out only while the VIA still shifts out
         * under the external clock. */
        if (mode == 7) command(k, mc_via_shift8(k->via, 0xFF), now);
        break;
    case WAIT:
        reply(k, MC_M0110_NULL, now);
        break;
    case REPLY:
        if (mode == 3) {
            k->state = IDLE;
            mc_via_shift8(k->via, k->reply);
        } else if (now < k->inquiry_end) {
            arm(k, REPLY, now + RETRY_TIME);
        } else {
            k->state = IDLE;
        }
        break;
    default:
        break;
    }
}

static void m0110_reset(void *dev) {
    mc_m0110 *k = dev;
    k->head = k->count = 0;
    k->state = IDLE;
    k->shift = 0;
    mc_dev_schedule(&k->dev, MC_NEVER);
}

const mc_dev_ops mc_m0110_ops = {NULL, NULL, m0110_event, m0110_reset, NULL};

void mc_m0110_init(mc_m0110 *k, const mc_hw_host *host, const char *name, mc_via *via, int model) {
    memset(k, 0, sizeof *k);
    k->dev.ops = &mc_m0110_ops;
    k->dev.host = host;
    k->dev.name = name;
    k->via = via;
    k->model = (uint8_t)model;
}

void mc_m0110_sr_written(mc_m0110 *k) {
    if (mc_via_sr_mode(k->via) != 7) return;
    arm(k, SEND, mc_dev_now(&k->dev) + COMMAND_TIME);
}

static int push(mc_m0110 *k, const uint8_t *bytes, unsigned n) {
    if (k->count + n > MC_M0110_QUEUE) return 0;
    for (unsigned i = 0; i < n; i++) k->queue[(k->head + k->count++) % MC_M0110_QUEUE] = bytes[i];
    /* An Inquiry waiting for a key answers now. */
    if (k->state == WAIT) {
        uint8_t byte;
        pop(k, &byte);
        reply(k, byte, mc_dev_now(&k->dev));
    }
    return 1;
}

/* A key code's transition byte (Guide, chapter 14): the code shifted left,
 * bit 0 set, bit 7 for up. */
static uint8_t transition(unsigned code, int down) { return (uint8_t)(((code & 0x3Fu) << 1) | 1u | (down ? 0u : 0x80u)); }

int mc_m0110_key(mc_m0110 *k, uint8_t adb, int down) {
    uint8_t seq[6];
    unsigned n = 0;
    /* The ADB keyboard's right-hand modifiers and Control. */
    if (adb == 0x7B) adb = 0x38;
    else if (adb == 0x7C) adb = 0x3A;
    else if (adb == 0x36 || adb == 0x7D) adb = 0x37;
    if (adb == 0x38) k->shift = (uint8_t)(down != 0);
    if (adb <= 0x3A && adb != 0x34) {
        seq[n++] = transition(adb, down);
        return push(k, seq, n);
    }
    if (k->model != MC_M0110A) return 1;    /* the M0110 has no arrows or keypad */
    /* The M0110A's keypad and arrows (key codes $40-$7F, Inside Macintosh
     * III, "Keyboard"), after the prefix: its arrows have the codes $46,
     * $42, $48, $4D; its *, /, = and + are Shift with the arrows' codes. */
    unsigned code = 0;
    int shifted = 0;
    switch (adb) {
    case 0x3B: code = 0x46; break;          /* left */
    case 0x3C: code = 0x42; break;          /* right */
    case 0x3D: code = 0x48; break;          /* down */
    case 0x3E: code = 0x4D; break;          /* up */
    case 0x43: code = 0x42; shifted = 1; break;     /* * */
    case 0x45: code = 0x46; shifted = 1; break;     /* + */
    case 0x4B: code = 0x4D; shifted = 1; break;     /* / */
    case 0x51: code = 0x48; shifted = 1; break;     /* = */
    case 0x41: case 0x47: case 0x4C: case 0x4E:     /* . clear enter - */
    case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57: case 0x58: case 0x59: case 0x5B: case 0x5C:
        code = adb;
        break;
    default:
        return 1;                           /* not on this keyboard */
    }
    const int add_shift = shifted && !k->shift && down;
    if (add_shift) seq[n++] = transition(0x38, 1);
    seq[n++] = MC_M0110_KEYPAD;
    seq[n++] = transition(code - 0x40u, down);
    if (add_shift) seq[n++] = transition(0x38, 0);
    return push(k, seq, n);
}
