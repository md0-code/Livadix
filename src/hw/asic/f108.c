/* Livadix — F108's memory controller (see f108.h). Livadix's own code. */
#include "f108.h"

#include <string.h>

#define REG_BANK0 1u            /* +$04 */
#define REG_END 11u             /* +$2C */
#define REG_LATCH 31u           /* +$7C */
#define FIELD 0x3FFu            /* bits 0-9: megabytes */
#define RAM_SPACE 0x40000000u   /* the first 1 GB (Developer Note, "RAM Addresses") */

static uint32_t rol6(uint32_t v) { return v << 6 | v >> 26; }
static uint32_t ror6(uint32_t v) { return v >> 6 | v << 26; }

static uint32_t f108_read(void *dev, uint32_t offset, unsigned size) {
    mc_f108 *f = dev;
    const unsigned n = (offset >> 2) % MC_F108_REGS;
    uint32_t v;
    if (n == REG_LATCH) v = f->reg[REG_LATCH];
    else {
        v = f->reg[n] & 0x3Fu;
        f->reg[REG_LATCH] = ror6(f->reg[n]);
    }
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}

static void f108_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_f108 *f = dev;
    const unsigned n = (offset >> 2) % MC_F108_REGS;
    /* The cycle's longword (a narrower write fills its lanes of the latch's view). */
    uint32_t v = value;
    if (size == 2) v = (offset & 2u) ? value & 0xFFFFu : value << 16;
    else if (size == 1) v = (value & 0xFFu) << (8u * (3u - (offset & 3u)));
    if (n == REG_LATCH) {
        f->reg[REG_LATCH] = v;
        return;
    }
    const uint32_t before = f->reg[n];
    f->reg[n] = (rol6(f->reg[REG_LATCH]) & ~0x3Fu) | (v & 0x3Fu);
    if (n >= REG_BANK0 && n <= REG_END && f->reg[n] != before && f->remap) f->remap(f->ctx);
}

static int f108_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)size;
    return ((offset >> 2) % MC_F108_REGS) == REG_LATCH;
}

const mc_dev_ops mc_f108_ops = {f108_read, f108_write, NULL, NULL, f108_stable};

static uint64_t mb(uint32_t field) { return (uint64_t)(field & FIELD) * MC_F108_UNIT; }

void mc_f108_map(const mc_f108 *f, mc_bus *bus, uint8_t *ram) {
    mc_bus_map_float(bus, 0, RAM_SPACE);
    uint64_t end = mb(f->reg[REG_END]) + MC_F108_UNIT;
    if (end > RAM_SPACE) end = RAM_SPACE;
    uint32_t host = 0;
    for (unsigned i = 0; i < MC_F108_BANKS; i++) {
        const uint32_t size = f->bank_size[i];
        const uint64_t start = mb(f->reg[REG_BANK0 + i]);
        uint64_t stop = i + 1 < MC_F108_BANKS ? mb(f->reg[REG_BANK0 + i + 1]) : end;
        if (stop > end || stop < start) stop = end;
        if (size && stop > start && start < end) mc_bus_map_memory(bus, (uint32_t)start, (uint32_t)(stop - start), ram + host, size, 1);
        host += size;
    }
}

int mc_f108_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_F108_BANKS]) {
    memset(bank_size, 0, MC_F108_BANKS * sizeof *bank_size);
    if (ram_mb < onboard_mb) return 0;
    bank_size[0] = onboard_mb << 20;
    uint32_t left = ram_mb - onboard_mb;
    for (unsigned i = 1; i < MC_F108_BANKS && i <= simm_banks && left; i++) {
        const uint32_t size = left >= 16 ? 16 : left >= 8 ? 8 : 4;
        if (left < size) return 0;
        bank_size[i] = size << 20;
        left -= size;
    }
    return left == 0;
}

void mc_f108_init(mc_f108 *f, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_F108_BANKS],
                  void (*remap)(void *ctx), void *ctx) {
    memset(f, 0, sizeof *f);
    f->dev.ops = &mc_f108_ops;
    f->dev.host = host;
    f->dev.name = name;
    memcpy(f->bank_size, bank_size, sizeof f->bank_size);
    /* Until the ROM programs it: the banks packed from 0, as after a start. */
    uint32_t at = 0;
    for (unsigned i = 0; i < MC_F108_BANKS; i++) {
        f->reg[REG_BANK0 + i] = at / MC_F108_UNIT;
        at += bank_size[i];
    }
    f->reg[REG_END] = at ? at / MC_F108_UNIT - 1u : 0;
    f->remap = remap;
    f->ctx = ctx;
}

/* --- the IDE interface and the slot interrupt register ------------------------ */

#define IDE_BUS 0x80u           /* bus n's task file at +n x $80 */
#define IDE_INT 0x100u          /* the interrupt byte at +$101 */

static uint8_t ide_int(const mc_f108_ide *i) { return (uint8_t)((i->inputs & 0x7Cu) | (i->control & 3u)); }

static void ide_update(mc_f108_ide *i) {
    const uint8_t enabled = (uint8_t)(0x70u | (i->control & 3u) << 2);
    const uint8_t out = (i->inputs & enabled) != 0;
    if (out != i->out) {
        i->out = out;
        mc_dev_line(&i->dev, MC_F108_LINE_SLOTIRQ, out);
    }
}

void mc_f108_ide_set(mc_f108_ide *i, uint8_t bit, int state) {
    if (state) i->inputs |= bit;
    else i->inputs &= (uint8_t)~bit;
    ide_update(i);
}

static uint32_t ide_read(void *dev, uint32_t offset, unsigned size) {
    mc_f108_ide *i = dev;
    if (offset < IDE_BUS) return mc_ata_ops.read(i->ata, offset, size);
    if (offset >= IDE_INT && offset < IDE_INT + 4u) {
        const uint32_t word = (uint32_t)ide_int(i) << 16;      /* the byte at +$101 in a longword at +$100 */
        if (size == 4) return word;
        if (size == 2) return (offset & 2u) ? 0 : word >> 16;
        return (offset & 3u) == 1u ? ide_int(i) : 0;
    }
    return 0;           /* no second bus: the cycle floats */
}

static void ide_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_f108_ide *i = dev;
    if (offset < IDE_BUS) {
        mc_ata_ops.write(i->ata, offset, size, value);
        return;
    }
    if (offset >= IDE_INT && offset < IDE_INT + 4u) {
        uint8_t b;
        if (size == 4) b = (uint8_t)(value >> 16);
        else if (size == 2) {
            if (offset & 2u) return;
            b = (uint8_t)value;
        } else {
            if ((offset & 3u) != 1u) return;
            b = (uint8_t)value;
        }
        i->control = b & 3u;
        ide_update(i);
    }
}

static int ide_stable(void *dev, uint32_t offset, unsigned size) {
    mc_f108_ide *i = dev;
    if (offset < IDE_BUS) return mc_ata_ops.stable ? mc_ata_ops.stable(i->ata, offset, size) : 0;
    return 1;
}

const mc_dev_ops mc_f108_ide_ops = {ide_read, ide_write, NULL, NULL, ide_stable};

void mc_f108_ide_init(mc_f108_ide *i, const mc_hw_host *host, const char *name, mc_ata *ata) {
    memset(i, 0, sizeof *i);
    i->dev.ops = &mc_f108_ide_ops;
    i->dev.host = host;
    i->dev.name = name;
    i->ata = ata;
}
