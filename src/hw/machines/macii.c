/* Livadix — the Macintosh II, II FDHD, IIx, IIcx and SE/30 boards.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990):
 * chapter 3, "GLUE in the Macintosh SE/30, Macintosh II, and Macintosh IIx
 * computers", the address maps of the Macintosh II, IIx and IIcx and of the
 * SE/30 (Figures 3-5 to 3-7, Tables 3-8 to 3-10; asic/glue.h), Table 3-4
 * and Table 3-5 (VIA1 level 1, VIA2 level 2, SCC level 4, the interrupt
 * switch level 7), "MC68030 interrupts" (each NuBus slot's line on a VIA2
 * port A input and their OR on a VIA2 interrupt line; the SE/30's
 * processor-direct slot carries slots $9-$B's lines and its video slot
 * $E's), "Auxiliary processors" (the AMU or the 68851 beside the
 * Macintosh II's 68020, the 68881; the 68030's own MMU and the 68882 on the
 * others); chapter 4, Table 4-5 (VIA1 port A: PA7 vSCCWrReq, PA6 CPU.ID0
 * (vPage2 on the SE/30), PA5 vHeadSel, PA4 vOverlay, PA3 vSync), Table 4-9
 * (VIA2 port A: PA7/PA6 the RAM size bits, PA5-PA0 the interrupts of slots
 * $E-$9), Table 4-14 (VIA1 port B: PB7 vSndEnb, PB6 vSyncEnA on the SE/30,
 * PB5/PB4 ADB ST1/ST0, PB3 /ADB.INT, PB2-PB0 the RTC), Table 4-15 (VIA2
 * port B: PB7 v2VBL, Timer 1's 60.15 Hz square wave to VIA1 CA1, PB6
 * v2SNDEXT, PB5/PB4 the NuBus transfer acknowledge, PB3 vFC3: the AMU's
 * 24/32-bit control on the Macintosh II, tied low on the SE/30, PB2
 * v2PowerOff, PB1 v2BusLk, PB0 v2CDis), Tables 4-16 and 4-18 (VIA1 CA1
 * the 60.15 Hz interrupt, CA2 the RTC's one-second interrupt, CB1/CB2 the
 * ADB) and 4-20 (VIA2 CA1 the slot interrupt, CA2 SCSI DRQ, CB1 the ASC,
 * CB2 SCSI IRQ); chapter 9 (the IWM of the Macintosh II's 800K drives, the
 * SWIM of the FDHD machines); chapter 11 (the 5380 and the GLUE's SCSI
 * handshake); chapter 12 (the SE/30's video, video/se30video.h). Designing
 * Cards and Drivers for the Macintosh Family, 3rd ed., chapter 3 (slots
 * $9-$E on the Macintosh II and IIx, $9-$B on the IIcx).
 *
 * Observed from the ROMs: ROM 97221136 (II FDHD, IIx,
 * IIcx, SE/30) picks its box flag at $4083F74A from VIA1 PA6 and VIA2 PB3
 * read as an input (table $4083F79E: PA6 0 and PB3 1 the Macintosh II, 0
 * and 0 the IIx, 1 and 1 the IIcx, 1 and 0 the SE/30), so PB3 floats high
 * while an input, and the AMU then decodes 32 bits: the mode after reset;
 * its VIA set-up ($408006F2: VIA2 PA7/PA6 and PB7 outputs, the others
 * inputs) and RAM sizing (asic/glue.h); the I/O globals (SCSIBase
 * $50F1 0000, SCSIDMA $50F1 2000, SCSIHsk $50F0 6000, ASC $50F1 4000, IWM
 * $50F1 6000).
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "../hw_machine.h"
#include "../adb.h"
#include "../adb_via.h"
#include "../asc.h"
#include "../enet_card.h"
#include "../livadix_card.h"
#include "../ncr5380.h"
#include "../nubus.h"
#include "../rtc.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../via.h"
#include "../asic/glue.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../video/se30video.h"
#include "../../core/romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The boards: VIA1 PA6 (CPU.ID0) and VIA2 PB3 as inputs (the box flag the
 * ROM 97221136 reads, above; Guide, Table 4-5: CPU.ID0 tied low on the II
 * and IIx), the NuBus slots, the floppy controller, the 24-bit decode. */
typedef struct macii_board {
    const char *model;
    uint8_t pa6, pb3;
    uint8_t slots;              /* bit s - 8 for NuBus slot s */
    uint8_t iwm;                /* the IWM and 800K drives (else a SWIM and SuperDrives) */
    uint8_t amu;                /* VIA2 PB3 drives the AMU */
    uint8_t se30;               /* the SE/30's video in slot $E, no soft power */
} macii_board;

/* The Livadix Display Card's 24-bit window (the first MB of its slot). */
#define MC_LIVADIX_CARD_WINDOW24 0x100000u

#define SLOTS_9_E 0x7Eu
#define SLOTS_9_B 0x0Eu

static const macii_board boards[] = {
    {"mac-ii-v1", 0, 1, SLOTS_9_E, 1, 1, 0},
    {"mac-ii-v2", 0, 1, SLOTS_9_E, 1, 1, 0},
    {"mac-ii-fdhd", 0, 1, SLOTS_9_E, 0, 1, 0},
    {"mac-iix", 0, 0, SLOTS_9_E, 0, 0, 0},
    {"mac-iicx", 1, 1, SLOTS_9_B, 0, 0, 0},
    /* The SE/30's processor-direct slot stands for slots $9-$B. */
    {"se-30", 1, 0, SLOTS_9_B, 0, 0, 1},
};

typedef struct macii {
    mc_hw *hw;
    const macii_board *board;
    mc_glue glue;
    mc_via via1, via2;
    mc_rtc rtc;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card, in card_slot when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    mc_se30video video;         /* the SE/30's */
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adb_via adb_via;
    uint64_t fb_serial;
    char nvram[1024];
    int64_t host_start;
} macii;

static const mc_region macii_map[] = {
    {"io", MC_GLUE_IO, MC_GLUE_IO_SIZE, MC_GLUE_IO_IMAGE - 1u, MC_REGION_IO},
};

static const mc_irq_route macii_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the RAM space and the overlay ------------------------------------------------ */

static void map_ram(macii *m) {
    mc_hw *hw = m->hw;
    mc_glue_map_ram(&m->glue, &hw->bus, hw->ram, hw->rom, hw->rom_size);
    mc_hw_maps_changed(hw);
}

/* VIA1 PA4 vOverlay: high (an input after reset, or driven high) the ROM
 * overlay map (Guide, Table 4-5). */
static void set_overlay(macii *m, int on) {
    on = on != 0;
    if (on == m->glue.overlay) return;
    m->glue.overlay = on;
    map_ram(m);
}

/* --- wiring ------------------------------------------------------------------------ */

static void via1_port_a(macii *m, uint8_t pa) {
    /* PA5 vHeadSel: the floppy drive's SEL line. */
    mc_swim_set_headsel(&m->swim, (pa & 0x20) != 0);
    set_overlay(m, pa & 0x10);
    if (m->board->se30) mc_se30video_set_page(&m->video, (pa & 0x40) != 0);
}

static void via1_port_b(macii *m, uint8_t pb) {
    const int data = (m->via1.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&m->rtc, pb & 4u, pb & 2u, data);
    mc_adb_via_state(&m->adb_via, (pb >> 4) & 3u);
    if (m->board->se30) mc_se30video_set_sync_enable(&m->video, (pb & 0x40) != 0);
}

static void via2_port_a(macii *m, uint8_t pa) {
    if (mc_glue_set_size_bits(&m->glue, pa) && !m->glue.overlay) map_ram(m);
}

static void via2_port_b(macii *m, uint8_t pb) {
    mc_hw *hw = m->hw;
    /* v2VBL: Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1 (Guide,
     * Table 4-15 and Table 4-18). */
    mc_via_set_ca1(&m->via1, pb & 0x80);
    /* vFC3 on the Macintosh II with the AMU: low, the AMU's 24-bit map;
     * high (or floating high while an input) 32 bits (Guide, Table 4-15). */
    if (m->board->amu) mc_hw_set_addr24(hw, !(pb & 0x08));
    /* v2PowerOff: driven low, the power supply switches off; the SE/30
     * only presents the signal on its processor-direct slot (Guide,
     * Table 4-15 and its notes). */
    if (!(pb & 0x04) && !m->board->se30) mc_hw_power_off(hw);
}

static uint8_t adb_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((macii *)ctx)->via1, in); }

static void board_line(void *board, void *dev, int line, int state) {
    macii *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PA) via1_port_a(m, (uint8_t)state);
        else if (line == MC_VIA_LINE_PB) via1_port_b(m, (uint8_t)state);
    } else if (dev == &m->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        else if (line == MC_VIA_LINE_PA) via2_port_a(m, (uint8_t)state);
        else if (line == MC_VIA_LINE_PB) via2_port_b(m, (uint8_t)state);
    } else if (dev == &m->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&m->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&m->via1, state);
    } else if (dev == &m->scsi) {
        /* The 5380's IRQ on VIA2 CB2 and its DRQ on CA2 (Guide, Table
         * 4-20), both inverted: the ROMs leave VIA2's PCR at 0 (negative
         * edges, †, ncr5380.h). */
        if (line == MC_NCR5380_LINE_IRQ) mc_via_set_cb2(&m->via2, !state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_via_set_ca2(&m->via2, !state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted. */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        /* The sound chip's /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&m->via2, !state);
    } else if (dev == &m->adb_via) {
        if (line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&m->via1, state ? 0x08u : 0, 0x08u);
    } else if (dev == &m->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&m->nubus, m->card_slot, state);
    } else if (dev == &m->video) {
        if (line == MC_SE30VIDEO_LINE_IRQ) mc_nubus_irq(&m->nubus, 0xE, state);
    }
}

/* The slots' interrupt lines, active low on VIA2 PA0-PA5 for slots $9-$E,
 * and their OR on CA1 (Guide, Table 4-9 and Table 4-20). */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    macii *m = ctx;
    mc_via_set_pa(&m->via2, levels, mask);
    mc_via_set_ca1(&m->via2, slotirq_high);
}

/* --- persistence ------------------------------------------------------------------ */

/* Livadix's PRAM file, as the IIci's (iici.c): "LVDXPRAM", a version byte, the
 * clock as seconds since 1904 minus the host's time, the 256 bytes. A new
 * file is all zeros, which the ROM finds invalid and initialises. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

static void load_nvram(macii *m) {
    FILE *f = m->nvram[0] ? fopen(m->nvram, "rb") : NULL;
    uint8_t buf[8 + 1 + 8 + 256];
    if (f && fread(buf, 1, sizeof buf, f) == sizeof buf && !memcmp(buf, NVRAM_MAGIC, 8) && buf[8] == 1) {
        int64_t offset = 0;
        offset = mc_nvram_clock_offset(buf + 9);
        memcpy(m->rtc.pram, buf + 17, 256);
        m->rtc.seconds = (uint32_t)offset;
    } else {
        memset(m->rtc.pram, 0, 256);
    }
    if (f) fclose(f);
}

static void save_nvram(macii *m) {
    if (!m->nvram[0]) return;
    uint8_t buf[8 + 1 + 8 + 256];
    memcpy(buf, NVRAM_MAGIC, 8);
    buf[8] = 1;
    const int64_t offset = (int64_t)mc_rtc_seconds(&m->rtc) - m->host_start - (int64_t)(mc_hw_now(m->hw) / MC_C15M_HZ);
    for (int i = 0; i < 8; i++) buf[9 + i] = (uint8_t)((uint64_t)offset >> (8 * i));
    memcpy(buf + 17, m->rtc.pram, 256);
    FILE *f = fopen(m->nvram, "wb");
    if (!f) return;
    fwrite(buf, 1, sizeof buf, f);
    fclose(f);
}

/* The screen depth video.depth asks for on the Livadix Display Card, as if
 * chosen in the Monitors control panel: its slot's eight PRAM bytes from
 * extended PRAM $46 for slot $9 (quadra700.c, set_card_depth), once the
 * Slot Manager has set the record up for the card. */
static void set_card_depth(macii *m) {
    const mc_config *c = &m->hw->config;
    uint8_t *p = m->rtc.pram + 0x46 + 8 * (m->card_slot - 9);
    if (m->rtc.pram[0x10] != 0xA8 || p[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || p[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
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

static void drop_disks(macii *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as the Quadra 700 lays them out. */
static int layout_disks(macii *m, char *error, size_t capacity) {
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
    macii *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    macii *m = board;
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
    macii *m = board;
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
    macii *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    macii *m = board;
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
    macii *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    macii *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    macii *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    macii *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    macii *m = board;
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
 * GLUE's handshake holds the cycle until DRQ; Livadix's 5380 moves the byte at
 * once). */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    macii *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    macii *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    macii *m = board;
    mc_via_ops.reset(&m->via1);
    mc_via_ops.reset(&m->via2);
    if (m->card_slot) mc_livadix_card_ops.reset(&m->card);
    mc_enet_card_reset(m->enet);
    if (m->board->se30) mc_se30video_ops.reset(&m->video);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
    /* The VIAs' ports are inputs again: the overlay pin and the RAM size
     * bits float high, PB3 too (32-bit decode on the AMU). */
    via1_port_a(m, 0xFF);
    via2_port_a(m, 0xFF);
    via2_port_b(m, 0xFF);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    macii *m = board;
    if (m->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&m->card, &cv)) return 0;
        fb->width = cv.width;
        fb->height = cv.height;
        fb->depth = cv.depth;
        fb->row_bytes = cv.row_bytes;
        fb->pixels = cv.pixels;
        fb->palette = cv.palette;
    } else if (m->board->se30) {
        mc_se30video_view v;
        mc_se30video_view_get(&m->video, &v);
        fb->width = MC_SE30VIDEO_WIDTH;
        fb->height = MC_SE30VIDEO_HEIGHT;
        fb->depth = 1;
        fb->row_bytes = MC_SE30VIDEO_ROW_BYTES;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
    } else {
        return 0;
    }
    fb->serial = ++m->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    macii *m = board;
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    macii *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    macii *m = board;
    m->host_start = host_seconds;
    mc_rtc_set_seconds(&m->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&m->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    macii *m = board;
    save_nvram(m);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    mc_scsi_bus_free(&m->scsi_bus);
    mc_livadix_card_free(&m->card);
    mc_enet_card_free(m->enet);
    free(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static int read_video_rom(const char *path, uint8_t *rom) {
    FILE *f = path ? fopen(path, "rb") : NULL;
    const int ok = f && fread(rom, 1, MC_SE30VIDEO_ROM, f) == MC_SE30VIDEO_ROM && fgetc(f) == EOF;
    if (f) fclose(f);
    return ok;
}

/* The SE/30's video declaration ROM (docs/NOTES.md): from the roms folders,
 * else under its standard name beside the Macintosh ROM's file; either way
 * identified by its size and CRC32. */
static int load_video_rom(mc_hw *hw, uint8_t *rom) {
    const mc_firmware *want = mc_firmware_find("video-se30");
    const mc_rom_file *file = mc_rom_index_firmware(hw->config.roms, want);
    if (file && read_video_rom(file->path, rom)) return 1;
    char path[MC_CONFIG_PATH + 32];
    snprintf(path, sizeof path, "%s", hw->config.rom);
    char *cut = strrchr(path, '/'), *back = strrchr(path, '\\');
    if (back > cut) cut = back;
    if (cut) cut[1] = 0;
    else path[0] = 0;
    strncat(path, "video-se30.bin", sizeof path - strlen(path) - 1);
    return read_video_rom(path, rom) && mc_firmware_identify(rom, MC_SE30VIDEO_ROM) == want;
}

static void *build(mc_hw *hw) {
    const macii_board *b = NULL;
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, hw->model->key)) b = &boards[i];
    uint32_t bank_a = 0, bank_b = 0;
    if (!b || !mc_glue_banks(hw->config.ram_mb, &bank_a, &bank_b)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: no %s RAM population gives %u MiB", hw->model->name,
                 (unsigned)hw->config.ram_mb);
        return NULL;
    }
    macii *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    m->board = b;
    hw->board = m;
    mc_glue_init(&m->glue, bank_a, bank_b);
    m->glue.size_bits = 3;
    const int io = mc_hw_region(hw, "io");
    /* Every I/O bus cycle takes 0.5 us of guest time (the Guide's VIA
     * access time on the Macintosh II family, chapter 4; quadra700.c). */
    hw->bus.region[io].cost = 8;
    hw->bus.region[io].unclaimed_ok = 1;
    /* The I/O block (asic/glue.h). */
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_via_init(&m->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, io, MC_GLUE_VIA1, 0x2000, &m->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, MC_GLUE_VIA2, 0x2000, &m->via2, &mc_via_ops, MC_WIN_PORT8);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_bus_attach(&hw->bus, io, MC_GLUE_SCC, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 0);
    mc_bus_attach(&hw->bus, io, MC_GLUE_SCSI, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, MC_GLUE_SCSI_DRQ, 0x2000, m, &pdma_ops, 0);
    mc_bus_attach(&hw->bus, io, MC_GLUE_SCSI_BLIND, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ORIGINAL);
    mc_bus_attach(&hw->bus, io, MC_GLUE_SOUND, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    if (b->iwm) mc_iwm_init(&m->swim, &hw->host, hw->media.floppy_drives, MC_DRIVE_800K);
    else mc_swim_init(&m->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, MC_GLUE_IWM, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    mc_glue_map_fixed(&m->glue, &hw->bus, hw->rom, hw->rom_size);
    /* NuBus: the board's slots, their lines on VIA2 PA0-PA5 for $9-$E. */
    mc_nubus_init(&m->nubus, &hw->bus, b->slots, slot_pins, m);
    for (unsigned s = 0x9; s <= 0xE; s++)
        if (b->slots >> (s - 8u) & 1u || (b->se30 && s == 0xE)) mc_nubus_line(&m->nubus, s, s - 9u);
    /* The SE/30's video in slot $E (video/se30video.h). */
    if (b->se30) {
        uint8_t rom[MC_SE30VIDEO_ROM];
        if (!load_video_rom(hw, rom) || !mc_se30video_init(&m->video, &hw->host, "Video", rom, sizeof rom)) {
            snprintf(hw->error, sizeof hw->error, "The SE/30's video declaration ROM (video-se30.bin) is in neither the roms folders nor the ROM's folder");
            free(m);
            return NULL;
        }
        mc_se30video_map(&m->video, &hw->bus);
    }
    /* The Livadix Display Card: always on the modular machines (they have no
     * video of their own), on the SE/30 when the built-in screen cannot show
     * the configured one. */
    /* video.card builtin: the SE/30's screen in 1 bit; another size: the card. */
    const int card = !b->se30 || hw->config.video_card == MC_VIDEO_CARD_LIVADIX ||
                     hw->config.width != MC_SE30VIDEO_WIDTH || hw->config.height != MC_SE30VIDEO_HEIGHT ||
                     (hw->config.video_card == MC_VIDEO_CARD_AUTO && hw->config.depth != 1);
    if (!card) hw->config.depth = 1;
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard24(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        m->card_slot = mc_nubus_free_slot(&m->nubus);
        if (mode < 0 || !m->card_slot ||
            !mc_livadix_card_init(&m->card, &hw->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size, hw->config.video_acceleration) ||
            !mc_livadix_card_attach(&m->card, &m->nubus, m->card_slot)) {
            snprintf(hw->error, sizeof hw->error, "The Livadix Display Card cannot show %ux%u", (unsigned)hw->config.width,
                     (unsigned)hw->config.height);
            mc_livadix_card_free(&m->card);
            free(m);
            return NULL;
        }
        /* The card as a 24-bit card decodes its first MB (its 24-bit
         * window, $s0 0000-$sF FFFF) ignoring /AD23-/AD20: the frame
         * buffer, then its declaration ROM in the last 64 KB, so the
         * screen's base its 24-bit ROM gives ($Fss0 0000, livadixcard24.yaml)
         * and the ROM's top at $Fss F FFFF, where the 1987 Macintosh II ROM
         * looks for it (ROM 97851DB6 at $408043CA; the later ROMs at
         * $FsFF FFFF), are reached in both modes (Designing Cards and
         * Drivers, 3rd ed., chapter 7, before Table 7-2). Its 32-bit
         * layout stays (registers at $C0 0000, the ROM at $FF 0000); slot
         * $C's alias would meet the registers. */
        const unsigned s = m->card_slot;
        mc_nubus_map_memory(&m->nubus, s, MC_LIVADIX_CARD_WINDOW24 - MC_LIVADIX_CARD_ROM_WINDOW, MC_LIVADIX_CARD_ROM_WINDOW, m->card.rom, 0);
        if (s != 0xC) {
            mc_nubus_map_memory(&m->nubus, s, s << 20, MC_LIVADIX_CARD_WINDOW24 - MC_LIVADIX_CARD_ROM_WINDOW, m->card.vram, 1);
            mc_nubus_map_memory(&m->nubus, s, (s << 20) + MC_LIVADIX_CARD_WINDOW24 - MC_LIVADIX_CARD_ROM_WINDOW, MC_LIVADIX_CARD_ROM_WINDOW,
                                m->card.rom, 0);
        }
    }
    /* The Ethernet card (network.card). */
    m->enet = mc_enet_card_fit(hw, &m->nubus, b->se30 ? 0 : MC_ENET_CARD_NUBUS, mc_nubus_free_slot(&m->nubus));
    if (!m->enet && hw->error[0]) {
        mc_livadix_card_free(&m->card);
        free(m);
        return NULL;
    }
    /* ADB: the transceiver on VIA1 and an Apple Extended Keyboard and mouse. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_adb_via_init(&m->adb_via, &hw->host, "ADB", &m->adb, adb_shift, m);
    /* The RTC and PRAM. */
    mc_rtc_init(&m->rtc, &hw->host, "RTC");
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    load_nvram(m);
    if (m->card_slot) set_card_depth(m);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        m->rtc.pram[0x78] = m->rtc.pram[0x79] = 0;
        m->rtc.pram[0x7A] = (uint8_t)(refnum >> 8);
        m->rtc.pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive. VIA1: vSCCWrReq high, CPU.ID0, the reserved
     * inputs high. VIA2: the slot lines high (none asserted), v2SNDEXT high
     * (no plug), the NuBus transfer acknowledge 0 0 (no error, Table
     * 4-16), PB3 as the board reads it. */
    mc_via_set_pa(&m->via1, (uint8_t)(0xBFu | b->pa6 << 6), 0xFFu);
    mc_via_set_pb(&m->via1, 0xFFu, 0xFFu);
    mc_via_set_pa(&m->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via2, (uint8_t)(0xC7u | b->pb3 << 3), 0xFFu);
    map_ram(m);
    mc_rtc_ops.reset(&m->rtc);
    mc_adb_via_ops.reset(&m->adb_via);
    return m;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((macii *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((macii *)board)->enet, frame, length);
}

static const mc_board_ops macii_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

/* RAM: two banks of four SIMMs (asic/glue.h); the Macintosh II's ROMs
 * size 256 KB and 1 MB SIMMs (8 MB), the II FDHD's 68 MB and the 68030
 * boards' 128 MB (docs/NOTES.md, the models' maxima). */
static const uint16_t macii_ram[] = {4, 5, 8, 0};
static const uint16_t fdhd_ram[] = {4, 5, 8, 16, 17, 20, 32, 64, 65, 68, 0};

static const char *const macii_models[] = {"mac-ii-v1", "mac-ii-v2", NULL};
static const uint32_t macii_roms[] = {0x97851DB6u, 0x9779D2C4u, 0};
static const char *const fdhd_models[] = {"mac-ii-fdhd", NULL};
static const char *const iix_models[] = {"mac-iix", "mac-iicx", "se-30", NULL};
static const uint32_t iix_roms[] = {0x97221136u, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
/* The SE/30's 512 x 342 screen in 1 bit; the others have no built-in
 * video. The SE/30's slot takes no Ethernet card. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    return !strcmp(model->key, "se-30") && w == 512 && h == 342 ? MC_DEPTH_BIT(1) : 0;
}
static unsigned features_of(const mc_model *model) { return strcmp(model->key, "se-30") ? MC_FEATURE_NUBUS : 0; }
static const mc_hw_caps macii_caps = {video_builtin, 1, MC_FEATURE_NUBUS, features_of};

const mc_hw_desc mc_macii_desc = {
    "macii", macii_models, M68K_68020, M68K_FPU_68881, macii_roms, macii_ram,
    macii_map, sizeof macii_map / sizeof *macii_map, macii_irqs, sizeof macii_irqs / sizeof *macii_irqs,
    build, &macii_ops, MC_MMU_AMU, mc_amu_mac_ii_map, &macii_caps,
};

const mc_hw_desc mc_macii_fdhd_desc = {
    "macii-fdhd", fdhd_models, M68K_68020, M68K_FPU_68881, iix_roms, fdhd_ram,
    macii_map, sizeof macii_map / sizeof *macii_map, macii_irqs, sizeof macii_irqs / sizeof *macii_irqs,
    build, &macii_ops, MC_MMU_AMU, mc_amu_mac_ii_map, &macii_caps,
};

const mc_hw_desc mc_iix_desc = {
    "iix", iix_models, M68K_68030, M68K_FPU_68882, iix_roms, mc_glue_ram_mb,
    macii_map, sizeof macii_map / sizeof *macii_map, macii_irqs, sizeof macii_irqs / sizeof *macii_irqs,
    build, &macii_ops, MC_MMU_030, NULL, &macii_caps,
};
