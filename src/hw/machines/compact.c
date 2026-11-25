/* Livadix — the Macintosh 128K, 512K, 512Ke and Plus boards.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990): chapter
 * 3, the Macintosh Plus's address map (RAM from 0 through the 4 MB space it
 * repeats in; ROM at $40 0000; the 5380 at $58 0000, read at even and
 * written at odd addresses, its registers every $10, A9 selecting the
 * pseudo-DMA DACK; the SCC read at $9F FFF8 and written at $BF FFF9, A1
 * the channel, A2 data or control; the IWM at $DF E1FF, its sixteen state
 * addresses every $200 on the low byte; the VIA at $EF E1FE, its registers
 * every $200 on the high byte; after reset the overlay puts the ROM at 0
 * and the RAM at $60 0000 until VIA PA4 clears it; no bus error: an
 * address nothing decodes completes); the interrupt lines (the VIA level
 * 1, the SCC level 2, the SCC's taking the VIA's off /IPL0 so that both
 * at once are level 2: ROM 4D1F8172's level-3 vector is a bare RTE, ROM
 * offset $1AB4);
 * chapter 4 (VIA port A: PA7 the SCC's /W//REQ, PA6 the main screen
 * buffer, PA5 the floppy drives' SEL, PA4 the overlay, PA3 the main sound
 * buffer, PA2-PA0 the volume; port B: PB7 the sound off, PB6 the
 * horizontal blanking, PB5/PB4 the mouse's Y2 and X2, PB3 the mouse
 * button, PB2-PB0 the clock chip's enable, clock and data; CA1 vertical
 * blanking, CA2 the clock's one-second interrupt, CB1/CB2 and the shift
 * register the keyboard); chapter 9 (the IWM with the 400K single-sided
 * drive of the 128K and 512K and the 800K drive of the 512Ke and Plus);
 * chapter 11 (the Plus's 5380, no interrupt line); chapters 12-14 (the
 * video and sound buffers, video/compactvideo.h; the keyboard, m0110.h;
 * the mouse, quadmouse.h). RAM: 128 KB and 512 KB soldered on the 128K and
 * 512K boards (the 512Ke is the 512K's board with the Plus's ROM); the
 * Plus's four SIMM sockets hold 1, 2, 2.5 or 4 MB, and the ROM finds the
 * size by where the RAM repeats.
 *
 * Observed from the ROMs: ROM 4D1F8172 reads a
 * diagnostic ROM's signature at $F8 0000 and the phase at $F0 0000 (both
 * nothing here: the ROM goes on), keeps the VIA's base at $1D4 and drives
 * the keyboard and mouse as m0110.h and quadmouse.h say; the cursor
 * globals MTemp $828, RawMouse $82C, CrsrNew $8CE and CrsrCouple $8CF are
 * the ones the VBL cursor task at ROM offset $1E86 reads, so
 * mc_machine_mouse_to (machine.c) places the cursor here as on later Macs.
 *
 * The processor is the 68000 model (m68k_000.c), its 24-bit bus an
 * identity map24 here (COMPACT_CPU, the one place it is chosen). Its bus
 * cycles wait (mc_compact_wait, the model's m68k_bus.wait): the video takes
 * every other RAM cycle while it fetches the visible part of a line (one
 * word each 16 dots; the processor's cycles of 4 clocks = 8 dots start
 * at the other slots), the RAM's cycles otherwise starting on a 4-clock
 * boundary (†: the slot phase); a VIA access and the interrupt
 * acknowledge (autovectored through VPA) wait for the E clock, a tenth of
 * the processor's (Guide, chapter 3 "Video and sound timing" and chapter
 * 4; MC68000 User's Manual, 6.4 and 7, the synchronous cycle).
 *
 * Livadix's own code. */
#include "../../core/nvram.h"
#include "compact.h"
#include "../guest/guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The processor (above). */
#define COMPACT_CPU M68K_68000

#define RAM_SPACE 0x400000u
#define ROM_BASE 0x400000u
#define OVERLAY_RAM 0x600000u
#define SCSI_BASE 0x580000u
#define SCC_READ 0x800000u
#define SCC_WRITE 0xA00000u
#define IWM_BASE 0xC00000u
#define VIA_BASE 0xE00000u

static const compact_board boards[] = {
    {MC_MODEL_BUILTIN, MC_DRIVE_400K, MC_M0110, 0},
    {"mac-128k", MC_DRIVE_400K, MC_M0110, 0},
    {"mac-512k", MC_DRIVE_400K, MC_M0110, 0},
    {"mac-512ke", MC_DRIVE_800K, MC_M0110A, 0},
    {"mac-plus", MC_DRIVE_800K, MC_M0110A, 1},
    {"mac-plus-v1", MC_DRIVE_800K, MC_M0110A, 1},
    {"mac-plus-v2", MC_DRIVE_800K, MC_M0110A, 1},
};

static const mc_region compact_map[] = {
    {"scc_r", SCC_READ, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"scc_w", SCC_WRITE, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"iwm", IWM_BASE, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"via", VIA_BASE, 0x100000u, 0xFFFFFu, MC_REGION_IO},
    {"scsi", SCSI_BASE, 0x80000u, 0x7FFFFu, MC_REGION_IO},
};

static const mc_irq_route compact_irqs[] = {
    {"via", 1}, {"scc", 2},
};

/* The 68000's 24-bit address bus: each MiB of the 24-bit space is itself. */
static const uint32_t compact_map24[16] = {
    0x000000u, 0x100000u, 0x200000u, 0x300000u, 0x400000u, 0x500000u, 0x600000u, 0x700000u,
    0x800000u, 0x900000u, 0xA00000u, 0xB00000u, 0xC00000u, 0xD00000u, 0xE00000u, 0xF00000u,
};

/* --- the memory map --------------------------------------------------------------- */

/* The RAM's decode (Guide, chapter 3): a whole power of two repeats
 * through the window; 2.5 MB is two rows, the 1 MB SIMMs' 2 MB where A21
 * is low and the 256 KB SIMMs' 512 KB, repeated, where it is high, so the
 * ROM's sizing (ROM 4D1F8172 offset $DA0: where a write in steps of 512 KB
 * first lands on an earlier one) finds 2.5 MB from 0 and the screen at the
 * top of the 4 MB space ($3F A700) lies in the second row. */
void mc_compact_map_ram(compact *m, uint32_t base, uint32_t size) {
    mc_hw *hw = m->hw;
    const uint32_t n = m->ram_size;
    if (!(n & (n - 1u))) {
        mc_bus_map_memory(&hw->bus, base, size, hw->ram, n, 1);
        return;
    }
    uint32_t row_a = 1;
    while (row_a * 2u <= n) row_a *= 2u;
    for (uint32_t off = 0; off < size; off += row_a) {
        if ((off / row_a) & 1u) mc_bus_map_memory(&hw->bus, base + off, row_a, hw->ram + row_a, n - row_a, 1);
        else mc_bus_map_memory(&hw->bus, base + off, row_a, hw->ram, row_a, 1);
    }
}

static void map_ram(compact *m) {
    mc_hw *hw = m->hw;
    if (m->overlay) {
        mc_bus_map_memory(&hw->bus, 0, RAM_SPACE, hw->rom, hw->rom_size, 0);
        mc_compact_map_ram(m, OVERLAY_RAM, 0x200000u);
    } else {
        mc_compact_map_ram(m, 0, RAM_SPACE);
        mc_bus_map_float(&hw->bus, OVERLAY_RAM, 0x200000u);
    }
    mc_hw_maps_changed(hw);
}

static void map_fixed(compact *m) {
    mc_hw *hw = m->hw;
    /* The 128K, 512K and 512Ke repeat their ROM through $40 0000-$4F FFFF;
     * the Plus decodes it in $40 0000-$43 FFFF only, and ROM 4D1F8172 takes
     * $42 0000 and $44 0000 reading differently for the Plus's SCSI
     * (ROM offset $3E4). */
    if (m->board->scsi) {
        mc_bus_map_memory(&hw->bus, ROM_BASE, 0x40000u, hw->rom, hw->rom_size, 0);
        mc_bus_map_float(&hw->bus, 0x440000u, 0x140000u);
    } else {
        mc_bus_map_memory(&hw->bus, ROM_BASE, 0x100000u, hw->rom, hw->rom_size, 0);
        mc_bus_map_float(&hw->bus, 0x500000u, 0x100000u);
    }
    mc_bus_map_float(&hw->bus, 0xF00000u, 0x100000u);
}

static void set_overlay(compact *m, int on) {
    on = on != 0;
    if (on == m->overlay) return;
    m->overlay = (uint8_t)on;
    map_ram(m);
}

/* --- byte lanes --------------------------------------------------------------------- */

static uint32_t lane_read(void *dev, uint32_t offset, unsigned size) {
    lane_port *p = dev;
    if (!p->reads) return 0xFFu;
    if (size == 1) {
        if (p->lane >= 0 && (int)(offset & 1u) != p->lane) return 0xFFu;
        return p->ops->read(p->dev, offset, 1) & 0xFFu;
    }
    uint32_t v = 0;
    for (unsigned w = 0; w < size; w += 2) {
        const uint32_t at = (offset & ~1u) + w + (p->lane > 0 ? 1u : 0u);
        const uint32_t b = p->ops->read(p->dev, at, 1) & 0xFFu;
        v = v << 16 | b << 8 | b;
    }
    return v;
}

static void lane_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lane_port *p = dev;
    if (!p->writes) return;
    if (size == 1) {
        if (p->lane >= 0 && (int)(offset & 1u) != p->lane) return;
        p->ops->write(p->dev, offset, 1, value & 0xFFu);
        return;
    }
    for (unsigned w = 0; w < size; w += 2) {
        const uint32_t at = (offset & ~1u) + w + (p->lane > 0 ? 1u : 0u);
        const uint32_t word = value >> (8u * (size - 2u - w)) & 0xFFFFu;
        p->ops->write(p->dev, at, 1, p->lane > 0 ? word & 0xFFu : word >> 8);
    }
}

static int lane_stable(void *dev, uint32_t offset, unsigned size) {
    lane_port *p = dev;
    if (!p->reads) return 1;
    if (!p->ops->stable) return 0;
    if (size == 1 && p->lane >= 0 && (int)(offset & 1u) != p->lane) return 1;
    return p->ops->stable(p->dev, (offset & ~1u) + (p->lane > 0 ? 1u : 0u), 1);
}

const mc_dev_ops mc_compact_lane_ops = {lane_read, lane_write, NULL, NULL, lane_stable};

/* The VIA: a write to the shift register is the keyboard's cue; a read of
 * port B finds PB6 as the video's horizontal blanking leaves it at that
 * moment (compactvideo.h). */
static uint32_t via_read(void *dev, uint32_t offset, unsigned size) {
    lane_port *p = dev;
    if (((offset >> 9) & 15u) == MC_VIA_ORB)
        mc_via_set_pb(&p->m->via, mc_compactvideo_hblank(&p->m->video, mc_dev_now(&p->m->via.dev)) ? 0x40u : 0, 0x40u);
    return lane_read(dev, offset, size);
}
static void via_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    lane_port *p = dev;
    lane_write(dev, offset, size, value);
    if (((offset >> 9) & 15u) == MC_VIA_SR) mc_m0110_sr_written(&p->m->keyboard);
}
static int via_stable(void *dev, uint32_t offset, unsigned size) {
    return ((offset >> 9) & 15u) == MC_VIA_ORB ? 0 : lane_stable(dev, offset, size);
}
static const mc_dev_ops via_lane_ops = {via_read, via_write, NULL, NULL, via_stable};

/* The Plus's 5380: registers every $10, A9 the pseudo-DMA DACK (each
 * access moves a byte at once, as macii.c's). */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) {
    compact *m = dev;
    (void)size;
    if (offset & 0x200u) {
        uint32_t v = 0;
        mc_ncr5380_dma_read(&m->scsi, 1, &v);
        return v;
    }
    return mc_ncr5380_ops.read(&m->scsi, (offset >> 4) & 7u, 1);
}
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    compact *m = dev;
    (void)size;
    if (offset & 0x200u) mc_ncr5380_dma_write(&m->scsi, 1, value);
    else mc_ncr5380_ops.write(&m->scsi, (offset >> 4) & 7u, 1, value);
}
const mc_dev_ops mc_compact_scsi_ops = {scsi_read, scsi_write, NULL, NULL, NULL};

/* --- wiring ------------------------------------------------------------------------ */

static void set_irqs(compact *m) {
    mc_hw *hw = m->hw;
    mc_irq_set(&hw->irq, mc_hw_irq(hw, "via"), m->via_irq);
    mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), m->scc_irq);
}

static void via_port_a(compact *m, uint8_t pa) {
    mc_swim_set_headsel(&m->iwm, (pa & 0x20) != 0);
    set_overlay(m, pa & 0x10);
    mc_compactvideo_set_pa(&m->video, pa);
}

static void via_port_b(compact *m, uint8_t pb) {
    const int data = (m->via.ddrb & 1u) ? (pb & 1u) : 1;
    mc_rtc_pins(&m->rtc, pb & 4u, pb & 2u, data);
    mc_compactvideo_set_pb7(&m->video, pb & 0x80);
}

static void board_line(void *board, void *dev, int line, int state) {
    compact *m = board;
    if (dev == &m->via) {
        if (line == MC_VIA_LINE_IRQ) { m->via_irq = (uint8_t)(state != 0); set_irqs(m); }
        else if (line == MC_VIA_LINE_PA) via_port_a(m, (uint8_t)state);
        else if (line == MC_VIA_LINE_PB) via_port_b(m, (uint8_t)state);
    } else if (dev == &m->scc) {
        if (line == MC_SCC_LINE_IRQ) { m->scc_irq = (uint8_t)(state != 0); set_irqs(m); }
        /* /W//REQ on PA7, low when asserted. */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->via, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&m->via, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&m->via, state);
    } else if (dev == &m->video) {
        /* Vertical blanking: CA1 low while blanking. */
        if (line == MC_COMPACTVIDEO_LINE_VBL) mc_via_set_ca1(&m->via, !state);
    } else if (dev == &m->mouse) {
        switch (line) {
        case MC_QUADMOUSE_LINE_X1: mc_scc_set_dcd(&m->scc, MC_SCC_A, state); break;
        case MC_QUADMOUSE_LINE_Y1: mc_scc_set_dcd(&m->scc, MC_SCC_B, state); break;
        case MC_QUADMOUSE_LINE_X2: mc_via_set_pb(&m->via, state ? 0x10u : 0, 0x10u); break;
        case MC_QUADMOUSE_LINE_Y2: mc_via_set_pb(&m->via, state ? 0x20u : 0, 0x20u); break;
        case MC_QUADMOUSE_LINE_BUTTON: mc_via_set_pb(&m->via, state ? 0 : 0x08u, 0x08u); break;
        default: break;
        }
    }
    /* The Plus's 5380 has no interrupt line; the ROM polls it. */
}

/* --- persistence (macii.c's PRAM file) -------------------------------------------- */

static const char NVRAM_MAGIC[8] = {'L', 'V', 'D', 'X', 'P', 'R', 'A', 'M'};

void mc_compact_load_nvram(compact *m) {
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

static void save_nvram(compact *m) {
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

/* --- storage (the Plus's SCSI disks as macii.c lays them out) ---------------------- */

#define CD_ID MC_COMPACT_CD_ID
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const mc_vdisk_drivers drivers = {driver, size, NULL, 0};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(compact *m) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
        m->vdisk[id] = NULL;
    }
}

static int layout_disks(compact *m, char *error, size_t capacity) {
    const mc_config *c = &m->hw->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(m);
    if (!m->board->scsi) {
        for (unsigned i = 0; i < MC_CONFIG_DISKS; i++)
            if (mc_media_get(&m->hw->media, MC_MEDIA_DISK, i, NULL)) {
                if (capacity) snprintf(error, capacity, "The %s has no SCSI port", m->hw->model->name);
                return 0;
            }
        return 1;
    }
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
    compact *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    compact *m = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(m, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&m->iwm, drive, disk, 0, error, capacity);
    if (!m->board->scsi) {
        if (capacity) snprintf(error, capacity, "The %s has no SCSI port", m->hw->model->name);
        return 0;
    }
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
    compact *m = board;
    mc_media *media = &m->hw->media;
    for (unsigned i = 0; i < m->iwm.drives; i++) {
        int ejected = 0, gone = 0;
        mc_disk *chip = mc_swim_media(&m->iwm, i, &ejected);
        mc_disk *d = mc_media_get(media, MC_MEDIA_FLOPPY, i, &gone);
        if (!chip) continue;
        if (ejected || chip != d || gone) {
            mc_swim_flush(&m->iwm);
            mc_swim_remove(&m->iwm, i);
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
    compact *m = board;
    return m->hw->config.sound ? MC_COMPACTVIDEO_RATE : 0;
}

/* emulation.startup_sound off: the first two seconds (the start-up beep)
 * are silent. */
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    compact *m = board;
    const size_t n = mc_compactvideo_pull(&m->video, stereo, frames);
    if (!m->hw->config.startup_sound && mc_hw_now(m->hw) < 2ull * MC_C15M_HZ) memset(stereo, 0, n * 2 * sizeof *stereo);
    return n;
}

/* --- serial ports -------------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    compact *m = ctx;
    if (m->serial.transmit) m->serial.transmit(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    compact *m = ctx;
    if (m->serial.outputs) m->serial.outputs(m->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    compact *m = board;
    if (host) m->serial = *host;
    else memset(&m->serial, 0, sizeof m->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    compact *m = board;
    return port < 2 ? mc_scc_receive(&m->scc, port_channel(port), data, count) : count;
}
/* The DCD inputs carry the mouse: the host's modem lines reach CTS only. */
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    compact *m = board;
    (void)dcd;
    if (port < 2) mc_scc_set_cts(&m->scc, port_channel(port), cts);
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    compact *m = board;
    mc_via_ops.reset(&m->via);
    if (m->board->scsi) mc_ncr5380_reset(&m->scsi);
    mc_scc_ops.reset(&m->scc);
    mc_swim_ops.reset(&m->iwm);
    mc_compactvideo_ops.reset(&m->video);
    mc_m0110_ops.reset(&m->keyboard);
    /* The VIA's ports are inputs again: the overlay pin floats high. */
    via_port_a(m, 0xFF);
    via_port_b(m, 0xFF);
    mc_hw_set_addr24(m->hw, 1);
    /* The RESET instruction (the processor running) restarts the processor
     * too, as the reset switch does (Apple's Shutdown Manager source,
     * ShutDownMgr.a, "Some things to be aware of about the RESET
     * instruction": "On the Mac Plus and Mac SE, a RESET will cause the
     * 68000 to get a Reset, and it will switch into overlay mode, and
     * Re-Boot through the reset vector"; the 128K and 512K are the Plus's
     * board before it). Finder 4.1's Shut Down on these ROMs
     * ejects the disks and ends with RESET alone (on any ROM but MacWorks',
     * whose byte 9 is $FF), its code after it reading RAM the overlay now
     * hides; the 128K then starts again at its disk prompt. */
    if (m->hw->running) mc_hw_cpu_restart(m->hw);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    compact *m = board;
    fb->width = MC_COMPACTVIDEO_WIDTH;
    fb->height = MC_COMPACTVIDEO_HEIGHT;
    fb->depth = 1;
    fb->row_bytes = MC_COMPACTVIDEO_ROW_BYTES;
    fb->pixels = mc_compactvideo_pixels(&m->video);
    fb->palette = m->video.palette;
    fb->serial = ++m->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) { return mc_m0110_key(&((compact *)board)->keyboard, code, down); }

static int board_mouse(void *board, int dx, int dy, int button) {
    return mc_quadmouse_move(&((compact *)board)->mouse, dx, dy, button);
}

static void board_set_clock(void *board, int64_t host_seconds) {
    compact *m = board;
    m->host_start = host_seconds;
    mc_rtc_set_seconds(&m->rtc, (uint32_t)((int64_t)mc_rtc_seconds(&m->rtc) + host_seconds));
}

static void board_destroy(void *board) {
    compact *m = board;
    save_nvram(m);
    mc_swim_flush(&m->iwm);
    for (int id = 0; id < 7; id++) {
        if (!m->vdisk[id]) continue;
        mc_scsi_bus_attach(&m->scsi_bus, id, NULL);
        mc_vdisk_destroy(m->vdisk[id]);
    }
    mc_scsi_bus_free(&m->scsi_bus);
    free(m);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

void mc_compact_attach(compact *m, lane_port *p, const char *region, void *dev, const mc_dev_ops *ops, int lane, const mc_dev_ops *port_ops) {
    mc_hw *hw = m->hw;
    const int r = mc_hw_region(hw, region);
    p->m = m;
    p->dev = dev;
    p->ops = ops;
    p->lane = lane;
    p->reads = strcmp(region, "scc_w") != 0;
    p->writes = strcmp(region, "scc_r") != 0;
    hw->bus.region[r].unclaimed_ok = 1;
    /* Device cycles take their time as wait states (mc_compact_wait). */
    hw->bus.region[r].cost = 0;
    mc_bus_attach(&hw->bus, r, 0, hw->bus.region[r].size ? hw->bus.region[r].size : 0x100000u, p, port_ops, 0);
}

/* --- the 68000's wait states (above) ------------------------------------------------ */

/* Guest time (C15M periods) at the processor's clock cycle `cycle`. */
static uint64_t time_at(const mc_hw *hw, uint64_t cycle) {
    return hw->time + ((cycle - hw->cyc) * MC_C15M_HZ + hw->rem) / hw->rate;
}

#define SLOT 8u         /* a RAM cycle: 4 processor clocks, 8 dot clocks */
#define E_PERIOD 20u    /* the E clock: 10 processor clocks */

unsigned mc_compact_wait(void *board, uint32_t a, unsigned fc, unsigned kind, uint64_t cycle) {
    compact *m = board;
    mc_hw *hw = m->hw;
    (void)fc;
    const uint64_t t = time_at(hw, cycle);
    uint64_t wait = 0;
    const int ram = m->overlay ? (a >= OVERLAY_RAM && a < OVERLAY_RAM + 0x200000u) : a < RAM_SPACE;
    if (kind == 3 || (a >= VIA_BASE && a < 0xF00000u)) {
        /* VPA: the cycle starts with the E clock's next period and ends
         * with its fall (about 4 more clocks than a plain cycle). */
        wait = (E_PERIOD - t % E_PERIOD) % E_PERIOD + SLOT;
    } else if (ram) {
        const int64_t d = (int64_t)(t - m->video.frame);
        const uint64_t pos = (uint64_t)((d % (int64_t)MC_COMPACTVIDEO_FRAME + (int64_t)MC_COMPACTVIDEO_FRAME) % (int64_t)MC_COMPACTVIDEO_FRAME);
        const uint64_t line = pos / MC_COMPACTVIDEO_LINE, dot = pos % MC_COMPACTVIDEO_LINE;
        if (line < MC_COMPACTVIDEO_HEIGHT && dot < MC_COMPACTVIDEO_WIDTH) {
            /* The video's fetch takes the last of each group of `slots`
             * RAM cycles (the Plus's every other one; the SE's one in four,
             * machines/se.c). */
            const uint64_t slots = m->video_slots ? m->video_slots : 2u;
            const uint64_t phase = pos % (slots * SLOT);
            uint64_t next = (phase + SLOT - 1u) / SLOT * SLOT;
            if (next == (slots - 1u) * SLOT) next += SLOT;
            wait = next - phase;
        } else {
            wait = (SLOT - pos % SLOT) % SLOT;
        }
    }
    /* In processor clocks, rounded up. */
    return (unsigned)((wait * hw->rate + MC_C15M_HZ - 1) / MC_C15M_HZ);
}

/* The disk-speed PWM: the 400K drives' motors (compactvideo.h). */
static uint32_t disk_speed(void *ctx) { return mc_compactvideo_speed(&((compact *)ctx)->video); }

/* The mouse steps an axis once the SCC took its last edge (its
 * external/status latch open again, quadmouse.h). */
static int mouse_ready(void *ctx, int axis) {
    compact *m = ctx;
    return !m->scc.ch[axis ? MC_SCC_B : MC_SCC_A].latch;
}

static void *build(mc_hw *hw) {
    const compact_board *b = NULL;
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, hw->model->key)) b = &boards[i];
    if (!b) {
        snprintf(hw->error, sizeof hw->error, "No compact Macintosh board for %s", hw->model->name);
        return NULL;
    }
    compact *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    m->hw = hw;
    m->board = b;
    hw->board = m;
    /* The soldered 128 or 512 KB, or the Plus's SIMMs. */
    m->ram_size = hw->model->ram_kb ? hw->model->ram_kb * 1024u : mc_config_ram_kb(&hw->config) << 10;
    hw->ram_size = m->ram_size;
    hw->wait = mc_compact_wait;
    /* The chips. */
    mc_via_init(&m->via, &hw->host, "VIA", 9);
    mc_compact_attach(m, &m->via_port, "via", &m->via, &mc_via_ops, 0, &via_lane_ops);
    mc_scc_init(&m->scc, &hw->host);
    mc_scc_set_clocks(&m->scc, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {m, scc_transmit, scc_outputs};
    mc_scc_set_port(&m->scc, &port);
    mc_compact_attach(m, &m->scc_read_port, "scc_r", &m->scc, &mc_scc_ops, 0, &mc_compact_lane_ops);
    mc_compact_attach(m, &m->scc_write_port, "scc_w", &m->scc, &mc_scc_ops, 1, &mc_compact_lane_ops);
    /* Writes to the read addresses and reads of the write addresses find
     * nothing (attach). */
    mc_iwm_init(&m->iwm, &hw->host, hw->media.floppy_drives, (mc_drive_type)b->drive);
    mc_compact_attach(m, &m->iwm_port, "iwm", &m->iwm, &mc_swim_ops, 1, &mc_compact_lane_ops);
    mc_swim_speed_source(&m->iwm, disk_speed, m);
    mc_scsi_bus_init(&m->scsi_bus);
    if (b->scsi) {
        mc_ncr5380_init(&m->scsi, &hw->host, &m->scsi_bus, 0);
        mc_compact_attach(m, &m->scsi_port, "scsi", m, &mc_compact_scsi_ops, -1, &mc_compact_lane_ops);
        mc_scsi_cd_init(&m->unit[CD_ID]);
        mc_scsi_unit_on_eject(&m->unit[CD_ID], cd_ejected, m);
        mc_scsi_bus_attach(&m->scsi_bus, CD_ID, mc_scsi_unit_target(&m->unit[CD_ID]));
    } else {
        hw->bus.region[mc_hw_region(hw, "scsi")].unclaimed_ok = 1;
    }
    map_fixed(m);
    mc_compactvideo_init(&m->video, &hw->host, "Video", hw->ram, m->ram_size);
    mc_m0110_init(&m->keyboard, &hw->host, "Keyboard", &m->via, b->keyboard);
    mc_quadmouse_init(&m->mouse, &hw->host, "Mouse");
    m->mouse.ready = mouse_ready;
    m->mouse.ready_ctx = m;
    mc_rtc_init(&m->rtc, &hw->host, "RTC");
    if (mc_hw_nvram_path(hw, m->nvram, sizeof m->nvram) == NULL) m->nvram[0] = 0;
    mc_compact_load_nvram(m);
    /* Pins other chips drive: /W//REQ released, the mouse's button up and
     * its X2, Y2 and the clock's data high (the horizontal blanking, PB6,
     * at each read: via_read). */
    mc_via_set_pa(&m->via, 0xFFu, 0xFFu);
    mc_via_set_pb(&m->via, 0xFFu, 0xFFu);
    m->overlay = 1;
    map_ram(m);
    mc_rtc_ops.reset(&m->rtc);
    return m;
}

/* The programmer's switch (Guide, chapter 3): the reset button on the
 * processor's and the chips' /RESET; the interrupt button on all three
 * /IPL lines, the NMI, is the machine's (machine.c). */
static int board_control(void *board, int control, int value) {
    compact *m = board;
    if (control != MC_CONTROL_RESET) return -1;
    mc_hw_reset_switch(m->hw, value);
    return 0;
}

const mc_board_ops mc_compact_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    NULL, NULL, board_control,
};

/* RAM: the 128K's, 512K's and 512Ke's soldered RAM (model.ram_kb; ram_mb
 * stands at 1); the Plus's 1, 2, 2.5 (two 1 MB and two 256 KB SIMMs) or
 * 4 MB. */
static const uint16_t soldered_ram[] = {1, 0};
static const uint16_t plus_ram[] = {1, 2, 4, 0};
static const uint32_t plus_ram_kb[] = {2560, 0};

static const char *const mac128k_models[] = {MC_MODEL_BUILTIN, "mac-128k", "mac-512k", NULL};
static const uint32_t mac128k_roms[] = {MC_BUILTIN_ROM, 0x28BA61CEu, 0x28BA4E50u, 0};
static const char *const mac512ke_models[] = {"mac-512ke", NULL};
static const char *const plus_models[] = {"mac-plus", "mac-plus-v1", "mac-plus-v2", NULL};
static const uint32_t plus_roms[] = {0x4D1F8172u, 0x4D1EEEE1u, 0x4D1EEAE1u, 0};

/* The 512 x 342 screen in one bit; no slot. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return w == MC_COMPACTVIDEO_WIDTH && h == MC_COMPACTVIDEO_HEIGHT ? MC_DEPTH_BIT(1) : 0;
}
static const mc_hw_caps compact_caps = {video_builtin, 0, 0, NULL};

/* The 128K's and 512K's map has no 5380 (its region is left without a
 * chip and reads as nothing). */
const mc_hw_desc mc_mac128k_desc = {
    "mac128k", mac128k_models, COMPACT_CPU, M68K_FPU_NONE, mac128k_roms, soldered_ram,
    compact_map, sizeof compact_map / sizeof *compact_map, compact_irqs, sizeof compact_irqs / sizeof *compact_irqs,
    build, &mc_compact_ops, MC_MMU_NONE, compact_map24, &compact_caps,
};

const mc_hw_desc mc_mac512ke_desc = {
    "mac512ke", mac512ke_models, COMPACT_CPU, M68K_FPU_NONE, plus_roms, soldered_ram,
    compact_map, sizeof compact_map / sizeof *compact_map, compact_irqs, sizeof compact_irqs / sizeof *compact_irqs,
    build, &mc_compact_ops, MC_MMU_NONE, compact_map24, &compact_caps,
};

const mc_hw_desc mc_macplus_desc = {
    "macplus", plus_models, COMPACT_CPU, M68K_FPU_NONE, plus_roms, plus_ram,
    compact_map, sizeof compact_map / sizeof *compact_map, compact_irqs, sizeof compact_irqs / sizeof *compact_irqs,
    build, &mc_compact_ops, MC_MMU_NONE, compact_map24, &compact_caps, plus_ram_kb,
};
