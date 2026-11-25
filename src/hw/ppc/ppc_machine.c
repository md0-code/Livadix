/* Livadix — the PowerPC machine back end (ppc_machine.h).
 *
 * Guest time is C15M periods (hw.h) derived from processor clocks: each
 * instruction takes the description's `cpi` clocks at emulation.cpu_mhz,
 * time = clocks x 15.6672 MHz / rate, kept exactly with a remainder, plus
 * the bus time device regions charge (bus.h). A run is cut into slices at
 * the earliest device deadline (sched.h), as on the 68k machines
 *; a deadline armed during a slice shortens it.
 *
 * The time base and decrementer count guest time: the time base input
 * (the 601's RTC oscillator, MPC601UM 2.2.5.3; a quarter of the bus clock
 * on the 603, 604 and 750, MPC603EUM/MPC604EUM/MPC750UM 2.1.2) ticks
 * floor(time x timebase_hz / C15M) times by guest time `time`, delivered to
 * the CPU (ppc_clock) when the next tick comes due.
 *
 * With emulation.jit the translator (src/core/ppc/ppc_jit.h) runs the steps
 * it can, up to the slice's end and the decrementer's next tick, and stops
 * after any device access; the per-step bookkeeping follows each of its
 * runs, and the interpreter makes the steps it declines.
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "ppc_machine.h"
#include "m68k_accel.h"
#include "../../core/disk.h"
#include "../../core/ppc/ppc_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(_M_X64)
#include "../../core/wide.h"
#endif

static const mc_ppc_desc *const descriptions[] = {&mc_pdm_desc,      &mc_pdm6100_desc, &mc_pdm7100_desc, &mc_pdm8100_desc,
                                                   &mc_pdm6150_desc, &mc_pdm8150_desc, &mc_pdm7100_80_desc,
                                                   &mc_pdm8100_100_desc, &mc_pdm8100_110_desc,
                                                   &mc_pdm9150_80_desc, &mc_pdm9150_120_desc,
                                                   &mc_pci7200_desc, &mc_pci7500_desc, &mc_pci7600_desc, &mc_pci8500_desc,
                                                   &mc_pci8600_desc, &mc_pci9500_desc, &mc_pci9600_desc, &mc_pci7300_desc,
                                                   &mc_capella5200_desc,
                                                   &mc_capella5300_desc, &mc_capella6200_desc, &mc_capella6300_desc, &mc_g3_desc,
                                                   &mc_psx5400_desc, &mc_psx6400_desc,
                                                   &mc_psx5500_desc, &mc_psx6500_desc, &mc_psx_tam_desc,
                                                   &mc_pbx2300_desc, &mc_pbx1400c_desc, &mc_pbx1400cs_desc,
                                                   &mc_pippin_desc, &mc_wallstreet_desc, &mc_pdq_desc};

const mc_ppc_desc *mc_ppc_find(const char *model_key) {
    if (!model_key) return NULL;
    for (size_t i = 0; i < sizeof descriptions / sizeof *descriptions; i++)
        for (const char *const *p = descriptions[i]->models; p && *p; p++)
            if (!strcmp(*p, model_key)) return descriptions[i];
    return NULL;
}

const mc_ppc_desc *mc_ppc_find_board(const char *model_key) {
    return mc_ppc_find(model_key);
}

/* --- guest time ------------------------------------------------------------------- */

/* a x b / d and its remainder, exactly (the quotient below 2^64). */
static uint64_t muldiv(uint64_t a, uint64_t b, uint64_t d, uint64_t *rem) {
#if defined(_MSC_VER) && defined(_M_X64)
    uint64_t hi, r;
    const uint64_t lo = _umul128(a, b, &hi);
    const uint64_t q = _udiv128(hi, lo, d, &r);
#elif defined(_MSC_VER)
    uint64_t hi, r;
    const uint64_t lo = mc_mul128(a, b, &hi);
    const uint64_t q = mc_div128(hi, lo, d, &r);
#else
    const unsigned __int128 p = (unsigned __int128)a * b;
    const uint64_t q = (uint64_t)(p / d), r = (uint64_t)(p % d);
#endif
    if (rem) *rem = r;
    return q;
}

/* Guest time since `time`, and its new remainder. */
static uint64_t elapsed(const mc_ppc *m, uint64_t *rem) {
    uint64_t r, q = muldiv(m->cycles - m->cyc, MC_C15M_HZ, m->rate, &r);
    r += m->rem;
    q += r / m->rate;
    if (rem) *rem = r % m->rate;
    return q;
}

uint64_t mc_ppc_now(mc_ppc *m) { return m->time + elapsed(m, NULL); }

static void rebase(mc_ppc *m) {
    m->time += elapsed(m, &m->rem);
    m->cyc = m->cycles;
}

/* Clocks after `cyc` until guest time reaches `when`. */
static uint64_t cycles_until(const mc_ppc *m, uint64_t when) {
    if (when == MC_NEVER) return UINT64_MAX;
    if (when <= m->time) return 0;
    const uint64_t ticks = when - m->time;
    if (ticks > UINT64_MAX / m->rate) return UINT64_MAX;
    const uint64_t need = ticks * m->rate;
    return need <= m->rem ? 0 : (need - m->rem + MC_C15M_HZ - 1) / MC_C15M_HZ;
}

static void sync_timebase(mc_ppc *m) {
    const uint64_t hz = m->desc->timebase_hz;
    const uint64_t target = muldiv(mc_ppc_now(m), hz, MC_C15M_HZ, NULL);
    while (target > m->tb_ticks) {
        uint64_t n = target - m->tb_ticks;
        if (n > 0x40000000u) n = 0x40000000u;
        ppc_clock(&m->cpu, (uint32_t)n);
        m->tb_ticks += n;
    }
    uint64_t r;
    uint64_t next = muldiv(m->tb_ticks + 1, MC_C15M_HZ, hz, &r);
    if (r) next++;
    const uint64_t until = cycles_until(m, next);
    m->tb_due = until > UINT64_MAX - m->cyc ? UINT64_MAX : m->cyc + until;
}

/* The processor clock count at which the tick that makes the decrementer
 * exception pending comes due (sync_timebase delivers it at the first step
 * ending there or later); UINT64_MAX: none within reach. The translator
 * leaves the time base to the machine, so a run of it ends there. */
static uint64_t decrementer_due(const mc_ppc *m) {
    const uint64_t ticks = ppc_decrementer_ticks(&m->cpu);
    if (ticks > (UINT64_C(1) << 40) || m->tb_ticks > (UINT64_C(1) << 46)) return UINT64_MAX;
    uint64_t r;
    uint64_t when = muldiv(m->tb_ticks + ticks, MC_C15M_HZ, m->desc->timebase_hz, &r);
    if (r) when++;
    const uint64_t until = cycles_until(m, when);
    return until > UINT64_MAX - m->cyc ? UINT64_MAX : m->cyc + until;
}

/* The steps the translator may run from here: to the slice's end and the
 * decrementer's tick, each step `cpi` clocks. */
static uint64_t jit_budget(const mc_ppc *m) {
    uint64_t limit = m->slice_end;
    const uint64_t due = decrementer_due(m);
    if (due < limit) limit = due;
    return limit > m->cycles ? (limit - m->cycles + m->desc->cpi - 1) / m->desc->cpi : 0;
}

/* A translated run's device access: the clock count of the accessing step
 * before it, as the interpreter's loop has it. */
static void jit_clock(void *ctx, uint64_t steps) {
    mc_ppc *m = ctx;
    m->cycles = m->jit_start + steps * m->desc->cpi;
    /* The interpreter delivered all ticks before this accessing step.
     * In particular, a device can reset the CPU here: pre-reset ticks
     * must not subsequently reach its new decrementer/time base. */
    if (m->cycles >= m->tb_due) sync_timebase(m);
}

/* Bus time (mc_bus_region.cost) passes in guest time; a deadline it
 * reaches ends the slice at this instruction. */
static void take_cost(mc_ppc *m) {
    m->time += m->bus.cost_due;
    m->bus.cost_due = 0;
    m->tb_due = m->cycles;
    if (m->running && mc_sched_next(&m->sched) <= mc_ppc_now(m)) m->slice_end = m->cycles;
}

/* --- the host the chips see ------------------------------------------------------- */

static uint64_t host_now(void *ctx) { return mc_ppc_now(ctx); }

static void host_schedule(void *ctx, void *dev, uint64_t when) {
    mc_ppc *m = ctx;
    int slot = mc_sched_slot(&m->sched, dev);
    if (slot < 0) slot = mc_sched_add(&m->sched, dev, ((const mc_dev *)dev)->ops->event);
    mc_sched_set(&m->sched, slot, when);
    if (m->running && when != MC_NEVER) {
        /* A deadline inside the current slice ends the slice there. */
        uint64_t target = m->cyc + cycles_until(m, when);
        if (target <= m->cycles) target = m->cycles + 1;
        if (target < m->slice_end) m->slice_end = target;
    }
}

static void host_line(void *ctx, void *dev, int line, int state) {
    mc_ppc *m = ctx;
    if (m->board && m->desc->ops && m->desc->ops->line) m->desc->ops->line(m->board, dev, line, state);
}

static int host_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    return mc_ppc_dma(ctx, address, data, length, write);
}

void mc_ppc_set_interrupt(mc_ppc *m, int asserted) {
    m->interrupt = asserted != 0;
    m->cpu.external_irq = m->interrupt != 0;
}

void mc_ppc_power_off(mc_ppc *m) {
    m->powered_off = 1;
    m->stop = 1;
}

/* --- the CPU's bus -------------------------------------------------------------------- */

/* No page or window answers: a transfer error (TEA), which the CPU takes as
 * a machine check (MPC601UM 5.4.2, 9.4.3.2). The first one is kept. */
static bool unanswered(mc_ppc *m, uint32_t address, unsigned size, int write, uint32_t value) {
    if (!m->probe.hit) {
        m->probe.hit = 1;
        m->probe.address = address;
        m->probe.size = size;
        m->probe.write = write;
        m->probe.value = value;
        m->probe.pc = m->cpu.last_pc;
        /* The PC moves past the instruction only after its fetch. */
        m->probe.fetch = !write && m->cpu.pc == m->cpu.last_pc;
        if (m->desc->unmapped == MC_PPC_UNMAPPED_STOP) m->stop = 1;
    }
    return false;
}

static bool cpu_read(void *ctx, uint32_t address, unsigned size, uint32_t *value) {
    mc_ppc *m = ctx;
    if (!mc_bus_read(&m->bus, address, size, value)) {
        if (!m->bus.read_map[address >> MC_BUS_PAGE_SHIFT]) {
            m->poll_pc = m->cpu.last_pc; m->poll_address = address;
            m->poll_value = *value; m->poll_size = size;
        }
        return true;
    }
    return unanswered(m, address, size, 0, 0);
}

static bool cpu_write(void *ctx, uint32_t address, unsigned size, uint32_t value) {
    mc_ppc *m = ctx;
    if (!mc_bus_write(&m->bus, address, size, value)) return true;
    return unanswered(m, address, size, 1, value);
}

/* --- building ---------------------------------------------------------------------- */

void mc_ppc_destroy(mc_ppc *m) {
    if (!m) return;
    mc_m68k_accel_destroy(m->accel);
    ppc_jit_destroy(m->jit);
    if (m->board && m->desc->ops && m->desc->ops->destroy) m->desc->ops->destroy(m->board);
    char nv_error[MC_CONFIG_PATH+128];
    if(!mc_nvram_end(m->nvram_session,mc_ppc_now(m)>0,nv_error,sizeof nv_error))fprintf(stderr,"%s\n",nv_error);
    mc_media_close(&m->media);
    mc_bus_free(&m->bus);
    free(m->rom);
    free(m->ram);
    free(m);
}

size_t mc_ppc_ram_sizes(const mc_ppc_desc *desc, uint32_t *kb, size_t max) {
    size_t n = 0;
    for (const uint16_t *r = desc ? desc->ram_mb : NULL; r && *r && n < max; r++) kb[n++] = (uint32_t)*r * 1024u;
    return n;
}

int mc_ppc_region(const mc_ppc *m, const char *name) {
    for (size_t i = 0; i < m->desc->nmap && i < 32; i++)
        if (!strcmp(m->desc->map[i].name, name)) return m->region[i];
    return 0;
}

static uint32_t rom_checksum(const uint8_t *rom) {
    return (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
}

mc_ppc *mc_ppc_create(const mc_ppc_desc *desc, const mc_config *c, const uint8_t *rom, size_t rom_size, char *error, size_t capacity) {
    const mc_model *p = mc_model_entry(c->model);
    int served = 0;
    for (const char *const *k = desc->models; p && k && *k; k++) served |= !strcmp(*k, p->key);
    const char *name = served ? p->name : desc->name;
    if (!rom || rom_size < 4096 || (rom_size & (rom_size - 1)) || rom_size > 0x10000000u) {
        snprintf(error, capacity, "The ROM image is not a whole power-of-two size");
        return NULL;
    }
    int known = 0;
    for (const uint32_t *r = desc->roms; *r; r++) known |= *r == rom_checksum(rom);
    if (!known) {
        snprintf(error, capacity, "%s does not run ROM %08X", name, (unsigned)rom_checksum(rom));
        return NULL;
    }
    int legal = 0;
    for (const uint16_t *r = desc->ram_mb; r && *r; r++) legal |= (uint32_t)*r * 1024u == mc_config_ram_kb(c);
    if (!legal) {
        snprintf(error, capacity, "emulation.ram_mb: the %s does not take %u KiB", name, (unsigned)mc_config_ram_kb(c));
        return NULL;
    }
    const uint64_t rate = served ? c->cpu_hz : desc->cpu_hz;
    if (rate < MC_CONFIG_MIN_HZ || rate > MC_CONFIG_MAX_HZ || !desc->timebase_hz || !desc->cpi) {
        snprintf(error, capacity, "emulation.cpu_mhz must be 1 to 10000");
        return NULL;
    }
    mc_ppc *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    m->desc = desc;
    m->model = served ? p : NULL;
    m->config = *c;
    if(!c->nvram_private){
        char base[MC_CONFIG_PATH],working[MC_CONFIG_PATH];
        const char *nvbase=mc_config_nvram_path(c,base,sizeof base);
        unsigned payload=!strncmp(desc->key,"pdm",3)||!strncmp(desc->key,"pbx",3)?273u:!strcmp(desc->key,"pippin")?131345u:8465u;
        if(!mc_nvram_begin(c,nvbase,rom_checksum(rom),payload,&m->nvram_session,working,sizeof working,error,capacity)){free(m);return NULL;}
        snprintf(m->config.nvram,sizeof m->config.nvram,"%s",working);m->config.nvram_private=1;
    }
    m->div = desc->cpi_div ? desc->cpi_div : 1;
    m->rate = rate * m->div; /* time counts in 1/div clocks */
    m->rom_size = (uint32_t)rom_size;
    m->rom = malloc(rom_size);
    m->ram_size = mc_config_ram_kb(c) << 10;
    m->ram = calloc(1, m->ram_size);
    mc_media_init(&m->media, c);
    if (!m->rom || !m->ram || !mc_bus_init(&m->bus)) {
        snprintf(error, capacity, "Out of memory");
        mc_ppc_destroy(m);
        return NULL;
    }
    memcpy(m->rom, rom, rom_size);
    snprintf(m->description, sizeof m->description, "%s", name);
    m->host.ctx = m;
    m->host.now = host_now;
    m->host.schedule = host_schedule;
    m->host.line = host_line;
    m->host.dma = host_dma;
    mc_sched_init(&m->sched);
    for (size_t i = 0; i < desc->nmap && i < 32; i++) {
        const mc_region *r = &desc->map[i];
        switch (r->kind) {
        case MC_REGION_RAM:
            m->region[i] = mc_bus_map_memory(&m->bus, r->base, m->ram_size < r->size ? m->ram_size : (uint32_t)r->size, m->ram, m->ram_size, 1);
            break;
        case MC_REGION_ROM:
            m->region[i] = mc_bus_map_memory(&m->bus, r->base, (uint32_t)r->size, m->rom, m->rom_size, 0);
            break;
        case MC_REGION_IO:
            m->region[i] = mc_bus_add_io(&m->bus, r->base, r->size, r->mask, 0);
            break;
        default:
            break;
        }
    }
    ppc_bus bus = {m, cpu_read, cpu_write};
    ppc_init_model(&m->cpu, bus, (ppc_model)desc->cpu);
    if (desc->build) {
        m->board = desc->build(m);
        if (!m->board) {
            snprintf(error, capacity, "%s", m->error[0] ? m->error : "The machine could not be built");
            mc_ppc_destroy(m);
            return NULL;
        }
    }
    if (m->board && desc->ops && desc->ops->reset) desc->ops->reset(m->board);
    if (c->jit) {
        m->jit = ppc_jit_create(&m->cpu, m->bus.read_map, m->bus.write_map, &m->bus.generation);
        ppc_jit_set_clock(m->jit, jit_clock, m);
        /* emulation.m68k_accelerator: it needs the translator, and runs only
         * on a ROM whose 68k emulator it knows (m68k_accel.c). */
        if (c->m68k_accelerator) mc_ppc_m68k_accel(m, 1);
    }
    return m;
}

int mc_ppc_m68k_accel(mc_ppc *m, int on) {
    if (!on) {
        if (m->accel) ppc_jit_trap(m->jit, 0, 0, NULL);
        mc_m68k_accel_destroy(m->accel);
        m->accel = NULL;
        return 1;
    }
    if (!m->accel) m->accel = mc_m68k_accel_create(m);
    return m->accel != NULL;
}

uint64_t mc_ppc_accel_resume(mc_ppc *m, uint64_t steps) {
    m->cycles = m->jit_start + steps * m->desc->cpi;
    if (m->bus.cost_due) take_cost(m);
    if (m->cycles >= m->tb_due) sync_timebase(m);
    if (m->stop || m->powered_off || m->reset_held || m->cpu.result == PPC_UNIMPLEMENTED || m->cpu.result == PPC_CHECKSTOP) return 0;
    if ((m->cpu.msr & PPC_MSR_EE) && (m->cpu.external_irq || m->cpu.decrementer_pending)) return 0;
    return m->cycles < m->slice_end ? jit_budget(m) : 0;
}

/* A stationary device poll, proven on the reference CPU. Only
 * register arithmetic, direct branches, barriers and lwbrx are accepted.
 * The one device read must still equal its last value until the next event;
 * no write, time register, interrupt or breakpoint is passed over. */
typedef struct poll_probe {
    mc_ppc *m;
    unsigned reads;
} poll_probe;
static bool poll_read(void *ctx, uint32_t address, unsigned size, uint32_t *value) {
    poll_probe *p = ctx;
    if (address == p->m->poll_address && size == p->m->poll_size) {
        *value = p->m->poll_value; ++p->reads; return true;
    }
    return mc_bus_peek(&p->m->bus, address, size, value) != 0;
}
static bool poll_write(void *ctx, uint32_t address, unsigned size, uint32_t value) {
    (void)ctx; (void)address; (void)size; (void)value; return false;
}
static int poll_loop(mc_ppc *m, uint64_t next) {
    ppc_cpu *c = &m->cpu;
    if (m->poll_disabled || m->poll_pc + 4u != c->pc || m->poll_size != 4 ||
        (c->msr & (PPC_MSR_EE | PPC_MSR_PR | PPC_MSR_SE | PPC_MSR_BE)) ||
        ppc_little_endian(c)) return 0;
    if (c->model == PPC_MODEL_601) {
        const unsigned mode = (c->hid[1] >> 28) & 7u, response = (c->hid[1] >> 22) & 3u;
        if (((response == 1u || response == 2u) && (mode == 2u || mode == 6u || mode == 7u)) || (c->hid[5] & 3u)) return 0;
    } else if ((c->iabr & 2u) || (c->dabr & 3u)) return 0;
    uint64_t cost;
    if (!mc_bus_read_stable(&m->bus, m->poll_address, 4, &cost)) return 0;
    poll_probe context = {m, 0};
    ppc_cpu probe = *c;
    probe.bus = (ppc_bus){&context, poll_read, poll_write};
    unsigned length = 0;
    do {
        uint32_t op, pa;
        ppc_cpu fetch = probe;
        if (length == 16 || !ppc_translate(&fetch, probe.pc, PPC_FETCH, &pa) ||
            !mc_bus_peek(&m->bus, pa, 4, &op)) return 0;
        const unsigned primary = op >> 26;
        if (!(primary == 14 || primary == 15 || primary == 16 || primary == 18 ||
              (primary >= 24 && primary <= 29) || op == 0x7C0006ACu ||
              (op & 0xFC0007FFu) == 0x7C00042Cu)) return 0;
        if (ppc_step(&probe) != PPC_OK) return 0;
        ++length;
    } while (probe.pc != c->pc);
    if (context.reads != 1) return 0;
    /* A whole turn must restore every architectural field. Last-instruction
     * metadata and the instruction counter are accounted for below. */
    ppc_cpu initial = *c, final = probe;
    final.bus = initial.bus; final.instructions = initial.instructions;
    final.last_pc = initial.last_pc; final.last_opcode = initial.last_opcode;
    final.last_exception = initial.last_exception;
    if (memcmp(&initial, &final, sizeof initial)) return 0;
    const uint64_t per = length * m->desc->cpi;
    uint64_t lo = 0, hi = (m->slice_end - m->cycles) / per;
    /* Leave the iteration that reaches a device event to the normal CPU. */
    while (lo < hi) {
        const uint64_t k = lo + (hi - lo + 1u) / 2u;
        uint64_t r, t = muldiv(m->cycles - m->cyc + k * per, MC_C15M_HZ, m->rate, &r);
        t += (r + m->rem) / m->rate;
        if (next == MC_NEVER || m->time + t + k * cost < next) lo = k;
        else hi = k - 1u;
    }
    if (!lo) return 0;
    m->time += lo * cost;
    m->cycles += lo * per;
    c->instructions += lo * length;
    c->last_pc = probe.last_pc; c->last_opcode = probe.last_opcode; c->last_exception = probe.last_exception;
    m->poll_steps += lo * length;
    sync_timebase(m);
    return 1;
}

/* --- running ------------------------------------------------------------------------ */

static mc_run_status fail(mc_ppc *m, ppc_result r) {
    const mc_ppc_probe *q = &m->probe;
    m->failed = 1;
    const int probe = m->stop && q->hit && m->desc->unmapped == MC_PPC_UNMAPPED_STOP;
    if (probe)
        snprintf(m->error, sizeof m->error, "First hardware probe: %s of %u byte%s at %08X by the instruction at %08X: no device answers",
                 q->fetch ? "instruction fetch" : q->write ? "write" : "read", q->size, q->size == 1 ? "" : "s",
                 (unsigned)q->address, (unsigned)q->pc);
    else if (r == PPC_UNIMPLEMENTED)
        snprintf(m->error, sizeof m->error, "Opcode %08X at %08X is not implemented", (unsigned)m->cpu.last_opcode, (unsigned)m->cpu.pc);
    else
        snprintf(m->error, sizeof m->error, "The processor entered checkstop at %08X", (unsigned)m->cpu.last_pc);
    if (probe && q->write) {
        const size_t used = strlen(m->error);
        snprintf(m->error + used, sizeof m->error - used, " (value %0*X)", (int)(2 * q->size), (unsigned)q->value);
    }
    return MC_RUN_ERROR;
}

mc_run_status mc_ppc_run(mc_ppc *m, uint64_t cycles, uint64_t deadline) {
    uint64_t done = 0;
    ppc_result r = PPC_OK;
    if (m->powered_off) return MC_RUN_HALTED;
    if (m->failed) return MC_RUN_ERROR;
    cycles = cycles > UINT64_MAX / m->div ? UINT64_MAX : cycles * m->div;
    while (done < cycles) {
        rebase(m);
        mc_sched_run(&m->sched, m->time);
        if (deadline && m->time >= deadline) break;
        uint64_t next = mc_sched_next(&m->sched);
        if (deadline && deadline < next) next = deadline;
        uint64_t n = cycles - done;
        const uint64_t until = cycles_until(m, next);
        if (until < n) n = until ? until : 1;
        if (n > (1u << 24)) n = 1u << 24;
        const uint64_t before = m->cycles;
        m->running = 1;
        m->slice_end = before + n;
        while (m->cycles < m->slice_end) {
            if (m->reset_held) {
                m->cycles = m->slice_end;
                sync_timebase(m);
                break;
            }
            if (ppc_power_waiting(&m->cpu)) {
                uint64_t wake = decrementer_due(m);
                if (wake > m->slice_end) wake = m->slice_end;
                m->cycles = wake > m->cycles ? wake : m->cycles + 1;
                sync_timebase(m);
                continue;
            }
            if (m->jit) {
                if (poll_loop(m, next)) continue;
                /* Translated steps: the same per-step bookkeeping, after a
                 * run that ends at any device access (ppc_jit.h). At a
                 * dispatch entry of Apple's 68k emulator the accelerator
                 * runs the 68k code itself when it can (m68k_accel.h). */
                m->jit_start = m->cycles;
                const int entry = m->accel && mc_m68k_accel_entry(m->accel, m->cpu.pc);
                uint64_t steps = entry ? mc_m68k_accel_run(m->accel, jit_budget(m)) : 0;
                /* an entry it leaves (the emulator's instruction) is translated PowerPC code like any other */
                if (!steps) steps = ppc_jit_run(m->jit, jit_budget(m));
                m->cycles = m->jit_start + steps * m->desc->cpi;
                if (steps) {
                    r = m->cpu.result;
                    if (m->bus.cost_due) take_cost(m);
                    if (m->cycles >= m->tb_due) sync_timebase(m);
                    if (r == PPC_UNIMPLEMENTED || r == PPC_CHECKSTOP || m->stop) break;
                    continue;
                }
            }
            r = ppc_step(&m->cpu);
            m->cycles += m->desc->cpi;
            if (m->bus.cost_due) take_cost(m);
            if (m->cycles >= m->tb_due) sync_timebase(m);
            if (r == PPC_UNIMPLEMENTED || r == PPC_CHECKSTOP || m->stop) break;
        }
        m->running = 0;
        done += m->cycles - before;
        if (m->powered_off) {
            mc_disk_sync(1);
            return MC_RUN_HALTED;
        }
        if (r == PPC_UNIMPLEMENTED || r == PPC_CHECKSTOP || m->stop) return fail(m, r);
    }
    rebase(m);
    mc_sched_run(&m->sched, m->time);
    if (m->board && m->desc->ops && m->desc->ops->poll) m->desc->ops->poll(m->board);
    mc_disk_sync(0);
    return MC_RUN_OK;
}

/* --- the machine interface ---------------------------------------------------------- */

const char *mc_ppc_description(const mc_ppc *m) { return m->description; }
const char *mc_ppc_error(const mc_ppc *m) { return m->error; }
uint64_t mc_ppc_cycles(const mc_ppc *m) { return m->cycles / m->div; }
uint64_t mc_ppc_instructions(const mc_ppc *m) { return m->cpu.instructions; }

/* Logical to physical as a supervisor data read sees it, without its side
 * effects: a copy of the CPU translates on a bus that refuses writes (no
 * R/C update), and its exceptions are dropped. */
static bool peek_read(void *ctx, uint32_t address, unsigned size, uint32_t *value) {
    return mc_bus_peek(ctx, address, size, value) != 0;
}
static bool peek_write(void *ctx, uint32_t address, unsigned size, uint32_t value) {
    const mc_bus *bus = ctx;
    (void)value;
    /* A debugger's copied CPU may set a PTE's R/C bits (MPC601UM
     * 6.4.1; PEM hashed-table walk). Acknowledge writable RAM without
     * changing it; refusing this made valid, unreferenced mappings
     * appear unreadable to probes such as the boot harness's box flag. */
    return size <= MC_BUS_PAGE_SIZE - (address & (MC_BUS_PAGE_SIZE - 1u)) &&
        bus->write_map[address >> MC_BUS_PAGE_SHIFT] != NULL;
}
static int physical(mc_ppc *m, uint32_t address, uint32_t *pa) {
    if (!(m->cpu.msr & PPC_MSR_DR)) {
        *pa = address;
        return 1;
    }
    ppc_cpu *copy = malloc(sizeof *copy);
    if (!copy) return 0;
    *copy = m->cpu;
    copy->bus.opaque = &m->bus;
    copy->bus.read = peek_read;
    copy->bus.write = peek_write;
    copy->msr &= ~PPC_MSR_PR;
    copy->result = PPC_OK; /* not the last step's */
    const ppc_cpu initial = *copy;
    int ok = ppc_translate(copy, address, PPC_READ, pa) && copy->result == PPC_OK;
    if (!ok && (initial.model == PPC_MODEL_603 || initial.model == PPC_MODEL_603E) &&
        copy->result == PPC_EXCEPTION && copy->last_exception == PPC_EX_DTLB_LOAD) {
        *copy = initial;
        ok = ppc_classic_debug_walk(copy, address, PPC_READ, pa) && copy->result == PPC_OK;
    }
    free(copy);
    return ok;
}
int mc_ppc_physical(mc_ppc *m, uint32_t address, uint32_t *pa) { return physical(m, address, pa); }
int mc_ppc_peek(mc_ppc *m, uint32_t address, unsigned size, uint32_t *value) {
    uint32_t pa;
    return physical(m, address, &pa) && mc_bus_peek(&m->bus, pa, size, value);
}
int mc_ppc_poke(mc_ppc *m, uint32_t address, unsigned size, uint32_t value) {
    uint32_t pa;
    if (!physical(m, address, &pa) || !m->bus.write_map[pa >> MC_BUS_PAGE_SHIFT] || mc_bus_write(&m->bus, pa, size, value)) return 0;
    ppc_jit_written(m->jit, pa, size);
    return 1;
}

#define BOARD(op) (m->board && m->desc->ops && m->desc->ops->op)

void mc_ppc_set_clock(mc_ppc *m, int64_t host_seconds) {
    if (BOARD(set_clock)) m->desc->ops->set_clock(m->board, host_seconds);
}
uint32_t mc_ppc_card_blits(mc_ppc *m) { return BOARD(card_blits) ? m->desc->ops->card_blits(m->board) : 0; }
int mc_ppc_framebuffer(mc_ppc *m, mc_framebuffer *fb) {
    fb->native_depth = fb->native_row_bytes = 0;
    return BOARD(framebuffer) ? m->desc->ops->framebuffer(m->board, fb) : 0;
}
int mc_ppc_key(mc_ppc *m, uint8_t code, int down) { return BOARD(key) ? m->desc->ops->key(m->board, code, down) : 1; }
int mc_ppc_mouse(mc_ppc *m, int dx, int dy, int button) { return BOARD(mouse) ? m->desc->ops->mouse(m->board, dx, dy, button) : 1; }
void mc_ppc_audio_startup(mc_ppc *m, int16_t *stereo, size_t frames, int played, int drained) {
    if (!m->config.startup_sound && !m->startup_sound_done)
        memset(stereo, 0, frames * 2u * sizeof *stereo);
    if (played && drained) m->startup_sound_done = 1;
}
uint32_t mc_ppc_audio_rate(const mc_ppc *m) { return m->config.sound && BOARD(audio_rate) ? m->desc->ops->audio_rate(m->board) : 0; }
size_t mc_ppc_audio_pull(mc_ppc *m, int16_t *stereo, size_t frames) {
    const size_t n = BOARD(audio_pull) ? m->desc->ops->audio_pull(m->board, stereo, frames) : 0;
    /* The CD drive's audio output joins the machine's sound. */
    int ejected = 0;
    mc_disk *cd = mc_media_get(&m->media, MC_MEDIA_CDROM, 0, &ejected);
    const size_t mixed = mc_disk_mix(ejected ? NULL : cd, stereo, n, frames, mc_ppc_audio_rate(m));
    if (!m->config.sound) memset(stereo, 0, mixed * 2u * sizeof *stereo);
    return mixed;
}
int mc_ppc_set_audio_source(mc_ppc *m, const mc_audio_source *source) {
    if (!BOARD(audio_source)) return 0;
    m->desc->ops->audio_source(m->board, source);
    return 1;
}
void mc_ppc_set_serial(mc_ppc *m, const mc_serial_host *host) {
    if (BOARD(serial_host)) m->desc->ops->serial_host(m->board, host);
}
size_t mc_ppc_serial_receive(mc_ppc *m, unsigned port, const uint8_t *data, size_t count) {
    return BOARD(serial_receive) ? m->desc->ops->serial_receive(m->board, port, data, count) : count;
}
void mc_ppc_serial_modem(mc_ppc *m, unsigned port, int dcd, int cts) {
    if (BOARD(serial_modem)) m->desc->ops->serial_modem(m->board, port, dcd, cts);
}
void mc_ppc_set_net(mc_ppc *m, const mc_net_host *host) {
    if (BOARD(net_host)) m->desc->ops->net_host(m->board, host);
}
int mc_ppc_net_receive(mc_ppc *m, const uint8_t *frame, size_t length) {
    return BOARD(net_receive) ? m->desc->ops->net_receive(m->board, frame, length) : 1;
}
/* A CD's audio player runs on guest time, in samples of 1/44100 s. */
static uint64_t cd_clock(void *ctx) {
    const uint64_t t = mc_ppc_now(ctx);
    return t / MC_C15M_HZ * 44100u + t % MC_C15M_HZ * 44100u / MC_C15M_HZ;
}
int mc_ppc_insert(mc_ppc *m, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    if (!mc_media_insert(&m->media, kind, drive, disk, m->cpu.instructions != 0, error, capacity)) return 0;
    if (kind == MC_MEDIA_CDROM) mc_disk_set_clock(disk, cd_clock, m);
    if (BOARD(insert) && !m->desc->ops->insert(m->board, kind, drive, disk, error, capacity)) {
        mc_media_forget(&m->media, kind, drive);
        return 0;
    }
    return 1;
}
int mc_ppc_reinsert(mc_ppc *m) {
    const int kind = m->media.last_kind, drive = m->media.last_drive;
    if (!mc_media_reinsert(&m->media)) return 0;
    mc_disk *disk = mc_media_get(&m->media, (mc_media_kind)kind, (unsigned)drive, NULL);
    if (kind == MC_MEDIA_CDROM) mc_disk_set_clock(disk, cd_clock, m);
    if (BOARD(insert) && !m->desc->ops->insert(m->board, (mc_media_kind)kind, (unsigned)drive, disk, NULL, 0)) {
        mc_media_eject(&m->media, (mc_media_kind)kind, (unsigned)drive);
        return 0;
    }
    return 1;
}
int mc_ppc_control(mc_ppc *m, mc_control control, int value) {
    return BOARD(control) ? m->desc->ops->control(m->board, (int)control, value) : -1;
}
