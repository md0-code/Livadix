/* Livadix — the LC family boards: Macintosh LC and LC II (V8), Classic II
 * (Eagle), Color Classic (Spice) and Macintosh TV.
 *
 * Sources: Macintosh Color Classic Developer Note (Apple, 1993; the
 * architecture "based on the design of the Macintosh LC II"): chapter 1,
 * "RAM Expansion" and Figure 1-3 (4 MB built in, two SIMMs of 1, 2 or 4
 * MB, at most 10 MB), "Video RAM" (256 KB, 512 KB with the VRAM SIMM),
 * "PDS Expansion Slot", "Power On and Off" (the keyboard's power key, the
 * master switch), Table 1-6 (reset and NMI keys); chapter 2, "Block
 * Diagram" (MC68030 at C15M, Spice, the Combo chip's SCC and 53C80 with
 * the LC II's SCSI and serial ports, Cuda, DFAC2), "Address Maps", "RAM
 * Addresses", "Video RAM" (v8.h); chapter 3 (the expansion slot,
 * /SLOTIRQ a level-2 interrupt); chapter 4 ("Machine Identification",
 * Gestalt 49, the one-second interrupt from the ADB microcontroller).
 * Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (VIA1, as on
 * the IIci: iici.c). Macintosh IIsi Developer Note for Egret (egret.h).
 *
 * Observed from the ROMs (LC 350EACF0, LC II
 * 35C28F5F, Classic II 3193670E, Color Classic ECD99DC0): the ProductInfo
 * records match VIA1 PA6/4/2/1 (mask $56) against $54 (LC, also the LC
 * II, ROM offset $3BA4), $12 (Classic II, $3D14) and $02 (Color Classic,
 * $3858), with decoder kind 7 (v8.h); their RAM records ($3C2A: chunks of
 * 1 MB, the banks $80 0000-$A0 0000 and $0-$80 0000) and video records
 * (VRAM $50F4 0000; the Classic II's screen at $9F 9A80); the LC's ROM
 * runs at $00A0 xxxx from reset and the overlay ends at its first access
 * there; the ADB controller on VIA1 PB3-PB5 and the shift register (Egret
 * 341S0850 on the LC and LC II, 341S0851 on the Classic II: docs/NOTES.md;
 * Cuda 341S0417 on the Color Classic).
 *
 * Observed from the Macintosh TV's ROM EAF1678D (docs/NOTES.md), of the Color Classic's line: its decoder test of
 * kind 18 ($3134) takes the V8 family's map (the record at $D0DF0, as
 * kind 7's) when VIA1 does not repeat at +$40000, and its one record of
 * that kind ($38A0, box flag 82, Gestalt 88) matches VIA1 PA6/4/2/1 =
 * $04; its RAM record ($3B92) sizes a SIMM bank at 0 and the board's 4 MB
 * after it, both up to 4 MB, and its sizing writes the SIMM's size to the
 * RAM configuration's bits 7-6 by the table at $A042C: 00 none, 01 1 MB,
 * 11 4 MB (10, 2 MB, marked invalid); its video record ($3C62) is the
 * LC's (VRAM at $50F4 0000), the built-in 640 x 480 screen answering as
 * the 13" RGB monitor (sense code 6)†, 8 bits from 512 KB. Cuda 341S0789
 * (docs/NOTES.md); the TV has no PDS slot for cards.
 *
 * Livadix's own code. */
#include "../hw_machine.h"
#include "../adb.h"
#include "../adbctl.h"
#include "../asc.h"
#include "../enet_card.h"
#include "../livadix_card.h"
#include "../ncr5380.h"
#include "../nubus.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../asic/v8.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IO_BASE 0x50F00000u
#define IO_BLOCK 0x40000u               /* the devices from $50F0 0000 (v8.h) */
#define ROM_AREA 0x00A00000u            /* the ROM's 4 MB, repeated (v8.h) */
#define VRAM_SIZE (512u << 10)          /* with the VRAM SIMM ("Video RAM") */
#define CARD_SLOT 0xEu                  /* the PDS card answers as slot $E */
#define MIRROR(k) ((uint32_t)(k) << 28) /* A30-A28 ignored (v8.h) */

/* The boards. VIA1 port A: vSCCWrReq (PA7) high, the ID on PA6/4/2/1
 * (v8.h; the TV's, above), PA0 high. The TV's ASIC taken as Spice (its
 * SWIM2, the DFAC2 on Cuda's IIC bus)†. */
typedef struct lc_board {
    const char *model;
    mc_v8_kind asic;
    uint8_t builtin_mb;         /* RAM on the board */
    uint8_t via1_pa;
    mc_adbctl_kind controller;
    const char *part;
    uint8_t soft_power;         /* the controller switches the supply off (Color Classic) */
    uint8_t slot;               /* a PDS expansion slot */
    uint8_t screen;             /* the built-in screen's sense code (0: a monitor port, the V8) */
    uint8_t simm_mb[4];         /* the SIMMs' bank of each RAM configuration code (v8.h) */
} lc_board;

static const lc_board boards[] = {
    {"lc", MC_V8_V8, 2, 0xD5, MC_ADBCTL_EGRET, "341S0850", 0, 1, 0, {0, 2, 4, 8}},
    {"lc-ii", MC_V8_V8, 4, 0xD5, MC_ADBCTL_EGRET, "341S0850", 0, 1, 0, {0, 2, 4, 8}},
    {"classic-ii", MC_V8_EAGLE, 2, 0x93, MC_ADBCTL_EGRET, "341S0851", 0, 0, MC_RBV_SENSE_RGB12, {0, 2, 4, 8}},
    {"color-classic", MC_V8_SPICE, 4, 0x83, MC_ADBCTL_CUDA, "341S0417", 1, 1, MC_RBV_SENSE_RGB12, {0, 2, 4, 8}},
    {"macintosh-tv", MC_V8_SPICE, 4, 0x85, MC_ADBCTL_CUDA, "341S0789", 1, 0, MC_RBV_SENSE_HIRES, {0, 1, 2, 4}},
};

static const lc_board *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct lc {
    mc_hw *hw;
    const lc_board *board;
    mc_via via1;
    mc_v8 v8;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card in the PDS slot, when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    uint8_t *vram;
    uint32_t simm;              /* the SIMMs' bytes (hw->ram first), the built-in RAM after them */
    uint8_t *builtin;           /* the built-in RAM's own bytes when SIMMs hide part of it (map_ram) */
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adbctl ctl;              /* Egret or Cuda */
    int powered;
    uint64_t fb_serial;
    int overlay;
    int overlay_region;
    char nvram[1024];
    int64_t host_start;
} lc;

/* The I/O block at $50F0 0000 (mirrored, v8.h). */
static const mc_region lc_map[] = {
    {"io", IO_BASE, IO_BLOCK, IO_BLOCK - 1u, MC_REGION_IO},
};

/* The levels of the Macintosh II family: the V8's VIA2
 * part on level 2 as the RBV's. */
static const mc_irq_route lc_irqs[] = {
    {"via1", 1}, {"v8", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- RAM and the reset overlay --------------------------------------------------- */

/* The RAM area of each mirror: the SIMMs' bank (repeating the SIMMs) from
 * 0, the built-in RAM after it up to 8 MB, the built-in RAM's top 2 MB at
 * $80 0000 (v8.h). The SIMMs' and built-in bytes are hw->ram's, but for 8
 * MB of SIMMs over 4 MB built in: emulation.ram_mb counts the 2 MB of it
 * still seen, and the whole 4 MB, which the ROM sizes before it places the
 * SIMMs (LC II ROM 35C28F5F, $40A4 A5D2), are the board's own (builtin).
 * Nothing answers elsewhere: the cycle reads 0†. The TV the same, with
 * its SIMM bank of 0, 1 or 4 MB (its ROM's RAM record ends at 8 MB; the
 * top 2 MB at $80 0000 kept as the family's)†. */
static void map_ram(lc *m) {
    mc_hw *hw = m->hw;
    const uint32_t bi = (uint32_t)m->board->builtin_mb << 20;
    const uint32_t kept = m->builtin ? bi : hw->ram_size - m->simm;
    uint8_t *bram = m->builtin ? m->builtin : hw->ram + m->simm;
    const uint32_t bank = mc_v8_simm_bank(&m->v8);
    for (unsigned k = 0; k < 8; k++) {
        const uint32_t b = MIRROR(k);
        mc_bus_map_float(&hw->bus, b, ROM_AREA);
        if (bank && m->simm) mc_bus_map_memory(&hw->bus, b, bank, hw->ram, m->simm, 1);
        if (bank < 0x800000u) {
            const uint32_t span = bi < 0x800000u - bank ? bi : 0x800000u - bank;
            mc_bus_map_memory(&hw->bus, b + bank, span, bram, kept, 1);
        }
        mc_bus_map_memory(&hw->bus, b + 0x800000u, 0x200000u, bram + kept - 0x200000u, 0x200000u, 1);
    }
    /* The Classic II's screen also at $FEE0 9A80 (its video record's
     * 32-bit base, ROM offset $3D54): the RAM's top 64 KB, from $9F 0000,
     * at $FEE0 0000†; the ROM's memory test reaches it there. */
    if (m->board->asic == MC_V8_EAGLE)
        mc_bus_map_memory(&hw->bus, 0xFEE00000u, 0x10000u, bram + kept - 0x10000u, 0x10000u, 1);
    mc_hw_maps_changed(hw);
}

static void remap(void *ctx) {
    lc *m = ctx;
    if (!m->overlay) map_ram(m);
}

/* From reset the ROM answers in the RAM area too, until the first access
 * to its own area (the ROM's code from $00A0 0000). */
static void overlay_on(lc *m) {
    mc_hw *hw = m->hw;
    m->overlay = 1;
    for (unsigned k = 0; k < 8; k++) {
        mc_bus_map_memory(&hw->bus, MIRROR(k), ROM_AREA, hw->rom, hw->rom_size, 0);
        mc_bus_remap(&hw->bus, m->overlay_region, MIRROR(k) + ROM_AREA, 0x400000u);
    }
    mc_hw_maps_changed(hw);
}

static void overlay_off(lc *m) {
    mc_hw *hw = m->hw;
    m->overlay = 0;
    for (unsigned k = 0; k < 8; k++) mc_bus_map_memory(&hw->bus, MIRROR(k) + ROM_AREA, 0x400000u, hw->rom, hw->rom_size, 0);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    lc *m = dev;
    if (m->overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    lc *m = dev;
    if (m->overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- VIA1 and the ADB controller ------------------------------------------------------ */

static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    lc *m = dev;
    mc_adbctl_sync(&m->ctl);
    return mc_via_ops.read(&m->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lc *m = dev;
    mc_adbctl_sync(&m->ctl);
    mc_via_ops.write(&m->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    lc *m = dev;
    return !m->ctl.firmware && mc_via_ops.stable && mc_via_ops.stable(&m->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* The processor runs while the controller releases /RESET and, on the
 * Color Classic, powers the system (the others have a power switch: the
 * power is on); a reset brings the overlay back. */
static void ctl_hold(lc *m) {
    /* Without soft power the controller's power-down (its firmware drives
     * the supply off and holds /RESET, Egret 341S0851 at Shut Down) does
     * not reach the processor: the switch keeps the supply on, and the
     * System ends at its final screen. */
    const int power = m->ctl.line_state[MC_ADBCTL_LINE_POWER];
    const int held = m->board->soft_power ? m->ctl.line_state[MC_ADBCTL_LINE_RESET] || !power
                                          : m->ctl.line_state[MC_ADBCTL_LINE_RESET] && (power || !m->powered);
    if (held && !m->overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* --- wiring ------------------------------------------------------------------------ */

static void board_line(void *board, void *dev, int line, int state) {
    lc *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: Egret's or Cuda's lines (egret.h, cuda.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&m->ctl, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-6). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
    } else if (dev == &m->v8.rbv) {
        if (line == MC_RBV_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "v8"), state);
        else if (line == MC_RBV_LINE_TICK) mc_via_set_ca1(&m->via1, state);
        /* Port B bit 3 selects the LC's decode: clear 24-bit, set 32-bit
         * (its _SwapMMUMode, ROM 350EACF0 at $40A0 3F42 and $40A0 3F5A, as
         * the Macintosh II's VIA2 PB3). */
        else if (line == MC_RBV_LINE_PB && hw->desc->map24 && hw->desc->mmu == MC_MMU_AMU) mc_hw_set_addr24(hw, !(state & 0x08));
    } else if (dev == &m->ctl) {
        /* The Color Classic's Cuda switches the supply off at Shut Down
         * ("Power On and Off"); the others have a power switch and their
         * controller keeps the power. */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) m->powered = 1;
                else if (m->powered && m->board->soft_power) mc_hw_power_off(hw);
            }
            ctl_hold(m);
        }
    } else if (dev == &m->scsi) {
        /* The 53C80's IRQ and DRQ in the VIA2 flags, as on the IIci. */
        if (line == MC_NCR5380_LINE_IRQ) mc_rbv_set_irq(&m->v8.rbv, MC_RBV_IRQ_SCSI, state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_rbv_set_irq(&m->v8.rbv, MC_RBV_IRQ_SCSI_DRQ, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_rbv_set_irq(&m->v8.rbv, MC_RBV_IRQ_ASC, state);
    } else if (dev == &m->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&m->nubus, m->card_slot, state);
    }
}

/* /SLOTIRQ: the slot flags' bit for slot $E (bit 5, as the IIci's slot $E,
 * rbv.h)†, active low there. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    lc *m = ctx;
    (void)slotirq_high;
    const uint8_t lines = (uint8_t)(~levels & mask);
    mc_rbv_set_slots(&m->v8.rbv, (uint8_t)((m->v8.rbv.slots & 0x40u) | lines));
}

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM at extended PRAM $56-$5D
 * (board ID $0029, the depth's mode $80 + log2(depth), the monitor's
 * sResource $A0 + sense code, the sense code), as the LC and LC II ROMs
 * write it at their first start (the Color Classic's too, its display as
 * the 12" RGB monitor). A PRAM the ROM has not set up for this
 * monitor is left to it (the first start is in 1 bit). */
static void set_depth(lc *m, uint8_t *pram) {
    const mc_config *c = &m->hw->config;
    uint8_t *p = pram + 0x56;
    const uint8_t sense = m->v8.rbv.sense;
    if (m->card_slot || m->board->asic == MC_V8_EAGLE || pram[0x10] != 0xA8 || p[0] != 0x00 || p[1] != 0x29 ||
        p[3] != (0xA0u | sense))
        return;
    if (c->depth > mc_v8_max_depth(m->board->asic, sense, VRAM_SIZE)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

/* The Livadix Display Card's depth from video.depth, through its slot PRAM
 * (iici.c, set_card_depth). */
static void set_card_depth(lc *m, uint8_t *pram) {
    const mc_config *c = &m->hw->config;
    uint8_t *p = pram + 0x46 + 8 * (m->card_slot - 9);
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

static void drop_disks(lc *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as the IIci lays them out (iici.c). */
static int layout_disks(lc *m, char *error, size_t capacity) {
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
    lc *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    lc *m = board;
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
    lc *m = board;
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
    lc *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    lc *m = board;
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
    lc *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    lc *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    lc *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    lc *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    lc *m = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&m->scc, port_channel(port), dcd);
    mc_scc_set_cts(&m->scc, port_channel(port), cts);
}

/* --- SCSI ------------------------------------------------------------------------ */

/* The 5380's registers every $10 bytes (register number in A6-A4). */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr5380_ops.read(dev, (offset >> 4) & 7u, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr5380_ops.write(dev, (offset >> 4) & 7u, size, value);
}
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

/* The pseudo-DMA windows: each CPU access is DACK cycles of the 5380 (the
 * MDU's handshake holds the cycle until DRQ; Livadix's 5380 moves the byte at
 * once). */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    lc *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lc *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    lc *m = board;
    mc_via_ops.reset(&m->via1);
    mc_v8_ops.reset(&m->v8);
    if (m->card_slot) mc_livadix_card_ops.reset(&m->card);
    mc_enet_card_reset(m->enet);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
    if (m->hw->desc->map24) mc_hw_set_addr24(m->hw, 1);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    lc *m = board;
    if (m->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&m->card, &cv)) return 0;
        fb->width = cv.width;
        fb->height = cv.height;
        fb->depth = cv.depth;
        fb->row_bytes = cv.row_bytes;
        fb->pixels = cv.pixels;
        fb->palette = cv.palette;
    } else {
        /* The Classic II's screen in the built-in RAM's top 2 MB (v8.h). */
        const uint32_t kept = m->builtin ? (uint32_t)m->board->builtin_mb << 20 : m->hw->ram_size - m->simm;
        const uint8_t *base = m->builtin ? m->builtin : m->hw->ram + m->simm;
        const uint8_t *screen = base + kept - (0x00A00000u - MC_V8_CLASSIC_SCREEN);
        mc_v8_view v;
        if (!mc_v8_view_get(&m->v8, m->vram, VRAM_SIZE, screen, &v)) return 0;
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
    lc *m = board;
    /* The power key also grounds the ADB connector's power-on pin, the
     * controller's power-key input (Color Classic Developer Note, Table
     * 1-5). */
    if (code == 0x7F) mc_adbctl_power_key(&m->ctl, down);
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adbctl_poke(&m->ctl);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    lc *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adbctl_poke(&m->ctl);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    lc *m = board;
    m->host_start = host_seconds;
    mc_adbctl_set_clock(&m->ctl, host_seconds);
}

static void board_destroy(void *board) {
    lc *m = board;
    if (m->nvram[0]) mc_adbctl_nvram_save(&m->ctl, m->nvram, m->host_start);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    mc_scsi_bus_free(&m->scsi_bus);
    mc_livadix_card_free(&m->card);
    mc_enet_card_free(m->enet);
    free(m->vram);
    free(m->builtin);
    free(m->ctl.mcu);
    free(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

/* The SIMMs for emulation.ram_mb: two equal SIMMs of 1, 2 or 4 MB (Color
 * Classic Developer Note, "RAM SIMM"), 8 MB of them hiding the lower 2 MB
 * of 4 built in (Figure 1-3). */
static int simm_bytes(const lc_board *b, uint32_t ram_mb, uint32_t *simm) {
    for (size_t i = 0; i < 4; i++) {
        const uint32_t s = b->simm_mb[i];
        const uint32_t total = s == 8 ? 10u : s + b->builtin_mb;
        if (total == ram_mb) {
            *simm = s << 20;
            return 1;
        }
    }
    return 0;
}

static void *build(mc_hw *hw) {
    const lc_board *b = board_of(hw->model);
    uint32_t simm = 0;
    if (!simm_bytes(b, hw->config.ram_mb, &simm)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: no %s RAM population gives %u MiB", hw->model->name,
                 (unsigned)hw->config.ram_mb);
        return NULL;
    }
    lc *m = calloc(1, sizeof *m);
    uint8_t *vram = calloc(1, VRAM_SIZE);
    if (!m || !vram) {
        free(m);
        free(vram);
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    m->board = b;
    m->simm = simm;
    m->vram = vram;
    if (simm + ((uint32_t)b->builtin_mb << 20) > hw->ram_size && !(m->builtin = calloc(1, (size_t)b->builtin_mb << 20))) {
        free(vram);
        free(m);
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    hw->board = m;
    const int io = mc_hw_region(hw, "io");
    /* Every I/O bus cycle takes 0.5 us of guest time, as on the IIci. */
    hw->bus.region[io].cost = 8;
    /* An address of the block no device takes completes the cycle and
     * reads 0†: the ROMs' power-on test looks for VIA1's IER repeated at
     * $50F2 1C00 and (on the Classic II, without VRAM) $50F4 1C00, and
     * stops with a failure when that cycle bus-errors (ROM $40A0 3124). */
    hw->bus.region[io].unclaimed_ok = 1;
    /* The I/O block (v8.h). */
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, m, &via1_ops, MC_WIN_PORT8);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x04000, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 0);
    mc_bus_attach(&hw->bus, io, 0x10000, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x06000, 0x2000, m, &pdma_ops, 0);
    mc_bus_attach(&hw->bus, io, 0x12000, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ORIGINAL);
    m->asc.input = 1;
    mc_bus_attach(&hw->bus, io, 0x14000, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    /* Spice holds a SWIM2 ("Spice Custom IC"). */
    if (b->asic == MC_V8_SPICE) mc_swim2_init(&m->swim, &hw->host, hw->media.floppy_drives);
    else mc_swim_init(&m->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x16000, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* The built-in video: the 12" or 13" RGB monitor video.resolution
     * names on the LC and LC II, the Color Classic's 512 x 384 display, the
     * Classic II's 512 x 342 one; the Livadix Display Card in the PDS slot
     * when the built-in video cannot show the screen (the V8 then senses no
     * monitor). */
    uint8_t sense = mc_rbv_sense_for(hw->config.width, hw->config.height);
    if (sense != MC_RBV_SENSE_RGB12 && sense != MC_RBV_SENSE_HIRES) sense = MC_RBV_SENSE_NONE;
    /* The built-in displays: their frame and vertical blanking as the 12"
     * RGB monitor's, 60.15 Hz from C15M (Color Classic Developer Note,
     * Table 2-1; the Classic II's 9-inch screen at the classic Macintosh's
     * rate)†; the TV's 640 x 480 screen as the 13" monitor's†. */
    if (b->screen) sense = b->screen;
    int fits;
    if (b->asic == MC_V8_EAGLE) fits = hw->config.width == 512 && hw->config.height == 342 && hw->config.depth == 1;
    else if (b->screen) fits = sense == mc_rbv_sense_for(hw->config.width, hw->config.height);
    else fits = sense != MC_RBV_SENSE_NONE;
    /* video.card builtin (or no slot) with a depth the built-in video lacks:
     * its deepest one at that size; a size it lacks: the card when there is
     * a slot (no black screen). */
    const uint32_t deepest = fits ? mc_v8_max_depth(b->asic, sense, VRAM_SIZE) : 0;
    const int over = hw->config.depth > deepest;
    const int card = b->slot && (hw->config.video_card == MC_VIDEO_CARD_LIVADIX || !fits ||
                                 (over && hw->config.video_card == MC_VIDEO_CARD_AUTO));
    if (!card && over && deepest) hw->config.depth = deepest;
    if (card && b->asic == MC_V8_V8) sense = MC_RBV_SENSE_NONE;
    mc_v8_init(&m->v8, &hw->host, b->asic == MC_V8_EAGLE ? "Eagle" : b->asic == MC_V8_SPICE ? "Spice" : "V8", b->asic,
               sense, remap, m);
    mc_v8_set_simm_sizes(&m->v8, b->simm_mb);
    mc_bus_attach(&hw->bus, io, 0x26000, 0x2000, &m->v8, &mc_v8_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x24000, 0x2000, &m->v8, &mc_v8_vdac_ops, MC_WIN_PORT8);
    if (b->asic == MC_V8_EAGLE) mc_bus_attach(&hw->bus, io, 0x18000, 0x2000, &m->v8, &mc_v8_eagle_ops, MC_WIN_PORT8);
    for (unsigned k = 0; k < 8; k++) {
        if (MIRROR(k) + 0x00F00000u != IO_BASE) mc_bus_remap(&hw->bus, io, MIRROR(k) + 0x00F00000u, IO_BLOCK);
        if (b->asic != MC_V8_EAGLE) mc_bus_map_memory(&hw->bus, MIRROR(k) + MC_V8_VRAM, VRAM_SIZE, m->vram, VRAM_SIZE, 1);
        else mc_bus_map_float(&hw->bus, MIRROR(k) + MC_V8_VRAM, VRAM_SIZE);
        /* The rest of the I/O decode up to $50FF FFFF completes the cycle
         * and reads 0†: the Classic II ROM's sound set-up, for its box
         * flag 17 past the end of a 16-entry table (ROM 3193670E at
         * $40A4 3B4A), runs into a byte write at $50FF ABBA, which must
         * not bus-error. */
        mc_bus_map_float(&hw->bus, MIRROR(k) + 0x00FC0000u, 0x40000u);
    }
    /* The PDS slot as NuBus slot $E. */
    mc_nubus_init(&m->nubus, &hw->bus, b->slot ? (uint8_t)(1u << (CARD_SLOT - 8)) : 0, slot_pins, m);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        mc_nubus_line(&m->nubus, CARD_SLOT, 5);
        m->card_slot = mc_nubus_free_slot(&m->nubus);
        if (mode < 0 || !m->card_slot ||
            !mc_livadix_card_init(&m->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&m->card, &m->nubus, m->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_livadix_card_free(&m->card);
            free(m->vram);
            free(m);
            return NULL;
        }
    }
    /* The Ethernet card (network.card). */
    m->enet = mc_enet_card_fit(hw, &m->nubus, b->slot ? MC_ENET_CARD_LC : 0, mc_nubus_free_slot(&m->nubus));
    if (!m->enet && hw->error[0]) {
        mc_livadix_card_free(&m->card);
        free(m->vram);
        free(m);
        return NULL;
    }
    if (m->enet) mc_nubus_line(&m->nubus, CARD_SLOT, 5);
    /* ADB, the clock, PRAM and power: Egret or Cuda, with an Apple
     * Extended Keyboard and mouse. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_adbctl_init(&m->ctl, &hw->host, b->controller == MC_ADBCTL_CUDA ? "Cuda" : "Egret", b->controller, &m->via1, &m->adb);
    mc_adbctl_set_part(&m->ctl, b->part);
    /* Egret's model strap stays high (adbctl.h): with it low, firmware
     * 341S0850 never releases /RESET on the LC. The power switch keeps
     * these boards powered whatever the controller drives (ctl_hold). */
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&m->ctl, m->nvram)) {
        memset(mc_adbctl_pram(&m->ctl), 0, 256);
        mc_adbctl_set_seconds(&m->ctl, 0);
    }
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        if (mc_adbctl_select(&m->ctl, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys) < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the %s firmware (%s) is not in the roms folders",
                     b->controller == MC_ADBCTL_CUDA ? "Cuda" : "Egret", keys[0] ? keys[0] : "?");
            mc_livadix_card_free(&m->card);
            free(m->vram);
            free(m);
            return NULL;
        }
    }
    /* Cuda's firmware starts the machine with the power key held (quadra605.c). */
    if (b->controller == MC_ADBCTL_CUDA) m->ctl.key_hold = (uint64_t)MC_C15M_HZ * 3u / 2u;
    /* The DFAC2 on Cuda's IIC bus ("Sound Circuits", block diagram): it
     * acknowledges the ROM's settings ($22 transactions at start, ROM
     * ECD99DC0 at $40AD03E8, which reads a 3-byte reply and waits forever
     * when Cuda answers an IIC failure). */
    if (b->asic == MC_V8_SPICE) m->ctl.iic_device = 1;
    uint8_t *pram = mc_adbctl_pram(&m->ctl);
    set_depth(m, pram);
    if (m->card_slot) set_card_depth(m, pram);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    mc_via_set_pa(&m->via1, b->via1_pa, 0xFFu);
    mc_via_set_pb(&m->via1, 0xF7u, 0xF7u);
    /* The ROM and the overlay; the 68020 LC starts in the 24-bit decode. */
    m->overlay_region = mc_bus_add_io(&hw->bus, ROM_AREA, 0x400000u, 0x3FFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x400000u, m, &trap_ops, 0);
    overlay_on(m);
    if (hw->desc->map24) mc_hw_set_addr24(hw, 1);
    mc_adbctl_ops.reset(&m->ctl);
    ctl_hold(m);
    return m;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((lc *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((lc *)board)->enet, frame, length);
}

/* The microphone, recorded by the ASC of the V8 family (asc.c). */
static void board_audio_source(void *board, const mc_audio_source *source) {
    mc_asc_set_audio_source(&((lc *)board)->asc, source);
}

static const mc_board_ops lc_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
    .audio_source = board_audio_source,
};

/* RAM: the built-in RAM and the SIMM pairs (simm_bytes), from Livadix's
 * 4 MB minimum. */
static const uint16_t lc_ram[] = {4, 6, 10, 0};
static const uint16_t cc_ram[] = {4, 6, 8, 10, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    const lc_board *b = board_of(model);
    if (b->asic == MC_V8_EAGLE) return w == 512 && h == 342 ? MC_DEPTH_BIT(1) : 0;
    uint8_t sense = mc_rbv_sense_for(w, h);
    if (sense != MC_RBV_SENSE_RGB12 && sense != MC_RBV_SENSE_HIRES) sense = MC_RBV_SENSE_NONE;
    int fits = sense != MC_RBV_SENSE_NONE;
    if (b->screen) { fits = b->screen == mc_rbv_sense_for(w, h); sense = b->screen; }
    return fits ? MC_DEPTHS_UPTO(mc_v8_max_depth(b->asic, sense, VRAM_SIZE)) : 0;
}
static const mc_hw_caps lc_caps = {video_builtin, 1, MC_FEATURE_LC_SLOT, NULL};
static const mc_hw_caps slotless_caps = {video_builtin, 0, 0, NULL};

/* The Macintosh LC: a 68020 at 16 MHz (15.6672 MHz, C15M), no MMU, no
 * FPU; the V8's 24-bit decode (v8.h). */
static const char *const lc_models[] = {"lc", NULL};
static const uint32_t lc_roms[] = {0x350EACF0u, 0};
const mc_hw_desc mc_lc_desc = {
    "lc", lc_models, M68K_68020, M68K_FPU_NONE, lc_roms, lc_ram,
    lc_map, sizeof lc_map / sizeof *lc_map, lc_irqs, sizeof lc_irqs / sizeof *lc_irqs,
    build, &lc_ops, MC_MMU_AMU, mc_v8_map24, &lc_caps,
};

/* The LC II and Color Classic (68030 at 16 MHz, C15M) and the Classic II
 * (68030 at 16 MHz): the 68030's MMU, no FPU (a socket on the Classic II
 * and Color Classic, none fitted); the ASIC's 24-bit decode
 * from reset, as on the LC (the LC II's ROM leaves it on: with A31 and
 * A27-A24 ignored its 32-bit addresses decode the same). */
static const char *const lc2_models[] = {"lc-ii", NULL};
static const uint32_t lc2_roms[] = {0x35C28F5Fu, 0};
const mc_hw_desc mc_lc2_desc = {
    "lc2", lc2_models, M68K_68030, M68K_FPU_NONE, lc2_roms, cc_ram,
    lc_map, sizeof lc_map / sizeof *lc_map, lc_irqs, sizeof lc_irqs / sizeof *lc_irqs,
    build, &lc_ops, MC_MMU_030, mc_v8_map24, &lc_caps,
};

static const char *const classic2_models[] = {"classic-ii", NULL};
static const uint32_t classic2_roms[] = {0x3193670Eu, 0};
const mc_hw_desc mc_classic2_desc = {
    "classic2", classic2_models, M68K_68030, M68K_FPU_NONE, classic2_roms, lc_ram,
    lc_map, sizeof lc_map / sizeof *lc_map, lc_irqs, sizeof lc_irqs / sizeof *lc_irqs,
    build, &lc_ops, MC_MMU_030, NULL, &slotless_caps,
};

static const char *const cc_models[] = {"color-classic", NULL};
static const uint32_t cc_roms[] = {0xECD99DC0u, 0};
const mc_hw_desc mc_color_classic_desc = {
    "colorclassic", cc_models, M68K_68030, M68K_FPU_NONE, cc_roms, cc_ram,
    lc_map, sizeof lc_map / sizeof *lc_map, lc_irqs, sizeof lc_irqs / sizeof *lc_irqs,
    build, &lc_ops, MC_MMU_030, mc_v8_map24, &lc_caps,
};

/* The Macintosh TV: a 68030 at 32 MHz, no FPU; the 4 MB on the board and
 * one SIMM of 1 or 4 MB (its ROM's RAM sizing, above). */
static const char *const tv_models[] = {"macintosh-tv", NULL};
static const uint32_t tv_roms[] = {0xEAF1678Du, 0};
static const uint16_t tv_ram[] = {4, 5, 8, 0};
const mc_hw_desc mc_tv_desc = {
    "tv", tv_models, M68K_68030, M68K_FPU_NONE, tv_roms, tv_ram,
    lc_map, sizeof lc_map / sizeof *lc_map, lc_irqs, sizeof lc_irqs / sizeof *lc_irqs,
    build, &lc_ops, MC_MMU_030, mc_v8_map24, &slotless_caps,
};
