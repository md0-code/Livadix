/* Livadix — the Egret and Cuda microcontrollers at the firmware level (see
 * adbctl_mcu.h for sources). Livadix's own code. */
#include "adbctl_mcu.h"
#include "hw.h"

#include <string.h>

enum { PORT_A, PORT_B, PORT_C };

static uint64_t cycle_units(const mc_adbctl_mcu *m) {
    return (m->pll & 0x40u) ? MC_ADBCTL_FAST_UNITS : MC_ADBCTL_SLOW_UNITS;
}

static void report(mc_adbctl_mcu *m, unsigned port) {
    if (port > PORT_C) return;
    const uint8_t driven = m->ddr[port];
    const uint8_t levels = (uint8_t)((m->latch[port] & driven) | (uint8_t)~driven);
    if (levels == m->out_levels[port] && driven == m->out_driven[port]) return;
    m->out_levels[port] = levels;
    m->out_driven[port] = driven;
    if (m->outputs) m->outputs(m->ctx, port, levels, driven, mc_adbctl_mcu_now(m));
}

static uint8_t read_port(mc_adbctl_mcu *m, unsigned port) {
    const uint8_t pins = m->pins ? m->pins(m->ctx, port, mc_adbctl_mcu_now(m)) : 0xFFu;
    return (uint8_t)((m->latch[port] & m->ddr[port]) | (pins & (uint8_t)~m->ddr[port]));
}

static uint8_t mcu_read(void *ctx, uint16_t address) {
    mc_adbctl_mcu *m = ctx;
    address &= 0x1FFFu;
    if (address >= MC_ADBCTL_FW_BASE) return m->rom[address - MC_ADBCTL_FW_BASE];
    if (address >= 0x90u && address < 0x200u) return m->ram[address];
    switch (address) {
    case 0x00: case 0x01: case 0x02: return read_port(m, address);
    case 0x03: return m->latch[3];
    case 0x04: case 0x05: case 0x06: return m->ddr[address - 4u];
    case 0x07: return m->pll;
    case 0x08: return m->ctcsr;
    case 0x09: return (uint8_t)(m->timer_cycles >> 2);
    case 0x12: return m->ostcsr;
    default: return address < 0x20u ? m->io[address] : 0;
    }
}

static void mcu_write(void *ctx, uint16_t address, uint8_t value) {
    mc_adbctl_mcu *m = ctx;
    address &= 0x1FFFu;
    if (address >= 0x90u && address < 0x200u) {
        if (m->keep_ram && (address >= 0x100u || (address >= 0xABu && address <= 0xAEu))) return;
        m->ram[address] = value;
        return;
    }
    switch (address) {
    case 0x00: case 0x01: case 0x02: m->latch[address] = value; report(m, address); break;
    case 0x03: m->latch[3] = value; break;
    case 0x04: case 0x05: case 0x06: m->ddr[address - 4u] = value; report(m, address - 4u); break;
    case 0x07: m->pll = value; break;
    case 0x08:
        /* Flags: written 0 clears, 1 keeps; the enables and rate as written. */
        m->ctcsr = (uint8_t)((m->ctcsr & value & 0xC0u) | (value & 0x3Fu));
        break;
    case 0x12:
        m->ostcsr = (uint8_t)((m->ostcsr & value & 0x40u) | (value & 0xBFu));
        break;
    default:
        if (address < 0x20u) m->io[address] = value;
        break;
    }
}

uint8_t mc_adbctl_mcu_peek(mc_adbctl_mcu *m, uint16_t address) {
    if ((address & 0x1FFFu) <= 0x02u) return m->latch[address & 3u];
    return mcu_read(m, address);
}

/* The core timer and the one-second timer after `n` more bus cycles. */
static void timers(mc_adbctl_mcu *m, unsigned n) {
    const uint64_t before = m->timer_cycles;
    m->timer_cycles += n;
    if ((before >> 10) != (m->timer_cycles >> 10)) m->ctcsr |= 0x80u;
    const unsigned rti = 14u + (m->ctcsr & 3u);
    if ((before >> rti) != (m->timer_cycles >> rti)) m->ctcsr |= 0x40u;
    while (mc_adbctl_mcu_now(m) >= m->next_second) {
        m->ostcsr |= 0x40u;
        m->next_second += MC_C15M_HZ;
    }
}

/* The highest pending interrupt's vector, 0 for none. */
static uint16_t pending(const mc_adbctl_mcu *m) {
    if (m->irq_latch) return 0x1FFAu;
    if ((m->ctcsr & 0x80u && m->ctcsr & 0x20u) || (m->ctcsr & 0x40u && m->ctcsr & 0x10u)) return 0x1FF8u;
    if (m->ostcsr & 0x40u && m->ostcsr & 0x10u) return 0x1FF6u;
    return 0;
}

void mc_adbctl_mcu_run(mc_adbctl_mcu *m, uint64_t until) {
    const uint64_t end = until * 512u;
    while (m->units < end) {
        const uint16_t vector = pending(m);
        unsigned n = vector ? mc_cpu6805_interrupt(&m->cpu, vector) : 0;
        if (n && vector == 0x1FFAu) m->irq_latch = 0;
        if (!n) n = mc_cpu6805_step(&m->cpu);
        if (!n) n = 1;                  /* STOP/WAIT: time passes cycle by cycle */
        m->units += n * cycle_units(m);
        timers(m, n);
    }
}

void mc_adbctl_mcu_set_irq(mc_adbctl_mcu *m, int level) {
    level = level != 0;
    if (m->cpu.irq_pin && !level) m->irq_latch = 1;
    m->cpu.irq_pin = (uint8_t)level;
}

void mc_adbctl_mcu_reset(mc_adbctl_mcu *m, uint64_t now) {
    memset(m->latch, 0, sizeof m->latch);
    memset(m->ddr, 0, sizeof m->ddr);
    memset(m->io, 0, sizeof m->io);
    m->pll = 0;
    m->ctcsr = 0;
    m->ostcsr = 0;
    m->units = now * 512u;
    m->timer_cycles = 0;
    m->next_second = now + MC_C15M_HZ;
    m->out_levels[0] = m->out_levels[1] = m->out_levels[2] = 0;
    m->out_driven[0] = m->out_driven[1] = m->out_driven[2] = 0xFFu;   /* forces the first report */
    for (unsigned p = 0; p < 3; p++) report(m, p);
    mc_cpu6805_reset(&m->cpu);
}

void mc_adbctl_mcu_init(mc_adbctl_mcu *m, const uint8_t *image, uint64_t now, void *ctx,
                        uint8_t (*pins)(void *, unsigned, uint64_t),
                        void (*outputs)(void *, unsigned, uint8_t, uint8_t, uint64_t)) {
    memset(m, 0, sizeof *m);
    memcpy(m->rom, image, MC_ADBCTL_FW_SIZE);
    m->ctx = ctx;
    m->pins = pins;
    m->outputs = outputs;
    mc_cpu6805_init(&m->cpu, m, mcu_read, mcu_write, 0x1FFEu);
    mc_adbctl_mcu_reset(m, now);
}
