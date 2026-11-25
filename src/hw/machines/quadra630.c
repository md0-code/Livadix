/* Livadix — the Quadra 630 / LC 630 / Performa 630 and LC 580 / Performa
 * 580/588 board (F108, PrimeTime II, Valkyrie, Cuda).
 *
 * Sources: Macintosh LC 630 and Quadra 630 Developer Note (1994): chapter 1
 * (4 MB built in, one 72-pin SIMM to 36 MB; one internal IDE hard disk and
 * an external SCSI port; the optional internal CD-ROM drive; the I/O
 * expansion slot "compatible with the PDS slot in the Macintosh LC family",
 * the communications slot), chapter 2 ("Custom ICs": F108 with the memory
 * control, a 53C96 at 16.5 MHz, the SCC with an 8 MHz PCLK and the IDE
 * interface; PrimeTime II with the I/O bus buffers, address decoding,
 * dynamic bus sizing, VIA1 and VIA2, interrupt controls, the SWIM II and
 * the sound buffers; Cuda; Valkyrie; "Address Map": RAM in the first 1 GB,
 * display RAM from $F900 0000, the I/O expansion card at $FE00 0000-$FEFF
 * FFFF as NuBus slot $E and $E000 0000-$EFFF FFFF as super slot $E), chapter
 * 3 ("IDE Hard Disk": the IDE data bus byte-swapped onto the I/O bus), chapter
 * 5 ("Machine Identification": gestaltMachineType 98; "ADB and Soft Power
 * Switch": Cuda and its soft power down), chapter 6 (the ROM's IDE driver
 * .ATDrvr, refnum -54, and the ATA Manager). Guide to the Macintosh Family
 * Hardware, 2nd ed., chapter 4 (VIA pin functions, as on the Quadra 700).
 *
 * Observed from ROMs 06684214 and 064DC91D:
 *   - the ROM reads the CPU ID register at $5FFF FFFC ($A55A in its high
 *     word) and picks the ProductInfo record whose word at +$44 matches its
 *     low word (records at ROM offset $A7EF8-$A8138): $2252, $2253, $2250,
 *     $2256, $2257, $2254 are the 630 (box flag 92, Gestalt 98), $225A,
 *     $225B, $2258 the 580 (93, Gestalt 99); both ROMs hold the records
 *     of $2252, $2253, $225A and $225B and start both families;
 *   - its decoder table (ROM offset $A84C4) puts VIA1 at $5000 0000, the
 *     SCC at $5000 C020, the SWIM II at $5001 E000, VIA2 at $5000 2000, the
 *     sound at $5001 4000, the CLUT/DAC at $5002 4000, the comm slot's
 *     Ethernet at $5000 A000, the 53C96 at $5001 0000 and the IDE interface
 *     at $5001 A000 (all within $50F0 0000 and its images, every $4 0000);
 *   - the F108's memory controller at $5000 E000 (f108.h), registers at
 *     $5001 8000 written by the ROM's 68040 start code as the IOSB's
 *     (ROM offset $4986; primetime2.h);
 *   - Valkyrie's registers at the VIA1 base + $2A000 (valkyrie.h).
 *
 * Livadix's own code. */
#include "../hw_machine.h"
#include "../absent.h"
#include "../adb.h"
#include "../adbctl.h"
#include "../asc.h"
#include "../enet_card.h"
#include "../livadix_card.h"
#include "../nubus.h"
#include "../ncr53c96.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../asic/f108.h"
#include "../asic/primetime2.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../video/valkyrie.h"
#include "../../core/models.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_IMAGE 0x40000u
/* A VIA access in C15M periods: 1.0 us, the average the Guide gives where
 * the CPU waits for the VIA clock (chapter 4, "VIA" introduction: 1.0 us on
 * the classic Macintosh and SE, 0.5 us where the glue synchronizes the VIA
 * clock to the CPU's accesses). PrimeTime II's VIA timing is not documented;
 * the ROM's Cuda resynchronisation at start (ROM offset $8574A) polls VIA1
 * 9,167 times for TREQ, and Cuda 341S0060's firmware answers it 9.3 ms after
 * its power-up, which the loop outlasts only at about 1 us an access: a
 * turn's instructions (BTST #3,(A1), BEQ not taken, DBRA taken) take 7 of the
 * 68040's cycles (MC68040 UM section 10, cache hits), 0.21 us at 33 MHz, so
 * at 0.5 us an access the loop gives up after 6.5 ms (and the machine does
 * not start with the firmware); at 1 us it lasts 11.1 ms. */
#define VIA_COST 16u

/* A board of the family: the CPU ID register's low word (the ProductInfo
 * record it selects), the RAM soldered on it and its SIMM banks. */
typedef struct q630_board {
    const char *model;
    uint16_t id;
    uint8_t onboard_mb, simm_banks;
} q630_board;

static const q630_board boards[] = {
    {"quadra-630", 0x2252, 4, 2},
    {"lc-630", 0x2252, 4, 2},
    {"performa-630", 0x2252, 4, 2},
    {"lc-580", 0x225A, 4, 4},
    {"performa-580", 0x225A, 4, 4},
    {"performa-588", 0x225A, 4, 4},
};

static const q630_board *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct q630 {
    mc_hw *hw;
    const q630_board *board;
    mc_via via1, via2;
    mc_adbctl cuda;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_f108 f108;
    mc_primetime2 pt2;
    mc_absent comm;                 /* the communication slot, empty */
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];           /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_ata ata;                     /* the internal IDE bus: the hard disk */
    mc_f108_ide ide;
    mc_vdisk *ata_vdisk;            /* the master device */
    mc_valkyrie valkyrie;
    mc_nubus nubus;                 /* the I/O expansion slot, NuBus slot $E's space */
    mc_livadix_card card;              /* the Livadix Display Card, in slot $E when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    int nubus_slotirq_high;         /* the slot's /NMRQ through mc_nubus */
    int ide_slotirq;                /* the F108's interrupt byte */
    mc_serial_host serial;
    int chime_done;
    uint64_t fb_serial;
    int overlay, overlay_region;
    int powered;                    /* Cuda switched the power on once */
    char nvram[1024];
    int64_t host_start;
} q630;

static const mc_region q630_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
    {"cpuid", 0x5FFF0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
};

static const mc_irq_route q630_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- RAM and the reset overlay ------------------------------------------------- */

static void map_ram(q630 *q) {
    mc_f108_map(&q->f108, &q->hw->bus, q->hw->ram);
    mc_hw_maps_changed(q->hw);
}

static void f108_remap(void *ctx) {
    q630 *q = ctx;
    if (!q->overlay) map_ram(q);
}

/* The ROM at 0 from reset until the first access to its own range, as on
 * the Quadra 700 (quadra700.c). */
static void overlay_on(q630 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(q630 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    q630 *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    q630 *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- the CPU ID register ------------------------------------------------------- */

static uint32_t cpu_id_read(void *dev, uint32_t offset, unsigned size) {
    const q630 *q = dev;
    const uint32_t id = 0xA55A0000u | q->board->id;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | ((id >> (8u * (3u - ((offset + i) & 3u)))) & 0xFFu);
    return v;
}
static void cpu_id_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}
static int always_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 1;
}
static const mc_dev_ops cpu_id_ops = {cpu_id_read, cpu_id_write, NULL, NULL, always_stable};

/* --- Cuda's reset and power lines ---------------------------------------------------- */

/* Cuda's /RESET: while held the 68040 waits in reset (mc_hw_hold_reset)
 * with the ROM's overlay back at 0; the release resets the board and starts
 * the CPU at the reset vector. */
static void cuda_reset(q630 *q, int held) {
    if (held && !q->overlay && q->overlay_region) overlay_on(q);
    mc_hw_hold_reset(q->hw, held);
}

/* VIA1 as the CPU sees it: Cuda brought up to the CPU's time before each
 * access (adbctl.h; its firmware runs in slices). A polling loop over VIA1
 * is not fast-forwarded while the firmware runs. */
static q630 *via1_board(void *dev) { return (q630 *)((char *)dev - offsetof(q630, via1)); }
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    mc_adbctl_sync(&via1_board(dev)->cuda);
    return mc_via_ops.read(dev, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_adbctl_sync(&via1_board(dev)->cuda);
    mc_via_ops.write(dev, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    return !via1_board(dev)->cuda.firmware && mc_via_ops.stable && mc_via_ops.stable(dev, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* --- wiring -------------------------------------------------------------------- */

/* /SLOTIRQ on VIA2 CA1, low (falling) when an interrupt comes: the F108's
 * interrupt byte or the expansion slot's /NMRQ, which the VIA2 slot handler
 * reads on port A bit 5 (ROM offset $8BC2C: bits 0 and 5 of port A are slot
 * lines, 5 dispatched as slot $E). */
static void update_slotirq(q630 *q) { mc_via_set_ca1(&q->via2, q->nubus_slotirq_high && !q->ide_slotirq); }

static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    q630 *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    q->nubus_slotirq_high = slotirq_high;
    update_slotirq(q);
}

static void board_line(void *board, void *dev, int line, int state) {
    q630 *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&q->cuda, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-2). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&q->swim, (state & 0x20) != 0);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        /* v2VBL: VIA2 Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1, as the
         * ROM programs it (as on the Quadra 700). */
        else if (line == MC_VIA_LINE_PB) mc_via_set_ca1(&q->via1, state & 0x80);
    } else if (dev == &q->cuda) {
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET) cuda_reset(q, state != 0);
        else if (line == MC_ADBCTL_LINE_POWER) {
            /* Cuda's power down: the soft power
             * supply switches off. */
            if (state) q->powered = 1;
            else if (q->powered) mc_hw_power_off(hw);
        }
    } else if (dev == &q->scsi) {
        /* The 53C96 interrupt on VIA2 CB2, low when asserted; its DREQ on
         * VIA2 CA2, high when asserted: the ROM programs CB2 for a falling
         * edge and CA2 for a rising one (PCR $26) and its SCSI Manager waits
         * for CA2's flag after a DMA selection (ROM offset $D1984). */
        if (line == MC_NCR53C96_LINE_IRQ) mc_via_set_cb2(&q->via2, !state);
        else if (line == MC_NCR53C96_LINE_DRQ) mc_via_set_ca2(&q->via2, state);
    } else if (dev == &q->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&q->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &q->asc) {
        /* The sound's /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->valkyrie) {
        /* The VBL in the F108's interrupt byte, bit 6 (f108.h). */
        if (line == MC_VALKYRIE_LINE_VBL) mc_f108_ide_set(&q->ide, MC_F108_INT_VIDEO, state);
    } else if (dev == &q->ata) {
        /* INTRQ in the F108's interrupt byte, bit 5. */
        if (line == MC_ATA_LINE_INTRQ) mc_f108_ide_set(&q->ide, MC_F108_INT_IDE, state);
    } else if (dev == &q->ide) {
        if (line == MC_F108_LINE_SLOTIRQ) {
            q->ide_slotirq = state != 0;
            update_slotirq(q);
        }
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
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

static int disk_on_ata(const q630 *q, unsigned i) {
    return i < q->hw->config.disk_count && q->hw->config.disks[i].bus == MC_BUS_ATA;
}

static void drop_disks(q630 *q) {
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

/* The `bus: ata` disks on the internal IDE bus: the hard disk, device 0
 * (master), the others further partitions of it (as on SCSI
 * 5.2). The ROM's ATA Manager drives the master device only (Developer
 * Note, chapter 6, "deviceID"), so device 1 stays empty. */
static int layout_ata(q630 *q, char *error, size_t capacity) {
    mc_disk *group[MC_CONFIG_DISKS];
    unsigned count = 0;
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&q->hw->media, MC_MEDIA_DISK, i, NULL);
        if (d && disk_on_ata(q, i)) group[count++] = d;
    }
    if (!count) return 1;
    char problem[256];
    q->ata_vdisk = make_vdisk(group, count, problem, sizeof problem);
    if (!q->ata_vdisk) {
        if (capacity) snprintf(error, capacity, "ATA disk: %s", problem);
        return 0;
    }
    mc_ata_attach_disk(&q->ata, 0, q->ata_vdisk);
    return 1;
}

/* The hard disks of hw->media on their SCSI IDs, as on the Quadra 800
 * (quadra800.c). */
static int layout_disks(q630 *q, char *error, size_t capacity) {
    const mc_config *c = &q->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(q);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&q->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set || disk_on_ata(q, i)) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&q->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || (i < c->disk_count && c->disks[i].scsi_id_set) || disk_on_ata(q, i)) continue;
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
    if (!layout_ata(q, error, capacity)) {
        drop_disks(q);
        return 0;
    }
    return 1;
}


static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    q630 *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    q630 *q = board;
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

static void board_poll(void *board) {
    q630 *q = board;
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

/* --- SCSI ------------------------------------------------------------------------ */

/* The 53C96's registers every $10 bytes from $5001 0000. */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, offset >> 4, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, offset >> 4, size); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};

/* The pseudo-DMA window: each CPU access is DACK cycles of the 53C96. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    q630 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q630 *q = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&q->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- sound ---------------------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    q630 *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    q630 *q = board;
    const size_t n = mc_asc_pull(&q->asc, stereo, frames);
    if (!q->hw->config.startup_sound && !q->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (q->asc.played && !mc_asc_fifo_level(&q->asc, 0) && !mc_asc_fifo_level(&q->asc, 1) && !q->asc.ring_count) q->chime_done = 1;
    }
    return n;
}

/* --- serial ports -------------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    q630 *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    q630 *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    q630 *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    q630 *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    q630 *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    q630 *q = board;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_ncr53c96_reset(&q->scsi);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_swim_ops.reset(&q->swim);
    mc_valkyrie_ops.reset(&q->valkyrie);
    if (q->card_slot) mc_livadix_card_ops.reset(&q->card);
    mc_enet_card_reset(q->enet);
    mc_ata_reset(&q->ata);
    mc_primetime2_ops.reset(&q->pt2);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    q630 *q = board;
    mc_valkyrie_view v;
    /* The main screen: the card when it is fitted (Valkyrie then senses no
     * monitor). */
    if (q->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&q->card, &cv)) return 0;
        v.width = cv.width;
        v.height = cv.height;
        v.depth = cv.depth;
        v.row_bytes = cv.row_bytes;
        v.pixels = cv.pixels;
        v.palette = cv.palette;
    } else if (!mc_valkyrie_view_get(&q->valkyrie, &v)) return 0;
    fb->width = v.width;
    fb->height = v.height;
    fb->depth = v.depth;
    fb->row_bytes = v.row_bytes;
    fb->pixels = v.pixels;
    fb->palette = v.palette;
    fb->serial = ++q->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    q630 *q = board;
    /* The power key also drives the ADB connector's PSW line to Cuda. */
    if ((code & 0x7Fu) == 0x7Fu) mc_adbctl_power_key(&q->cuda, down);
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adbctl_poke(&q->cuda);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    q630 *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adbctl_poke(&q->cuda);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    q630 *q = board;
    q->host_start = host_seconds;
    mc_adbctl_set_clock(&q->cuda, host_seconds);
}

static void board_destroy(void *board) {
    q630 *q = board;
    mc_adbctl_nvram_save(&q->cuda, q->nvram, q->host_start);
    mc_swim_flush(&q->swim);
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    if (q->ata_vdisk) mc_vdisk_destroy(q->ata_vdisk);
    mc_scsi_bus_free(&q->scsi_bus);
    mc_valkyrie_free(&q->valkyrie);
    mc_livadix_card_free(&q->card);
    mc_enet_card_free(q->enet);
    free(q->cuda.mcu);
    free(q);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

/* --- the screen in PRAM ------------------------------------------------------------ */

/* The built-in video's slot PRAM record (extended PRAM $46-$4D, as the ROM
 * writes it: board ID $0065, the depth's mode $80 + log2(depth), the
 * current sResource, the monitor's default sResource, the monitor's code;
 * the PrimaryInit at ROM offset $F9EE8 keeps the current sResource while the
 * default one matches the monitor): the monitors' codes and sResources from
 * the ROM's table at ROM offset $FA06E. */
static const struct { uint8_t monitor, code, rsrc; } video_rsrc[] = {
    {MC_VALKYRIE_RGB12, 0x02, 0x80}, {MC_VALKYRIE_RGB14, 0x06, 0x81},
    {MC_VALKYRIE_VGA, 0x08, 0x82}, {MC_VALKYRIE_RGB16, 0x0A, 0x83},
};
#define VIDEO_BOARD_ID 0x0065u
#define SVGA_RSRC 0x84u             /* 800 x 600 on the VGA monitor's code (the ROM's list at ROM offset $FA05A) */

/* video.depth and video.resolution as if chosen in the Monitors control
 * panel, once the ROM has written the record for the monitor connected (a
 * fresh PRAM is left to the ROM). The VGA monitor starts at 640 x 480; 800
 * x 600 is its SVGA sResource. */
static void set_video_pram(q630 *q) {
    const mc_config *c = &q->hw->config;
    uint8_t *p = mc_adbctl_pram(&q->cuda);
    int k = -1;
    for (size_t i = 0; i < sizeof video_rsrc / sizeof *video_rsrc; i++)
        if (video_rsrc[i].monitor == q->valkyrie.monitor) k = (int)i;
    if (k < 0 || p[0x10] != 0xA8 || p[0x46] != (VIDEO_BOARD_ID >> 8) || p[0x47] != (VIDEO_BOARD_ID & 0xFF) ||
        p[0x4A] != video_rsrc[k].rsrc || p[0x4B] != video_rsrc[k].code)
        return;
    p[0x49] = video_rsrc[k].monitor == MC_VALKYRIE_VGA && c->width == 800 ? SVGA_RSRC : video_rsrc[k].rsrc;
    if (c->depth > mc_valkyrie_max_depth(q->valkyrie.monitor)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card: its slot's record (eight bytes per
 * slot from extended PRAM $46 for slot $9; Designing Cards and Drivers for
 * the Macintosh Family, 3rd ed., chapter 9, "Video drivers"). */
static void set_card_depth(q630 *q) {
    const mc_config *c = &q->hw->config;
    uint8_t *pram = mc_adbctl_pram(&q->cuda);
    uint8_t *p = pram + 0x46 + 8 * (q->card_slot - 9);
    if (pram[0x10] != 0xA8 || p[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || p[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

static void *build(mc_hw *hw) {
    q630 *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    q->hw = hw;
    q->board = board_of(hw->model);
    hw->board = q;
    uint32_t banks[MC_F108_BANKS];
    if (!mc_f108_layout(hw->config.ram_mb, q->board->onboard_mb, q->board->simm_banks, banks)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: %u MiB does not fit the %s's banks",
                 (unsigned)hw->config.ram_mb, hw->model->name);
        free(q);
        return NULL;
    }
    const int io = mc_hw_region(hw, "io");
    hw->bus.region[io].cost = 8;
    /* The VIAs, in a region of their own over the first 16 KB of each image
     * of the I/O block: their accesses wait for the 783.36 kHz VIA clock. */
    const int vias = mc_bus_add_io(&hw->bus, IO_BASE, 0x04000000u, IO_IMAGE - 1u, 0);
    hw->bus.region[vias].cost = VIA_COST;
    for (uint32_t image = IO_BASE; image < IO_BASE + 0x04000000u; image += IO_IMAGE)
        mc_bus_remap(&hw->bus, io, image + 0x4000u, IO_IMAGE - 0x4000u);
    /* The I/O block (the ROM's decoder table). */
    mc_via_init(&q->via1, &hw->host, "VIA1", 9);
    mc_via_init(&q->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, vias, 0x00000, 0x2000, &q->via1, &via1_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, vias, 0x02000, 0x2000, &q->via2, &mc_via_ops, MC_WIN_PORT8);
    mc_f108_init(&q->f108, &hw->host, "F108", banks, f108_remap, q);
    mc_bus_attach(&hw->bus, io, 0x0E000, 0x2000, &q->f108, &mc_f108_ops, 0);
    mc_primetime2_init(&q->pt2, &hw->host, "PrimeTime II");
    mc_bus_attach(&hw->bus, io, 0x18000, MC_PRIMETIME2_SIZE, &q->pt2, &mc_primetime2_ops, 0);
    /* The communication slot ($5000 8000-$5000 BFFF: the decoder table's
     * Ethernet at $5000 A000, the ROM's card probe at $5000 9400) without a
     * card: the probe's latch cycles float, reading 0†; the card's own
     * window (its SONIC at $5000 A000) bus-errors, as the ROM's existence
     * test ($4081BE1A on the 5200's ROM, the same universal code) needs to
     * find no comm slot Ethernet. */
    mc_absent_init(&q->comm, &hw->host, "Communication slot", NULL, 0x00);
    mc_bus_attach(&hw->bus, io, 0x08000, 0x2000, &q->comm, &mc_absent_ops, 0);
    mc_bus_attach(&hw->bus, io, 0x0A000, 0x2000, &q->comm, &mc_absent_ops, MC_WIN_BUSERR);
    mc_bus_attach(&hw->bus, mc_hw_region(hw, "cpuid"), 0xFFFC, 4, q, &cpu_id_ops, 0);
    /* The SCC (the F108's), PCLK 8 MHz (Developer Note, "F108 IC"). */
    mc_scc_init(&q->scc, &hw->host);
    mc_scc_set_clocks(&q->scc, 8000000u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x0C000, 0x2000, &q->scc, &mc_scc_ops, MC_WIN_PORT8);
    /* SCSI: the F108's 53C96 at 16.5 MHz, its pseudo-DMA window, the
     * targets: hard disks and the CD-ROM drive at ID 3. */
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr53c96_init(&q->scsi, &hw->host, &q->scsi_bus, 16500000u);
    mc_bus_attach(&hw->bus, io, 0x10000, 0x0100, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x10100, 0x1F00, q, &pdma_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    /* Sound, the SWIM II. */
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ENHANCED);
    mc_bus_attach(&hw->bus, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim2_init(&q->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* The F108's IDE interface: the task file and the interrupt byte. */
    mc_ata_init(&q->ata, &hw->host, 2);
    mc_f108_ide_init(&q->ide, &hw->host, "F108 IDE", &q->ata);
    mc_bus_attach(&hw->bus, io, 0x1A000, 0x2000, &q->ide, &mc_f108_ide_ops, 0);
    /* Valkyrie: the CLUT/DAC, the registers, 1 MB of display memory.
     * video.card: Valkyrie when it drives the configured mode (Developer
     * Note, Table 3-9), else the Livadix Display Card in the expansion slot;
     * Valkyrie then senses no monitor. */
    mc_valkyrie_monitor monitor = mc_valkyrie_monitor_for(hw->config.width, hw->config.height);
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > mc_valkyrie_max_depth(monitor);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX || monitor == MC_VALKYRIE_NONE || mc_valkyrie_max_depth(monitor) == 0 ||
                     (over && hw->config.video_card == MC_VIDEO_CARD_AUTO);
    if (!card && over) hw->config.depth = mc_valkyrie_max_depth(monitor);
    if (card) monitor = MC_VALKYRIE_NONE;
    if (!mc_valkyrie_init(&q->valkyrie, &hw->host, "Valkyrie", monitor)) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(q);
        return NULL;
    }
    mc_bus_attach(&hw->bus, io, 0x24000, 0x2000, &q->valkyrie, &mc_valkyrie_clut_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x2A000, 0x2000, &q->valkyrie, &mc_valkyrie_ops, 0);
    mc_bus_map_float(&hw->bus, 0xF9000000u, 2u << 20);
    mc_bus_map_memory(&hw->bus, 0xF9000000u, MC_VALKYRIE_VRAM, q->valkyrie.vram, MC_VALKYRIE_VRAM, 1);
    /* The I/O expansion slot: NuBus slot $E's spaces (Developer Note,
     * "Addresses for I/O Expansion Cards"), its interrupt on VIA2 PA5. */
    q->nubus_slotirq_high = 1;
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)(1u << (0xE - 8)), slot_pins, q);
    mc_nubus_line(&q->nubus, 0xE, 5);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        q->card_slot = 0xE;
        if (mode < 0 ||
            !mc_livadix_card_init(&q->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&q->card, &q->nubus, q->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_valkyrie_free(&q->valkyrie);
            mc_livadix_card_free(&q->card);
            free(q);
            return NULL;
        }
    }
    /* The Ethernet card (network.card). */
    q->enet = mc_enet_card_fit(hw, &q->nubus, MC_ENET_CARD_LC, q->card_slot ? 0 : 0xEu);
    if (!q->enet && hw->error[0]) {
        mc_valkyrie_free(&q->valkyrie);
        mc_livadix_card_free(&q->card);
        free(q);
        return NULL;
    }
    /* ADB through Cuda: an Apple Extended Keyboard and a mouse. */
    mc_adb_bus_init(&q->adb);
    mc_adb_keyboard_init(&q->keyboard);
    mc_adb_mouse_init(&q->mouse);
    mc_adb_attach(&q->adb, &q->keyboard.dev);
    mc_adb_attach(&q->adb, &q->mouse.dev);
    mc_adbctl_init(&q->cuda, &hw->host, "Cuda", MC_ADBCTL_CUDA, &q->via1, &q->adb);
    mc_adbctl_set_part(&q->cuda, "341S0060");
    /* PRAM and the clock: Cuda's, in emulation.nvram. */
    if (mc_hw_nvram_path(hw, q->nvram, sizeof q->nvram) == NULL) q->nvram[0] = 0;
    mc_adbctl_nvram_load(&q->cuda, q->nvram);
    set_video_pram(q);
    if (q->card_slot) set_card_depth(q);
    /* storage.boot_priority: cdrom names the CD-ROM drive the default
     * startup device in PRAM ($78-$7B; Inside Macintosh V, "The Start
     * Manager"): the SCSI CD's driver reference number (ID 3). */
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        uint8_t *p = mc_adbctl_pram(&q->cuda);
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        p[0x78] = p[0x79] = 0;
        p[0x7A] = (uint8_t)(refnum >> 8);
        p[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive: VIA1 port A with vSCCWrReq (PA7) high and the
     * unconnected inputs high; VIA2 CA2 (the 53C96's DREQ) low. */
    mc_via_set_pa(&q->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via1, 0xFFu, 0xF7u);
    mc_via_set_pa(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_ca2(&q->via2, 0);
    mc_via_set_ca1(&q->via2, 1);
    /* The overlay. */
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_adbctl_ops.reset(&q->cuda);
    /* emulation.controller: Apple's Cuda firmware (it then holds /RESET
     * until it powers the machine up), or the protocol level. */
    const char *fw = mc_model_controller_firmware(hw->model);
    const char *keys[] = {fw, NULL};
    q->powered = 0;                 /* the firmware's start switches the power off: not a power down */
    const int level = mc_adbctl_select(&q->cuda, (mc_adbctl_level)hw->config.controller, hw->config.roms, fw ? keys : NULL);
    if (level < 0) {
        snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: no Cuda firmware (%s) in the roms folders",
                 fw ? fw : "none");
        board_destroy(q);
        return NULL;
    }
    q->powered = level == 0;
    return q;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((q630 *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((q630 *)board)->enet, frame, length);
}

/* The microphone: the DFAC II's input, recorded by the EASC (asc.c). */
static void board_audio_source(void *board, const mc_audio_source *source) {
    mc_asc_set_audio_source(&((q630 *)board)->asc, source);
}

static const mc_board_ops q630_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
    .audio_source = board_audio_source,
};

static const char *const q630_models[] = {"quadra-630", NULL};
static const char *const lc630_models[] = {"lc-630", "performa-630", "lc-580", "performa-580", "performa-588", NULL};
/* Each ROM holds the records of both families (docs/NOTES.md). */
static const uint32_t q630_roms[] = {0x06684214u, 0x064DC91Du, 0};
/* RAM (Developer Note, chapter 1): 4 MB on the board and one 72-pin SIMM
 * of 4, 8, 16 or 32 MB; the 580's two sockets reach 52 MB. */
static const uint16_t q630_ram[] = {4, 8, 12, 20, 36, 52, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    const mc_valkyrie_monitor monitor = mc_valkyrie_monitor_for(w, h);
    return monitor == MC_VALKYRIE_NONE ? 0 : MC_DEPTHS_UPTO(mc_valkyrie_max_depth(monitor));
}
static const mc_hw_caps q630_caps = {video_builtin, 1, MC_FEATURE_ATA_DISK | MC_FEATURE_LC_SLOT, NULL};

const mc_hw_desc mc_quadra630_desc = {
    "quadra630", q630_models, M68K_68040, M68K_FPU_68040, q630_roms, q630_ram,
    q630_map, sizeof q630_map / sizeof *q630_map, q630_irqs, sizeof q630_irqs / sizeof *q630_irqs,
    build, &q630_ops, MC_MMU_040, NULL, &q630_caps,
};

/* The LC and Performa 630 and the 580 family: the MC68LC040, no FPU
 * (Developer Note, "Microprocessor"). */
const mc_hw_desc mc_lc630_desc = {
    "lc630", lc630_models, M68K_68LC040, M68K_FPU_NONE, q630_roms, q630_ram,
    q630_map, sizeof q630_map / sizeof *q630_map, q630_irqs, sizeof q630_irqs / sizeof *q630_irqs,
    build, &q630_ops, MC_MMU_040, NULL, &q630_caps,
};
