/* Livadix — the Centris 610, Centris 650, Quadra 610, Quadra 650 and Quadra
 * 800 board (DJMEMC and IOSB).
 *
 * Sources: Macintosh Developer Note Number 3 (Centris 610, Centris 650 and
 * Quadra 800, 1993): chapter 1, Table 1-4 (built-in video: the maximum
 * depth per monitor with 512 KB and 1 MB of VRAM), chapter 2, "Block
 * Diagram", "Custom ICs" (MEMC, IOSB with the SWIM2, VIA1, VIA2 and the
 * sound circuits, Kiwi as compatible with the YANCC), "RAM" (4 or 8 MB on
 * the board, two 72-pin SIMM sockets on the Centris 610, four on the 650 and
 * 800), Figure 2-4 (the I/O map: VIA1 $5000 0000, VIA2 $5000 2000, Ethernet
 * PROM $5000 8000, Ethernet $5000 A000, SCC $5000 C000, MEMC $5000 E000,
 * SCSI $5001 0000, sound $5001 4000, ADB $5001 6000, IOSB $5001 8000, SWIM
 * II $5001 E000, Kiwi $5002 8000, images repeated from $5004 0000, CPU ID
 * at $5FFF FFFC), Figure 2-5 (built-in video in slot $9, NuBus slots $C, $D
 * and $E, the PDS as $E; the Centris 610's one expansion slot as $E),
 * chapter 4, "Gestalt and SysEnvirons Values". Macintosh Quadra 610 and 650
 * Developer Note (1993), Tables 1-1 and 1-2 (the Quadras' clocks and machine
 * types). Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (VIA
 * pin functions, as on the Quadra 700: quadra700.c).
 *
 * Observed from ROMs F1A6F343 and F1ACAD13:
 *   - the ROM finds the CPU ID register ($A55A 2BAD, iosb.h), then the
 *     ProductInfo record whose VIA1 PA6/PA4/PA2/PA1 match: Centris 610 1, 0,
 *     0, 0 (box 46), Quadra 610 1, 0, 1, 0 (47), Centris 650 1, 0, 1, 1
 *     (24), Quadra 650 1, 1, 0, 1 (30), Quadra 800 0, 1, 0, 1 (29), as the
 *     records at ROM offset $D2B24-$D2D64 read (mask $56 at +$20, ID at
 *     +$24); PA4 and PA2 are also the CPU clock the ROM sets its memory,
 *     IOSB and SCSI timings from (none 20, PA2 25, PA4 33 MHz; ROM offsets
 *     $4944, $D1E06);
 *   - the I/O devices where Figure 2-4 puts them, the SCSI pseudo-DMA
 *     window at $5005 0100 (the image at +$40000 of $5001 0100);
 *   - the 53C96's DREQ reaches VIA2 CA2 inverted: the ROM's SCSI Manager
 *     programs CA2 for a falling edge (PCR $22) and after a DMA selection
 *     waits for that flag (ROM offset $D1982);
 *   - the MEMC's frame buffer controller is programmed through the same
 *     registers as the Quadra 700's DAFB (VRAM from $F900 0000, controls
 *     at $F980 0000; dafb.h), so the DAFB model stands for it; the ROM's
 *     slot PRAM record for it has board ID $003B;
 *   - the sound circuits answer as the Enhanced ASC: the ROM reads the
 *     version $B0 at $5001 4800 and plays the chime through FIFO control
 *     mode 2 as on the Quadra 700 (asc.h).
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../absent.h"
#include "../adb.h"
#include "../adb_via.h"
#include "../rtc.h"
#include "../via.h"
#include "../asic/djmemc.h"
#include "../asic/iosb.h"
#include "../livadix_card.h"
#include "../nubus.h"
#include "../asc.h"
#include "../ncr53c96.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../sonic.h"
#include "../swim.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../video/dafb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_IMAGE 0x40000u
#define VRAM_SIZE (1u << 20)        /* VRAM fully populated: 1 MB (Developer Note, Table 1-4) */
#define VIDEO_BOARD_ID 0x003Bu      /* the ROM's board ID for the built-in video (slot $9 PRAM) */

/* A board of the family: its VIA1 PA6/PA4/PA2/PA1 identity (the bits
 * under mask $56), the RAM on the board, its SIMM banks, its NuBus slots. */
typedef struct q800_board {
    const char *model;
    uint8_t id;
    uint8_t onboard_mb, simm_banks;
    uint8_t slots;                  /* bit s - 8 for each NuBus slot s */
} q800_board;

static const q800_board boards[] = {
    {"centris-610", 0x40, 4, 4, 1u << (0xE - 8)},
    {"quadra-610", 0x44, 8, 4, 1u << (0xE - 8)},
    {"centris-650", 0x46, 8, 8, 1u << (0xC - 8) | 1u << (0xD - 8) | 1u << (0xE - 8)},
    {"quadra-650", 0x52, 8, 8, 1u << (0xC - 8) | 1u << (0xD - 8) | 1u << (0xE - 8)},
    {"quadra-800", 0x12, 8, 8, 1u << (0xC - 8) | 1u << (0xD - 8) | 1u << (0xE - 8)},
};

static const q800_board *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[sizeof boards / sizeof *boards - 1];
}

typedef struct q800 {
    mc_hw *hw;
    const q800_board *board;
    mc_via via1, via2;
    mc_rtc rtc;
    mc_djmemc memc;
    mc_iosb iosb;
    mc_absent kiwi;
    mc_scc scc;
    mc_asc asc;
    mc_sonic sonic;
    mc_sonic_prom enet_prom;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_dafb dafb;
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card, in card_slot when fitted */
    unsigned card_slot;
    mc_serial_host serial;
    mc_net_host net;
    int chime_done;             /* emulation.startup_sound off: the ROM's chime is over */
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adb_via adb_via;
    uint64_t fb_serial;
    int overlay;
    int overlay_region;         /* the trap over the ROM's own range while the overlay is on */
    char nvram[1024];
    int64_t host_start;         /* the host's time given at start (mc_machine_set_clock) */
} q800;

static const mc_region q800_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
    {"cpuid", 0x5FFF0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
};

static const mc_irq_route q800_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay ------------------------------------------------------------- */

static void map_ram(q800 *q);

/* The ROM at 0 from reset until the first access to its own range, as on
 * the Quadra 700 (quadra700.c); the ROM's start runs the same way here. */
static void overlay_on(q800 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(q800 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    q800 *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    q800 *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

/* The banks where the DJMEMC decodes them (djmemc.c). */
static void map_ram(q800 *q) {
    mc_djmemc_map(&q->memc, &q->hw->bus, q->hw->ram);
    mc_hw_maps_changed(q->hw);
}

static void memc_remap(void *ctx) {
    q800 *q = ctx;
    if (!q->overlay) map_ram(q);
}

/* --- wiring ------------------------------------------------------------------------ */

static void slot_irq(q800 *q, unsigned bit, int asserted);

static void via1_port_b(q800 *q, uint8_t pb) {
    const int data = (q->via1.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&q->rtc, pb & 4u, pb & 2u, data);
    mc_adb_via_state(&q->adb_via, (pb >> 4) & 3u);
}

static uint8_t adb_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((q800 *)ctx)->via1, in); }

static void board_line(void *board, void *dev, int line, int state) {
    q800 *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PB) via1_port_b(q, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-2). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&q->swim, (state & 0x20) != 0);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        /* v2VBL: VIA2 Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1, as the
         * ROM programs it (latch $196E, as on the Quadra 700). */
        else if (line == MC_VIA_LINE_PB) mc_via_set_ca1(&q->via1, state & 0x80);
    } else if (dev == &q->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&q->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&q->via1, state);
    } else if (dev == &q->scsi) {
        /* The 53C96 interrupt on VIA2 CB2 (Guide, Table 4-20); its DREQ on
         * VIA2 CA2, low when asserted (the ROM waits for CA2's falling
         * edge). */
        if (line == MC_NCR53C96_LINE_IRQ) mc_via_set_cb2(&q->via2, !state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            mc_via_set_ca2(&q->via2, !state);
            /* IOSB's SCSI DMA handshake: CA2 no longer reports a request
             * once the FIFO has consumed it. The native SCSI Manager
             * polls IFR bit 0 before staging a CDB (7.6.1 SIM $D42D4),
             * without acknowledging a previous transfer there. */
            if (!state) mc_via_set_ifr(&q->via2, MC_VIA_IRQ_CA2, 0);
        }
    } else if (dev == &q->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-2). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&q->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &q->asc) {
        /* The sound circuits' /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->sonic) {
        /* The Ethernet's interrupt as on the Quadra 700 (VIA2 PA0). */
        if (line == MC_SONIC_LINE_IRQ) slot_irq(q, 0, state);
    } else if (dev == &q->adb_via) {
        if (line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&q->via1, state ? 0x08u : 0, 0x08u);
    } else if (dev == &q->dafb) {
        if (line == MC_DAFB_LINE_VBL) slot_irq(q, 6, state);
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
    }
}

/* Slot interrupts: active low on VIA2 port A (slot s on bit s - 9, the
 * video on PA6, the Ethernet on PA0), ORed into /SLOTIRQ on CA1 (Guide,
 * chapter 4; Quadra 700 Developer Note, Table 2-6). */
static void slot_irq(q800 *q, unsigned bit, int asserted) { mc_nubus_irq_bit(&q->nubus, bit, asserted); }

static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    q800 *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    mc_via_set_ca1(&q->via2, slotirq_high);
}

/* --- persistence ------------------------------------------------------------------ */

/* Livadix's PRAM file, as the Quadra 700's (quadra700.c): "LVDXPRAM", a version
 * byte, the clock as seconds since 1904 minus the host's time (signed
 * 64-bit, little-endian), the 256 bytes. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

/* A new PRAM file: what these ROMs write when they find the PRAM invalid
 * (the same bytes as the Quadra 700's ROM: the original 20 bytes, the
 * extended PRAM's signature "NuMc", the default startup device at
 * $76-$7B), with 32-bit addressing on ($8A = $05, as Mac OS 8.0 sets it). */
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

static void load_nvram(q800 *q) {
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

static void save_nvram(q800 *q) {
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

/* The depth the built-in video drives a monitor at with its 1 MB of VRAM;
 * 0 for none: 16 bits on the 12", 13", 16" and VGA monitors, 8 on the
 * others (Developer Note, Table 1-4 for the 610-800, Table 1-3 for the LC
 * 475 family), as the ROM offers them with the board's DAC (dafb.h,
 * antelope). */
static uint32_t video_max_depth(mc_dafb_monitor monitor) {
    switch (monitor) {
    case MC_MONITOR_NONE: return 0;
    case MC_MONITOR_RGB12: case MC_MONITOR_HIRES13: case MC_MONITOR_RGB16: case MC_MONITOR_VGA: return 16;
    default: return 8;
    }
}

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM (slot $9 at extended PRAM
 * $46-$4D: board ID $003B, the depth's mode $80 + log2(depth), the
 * monitor's sResource twice, its code: the sense code, $0A for the 16"
 * monitor), as these ROMs write it, takes
 * the mode when the record is for the monitor connected. A fresh PRAM is
 * left to the ROM, which starts in its default depth. */
static void set_depth(q800 *q) {
    static const struct { uint8_t monitor, code; } codes[] = {
        {MC_MONITOR_RGB12, 0x02}, {MC_MONITOR_HIRES13, 0x06}, {MC_MONITOR_PORTRAIT, 0x05},
        {MC_MONITOR_RGB16, 0x0A}, {MC_MONITOR_RGB19, 0x0B}, {MC_MONITOR_RGB21, 0x00},
    };
    const mc_config *c = &q->hw->config;
    uint8_t *p = q->rtc.pram;
    int code = -1;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (codes[i].monitor == q->dafb.monitor) code = codes[i].code;
    if (code < 0 || p[0x10] != 0xA8 || p[0x46] != (VIDEO_BOARD_ID >> 8) || p[0x47] != (VIDEO_BOARD_ID & 0xFF) ||
        p[0x4B] != code)
        return;
    if (c->depth > video_max_depth(q->dafb.monitor)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card: its slot's PRAM record (eight bytes
 * per slot from extended PRAM $46 for slot $9; Designing Cards and Drivers
 * for the Macintosh Family, 3rd ed., chapter 9, "Video drivers"). */
static void set_card_depth(q800 *q) {
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

static void drop_disks(q800 *q) {
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
static int layout_disks(q800 *q, char *error, size_t capacity) {
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
    q800 *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    q800 *q = board;
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
 * chip and is marked ejected; what the host ejected leaves the chip. */
static void board_poll(void *board) {
    q800 *q = board;
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
    q800 *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

/* emulation.startup_sound off: the output stays silent until the ROM's
 * chime is over, the first sound played to the end (both FIFOs drained and every
 * frame made from them pulled). */
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    q800 *q = board;
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
    q800 *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    q800 *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}

static void board_serial_host(void *board, const mc_serial_host *host) {
    q800 *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    q800 *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    q800 *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
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
    q800 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q800 *q = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&q->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- Ethernet ------------------------------------------------------------------- */

static void net_transmit(void *ctx, const uint8_t *frame, size_t length) {
    const mc_net_host *net = ctx;
    if (net->transmit) net->transmit(net->ctx, frame, length);
}
static void board_net_host(void *board, const mc_net_host *host) {
    q800 *q = board;
    if (host) q->net = *host;
    else memset(&q->net, 0, sizeof q->net);
    const mc_sonic_port port = {host ? &q->net : NULL, host ? net_transmit : NULL};
    mc_sonic_set_port(&q->sonic, host ? &port : NULL);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    q800 *q = board;
    return mc_sonic_receive(&q->sonic, frame, length);
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    q800 *q = board;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_iosb_ops.reset(&q->iosb);
    mc_dafb_ops.reset(&q->dafb);
    if (q->card_slot) mc_livadix_card_ops.reset(&q->card);
    mc_ncr53c96_reset(&q->scsi);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_sonic_ops.reset(&q->sonic);
    mc_swim_ops.reset(&q->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    q800 *q = board;
    mc_dafb_view v;
    /* The main screen: the card when it is fitted (the built-in video then
     * has no monitor). */
    if (q->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&q->card, &cv)) return 0;
        v.width = cv.width;
        v.height = cv.height;
        v.depth = cv.depth;
        v.row_bytes = cv.row_bytes;
        v.pixels = cv.pixels;
        v.palette = cv.palette;
    } else if (!mc_dafb_view_get(&q->dafb, &v)) return 0;
    fb->width = v.width;
    fb->height = v.height;
    fb->depth = v.depth;
    fb->row_bytes = v.row_bytes;
    fb->pixels = v.pixels;
    fb->palette = v.palette;
    fb->serial = ++q->fb_serial;   /* VRAM is written through the CPU's fast path: always fresh */
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    q800 *q = board;
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adb_via_poke(&q->adb_via);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    q800 *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adb_via_poke(&q->adb_via);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    q800 *q = board;
    q->host_start = host_seconds;
    mc_rtc_set_seconds(&q->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&q->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    q800 *q = board;
    save_nvram(q);
    mc_swim_flush(&q->swim);
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    mc_scsi_bus_free(&q->scsi_bus);
    mc_dafb_free(&q->dafb);
    mc_livadix_card_free(&q->card);
    free(q);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    q800 *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    q->hw = hw;
    q->board = board_of(hw->model);
    hw->board = q;
    /* RAM: the board's own and the SIMM banks (Developer Note, "RAM"). */
    uint32_t banks[MC_DJMEMC_BANKS];
    if (!mc_djmemc_layout(hw->config.ram_mb, q->board->onboard_mb, q->board->simm_banks, banks)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: %u MiB does not fit the %s's banks",
                 (unsigned)hw->config.ram_mb, hw->model->name);
        free(q);
        return NULL;
    }
    const int io = mc_hw_region(hw, "io");
    /* I/O bus cycles take 0.5 us of guest time, as on the Quadra 700
     * (quadra700.c): the ROM's and the System's polling loops count them. */
    hw->bus.region[io].cost = 8;
    /* The I/O block (Developer Note, Figure 2-4). */
    mc_via_init(&q->via1, &hw->host, "VIA1", 9);
    mc_via_init(&q->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, &q->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x02000, 0x2000, &q->via2, &mc_via_ops, MC_WIN_PORT8);
    mc_djmemc_init(&q->memc, &hw->host, "DJMEMC", banks, memc_remap, q);
    mc_bus_attach(&hw->bus, io, 0x0E000, 0x2000, &q->memc, &mc_djmemc_ops, 0);
    mc_iosb_init(&q->iosb, &hw->host, "IOSB");
    mc_bus_attach(&hw->bus, io, 0x18000, MC_IOSB_SIZE, &q->iosb, &mc_iosb_ops, 0);
    mc_bus_attach(&hw->bus, mc_hw_region(hw, "cpuid"), 0xFFFC, 4, &q->iosb, &mc_iosb_cpu_id_ops, 0);
    /* Ethernet: the SONIC and its address PROM, as on the Quadra 700. */
    mc_sonic_init(&q->sonic, &hw->host, 2);
    mc_sonic_prom_init(&q->enet_prom, &hw->host, mc_sonic_default_address, 0, 1);
    mc_bus_attach(&hw->bus, io, 0x08000, 0x2000, &q->enet_prom, &mc_sonic_prom_ops, 0);
    mc_bus_attach(&hw->bus, io, 0x0A000, 0x2000, &q->sonic, &mc_sonic_ops, 0);
    /* The SCC: B control +0, A control +2, B data +4, A data +6 from
     * $5000 C020 (the ROM's decoder table), clocked as on the Quadra 700. */
    mc_scc_init(&q->scc, &hw->host);
    mc_scc_set_clocks(&q->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x0C000, 0x2000, &q->scc, &mc_scc_ops, MC_WIN_PORT8);
    /* SCSI: the 53C96's registers every $10, its pseudo-DMA window, the
     * targets: hard disks (layout_disks) and the CD-ROM drive at ID 3. */
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr53c96_init(&q->scsi, &hw->host, &q->scsi_bus, 25000000u);
    mc_bus_attach(&hw->bus, io, 0x10000, 0x0100, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x10100, 0x1F00, q, &pdma_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    /* Sound (the IOSB's, EASC-compatible), the SWIM2 and the Kiwi (its
     * registers not modelled, as the Quadra 700's YANCC). */
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ENHANCED);
    mc_bus_attach(&hw->bus, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim2_init(&q->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->swim, &mc_swim_ops, MC_WIN_PORT8);
    mc_absent_init(&q->kiwi, &hw->host, "Kiwi", NULL, 0x00);
    mc_bus_attach(&hw->bus, io, 0x28000, 0x2000, &q->kiwi, &mc_absent_ops, 0);
    /* Built-in video in slot $9 (Figure 2-5): VRAM from $F900 0000, the
     * controls at $F980 0000; the rest of slot 9 bus-errors. video.card: the
     * built-in video when it drives the configured mode (Table 1-4), else the
     * Livadix Display Card in the first free slot; the built-in video then
     * senses no monitor. */
    mc_dafb_monitor monitor = mc_dafb_monitor_for(hw->config.width, hw->config.height);
    /* These ROMs drive the 19" monitor (1024 x 768), which the Quadra 700's
     * does not (dafb.c leaves it to the card). */
    if (hw->config.width == 1024 && hw->config.height == 768) monitor = MC_MONITOR_RGB19;
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > video_max_depth(monitor);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX ||
                     (over && (hw->config.video_card == MC_VIDEO_CARD_AUTO || video_max_depth(monitor) == 0));
    if (!card && over) hw->config.depth = video_max_depth(monitor);
    if (card) monitor = MC_MONITOR_NONE;
    if (!mc_dafb_init(&q->dafb, &hw->host, "Video", VRAM_SIZE, monitor)) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(q);
        return NULL;
    }
    q->dafb.antelope = 1;
    mc_bus_map_float(&hw->bus, 0xF9000000u, 2u << 20);
    mc_bus_map_memory(&hw->bus, 0xF9000000u, VRAM_SIZE, q->dafb.vram, VRAM_SIZE, 1);
    const int video_io = mc_bus_add_io(&hw->bus, 0xF9800000u, 0x1000u, 0xFFFu, 0);
    mc_bus_attach(&hw->bus, video_io, 0, 0x400, &q->dafb, &mc_dafb_ops, 0);
    /* NuBus: the board's slots; the built-in video is slot $9. Interrupts on
     * VIA2 port A: slot s on bit s - 9, the video on PA6, the Ethernet on
     * PA0, as on the Quadra 700. */
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)(1u << (0x9 - 8) | q->board->slots), slot_pins, q);
    mc_nubus_claim(&q->nubus, 0x9);
    mc_nubus_line(&q->nubus, 0xF, 0);         /* the Ethernet, on a line of its own (no slot $F) */
    for (unsigned s = 0xC; s <= 0xE; s++)
        if (q->board->slots & 1u << (s - 8)) mc_nubus_line(&q->nubus, s, s - 9);
    mc_nubus_line(&q->nubus, 0x9, 6);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        q->card_slot = mc_nubus_free_slot(&q->nubus);
        if (mode < 0 || !q->card_slot ||
            !mc_livadix_card_init(&q->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&q->card, &q->nubus, q->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_dafb_free(&q->dafb);
            mc_livadix_card_free(&q->card);
            free(q);
            return NULL;
        }
    }
    /* ADB: the transceiver on VIA1 (the ROM's VIA ADB primitives, ROM offset
     * $B2C52) and an Apple Extended Keyboard and mouse. */
    mc_adb_bus_init(&q->adb);
    mc_adb_keyboard_init(&q->keyboard);
    mc_adb_mouse_init(&q->mouse);
    mc_adb_attach(&q->adb, &q->keyboard.dev);
    mc_adb_attach(&q->adb, &q->mouse.dev);
    mc_adb_via_init(&q->adb_via, &hw->host, "ADB", &q->adb, adb_shift, q);
    /* The RTC and PRAM. */
    mc_rtc_init(&q->rtc, &hw->host, "RTC");
    if (mc_hw_nvram_path(hw, q->nvram, sizeof q->nvram) == NULL) q->nvram[0] = 0;
    load_nvram(q);
    set_depth(q);
    if (q->card_slot) set_card_depth(q);
    /* storage.boot_priority: cdrom names the CD-ROM drive the default
     * startup device in PRAM ($78-$7B; Inside Macintosh V, "The Start
     * Manager"; the driver reference number of SCSI ID 3). */
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        q->rtc.pram[0x78] = q->rtc.pram[0x79] = 0;
        q->rtc.pram[0x7A] = (uint8_t)(refnum >> 8);
        q->rtc.pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive: VIA1 port A with vSCCWrReq (PA7) high, the
     * board's identity on PA6/PA4/PA2/PA1, unconnected inputs high; VIA2
     * CA2 (the 53C96's /DREQ) high. */
    mc_via_set_pa(&q->via1, (uint8_t)(0xA9u | q->board->id), 0xFFu);
    mc_via_set_pb(&q->via1, 0xFFu, 0xFFu);
    mc_via_set_pa(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_ca2(&q->via2, 1);
    /* The overlay. */
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_rtc_ops.reset(&q->rtc);
    mc_adb_via_ops.reset(&q->adb_via);
    return q;
}

static const mc_board_ops q800_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const q800_models[] = {"centris-650", "quadra-610", "quadra-650", "quadra-800", NULL};
static const char *const c610_models[] = {"centris-610", NULL};
/* Both ROMs list every model of the family (docs/NOTES.md). */
static const uint32_t q800_roms[] = {0xF1A6F343u, 0xF1ACAD13u, 0};
/* RAM (Developer Note, "RAM"): 8 MB on the board (4 MB on the Centris 610,
 * and on some Centris 650s) and 72-pin SIMMs of 4, 8, 16 or 32 MB, one or
 * two banks each; four sockets give 136 MB, the Centris 610's two 68 MB. */
static const uint16_t q800_ram[] = {4, 8, 12, 16, 20, 24, 40, 72, 136, 0};
static const uint16_t c610_ram[] = {4, 8, 12, 16, 20, 24, 36, 40, 68, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    mc_dafb_monitor monitor = mc_dafb_monitor_for(w, h);
    if (w == 1024 && h == 768) monitor = MC_MONITOR_RGB19;
    return MC_DEPTHS_UPTO(video_max_depth(monitor));
}
static const mc_hw_caps q800_caps = {video_builtin, 1, MC_FEATURE_ENET, NULL};

const mc_hw_desc mc_quadra800_desc = {
    "quadra800", q800_models, M68K_68040, M68K_FPU_68040, q800_roms, q800_ram,
    q800_map, sizeof q800_map / sizeof *q800_map, q800_irqs, sizeof q800_irqs / sizeof *q800_irqs,
    build, &q800_ops, MC_MMU_040, NULL, &q800_caps,
};

/* The Centris 610: the same board with the MC68LC040, no FPU (Developer
 * Note, "The MC68040 Microprocessor"). */
const mc_hw_desc mc_centris610_desc = {
    "centris610", c610_models, M68K_68LC040, M68K_FPU_NONE, q800_roms, c610_ram,
    q800_map, sizeof q800_map / sizeof *q800_map, q800_irqs, sizeof q800_irqs / sizeof *q800_irqs,
    build, &q800_ops, MC_MMU_040, NULL, &q800_caps,
};
