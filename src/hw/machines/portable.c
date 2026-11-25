/* Livadix — the Macintosh Portable (ROM 96CA3846) and the PowerBook 100 (ROM
 * 96645F9C, built by Sony on the Portable's architecture): the 68HC000 at
 * 15.6672 MHz on the Portable's CPU GLU, Miscellaneous GLU and VDI.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed.: chapter 3
 * ("Address map for the Macintosh Portable computer", Figure 3-4: the ROM
 * overlay at $00 0000 after a reset until the first access in $90 0000-
 * $9F FFFF; $E0 0000-$EF FFFF the processor-direct slot's; every other
 * address acknowledged, an empty one reading the undriven bus, so nothing
 * bus-errors; $FF 0000-$FF FFFF the interrupt acknowledge, autovectored;
 * Table 3-3: the VIA on /IPL0 (level 1), the SCC and the ASC on /IPL1
 * (level 2; the CPU GLU drops /IPL0 while /IPL1 is asserted), the
 * programmer's switch through the Miscellaneous GLU on /IPL2 (level 4);
 * "General logic circuits": the Miscellaneous GLU's 60 Hz to the PMU),
 * chapter 4 (Tables 4-4, 4-13 and 4-19: the VIA's port A the PMU's data
 * bus, port B PB0 vPMReq, PB1 vPMAck, PB2 vTest, PB3 vSync, PB4 vDriveSel,
 * PB5 vHeadSel, PB6 vSndExt, PB7 vSCCWrReq/vSndEnb; CA1 the 60.15 Hz, CA2
 * the PMU's one-second interrupt, CB1 the PMU's interrupt, CB2 the 5380's
 * IRQ), chapter 5 (1 MB of static RAM from $00 0000, the expansion card
 * from $10 0000, up to 9 MB; one wait state for RAM, two for the ROM),
 * chapter 12 ("Displays on the Macintosh Portable": 640 x 400, 32,000
 * bytes from ScrnBase, the most significant bit the leftmost pixel). The
 * devices' addresses are the ones the ROM itself stores in low memory
 * (ROM offset $46E-$838: VIA $F7 0000, SCC $FD 0000 read and $FD 8000
 * write, the IWM-addressed SWIM $F6 E1FF, the 5380 $F9 0000 with its
 * pseudo-DMA at $F9 0200, the ASC $FB 0000, ScrnBase $FA 8000). The
 * PowerBook 100's ROM is the Portable's with a battery check at reset
 * ($939000-$9399B5, reached from $90011E) and ROM release $11F1 (header
 * offset $12), from which the System reports the PowerBook 100 (Gestalt
 * 24); the board is the same. Everything else is observed from the ROMs:
 * $FC 0200 bit 3, a processor-direct card that
 * claims $50 0000-$8F FFFF (read by the memory sizing at ROM offset $3EB2
 * and the slot probe at $29D10: clear, no card); the 3-bit registers at
 * $FC 0001-$FC 001F the ROM's test writes and reads ($2110); $FE 0000 and
 * $FE 0200, which the ROM touches before the PMU's handshake and at reset
 * (kept as written†).
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../adb.h"
#include "../asc.h"
#include "../ncr5380.h"
#include "../pmu.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x900000u
#define RAM_SPACE 0x900000u          /* RAM and the expansion card: $00 0000-$8F FFFF */
#define OVERLAY_SIZE 0x100000u
#define VRAM_BASE 0xFA0000u
#define VRAM_SIZE 0x8000u            /* 32 KB (Guide, chapter 5), at $FA 8000 */
#define SCREEN_BYTES 32000u
/* The 60.15 Hz tick (as jaws.h's: 704 x 370 C15M periods). */
#define TICK_PERIOD (704u * 370u)

typedef struct portable portable;

/* The 60.15 Hz square wave of the Miscellaneous GLU: the VIA's CA1 and the
 * PMU's INT1. */
typedef struct pt_tick {
    mc_dev dev;
    portable *m;
    uint8_t level;
    uint64_t next;
} pt_tick;

struct portable {
    mc_hw *hw;
    mc_via via;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];
    mc_vdisk *vdisk[7];
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_pmu pmu;
    pt_tick tick;
    mc_serial_host serial;
    int chime_done;
    int powered;
    uint8_t switch_down;         /* the interrupt switch held */
    uint8_t contrast;            /* the PMU's contrast (0-31) */
    uint8_t contrast_ref;        /* the contrast it drove when the processor first ran (0: none) */
    uint8_t slow;                /* the CPU GLU's idle slow-down ($FE 0002 read; $FE 0000 ends it) */
    int overlay;
    int overlay_region;
    uint8_t vram[VRAM_SIZE];
    uint8_t glu[32];             /* $FC 0000-$FC 001F: 3-bit registers† */
    uint8_t misc[2];             /* $FE 0000, $FE 0200 as written† */
    uint32_t palette[2];
    uint64_t fb_serial;
    char nvram[1024];
    int64_t host_start;
};

static const mc_region pt_map[] = {
    {"rom", ROM_BASE, 0x100000u, 0, MC_REGION_ROM},
    {"swim", 0xF60000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
    {"via", 0xF70000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
    {"scsi", 0xF90000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
    {"asc", 0xFB0000u, 0x10000u, 0xFFFu, MC_REGION_IO},
    {"glu", 0xFC0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
    {"scc", 0xFD0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
    {"misc", 0xFE0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
};

static const mc_irq_route pt_irqs[] = {
    {"via", 1}, {"scc", 2}, {"asc", 2}, {"switch", 4},
};

/* --- RAM, ROM and the reset overlay ------------------------------------------------- */

static void map_ram(portable *m) {
    mc_hw *hw = m->hw;
    mc_bus_map_float(&hw->bus, 0, RAM_SPACE);
    mc_bus_map_memory(&hw->bus, 0, hw->ram_size, hw->ram, hw->ram_size, 1);
    mc_hw_maps_changed(hw);
}

static void overlay_on(portable *m) {
    mc_hw *hw = m->hw;
    m->overlay = 1;
    mc_bus_map_float(&hw->bus, 0, RAM_SPACE);
    mc_bus_map_memory(&hw->bus, 0, OVERLAY_SIZE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, m->overlay_region, ROM_BASE, 0x100000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(portable *m) {
    mc_hw *hw = m->hw;
    m->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x100000u, hw->rom, hw->rom_size, 0);
    map_ram(m);
}

/* The first access in $90 0000-$9F FFFF ends the overlay (the CPU GLU). */
static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    portable *m = dev;
    if (m->overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    portable *m = dev;
    if (m->overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* Wait states (Guide, chapter 5): one for RAM, two for the ROM (the overlay
 * included); a clock each. While idle (chapter 6, the CPU GLU's slow-down,
 * which the Power Manager's idle code at ROM offset $158E8 starts with a
 * read of $FE 0002 and the interrupt handlers, $4FC8 and $4FD8, and the
 * PMU's transfer, $1B3E, end with a read of $FE 0000), 64 on RAM and ROM. */
#define SLOW_WAIT 64u
static unsigned bus_wait(void *board, uint32_t a, unsigned fc, unsigned kind, uint64_t cycle) {
    (void)fc;
    (void)cycle;
    portable *m = board;
    if (kind == 3) {
        /* The interrupt switch's level 4 acknowledged: the Miscellaneous
         * GLU drops it, one interrupt a press†. */
        if (((a >> 1) & 7u) == 4u && m->switch_down) mc_irq_set(&m->hw->irq, mc_hw_irq(m->hw, "switch"), 0);
        return 0;
    }
    if (a < RAM_SPACE) return m->slow ? SLOW_WAIT : m->overlay && a < OVERLAY_SIZE ? 2u : 1u;
    if (a < 0xA00000u) return m->slow ? SLOW_WAIT : 2u;
    return 0;
}

/* --- the PMU and power ---------------------------------------------------------------- */

static void pmu_hold(portable *m) {
    const int held = m->pmu.reset || m->pmu.power != MC_PMU_POWER_ON;
    if (!held && !m->contrast_ref) m->contrast_ref = m->contrast ? m->contrast : 31u;
    if (held && !m->overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* The ROM's resume signature: its sleep code (ROM offset $46A8) stores
 * 'SARA' at $CFC before the PMU cuts the power and its reset code
 * ($7C) resumes from it. Shut Down, and the low-battery sleep that clears
 * the screen and stops ($27F4), sleep without it: the next key restarts
 * the machine. Livadix ends there, as after a power-off†. */
static int resumes(portable *m) {
    const uint8_t *r = m->hw->ram;
    return m->hw->ram_size > 0xD00u && r[0xCFC] == 'S' && r[0xCFD] == 'A' && r[0xCFE] == 'R' && r[0xCFF] == 'A';
}

static void tick_event(void *dev, uint64_t now) {
    pt_tick *t = dev;
    while (t->next <= now) {
        t->level ^= 1u;
        mc_via_set_ca1(&t->m->via, t->level);
        mc_pmu_set_tick(&t->m->pmu, t->level);
        t->next += TICK_PERIOD / 2u;
    }
    mc_dev_schedule(&t->dev, t->next);
}
static void tick_reset(void *dev) {
    pt_tick *t = dev;
    t->next = mc_dev_now(&t->dev) + TICK_PERIOD / 2u;
    mc_dev_schedule(&t->dev, t->next);
}
static const mc_dev_ops tick_ops = {NULL, NULL, tick_event, tick_reset, NULL};

/* --- wiring ----------------------------------------------------------------------------- */

static void board_line(void *board, void *dev, int line, int state) {
    portable *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via"), state);
        else if (line == MC_VIA_LINE_PA) mc_pmu_set_port(&m->pmu, (uint8_t)state);
        else if (line == MC_VIA_LINE_PB) {
            mc_pmu_set_req(&m->pmu, state & 0x01);
            mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
        }
    } else if (dev == &m->pmu) {
        if (line == MC_PMU_LINE_ACK) mc_via_set_pb(&m->via, state ? 0x02u : 0, 0x02u);
        else if (line == MC_PMU_LINE_INT) mc_via_set_cb1(&m->via, state);
        else if (line == MC_PMU_LINE_SECOND) mc_via_set_ca2(&m->via, state);
        else if (line == MC_PMU_LINE_DATA) mc_via_set_pa(&m->via, state < 0 ? 0xFFu : (uint8_t)state, 0xFFu);
        else if (line == MC_PMU_LINE_CONTRAST) m->contrast = (uint8_t)state;
        else if (line == MC_PMU_LINE_POWER || line == MC_PMU_LINE_RESET) {
            if (line == MC_PMU_LINE_POWER) {
                if (state == MC_PMU_POWER_ON) m->powered = 1;
                else if (m->powered && (state == MC_PMU_POWER_OFF || !resumes(m))) mc_hw_power_off(hw);
            }
            pmu_hold(m);
        }
    } else if (dev == &m->scsi) {
        if (line == MC_NCR5380_LINE_IRQ) mc_via_set_cb2(&m->via, !state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pb(&m->via, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "asc"), state);
    }
}

/* --- devices on their byte lanes ----------------------------------------------------- */

/* The 5380 (as the SE's): registers at n x $10, read at even and written at
 * odd addresses; A9 the pseudo-DMA's DACK ($F9 0200; the ROM moves data at
 * +$60 and +$61), the CPU GLU holding the cycle for DRQ. */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) {
    portable *m = dev;
    if (offset & 0x200u) {
        uint32_t v = 0;
        mc_ncr5380_dma_read(&m->scsi, size, &v);
        return v;
    }
    return mc_ncr5380_ops.read(&m->scsi, (offset >> 4) & 7u, size);
}
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    portable *m = dev;
    if (offset & 0x200u) mc_ncr5380_dma_write(&m->scsi, size, value);
    else mc_ncr5380_ops.write(&m->scsi, (offset >> 4) & 7u, size, value);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

/* The SCC: read at $FD 0000 + n, written at $FD 8000 + n (A15), Apple's
 * offsets (scc.h). A read of the write space or a write to the read space
 * reaches nothing†. */
static uint32_t scc_read(void *dev, uint32_t offset, unsigned size) {
    portable *m = dev;
    if (offset & 0x8000u) return 0xFFu;
    return mc_scc_ops.read(&m->scc, offset & 7u, size);
}
static void scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    portable *m = dev;
    if (offset & 0x8000u) mc_scc_ops.write(&m->scc, offset & 7u, size, value);
}
static const mc_dev_ops scc_ops = {scc_read, scc_write, NULL, NULL, NULL};

/* $FC 0000: the 3-bit registers at odd addresses the ROM's test writes and
 * reads back; $FC 0200 bit 3 clear (no processor-direct card claiming
 * $50 0000-$8F FFFF); the rest reads 0†. */
static uint32_t glu_read(void *dev, uint32_t offset, unsigned size) {
    portable *m = dev;
    (void)size;
    if (offset < 0x20u) return m->glu[offset] & 7u;
    return 0;
}
static void glu_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    portable *m = dev;
    (void)size;
    if (offset < 0x20u) m->glu[offset] = (uint8_t)(value & 7u);
}
static const mc_dev_ops glu_ops = {glu_read, glu_write, NULL, NULL, NULL};

/* $FE 0000: the CPU GLU's speed (bus_wait): a read of $FE 0000 runs the
 * processor at full speed, one of $FE 0002 slows it down (the Power
 * Manager's idle code, ROM offset $158E8); they read as 0†. $FE 0200,
 * written $0006 at reset ($74) and on the way back from sleep ($46E0), is
 * kept as written†. */
static uint32_t misc_read(void *dev, uint32_t offset, unsigned size) {
    portable *m = dev;
    (void)size;
    if (offset < 0x200u) m->slow = (offset & 2u) != 0;
    return 0;
}
static void misc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    portable *m = dev;
    (void)size;
    if (offset < 0x200u) m->slow = (offset & 2u) != 0;
    if (offset < 0x400u) m->misc[(offset >> 9) & 1u] = (uint8_t)value;
}
static const mc_dev_ops misc_ops = {misc_read, misc_write, NULL, NULL, NULL};

/* --- storage -------------------------------------------------------------------------- */

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0, ata_size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const uint8_t *ata = mc_guest_livadixdisk_ata(&ata_size);
    const mc_vdisk_drivers drivers = {driver, size, ata, ata_size};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(portable *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

static int layout_disks(portable *m, char *error, size_t capacity) {
    const mc_config *c = &m->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(m);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL);
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
        m->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!m->vdisk[id]) {
            if (capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(m);
            return 0;
        }
        mc_scsi_disk_init(&m->unit[id], m->vdisk[id]);
        mc_scsi_bus_attach(&m->scsi_bus, id, mc_scsi_unit_target(&m->unit[id]));
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    portable *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    portable *m = board;
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
    portable *m = board;
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

/* --- sound and serial ports ---------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    portable *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    portable *m = board;
    const size_t n = mc_asc_pull(&m->asc, stereo, frames);
    if (!m->hw->config.startup_sound && !m->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (m->asc.played && !mc_asc_fifo_level(&m->asc, 0) && !mc_asc_fifo_level(&m->asc, 1) && !m->asc.ring_count) m->chime_done = 1;
    }
    return n;
}

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    portable *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    portable *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    portable *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    portable *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    portable *m = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&m->scc, port_channel(port), dcd);
    mc_scc_set_cts(&m->scc, port_channel(port), cts);
}

/* --- board operations ---------------------------------------------------------------- */

static void board_reset(void *board) {
    portable *m = board;
    mc_via_ops.reset(&m->via);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
    memset(m->glu, 0, sizeof m->glu);
    m->slow = 0;
    /* Pins other chips drive: no plug in the sound jack, /W//REQ released,
     * /ACK as the PMU holds it. */
    mc_via_set_pb(&m->via, (uint8_t)(0xFDu | (m->pmu.ack ? 0x02u : 0)), 0xFFu);
    mc_via_set_pa(&m->via, m->pmu.data_out < 0 ? 0xFFu : (uint8_t)m->pmu.data_out, 0xFFu);
}

/* The panel: 640 x 400 x 1 from the video RAM at $FA 8000, at the
 * contrast the PMU drives (0-31, pmu.h): full at the value it drove when
 * the processor first ran (4, its firmware's own; the ROMs set 7 for the
 * PowerBook 100's battery-charging screen with command $40, ROM 96645F9C
 * offset $3906E), drawn toward mid-gray by 1/16 a step below it (as
 * powerbook.c's panels†). */
static int board_framebuffer(void *board, mc_framebuffer *fb) {
    portable *m = board;
    fb->width = 640;
    fb->height = 400;
    fb->depth = 1;
    fb->row_bytes = 80;
    fb->pixels = m->vram;
    m->palette[0] = 0xFFFFFFFFu;
    m->palette[1] = 0xFF000000u;
    fb->palette = m->palette;
    fb->serial = ++m->fb_serial;
    if (m->contrast_ref && m->contrast < m->contrast_ref) {
        const int c = 256 - 16 * (m->contrast_ref - m->contrast);
        fb->contrast = (uint16_t)(c < 32 ? 32 : c);
    }
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    portable *m = board;
    if (mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    return mc_adb_keyboard_key(&m->keyboard, code, down);
}

static int board_mouse(void *board, int dx, int dy, int button) {
    portable *m = board;
    if (button && mc_pmu_asleep(&m->pmu)) mc_pmu_wake(&m->pmu);
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    return 1;
}

/* --- persistence ---------------------------------------------------------------------- */

/* emulation.nvram: Livadix's PRAM file ("LVDXPRAM", version 1: the clock as
 * seconds since 1904 less the host's, then the PMU's 128 bytes of
 * parameter RAM; powerbook.c). */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};
#define NVRAM_RECORD (8 + 1 + 8 + 256)

/* A new PRAM: what the ROM writes when it finds it invalid (as the
 * PowerBooks', powerbook.c). */
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

static void load_nvram(portable *m) {
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

static void save_nvram(portable *m) {
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
    portable *m = board;
    m->host_start = host_seconds;
    mc_pmu_set_seconds(&m->pmu, (uint32_t)((int64_t)mc_pmu_seconds(&m->pmu) + host_seconds));
}

static void free_board(portable *m) {
    mc_scsi_bus_free(&m->scsi_bus);
    mc_pmu_free(&m->pmu);
    free(m);
}

static void board_destroy(void *board) {
    portable *m = board;
    save_nvram(m);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    free_board(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static int attach(portable *m, const char *region, void *dev, const mc_dev_ops *ops, uint16_t cost) {
    mc_hw *hw = m->hw;
    const int r = mc_hw_region(hw, region);
    hw->bus.region[r].cost = cost;
    return mc_bus_attach(&hw->bus, r, 0, (uint32_t)hw->bus.region[r].mask + 1u, dev, ops, MC_WIN_PORT8);
}

static void *build(mc_hw *hw) {
    portable *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    hw->board = m;
    hw->wait = bus_wait;
    /* Nothing bus-errors: the empty spaces read the undriven bus. */
    mc_bus_map_float(&hw->bus, 0xA00000u, 0x560000u);
    mc_bus_map_float(&hw->bus, 0xF80000u, 0x10000u);
    mc_bus_map_float(&hw->bus, 0xFF0000u, 0x10000u);
    mc_bus_map_memory(&hw->bus, VRAM_BASE, 0x10000u, m->vram, VRAM_SIZE, 1);

    mc_via_init(&m->via, &hw->host, "VIA", 9);
    attach(m, "via", &m->via, &mc_via_ops, 16);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    attach(m, "scc", m, &scc_ops, 8);
    mc_swim_init(&m->swim, &hw->host, hw->media.floppy_drives);
    attach(m, "swim", &m->swim, &mc_swim_ops, 8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 0);
    attach(m, "scsi", m, &scsi_ops, 8);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ORIGINAL);
    attach(m, "asc", &m->asc, &mc_asc_ops, 8);
    attach(m, "glu", m, &glu_ops, 8);
    attach(m, "misc", m, &misc_ops, 8);

    m->tick.dev.ops = &tick_ops;
    m->tick.dev.host = &hw->host;
    m->tick.dev.name = "60.15 Hz";
    m->tick.m = m;
    m->tick.level = 1;

    /* ADB, the clock, PRAM, power and sleep: the PMU, master of the
     * keyboard and the trackball. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_pmu_init(&m->pmu, &hw->host, "PMU", MC_PMU_PORTABLE, MC_PMU_PARALLEL, &m->adb, NULL, 0);
    m->pmu.tick_in = 1;
    /* emulation.charger_start: the PowerBook 100 started by plugging in its
     * charger: the PMU's IN bit 5 clear, which its ROM reads at reset
     * (offset $3980E) and answers with its battery-charging screen, then
     * switches itself off ($39956: PMU command $7F 'MATT'). */
    m->pmu.charger_start = (uint8_t)(hw->config.charger_start && !strcmp(hw->model->key, "powerbook-100"));
    const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
    if (mc_pmu_select(&m->pmu, hw->config.controller, hw->config.roms, keys) < 0) {
        snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the PMU firmware (%s) is not in the roms folders",
                 keys[0] ? keys[0] : "?");
        free_board(m);
        return NULL;
    }
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    load_nvram(m);

    mc_via_set_pa(&m->via, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via, 0xFFu, 0xFFu);
    m->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x100000u, 0xFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x100000u, m, &trap_ops, 0);
    overlay_on(m);
    tick_reset(&m->tick);
    mc_pmu_ops.reset(&m->pmu);
    pmu_hold(m);
    return m;
}

/* The programmer's switch (machine.h): the interrupt button on the
 * Miscellaneous GLU's level 4 (Guide, Table 3-3), the reset button on the
 * PMU's INT2 (pmu.h, mc_pmu_reset_switch). */
static int board_control(void *board, int control, int value) {
    portable *m = board;
    if (control == MC_CONTROL_INTERRUPT) {
        m->switch_down = (uint8_t)(value != 0);
        mc_irq_set(&m->hw->irq, mc_hw_irq(m->hw, "switch"), value != 0);
        return 0;
    }
    if (control == MC_CONTROL_RESET) {
        mc_pmu_reset_switch(&m->pmu, value != 0);
        return 0;
    }
    return -1;
}

static const mc_board_ops pt_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem, NULL, NULL, board_control,
};

/* The panel: 640 x 400 x 1 whatever video.resolution says; no slot for the
 * card. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return w == 640 && h == 400 ? MC_DEPTH_BIT(1) : 0;
}
static const mc_hw_caps pt_caps = {video_builtin, 0, 0, NULL};

/* RAM: the Portable's 1 MB on the board and its card's up to 8 MB; the
 * PowerBook 100's 2 MB and its card's 2, 4 or 6 MB. */
static const uint16_t portable_ram[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 0};
static const uint16_t pb100_ram[] = {2, 4, 6, 8, 0};

static const char *const portable_models[] = {"mac-portable", NULL};
static const uint32_t portable_roms[] = {0x96CA3846u, 0};
const mc_hw_desc mc_portable_desc = {
    "portable", portable_models, M68K_68000, M68K_FPU_NONE, portable_roms, portable_ram,
    pt_map, sizeof pt_map / sizeof *pt_map, pt_irqs, sizeof pt_irqs / sizeof *pt_irqs,
    build, &pt_ops, MC_MMU_NONE, NULL, &pt_caps,
};

static const char *const pb100_models[] = {"powerbook-100", NULL};
static const uint32_t pb100_roms[] = {0x96645F9Cu, 0};
const mc_hw_desc mc_pb100_desc = {
    "pb100", pb100_models, M68K_68000, M68K_FPU_NONE, pb100_roms, pb100_ram,
    pt_map, sizeof pt_map / sizeof *pt_map, pt_irqs, sizeof pt_irqs / sizeof *pt_irqs,
    build, &pt_ops, MC_MMU_NONE, NULL, &pt_caps,
};
