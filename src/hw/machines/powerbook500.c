/* Livadix — the 68LC040 PowerBooks of ROMs B6909089 and 4D27039C: the
 * PowerBook 520, 520c, 540 and 540c (box flag 66, decoder kind 16) and the
 * PowerBook 190 and 190cs (box flag 79, decoder kind 21).
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (the
 * VIAs' pins as on the Macintosh II family: VIA1 PA5 vHeadSel, PA7
 * vSCCWrReq), chapter 2 "Power Manager microprocessor" (pmu.h); Designing
 * Cards and Drivers, 3rd ed. (the declaration ROM). Everything else is
 * observed from the ROMs (pratt.h):
 *   - the ProductInfo records (ROM B6909089 offset $A7A5C, box flag 66;
 *     ROM 4D27039C offsets $3EB78 and $3EBE0, box flags 66 and 79): VIA1
 *     PA6/PA4/PA2/PA1 against mask $56 and ID $12;
 *   - their decoder records (ROM B6909089 offset $A8578; ROM 4D27039C
 *     $3EE18 and $3EF00): ROM $4000 0000, VIA1 $50F0 0000, VIA2 $50F0 2000
 *     (the 190's also as its VIA2-functions entry), SCC $50F0 4000, the
 *     SCSI controller $50F1 0000 and its pseudo-DMA window $50F0 6000
 *     (a 5380: the ROM clears registers 1-4 at stride $10, ROM B6909089
 *     offset $98678, and arbitrates by writing 1 to the mode register and
 *     waiting for bit 6 of the initiator command register, $76D2), sound
 *     $50F1 4000, SWIM II $50F1 6000, the panel's controller $50F2 0000,
 *     Pratt's registers $50F0 A000, $5014 0000 and $5008 0000†;
 *   - the PMU's wiring (the ROMs' byte send, ROM B6909089 offset $478B0,
 *     ROM 4D27039C $10F538): /REQ on VIA2 PB2, /ACK on VIA2 PB1, /INT on
 *     VIA1 CB1; the bytes through VIA1's shift register, as a flag bit
 *     before the decoder record asks (bit 1 of the byte 15 before it on
 *     ROM B6909089, $A8569 = $03; 27 before it on ROM 4D27039C, $3EEE5),
 *     not through VIA2 port A.
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../adb.h"
#include "../asc.h"
#include "../ata.h"
#include "../ncr5380.h"
#include "../pmu.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../asic/pratt.h"
#include "../video/csc.h"
#include "../video/gsc.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"
#include "../../core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define VRAM_BASE 0x60000000u
#define VRAM_SIZE 0x100000u

/* Offsets in the I/O region ($5000 0000; the decoder records above). */
enum {
    OFS_VIA1 = 0xF00000, OFS_VIA2 = 0xF02000, OFS_SCC = 0xF04000, OFS_SCSI_DMA = 0xF06000, OFS_PRATT = 0xF0A000,
    OFS_SCSI = 0xF10000, OFS_SOUND = 0xF14000, OFS_SWIM = 0xF16000, OFS_ATA = 0xF1A000, OFS_VIDEO = 0xF20000,
    OFS_PRATT_B = 0x140000, OFS_PRATT_C = 0x080000, OFS_PRATT_D = 0xF96000
};

enum { KIND_520, KIND_190 };

typedef struct board_info {
    const char *model;
    uint8_t kind;
    uint8_t color;              /* 1: the color panel on the CSC; 0: the gray panel on the GSC */
} board_info;

static const board_info boards[] = {
    {"powerbook-520", KIND_520, 0}, {"powerbook-520c", KIND_520, 1}, {"powerbook-540", KIND_520, 0},
    {"powerbook-540c", KIND_520, 1}, {"powerbook-190", KIND_190, 0}, {"powerbook-190cs", KIND_190, 1},
};

static const board_info *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct pb5 {
    mc_hw *hw;
    const board_info *board;
    mc_via via1, via2;
    mc_pratt pratt;
    mc_gsc gsc;
    mc_csc csc;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];
    mc_vdisk *vdisk[7];
    mc_ata ata;                 /* the PowerBook 190's internal IDE bus */
    mc_vdisk *ata_vdisk;
    mc_ata bay;                 /* the 190's second ATA bus: the expansion bay's ATA module */
    mc_vdisk *bay_cd;           /* the disc in the expansion bay's ATAPI CD-ROM drive */
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_pmu pmu;
    int powered;
    uint64_t fb_serial;
    int overlay;
    int overlay_region;
    uint8_t *vram;
    uint8_t ext[256];           /* the external video's registers (slot $E) */
    uint8_t via2x[0x200];
    uint16_t ide_ctl;           /* the 190's IDE control word (+$D0) */
    uint8_t ata_irq, bay_irq;   /* the internal bus's and the bay bus's INTRQ */       /* the VIA2 functions between VIA2's registers */
    char nvram[1024];
    int64_t host_start;
} pb5;

static int has_ata(const pb5 *m) { return m->board->kind == KIND_190; }
/* storage.cdrom bus: ata on the 190: the CD-ROM module in the expansion
 * bay (instead of the floppy module), the master of the bay's ATA bus. */
static int bay_cd(const pb5 *m) { return has_ata(m) && m->hw->config.cdrom_bus == MC_BUS_ATA; }
static int has_gsc(const pb5 *m) { return m->board->kind == KIND_190 && !m->board->color; }

static const mc_region pb5_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x01000000u, 0x00FFFFFFu, MC_REGION_IO},
};

static const mc_irq_route pb5_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- RAM and the reset overlay --------------------------------------------------- */

static void map_ram(pb5 *m) {
    mc_hw *hw = m->hw;
    mc_bus_map_float(&hw->bus, 0, ROM_BASE);
    mc_bus_map_memory(&hw->bus, 0, hw->ram_size, hw->ram, hw->ram_size, 1);
    mc_hw_maps_changed(hw);
}

static void overlay_on(pb5 *m) {
    mc_hw *hw = m->hw;
    m->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, m->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(pb5 *m) {
    mc_hw *hw = m->hw;
    m->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    pb5 *m = dev;
    if (m->overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    pb5 *m = dev;
    if (m->overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

static int pmu_shifts(const pb5 *m) { (void)m; return 1; }
static uint8_t pmu_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((pb5 *)ctx)->via1, in); }

static void pmu_hold(pb5 *m) {
    const int held = m->pmu.reset || m->pmu.power != MC_PMU_POWER_ON;
    if (held && !m->overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* --- wiring ------------------------------------------------------------------------ */

static void board_line(void *board, void *dev, int line, int state) {
    pb5 *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
    } else if (dev == &m->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_VIA_LINE_PA) mc_pmu_set_port(&m->pmu, (uint8_t)state);
    } else if (dev == &m->pratt) {
        if (line == MC_PRATT_LINE_TICK) mc_via_set_ca1(&m->via1, state);
    } else if (dev == &m->pmu) {
        if (line == MC_PMU_LINE_ACK) mc_via_set_pb(&m->via2, state ? 0x02u : 0, 0x02u);
        else if (line == MC_PMU_LINE_INT) {
            /* CB1 is also the shift register's clock: the interrupt's
             * edges set the CB1 flag without clocking a bit (as on the
             * Duos, powerbook.c)†. */
            const uint8_t sr = m->via1.sr, count = m->via1.sr_count;
            mc_via_set_cb1(&m->via1, state);
            if (pmu_shifts(m)) {
                m->via1.sr = sr;
                m->via1.sr_count = count;
            }
        }
        else if (line == MC_PMU_LINE_DATA) mc_via_set_pa(&m->via2, state < 0 ? 0xFFu : (uint8_t)state, 0xFFu);
        else if (line == MC_PMU_LINE_POWER || line == MC_PMU_LINE_RESET) {
            if (line == MC_PMU_LINE_POWER) {
                if (state == MC_PMU_POWER_ON) m->powered = 1;
                else if (state == MC_PMU_POWER_OFF && m->powered) mc_hw_power_off(hw);
            }
            pmu_hold(m);
        }
    } else if (dev == &m->scsi) {
        if (line == MC_NCR5380_LINE_IRQ) mc_via_set_cb2(&m->via2, !state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_via_set_ca2(&m->via2, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&m->via2, !state);
    } else if (dev == &m->ata || dev == &m->bay) {
        if (line == MC_ATA_LINE_INTRQ) {
            if (dev == &m->ata) m->ata_irq = state != 0;
            else m->bay_irq = state != 0;
            const int any = m->ata_irq || m->bay_irq;
            mc_via_set_pa(&m->via2, any ? 0 : 0x08u, 0x08u);
            mc_via_set_ca1(&m->via2, !any);
        }
    }
}

/* --- storage -------------------------------------------------------------------- */

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0, ata_size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const uint8_t *ata = mc_guest_livadixdisk_ata(&ata_size);
    const mc_vdisk_drivers drivers = {driver, size, ata, ata_size};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static int disk_on_ata(const pb5 *m, unsigned i) {
    return has_ata(m) && i < m->hw->config.disk_count && m->hw->config.disks[i].bus == MC_BUS_ATA;
}

static void drop_disks(pb5 *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
    if (m->ata_vdisk) {
        mc_ata_attach_disk(&m->ata, 0, NULL);
        mc_vdisk_destroy(m->ata_vdisk);
        m->ata_vdisk = NULL;
    }
}

/* The `bus: ata` disks of the PowerBook 190: its internal hard disk,
 * device 0, the others further partitions of it. */
static int layout_ata(pb5 *m, char *error, size_t capacity) {
    mc_disk *group[MC_CONFIG_DISKS];
    unsigned count = 0;
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL);
        if (d && disk_on_ata(m, i)) group[count++] = d;
    }
    if (!count) return 1;
    char problem[256];
    m->ata_vdisk = make_vdisk(group, count, problem, sizeof problem);
    if (!m->ata_vdisk) {
        if (capacity) snprintf(error, capacity, "ATA disk: %s", problem);
        return 0;
    }
    mc_ata_attach_disk(&m->ata, 0, m->ata_vdisk);
    return 1;
}

static int layout_disks(pb5 *m, char *error, size_t capacity) {
    const mc_config *c = &m->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(m);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set || disk_on_ata(m, i)) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || (i < c->disk_count && c->disks[i].scsi_id_set) || disk_on_ata(m, i)) continue;
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
        m->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!m->vdisk[id]) {
            if (capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(m);
            return 0;
        }
        mc_scsi_disk_init(&m->unit[id], m->vdisk[id]);
        mc_scsi_bus_attach(&m->scsi_bus, id, mc_scsi_unit_target(&m->unit[id]));
    }
    if (!layout_ata(m, error, capacity)) {
        drop_disks(m);
        return 0;
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    pb5 *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static void bay_cd_ejected(void *ctx, unsigned dev, mc_vdisk *media) {
    pb5 *m = ctx;
    (void)dev;
    mc_vdisk_destroy(media);
    m->bay_cd = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    pb5 *m = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(m, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&m->swim, drive, disk, 0, error, capacity);
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    if (bay_cd(m)) {
        mc_vdisk *old = mc_ata_cd_remove(&m->bay, 0);
        if (old) mc_vdisk_destroy(old);
        mc_ata_cd_insert(&m->bay, 0, vd);
        m->bay_cd = vd;
        return 1;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&m->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&m->unit[CD_ID], vd);
    m->vdisk[CD_ID] = vd;
    return 1;
}

static void board_poll(void *board) {
    pb5 *m = board;
    mc_media *media = &m->hw->media;
    for (unsigned i = 0; i < m->swim.drives; i++) {
        int ejected = 0, gone = 0;
        mc_disk *chip = mc_swim_media(&m->swim, i, &ejected);
        mc_disk *d = mc_media_get(media, MC_MEDIA_FLOPPY, i, &gone);
        if (!chip) continue;
        if (ejected || chip != d || gone) {
            mc_swim_flush(&m->swim);
            mc_swim_remove(&m->swim, i);
            if (ejected && chip == d && !gone) mc_media_eject(media, MC_MEDIA_FLOPPY, i);
        }
    }
    int gone = 0;
    mc_disk *cd = mc_media_get(media, MC_MEDIA_CDROM, 0, &gone);
    if (m->vdisk[CD_ID] && (!cd || gone)) {
        mc_vdisk *old = mc_scsi_unit_remove(&m->unit[CD_ID]);
        if (old) mc_vdisk_destroy(old);
        m->vdisk[CD_ID] = NULL;
    }
    if (m->bay_cd && (!cd || gone)) {
        mc_vdisk *old = mc_ata_cd_remove(&m->bay, 0);
        if (old) mc_vdisk_destroy(old);
        m->bay_cd = NULL;
    }
}

/* --- sound --------------------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    pb5 *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    pb5 *m = board;
    const size_t n = mc_asc_pull(&m->asc, stereo, frames);
    if (!m->hw->config.startup_sound && !m->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (m->asc.played && !mc_asc_fifo_level(&m->asc, 0) && !mc_asc_fifo_level(&m->asc, 1) && !m->asc.ring_count) m->chime_done = 1;
    }
    return n;
}

/* --- serial ports -------------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    pb5 *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    pb5 *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    pb5 *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    pb5 *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    pb5 *m = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&m->scc, port_channel(port), dcd);
    mc_scc_set_cts(&m->scc, port_channel(port), cts);
}

/* --- SCSI: the 5380's registers every 16 bytes, its pseudo-DMA window -------------- */

static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr5380_ops.read(dev, (offset >> 4) & 7u, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr5380_ops.write(dev, (offset >> 4) & 7u, size, value);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    pb5 *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb5 *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* The CPU ID register at $5FFF FFFC: $A55A in the high word and the board
 * in the low word, matched against the ProductInfo records' word at +$54
 * of ROM 4D27039C (box flag 66 $1808 at $3EB78, box flag 79 $180B at
 * $3EBE0; ROM B6909089's record holds $1808 at +$44); writes are ignored.
 * The longword before it, $5FFF FFF8, is read by ROM 4D27039C's CSC driver
 * (in RAM at $78CA; it compares it with $5AA5 0004): it answers 0†. */
static uint32_t cpuid_read(void *dev, uint32_t offset, unsigned size) {
    const pb5 *m = dev;
    if (offset < 4u) return 0;
    offset -= 4u;
    const uint32_t id = 0xA55A0000u | (m->board->kind == KIND_190 ? 0x180Bu : 0x1808u);
    const unsigned shift = 8u * (4u - size - (offset & 3u));
    return size == 4 ? id : (id >> shift) & ((1u << (8u * size)) - 1u);
}
static void cpuid_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}
static const mc_dev_ops cpuid_ops = {cpuid_read, cpuid_write, NULL, NULL};

/* VIA2: the PMU's /REQ follows port B's output register bit 2, as written,
 * whatever the direction register holds (the ROM's start leaves DDRB at $00, ROM
 * B6909089 offset $2ED8, and then sends its PMU bytes by writing ORB,
 * $478E0)†. The VIA's registers are every $200 bytes, the low address
 * bits ignored (ROM 4D27039C enables VIA2's CA1 and CB1 interrupts at
 * $50F0 3C13, $3FA08-$3FA16), but for the first $200 bytes: the bytes
 * above port B are the VIA2 functions of the 190's decoder record (its
 * RBV entry, the same $50F0 2000), where the ROM's start writes $7F, $40
 * and $FF to +$13, +$10 and +$12 ($1A82A-$1A888) without touching the
 * PMU's /REQ. They are kept as written†. */
static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    pb5 *m = dev;
    if (offset < 0x200u && offset) return m->via2x[offset];
    return mc_via_ops.read(&m->via2, offset & ~0x1FFu, size);
}
static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb5 *m = dev;
    if (offset < 0x200u && offset) {
        m->via2x[offset] = (uint8_t)value;
        return;
    }
    offset &= ~0x1FFu;
    mc_via_ops.write(&m->via2, offset, size, value);
    if (((offset >> 9) & 15u) == 0) mc_pmu_set_req(&m->pmu, (m->via2.orb & 0x04u) != 0);
}
static const mc_dev_ops via2_ops = {via2_read, via2_write, NULL, NULL};

/* The external video port in slot $E, as on the PowerBook 160/180
 * (powerbook.c): its registers at $FEE0 0000, the monitor sense lines in
 * bits 7-4 of $FEEF E004 (the driver ROM B6909089 loads into RAM sets
 * $FEE0 0021 bit 0 and clears $FEEF E010 first). No monitor is connected:
 * every sense line reads high. The registers are kept as written†. */
static uint32_t ext_read(void *dev, uint32_t offset, unsigned size) {
    pb5 *m = dev;
    (void)size;
    if (offset == 0xFE004u) return 0x70u | (m->ext[4] & 0x0Fu);
    return m->ext[offset & 0xFFu];
}
static void ext_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb5 *m = dev;
    (void)size;
    m->ext[offset & 0xFFu] = (uint8_t)value;
}
static const mc_dev_ops ext_ops = {ext_read, ext_write, NULL, NULL, NULL};

/* The PowerBook 190's IDE window at $50F1 A000: the ATA task file every 4
 * bytes from +0 (ata.h), and above it two registers the ROM's ATA Manager
 * uses (ROM 4D27039C offsets $C0758-$C08B4, $C0B96-$C0EF6): +$D0 a control
 * word (the ROM writes bits 2, 4 and 6 and polls bits 0 and 1; kept as
 * written†), +$D4 the interrupt status, bit 1 the internal bus's INTRQ
 * and bit 0 the bay bus's (the ATA Manager's CheckATAInt at $ADED0 swaps
 * them into its per-bus result), bit 2 clear while a module sits in the
 * expansion bay (Baboon's socket services, $C01E2 and $C0758; the wait for
 * a module at $C23A6 loops while it is set); +$D8 and the timing words at
 * +$40 and +$C0 read 0†. The expansion bay's ATA bus is a second task file
 * every 4 bytes from +$80 (alternate status and device control at +$B8):
 * after Card Services reports the bay's "MB2" module (the fake CIS at
 * $BF9E0, IsMediaBay at $ACC90), the ATA Manager probes its master there
 * with IDENTIFY DEVICE, then IDENTIFY PACKET DEVICE ($ADC2A-$ADC72).
 * INTRQ of either bus reaches the CPU as slot $C's interrupt (VIA2 PA3 low
 * and CA1: the ATA Manager installs its handler with SIntInstall for slot
 * $C, $AB1D4). */
static uint32_t ide_read(void *dev, uint32_t offset, unsigned size) {
    pb5 *m = dev;
    if (offset < 0x40u) return mc_ata_ops.read(&m->ata, offset, size);
    if (offset >= 0x80u && offset < 0xC0u) return mc_ata_ops.read(&m->bay, offset - 0x80u, size);
    if (offset == 0xD0u && size == 2) return m->ide_ctl;
    if (offset == 0xD4u && size == 2) return (m->ata_irq ? 0x02u : 0u) | (m->bay_irq ? 0x01u : 0u);
    return 0;
}
static void ide_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb5 *m = dev;
    if (offset < 0x40u) {
        mc_ata_ops.write(&m->ata, offset, size, value);
        return;
    }
    if (offset >= 0x80u && offset < 0xC0u) {
        mc_ata_ops.write(&m->bay, offset - 0x80u, size, value);
        return;
    }
    if (offset == 0xD0u && size == 2) m->ide_ctl = (uint16_t)value;
}
static const mc_dev_ops ide_ops = {ide_read, ide_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    pb5 *m = board;
    mc_via_ops.reset(&m->via1);
    mc_via_ops.reset(&m->via2);
    mc_pratt_ops.reset(&m->pratt);
    if (!has_gsc(m)) mc_csc_ops.reset(&m->csc);
    else mc_gsc_ops.reset(&m->gsc);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    /* The ROMs never write the enhanced ASC's FIFO volumes ($F06, $F26;
     * the Quadra 630's ROM sets $7F00 and $007F there) but the sound
     * block's registers above them ($F40-$F4A: ROM B6909089 offset
     * $E0942-$E09AA); the outputs are taken at full volume, FIFO A left
     * and FIFO B right†. */
    mc_asc_ops.write(&m->asc, 0xF06, 2, 0x7F00);
    mc_asc_ops.write(&m->asc, 0xF26, 2, 0x007F);
    mc_swim_ops.reset(&m->swim);
    if (has_ata(m)) {
        mc_ata_reset(&m->ata);
        mc_ata_reset(&m->bay);
    }
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    pb5 *m = board;
    /* The PMU removes panel power in sleep; retain VRAM for wake. */
    if (mc_pmu_asleep(&m->pmu)) return 0;
    if (!has_gsc(m)) {
        mc_csc_view v;
        if (!mc_csc_view_get(&m->csc, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
    } else {
        mc_gsc_view v;
        if (!mc_gsc_view_get(&m->gsc, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
    }
    fb->serial = ++m->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    pb5 *m = board;
    if (mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    return mc_adb_keyboard_key(&m->keyboard, code, down);
}

static int board_mouse(void *board, int dx, int dy, int button) {
    pb5 *m = board;
    if (button && mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    return 1;
}

/* --- persistence: the PMU's clock and parameter RAM (powerbook.c's format) -------- */

static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};
#define NVRAM_RECORD (8 + 1 + 8 + 256)

static void fresh_pram(uint8_t *p) {
    static const uint8_t original[16] = {0xA8, 0x00, 0x00, 0x00, 0xCC, 0x0A, 0xCC, 0x0A,
                                         0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x63, 0x00};
    static const uint8_t original_low[8] = {0x13, 0x88, 0x00, 0x4C, 'N', 'u', 'M', 'c'};
    static const uint8_t startup[6] = {0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xDF};
    memset(p, 0, 256);
    memcpy(p + 0x10, original, sizeof original);
    memcpy(p + 0x08, original_low, sizeof original_low);
    memcpy(p + 0x76, startup, sizeof startup);
    p[0x8A] = 0x05;
}

static void load_nvram(pb5 *m) {
    FILE *f = m->nvram[0] ? fopen(m->nvram, "rb") : NULL;
    uint8_t buf[NVRAM_RECORD];
    const size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    if (n >= NVRAM_RECORD && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1) {
        int64_t offset = 0;
        offset = mc_nvram_clock_offset(buf + 9);
        mc_pmu_pram_put(&m->pmu, buf + 17);
        mc_pmu_set_seconds(&m->pmu, (uint32_t)offset);
    } else {
        uint8_t p[256];
        fresh_pram(p);
        mc_pmu_pram_put(&m->pmu, p);
        mc_pmu_set_seconds(&m->pmu, 0);
    }
}

static void save_nvram(pb5 *m) {
    if (!m->nvram[0]) return;
    uint8_t buf[NVRAM_RECORD];
    memset(buf, 0, sizeof buf);
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t host = m->host_start + (int64_t)(mc_hw_now(m->hw) / MC_C15M_HZ);
    const int64_t offset = (int64_t)mc_pmu_seconds(&m->pmu) - host;
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    mc_pmu_pram_get(&m->pmu, buf + 17);
    FILE *f = fopen(m->nvram, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

static void board_set_clock(void *board, int64_t host_seconds) {
    pb5 *m = board;
    m->host_start = host_seconds;
    mc_pmu_set_seconds(&m->pmu, (uint32_t)((int64_t)mc_pmu_seconds(&m->pmu) + host_seconds));
}

static void free_board(pb5 *m) {
    mc_scsi_bus_free(&m->scsi_bus);
    mc_pmu_free(&m->pmu);
    free(m->vram);
    free(m);
}

static void board_destroy(void *board) {
    pb5 *m = board;
    save_nvram(m);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    if (m->ata_vdisk) {
        mc_ata_attach_disk(&m->ata, 0, NULL);
        mc_vdisk_destroy(m->ata_vdisk);
    }
    if (m->bay_cd) {
        mc_ata_cd_remove(&m->bay, 0);
        mc_vdisk_destroy(m->bay_cd);
    }
    free_board(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    pb5 *m = calloc(1, sizeof *m);
    if (m) m->vram = calloc(1, VRAM_SIZE);
    if (!m || !m->vram) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(m);
        return NULL;
    }
    m->hw = hw;
    m->board = board_of(hw->model);
    hw->board = m;
    const int io = mc_hw_region(hw, "io");
    hw->bus.region[io].cost = 8;
    mc_bus_map_memory(&hw->bus, VRAM_BASE, VRAM_SIZE, m->vram, VRAM_SIZE, 1);
    if (!has_gsc(m)) {
        mc_csc_init(&m->csc, &hw->host, "CSC", m->vram, 0);
        m->csc.id = 0x01;
        mc_bus_attach(&hw->bus, io, OFS_VIDEO, 0x2000, &m->csc, &mc_csc_ops, MC_WIN_PORT8);
    } else {
        mc_gsc_init(&m->gsc, &hw->host, "GSC", m->vram, 3);
        m->gsc.height = 480;
        mc_bus_attach(&hw->bus, io, OFS_VIDEO, 0x2000, &m->gsc, &mc_gsc_ops, MC_WIN_PORT8);
    }
    const int id = mc_bus_add_io(&hw->bus, 0x5FFFF000u, 0x1000u, 0xFFFu, 0);
    mc_bus_attach(&hw->bus, id, 0xFF8u, 8, m, &cpuid_ops, 0);
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, OFS_VIA1, 0x2000, &m->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_via_init(&m->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, io, OFS_VIA2, 0x2000, m, &via2_ops, MC_WIN_PORT8);
    mc_pratt_init(&m->pratt, &hw->host, "Pratt", m->board->kind == KIND_190);
    mc_bus_attach(&hw->bus, io, OFS_PRATT, 0x2000, &m->pratt, &mc_pratt_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, OFS_PRATT_B, 0x2000, &m->pratt, &mc_pratt_b_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, OFS_PRATT_C, 0x2000, &m->pratt, &mc_pratt_c_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, OFS_PRATT_D, 0x2000, &m->pratt, &mc_pratt_d_ops, MC_WIN_PORT8);
    const int ext = mc_bus_add_io(&hw->bus, 0xFEE00000u, 0x100000u, 0xFFFFFu, 0);
    mc_bus_attach(&hw->bus, ext, 0, 0x100000u, m, &ext_ops, MC_WIN_PORT8);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_bus_attach(&hw->bus, io, OFS_SCC, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 1);
    mc_bus_attach(&hw->bus, io, OFS_SCSI, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, OFS_SCSI_DMA, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    if (has_ata(m)) {
        mc_ata_init(&m->ata, &hw->host, 2);
        mc_ata_init(&m->bay, &hw->host, 2);
        m->bay.dev.name = "ATA (bay)";
        if (bay_cd(m)) {
            mc_ata_attach_cd(&m->bay, 0);
            mc_ata_on_eject(&m->bay, bay_cd_ejected, m);
        }
        mc_bus_attach(&hw->bus, io, OFS_ATA, 0x2000, m, &ide_ops, 0);
    }
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ENHANCED);
    m->asc.block = 1;           /* the sound block: $F40-$F4F, the 16-bit ports (asc.c) */
    mc_bus_attach(&hw->bus, io, OFS_SOUND, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    /* The 190's floppy drive is the expansion bay's floppy module: none
     * while the CD-ROM module sits there. */
    mc_swim2_init(&m->swim, &hw->host, bay_cd(m) ? 0u : hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, OFS_SWIM, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    /* The PMU99 at the protocol level (its firmware is not available). */
    mc_pmu_init(&m->pmu, &hw->host, "PMU", MC_PMU_99, pmu_shifts(m) ? MC_PMU_SHIFT : MC_PMU_PARALLEL, &m->adb, NULL, 0);
    if (pmu_shifts(m)) mc_pmu_set_shift(&m->pmu, pmu_shift, m);
    /* The PMU99's command and reply counts are the ROM's PmgrOp tables'
     * (ROM B6909089 offset $D961C, where PmgrOp looks them up at $D9430;
     * ROM 4D27039C $3D370): they differ from the PG&E's in $61 and $6F
     * (counted) and, on the 190, $4C (one byte, no reply). */
    mc_pmu_set_counts(&m->pmu, hw->rom + (m->board->kind == KIND_190 ? 0x3D370u : 0xD961Cu));
    if (has_ata(m)) mc_pmu_set_bay(&m->pmu, bay_cd(m) ? 3u : 0u);
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    load_nvram(m);
    /* VIA1 port A: the identity PA6, PA4, PA2, PA1 = 0, 1, 0, 1 (ID $12
     * under mask $56); the other pins high. */
    mc_via_set_pa(&m->via1, 0xBBu, 0xFFu);
    mc_via_set_pb(&m->via1, 0xFFu, 0xFFu);
    mc_via_set_pa(&m->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via2, 0xFFu, 0xFFu);
    m->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x10000000u, m, &trap_ops, 0);
    overlay_on(m);
    mc_pmu_ops.reset(&m->pmu);
    pmu_hold(m);
    return m;
}

static const mc_board_ops pb5_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
/* The panel: the model's own size, which the board shows whatever
 * video.resolution says; no slot for the card. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    return w == model->width && h == model->height ? MC_DEPTHS_UPTO(model->depth) : 0;
}
static const mc_hw_caps pb520_caps = {video_builtin, 0, 0, NULL};
static const mc_hw_caps pb190_caps = {video_builtin, 0, MC_FEATURE_ATA_DISK | MC_FEATURE_ATA_CD | MC_FEATURE_MEDIA_BAY, NULL};

static const char *const pb520_models[] = {"powerbook-520", "powerbook-520c", "powerbook-540", "powerbook-540c", NULL};
static const uint32_t pb520_roms[] = {0xB6909089u, 0};
static const uint16_t pb520_ram[] = {4, 8, 12, 16, 20, 24, 32, 36, 0};
const mc_hw_desc mc_pb520_desc = {
    "pb520", pb520_models, M68K_68LC040, M68K_FPU_NONE, pb520_roms, pb520_ram,
    pb5_map, sizeof pb5_map / sizeof *pb5_map, pb5_irqs, sizeof pb5_irqs / sizeof *pb5_irqs,
    build, &pb5_ops, MC_MMU_040, NULL, &pb520_caps,
};

static const char *const pb190_models[] = {"powerbook-190", "powerbook-190cs", NULL};
static const uint32_t pb190_roms[] = {0x4D27039Cu, 0};
static const uint16_t pb190_ram[] = {8, 12, 16, 24, 32, 40, 0};
const mc_hw_desc mc_pb190_desc = {
    "pb190", pb190_models, M68K_68LC040, M68K_FPU_NONE, pb190_roms, pb190_ram,
    pb5_map, sizeof pb5_map / sizeof *pb5_map, pb5_irqs, sizeof pb5_irqs / sizeof *pb5_irqs,
    build, &pb5_ops, MC_MMU_040, NULL, &pb190_caps,
};
