/* Livadix — the PMU at firmware level (see pmu_fw.h, pmu.h). Livadix's own code.
 *
 * The board around the M50753, as both firmwares drive it (observed in their code; the pins the code only reads are wired to
 * the levels a running PowerBook on its adapter shows†):
 *   P0 ($E0, outputs): the power planes; $D7 everything off, $40 running;
 *      bit 7 set is main power off (the sleep and shutdown paths set it).
 *   P1 ($E2): bit 4 out the CPU's /RESET (low at power-on, raised once the
 *      planes are on, pulsed on wake); bit 0 out the contrast PWM enable;
 *      bit 1 in, low while a key or the mouse asks to wake (the sleep loop
 *      wakes on its release)†; bit 3 in the charger (1 present)†; bit 6
 *      in†, high.
 *   P2 ($E4): the data bus to the VIA's port A.
 *   P3 ($E8): bit 7 in /REQ; bit 6 out /ACK; bit 5 out /INT (a pulse);
 *      bit 3 out modem power; bit 2 in, low on a machine that is on†.
 *   P4 ($EA): bit 0 out pulls the ADB line low; bit 1 in the ADB line.
 *   IN ($EE): bit 4 the power key (low pressed); bits 2, 3 high, bit 6 low
 *      (the battery table the firmware selects)†.
 *   A-D: channel 1 the battery, 5 the temperature, 7 the charger sense
 *      (pmu.h's MC_PMU_* values), the others mid-scale†.
 * Sleep and shutdown both turn main power off; the firmware's RAM $2E
 * ($AA when it goes to sleep) tells them apart†.
 *
 * The Macintosh Portable's firmware (pmu-portable.bin; MC_PMU_PORTABLE)
 * drives the same handshake, data, /INT, modem, ADB and reset pins and P0
 * the same way (bit 7 main power off, $9F asleep), with these differences
 * (its code; † the levels a running Portable on its adapter shows):
 *   INT1: the Miscellaneous GLU's 60.15 Hz (Guide, chapter 3), polled
 *      ($00FE bit 7) as the firmware's tick; 60 of them a second.
 *   P1: bit 1 in, high while a key asks to wake (the sleep loop wakes on
 *      its press and powers up on its release, $F0EA-$F127); bit 3 in, low
 *      with the charger connected (its $1D bit 0, $EA83)†; bit 6 in, low
 *      on a modem's ring†; bit 4 out the CPU's /RESET.
 *   P3: bit 1 out, the CPU running: the firmware raises it as it enters
 *      its main loop (two ticks after main power, $E905) and lowers it on
 *      the way to sleep ($F0DF) and on INT2 ($F326: the programmer's reset
 *      button, mc_pmu_fw_reset_switch), so the processor waits
 *      for it as for P1.4 (the ROM's first command comes some 20 us after
 *      the processor starts, ROM offset $B34, and the firmware only listens
 *      in its main loop)†; bit 4 out the one-second pulse to the VIA's CA2
 *      (Guide, Table 4-19; $EBAB); bit 2 in, low while the machine may run (a cold reset
 *      with it high goes to sleep, $E8F3)†; bit 0 in, the reset button,
 *      low while pressed (INT2's handler waits for it high, $F336).
 *   A-D: channel 1 the battery, channel 0 the third byte of $68.
 *   No power key and no shutdown: main power off is always sleep.
 *   Parameter RAM is RAM $2F-$AE with its sum in $AF, the clock $25-$28
 *   (big-endian seconds) with its sum in $29 ($EF84, $EFAF).
 *
 * The chip runs behind guest time: every input change first brings it up
 * to the moment of the change, and a deadline brings it up regularly, every
 * 16 phi cycles while a handshake is under way (the firmware waits only
 * some 45 us for the Mac's next edge) and every millisecond otherwise. */
#include "pmu_fw.h"

#include <stdlib.h>
#include <string.h>

#define ACTIVE_SLICE 4u             /* phi cycles */
#define IDLE_SLICE 128u
#define POWER_KEY_TICKS 10u           /* the press: the firmware debounces 8 ticks */
#define ACTIVE_WINDOW (MC_C15M_HZ / 500u)   /* 2 ms after a handshake edge */

static uint64_t chip_time(const mc_pmu_fw *f) {
    return f->base_c15m + (f->chip.cycles - f->base_cycles) * MC_C15M_HZ / f->phi;
}

static void set_clock(mc_pmu_fw *f, uint32_t phi) {
    if (phi == f->phi) return;
    f->base_c15m = chip_time(f);
    f->base_cycles = f->chip.cycles;
    f->phi = phi;
}

static void line(mc_pmu_fw *f, int which, int state) { mc_dev_line(&f->pmu->dev, which, state); }

static int portable(const mc_pmu_fw *f) { return f->pmu->model == MC_PMU_PORTABLE; }

static void set_power(mc_pmu_fw *f, uint8_t power) {
    if (power == f->pmu->power) return;
    f->pmu->power = power;
    line(f, MC_PMU_LINE_POWER, power);
}

static void chip_out(void *ctx, int port, uint8_t levels) {
    mc_pmu_fw *f = ctx;
    mc_pmu *p = f->pmu;
    switch (port) {
    case 0:
        set_clock(f, (levels & 0x17u) == 0x17u && (f->chip.ram[0] & 0x40u) ? MC_PMU_FW_SLOW_PHI : MC_PMU_FW_PHI);
        line(f, MC_PMU_LINE_PLANES, levels);
        if (f->wake_hold && (levels & 0x17u) != 0x17u) {
            /* Out of the sleep loop: the key comes up three ticks later. */
            f->wake_hold = 0;
            f->wake_release = chip_time(f) + 3u * MC_PMU_TICK;
        }
        if (levels & 0x80u) set_power(f, portable(f) || f->chip.ram[0x2E] == 0xAA ? MC_PMU_POWER_SLEEP : MC_PMU_POWER_OFF);
        else set_power(f, MC_PMU_POWER_ON);
        break;
    case 1: {
        /* The Portable's CPU also waits for P3.1 (below). */
        const uint8_t reset = (levels & 0x10u) && (!portable(f) || (mc_m50753_pins(&f->chip, 3) & 0x02u)) ? 0 : 1;
        if (reset != p->reset) {
            p->reset = reset;
            line(f, MC_PMU_LINE_RESET, reset);
        }
        break;
    }
    case 2: {
        const int data = f->chip.ddr[2] == 0xFFu ? (int)f->chip.latch[2] : -1;
        if (data != p->data_out) {
            p->data_out = data;
            line(f, MC_PMU_LINE_DATA, data);
        }
        break;
    }
    case 3: {
        const uint8_t ack = (levels >> 6) & 1u, irq = (levels >> 5) & 1u, modem = (levels >> 3) & 1u;
        if (ack != p->ack) {
            p->ack = ack;
            f->last_edge = chip_time(f);
            line(f, MC_PMU_LINE_ACK, ack);
        }
        if (irq != p->int_level) {
            p->int_level = irq;
            line(f, MC_PMU_LINE_INT, irq);
        }
        if (modem != p->modem) {
            p->modem = modem;
            line(f, MC_PMU_LINE_MODEM, modem);
        }
        if (portable(f)) chip_out(ctx, 1, mc_m50753_pins(&f->chip, 1));
        if (portable(f) && ((levels >> 4) & 1u) != p->second_level) {
            p->second_level = (levels >> 4) & 1u;
            line(f, MC_PMU_LINE_SECOND, p->second_level);
        }
        break;
    }
    case 4:
        mc_adb_wire_host(&f->wire, (levels & 1u) != 0, chip_time(f));
        break;
    default: break;
    }
}

mc_pmu_fw *mc_pmu_fw_new(mc_pmu *p, const uint8_t *firmware) {
    mc_pmu_fw *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->pmu = p;
    mc_m50753_init(&f->chip, firmware, f, chip_out);
    mc_adb_wire_init(&f->wire, p->bus);
    f->phi = MC_PMU_FW_PHI;
    return f;
}

void mc_pmu_fw_free(mc_pmu_fw *f) { free(f); }

static void board_inputs(mc_pmu_fw *f) {
    mc_m50753 *c = &f->chip;
    if (portable(f)) {
        mc_m50753_set_pins(c, 1, 0x40, 0x4A);       /* no key, the charger, no ring */
        mc_m50753_set_pins(c, 3, (uint8_t)(0x01u | (f->pmu->req ? 0x80u : 0)), 0x85);
        mc_m50753_set_int1(c, f->pmu->tick_in);
        mc_m50753_set_int2(c, 1);
    } else {
        mc_m50753_set_pins(c, 1, 0x48, 0x4A);       /* no key, charger, P1.6 */
        mc_m50753_set_pins(c, 3, (uint8_t)(0x14u | (f->pmu->req ? 0x80u : 0)), 0x94);
    }
    mc_m50753_set_pins(c, 4, 0x0A, 0x0A);
    mc_m50753_set_pins(c, 2, f->pmu->port_in, 0xFF);
    /* The power key pressed: the machine is switched on (or, bit 5 clear,
     * the charger plugged in: a start by the charger). */
    c->in_port = f->pmu->charger_start ? 0x8Fu : 0xAFu;
    for (int i = 0; i < 8; i++) c->analog[i] = 0x80;
    c->analog[0] = portable(f) ? MC_PMU_TEMPERATURE : f->pmu->slider;
    c->analog[1] = MC_PMU_BATTERY_LEVEL;
    c->analog[5] = MC_PMU_TEMPERATURE;
    c->analog[7] = MC_PMU_CHARGER_SENSE;
}

void mc_pmu_fw_reset(mc_pmu_fw *f) {
    const uint64_t now = mc_dev_now(&f->pmu->dev);
    f->base_c15m = now;
    f->base_cycles = f->chip.cycles;
    f->phi = MC_PMU_FW_PHI;
    f->last_edge = now;
    f->wake_release = 0;
    f->wake_hold = f->key_hold = 0;
    f->contrast_reported = -1;
    board_inputs(f);
    f->key_release = portable(f) ? 0 : now + POWER_KEY_TICKS * MC_PMU_TICK;
    mc_m50753_reset(&f->chip);
    mc_dev_schedule(&f->pmu->dev, now + 1);
}

void mc_pmu_fw_catch_up(mc_pmu_fw *f, uint64_t now) {
    mc_m50753 *c = &f->chip;
    while (chip_time(f) < now) {
        /* The ADB line as the devices and the host leave it at this cycle. */
        const uint8_t adb = (uint8_t)(mc_adb_wire_level(&f->wire, chip_time(f)) ? 0x02u : 0);
        mc_m50753_set_pins(c, 4, adb, 0x02);
        mc_m50753_step(c);
    }
    /* Keep the arithmetic in range: move the base by whole seconds. */
    const uint64_t delta = c->cycles - f->base_cycles;
    if (delta > ((uint64_t)1 << 31)) {
        const uint64_t seconds = delta / f->phi;
        f->base_cycles += seconds * f->phi;
        f->base_c15m += seconds * MC_C15M_HZ;
    }
}

static void contrast(mc_pmu_fw *f) {
    const mc_m50753 *c = &f->chip;
    const int value = (c->pwm_control & 1u) && c->t1_latch >= 2 ? c->t1_latch / 2 - 1 : 31;
    if (value != f->contrast_reported) {
        f->contrast_reported = value;
        f->pmu->contrast = (uint8_t)value;
        line(f, MC_PMU_LINE_CONTRAST, value);
    }
}

void mc_pmu_fw_event(mc_pmu_fw *f, uint64_t now) {
    if (f->key_release && now >= f->key_release) {
        mc_pmu_fw_catch_up(f, f->key_release);
        f->chip.in_port |= 0x10u;
        f->key_release = 0;
    }
    if (f->wake_release && now >= f->wake_release) {
        mc_pmu_fw_catch_up(f, f->wake_release);
        mc_m50753_set_pins(&f->chip, 1, 0x00, 0x02);
        f->wake_release = 0;
    }
    mc_pmu_fw_catch_up(f, now);
    if (f->wake_hold == 2 && (f->chip.ram[0] & 0x04u)) {
        /* The sleep loop has seen the keys up (its $00 bit 2): one goes down. */
        mc_m50753_set_pins(&f->chip, 1, 0x02, 0x02);
        f->wake_hold = 1;
    }
    if (f->key_hold && (f->chip.ram[0x2D] & 0x80u)) {
        /* The firmware has the key down (its debounce, $2D bit 7): up. */
        f->key_hold = 0;
        f->chip.in_port |= 0x10u;
    }
    contrast(f);
    const mc_pmu *p = f->pmu;
    const int active = !p->req || !p->ack || now - f->last_edge < ACTIVE_WINDOW;
    uint64_t next = now + (uint64_t)(active ? ACTIVE_SLICE : IDLE_SLICE) * MC_C15M_HZ / f->phi;
    if (f->wake_release && f->wake_release < next) next = f->wake_release;
    if (f->key_release && f->key_release < next) next = f->key_release;
    mc_dev_schedule(&f->pmu->dev, next);
}

void mc_pmu_fw_req(mc_pmu_fw *f, int level) {
    const uint64_t now = mc_dev_now(&f->pmu->dev);
    mc_pmu_fw_catch_up(f, now);
    mc_m50753_set_pins(&f->chip, 3, level ? 0x80u : 0, 0x80);
    f->last_edge = now;
    mc_dev_schedule(&f->pmu->dev, now + (uint64_t)ACTIVE_SLICE * MC_C15M_HZ / f->phi);
}

void mc_pmu_fw_port(mc_pmu_fw *f, uint8_t value) {
    mc_pmu_fw_catch_up(f, mc_dev_now(&f->pmu->dev));
    mc_m50753_set_pins(&f->chip, 2, value, 0xFF);
}

int mc_pmu_fw_wake(mc_pmu_fw *f) {
    if (f->pmu->power == MC_PMU_POWER_ON) return 0;
    const uint64_t now = mc_dev_now(&f->pmu->dev);
    mc_pmu_fw_catch_up(f, now);
    if (f->pmu->power == MC_PMU_POWER_SLEEP) {
        /* A key goes down once the sleep loop has seen the keys up, and
         * stays down until the loop has seen it (the planes leave their
         * sleep pattern); it comes up three ticks later and the CPU is
         * reset after it. */
        f->wake_hold = 2;
    } else {
        /* Off: the power key, down until the firmware's debounce has it
         * (the off loop samples it once a slow pass), then up. */
        f->chip.in_port &= 0xEFu;
        f->key_hold = 1;
    }
    mc_dev_schedule(&f->pmu->dev, now + 1);
    return 1;
}

void mc_pmu_fw_reset_switch(mc_pmu_fw *f, int pressed) {
    const uint64_t now = mc_dev_now(&f->pmu->dev);
    mc_pmu_fw_catch_up(f, now);
    mc_m50753_set_pins(&f->chip, 3, pressed ? 0 : 0x01u, 0x01);
    mc_m50753_set_int2(&f->chip, !pressed);
    mc_dev_schedule(&f->pmu->dev, now + 1);
}

/* The Portable's firmware keeps sums beside its clock and parameter RAM
 * and clears either when its sum does not match. */
static void portable_sums(mc_pmu_fw *f) {
    uint8_t *r = f->chip.ram, sum = 0;
    for (unsigned i = 0; i < 128; i++) sum = (uint8_t)(sum + r[0x2F + i]);
    r[0xAF] = sum;
    r[0x29] = (uint8_t)(r[0x25] + r[0x26] + r[0x27] + r[0x28]);
}

static uint8_t *pram_of(mc_pmu_fw *f) { return f->chip.ram + (portable(f) ? 0x2F : 0x30); }
static uint8_t *clock_of(mc_pmu_fw *f) { return f->chip.ram + (portable(f) ? 0x25 : 0x30); }

void mc_pmu_fw_pram_get(mc_pmu_fw *f, uint8_t *out) { memcpy(out, pram_of(f), 128); }
void mc_pmu_fw_pram_put(mc_pmu_fw *f, const uint8_t *in) {
    memcpy(pram_of(f), in, 128);
    if (portable(f)) portable_sums(f);
}

uint32_t mc_pmu_fw_seconds(mc_pmu_fw *f) {
    const uint8_t *r = clock_of(f);
    return (uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3];
}

void mc_pmu_fw_set_seconds(mc_pmu_fw *f, uint32_t s) {
    uint8_t *r = clock_of(f);
    r[0] = (uint8_t)(s >> 24); r[1] = (uint8_t)(s >> 16); r[2] = (uint8_t)(s >> 8); r[3] = (uint8_t)s;
    if (portable(f)) portable_sums(f);
}

void mc_pmu_fw_tick(mc_pmu_fw *f, int level) {
    mc_pmu_fw_catch_up(f, mc_dev_now(&f->pmu->dev));
    mc_m50753_set_int1(&f->chip, level);
}
