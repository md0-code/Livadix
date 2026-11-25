/* Livadix — the 6522 VIA, from the Rockwell R6522 VIA data sheet and the Guide
 * to the Macintosh Family Hardware, 2nd ed., chapter 4 (see via.h).
 *
 * Timing (R6522 data sheet, Figures 15-19): a counter loaded at time w
 * shows N at w and counts down once per E cycle; its flag sets N + 1.5
 * cycles after the load, as the counter passes from 0 to $FFFF. In
 * free-running mode Timer 1 reloads from the latches half a cycle later, so
 * its period is N + 2 cycles and PB7 inverts at each flag. Timer 2 one-shot
 * keeps counting after its flag, which it sets only once per T2C-H write.
 * All of this is computed from the guest time when it matters (a register
 * read, a flag due); the scheduler is armed only for the next flag or PB7
 * edge something can observe. */
#include "via.h"

#include <string.h>

#define E MC_VIA_ECLOCK

static void reschedule(mc_via *v);

/* --- outputs ----------------------------------------------------------------- */

static void update_irq(mc_via *v) {
    const uint8_t irq = (v->ifr & v->ier & 0x7Fu) != 0;
    if (irq != v->irq_out) {
        v->irq_out = irq;
        mc_dev_line(&v->dev, MC_VIA_LINE_IRQ, irq);
    }
}

static uint8_t pa_driven(const mc_via *v) { return (uint8_t)((v->ora & v->ddra) | (uint8_t)~v->ddra); }
static uint8_t pb_driven(const mc_via *v) {
    uint8_t pb = (uint8_t)((v->orb & v->ddrb) | (uint8_t)~v->ddrb);
    if (v->acr & 0x80u) pb = (uint8_t)((pb & 0x7Fu) | (v->pb7 ? 0x80u : 0));
    return pb;
}

static void update_ports(mc_via *v) {
    const uint8_t pa = pa_driven(v), pb = pb_driven(v);
    if (pa != v->pa_out) {
        v->pa_out = pa;
        mc_dev_line(&v->dev, MC_VIA_LINE_PA, pa);
    }
    if (pb != v->pb_out) {
        v->pb_out = pb;
        mc_dev_line(&v->dev, MC_VIA_LINE_PB, pb);
    }
}

static void set_ca2_out(mc_via *v, uint8_t level) {
    if (level != v->ca2_out) {
        v->ca2_out = level;
        mc_dev_line(&v->dev, MC_VIA_LINE_CA2, level);
    }
}
static void set_cb2_out(mc_via *v, uint8_t level) {
    if (level != v->cb2_out) {
        v->cb2_out = level;
        mc_dev_line(&v->dev, MC_VIA_LINE_CB2, level);
    }
}

/* PCR CA2 (bits 3-1) and CB2 (bits 7-5) control (R6522 data sheet, Figure 12). */
static unsigned ca2_mode(const mc_via *v) { return (v->pcr >> 1) & 7u; }
static unsigned cb2_mode(const mc_via *v) { return (v->pcr >> 5) & 7u; }
/* Input modes 001 and 011 are "independent": port accesses leave the flag. */
static int independent(unsigned mode) { return mode == 1 || mode == 3; }
static int sr_drives_cb2(const mc_via *v) { return mc_via_sr_mode(v) >= 4; }

static void pcr_outputs(mc_via *v) {
    const unsigned ca = ca2_mode(v), cb = cb2_mode(v);
    if (ca == 6) set_ca2_out(v, 0);
    else if (ca == 7) set_ca2_out(v, 1);
    else if (ca < 4) set_ca2_out(v, 1);
    if (!sr_drives_cb2(v)) {
        if (cb == 6) set_cb2_out(v, 0);
        else if (cb == 7) set_cb2_out(v, 1);
        else if (cb < 4) set_cb2_out(v, 1);
    }
}

/* --- timers ------------------------------------------------------------------ */

static int t1_free(const mc_via *v) { return (v->acr & 0x40u) != 0; }
static int t2_pulses(const mc_via *v) { return (v->acr & 0x20u) != 0; }

/* The flag instant of the current Timer 1 count: N + 1.5 cycles after the load. */
static uint64_t t1_flag_time(const mc_via *v) { return v->t1_start + (2u * (uint64_t)v->t1_n + 3u) * (E / 2u); }

static void t1_sync(mc_via *v, uint64_t now) {
    if (!v->t1_armed) return;
    if (!t1_free(v)) {
        if (now >= t1_flag_time(v)) {
            v->ifr |= MC_VIA_IRQ_T1;
            v->t1_armed = 0;
            v->pb7 = 1;
        }
        return;
    }
    while (now >= t1_flag_time(v)) {
        const uint64_t period = ((uint64_t)v->t1_n + 2u) * E;
        uint64_t m = 1;
        if (v->t1_n == v->t1_latch) m = (now - t1_flag_time(v)) / period + 1u;
        v->ifr |= MC_VIA_IRQ_T1;
        if (m & 1u) v->pb7 ^= 1u;
        v->t1_start += m * period;
        v->t1_n = v->t1_latch;
    }
}

uint16_t mc_via_t1(mc_via *v) {
    const uint64_t now = mc_dev_now(&v->dev);
    t1_sync(v, now);
    if (now < v->t1_start) return 0xFFFFu;     /* between the flag and the reload */
    const uint64_t k = (now - v->t1_start) / E;
    if (t1_free(v) && v->t1_armed) return k <= v->t1_n ? (uint16_t)(v->t1_n - k) : 0xFFFFu;
    return (uint16_t)(v->t1_n - k);
}

static uint64_t t2_flag_time(const mc_via *v) { return v->t2_start + (2u * (uint64_t)v->t2_n + 3u) * (E / 2u); }

static void t2_sync(mc_via *v, uint64_t now) {
    if (!v->t2_armed || t2_pulses(v)) return;
    if (now >= t2_flag_time(v)) {
        v->ifr |= MC_VIA_IRQ_T2;
        v->t2_armed = 0;
    }
}

uint16_t mc_via_t2(mc_via *v) {
    if (t2_pulses(v)) return v->t2_count;
    const uint64_t now = mc_dev_now(&v->dev);
    t2_sync(v, now);
    return (uint16_t)(v->t2_n - (now - v->t2_start) / E);
}

/* --- shift register ---------------------------------------------------------- */

/* Internally clocked modes: 1 and 5 shift at the Timer 2 rate (CB1 half
 * period N + 2 cycles, N the low latch), 2 and 6 once per E cycle. */
static uint64_t sr_done_time(const mc_via *v) {
    const unsigned mode = mc_via_sr_mode(v);
    const uint64_t per_bit = (mode == 2 || mode == 6) ? E : 2u * ((uint64_t)v->t2_latch_lo + 2u) * E;
    return v->sr_start + 8u * per_bit;
}

static void sr_sync(mc_via *v, uint64_t now) {
    if (!v->sr_running || now < sr_done_time(v)) return;
    const unsigned mode = mc_via_sr_mode(v);
    v->sr_running = 0;
    v->sr_count = 8;
    if (mode == 1 || mode == 2) v->sr = v->cb2 ? 0xFFu : 0x00u;   /* CB2 sampled 8 times */
    else set_cb2_out(v, (uint8_t)(v->sr & 1u));                     /* the last bit out; SR rotated back */
    v->ifr |= MC_VIA_IRQ_SR;
}

static void sr_access(mc_via *v, uint64_t now) {
    v->ifr &= (uint8_t)~MC_VIA_IRQ_SR;
    v->sr_count = 0;
    const unsigned mode = mc_via_sr_mode(v);
    v->sr_running = mode == 1 || mode == 2 || mode == 5 || mode == 6;
    v->sr_start = now;
}

/* One CB1 clock edge seen by the shift register (modes 0, 3, 7: external). */
static void sr_cb1_edge(mc_via *v, int rising) {
    const unsigned mode = mc_via_sr_mode(v);
    if (mode == 7) {
        if (!rising) {                 /* the next bit goes out on the falling edge */
            const uint8_t bit = (uint8_t)(v->sr >> 7);
            v->sr = (uint8_t)(v->sr << 1 | bit);
            set_cb2_out(v, bit);
        } else if (++v->sr_count >= 8) {
            v->sr_count = 0;
            v->ifr |= MC_VIA_IRQ_SR;
        }
    } else if ((mode == 0 || mode == 3) && rising) {
        v->sr = (uint8_t)(v->sr << 1 | (v->cb2 ? 1u : 0u));
        if (mode == 3 && ++v->sr_count >= 8) {
            v->sr_count = 0;
            v->ifr |= MC_VIA_IRQ_SR;
        }
    }
}

/* --- the scheduler ------------------------------------------------------------ */

static void sync(mc_via *v, uint64_t now) {
    const uint8_t pb7 = v->pb7;
    t1_sync(v, now);
    t2_sync(v, now);
    sr_sync(v, now);
    if (pb7 != v->pb7) update_ports(v);
}

static void reschedule(mc_via *v) {
    uint64_t next = MC_NEVER;
    if (v->t1_armed && ((v->ier & MC_VIA_IRQ_T1) || (v->acr & 0x80u))) {
        const uint64_t t = t1_flag_time(v);
        if (t < next) next = t;
    }
    if (v->t2_armed && !t2_pulses(v) && (v->ier & MC_VIA_IRQ_T2)) {
        const uint64_t t = t2_flag_time(v);
        if (t < next) next = t;
    }
    if (v->sr_running && (v->ier & MC_VIA_IRQ_SR)) {
        const uint64_t t = sr_done_time(v);
        if (t < next) next = t;
    }
    if (next != v->next_event) {
        v->next_event = next;
        mc_dev_schedule(&v->dev, next);
    }
}

static void finish(mc_via *v) {
    update_irq(v);
    reschedule(v);
}

static void via_event(void *dev, uint64_t now) {
    mc_via *v = dev;
    v->next_event = MC_NEVER;
    sync(v, now);
    finish(v);
}

/* --- registers ---------------------------------------------------------------- */

static void port_a_access(mc_via *v) {
    v->ifr &= (uint8_t)~MC_VIA_IRQ_CA1;
    if (!independent(ca2_mode(v))) v->ifr &= (uint8_t)~MC_VIA_IRQ_CA2;
    const unsigned mode = ca2_mode(v);
    if (mode == 4) set_ca2_out(v, 0);       /* handshake: low until the CA1 edge */
    else if (mode == 5) {                   /* pulse: low for one cycle */
        set_ca2_out(v, 0);
        set_ca2_out(v, 1);
    }
}

static void port_b_flags(mc_via *v) {
    v->ifr &= (uint8_t)~MC_VIA_IRQ_CB1;
    if (!independent(cb2_mode(v))) v->ifr &= (uint8_t)~MC_VIA_IRQ_CB2;
}

static uint8_t read_pa(const mc_via *v) {
    if (v->acr & 0x01u) return v->ira;
    return (uint8_t)((v->ora & v->ddra) | (v->pa_in & ~v->ddra));
}

static uint8_t read_pb(const mc_via *v) {
    const uint8_t pins = (v->acr & 0x02u) ? v->irb : v->pb_in;
    uint8_t value = (uint8_t)((v->orb & v->ddrb) | (pins & ~v->ddrb));
    if (v->acr & 0x80u) value = (uint8_t)((value & 0x7Fu) | (v->pb7 ? 0x80u : 0));
    return value;
}

static uint32_t via_read(void *dev, uint32_t offset, unsigned size) {
    (void)size;
    mc_via *v = dev;
    const uint64_t now = mc_dev_now(&v->dev);
    sync(v, now);
    uint8_t value = 0;
    switch ((offset >> v->shift) & 15u) {
    case MC_VIA_ORB: value = read_pb(v); port_b_flags(v); break;
    case MC_VIA_ORA: value = read_pa(v); port_a_access(v); break;
    case MC_VIA_DDRB: value = v->ddrb; break;
    case MC_VIA_DDRA: value = v->ddra; break;
    case MC_VIA_T1CL: value = (uint8_t)mc_via_t1(v); v->ifr &= (uint8_t)~MC_VIA_IRQ_T1; break;
    case MC_VIA_T1CH: value = (uint8_t)(mc_via_t1(v) >> 8); break;
    case MC_VIA_T1LL: value = (uint8_t)v->t1_latch; break;
    case MC_VIA_T1LH: value = (uint8_t)(v->t1_latch >> 8); break;
    case MC_VIA_T2CL: value = (uint8_t)mc_via_t2(v); v->ifr &= (uint8_t)~MC_VIA_IRQ_T2; break;
    case MC_VIA_T2CH: value = (uint8_t)(mc_via_t2(v) >> 8); break;
    case MC_VIA_SR: value = v->sr; sr_access(v, now); break;
    case MC_VIA_ACR: value = v->acr; break;
    case MC_VIA_PCR: value = v->pcr; break;
    case MC_VIA_IFR: value = (uint8_t)(v->ifr | ((v->ifr & v->ier & 0x7Fu) ? 0x80u : 0)); break;
    case MC_VIA_IER: value = (uint8_t)(v->ier | 0x80u); break;
    case MC_VIA_ORA_NH: value = read_pa(v); break;
    }
    finish(v);
    return value;
}

static void write_acr(mc_via *v, uint8_t value, uint64_t now) {
    const uint8_t old = v->acr;
    /* Timer 2 changing between timing and pulse counting keeps its count. */
    if ((old ^ value) & 0x20u) {
        if (value & 0x20u) v->t2_count = mc_via_t2(v);
        else {
            v->t2_n = v->t2_count;
            v->t2_start = now;
        }
    }
    v->acr = value;
    if ((old ^ value) & 0x1Cu) {
        v->sr_running = 0;
        if (sr_drives_cb2(v)) set_cb2_out(v, (uint8_t)(v->sr >> 7));
        else pcr_outputs(v);
    }
    update_ports(v);
}

static void via_write(void *dev, uint32_t offset, unsigned size, uint32_t value32) {
    (void)size;
    mc_via *v = dev;
    const uint64_t now = mc_dev_now(&v->dev);
    const uint8_t value = (uint8_t)value32;
    sync(v, now);
    switch ((offset >> v->shift) & 15u) {
    case MC_VIA_ORB:
        v->orb = value;
        port_b_flags(v);
        if (cb2_mode(v) == 4 && !sr_drives_cb2(v)) set_cb2_out(v, 0);
        else if (cb2_mode(v) == 5 && !sr_drives_cb2(v)) {
            set_cb2_out(v, 0);
            set_cb2_out(v, 1);
        }
        update_ports(v);
        break;
    case MC_VIA_ORA: v->ora = value; port_a_access(v); update_ports(v); break;
    case MC_VIA_DDRB: v->ddrb = value; update_ports(v); break;
    case MC_VIA_DDRA: v->ddra = value; update_ports(v); break;
    case MC_VIA_T1CL:
    case MC_VIA_T1LL: v->t1_latch = (uint16_t)((v->t1_latch & 0xFF00u) | value); break;
    case MC_VIA_T1CH:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FFu) | value << 8);
        v->t1_n = v->t1_latch;
        v->t1_start = now;
        v->t1_armed = 1;
        v->ifr &= (uint8_t)~MC_VIA_IRQ_T1;
        if (v->acr & 0x80u) {
            v->pb7 = 0;
            update_ports(v);
        }
        break;
    case MC_VIA_T1LH:
        v->t1_latch = (uint16_t)((v->t1_latch & 0x00FFu) | value << 8);
        v->ifr &= (uint8_t)~MC_VIA_IRQ_T1;
        break;
    case MC_VIA_T2CL: v->t2_latch_lo = value; break;
    case MC_VIA_T2CH:
        v->t2_n = (uint16_t)(value << 8 | v->t2_latch_lo);
        v->t2_count = v->t2_n;
        v->t2_start = now;
        v->t2_armed = 1;
        v->ifr &= (uint8_t)~MC_VIA_IRQ_T2;
        break;
    case MC_VIA_SR:
        v->sr = value;
        sr_access(v, now);
        if (sr_drives_cb2(v)) set_cb2_out(v, (uint8_t)(value >> 7));
        break;
    case MC_VIA_ACR: write_acr(v, value, now); break;
    case MC_VIA_PCR: v->pcr = value; pcr_outputs(v); break;
    case MC_VIA_IFR: v->ifr &= (uint8_t)~(value & 0x7Fu); break;
    case MC_VIA_IER:
        if (value & 0x80u) v->ier |= value & 0x7Fu;
        else v->ier &= (uint8_t)~(value & 0x7Fu);
        break;
    case MC_VIA_ORA_NH: v->ora = value; update_ports(v); break;
    }
    finish(v);
}

/* --- input pins --------------------------------------------------------------- */

void mc_via_set_pa(mc_via *v, uint8_t value, uint8_t mask) {
    v->pa_in = (uint8_t)((v->pa_in & ~mask) | (value & mask));
}

void mc_via_set_pb(mc_via *v, uint8_t value, uint8_t mask) {
    const uint8_t old = v->pb_in;
    v->pb_in = (uint8_t)((v->pb_in & ~mask) | (value & mask));
    /* Timer 2 pulse counting: negative edges on PB6 (R6522 data sheet, Figure 19). */
    if (t2_pulses(v) && (old & 0x40u) && !(v->pb_in & 0x40u)) {
        if (v->t2_count == 0 && v->t2_armed) {
            v->ifr |= MC_VIA_IRQ_T2;
            v->t2_armed = 0;
        }
        v->t2_count--;
        update_irq(v);
    }
}

static int active_edge(int old, int level, int positive) { return positive ? (!old && level) : (old && !level); }

void mc_via_set_ca1(mc_via *v, int level) {
    level = level != 0;
    const int old = v->ca1;
    v->ca1 = (uint8_t)level;
    if (!active_edge(old, level, v->pcr & 0x01u)) return;
    sync(v, mc_dev_now(&v->dev));
    v->ifr |= MC_VIA_IRQ_CA1;
    v->ira = (uint8_t)((v->ora & v->ddra) | (v->pa_in & ~v->ddra));
    if (ca2_mode(v) == 4) set_ca2_out(v, 1);
    finish(v);
}

void mc_via_set_ca2(mc_via *v, int level) {
    level = level != 0;
    const int old = v->ca2;
    v->ca2 = (uint8_t)level;
    if (ca2_mode(v) >= 4 || !active_edge(old, level, v->pcr & 0x04u)) return;
    sync(v, mc_dev_now(&v->dev));
    v->ifr |= MC_VIA_IRQ_CA2;
    finish(v);
}

void mc_via_set_cb1(mc_via *v, int level) {
    level = level != 0;
    const int old = v->cb1;
    v->cb1 = (uint8_t)level;
    if (old == level) return;
    sync(v, mc_dev_now(&v->dev));
    sr_cb1_edge(v, level);
    if (active_edge(old, level, v->pcr & 0x10u)) {
        v->ifr |= MC_VIA_IRQ_CB1;
        v->irb = (uint8_t)((v->orb & v->ddrb) | (v->pb_in & ~v->ddrb));
        if (cb2_mode(v) == 4 && !sr_drives_cb2(v)) set_cb2_out(v, 1);
    }
    finish(v);
}

void mc_via_set_cb2(mc_via *v, int level) {
    level = level != 0;
    const int old = v->cb2;
    v->cb2 = (uint8_t)level;
    if (cb2_mode(v) >= 4 || sr_drives_cb2(v) || !active_edge(old, level, v->pcr & 0x40u)) return;
    sync(v, mc_dev_now(&v->dev));
    v->ifr |= MC_VIA_IRQ_CB2;
    finish(v);
}

uint8_t mc_via_shift8(mc_via *v, uint8_t in) {
    uint8_t out = 0;
    const unsigned mode = mc_via_sr_mode(v);
    if (mode == 3 || mode == 7) {
        /* This protocol helper delivers a completed serial byte, raising
         * IFR2. Its bit clocks are not separate control-line events.
         * PMU requests still use mc_via_set_cb1: ApplePMU keeps that
         * interrupt enabled during transfers, and spurious control edges
         * make Heathrow latch a new request for every acknowledged byte. */
        sync(v, mc_dev_now(&v->dev));
        for (int i = 7; i >= 0; --i) {
            sr_cb1_edge(v, 0);
            out = (uint8_t)(out << 1 | v->cb2_out);
            v->cb2 = (uint8_t)((in >> i) & 1);
            sr_cb1_edge(v, 1);
        }
        finish(v);
    } else {
        for (int i = 7; i >= 0; --i) {
            mc_via_set_cb1(v, 0);
            out = (uint8_t)(out << 1 | v->cb2_out);
            mc_via_set_cb2(v, (in >> i) & 1);
            mc_via_set_cb1(v, 1);
        }
    }
    return out;
}

/* --- life cycle --------------------------------------------------------------- */

static void via_reset(void *dev) {
    mc_via *v = dev;
    /* RES clears every register except the timer latches and counters and
     * the shift register (R6522 data sheet, "Reset"). */
    v->ora = v->orb = v->ddra = v->ddrb = v->acr = v->pcr = v->ifr = v->ier = 0;
    v->t1_armed = v->t2_armed = 0;
    v->sr_running = 0;
    v->sr_count = 0;
    v->pb7 = 1;
    v->ca2_out = v->cb2_out = 1;
    v->pa_out = v->pb_out = 0;      /* forces the first report */
    update_ports(v);
    update_irq(v);
    v->next_event = MC_NEVER;
    mc_dev_schedule(&v->dev, MC_NEVER);
}

/* IFR, IER, the data directions, the latches, ACR and PCR read without side
 * effects. IFR's timer and shift flags rise at a scheduled event only while
 * their interrupt is enabled (or PB7 runs); otherwise a read finds them
 * when it comes, so the IFR is stable only when no such flag is pending. */
static int via_stable(void *dev, uint32_t offset, unsigned size) {
    const mc_via *v = dev;
    (void)size;
    switch ((offset >> v->shift) & 15u) {
    case MC_VIA_DDRB: case MC_VIA_DDRA: case MC_VIA_T1LL: case MC_VIA_T1LH: case MC_VIA_ACR: case MC_VIA_PCR: case MC_VIA_IER:
        return 1;
    case MC_VIA_IFR:
        if (v->t1_armed && !(v->ier & MC_VIA_IRQ_T1) && !(v->acr & 0x80u)) return 0;
        if (v->t2_armed && !t2_pulses(v) && !(v->ier & MC_VIA_IRQ_T2)) return 0;
        if (v->sr_running && !(v->ier & MC_VIA_IRQ_SR)) return 0;
        return 1;
    default:
        return 0;
    }
}

const mc_dev_ops mc_via_ops = {via_read, via_write, via_event, via_reset, via_stable};

void mc_via_set_ifr(mc_via *via, uint8_t bits, int on) {
    if (on) via->ifr |= (uint8_t)(bits & 0x7Fu);
    else via->ifr &= (uint8_t)~bits;
    update_irq(via);
}

void mc_via_init(mc_via *v, const mc_hw_host *host, const char *name, unsigned shift) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_via_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->shift = shift;
    v->pa_in = v->pb_in = 0xFFu;
    v->ca1 = v->ca2 = v->cb1 = v->cb2 = 1;
    v->ca2_out = v->cb2_out = 1;
    v->pb7 = 1;
    v->next_event = MC_NEVER;
}
