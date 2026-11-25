/* Livadix — the M50753 single-chip microcomputer (see m50753.h for sources).
 * Livadix's own code. Where the data sheet is silent Livadix chooses, marked †:
 * a write to a timer or prescaler loads its latch and its counter; a latch
 * of 0 divides by 256; unused addresses read $FF; P4's upper four bits read
 * 1; port IN reads 0 during a conversion; serial transfers with an internal
 * clock shift SIN's level in and end after eight transfer clocks; the PWM
 * timers count f(XIN)/16 directly, timer 1 high then timer 2 low. */
#include "m50753.h"

#include <string.h>

static const uint8_t PORT_MASK[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x0F};

static uint8_t driven(const mc_m50753 *m, int p) {
    /* N-channel open drain: an output latch of 0 pulls the pin low, anything
     * else leaves it released (high). */
    return (uint8_t)((m->latch[p] | (uint8_t)~m->ddr[p]) & PORT_MASK[p]);
}

static void report(mc_m50753 *m, int p) {
    const uint8_t level = driven(m, p);
    if (level != m->reported[p]) {
        m->reported[p] = level;
        if (m->out) m->out(m->ctx, p, level);
    }
}

uint8_t mc_m50753_pins(const mc_m50753 *m, int p) {
    return (uint8_t)(driven(m, p) & m->ext[p]);
}

static uint8_t port_read(const mc_m50753 *m, int p) {
    const uint8_t v = (uint8_t)((m->latch[p] & m->ddr[p]) | (m->ext[p] & (uint8_t)~m->ddr[p]));
    return p == 4 ? (uint8_t)((v & 0x0Fu) | 0xF0u) : v;
}

/* --- timers ---------------------------------------------------------------------- */

static uint16_t period(uint8_t latch) { return latch ? latch : 256u; }

static void timer_x_count(mc_m50753 *m) {
    if (--m->prex) return;
    m->prex = period(m->prex_latch);
    if (--m->tx) return;
    m->tx = period(m->tx_latch);
    m->tcr |= 0x80u;
    if (((m->tcr >> 2) & 3u) == 1u) {
        m->cntr_out ^= 1u;
        if (m->out) m->out(m->ctx, MC_M50753_PIN_CNTR, m->cntr_out);
    }
}

/* One period of f(XIN)/16. */
static void count_clock(mc_m50753 *m) {
    const unsigned xmode = (m->tcr >> 2) & 3u;
    if (!(m->tcr & 0x20u) && xmode != 2u && (xmode != 3u || !m->cntr)) timer_x_count(m);
    if (m->pwm_control & 1u) {
        /* PWM: timer 1's period high, then timer 2's low. */
        if (m->pwm_phase == 0) {
            if (!--m->t1) {
                m->t1 = period(m->t1_latch);
                m->pwm_phase = 1;
                m->pwm_out = 0;
                if (m->out) m->out(m->ctx, MC_M50753_PIN_PWM, 0);
            }
        } else if (!--m->t2) {
            m->t2 = period(m->t2_latch);
            m->pwm_phase = 0;
            m->pwm_out = 1;
            if (m->out) m->out(m->ctx, MC_M50753_PIN_PWM, 1);
        }
        return;
    }
    if (--m->pre12) return;
    m->pre12 = period(m->pre12_latch);
    if (!--m->t1) {
        m->t1 = period(m->t1_latch);
        if (!(m->adc_control & 0x08u)) m->icr |= 0x20u;
    }
    if (!--m->t2) {
        m->t2 = period(m->t2_latch);
        if (!(m->sio_mode & 0x04u)) m->icr |= 0x08u;
    }
}

static void run_peripherals(mc_m50753 *m, unsigned cycles) {
    while (cycles--) {
        m->cycles++;
        if (m->adc_busy && !--m->adc_left) {
            m->adc_busy = 0;
            m->adc_result = m->analog[m->adc_control & 7u];
            if (m->adc_control & 0x08u) m->icr |= 0x20u;
        }
        if (m->sio_left && !--m->sio_left) {
            m->sio_data = (mc_m50753_pins(m, 3) & 0x10u) ? 0xFFu : 0x00u;
            if (m->sio_mode & 0x04u) m->icr |= 0x08u;
        }
        if (++m->div16 == 4) {
            m->div16 = 0;
            if (!m->cpu.stopped || m->stp_left) count_clock(m);
        }
    }
}

/* --- memory ---------------------------------------------------------------------- */

uint8_t mc_m50753_read(mc_m50753 *m, uint16_t a) {
    if (a < MC_M50753_RAM_SIZE) return m->ram[a];
    if (a >= MC_M50753_ROM_BASE) return m->rom[a - MC_M50753_ROM_BASE];
    switch (a) {
    case 0xE0: return port_read(m, 0);
    case 0xE1: return m->ddr[0];
    case 0xE2: return port_read(m, 1);
    case 0xE3: return m->ddr[1];
    case 0xE4: return port_read(m, 2);
    case 0xE5: return m->ddr[2];
    case 0xE8: return port_read(m, 3);
    case 0xE9: return m->ddr[3];
    case 0xEA: return port_read(m, 4);
    case 0xEB: return (uint8_t)(m->ddr[4] | 0xF0u);
    case 0xEE: return m->adc_busy ? 0 : m->in_port;
    case 0xEF: return m->adc_result;
    case 0xF3: return (uint8_t)(m->adc_control | 0xF0u);
    case 0xF5: return (uint8_t)(m->pwm_control | 0xFEu);
    case 0xF6: return (uint8_t)(m->sio_mode | 0xF0u);
    case 0xF7: return m->sio_data;
    case 0xF9: return (uint8_t)m->pre12;
    case 0xFA: return (uint8_t)m->t1;
    case 0xFB: return (uint8_t)m->t2;
    case 0xFC: return (uint8_t)m->prex;
    case 0xFD: return (uint8_t)m->tx;
    case 0xFE: return m->icr;
    case 0xFF: return m->tcr;
    default: return 0xFF;
    }
}

void mc_m50753_write(mc_m50753 *m, uint16_t a, uint8_t v) {
    if (a < MC_M50753_RAM_SIZE) {
        m->ram[a] = v;
        return;
    }
    switch (a) {
    case 0xE0: m->latch[0] = v; report(m, 0); break;
    case 0xE1: m->ddr[0] = v; report(m, 0); break;
    case 0xE2: m->latch[1] = v; report(m, 1); break;
    case 0xE3: m->ddr[1] = v; report(m, 1); break;
    case 0xE4: m->latch[2] = v; report(m, 2); break;
    case 0xE5: m->ddr[2] = v; report(m, 2); break;
    case 0xE8: m->latch[3] = v; report(m, 3); break;
    case 0xE9: m->ddr[3] = v; report(m, 3); break;
    case 0xEA: m->latch[4] = (uint8_t)(v & 0x0Fu); report(m, 4); break;
    case 0xEB: m->ddr[4] = (uint8_t)(v & 0x0Fu); report(m, 4); break;
    case 0xF2:
        m->adc_busy = 1;
        m->adc_left = MC_M50753_ADC_CYCLES;
        break;
    case 0xF3: m->adc_control = (uint8_t)(v & 0x0Fu); break;
    case 0xF5:
        m->pwm_control = (uint8_t)(v & 1u);
        if (!(v & 1u)) m->pwm_out = 1;
        break;
    case 0xF6: m->sio_mode = (uint8_t)(v & 0x0Fu); break;
    case 0xF7:
        m->sio_data = v;
        /* Internal clocks: eight transfer clocks of f(XIN)/16 (mode 3) or of
         * timer 2's overflow halved (mode 2). */
        if ((m->sio_mode & 0x04u) && (m->sio_mode & 0x02u))
            m->sio_left = (m->sio_mode & 1u) ? 8u * 4u : 8u * 2u * 4u * period(m->t2_latch) * period(m->pre12_latch);
        break;
    case 0xF9: m->pre12_latch = v; m->pre12 = period(v); break;
    case 0xFA: m->t1_latch = v; m->t1 = period(v); break;
    case 0xFB: m->t2_latch = v; m->t2 = period(v); break;
    case 0xFC: m->prex_latch = v; m->prex = period(v); break;
    case 0xFD: m->tx_latch = v; m->tx = period(v); break;
    case 0xFE:
        /* Request bits (7, 5, 3, 1) can be cleared, not set. */
        m->icr = (uint8_t)((v & 0x55u) | (m->icr & v & 0xAAu));
        break;
    case 0xFF:
        m->tcr = (uint8_t)((v & 0x7Fu) | (m->tcr & v & 0x80u));
        m->cpu.stack_page = (v & 0x10u) ? 0x0100u : 0x0000u;
        break;
    default: break;
    }
}

static uint8_t cpu_read(void *ctx, uint16_t a) { return mc_m50753_read(ctx, a); }
static void cpu_write(void *ctx, uint16_t a, uint8_t v) { mc_m50753_write(ctx, a, v); }

/* --- interrupts ------------------------------------------------------------------- */

/* The highest-priority request whose enable is set (Table 1); clears it and
 * returns its vector, 0 when none. `take` 0 only looks. */
static uint16_t pending(mc_m50753 *m, int take) {
    if ((m->icr & 0xC0u) == 0xC0u) { if (take) m->icr &= 0x7Fu; return 0xFFFC; }
    if ((m->tcr & 0xC0u) == 0xC0u) { if (take) m->tcr &= 0x7Fu; return 0xFFFA; }
    if ((m->icr & 0x30u) == 0x30u) { if (take) m->icr &= 0xDFu; return 0xFFF8; }
    if ((m->icr & 0x0Cu) == 0x0Cu) { if (take) m->icr &= 0xF7u; return 0xFFF6; }
    if ((m->icr & 0x03u) == 0x03u) { if (take) m->icr &= 0xFDu; return 0xFFF4; }
    return 0;
}

unsigned mc_m50753_step(mc_m50753 *m) {
    unsigned cycles;
    if (m->cpu.stopped) {
        /* STP: the oscillator restarts on an interrupt request; phi resumes
         * when timer X overflows from its STP values. */
        if (!m->stp_left && pending(m, 0)) {
            m->stp_left = 1;
            m->tcr &= 0x7Fu;
        }
        if (m->stp_left && (m->tcr & 0x80u)) {
            m->stp_left = 0;
            m->cpu.stopped = 0;
        }
        run_peripherals(m, 1);
        return 1;
    }
    if (m->cpu.waiting) {
        if (pending(m, 0)) m->cpu.waiting = 0;
        else {
            run_peripherals(m, 1);
            return 1;
        }
    }
    const uint16_t vector = (m->cpu.p & MC_740_I) ? 0 : pending(m, 0);
    if (vector) {
        pending(m, 1);
        cycles = mc_cpu740_interrupt(&m->cpu, vector);
    } else {
        cycles = mc_cpu740_step(&m->cpu);
        if (m->cpu.stopped) {
            m->prex_latch = 0xFF;
            m->prex = 0xFF;
            m->tx_latch = 0x01;
            m->tx = 0x01;
        }
    }
    if (!cycles) cycles = 1;
    run_peripherals(m, cycles);
    return cycles;
}

/* --- pins ------------------------------------------------------------------------- */

void mc_m50753_set_pins(mc_m50753 *m, int p, uint8_t levels, uint8_t mask) {
    m->ext[p] = (uint8_t)((m->ext[p] & ~mask) | (levels & mask));
}

void mc_m50753_set_int1(mc_m50753 *m, int level) {
    level = level != 0;
    if (m->int1 && !level) m->icr |= 0x80u;
    m->int1 = (uint8_t)level;
}

void mc_m50753_set_int2(mc_m50753 *m, int level) {
    level = level != 0;
    if (m->int2 && !level) m->icr |= 0x02u;
    m->int2 = (uint8_t)level;
}

void mc_m50753_set_cntr(mc_m50753 *m, int level) {
    level = level != 0;
    /* Event counter mode: each rising edge clocks prescaler X. */
    if (!m->cntr && level && ((m->tcr >> 2) & 3u) == 2u && !(m->tcr & 0x20u)) timer_x_count(m);
    m->cntr = (uint8_t)level;
}

/* --- reset ------------------------------------------------------------------------ */

void mc_m50753_init(mc_m50753 *m, const uint8_t rom[MC_M50753_ROM_SIZE], void *ctx,
                    void (*out)(void *, int, uint8_t)) {
    memset(m, 0, sizeof *m);
    memcpy(m->rom, rom, MC_M50753_ROM_SIZE);
    m->ctx = ctx;
    m->out = out;
    memset(m->ext, 0xFF, sizeof m->ext);
    memset(m->reported, 0xFF, sizeof m->reported);
    m->int1 = m->int2 = 1;
    mc_cpu740_init(&m->cpu, m, cpu_read, cpu_write);
}

void mc_m50753_reset(mc_m50753 *m) {
    for (int p = 0; p < 5; p++) m->ddr[p] = 0;
    m->sio_mode = 0;
    m->sio_left = 0;
    m->prex_latch = 0xFF; m->prex = 0xFF;
    m->tx_latch = 0x01; m->tx = 0x01;
    m->icr = m->tcr = 0;
    m->adc_control = 0;
    m->adc_busy = 0;
    m->pwm_control = 0;
    m->pwm_out = 1;
    m->cpu.stack_page = 0;
    m->stp_left = 0;
    for (int p = 0; p < 5; p++) report(m, p);
    mc_cpu740_reset(&m->cpu);
}
