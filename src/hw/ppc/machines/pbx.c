/* Livadix — PBX PowerBooks, still in bring-up.
 * Apple PowerBook Duo 2300c and PowerBook 1400 Developer Notes, ch. 2-3;
 * original ROMs 83C54F75 and 83A21950. No catalogue availability until
 * the native display, power and Finder acceptance checks pass. */
#include "../ppc_machine.h"
#include "../chips/pbx.h"
#include "../chips/cpuid.h"
#include "../../asic/pratt.h"
#include "../../asc.h"
#include "../../pmu.h"
#include "../../via.h"
#include "../../scc.h"
#include "../../video/csc.h"
#include "../../ncr5380.h"
#include "../../scsi_target.h"
#include "../../ata.h"
#include "../../swim.h"
#include "../../storage/vdisk.h"
#include "../../guest/guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PBX_CD_ID = 3, PBX_TARGETS = 7 };

typedef struct pbx_machine {
    mc_ppc *m;
    mc_pbx memory;
    mc_whitney whitney;
    mc_asc singer;
    mc_cpuid id[2];
    mc_via via1, via2;
    mc_pratt pratt;
    mc_pmu pmu;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_scc scc;
    mc_serial_host serial;
    mc_csc csc;
    mc_ncr5380 scsi;
    mc_scsi_bus scsi_bus;
    mc_scsi_unit targets[PBX_TARGETS];
    int scsi_drive[PBX_TARGETS];
    mc_ata ata, bay;
    mc_swim swim;
    mc_vdisk *disk, *bay_cd;
    uint8_t vram[1024u * 1024u], via2x[0x200], board_control[8];
    uint16_t ide_ctl, bay_ctl;
    int duo, atapi_bay, floppy_bay, ata_irq, bay_irq, powered, resetting, in_reset;
    int64_t host_start;
    uint64_t fb_serial, lcd_frame_ack;
} pbx_machine;

static void reset(void *board);
static void update_irq(pbx_machine *p) {
    mc_whitney_set_ipl(&p->whitney, p->scc.irq ? 4u : p->via2.irq_out ? 2u : p->via1.irq_out ? 1u : 0u);
}
static uint8_t slots(const pbx_machine *p) {
    /* Original Duo ROM's ATA Manager registers slot 9, not slot C.
     * $400474D0 maps Whitney PA0 to slot 9 and PA3 to slot C. */
    return p->ata_irq || p->bay_irq ? (p->duo ? 0xFEu : 0xF7u) : 0xFFu;
}
static uint8_t pmu_shift(void *ctx, uint8_t input) {
    return mc_via_shift8(&((pbx_machine *)ctx)->via1, input);
}
static void line(void *board, void *chip, int signal, int state) {
    pbx_machine *p = board;
    if (chip == &p->whitney && signal == MC_WHITNEY_LINE_CPU) mc_ppc_set_interrupt(p->m, state);
    else if ((chip == &p->via1 || chip == &p->via2) && signal == MC_VIA_LINE_IRQ) update_irq(p);
    else if (chip == &p->via1 && signal == MC_VIA_LINE_PA)
        mc_swim_set_headsel(&p->swim, (state & 0x20) != 0);
    else if (chip == &p->singer && signal == MC_ASC_LINE_IRQ) mc_via_set_cb1(&p->via2, !state);
    else if (chip == &p->scc && signal == MC_SCC_LINE_IRQ) update_irq(p);
    else if (chip == &p->via2 && signal == MC_VIA_LINE_PA) mc_pmu_set_port(&p->pmu, (uint8_t)state);
    else if ((chip == &p->ata || chip == &p->bay) && signal == MC_ATA_LINE_INTRQ) {
        if (chip == &p->ata) p->ata_irq = state; else p->bay_irq = state;
        mc_via_set_ca1(&p->via2, !(p->ata_irq || p->bay_irq));
    } else if (chip == &p->scsi) {
        if (signal == MC_NCR5380_LINE_IRQ) mc_via_set_cb2(&p->via2, !state);
        else if (signal == MC_NCR5380_LINE_DRQ) mc_via_set_ifr(&p->via2, MC_VIA_IRQ_CA2, state);
    } else if (chip == &p->pratt && signal == MC_PRATT_LINE_TICK) mc_via_set_ca1(&p->via1, state);
    else if (chip == &p->pmu) {
        if (signal == MC_PMU_LINE_ACK) mc_via_set_pb(&p->via2, state ? 2u : 0u, 2u);
        else if (signal == MC_PMU_LINE_DATA) mc_via_set_pa(&p->via2, state < 0 ? 0xFFu : (uint8_t)state, 0xFFu);
        else if (signal == MC_PMU_LINE_INT) {
            const uint8_t sr = p->via1.sr, count = p->via1.sr_count;
            mc_via_set_cb1(&p->via1, state);
            p->via1.sr = sr; p->via1.sr_count = count;
        } else if (signal == MC_PMU_LINE_POWER) {
            if (state == MC_PMU_POWER_ON) { p->powered = 1; p->m->reset_held = p->pmu.reset != 0; }
            else if (state == MC_PMU_POWER_SLEEP) p->m->reset_held = 1;
            else if (state == MC_PMU_POWER_OFF && p->powered && !p->resetting) mc_ppc_power_off(p->m);
        } else if (signal == MC_PMU_LINE_RESET && !p->resetting) {
            p->m->reset_held = state != 0 || mc_pmu_asleep(&p->pmu);
            if (state && !p->in_reset) {
                p->in_reset = 1;
                if (p->pmu.power == MC_PMU_POWER_SLEEP) {
                    /* Both ROMs' reset vector ($40300108) checks saved
                     * SRR1 and HID0 for the sleep return path. Wake is
                     * a 603e SRESET, which preserves the CPU and board
                     * state and disables the instruction cache
                     * (MPC603EUM 4.5.1.2). */
                    ppc_cpu *cpu = &p->m->cpu;
                    cpu->hid[0] &= ~0x00008000u; /* HID0[ICE] */
                    ppc_exception(cpu, PPC_EX_RESET, 0, cpu->pc);
                    cpu->srr1 &= 0xFFFFu;
                } else {
                    reset(p);
                    ppc_reset(&p->m->cpu);
                    ppc_jit_cpu_reset(p->m->jit);
                }
            } else if (!state) p->in_reset = 0;
        }
    }
}
static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    pbx_machine *p = dev;
    if (offset == 0x1E00u) return slots(p);
    if (offset && offset < 0x200u) return p->via2x[offset];
    uint32_t v = mc_via_ops.read(&p->via2, offset & ~0x1FFu, size);
    if (!(offset >> 9)) v = (v & ~4u) | (p->via2.orb & 4u);
    return v;
}
static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pbx_machine *p = dev;
    if (offset && offset < 0x200u) { p->via2x[offset] = (uint8_t)value; return; }
    offset &= ~0x1FFu;
    mc_via_ops.write(&p->via2, offset, size, value);
    if (!offset) mc_pmu_set_req(&p->pmu, (p->via2.orb & 4u) != 0);
}
static const mc_dev_ops via2_ops = {via2_read, via2_write, NULL, NULL, NULL};

/* ROM 83A21950 $402F814E acknowledges the frame latch in CONTROL bit 1
 * and polls for the next frame. This diagnostic latch fallback follows
 * the measured Mac OS 8.0 slot-0 callback cadence: 13,850 us on the Duo
 * (ROM 83C54F75 $402F9836) and one system tick on the 1400s. The ROM's
 * Time Manager dispatch is not evidence of physical LCD scan timing;
 * neither that timing nor an internal LCD interrupt route is verified. */
static uint64_t lcd_frame(const pbx_machine *p) {
    const uint64_t duo_period = (MC_C15M_HZ * 13850ull + 500000u) / 1000000u;
    return mc_ppc_now(p->m) / (p->duo ? duo_period : MC_PRATT_TICK_PERIOD);
}

static uint32_t lcd_ack_mask(uint32_t offset, unsigned size) {
    if (offset > MC_CSC_CONTROL || offset + size <= MC_CSC_CONTROL) return 0;
    return 2u << (8u * (offset + size - 1u - MC_CSC_CONTROL));
}

static uint32_t lcd_read(void *dev, uint32_t offset, unsigned size) {
    pbx_machine *p = dev;
    uint32_t v = mc_csc_ops.read(&p->csc, offset, size);
    const uint32_t mask = lcd_ack_mask(offset, size);
    if (mask && lcd_frame(p) != p->lcd_frame_ack) v |= mask;
    return v;
}

static void lcd_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pbx_machine *p = dev;
    const uint32_t mask = lcd_ack_mask(offset, size);
    if (value & mask) p->lcd_frame_ack = lcd_frame(p);
    mc_csc_ops.write(&p->csc, offset, size, value & ~mask);
}

static const mc_dev_ops lcd_ops = {lcd_read, lcd_write, NULL, NULL, NULL};

static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr5380_ops.read(dev, (offset >> 4) & 7u, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { mc_ncr5380_ops.write(dev, (offset >> 4) & 7u, size, value); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};
static uint32_t dma_read(void *dev, uint32_t offset, unsigned size) {
    pbx_machine *p = dev; uint32_t value = 0; (void)offset;
    mc_ncr5380_dma_read(&p->scsi, size, &value); return value;
}
static void dma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset; mc_ncr5380_dma_write(&((pbx_machine *)dev)->scsi, size, value);
}
static const mc_dev_ops dma_ops = {dma_read, dma_write, NULL, NULL, NULL};
/* Baboon, PowerBook 1400 Developer Note ch. 2 and Tables 4-6/7:
 * the bay drive is a master on its own task file. ROM 83A21950 uses
 * the same ATA/Baboon interface as the 190: task files at +0 and +$80,
 * interrupt bits 1 and 0 at +$D4 ($400AE650), and bay control at +$D8
 * ($400C314A). The PMU module query is $4D ($400C2C10). */
static uint32_t ide_read(void *dev, uint32_t offset, unsigned size) {
    pbx_machine *p = dev;
    if (offset < 0x40u) return mc_ata_ops.read(&p->ata, offset, size);
    if (!p->duo && offset >= 0x80u && offset < 0xC0u)
        return mc_ata_ops.read(&p->bay, offset - 0x80u, size);
    if (offset == 0xD0u) return p->ide_ctl;
    if (offset == 0xD4u) return (p->ata_irq ? 2u : 0u) | (p->bay_irq ? 1u : 0u);
    if (!p->duo && offset == 0xD8u) return p->bay_ctl;
    return 0;
}
static void ide_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pbx_machine *p = dev;
    if (offset < 0x40u) mc_ata_ops.write(&p->ata, offset, size, value);
    else if (!p->duo && offset >= 0x80u && offset < 0xC0u)
        mc_ata_ops.write(&p->bay, offset - 0x80u, size, value);
    else if (offset == 0xD0u) p->ide_ctl = (uint16_t)value;
    else if (!p->duo && offset == 0xD8u) p->bay_ctl = (uint16_t)value;
}
static const mc_dev_ops ide_ops = {ide_read, ide_write, NULL, NULL, NULL};
/* Both ROMs write $12 here at reset ($4030346C). The control bits'
 * external effects remain unknown; retain the register, not a success flag. */
static uint32_t control_read(void *dev, uint32_t offset, unsigned size) {
    pbx_machine *p = dev; uint32_t v = 0;
    if (offset >= sizeof p->board_control || size > sizeof p->board_control - offset) return 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | p->board_control[offset + i];
    return v;
}
static void control_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pbx_machine *p = dev;
    if (offset >= sizeof p->board_control || size > sizeof p->board_control - offset) return;
    for (unsigned i = 0; i < size; i++) p->board_control[offset + i] = (uint8_t)(value >> (8u * (size - i - 1u)));
}
static const mc_dev_ops control_ops = {control_read, control_write, NULL, NULL, NULL};
static mc_vdisk *make_disk(mc_disk *disk, char *error, size_t capacity) {
    size_t scsi_size, ata_size;
    const uint8_t *scsi = mc_guest_livadixdisk(&scsi_size), *ata = mc_guest_livadixdisk_ata(&ata_size);
    const mc_vdisk_drivers drivers = {scsi, scsi_size, ata, ata_size};
    return mc_vdisk_create(&disk, 1, &drivers, error, capacity);
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    pbx_machine *p = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    mc_media_eject(&p->m->media, MC_MEDIA_CDROM, 0);
}

static void bay_ejected(void *ctx, unsigned device, mc_vdisk *media) {
    pbx_machine *p = ctx;
    (void)device;
    mc_vdisk_destroy(media);
    p->bay_cd = NULL;
    mc_media_eject(&p->m->media, MC_MEDIA_CDROM, 0);
}

static int insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    pbx_machine *p = board;
    const mc_config *c = &p->m->config;
    int id = -1;
    if (!disk) return 0;
    if (kind == MC_MEDIA_FLOPPY) {
        if (p->floppy_bay) return mc_swim_insert(&p->swim, drive, disk, 0, error, capacity);
        if (error && capacity) snprintf(error, capacity, "No floppy module is fitted");
        return 0;
    }
    /* The 1400 accepts a CD in its bay or on the external HDI-30 bus
     * (Developer Note chapters 3 and 4). */
    const int bay_cd = kind == MC_MEDIA_CDROM && p->atapi_bay && drive == 0;
    if (kind == MC_MEDIA_CDROM && !p->duo && drive == 0) {
        if (!bay_cd) id = PBX_CD_ID;
    } else if (kind == MC_MEDIA_DISK) {
        const mc_disk_config *cfg = drive < c->disk_count ? &c->disks[drive] : NULL;
        if (!p->duo && cfg && cfg->bus == MC_BUS_SCSI) {
            if (cfg->scsi_id_set) id = cfg->scsi_id;
            else for (int k = 0; k < PBX_TARGETS; k++)
                if (k != PBX_CD_ID && p->scsi_drive[k] < 0) { id = k; break; }
            if (id < 0 || id >= PBX_TARGETS || id == PBX_CD_ID || p->scsi_drive[id] >= 0) {
                if (error && capacity) snprintf(error, capacity, "Unavailable PowerBook 1400 SCSI disk ID %d", id);
                return 0;
            }
        } else if (drive != 0) {
            if (error && capacity) snprintf(error, capacity, "PBX supports one internal ATA disk in drive 0");
            return 0;
        }
    } else {
        if (error && capacity) snprintf(error, capacity, "Unsupported PBX media drive");
        return 0;
    }
    mc_vdisk *next = make_disk(disk, error, capacity);
    if (!next) return 0;
    if (bay_cd) {
        mc_vdisk_destroy(mc_ata_cd_remove(&p->bay, 0));
        p->bay_cd = NULL;
        if (!mc_ata_cd_insert(&p->bay, 0, next)) { mc_vdisk_destroy(next); return 0; }
        p->bay_cd = next;
    } else if (id == PBX_CD_ID) {
        mc_vdisk_destroy(mc_scsi_unit_remove(&p->targets[id]));
        mc_scsi_cd_insert(&p->targets[id], next);
        mc_scsi_bus_attach(&p->scsi_bus, id, mc_scsi_unit_target(&p->targets[id]));
    } else if (id >= 0) {
        p->scsi_drive[id] = (int)drive;
        mc_scsi_disk_init(&p->targets[id], next);
        mc_scsi_bus_attach(&p->scsi_bus, id, mc_scsi_unit_target(&p->targets[id]));
    } else {
        mc_ata_attach_disk(&p->ata, 0, next);
        mc_vdisk_destroy(p->disk);
        p->disk = next;
    }
    return 1;
}
static void drop_target(pbx_machine *p, int id) {
    mc_scsi_bus_attach(&p->scsi_bus, id, NULL);
    mc_vdisk_destroy(mc_scsi_unit_remove(&p->targets[id]));
    p->scsi_drive[id] = -1;
}
static void poll(void *board) {
    pbx_machine *p = board;
    for (unsigned i = 0; i < p->swim.drives; i++) {
        int ejected = 0, gone = 0;
        mc_disk *image = mc_swim_media(&p->swim, i, &ejected);
        if (!image) continue;
        mc_disk *media = mc_media_get(&p->m->media, MC_MEDIA_FLOPPY, i, &gone);
        if (ejected || image != media || gone) {
            mc_swim_remove(&p->swim, i);
            if (ejected && image == media && !gone) mc_media_eject(&p->m->media, MC_MEDIA_FLOPPY, i);
        }
    }
    int gone = 0;
    mc_disk *disk = mc_media_get(&p->m->media, MC_MEDIA_DISK, 0, &gone);
    if (p->disk && (!disk || gone)) {
        mc_ata_attach_disk(&p->ata, 0, NULL);
        mc_vdisk_destroy(p->disk);
        p->disk = NULL;
    }
    if (p->bay_cd && (!mc_media_get(&p->m->media, MC_MEDIA_CDROM, 0, &gone) || gone)) {
        mc_vdisk_destroy(mc_ata_cd_remove(&p->bay, 0));
        p->bay_cd = NULL;
    }
    for (int id = 0; id < PBX_TARGETS; id++) {
        if (id != PBX_CD_ID && p->scsi_drive[id] < 0) continue;
        if (!p->targets[id].media) continue;
        const int cd = id == PBX_CD_ID;
        disk = mc_media_get(&p->m->media, cd ? MC_MEDIA_CDROM : MC_MEDIA_DISK, cd ? 0u : (unsigned)p->scsi_drive[id], &gone);
        if (!disk || gone) {
            if (cd) mc_vdisk_destroy(mc_scsi_unit_remove(&p->targets[id]));
            else drop_target(p, id);
        }
    }
}
static int framebuffer(void *board, mc_framebuffer *fb) {
    pbx_machine *p = board; mc_csc_view v;
    if (mc_pmu_asleep(&p->pmu) || !(p->csc.reg[MC_CSC_CONTROL] & 0x0Cu) ||
        !mc_csc_view_get(&p->csc, &v)) return 0;
    const unsigned mode = p->csc.reg[MC_CSC_DEPTH] & 7u;
    if (mode > 4u) return 0;
    /* 83A21950 programs $12=3 and a real 800x600, 800-byte PixMap.
     * PBX CSC/ECSC extends the legacy depth selector to RGB555 (4).
     * The Duo's 16-bit mode uses its documented 640x400 viewport. */
    fb->width = p->duo ? 640u : 800u;
    fb->height = p->duo ? (mode == 4u ? 400u : 480u) : 600u;
    fb->depth = 1u << mode; fb->row_bytes = fb->width * fb->depth / 8u;
    fb->pixels = v.pixels;
    fb->palette = mode == 4u ? NULL : v.palette;
    fb->serial = ++p->fb_serial;
    return 1;
}
static int key(void *board, uint8_t code, int down) {
    pbx_machine *p = board;
    /* Apple PowerBook 1400 and Duo 2300c manuals: press a key other
     * than Caps Lock to wake. A key release does not wake the PMU. */
    if (down && code != 0x39 && mc_pmu_asleep(&p->pmu)) mc_pmu_wake(&p->pmu);
    return mc_adb_keyboard_key(&p->keyboard, code, down);
}
static int mouse(void *board, int dx, int dy, int button) {
    pbx_machine *p = board;
    /* The built-in trackpad button does not wake the PowerBook. */
    mc_adb_mouse_move(&p->mouse, dx, dy, button); return 1;
}
static void set_clock(void *board, int64_t seconds) {
    pbx_machine *p = board; p->host_start = seconds;
    if (!mc_pmu_nvram_load(&p->pmu, p->m->config.nvram, seconds))
        mc_pmu_set_seconds(&p->pmu, (uint32_t)(seconds + 2082844800LL));
}
static uint32_t audio_rate(void *board) {
    pbx_machine *p = board;
    return p->m->config.sound ? mc_asc_rate(&p->singer) : 0u;
}
static size_t audio_pull(void *board, int16_t *stereo, size_t frames) {
    pbx_machine *p = board; const size_t count = mc_asc_pull(&p->singer, stereo, frames);
    if (!p->m->config.sound) memset(stereo, 0, count * 2u * sizeof *stereo);
    else if (p->duo) for (size_t i = 0; i < count; i++) stereo[2 * i + 1] = stereo[2 * i];
    mc_ppc_audio_startup(p->m, stereo, count, p->singer.played != 0,
                         !mc_asc_fifo_level(&p->singer, 0) && !mc_asc_fifo_level(&p->singer, 1) && !p->singer.ring_count);
    return count;
}
static void audio_source(void *board, const mc_audio_source *source) {
    pbx_machine *p = board;
    mc_asc_set_audio_source(&p->singer, source);
}
static void transmit(void *ctx, int channel, uint8_t byte) {
    pbx_machine *p = ctx;
    if (p->serial.transmit) p->serial.transmit(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void outputs(void *ctx, int channel, unsigned value) {
    pbx_machine *p = ctx;
    if (p->serial.outputs) p->serial.outputs(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (value & MC_SCC_OUT_DTR) != 0);
}
static void serial_host(void *board, const mc_serial_host *host) {
    pbx_machine *p = board;
    if (host) p->serial = *host; else memset(&p->serial, 0, sizeof p->serial);
}
static size_t receive(void *board, unsigned port, const uint8_t *bytes, size_t count) {
    return port < 2 ? mc_scc_receive(&((pbx_machine *)board)->scc, port ? MC_SCC_B : MC_SCC_A, bytes, count) : count;
}
static void modem(void *board, unsigned port, int dcd, int cts) {
    if (port >= 2) return;
    mc_scc *s = &((pbx_machine *)board)->scc; const int ch = port ? MC_SCC_B : MC_SCC_A;
    mc_scc_set_dcd(s, ch, dcd); mc_scc_set_cts(s, ch, cts);
}
static void reset(void *board) {
    pbx_machine *p = board; p->resetting = 1;
    p->m->startup_sound_done = 0;
    mc_pbx_ops.reset(&p->memory); mc_whitney_irq_ops.reset(&p->whitney);
    mc_asc_ops.reset(&p->singer);
    /* The PBX chime configures the 16-bit ports but leaves FIFO volumes
     * alone. Route A left and B right as on the portable sound block. */
    mc_asc_ops.write(&p->singer, 0xF06, 2, 0x7F00);
    mc_asc_ops.write(&p->singer, 0xF26, 2, 0x007F);
    mc_via_ops.reset(&p->via1); mc_via_ops.reset(&p->via2); mc_scc_ops.reset(&p->scc);
    mc_ncr5380_ops.reset(&p->scsi); mc_ata_reset(&p->ata); mc_swim_ops.reset(&p->swim);
    if (!p->duo) mc_ata_reset(&p->bay);
    mc_csc_ops.reset(&p->csc); p->lcd_frame_ack = lcd_frame(p);
    mc_pratt_ops.reset(&p->pratt);
    memset(p->via2x, 0, sizeof p->via2x); memset(p->board_control, 0, sizeof p->board_control);
    p->ata_irq = p->bay_irq = 0; p->ide_ctl = p->bay_ctl = 0;
    p->id[0].bus = p->id[0].value; p->id[1].bus = p->id[1].value;
    mc_via_set_pa(&p->via1, 0xFF, 0xFF); mc_via_set_pa(&p->via2, 0xFF, 0xFF); mc_via_set_pb(&p->via2, 0xFF, 0xFF);
    mc_via_set_cb2(&p->via2, 1); mc_via_set_cb1(&p->via2, 1); mc_via_set_ca1(&p->via2, 1);
    p->m->cpu.hid[1] = (p->duo ? 8u : p->m->model && p->m->model->cpu_hz > 125000000u ? 2u : 14u) << 28;
    update_irq(p); p->resetting = 0;
}
static void destroy(void *board) {
    pbx_machine *p = board;
    mc_asc_set_audio_source(&p->singer, NULL);
    mc_pmu_nvram_save(&p->pmu, p->m->config.nvram, p->host_start + (int64_t)(mc_ppc_now(p->m) / MC_C15M_HZ));
    mc_swim_flush(&p->swim);
    mc_ata_attach_disk(&p->ata, 0, NULL); mc_vdisk_destroy(p->disk);
    if (p->bay_cd) mc_vdisk_destroy(mc_ata_cd_remove(&p->bay, 0));
    for (int id = 0; id < PBX_TARGETS; id++) drop_target(p, id);
    mc_scsi_bus_free(&p->scsi_bus);
    mc_pmu_free(&p->pmu); free(p);
}
static void *build(mc_ppc *m) {
    pbx_machine *p = calloc(1, sizeof *p);
    if (!p) { snprintf(m->error, sizeof m->error, "out of memory for PBX board"); return NULL; }
    p->m = m; m->board = p; p->duo = m->desc == &mc_pbx2300_desc;
    p->atapi_bay = !p->duo && m->config.cdrom_bus == MC_BUS_ATA;
    p->floppy_bay = !p->duo && !p->atapi_bay && m->config.floppy_count && !m->config.floppy_off[0];
    const mc_hw_host *h = &m->host;
    mc_whitney_init(&p->whitney, h);
    /* The ROM and Sound Manager use the EASC FIFO registers and the
     * portable 16-bit ports. FIFO B enable ($F29) reaches VIA2 CB1;
     * ROM $4009012E masks it during service before the driver refills it. */
    mc_asc_init(&p->singer, h, MC_ASC_ENHANCED);
    p->singer.dev.name = "Singer";
    p->singer.block = 1;
    p->singer.singer = 1;
    mc_via_init(&p->via1, h, "VIA1", 9); mc_via_init(&p->via2, h, "VIA2", 9);
    mc_scc_init(&p->scc, h); const mc_scc_port serial = {p, transmit, outputs}; mc_scc_set_port(&p->scc, &serial);
    mc_scsi_bus_init(&p->scsi_bus); mc_ncr5380_init(&p->scsi, h, &p->scsi_bus, 1);
    for (int id = 0; id < PBX_TARGETS; id++) p->scsi_drive[id] = -1;
    mc_scsi_cd_init(&p->targets[PBX_CD_ID]);
    mc_scsi_unit_on_eject(&p->targets[PBX_CD_ID], cd_ejected, p);
    mc_ata_init(&p->ata, h, 2); mc_swim2_init(&p->swim, h, p->atapi_bay ? 0 : 1);
    if (!p->duo) mc_ata_init(&p->bay, h, 2);
    if (p->atapi_bay) {
        p->bay.dev.name = "ATA (bay)";
        mc_ata_attach_cd(&p->bay, 0);
        mc_ata_on_eject(&p->bay, bay_ejected, p);
    }
    mc_csc_init(&p->csc, h, p->duo ? "CSC" : "ECSC", p->vram, 0);
    /* ROM 83A21950 $402F7314 masks the ID with $F0; zero selects
     * legacy CSC and suppresses the ECSC's 16-bit modes. Keep the
     * low nibble and expose the ECSC's nonzero revision field. */
    p->csc.id = p->duo ? 1 : 0x11;
    mc_adb_bus_init(&p->adb); mc_adb_keyboard_init(&p->keyboard); mc_adb_mouse_init(&p->mouse);
    mc_adb_attach(&p->adb, &p->keyboard.dev); mc_adb_attach(&p->adb, &p->mouse.dev);
    mc_pmu_init(&p->pmu, h, "PMU", MC_PMU_99, MC_PMU_SHIFT, &p->adb, NULL, 0);
    mc_pmu_set_shift(&p->pmu, pmu_shift, p);
    mc_pmu_set_pbx_lcd(&p->pmu);
    /* The PMU reply is not the raw connector pin value. ROM 83A21950
     * $400C2BC2/$400C2BCE maps PMU $4D types 2 and 3 to a floppy
     * and an IDE bay device. Its .Sony driver uses the SWIM register
     * interface at $50F16000 ($4018E1A6 and following). */
    if (p->atapi_bay || p->floppy_bay) mc_pmu_set_bay(&p->pmu, p->atapi_bay ? 3 : 2);
    mc_pmu_set_counts(&p->pmu, m->rom + (p->duo ? 0x3D370u : 0x3D8B6u));
    mc_pratt_init(&p->pratt, h, "Whitney tick", 0);
    if (!mc_pbx_init(&p->memory, h, &m->bus, m->ram, m->ram_size >> 20, p->duo)) {
        snprintf(m->error, sizeof m->error, "invalid PBX RAM population"); goto fail;
    }
    mc_cpuid_init(&p->id[0], h, "CPU ID low", p->duo ? 0x5AA50004u : 0x5AA50007u);
    mc_cpuid_init(&p->id[1], h, "CPU ID high", p->duo ? 0xA55A1809u : 0xA55A180Au);
    const int io = mc_ppc_region(m, "io"); const mc_ppc_port port = {MC_PPC_PORT_DIRECT, 4, 0, 0};
#define ATTACH(at, length, chip, ops) if (!mc_ppc_attach(m, io, at, length, chip, ops, port)) goto fail
    ATTACH(0xF00000, 0x2000, &p->via1, &mc_via_ops);
    ATTACH(0xF02000, 0x2000, p, &via2_ops);
    ATTACH(0xF04000, 0x2000, &p->scc, &mc_scc_ops);
    ATTACH(0xF06000, 0x2000, p, &dma_ops);
    ATTACH(0xF0A000, 0x2000, &p->pratt, &mc_pratt_ops);
    ATTACH(0xF10000, 0x2000, &p->scsi, &scsi_ops);
    ATTACH(0xF14000, 0x2000, &p->singer, &mc_asc_ops);
    if (p->floppy_bay) {
        /* PowerBook 1400 Developer Note ch. 2: the PBX I/O bus runs at
         * 25.175 MHz with 68030 cycles. A byte access costs at least three
         * clocks (MC68030UM, Asynchronous Operation); round up to two scheduler ticks.
         * .Sony's MFM polling counts expire before the next 16-us byte
         * if the controller is given zero I/O bus time. */
        const int swim_io = mc_bus_add_io(&m->bus, 0x50F16000u, 0x2000u, 0x1FFFu, 0);
        if (!swim_io || !mc_ppc_attach(m, swim_io, 0, 0x2000, &p->swim, &mc_swim_ops, port)) goto fail;
        m->bus.region[swim_io].cost = (3u * MC_C15M_HZ + 25175000u - 1u) / 25175000u;
    } else {
        ATTACH(0xF16000, 0x2000, &p->swim, &mc_swim_ops);
    }
    ATTACH(0xF1A000, 0x2000, p, &ide_ops);
    ATTACH(0xF20000, 0x1000, p, &lcd_ops);
    ATTACH(0xF2A000, 4, &p->whitney, &mc_whitney_irq_ops);
    ATTACH(0xF80000, 0x20, &p->memory, &mc_pbx_ops);
    ATTACH(0xF96000, 8, p, &control_ops);
    if (!mc_bus_attach(&m->bus, io, 0xFFFFFF8, 4, &p->id[0], &mc_cpuid_ops, 0) ||
        !mc_bus_attach(&m->bus, io, 0xFFFFFFC, 4, &p->id[1], &mc_cpuid_ops, 0)) goto fail;
#undef ATTACH
    mc_bus_map_memory(&m->bus, 0x60000000u, sizeof p->vram, p->vram, sizeof p->vram, 1);
    reset(p);
    /* A CPU reset leaves the PMU running; a cold board starts its
     * housekeeping clock and holds the CPU through power-up. */
    p->resetting = 1; mc_pmu_ops.reset(&p->pmu); p->resetting = 0;
    m->reset_held = p->pmu.reset != 0;
    return p;
fail:
    if (!m->error[0]) snprintf(m->error, sizeof m->error, "could not attach PBX device");
    destroy(p); m->board = NULL; return NULL;
}
static const mc_board_ops ops = {.reset=reset, .line=line, .framebuffer=framebuffer, .key=key, .mouse=mouse,
    .set_clock=set_clock, .destroy=destroy, .audio_rate=audio_rate, .audio_pull=audio_pull, .audio_source=audio_source,
    .insert=insert, .poll=poll, .serial_host=serial_host, .serial_receive=receive, .serial_modem=modem};
static unsigned builtin(const mc_model *model, unsigned width, unsigned height) {
    if (!model) return 0;
    if (!strcmp(model->key, "powerbook-duo-2300c")) {
        if (width != 640) return 0;
        if (height == 400) return MC_DEPTH_BIT(16);
        return height == 480 ? MC_DEPTHS_UPTO(8) : 0;
    }
    return width == 800 && height == 600 ? MC_DEPTHS_UPTO(16) : 0;
}
static const mc_hw_caps duo_caps = {.builtin=builtin,
    .features=MC_FEATURE_ATA_DISK | MC_FEATURE_FIXED_SCREEN | MC_FEATURE_NO_FLOPPY};
static const mc_hw_caps pb1400_caps = {.builtin=builtin,
    .features=MC_FEATURE_ATA_DISK | MC_FEATURE_ATA_CD | MC_FEATURE_MEDIA_BAY | MC_FEATURE_FIXED_SCREEN};
static const mc_region map[] = {
    {"ram", 0, 0x4000000, 0, MC_REGION_RAM},
    {"rom-low", 0x30000000u, 0x10000000u, 0, MC_REGION_ROM},
    {"rom-alias", 0x40000000u, 0x10000000u, 0, MC_REGION_ROM},
    {"io", 0x50000000u, 0x10000000u, 0xFFFFFFFu, MC_REGION_IO},
    {"rom", 0xFF000000u, 0x1000000u, 0, MC_REGION_ROM},
};
static const uint32_t duo_roms[] = {0x83C54F75u, 0}, pb1400_roms[] = {0x83A21950u, 0};
static const uint16_t duo_ram[] = {8, 12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 0};
static const uint16_t pb1400_ram[] = {12, 16, 20, 24, 28, 32, 36, 40, 44, 48, 52, 56, 60, 64, 0};
static const char *const duo_models[] = {"powerbook-duo-2300c", NULL};
static const char *const c_models[] = {"powerbook-1400", NULL}, *const cs_models[] = {"powerbook-1400cs", NULL};
#define DESC(key_, models_, name_, hz_, roms_, ram_, caps_) { .key=key_, .models=models_, .name=name_, .cpu=PPC_MODEL_603E, \
    .cpu_hz=hz_, .timebase_hz=8333333u, .cpi=8, .cpi_div=9, .roms=roms_, .ram_mb=ram_, \
    .map=map, .nmap=sizeof map / sizeof *map, .unmapped=MC_PPC_UNMAPPED_TEA, .build=build, .ops=&ops, .caps=caps_ }
const mc_ppc_desc mc_pbx2300_desc = DESC("pbx2300", duo_models, "PowerBook Duo 2300c", 100000000u, duo_roms, duo_ram, &duo_caps);
const mc_ppc_desc mc_pbx1400c_desc = DESC("pbx1400c", c_models, "PowerBook 1400c", 133000000u, pb1400_roms, pb1400_ram, &pb1400_caps);
const mc_ppc_desc mc_pbx1400cs_desc = DESC("pbx1400cs", cs_models, "PowerBook 1400cs", 117000000u, pb1400_roms, pb1400_ram, &pb1400_caps);
