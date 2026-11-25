/* Livadix — the real-time clock chip (see rtc.h for sources). Livadix's own code. */
#include "rtc.h"

#include <string.h>

#define HALF_SECOND (MC_C15M_HZ / 2u)

static void drive(mc_rtc *r, uint8_t level) {
    if (level != r->data_out) {
        r->data_out = level;
        mc_dev_line(&r->dev, MC_RTC_LINE_DATA, level);
    }
}

uint32_t mc_rtc_seconds(const mc_rtc *r) {
    const uint64_t now = mc_dev_now(&r->dev);
    return r->seconds + (uint32_t)((now - r->base) / MC_C15M_HZ);
}

/* The next edge of the one-second signal: low at each whole second, high
 * half a second later. */
static void arm(mc_rtc *r) {
    const uint64_t now = mc_dev_now(&r->dev);
    const uint64_t into = (now - r->base) % MC_C15M_HZ;
    const uint64_t start = now - into;
    const uint64_t next = into < HALF_SECOND ? start + HALF_SECOND : start + MC_C15M_HZ;
    mc_dev_schedule(&r->dev, next);
}

void mc_rtc_set_seconds(mc_rtc *r, uint32_t seconds) {
    const uint64_t now = mc_dev_now(&r->dev);
    /* Keep the phase of the second: the counter changes, its ticks do not. */
    r->seconds = seconds - (uint32_t)((now - r->base) / MC_C15M_HZ);
}

static void rtc_event(void *dev, uint64_t now) {
    mc_rtc *r = dev;
    const uint64_t into = (now - r->base) % MC_C15M_HZ;
    const uint8_t level = into >= HALF_SECOND;
    if (level != r->second_level) {
        r->second_level = level;
        mc_dev_line(&r->dev, MC_RTC_LINE_SECOND, level);
    }
    arm(r);
}

static uint8_t read_register(mc_rtc *r, uint16_t address) {
    if (address < 0x100) return r->pram[address];
    if (address < 0x104) return (uint8_t)(mc_rtc_seconds(r) >> (8u * (address - 0x100u)));
    return 0;   /* the test and write-protect registers are write-only */
}

static void write_register(mc_rtc *r, uint16_t address, uint8_t value) {
    if (address == 0x201) {
        r->wp = value;
        return;
    }
    if (r->wp & 0x80u) return;
    if (address < 0x100) {
        if (r->pram[address] != value) r->dirty = 1;
        r->pram[address] = value;
    } else if (address < 0x104) {
        const unsigned shift = 8u * (address - 0x100u);
        uint32_t s = mc_rtc_seconds(r);
        s = (s & ~(0xFFu << shift)) | (uint32_t)value << shift;
        mc_rtc_set_seconds(r, s);
        r->dirty = 1;
    } else if (address == 0x200) {
        r->test = value;
    }
}

/* Decodes the command byte (Inside Macintosh III-38); returns 0 for an
 * extended command still waiting for its address byte, -1 for an unknown
 * one. */
static int decode(mc_rtc *r, uint8_t cmd) {
    const uint8_t c = cmd & 0x7Fu;
    /* Seconds: z00xaa01, bit 4 ignored (the ROM reads the clock through
     * both forms and compares them). */
    if ((c & 0x63u) == 0x01u) r->address = (uint16_t)(0x100u + ((c >> 2) & 3u));
    else if (c == 0x31u) r->address = 0x200;
    else if (c == 0x35u) r->address = 0x201;
    else if ((c & 0x73u) == 0x21u) r->address = (uint16_t)(0x08u + ((c >> 2) & 3u));
    else if ((c & 0x43u) == 0x41u) r->address = (uint16_t)(0x10u + ((c >> 2) & 15u));
    else if ((c & 0x78u) == 0x38u) return 0;
    else return -1;
    return 1;
}

static void start_out(mc_rtc *r) {
    r->out = read_register(r, r->address);
    r->out_bits = 0;
    r->phase = 3;
    drive(r, (uint8_t)(r->out >> 7));
}

static void byte_in(mc_rtc *r, uint8_t byte) {
    switch (r->phase) {
    case 0: {
        r->cmd = byte;
        const int d = decode(r, byte);
        if (d < 0) r->phase = 4;
        else if (d == 0) r->phase = 1;
        else if (byte & 0x80u) start_out(r);
        else r->phase = 2;
        break;
    }
    case 1:
        r->address = (uint16_t)(((r->cmd & 7u) << 5) | ((byte >> 2) & 0x1Fu));
        if (r->cmd & 0x80u) start_out(r);
        else r->phase = 2;
        break;
    case 2:
        write_register(r, r->address, byte);
        r->phase = 4;
        break;
    default: break;
    }
}

void mc_rtc_pins(mc_rtc *r, int enb, int clk, int data) {
    enb = enb != 0;
    clk = clk != 0;
    if (enb) {
        /* Enable high ends (or aborts) the transaction. */
        r->enb = 1;
        r->clk = (uint8_t)clk;
        r->phase = 0;
        r->bits = 0;
        drive(r, 1);
        return;
    }
    if (r->enb) {
        r->enb = 0;
        r->phase = 0;
        r->bits = 0;
        r->shift = 0;
    }
    const uint8_t old = r->clk;
    r->clk = (uint8_t)clk;
    if (old == clk) return;
    if (clk) {
        /* Rising edge: the processor's bit is taken. */
        if (r->phase == 3) return;
        r->shift = (uint8_t)(r->shift << 1 | (data ? 1u : 0u));
        if (++r->bits == 8) {
            r->bits = 0;
            byte_in(r, r->shift);
        }
    } else if (r->phase == 3) {
        /* Falling edge while sending: the next bit appears. The first falling
         * edge presents bit 7 (already driven when the command ended). */
        if (r->out_bits < 8) {
            drive(r, (uint8_t)(r->out >> (7u - r->out_bits) & 1u));
            r->out_bits++;
        } else {
            r->phase = 4;
            drive(r, 1);
        }
    }
}

static void rtc_reset(void *dev) {
    mc_rtc *r = dev;
    r->phase = 0;
    r->bits = 0;
    r->enb = 1;
    drive(r, 1);
    arm(r);
}

const mc_dev_ops mc_rtc_ops = {NULL, NULL, rtc_event, rtc_reset};

void mc_rtc_init(mc_rtc *r, const mc_hw_host *host, const char *name) {
    memset(r, 0, sizeof *r);
    r->dev.ops = &mc_rtc_ops;
    r->dev.host = host;
    r->dev.name = name;
    r->wp = 0x80u;
    r->enb = 1;
    r->data_out = 1;
    r->second_level = 1;
}
