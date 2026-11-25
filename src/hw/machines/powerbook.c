/* Livadix — the 68030 PowerBooks: the PowerBook 140 and 170 (Jaws, ROM
 * 420DBFF3), 160, 165, 165c, 180 and 180c (Niagara, ROM E33B2724), the
 * PowerBook Duo 210, 230, 250 (the MSC, ROM ECFA989B), 270c (0024D346),
 * 280 and 280c (the 68LC040, 015621D7); the PowerBook 150 (FDA22562: the
 * Duo 280's MSC board with the 68030 and an ATA disk at $50F1 A000).
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (the
 * VIAs' pins as on the Macintosh II family: VIA1 PB0-PB2 the clock chip,
 * PA5 vHeadSel, PA7 vSCCWrReq; VIA2 CA2/CB2 the 5380's DRQ and IRQ, CB1
 * the sound chip's), chapter 2 "Power Manager microprocessor" (pmu.h);
 * Macintosh PowerBook 165c Developer Note, chapter 1 (Figure 1-2, the I/O
 * map at $5000 0000 with VIA1 +0, VIA2 +$2000, SCC +$4000, the 5380's DMA
 * window +$6000 and registers +$1 0000, sound +$1 4000, SWIM +$1 6000;
 * the RAM in one continuous bank from 0, overlaid by the ROM after a reset
 * or a sleep until the ROM's own range is accessed; slot $C the color
 * panel, slot $E the external video). Inside Macintosh V, "The Start
 * Manager" (the default startup device in PRAM). Everything else is
 * observed from the ROMs (jaws.h, msc.h, gsc.h,
 * csc.h, wd90c26.h): the decoder records and tests, the ProductInfo
 * records' VIA1 identity and CPU ID words, the PMU's wiring (VIA2 port A
 * and PB1/PB2 on the 140-180c, VIA1's shift register and the MSC's port B
 * on the Duos, /INT on VIA1 CB1), the clock chip the 140-180c ROMs read
 * through _ClkNoMem, the slot $E declaration ROM inside ROM E33B2724.
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../adb.h"
#include "../asc.h"
#include "../ata.h"
#include "../ncr5380.h"
#include "../pmu.h"
#include "../rtc.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../asic/jaws.h"
#include "../asic/msc.h"
#include "../video/csc.h"
#include "../video/gsc.h"
#include "../video/wd90c26.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"
#include "../../core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_BLOCK 0x50F00000u

enum { FAM_JAWS, FAM_NIAGARA, FAM_MSC };

/* I/O block offsets (the decoder record, jaws.h). */
enum {
    OFS_VIA1 = 0x00000, OFS_VIA2 = 0x02000, OFS_SCC = 0x04000, OFS_SCSI_DMA = 0x06000, OFS_SCSI = 0x10000,
    OFS_SOUND = 0x14000, OFS_SWIM = 0x16000
};

typedef struct board_info {
    const char *model;
    uint8_t family;
    uint8_t pmu;                /* MC_PMU_* */
    uint8_t color;              /* the color panel: 0 none, else its height's code (1 640 x 400, 2 640 x 480) */
    uint16_t cpu_id;            /* the CPU ID register's low word (MSC; 0: no register) */
    uint8_t ata;                /* the internal hard disk on ATA (PowerBook 150) */
} board_info;

static const board_info boards[] = {
    {"powerbook-140", FAM_JAWS, MC_PMU_PB140},
    {"powerbook-145", FAM_JAWS, MC_PMU_PB140},
    {"powerbook-145b", FAM_JAWS, MC_PMU_PB140},
    {"powerbook-170", FAM_JAWS, MC_PMU_PB140},
    {"powerbook-160", FAM_NIAGARA, MC_PMU_PB160},
    {"powerbook-165", FAM_NIAGARA, MC_PMU_PB160},
    {"powerbook-180", FAM_NIAGARA, MC_PMU_PB160},
    {"powerbook-165c", FAM_NIAGARA, MC_PMU_PB160, 1},
    {"powerbook-180c", FAM_NIAGARA, MC_PMU_PB160, 2},
    {"powerbook-duo-210", FAM_MSC, MC_PMU_PGE, 0, 0x1004},
    {"powerbook-duo-230", FAM_MSC, MC_PMU_PGE, 0, 0x1005},
    {"powerbook-duo-250", FAM_MSC, MC_PMU_PGE, 0, 0x1006},
    {"powerbook-duo-270c", FAM_MSC, MC_PMU_PGE, 3, 0x1002},
    {"powerbook-duo-280", FAM_MSC, MC_PMU_PGE, 3, 0x1000},
    {"powerbook-duo-280c", FAM_MSC, MC_PMU_PGE, 3, 0x1000},
    {"powerbook-150", FAM_MSC, MC_PMU_99, 0, 0x1001, 1},
};

static const board_info *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct pb pb;
/* The MSC's power cycling (cycle_ops). */
typedef struct pb_cycle {
    mc_dev dev;
    pb *m;
    int state;                  /* 0 running, 1 the clock stopped, 2 the NMI raised */
} pb_cycle;

struct pb {
    mc_hw *hw;
    const board_info *board;
    mc_via via1, via2;
    mc_jaws jaws;
    mc_msc msc;                 /* the Duos' system controller */
    pb_cycle cycle;             /* its power cycling */
    mc_gsc gsc;
    mc_csc csc;                 /* the Duo 270c's, 280's and 280c's panel */
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];
    mc_vdisk *vdisk[7];
    mc_ata ata;                 /* the PowerBook 150's internal IDE bus */
    mc_vdisk *ata_vdisk;
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_pmu pmu;
    uint8_t backlight;          /* the brightness control, 0-31 (31 at start) */
    uint8_t contrast_mid;       /* the contrast the machine sets with the slider in the middle */
    mc_rtc rtc;                 /* the clock and parameter RAM chip on VIA1 (Jaws) */
    int powered;
    int cycling;                /* power cycling: the processor stopped until an interrupt */
    uint8_t via2_pa;            /* the levels VIA2 drives on port A */
    uint64_t fb_serial;
    int overlay;
    int overlay_region;
    uint8_t *vram;
    uint32_t vram_size;
    uint32_t palette[2];
    uint8_t ext[256];           /* the external video's registers (Niagara, slot $E) */
    mc_wd90c26 wd;              /* the color panel's video (165c, 180c) */
    uint8_t *declrom;           /* its declaration ROM (video-pb180c.bin) */
    char nvram[1024];
    int64_t host_start;
};

static const mc_region pb_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x10000000u, 0x000FFFFFu, MC_REGION_IO},
};

static const mc_irq_route pb_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* The PowerBook 165c and 180c: the color panel on Pangola and the WD90C26. */
static int has_wd(const pb *m) { return m->board->color == 1 || m->board->color == 2; }
static int has_csc(const pb *m) { return m->board->color == 3; }

/* --- RAM and the reset overlay --------------------------------------------------- */

static void map_ram(pb *m) {
    mc_hw *hw = m->hw;
    mc_bus_map_float(&hw->bus, 0, ROM_BASE);
    mc_bus_map_memory(&hw->bus, 0, hw->ram_size, hw->ram, hw->ram_size, 1);
    mc_hw_maps_changed(hw);
}

static void overlay_on(pb *m) {
    mc_hw *hw = m->hw;

    m->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, m->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(pb *m) {
    mc_hw *hw = m->hw;
    m->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    pb *m = dev;
    if (m->overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    pb *m = dev;
    if (m->overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- the PMU ------------------------------------------------------------------------ */

/* The Duos' PMU clocks its bytes through VIA1's shift register. */
static uint8_t pmu_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((pb *)ctx)->via1, in); }

static void pmu_hold(pb *m) {
    const int held = m->pmu.reset || m->pmu.power != MC_PMU_POWER_ON;
    if (held && !m->overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* --- wiring ------------------------------------------------------------------------ */

/* Power cycling (Jaws and Niagara): the Power Manager's idle routine saves
 * the processor's context, points the reset vectors and the level-7
 * autovector (VBR 0) at its resume code and writes the decoder's register at
 * +$2 0000 ($50FA 0000; PowerMgr globals +$9E, ROM 420DBFF3 $8853C), then
 * branches to itself with interrupts masked ($88A9C-$88B14). The processor
 * stays stopped until an interrupt is requested and then restarts through
 * its reset vectors, which the routine left in RAM at 0 and 4 (the resume
 * code at $88B98 reloads the stack, the MMU and the registers); the devices
 * keep their state†. Not during the PMU's way down to sleep or off. */
static void cycle_wake(pb *m) {
    if (!m->cycling) return;
    m->cycling = 0;
    mc_hw_cpu_restart(m->hw);
}

static void board_line(void *board, void *dev, int line, int state) {
    pb *m = board;
    mc_hw *hw = m->hw;
    if (state && ((dev == &m->via1 && line == MC_VIA_LINE_IRQ) || (dev == &m->via2 && line == MC_VIA_LINE_IRQ) ||
                  (dev == &m->msc.rbv && line == MC_RBV_LINE_IRQ) || (dev == &m->scc && line == MC_SCC_LINE_IRQ)))
        cycle_wake(m);
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
        else if (line == MC_VIA_LINE_PB) {
            /* The clock chip on PB0 rtcData, PB1 rtcClk, PB2 rtcEnb (Guide,
             * chapter 4, as on the Macintosh II family). */
            const int data = (m->via1.ddrb & 1u) ? (state & 1) : 1;
            mc_rtc_pins(&m->rtc, state & 4, state & 2, data);
        }
    } else if (dev == &m->jaws) {
        if (line == MC_JAWS_LINE_TICK) mc_via_set_ca1(&m->via1, state);
    } else if (dev == &m->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&m->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&m->via1, state);
    } else if (dev == &m->msc.rbv) {
        if (line == MC_RBV_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_RBV_LINE_TICK) mc_via_set_ca1(&m->via1, state);
        else if (line == MC_RBV_LINE_PB) mc_pmu_set_req(&m->pmu, (state & 0x04) != 0);
    } else if (dev == &m->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_VIA_LINE_PA) {
            m->via2_pa = (uint8_t)state;
            mc_pmu_set_port(&m->pmu, (uint8_t)state);
        } else if (line == MC_VIA_LINE_PB) mc_pmu_set_req(&m->pmu, (state & 0x04) != 0);
    } else if (dev == &m->pmu) {
        if (line == MC_PMU_LINE_CONTRAST && m->pmu.slider == 0x80) m->contrast_mid = (uint8_t)state;
        if (line == MC_PMU_LINE_ACK) {
            if (m->board->family == FAM_MSC) mc_msc_set_ack(&m->msc, state);
            else mc_via_set_pb(&m->via2, state ? 0x02u : 0, 0x02u);
        } else if (line == MC_PMU_LINE_INT) {
            /* /INT on VIA1 CB1, where the ROM's handler waits (Lvl1DT's
             * CB1 slot, $1A2; the Duo ROM too, ROM ECFA989B offset $884BC).
             * On the Duos CB1 is also the shift register's clock: the
             * interrupt's edges set the CB1 flag without clocking a bit
             * into a byte the Mac has just written for the PMU†. */
            const uint8_t sr = m->via1.sr, count = m->via1.sr_count;
            mc_via_set_cb1(&m->via1, state);
            if (m->board->family == FAM_MSC) {
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
        if (m->board->family == FAM_MSC) {
            if (line == MC_NCR5380_LINE_IRQ) mc_rbv_set_irq(&m->msc.rbv, MC_RBV_IRQ_SCSI, state);
            else if (line == MC_NCR5380_LINE_DRQ) mc_rbv_set_irq(&m->msc.rbv, MC_RBV_IRQ_SCSI_DRQ, state);
        } else if (line == MC_NCR5380_LINE_IRQ) mc_via_set_cb2(&m->via2, !state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_via_set_ca2(&m->via2, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) {
            if (m->board->family == FAM_MSC) mc_rbv_set_irq(&m->msc.rbv, MC_RBV_IRQ_ASC, state);
            else mc_via_set_cb1(&m->via2, !state);
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

static void drop_disks(pb *m) {
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

static int disk_on_ata(const pb *m, unsigned i) {
    return m->board->ata && i < m->hw->config.disk_count && m->hw->config.disks[i].bus == MC_BUS_ATA;
}

/* The `bus: ata` disks of the PowerBook 150: its internal hard disk, device
 * 0, the others further partitions of it (as on SCSI). */
static int layout_ata(pb *m, char *error, size_t capacity) {
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

static int layout_disks(pb *m, char *error, size_t capacity) {
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
    pb *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    pb *m = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(m, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&m->swim, drive, disk, 0, error, capacity);
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&m->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&m->unit[CD_ID], vd);
    m->vdisk[CD_ID] = vd;
    return 1;
}

static void board_poll(void *board) {
    pb *m = board;
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
}

/* --- sound --------------------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    pb *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    pb *m = board;
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
    pb *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    pb *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    pb *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    pb *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    pb *m = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&m->scc, port_channel(port), dcd);
    mc_scc_set_cts(&m->scc, port_channel(port), cts);
}

/* --- SCSI ------------------------------------------------------------------------ */

static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr5380_ops.read(dev, (offset >> 4) & 7u, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr5380_ops.write(dev, (offset >> 4) & 7u, size, value);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    pb *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- the external video port (Niagara) ------------------------------------------------
 * The PowerBook 160/180 video output circuitry answers in slot $E: the
 * declaration ROM's PrimaryInit (ROM E33B2724 offset $9B82E) writes its
 * CLUT/DAC at $FEE0 0000 and drives and reads the monitor sense lines in
 * bits 7-4 of $FEEF E004. No monitor is connected: every sense line reads
 * high (code 7), so the built-in panel is the only screen. The registers
 * are kept as written†. */
static uint32_t ext_read(void *dev, uint32_t offset, unsigned size) {
    pb *m = dev;
    (void)size;
    if (offset == 0xFE004u) return 0x70u | (m->ext[4] & 0x0Fu);
    return m->ext[offset & 0xFFu];
}
static void ext_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb *m = dev;
    (void)size;
    m->ext[offset & 0xFFu] = (uint8_t)value;
}
static const mc_dev_ops ext_ops = {ext_read, ext_write, NULL, NULL, NULL};


/* --- the CPU ID register (MSC) ------------------------------------------------------
 * $5FFF FFFC: $A55A in the high word and the board in the low word, which
 * the ROM matches against its ProductInfo records (ROM ECFA989B: the Duo
 * 210's record at $D1598 holds $1004, the 230's $1005, the 250's $1006);
 * writes are ignored, as the LC III's (lc3.c). */
static uint32_t cpuid_read(void *dev, uint32_t offset, unsigned size) {
    const pb *m = dev;
    const uint32_t id = 0xA55A0000u | m->board->cpu_id;
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

/* Power cycling on the MSC: the Power Manager's idle code (ROM ECFA989B
 * offsets $88A9C and $88B2C, through PMgrVars+$E6) saves the processor's
 * state, points the level-7 autovector ($7C) at its resume code
 * (PMgrVars+$EA, offset $88B98), masks every interrupt, writes 0 to the
 * address in PMgrVars+$9E ($50FA 0000) and loops on itself: the clock
 * stops there, and the next interrupt restarts it with an NMI, the only
 * level the masked loop takes. Its resume code restores the processor
 * (not the MMU) and the SR, so the NMI is gone before that: it is
 * released once the processor took it. Livadix's processor runs the loop;
 * the board raises the NMI once an interrupt is pending (checked every
 * millisecond)†. */
#define CYCLE_POLL (MC_C15M_HZ / 1000u)
static void cycle_event(void *dev, uint64_t now) {
    pb_cycle *c = dev;
    mc_hw *hw = c->m->hw;
    const int nmi = mc_hw_irq(hw, "nmi");
    if (c->state == 1) {
        if (!mc_irq_level(&hw->irq)) {
            mc_dev_schedule(&c->dev, now + CYCLE_POLL);
            return;
        }
        if (hw->cpu->model == M68K_68LC040 || hw->cpu->model == M68K_68040) {
            /* The Duo 280's 68LC040 loses its power: its ROM's idle code
             * ($4088A6D0, PMgrVars+$9E written $5A) leaves the save area
             * at 0 and the resume code at 4 (and $7C), and the resume code
             * reloads the registers from the stack the reset gives it
             * ($4088A762), so the processor restarts through its reset
             * vectors, as on Jaws and Niagara (cycle_wake)†. */
            c->state = 0;
            mc_hw_cpu_restart(hw);
            return;
        }
        c->state = 2;
        mc_irq_set(&hw->irq, nmi, 1);
        mc_dev_schedule(&c->dev, now + 1);
    } else if (c->state == 2) {
        if (!hw->cpu->nmi_seen) {
            mc_dev_schedule(&c->dev, now + 1);
            return;
        }
        c->state = 0;
        mc_irq_set(&hw->irq, nmi, 0);
    }
}
static uint32_t cycle_read(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 0;
}
static void cycle_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb_cycle *c = dev;
    (void)offset;
    /* MSC sleep power control is not an idle-cycle request: PB 150 ROM
     * FDA22562 $B4992 writes $FF000000 here before its masked sleep loop
     * ($8921A). Raising an idle-wake NMI there enters the uninstalled
     * interrupt handler and leaves Finder's resource file changed.
     * The idle primitives write zero (68030) or $5A (Duo 280). */
    if (value == (0xFFu << (8u * (size - 1u)))) return;
    if (c->state) return;
    c->state = 1;
    mc_dev_schedule(&c->dev, mc_dev_now(&c->dev) + CYCLE_POLL);
}
static void cycle_reset(void *dev) {
    pb_cycle *c = dev;
    if (c->state == 2) mc_irq_set(&c->m->hw->irq, mc_hw_irq(c->m->hw, "nmi"), 0);
    c->state = 0;
    mc_dev_schedule(&c->dev, MC_NEVER);
}
static const mc_dev_ops cycle_ops = {cycle_read, cycle_write, cycle_event, cycle_reset};

/* --- board operations ------------------------------------------------------------- */

/* The MSC boards' VIA1: the ROM's floppy driver selects the drive's head
 * (SEL) through ORA bit 5 (ROM FDA22562 $6D430, $6D438) while vDirA leaves
 * PA5 an input ($00, then $07: $2EA0, $47462), so SEL follows the output
 * register's bit 5 whatever the direction bit says†. */
static void via1_msc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb *m = (pb *)((char *)dev - offsetof(pb, via1));
    mc_via_ops.write(dev, offset, size, value);
    mc_swim_set_headsel(&m->swim, (m->via1.ora & 0x20u) != 0);
}
static mc_dev_ops via1_msc_ops;

/* Jaws's and Niagara's registers: a write at +$2 0000 starts power cycling
 * (see cycle_wake), at once undone when an interrupt is already pending. */
static void jaws_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pb *m = (pb *)((char *)dev - offsetof(pb, jaws));
    mc_jaws_ops.write(dev, offset, size, value);
    if ((offset & ~0x1FFFu) != 0x20000u || m->pmu.sleep_mode || m->pmu.power != MC_PMU_POWER_ON) return;
    m->cycling = 1;
    if (mc_irq_level(&m->hw->irq)) cycle_wake(m);
}
/* Niagara's register at +$3 6000 ($50FB 6000): bits 0-1 the processor's
 * clock, 16, 20, 25 or 33 MHz (the Power Manager's speed primitive, ROM
 * E33B2724 $B47A0, and its table at $B47CC). System Enabler 131 makes a
 * gray model without an FPU at 25 MHz a PowerBook 160 (box flag 28) and at
 * 33 MHz a 165 (78). The other bits read as stored†. */
static uint32_t jaws_read(void *dev, uint32_t offset, unsigned size) {
    pb *m = (pb *)((char *)dev - offsetof(pb, jaws));
    uint32_t v = mc_jaws_ops.read(dev, offset, size);
    if (m->board->family == FAM_NIAGARA && (offset & ~0x1FFFu) == 0x36000u) {
        const uint32_t hz = m->hw->model->cpu_hz;
        const uint32_t code = hz <= 16500000u ? 0u : hz <= 20500000u ? 1u : hz <= 25500000u ? 2u : 3u;
        v = (v & ~3u) | code;
    }
    return v;
}
static mc_dev_ops jaws_cycle_ops;

static void board_reset(void *board) {
    pb *m = board;
    m->cycling = 0;
    mc_via_ops.reset(&m->via1);
    if (m->board->family != FAM_MSC) mc_via_ops.reset(&m->via2);
    if (m->board->family == FAM_MSC) mc_msc_ops.reset(&m->msc);
    if (m->board->family == FAM_MSC) cycle_reset(&m->cycle);
    else mc_jaws_ops.reset(&m->jaws);
    if (m->board->family != FAM_JAWS) mc_gsc_ops.reset(&m->gsc);
    if (has_wd(m)) mc_wd90c26_ops.reset(&m->wd);
    if (has_csc(m)) mc_csc_ops.reset(&m->csc);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
    if (m->board->ata) mc_ata_reset(&m->ata);
}

static int panel_framebuffer(pb *m, mc_framebuffer *fb) {
    if (has_csc(m)) {
        mc_csc_view v;
        if (!mc_csc_view_get(&m->csc, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
        fb->serial = ++m->fb_serial;
        return 1;
    }
    if (has_wd(m)) {
        mc_wd90c26_view v;
        if (!mc_wd90c26_view_get(&m->wd, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
        fb->serial = ++m->fb_serial;
        return 1;
    }
    if (m->board->family != FAM_JAWS) {
        mc_gsc_view v;
        if (!mc_gsc_view_get(&m->gsc, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
        fb->serial = ++m->fb_serial;
        return 1;
    }

    fb->width = 640;
    fb->height = 400;
    fb->depth = 1;
    fb->row_bytes = 80;
    fb->pixels = m->vram + 0x8000u;

    m->palette[0] = 0xFFFFFFFFu;
    m->palette[1] = 0xFF000000u;
    fb->palette = m->palette;
    fb->serial = ++m->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    pb *m = board;
    if (mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    if (mc_pmu_input_key(&m->pmu, code, down)) return 1;
    return mc_adb_keyboard_key(&m->keyboard, code, down);
}

static int board_mouse(void *board, int dx, int dy, int button) {
    pb *m = board;
    if (button && mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    if (mc_pmu_input_mouse(&m->pmu, dx, dy, button)) return 1;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    return 1;
}

/* --- persistence ------------------------------------------------------------------ */

/* emulation.nvram: Livadix's PRAM file ("LVDXPRAM", version 1: the clock as
 * seconds since 1904 less the host's, 256 bytes of PRAM; quadra700.c) for
 * the clock chip (Jaws) or the PMU, followed on the boards with both by
 * the PMU's own parameter RAM. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};
#define NVRAM_RECORD (8 + 1 + 8 + 256)

/* A new PRAM: what the ROM writes when it finds the PRAM invalid, with
 * 32-bit addressing on ($8A = $05), as the Quadra 700's (quadra700.c). */
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

static int has_rtc(const pb *m) { return m->board->family == FAM_JAWS || m->board->family == FAM_NIAGARA; }

static void load_nvram(pb *m) {
    FILE *f = m->nvram[0] ? fopen(m->nvram, "rb") : NULL;
    uint8_t buf[NVRAM_RECORD + 256];
    const size_t n = f ? fread(buf, 1, sizeof buf, f) : 0;
    if (f) fclose(f);
    const int ok = n >= NVRAM_RECORD && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1;
    int64_t offset = 0;
    if (ok)
        offset = mc_nvram_clock_offset(buf + 9);
    if (has_rtc(m)) {
        if (ok) {
            memcpy(m->rtc.pram, buf + 17, 256);
            m->rtc.seconds = (uint32_t)offset;      /* the host's time is added by set_clock */
        } else {
            fresh_pram(m->rtc.pram);
        }
        if (n >= NVRAM_RECORD + mc_pmu_pram_size(&m->pmu)) mc_pmu_pram_put(&m->pmu, buf + NVRAM_RECORD);
    } else if (ok) {
        mc_pmu_pram_put(&m->pmu, buf + 17);
        mc_pmu_set_seconds(&m->pmu, (uint32_t)offset);
    } else {
        uint8_t p[256];
        fresh_pram(p);
        mc_pmu_pram_put(&m->pmu, p);
        mc_pmu_set_seconds(&m->pmu, 0);
    }
}

static void save_nvram(pb *m) {
    if (!m->nvram[0]) return;
    uint8_t buf[NVRAM_RECORD + 256];
    size_t n = NVRAM_RECORD;
    memset(buf, 0, sizeof buf);
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t host = m->host_start + (int64_t)(mc_hw_now(m->hw) / MC_C15M_HZ);
    const int64_t offset = (int64_t)(has_rtc(m) ? mc_rtc_seconds(&m->rtc) : mc_pmu_seconds(&m->pmu)) - host;
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    if (has_rtc(m)) {
        memcpy(buf + 17, m->rtc.pram, 256);
        mc_pmu_pram_get(&m->pmu, buf + NVRAM_RECORD);
        n += mc_pmu_pram_size(&m->pmu);
    } else {
        mc_pmu_pram_get(&m->pmu, buf + 17);
    }
    FILE *f = fopen(m->nvram, "wb");
    if (!f) return;
    fwrite(buf, 1, n, f);
    fclose(f);
}

static void board_set_clock(void *board, int64_t host_seconds) {
    pb *m = board;
    m->host_start = host_seconds;
    /* The clock is the PRAM file's offset (or 0) plus the host's time. */
    if (has_rtc(m)) mc_rtc_set_seconds(&m->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&m->rtc) + host_seconds));
    else mc_pmu_set_seconds(&m->pmu, (uint32_t)((int64_t)mc_pmu_seconds(&m->pmu) + host_seconds));
}

static void free_board(pb *m) {
    mc_scsi_bus_free(&m->scsi_bus);
    mc_pmu_free(&m->pmu);
    free(m->vram);
    free(m->declrom);
    free(m);
}

static void board_destroy(void *board) {
    pb *m = board;
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
    free_board(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

/* A device of the I/O block: Jaws repeats the block every MB through
 * $5000 0000-$5FFF FFFF (its region's mask); Niagara answers at
 * $5000 0000 and $50F0 0000 only (its decoder test, ROM E33B2724 offset
 * $31AC: VIA1 not at $5010 1C00 nor at +$8 0000 and +$4 0000; the ROM's
 * MMU tables put the block at physical $5000 0000). */
static void attach_io(pb *m, int io, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops, unsigned flags) {
    mc_bus *bus = &m->hw->bus;
    mc_bus_attach(bus, io, offset, size, dev, ops, flags);
    if (m->board->family != FAM_JAWS) mc_bus_attach(bus, io, 0x00F00000u + offset, size, dev, ops, flags);
}

static void *build(mc_hw *hw) {
    pb *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    m->board = board_of(hw->model);
    m->backlight = 31;
    hw->board = m;
    const int io = mc_hw_region(hw, "io");
    hw->bus.region[io].cost = 8;
    m->vram_size = m->board->family == FAM_JAWS ? 0x10000u : 0x80000u;
    m->vram = calloc(1, m->vram_size);
    if (!m->vram) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(m);
        return NULL;
    }
    if (m->board->family == FAM_JAWS) {
        mc_bus_map_memory(&hw->bus, 0xFEE00000u, m->vram_size, m->vram, m->vram_size, 1);
    } else {
        if (has_wd(m)) {
            m->declrom = calloc(1, 0x8000u);
            const mc_rom_file *file = mc_rom_index_firmware(hw->config.roms, mc_firmware_find("video-pb180c"));
            FILE *f = file ? fopen(file->path, "rb") : NULL;
            const size_t got = f ? fread(m->declrom, 1, 0x8000u, f) : 0;
            if (f) fclose(f);
            if (got != 0x8000u) {
                snprintf(hw->error, sizeof hw->error, "The %s's video declaration ROM (video-pb180c.bin) is not in the roms folders",
                         hw->model->name);
                free_board(m);
                return NULL;
            }
            mc_wd90c26_init(&m->wd, &hw->host, "WD90C26", m->vram, m->board->color == 2 ? 480u : 400u);
            const int wd = mc_bus_add_io(&hw->bus, 0xFC000000u, 0x01000000u, 0xFFFFFFu, 0);
            mc_bus_attach(&hw->bus, wd, 0, 0x01000000u, &m->wd, &mc_wd90c26_ops, MC_WIN_PORT8);
            mc_bus_map_memory(&hw->bus, 0xFC000000u + MC_WD_VRAM_OFFSET, MC_WD_VRAM_SIZE, m->vram, MC_WD_VRAM_SIZE, 1);
            mc_bus_map_memory(&hw->bus, 0xFCFF8000u, 0x8000u, m->declrom, 0x8000u, 0);
        }
        mc_bus_map_memory(&hw->bus, 0x60000000u, m->vram_size, m->vram, m->vram_size, 1);
        /* The built-in video's declaration ROM: the top of slot $E's space
         * shows the main ROM's offsets $9 0000-$9 FFFF (a format block at
         * ROM offset $9 FFFA, byte lanes $0F; the Slot Manager reads its
         * sResources from $FEFF xxxx, ROM E33B2724). */
        if (m->board->family == FAM_NIAGARA) {
            mc_bus_map_memory(&hw->bus, 0xFEFF0000u, 0x10000u, hw->rom + 0x90000u, 0x10000u, 0);
            const int ext = mc_bus_add_io(&hw->bus, 0xFEE00000u, 0x100000u, 0xFFFFFu, 0);
            mc_bus_attach(&hw->bus, ext, 0, 0x100000u, m, &ext_ops, MC_WIN_PORT8);
        }


        mc_gsc_init(&m->gsc, &hw->host, "GSC", m->vram + (m->board->family == FAM_MSC ? 0 : 0x40000u), m->board->ata ? 4 : 0);
        if (m->board->ata) m->gsc.height = 480;
        if (has_csc(m)) {
            /* The panel's type (CSC $01/$02): 4 the Duo 280's grayscale
             * panel, 0 the color one. The driver takes the box flag from it
             * (ROM 015621D7 $F8F0A-$F8F44, table $F9214: 97 for type 0, 96
             * for type 4): Gestalt 103 or 102. */
            mc_csc_init(&m->csc, &hw->host, "CSC", m->vram, strcmp(hw->model->key, "powerbook-duo-280") ? 0 : MC_CSC_PANEL_GRAY);
            attach_io(m, io, 0x20000, 0x2000, &m->csc, &mc_csc_ops, MC_WIN_PORT8);
        } else if (!has_wd(m)) {
            attach_io(m, io, 0x20000, 0x2000, &m->gsc, &mc_gsc_ops, MC_WIN_PORT8);
        }
    }

    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    via1_msc_ops = mc_via_ops;
    via1_msc_ops.write = via1_msc_write;
    attach_io(m, io, OFS_VIA1, 0x2000, &m->via1, m->board->family == FAM_MSC ? &via1_msc_ops : &mc_via_ops, MC_WIN_PORT8);
    if (m->board->family == FAM_MSC) {
        /* The MSC's VIA2 functions at $50F2 6000 (the decoder record's RBV
         * entry, ROM ECFA989B offset $D1B44), its 60.15 Hz tick to VIA1
         * CA1; no monitor of its own. */
        mc_msc_init(&m->msc, &hw->host, "MSC");
        const int id = mc_bus_add_io(&hw->bus, 0x5FFFF000u, 0x1000u, 0xFFFu, 0);
        mc_bus_attach(&hw->bus, id, 0xFFCu, 4, m, &cpuid_ops, 0);
        attach_io(m, io, 0x26000, 0x2000, &m->msc, &mc_msc_ops, MC_WIN_PORT8);
        attach_io(m, io, 0x12000, 0x2000, m, &pdma_ops, 0);
        m->cycle.dev.ops = &cycle_ops;
        m->cycle.dev.host = &hw->host;
        m->cycle.dev.name = "MSC power cycling";
        m->cycle.m = m;
        attach_io(m, io, 0xA0000, 0x2000, &m->cycle, &cycle_ops, 0);
    } else {
        mc_via_init(&m->via2, &hw->host, "VIA2", 9);
        attach_io(m, io, OFS_VIA2, 0x2000, &m->via2, &mc_via_ops, MC_WIN_PORT8);
        mc_jaws_init(&m->jaws, &hw->host, "Jaws");
        jaws_cycle_ops = mc_jaws_ops;
        jaws_cycle_ops.write = jaws_write;
        jaws_cycle_ops.read = jaws_read;
        attach_io(m, io, MC_JAWS_REGS, 0x80000, &m->jaws, &jaws_cycle_ops, MC_WIN_PORT8);
    }
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    attach_io(m, io, OFS_SCC, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 1);
    attach_io(m, io, OFS_SCSI, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    attach_io(m, io, OFS_SCSI_DMA, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    if (m->board->ata) {
        mc_ata_init(&m->ata, &hw->host, 2);
        attach_io(m, io, 0x1A000, 0x2000, &m->ata, &mc_ata_ops, 0);
    }
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ORIGINAL);
    attach_io(m, io, OFS_SOUND, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim_init(&m->swim, &hw->host, hw->media.floppy_drives);
    attach_io(m, io, OFS_SWIM, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* ADB, the clock, PRAM and power: the PMU, master of the keyboard and
     * the trackball. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_pmu_init(&m->pmu, &hw->host, "PMU", m->board->pmu, m->board->family == FAM_MSC ? MC_PMU_SHIFT : MC_PMU_PARALLEL, &m->adb, NULL, 0);
    if (m->board->family == FAM_MSC) mc_pmu_set_shift(&m->pmu, pmu_shift, m);
    /* The PowerBook 150's PMU99 answers with its ROM's PmgrOp counts (ROM
     * FDA22562 offset $89DA8: the PG&E's but for $D2's counted reply). */
    if (m->board->pmu == MC_PMU_99) mc_pmu_set_counts(&m->pmu, hw->rom + 0x89DA8u);
    /* The M50753 PMUs and the Duo 210/230/250's PG&E run Apple's firmware
     * as emulation.controller says (the PG&E's main program is the one the
     * ROM downloads into it); the later Duos' PG&E and the PMU99 stay at
     * the protocol level. */
    if (m->board->pmu != MC_PMU_99 && (m->board->pmu != MC_PMU_PGE || mc_model_controller_firmware(hw->model))) {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        if (mc_pmu_select(&m->pmu, hw->config.controller, hw->config.roms, keys) < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the PMU firmware (%s) is not in the roms folders",
                     keys[0] ? keys[0] : "?");
            free_board(m);
            return NULL;
        }
        /* The PG&E starts with the main program of the Duo ROM (ECFA989B,
         * "BORG" at $AA9C0) in its RAM: a Duo's PG&E keeps it while a
         * battery holds, and the ROM then skips the download ($E2 answers
         * $62). After a download the program's first set-up (some 1.4 s of
         * delays) outlasts the 31 $E2 the ROM sends ($B2A3C) under Livadix's
         * timing†. */
        if (m->pmu.pge && hw->rom_size >= 0xAA9C0u + 0x8004u && !memcmp(hw->rom + 0xAA9C0u, "BORG", 4))
            mc_pmu_pge_program(&m->pmu, hw->rom + 0xAA9C0u, 0x8004u);
    }
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    mc_rtc_init(&m->rtc, &hw->host, "RTC");
    load_nvram(m);
    /* VIA1 port A: the board's identity on PA6, PA4, PA2, PA1 (0, 1, 0, 1
     * on Jaws: the ProductInfo record's ID $12 under mask $56, jaws.h);
     * the other pins high. */
    mc_via_set_pa(&m->via1, m->board->family == FAM_MSC ? 0xA9u : 0xBBu, 0xFFu);
    mc_via_set_pb(&m->via1, 0xFFu, 0xFFu);
    mc_via_set_pa(&m->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via2, 0xFFu, 0xFFu);
    m->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x10000000u, m, &trap_ops, 0);
    overlay_on(m);
    if (has_rtc(m)) mc_rtc_ops.reset(&m->rtc);
    mc_pmu_ops.reset(&m->pmu);
    pmu_hold(m);
    return m;
}

/* The panel as the brightness and contrast controls leave it: the
 * backlight's brightness control (an analog control of the backlight on
 * these machines, which no software reads) as a fraction of full, 1/8 with
 * it at 0; the contrast the PMU drives (0-31: from its contrast slider, A-D
 * channel 0, pmu.h; System 7.1 settles at 15 with the slider in the
 * middle), full from the value the machine set with the slider in the
 * middle up, drawn toward mid-gray by 1/16 a step below (†: the panels'
 * response to either is Livadix's). */
static int board_framebuffer(void *board, mc_framebuffer *fb) {
    pb *m = board;
    /* The PMU removes panel power in sleep; retain VRAM for wake. */
    if (mc_pmu_asleep(&m->pmu)) return 0;
    if (!panel_framebuffer(m, fb)) return 0;
    if (m->backlight < 31) fb->level = (uint16_t)(32u + 224u * m->backlight / 31u);
    if (m->pmu.slider != 0x80 && m->pmu.contrast < m->contrast_mid) {
        const int c = 256 - 16 * (m->contrast_mid - m->pmu.contrast);
        fb->contrast = (uint16_t)(c < 32 ? 32 : c);
    }
    return 1;
}

/* The brightness control in steps of 4 (0-31); the contrast slider in
 * steps of 32, up lowering its A-D code (more contrast). */
static int board_control(void *board, int control, int value) {
    pb *m = board;
    if (control == MC_CONTROL_BRIGHTNESS) {
        const int b = (int)m->backlight + (value > 0 ? 4 : -4);
        m->backlight = (uint8_t)(b < 0 ? 0 : b > 31 ? 31 : b);
        return m->backlight;
    }
    if (control == MC_CONTROL_CONTRAST) {
        const int s = (int)m->pmu.slider + (value > 0 ? -32 : 32);
        mc_pmu_set_slider(&m->pmu, (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s));
        return 0;
    }
    return -1;
}

static const mc_board_ops pb_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem, NULL, NULL, board_control,
};

static const uint32_t jaws_roms[] = {0x420DBFF3u, 0};
static const uint16_t jaws_ram[] = {2, 4, 6, 8, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
/* The panel: the model's own size, which the board shows whatever
 * video.resolution says; no slot for the card. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    return w == model->width && h == model->height ? MC_DEPTHS_UPTO(model->depth) : 0;
}
static const mc_hw_caps pb_caps = {video_builtin, 0, 0, NULL};
static const mc_hw_caps pb150_caps = {video_builtin, 0, MC_FEATURE_ATA_DISK, NULL};

static const char *const pb140_models[] = {"powerbook-140", "powerbook-145", "powerbook-145b", NULL};
const mc_hw_desc mc_pb140_desc = {
    "pb140", pb140_models, M68K_68030, M68K_FPU_NONE, jaws_roms, jaws_ram,
    pb_map, sizeof pb_map / sizeof *pb_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};

static const char *const pb170_models[] = {"powerbook-170", NULL};
const mc_hw_desc mc_pb170_desc = {
    "pb170", pb170_models, M68K_68030, M68K_FPU_68882, jaws_roms, jaws_ram,
    pb_map, sizeof pb_map / sizeof *pb_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};

static const mc_region niagara_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x01000000u, 0x00FFFFFFu, MC_REGION_IO},
};

static const uint32_t niagara_roms[] = {0xE33B2724u, 0};

static const uint16_t niagara_ram[] = {2, 4, 6, 8, 10, 12, 14, 0};

static const char *const pb160_models[] = {"powerbook-160", "powerbook-165", NULL};
const mc_hw_desc mc_pb160_desc = {
    "pb160", pb160_models, M68K_68030, M68K_FPU_NONE, niagara_roms, niagara_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};

static const char *const pb180_models[] = {"powerbook-180", "powerbook-165c", "powerbook-180c", NULL};
static const uint32_t msc_roms[] = {0xECFA989Bu, 0};
static const uint16_t msc_ram[] = {4, 8, 12, 16, 20, 24, 0};

const mc_hw_desc mc_pb180_desc = {
    "pb180", pb180_models, M68K_68030, M68K_FPU_68882, niagara_roms, niagara_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};


static const char *const duo_models[] = {"powerbook-duo-210", "powerbook-duo-230", "powerbook-duo-250", NULL};
const mc_hw_desc mc_duo210_desc = {
    "duo210", duo_models, M68K_68030, M68K_FPU_NONE, msc_roms, msc_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};

static const char *const duo270_models[] = {"powerbook-duo-270c", NULL};
static const uint32_t duo270_roms[] = {0x0024D346u, 0};
static const uint16_t duo270_ram[] = {4, 8, 12, 16, 20, 24, 32, 0};
const mc_hw_desc mc_duo270_desc = {
    "duo270", duo270_models, M68K_68030, M68K_FPU_68882, duo270_roms, duo270_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb_caps,
};

static const char *const duo280_models[] = {"powerbook-duo-280", "powerbook-duo-280c", NULL};
static const uint32_t duo280_roms[] = {0x015621D7u, 0};
static const uint16_t duo280_ram[] = {4, 8, 12, 16, 20, 24, 32, 40, 0};
const mc_hw_desc mc_duo280_desc = {
    "duo280", duo280_models, M68K_68LC040, M68K_FPU_NONE, duo280_roms, duo280_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_040, NULL, &pb_caps,
};

/* The PowerBook 150 (ROM FDA22562): the Duo 280's MSC-class board (decoder
 * kind 10, CPU ID $1001) with the 68030 and the ATA disk. */
static const char *const pb150_models[] = {"powerbook-150", NULL};
static const uint32_t pb150_roms[] = {0xFDA22562u, 0};
static const uint16_t pb150_ram[] = {4, 8, 12, 16, 20, 24, 32, 40, 0};
const mc_hw_desc mc_pb150_desc = {
    "pb150", pb150_models, M68K_68030, M68K_FPU_NONE, pb150_roms, pb150_ram,
    niagara_map, sizeof niagara_map / sizeof *niagara_map, pb_irqs, sizeof pb_irqs / sizeof *pb_irqs,
    build, &pb_ops, MC_MMU_030, NULL, &pb150_caps,
};
