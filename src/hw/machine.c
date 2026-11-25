/* Livadix — the machine interface (src/core/machine.h) for machines built from
 * a description (hw_machine.h), the PowerPC machines
 * (ppc/ppc_machine.h), and the stub machine for every
 * other model.
 *
 * Guest time is a count of C15M periods derived from
 * the CPU's clock cycles (each instruction charged its cycles for the model,
 * src/core/cpu/m68k_timing.c): time = cycles x 15.6672 MHz / cpu_hz, kept
 * exactly with a remainder. A run is cut into slices that end at the
 * earliest device deadline, so a device sees its event at the guest time it
 * asked for; a device that arms an earlier deadline during a slice shortens
 * it. While the CPU is stopped (STOP) time jumps to the next deadline.
 *
 * Livadix's own code. */
#include "hw_machine.h"
#include "../core/nvram.h"
#include "../core/stub_machine.h"
#include "ppc/ppc_machine.h"
#include "ppc/m68k_accel.h"
#include "../core/disk.h"
#include "../core/cpu/m68k_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_MSC_VER) && !defined(_M_X64)
#include "../core/wide.h"
#endif
#ifdef _WIN32
#include <direct.h>
#else
#include <strings.h>
#include <sys/stat.h>
#define _mkdir(path) mkdir(path, 0777)
#define _strnicmp strncasecmp
#endif

static const mc_hw_desc *const descriptions[] = {&mc_quadra700_desc, &mc_quadra800_desc, &mc_centris610_desc, &mc_iici_desc,
                                                    &mc_quadra605_desc, &mc_quadra630_desc, &mc_lc630_desc, &mc_iisi_desc,
                                                    &mc_lc_desc, &mc_lc2_desc, &mc_classic2_desc, &mc_color_classic_desc, &mc_tv_desc,
                                                    &mc_lc3_desc, &mc_cc2_desc, &mc_iivx_desc, &mc_iivi_desc,
                                                    &mc_macii_desc, &mc_macii_fdhd_desc, &mc_iix_desc,
                                                    &mc_quadra840av_desc, &mc_quadra660av_desc,
                                                    &mc_pb140_desc, &mc_pb170_desc, &mc_pb160_desc, &mc_pb180_desc,
                                                    &mc_duo210_desc, &mc_duo270_desc, &mc_duo280_desc, &mc_pb150_desc,
                                                    &mc_pb520_desc, &mc_pb190_desc, &mc_portable_desc, &mc_pb100_desc,
                                                    &mc_quadra900_desc, &mc_quadra950_desc, &mc_iifx_desc,
                                                    &mc_mac128k_desc, &mc_mac512ke_desc, &mc_macplus_desc,
                                                    &mc_macse_desc};

const mc_hw_desc *mc_hw_find(const char *model_key) {
    if (!model_key) return NULL;
    for (size_t i = 0; i < sizeof descriptions / sizeof *descriptions; i++)
        for (const char *const *p = descriptions[i]->models; *p; p++)
            if (!strcmp(*p, model_key)) return descriptions[i];
    return NULL;
}

struct mc_machine {
    mc_stub *stub;
    mc_hw *hw;
    mc_ppc *ppc;
    mc_nvram *nvram;
    mc_drivesnd drive_sound;
    int speaker_enabled;
};

mc_hw *mc_machine_hw(mc_machine *m) { return m ? m->hw : NULL; }

int mc_machine_emulated(const mc_model *model) { return model && (mc_hw_find(model->key) || mc_ppc_find(model->key)); }
static size_t desc_ram_sizes(const mc_hw_desc *desc, uint32_t *kb, size_t max) {
    size_t n = 0;
    if (!desc) return 0;
    for (const uint16_t *r = desc->ram_mb; r && *r && n < max; r++) kb[n++] = (uint32_t)*r * 1024u;
    for (const uint32_t *r = desc->ram_kb; r && *r && n < max; r++) kb[n++] = *r;
    return n;
}

size_t mc_machine_ram_sizes(const mc_model *model, uint32_t *kb, size_t max) {
    if (model && mc_ppc_find(model->key)) return mc_ppc_ram_sizes(mc_ppc_find(model->key), kb, max);
    return desc_ram_sizes(model ? mc_hw_find(model->key) : NULL, kb, max);
}

void mc_machine_video(const mc_model *model, unsigned w, unsigned h, unsigned *builtin, unsigned *card) {
    const mc_hw_desc *desc = model ? mc_hw_find(model->key) : NULL;
    const mc_ppc_desc *ppc = model ? mc_ppc_find(model->key) : NULL;
    unsigned fits = 0;
    for (unsigned d = 1; d <= 32; d *= 2)
        if ((uint64_t)w * h * d / 8 <= MC_CONFIG_VRAM) fits |= MC_DEPTH_BIT(d);
    if (ppc) {
        *builtin = ppc->caps && ppc->caps->builtin ? ppc->caps->builtin(model, w, h) : 0;
        *card = ppc->caps && ppc->caps->card ? fits : 0;
        return;
    }
    if (!desc || !desc->caps) { *builtin = fits; *card = 0; return; }
    *builtin = desc->caps->builtin ? desc->caps->builtin(model, w, h) : 0;
    *card = desc->caps->card ? fits : 0;
}
unsigned mc_machine_features(const mc_model *model) {
    const mc_hw_desc *desc = model ? mc_hw_find(model->key) : NULL;
    const mc_ppc_desc *ppc = model ? mc_ppc_find(model->key) : NULL;
    if (ppc) return !ppc->caps ? 0 : ppc->caps->features_of ? ppc->caps->features_of(model) : ppc->caps->features;
    if (!desc || !desc->caps) return 0;
    return desc->caps->features_of ? desc->caps->features_of(model) : desc->caps->features;
}

/* --- guest time ------------------------------------------------------------------- */

/* n / rate, exactly: the high half of n x floor((2^64 - 1) / rate) is the
 * quotient or falls short of it by one or two, which the remainder test
 * corrects (division by an invariant through multiplication). */
static uint64_t mulhi64(uint64_t a, uint64_t b) {
#if defined(_MSC_VER) && defined(_M_X64)
    return __umulh(a, b);
#elif defined(_MSC_VER)
    uint64_t hi;
    (void)mc_mul128(a, b, &hi);
    return hi;
#else
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
#endif
}
static uint64_t div_rate(const mc_hw *hw, uint64_t n, uint64_t *rem) {
    uint64_t q = mulhi64(n, hw->rate_inv);
    uint64_t r = n - q * hw->rate;
    while (r >= hw->rate) {
        q++;
        r -= hw->rate;
    }
    if (rem) *rem = r;
    return q;
}

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

uint64_t mc_hw_now(mc_hw *hw) {
    /* A function of (cycles, time) while cyc and rem stay: rebase moves
     * them together without changing the result. */
    const uint64_t count = hw->cpu->cycles;
    if (count == hw->now_count && hw->time == hw->now_time && count != 0) return hw->now_value;
    const uint64_t d = count - hw->cyc;
    const uint64_t v = hw->time + div_rate(hw, d * MC_C15M_HZ + hw->rem, NULL);
    hw->now_count = count;
    hw->now_time = hw->time;
    hw->now_value = v;
    return v;
}

static void rebase(mc_hw *hw) {
    const uint64_t d = hw->cpu->cycles - hw->cyc;
    if (!d) return;
    const uint64_t total = d * MC_C15M_HZ + hw->rem;
    hw->time += div_rate(hw, total, &hw->rem);
    hw->cyc = hw->cpu->cycles;
    hw->now_count = 0;
}

/* Cycles after hw->cyc until guest time reaches `when`. */
static uint64_t cycles_until(const mc_hw *hw, uint64_t when) {
    if (when == MC_NEVER) return UINT64_MAX;
    if (when <= hw->time) return 0;
    const uint64_t ticks = when - hw->time;
    if (ticks > UINT64_MAX / hw->rate) return UINT64_MAX;
    const uint64_t need = ticks * hw->rate;
    return need <= hw->rem ? 0 : (need - hw->rem + MC_C15M_HZ - 1) / MC_C15M_HZ;
}

/* Time passes without instructions (STOP): `n` cycles. */
static void idle(mc_hw *hw, uint64_t n) {
    const uint64_t total = n * MC_C15M_HZ + hw->rem;
    hw->time += div_rate(hw, total, &hw->rem);
    hw->idle += n;
    hw->now_count = 0;
}

/* --- the host the chips see ------------------------------------------------------- */

static uint64_t host_now(void *ctx) { return mc_hw_now(ctx); }

static void host_schedule(void *ctx, void *dev, uint64_t when) {
    mc_hw *hw = ctx;
    int slot = mc_sched_slot(&hw->sched, dev);
    if (slot < 0) {
        const mc_dev *d = dev;
        slot = mc_sched_add(&hw->sched, dev, d->ops->event);
    }
    mc_sched_set(&hw->sched, slot, when);
    if (hw->running && when != MC_NEVER) {
        /* A deadline inside the current slice ends the slice there. */
        uint64_t target = hw->cyc + cycles_until(hw, when);
        if (target <= hw->cpu->cycles) target = hw->cpu->cycles + 1;
        if (target < hw->slice_end) {
            hw->slice_end = target;
            hw->cpu->budget = (int64_t)(target - hw->cpu->cycles);
        }
    }
}

static void host_line(void *ctx, void *dev, int line, int state) {
    mc_hw *hw = ctx;
    if (hw->board && hw->desc->ops->line) hw->desc->ops->line(hw->board, dev, line, state);
}

/* A chip as bus master (hw.h): RAM pages directly, anything else through
 * the bus a byte at a time; what is written into RAM drops the translations
 * made from it. */
static int host_dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    mc_hw *hw = ctx;
    uint8_t *p = data;
    const uint32_t start = address;
    int ok = 1;
    while (length && ok) {
        const uint32_t page = address >> MC_BUS_PAGE_SHIFT, offset = address & (MC_BUS_PAGE_SIZE - 1);
        uint32_t n = MC_BUS_PAGE_SIZE - offset;
        if (n > length) n = length;
        uint8_t *host = write ? hw->bus.write_map[page] : (uint8_t *)hw->bus.read_map[page];
        if (host) {
            if (write) memcpy(host + offset, p, n);
            else memcpy(p, host + offset, n);
        } else {
            for (uint32_t k = 0; k < n && ok; k++) {
                uint32_t v = 0;
                if (write) ok = !mc_bus_write(&hw->bus, address + k, 1, p[k]);
                else {
                    ok = !mc_bus_read(&hw->bus, address + k, 1, &v);
                    p[k] = (uint8_t)v;
                }
            }
        }
        address += n;
        p += n;
        length -= n;
    }
    if (write && address != start) m68k_jit_invalidate(hw->cpu, start, address - start);
    return ok;
}

static void irq_changed(void *ctx, unsigned ipl) {
    mc_hw *hw = ctx;
    m68k_set_irq(hw->cpu, ipl);
}

/* Device access time (mc_bus_region.cost) passes in guest time; a deadline
 * it reaches ends the slice at this instruction. */
static void take_cost(mc_hw *hw) {
    if (!hw->bus.cost_due) return;
    hw->time += hw->bus.cost_due;
    hw->bus.cost_due = 0;
    if (hw->running && mc_sched_next(&hw->sched) <= mc_hw_now(hw)) {
        hw->slice_end = hw->cpu->cycles + 1;
        hw->cpu->budget = 1;
    }
}
static int cpu_read(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t *value) {
    (void)fc;
    mc_hw *hw = ctx;
    if (hw->amu.on) address = mc_amu_map(&hw->amu, address);
    const int fault = mc_bus_read(&hw->bus, address, size, value);
    take_cost(hw);
    return fault;
}
static int cpu_write(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t value) {
    (void)fc;
    mc_hw *hw = ctx;
    if (hw->amu.on) address = mc_amu_map(&hw->amu, address);
    const int fault = mc_bus_write(&hw->bus, address, size, value);
    take_cost(hw);
    return fault;
}
/* A polling loop of the guest (m68k_bus.poll): the iterations whose every
 * device access would come before the next deadline, as take_cost tests it
 * after each access (time = the guest time of the cycles done so far plus
 * every access's cost), and within the slice's cycles. The
 * accesses read the same throughout (their chips say so until their next
 * event, which is a deadline), so those iterations are the same as the one
 * just run; their device time is added here. */
static int64_t cpu_poll(void *ctx, const m68k_poll *p) {
    mc_hw *hw = ctx;
    if (!hw->running || !p->n || p->n > 8 || !p->length) return 0;
    uint64_t cost[8], per = 0;
    for (unsigned m = 0; m < p->n; m++) {
        const uint32_t a = hw->amu.on ? mc_amu_map(&hw->amu, p->address[m]) : p->address[m];
        if (!mc_bus_read_stable(&hw->bus, a, p->size[m], &cost[m])) return 0;
        per += cost[m];
    }
    const int64_t most = p->budget / (int64_t)p->length;
    /* Accesses that cost nothing are not tested (take_cost): with none
     * that costs, the slice's own end is the bound. */
    if (!per) return most;
    const uint64_t next = mc_sched_next(&hw->sched);
    if (next == MC_NEVER) {
        hw->time += (uint64_t)most * per;
        return most;
    }
    const uint64_t base = hw->cpu->cycles - hw->cyc;
    /* The iteration's last access sees the latest time: the first iteration
     * k whose last access reaches the deadline ends the slice; binary
     * search (time grows with k). */
    unsigned last = p->n - 1;       /* the last access that costs (and is tested) */
    while (!cost[last]) last--;
    int64_t lo = 0, hi = most;      /* lo iterations pass; hi + 1 is the bound */
    while (lo < hi) {
        const int64_t k = lo + (hi - lo + 1) / 2;   /* do iterations 0..k-1 all pass? test iteration k-1 */
        const uint64_t i = (uint64_t)(k - 1);
        const uint64_t d = base + i * p->length + p->at[last];
        const uint64_t now = hw->time + i * per + per + div_rate(hw, d * MC_C15M_HZ + hw->rem, NULL);
        if (now < next) lo = k;
        else hi = k - 1;
    }
    hw->time += (uint64_t)lo * per;
    return lo;
}

/* Executions of a DBcc to itself (m68k_bus.dbra), set only when
 * emulation.cpu_mhz is above the model's clock: a delay loop takes the time
 * it takes on the real machine, its cycles at the model's clock, so the
 * ROM's calibration of it (TimeDBRA, a 16-bit count) stays the real one and
 * the delays keep their length. Each of its cycles is one cycle at cpu_hz
 * (in the cycle count) plus the rest, dbra_num / dbra_den C15M periods,
 * accumulated exactly. Execution k of the n (1-based) ends the slice when
 * the time after it reaches the next deadline, as take_cost tests a device
 * access; otherwise the slice is shortened to that deadline, as
 * cycles_until would have set it. */
static uint64_t dbra_time(const mc_hw *hw, uint64_t cycles, uint64_t *acc) {
    uint64_t r;
    uint64_t q = muldiv(cycles, hw->dbra_num, hw->dbra_den, &r);
    r += hw->dbra_acc;
    if (r >= hw->dbra_den) {
        q++;
        r -= hw->dbra_den;
    }
    if (acc) *acc = r;
    return q;
}
static int64_t cpu_dbra(void *ctx, int64_t n, unsigned t) {
    mc_hw *hw = ctx;
    m68k_cpu *cpu = hw->cpu;
    if (n < 1) return n;
    const uint64_t next = hw->running ? mc_sched_next(&hw->sched) : MC_NEVER;
    const uint64_t base = cpu->cycles - hw->cyc;
    /* The guest time after execution k. */
    #define DBRA_NOW(k) (hw->time + dbra_time(hw, (k) * t, NULL) + div_rate(hw, (base + (k) * t) * MC_C15M_HZ + hw->rem, NULL))
    int64_t k = n;
    if (next != MC_NEVER && DBRA_NOW((uint64_t)n) >= next) {
        int64_t lo = 1, hi = n;     /* the first k whose time reaches next */
        while (lo < hi) {
            const int64_t mid = lo + (hi - lo) / 2;
            if (DBRA_NOW((uint64_t)mid) >= next) hi = mid;
            else lo = mid + 1;
        }
        k = lo;
    }
    #undef DBRA_NOW
    uint64_t acc = 0;
    hw->time += dbra_time(hw, (uint64_t)k * t, &acc);
    hw->dbra_acc = acc;
    if (!hw->running || next == MC_NEVER) return k;
    /* The slice ends at the deadline: after these k when they reach it. */
    const uint64_t until = cycles_until(hw, next);
    const uint64_t after = cpu->cycles + (uint64_t)k * t;
    const uint64_t end = until == UINT64_MAX ? UINT64_MAX : hw->cyc + until;
    if (end < hw->slice_end) {
        const uint64_t stop = end > after ? end : after;
        cpu->budget -= (int64_t)(hw->slice_end - stop);
        hw->slice_end = stop;
    }
    return k;
}

/* The 68000's wait states: the board's (mc_hw.wait). */
static unsigned cpu_wait(void *ctx, uint32_t address, unsigned fc, unsigned kind, uint64_t cycle) {
    mc_hw *hw = ctx;
    return hw->wait && hw->board ? hw->wait(hw->board, address, fc, kind, cycle) : 0;
}

static void cpu_reset(void *ctx) {
    mc_hw *hw = ctx;
    if (hw->board && hw->desc->ops->reset) hw->desc->ops->reset(hw->board);
}

void mc_hw_power_off(mc_hw *hw) {
    hw->powered_off = 1;
    if (hw->running) {
        hw->slice_end = hw->cpu->cycles + 1;
        hw->cpu->budget = 1;
    }
}

void mc_hw_hold_reset(mc_hw *hw, int held) {
    if (held == hw->reset_held) return;
    hw->reset_held = held;
    if (!held) hw->reset_pending = 1;
    if (hw->running) {
        hw->slice_end = hw->cpu->cycles + 1;
        hw->cpu->budget = 1;
    }
}

void mc_hw_reset_switch(mc_hw *hw, int down) {
    down = down != 0;
    if (down == hw->switch_reset) return;
    hw->switch_reset = down;
    if (!down) hw->reset_pending = 1;
    if (hw->running) {
        hw->slice_end = hw->cpu->cycles + 1;
        hw->cpu->budget = 1;
    }
}

void mc_hw_cpu_restart(mc_hw *hw) {
    hw->cpu_reset_pending = 1;
    if (hw->running) {
        hw->slice_end = hw->cpu->cycles + 1;
        hw->cpu->budget = 1;
    }
}

void mc_hw_maps_changed(mc_hw *hw) {
    if (hw->amu.read_map) mc_amu_bus_changed(&hw->amu, &hw->bus);
    uint8_t **r = hw->amu.on ? hw->amu.read_map : hw->bus.read_map, **w = hw->amu.on ? hw->amu.write_map : hw->bus.write_map;
    m68k_set_maps(hw->cpu, r, w, r, w);
}

void mc_hw_set_addr24(mc_hw *hw, int on) {
    if (!hw->amu.read_map || !mc_amu_set(&hw->amu, &hw->bus, on)) return;
    uint8_t **r = hw->amu.on ? hw->amu.read_map : hw->bus.read_map, **w = hw->amu.on ? hw->amu.write_map : hw->bus.write_map;
    m68k_set_maps(hw->cpu, r, w, r, w);
}

int mc_hw_region(const mc_hw *hw, const char *name) {
    for (size_t i = 0; i < hw->desc->nmap; i++)
        if (!strcmp(hw->desc->map[i].name, name)) return hw->region[i];
    return 0;
}

int mc_hw_irq(const mc_hw *hw, const char *name) {
    for (size_t i = 0; i < hw->desc->nirq; i++)
        if (!strcmp(hw->desc->irqs[i].source, name)) return hw->irq_source[i];
    return -1;
}

/* --- building ---------------------------------------------------------------------- */

static void hw_destroy(mc_hw *hw) {
    if (!hw) return;
    if (hw->board && hw->desc->ops->destroy) hw->desc->ops->destroy(hw->board);
    mc_media_close(&hw->media);
    if (hw->cpu) {
        m68k_free(hw->cpu);
        free(hw->cpu);
    }
    mc_bus_free(&hw->bus);
    mc_amu_free(&hw->amu);
    free(hw->rom);
    free(hw->ram);
    free(hw);
}

static uint32_t rom_checksum(const uint8_t *rom) {
    return (uint32_t)rom[0] << 24 | (uint32_t)rom[1] << 16 | (uint32_t)rom[2] << 8 | rom[3];
}

static mc_hw *hw_create(const mc_hw_desc *desc, const mc_config *c, const uint8_t *rom, size_t rom_size, char *error, size_t capacity) {
    const mc_model *p = mc_model_entry(c->model);
    if (!rom || rom_size < 4096 || (rom_size & (rom_size - 1)) || rom_size > 0x10000000u) {
        snprintf(error, capacity, "The ROM image is not a whole power-of-two size");
        return NULL;
    }
    int known = 0;
    for (const uint32_t *r = desc->roms; *r; r++) known |= *r == rom_checksum(rom);
    if (!known) {
        snprintf(error, capacity, "%s does not run ROM %08X", p->name, (unsigned)rom_checksum(rom));
        return NULL;
    }
    int legal = 0;
    char sizes[160] = "";
    uint32_t kb[64];
    const size_t nkb = desc_ram_sizes(desc, kb, 64);
    for (size_t i = 0; i < nkb; i++) {
        char text[24];
        legal |= kb[i] == mc_config_ram_kb(c);
        size_t used = strlen(sizes);
        snprintf(sizes + used, sizeof sizes - used, "%s%s", used ? ", " : "", mc_config_ram_text(kb[i], text, sizeof text));
    }
    if (!legal) {
        snprintf(error, capacity, "emulation.ram_mb: the %s takes %s MiB", p->name, sizes);
        return NULL;
    }
    if (c->cpu_hz < MC_CONFIG_MIN_HZ || c->cpu_hz > MC_CONFIG_MAX_HZ) {
        snprintf(error, capacity, "emulation.cpu_mhz must be 1 to 10000");
        return NULL;
    }
    mc_hw *hw = calloc(1, sizeof *hw);
    if (!hw) {
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    hw->desc = desc;
    hw->model = p;
    hw->config = *c;
    hw->rate = c->cpu_hz;
    hw->rate_inv = UINT64_MAX / hw->rate;
    hw->rom_size = (uint32_t)rom_size;
    hw->rom = malloc(rom_size);
    hw->ram_size = mc_config_ram_kb(c) << 10;
    hw->ram = calloc(1, hw->ram_size);
    hw->cpu = calloc(1, sizeof *hw->cpu);
    mc_media_init(&hw->media, c);
    if (!hw->rom || !hw->ram || !hw->cpu || !mc_bus_init(&hw->bus)) {
        snprintf(error, capacity, "Out of memory");
        hw_destroy(hw);
        return NULL;
    }
    memcpy(hw->rom, rom, rom_size);
    snprintf(hw->description, sizeof hw->description, "%s", p->name);
    hw->host.ctx = hw;
    hw->host.now = host_now;
    hw->host.schedule = host_schedule;
    hw->host.line = host_line;
    hw->host.dma = host_dma;
    mc_sched_init(&hw->sched);
    mc_irq_init(&hw->irq, irq_changed, hw);
    for (size_t i = 0; i < desc->nirq && i < 32; i++) hw->irq_source[i] = mc_irq_add(&hw->irq, desc->irqs[i].level);
    hw->switch_irq = mc_hw_irq(hw, "switch");
    if (hw->switch_irq < 0) hw->switch_irq = mc_irq_add(&hw->irq, 7);
    /* The map: ROM windows, device regions and empty spaces; the RAM
     * windows are the board's (its memory controller decodes them). */
    for (size_t i = 0; i < desc->nmap && i < 32; i++) {
        const mc_region *r = &desc->map[i];
        switch (r->kind) {
        case MC_REGION_ROM:
            hw->region[i] = mc_bus_map_memory(&hw->bus, r->base, (uint32_t)r->size, hw->rom, hw->rom_size, 0);
            break;
        case MC_REGION_IO:
            hw->region[i] = mc_bus_add_io(&hw->bus, r->base, r->size, r->mask, 0);
            break;
        default:
            hw->region[i] = 0;
            break;
        }
    }
    m68k_bus bus;
    memset(&bus, 0, sizeof bus);
    bus.ctx = hw;
    bus.read_map[0] = bus.read_map[1] = hw->bus.read_map;
    bus.write_map[0] = bus.write_map[1] = hw->bus.write_map;
    bus.read = cpu_read;
    bus.write = cpu_write;
    bus.reset = cpu_reset;
    bus.poll = cpu_poll;
    if (desc->cpu == M68K_68000) bus.wait = cpu_wait;
    /* Delay loops at the real machine's clock (cpu_dbra): per cycle,
     * C15M / model clock - C15M / cpu_hz. */
    if (c->cpu_hz > p->cpu_hz) {
        bus.dbra = cpu_dbra;
        hw->dbra_num = (uint64_t)MC_C15M_HZ * (c->cpu_hz - p->cpu_hz);
        hw->dbra_den = c->cpu_hz * p->cpu_hz;
    }
    m68k_init(hw->cpu, (m68k_model)desc->cpu, &bus);
    /* Each instruction takes its cycles for the model (m68k_timing.c). */
    m68k_set_timing(hw->cpu, 1);
    m68k_set_fpu(hw->cpu, (m68k_fpu_type)desc->fpu, c->fpu_full);
    /* emulation.pmmu: a 68851 in the Macintosh II's PMMU socket, where the
     * AMU otherwise sits (Guide to the Macintosh Family Hardware, 2nd ed.,
     * the Macintosh II: the 68851 option in place of the Address
     * Management Unit). */
    const int pmmu = desc->mmu == MC_MMU_68851 || (desc->mmu == MC_MMU_AMU && c->pmmu && desc->cpu == M68K_68020);
    if (pmmu) m68k_set_pmmu(hw->cpu, 1);
    /* The 24-bit decode of a board without a PMMU (the AMU's socket holds
     * the 68851 when one is fitted). */
    if (desc->map24 && !pmmu && !mc_amu_init(&hw->amu, desc->map24)) {
        snprintf(error, capacity, "Out of memory");
        hw_destroy(hw);
        return NULL;
    }
    /* emulation.jit: guest code translated to host code (jit_x64.c) where
     * the host has the translator; the interpreter otherwise. */
    if (c->jit) m68k_jit_enable(hw->cpu, 1);
    hw->board = desc->build(hw);
    if (!hw->board) {
        snprintf(error, capacity, "%s", hw->error[0] ? hw->error : "The machine could not be built");
        hw_destroy(hw);
        return NULL;
    }
    mc_hw_maps_changed(hw);
    if (desc->ops->reset) desc->ops->reset(hw->board);
    m68k_reset(hw->cpu);
    return hw;
}

static mc_run_status hw_run(mc_hw *hw, uint64_t cycles, uint64_t deadline) {
    m68k_cpu *cpu = hw->cpu;
    uint64_t done = 0;
    if (hw->powered_off) return MC_RUN_HALTED;
    while (done < cycles) {
        rebase(hw);
        mc_sched_run(&hw->sched, hw->time);
        if (deadline && hw->time >= deadline) break;
        if (hw->cpu_reset_pending) {
            hw->cpu_reset_pending = 0;
            m68k_reset(cpu);
        }
        if (hw->reset_pending) {
            hw->reset_pending = 0;
            if (hw->desc->ops->reset) hw->desc->ops->reset(hw->board);
            m68k_reset(cpu);
        }
        if (hw->reset_held || hw->switch_reset) {
            /* In reset: time passes to the next deadline (the controller's). */
            rebase(hw);
            uint64_t wake = mc_sched_next(&hw->sched);
            if (deadline && deadline < wake) wake = deadline;
            uint64_t skip = cycles_until(hw, wake);
            if (skip > cycles - done) skip = cycles - done;
            if (!skip) skip = 1;
            idle(hw, skip);
            done += skip;
            if (hw->powered_off) return MC_RUN_HALTED;
            continue;
        }
        uint64_t next = mc_sched_next(&hw->sched);
        if (deadline && deadline < next) next = deadline;
        uint64_t n = cycles - done;
        const uint64_t until = cycles_until(hw, next);
        if (until < n) n = until ? until : 1;
        if (n > (1u << 24)) n = 1u << 24;
        const uint64_t before = cpu->cycles;
        hw->running = 1;
        hw->slice_end = before + n;
        const m68k_status st = m68k_run(cpu, (int64_t)n);
        hw->running = 0;
        done += cpu->cycles - before;
        if (hw->powered_off) {
            mc_disk_sync(1);
            return MC_RUN_HALTED;
        }
        if (st == M68K_HALTED) {
            snprintf(hw->error, sizeof hw->error, "The processor halted (double bus fault) at %08X", (unsigned)cpu->pc);
            return MC_RUN_ERROR;
        }
        if (st == M68K_UNIMPLEMENTED) {
            snprintf(hw->error, sizeof hw->error, "Opcode %04X at %08X is not implemented", (unsigned)cpu->unimplemented_op, (unsigned)cpu->pc);
            return MC_RUN_ERROR;
        }
        if (st == M68K_STOPPED && cpu->cycles == before) {
            /* Waiting for an interrupt: time goes to the next deadline; the
             * guest is idle, its disk writes are made durable. */
            mc_disk_sync(1);
            rebase(hw);
            uint64_t wake = mc_sched_next(&hw->sched);
            if (deadline && deadline < wake) wake = deadline;
            uint64_t skip = cycles_until(hw, wake);
            if (skip > cycles - done) skip = cycles - done;
            if (!skip) skip = 1;
            idle(hw, skip);
            done += skip;
        }
    }
    rebase(hw);
    mc_sched_run(&hw->sched, hw->time);
    if (hw->desc->ops->poll) hw->desc->ops->poll(hw->board);
    mc_disk_sync(0);
    return MC_RUN_OK;
}

/* Compute the base path only. The owned store creates directories and
 * never moves or implicitly imports an unowned legacy file. */
const char *mc_config_nvram_path(const mc_config *c, char *buffer, size_t capacity) {
    /* A board's private file (an owned session's, or a tool's --nvram FILE): that file. */
    if (c->nvram_private && c->nvram[0]) {
        snprintf(buffer, capacity, "%s", c->nvram);
        return buffer;
    }
    /* A Built-in profile must not overwrite another model's saved firmware. */
    if (mc_model_builtin(mc_model_entry(c->model))) return NULL;
    if (c->nvram[0]) {
        snprintf(buffer, capacity, "%s", c->nvram);
        return buffer;
    }
    if (!c->source[0]) return NULL;
    const char *source = c->source;
    const char *slash = strrchr(source, '/'), *back = strrchr(source, '\\');
    if (back > slash) slash = back;
    const char *name = slash ? slash + 1 : source;
    const char *dot = strrchr(name, '.');
    snprintf(buffer, capacity, "%.*snvram/%.*s.nvram", (int)(name - source), source,
             dot ? (int)(dot - name) : (int)strlen(name), name);
    return buffer;
}
const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);
const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity) {
    return mc_config_nvram_path(&hw->config, buffer, capacity);
}

/* --- the machine interface --------------------------------------------------------- */

int mc_machine_m68k_accelerator_known(const uint8_t *rom, size_t rom_size) { return mc_m68k_accel_knows(rom, rom_size); }

mc_machine *mc_machine_create(const mc_config *c, const uint8_t *rom, size_t rom_size, char *error, size_t capacity) {
    const mc_model *p = mc_model_entry(c->model);
    if (!p) {
        if (capacity) snprintf(error, capacity, "Unknown model");
        return NULL;
    }
    mc_machine *m = calloc(1, sizeof *m);
    if (!m) {
        if (capacity) snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    const mc_hw_desc *desc = mc_hw_find(p->key);
    const mc_ppc_desc *ppc = desc ? NULL : mc_ppc_find(p->key);
    mc_config session = *c;
    char base[MC_CONFIG_PATH],working[MC_CONFIG_PATH];
    unsigned payload=273;
    if(ppc && ppc!=&mc_pdm_desc && ppc!=&mc_pdm7100_desc && ppc!=&mc_pdm8100_desc && strncmp(ppc->key,"pdm",3) && strncmp(ppc->key,"pbx",3))payload=!strcmp(ppc->key,"pippin")?131345u:8465u;
    if(desc && (desc==&mc_pb140_desc || desc==&mc_pb170_desc || desc==&mc_pb160_desc || desc==&mc_pb180_desc))payload=401;
    const char *nvbase=mc_config_nvram_path(c,base,sizeof base);
    /* nvram_private (the tools' --nvram FILE): the board's own file as given, no owned record */
    const int owned=(desc||ppc)&&!c->nvram_private;
    if(owned && !mc_nvram_begin(c,nvbase,rom&&rom_size>=4?rom_checksum(rom):0,payload,&m->nvram,working,sizeof working,error,capacity)){free(m);return NULL;}
    if(owned){snprintf(session.nvram,sizeof session.nvram,"%s",working);session.nvram_private=1;}
    if (desc) m->hw = hw_create(desc, &session, rom, rom_size, error, capacity);
    else if (ppc) m->ppc = mc_ppc_create(ppc, &session, rom, rom_size, error, capacity);
    else m->stub = mc_stub_create(c, rom, rom_size, error, capacity);
    if (!m->hw && !m->ppc && !m->stub) {
        mc_nvram_end(m->nvram,0,NULL,0);
        free(m);
        return NULL;
    }
    mc_drivesnd_init(&m->drive_sound, MC_C15M_HZ, 48000);
    m->speaker_enabled=c->sound;
    mc_drivesnd_set(&m->drive_sound,c->floppy_sound,50);
    if(m->hw)m->hw->host.drive_sound=&m->drive_sound;
    if(m->ppc)m->ppc->host.drive_sound=&m->drive_sound;
    return m;
}

int mc_machine_close(mc_machine *m,char *error,size_t capacity) {
    if (!m) return 1;
    int commit=m->hw?(mc_hw_now(m->hw)>0):m->ppc?(mc_ppc_now(m->ppc)>0):0;
    mc_stub_destroy(m->stub);
    hw_destroy(m->hw);
    mc_ppc_destroy(m->ppc);
    int ok=mc_nvram_end(m->nvram,commit,error,capacity);
    free(m);
    return ok;
}
void mc_machine_destroy(mc_machine *m) {
    char error[MC_CONFIG_PATH+128];
    if(!mc_machine_close(m,error,sizeof error))fprintf(stderr,"%s\n",error);
}

void mc_machine_set_clock(mc_machine *m, int64_t host_seconds) {
    if (m && m->ppc) mc_ppc_set_clock(m->ppc, host_seconds);
    if (m && m->hw && m->hw->desc->ops->set_clock) m->hw->desc->ops->set_clock(m->hw->board, host_seconds);
}

const char *mc_machine_description(const mc_machine *m) {
    return !m ? "" : m->stub ? mc_stub_description(m->stub) : m->ppc ? mc_ppc_description(m->ppc) : m->hw->description;
}
const char *mc_machine_error(const mc_machine *m) {
    return !m ? "" : m->stub ? mc_stub_error(m->stub) : m->ppc ? mc_ppc_error(m->ppc) : m->hw->error;
}

mc_run_status mc_machine_run(mc_machine *m, uint64_t cycles, uint64_t deadline) {
    return m->stub ? mc_stub_run(m->stub, cycles, deadline) : m->ppc ? mc_ppc_run(m->ppc, cycles, deadline) : hw_run(m->hw, cycles, deadline);
}
uint64_t mc_machine_cycles(const mc_machine *m) {
    return !m ? 0 : m->stub ? mc_stub_cycles(m->stub) : m->ppc ? mc_ppc_cycles(m->ppc) : m->hw->cpu->cycles + m->hw->idle;
}
uint64_t mc_machine_instructions(const mc_machine *m) {
    return !m ? 0 : m->stub ? 0 : m->ppc ? mc_ppc_instructions(m->ppc) : m->hw->cpu->instructions;
}
int mc_machine_powered_off(const mc_machine *m) { return m && ((m->hw && m->hw->powered_off) || (m->ppc && m->ppc->powered_off)); }
int mc_machine_peek(mc_machine *m, uint32_t address, unsigned size, uint32_t *value) {
    if (m && m->ppc) return mc_ppc_peek(m->ppc, address, size, value);
    if (!m || !m->hw) return 0;
    mc_hw *hw = m->hw;
    uint32_t pa = address;
    unsigned fill = 0;
    if (hw->cpu->mmu.mode == 1 && m68k_mmu_translate(hw->cpu, address, 5, 0, 0, &pa, &fill)) return 0;
    return mc_bus_peek(&hw->bus, pa, size, value);
}
static int poke(mc_machine *m, uint32_t address, unsigned size, uint32_t value) {
    if (m->ppc) return mc_ppc_poke(m->ppc, address, size, value);
    mc_hw *hw = m->hw;
    uint32_t pa = address;
    unsigned fill = 0;
    if (hw->cpu->mmu.mode == 1 && m68k_mmu_translate(hw->cpu, address, 5, 0, 0, &pa, &fill)) return 0;
    return mc_bus_write(&hw->bus, pa, size, value) == 0;   /* 0: written */
}
int mc_machine_absolute_ready(mc_machine *m) {
    if (!m || m->stub) return 0;
    const mc_board_ops *ops = m->ppc ? m->ppc->desc->ops : m->hw->desc->ops;
    void *board = m->ppc ? m->ppc->board : m->hw->board;
    return ops && ops->absolute_mouse && ops->absolute_ready && ops->absolute_ready(board);
}
int mc_machine_absolute_mouse(mc_machine *m, int x, int y, int button) {
    if (!m || m->stub) return 0;
    const mc_board_ops *ops = m->ppc ? m->ppc->desc->ops : m->hw->desc->ops;
    return ops && ops->absolute_mouse && ops->absolute_mouse(m->ppc ? m->ppc->board : m->hw->board, x, y, button);
}
int mc_machine_mouse_to(mc_machine *m, int x, int y) {
    if (!m || (!m->hw && !m->ppc)) return 0;
    uint32_t couple = 0;
    if (!mc_machine_peek(m, 0x8CF, 1, &couple)) return 0;                 /* CrsrCouple */
    const uint32_t point = (uint32_t)(uint16_t)y << 16 | (uint16_t)x;     /* a Point: v, then h */
    return poke(m, 0x828, 4, point) && poke(m, 0x82C, 4, point) &&      /* MTemp, RawMouse */
           poke(m, 0x8CE, 1, couple);                                    /* CrsrNew = CrsrCouple: the cursor task moves it */
}
uint64_t mc_machine_time(const mc_machine *m) {
    return !m ? 0 : m->stub ? mc_stub_time(m->stub) : m->ppc ? mc_ppc_now(m->ppc) : mc_hw_now(m->hw);
}
uint64_t mc_machine_rate(const mc_machine *m) { return !m ? 0 : m->stub ? mc_stub_rate(m->stub) : m->ppc ? m->ppc->rate : m->hw->rate; }

int mc_machine_framebuffer(mc_machine *m, mc_framebuffer *fb) {
    if (!m || !fb) return 0;
    fb->level = 0;
    fb->contrast = 0;
    fb->native_depth = fb->native_row_bytes = 0;
    if (m->stub) return mc_stub_framebuffer(m->stub, fb);
    if (m->ppc) return mc_ppc_framebuffer(m->ppc, fb);
    return m->hw->desc->ops->framebuffer ? m->hw->desc->ops->framebuffer(m->hw->board, fb) : 0;
}

int mc_machine_key(mc_machine *m, uint8_t code, int down) {
    if (m->stub) return mc_stub_key(m->stub, code, down);
    if (m->ppc) return mc_ppc_key(m->ppc, code, down);
    return m->hw->desc->ops->key ? m->hw->desc->ops->key(m->hw->board, code, down) : 1;
}
int mc_machine_mouse(mc_machine *m, int dx, int dy, int button) {
    if (m->stub) return mc_stub_mouse(m->stub, dx, dy, button);
    if (m->ppc) return mc_ppc_mouse(m->ppc, dx, dy, button);
    return m->hw->desc->ops->mouse ? m->hw->desc->ops->mouse(m->hw->board, dx, dy, button) : 1;
}

static uint32_t speaker_rate(const mc_machine *m) {
    if (!m) return 0;
    if (m->stub) return mc_stub_audio_rate(m->stub);
    if (m->ppc) return mc_ppc_audio_rate(m->ppc);
    return m->hw->desc->ops->audio_rate ? m->hw->desc->ops->audio_rate(m->hw->board) : 0;
}
static size_t speaker_pull(mc_machine *m, int16_t *stereo, size_t frames) {
    if (m->stub) return mc_stub_audio_pull(m->stub, stereo, frames);
    if (m->ppc) return mc_ppc_audio_pull(m->ppc, stereo, frames);
    const size_t n = m->hw->desc->ops->audio_pull ? m->hw->desc->ops->audio_pull(m->hw->board, stereo, frames) : 0;
    /* The CD drive's audio output joins the machine's sound. */
    int ejected = 0;
    mc_disk *cd = mc_media_get(&m->hw->media, MC_MEDIA_CDROM, 0, &ejected);
    return mc_disk_mix(ejected ? NULL : cd, stereo, n, frames, mc_machine_audio_rate(m));
}

uint32_t mc_machine_audio_rate(const mc_machine *m) {
    const uint32_t rate=speaker_rate(m);
    return rate ? rate : m && m->drive_sound.enabled ? 48000u : 0u;
}
static uint64_t drive_time(const mc_machine *m) {
    return m->hw?mc_hw_now(m->hw):m->ppc?mc_ppc_now(m->ppc):0;
}
void mc_machine_set_sound(mc_machine *m,int speaker,int floppy) {
    if(!m)return;
    m->speaker_enabled=speaker;
    if(m->drive_sound.enabled != (floppy!=0)) {
        const uint8_t motors=m->drive_sound.physical_motor;
        mc_drivesnd_set(&m->drive_sound,floppy,50);
        mc_drivesnd_reset(&m->drive_sound,drive_time(m));
        if(floppy)for(unsigned d=0;d<MC_DRIVESND_DRIVES;d++)
            if(motors&(1u<<d))mc_drivesnd_event(&m->drive_sound,d,MC_DRIVE_MOTOR,drive_time(m),1,0);
    }
}
size_t mc_machine_audio_pull(mc_machine *m,int16_t *stereo,size_t frames) {
    const uint32_t rate=speaker_rate(m);
    if(!rate) {
        /* Muted guest DACs may have no sample clock. The mechanism still runs. */
        return m->drive_sound.enabled?mc_drivesnd_render(&m->drive_sound,drive_time(m),stereo,frames):0;
    }
    const size_t n=speaker_pull(m,stereo,frames);
    if(!m->speaker_enabled)memset(stereo,0,n*2u*sizeof *stereo);
    mc_drivesnd_mix_add(&m->drive_sound,drive_time(m),stereo,n,rate);
    return n;
}

int mc_machine_set_audio_source(mc_machine *m, const mc_audio_source *source) {
    if (!m || m->stub) return 0;
    if (m->ppc) return mc_ppc_set_audio_source(m->ppc, source);
    if (!m->hw->desc->ops->audio_source) return 0;
    m->hw->desc->ops->audio_source(m->hw->board, source);
    return 1;
}

void mc_machine_set_serial(mc_machine *m, const mc_serial_host *host) {
    if (m && m->ppc) mc_ppc_set_serial(m->ppc, host);
    if (m && m->hw && m->hw->desc->ops->serial_host) m->hw->desc->ops->serial_host(m->hw->board, host);
}
size_t mc_machine_serial_receive(mc_machine *m, unsigned port, const uint8_t *data, size_t count) {
    if (m && m->ppc) return mc_ppc_serial_receive(m->ppc, port, data, count);
    if (!m || !m->hw || !m->hw->desc->ops->serial_receive) return count;   /* no port: the bytes are lost */
    return m->hw->desc->ops->serial_receive(m->hw->board, port, data, count);
}
void mc_machine_set_net(mc_machine *m, const mc_net_host *host) {
    if (m && m->ppc) mc_ppc_set_net(m->ppc, host);
    if (m && m->hw && m->hw->desc->ops->net_host) m->hw->desc->ops->net_host(m->hw->board, host);
}
int mc_machine_net_receive(mc_machine *m, const uint8_t *frame, size_t length) {
    if (m && m->ppc) return mc_ppc_net_receive(m->ppc, frame, length);
    if (!m || !m->hw || !m->hw->desc->ops->net_receive) return 1;   /* no Ethernet: the frame is lost */
    return m->hw->desc->ops->net_receive(m->hw->board, frame, length);
}
void mc_machine_serial_modem(mc_machine *m, unsigned port, int dcd, int cts) {
    if (m && m->ppc) mc_ppc_serial_modem(m->ppc, port, dcd, cts);
    if (m && m->hw && m->hw->desc->ops->serial_modem) m->hw->desc->ops->serial_modem(m->hw->board, port, dcd, cts);
}

unsigned mc_machine_drives(const mc_machine *m, mc_media_kind kind) {
    return m->stub ? mc_stub_drives(m->stub, kind) : mc_media_drives(m->ppc ? &m->ppc->media : &m->hw->media, kind);
}
/* A CD's audio player runs on guest time, in samples of 1/44100 s. */
static uint64_t cd_clock(void *ctx) {
    const uint64_t t = mc_hw_now(ctx);
    return t / MC_C15M_HZ * 44100u + t % MC_C15M_HZ * 44100u / MC_C15M_HZ;
}
int mc_machine_insert(mc_machine *m, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    if (m->stub) return mc_stub_insert(m->stub, kind, drive, disk, error, capacity);
    if (m->ppc) return mc_ppc_insert(m->ppc, kind, drive, disk, error, capacity);
    mc_hw *hw = m->hw;
    if (!mc_media_insert(&hw->media, kind, drive, disk, hw->cpu->instructions != 0, error, capacity)) return 0;
    if (kind == MC_MEDIA_CDROM) mc_disk_set_clock(disk, cd_clock, hw);
    if (hw->desc->ops->insert && !hw->desc->ops->insert(hw->board, kind, drive, disk, error, capacity)) {
        /* The chip refused it: the drive is empty again; the caller keeps the image. */
        mc_media_forget(&hw->media, kind, drive);
        return 0;
    }
    return 1;
}
mc_disk *mc_machine_media(const mc_machine *m, mc_media_kind kind, unsigned drive, int *ejected) {
    return m->stub ? mc_stub_media(m->stub, kind, drive, ejected) : mc_media_get(m->ppc ? &m->ppc->media : &m->hw->media, kind, drive, ejected);
}
int mc_machine_eject(mc_machine *m, mc_media_kind kind, unsigned drive) {
    return m->stub ? mc_stub_eject(m->stub, kind, drive) : mc_media_eject(m->ppc ? &m->ppc->media : &m->hw->media, kind, drive);
}
int mc_machine_control(mc_machine *m, mc_control control, int value) {
    if (m->stub) return -1;
    if (m->ppc) return mc_ppc_control(m->ppc, control, value);
    mc_hw *hw = m->hw;
    const int r = hw->desc->ops->control ? hw->desc->ops->control(hw->board, (int)control, value) : -1;
    if (r >= 0) return r;
    /* The interrupt switch the board leaves to the machine: its irq source
     * while pressed (the NMI, level 7, whose press the processor takes
     * once). The reset switch is the board's (mc_hw_reset_switch). */
    if (control == MC_CONTROL_INTERRUPT && hw->switch_irq >= 0) {
        mc_irq_set(&hw->irq, hw->switch_irq, value != 0);
        return 0;
    }
    return -1;
}

int mc_machine_reinsert(mc_machine *m) {
    if (m->stub) return mc_stub_reinsert(m->stub);
    if (m->ppc) return mc_ppc_reinsert(m->ppc);
    mc_hw *hw = m->hw;
    const int kind = hw->media.last_kind, drive = hw->media.last_drive;
    if (!mc_media_reinsert(&hw->media)) return 0;
    mc_disk *disk = mc_media_get(&hw->media, (mc_media_kind)kind, (unsigned)drive, NULL);
    if (kind == MC_MEDIA_CDROM) mc_disk_set_clock(disk, cd_clock, hw);
    if (hw->desc->ops->insert && !hw->desc->ops->insert(hw->board, (mc_media_kind)kind, (unsigned)drive, disk, NULL, 0)) {
        mc_media_eject(&hw->media, (mc_media_kind)kind, (unsigned)drive);
        return 0;
    }
    return 1;
}
