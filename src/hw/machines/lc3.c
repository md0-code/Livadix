/* Livadix — the Sonora boards: the Macintosh LC III (both ROMs) and LC III+ with Egret, and
 * the Color Classic II, LC 520, LC 550, Performa 275, 550 and 560
 * with Cuda and a built-in screen.
 *
 * Sources: Macintosh LC III Developer Note (Apple, 1993), chapter 1:
 * Figure 1-1 (block diagram: the 68030 and its FPU socket, Sonora, the
 * soldered DRAM and VRAM and their SIMM sockets, the CLUT/DAC, the Combo
 * chip with the SCC and the 53C80, the 68HC05 ADB/RTC microcontroller,
 * the 114-pin PDS), "Microprocessor" (25 MHz), Table 1-1 (the memory map:
 * RAM, ROM at $4080 0000, I/O at $50F0 0000, VRAM at $60B0 0000, the PDS
 * as slot $E at $FE00 0000 and super slot $E000 0000), "ROM" (the
 * overlay: the ROM at 0 after reset until the first access to its own
 * range), "RAM" (4 MB soldered from 0), Table 1-2 (the SIMM: 1 to 32 MB in
 * one or two banks, from $0040 0000), "Serial and SCSI Interfaces" (the
 * Combo chip: 85C30 and 53C80, as on the LC and LC II), "Floppy Disk
 * Control" (SWIM2 inside Sonora), "Sound System"; sonora.h. Guide to the
 * Macintosh Family Hardware, 2nd ed., chapter 4 (VIA1: port A PA7
 * vSCCWrReq, PA5 vHeadSel; the interrupt levels of the Macintosh II family:
 * VIA1 1, VIA2 2, SCC 4, NMI 7), chapter 11 ("Handshaking
 * for SCSI data transfers": the 5380's IRQ and DRQ in the VIA2 functions'
 * flags, the pseudo-DMA windows with and without DRQ). Macintosh IIsi
 * Developer Note, chapter 5 (Egret: egret.h).
 *
 * Observed from the LC III ROMs ECBBC41C and EC904829:
 *   - the CPU ID register at $5FFF FFFC reads $A55A 0001 (LC III) and the
 *     ROM takes the ProductInfo record whose word at +$3C is the low word
 *     (ROM ECBBC41C offsets $4B3C, $4AF2: record $D2918, box flag 21);
 *     $A55A 0003 is the LC III+ (box 56, 33 MHz), which the ROM also
 *     tells by its bus speed register ($4896);
 *   - its decoder record ($D2E28, decoder kind 11): VIA1 $50F0 0000, SCC
 *     $50F0 4000, SWIM2 $50F1 6000, SCSI $50F1 0000, pseudo-DMA $50F1 2000
 *     and $50F0 6000, sound $50F1 4000, Sonora's pseudo-VIA2 $50F2 6000,
 *     the CLUT DAC $50F2 4000; the video registers at $50F2 8000 (the
 *     driver, $FDB2A); the I/O block does not repeat at $50F4 0000 (the
 *     decoder tests, $3128, $31B4);
 *   - the RAM information ($D3038): banks of 16 MB from 0 while it sizes
 *     memory, then contiguous (sonora.h);
 *   - the VRAM at $60B0 0000, 512 KB (the sizing at $FA120 finds no
 *     memory at +$80000 and writes 4 to Sonora's $04).
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
#include "../asic/sonora.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_BLOCK 0x50F00000u            /* the devices, I/O region offset $F0 0000 */
#define VRAM_BASE 0x60B00000u           /* Table 1-1 */
#define BOARD_RAM (4u << 20)            /* "RAM": 4 MB soldered */
#define VRAM_STD 0x80000u               /* "VRAM": 512 KB soldered, */
#define VRAM_SIMM 0xC0000u              /* 768 KB with the 256 KB SIMM */
#define CARD_SLOT 0xEu                  /* the PDS as slot $E (Table 1-1) */

/* I/O block offsets (the decoder record, above). */
enum {
    OFS_VIA1 = 0x00000, OFS_SCC = 0x04000, OFS_SCSI_HSK = 0x06000, OFS_SCSI = 0x10000, OFS_SCSI_DMA = 0x12000,
    OFS_SOUND = 0x14000, OFS_SWIM = 0x16000, OFS_CFG = 0x18000, OFS_DAC = 0x24000, OFS_VIA2 = 0x26000, OFS_VIDEO = 0x28000
};

/* What tells the boards apart. */
typedef struct board_info {
    const char *model;
    uint16_t cpu_id;            /* the CPU ID register's low word */
    uint8_t cuda;               /* Cuda (else Egret) */
    const char *part;           /* the controller's part */
    mc_sonora_monitor builtin;  /* the built-in screen (NONE: a monitor port) */
} board_info;

/* The CPU ID's low word: $0001 the LC III; $0101 the 33 MHz boards of the
 * Color Classic II ROM ($0100 at 25 MHz), whose ProductInfo records it
 * tells apart by the built-in screen's sense code: 2 (512 x 384) the
 * Color Classic II and Performa 275 (box flag 77), 6 (640 x 480) the
 * LC 550 and Performa 550/560 (box flag 74) (ROM EDE66CBD offsets $4B0C,
 * $4B4E-$4B86). */
static const board_info boards[] = {
    {"lc-iii", 0x0001, 0, "341S0851", MC_SONORA_NONE},
    {"lc-iii-v1", 0x0001, 0, "341S0851", MC_SONORA_NONE},
    {"lc-iii-plus", 0x0003, 0, "341S0851", MC_SONORA_NONE},
    {"color-classic-ii", 0x0101, 1, "341S0060", MC_SONORA_RGB12},
    {"performa-275", 0x0101, 1, "341S0060", MC_SONORA_RGB12},
    {"lc-520", 0x0100, 1, "341S0060", MC_SONORA_HIRES13},
    {"lc-550", 0x0101, 1, "341S0060", MC_SONORA_HIRES13},
    {"performa-550", 0x0101, 1, "341S0060", MC_SONORA_HIRES13},
    {"performa-560", 0x0101, 1, "341S0060", MC_SONORA_HIRES13},
};

static const board_info *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[0];
}

typedef struct lc3 {
    mc_hw *hw;
    const board_info *board;
    mc_via via1;
    mc_sonora sonora;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card in the PDS, when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
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
    uint8_t *vram;
    uint32_t vram_size;
    char nvram[1024];
    int64_t host_start;
} lc3;

static const mc_region lc3_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x10000000u, 0x0FFFFFFFu, MC_REGION_IO},
};

static const mc_irq_route lc3_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- RAM and the reset overlay --------------------------------------------------- */

static void map_bank(void *ctx, uint32_t base, uint32_t window, uint32_t offset, uint32_t size) {
    lc3 *m = ctx;
    mc_bus_map_memory(&m->hw->bus, base, window, m->hw->ram + offset, size, 1);
}

static void map_ram(lc3 *m) {
    mc_hw *hw = m->hw;
    /* Everything below the ROM that no bank answers floats (the ROM's
     * sizing reads the empty banks' windows, $A023A). */
    mc_bus_map_float(&hw->bus, 0, ROM_BASE);
    mc_sonora_ram_layout(&m->sonora, BOARD_RAM, hw->ram_size - BOARD_RAM, map_bank, m);
    mc_hw_maps_changed(hw);
}

static void remap(void *ctx) {
    lc3 *m = ctx;
    if (!m->overlay && m->overlay_region) map_ram(m);
}

/* The ROM at 0 from reset until the first access to its own range (LC III
 * Developer Note, "ROM"). */
static void overlay_on(lc3 *m) {
    mc_hw *hw = m->hw;
    m->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, m->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(lc3 *m) {
    mc_hw *hw = m->hw;
    m->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    lc3 *m = dev;
    if (m->overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    lc3 *m = dev;
    if (m->overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- the CPU ID register ------------------------------------------------------------ */

/* $5FFF FFFC: $A55A in the high word, the board in the low word (above);
 * writes are ignored (the ROM writes 0 and $FFFFFFFF and reads it back
 * unchanged, $4B46). */
static uint32_t cpuid_read(void *dev, uint32_t offset, unsigned size) {
    const lc3 *m = dev;
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

/* --- VIA1 and the controller -------------------------------------------------------- */

/* Every CPU access to VIA1 comes after the controller's own up to now
 * (adbctl.h). */
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    lc3 *m = dev;
    mc_adbctl_sync(&m->ctl);
    return mc_via_ops.read(&m->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lc3 *m = dev;
    mc_adbctl_sync(&m->ctl);
    mc_via_ops.write(&m->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    lc3 *m = dev;
    return !m->ctl.firmware && mc_via_ops.stable && mc_via_ops.stable(&m->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

static void ctl_hold(lc3 *m) {
    const int held = m->ctl.line_state[MC_ADBCTL_LINE_RESET] || !m->ctl.line_state[MC_ADBCTL_LINE_POWER];
    if (held && !m->overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* --- wiring ------------------------------------------------------------------------ */

static void board_line(void *board, void *dev, int line, int state) {
    lc3 *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: the controller's lines (egret.h, cuda.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&m->ctl, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-6). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
    } else if (dev == &m->sonora) {
        if (line == MC_SONORA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_SONORA_LINE_TICK) mc_via_set_ca1(&m->via1, state);
    } else if (dev == &m->ctl) {
        /* The processor runs while the controller powers the system and
         * releases /RESET; its power down switches the machine off. */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) m->powered = 1;
                else if (m->powered) mc_hw_power_off(hw);
            }
            ctl_hold(m);
        }
    } else if (dev == &m->scsi) {
        /* The 5380's IRQ and DRQ in the pseudo-VIA2's flags (Guide, chapter
         * 11, as on the IIci's RBV). */
        if (line == MC_NCR5380_LINE_IRQ) mc_sonora_set_irq(&m->sonora, MC_SONORA_IRQ_SCSI, state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_sonora_set_irq(&m->sonora, MC_SONORA_IRQ_SCSI_DRQ, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-6). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_sonora_set_irq(&m->sonora, MC_SONORA_IRQ_SOUND, state);
    } else if (dev == &m->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&m->nubus, m->card_slot, state);
    }
}

/* The PDS's interrupt (slot $E) in the pseudo-VIA2's slot flags, bit 5. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    lc3 *m = ctx;
    (void)slotirq_high;
    mc_sonora_set_slots(&m->sonora, (uint8_t)(~levels & mask), mask);
}

/* --- persistence ------------------------------------------------------------------ */

/* A new PRAM: what the ROM writes when it finds the PRAM invalid, with
 * 32-bit addressing on ($8A = $05), as the Quadra 605's (quadra605.c). */
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

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video keeps its slot PRAM at extended PRAM
 * $56-$5D (board ID $003A, the depth's mode $80 + log2(depth), the
 * monitor's sResource twice, the monitor's code), as the LC III and Color
 * Classic II ROMs write it (observed: $00 $3A $83 $8B $8B $06 on the
 * 13-inch, $84 $84 $02 on the 12-inch). The sResource follows the VRAM
 * fitted (the PrimaryInit's table, ROM ECBBC41C offset $FA484: 512 KB, then
 * 768 KB), so a record made with the other VRAM is brought to this one
 * and the ROM keeps the mode. A PRAM the ROM has not set up for this
 * monitor is left to it (the ROMs start in 8 bits). */
static void set_depth(lc3 *m, uint8_t *pram) {
    static const uint8_t codes[MC_SONORA_MONITORS] = {7, 2, 6, 1, 8, 0x0A};
    static const uint8_t rsrc[MC_SONORA_MONITORS][2] = {
        {0, 0}, {0x84, 0x84}, {0x8B, 0x8C}, {0x81, 0x82}, {0x90, 0x91}, {0x93, 0x93},
    };
    const mc_config *c = &m->hw->config;
    uint8_t *p = pram + 0x56;
    const mc_sonora_monitor mon = m->sonora.monitor;
    if (m->card_slot || mon == MC_SONORA_NONE || pram[0x10] != 0xA8 || p[0] != 0x00 || p[1] != 0x3A || p[5] != codes[mon]) return;
    if (c->depth > mc_sonora_max_depth(mon, m->vram_size)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
    p[3] = p[4] = rsrc[mon][m->vram_size > VRAM_STD];
}

/* The same for the Livadix Display Card: its slot's eight bytes from extended
 * PRAM $46 for slot $9 (quadra700.c, set_card_depth). */
static void set_card_depth(lc3 *m, uint8_t *pram) {
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

static void drop_disks(lc3 *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as the IIci lays them out (iici.c). */
static int layout_disks(lc3 *m, char *error, size_t capacity) {
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
    lc3 *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    lc3 *m = board;
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
    lc3 *m = board;
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
    lc3 *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    lc3 *m = board;
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
    lc3 *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    lc3 *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    lc3 *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    lc3 *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    lc3 *m = board;
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

/* The pseudo-DMA windows: each CPU access is DACK cycles of the 5380. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    lc3 *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lc3 *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    lc3 *m = board;
    mc_via_ops.reset(&m->via1);
    mc_sonora_ops.reset(&m->sonora);
    if (m->card_slot) mc_livadix_card_ops.reset(&m->card);
    mc_enet_card_reset(m->enet);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    lc3 *m = board;
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
        mc_sonora_view v;
        if (!mc_sonora_view_get(&m->sonora, &v)) return 0;
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
    lc3 *m = board;
    if (code == 0x7F) mc_adbctl_power_key(&m->ctl, down);
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adbctl_poke(&m->ctl);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    lc3 *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adbctl_poke(&m->ctl);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    lc3 *m = board;
    m->host_start = host_seconds;
    mc_adbctl_set_clock(&m->ctl, host_seconds);
}

static void free_board(lc3 *m) {
    mc_scsi_bus_free(&m->scsi_bus);
    mc_livadix_card_free(&m->card);
    mc_enet_card_free(m->enet);
    free(m->ctl.mcu);
    free(m->vram);
    free(m);
}

static void board_destroy(void *board) {
    lc3 *m = board;
    if (m->nvram[0]) mc_adbctl_nvram_save(&m->ctl, m->nvram, m->host_start);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    free_board(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    lc3 *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    m->board = board_of(hw->model);
    hw->board = m;
    const int io = mc_hw_region(hw, "io");
    const uint32_t blk = IO_BLOCK - IO_BASE;
    /* Every I/O bus cycle takes 0.5 us of guest time, as on the IIci
     * (iici.c: the Guide's VIA access time, chapter 4). */
    hw->bus.region[io].cost = 8;
    /* The built-in video and its monitor (video.resolution), or the Livadix
     * Display Card in the PDS when Sonora cannot show the screen: Sonora then senses no monitor. The 256 KB VRAM SIMM is fitted
     * when the screen needs it (640 x 480 x 16, 640 x 870 x 8). */
    mc_sonora_monitor monitor = mc_sonora_monitor_for(hw->config.width, hw->config.height);
    int fits = monitor != MC_SONORA_NONE;
    if (m->board->builtin != MC_SONORA_NONE) {
        fits = monitor == m->board->builtin;
        monitor = m->board->builtin;
    }
    m->vram_size = mc_sonora_max_depth(monitor, VRAM_STD) >= hw->config.depth ? VRAM_STD : VRAM_SIMM;
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > mc_sonora_max_depth(monitor, VRAM_SIMM);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX || !fits || mc_sonora_max_depth(monitor, VRAM_SIMM) == 0 ||
                     (over && hw->config.video_card == MC_VIDEO_CARD_AUTO);
    if (!card && over) { hw->config.depth = mc_sonora_max_depth(monitor, VRAM_SIMM); m->vram_size = VRAM_SIMM; }
    if (card) {
        /* A built-in screen stays connected; the card is the main screen. */
        if (m->board->builtin == MC_SONORA_NONE) monitor = MC_SONORA_NONE;
        m->vram_size = VRAM_STD;
    }
    m->vram = calloc(1, VRAM_SIMM);
    if (!m->vram) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(m);
        return NULL;
    }
    mc_sonora_init(&m->sonora, &hw->host, "Sonora", monitor, m->vram, m->vram_size, remap, m);
    mc_bus_attach(&hw->bus, io, blk + OFS_VIA2, 0x2000, &m->sonora, &mc_sonora_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, blk + OFS_VIDEO, 0x2000, &m->sonora, &mc_sonora_video_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, blk + OFS_DAC, 0x2000, &m->sonora, &mc_sonora_dac_ops, MC_WIN_PORT8);
    if (m->board->builtin != MC_SONORA_NONE)
        mc_bus_attach(&hw->bus, io, blk + OFS_CFG, 0x2000, &m->sonora, &mc_sonora_cfg_ops, MC_WIN_PORT8);
    /* VRAM: its window of 1 MB; beyond the VRAM fitted nothing answers (the
     * ROM's sizing, $FA176). */
    mc_bus_map_float(&hw->bus, VRAM_BASE, 0x100000u);
    mc_bus_map_memory(&hw->bus, VRAM_BASE, m->vram_size, m->vram, m->vram_size, 1);
    mc_bus_attach(&hw->bus, io, 0x0FFFFFFCu, 4, m, &cpuid_ops, 0);
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, blk + OFS_VIA1, 0x2000, m, &via1_ops, MC_WIN_PORT8);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_bus_attach(&hw->bus, io, blk + OFS_SCC, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 1);
    mc_bus_attach(&hw->bus, io, blk + OFS_SCSI, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, blk + OFS_SCSI_HSK, 0x2000, m, &pdma_ops, 0);
    mc_bus_attach(&hw->bus, io, blk + OFS_SCSI_DMA, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    mc_asc_init(&m->asc, &hw->host, MC_ASC_V8);
    mc_bus_attach(&hw->bus, io, blk + OFS_SOUND, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim2_init(&m->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, blk + OFS_SWIM, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* The PDS as slot $E; its interrupt in the slot flags' bit 5. */
    mc_nubus_init(&m->nubus, &hw->bus, (uint8_t)(1u << (CARD_SLOT - 8)), slot_pins, m);
    mc_nubus_line(&m->nubus, CARD_SLOT, CARD_SLOT - 9);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        m->card_slot = CARD_SLOT;
        if (mode < 0 ||
            !mc_livadix_card_init(&m->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&m->card, &m->nubus, m->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            free_board(m);
            return NULL;
        }
    }
    /* The Ethernet card (network.card). */
    m->enet = mc_enet_card_fit(hw, &m->nubus, MC_ENET_CARD_LC, m->card_slot ? 0 : CARD_SLOT);
    if (!m->enet && hw->error[0]) {
        free_board(m);
        return NULL;
    }
    /* ADB, the clock, PRAM and power: Egret or Cuda, with an Apple Extended
     * Keyboard and mouse. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_adbctl_init(&m->ctl, &hw->host, m->board->cuda ? "Cuda" : "Egret", m->board->cuda ? MC_ADBCTL_CUDA : MC_ADBCTL_EGRET,
                   &m->via1, &m->adb);
    mc_adbctl_set_part(&m->ctl, m->board->part);
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&m->ctl, m->nvram)) {
        fresh_pram(mc_adbctl_pram(&m->ctl));
        mc_adbctl_set_seconds(&m->ctl, 0);
    }
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        const int level = mc_adbctl_select(&m->ctl, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys);
        if (level < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the %s firmware (%s) is not in the roms folders",
                     m->board->cuda ? "Cuda" : "Egret", keys[0] ? keys[0] : "?");
            free_board(m);
            return NULL;
        }
    }
    /* Cuda's firmware: the power key that starts the machine stays held
     * through its power-up and is released after the processor starts, as
     * on the Quadra 605 (quadra605.c: released earlier, the firmware's
     * debounce after the reset outlasts the ROM's first wait for it). */
    if (m->board->cuda) m->ctl.key_hold = (uint64_t)MC_C15M_HZ * 3u / 2u;
    uint8_t *pram = mc_adbctl_pram(&m->ctl);
    set_depth(m, pram);
    if (m->card_slot) set_card_depth(m, pram);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive: VIA1 port A with vSCCWrReq (PA7) high,
     * unconnected inputs high. */
    mc_via_set_pa(&m->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via1, 0xF7u, 0xF7u);
    m->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x10000000u, m, &trap_ops, 0);
    overlay_on(m);
    mc_adbctl_ops.reset(&m->ctl);
    ctl_hold(m);
    return m;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((lc3 *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((lc3 *)board)->enet, frame, length);
}

static const mc_board_ops lc3_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const lc3_models[] = {"lc-iii", "lc-iii-v1", "lc-iii-plus", NULL};
static const uint32_t lc3_roms[] = {0xECBBC41Cu, 0xEC904829u, 0};
/* RAM (Table 1-2): 4 MB on the board and one SIMM of 1 to 32 MB. */
static const uint16_t lc3_ram[] = {4, 5, 6, 8, 12, 20, 36, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    const board_info *b = board_of(model);
    mc_sonora_monitor monitor = mc_sonora_monitor_for(w, h);
    int fits = monitor != MC_SONORA_NONE;
    if (b->builtin != MC_SONORA_NONE) { fits = monitor == b->builtin; monitor = b->builtin; }
    return fits ? MC_DEPTHS_UPTO(mc_sonora_max_depth(monitor, VRAM_SIMM)) : 0;
}
static const mc_hw_caps lc3_caps = {video_builtin, 1, MC_FEATURE_LC_SLOT, NULL};

/* The 68030; the FPU socket empty, as the LC III shipped ("A socket for an
 * optional FPU"). */
const mc_hw_desc mc_lc3_desc = {
    "lc3", lc3_models, M68K_68030, M68K_FPU_NONE, lc3_roms, lc3_ram,
    lc3_map, sizeof lc3_map / sizeof *lc3_map, lc3_irqs, sizeof lc3_irqs / sizeof *lc3_irqs,
    build, &lc3_ops, MC_MMU_030, NULL, &lc3_caps,
};

static const char *const cc2_models[] = {"color-classic-ii", "performa-275", "lc-520", "lc-550", "performa-550", "performa-560", NULL};
static const uint32_t cc2_roms[] = {0xEDE66CBDu, 0};

/* The Color Classic II ROM's boards: the same RAM as the LC III (Macintosh
 * LC 520 Developer Note, "RAM Expansion": 4 MB built in, one SIMM to 36
 * MB), the 68030 without an FPU. */
const mc_hw_desc mc_cc2_desc = {
    "colorclassic2", cc2_models, M68K_68030, M68K_FPU_NONE, cc2_roms, lc3_ram,
    lc3_map, sizeof lc3_map / sizeof *lc3_map, lc3_irqs, sizeof lc3_irqs / sizeof *lc3_irqs,
    build, &lc3_ops, MC_MMU_030, NULL, &lc3_caps,
};
