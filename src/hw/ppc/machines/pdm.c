/* Livadix — the Power Macintosh 6100/7100/8100 and Workgroup Server PDM boards.
 *
 * The physical map is the Power Macintosh Computers Developer Note's
 * (Apple, 1994), Table 2-5: RAM $0000 0000-$0FFF FFFF (the 8100's upper
 * SIMM banks above, to $1EFF FFFF: Enhanced Developer Note, Table A-2), ROM
 * $4000 0000-$4FFF FFFF, I/O $5000 0000-$5FFF FFFF, the ROM's alias
 * $FF00 0000-$FFFF FFFF, through which the 601 starts at $FFF0 0100
 * (MPC601UM 2.7.1). The ROM is 4 MB, repeated through its windows.
 * Processor: the 601 at 60-120 MHz; RTC and decrementer calibrated below
 * (MPC601UM 2.2.5.3). Enhanced and Workgroup Server 9150 Developer Notes
 * supply the later clocks, cache pins, BART revisions and FAT AMIC wiring.
 *
 * mc_pdm_desc is the board of RAM and ROM alone (phase 6.3's probe). The
 * production machines (the mc_pdm*_desc descriptions) carry
 * the chips at the addresses of the ROM's PDM decoder tables ($FFC154F0,
 * $FFC155C4, the table of windows, interrupts and DMA):
 *   $50F0 0000 VIA1 (a 6522; Cuda on PB3-PB5 and the shift register)
 *   $50F0 4000 SCC (Curio's; B control +0, A control +2, B data +4, A
 *              data +6: the POST's monitor writes WR9 at +2, $FFF074E8)
 *   $50F0 8000 the Ethernet address PROM† (as on the Quadra 840AV)
 *   $50F0 A000 MACE (Curio's; registers every $10)
 *   $50F1 0000 53C94 (Curio's SCSI, the external and standard bus;
 *              registers every $10), $50F1 1000 the 8100's second (fast)
 *              53C94 (CPU ID $3013, the SCSI Manager's bus B, $FFC2E10A)
 *   $50F1 4000 the AMIC's sound and codec port (amic.h, awac.h)
 *   $50F1 6000 SWIM III (swim3.h)
 *   $50F2 4000 Ariel II (ariel.h)
 *   $50F2 6000 the AMIC's pseudo-VIA2, $50F2 8000 the video (pdm_video.h),
 *   $50F2 A000 its interrupt controller, $50F2 C000 the diagnostic bit,
 *   $50F3 1000 its DMA channels
 *   $50F4 0000 the HMC (hmc.h)
 *   $5FFF FFFC the CPU ID (cpuid.h)
 *   $F000 0000 BART's registers; NuBus slot space through it (bart.h)
 * Interrupts (amic.h): VIA1 and the pseudo-VIA2 at levels 1 and 2, the
 * MACE at 3, the SCC and the DMA channels at 4, Cuda's NMI at 7; the
 * 53C94s, SWIM III and the slots through the pseudo-VIA2; the AMIC drives
 * the 601's INT.
 *
 * NuBus $B-$D and PDS $E on 7100/8100, plus NuBus $A on 9150:
 * pseudo-VIA2 slot bit s - 9 (9150 Developer Note, Figure 2). The 6100
 * uses slot $E on its adapter. Livadix's choices†: the 53C94s at 25 MHz;
 * the frame buffer (pdm_video.h) where the guest's screen base points.
 *
 * Livadix's own code. */
#include "../ppc_machine.h"
#include "../chips/amic.h"
#include "../chips/ariel.h"
#include "../chips/awac.h"
#include "../chips/bart.h"
#include "../chips/cpuid.h"
#include "../chips/hmc.h"
#include "../chips/pdm_video.h"
#include "../chips/swim3.h"
#include "../../adb.h"
#include "../../adbctl.h"
#include "../../am79c940.h"
#include "../../livadix_card.h"
#include "../../ncr53c96.h"
#include "../../scc.h"
#include "../../scsi_bus.h"
#include "../../scsi_target.h"
#include "../../sonic.h"
#include "../../via.h"
#include "../../guest/guest.h"
#include "../../storage/vdisk.h"
#include "../../hw_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024u * 1024u)

static const uint32_t pdm_roms[] = {0x9FEB69B3u, 0x9B7A3AADu, 0x9C7C98F7u, 0x9B037F6Fu, 0};
static const uint16_t pdm_ram[] = {8, 12, 16, 24, 40, 72, 0};

static const mc_region pdm_map[] = {
    {"ram", 0x00000000u, 0x10000000u, 0, MC_REGION_RAM},
    {"rom", 0x40000000u, 0x10000000u, 0, MC_REGION_ROM},
    {"io", 0x50000000u, 0x10000000u, 0x0FFFFFFFu, MC_REGION_IO},
    {"rom-alias", 0xFF000000u, 0x01000000u, 0, MC_REGION_ROM},
};

const mc_ppc_desc mc_pdm_desc = {
    .key = "pdm",
    .models = NULL,
    .name = "Power Macintosh 6100",
    .cpu = PPC_MODEL_601,
    .cpu_hz = 60000000u,
    .timebase_hz = 7812500u,
    .cpi = 1,
    .roms = pdm_roms,
    .ram_mb = pdm_ram,
    .map = pdm_map,
    .nmap = sizeof pdm_map / sizeof *pdm_map,
    .unmapped = MC_PPC_UNMAPPED_STOP,
    .build = NULL,
    .ops = NULL,
    .caps = NULL,
};

/* --- the machines ------------------------------------------------------------------ */

typedef struct pdm_board {
    uint32_t cpuid;
    mc_hmc_board hmc;
    unsigned simm_banks;
    uint16_t slots;             /* NuBus slots (bit s) */
    unsigned scsi_buses;
    uint32_t cache_kb;
    uint32_t bart_id;
    uint8_t amic_variant;
} pdm_board;

/* Slot interrupt wiring: NuBus $B-$D and PDS $E, plus NuBus $A on the
 * 9150 (9150 Developer Note, Figure 2). The 6100 uses its PDS adapter. */
static const pdm_board board_6100 = {MC_CPUID_PDM_6100, MC_HMC_BOARD_6100, 2, 0x4000, 1, 0, MC_BART_ID_4, MC_AMIC_STANDARD};
static const pdm_board board_7100 = {MC_CPUID_PDM_7100, MC_HMC_BOARD_7100_8100, 4, 0x7800, 1, 0, MC_BART_ID_4, MC_AMIC_STANDARD};
static const pdm_board board_8100 = {MC_CPUID_PDM_8100, MC_HMC_BOARD_7100_8100, 8, 0x7800, 2, 256, MC_BART_ID_4, MC_AMIC_STANDARD};
/* Enhanced Developer Note: the 66/80/100 MHz machines include 256 KB L2;
 * 7100/80 retains BART 4, while the 8100/100 and /110 use BART 21. */
static const pdm_board board_6150 = {MC_CPUID_PDM_6100, MC_HMC_BOARD_6100, 2, 0x4000, 1, 256, MC_BART_ID_4, MC_AMIC_STANDARD};
static const pdm_board board_7100_80 = {MC_CPUID_PDM_7100, MC_HMC_BOARD_7100_8100, 4, 0x7800, 1, 256, MC_BART_ID_4, MC_AMIC_STANDARD};
static const pdm_board board_8100_fast = {MC_CPUID_PDM_8100, MC_HMC_BOARD_7100_8100, 8, 0x7800, 2, 256, MC_BART_ID_21, MC_AMIC_STANDARD};
/* 9150 Developer Note, Tables 1-4 and "FAT AMIC IC". */
static const pdm_board board_9150_80 = {MC_CPUID_PDM_9150, MC_HMC_BOARD_7100_8100, 8, 0x7C00, 2, 512, MC_BART_ID_4, MC_AMIC_FAT_1};
static const pdm_board board_9150_120 = {MC_CPUID_PDM_9150, MC_HMC_BOARD_7100_8100, 8, 0x7C00, 2, 1024, MC_BART_ID_21, MC_AMIC_FAT_2};

typedef struct pdm {
    mc_ppc *m;
    const pdm_board *board;
    mc_hmc hmc;
    mc_cpuid cpuid;
    mc_via via1;
    mc_amic amic;
    mc_awac awac;
    mc_pdm_video video;
    mc_ariel ariel;
    mc_bart bart;
    mc_swim3 swim3;
    mc_scc scc;
    mc_scsi_bus scsi_bus[2];
    mc_ncr53c96 scsi[2];
    mc_scsi_unit unit[7];       /* bus A: SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_am79c940 enet;
    mc_sonic_prom enet_prom;
    mc_net_host net;
    mc_serial_host serial;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adbctl cuda;
    int powered, in_reset;
    char nvram[1024];           /* the PRAM file (empty: none) */
    int64_t host_start;
    mc_livadix_card card;
    unsigned card_slot;
    int nubus;                  /* the bus region of NuBus space */
    uint64_t fb_serial;
    uint32_t screen_pa;                 /* the frame buffer last found (board_framebuffer) */
    int screen_found;
    uint32_t palette[256];
} pdm;

/* --- the bus ----------------------------------------------------------------------- */

/* RAM as the HMC decodes it: each bank repeated through its window; the
 * rest of the RAM space completes without data (the ROM's sizing writes
 * and reads the top of every window, $FFF03B00, and the HMC acknowledges:
 * no transfer error is documented there). */
static void map_ram(pdm *p) {
    mc_ppc *m = p->m;
    mc_bus_map_float(&m->bus, 0, 0x40000000ull);
    for (unsigned i = 0; i < MC_HMC_BANKS; i++) {
        const mc_hmc_bank *b = &p->hmc.bank[i];
        if (b->size) mc_bus_map_memory(&m->bus, b->window, b->window_size, m->ram + b->offset, b->size, 1);
    }
}

static void remap_ram(void *ctx) { map_ram(ctx); }

static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    pdm *p = dev;
    mc_adbctl_sync(&p->cuda);
    return mc_via_ops.read(&p->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pdm *p = dev;
    mc_adbctl_sync(&p->cuda);
    mc_via_ops.write(&p->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    pdm *p = dev;
    return !p->cuda.firmware && mc_via_ops.stable && mc_via_ops.stable(&p->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* The 53C94s' registers every $10. */
static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, offset >> 4, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, offset >> 4, size); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};

/* The controllers' data ports at +$100 (the SCSI Manager reads words
 * there, ROM $40829606, as at the Quadra 840AV's $50F1 8100): each access
 * is DACK cycles. */
static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(dev, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    mc_ncr53c96_dma_write(dev, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL, NULL};

/* The AMIC's SCSI channels: DACK cycles of the controllers. */
static int scsi_a_read(void *ctx, uint8_t *byte) {
    uint32_t v = 0;
    if (!mc_ncr53c96_dma_read(&((pdm *)ctx)->scsi[0], 1, &v)) return 0;
    *byte = (uint8_t)v;
    return 1;
}
static int scsi_a_write(void *ctx, uint8_t byte) { return mc_ncr53c96_dma_write(&((pdm *)ctx)->scsi[0], 1, byte) == 1; }
static int scsi_b_read(void *ctx, uint8_t *byte) {
    uint32_t v = 0;
    if (!mc_ncr53c96_dma_read(&((pdm *)ctx)->scsi[1], 1, &v)) return 0;
    *byte = (uint8_t)v;
    return 1;
}
static int scsi_b_write(void *ctx, uint8_t byte) { return mc_ncr53c96_dma_write(&((pdm *)ctx)->scsi[1], 1, byte) == 1; }

/* --- wiring ------------------------------------------------------------------------ */

static void find_screen(pdm *p);

static void board_line(void *board, void *dev, int line, int state) {
    pdm *p = board;
    if (dev == &p->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_amic_set_source(&p->amic, MC_AMIC_SRC_VIA1, state);
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&p->cuda, (uint8_t)state);
    } else if (dev == &p->amic) {
        if (line == MC_AMIC_LINE_CPU) mc_ppc_set_interrupt(p->m, state);
        else if (line == MC_AMIC_LINE_TICK) mc_via_set_ca1(&p->via1, state);
    } else if (dev == &p->video) {
        if (line == MC_PDM_VIDEO_LINE_VBL) {
            mc_amic_set_slots(&p->amic, state ? 0x40u : 0, 0x40u);
            if (state && !p->card_slot) find_screen(p);
        }
    } else if (dev == &p->bart) {
        if (line == MC_BART_LINE_SLOTS) {
            const uint8_t mask = (uint8_t)(p->board->slots >> 9);
            mc_amic_set_slots(&p->amic, (uint8_t)((unsigned)state >> 9), mask);
        }
    } else if (dev == &p->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_bart_irq(&p->bart, p->card_slot, state);
    } else if (dev == &p->cuda) {
        if (line == MC_ADBCTL_LINE_NMI) mc_amic_set_source(&p->amic, MC_AMIC_SRC_NMI, state);
        else if (line == MC_ADBCTL_LINE_POWER) {
            if (state) p->powered = 1;
            else if (p->powered) mc_ppc_power_off(p->m);
        } else if (line == MC_ADBCTL_LINE_RESET) {
            /* Cuda's restart: the 601 and the board start over at once
             * (the back end cannot hold the processor through Cuda's
             * reset pulse; the ROM's checksum takes far longer than the
             * pulse before it talks to Cuda)†. */
            if (state && !p->in_reset) {
                p->in_reset = 1;
                if (p->m->desc->ops->reset) p->m->desc->ops->reset(p);
                ppc_reset(&p->m->cpu);
                ppc_jit_cpu_reset(p->m->jit);
            } else if (!state) {
                p->in_reset = 0;
            }
        }
    } else if (dev == &p->scsi[0]) {
        if (line == MC_NCR53C96_LINE_IRQ) mc_amic_set_via2(&p->amic, MC_AMIC_VIA2_SCSI_A_IRQ, state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            mc_amic_set_via2(&p->amic, MC_AMIC_VIA2_SCSI_A_DRQ, state);
            mc_amic_request(&p->amic, MC_AMIC_CH_SCSI_A, state);
        }
    } else if (dev == &p->scsi[1]) {
        if (line == MC_NCR53C96_LINE_IRQ) mc_amic_set_via2(&p->amic, MC_AMIC_VIA2_SCSI_B_IRQ, state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            mc_amic_set_via2(&p->amic, MC_AMIC_VIA2_SCSI_B_DRQ, state);
            mc_amic_request(&p->amic, MC_AMIC_CH_SCSI_B, state);
        }
    } else if (dev == &p->swim3) {
        if (line == MC_SWIM3_LINE_IRQ) {
            mc_amic_set_via2(&p->amic, MC_AMIC_VIA2_FDC, state);
            mc_amic_floppy_irq(&p->amic, state);
        }
        else if (line == MC_SWIM3_LINE_DRQ) mc_amic_request(&p->amic, MC_AMIC_CH_FLOPPY, state);
    } else if (dev == &p->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_amic_set_source(&p->amic, MC_AMIC_SRC_SCC, state);
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&p->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &p->enet) {
        if (line == MC_AM79C940_LINE_IRQ) mc_amic_set_source(&p->amic, MC_AMIC_SRC_ENET, state);
    }
}

/* --- PRAM ------------------------------------------------------------------------- */

/* A new PRAM: the ROM's own defaults (as quadra605.c), 32-bit addressing on. */
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

/* --- storage ---------------------------------------------------------------------- */

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

/* Developer Note, chapter 3 "SCSI Connectors": the 8100 ships its hard
 * disk on the second, fast internal bus, and its CD on the standard bus. */
static mc_scsi_bus *hard_disk_bus(pdm *p) {
    return &p->scsi_bus[p->board->scsi_buses > 1 ? 1 : 0];
}

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const mc_vdisk_drivers drivers = {driver, size, NULL, 0};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(pdm *p) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !p->vdisk[id]) continue;
        mc_scsi_bus_attach(id == CD_ID ? &p->scsi_bus[0] : hard_disk_bus(p), id, NULL);
        mc_vdisk_destroy(p->vdisk[id]);
        p->vdisk[id] = NULL;
    }
}

/* The hard disks on the board's internal SCSI bus. */
static int layout_disks(pdm *p, char *error, size_t capacity) {
    const mc_config *c = &p->m->config;
    mc_disk *group[7][MC_CONFIG_DISKS];
    unsigned count[7] = {0};
    int last = -1;
    drop_disks(p);
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&p->m->media, MC_MEDIA_DISK, i, NULL);
        if (!d || i >= c->disk_count || !c->disks[i].scsi_id_set) continue;
        const int id = c->disks[i].scsi_id;
        group[id][count[id]++] = d;
    }
    for (unsigned i = 0; i < MC_CONFIG_DISKS; i++) {
        mc_disk *d = mc_media_get(&p->m->media, MC_MEDIA_DISK, i, NULL);
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
        p->vdisk[id] = make_vdisk(group[id], count[id], problem, sizeof problem);
        if (!p->vdisk[id]) {
            if (capacity) snprintf(error, capacity, "SCSI ID %d: %s", id, problem);
            drop_disks(p);
            return 0;
        }
        mc_scsi_disk_init(&p->unit[id], p->vdisk[id]);
        mc_scsi_bus_attach(hard_disk_bus(p), id, mc_scsi_unit_target(&p->unit[id]));
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    pdm *p = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    p->vdisk[CD_ID] = NULL;
    mc_media_eject(&p->m->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    pdm *p = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(p, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim3_insert(&p->swim3, drive, disk, 0, error, capacity);
    char problem[256];
    mc_vdisk *vd = make_vdisk(&disk, 1, problem, sizeof problem);
    if (!vd) {
        if (capacity) snprintf(error, capacity, "%s", problem);
        return 0;
    }
    mc_vdisk *old = mc_scsi_unit_remove(&p->unit[CD_ID]);
    if (old) mc_vdisk_destroy(old);
    mc_scsi_cd_insert(&p->unit[CD_ID], vd);
    p->vdisk[CD_ID] = vd;
    return 1;
}

static void board_poll(void *board) {
    pdm *p = board;
    mc_media *media = &p->m->media;
    for (unsigned i = 0; i < p->swim3.drives; i++) {
        int ejected = 0, lost = 0;
        mc_disk *chip = mc_swim3_media(&p->swim3, i, &ejected);
        mc_disk *d = mc_media_get(media, MC_MEDIA_FLOPPY, i, &lost);
        if (!chip) continue;
        if (ejected || chip != d || lost) {
            mc_swim3_remove(&p->swim3, i);
            if (ejected && chip == d && !lost) mc_media_eject(media, MC_MEDIA_FLOPPY, i);
        }
    }
    int gone = 0;
    mc_disk *cd = mc_media_get(media, MC_MEDIA_CDROM, 0, &gone);
    if (p->vdisk[CD_ID] && (!cd || gone)) {
        mc_vdisk *old = mc_scsi_unit_remove(&p->unit[CD_ID]);
        if (old) mc_vdisk_destroy(old);
        p->vdisk[CD_ID] = NULL;
    }
}

/* --- Ethernet, serial, sound ------------------------------------------------------- */

static void enet_dma_transmit(void *ctx, const uint8_t *frame, size_t length) {
    mc_am79c940_transmit(&((pdm *)ctx)->enet, frame, length);
}
static int enet_dma_receive(void *ctx, const uint8_t *frame, size_t length, const uint8_t status[4]) {
    return mc_amic_enet_receive(&((pdm *)ctx)->amic, frame, length, status);
}
static void enet_transmit(void *ctx, const uint8_t *frame, size_t length) {
    pdm *p = ctx;
    if (p->net.transmit) p->net.transmit(p->net.ctx, frame, length);
}
static void board_net_host(void *board, const mc_net_host *host) {
    pdm *p = board;
    if (host) p->net = *host;
    else memset(&p->net, 0, sizeof p->net);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_am79c940_receive(&((pdm *)board)->enet, frame, length);
}

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }
static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    pdm *p = ctx;
    if (p->serial.transmit) p->serial.transmit(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    pdm *p = ctx;
    if (p->serial.outputs) p->serial.outputs(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    pdm *p = board;
    if (host) p->serial = *host;
    else memset(&p->serial, 0, sizeof p->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    pdm *p = board;
    return port < 2 ? mc_scc_receive(&p->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    pdm *p = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&p->scc, port_channel(port), dcd);
    mc_scc_set_cts(&p->scc, port_channel(port), cts);
}

static uint32_t board_audio_rate(void *board) { return ((pdm *)board)->m->config.sound ? MC_AMIC_SOUND_RATE : 0; }
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    pdm *p = board;
    const size_t n = mc_amic_pull(&p->amic, stereo, frames);
    mc_ppc_audio_startup(p->m, stereo, n, p->amic.snd_played,
                         !(p->amic.snd_reg[0x10] & 1u) && !p->amic.ring_count);
    return n;
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    pdm *p = board;
    p->m->startup_sound_done = 0;
    mc_hmc_ops.reset(&p->hmc);
    mc_via_ops.reset(&p->via1);
    mc_amic_int_ops.reset(&p->amic);
    mc_pdm_video_ops.reset(&p->video);
    mc_ariel_ops.reset(&p->ariel);
    mc_bart_ops.reset(&p->bart);
    mc_swim3_ops.reset(&p->swim3);
    mc_am79c940_ops.reset(&p->enet);
    for (unsigned i = 0; i < p->board->scsi_buses; i++) mc_ncr53c96_reset(&p->scsi[i]);
    mc_scc_ops.reset(&p->scc);
    if (p->card_slot) mc_livadix_card_ops.reset(&p->card);
    mc_via_set_pa(&p->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&p->via1, 0xF7u, 0xF7u);
}

/* The built-in screen: its frame buffer where the guest's screen base
 * (ScrnBase, $0824) points, translated through the 601's MMU†: the 68k
 * world's ScrnBase through the current translation, looked at for each
 * picture and at every VBL; while another nanokernel task runs (its own
 * address space, the idle task) the last base found stays. */
static void find_screen(pdm *p) {
    uint32_t base = 0, pa;
    if (mc_ppc_peek(p->m, 0x0824, 4, &base) && base && mc_ppc_physical(p->m, base, &pa) &&
        mc_hmc_ram_offset(&p->hmc, pa) >= 0)
        p->screen_pa = pa, p->screen_found = 1;
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    pdm *p = board;
    if (p->card_slot) {
        mc_livadix_card_view cv;
        if (!mc_livadix_card_view_get(&p->card, &cv)) return 0;
        fb->width = cv.width;
        fb->height = cv.height;
        fb->depth = cv.depth;
        fb->row_bytes = cv.row_bytes;
        fb->pixels = cv.pixels;
        fb->palette = cv.palette;
        fb->serial = ++p->fb_serial;
        return 1;
    }
    mc_pdm_video_view v;
    find_screen(p);
    if (!p->screen_found) return 0;
    mc_pdm_video_set_base(&p->video, p->screen_pa);
    if (!mc_pdm_video_view_get(&p->video, &v)) return 0;
    const int64_t off = mc_hmc_ram_offset(&p->hmc, v.base);
    if (off < 0 || (uint64_t)off + (uint64_t)v.row_bytes * v.height > p->m->ram_size) return 0;
    if (v.depth <= 8) mc_ariel_view_palette(&p->ariel, v.depth, p->palette);
    fb->width = v.width;
    fb->height = v.height;
    fb->depth = v.depth;
    fb->row_bytes = v.row_bytes;
    fb->pixels = p->m->ram + off;
    fb->palette = p->palette;
    fb->serial = ++p->fb_serial;
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    pdm *p = board;
    if (code == 0x7F) mc_adbctl_power_key(&p->cuda, down);
    if (!mc_adb_keyboard_key(&p->keyboard, code, down)) return 0;
    mc_adbctl_poke(&p->cuda);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    pdm *p = board;
    mc_adb_mouse_move(&p->mouse, dx, dy, button);
    mc_adbctl_poke(&p->cuda);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    pdm *p = board;
    p->host_start = host_seconds;
    mc_adbctl_set_clock(&p->cuda, host_seconds);
}

static void board_destroy(void *board) {
    pdm *p = board;
    if (p->nvram[0]) mc_adbctl_nvram_save(&p->cuda, p->nvram, p->host_start);
    for (int id = 0; id < 7; id++) {
        if (!p->vdisk[id]) continue;
        mc_scsi_bus_attach(id == CD_ID ? &p->scsi_bus[0] : hard_disk_bus(p), id, NULL);
        mc_vdisk_destroy(p->vdisk[id]);
    }
    for (unsigned i = 0; i < 2; i++) mc_scsi_bus_free(&p->scsi_bus[i]);
    mc_amic_free(&p->amic);
    mc_livadix_card_free(&p->card);
    free(p->cuda.mcu);
    free(p);
}

static uint32_t board_card_blits(void *board) {
    pdm *p = board;
    return p->card_slot ? p->card.blt.count : 0;
}

static const mc_board_ops pdm_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive, NULL, board_card_blits,
};

/* RAM: 8 MB on the board, the rest in equal SIMM banks, two when they fit
 * their windows, else 32 MB banks (Enhanced Developer Note, Table A-1). */
static int ram_banks(const pdm_board *b, uint32_t total, uint32_t *banks) {
    memset(banks, 0, 8 * sizeof *banks);
    if (total < 8u * MB) return 0;
    const uint32_t simm = total - 8u * MB;
    if (!simm) return 1;
    const uint32_t window = b->hmc == MC_HMC_BOARD_6100 ? 128u * MB : 32u * MB;
    uint32_t size = simm / 2u;
    unsigned n = 2;
    if (size > window || (size & (size - 1u))) {
        size = 32u * MB;
        n = simm / size;
        if (simm % size) return 0;
    }
    if (n > b->simm_banks || size < 2u * MB) return 0;
    for (unsigned i = 0; i < n; i++) banks[i] = size;
    return 1;
}

/* The Livadix Display Card in the first free slot: BART's slot windows, and
 * the same windows on the bus (VRAM and the declaration ROM as memory, the
 * registers as a device). */
static int fit_card(pdm *p) {
    mc_ppc *m = p->m;
    size_t rom_size = 0;
    const uint8_t *rom = mc_guest_livadixcard(&rom_size);
    const int mode = mc_livadix_card_mode(m->config.width, m->config.height);
    const unsigned slot = mc_bart_free_slot(&p->bart);
    if (mode < 0 || !slot ||
        !mc_livadix_card_init(&p->card, &m->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size,
                           m->config.video_acceleration))
        return 0;
    const uint32_t base = 0xF0000000u | (uint32_t)slot << 24;
    const mc_ppc_port direct = {MC_PPC_PORT_DIRECT, 0, 0, 0};
    if (!mc_bart_fit(&p->bart, slot) ||
        !mc_bart_map_memory(&p->bart, slot, MC_LIVADIX_CARD_VRAM_OFFSET, p->card.vram_size, p->card.vram, 1) ||
        !mc_bart_map_device(&p->bart, slot, MC_LIVADIX_CARD_REGS_OFFSET, 0x1000u, &p->card, &mc_livadix_card_ops) ||
        !mc_bart_map_memory(&p->bart, slot, MC_LIVADIX_CARD_ROM_OFFSET, MC_LIVADIX_CARD_ROM_WINDOW, p->card.rom, 0))
        return 0;
    mc_bus_map_memory(&m->bus, base + MC_LIVADIX_CARD_VRAM_OFFSET, p->card.vram_size, p->card.vram, p->card.vram_size, 1);
    mc_bus_map_memory(&m->bus, base + MC_LIVADIX_CARD_ROM_OFFSET, MC_LIVADIX_CARD_ROM_WINDOW, p->card.rom, MC_LIVADIX_CARD_ROM_WINDOW, 0);
    if (!mc_ppc_attach(m, p->nubus, base - 0xF0000000u + MC_LIVADIX_CARD_REGS_OFFSET, 0x1000u, &p->card, &mc_livadix_card_ops, direct))
        return 0;
    p->card_slot = slot;
    return 1;
}

/* video.depth on the Livadix Display Card, as if chosen in the Monitors control
 * panel: its slot's eight bytes of extended PRAM from $46 for slot $9 (board
 * ID, then vendorUse1, the depth mode $80 + log2(depth) the driver starts
 * in; Designing Cards and Drivers for the Macintosh Family, 3rd ed.,
 * chapter 9), once the Slot Manager has set the record up for the card
 * (quadra700.c, set_card_depth). */
static void set_card_depth(pdm *p, uint8_t *pram) {
    uint8_t *s = pram + 0x46 + 8 * (p->card_slot - 9);
    if (pram[0x10] != 0xA8 || s[0] != (MC_LIVADIX_CARD_BOARD_ID >> 8) || s[1] != (MC_LIVADIX_CARD_BOARD_ID & 0xFF)) return;
    unsigned mode = 0x80;
    for (uint32_t d = p->m->config.depth; d > 1; d >>= 1) mode++;
    s[2] = (uint8_t)mode;
}

static void *build(mc_ppc *m, const pdm_board *b) {
    pdm *p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        return NULL;
    }
    p->m = m;
    p->board = b;
    m->board = p;
    const mc_hw_host *host = &m->host;
    uint32_t banks[8];
    if (!ram_banks(b, m->ram_size, banks)) {
        snprintf(m->error, sizeof m->error, "emulation.ram_mb: %u MiB does not fit the board's banks", (unsigned)(m->ram_size / MB));
        free(p);
        return NULL;
    }
    mc_hmc_init(&p->hmc, host, "HMC", b->hmc, 8u * MB, banks, b->simm_banks, b->cache_kb);
    mc_hmc_set_remap(&p->hmc, remap_ram, p);
    map_ram(p);
    const int io = mc_ppc_region(m, "io");
    const mc_ppc_port byte = {MC_PPC_PORT_SIZED, 1, 0, 0}, direct = {MC_PPC_PORT_DIRECT, 0, 0, 0};
    m->bus.region[io].cost = 8;     /* an I/O cycle 0.5 us, as on the Quadra 700 */
    mc_ppc_attach(m, io, 0x00F40000u, MC_HMC_SIZE, &p->hmc, &mc_hmc_ops, direct);
    mc_cpuid_init(&p->cpuid, host, "CPU ID", b->cpuid);
    mc_ppc_attach(m, io, 0x0FFFFFF8u, 8, &p->cpuid, &mc_cpuid_ops, direct);
    /* The AMIC and its codec. */
    mc_amic_init(&p->amic, host, "AMIC");
    p->amic.variant = b->amic_variant;
    mc_awac_init(&p->awac, host, "AWAC");
    mc_amic_set_codec(&p->amic, &p->awac);
    mc_ppc_attach(m, io, 0x0F2A000u, MC_AMIC_INT_SIZE, &p->amic, &mc_amic_int_ops, direct);
    mc_ppc_attach(m, io, 0x0F26000u, MC_AMIC_VIA2_SIZE, &p->amic, &mc_amic_via2_ops, direct);
    mc_ppc_attach(m, io, 0x0F31000u, MC_AMIC_DMA_SIZE, &p->amic, &mc_amic_dma_ops, direct);
    mc_ppc_attach(m, io, 0x0F14000u, MC_AMIC_SOUND_SIZE, &p->amic, &mc_amic_sound_ops, direct);
    mc_ppc_attach(m, io, 0x0F2C000u, MC_AMIC_DIAG_SIZE, &p->amic, &mc_amic_diag_ops, direct);
    /* Video. */
    mc_pdm_monitor monitor = mc_pdm_video_monitor_for(m->config.width, m->config.height);
    const int card = m->config.video_card == MC_VIDEO_CARD_LIVADIX || monitor == MC_PDM_MONITOR_NONE;
    if (card) monitor = MC_PDM_MONITOR_NONE;
    mc_pdm_video_init(&p->video, host, "video", monitor);
    mc_ppc_attach(m, io, 0x0F28000u, MC_PDM_VIDEO_SIZE, &p->video, &mc_pdm_video_ops, direct);
    mc_ariel_init(&p->ariel, host, "Ariel II");
    mc_ppc_attach(m, io, 0x0F24000u, MC_ARIEL_SIZE, &p->ariel, &mc_ariel_ops, direct);
    /* VIA1. */
    mc_via_init(&p->via1, host, "VIA1", 9);
    mc_ppc_attach(m, io, 0x0F00000u, 0x2000, p, &via1_ops, byte);
    /* The SCC. */
    mc_scc_init(&p->scc, host);
    mc_scc_set_clocks(&p->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port sport = {p, scc_transmit, scc_outputs};
    mc_scc_set_port(&p->scc, &sport);
    mc_ppc_attach(m, io, 0x0F04000u, 0x2000, &p->scc, &mc_scc_ops, byte);
    /* SCSI. */
    for (unsigned i = 0; i < b->scsi_buses; i++) {
        mc_scsi_bus_init(&p->scsi_bus[i]);
        mc_ncr53c96_init(&p->scsi[i], host, &p->scsi_bus[i], 25000000u);
        mc_ppc_attach(m, io, 0x0F10000u + 0x1000u * i, 0x100, &p->scsi[i], &scsi_ops, byte);
        mc_ppc_attach(m, io, 0x0F10100u + 0x1000u * i, 0x100, &p->scsi[i], &pdma_ops, direct);
    }
    /* With one SCSI bus the controller also answers at $50F1 1000: the
     * ROM tells the 6100 (CPU ID $3010, ProductInfo $3011) from an 8100
     * board by probing a second controller there for the 53CF94's
     * identification $A2 ($FFC100A2-$FFC100F4), and the 6100's record is
     * the one it takes when the probe answers otherwise†. */
    if (b->scsi_buses == 1) {
        mc_ppc_attach(m, io, 0x0F11000u, 0x100, &p->scsi[0], &scsi_ops, byte);
        mc_ppc_attach(m, io, 0x0F11100u, 0x100, &p->scsi[0], &pdma_ops, direct);
    }
    {
        const mc_amic_port a = {p, scsi_a_read, scsi_a_write}, bb = {p, scsi_b_read, scsi_b_write};
        mc_amic_set_port(&p->amic, MC_AMIC_CH_SCSI_A, &a);
        mc_amic_set_port(&p->amic, MC_AMIC_CH_SCSI_B, &bb);
    }
    mc_scsi_cd_init(&p->unit[CD_ID]);
    mc_scsi_unit_on_eject(&p->unit[CD_ID], cd_ejected, p);
    mc_scsi_bus_attach(&p->scsi_bus[0], CD_ID, mc_scsi_unit_target(&p->unit[CD_ID]));
    /* The floppy. */
    mc_swim3_init(&p->swim3, host, "SWIM III", 1);
    mc_ppc_attach(m, io, 0x0F16000u, MC_SWIM3_SIZE, &p->swim3, &mc_swim3_ops, byte);
    {
        const mc_amic_port f = {&p->swim3, mc_swim3_dma_read, mc_swim3_dma_write};
        mc_amic_set_port(&p->amic, MC_AMIC_CH_FLOPPY, &f);
    }
    /* Ethernet. */
    mc_am79c940_init(&p->enet, host, "MACE");
    {
        const mc_am79c940_port e = {p, enet_transmit, enet_dma_receive};
        mc_am79c940_set_port(&p->enet, &e);
    }
    mc_amic_set_enet_transmit(&p->amic, enet_dma_transmit, p);
    mc_ppc_attach(m, io, 0x0F0A000u, MC_AM79C940_SIZE, &p->enet, &mc_am79c940_ops, byte);
    mc_sonic_prom_init(&p->enet_prom, host, mc_sonic_default_address, 4, 1);
    mc_ppc_attach(m, io, 0x0F08000u, 0x1000, &p->enet_prom, &mc_sonic_prom_ops, byte);
    /* NuBus through BART (on the 6100 its NuBus adapter, taken as fitted):
     * its registers, and the slot space where cards place their windows;
     * every other NuBus address times out (a transfer error). */
    mc_bart_init(&p->bart, host, "BART", b->slots);
    p->bart.id = b->bart_id;
    p->nubus = mc_bus_add_io(&m->bus, 0xF0000000u, 0x0F000000u, 0x0FFFFFFFu, 0);
    mc_ppc_attach(m, p->nubus, 0, MC_BART_SIZE, &p->bart, &mc_bart_ops, direct);
    if (card && !fit_card(p)) {
        snprintf(m->error, sizeof m->error, "The Livadix Display Card cannot show %ux%u", (unsigned)m->config.width,
                 (unsigned)m->config.height);
        mc_amic_free(&p->amic);
        mc_livadix_card_free(&p->card);
        free(p);
        return NULL;
    }
    /* ADB, the clock, PRAM and power: Cuda. */
    mc_adb_bus_init(&p->adb);
    mc_adb_keyboard_init(&p->keyboard);
    mc_adb_mouse_init(&p->mouse);
    mc_adb_attach(&p->adb, &p->keyboard.dev);
    mc_adb_attach(&p->adb, &p->mouse.dev);
    mc_adbctl_init(&p->cuda, host, "Cuda", MC_ADBCTL_CUDA, &p->via1, &p->adb);
    mc_adbctl_set_part(&p->cuda, "341S0788");
    {
        /* Cuda at the protocol level, or Apple's firmware when the roms
         * folders hold it (emulation.controller), as on the 68k Cuda
         * machines. */
        static const char *const keys[2] = {"cuda-341s0788", NULL};
        if (mc_adbctl_select(&p->cuda, (mc_adbctl_level)m->config.controller, m->config.roms, keys) < 0) {
            snprintf(m->error, sizeof m->error, "emulation.controller: firmware: the Cuda firmware (cuda-341s0788) is not in the roms folders");
            mc_amic_free(&p->amic);
            free(p);
            return NULL;
        }
    }
    /* PRAM and the clock in the configuration's PRAM file (emulation.nvram,
     * else in the folder nvram beside the configuration), as on the 68k
     * Cuda machines. */
    if (!mc_config_nvram_path(&m->config, p->nvram, sizeof p->nvram)) p->nvram[0] = 0;
    if (!mc_adbctl_nvram_load(&p->cuda, p->nvram)) {
        fresh_pram(mc_adbctl_pram(&p->cuda));
        mc_adbctl_set_seconds(&p->cuda, 0);
    }
    if (p->card_slot) set_card_depth(p, mc_adbctl_pram(&p->cuda));
    if (m->config.boot_priority == MC_BOOT_CDROM) {
        uint8_t *pram = mc_adbctl_pram(&p->cuda);
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    mc_adbctl_ops.reset(&p->cuda);
    if (!p->amic.ring) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        mc_amic_free(&p->amic);
        free(p);
        return NULL;
    }
    return p;
}

static void *build_6100(mc_ppc *m) { return build(m, &board_6100); }
static void *build_7100(mc_ppc *m) { return build(m, &board_7100); }
static void *build_8100(mc_ppc *m) { return build(m, &board_8100); }
static void *build_6150(mc_ppc *m) { return build(m, &board_6150); }
static void *build_7100_80(mc_ppc *m) { return build(m, &board_7100_80); }
static void *build_8100_fast(mc_ppc *m) { return build(m, &board_8100_fast); }
static void *build_9150_80(mc_ppc *m) { return build(m, &board_9150_80); }
static void *build_9150_120(mc_ppc *m) { return build(m, &board_9150_120); }

static const uint16_t ram_6100[] = {8, 12, 16, 24, 40, 72, 0};
static const uint16_t ram_7100[] = {8, 12, 16, 24, 40, 72, 136, 0};
static const uint16_t ram_8100[] = {8, 12, 16, 24, 40, 72, 136, 264, 0};

static const char *const models_6100[] = {"power-macintosh-6100", NULL};
static const char *const models_7100[] = {"power-macintosh-7100", NULL};
static const char *const models_8100[] = {"power-macintosh-8100", NULL};
static const char *const models_6150[] = {"workgroup-server-6150", NULL};
static const char *const models_8150[] = {"workgroup-server-8150", NULL};
static const char *const models_7100_80[] = {"power-macintosh-7100-80", NULL};
static const char *const models_8100_100[] = {"power-macintosh-8100-100", NULL};
static const char *const models_8100_110[] = {"power-macintosh-8100-110", NULL};
static const char *const models_9150_80[] = {"workgroup-server-9150-80", NULL};
static const char *const models_9150_120[] = {"workgroup-server-9150-120", NULL};

/* The 601's RTC input: C15M / 2. The nanokernel's clock measurement
 * (ROM $FFF03020) scales its decrementer count by $3BC40000 / 1e9, the ratio
 * of 7.8336 MHz to the 7.8125 MHz the 601's 128 ns step assumes (MPC601UM
 * 2.2.5.3); with it the measured clock lands on the standard values the
 * ROM's table at $FFF0370C holds, and the box flag of $FFC0139C. */
#define PDM_RTC_HZ 7833600u

/* The built-in video's modes (pdm_video.h: the monitor a size names, its
 * deepest depth) and the Livadix Display Card in a NuBus slot or the 6100's
 * processor-direct slot. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return MC_DEPTHS_UPTO(mc_pdm_video_max_depth(mc_pdm_video_monitor_for(w, h)));
}
static const mc_hw_caps pdm_caps = {video_builtin, 1, 0, NULL};

#define PDM_DESC(id, mname, hz, rams, b)                                                                              \
    const mc_ppc_desc mc_pdm##id##_desc = {                                                                           \
        .key = "pdm" #id, .models = models_##id, .name = mname, .cpu = PPC_MODEL_601, .cpu_hz = hz,                  \
        .timebase_hz = PDM_RTC_HZ, .cpi = 8, .cpi_div = 9, .roms = pdm_roms, .ram_mb = rams, .map = pdm_map,            \
        .nmap = sizeof pdm_map / sizeof *pdm_map, .unmapped = MC_PPC_UNMAPPED_TEA, .build = b, .ops = &pdm_ops,       \
        .caps = &pdm_caps,                                                                                            \
    }

PDM_DESC(6100, "Power Macintosh 6100", 60000000u, ram_6100, build_6100);
PDM_DESC(7100, "Power Macintosh 7100", 66000000u, ram_7100, build_7100);
PDM_DESC(8100, "Power Macintosh 8100", 80000000u, ram_8100, build_8100);
PDM_DESC(6150, "Workgroup Server 6150", 66000000u, ram_6100, build_6150);
PDM_DESC(8150, "Workgroup Server 8150", 80000000u, ram_8100, build_8100);
PDM_DESC(7100_80, "Power Macintosh 7100/80", 80000000u, ram_7100, build_7100_80);
PDM_DESC(8100_100, "Power Macintosh 8100/100", 100000000u, ram_8100, build_8100_fast);
PDM_DESC(8100_110, "Power Macintosh 8100/110", 110000000u, ram_8100, build_8100_fast);
PDM_DESC(9150_80, "Workgroup Server 9150/80", 80000000u, ram_8100, build_9150_80);
PDM_DESC(9150_120, "Workgroup Server 9150/120", 120000000u, ram_8100, build_9150_120);

