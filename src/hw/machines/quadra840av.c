/* Livadix — the Quadra 840AV and Centris 660AV board (MCA, PSC, Civic, New
 * Age, Cuda).
 *
 * Sources: Macintosh Quadra 840AV and Centris 660AV Developer Note (Apple,
 * 1993): chapter 1, Table 1-1 (machine types 78 and 60); chapter 2, Figure
 * 2-1 (block diagram: MCA, PSC, VIA1, Cuda, Curio with the SCSI controller,
 * the Ethernet MACE and the SCC, New Age, Civic, Sebastian, Singer, the
 * DSP3210, MUNI), "Random-Access Memory", "Memory Controller and Arbiter"
 * (mca.h), "Digital Signal Processor" (66.6667 and 55.5 MHz), "Peripheral
 * Subsystem Controller" (psc.h), "Macintosh Universal NuBus Interface"
 * (the MUNI on the 840AV's board; on the 660AV on an adapter in its PDS),
 * "Cyclone Integrated Video Interfaces Controller" (civic.h), "New Age"
 * (newage.h), "Curio", "Cuda" (cuda.h), Table 2-2 (clocks: 40 and 25 MHz
 * bus clocks, C25M for the SCSI controller), "NuBus Interface" (slots $C,
 * $D, $E on the 840AV; $C on the 660AV), Table 2-13 (VRAM: 1 MB, the
 * 840AV's expansion to 2 MB). Guide to the Macintosh Family Hardware, 2nd
 * ed., chapter 4 (VIA1's pins).
 *
 * Observed from ROM 5BF10FD1:
 *   - the ROM reads the CPU ID register, $A55A 2830 (its records' word at
 *     +$54, ROM offset $F6CA), then, for this decoder (kind 13), the MCA's
 *     identification inputs (mca.h): $F the 840AV (box 72), $B the 660AV
 *     (box 54);
 *   - the decoder table at ROM offset $14D7C: VIA1 $50F0 0000, SCC $50F0
 *     4000, VIA2 $50F0 2000, SCSI $50F1 8000, the Ethernet MACE $50F1 C000,
 *     New Age $50F2 A000, the PSC $50F3 1000, the MCA $50F3 0000, Civic
 *     $5003 6000, Sebastian $50F3 0800; VRAM at $5010 0000 (the video
 *     driver's PrimaryInit tests 2 MB there, ROM offset $1FF38A);
 *   - the SCSI controller's registers every $10 from $50F1 8000 and a
 *     16-bit data port at $50F1 8100 the SCSI Manager writes command bytes
 *     through ($1D98A); its interrupt and DREQ on VIA2 (psc.h);
 *   - the level 1-6 autovectors: VIA1, VIA2, the PSC's levels 3-6 (ROM
 *     offsets $107D0-$10820);
 *   - Cuda on VIA1 PB3-PB5 and the shift register, as on the LC 475
 *     (quadra605.c).
 *
 * Livadix's own code. */
#include "../hw_machine.h"
#include "../adb.h"
#include "../adbctl.h"
#include "../via.h"
#include "../asic/mca.h"
#include "../asic/psc.h"
#include "../livadix_card.h"
#include "../nubus.h"
#include "../ncr53c96.h"
#include "../newage.h"
#include "../am79c940.h"
#include "../dsp3210.h"
#include "../sonic.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../video/civic.h"
#include "../../core/models.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define VRAM_BASE 0x50100000u
#define CPU_ID 0xA55A2830u          /* the ROM's ProductInfo word +$54 for both models, with $A55A */

typedef struct av_board {
    const char *model;
    uint8_t id;                     /* the MCA's identification inputs */
    uint8_t onboard_mb, simm_banks;
    uint32_t vram;
    uint16_t video_id;              /* the built-in video's board ID in its slot PRAM (observed) */
    uint8_t slots;                  /* bit s - 8 for each NuBus slot s */
    uint32_t dsp_ips;               /* the DSP3210's instructions per second: CKI / 4 */
} av_board;

static const av_board boards[] = {
    /* The DSP at 66.6667 MHz on the 840AV, 55.5 MHz on the 660AV (Developer
     * Note, chapter 2, "Digital Signal Processor"). */
    {"quadra-840av", MC_MCA_ID_840AV, 0, 8, 2u << 20, 0x003D, 1u << (0xC - 8) | 1u << (0xD - 8) | 1u << (0xE - 8), 16666667u},
    {"quadra-660av", MC_MCA_ID_660AV, 4, 4, 1u << 20, 0x0050, 1u << (0xC - 8), 13875000u},
};

static const av_board *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct av {
    mc_hw *hw;
    const av_board *board;
    mc_via via1;
    mc_mca mca;
    mc_psc psc;
    /* The DSP3210: run in slices of guest time (dsp_event) while it is not
     * held or waiting for an interrupt. */
    mc_dsp3210 dsp;
    mc_dev dsp_dev;
    uint64_t dsp_time, dsp_owed;
    uint8_t dsp_bio0;
    mc_civic civic;
    mc_newage newage;
    mc_am79c940 enet;
    mc_sonic_prom enet_prom;
    mc_net_host net;
    mc_scc scc;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];           /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;
    unsigned card_slot;
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adbctl cuda;
    int powered;
    uint64_t fb_serial;
    int overlay;
    int overlay_region;
    char nvram[1024];
    int64_t host_start;
} av;

static const mc_region av_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x01000000u, 0xFFFFFFu, MC_REGION_IO},
    {"cpuid", 0x5FFF0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
};

static const mc_irq_route av_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"psc3", 3}, {"psc4", 4}, {"psc5", 5}, {"psc6", 6}, {"nmi", 7},
};

/* --- the reset overlay ------------------------------------------------------------- */

static void map_ram(av *a);

/* The ROM at 0 from reset until the first access to its own range
 * (Developer Note, "Startup Memory Addressing"). */
static void overlay_on(av *a) {
    mc_hw *hw = a->hw;
    a->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, a->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(av *a) {
    mc_hw *hw = a->hw;
    a->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(a);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    av *a = dev;
    if (a->overlay) overlay_off(a);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | a->hw->rom[(offset + i) & (a->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    av *a = dev;
    if (a->overlay) overlay_off(a);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

static void map_ram(av *a) {
    mc_mca_map(&a->mca, &a->hw->bus, a->hw->ram);
    mc_hw_maps_changed(a->hw);
}

static void mca_remap(void *ctx) {
    av *a = ctx;
    if (!a->overlay) map_ram(a);
}

/* --- VIA1 and Cuda ----------------------------------------------------------------- */

static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    av *a = dev;
    mc_adbctl_sync(&a->cuda);
    return mc_via_ops.read(&a->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    av *a = dev;
    mc_adbctl_sync(&a->cuda);
    mc_via_ops.write(&a->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    av *a = dev;
    return !a->cuda.firmware && mc_via_ops.stable && mc_via_ops.stable(&a->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* --- wiring ------------------------------------------------------------------------ */

static void cuda_hold(av *a) {
    const int held = a->cuda.line_state[MC_ADBCTL_LINE_RESET] || !a->cuda.line_state[MC_ADBCTL_LINE_POWER];
    if (held && !a->overlay && a->overlay_region) overlay_on(a);
    mc_hw_hold_reset(a->hw, held);
}

/* --- the DSP3210 --------------------------------------------------------------------- */

/* The DSP is a bus master on the CPU bus (Developer Note, Figure 2-1): it
 * reads and writes physical addresses, DRAM through the bus's page tables,
 * everything else through the bus as the processor's cycles would. */
#define DSP_SLICE (MC_C15M_HZ / 4000u)      /* 0.25 ms of guest time per run */

static av *dsp_board(void *dev) { return (av *)((char *)dev - offsetof(av, dsp_dev)); }

static int dsp_bus_read(void *ctx, uint32_t address, unsigned size, uint32_t *value) {
    return mc_bus_read(&((av *)ctx)->hw->bus, address, size, value);
}
static int dsp_bus_write(void *ctx, uint32_t address, unsigned size, uint32_t value) {
    return mc_bus_write(&((av *)ctx)->hw->bus, address, size, value);
}

/* BIO0, an output, rings the host on each change (level 5 bit 0)†. */
static void dsp_bio(void *ctx, uint8_t levels, uint8_t outputs) {
    av *a = ctx;
    const uint8_t bio0 = (outputs & 1u) ? (uint8_t)(levels & 1u) : 0;
    if (bio0 != a->dsp_bio0) mc_psc_dsp_doorbell(&a->psc);
    a->dsp_bio0 = bio0;
}
/* IACK1 acknowledges the PSC's frame interrupt. */
static void dsp_iack(void *ctx, int line) {
    av *a = ctx;
    if (line == 1) mc_psc_dsp_ack(&a->psc);
}

/* Runs the DSP up to the bus's present. */
static void dsp_sync(av *a) {
    const uint64_t now = mc_dev_now(&a->dsp_dev);
    if (now <= a->dsp_time) return;
    if (mc_dsp3210_idle(&a->dsp)) {
        a->dsp_time = now;
        a->dsp_owed = 0;
        return;
    }
    a->dsp_owed += (now - a->dsp_time) * a->board->dsp_ips;
    a->dsp_time = now;
    uint64_t n = a->dsp_owed / MC_C15M_HZ;
    a->dsp_owed -= n * MC_C15M_HZ;
    while (n) {
        const unsigned chunk = n > 1000000u ? 1000000u : (unsigned)n;
        const unsigned ran = mc_dsp3210_run(&a->dsp, chunk);
        if (ran < chunk) { a->dsp_owed = 0; break; }
        n -= chunk;
    }
}

static void dsp_rearm(av *a) {
    mc_dev_schedule(&a->dsp_dev, mc_dsp3210_idle(&a->dsp) ? MC_NEVER : mc_dev_now(&a->dsp_dev) + DSP_SLICE);
}

static void dsp_event(void *dev, uint64_t now) {
    av *a = dsp_board(dev);
    (void)now;
    dsp_sync(a);
    dsp_rearm(a);
}
static const mc_dev_ops dsp_ops = {NULL, NULL, dsp_event, NULL, NULL};

static void dsp_reset_line(av *a, int held) {
    dsp_sync(a);
    mc_dsp3210_reset(&a->dsp, held);
    a->dsp_time = mc_dev_now(&a->dsp_dev);
    dsp_rearm(a);
}

static void board_line(void *board, void *dev, int line, int state) {
    av *a = board;
    mc_hw *hw = a->hw;
    if (dev == &a->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: Cuda's TREQ, BYTEACK and TIP (cuda.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&a->cuda, (uint8_t)state);
    } else if (dev == &a->psc) {
        static const char *const levels[4] = {"psc3", "psc4", "psc5", "psc6"};
        if (line >= MC_PSC_LINE_IRQ3 && line <= MC_PSC_LINE_IRQ6) mc_irq_set(&hw->irq, mc_hw_irq(hw, levels[line - MC_PSC_LINE_IRQ3]), state);
        else if (line == MC_PSC_LINE_VIA2) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_PSC_LINE_TICK) mc_via_set_ca1(&a->via1, state);
        else if (line == MC_PSC_LINE_DSP_RESET) dsp_reset_line(a, state);
        else if (line == MC_PSC_LINE_DSP_FRAME) {
            dsp_sync(a);
            mc_dsp3210_set_irq(&a->dsp, 1, state);
            dsp_rearm(a);
        }
    } else if (dev == &a->cuda) {
        /* The processor runs while Cuda powers the system and releases
         * /RESET; Cuda's power down switches the machine off. */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) a->powered = 1;
                else if (a->powered) mc_hw_power_off(hw);
            }
            cuda_hold(a);
        }
    } else if (dev == &a->scsi) {
        if (line == MC_NCR53C96_LINE_IRQ) mc_psc_set_source(&a->psc, MC_PSC_SRC_VIA2_SCSI_IRQ, state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            mc_psc_set_source(&a->psc, MC_PSC_SRC_VIA2_SCSI_DRQ, state);
            mc_psc_request(&a->psc, MC_PSC_CH_SCSI, state);
        }
    } else if (dev == &a->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_psc_set_source(&a->psc, MC_PSC_SRC_SCC_A, state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&a->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &a->newage) {
        if (line == MC_NEWAGE_LINE_IRQ) mc_psc_set_source(&a->psc, MC_PSC_SRC_VIA2_FDC, state);
        else if (line == MC_NEWAGE_LINE_DRQ) mc_psc_request(&a->psc, MC_PSC_CH_FDC, state);
    } else if (dev == &a->enet) {
        if (line == MC_AM79C940_LINE_IRQ) mc_psc_set_source(&a->psc, MC_PSC_SRC_ENET, state);
    } else if (dev == &a->civic) {
        if (line == MC_CIVIC_LINE_VBL) mc_nubus_irq_bit(&a->nubus, 6, state);
    } else if (dev == &a->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&a->nubus, a->card_slot, state);
    }
}

/* The slots' interrupts on VIA2 port A, active low, and VIA2's slot bit. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    av *a = ctx;
    (void)slotirq_high;
    mc_psc_set_slots(&a->psc, (uint8_t)(~levels & mask));
}

/* --- persistence ------------------------------------------------------------------ */

/* A new PRAM: the ROM's own defaults (as quadra605.c), 32-bit addressing on. */
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

static void load_nvram(av *a) {
    if (!mc_adbctl_nvram_load(&a->cuda, a->nvram)) {
        fresh_pram(mc_adbctl_pram(&a->cuda));
        mc_adbctl_set_seconds(&a->cuda, 0);
    }
}

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM (slot $9 at extended PRAM
 * $46-$4D: board ID $003D on the 840AV, $0050 on the 660AV, the depth's mode $80 + log2(depth), the
 * monitor's sResource twice, its sense code), as the ROM writes it ($06
 * for the 13" monitor, observed), takes the mode when the record is for the
 * monitor connected. A fresh PRAM is left to the ROM (8 bits). */
static void set_depth(av *a, uint8_t *p) {
    static const struct { uint8_t monitor, code; } codes[] = {
        {MC_CIVIC_RGB12, 0x02}, {MC_CIVIC_RGB13, 0x06}, {MC_CIVIC_PORTRAIT, 0x05}, {MC_CIVIC_RGB16, 0x0A},
        {MC_CIVIC_RGB19, 0x0B}, {MC_CIVIC_RGB21, 0x00},
    };
    const mc_config *c = &a->hw->config;
    int code = -1;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (codes[i].monitor == a->civic.monitor) code = codes[i].code;
    if (code < 0 || p[0x10] != 0xA8 || p[0x46] != (a->board->video_id >> 8) || p[0x47] != (a->board->video_id & 0xFF) || p[0x4B] != code) return;
    if (c->depth > mc_civic_max_depth(a->civic.monitor, a->board->vram)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card (Designing Cards and Drivers, 3rd
 * ed., chapter 9). */
static void set_card_depth(av *a, uint8_t *pram) {
    const mc_config *c = &a->hw->config;
    uint8_t *p = pram + 0x46 + 8 * (a->card_slot - 9);
    if (pram[0x10] != 0xA8 || p[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || p[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

/* --- storage -------------------------------------------------------------------- */

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const mc_vdisk_drivers drivers = {driver, size, NULL, 0};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(av *a) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !a->vdisk[id]) continue;
        mc_scsi_bus_attach(&a->scsi_bus, id, NULL);
        mc_vdisk_destroy(a->vdisk[id]);
        a->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as on the Quadra 800 (quadra800.c). */
static int layout_disks(av *a, char *error, size_t capacity) {
    const mc_config *c = &a->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(a);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&a->hw->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&a->hw->media, MC_MEDIA_DISK, i, NULL);
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
        a->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!a->vdisk[id]) {
            if (capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(a);
            return 0;
        }
        mc_scsi_disk_init(&a->unit[id], a->vdisk[id]);
        mc_scsi_bus_attach(&a->scsi_bus, id, mc_scsi_unit_target(&a->unit[id]));
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    av *a = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    a->vdisk[CD_ID] = NULL;
    mc_media_eject(&a->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    av *a = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(a, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_newage_insert(&a->newage, drive, disk, 0, error, capacity);
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&a->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&a->unit[CD_ID], vd);
    a->vdisk[CD_ID] = vd;
    return 1;
}

static void board_poll(void *board) {
    av *a = board;
    mc_media *media = &a->hw->media;
    /* The floppy against hw->media: what the guest ejected leaves the
     * drive and is marked ejected; what the host ejected leaves the drive. */
    for (unsigned i = 0; i < a->newage.drives; i++) {
        int ejected = 0, lost = 0;
        mc_disk *chip = mc_newage_media(&a->newage, i, &ejected);
        mc_disk *d = mc_media_get(media, MC_MEDIA_FLOPPY, i, &lost);
        if (!chip) continue;
        if (ejected || chip != d || lost) {
            mc_newage_remove(&a->newage, i);
            if (ejected && chip == d && !lost) mc_media_eject(media, MC_MEDIA_FLOPPY, i);
        }
    }
    int gone = 0;
    mc_disk *cd = mc_media_get(media, MC_MEDIA_CDROM, 0, &gone);
    if (a->vdisk[CD_ID] && (!cd || gone)) {
        mc_vdisk *old = mc_scsi_unit_remove(&a->unit[CD_ID]);
        if (old) mc_vdisk_destroy(old);
        a->vdisk[CD_ID] = NULL;
    }
}

/* --- Ethernet: the MACE through the PSC's ENetRd and ENetWr ------------------------ */

static void enet_dma_frame(void *ctx, const uint8_t *data, size_t length) {
    av *a = ctx;
    mc_am79c940_transmit(&a->enet, data, length);
}
static int enet_dma_receive(void *ctx, const uint8_t *frame, size_t length, const uint8_t status[4]) {
    av *a = ctx;
    return mc_psc_enet_receive(&a->psc, frame, length, status);
}
static void enet_transmit(void *ctx, const uint8_t *frame, size_t length) {
    av *a = ctx;
    if (a->net.transmit) a->net.transmit(a->net.ctx, frame, length);
}
static void board_net_host(void *board, const mc_net_host *host) {
    av *a = board;
    if (host) a->net = *host;
    else memset(&a->net, 0, sizeof a->net);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    av *a = board;
    return mc_am79c940_receive(&a->enet, frame, length);
}

/* --- sound --------------------------------------------------------------------- */

static uint32_t board_audio_rate(void *board) {
    av *a = board;
    return a->hw->config.sound ? MC_PSC_SOUND_RATE : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    av *a = board;
    const size_t n = mc_psc_pull(&a->psc, stereo, frames);
    if (!a->hw->config.startup_sound && !a->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (a->psc.played && !a->psc.snd_left && !a->psc.ring_count) a->chime_done = 1;
    }
    return n;
}

/* The sound input: Singer's, recorded through the PSC's DSP frames. */
static void board_audio_source(void *board, const mc_audio_source *source) {
    mc_psc_set_audio_source(&((av *)board)->psc, source);
}

/* --- serial ports -------------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    av *a = ctx;
    if (a->serial.transmit) a->serial.transmit(a->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    av *a = ctx;
    if (a->serial.outputs) a->serial.outputs(a->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    av *a = board;
    if (host) a->serial = *host;
    else memset(&a->serial, 0, sizeof a->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    av *a = board;
    return port < 2 ? mc_scc_receive(&a->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    av *a = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&a->scc, port_channel(port), dcd);
    mc_scc_set_cts(&a->scc, port_channel(port), cts);
}

/* --- SCSI ------------------------------------------------------------------------ */

/* The controller's registers every $10 from $50F1 8000. */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, offset >> 4, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, offset >> 4, size); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};

/* The data port at $50F1 8100: each access is DACK cycles. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    av *a = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&a->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    av *a = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&a->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* The PSC's SCSI channel: DACK cycles of the controller. */
static int scsi_dma_read(void *ctx, uint8_t *byte) {
    av *a = ctx;
    uint32_t v = 0;
    if (!mc_ncr53c96_dma_read(&a->scsi, 1, &v)) return 0;
    *byte = (uint8_t)v;
    return 1;
}
static int scsi_dma_write(void *ctx, uint8_t byte) { return mc_ncr53c96_dma_write(&((av *)ctx)->scsi, 1, byte) == 1; }

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    av *a = board;
    mc_via_ops.reset(&a->via1);
    mc_psc_ops.reset(&a->psc);
    mc_civic_ops.reset(&a->civic);
    mc_newage_ops.reset(&a->newage);
    mc_am79c940_ops.reset(&a->enet);
    if (a->card_slot) mc_livadix_card_ops.reset(&a->card);
    mc_ncr53c96_reset(&a->scsi);
    mc_scc_ops.reset(&a->scc);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    av *a = board;
    if (a->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&a->card, &cv)) return 0;
        fb->width = cv.width;
        fb->height = cv.height;
        fb->depth = cv.depth;
        fb->row_bytes = cv.row_bytes;
        fb->pixels = cv.pixels;
        fb->palette = cv.palette;
    } else {
        mc_civic_view v;
        if (!mc_civic_view_get(&a->civic, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
    }
    fb->serial = ++a->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    av *a = board;
    if (code == 0x7F) mc_adbctl_power_key(&a->cuda, down);
    if (!mc_adb_keyboard_key(&a->keyboard, code, down)) return 0;
    mc_adbctl_poke(&a->cuda);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    av *a = board;
    mc_adb_mouse_move(&a->mouse, dx, dy, button);
    mc_adbctl_poke(&a->cuda);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    av *a = board;
    a->host_start = host_seconds;
    mc_adbctl_set_clock(&a->cuda, host_seconds);
}

static void board_destroy(void *board) {
    av *a = board;
    if (a->nvram[0]) mc_adbctl_nvram_save(&a->cuda, a->nvram, a->host_start);
    for (int id = 0; id < 7; id++) {
        if (!a->vdisk[id]) continue;
        mc_scsi_bus_attach(&a->scsi_bus, id, NULL);
        mc_vdisk_destroy(a->vdisk[id]);
    }
    mc_scsi_bus_free(&a->scsi_bus);
    mc_civic_free(&a->civic);
    mc_psc_free(&a->psc);
    mc_livadix_card_free(&a->card);
    free(a->cuda.mcu);
    free(a);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static uint32_t cpuid_read(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    const uint32_t v = CPU_ID;
    if (size == 4) return v;
    if (size == 2) return (offset & 2u) ? v & 0xFFFFu : v >> 16;
    return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
}
static void cpuid_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev; (void)offset; (void)size; (void)value;
}
static const mc_dev_ops cpuid_ops = {cpuid_read, cpuid_write, NULL, NULL};

static void *build(mc_hw *hw) {
    av *a = calloc(1, sizeof *a);
    if (!a) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    a->hw = hw;
    a->board = board_of(hw->model);
    hw->board = a;
    uint32_t banks[MC_MCA_BANKS];
    if (!mc_mca_layout(hw->config.ram_mb, a->board->onboard_mb, a->board->simm_banks, banks)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: %u MiB does not fit the %s's banks",
                 (unsigned)hw->config.ram_mb, hw->model->name);
        free(a);
        return NULL;
    }
    const int io = mc_hw_region(hw, "io");
    /* I/O bus cycles take 0.5 us of guest time, as on the Quadra 700. */
    hw->bus.region[io].cost = 8;
    hw->bus.region[io].unclaimed_ok = 1;
    mc_bus_attach(&hw->bus, mc_hw_region(hw, "cpuid"), 0xFFFC, 4, a, &cpuid_ops, 0);
    mc_via_init(&a->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, 0xF00000u, 0x2000, a, &via1_ops, MC_WIN_PORT8);
    mc_mca_init(&a->mca, &hw->host, "MCA", a->board->id, banks, mca_remap, a);
    mc_bus_attach(&hw->bus, io, 0xF30000u, MC_MCA_SIZE, &a->mca, &mc_mca_ops, 0);
    a->dsp_dev = (mc_dev){&dsp_ops, &hw->host, "DSP3210"};
    {
        const mc_dsp3210_bus dsp_bus = {a, dsp_bus_read, dsp_bus_write, dsp_bio, dsp_iack, NULL};
        mc_dsp3210_init(&a->dsp, &dsp_bus, hw->bus.read_map, hw->bus.write_map);
    }
    mc_psc_init(&a->psc, &hw->host, "PSC");
    mc_bus_attach(&hw->bus, io, 0xF31000u, MC_PSC_SIZE, &a->psc, &mc_psc_ops, 0);
    mc_bus_attach(&hw->bus, io, 0xF02000u, MC_PSC_VIA2_SIZE, &a->psc, &mc_psc_via2_ops, MC_WIN_PORT8);
    /* The SCC: B control +0, A control +2, B data +4, A data +6 from
     * $50F0 4000. */
    mc_scc_init(&a->scc, &hw->host);
    mc_scc_set_clocks(&a->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {a, scc_transmit, scc_outputs};
    mc_scc_set_port(&a->scc, &port);
    mc_bus_attach(&hw->bus, io, 0xF04000u, 0x2000, &a->scc, &mc_scc_ops, MC_WIN_PORT8);
    /* SCSI: the controller at C25M (Table 2-2). */
    mc_scsi_bus_init(&a->scsi_bus);
    mc_ncr53c96_init(&a->scsi, &hw->host, &a->scsi_bus, 25000000u);
    mc_bus_attach(&hw->bus, io, 0xF18000u, 0x0100, &a->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0xF18100u, 0x0100, a, &pdma_ops, 0);
    {
        const mc_psc_port scsi_port = {a, scsi_dma_read, scsi_dma_write};
        mc_psc_set_port(&a->psc, MC_PSC_CH_SCSI, &scsi_port);
    }
    mc_scsi_cd_init(&a->unit[CD_ID]);
    mc_scsi_unit_on_eject(&a->unit[CD_ID], cd_ejected, a);
    mc_scsi_bus_attach(&a->scsi_bus, CD_ID, mc_scsi_unit_target(&a->unit[CD_ID]));
    /* The floppy controller. */
    /* Ethernet: the MACE, its address PROM, its DMA. */
    mc_am79c940_init(&a->enet, &hw->host, "MACE");
    {
        const mc_am79c940_port enet_port = {a, enet_transmit, enet_dma_receive};
        mc_am79c940_set_port(&a->enet, &enet_port);
        const mc_psc_port wr = {a, NULL, NULL, enet_dma_frame};
        mc_psc_set_port(&a->psc, MC_PSC_CH_ENET_WR, &wr);
    }
    mc_bus_attach(&hw->bus, io, 0xF1C000u, MC_AM79C940_SIZE, &a->enet, &mc_am79c940_ops, MC_WIN_PORT8);
    mc_sonic_prom_init(&a->enet_prom, &hw->host, mc_sonic_default_address, 4, 1);
    mc_bus_attach(&hw->bus, io, 0xF08000u, 0x1000, &a->enet_prom, &mc_sonic_prom_ops, MC_WIN_PORT8);
    mc_newage_init(&a->newage, &hw->host, "New Age");
    mc_newage_fit(&a->newage, 1);       /* the internal SuperDrive */
    mc_bus_attach(&hw->bus, io, 0xF2A000u, MC_NEWAGE_SIZE, &a->newage, &mc_newage_ops, 0);
    {
        const mc_psc_port fdc_port = {&a->newage, mc_newage_dma_read, mc_newage_dma_write};
        mc_psc_set_port(&a->psc, MC_PSC_CH_FDC, &fdc_port);
    }
    /* Video: Civic's registers, Sebastian, the dot clock, VRAM. */
    mc_civic_monitor monitor = mc_civic_monitor_for(hw->config.width, hw->config.height);
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > mc_civic_max_depth(monitor, a->board->vram);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX || monitor == MC_CIVIC_NONE || mc_civic_max_depth(monitor, a->board->vram) == 0 ||
                     (over && hw->config.video_card == MC_VIDEO_CARD_AUTO);
    if (!card && over) hw->config.depth = mc_civic_max_depth(monitor, a->board->vram);
    if (card) monitor = MC_CIVIC_NONE;
    if (!mc_civic_init(&a->civic, &hw->host, "Civic", a->board->vram, monitor) || !a->psc.ring) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        mc_civic_free(&a->civic);
        mc_psc_free(&a->psc);
        free(a);
        return NULL;
    }
    mc_bus_attach(&hw->bus, io, 0x036000u, MC_CIVIC_SIZE, &a->civic, &mc_civic_ops, 0);
    mc_bus_attach(&hw->bus, io, 0xF30800u, MC_CIVIC_SEBASTIAN_SIZE, &a->civic, &mc_civic_sebastian_ops, 0);
    mc_bus_attach(&hw->bus, io, 0xF2E000u, MC_CIVIC_CLOCK_SIZE, &a->civic, &mc_civic_clock_ops, 0);
    mc_bus_map_float(&hw->bus, VRAM_BASE, MC_CIVIC_VRAM_WINDOW);
    mc_bus_map_memory(&hw->bus, VRAM_BASE, a->board->vram, a->civic.vram, a->board->vram, 1);
    /* NuBus: the board's slots; their interrupts on VIA2 port A (slot s on
     * bit s - 9), and Civic's VBL on bit 6, which the ROM's slot interrupt
     * dispatcher takes for slot 0 (ROM offset $155A8: port A bits 3-6, its
     * table at $1566E: bit 6 is slot 0), where the video driver installs
     * its VBL handler. */
    mc_nubus_init(&a->nubus, &hw->bus, a->board->slots, slot_pins, a);
    for (unsigned s = 0xC; s <= 0xE; s++)
        if (a->board->slots & 1u << (s - 8)) mc_nubus_line(&a->nubus, s, s - 9);
    mc_nubus_line(&a->nubus, 0x0, 6);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        a->card_slot = mc_nubus_free_slot(&a->nubus);
        if (mode < 0 || !a->card_slot ||
            !mc_livadix_card_init(&a->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&a->card, &a->nubus, a->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_civic_free(&a->civic);
            mc_psc_free(&a->psc);
            mc_livadix_card_free(&a->card);
            free(a);
            return NULL;
        }
    }
    /* ADB, the clock, PRAM and power: Cuda 341S0788 (docs/NOTES.md). */
    mc_adb_bus_init(&a->adb);
    mc_adb_keyboard_init(&a->keyboard);
    mc_adb_mouse_init(&a->mouse);
    mc_adb_attach(&a->adb, &a->keyboard.dev);
    mc_adb_attach(&a->adb, &a->mouse.dev);
    mc_adbctl_init(&a->cuda, &hw->host, "Cuda", MC_ADBCTL_CUDA, &a->via1, &a->adb);
    mc_adbctl_set_part(&a->cuda, "341S0788");
    if (mc_hw_nvram_path(hw, a->nvram, sizeof a->nvram) == NULL) a->nvram[0] = 0;
    load_nvram(a);
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        const int level = mc_adbctl_select(&a->cuda, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys);
        if (level < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the Cuda firmware (%s) is not in the roms folders",
                     keys[0] ? keys[0] : "?");
            mc_civic_free(&a->civic);
            mc_psc_free(&a->psc);
            mc_livadix_card_free(&a->card);
            free(a);
            return NULL;
        }
    }
    /* The power key that starts the machine stays held through Cuda's
     * power-up, as on the LC 475 (quadra605.c). */
    a->cuda.key_hold = (uint64_t)MC_C15M_HZ * 3u / 2u;
    uint8_t *pram = mc_adbctl_pram(&a->cuda);
    set_depth(a, pram);
    if (a->card_slot) set_card_depth(a, pram);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    /* VIA1 port A with vSCCWrReq (PA7) high, unconnected inputs high. */
    mc_via_set_pa(&a->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&a->via1, 0xF7u, 0xF7u);
    a->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, a->overlay_region, 0, 0x10000000u, a, &trap_ops, 0);
    overlay_on(a);
    mc_adbctl_ops.reset(&a->cuda);
    cuda_hold(a);
    return a;
}

static const mc_board_ops av_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
    .audio_source = board_audio_source,
};

static const char *const q840_models[] = {"quadra-840av", NULL};
static const char *const q660_models[] = {"quadra-660av", NULL};
static const uint32_t av_roms[] = {0x5BF10FD1u, 0};
/* RAM (Developer Note, "Random-Access Memory"): the 840AV's four SIMM
 * slots, eight banks of up to 16 MB, 128 MB at most; the 660AV's 4 MB on
 * the board and two SIMM slots, 68 MB at most. */
static const uint16_t q840_ram[] = {8, 12, 16, 20, 24, 32, 36, 40, 48, 64, 72, 96, 128, 0};
static const uint16_t q660_ram[] = {4, 8, 12, 16, 20, 24, 36, 40, 68, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    const mc_civic_monitor monitor = mc_civic_monitor_for(w, h);
    return monitor == MC_CIVIC_NONE ? 0 : MC_DEPTHS_UPTO(mc_civic_max_depth(monitor, board_of(model)->vram));
}
static const mc_hw_caps av_caps = {video_builtin, 1, MC_FEATURE_ENET, NULL};

const mc_hw_desc mc_quadra840av_desc = {
    "quadra840av", q840_models, M68K_68040, M68K_FPU_68040, av_roms, q840_ram,
    av_map, sizeof av_map / sizeof *av_map, av_irqs, sizeof av_irqs / sizeof *av_irqs,
    build, &av_ops, MC_MMU_040, NULL, &av_caps,
};

const mc_hw_desc mc_quadra660av_desc = {
    "quadra660av", q660_models, M68K_68040, M68K_FPU_68040, av_roms, q660_ram,
    av_map, sizeof av_map / sizeof *av_map, av_irqs, sizeof av_irqs / sizeof *av_irqs,
    build, &av_ops, MC_MMU_040, NULL, &av_caps,
};
