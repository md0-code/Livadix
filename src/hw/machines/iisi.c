/* Livadix — the Macintosh IIsi board (MDU, the RBV, Egret).
 *
 * Sources: Macintosh IIsi Developer Note (Apple, 1990): chapter 1
 * (features: the MDU/RBV architecture of the IIci, 20 MHz 68030 without
 * an FPU, the 120-pin expansion connector taking a NuBus or 68030 Direct
 * Slot adapter, each with a 68882, the ADB microcontroller with the RTC,
 * PRAM and soft power, the Combo chip's SCC and 53C80); chapter 2,
 * "Memory map" (the ROM at 0 after reset until the first access to
 * $4000 0000-$4FFF FFFF), Table 2-1 (the 24-bit map's segments: NuBus
 * slot $9 at $F900 0000); chapter 3 (banks A and B at $0000 0000 and
 * $0400 0000, the video frame buffer in bank A); chapter 4, Table 4-1 and
 * 4-2 (the monitors the RBV drives, their sense codes and timing);
 * chapter 5, the complete memory map (the IIci's I/O block: VIA1
 * $5000 0000, SCC $5000 4000, SCSI pseudo-DMA $5000 6000, SCSI
 * $5001 0000 and $5001 2000, sound $5001 4000, SWIM $5001 6000, VDAC
 * $5002 4000, RBV $5002 6000, repeated from $5004 0000), "Versatile
 * Interface Adapter (VIA) interface" (VIA2 functions in the RBV), "ADB
 * microcontroller", "Power control" (Shut Down's command pulls PFW low),
 * "Keyboard reset and NMI"; chapter 6 (the adapters). Guide to the
 * Macintosh Family Hardware, 2nd ed., for what the IIsi shares with the
 * IIci (iici.c: VIA1 port A, the RBV's VIA2 part, the 5380's handshake).
 *
 * Observed from the IIsi ROM 36B7FB6C: its
 * ProductInfo record for the IIsi (ROM offset $3B64, box 12) matches VIA1
 * PA6/4/2/1 (mask $56) against $16 (0, 1, 1, 1) and uses the IIci's
 * decoder table (ROM offset $348C: the I/O block's image at $50F0 0000);
 * Egret on VIA1 PB3-PB5 and the shift register (egret.h).
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
#include "../asic/mdu.h"
#include "../asic/rbv.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../../core/models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* VIA1 port A inputs: vSCCWrReq (PA7) high (no request); the machine ID
 * PA6, PA4, PA2, PA1 = 0, 1, 1, 1 (the ROM's IIsi record, $16 under mask
 * $56); the unused PA0 high. */
#define VIA1_PA_IN 0x97u

/* The expansion adapter's NuBus slot (Developer Note, Table 2-1). */
#define CARD_SLOT 0x9u

typedef struct iisi {
    mc_hw *hw;
    mc_mdu mdu;
    mc_via via1;
    mc_rbv rbv;
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr5380 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card on the NuBus adapter, when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    mc_serial_host serial;
    int chime_done;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adbctl egret;
    int powered;                /* Egret turned the power on */
    uint64_t fb_serial;
    int overlay_region;
    char nvram[1024];
    int64_t host_start;
} iisi;

/* RAM (Developer Note, chapter 3, "RAM", Figure 3-1): bank A is the 1 MB
 * soldered on the board, bank B four equal SIMMs of 256 KB, 512 KB, 1 MB,
 * 2 MB, 4 MB or 16 MB (1, 2, 4, 8, 16 or 64 MB); the totals of at least
 * Livadix's 4 MB minimum. */
static const uint16_t iisi_ram[] = {5, 9, 17, 65, 0};

static const mc_region iisi_map[] = {
    {"io", MC_MDU_IO, MC_MDU_IO_SIZE, MC_MDU_IO_IMAGE - 1u, MC_REGION_IO},
    {"rbv", MC_MDU_RBV, 0x01000000u, MC_RBV_SIZE - 1u, MC_REGION_IO},
};

/* The IIci's levels (Guide, chapter 3). */
static const mc_irq_route iisi_irqs[] = {
    {"via1", 1}, {"rbv", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay and RAM ------------------------------------------------ */

static void map_ram(iisi *m) {
    mc_hw *hw = m->hw;
    mc_mdu_map_ram(&m->mdu, &hw->bus, hw->ram, hw->rom, hw->rom_size);
    mc_hw_maps_changed(hw);
}

static void overlay_on(iisi *m) {
    mc_hw *hw = m->hw;
    m->mdu.overlay = 1;
    map_ram(m);
    mc_bus_remap(&hw->bus, m->overlay_region, MC_MDU_ROM, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(iisi *m) {
    mc_hw *hw = m->hw;
    m->mdu.overlay = 0;
    mc_mdu_map_fixed(&m->mdu, &hw->bus, hw->rom, hw->rom_size);
    map_ram(m);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    iisi *m = dev;
    if (m->mdu.overlay) overlay_off(m);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | m->hw->rom[(offset + i) & (m->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    iisi *m = dev;
    if (m->mdu.overlay) overlay_off(m);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- VIA1 and Egret ---------------------------------------------------------------- */

/* Every CPU access to VIA1 comes after Egret's own up to now (adbctl.h). */
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    iisi *m = dev;
    mc_adbctl_sync(&m->egret);
    return mc_via_ops.read(&m->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    iisi *m = dev;
    mc_adbctl_sync(&m->egret);
    mc_via_ops.write(&m->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    iisi *m = dev;
    return !m->egret.firmware && mc_via_ops.stable && mc_via_ops.stable(&m->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* The processor runs while Egret powers the system and releases /RESET; a
 * reset brings the overlay back (Developer Note, "Power-on reset"). */
static void egret_hold(iisi *m) {
    const int held = m->egret.line_state[MC_ADBCTL_LINE_RESET] || !m->egret.line_state[MC_ADBCTL_LINE_POWER];
    if (held && !m->mdu.overlay && m->overlay_region) overlay_on(m);
    mc_hw_hold_reset(m->hw, held);
}

/* --- wiring ------------------------------------------------------------------------ */

static void board_line(void *board, void *dev, int line, int state) {
    iisi *m = board;
    mc_hw *hw = m->hw;
    if (dev == &m->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: Egret's XcvrSes, ViaFull and SysSes (egret.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&m->egret, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-6). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&m->swim, (state & 0x20) != 0);
    } else if (dev == &m->rbv) {
        if (line == MC_RBV_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "rbv"), state);
        else if (line == MC_RBV_LINE_TICK) mc_via_set_ca1(&m->via1, state);
    } else if (dev == &m->egret) {
        /* Shut Down's power-down command pulls PFW low and the supply goes
         * off (Developer Note, "Soft power control"). */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) m->powered = 1;
                else if (m->powered) mc_hw_power_off(hw);
            }
            egret_hold(m);
        }
    } else if (dev == &m->scsi) {
        /* The 5380's IRQ and DRQ in the RBV's interrupt flags, as on the
         * IIci (Guide, chapter 11). */
        if (line == MC_NCR5380_LINE_IRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_SCSI, state);
        else if (line == MC_NCR5380_LINE_DRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_SCSI_DRQ, state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-6). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->asc) {
        if (line == MC_ASC_LINE_IRQ) mc_rbv_set_irq(&m->rbv, MC_RBV_IRQ_ASC, state);
    } else if (dev == &m->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&m->nubus, m->card_slot, state);
    }
}

/* The adapter's slot interrupt: RBV port A bit 0 for slot $9 (Guide, Table
 * 4-9: bits 0-5 are slots $9-$E), active low there. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    iisi *m = ctx;
    (void)slotirq_high;
    const uint8_t lines = (uint8_t)(~levels & mask);
    mc_rbv_set_slots(&m->rbv, (uint8_t)((m->rbv.slots & 0x40u) | lines));
}

/* --- persistence ------------------------------------------------------------------ */

/* The screen depth video.depth asks for, as on the IIci (iici.c): the
 * built-in video's slot PRAM, here slot $E's at extended PRAM $6E-$75
 * (board ID $0028, the depth's mode, the monitor's sResource $80 + sense
 * code), as the IIsi ROM writes it at its first start. */
static void set_depth(iisi *m, uint8_t *pram) {
    const mc_config *c = &m->hw->config;
    uint8_t *p = pram + 0x6E;
    if (m->card_slot || pram[0x10] != 0xA8 || p[0] != 0x00 || p[1] != 0x28 || p[3] != (0x80u | m->rbv.sense)) return;
    if (c->depth > mc_rbv_max_depth(m->rbv.sense)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[2] = (uint8_t)mode;
}

/* The same for the Livadix Display Card in slot $9 (extended PRAM $46). */
static void set_card_depth(iisi *m, uint8_t *pram) {
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

static void drop_disks(iisi *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as the IIci lays them out (iici.c). */
static int layout_disks(iisi *m, char *error, size_t capacity) {
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
    iisi *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    iisi *m = board;
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
    iisi *m = board;
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
    iisi *m = board;
    return m->hw->config.sound ? mc_asc_rate(&m->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    iisi *m = board;
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
    iisi *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    iisi *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    iisi *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    iisi *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    iisi *m = board;
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
    iisi *m = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr5380_dma_read(&m->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    iisi *m = dev;
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
    iisi *m = board;
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
    iisi *m = board;
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
    iisi *m = board;
    /* The keyboard's power key also grounds the ADB connector's power-on
     * pin, Egret's power-key input (Developer Note, Table 5-5). */
    if (code == 0x7F) mc_adbctl_power_key(&m->egret, down);
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adbctl_poke(&m->egret);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    iisi *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adbctl_poke(&m->egret);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    iisi *m = board;
    m->host_start = host_seconds;
    mc_adbctl_set_clock(&m->egret, host_seconds);
}

static void board_destroy(void *board) {
    iisi *m = board;
    if (m->nvram[0]) mc_adbctl_nvram_save(&m->egret, m->nvram, m->host_start);
    mc_swim_flush(&m->swim);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    mc_scsi_bus_free(&m->scsi_bus);
    mc_livadix_card_free(&m->card);
    mc_enet_card_free(m->enet);
    free(m->egret.mcu);
    free(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    uint32_t bank_b = 0;
    for (const uint16_t *r = iisi_ram; *r; r++)
        if (*r == hw->config.ram_mb) bank_b = (uint32_t)(*r - 1u) << 20;
    if (!bank_b) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: no IIsi RAM population gives %u MiB", (unsigned)hw->config.ram_mb);
        return NULL;
    }
    iisi *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    hw->board = m;
    mc_mdu_init(&m->mdu, 1u << 20, bank_b);
    const int io = mc_hw_region(hw, "io");
    /* Every I/O bus cycle takes 0.5 us of guest time, as on the IIci. */
    hw->bus.region[io].cost = 8;
    /* The I/O block (Developer Note, chapter 5; the IIci's, mdu.h). */
    mc_via_init(&m->via1, &hw->host, "VIA1", 9);
    mc_bus_attach(&hw->bus, io, MC_MDU_VIA1, 0x2000, m, &via1_ops, MC_WIN_PORT8);
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
    /* The built-in video (Developer Note, chapter 4), or the Livadix Display
     * Card on the NuBus adapter when the RBV cannot show the screen. */
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
    /* The expansion connector: no adapter (its slot space bus-errors),
     * or the NuBus adapter with the card in slot $9 and the adapter's
     * 68882 (Developer Note, chapter 6): the Livadix Display Card, or the
     * Ethernet card when network.card asks for the NuBus one (auto leaves
     * the connector empty: the adapter is an addition). */
    const int adapter = card || hw->config.network_card == MC_NET_CARD_NUBUS;
    mc_nubus_init(&m->nubus, &hw->bus, adapter ? (uint8_t)(1u << (CARD_SLOT - 8)) : 0, slot_pins, m);
    if (card) {
        size_t rom_size = 0;
        const uint8_t *rom = mc_guest_livadixcard(&rom_size);
        const int mode = mc_livadix_card_mode(hw->config.width, hw->config.height);
        mc_nubus_line(&m->nubus, CARD_SLOT, 0);
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
        m68k_set_fpu(hw->cpu, M68K_FPU_68882, hw->config.fpu_full);
    }
    m->enet = mc_enet_card_fit(hw, &m->nubus, hw->config.network_card == MC_NET_CARD_NUBUS ? MC_ENET_CARD_NUBUS : 0,
                               mc_nubus_free_slot(&m->nubus));
    if (!m->enet && hw->error[0]) {
        mc_livadix_card_free(&m->card);
        free(m);
        return NULL;
    }
    if (m->enet) {
        mc_nubus_line(&m->nubus, CARD_SLOT, 0);
        m68k_set_fpu(hw->cpu, M68K_FPU_68882, hw->config.fpu_full);
    }
    /* ADB, the clock, PRAM and power: Egret, with an Apple Extended
     * Keyboard and mouse. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_adbctl_init(&m->egret, &hw->host, "Egret", MC_ADBCTL_EGRET, &m->via1, &m->adb);
    mc_adbctl_set_part(&m->egret, "344S0100");
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&m->egret, m->nvram)) {
        memset(mc_adbctl_pram(&m->egret), 0, 256);
        mc_adbctl_set_seconds(&m->egret, 0);
    }
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        if (mc_adbctl_select(&m->egret, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys) < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the Egret firmware (%s) is not in the roms folders",
                     keys[0] ? keys[0] : "?");
            mc_livadix_card_free(&m->card);
            free(m);
            return NULL;
        }
    }
    uint8_t *pram = mc_adbctl_pram(&m->egret);
    set_depth(m, pram);
    if (m->card_slot) set_card_depth(m, pram);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    mc_via_set_pa(&m->via1, VIA1_PA_IN, 0xFFu);
    mc_via_set_pb(&m->via1, 0xF7u, 0xF7u);
    /* The ROM and the overlay. */
    m->overlay_region = mc_bus_add_io(&hw->bus, MC_MDU_ROM, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, m->overlay_region, 0, 0x10000000u, m, &trap_ops, 0);
    overlay_on(m);
    mc_adbctl_ops.reset(&m->egret);
    egret_hold(m);
    return m;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((iisi *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((iisi *)board)->enet, frame, length);
}

static const mc_board_ops iisi_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive,
};

static const char *const iisi_models[] = {"iisi", NULL};
static const uint32_t iisi_roms[] = {0x36B7FB6Cu, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return MC_DEPTHS_UPTO(mc_rbv_max_depth(mc_rbv_sense_for(w, h)));
}
static const mc_hw_caps iisi_caps = {video_builtin, 1, MC_FEATURE_NUBUS, NULL};

/* The 68030 at 20 MHz, no FPU on the board (Developer Note, chapter 1). */
const mc_hw_desc mc_iisi_desc = {
    "iisi", iisi_models, M68K_68030, M68K_FPU_NONE, iisi_roms, iisi_ram,
    iisi_map, sizeof iisi_map / sizeof *iisi_map, iisi_irqs, sizeof iisi_irqs / sizeof *iisi_irqs,
    build, &iisi_ops, MC_MMU_030, NULL, &iisi_caps,
};
