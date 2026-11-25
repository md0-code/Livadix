/* Livadix — Apple's I/O Processor (see iop.h for sources). Livadix's own code. */
#include "iop.h"
#include "scc.h"
#include "swim.h"

#include <string.h>

/* IOP I/O page registers (iop.h). */
enum {
    R_TIMER_LO = 0x10, R_TIMER_HI = 0x11, R_LATCH_LO = 0x12, R_LATCH_HI = 0x13,
    R_CONFIG = 0x30, R_SETUP = 0x31, R_PORT = 0x32, R_IE = 0x33, R_IF = 0x34, R_HOST_INT = 0x35,
    R_CHIP = 0x40
};
enum { F_DMA1 = 0x02, F_CHIP = 0x08, F_HOST = 0x10, F_TIMER = 0x20 };
/* DMA channel n at $F020 + 8n: control (bit 0 enable, bit 2 chip to RAM),
 * address low/high, count low/high. */
enum { R_DMA = 0x20, DMA_CTL = 0, DMA_ADDR = 1, DMA_COUNT = 3, DMA_ENABLE = 0x01, DMA_TO_RAM = 0x04 };
enum { ISM_HANDSHAKE = 15, ISM_DATA_READ = 8, ISM_DATA_WRITE = 0, ISM_READY = 0x80 };
enum { PORT_OUT = 0x03, PORT_IN = 0x04 };
enum { ISM_MODE_HDSEL = 0x20 };     /* ISM mode bit 5, HeadSelect (IOP SWIM Driver ERS; swim.c) */

static int bypass(const mc_iop *iop) { return iop->io[R_CONFIG] & 1u; }

static void update_host_irq(mc_iop *iop) {
    const int level = iop->int0 || iop->int1 || (bypass(iop) && iop->chip_irq);
    if (level != iop->irq_out) {
        iop->irq_out = level;
        mc_dev_line(&iop->dev, MC_IOP_LINE_IRQ, level);
    }
}

/* The interrupt sources pending, latched and level. */
static uint8_t pending(const mc_iop *iop) {
    return (uint8_t)(iop->flags | ((iop->chip_irq && !bypass(iop)) ? F_CHIP : 0));
}

/* Brings the timer to IOP time t: one count per 65C02 cycle, an underflow
 * past 0 reloads the latch and sets the flag. */
static void timer_sync(mc_iop *iop, uint64_t t) {
    if (t <= iop->timer_at) return;
    const uint64_t cycles = (t - iop->timer_at) / MC_IOP_CYCLE;
    iop->timer_at += cycles * MC_IOP_CYCLE;
    if (cycles <= iop->timer) {
        iop->timer = (uint16_t)(iop->timer - cycles);
    } else {
        const uint64_t period = (uint64_t)iop->latch + 1u;
        const uint64_t rest = (cycles - iop->timer - 1u) % period;
        iop->timer = (uint16_t)(iop->latch - rest);
        iop->flags |= F_TIMER;
    }
    iop->timer_next = iop->timer_at + ((uint64_t)iop->timer + 1u) * MC_IOP_CYCLE;
}

static void timer_load(mc_iop *iop) {
    iop->timer = iop->latch;
    iop->timer_at = iop->time;
    iop->timer_next = iop->time + ((uint64_t)iop->timer + 1u) * MC_IOP_CYCLE;
    iop->flags &= (uint8_t)~F_TIMER;
}

/* The chip's offset for an IOP address $F040-$F04F. */
static uint32_t chip_offset(const mc_iop *iop, uint16_t a) {
    if (iop->kind == MC_IOP_SWIM) return (uint32_t)(a & 15u) << 9;
    return (uint32_t)((a & 1u) << 1 | (a & 2u) << 1);  /* A0 channel A, A1 data */
}

/* The chip asks channel `ch` for a byte: the SWIM's handshake (ISM data
 * ready or room in the write FIFO), the SCC's /W//REQ of channel A (DMA 1)
 * or B (DMA 2)†. */
static int dma_request(mc_iop *iop, unsigned ch) {
    if (iop->kind == MC_IOP_SWIM)
        return (iop->chip_ops->read(iop->chip, (uint32_t)ISM_HANDSHAKE << 9, 1) & ISM_READY) != 0;
    return iop->wreq[ch];
}

/* Moves the bytes the chip has asked for on the enabled channels. */
static void dma_service(mc_iop *iop) {
    for (unsigned ch = 0; ch < 2; ch++) {
        uint8_t *r = &iop->io[R_DMA + 8u * ch];
        if (!(r[DMA_CTL] & DMA_ENABLE) || !iop->chip_ops) continue;
        unsigned count = (unsigned)r[DMA_COUNT] | (unsigned)r[DMA_COUNT + 1] << 8;
        if (!count) continue;
        unsigned addr = (unsigned)r[DMA_ADDR] | (unsigned)r[DMA_ADDR + 1] << 8;
        const uint32_t data = iop->kind == MC_IOP_SWIM ? 0 : (uint32_t)(ch ? 4u : 6u);    /* SCC: aData, bData */
        while (count && dma_request(iop, ch)) {
            uint8_t *m = &iop->ram[addr & (MC_IOP_RAM_SIZE - 1u)];
            if (r[DMA_CTL] & DMA_TO_RAM)
                *m = (uint8_t)iop->chip_ops->read(iop->chip, iop->kind == MC_IOP_SWIM ? (uint32_t)ISM_DATA_READ << 9 : data, 1);
            else
                iop->chip_ops->write(iop->chip, iop->kind == MC_IOP_SWIM ? (uint32_t)ISM_DATA_WRITE << 9 : data, 1, *m);
            addr = (addr + 1u) & 0xFFFFu;
            count--;
        }
        r[DMA_ADDR] = (uint8_t)addr;
        r[DMA_ADDR + 1] = (uint8_t)(addr >> 8);
        r[DMA_COUNT] = (uint8_t)count;
        r[DMA_COUNT + 1] = (uint8_t)(count >> 8);
        if (!count) iop->flags |= (uint8_t)(F_DMA1 << ch);
    }
}

static uint8_t io_read(mc_iop *iop, uint16_t a) {
    const unsigned r = a & 0xFFu;
    if (r >= R_CHIP && r < R_CHIP + 0x10u)
        return iop->chip_ops ? (uint8_t)iop->chip_ops->read(iop->chip, chip_offset(iop, a), 1) : 0;
    switch (r) {
    case R_TIMER_LO: timer_sync(iop, iop->time); return (uint8_t)iop->timer;
    case R_TIMER_HI: timer_sync(iop, iop->time); return (uint8_t)(iop->timer >> 8);
    case R_LATCH_LO: return (uint8_t)iop->latch;
    case R_LATCH_HI: return (uint8_t)(iop->latch >> 8);
    case R_PORT: {
        const uint8_t in = iop->port.input ? (uint8_t)(iop->port.input(iop->port.ctx, iop->time) & PORT_IN) : 0;
        return (uint8_t)((iop->io[R_PORT] & ~PORT_IN) | in);
    }
    case R_IE: return iop->ie;
    case R_IF: timer_sync(iop, iop->time); return (uint8_t)(pending(iop) & iop->ie);
    case R_HOST_INT: return (uint8_t)((iop->io[R_HOST_INT] & ~0x0Cu) | (iop->int0 ? 0x04u : 0) | (iop->int1 ? 0x08u : 0));
    default: return iop->io[r];
    }
}

static void io_write(mc_iop *iop, uint16_t a, uint8_t v) {
    const unsigned r = a & 0xFFu;
    if (r >= R_CHIP && r < R_CHIP + 0x10u) {
        if (!iop->chip_ops) return;
        iop->chip_ops->write(iop->chip, chip_offset(iop, a), 1, v);
        if (iop->kind == MC_IOP_SWIM) {
            /* The drive's SEL comes from the ISM's HeadSelect (SWIM ERS). */
            mc_swim *s = iop->chip;
            mc_swim_set_headsel(s, (s->ism_mode & ISM_MODE_HDSEL) != 0);
        }
        return;
    }
    const uint8_t old = iop->io[r];
    iop->io[r] = v;
    switch (r) {
    case R_TIMER_LO: case R_LATCH_LO:
        timer_sync(iop, iop->time);
        iop->latch = (uint16_t)((iop->latch & 0xFF00u) | v);
        break;
    case R_LATCH_HI:
        timer_sync(iop, iop->time);
        iop->latch = (uint16_t)((iop->latch & 0x00FFu) | (unsigned)v << 8);
        break;
    case R_TIMER_HI:
        timer_sync(iop, iop->time);
        iop->latch = (uint16_t)((iop->latch & 0x00FFu) | (unsigned)v << 8);
        timer_load(iop);
        break;
    case R_CONFIG:
        if ((old ^ v) & 1u) update_host_irq(iop);
        break;
    case R_PORT:
        if (((old ^ v) & PORT_OUT) && iop->port.output) iop->port.output(iop->port.ctx, (uint8_t)(v & PORT_OUT), iop->time);
        break;
    case R_IE: iop->ie = v; break;
    case R_IF: timer_sync(iop, iop->time); iop->flags &= (uint8_t)~v; break;
    case R_HOST_INT:
        if (v & 0x04u) iop->int0 = 1;
        if (v & 0x08u) iop->int1 = 1;
        update_host_irq(iop);
        break;
    default: break;
    }
}

static uint8_t cpu_read(void *ctx, uint16_t a) {
    mc_iop *iop = ctx;
    if ((a & 0xFF00u) == 0xF000u) return io_read(iop, a);
    return iop->ram[a & (MC_IOP_RAM_SIZE - 1u)];
}

static void cpu_write(void *ctx, uint16_t a, uint8_t v) {
    mc_iop *iop = ctx;
    if ((a & 0xFF00u) == 0xF000u) io_write(iop, a, v);
    else iop->ram[a & (MC_IOP_RAM_SIZE - 1u)] = v;
}

/* Delivers the chip's deadline once the IOP's time has reached it. */
static void chip_event(mc_iop *iop) {
    iop->chip_deadline = MC_NEVER;
    if (iop->chip_ops && iop->chip_ops->event) iop->chip_ops->event(iop->chip, iop->time);
}

void mc_iop_run(mc_iop *iop, uint64_t to) {
    iop->in_run = 1;
    while (iop->time < to || iop->chip_deadline <= to) {
        if (iop->chip_deadline <= iop->time) { chip_event(iop); continue; }
        const uint64_t stop = iop->chip_deadline < to ? iop->chip_deadline : to;
        if (!(iop->ctl & MC_IOP_RUN)) { iop->time = stop; continue; }
        if (iop->time >= iop->timer_next) timer_sync(iop, iop->time);
        mc_cpu6502_set_irq(&iop->cpu, (pending(iop) & iop->ie) != 0);
        const unsigned n = mc_cpu6502_step(&iop->cpu);
        if (!n) {
            /* WAI or STP: on to the timer's next underflow or the stop. */
            iop->time = iop->timer_next < stop ? iop->timer_next : stop;
            continue;
        }
        iop->steps++;
        iop->time += (uint64_t)n * MC_IOP_CYCLE;
        if ((iop->io[R_DMA] | iop->io[R_DMA + 8]) & DMA_ENABLE) dma_service(iop);
    }
    iop->in_run = 0;
}

/* The IOP's deadline: the chip's, or the next slice while the 65C02 runs. */
static void rearm(mc_iop *iop) {
    uint64_t next = iop->chip_deadline;
    if (iop->ctl & MC_IOP_RUN) {
        const uint64_t slice = iop->time + MC_IOP_SLICE;
        if (slice < next) next = slice;
    }
    mc_dev_schedule(&iop->dev, next);
}

static void sync(mc_iop *iop) {
    mc_iop_run(iop, mc_dev_now(&iop->dev));
    rearm(iop);
}

/* The chip's host: its time is the IOP's (never going back), its deadline
 * is delivered by the IOP's run, its lines go to the IOP. */
static uint64_t chip_now(void *ctx) {
    mc_iop *iop = ctx;
    uint64_t t = iop->time;
    if (!iop->in_run) {
        const uint64_t now = mc_dev_now(&iop->dev);
        if (now > t) t = now;
    }
    if (t < iop->chip_time) t = iop->chip_time;
    iop->chip_time = t;
    return t;
}

static void chip_schedule(void *ctx, void *dev, uint64_t when) {
    mc_iop *iop = ctx;
    (void)dev;
    iop->chip_deadline = when;
    if (!iop->in_run) rearm(iop);
}

/* The chip's outputs end on the IOP board: the SCC's interrupt and its
 * /W//REQ lines (the DMA requests); the rest is not wired. */
static void chip_line(void *ctx, void *dev, int line, int state) {
    mc_iop *iop = ctx;
    (void)dev;
    if (iop->kind != MC_IOP_SCC) return;
    if (line == MC_SCC_LINE_IRQ) mc_iop_set_chip_irq(iop, state);
    else if (line == MC_SCC_LINE_WREQ_A || line == MC_SCC_LINE_WREQ_B) iop->wreq[line == MC_SCC_LINE_WREQ_B] = state ? 1 : 0;
}

const mc_hw_host *mc_iop_chip_host(mc_iop *iop) { return &iop->chip_host; }

static uint8_t host_read_byte(mc_iop *iop, uint32_t off) {
    if (off >= MC_IOP_CHIP) {
        if (!bypass(iop) || !iop->chip_ops) return 0;
        return (uint8_t)iop->chip_ops->read(iop->chip, off - MC_IOP_CHIP, 1);
    }
    switch (off & 0x0Cu) {
    case 0x00: return (off & 2u) ? (uint8_t)iop->addr : (uint8_t)(iop->addr >> 8);
    case 0x04:
        return (uint8_t)((iop->ctl & (MC_IOP_INC | MC_IOP_RUN)) | (bypass(iop) ? MC_IOP_BYPASS : 0) |
                         (iop->int0 ? MC_IOP_INT0 : 0) | (iop->int1 ? MC_IOP_INT1 : 0) |
                         ((bypass(iop) && iop->chip_irq) ? MC_IOP_CHIP_INT : 0));
    case 0x08: {
        const uint8_t v = iop->ram[iop->addr & (MC_IOP_RAM_SIZE - 1u)];
        if (iop->ctl & MC_IOP_INC) iop->addr++;
        return v;
    }
    default: return 0;
    }
}

static void host_write_byte(mc_iop *iop, uint32_t off, uint8_t v) {
    if (off >= MC_IOP_CHIP) {
        if (bypass(iop) && iop->chip_ops) iop->chip_ops->write(iop->chip, off - MC_IOP_CHIP, 1, v);
        return;
    }
    switch (off & 0x0Cu) {
    case 0x00:
        iop->addr = (off & 2u) ? (uint16_t)((iop->addr & 0xFF00u) | v) : (uint16_t)((iop->addr & 0x00FFu) | (unsigned)v << 8);
        break;
    case 0x04: {
        const uint8_t was = iop->ctl;
        iop->ctl = (uint8_t)(v & (MC_IOP_INC | MC_IOP_RUN));
        if (v & MC_IOP_INT0) iop->int0 = 0;
        if (v & MC_IOP_INT1) iop->int1 = 0;
        if (v & MC_IOP_GEN_INT) iop->flags |= F_HOST;
        if ((iop->ctl & MC_IOP_RUN) && !(was & MC_IOP_RUN)) mc_cpu6502_reset(&iop->cpu);
        if ((iop->ctl ^ was) & MC_IOP_RUN) rearm(iop);
        update_host_irq(iop);
        break;
    }
    case 0x08:
        iop->ram[iop->addr & (MC_IOP_RAM_SIZE - 1u)] = v;
        if (iop->ctl & MC_IOP_INC) iop->addr++;
        break;
    default: break;
    }
}

static uint32_t iop_read(void *dev, uint32_t offset, unsigned size) {
    mc_iop *iop = dev;
    uint32_t v = 0;
    sync(iop);
    for (unsigned i = 0; i < size; i++) {
        const uint32_t off = offset + i;
        v = v << 8 | host_read_byte(iop, off >= MC_IOP_CHIP ? off : (off & 0x0Fu));
    }
    return v;
}

static void iop_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_iop *iop = dev;
    sync(iop);
    for (unsigned i = 0; i < size; i++) {
        const uint32_t off = offset + i;
        host_write_byte(iop, off >= MC_IOP_CHIP ? off : (off & 0x0Fu), (uint8_t)(value >> (8u * (size - 1u - i))));
    }
}

static void iop_event(void *dev, uint64_t now) {
    mc_iop *iop = dev;
    mc_iop_run(iop, now);
    rearm(iop);
}

static void iop_reset(void *dev) {
    mc_iop *iop = dev;
    iop->addr = 0;
    iop->ctl = 0;
    iop->int0 = iop->int1 = 0;
    memset(iop->io, 0, sizeof iop->io);
    iop->ie = iop->flags = 0;
    iop->timer = iop->latch = 0;
    iop->timer_at = iop->time;
    iop->timer_next = iop->time + MC_IOP_CYCLE;
    rearm(iop);
    update_host_irq(iop);
}

const mc_dev_ops mc_iop_ops = {iop_read, iop_write, iop_event, iop_reset, NULL};

void mc_iop_init(mc_iop *iop, const mc_hw_host *host, mc_iop_kind kind, void *chip, const mc_dev_ops *chip_ops) {
    memset(iop, 0, sizeof *iop);
    iop->dev.ops = &mc_iop_ops;
    iop->dev.host = host;
    iop->dev.name = kind == MC_IOP_SWIM ? "SWIM IOP" : "SCC IOP";
    iop->kind = (uint8_t)kind;
    iop->chip = chip;
    iop->chip_ops = chip_ops;
    iop->chip_deadline = MC_NEVER;
    iop->chip_host.ctx = iop;
    iop->chip_host.now = chip_now;
    iop->chip_host.schedule = chip_schedule;
    iop->chip_host.line = chip_line;
    mc_cpu6502_init(&iop->cpu, iop, cpu_read, cpu_write, MC_6502_R65C02);
    iop->time = mc_dev_now(&iop->dev);
    iop_reset(iop);
}

void mc_iop_set_port(mc_iop *iop, const mc_iop_port *port) { iop->port = *port; }

/* Not synchronised: the chip reports from inside its own event or access;
 * the level counts from the IOP's present time. */
void mc_iop_set_chip_irq(mc_iop *iop, int asserted) {
    iop->chip_irq = asserted ? 1 : 0;
    update_host_irq(iop);
}
