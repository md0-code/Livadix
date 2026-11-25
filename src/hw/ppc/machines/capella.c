/* Livadix — Power Macintosh 5200/5300 and 6200/6300 (Capella) boards.
 *
 * Apple, Power Macintosh 5200/6200 Developer Note (1995), chapter 2:
 * Capella bridges the 603's 64-bit bus to the 68040 bus; F108's memory
 * controller is identical to the 630's, with an SCC (8 MHz PCLK), a 53C96
 * (18.75 MHz), and IDE; PrimeTime II holds the VIAs, SWIM II and sound;
 * Cuda supplies ADB, PRAM, RTC and soft power. Figure 2-2 reserves the
 * first GiB for RAM, the old ROM space at $40000000, display RAM at
 * $F9000000-$F97FFFFF and a high ROM image. Chapter 4, Table 4-1 gives
 * the two 72-pin SIMMs' populations: no soldered RAM, 8 through 64 MiB.
 * Chapter 3 describes the 5200's fixed 640x480 display and the 6200's
 * external monitors; the built-in controller is Valkyrie.
 *
 * ROM 63ABFD3F observations supplied for phase 6.7: Capella at $53000000,
 * its active-low IPL at +$24 and change-latch acknowledgement at +$1C;
 * the read-only CPU ID at $5FFFFFFC is $A55A225C/$2259/$2254/$2251.
 * The ROM ORs $3000 into the low word for the 5200/5300/6200/6300
 * ProductInfo keys. ROM $FFF03684 selects a 37.5 MHz bus for speed bits
 * 4 and a 33 MHz bus for bits 1. Time base is bus/4 (MPC603EUM, chapter 2).
 * ROM aliases at $41000000 and $48000000 are used during Capella setup.
 *
 * Reuses Livadix's original quadra630.c chip orchestration and pdm.c's PPC
 * board interface. The SIMM-to-F108 bank geometry, later models' shared
 * board wiring and flat 8/9 CPI timing remain †;
 * no ROM patch or substitute guest service is installed.
 *
 * The LC processor-direct slot (chapter 1, "Expansion slots"): NuBus slot
 * $E's standard and super spaces through Capella's 68040 bus, as on the
 * 630 whose I/O system this board shares; its /NMRQ on VIA2 port A bit 5
 * and /SLOTIRQ (VIA2 CA1) with the F108's interrupt byte. It takes the
 * Ethernet LC Card (network.card, enet_card.h).
 */
#include "../ppc_machine.h"
#include "../chips/capella.h"
#include "../../absent.h"
#include "../../adb.h"
#include "../../adbctl.h"
#include "../../asc.h"
#include "../../enet_card.h"
#include "../../ncr53c96.h"
#include "../../nubus.h"
#include "../../scc.h"
#include "../../scsi_bus.h"
#include "../../scsi_target.h"
#include "../../swim.h"
#include "../../via.h"
#include "../../asic/f108.h"
#include "../../asic/primetime2.h"
#include "../../guest/guest.h"
#include "../../storage/vdisk.h"
#include "../../video/valkyrie.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1u << 20)
#define IO_BASE 0x50000000u
#define IO_SIZE 0x03000000u
#define IO_IMAGE 0x00040000u
/* The 630's measured Cuda startup timing: 8 C15M periods per I/O cycle,
 * 16 for VIAs (quadra630.c, VIA_COST); inherited for PrimeTime II †. */
#define IO_COST 8u
#define VIA_COST 16u

static const uint32_t capella_roms[] = {0x63ABFD3Fu, 0};
static const uint16_t capella_ram[] = {8, 12, 16, 20, 24, 32, 36, 40, 48, 64, 0};

static const mc_region capella_map[] = {
    {"ram", 0x00000000u, 0x40000000u, 0, MC_REGION_RAM},
    {"rom", 0x40000000u, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, IO_SIZE, IO_IMAGE - 1u, MC_REGION_IO},
    {"capella", 0x53000000u, MC_CAPELLA_SIZE, MC_CAPELLA_SIZE - 1u, MC_REGION_IO},
    {"cpuid", 0x5FFFF000u, 0x1000u, 0xFFFu, MC_REGION_IO},
    {"rom-alias", 0xFFC00000u, 0x00400000u, 0, MC_REGION_ROM},
};

typedef struct capella_board {
    uint16_t id;
    uint32_t bus_hz;
    int panel;
} capella_board;

static const capella_board board_5200 = {0x225C, 37500000u, 1};
static const capella_board board_5300 = {0x2259, 33000000u, 1};
static const capella_board board_6200 = {0x2254, 37500000u, 0};
static const capella_board board_6300 = {0x2251, 33000000u, 0};

typedef struct capella_machine {
    mc_ppc *m;
    const capella_board *board;
    mc_capella bridge;
    mc_f108 f108;
    uint32_t banks[MC_F108_BANKS];
    mc_primetime2 pt2;
    mc_absent comm;
    mc_via via1, via2;
    mc_adbctl cuda;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];
    mc_vdisk *vdisk[7];
    mc_ata ata;
    mc_f108_ide ide;
    mc_vdisk *ata_vdisk;
    mc_valkyrie valkyrie;
    mc_serial_host serial;
    mc_nubus nubus;                 /* the LC processor-direct slot, NuBus slot $E */
    mc_enet_card *enet;             /* the Ethernet LC Card (network.card), NULL for none */
    int nubus_slotirq_high;         /* the slot's /NMRQ through mc_nubus */
    unsigned irq_mask;
    int resetting, in_reset, powered;
    int ide_slotirq, chime_done;
    int io_region, via_region, cache_region, tag_region;
    uint8_t cache[256u * 1024u], tags[128u * 1024u];
    uint64_t fb_serial;
    char nvram[1024];
    int64_t host_start;
} capella_machine;

/* L2 POST apertures: ROM $FFF089CC–$FFF08ADC; timing remains unmodelled †. */
static uint32_t tag_read(void *dev, uint32_t offset, unsigned size) {
    const capella_machine *q = dev;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) value = value << 8 | q->tags[offset + i];
    return value;
}
static void tag_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    capella_machine *q = dev;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t a = offset + i;
        q->tags[a] = (uint8_t)(value >> (8u * (size - i - 1u))) & ((a & 0x18u) ? 15u : 7u);
    }
}
static const mc_dev_ops tag_ops = {.read = tag_read, .write = tag_write};

static void map_cache(void *ctx, uint32_t control) {
    capella_machine *q = ctx;
    mc_bus *bus = &q->m->bus;
    if (!q->cache_region || !q->tag_region) return;
    if (control & 2u) mc_bus_map_memory(bus, 0x51000000u, sizeof q->cache, q->cache, sizeof q->cache, 1);
    else mc_bus_remap(bus, q->io_region, 0x51000000u, sizeof q->cache);
    mc_bus_remap(bus, control & 4u ? q->tag_region : q->io_region, 0x52000000u, sizeof q->tags);
    if (!(control & 2u)) mc_bus_remap(bus, q->via_region, 0x51000000u, 0x4000);
    if (!(control & 4u)) mc_bus_remap(bus, q->via_region, 0x52000000u, 0x4000);
}

/* Table 4-1 is a slot population, not a soldered bank plus extra memory.
 * Slot 1 occupies banks 0/1, slot 2 banks 2/3, bank 4 is absent. A SIMM
 * up to 16 MiB uses one bank; 32 MiB uses two 16 MiB banks †: the electrical
 * mapping of single/double-sided SIMMs to F108 bank selects is unverified.
 * mc_f108_map takes successive populated slices of the one m->ram buffer. */
static int ram_banks(uint32_t total_mb, uint32_t banks[MC_F108_BANKS]) {
    static const uint8_t slots[][2] = {
        {8, 0}, {8, 4}, {8, 8}, {16, 4}, {16, 8},
        {16, 16}, {32, 4}, {32, 8}, {32, 16}, {32, 32},
    };
    memset(banks, 0, MC_F108_BANKS * sizeof *banks);
    for (size_t i = 0; i < sizeof slots / sizeof *slots; i++) {
        if ((uint32_t)slots[i][0] + slots[i][1] != total_mb) continue;
        for (unsigned slot = 0; slot < 2; slot++) {
            const uint32_t mb = slots[i][slot];
            banks[2u * slot] = (mb > 16u ? 16u : mb) * MB;
            banks[2u * slot + 1u] = (mb > 16u ? mb - 16u : 0u) * MB;
        }
        return 1;
    }
    return 0;
}

static void map_ram(void *ctx) {
    capella_machine *q = ctx;
    mc_f108_map(&q->f108, &q->m->bus, q->m->ram);
}

/* Keep the 32-bit F108 register cycles intact, including its +$7C latch.
 * PrimeTime II and the byte chips are sized separately at attachment. */
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    capella_machine *q = dev;
    mc_adbctl_sync(&q->cuda);
    return mc_via_ops.read(&q->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    capella_machine *q = dev;
    mc_adbctl_sync(&q->cuda);
    mc_via_ops.write(&q->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    capella_machine *q = dev;
    return !q->cuda.firmware && mc_via_ops.stable && mc_via_ops.stable(&q->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* PrimeTime II's VIA2-compatible IFR exposes the live SCSI DREQ level on
 * CA2, not a 6522 edge latch. PowerPC Enabler 9.0.4, nitt 43 "Native 4.3",
 * resource +$5B1C-$5B34 polls IFR bit 0 without clearing it; the input
 * loop at +$2B1C-$2B44 copies until DREQ drops. A retained edge overruns
 * the guest buffer after the 53C96 has finished its DMA transfer. */
static void sync_scsi_dreq(capella_machine *q) {
    mc_via_set_ifr(&q->via2, MC_VIA_IRQ_CA2, q->scsi.drq);
}
static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    capella_machine *q = dev;
    sync_scsi_dreq(q);
    const uint32_t value = mc_via_ops.read(&q->via2, offset, size);
    sync_scsi_dreq(q);
    return value;
}
static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    capella_machine *q = dev;
    mc_via_ops.write(&q->via2, offset, size, value);
    sync_scsi_dreq(q);
}
static int via2_stable(void *dev, uint32_t offset, unsigned size) {
    capella_machine *q = dev;
    if (((offset >> 9) & 15u) == MC_VIA_IFR) return 0;
    return mc_via_ops.stable && mc_via_ops.stable(&q->via2, offset, size);
}
static const mc_dev_ops via2_ops = {via2_read, via2_write, NULL, NULL, via2_stable};

static uint32_t cpu_id_read(void *dev, uint32_t offset, unsigned size) {
    const capella_machine *q = dev;
    const uint32_t id = 0xA55A0000u | q->board->id;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++)
        value = value << 8 | ((id >> (8u * (3u - ((offset + i) & 3u)))) & 0xFFu);
    return value;
}
static void cpu_id_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}
static int cpu_id_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}
static const mc_dev_ops cpu_id_ops = {cpu_id_read, cpu_id_write, NULL, NULL, cpu_id_stable};

static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) {
    return mc_ncr53c96_ops.read(dev, offset >> 4, size);
}
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) {
    return mc_ncr53c96_ops.stable && mc_ncr53c96_ops.stable(dev, offset >> 4, size);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};

/* PrimeTime II's longword accumulation of SCSI data: each byte is DACK. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    (void)offset;
    uint32_t value = 0;
    mc_ncr53c96_dma_read(dev, size, &value);
    return value;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    mc_ncr53c96_dma_write(dev, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL, NULL};

/* --- interrupt and pin wiring ------------------------------------------------ */

static void board_reset(void *board);

static void set_irq(capella_machine *q, unsigned level, int state) {
    if (state) q->irq_mask |= 1u << level;
    else q->irq_mask &= ~(1u << level);
    if (q->resetting) return;
    unsigned highest = 7;
    while (highest && !(q->irq_mask & (1u << highest))) highest--;
    mc_capella_set_ipl(&q->bridge, highest);
}

static void update_slotirq(capella_machine *q) {
    mc_via_set_ca1(&q->via2, q->nubus_slotirq_high && !q->ide_slotirq);
}

static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    capella_machine *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    q->nubus_slotirq_high = slotirq_high;
    update_slotirq(q);
}

static void board_line(void *board, void *dev, int line, int state) {
    capella_machine *q = board;
    if (dev == &q->bridge) {
        if (line == MC_CAPELLA_LINE_CPU) mc_ppc_set_interrupt(q->m, state);
    } else if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) set_irq(q, 1, state);
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&q->cuda, (uint8_t)state);
        /* Guide, Table 4-2: VIA1 PA5 is the floppy SEL line. */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&q->swim, (state & 0x20) != 0);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) set_irq(q, 2, state);
        /* The 630's VIA2 T1 squares PB7 into VIA1 CA1 (v2VBL) †. */
        else if (line == MC_VIA_LINE_PB) mc_via_set_ca1(&q->via1, state & 0x80);
    } else if (dev == &q->cuda) {
        if (line == MC_ADBCTL_LINE_NMI) set_irq(q, 7, state);
        else if (line == MC_ADBCTL_LINE_POWER) {
            if (state) q->powered = 1;
            else if (q->powered) mc_ppc_power_off(q->m);
        } else if (line == MC_ADBCTL_LINE_RESET) {
            q->m->reset_held = state != 0;
            if (state && !q->in_reset) {
                q->in_reset = 1;
                if (!q->resetting) {
                    board_reset(q);
                    ppc_reset(&q->m->cpu);
                    ppc_jit_cpu_reset(q->m->jit);
                }
            } else if (!state) q->in_reset = 0;
        }
    } else if (dev == &q->scsi) {
        /* Active-low IRQ on CB2; live DREQ in the CA2 compatibility bit. */
        if (line == MC_NCR53C96_LINE_IRQ) mc_via_set_cb2(&q->via2, !state);
        else if (line == MC_NCR53C96_LINE_DRQ) mc_via_set_ifr(&q->via2, MC_VIA_IRQ_CA2, state);
    } else if (dev == &q->scc) {
        if (line == MC_SCC_LINE_IRQ) set_irq(q, 4, state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&q->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &q->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->valkyrie) {
        if (line == MC_VALKYRIE_LINE_VBL) mc_f108_ide_set(&q->ide, MC_F108_INT_VIDEO, state);
    } else if (dev == &q->ata) {
        if (line == MC_ATA_LINE_INTRQ) mc_f108_ide_set(&q->ide, MC_F108_INT_IDE, state);
    } else if (dev == &q->ide) {
        if (line == MC_F108_LINE_SLOTIRQ) {
            q->ide_slotirq = state != 0;
            update_slotirq(q);
        }
    }
}

/* --- storage ---------------------------------------------------------------- */

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned count, char *error, size_t capacity) {
    size_t size = 0, ata_size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const uint8_t *ata = mc_guest_livadixdisk_ata(&ata_size);
    const mc_vdisk_drivers drivers = {driver, size, ata, ata_size};
    return mc_vdisk_create(images, count, &drivers, error, capacity);
}

static int disk_on_ata(const capella_machine *q, unsigned i) {
    return i < q->m->config.disk_count && q->m->config.disks[i].bus == MC_BUS_ATA;
}

static void drop_disks(capella_machine *q) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
        q->vdisk[id] = NULL;
    }
    if (q->ata_vdisk) {
        mc_ata_attach_disk(&q->ata, 0, NULL);
        mc_vdisk_destroy(q->ata_vdisk);
        q->ata_vdisk = NULL;
    }
}

/* The internal ATA Manager drives the master only. Additional ATA images
 * become its partitions, using Livadix's existing ATA virtual-disk driver. */
static int layout_ata(capella_machine *q, char *error, size_t capacity) {
    mc_disk *group[MC_CONFIG_DISKS];
    unsigned count = 0;
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (disk && disk_on_ata(q, i)) group[count++] = disk;
    }
    if (!count) return 1;
    char problem[256];
    q->ata_vdisk = make_vdisk(group, count, problem, sizeof problem);
    if (!q->ata_vdisk) {
        if (error && capacity) snprintf(error, capacity, "ATA disk: %s", problem);
        return 0;
    }
    mc_ata_attach_disk(&q->ata, 0, q->ata_vdisk);
    return 1;
}

static int layout_disks(capella_machine *q, char *error, size_t capacity) {
    const mc_config *c = &q->m->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(q);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || i >= c->disk_count || !c->disks[i].scsi_id_set || disk_on_ata(q, i)) continue;
        const int id = c->disks[i].scsi_id;
        if (id < 0 || id >= 7 || id == CD_ID) {
            if (error && capacity) snprintf(error, capacity, "SCSI disk ID %d is unavailable (CD-ROM uses ID 3)", id);
            return 0;
        }
        group[id][count[id]++] = disk;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || (i < c->disk_count && c->disks[i].scsi_id_set) || disk_on_ata(q, i)) continue;
        int id = -1;
        for (size_t k = 0; k < sizeof disk_ids / sizeof *disk_ids && id < 0; k++)
            if (!count[disk_ids[k]]) id = disk_ids[k];
        if (id < 0) id = last;
        if (id < 0) continue;
        group[id][count[id]++] = disk;
        last = id;
    }
    for (int id = 0; id < 7; id++) {
        if (!count[id]) continue;
        char problem[256];
        q->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!q->vdisk[id]) {
            if (error && capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(q);
            return 0;
        }
        mc_scsi_disk_init(&q->unit[id], q->vdisk[id]);
        mc_scsi_bus_attach(&q->scsi_bus, id, mc_scsi_unit_target(&q->unit[id]));
    }
    if (!layout_ata(q, error, capacity)) {
        drop_disks(q);
        return 0;
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    capella_machine *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->m->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    capella_machine *q = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(q, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&q->swim, drive, disk, 0, error, capacity);
    if (kind != MC_MEDIA_CDROM || drive != 0) {
        if (error && capacity) snprintf(error, capacity, "Unsupported Capella media drive");
        return 0;
    }
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (error && capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&q->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&q->unit[CD_ID], vd);
    q->vdisk[CD_ID] = vd;
    return 1;
}

static void board_poll(void *board) {
    capella_machine *q = board;
    mc_media *media = &q->m->media;
    for (unsigned i = 0; i < q->swim.drives; i++) {
        int ejected = 0, gone = 0;
        mc_disk *chip = mc_swim_media(&q->swim, i, &ejected);
        mc_disk *disk = mc_media_get(media, MC_MEDIA_FLOPPY, i, &gone);
        if (!chip) continue;
        if (ejected || chip != disk || gone) {
            mc_swim_flush(&q->swim);
            mc_swim_remove(&q->swim, i);
            if (ejected && chip == disk && !gone) mc_media_eject(media, MC_MEDIA_FLOPPY, i);
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

/* --- host interfaces -------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    capella_machine *q = board;
    return q->m->config.sound ? mc_asc_rate(&q->asc) : 0;
}
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    capella_machine *q = board;
    const size_t count = mc_asc_pull(&q->asc, stereo, frames);
    if (!q->m->config.startup_sound && !q->chime_done) {
        memset(stereo, 0, count * 2u * sizeof *stereo);
        if (q->asc.played && !mc_asc_fifo_level(&q->asc, 0) && !mc_asc_fifo_level(&q->asc, 1) && !q->asc.ring_count)
            q->chime_done = 1;
    }
    return count;
}

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }
static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    capella_machine *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    capella_machine *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    capella_machine *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    capella_machine *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    capella_machine *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    capella_machine *q = board;
    mc_valkyrie_view view;
    if (!mc_valkyrie_view_get(&q->valkyrie, &view)) return 0;
    fb->width = view.width;
    fb->height = view.height;
    fb->depth = view.depth;
    fb->row_bytes = view.row_bytes;
    fb->pixels = view.pixels;
    fb->palette = view.palette;
    fb->serial = ++q->fb_serial;
    return 1;
}
static int board_key(void *board, uint8_t code, int down) {
    capella_machine *q = board;
    if ((code & 0x7Fu) == 0x7Fu) mc_adbctl_power_key(&q->cuda, down);
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adbctl_poke(&q->cuda);
    return 1;
}
static int board_mouse(void *board, int dx, int dy, int button) {
    capella_machine *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adbctl_poke(&q->cuda);
    return 1;
}
static void board_set_clock(void *board, int64_t host_seconds) {
    capella_machine *q = board;
    q->host_start = host_seconds;
    mc_adbctl_set_clock(&q->cuda, host_seconds);
}

static void board_reset(void *board) {
    capella_machine *q = board;
    q->resetting = 1;
    q->irq_mask = 0;
    mc_f108_init(&q->f108, &q->m->host, "F108", q->banks, map_ram, q);
    map_ram(q);
    mc_primetime2_ops.reset(&q->pt2);
    mc_f108_ide_init(&q->ide, &q->m->host, "F108 IDE", &q->ata);
    q->ide_slotirq = 0;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_ncr53c96_reset(&q->scsi);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_swim_ops.reset(&q->swim);
    mc_ata_reset(&q->ata);
    mc_valkyrie_ops.reset(&q->valkyrie);
    mc_enet_card_reset(q->enet);
    mc_via_set_pa(&q->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via1, 0xFFu, 0xF7u);
    mc_via_set_pa(&q->via2, 0xFFu, (uint8_t)~q->nubus.mask);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_ca2(&q->via2, 0);
    mc_via_set_cb1(&q->via2, 1);
    mc_via_set_cb2(&q->via2, 1);
    update_slotirq(q);
    q->chime_done = 0;
    if (mc_capella_ops.reset) mc_capella_ops.reset(&q->bridge);
    q->resetting = 0;
    /* Cuda stays running through CPU reset; its NMI pin still applies. */
    set_irq(q, 7, q->cuda.line_state[MC_ADBCTL_LINE_NMI]);
}

static void free_board(capella_machine *q) {
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    if (q->ata_vdisk) {
        mc_ata_attach_disk(&q->ata, 0, NULL);
        mc_vdisk_destroy(q->ata_vdisk);
    }
    mc_scsi_bus_free(&q->scsi_bus);
    mc_enet_card_free(q->enet);
    mc_valkyrie_free(&q->valkyrie);
    free(q->cuda.mcu);
    free(q);
}
static void board_destroy(void *board) {
    capella_machine *q = board;
    if (q->nvram[0]) mc_adbctl_nvram_save(&q->cuda, q->nvram, q->host_start);
    mc_swim_flush(&q->swim);
    free_board(q);
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((capella_machine *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((capella_machine *)board)->enet, frame, length);
}

static const mc_board_ops capella_ops = {
    .reset = board_reset, .line = board_line, .framebuffer = board_framebuffer,
    .key = board_key, .mouse = board_mouse, .set_clock = board_set_clock,
    .destroy = board_destroy, .audio_rate = board_audio_rate, .audio_pull = board_audio_pull,
    .insert = board_insert, .poll = board_poll,
    .serial_host = board_serial_host, .serial_receive = board_serial_receive, .serial_modem = board_serial_modem,
    .net_host = board_net_host, .net_receive = board_net_receive,
};

/* --- PRAM ------------------------------------------------------------------- */

static void fresh_pram(uint8_t *p) {
    /* The existing PDM/Cuda board defaults; a new video record is left for
     * the ROM's own PrimaryInit, not synthesized by the host. */
    static const uint8_t original[16] = {
        0xA8, 0x00, 0x00, 0x00, 0xCC, 0x0A, 0xCC, 0x0A,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00,
    };
    static const uint8_t original_low[8] = {0x13, 0x88, 0x00, 0x4C, 'N', 'u', 'M', 'c'};
    static const uint8_t startup[6] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF};
    memset(p, 0, 256);
    memcpy(p + 0x10, original, sizeof original);
    memcpy(p + 0x08, original_low, sizeof original_low);
    memcpy(p + 0x76, startup, sizeof startup);
    p[0x8A] = 0x05;
}

/* ROM 63ABFD3F's Valkyrie driver, $2FEF60-$2FEF7E, adds two to
 * the mode index before writing the depth register: mode $80 is 4-bit,
 * $81 is 8-bit, $82 is 16-bit. The note, chapter 2 "Valkyrie IC" and
 * chapter 3 Table 3-10, offers only 4/8/16 bits (4/8 on large monitors).
 * Change a record only after the ROM wrote the matching board/monitor. */
static void set_video_pram(capella_machine *q) {
    static const struct { uint8_t monitor, code, rsrc; } resources[] = {
        {MC_VALKYRIE_RGB12, 0x02, 0x80}, {MC_VALKYRIE_RGB14, 0x06, 0x81},
        {MC_VALKYRIE_VGA, 0x08, 0x82}, {MC_VALKYRIE_RGB16, 0x0A, 0x83},
    };
    const mc_config *c = &q->m->config;
    uint8_t *p = mc_adbctl_pram(&q->cuda);
    for (size_t i = 0; i < sizeof resources / sizeof *resources; i++) {
        if (resources[i].monitor != q->valkyrie.monitor) continue;
        if (p[0x10] != 0xA8 || p[0x46] != 0 || p[0x47] != 0x65 ||
            p[0x4A] != resources[i].rsrc || p[0x4B] != resources[i].code) return;
        p[0x49] = q->valkyrie.monitor == MC_VALKYRIE_VGA && c->width == 800 ? 0x84 : resources[i].rsrc;
        unsigned mode = 0x80;
        for (uint32_t depth = c->depth; depth > 4; depth >>= 1) mode++;
        p[0x48] = (uint8_t)mode;
        return;
    }
}

/* --- construction and descriptions ----------------------------------------- */

static void *build(mc_ppc *m, const capella_board *board) {
    capella_machine *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        return NULL;
    }
    q->m = m;
    q->board = board;
    m->board = q;
    if ((m->ram_size % MB) || !ram_banks(m->ram_size / MB, q->banks)) {
        snprintf(m->error, sizeof m->error, "emulation.ram_mb: %u MiB does not fit Capella's two SIMM slots",
                 (unsigned)(m->ram_size / MB));
        goto fail;
    }
    if (m->config.video_card == MC_VIDEO_CARD_LIVADIX) {
        snprintf(m->error, sizeof m->error, "Capella supports built-in Valkyrie video only");
        goto fail;
    }
    if (board->panel) {
        m->config.width = 640;
        m->config.height = 480;
    }
    const mc_valkyrie_monitor monitor = mc_valkyrie_monitor_for(m->config.width, m->config.height);
    const uint32_t max_depth = mc_valkyrie_max_depth(monitor);
    if (!max_depth) {
        snprintf(m->error, sizeof m->error, "Valkyrie cannot show %ux%u", (unsigned)m->config.width, (unsigned)m->config.height);
        goto fail;
    }
    if (m->config.depth > max_depth) m->config.depth = max_depth;

    const mc_hw_host *host = &m->host;
    const int io = mc_ppc_region(m, "io");
    const int bridge = mc_ppc_region(m, "capella");
    const int cpuid = mc_ppc_region(m, "cpuid");
    if (!io || !bridge || !cpuid) goto map_fail;
    m->bus.region[io].cost = IO_COST;
    /* Overlay only the VIA pages of each I/O image with their longer cost;
     * $53000000 is a separate region, not another image of the legacy I/O. */
    const int vias = mc_bus_add_io(&m->bus, IO_BASE, IO_SIZE, IO_IMAGE - 1u, 0);
    if (!vias) goto map_fail;
    m->bus.region[vias].cost = VIA_COST;
    for (uint32_t image = IO_BASE; image < IO_BASE + IO_SIZE; image += IO_IMAGE)
        mc_bus_remap(&m->bus, io, image + 0x4000u, IO_IMAGE - 0x4000u);

    const mc_ppc_port byte = {MC_PPC_PORT_SIZED, 1, 0, 0};
    const mc_ppc_port word = {MC_PPC_PORT_SIZED, 2, 0, 0};
    const mc_ppc_port direct = {MC_PPC_PORT_DIRECT, 0, 0, 0};
    mc_capella_init(&q->bridge, host, "Capella");
    q->io_region = io;
    q->via_region = vias;
    q->cache_region = mc_bus_map_memory(&m->bus, 0x51000000u, sizeof q->cache, q->cache, sizeof q->cache, 1);
    q->tag_region = mc_bus_add_io(&m->bus, 0x52000000u, sizeof q->tags, sizeof q->tags - 1u, 0);
    if (!q->cache_region || !q->tag_region ||
        !mc_bus_attach(&m->bus, q->tag_region, 0, sizeof q->tags, q, &tag_ops, 0)) goto map_fail;
    q->bridge.map_cache = map_cache;
    q->bridge.map_ctx = q;
    map_cache(q, 0);
    mc_f108_init(&q->f108, host, "F108", q->banks, map_ram, q);
    map_ram(q);
    mc_primetime2_init(&q->pt2, host, "PrimeTime II");
    mc_via_init(&q->via1, host, "VIA1", 9);
    mc_via_init(&q->via2, host, "VIA2", 9);
    mc_absent_init(&q->comm, host, "Communication slot", NULL, 0);
    mc_scc_init(&q->scc, host);
    mc_scc_set_clocks(&q->scc, 8000000u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port serial = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &serial);
    mc_scsi_bus_init(&q->scsi_bus);
    if (!q->scsi_bus.buffer) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        goto fail;
    }
    /* Chapter 2 gives 18.75 MHz on 5200/6200; retain bus/2 for the later
     * 33 MHz boards †, matching the 630's 16.5 MHz F108 clock. */
    mc_ncr53c96_init(&q->scsi, host, &q->scsi_bus, board->bus_hz / 2u);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    mc_asc_init(&q->asc, host, MC_ASC_ENHANCED);
    mc_swim2_init(&q->swim, host, m->media.floppy_drives);
    mc_ata_init(&q->ata, host, 2);
    mc_f108_ide_init(&q->ide, host, "F108 IDE", &q->ata);
    if (!mc_valkyrie_init(&q->valkyrie, host, "Valkyrie", monitor)) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        goto fail;
    }

    if (!mc_ppc_attach(m, bridge, 0, MC_CAPELLA_SIZE, &q->bridge, &mc_capella_ops, direct) ||
        !mc_ppc_attach(m, vias, 0x00000, 0x2000, q, &via1_ops, byte) ||
        !mc_ppc_attach(m, vias, 0x02000, 0x2000, q, &via2_ops, byte) ||
        !mc_ppc_attach(m, io, 0x08000, 0x2000, &q->comm, &mc_absent_ops, direct) ||
        /* The comm slot card's own window (its SONIC at +$2000): without a
         * card the cycle ends in a bus error, which the ROM's existence test
         * ($4081BE1A) takes as no comm slot Ethernet; answering it lists a
         * phantom Ethernet in pseudo-slot $D ahead of a PDS card. */
        !mc_bus_attach(&m->bus, io, 0x0A000, 0x2000, &q->comm, &mc_absent_ops, MC_WIN_BUSERR) ||
        !mc_ppc_attach(m, io, 0x0C000, 0x2000, &q->scc, &mc_scc_ops, byte) ||
        !mc_ppc_attach(m, io, 0x0E000, 0x2000, &q->f108, &mc_f108_ops, direct) ||
        !mc_ppc_attach(m, io, 0x10000, 0x0100, &q->scsi, &scsi_ops, byte) ||
        !mc_ppc_attach(m, io, 0x10100, 0x1F00, &q->scsi, &pdma_ops, direct) ||
        !mc_ppc_attach(m, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, byte) ||
        !mc_ppc_attach(m, io, 0x18000, MC_PRIMETIME2_SIZE, &q->pt2, &mc_primetime2_ops, word) ||
        !mc_ppc_attach(m, io, 0x1A000, 0x2000, &q->ide, &mc_f108_ide_ops, direct) ||
        !mc_ppc_attach(m, io, 0x1E000, 0x2000, &q->swim, &mc_swim_ops, byte) ||
        !mc_ppc_attach(m, io, 0x24000, 0x2000, &q->valkyrie, &mc_valkyrie_clut_ops, byte) ||
        !mc_ppc_attach(m, io, 0x2A000, 0x2000, &q->valkyrie, &mc_valkyrie_ops, byte)) goto map_fail;
    /* A four-byte, not eight-byte, decode: mc_ppc_attach requires an
     * eight-aligned offset, so this DIRECT window uses the shared bus.
     * $5FFFFFF8 and all other CPU-ID-page offsets remain unclaimed. */
    if (!mc_bus_attach(&m->bus, cpuid, 0xFFCu, 4, q, &cpu_id_ops, 0) ||
        !mc_bus_map_memory(&m->bus, 0xF9000000u, 0x00800000u, q->valkyrie.vram, MC_VALKYRIE_VRAM, 1)) goto map_fail;

    /* The LC processor-direct slot as NuBus slot $E, its interrupt on VIA2
     * PA5, and the Ethernet card network.card asks for there. */
    q->nubus_slotirq_high = 1;
    mc_nubus_init(&q->nubus, &m->bus, (uint8_t)(1u << (0xE - 8)), slot_pins, q);
    mc_nubus_line(&q->nubus, 0xE, 5);
    {
        const mc_enet_card_env env = {&m->config, m->model ? m->model->name : m->desc->name, host, &m->bus,
                                      m->error, sizeof m->error};
        q->enet = mc_enet_card_fit_on(&env, &q->nubus, MC_ENET_CARD_LC, 0xEu);
        if (!q->enet && m->error[0]) goto fail;
    }

    mc_adb_bus_init(&q->adb);
    mc_adb_keyboard_init(&q->keyboard);
    mc_adb_mouse_init(&q->mouse);
    mc_adb_attach(&q->adb, &q->keyboard.dev);
    mc_adb_attach(&q->adb, &q->mouse.dev);
    mc_adbctl_init(&q->cuda, host, "Cuda", MC_ADBCTL_CUDA, &q->via1, &q->adb);
    mc_adbctl_set_part(&q->cuda, "341S0060");
    mc_adbctl_ops.reset(&q->cuda);
    q->powered = 0;
    static const char *const firmware[] = {"cuda-341s0060", NULL};
    const int level = mc_adbctl_select(&q->cuda, (mc_adbctl_level)m->config.controller, m->config.roms, firmware);
    if (level < 0) {
        snprintf(m->error, sizeof m->error, "emulation.controller: firmware: Cuda firmware (cuda-341s0060) is not in the roms folders");
        goto fail;
    }
    q->powered = level == 0;
    if (!mc_config_nvram_path(&m->config, q->nvram, sizeof q->nvram)) q->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&q->cuda, q->nvram)) {
        fresh_pram(mc_adbctl_pram(&q->cuda));
        mc_adbctl_set_seconds(&q->cuda, 0);
    }
    set_video_pram(q);
    if (m->config.boot_priority == MC_BOOT_CDROM) {
        uint8_t *pram = mc_adbctl_pram(&q->cuda);
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    return q;

map_fail:
    snprintf(m->error, sizeof m->error, "Could not attach Capella board devices");
fail:
    m->board = NULL;
    free_board(q);
    return NULL;
}

static void *build_5200(mc_ppc *m) { return build(m, &board_5200); }
static void *build_5300(mc_ppc *m) { return build(m, &board_5300); }
static void *build_6200(mc_ppc *m) { return build(m, &board_6200); }
static void *build_6300(mc_ppc *m) { return build(m, &board_6300); }

static const char *const models_5200[] = {"power-macintosh-5200", NULL};
static const char *const models_5300[] = {"power-macintosh-5300", NULL};
static const char *const models_6200[] = {"power-macintosh-6200", NULL};
static const char *const models_6300[] = {"power-macintosh-6300", NULL};

static unsigned video_panel(const mc_model *model, unsigned width, unsigned height) {
    (void)model;
    return width == 640 && height == 480 ? MC_DEPTHS_UPTO(16) & ~MC_DEPTHS_UPTO(2) : 0;
}
static unsigned video_external(const mc_model *model, unsigned width, unsigned height) {
    (void)model;
    return MC_DEPTHS_UPTO(mc_valkyrie_max_depth(mc_valkyrie_monitor_for(width, height))) & ~MC_DEPTHS_UPTO(2);
}
static const mc_hw_caps panel_caps = {.builtin = video_panel, .features = MC_FEATURE_ATA_DISK | MC_FEATURE_LC_SLOT};
static const mc_hw_caps external_caps = {.builtin = video_external, .features = MC_FEATURE_ATA_DISK | MC_FEATURE_LC_SLOT};

/* Processor models/speeds follow the catalogue and the ROM's ProductInfo.
 * Flat CPI 8/9 is the conservative PDM measurement model, not a measured
 * 603/603e pipeline model †; timebase clocks remain the real bus/4. */
#define CAPELLA_DESC(id, cpu_model, hz, tb_hz, capabilities) \
    const mc_ppc_desc mc_capella##id##_desc = { \
        .key = "capella" #id, .models = models_##id, .name = "Power Macintosh " #id, \
        .cpu = cpu_model, .cpu_hz = hz, .timebase_hz = tb_hz, .cpi = 8, .cpi_div = 9, \
        .roms = capella_roms, .ram_mb = capella_ram, .map = capella_map, \
        .nmap = sizeof capella_map / sizeof *capella_map, .unmapped = MC_PPC_UNMAPPED_TEA, \
        .build = build_##id, .ops = &capella_ops, .caps = capabilities, \
    }

CAPELLA_DESC(5200, PPC_MODEL_603, 75000000u, 9375000u, &panel_caps);
CAPELLA_DESC(5300, PPC_MODEL_603E, 100000000u, 8250000u, &panel_caps);
CAPELLA_DESC(6200, PPC_MODEL_603, 75000000u, 9375000u, &external_caps);
CAPELLA_DESC(6300, PPC_MODEL_603E, 100000000u, 8250000u, &external_caps);
