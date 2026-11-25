/* Livadix — the Macintosh Quadra 900 and Quadra 950 board.
 *
 * The Quadra 700's board (quadra700.c: DAFB, EASC, SONIC, the 53C96) with
 * the I/O Processors of iop.h in front of the SCC and the SWIM, ADB through
 * the SWIM IOP's port, a second 53C96 for the external SCSI bus, the MCU
 * with four banks of SIMMs, and Caboose for the clock, PRAM, power and the
 * keyswitch.
 *
 * Sources: Macintosh Quadra 900 Developer Note (Apple, 1991): chapter 1
 * (IOPs for the floppy disk, ADB and serial ports; the two SCSI buses; the
 * keyswitch; Caboose, "a custom processor that manages the keyswitch,
 * system power, the real-time clock, and parameter RAM"), chapter 2 Table
 * 2-6 (VIA2 PA0 Ethernet, PA1-PA5 slots, PA6 video) and Table 2-7
 * (interrupt levels: VIA1 1, VIA2 2, SCC 4, NMI 7), chapter 3 ("Support for
 * the MCU IC": four banks on 64 MB boundaries); the Quadra 700 Developer
 * Note for the parts the boards share (see quadra700.c); the Guide to the Macintosh Family Hardware, 2nd ed.,
 * chapter 4 (VIA1 PB0-2 RTC, PA6/4/2/1 CPU.ID3-0, VIA2 CA1 slots, CA2 SCSI
 * DRQ, CB1 sound, CB2 SCSI IRQ, PB7 v2VBL, PB2 v2PowerOff), chapter 8 (ADB
 * bit cells, adb_wire.h); the IOP SWIM Driver ERS (iop.h).
 *
 * Observed from the Quadra 700 and Quadra 950 ROMs: the decoder table at ROM offset $3554 (valid mask
 * $07A3181F) names VIA1 $50F0 0000, VIA2 $50F0 2000, the SCC IOP $50F0 C020,
 * the SWIM IOP $50F1 E020, the sound chip $50F1 4000, the MCU $5000 E000,
 * the SONIC $50F0 A000, the two 53C96s $50F0 F000 and $50F0 F402 and the
 * DAFB $F980 0000; the ProductInfo records at $387C (box flag 14, the
 * Quadra 900) and $38BC (box flag 20, the Quadra 950) match VIA1 port A
 * under the mask $56 against $50 (PA6 and PA4 high) and $10 (PA4 high);
 * their ExtValid field (bits 4-6 = 2) sends the ROM's clock and PRAM
 * routines to Egret's protocol on VIA1 (ROM offset $4B258), which Caboose
 * answers; the ROM enables VIA2's CA2 interrupt for the SWIM IOP.
 *
 * Livadix's own code. */
#include "../hw_machine.h"
#include "../../core/models.h"
#include "../absent.h"
#include "../adb.h"
#include "../adbctl.h"
#include "../adb_wire.h"
#include "../iop.h"
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

/* VIA1 port A inputs: CPU.ID3-0 (PA6, PA4, PA2, PA1) as the model's
 * ProductInfo record wants them; the other inputs read high. */
#define VIA1_PA_OTHER 0x89u
#define VIA1_PA_Q900 0x50u
#define VIA1_PA_Q950 0x10u

typedef struct q900 {
    mc_hw *hw;
    mc_via via1, via2;
    mc_adbctl ctl;              /* Caboose: the clock, PRAM, power and the keyswitch (Egret's protocol) */
    uint8_t keyswitch;          /* 0 Off, 1 On, 2 Secure (mc_machine_control) */
    int powered;
    mc_adb_wire adb_wire;       /* the ADB line as the SWIM IOP's port drives it */
    mc_mcu mcu;
    mc_absent yancc;
    mc_iop scc_iop, swim_iop;   /* the I/O Processors in front of the SCC and the SWIM */
    mc_scc scc;
    mc_asc asc;
    mc_sonic sonic;
    mc_sonic_prom enet_prom;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;           /* the internal bus: the disks and the CD-ROM drive */
    mc_scsi_bus scsi_bus2;
    mc_ncr53c96 scsi2;          /* the external bus: no devices */
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
    uint8_t scsi_irq;       /* the 53C96s' interrupts asserted, bit 0 and 1 */
    uint8_t slot_irq;       /* VIA2 PA0-PA6 sources asserted: bit 0 Ethernet, 1-5 slots $A-$E, 6 video */
    uint64_t fb_serial;
    int overlay;
    int overlay_region;     /* the trap over the ROM's own range while the overlay is on */
    uint8_t via1_pb;        /* last levels VIA1 drives on port B */
    char nvram[1024];
    int64_t host_start;     /* the host's time given at start (mc_machine_set_clock) */
} q900;

static const mc_region q900_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
};

static const mc_irq_route q900_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay -------------------------------------------------------- */

static void map_ram(q900 *q);

static void overlay_on(q900 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    /* The ROM answers at 0 (through the RAM space) and its own range traps
     * the first access, which releases the overlay. */
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(q900 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    q900 *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    q900 *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

/* The banks where the MCU decodes them (mcu.c). */
static void map_ram(q900 *q) {
    mc_hw *hw = q->hw;
    mc_mcu_map(&q->mcu, &hw->bus, hw->ram, hw->ram_size);
    mc_hw_maps_changed(hw);
}

static void mcu_remap(void *ctx) {
    q900 *q = ctx;
    if (!q->overlay) map_ram(q);
}

/* --- wiring ------------------------------------------------------------------------ */

static void slot_irq(q900 *q, unsigned bit, int asserted);

/* Every CPU access to VIA1 comes after the controller's own up to now
 * (adbctl.h). */
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    q900 *q = dev;
    mc_adbctl_sync(&q->ctl);
    return mc_via_ops.read(&q->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q900 *q = dev;
    mc_adbctl_sync(&q->ctl);
    mc_via_ops.write(&q->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    q900 *q = dev;
    return !q->ctl.firmware && mc_via_ops.stable && mc_via_ops.stable(&q->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* The SWIM IOP's port: bit 1 pulls the ADB line low, bit 2 reads the
 * line's level (iop.h). */
static void adb_port_out(void *ctx, uint8_t value, uint64_t t) {
    q900 *q = ctx;
    mc_adb_wire_host(&q->adb_wire, (value & 0x02u) != 0, t);
}
static uint8_t adb_port_in(void *ctx, uint64_t t) {
    q900 *q = ctx;
    return mc_adb_wire_level(&q->adb_wire, t) ? 0x04u : 0;
}

/* The processor runs while Caboose powers the system and releases /RESET. */
static void ctl_hold(q900 *q) {
    const int held = q->ctl.line_state[MC_ADBCTL_LINE_RESET] || !q->ctl.line_state[MC_ADBCTL_LINE_POWER];
    if (held && !q->overlay && q->overlay_region) overlay_on(q);
    mc_hw_hold_reset(q->hw, held);
}

static void board_line(void *board, void *dev, int line, int state) {
    q900 *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: Caboose's lines, Egret's (egret.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&q->ctl, (uint8_t)state);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_VIA_LINE_PB) {
            /* v2VBL: VIA2 Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1
             * (Guide, Table 4-15 and "Auxiliary Control register"). */
            mc_via_set_ca1(&q->via1, state & 0x80);
        }
    } else if (dev == &q->ctl) {
        /* Caboose powers the system and holds /RESET; its power down switches
         * the machine off. */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) q->powered = 1;
                else if (q->powered) mc_hw_power_off(hw);
            }
            ctl_hold(q);
        }
    } else if (dev == &q->scsi || dev == &q->scsi2) {
        /* Either 53C96's interrupt on VIA2 CB2 (Guide, Table 4-20); DREQ in
         * the DAFB's Turbo SCSI register, one bit per chip. */
        const int n = dev == &q->scsi2;
        if (line == MC_NCR53C96_LINE_IRQ) {
            q->scsi_irq = (uint8_t)(state ? q->scsi_irq | 1u << n : q->scsi_irq & ~(1u << n));
            mc_via_set_cb2(&q->via2, !q->scsi_irq);
        } else if (line == MC_NCR53C96_LINE_DRQ) q->dafb.scsi_drq[n] = (uint8_t)(state != 0);
    } else if (dev == &q->scc_iop) {
        if (line == MC_IOP_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
    } else if (dev == &q->swim_iop) {
        /* The SWIM IOP's interrupt on VIA2 CA2. */
        if (line == MC_IOP_LINE_IRQ) mc_via_set_ca2(&q->via2, !state);
    } else if (dev == &q->asc) {
        /* The sound chip's /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->sonic) {
        if (line == MC_SONIC_LINE_IRQ) slot_irq(q, 0, state);
    } else if (dev == &q->dafb) {
        if (line == MC_DAFB_LINE_VBL) slot_irq(q, 6, state);
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
    }
}

/* Slot interrupts: active low on VIA2 PA0, PA4-6, ORed into /SLOTIRQ on
 * CA1 (Developer Note, "Direct Slot interrupt handling", Table 2-6). */
static void slot_irq(q900 *q, unsigned bit, int asserted) {
    q->slot_irq = (uint8_t)(asserted ? q->slot_irq | 1u << bit : q->slot_irq & ~(1u << bit));
    mc_nubus_irq_bit(&q->nubus, bit, asserted);
}

static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    q900 *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    mc_via_set_ca1(&q->via2, slotirq_high);
}

/* --- persistence ------------------------------------------------------------------ */

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

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM (slot $9 at extended PRAM
 * $46-$4D: board ID $002B on the Quadra 900, $0036 on the 950, the depth's
 * mode $80 + log2(depth), the monitor's sResource twice, its sense code),
 * as their ROMs write it, takes
 * the mode when the record is for the monitor connected. A fresh PRAM is
 * left to the ROM, which starts in its default depth (1 bit). */
static void set_depth(q900 *q, uint8_t *p) {
    static const struct { uint8_t monitor, code; } codes[] = {
        {MC_MONITOR_RGB12, 0x02}, {MC_MONITOR_HIRES13, 0x06}, {MC_MONITOR_PORTRAIT, 0x05},
        {MC_MONITOR_RGB16, 0x2D}, {MC_MONITOR_RGB21, 0x00},
    };
    const mc_config *c = &q->hw->config;
    int code = -1;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (codes[i].monitor == q->dafb.monitor) code = codes[i].code;
    if (code < 0 || p[0x10] != 0xA8 || p[0x46] != 0x00 || p[0x47] != (q->hw->desc == &mc_quadra950_desc ? 0x36 : 0x2B) || p[0x4B] != code) return;
    if ((uint64_t)c->width * c->height * c->depth / 8u > q->dafb.vram_size) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card: its slot's PRAM (eight bytes per
 * slot from extended PRAM $46 for slot $9, as the Quadra 700 ROM keeps
 * them: the board ID, then vendorUse1, the depth mode the system starts
 * in; Designing Cards and Drivers for the Macintosh Family, 3rd ed.,
 * chapter 9, "Video drivers"), once the Slot Manager has set the record up
 * for the card. */
static void set_card_depth(q900 *q, uint8_t *pram) {
    const mc_config *c = &q->hw->config;
    uint8_t *p = pram + 0x46 + 8 * (q->card_slot - 9);
    if (pram[0x10] != 0xA8 || p[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || p[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
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

static void drop_disks(q900 *q) {
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
static int layout_disks(q900 *q, char *error, size_t capacity) {
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
    q900 *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    q900 *q = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(q, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) {
        if (q->keyswitch == 2) {
            if (capacity) snprintf(error, capacity, "the keyswitch is at Secure: the floppy drive is off");
            return 0;
        }
        return mc_swim_insert(&q->swim, drive, disk, 0, error, capacity);
    }
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
    q900 *q = board;
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
    q900 *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

/* emulation.startup_sound off: the output stays silent until the ROM's
 * chime is over, the first sound played to the end (both FIFOs drained and every
 * frame made from them pulled). */
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    q900 *q = board;
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
    q900 *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    q900 *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}

static void board_serial_host(void *board, const mc_serial_host *host) {
    q900 *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    q900 *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    q900 *q = board;
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
    q900 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q900 *q = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&q->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* The second chip: its registers from $F402 (offset 2 in its window). */
static uint32_t scsi2_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, (offset - 2u) >> 4, size); }
static void scsi2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, (offset - 2u) >> 4, size, value);
}
static int scsi2_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, (offset - 2u) >> 4, size); }
static const mc_dev_ops scsi_ops2 = {scsi2_read, scsi2_write, NULL, NULL, scsi2_stable};

static uint32_t pdma2_read(void *dev, uint32_t offset, unsigned size) {
    q900 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi2, size, &v);
    return v;
}
static void pdma2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q900 *q = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&q->scsi2, size, value);
}
static const mc_dev_ops pdma2_ops = {pdma2_read, pdma2_write, NULL, NULL};

/* --- Ethernet ------------------------------------------------------------------- */

static void net_transmit(void *ctx, const uint8_t *frame, size_t length) {
    const mc_net_host *net = ctx;
    if (net->transmit) net->transmit(net->ctx, frame, length);
}

static void board_net_host(void *board, const mc_net_host *host) {
    q900 *q = board;
    if (host) q->net = *host;
    else memset(&q->net, 0, sizeof q->net);
    const mc_sonic_port port = {host ? &q->net : NULL, host ? net_transmit : NULL};
    mc_sonic_set_port(&q->sonic, host ? &port : NULL);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    q900 *q = board;
    return mc_sonic_receive(&q->sonic, frame, length);
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    q900 *q = board;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_dafb_ops.reset(&q->dafb);
    if (q->card_slot) mc_livadix_card_ops.reset(&q->card);
    mc_ncr53c96_reset(&q->scsi);
    mc_iop_ops.reset(&q->scc_iop);
    mc_iop_ops.reset(&q->swim_iop);
    mc_ncr53c96_reset(&q->scsi2);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_sonic_ops.reset(&q->sonic);
    mc_swim_ops.reset(&q->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    q900 *q = board;
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
    q900 *q = board;
    /* The keyswitch: at Off the power key starts nothing, at Secure the ADB
     * port is off (the power key still reaches Caboose). */
    if (code == 0x7F && q->keyswitch) mc_adbctl_power_key(&q->ctl, down);
    if (q->keyswitch != 1) return 1;
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adbctl_poke(&q->ctl);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    q900 *q = board;
    if (q->keyswitch != 1) return 1;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adbctl_poke(&q->ctl);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    q900 *q = board;
    q->host_start = host_seconds;
    mc_adbctl_set_clock(&q->ctl, host_seconds);
}

static void board_destroy(void *board) {
    q900 *q = board;
    if (q->nvram[0]) mc_adbctl_nvram_save(&q->ctl, q->nvram, q->host_start);
    mc_swim_flush(&q->swim);
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    mc_scsi_bus_free(&q->scsi_bus);
    mc_scsi_bus_free(&q->scsi_bus2);
    mc_dafb_free(&q->dafb);
    mc_livadix_card_free(&q->card);
    free(q->ctl.mcu);
    free(q);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    q900 *q = calloc(1, sizeof *q);
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
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, q, &via1_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x02000, 0x2000, &q->via2, &mc_via_ops, MC_WIN_PORT8);
    mc_mcu_init(&q->mcu, &hw->host, "MCU", hw->ram_size, mcu_remap, q);
    /* Four banks of four SIMMs, of 1, 4 or 16 MB: the RAM in the fewest
     * banks, the largest first. */
    {
        uint32_t banks[4] = {0, 0, 0, 0}, left = hw->ram_size;
        for (unsigned k = 0; k < 4 && left; k++) {
            uint32_t b = 64u << 20;
            while (b > left && b > (4u << 20)) b >>= 2;
            if (b > left) break;
            banks[k] = b;
            left -= b;
        }
        mc_mcu_set_banks(&q->mcu, banks);
    }
    mc_bus_attach(&hw->bus, io, 0x0E000, 0x1000, &q->mcu, &mc_mcu_ops, 0);
    /* Ethernet: the SONIC and its address PROM. */
    mc_sonic_init(&q->sonic, &hw->host, 2);
    mc_sonic_prom_init(&q->enet_prom, &hw->host, mc_sonic_default_address, 0, 1);
    mc_bus_attach(&hw->bus, io, 0x08000, 0x2000, &q->enet_prom, &mc_sonic_prom_ops, 0);
    mc_bus_attach(&hw->bus, io, 0x0A000, 0x2000, &q->sonic, &mc_sonic_ops, 0);
    /* The SCC: B control +0, A control +2, B data +4, A data +6 (the ROM's
     * base $5000 C020). */
    mc_iop_init(&q->scc_iop, &hw->host, MC_IOP_SCC, &q->scc, &mc_scc_ops);
    mc_scc_init(&q->scc, mc_iop_chip_host(&q->scc_iop));
    /* PCLK at C15M/2, the I/O bus clock halved (Developer Note, "I/O bus
     * adapter ICs": 15.6672 MHz): the ROM's SCC timer test (ROM offset
     * $47A7A) counts at most 65,536 turns of a three-instruction loop
     * between two baud-rate generator zero counts with a time constant of
     * $FFFF from PCLK, which a 3.672 MHz PCLK (17.9 ms) overruns. RTxC on
     * both channels is the 3.672 MHz clock (Guide, ch. 10). */
    mc_scc_set_clocks(&q->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x0C000, 0x2000, &q->scc_iop, &mc_iop_ops, MC_WIN_PORT8);
    /* SCSI: the 53C96's registers every $10, its pseudo-DMA window, the
     * targets: hard disks (layout_disks) and the CD-ROM drive at ID 3. */
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr53c96_init(&q->scsi, &hw->host, &q->scsi_bus, 25000000u);
    mc_bus_attach(&hw->bus, io, 0x0F000, 0x0100, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x0F100, 0x0300, q, &pdma_ops, 0);
    /* The second 53C96 (external bus) at $50F0 F402, its registers every
     * $10 as the first's, and its pseudo-DMA window above them. */
    mc_scsi_bus_init(&q->scsi_bus2);
    mc_ncr53c96_init(&q->scsi2, &hw->host, &q->scsi_bus2, 25000000u);
    mc_bus_attach(&hw->bus, io, 0x0F400, 0x0100, &q->scsi2, &scsi_ops2, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x0F500, 0x0B00, q, &pdma2_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    /* Sound, the floppy controller and the YANCC (not modelled). */
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ENHANCED);
    mc_bus_attach(&hw->bus, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_iop_init(&q->swim_iop, &hw->host, MC_IOP_SWIM, &q->swim, &mc_swim_ops);
    mc_swim_init(&q->swim, mc_iop_chip_host(&q->swim_iop), hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->swim_iop, &mc_iop_ops, MC_WIN_PORT8);
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
    /* NuBus: slots $A-$E (the PDS shares $E); the built-in video is slot
     * $9. Their interrupts and the Ethernet's and video's on VIA2 port A,
     * PA1-PA5 for slots $A-$E (Quadra 900 Developer Note, Table 2-6). */
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)(1u << (0x9 - 8) | 0x7Cu), slot_pins, q);
    mc_nubus_claim(&q->nubus, 0x9);
    mc_nubus_line(&q->nubus, 0xF, 0);         /* the Ethernet, on a line of its own (no slot $F) */
    for (unsigned slot = 0xA; slot <= 0xE; slot++) mc_nubus_line(&q->nubus, slot, slot - 9);
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
    mc_adbctl_init(&q->ctl, &hw->host, "Egret", MC_ADBCTL_EGRET, &q->via1, &q->adb);
    mc_adbctl_set_part(&q->ctl, "341S0851");
    q->ctl.caboose = 1;
    q->keyswitch = 1;
    mc_adb_wire_init(&q->adb_wire, &q->adb);
    const mc_iop_port adb_port = {q, adb_port_out, adb_port_in};
    mc_iop_set_port(&q->swim_iop, &adb_port);
    if (mc_hw_nvram_path(hw, q->nvram, sizeof q->nvram) == NULL) q->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&q->ctl, q->nvram)) {
        fresh_pram(mc_adbctl_pram(&q->ctl));
        mc_adbctl_set_seconds(&q->ctl, 0);
    }
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        if (mc_adbctl_select(&q->ctl, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys) < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the Egret firmware (%s) is not in the roms folders",
                     keys[0] ? keys[0] : "?");
            mc_dafb_free(&q->dafb);
            mc_livadix_card_free(&q->card);
            free(q);
            return NULL;
        }
    }
    uint8_t *pram = mc_adbctl_pram(&q->ctl);
    set_depth(q, pram);
    if (q->card_slot) set_card_depth(q, pram);
    /* storage.boot_priority: cdrom names the CD-ROM drive the default
     * startup device in PRAM ($78-$7B, which the ROM reads at startup): the
     * scsiDev form of the Start Manager's DefStartRec, two reserved bytes
     * and the driver reference number of SCSI ID 3, -(32 + 3 + 1) (Inside
     * Macintosh V, "The Start Manager", GetDefaultStartup; "The SCSI
     * Manager", driver reference numbers). */
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive. */
    mc_via_set_pa(&q->via1, VIA1_PA_OTHER | (hw->desc == &mc_quadra950_desc ? VIA1_PA_Q950 : VIA1_PA_Q900), 0xFFu);
    mc_via_set_pb(&q->via1, 0xF7u, 0xF7u);
    mc_via_set_pa(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    /* The overlay. */
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_adbctl_ops.reset(&q->ctl);
    ctl_hold(q);
    return q;
}

/* The front-panel keyswitch (Developer Note, chapter 1: Off, On, Secure;
 * Caboose "manages the keyswitch"). Turned to Off, Caboose sends a packet
 * of type 4 with 0: the Quadra 950 ROM's handler for it (installed with
 * the Egret Manager's type $FE at ROM offset $854D0, called from $85444
 * for packets of type 4 with their third byte) unmounts the volumes, waits
 * five seconds and powers down through Caboose ($0A, $15466). Secure turns
 * the ADB port and the floppy drive off: the machine runs, the keyboard
 * and mouse and floppy disks reach nothing. From Off to On or Secure with
 * the machine off, Caboose switches it on (as the power key). */
static int board_control(void *board, int control, int value) {
    q900 *q = board;
    if (control != MC_CONTROL_KEYSWITCH) return -1;
    if (value < 0 || value > 2) return q->keyswitch;
    const uint8_t was = q->keyswitch;
    q->keyswitch = (uint8_t)value;
    if (value == 0 && was != 0) {
        const uint8_t p[3] = {4, 0, 0};
        mc_adbctl_post(&q->ctl, p, 3);
    } else if (value != 0 && was == 0 && !q->ctl.line_state[MC_ADBCTL_LINE_POWER]) {
        mc_adbctl_power_key(&q->ctl, 1);
        mc_adbctl_power_key(&q->ctl, 0);
    }
    return q->keyswitch;
}

static const mc_board_ops q900_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive, board_control,
};

/* The Quadra 900 on its own ROM (the Quadra 700's) or the Quadra 950's,
 * which has its record too; the Quadra 950 on its own. */
static const char *const q900_models[] = {"quadra-900", NULL};
static const char *const q950_models[] = {"quadra-950", NULL};
static const uint32_t q900_roms[] = {0x420DBFF3u, 0x3DC27823u, 0};
static const uint32_t q950_roms[] = {0x3DC27823u, 0};
/* Sixteen 30-pin SIMM sockets in four banks of four, no soldered RAM; 1, 4
 * or 16 MB SIMMs (Apple's specifications: 4 MB on the Quadra 900, 8 MB on
 * the 950, up to 256 MB). */
static const uint16_t q900_ram[] = {4, 8, 16, 20, 32, 36, 64, 68, 128, 132, 256, 0};
static const uint16_t q950_ram[] = {8, 16, 20, 32, 36, 64, 68, 128, 132, 256, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    /* The DAFB has no 16-bit mode. */
    return MC_DEPTHS_UPTO(mc_dafb_max_depth(mc_dafb_monitor_for(w, h))) & ~MC_DEPTH_BIT(16);
}
static const mc_hw_caps q900_caps = {video_builtin, 1, MC_FEATURE_ENET, NULL};

const mc_hw_desc mc_quadra900_desc = {
    "quadra900", q900_models, M68K_68040, M68K_FPU_68040, q900_roms, q900_ram,
    q900_map, sizeof q900_map / sizeof *q900_map, q900_irqs, sizeof q900_irqs / sizeof *q900_irqs,
    build, &q900_ops, MC_MMU_040, NULL, &q900_caps,
};
const mc_hw_desc mc_quadra950_desc = {
    "quadra950", q950_models, M68K_68040, M68K_FPU_68040, q950_roms, q950_ram,
    q900_map, sizeof q900_map / sizeof *q900_map, q900_irqs, sizeof q900_irqs / sizeof *q900_irqs,
    build, &q900_ops, MC_MMU_040, NULL, &q900_caps,
};
