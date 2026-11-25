/* Livadix - Power Macintosh 5400/6400 and 5500/6500/Twentieth Anniversary.
 * Apple Power Macintosh 5500 and 6500 Developer Note, chapter 2, PSX+ and O'Hare.
 * Register addresses and reset accesses from 6F5724C0 $FFF030A0-$FFF03B88.
 * Physical PSX banks and O'Hare register evidence are recorded in their
 * chip headers. AV2 uses Valkyrie's display engine at doubled register
 * spacing, with VBL status and acknowledgement sharing +$20.
 * Storage layout follows Livadix's existing G3 media grouping and drivers.
 */
#include "../ppc_machine.h"
#include "../chips/awacs.h"
#include "../chips/awac.h"
#include "../chips/bandit.h"
#include "../chips/heathrow.h"
#include "../chips/ltpc.h"
#include "../chips/psx.h"
#include "../chips/heathrow_ide.h"
#include "../chips/mesh.h"
#include "../chips/swim3.h"
#include "../../adbctl.h"
#include "../../scc.h"
#include "../../via.h"
#include "../../video/valkyrie.h"
#include "../../video/mach64.h"
#include "../../storage/vdisk.h"
#include "../../guest/guest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


typedef struct serial_dma_port {
    mc_scc *scc;
    int channel;
    mc_ltpc *ltpc;
} serial_dma_port;

typedef struct psx_board {
    mc_ppc *m;
    mc_bandit bandit;
    mc_heathrow io;
    mc_heathrow_ide ide;
    mc_vdisk *ata_vdisk;
    mc_scsi_unit unit[7];
    mc_vdisk *vdisk[7];
    mc_via via;
    mc_adbctl cuda;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_serial_host serial;
    int powered, in_reset, plus, tam;
    char nvram_path[1024];
    int64_t host_start;
    uint64_t fb_serial;
    mc_dev_ops video_ops;
    mc_scc scc;
    serial_dma_port serial_dma[2];
    mc_ltpc ltpc;
    mc_awacs sound;
    mc_awac codec;
    mc_swim3 floppy;
    mc_mesh mesh;
    mc_valkyrie video;
    mc_mach64 ati;
    mc_scsi_bus scsi_bus;
    mc_psx memory;
    uint8_t nvram[8192];
} psx_board;

/* 603e PID7 PLL_CFG: 4.5x = 0111 and 5x = 1011 (MPC603E7TEC
 * section 8.1). PSX+ runs the processor bus at 50 MHz. */
static uint32_t pll(const psx_board *p) {
    return (p->plus && p->m->desc->cpu_hz == 250000000u ? 11u :
            p->m->desc->cpu_hz == 120000000u ? 8u : 7u) << 28;
}

static void map_banks(psx_board *p) {
    mc_mem_window windows[MC_PSX_SEGMENTS];
    const unsigned count = mc_psx_windows(&p->memory, windows, MC_PSX_SEGMENTS);
    mc_bus_map_float(&p->m->bus, 0, MC_PSX_SPACE);
    mc_bus_map_float(&p->m->bus, 0x40000000u, MC_PSX_SPACE);
    for (unsigned i = 0; i < count; i++) {
        const mc_mem_window *w = &windows[i];
        mc_bus_map_memory(&p->m->bus, w->base, w->size, p->m->ram + w->offset, w->bank_size, 1);
        mc_bus_map_memory(&p->m->bus, 0x40000000u + w->base, w->size, p->m->ram + w->offset, w->bank_size, 1);
    }
}

static uint32_t pci_read(void *dev, uint32_t offset, unsigned size) {
    psx_board *p = dev;
    uint32_t value;
    mc_bandit_mem_read(&p->bandit, offset + 0x80000000u, size, &value);
    return value;
}

static void pci_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    mc_bandit_mem_write(&p->bandit, offset + 0x80000000u, size, value);
}

static int pci_ops_stable(void *dev, uint32_t offset, unsigned size) {
    psx_board *p = dev; uint32_t off;
    return mc_pci_config_hit(&p->io.cfg, 0, 0x80000000u + offset, &off) &&
           mc_heathrow_ops.stable(&p->io, off, size);
}
static const mc_dev_ops pci_ops = {pci_read, pci_write, NULL, NULL, pci_ops_stable};

/* The original firmware maps ATI's prefetchable BAR at F1000000. */
static uint32_t pci_high_read(void *dev, uint32_t offset, unsigned size) {
    psx_board *p = dev;
    uint32_t value;
    mc_bandit_mem_read(&p->bandit, offset + 0xF1000000u, size, &value);
    return value;
}
static void pci_high_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    mc_bandit_mem_write(&p->bandit, offset + 0xF1000000u, size, value);
}
static const mc_dev_ops pci_high_ops = {pci_high_read, pci_high_write, NULL, NULL, NULL};

static uint32_t nv_read(void *dev, uint32_t offset, unsigned size) {
    const psx_board *p = dev;
    return size == 1 && !(offset & 15u) ? p->nvram[offset >> 4] : 0;
}

static void nv_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    if (size == 1 && !(offset & 15u)) p->nvram[offset >> 4] = (uint8_t)value;
}

static const mc_dev_ops nv_ops = {nv_read, nv_write, NULL, NULL, NULL};

static uint32_t via_read(void *dev, uint32_t offset, unsigned size) {
    psx_board *p = dev;
    mc_adbctl_sync(&p->cuda);
    return mc_via_ops.read(&p->via, offset, size);
}

static void via_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    mc_adbctl_sync(&p->cuda);
    mc_via_ops.write(&p->via, offset, size, value);
}

static const mc_dev_ops via_ops = {via_read, via_write, NULL, NULL, NULL};

static uint32_t video_read(void *dev, uint32_t offset, unsigned size) {
    return mc_valkyrie_ops.read(dev, offset == 0x20 ? 0x14 : offset >> 1, size);
}

static void video_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_valkyrie_ops.write(dev, offset >> 1, size, value);
}

static int framebuffer(void *dev, mc_framebuffer *fb) {
    psx_board *p = dev;
    if (p->plus) {
        mc_mach64_view view;
        if (!mc_mach64_view_get(&p->ati, &view)) return 0;
        *fb = (mc_framebuffer){.width = view.width, .height = view.height, .depth = view.depth,
            .row_bytes = view.row_bytes, .pixels = view.pixels, .palette = view.palette,
            .serial = ++p->fb_serial, .native_depth = view.native_depth, .native_row_bytes = view.native_row_bytes};
        return 1;
    }
    mc_valkyrie_view view;
    if (!mc_valkyrie_view_get(&p->video, &view)) return 0;
    fb->width = view.width;
    fb->height = view.height;
    fb->depth = view.depth;
    fb->row_bytes = view.row_bytes;
    fb->pixels = view.pixels;
    fb->palette = view.palette;
    fb->serial = ++p->fb_serial;
    return 1;
}

static void reset(void *dev);
static void destroy(void *dev);

static void line(void *dev, void *chip, int signal, int state) {
    psx_board *p = dev;
    if (chip == &p->cuda) {
        if (signal == MC_ADBCTL_LINE_NMI) mc_heathrow_set_source(&p->io, 20, state);
        else if (signal == MC_ADBCTL_LINE_POWER) {
            if (state) p->powered = 1;
            else if (p->powered) mc_ppc_power_off(p->m);
        } else if (signal == MC_ADBCTL_LINE_RESET) {
            if (state && !p->in_reset) {
                p->in_reset = 1;
                reset(p);
                ppc_reset(&p->m->cpu);
                ppc_jit_cpu_reset(p->m->jit);
                p->m->cpu.hid[1] = pll(p);
            } else if (!state) p->in_reset = 0;
        }
    }
    /* ROM 6F5724C0 OF valkyrie node, file $340F60: interrupt $18. */
    if (chip == &p->video && signal == MC_VALKYRIE_LINE_VBL) mc_heathrow_set_source(&p->io, 24, state);
    if (chip == &p->ati && signal == MC_MACH64_LINE_IRQ) mc_heathrow_set_source(&p->io, 24, state);
    if (chip == &p->floppy && signal == MC_SWIM3_LINE_IRQ) mc_heathrow_set_source(&p->io, 19, state);
    if (chip == &p->floppy && signal == MC_SWIM3_LINE_DRQ && state) mc_heathrow_dma_kick(&p->io, 1);
    if (chip == &p->memory && signal == MC_PSX_LINE_MAP) map_banks(p);
    if (chip == &p->io) mc_ppc_set_interrupt(p->m, state);
    if (chip == &p->via && signal == MC_VIA_LINE_IRQ) mc_heathrow_set_source(&p->io, 18, state);
    if (chip == &p->via && signal == MC_VIA_LINE_PB) mc_adbctl_via_pb(&p->cuda, (uint8_t)state);
    if (chip == &p->mesh && signal == MC_MESH_LINE_IRQ) mc_heathrow_set_source(&p->io, 12, state);
    if (chip == &p->mesh && signal == MC_MESH_LINE_DMA_STATUS) mc_heathrow_dma_bits(&p->io, 0, 0xE0, (uint8_t)state);
    if (chip == &p->mesh && signal == MC_MESH_LINE_DRQ && state) mc_heathrow_dma_kick(&p->io, 0);
    if (chip == &p->scc && signal == MC_SCC_LINE_IRQ) mc_heathrow_set_source(&p->io, 15, state);
    if (chip == &p->scc && state && signal >= MC_SCC_LINE_TX_READY_A) {
        const unsigned input = signal >= MC_SCC_LINE_RX_READY_A;
        const unsigned channel = input ? (unsigned)(signal - MC_SCC_LINE_RX_READY_A) : (unsigned)(signal - MC_SCC_LINE_TX_READY_A);
        mc_heathrow_dma_kick(&p->io, 4u + channel * 2u + input);
    }
    if (chip == &p->ltpc) mc_heathrow_dma_bits(&p->io, signal == MC_LTPC_LINE_DETECT_A ? 4u : 6u, 0x20, state ? 0x20 : 0);
    if (chip == &p->ide.ata && signal == MC_ATA_LINE_INTRQ) mc_heathrow_set_source(&p->io, 13, state);
    if (chip == &p->sound && state) mc_heathrow_dma_kick(&p->io, signal == MC_AWACS_LINE_OUT_DRQ ? 8u : 9u);
}

static void codec_control(void *ctx, uint32_t word) { mc_awac_control(ctx, (uint16_t)word); }
static uint32_t codec_status(void *ctx) {
    const mc_awac *a = ctx;
    return (uint32_t)mc_awac_status(a, 0) | (uint32_t)mc_awac_status(a, 1) << 8 | (uint32_t)mc_awac_status(a, 2) << 16 |
           (uint32_t)mc_awac_status(a, 3) << 24;
}

/* O'Hare native SCC command/data ports are sixteen bytes apart. */
static uint32_t scc_offset(uint32_t offset) {
    static const uint8_t order[4] = {0, 4, 2, 6};
    return order[(offset >> 4) & 3u];
}

static uint32_t scc_read(void *dev, uint32_t offset, unsigned size) {
    psx_board *p = dev;
    return mc_scc_ops.read(&p->scc, scc_offset(offset), size);
}

static void scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    mc_scc_ops.write(&p->scc, scc_offset(offset), size, value);
    mc_ltpc_changed(&p->ltpc, MC_SCC_A);
    mc_ltpc_changed(&p->ltpc, MC_SCC_B);
}

static const mc_dev_ops scc_ops = {scc_read, scc_write, NULL, NULL, NULL};

static uint32_t legacy_scc_read(void *dev, uint32_t offset, unsigned size) {
    return mc_scc_ops.read(&((psx_board *)dev)->scc, offset, size);
}
static void legacy_scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    psx_board *p = dev;
    mc_scc_ops.write(&p->scc, offset, size, value);
    mc_ltpc_changed(&p->ltpc, MC_SCC_A);
    mc_ltpc_changed(&p->ltpc, MC_SCC_B);
}
static const mc_dev_ops legacy_scc_ops = {legacy_scc_read, legacy_scc_write, NULL, NULL, NULL};

static int serial_dma_read(void *dev, uint8_t *byte) {
    const serial_dma_port *port = dev;
    const int ch = port->channel;
    return mc_scc_dma_read(port->scc, ch, byte) ? MC_DBDMA_BYTE : MC_DBDMA_NONE;
}

static int serial_dma_write(void *dev, uint8_t byte) {
    const serial_dma_port *port = dev;
    const int ch = port->channel;
    const int accepted = mc_scc_dma_write(port->scc, ch, byte);
    if (accepted) mc_ltpc_changed(port->ltpc, (unsigned)ch);
    return accepted;
}


#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned count, char *error, size_t capacity) {
    /* An all-zero device is uninitialized hardware for Drive Setup. Its
     * block0/map/driver writes must survive closing and reopening it. */
    if (count == 1) {
        uint8_t first[4096];
        int blank = mc_disk_read_bytes(images[0], 0, sizeof first, first);
        for (unsigned i = 0; blank && i < sizeof first; i++) blank = first[i] == 0;
        if (blank) return mc_vdisk_create_device(images[0], error, capacity);
    }
    size_t size = 0, ata_size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const uint8_t *ata = mc_guest_livadixdisk_ata(&ata_size);
    const mc_vdisk_drivers drivers = {driver, size, ata, ata_size};
    return mc_vdisk_create(images, count, &drivers, error, capacity);
}

static int disk_on_ata(const psx_board *q, unsigned i) {
    return i < q->m->config.disk_count && q->m->config.disks[i].bus == MC_BUS_ATA;
}

static void drop_disks(psx_board *q) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
        q->vdisk[id] = NULL;
    }
    if (q->ata_vdisk) {
        mc_ata_attach_disk(&q->ide.ata, 0, NULL);
        mc_vdisk_destroy(q->ata_vdisk);
        q->ata_vdisk = NULL;
    }
}

/* The internal ATA Manager drives the master only. Additional ATA images
 * become its partitions, using Livadix's existing ATA virtual-disk driver. */
static int layout_ata(psx_board *q, char *error, size_t capacity) {
    mc_disk *group[MC_CONFIG_DISKS];
    unsigned count = 0;
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (disk && disk_on_ata(q, i)) group[count++] = disk;
    }
    if (!count) return 1;
    char problem[256];
    q->ata_vdisk = make_vdisk(group, count, problem, sizeof problem);
    if (!q->ata_vdisk) {
        if (error && capacity) snprintf(error, capacity, "ATA disk: %s", problem);
        return 0;
    }
    mc_ata_attach_disk(&q->ide.ata, 0, q->ata_vdisk);
    return 1;
}

static int layout_disks(psx_board *q, char *error, size_t capacity) {
    const mc_config *c = &q->m->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(q);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || i >= c->disk_count || !c->disks[i].scsi_id_set || disk_on_ata(q, i)) continue;
        const int id = c->disks[i].scsi_id;
        if (id < 0 || id >= 7 || id == CD_ID) {
            if (error && capacity) snprintf(error, capacity, "SCSI disk ID %d is unavailable (CD-ROM uses ID 3)", id);
            return 0;
        }
        group[id][count[id]++] = disk;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *disk = mc_media_get(&q->m->media, MC_MEDIA_DISK, i, NULL);
        if (!disk || (i < c->disk_count && c->disks[i].scsi_id_set) || disk_on_ata(q, i)) continue;
        int id = -1;
        for (size_t k = 0; k < sizeof disk_ids / sizeof *disk_ids && id < 0; k++)
            if (!count[disk_ids[k]]) id = disk_ids[k];
        if (id < 0) id = last;
        if (id < 0) continue;
        group[id][count[id]++] = disk;
        last = id;
    }
    for (int id = 0; id < 7; id++) {
        if (!count[id]) continue;
        char problem[256];
        q->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!q->vdisk[id]) {
            if (error && capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(q);
            return 0;
        }
        mc_scsi_disk_init(&q->unit[id], q->vdisk[id]);
        mc_scsi_bus_attach(&q->scsi_bus, id, mc_scsi_unit_target(&q->unit[id]));
    }
    if (!layout_ata(q, error, capacity)) {
        drop_disks(q);
        return 0;
    }
    return 1;
}



static void cd_ejected(void *dev, mc_scsi_unit *unit, mc_vdisk *media) {
    psx_board *p = dev;
    (void)unit;
    mc_vdisk_destroy(media);
    p->vdisk[CD_ID] = NULL;
    mc_media_eject(&p->m->media, MC_MEDIA_CDROM, 0);
}

static void poll(void *dev) {
    psx_board *p = dev;
    int gone = 0;
    mc_disk *disc = mc_media_get(&p->m->media, MC_MEDIA_CDROM, 0, &gone);
    if ((!disc || gone) && p->vdisk[CD_ID]) {
        mc_vdisk_destroy(mc_scsi_unit_remove(&p->unit[CD_ID]));
        p->vdisk[CD_ID] = NULL;
    }
    for (unsigned drive = 0; drive < p->floppy.drives; drive++) {
        int ejected = 0;
        gone = 0;
        mc_disk *chip = mc_swim3_media(&p->floppy, drive, &ejected);
        mc_disk *disk = mc_media_get(&p->m->media, MC_MEDIA_FLOPPY, drive, &gone);
        if (chip && (ejected || gone || chip != disk)) {
            mc_swim3_remove(&p->floppy, drive);
            if (ejected && chip == disk && !gone) mc_media_eject(&p->m->media, MC_MEDIA_FLOPPY, drive);
        }
    }
}

static int insert(void *dev, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    psx_board *p = dev;
    if (kind == MC_MEDIA_DISK) return layout_disks(p, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim3_insert(&p->floppy, drive, disk, 0, error, capacity);
    if (kind != MC_MEDIA_CDROM || drive) {
        if (error && capacity) snprintf(error, capacity, "Unsupported PSX media drive");
        return 0;
    }
    if (!disk) { poll(p); return 1; }
    mc_vdisk *next = make_vdisk(&disk, 1, error, capacity);
    if (!next) return 0;
    mc_vdisk_destroy(mc_scsi_unit_remove(&p->unit[CD_ID]));
    p->vdisk[CD_ID] = next;
    mc_scsi_cd_insert(&p->unit[CD_ID], next);
    return 1;
}

static int floppy_read(void *dev, uint8_t *byte) {
    return mc_swim3_dma_read(dev, byte) ? MC_DBDMA_BYTE : MC_DBDMA_NONE;
}
static int floppy_write(void *dev, uint8_t byte) { return mc_swim3_dma_write(dev, byte); }

static int key(void *dev, uint8_t code, int down) {
    psx_board *p = dev;
    if (code == 0x7F) mc_adbctl_power_key(&p->cuda, down);
    const int accepted = mc_adb_keyboard_key(&p->keyboard, code, down);
    mc_adbctl_poke(&p->cuda);
    return accepted;
}
static int mouse(void *dev, int dx, int dy, int button) {
    psx_board *p = dev;
    mc_adb_mouse_move(&p->mouse, dx, dy, button);
    mc_adbctl_poke(&p->cuda);
    return 1;
}
static void set_clock(void *dev, int64_t seconds) {
    psx_board *p = dev;
    p->host_start = seconds;
    mc_adbctl_set_clock(&p->cuda, seconds);
}
static uint32_t audio_rate(void *dev) { return mc_awacs_rate(&((psx_board *)dev)->sound); }
static size_t audio_pull(void *dev, int16_t *stereo, size_t frames) {
    psx_board *p = dev;
    frames = mc_awacs_pull(&p->sound, stereo, frames);
    mc_awac_output(&p->codec, stereo, frames);
    mc_ppc_audio_startup(p->m, stereo, frames, p->sound.played,
                         !p->sound.out_n && !p->sound.ring_n && !(p->io.ch[8].status & MC_DBDMA_ACTIVE));
    return frames;
}
static void serial_tx(void *dev, int channel, uint8_t byte) {
    psx_board *p = dev;
    if (p->serial.transmit) p->serial.transmit(p->serial.ctx, (unsigned)channel, byte);
}
static void serial_outputs(void *dev, int channel, unsigned outputs) {
    psx_board *p = dev;
    if (p->serial.outputs) p->serial.outputs(p->serial.ctx, (unsigned)channel, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void serial_host(void *dev, const mc_serial_host *host) {
    psx_board *p = dev;
    if (host) p->serial = *host;
    else memset(&p->serial, 0, sizeof p->serial);
}
static size_t serial_receive(void *dev, unsigned port, const uint8_t *data, size_t count) {
    return port < 2 ? mc_scc_receive(&((psx_board *)dev)->scc, (int)port, data, count) : 0;
}
static void serial_modem(void *dev, unsigned port, int dcd, int cts) {
    psx_board *p = dev;
    if (port >= 2) return;
    mc_scc_set_dcd(&p->scc, (int)port, dcd);
    mc_scc_set_cts(&p->scc, (int)port, cts);
    mc_ltpc_changed(&p->ltpc, port);
}

static void reset(void *dev) {
    psx_board *p = dev;
    p->m->startup_sound_done = 0;
    p->m->cpu.hid[1] = pll(p);
    mc_bandit_ops.reset(&p->bandit);
    mc_psx_ops.reset(&p->memory);
    mc_heathrow_reset(&p->io);
    mc_via_ops.reset(&p->via);
    mc_scc_ops.reset(&p->scc);
    mc_ltpc_ops.reset(&p->ltpc);
    mc_mesh_reset(&p->mesh);
    mc_swim3_ops.reset(&p->floppy);
    mc_heathrow_ide_ops.reset(&p->ide);
    mc_awacs_reset(&p->sound);
    if (p->plus) mc_mach64_reset(&p->ati);
    else mc_valkyrie_ops.reset(&p->video);
    mc_via_set_pa(&p->via, 0xFF, 0xFF);
    mc_via_set_pb(&p->via, 0xF7, 0xF7);
}

static int load_nvram(psx_board *p) {
    if (!p->nvram_path[0] || !mc_adbctl_nvram_load(&p->cuda, p->nvram_path)) return 0;
    FILE *file = fopen(p->nvram_path, "rb");
    if (!file) return 0;
    const int complete = fseek(file, 8 + 1 + 8 + 256, SEEK_SET) == 0 &&
                         fread(p->nvram, 1, sizeof p->nvram, file) == sizeof p->nvram;
    fclose(file);
    return complete;
}
static void save_nvram(psx_board *p) {
    if (!p->nvram_path[0] || !mc_adbctl_nvram_save(&p->cuda, p->nvram_path, p->host_start)) return;
    FILE *file = fopen(p->nvram_path, "ab");
    if (!file) return;
    fwrite(p->nvram, 1, sizeof p->nvram, file);
    fclose(file);
}
static void destroy(void *dev) {
    psx_board *p = dev;
    save_nvram(p);
    drop_disks(p);
    mc_vdisk_destroy(p->vdisk[CD_ID]);
    mc_scsi_bus_free(&p->scsi_bus);
    mc_valkyrie_free(&p->video);
    mc_mach64_free(&p->ati);
    free(p->cuda.mcu);
    free(p);
}

/* Designing PCI Cards and Drivers, chapter 12, "Using NVRAM to Store
 * Name Registry Properties". ROM 6F5724C0 saves Valkyrie-AR's gprf at
 * NVRAM $1400: six-byte device location, record header, Pascal property
 * name, then eight bytes. Its driver at $FFEC9CA8-$FFEC9CEC packs the
 * display mode, depth selector and monitor identity into bytes 1-3.
 * Apply a preference only to a record initialized by the original ROM,
 * as the other built-in video boards do with their slot PRAM records. */
static void set_video_pram(psx_board *p) {
    static const uint8_t record[] = {0x14, 0x16, 0x10, 0xC0, 0, 0, 0, 0x11, 4, 'g', 'p', 'r', 'f', 8, 0};
    uint8_t *preference = p->nvram + 0x1400;
    if (memcmp(preference, record, sizeof record)) return;
    const mc_config *config = &p->m->config;
    unsigned mode, monitor;
    switch (p->video.monitor) {
    case MC_VALKYRIE_RGB12: mode = 2; monitor = 2; break;
    case MC_VALKYRIE_RGB14: mode = 6; monitor = 3; break;
    case MC_VALKYRIE_RGB16: mode = 13; monitor = 6; break;
    case MC_VALKYRIE_VGA: mode = config->width == 1024 ? 14 : 10; monitor = 10; break;
    default: return;
    }
    unsigned depth = 0;
    for (unsigned bits = config->depth; bits > 1; bits >>= 1) depth++;
    preference[15] = (uint8_t)mode;
    preference[16] = (uint8_t)depth;
    preference[17] = (uint8_t)monitor;
}

/* The original GT-B driver saves Sime as two 16-bit timing IDs, the
 * pixel-size mode (80/81/82 = 8/16/32 bits), and flags. Its native timing
 * table and globals were read independently from original ROM 6E92FE08.
 * As with Valkyrie, only alter a property the guest has initialized. */
static void set_ati_pram(psx_board *p) {
    static const uint8_t header[] = {0x14, 0x16, 0x11, 0, 0, 0, 0x12, 0x12, 4, 'S', 'i', 'm', 'e', 8};
    uint8_t *property = p->nvram + 0x1400;
    if (memcmp(property, header, sizeof header)) return;
    const mc_config *c = &p->m->config;
    const unsigned mode = p->tam ? (c->width == 800 ? 50 : 35) : c->width == 512 ? 5 : c->width == 640 ? (c->height == 870 ? 15 : 10) :
        c->width == 800 ? 45 : c->width == 832 ? 20 : c->width == 1024 ? 25 :
        c->width == 1152 ? 30 : 80;
    property[14] = property[16] = property[18] = 0;
    property[15] = property[17] = (uint8_t)mode;
    property[19] = (uint8_t)(c->depth == 32 ? 0x82 : c->depth == 16 ? 0x81 : 0x80);
    /* The saved monitor-table index must agree with the sensed connection
     * after a configuration-window monitor change. */
    property[20] = (uint8_t)(p->tam ? 11 : c->width == 512 ? 0 : c->width == 640 ?
        (c->height == 870 ? (c->monitor == MC_MONITOR_MONO ? 3 : 2) : 1) :
        c->width == 800 ? 14 : c->width == 832 ? 4 : c->width == 1024 ? 5 : c->width == 1152 ? 6 : 11);
}

static void *build(mc_ppc *m) {
    psx_board *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    m->board = p;
    p->m = m;
    p->plus = m->desc->roms[0] == 0x6E92FE08u;
    p->tam = !strcmp(m->desc->key, "psx-tam");
    const mc_hw_host *host = &m->host;
    const mc_ppc_port port = {MC_PPC_PORT_DIRECT, 4, 0, 0};
    uint32_t banks[MC_PSX_BANKS];
    if (!mc_psx_population(m->ram_size, p->plus, banks)) { free(p); return NULL; }
    mc_psx_init(&p->memory, host, banks, p->plus ? 3 : 2);
    m->cpu.hid[1] = pll(p);
    mc_ppc_attach(m, mc_ppc_region(m, "psx"), 0, MC_PSX_SIZE, &p->memory, &mc_psx_ops, port);
    mc_ppc_attach(m, mc_ppc_region(m, "pci"), 0, 0x70000000, p, &pci_ops, port);
    mc_bandit_init(&p->bandit, host, "PSX Bandit PCI", MC_BANDIT_BANDIT, 0xF2000000);
    mc_ppc_attach(m, mc_ppc_region(m, "bandit"), 0, MC_BANDIT_SPACE, &p->bandit, &mc_bandit_ops, port);
    mc_ohare_init(&p->io, host, "O'Hare");
    /* 6E92FE08 $FFF203D8-$FFF2041C interprets released GPIO6 low as
     * a diagnostic request on a subsequent power-on. AIO sense $15E
     * (ROM $FFC15248) selects ProductInfo $302D, extended machine 506
     * (Gestalt 512), while leaving that diagnostic input released high.
     * The tower's $1AB selects extended machine 507 (Gestalt 513). */
    p->io.board_id = m->desc->cpu_hz == 120000000u ? 0x10u :
        !strcmp(m->desc->key, "psx-5500") || p->tam ? 0x50u : 0x60u;
    if (p->plus) {
        if (!mc_mach64_init(&p->ati, host, 2u << 20)) { destroy(p); return NULL; }
        /* Monitor straps, not forced CRTC geometry: the original driver
         * selects and programs its own timing. */
        const unsigned width = m->config.width, height = m->config.height;
        unsigned code = 7; int extended = 0x17;
        if (width == 512 && height == 384) { code = 2; extended = -1; }
        if (width == 640 && height == 480) { code = 6; extended = -1; }
        if (width == 640 && height == 870) { code = m->config.monitor == MC_MONITOR_MONO ? 1 : 5; extended = -1; }
        if (width == 800 && height == 600) extended = 0x03;
        if (width == 832 && height == 624) extended = 0x2D;
        if (width == 1024 && height == 768) extended = 0x3A;
        if (width == 1152 && height == 870) { code = 0; extended = -1; }
        /* TAM's service specifications list 640x480 and 800x600 at
         * 60 Hz. Its saved timing is VGA/SVGA (35/50), not SVGA 56 Hz. */
        if (p->tam) { code = 7; extended = 0x17; }
        mc_mach64_set_monitor(&p->ati, code, extended);
        mc_pci_function video = mc_mach64_function(&p->ati);
        mc_bandit_attach(&p->bandit, 18, &video);
        mc_ppc_attach(m, mc_ppc_region(m, "ati-high"), 0, MC_MACH64_APERTURE, p, &pci_high_ops, port);
    }
    mc_pci_function io = mc_heathrow_function(&p->io);
    mc_bandit_attach(&p->bandit, 16, &io);
    mc_via_init(&p->via, host, "VIA", 9);
    mc_heathrow_attach(&p->io, 0x16000, 0x2000, 0, p, &via_ops);
    mc_via_set_pa(&p->via, 0xFF, 0xFF);
    mc_via_set_pb(&p->via, 0xF7, 0xF7);
    mc_adb_bus_init(&p->adb);
    mc_adb_keyboard_init(&p->keyboard);
    mc_adb_mouse_init(&p->mouse);
    mc_adb_attach(&p->adb, &p->keyboard.dev);
    mc_adb_attach(&p->adb, &p->mouse.dev);
    mc_adbctl_init(&p->cuda, host, "Cuda", MC_ADBCTL_CUDA, &p->via, &p->adb);

    mc_heathrow_attach(&p->io, 0x60000, 0x20000, 0, p, &nv_ops);
    mc_heathrow_ide_init(&p->ide, host);
    mc_heathrow_attach(&p->io, 0x20000, 0x1000, 0, &p->ide, &mc_heathrow_ide_ops);
    mc_scsi_bus_init(&p->scsi_bus);
    mc_mesh_init(&p->mesh, host, "MESH", &p->scsi_bus, 0xE2);
    mc_scsi_cd_init(&p->unit[CD_ID]);
    mc_scsi_unit_on_eject(&p->unit[CD_ID], cd_ejected, p);
    mc_scsi_bus_attach(&p->scsi_bus, CD_ID, mc_scsi_unit_target(&p->unit[CD_ID]));
    mc_heathrow_attach(&p->io, 0x10000, 0x100, 4, &p->mesh, &mc_mesh_ops);
    const mc_dbdma_port mesh_port = {&p->mesh, mc_mesh_dma_read, mc_mesh_dma_write, NULL, NULL};
    mc_heathrow_dma_port(&p->io, 0, &mesh_port);
    mc_swim3_init(&p->floppy, host, "SWIM III", p->tam ? 0 : 1);
    mc_swim3_set_stride(&p->floppy, 4);
    const mc_dbdma_port floppy_port = {&p->floppy, floppy_read, floppy_write, NULL, NULL};
    mc_heathrow_dma_port(&p->io, 1, &floppy_port);
    mc_heathrow_attach(&p->io, 0x15000, 0x100, 0, &p->floppy, &mc_swim3_ops);
    mc_scc_init(&p->scc, host);
    const mc_scc_port serial = {p, serial_tx, serial_outputs};
    mc_scc_set_port(&p->scc, &serial);
    mc_ltpc_init(&p->ltpc, host, &p->scc);
    for (unsigned ch = 0; ch < 2; ch++) {
        p->serial_dma[ch] = (serial_dma_port){&p->scc, (int)ch, &p->ltpc};
        const mc_dbdma_port output = {&p->serial_dma[ch], NULL, serial_dma_write, NULL, NULL};
        const mc_dbdma_port input = {&p->serial_dma[ch], serial_dma_read, NULL, NULL, NULL};
        mc_heathrow_dma_port(&p->io, 4u + 2u * ch, &output);
        mc_heathrow_dma_port(&p->io, 5u + 2u * ch, &input);
    }
    mc_scc_set_clocks(&p->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    mc_heathrow_attach(&p->io, 0x13000, 0x40, 0, p, &scc_ops);
    mc_heathrow_attach(&p->io, 0x13090, 0x30, 0, &p->ltpc, &mc_ltpc_ops);

    mc_heathrow_attach(&p->io, 0x12000, 0x1000, 0, p, &legacy_scc_ops);
    mc_awac_init(&p->codec, host, "AWAC");
    mc_awac_set_status(&p->codec, 1, 0x20);
    /* ROM 6E92FE08 $FFF03C14-$FFF03C30 waits for codec-ready bit 22
     * before issuing the startup sample's DBDMA transfer. */
    if (p->plus) mc_awac_set_status(&p->codec, 2, 0x40);
    const mc_awacs_codec codec = {&p->codec, codec_control, codec_status};
    mc_awacs_init(&p->sound, host, "AWACS", &codec);
    mc_heathrow_attach(&p->io, 0x14000, 0x1000, 0, &p->sound, &mc_awacs_ops);
    if (!p->plus) {
        const mc_valkyrie_monitor monitor = m->config.width == 1024 && m->config.height == 768 ?
                                              MC_VALKYRIE_VGA : mc_valkyrie_monitor_for(m->config.width, m->config.height);
        if (!mc_valkyrie_ar_init(&p->video, host, "Valkyrie AV2", monitor)) {
            destroy(p);
            return NULL;
        }
        mc_bus_map_memory(&m->bus, 0xF1000000u, MC_VALKYRIE_VRAM, p->video.vram, MC_VALKYRIE_VRAM, 1);
        mc_valkyrie_ops.reset(&p->video);
        p->video_ops = mc_valkyrie_ops;
        p->video_ops.read = video_read;
        p->video_ops.write = video_write;
        mc_ppc_attach(m, mc_ppc_region(m, "valkyrie"), 0, 0x1000, &p->video, &p->video_ops, port);
        mc_ppc_attach(m, mc_ppc_region(m, "valkyrie-clut"), 0, 0x1000, &p->video, &mc_valkyrie_clut_ops, port);
    }
    const mc_dbdma_port audio_port = {&p->sound, NULL, mc_awacs_dma_write, NULL, NULL};
    mc_heathrow_dma_port(&p->io, 8, &audio_port);
    const mc_dbdma_port audio_input = {&p->sound, mc_awacs_dma_read, NULL, NULL, NULL};
    mc_heathrow_dma_port(&p->io, 9, &audio_input);
    if (m->config.controller == MC_CONTROLLER_FIRMWARE) {
        snprintf(m->error, sizeof m->error, "No matching PSX-family Cuda firmware is indexed; use controller auto or protocol");
        destroy(p);
        return NULL;
    }
    if (!mc_config_nvram_path(&m->config, p->nvram_path, sizeof p->nvram_path)) p->nvram_path[0] = 0;
    if (!load_nvram(p)) mc_adbctl_set_seconds(&p->cuda, 0);
    if (p->plus) set_ati_pram(p);
    else set_video_pram(p);
    if (m->config.boot_priority == MC_BOOT_CDROM) {
        uint8_t *pram = mc_adbctl_pram(&p->cuda);
        const uint16_t reference = (uint16_t)-(33 + CD_ID);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(reference >> 8);
        pram[0x7B] = (uint8_t)reference;
    }
    mc_adbctl_ops.reset(&p->cuda);
    return p;
}


static unsigned builtin(const mc_model *model, unsigned width, unsigned height) {
    if (model && !strcmp(model->key, "power-macintosh-5400") && (width != 640 || height != 480)) return 0;
    /* Performa 6400 Developer Note chapter 2, Display RAM, and Table 3-10. */
    unsigned depth = mc_valkyrie_max_depth(mc_valkyrie_monitor_for(width, height));
    if (width == 800 && height == 600) depth = 16;
    if (width == 1024 && height == 768) depth = 8;
    return MC_DEPTHS_UPTO(depth) & ~MC_DEPTHS_UPTO(2);
}
static const mc_board_ops ops = {
    .reset = reset, .line = line, .framebuffer = framebuffer,
    .key = key, .mouse = mouse, .set_clock = set_clock, .destroy = destroy,
    .audio_rate = audio_rate, .audio_pull = audio_pull, .insert = insert, .poll = poll,
    .serial_host = serial_host, .serial_receive = serial_receive, .serial_modem = serial_modem,
};
static const mc_hw_caps caps = {.builtin = builtin, .features = MC_FEATURE_ATA_DISK};
static const uint32_t roms[] = {0x6F5724C0u, 0};
static const uint16_t ram[] = {8, 12, 16, 20, 24, 28, 32, 40, 44, 48, 56, 72, 76, 80, 88, 104, 136, 0};
static const char *const models5400[] = {"power-macintosh-5400", NULL};
static const char *const models6400[] = {"power-macintosh-6400", NULL};
static const mc_region map[] = {
    {"ram", 0, MC_PSX_SPACE, 0, MC_REGION_RAM},
    {"pci", 0x80000000u, 0x70000000u, 0x7FFFFFFFu, MC_REGION_IO},
    {"valkyrie-clut", 0xF1304000u, 0x1000, 0xFFF, MC_REGION_IO},
    {"valkyrie", 0xF130A000u, 0x1000, 0xFFF, MC_REGION_IO},
    {"bandit", 0xF2000000u, MC_BANDIT_SPACE, MC_BANDIT_SPACE - 1, MC_REGION_IO},
    {"psx", 0xF8000000u, MC_PSX_SIZE, MC_PSX_SIZE - 1, MC_REGION_IO},
    {"rom", 0xFF000000u, 0x1000000, 0, MC_REGION_ROM},
};
const mc_ppc_desc mc_psx5400_desc = {
    .key = "psx-5400", .name = "Power Macintosh 5400", .models = models5400, .cpu = PPC_MODEL_603E,
    .cpu_hz = 120000000u, .timebase_hz = 10000000u, .cpi = 8, .cpi_div = 9,
    .roms = roms, .ram_mb = ram, .map = map, .nmap = sizeof map / sizeof *map,
    .unmapped = MC_PPC_UNMAPPED_STOP, .build = build, .ops = &ops, .caps = &caps,
};
const mc_ppc_desc mc_psx6400_desc = {
    .key = "psx-6400", .name = "Power Macintosh 6400", .models = models6400, .cpu = PPC_MODEL_603E,
    .cpu_hz = 180000000u, .timebase_hz = 10000000u, .cpi = 8, .cpi_div = 9,
    .roms = roms, .ram_mb = ram, .map = map, .nmap = sizeof map / sizeof *map,
    .unmapped = MC_PPC_UNMAPPED_STOP, .build = build, .ops = &ops, .caps = &caps,
};

/* Apple 5500/6500 Developer Note Table 3-10; the 5500 CRT is fixed to
 * 640x480, and TAM's integrated panel is 800x600 (Apple specifications). */
static unsigned builtin_plus(const mc_model *model, unsigned width, unsigned height) {
    if (!model) return 0;
    if (!strcmp(model->key, "power-macintosh-5500") && (width != 640 || height != 480)) return 0;
    if (!strcmp(model->key, "twentieth-anniversary-macintosh") &&
        !((width == 800 && height == 600) || (width == 640 && height == 480))) return 0;
    unsigned depth = 0;
    if ((width == 512 && height == 384) || (width == 640 && height == 480) ||
        (width == 800 && height == 600) || (width == 832 && height == 624)) depth = 32;
    if ((width == 640 && height == 870) || (width == 1024 && height == 768) ||
        (width == 1152 && height == 870)) depth = 16;
    if (width == 1280 && height == 1024) depth = 8;
    if (!strcmp(model->key, "twentieth-anniversary-macintosh") && depth > 16) depth = 16;
    return depth ? MC_DEPTHS_UPTO(depth) & ~MC_DEPTHS_UPTO(4) : 0;
}
static const mc_hw_caps tam_caps = {.builtin = builtin_plus, .features = MC_FEATURE_ATA_DISK | MC_FEATURE_NO_FLOPPY};
static const mc_hw_caps plus_caps = {.builtin = builtin_plus, .features = MC_FEATURE_ATA_DISK};
static const uint32_t plus_roms[] = {0x6E92FE08u, 0};
static const uint16_t plus_ram[] = {8, 12, 16, 20, 24, 32, 36, 40, 48, 64, 68, 72, 80, 96, 128, 0};
static const char *const models5500[] = {"power-macintosh-5500", NULL};
static const char *const models6500[] = {"power-macintosh-6500", NULL};
static const char *const models_tam[] = {"twentieth-anniversary-macintosh", NULL};
static const mc_region plus_map[] = {
    {"ram", 0, MC_PSX_SPACE, 0, MC_REGION_RAM},
    {"pci", 0x80000000u, 0x70000000u, 0x7FFFFFFFu, MC_REGION_IO},
    {"ati-high", 0xF1000000u, MC_MACH64_APERTURE, MC_MACH64_APERTURE - 1, MC_REGION_IO},
    {"bandit", 0xF2000000u, MC_BANDIT_SPACE, MC_BANDIT_SPACE - 1, MC_REGION_IO},
    {"psx", 0xF8000000u, MC_PSX_SIZE, MC_PSX_SIZE - 1, MC_REGION_IO},
    {"rom", 0xFF000000u, 0x1000000, 0, MC_REGION_ROM},
};
#define PLUS_DESC(key_, name_, models_, hz_) { \
    .key = key_, .name = name_, .models = models_, .cpu = PPC_MODEL_603E, \
    .cpu_hz = hz_, .timebase_hz = 12500000u, .cpi = 8, .cpi_div = 9, \
    .roms = plus_roms, .ram_mb = plus_ram, .map = plus_map, .nmap = sizeof plus_map / sizeof *plus_map, \
    .unmapped = MC_PPC_UNMAPPED_STOP, .build = build, .ops = &ops, .caps = &plus_caps, \
}
const mc_ppc_desc mc_psx5500_desc = PLUS_DESC("psx-5500", "Power Macintosh 5500", models5500, 225000000u);
const mc_ppc_desc mc_psx6500_desc = PLUS_DESC("psx-6500", "Power Macintosh 6500", models6500, 250000000u);
const mc_ppc_desc mc_psx_tam_desc = {
    .key = "psx-tam", .name = "Twentieth Anniversary Macintosh", .models = models_tam, .cpu = PPC_MODEL_603E,
    .cpu_hz = 250000000u, .timebase_hz = 12500000u, .cpi = 8, .cpi_div = 9,
    .roms = plus_roms, .ram_mb = plus_ram, .map = plus_map, .nmap = sizeof plus_map / sizeof *plus_map,
    .unmapped = MC_PPC_UNMAPPED_STOP, .build = build, .ops = &ops, .caps = &tam_caps,
};
