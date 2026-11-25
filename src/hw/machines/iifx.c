/* Livadix — the Macintosh IIfx board.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990): chapter
 * 1 (the IIfx: 68030 and 68882 at 40 MHz, six NuBus slots, the OSS, FMC and
 * two IOPs), chapter 2 (the SCSI DMA IC: the 53C80's functions plus DMA;
 * the slot interrupts to the OSS), chapter 3 ("General logic circuits in
 * the Macintosh IIfx computer": the OSS (oss.h) and the FMC, the Fast
 * Memory Controller of the cache, RAM and ROM), chapter 4 (VIA1 PB0-2 the
 * RTC; PA6/4/2/1 CPU.ID3-0); the IOP SWIM Driver ERS (iop.h). Designing
 * Cards and Drivers for the Macintosh Family, 3rd ed., chapter 3 (NuBus
 * slots $9-$E).
 *
 * Observed from the IIfx ROM 4147DD77: its decoder table (ROM offset $351C, valid mask $001F9007) names
 * VIA1 $50F0 0000, the sound chip $50F1 0000, the SCSI DMA IC $50F0 8000,
 * the SWIM IOP $50F1 2020, the SCC IOP $50F0 4020, the OSS $50F1 A000, the
 * FMC $50F1 C000 and the RAM parity unit $50F1 E000; its ProductInfo record
 * (ROM offset $37C0, box flag 7) matches VIA1 port A under the mask $56
 * against $52 (PA6, PA4 and PA1 high), with the clock and PRAM on VIA1's
 * RTC lines. The IIfx has no built-in video: the screen is the Livadix Display
 * Card.
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../absent.h"
#include "../adb.h"
#include "../adb_wire.h"
#include "../iop.h"
#include "../rtc.h"
#include "../via.h"
#include "../enet_card.h"
#include "../livadix_card.h"
#include "../nubus.h"
#include "../asc.h"
#include "../ncr5380.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../absent.h"
#include "../asic/oss.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_IMAGE 0x40000u

/* VIA1 port A inputs: CPU.ID3-0 = 1, 1, 0, 1 (PA6, PA4, PA2, PA1) as the
 * ProductInfo record wants them; the other inputs read high. */
#define VIA1_PA_IN 0xFBu

/* The cache: 32 KB of data in lines of four long words, one tag each
 * (Guide, chapter 5, "RAM cache in the Macintosh IIfx computer"). */
#define CACHE_LINES 2048u
#define CACHE_VALID 1u
#define ROM_LOW 0x00800000u     /* the ROM's image below ROMBase ($4080 0000) */

typedef struct fx {
    mc_hw *hw;
    mc_via via1;
    mc_rtc rtc;
    mc_oss oss;
    mc_dev fmc;                 /* the FMC's configuration register */
    uint32_t bank_a;            /* bank A's size in bytes */
    uint16_t fmc_reg, fmc_config;   /* its shift register; the value last latched */
    mc_absent rpu;              /* the RAM parity unit: absent, as without the parity option (ROM probe $39E2) */
    uint32_t dma_control;       /* the SCSI DMA IC's control register (+$80) */
    /* The 32 KB cache (see "the cache"): a tag per line of 16 bytes, the
     * lines whose data differs from the memory at their tag (with that
     * data), and the regions that route RAM and the ROM's low image
     * through it. */
    uint32_t cache_tag[CACHE_LINES];    /* line address | CACHE_VALID */
    uint8_t cache_stale[CACHE_LINES];
    uint8_t cache_data[CACHE_LINES][16];
    unsigned cache_nstale;
    int cache_on;               /* OSS control bit 3 */
    int cache_ram;              /* RAM routed through the cache (cache_ram_region) */
    int cache_ram_region, cache_rom_region;
    mc_iop scc_iop, swim_iop;   /* the I/O Processors in front of the SCC and the SWIM */
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card, in card_slot */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    mc_serial_host serial;
    int chime_done;             /* emulation.startup_sound off: the ROM's chime is over */
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adb_wire adb_wire;       /* the ADB line as the SWIM IOP's port drives it */
    uint64_t fb_serial;
    int overlay;
    int overlay_region;     /* the trap over the ROM's own range while the overlay is on */
    char nvram[1024];
    int64_t host_start;     /* the host's time given at start (mc_machine_set_clock) */
} fx;

static const mc_region fx_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
};

static const mc_irq_route fx_irqs[] = {
    {"oss1", 1}, {"oss2", 2}, {"oss3", 3}, {"oss4", 4}, {"oss5", 5}, {"oss6", 6}, {"nmi", 7},
};

/* --- the reset overlay -------------------------------------------------------- */

static void map_ram(fx *q);

static void overlay_on(fx *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    /* The ROM answers at 0 (through the RAM space) and its own range traps
     * the first access, which releases the overlay. */
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(fx *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->cache_rom_region, ROM_BASE, ROM_LOW);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    fx *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    fx *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

/* Two banks of four SIMMs: bank A from 0, bank B from where the FMC's
 * configuration bits 7-6 put it (fmc_write: the size of bank A), each
 * repeating through its window (bank A's up to bank B, bank B's 64 MB);
 * the space above floats†. */
static uint32_t bank_b(const fx *q) {
    static const uint8_t mb[4] = {4, 2, 16, 64};
    return (uint32_t)mb[(q->fmc_config >> 6) & 3u] << 20;
}

/* The RAM byte at a physical address below $4000 0000 as map_ram lays the
 * banks out; NULL where the bus floats. */
static uint8_t *ram_byte(fx *q, uint32_t a) {
    mc_hw *hw = q->hw;
    const uint32_t b = bank_b(q);
    if (a < b) return hw->ram + a % q->bank_a;
    if (hw->ram_size > q->bank_a && a - b < (64u << 20)) return hw->ram + q->bank_a + (a - b) % (hw->ram_size - q->bank_a);
    return NULL;
}

static void map_ram(fx *q) {
    mc_hw *hw = q->hw;
    if (q->cache_ram) {
        mc_bus_remap(&hw->bus, q->cache_ram_region, 0, 0x40000000u);
        mc_hw_maps_changed(hw);
        return;
    }
    const uint32_t b = bank_b(q);
    mc_bus_map_float(&hw->bus, 0, 0x40000000u);
    mc_bus_map_memory(&hw->bus, 0, b, hw->ram, q->bank_a, 1);
    if (hw->ram_size > q->bank_a) mc_bus_map_memory(&hw->bus, b, 64u << 20, hw->ram + q->bank_a, hw->ram_size - q->bank_a, 1);
    mc_hw_maps_changed(hw);
}

/* --- the cache ---------------------------------------------------------------------- */

/* The FMC's 32 KB cache of RAM and ROM (Guide, chapter 5, "RAM cache in
 * the Macintosh IIfx computer"): direct mapped, a tag per line of four long
 * words; a read that hits is served from the cache, a burst read that
 * misses fills the line and writes its tag, a single read leaves the cache
 * as it is, and a write that hits updates the line. Kept coherent, it is
 * invisible; the ROM's cache test (ROM offset $42EB2) sees it through OSS
 * control bit 3: with the bit clear the cache serves no hits and a burst
 * fill writes the tag but not the data, so the line keeps the data of the
 * address it held before (the test reads the ROM at $4000 8000 through
 * the cache, clears the bit, reads RAM at $8000 by bursts, sets the bit and
 * expects RAM at $8000 to read the ROM's data; then, with RAM written again
 * and the bit clear, RAM's own).
 *
 * Livadix routes through it the ROM's image below ROMBase, $4000 0000-$407F
 * FFFF, which only that test reads, and RAM from the moment bit 3 is
 * cleared until it is set again and every line agrees with memory; other
 * accesses neither fill nor look up a line†. A burst is a read with the
 * 68030's data cache and data burst enabled (CACR ED and DBE, MC68030 UM
 * 6.1.3: its line fill reads four long words). */
static int cache_mem(fx *q, uint32_t a, uint8_t **p) {
    if (a >= ROM_BASE && a - ROM_BASE < 0x10000000u) {
        *p = q->hw->rom + ((a - ROM_BASE) & (q->hw->rom_size - 1u));
        return 0;   /* read only */
    }
    *p = a < ROM_BASE ? ram_byte(q, a) : NULL;
    return 1;
}

static int cache_line_agrees(fx *q, unsigned i) {
    const uint32_t line = q->cache_tag[i] & ~15u;
    for (unsigned k = 0; k < 16; k++) {
        uint8_t *p;
        cache_mem(q, line + k, &p);
        if ((p ? *p : 0) != q->cache_data[i][k]) return 0;
    }
    return 1;
}

static void cache_set_stale(fx *q, unsigned i, int stale) {
    if (stale == q->cache_stale[i]) return;
    q->cache_stale[i] = (uint8_t)stale;
    q->cache_nstale = stale ? q->cache_nstale + 1u : q->cache_nstale - 1u;
}

/* RAM back on the fast path once the cache is on and holds nothing stale. */
static void cache_settle(fx *q) {
    if (q->cache_ram && q->cache_on && !q->cache_nstale) {
        q->cache_ram = 0;
        if (!q->overlay) map_ram(q);
    }
}

static uint8_t cache_read_byte(fx *q, uint32_t a, int burst) {
    const unsigned i = (a >> 4) & (CACHE_LINES - 1u);
    const uint32_t line = a & ~15u;
    uint8_t *p;
    cache_mem(q, a, &p);
    const int hit = q->cache_tag[i] == (line | CACHE_VALID);
    if (hit && q->cache_on && q->cache_stale[i]) return q->cache_data[i][a & 15u];
    if (!hit && burst) {
        if (!q->cache_on && (q->cache_tag[i] & CACHE_VALID)) {
            /* The tag moves; the data stays that of the old line. */
            if (!q->cache_stale[i])
                for (unsigned k = 0; k < 16; k++) {
                    uint8_t *o;
                    cache_mem(q, (q->cache_tag[i] & ~15u) + k, &o);
                    q->cache_data[i][k] = o ? *o : 0;
                }
            q->cache_tag[i] = line | CACHE_VALID;
            cache_set_stale(q, i, !cache_line_agrees(q, i));
        } else {
            q->cache_tag[i] = line | CACHE_VALID;
            cache_set_stale(q, i, 0);
        }
    }
    return p ? *p : 0;
}

static uint32_t cache_read(fx *q, uint32_t a, unsigned size) {
    const int burst = (q->hw->cpu->cacr & 0x1100u) == 0x1100u;
    uint32_t v = 0;
    for (unsigned k = 0; k < size; k++) v = v << 8 | cache_read_byte(q, a + k, burst);
    return v;
}

static void cache_write(fx *q, uint32_t a, unsigned size, uint32_t value) {
    for (unsigned k = 0; k < size; k++) {
        const uint32_t b = a + k;
        const uint8_t v = (uint8_t)(value >> 8 * (size - 1u - k));
        uint8_t *p;
        if (cache_mem(q, b, &p) && p) *p = v;
        const unsigned i = (b >> 4) & (CACHE_LINES - 1u);
        if (q->cache_stale[i] && q->cache_tag[i] == ((b & ~15u) | CACHE_VALID)) {
            q->cache_data[i][b & 15u] = v;
            if (cache_line_agrees(q, i)) cache_set_stale(q, i, 0);
        }
    }
    cache_settle(q);
}

static uint32_t cram_read(void *dev, uint32_t offset, unsigned size) { return cache_read(dev, offset, size); }
static void cram_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { cache_write(dev, offset, size, value); }
static const mc_dev_ops cram_ops = {cram_read, cram_write, NULL, NULL};
static uint32_t crom_read(void *dev, uint32_t offset, unsigned size) { return cache_read(dev, ROM_BASE + offset, size); }
static void crom_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { cache_write(dev, ROM_BASE + offset, size, value); }
static const mc_dev_ops crom_ops = {crom_read, crom_write, NULL, NULL};

/* OSS control bit 3. */
static void cache_switch(fx *q, int on) {
    q->cache_on = on;
    if (!on && !q->cache_ram) {
        q->cache_ram = 1;
        if (!q->overlay) map_ram(q);
    }
    cache_settle(q);
}

static void cache_reset(fx *q) {
    memset(q->cache_tag, 0, sizeof q->cache_tag);
    memset(q->cache_stale, 0, sizeof q->cache_stale);
    q->cache_nstale = 0;
}

/* --- wiring ------------------------------------------------------------------------ */

/* The SWIM IOP's port: bit 1 pulls the ADB line low, bit 2 reads the
 * line's level (iop.h). */
static void adb_port_out(void *ctx, uint8_t value, uint64_t t) {
    fx *q = ctx;
    mc_adb_wire_host(&q->adb_wire, (value & 0x02u) != 0, t);
}
static uint8_t adb_port_in(void *ctx, uint64_t t) {
    fx *q = ctx;
    return mc_adb_wire_level(&q->adb_wire, t) ? 0x04u : 0;
}

static void via1_port_b(fx *q, uint8_t pb) {
    const int data = (q->via1.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&q->rtc, pb & 4u, pb & 2u, data);
}

static void board_line(void *board, void *dev, int line, int state) {
    fx *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_oss_set(&q->oss, MC_OSS_VIA1, state);
        else if (line == MC_VIA_LINE_PB) via1_port_b(q, (uint8_t)state);
    } else if (dev == &q->oss) {
        /* The OSS drives the processor's IPL with the level it chose. */
        if (line == MC_OSS_LINE_IPL) {
            static const char *const names[] = {NULL, "oss1", "oss2", "oss3", "oss4", "oss5", "oss6"};
            for (int l = 1; l <= 6; l++) mc_irq_set(&hw->irq, mc_hw_irq(hw, names[l]), state == l);
        } else if (line == MC_OSS_LINE_POWER_OFF) mc_hw_power_off(hw);
        else if (line == MC_OSS_LINE_CACHE) cache_switch(q, state);
    } else if (dev == &q->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&q->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&q->via1, state);
    } else if (dev == &q->scsi) {
        if (line == MC_NCR5380_LINE_IRQ) mc_oss_set(&q->oss, MC_OSS_SCSI, state);
    } else if (dev == &q->scc_iop) {
        if (line == MC_IOP_LINE_IRQ) mc_oss_set(&q->oss, MC_OSS_SCC_IOP, state);
    } else if (dev == &q->swim_iop) {
        if (line == MC_IOP_LINE_IRQ) mc_oss_set(&q->oss, MC_OSS_SWIM_IOP, state);
    } else if (dev == &q->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_oss_set(&q->oss, MC_OSS_SOUND, state);
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
    }
}

/* The NuBus slots' interrupt lines (active low) to the OSS, slot $9 its
 * source 0 (Guide, chapter 2). */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    fx *q = ctx;
    (void)slotirq_high;
    for (unsigned b = 0; b < 6; b++)
        if (mask >> b & 1u) mc_oss_set(&q->oss, MC_OSS_SLOT9 + b, !(levels >> b & 1u));
}

/* --- persistence ------------------------------------------------------------------ */

/* Livadix's PRAM file: "LVDXPRAM", a version byte, the clock as seconds since
 * 1904 minus the host's time (signed 64-bit, little-endian), the 256 bytes. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

/* A new PRAM file: the PRAM a ROM writes when it finds it invalid, as the
 * Quadra 700's (quadra700.c: the original 20 bytes, the extended PRAM's
 * signature "NuMc", the default startup device), with 24-bit addressing:
 * the IIfx's Systems from 6.0.8 run in it (System 6.0.8 refuses 32-bit
 * addressing). */
static void fresh_pram(uint8_t *p) {
    static const uint8_t original[16] = {0xA8, 0x00, 0x00, 0x00, 0xCC, 0x0A, 0xCC, 0x0A,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00};
    static const uint8_t original_low[8] = {0x13, 0x88, 0x00, 0x4C, 'N', 'u', 'M', 'c'};
    static const uint8_t startup[6] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF};
    memset(p, 0, 256);
    memcpy(p + 0x10, original, sizeof original);
    memcpy(p + 0x08, original_low, sizeof original_low);
    memcpy(p + 0x76, startup, sizeof startup);
}

static void load_nvram(fx *q) {
    FILE *f = q->nvram[0] ? fopen(q->nvram, "rb") : NULL;
    uint8_t buf[8 + 1 + 8 + 256];
    if (f && fread(buf, 1, sizeof buf, f) == sizeof buf && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1) {
        int64_t offset = 0;
        offset = mc_nvram_clock_offset(buf + 9);
        memcpy(q->rtc.pram, buf + 17, 256);
        q->rtc.seconds = (uint32_t)offset;  /* the host's time is added by set_clock */
    } else {
        fresh_pram(q->rtc.pram);
    }
    if (f) fclose(f);
}

static void save_nvram(fx *q) {
    if (!q->nvram[0]) return;
    uint8_t buf[8 + 1 + 8 + 256];
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t offset = (int64_t)mc_rtc_seconds(&q->rtc) - q->host_start -
                           (int64_t)(mc_hw_now(q->hw) / MC_C15M_HZ);
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    memcpy(buf + 17, q->rtc.pram, 256);
    FILE *f = fopen(q->nvram, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

/* The screen depth video.depth asks for, on the Livadix Display Card: its slot's PRAM (eight bytes per
 * slot from extended PRAM $46 for slot $9, as the Quadra 700 ROM keeps
 * them: the board ID, then vendorUse1, the depth mode the system starts
 * in; Designing Cards and Drivers for the Macintosh Family, 3rd ed.,
 * chapter 9, "Video drivers"), once the Slot Manager has set the record up
 * for the card. */
static void set_card_depth(fx *q) {
    const mc_config *c = &q->hw->config;
    uint8_t *p = q->rtc.pram + 0x46 + 8 * (q->card_slot - 9);
    if (q->rtc.pram[0x10] != 0xA8 || p[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || p[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

/* --- storage -------------------------------------------------------------------- */

/* The CD-ROM drive's SCSI ID and the IDs hard disks take
 * in the configured order; 7 is the Macintosh. */
#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const mc_vdisk_drivers drivers = {driver, size, NULL, 0};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(fx *q) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
        q->vdisk[id] = NULL;
    }
}

/* The hard disks of hw->media on their SCSI IDs: `storage.disks` entries
 * with a scsi_id there, the others in order on the free IDs; images beyond
 * the free IDs become further partitions of the last disk. */
static int layout_disks(fx *q, char *error, size_t capacity) {
    const mc_config *c = &q->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(q);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&q->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&q->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || (i < c->disk_count && c->disks[i].scsi_id_set)) continue;
        int id = -1;
        for (size_t k = 0; k < sizeof disk_ids / sizeof *disk_ids && id < 0; k++)
            if (!count[disk_ids[k]]) id = disk_ids[k];
        if (id < 0) id = last;
        if (id < 0) continue;
        group[id][count[id]++] = d;
        last = id;
    }
    for (int id = 0; id < 7; id++) {
        if (!count[id]) continue;
        char problem[256];
        q->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!q->vdisk[id]) {
            if (capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(q);
            return 0;
        }
        mc_scsi_disk_init(&q->unit[id], q->vdisk[id]);
        mc_scsi_bus_attach(&q->scsi_bus, id, mc_scsi_unit_target(&q->unit[id]));
    }
    return 1;
}

/* The Macintosh ejected the disc (START STOP UNIT). */
static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    fx *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    fx *q = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(q, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&q->swim, drive, disk, 0, error, capacity);
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&q->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&q->unit[CD_ID], vd);
    q->vdisk[CD_ID] = vd;
    return 1;
}

/* The chips' media against hw->media: what the guest ejected leaves its
 * chip and is marked ejected (Ctrl+Alt+R puts it back); what the host
 * ejected leaves the chip. */
static void board_poll(void *board) {
    fx *q = board;
    mc_media *media = &q->hw->media;
    for (unsigned i = 0; i < q->swim.drives; i++) {
        int ejected = 0, gone = 0;
        mc_disk *chip = mc_swim_media(&q->swim, i, &ejected);
        mc_disk *d = mc_media_get(media, MC_MEDIA_FLOPPY, i, &gone);
        if (!chip) continue;
        if (ejected || chip != d || gone) {
            mc_swim_flush(&q->swim);
            mc_swim_remove(&q->swim, i);
            if (ejected && chip == d && !gone) mc_media_eject(media, MC_MEDIA_FLOPPY, i);
        }
    }
    int gone = 0;
    mc_disk *cd = mc_media_get(media, MC_MEDIA_CDROM, 0, &gone);
    if (q->vdisk[CD_ID] && (!cd || gone)) {
        mc_vdisk *old = mc_scsi_unit_remove(&q->unit[CD_ID]);
        if (old) mc_vdisk_destroy(old);
        q->vdisk[CD_ID] = NULL;
    }
}

/* --- sound --------------------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    fx *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

/* emulation.startup_sound off: the output stays silent until the ROM's
 * chime is over, the first sound played to the end (both FIFOs drained and every
 * frame made from them pulled). */
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    fx *q = board;
    const size_t n = mc_asc_pull(&q->asc, stereo, frames);
    if (!q->hw->config.startup_sound && !q->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (q->asc.played && !mc_asc_fifo_level(&q->asc, 0) && !mc_asc_fifo_level(&q->asc, 1) && !q->asc.ring_count) q->chime_done = 1;
    }
    return n;
}

/* --- serial ports -------------------------------------------------------------- */

/* Port 0 is the modem port, SCC channel A; port 1 the printer port, B. */
static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    fx *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    fx *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}

static void board_serial_host(void *board, const mc_serial_host *host) {
    fx *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    fx *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    fx *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
}

/* --- the FMC ----------------------------------------------------------------------- */

/* The FMC's configuration: a 16-bit shift register reached one bit at a
 * time in bit 0 of its byte, as the IIfx ROM uses it:
 * a write shifts its bit in at the top (sixteen writes, least significant
 * bit first, load a value: $F3FF at start, ROM offset $2E60); a read gives
 * the bottom bit and rotates it to the top (sixteen reads give the value,
 * least significant bit first, and leave it in place; the ROM's FMC test at
 * $42E42); a write at +$10 latches it as the configuration (ROM offset $B8A).
 * Bits 7-6 give bank A's size, where bank B starts: 4, 2, 16 or 64 MB for
 * 0-3 (the ROM's table at ROM offset $B96, which it fills in from the RAM it
 * sized, $B56). The other bits (RAM and ROM timing) are kept without
 * effect†; the ROM and the Systems load only $F3FF and bank A's code, and
 * the cache follows OSS control bit 3 ("the cache"). */
static uint32_t fmc_read(void *dev, uint32_t offset, unsigned size) {
    fx *q = (fx *)((char *)dev - offsetof(fx, fmc));
    (void)offset;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const unsigned b = q->fmc_reg & 1u;
        q->fmc_reg = (uint16_t)(q->fmc_reg >> 1 | b << 15);
        v = v << 8 | b;
    }
    return v;
}
static void fmc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    fx *q = (fx *)((char *)dev - offsetof(fx, fmc));
    if (offset & 0x10u) {
        const int moved = (q->fmc_config ^ q->fmc_reg) & 0xC0u;
        q->fmc_config = q->fmc_reg;
        if (moved && !q->overlay) map_ram(q);
        return;
    }
    for (unsigned i = 0; i < size; i++)
        q->fmc_reg = (uint16_t)(q->fmc_reg >> 1 | ((value >> 8 * (size - 1u - i)) & 1u) << 15);
}
static const mc_dev_ops fmc_ops = {fmc_read, fmc_write, NULL, NULL};

/* --- SCSI ------------------------------------------------------------------------ */

/* The SCSI DMA IC's 53C80 registers every $10 bytes (register number in
 * A6-A4), as the ROM's SCSI Manager addresses them. In DMA mode the ROM
 * moves the data through the registers themselves, polling DRQ in the Bus
 * and Status register: it reads the Input Data register (6) after a Start
 * DMA Initiator Receive (ROM offset $8D48) and writes the Output Data
 * register (0) after a Start DMA Send; the IC makes those accesses the DMA
 * handshake (DACK cycles). Its own DMA channel (the control register at
 * +$80) is not modelled†. */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) {
    mc_ncr5380 *c = dev;
    const uint32_t reg = (offset >> 4) & 7u;
    if (reg == 6 && c->dma_run == 2) {
        uint32_t v = 0;
        mc_ncr5380_dma_read(c, 1, &v);
        return v;
    }
    return mc_ncr5380_ops.read(dev, reg, size);
}
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr5380 *c = dev;
    const uint32_t reg = (offset >> 4) & 7u;
    if (reg == 0 && c->dma_run == 1) mc_ncr5380_dma_write(c, 1, value & 0xFFu);
    else mc_ncr5380_ops.write(dev, reg, size, value);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

/* The IC's DMA registers from +$80. The ROM's SCSI Manager, with its DMA
 * flag set (SCSI globals +$67 bit 4, from HWCfgFlags $DD2 bit 7, ROM offset
 * $97FE), writes the control register at +$80: 8 before a transfer in DMA
 * mode, 2 after it and at start (ROM offsets $88A0, $8E28, $94BC), and its
 * device test reads the long there ($3212); the data still moves through
 * the 53C80's registers. Its routines that run the channel itself (control
 * 7, the address at +$100, the count at +$C0, +$140; ROM offsets $9042,
 * $91D0) are installed only by code the ROM skips (the BRA at ROM offset
 * $9624 jumps over the installation at $9626-$9652), and System 6.0.8, 7.1
 * and 7.5.3 access nothing else here: the control register is kept and
 * reads back, the rest reads 0 and ignores writes†. */
static uint32_t dma_read(void *dev, uint32_t offset, unsigned size) {
    const fx *q = dev;
    if (offset + size <= 4u) return (q->dma_control >> 8 * (4u - offset - size)) & (size == 4u ? 0xFFFFFFFFu : (1u << 8 * size) - 1u);
    return 0;
}
static void dma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    fx *q = dev;
    if (offset == 0 && size == 4u) q->dma_control = value;
}
static const mc_dev_ops dma_ops = {dma_read, dma_write, NULL, NULL};


/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    fx *q = board;
    mc_via_ops.reset(&q->via1);
    cache_reset(q);
    mc_oss_ops.reset(&q->oss);
    mc_livadix_card_ops.reset(&q->card);
    mc_enet_card_reset(q->enet);
    mc_ncr5380_reset(&q->scsi);
    mc_iop_ops.reset(&q->scc_iop);
    mc_iop_ops.reset(&q->swim_iop);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_swim_ops.reset(&q->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    fx *q = board;
    mc_livadix_card_view cv;
    if (!mc_livadix_card_view_get(&q->card, &cv)) return 0;
    fb->width = cv.width;
    fb->height = cv.height;
    fb->depth = cv.depth;
    fb->row_bytes = cv.row_bytes;
    fb->pixels = cv.pixels;
    fb->palette = cv.palette;
    fb->serial = ++q->fb_serial;   /* VRAM is written through the CPU's fast path: always fresh */
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    fx *q = board;
    return mc_adb_keyboard_key(&q->keyboard, code, down);
}

static int board_mouse(void *board, int dx, int dy, int button) {
    fx *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    fx *q = board;
    q->host_start = host_seconds;
    mc_rtc_set_seconds(&q->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&q->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    fx *q = board;
    save_nvram(q);
    mc_swim_flush(&q->swim);
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    mc_scsi_bus_free(&q->scsi_bus);
    mc_livadix_card_free(&q->card);
    mc_enet_card_free(q->enet);
    free(q);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    fx *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    q->hw = hw;
    hw->board = q;
    /* Bank A takes the larger SIMMs (4 x 1, 4 or 16 MB), bank B the rest. */
    q->bank_a = 64u << 20;
    while (q->bank_a > hw->ram_size) q->bank_a >>= 2;
    q->fmc_config = 0xF3FFu;    /* bank B at 64 MB until the ROM loads its value */
    const int io = mc_hw_region(hw, "io");
    /* Every I/O bus cycle takes 0.5 us of guest time, as on the Quadra 700
     * (quadra700.c: the Guide's VIA access time, chapter 4). */
    hw->bus.region[io].cost = 8;
    /* The I/O block (the decoder table, above). */
    mc_via_init(&q->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, &q->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_oss_init(&q->oss, &hw->host, "OSS");
    mc_bus_attach(&hw->bus, io, 0x1A000, MC_OSS_SIZE, &q->oss, &mc_oss_ops, MC_WIN_PORT8);
    q->fmc.ops = &fmc_ops;
    q->fmc.host = &hw->host;
    q->fmc.name = "FMC";
    mc_bus_attach(&hw->bus, io, 0x1C000, 0x2000, &q->fmc, &fmc_ops, MC_WIN_PORT8);
    mc_absent_init(&q->rpu, &hw->host, "RPU", NULL, 0x00);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->rpu, &mc_absent_ops, 0);
    /* The SCC behind its IOP: PCLK and RTxC as on the Quadra 700. */
    mc_iop_init(&q->scc_iop, &hw->host, MC_IOP_SCC, &q->scc, &mc_scc_ops);
    mc_scc_init(&q->scc, mc_iop_chip_host(&q->scc_iop));
    mc_scc_set_clocks(&q->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x04000, 0x2000, &q->scc_iop, &mc_iop_ops, MC_WIN_PORT8);
    /* SCSI: the SCSI DMA IC's 53C80, the hard disks (layout_disks) and the
     * CD-ROM drive at ID 3. */
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr5380_init(&q->scsi, &hw->host, &q->scsi_bus, 1);
    mc_bus_attach(&hw->bus, io, 0x08000, 0x0080, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x08080, 0x1F80, q, &dma_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    /* Sound and the floppy controller behind its IOP. */
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ORIGINAL);
    mc_bus_attach(&hw->bus, io, 0x10000, 0x2000, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_iop_init(&q->swim_iop, &hw->host, MC_IOP_SWIM, &q->swim, &mc_swim_ops);
    mc_swim_init(&q->swim, mc_iop_chip_host(&q->swim_iop), hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x12000, 0x2000, &q->swim_iop, &mc_iop_ops, MC_WIN_PORT8);
    /* NuBus: slots $9-$E, the Livadix Display Card in the first. */
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)0x7Eu, slot_pins, q);
    for (unsigned slot = 0x9; slot <= 0xE; slot++) mc_nubus_line(&q->nubus, slot, slot - 9);
    {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        q->card_slot = mc_nubus_free_slot(&q->nubus);
        if (mode < 0 || !q->card_slot ||
            !mc_livadix_card_init(&q->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&q->card, &q->nubus, q->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_livadix_card_free(&q->card);
            free(q);
            return NULL;
        }
    }
    /* The Ethernet card (network.card). */
    q->enet = mc_enet_card_fit(hw, &q->nubus, MC_ENET_CARD_NUBUS, mc_nubus_free_slot(&q->nubus));
    if (!q->enet && hw->error[0]) {
        mc_livadix_card_free(&q->card);
        free(q);
        return NULL;
    }
    /* ADB: the transceiver on VIA1 and an Apple Extended Keyboard and mouse. */
    mc_adb_bus_init(&q->adb);
    mc_adb_keyboard_init(&q->keyboard);
    mc_adb_mouse_init(&q->mouse);
    mc_adb_attach(&q->adb, &q->keyboard.dev);
    mc_adb_attach(&q->adb, &q->mouse.dev);
    mc_adb_wire_init(&q->adb_wire, &q->adb);
    const mc_iop_port adb_port = {q, adb_port_out, adb_port_in};
    mc_iop_set_port(&q->swim_iop, &adb_port);
    /* The RTC and PRAM. */
    mc_rtc_init(&q->rtc, &hw->host, "RTC");
    if (mc_hw_nvram_path(hw, q->nvram, sizeof q->nvram) == NULL) q->nvram[0] = 0;
    load_nvram(q);
    set_card_depth(q);
    /* storage.boot_priority: cdrom names the CD-ROM drive the default
     * startup device in PRAM ($78-$7B, which the ROM reads at startup): the
     * scsiDev form of the Start Manager's DefStartRec, two reserved bytes
     * and the driver reference number of SCSI ID 3, -(32 + 3 + 1) (Inside
     * Macintosh V, "The Start Manager", GetDefaultStartup; "The SCSI
     * Manager", driver reference numbers). */
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        q->rtc.pram[0x78] = q->rtc.pram[0x79] = 0;
        q->rtc.pram[0x7A] = (uint8_t)(refnum >> 8);
        q->rtc.pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive. */
    mc_via_set_pa(&q->via1, VIA1_PA_IN, 0xFFu);
    mc_via_set_pb(&q->via1, 0xFFu, 0xFFu);
    /* The cache's routes (the cache). */
    q->cache_ram_region = mc_bus_add_io(&hw->bus, 0, 0x40000000u, 0xFFFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->cache_ram_region, 0, 0x40000000u, q, &cram_ops, 0);
    q->cache_rom_region = mc_bus_add_io(&hw->bus, ROM_BASE, ROM_LOW, ROM_LOW - 1u, 0);
    mc_bus_attach(&hw->bus, q->cache_rom_region, 0, ROM_LOW, q, &crom_ops, 0);
    /* The overlay. */
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_rtc_ops.reset(&q->rtc);
    return q;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((fx *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((fx *)board)->enet, frame, length);
}

static const mc_board_ops fx_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const fx_models[] = {"iifx", NULL};
static const uint32_t fx_roms[] = {0x4147DD77u, 0};
/* Eight 64-pin SIMM sockets in two banks of four, 1 or 4 MB SIMMs (Guide,
 * chapter 5; Apple's specifications: 4 to 128 MB with 16 MB SIMMs). */
static const uint16_t fx_ram[] = {4, 8, 16, 20, 32, 64, 68, 80, 128, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static const mc_hw_caps fx_caps = {NULL, 1, MC_FEATURE_NUBUS, NULL};

const mc_hw_desc mc_iifx_desc = {
    "iifx", fx_models, M68K_68030, M68K_FPU_68882, fx_roms, fx_ram,
    fx_map, sizeof fx_map / sizeof *fx_map, fx_irqs, sizeof fx_irqs / sizeof *fx_irqs,
    build, &fx_ops, MC_MMU_030, NULL, &fx_caps,
};
