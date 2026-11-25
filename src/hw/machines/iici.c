/* Livadix — the Macintosh IIci board.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990):
 * chapter 3, Figure 3-8 (the address map: RAM, ROM at $4000 0000, I/O at
 * $5000 0000 with VIA1 $5000 0000, SCC $5000 4000, SCSI pseudo-DMA with DRQ
 * $5000 6000, SCSI normal mode $5001 0000, SCSI pseudo-DMA without DRQ
 * $5001 2000, sound $5001 4000, SWIM $5001 6000, VDAC $5002 4000, RBV
 * $5002 6000, repeated images from $5004 0000; the RBV again at
 * $5100 0000, the cache card at $5200 0000), "Memory Decode Unit (MDU)",
 * "RAM-Based Video controller (RBV)" and the interrupt levels of the
 * Macintosh II family (VIA1 level 1, the RBV's VIA2 part level 2, SCC level
 * 4, NMI level 7); chapter 4, Tables 4-6 and 4-8 (VIA1 port A:
 * PA7 vSCCWrReq, PA6/4/2/1 CPU.ID3-0 = 1, 0, 1, 1, PA5 vHeadSel, PA3
 * vSync), Table 4-14 (VIA1 port B: PB7 /Par.Err on the IIci, PB6 /Par.En,
 * PB5-4 ADB ST1/ST0, PB3 /ADB.INT, PB2-0 the RTC), Tables 4-9, 4-15, 4-27
 * (the RBV's VIA2 part, rbv.h); chapter 5, "Video RAM in the Macintosh IIci
 * computer" (bank A from physical 0, bank B from $0400 0000); chapter 11,
 * "Handshaking for SCSI data transfers" (the 5380's IRQ and DRQ in the
 * RBV's flags, the MDU's handshake); chapter 12 (the built-in video, the
 * Bt478 CLUT DAC, the sense lines). Designing Cards and Drivers for the
 * Macintosh Family, 3rd ed., chapter 3 (NuBus slots $C-$E on the IIci).
 *
 * Observed from the IIci ROM 368CADFE: its ProductInfo
 * records for the IIci (ROM offsets $36C0, $3700) match VIA1 PA6/4/2/1
 * against 1, 0, 1, 1 (and 1, 1, 1, 1 with parity); its decoder table
 * (ROM offset $348C) puts the devices in the I/O block's image at
 * $50F0 0000.
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
#include "../asic/mdu.h"
#include "../asic/rbv.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VIA1 port A inputs: vSCCWrReq high (no request), CPU.ID3 1, CPU.ID2 0
 * (no parity feature), CPU.ID1 1, CPU.ID0 1 (Guide, Table 4-8); bit 0
 * reserved, read high. */
#define VIA1_PA_IN 0xC7u

typedef struct iici {
    mc_hw *hw;
    mc_mdu mdu;
    mc_via via1;
    mc_rbv rbv;
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
    uint8_t slot_lines;         /* the NuBus slots' lines asserted, RBV port A bits */
    int parity_on, parity_err;  /* parity checking on; its error (via1_port_b) */
    int parity_region;          /* the RAM area while the check is on */
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adb_via adb_via;
    uint64_t fb_serial;
    int overlay_region;
    char nvram[1024];
    int64_t host_start;
} iici;

static const mc_region iici_map[] = {
    {"io", MC_MDU_IO, MC_MDU_IO_SIZE, MC_MDU_IO_IMAGE - 1u, MC_REGION_IO},
    {"rbv", MC_MDU_RBV, 0x01000000u, MC_RBV_SIZE - 1u, MC_REGION_IO},
};

static const mc_irq_route iici_irqs[] = {
    {"via1", 1}, {"rbv", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay and RAM ------------------------------------------------ */

static void map_ram(iici *m) {
    mc_hw *hw = m->hw;
    mc_mdu_map_ram(&m->mdu, &hw->bus, hw->ram, hw->rom, hw->rom_size);
    mc_hw_maps_changed(hw);
}

static void overlay_on(iici *m) {
    mc_hw *hw = m->hw;
    m->mdu.overlay = 1;
    map_ram(m);
    /* The ROM's own range traps the first access, which ends the overlay. */
    mc_bus_remap(&hw->bus, m->overlay_region, MC_MDU_ROM, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(iici *m) {
    mc_hw *hw = m->hw;
    m->mdu.overlay = 0;
    mc_mdu_map_fixed(&m->mdu, &hw->bus, hw->rom, hw->rom_size);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    iici *m = dev;
    if (m->mdu.overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    iici *m = dev;
    if (m->mdu.overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- wiring ------------------------------------------------------------------------ */

static void via1_port_b(iici *m, uint8_t pb) {
    const int data = (m->via1.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&m->rtc, pb & 4u, pb & 2u, data);
    mc_adb_via_state(&m->adb_via, (pb >> 4) & 3u);
    /* PB6 /Par.En low turns parity checking on (Guide, Table 4-14). */
    const int on = !(pb & 0x40u);
    if (on != m->parity_on) {
        m->parity_on = on;
        if (!on && m->parity_err) {
            m->parity_err = 0;
            mc_via_set_pb(&m->via1, 0x80u, 0x80u);
            mc_irq_set(&m->hw->irq, mc_hw_irq(m->hw, "nmi"), 0);
        }
        if (!m->mdu.overlay) {
            if (on) {
                mc_bus_remap(&m->hw->bus, m->parity_region, 0, MC_MDU_ROM);
                mc_hw_maps_changed(m->hw);
            } else {
                map_ram(m);
            }
        }
    }
}

/* Parity checking without the parity feature (mdu.h): the RAM has no
 * parity bits, so the first read with the check on fails: the parity
 * circuit's NMI and /Par.Err (VIA1 PB7) low until the check is turned off.
 * The IIci ROM finds the feature absent this way: it turns the check on,
 * reads each bank and expects an exception (ROM 368CADFE at $4084361A,
 * $40843678, $4084386A), else runs its parity test ($40845E32), which
 * forces bad parity with RBV port B bit 7 and counts the NMIs†. While the
 * check is on, the RAM area is served here, through the banks' windows. */
static uint8_t *ram_byte(iici *m, uint32_t a) {
    if (a < MC_MDU_BANK_B) return m->hw->ram + a % m->mdu.bank_a;
    if (a < MC_MDU_BANK_B + MC_MDU_WINDOW && m->mdu.bank_b) return m->hw->ram + m->mdu.bank_a + (a - MC_MDU_BANK_B) % m->mdu.bank_b;
    return NULL;
}
static uint32_t parity_read(void *dev, uint32_t offset, unsigned size) {
    iici *m = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint8_t *p = ram_byte(m, offset + i);
        v = v << 8 | (p ? *p : 0);
    }
    if (!m->parity_err) {
        m->parity_err = 1;
        mc_via_set_pb(&m->via1, 0, 0x80u);
        mc_irq_set(&m->hw->irq, mc_hw_irq(m->hw, "nmi"), 1);
    }
    return v;
}
static void parity_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    iici *m = dev;
    for (unsigned i = 0; i < size; i++) {
        uint8_t *p = ram_byte(m, offset + i);
        if (p) *p = (uint8_t)(value >> (8 * (size - 1 - i)));
    }
}
static const mc_dev_ops parity_ops = {parity_read, parity_write, NULL, NULL};

static uint8_t adb_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((iici *)ctx)->via1, in); }

static void board_line(void *board, void *dev, int line, int state) {
    iici *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        else if (line == MC_VIA_LINE_PB) via1_port_b(m, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-6). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
    } else if (dev == &m->rbv) {
        if (line == MC_RBV_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "rbv"), state);
        else if (line == MC_RBV_LINE_TICK) mc_via_set_ca1(&m->via1, state);
        /* v2PowerOff, RBV port B bit 2 (Guide, Table 4-15): low switches
         * the power supply off. The ROM's ShutDown sets the bit then clears
         * it as its last step (ROM 368CADFE at $40809E2C-$40809E32); every
         * other write keeps it high (the early $8F, $40802F06; its RBV
         * register test ORs $0D into every value, $40845292). */
        else if (line == MC_RBV_LINE_PB && !(state & 0x04)) mc_hw_power_off(hw);
    } else if (dev == &m->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&m->via1, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&m->via1, state);
    } else if (dev == &m->scsi) {
        /* The 5380's IRQ and DRQ in the RBV's interrupt flags (Guide,
         * chapter 11, "Handshaking for SCSI data transfers"). */
        if (line == MC_NCR5380_LINE_IRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_SCSI, state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_SCSI_DRQ, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-6). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_ASC, state);
    } else if (dev == &m->adb_via) {
        if (line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&m->via1, state ? 0x08u : 0, 0x08u);
    } else if (dev == &m->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&m->nubus, m->card_slot, state);
    }
}

/* The NuBus slots' interrupt lines: RBV port A bits 3-5 for slots $C-$E
 * (Guide, Table 4-9), active low there. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    iici *m = ctx;
    (void)slotirq_high;
    m->slot_lines = (uint8_t)(~levels & mask);
    mc_rbv_set_slots(&m->rbv, (uint8_t)((m->rbv.slots & 0x40u) | m->slot_lines));
}

/* --- persistence ------------------------------------------------------------------ */

/* Livadix's PRAM file, as the Quadra 700's: "LVDXPRAM", a version byte, the
 * clock as seconds since 1904 minus the host's time (signed 64-bit,
 * little-endian), the 256 bytes. A new file is all zeros, which the ROM
 * finds invalid and initialises. */
static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

static void load_nvram(iici *m) {
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

static void save_nvram(iici *m) {
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

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video keeps its slot PRAM at extended PRAM
 * $56-$5D (board ID $001F, then vendorUse1 the depth mode $80 + log2(depth)
 * the driver starts in, vendorUse2 the monitor's sResource $80 + sense code),
 * as the IIci ROM writes it (PrimaryInit, ROM offset $7ECAA). A PRAM the ROM
 * has not set up for this monitor is left to it (the first start is in 1
 * bit). */
static void set_depth(iici *m) {
    const mc_config *c = &m->hw->config;
    uint8_t *p = m->rtc.pram + 0x56;
    if (m->card_slot || m->rtc.pram[0x10] != 0xA8 || p[0] != 0x00 || p[1] != 0x1F || p[3] != (0x80u | m->rbv.sense)) return;
    if (c->depth > mc_rbv_max_depth(m->rbv.sense)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

/* The same for the Livadix Display Card: its slot's eight bytes from extended
 * PRAM $46 for slot $9 (quadra700.c, set_card_depth). */
static void set_card_depth(iici *m) {
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

static void drop_disks(iici *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as the Quadra 700 lays them out. */
static int layout_disks(iici *m, char *error, size_t capacity) {
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
    iici *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    iici *m = board;
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
    iici *m = board;
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
    iici *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    iici *m = board;
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
    iici *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    iici *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    iici *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    iici *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    iici *m = board;
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
    iici *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    iici *m = dev;
    (void)offset;
    mc_ncr5380_dma_write(&m->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* An unused address inside a range the MDU acknowledges (/DSACK0, Guide,
 * Figure 3-8): the cycle completes and reads 0†. The IIci ROM reads the
 * byte below the SCC's base ($50F0 3FFF, ROM offset $A42) at every start. */
static uint32_t hole_read(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)offset;
    (void)size;
    return 0;
}
static void hole_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev;
    (void)offset;
    (void)size;
    (void)value;
}
static const mc_dev_ops hole_ops = {hole_read, hole_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    iici *m = board;
    mc_via_ops.reset(&m->via1);
    mc_rbv_ops.reset(&m->rbv);
    if (m->card_slot) mc_livadix_card_ops.reset(&m->card);
    mc_enet_card_reset(m->enet);
    mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_asc_ops.reset(&m->asc);
    mc_swim_ops.reset(&m->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    iici *m = board;
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
        mc_rbv_view v;
        if (!mc_rbv_view_get(&m->rbv, m->hw->ram, m->mdu.bank_a, &v)) return 0;
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
    iici *m = board;
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    iici *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    iici *m = board;
    m->host_start = host_seconds;
    mc_rtc_set_seconds(&m->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&m->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    iici *m = board;
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

static void *build(mc_hw *hw) {
    uint32_t bank_a = 0, bank_b = 0;
    if (!mc_mdu_banks(hw->config.ram_mb, &bank_a, &bank_b)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: no IIci RAM population gives %u MiB", (unsigned)hw->config.ram_mb);
        return NULL;
    }
    iici *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    hw->board = m;
    mc_mdu_init(&m->mdu, bank_a, bank_b);
    const int io = mc_hw_region(hw, "io");
    /* Every I/O bus cycle takes 0.5 us of guest time, as on the Quadra 700
     * (quadra700.c: the Guide's VIA access time, chapter 4). */
    hw->bus.region[io].cost = 8;
    /* The I/O block (Guide, Figure 3-8). */
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, MC_MDU_VIA1, 0x2000, &m->via1, &mc_via_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, MC_MDU_VIA1 + 0x2000, 0x2000, m, &hole_ops, 0);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_bus_attach(&hw->bus, io, MC_MDU_SCC, 0x2000, &m->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&m->scsi_bus);
    mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 0);
    mc_bus_attach(&hw->bus, io, MC_MDU_SCSI, 0x2000, &m->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, MC_MDU_SCSI_DRQ, 0x2000, m, &pdma_ops, 0);
    mc_bus_attach(&hw->bus, io, MC_MDU_SCSI_BLIND, 0x2000, m, &pdma_ops, 0);
    mc_scsi_cd_init(&m->unit[CD_ID]);
    mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
    mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    mc_asc_init(&m->asc, &hw->host, MC_ASC_ORIGINAL);
    mc_bus_attach(&hw->bus, io, MC_MDU_SOUND, 0x2000, &m->asc, &mc_asc_ops, MC_WIN_PORT8);
    mc_swim_init(&m->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, MC_MDU_SWIM, 0x2000, &m->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* The built-in video: the RBV and its Bt478, the monitor video.resolution
     * names (Guide, Table 12-8), or the Livadix Display Card when the RBV cannot
     * show the configured screen: the RBV then senses no
     * monitor. */
    uint8_t sense = mc_rbv_sense_for(hw->config.width, hw->config.height);
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > mc_rbv_max_depth(sense);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX ||
                     (over && (hw->config.video_card == MC_VIDEO_CARD_AUTO || mc_rbv_max_depth(sense) == 0));
    if (!card && over) hw->config.depth = mc_rbv_max_depth(sense);
    if (card) sense = MC_RBV_SENSE_NONE;
    mc_rbv_init(&m->rbv, &hw->host, "RBV", sense);
    mc_bus_attach(&hw->bus, io, MC_MDU_RBV_IO, 0x2000, &m->rbv, &mc_rbv_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, mc_hw_region(hw, "rbv"), 0, MC_RBV_SIZE, &m->rbv, &mc_rbv_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, MC_MDU_VDAC, 0x2000, &m->rbv, &mc_rbv_vdac_ops, MC_WIN_PORT8);
    /* NuBus: slots $C-$E (Designing Cards and Drivers, chapter 3). */
    mc_nubus_init(&m->nubus, &hw->bus, (uint8_t)(1u << (0xC - 8) | 1u << (0xD - 8) | 1u << (0xE - 8)), slot_pins, m);
    mc_nubus_line(&m->nubus, 0xC, 3);
    mc_nubus_line(&m->nubus, 0xD, 4);
    mc_nubus_line(&m->nubus, 0xE, 5);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
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
    }
    /* The Ethernet card (network.card). */
    m->enet = mc_enet_card_fit(hw, &m->nubus, MC_ENET_CARD_NUBUS, mc_nubus_free_slot(&m->nubus));
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
    set_depth(m);
    if (m->card_slot) set_card_depth(m);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        m->rtc.pram[0x78] = m->rtc.pram[0x79] = 0;
        m->rtc.pram[0x7A] = (uint8_t)(refnum >> 8);
        m->rtc.pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive: VIA1 PB7 /Par.Err high (no parity error). */
    mc_via_set_pa(&m->via1, VIA1_PA_IN, 0xFFu);
    mc_via_set_pb(&m->via1, 0xFFu, 0xFFu);
    m->parity_region = mc_bus_add_io(&hw->bus, 0, MC_MDU_ROM, 0x3FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->parity_region, 0, MC_MDU_ROM, m, &parity_ops, 0);
    /* The ROM and the overlay. */
    m->overlay_region = mc_bus_add_io(&hw->bus, MC_MDU_ROM, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x10000000u, m, &trap_ops, 0);
    overlay_on(m);
    mc_rtc_ops.reset(&m->rtc);
    mc_adb_via_ops.reset(&m->adb_via);
    return m;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((iici *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((iici *)board)->enet, frame, length);
}

static const mc_board_ops iici_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const iici_models[] = {"iici", NULL};
static const uint32_t iici_roms[] = {0x368CADFEu, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return MC_DEPTHS_UPTO(mc_rbv_max_depth(mc_rbv_sense_for(w, h)));
}
static const mc_hw_caps iici_caps = {video_builtin, 1, MC_FEATURE_NUBUS, NULL};

const mc_hw_desc mc_iici_desc = {
    "iici", iici_models, M68K_68030, M68K_FPU_68882, iici_roms, mc_mdu_ram_mb,
    iici_map, sizeof iici_map / sizeof *iici_map, iici_irqs, sizeof iici_irqs / sizeof *iici_irqs,
    build, &iici_ops, MC_MMU_030, NULL, &iici_caps,
};
