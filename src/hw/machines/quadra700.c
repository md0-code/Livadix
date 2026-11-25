/* Livadix — the Macintosh Quadra 700 board.
 *
 * Sources: Macintosh Quadra 700 Developer Note (1991): Figure 1-2 (the I/O
 * map: VIA1 $5000 0000, VIA2 $5000 2000, Ethernet PROM $5000 8000, Ethernet
 * $5000 A000, SCC $5000 C000, MCU controls $5000 E000, SCSI $5000 F000,
 * sound $5001 4000, SWIM $5001 E000, YANCC $5002 8000, repeated images from
 * $5004 0000), Figure 1-3 (NuBus map: built-in video in slot $9, slots $D
 * and $E), "ROM control" (the reset overlay of the ROM at 0, released by the
 * first access to $4000 0000), "RAM control" (4 MB soldered in bank 1, four
 * SIMMs in bank 2), Table 2-6 (VIA2 PA0 Ethernet, PA4 slot $D, PA5 slot $E,
 * PA6 video), "Direct Slot interrupt handling" (/SLOTIRQ on VIA2 CA1, VIA2
 * at level 2) and Table 2-7 (VIA1 level 1, SCC level 4, NMI level 7). The
 * Guide to the Macintosh Family Hardware, 2nd ed., chapter 4 (VIA1 PB0-2
 * RTC, PB3 /ADB.INT, PB4-5 ADB ST0/ST1, PB7 vSndEnb; PA7 vSCCWrReq, PA5
 * vHeadSel, PA3 vSync, PA6/4/2/1 CPU.ID3-0; VIA1 CA1 the 60.15 Hz tick, CA2
 * the RTC's one-second signal, CB1/CB2/SR the ADB; VIA2 CA1 slots, CA2 SCSI
 * DRQ, CB1 sound, CB2 SCSI IRQ; VIA2 PB7 v2VBL, PB6 v2SNDEXT, PB2
 * v2PowerOff).
 *
 * Observed from the Quadra 700 ROM: the ROM identifies
 * its address decoder by reading the MCU at $5000 E000 and matches the
 * Quadra 700 ProductInfo record against CPU.ID3-0 = 1, 0, 0, 0 (VIA1 PA6 high,
 * PA4/PA2/PA1 low; the Quadra 900 record wants PA4 high).
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../absent.h"
#include "../adb.h"
#include "../adb_via.h"
#include "../rtc.h"
#include "../via.h"
#include "../mcu.h"
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

/* VIA1 port A inputs: vSCCWrReq high (no request), CPU.ID3 high, CPU.ID2-0
 * low: the Quadra 700's identity. Unconnected inputs read high. */
#define VIA1_PA_IN 0xC9u
#define VIA1_PA_ID_MASK 0xD6u

typedef struct q700 {
    mc_hw *hw;
    mc_via via1, via2;
    mc_rtc rtc;
    mc_mcu mcu;
    mc_absent yancc;
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
    uint8_t slot_irq;       /* VIA2 PA0/PA4/PA5/PA6 sources asserted: bit 0 Ethernet, 4 slot $D, 5 slot $E, 6 video */
    uint64_t fb_serial;
    int overlay;
    int overlay_region;     /* the trap over the ROM's own range while the overlay is on */
    uint8_t via1_pb;        /* last levels VIA1 drives on port B */
    char nvram[1024];
    int64_t host_start;     /* the host's time given at start (mc_machine_set_clock) */
} q700;

static const mc_region q700_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
};

static const mc_irq_route q700_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay -------------------------------------------------------- */

static void map_ram(q700 *q);

static void overlay_on(q700 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    /* The ROM answers at 0 (through the RAM space) and its own range traps
     * the first access, which releases the overlay. */
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(q700 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    q700 *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    q700 *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

/* The banks where the MCU decodes them (mcu.c). */
static void map_ram(q700 *q) {
    mc_hw *hw = q->hw;
    mc_mcu_map(&q->mcu, &hw->bus, hw->ram, hw->ram_size);
    mc_hw_maps_changed(hw);
}

static void mcu_remap(void *ctx) {
    q700 *q = ctx;
    if (!q->overlay) map_ram(q);
}

/* --- wiring ------------------------------------------------------------------------ */

static void slot_irq(q700 *q, unsigned bit, int asserted);

static void via1_port_b(q700 *q, uint8_t pb) {
    q->via1_pb = pb;
    const int data = (q->via1.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&q->rtc, pb & 4u, pb & 2u, data);
    mc_adb_via_state(&q->adb_via, (pb >> 4) & 3u);
}

static uint8_t adb_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((q700 *)ctx)->via1, in); }

static void board_line(void *board, void *dev, int line, int state) {
    q700 *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PB) via1_port_b(q, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-2). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&q->swim, (state & 0x20) != 0);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_VIA_LINE_PB) {
            /* v2VBL: VIA2 Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1
             * (Guide, Table 4-15 and "Auxiliary Control register"). */
            mc_via_set_ca1(&q->via1, state & 0x80);
            /* v2PowerOff (Guide, chapter 4): driven low, the power supply
             * switches off. The ROM's ShutDown makes PB2 an output and
             * clears it as its last step (ROM $408154A0-$408154A4); it stays
             * an input from reset until then. */
            if (!(state & 0x04)) mc_hw_power_off(hw);
        }
    } else if (dev == &q->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&q->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&q->via1, state);
    } else if (dev == &q->scsi) {
        /* The 53C96 interrupt on VIA2 CB2 and its DREQ on VIA2 CA2 (Guide,
         * Table 4-20), DREQ also in the DAFB's Turbo SCSI register bit 9. */
        if (line == MC_NCR53C96_LINE_IRQ) mc_via_set_cb2(&q->via2, !state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            mc_via_set_ca2(&q->via2, state);
            q->dafb.scsi_drq[0] = (uint8_t)(state != 0);
        }
    } else if (dev == &q->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-2). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&q->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &q->asc) {
        /* The sound chip's /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->sonic) {
        if (line == MC_SONIC_LINE_IRQ) slot_irq(q, 0, state);
    } else if (dev == &q->adb_via) {
        if (line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&q->via1, state ? 0x08u : 0, 0x08u);
    } else if (dev == &q->dafb) {
        if (line == MC_DAFB_LINE_VBL) slot_irq(q, 6, state);
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
    }
}

/* Slot interrupts: active low on VIA2 PA0, PA4-6, ORed into /SLOTIRQ on
 * CA1 (Developer Note, "Direct Slot interrupt handling", Table 2-6). */
static void slot_irq(q700 *q, unsigned bit, int asserted) {
    q->slot_irq = (uint8_t)(asserted ? q->slot_irq | 1u << bit : q->slot_irq & ~(1u << bit));
    mc_nubus_irq_bit(&q->nubus, bit, asserted);
}

static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    q700 *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    mc_via_set_ca1(&q->via2, slotirq_high);
}

/* --- persistence ------------------------------------------------------------------ */

/* Livadix's PRAM file: "LVDXPRAM", a version byte, the clock as seconds since
 * 1904 minus the host's time (signed 64-bit, little-endian), the 256 bytes. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

/* A new PRAM file: the PRAM of a Quadra 700 whose ROM has initialised it,
 * with 32-bit addressing on. The values are those the Quadra 700 ROM writes
 * when it finds the PRAM invalid: the
 * original 20 bytes, in the fields Inside Macintosh II, "Parameter RAM",
 * lays out (validity $A8 at $10, the modem and printer port settings $CC0A,
 * font 2, kbdPrint $63, then the bytes the 256-byte PRAM keeps at $08-$0B),
 * the extended PRAM's signature "NuMc" at $0C,
 * the default startup device at $76-$7B and zeros elsewhere. Then the
 * addressing mode byte $8A holds $05, what Mac OS 8.0 (which needs 32-bit
 * addressing) writes there before restarting a machine it found in 24-bit
 * mode: a new file starts once, as a Quadra 700 set up for its system. */
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

static void load_nvram(q700 *q) {
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

static void save_nvram(q700 *q) {
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

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM (slot $9 at extended PRAM
 * $46-$4D: board ID $0033, the depth's mode $80 + log2(depth) up to 8 bits
 * and $84 for 32 (the DAFB has no 16-bit mode: the ROM's 13" sResource with
 * four banks lists 1-8 and 32 bits), the monitor's
 * sResource twice, its sense code), as the Quadra 700 ROM writes it, takes
 * the mode when the record is for the monitor connected. A fresh PRAM is
 * left to the ROM, which starts in its default depth (1 bit). */
static void set_depth(q700 *q) {
    static const struct { uint8_t monitor, code; } codes[] = {
        {MC_MONITOR_RGB12, 0x02}, {MC_MONITOR_HIRES13, 0x06}, {MC_MONITOR_PORTRAIT, 0x05},
        {MC_MONITOR_RGB16, 0x2D}, {MC_MONITOR_RGB21, 0x00},
    };
    const mc_config *c = &q->hw->config;
    uint8_t *p = q->rtc.pram;
    int code = -1;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (codes[i].monitor == q->dafb.monitor) code = codes[i].code;
    if (code < 0 || p[0x10] != 0xA8 || p[0x46] != 0x00 || p[0x47] != 0x33 || p[0x4B] != code) return;
    if ((uint64_t)c->width * c->height * c->depth / 8u > q->dafb.vram_size || c->depth == 16) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1 && mode < 0x84; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card: its slot's PRAM (eight bytes per
 * slot from extended PRAM $46 for slot $9, as the Quadra 700 ROM keeps
 * them: the board ID, then vendorUse1, the depth mode the system starts
 * in; Designing Cards and Drivers for the Macintosh Family, 3rd ed.,
 * chapter 9, "Video drivers"), once the Slot Manager has set the record up
 * for the card. */
static void set_card_depth(q700 *q) {
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

static void drop_disks(q700 *q) {
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
static int layout_disks(q700 *q, char *error, size_t capacity) {
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
    q700 *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    q700 *q = board;
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
    q700 *q = board;
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
    q700 *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

/* emulation.startup_sound off: the output stays silent until the ROM's
 * chime is over, the first sound played to the end (both FIFOs drained and every
 * frame made from them pulled). */
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    q700 *q = board;
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
    q700 *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    q700 *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}

static void board_serial_host(void *board, const mc_serial_host *host) {
    q700 *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    q700 *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    q700 *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
}

/* --- SCSI ------------------------------------------------------------------------ */

/* The 53C96's registers every $10 bytes (the ROM's accesses: command at +$30,
 * configuration 1 at +$80, clock conversion at +$90). */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, offset >> 4, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, offset >> 4, size); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};


/* The DAFB's glue turns each CPU access here into DACK cycles of the 53C96. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    q700 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q700 *q = dev;
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
    q700 *q = board;
    if (host) q->net = *host;
    else memset(&q->net, 0, sizeof q->net);
    const mc_sonic_port port = {host ? &q->net : NULL, host ? net_transmit : NULL};
    mc_sonic_set_port(&q->sonic, host ? &port : NULL);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    q700 *q = board;
    return mc_sonic_receive(&q->sonic, frame, length);
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    q700 *q = board;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_dafb_ops.reset(&q->dafb);
    if (q->card_slot) mc_livadix_card_ops.reset(&q->card);
    mc_ncr53c96_reset(&q->scsi);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_sonic_ops.reset(&q->sonic);
    mc_swim_ops.reset(&q->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    q700 *q = board;
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
    q700 *q = board;
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adb_via_poke(&q->adb_via);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    q700 *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adb_via_poke(&q->adb_via);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    q700 *q = board;
    q->host_start = host_seconds;
    mc_rtc_set_seconds(&q->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&q->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    q700 *q = board;
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
    q700 *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    q->hw = hw;
    hw->board = q;
    const int io = mc_hw_region(hw, "io");
    /* Every cycle on the I/O bus takes 0.5 us of guest time (8 C15M
     * periods): the Guide's average for a VIA access on the Macintosh II
     * family (chapter 4, "VIA ICs": 0.5 us against 1.0 us on the classic
     * Macintosh), applied to the other chips the I/O bus adapter serves
     * (Developer Note, "I/O bus adapter ICs"), which Livadix's choice; the
     * ROM's and the System's polling loops count on accesses that slow (the
     * floppy driver's mark search, the sound and SCSI time-outs). */
    hw->bus.region[io].cost = 8;
    /* The I/O block (Developer Note, Figure 1-2). */
    mc_via_init(&q->via1, &hw->host, "VIA1", 9);
    mc_via_init(&q->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, &q->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x02000, 0x2000, &q->via2, &mc_via_ops, MC_WIN_PORT8);
    mc_mcu_init(&q->mcu, &hw->host, "MCU", hw->ram_size, mcu_remap, q);
    mc_bus_attach(&hw->bus, io, 0x0E000, 0x1000, &q->mcu, &mc_mcu_ops, 0);
    /* Ethernet: the SONIC and its address PROM. */
    mc_sonic_init(&q->sonic, &hw->host, 2);
    mc_sonic_prom_init(&q->enet_prom, &hw->host, mc_sonic_default_address, 0, 1);
    mc_bus_attach(&hw->bus, io, 0x08000, 0x2000, &q->enet_prom, &mc_sonic_prom_ops, 0);
    mc_bus_attach(&hw->bus, io, 0x0A000, 0x2000, &q->sonic, &mc_sonic_ops, 0);
    /* The SCC: B control +0, A control +2, B data +4, A data +6 (the ROM's
     * base $5000 C020). */
    mc_scc_init(&q->scc, &hw->host);
    /* PCLK at C15M/2, the I/O bus clock halved (Developer Note, "I/O bus
     * adapter ICs": 15.6672 MHz): the ROM's SCC timer test (ROM offset
     * $47A7A) counts at most 65,536 turns of a three-instruction loop
     * between two baud-rate generator zero counts with a time constant of
     * $FFFF from PCLK, which a 3.672 MHz PCLK (17.9 ms) overruns. RTxC on
     * both channels is the 3.672 MHz clock (Guide, ch. 10). */
    mc_scc_set_clocks(&q->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x0C000, 0x2000, &q->scc, &mc_scc_ops, MC_WIN_PORT8);
    /* SCSI: the 53C96's registers every $10, its pseudo-DMA window, the
     * targets: hard disks (layout_disks) and the CD-ROM drive at ID 3. */
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr53c96_init(&q->scsi, &hw->host, &q->scsi_bus, 25000000u);
    mc_bus_attach(&hw->bus, io, 0x0F000, 0x0100, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x0F100, 0x0F00, q, &pdma_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    /* Sound, the floppy controller and the YANCC (not modelled). */
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ENHANCED);
    mc_bus_attach(&hw->bus, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim_init(&q->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->swim, &mc_swim_ops, MC_WIN_PORT8);
    mc_absent_init(&q->yancc, &hw->host, "YANCC", NULL, 0x00);
    mc_bus_attach(&hw->bus, io, 0x28000, 0x2000, &q->yancc, &mc_absent_ops, 0);
    /* Built-in video in slot $9 (Developer Note, Figure 1-3; DAFB II spec 6.4):
     * VRAM banks of 512 KB from $F900 0000, the controls at $F980 0000, a
     * bus error (TEA) for the rest of slot 9 and super slot 9. Two banks
     * (1 MB) unless the configured screen needs four. */
    /* video.card: the built-in video when it drives the configured mode
     * (Developer Note, Table 1-1, with four banks of VRAM), else the Livadix
     * Display Card in the first free slot, $D; the
     * built-in video then senses no monitor. */
    mc_dafb_monitor monitor = mc_dafb_monitor_for(hw->config.width, hw->config.height);
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    /* The DAFB has no 16-bit mode (16 bits: 8 on the built-in video). */
    const int over = hw->config.depth > mc_dafb_max_depth(monitor) || hw->config.depth == 16;
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX ||
                     (over && (hw->config.video_card == MC_VIDEO_CARD_AUTO || mc_dafb_max_depth(monitor) == 0));
    if (!card && over) hw->config.depth = hw->config.depth == 16 && mc_dafb_max_depth(monitor) >= 8 ? 8 : mc_dafb_max_depth(monitor);
    if (card) monitor = MC_MONITOR_NONE;
    const uint32_t vram = (uint64_t)hw->config.width * hw->config.height * hw->config.depth / 8u > (1u << 20) ? 2u << 20 : 1u << 20;
    if (!mc_dafb_init(&q->dafb, &hw->host, "DAFB", vram, monitor)) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(q);
        return NULL;
    }
    mc_bus_map_float(&hw->bus, 0xF9000000u, 2u << 20);
    mc_bus_map_memory(&hw->bus, 0xF9000000u, vram, q->dafb.vram, vram, 1);
    const int dafb_io = mc_bus_add_io(&hw->bus, 0xF9800000u, 0x1000u, 0xFFFu, 0);
    mc_bus_attach(&hw->bus, dafb_io, 0, 0x400, &q->dafb, &mc_dafb_ops, 0);
    /* NuBus: slots $D and $E (the PDS shares $E); the built-in video is
     * slot $9. Their interrupts and the Ethernet's and video's on VIA2
     * port A (Developer Note, Table 2-6). */
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)(1u << (0x9 - 8) | 1u << (0xD - 8) | 1u << (0xE - 8)), slot_pins, q);
    mc_nubus_claim(&q->nubus, 0x9);
    mc_nubus_line(&q->nubus, 0xF, 0);         /* the Ethernet, on a line of its own (no slot $F) */
    mc_nubus_line(&q->nubus, 0xD, 4);
    mc_nubus_line(&q->nubus, 0xE, 5);
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
    /* ADB: the transceiver on VIA1 and an Apple Extended Keyboard and mouse. */
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
    mc_via_set_pa(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    /* The overlay. */
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_rtc_ops.reset(&q->rtc);
    mc_adb_via_ops.reset(&q->adb_via);
    return q;
}

static const mc_board_ops q700_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const q700_models[] = {"quadra-700", NULL};
static const uint32_t q700_roms[] = {0x420DBFF3u, 0};
/* 4 MB soldered plus four 30-pin SIMMs of 1, 4 or 16 MB (Developer Note,
 * "RAM control": 1 and 4 MB; 16 MB SIMMs give the 68 MB maximum). */
static const uint16_t q700_ram[] = {4, 8, 20, 68, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    /* The DAFB has no 16-bit mode. */
    return MC_DEPTHS_UPTO(mc_dafb_max_depth(mc_dafb_monitor_for(w, h))) & ~MC_DEPTH_BIT(16);
}
static const mc_hw_caps q700_caps = {video_builtin, 1, MC_FEATURE_ENET, NULL};

const mc_hw_desc mc_quadra700_desc = {
    "quadra700", q700_models, M68K_68040, M68K_FPU_68040, q700_roms, q700_ram,
    q700_map, sizeof q700_map / sizeof *q700_map, q700_irqs, sizeof q700_irqs / sizeof *q700_irqs,
    build, &q700_ops, MC_MMU_040, NULL, &q700_caps,
};
