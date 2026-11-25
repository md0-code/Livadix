/* Livadix — the first PCI Power Macintosh generation: the 7200, 7500, 8500 and
 * 9500, and the 7300, 7600, 8600 and 9600 on the same boards.
 *
 * The physical map (Designing PCI Cards and Drivers for Power Macintosh
 * Computers, Apple, 1995, Table 2-1, the machines'
 * table): RAM from $0000 0000 through the memory controller's banks
 * (Hammerhead, hammerhead.h; Platinum on the 7200, platinum.h); PCI memory
 * $8000 0000-$EFFF FFFF and $F900 0000-$FEFF FFFF through the Bandits'
 * address masks (bandit.h); host bridge n's control space at $F000 0000 +
 * n x $200 0000: Chaos (the video bus, Control and Plan B: the 7500, 8500,
 * 7600, 8600) at $F000 0000, Bandit 1 at $F200 0000, the 9500's and 9600's
 * Bandit 2 at $F400 0000; the 7200's VRAM at $F100 0000 (Platinum); the
 * memory controller's registers at $F800 0000; the ROM, 4 MB, repeating
 * through $FF00 0000-$FFFF FFFF (reset at $FFF0 0100).
 *
 * Grand Central (grand_central.h) at device 16 of Bandit 1, its BAR at
 * $F300 0000 (POST, $FFF036C4), collects the interrupts and runs the I/O
 * devices' DBDMA channels; its devices (the 7500 and 8500 Developer Note,
 * "Grand Central I/O Subsystem IC"; Designing PCI Cards, Listing 8-1; ROM):
 *   + $10000 Curio's 53C94 (ncr53c96.h; registers every $10), the
 *            external SCSI bus (the 7200's only one), DBDMA channel 0,
 *            source 12
 *   + $11000 Curio's MACE (am79c940.h; registers every $10), channels 2
 *            (transmit) and 3 (receive), source 14
 *   + $13000 Curio's SCC (scc.h): channel B control + $00, data + $10,
 *            channel A control + $20, data + $30; sources 15 (A), 16 (B)
 *   + $14000 the AWACS port (awacs.h) with the AWAC codec (awac.h),
 *            channels 8 (out) and 9 (in), source 17
 *   + $15000 SWIM III (swim3.h; registers every $10), channel 1, source 19
 *   + $16000 Cuda's VIA (via.h; registers every $200), source 18
 *   + $18000 MESH (mesh.h), the internal SCSI bus (not on the 7200),
 *            channel 10, source 13
 *   + $19000 the Ethernet address ROM
 *   + $1B000 the DAC (radacal.h: RaDACal, the 7200's DACula)
 *   + $1D000, $1F000 the NVRAM (nvram.h)
 * Cuda (adbctl.h, 341S0788) on the VIA's port B and shift register as on
 * the PDM boards; its NMI on source 20.
 *
 * Livadix's choices†: the disks on the internal bus (MESH; the 7200's 53C94)
 * at the SCSI IDs the PDM boards use, the CD-ROM drive at 3; a PCI memory
 * cycle nobody claims reads all ones (the bridge's master abort) rather
 * than raising the machine check; the VRAM mapped as memory where the
 * video controller's BAR or window puts it (its bytes in address order).
 *
 * Livadix's own code. */
#include "../ppc_machine.h"
#include "../chips/awac.h"
#include "../chips/awacs.h"
#include "../chips/bandit.h"
#include "../chips/control.h"
#include "../chips/dbdma.h"
#include "../chips/grand_central.h"
#include "../chips/hammerhead.h"
#include "../chips/mesh.h"
#include "../chips/nvram.h"
#include "../chips/planb.h"
#include "../chips/platinum.h"
#include "../chips/radacal.h"
#include "../chips/swim3.h"
#include "../../adb.h"
#include "../../adbctl.h"
#include "../../am79c940.h"
#include "../../ncr53c96.h"
#include "../../scc.h"
#include "../../scsi_bus.h"
#include "../../scsi_target.h"
#include "../../sonic.h"
#include "../../via.h"
#include "../../guest/guest.h"
#include "../../livadix_card.h"
#include "../../storage/vdisk.h"
#include "../../hw_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MB (1024u * 1024u)

/* --- the boards ------------------------------------------------------------------- */

enum { MEMC_HAMMERHEAD, MEMC_PLATINUM };
enum { VIDEO_NONE, VIDEO_CONTROL, VIDEO_PLATINUM };

typedef struct pci1_board {
    uint8_t memc, video;
    uint8_t bandits;            /* 1, or 2 on the 9500 and 9600 */
    uint8_t mesh;               /* the internal bus is MESH's (not the 7200's) */
    uint8_t l2;                 /* MC_HAMMERHEAD_L2_* / MC_PLATINUM_L2_* */
    uint32_t id;                /* the memory controller's identification */
    uint8_t straps;             /* Hammerhead's $020 */
    uint16_t gc_id;             /* Grand Central + $1A000 */
    uint8_t vram_mb;            /* Control's VRAM */
} pci1_board;

/* The identification's bits 11-8 and 15, 6 give POST the bus clock
 * ($FFF03AB8: with bits 15 and 6 clear the table at $FFF03B1C, where code
 * 3 is 50 MHz). */
#define ID_BUS_50 0x39000300u

static const pci1_board board_7200 = {MEMC_PLATINUM, VIDEO_PLATINUM, 1, 0, MC_PLATINUM_L2_NONE, ID_BUS_50, 0, 0x0100, 4};
static const pci1_board board_7500 = {MEMC_HAMMERHEAD, VIDEO_CONTROL, 1, 1, MC_HAMMERHEAD_L2_NONE, ID_BUS_50, 0x80, 0xE100, 4};
static const pci1_board board_8500 = {MEMC_HAMMERHEAD, VIDEO_CONTROL, 1, 1, MC_HAMMERHEAD_L2_256K, ID_BUS_50, 0x80, 0x4900, 4};
/* Apple 7300/7600/8600/9600 Developer Note, Table 1-1: the 7300
 * retains Control video, without the 7500/7600's external AV input. The original
 * System 8.0 gpch 201 ($216-$2A2) recognises Hammerhead $39, box 62,
 * and Grand Central ID bit 23 clear as the 7300 (Gestalt 109). The 7500
 * keeps that pin high; bit 21 is high on both. IDs below are PCI-native
 * little endian, so $6000 reads as $00600000 through a CPU long access. */
static const pci1_board board_7300 = {MEMC_HAMMERHEAD, VIDEO_CONTROL, 1, 1, MC_HAMMERHEAD_L2_256K, ID_BUS_50, 0x80, 0x6100, 4};
static const pci1_board board_9500 = {MEMC_HAMMERHEAD, VIDEO_NONE, 2, 1, MC_HAMMERHEAD_L2_512K, ID_BUS_50, 0x40, 0x4100, 0};

#define CD_ID 3
static const int disk_ids[] = {0, 1, 2, 4, 5, 6};

/* An Ethernet frame waiting for the receive channel. */
#define RX_FRAMES 16
#define FRAME_MAX 1536

typedef struct pci1 {
    mc_ppc *m;
    const pci1_board *board;
    mc_hammerhead hh;
    int hh_ram_region;          /* separate views of paired DIMMs */
    mc_platinum plat;
    mc_bandit bandit[2], chaos;
    mc_gc gc;
    mc_control_chip control;
    mc_planb planb;
    mc_livadix_card card;          /* the PCI Livadix Display Card in Bandit 1 slot 13 when fitted */
    int card_fitted;
    mc_radacal dac;
    mc_nvram nvram;
    mc_awacs awacs;
    mc_awac awac;
    mc_swim3 swim3;
    mc_via via;
    mc_scc scc;
    mc_scsi_bus scsi_bus[2];    /* 0 the internal bus (MESH; the 7200's 53C94), 1 the 53C94's external bus */
    mc_mesh mesh;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];
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
    char nvram_path[1024];
    int64_t host_start;
    uint32_t vram_base;         /* where Control's or Platinum's VRAM is mapped (0: not) */
    uint32_t card_vram_base;    /* where the Livadix card's BAR0 is mapped (0: not) */
    uint64_t fb_serial;
    uint32_t palette[256];
    /* Ethernet: the transmit channel's frame, the frames received. */
    uint8_t tx[FRAME_MAX];
    unsigned tx_n;
    uint8_t rx[RX_FRAMES][FRAME_MAX];
    uint16_t rx_len[RX_FRAMES];
    unsigned rx_head, rx_n, rx_i;
} pci1;

/* MPC604eUM 2.1.2.4 and Table 7-6: HID1 exposes the read-only
 * PLL_CFG[0..3] pins in bits 0..3. The later PCI ROM derives its processor
 * frequency from these pins and the bus clock (Apple enhanced 8600/9600
 * Developer Note, Table 1-2: 300/50 and 350/50 MHz). */
static void cpu_straps(pci1 *p) {
    if (p->m->cpu.model != PPC_MODEL_604E) return;
    /* Apple's enhanced models use the 604e-based Mach 5. ROM 960FC647
     * $FFF03E88-$FFF03EC8 selects its 7:1 PLL table for PVR version $A;
     * version $9 treats that pin setting as 1:1. Retain revision 1.0. */
    if (p->m->model && (!strcmp(p->m->model->key, "power-macintosh-8600-300") ||
                        !strcmp(p->m->model->key, "power-macintosh-9600-350")))
        p->m->cpu.pvr = 0x000A0100u;
    static const uint8_t pll[] = {15, 15, 0, 12, 4, 6, 8, 14, 10, 7, 11, 9, 13, 5, 2};
    const uint64_t bus = (uint64_t)p->m->desc->timebase_hz * 4u;
    const uint64_t hz = p->m->rate / p->m->div;
    const uint64_t half_ratio = (hz * 2u + bus / 2u) / bus;
    const unsigned code = half_ratio < sizeof pll ? pll[half_ratio] : 0;
    p->m->cpu.hid[1] = (uint32_t)code << 28;
}

/* --- memory ----------------------------------------------------------------------- */

/* Canonical RAM holds each equal DIMM pair in eight-byte lanes (9500
 * Developer Note, "Data Interleaving"). Before POST combines the pair,
 * each bank's independent sizing window accesses only its own lanes. */
static uint32_t hh_ram_read(void *dev, uint32_t address, unsigned size) {
    pci1 *p = dev;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        uint32_t at = 0;
        const uint8_t byte = mc_hammerhead_decode(&p->hh, address + i, NULL, &at)
                             ? p->m->ram[at] : 0xFFu;
        value = value << 8 | byte;
    }
    return value;
}
static void hh_ram_write(void *dev, uint32_t address, unsigned size, uint32_t value) {
    pci1 *p = dev;
    for (unsigned i = 0; i < size; i++) {
        uint32_t at = 0;
        if (mc_hammerhead_decode(&p->hh, address + i, NULL, &at))
            p->m->ram[at] = (uint8_t)(value >> (8u * (size - 1u - i)));
    }
}
static const mc_dev_ops hh_ram_ops = {hh_ram_read, hh_ram_write, NULL, NULL};

static void map_ram(pci1 *p) {
    mc_ppc *m = p->m;
    mc_mem_window w[MC_HAMMERHEAD_BANKS];
    const unsigned n = p->board->memc == MEMC_PLATINUM ? mc_platinum_windows(&p->plat, w, MC_HAMMERHEAD_BANKS)
                                                       : mc_hammerhead_windows(&p->hh, w, MC_HAMMERHEAD_BANKS);
    mc_bus_map_float(&m->bus, 0, 0x80000000u);
    for (unsigned i = 0; i < n; i++) {
        if (w[i].base >= 0x80000000u) continue;
        const uint32_t span = (uint32_t)((uint64_t)w[i].base + w[i].size > 0x80000000u
                              ? 0x80000000u - w[i].base : w[i].size);
        const unsigned bank = w[i].bank, even = bank & ~1u;
        if (p->hh_ram_region && p->hh.bank_size[even] &&
            p->hh.bank_size[even] == p->hh.bank_size[even + 1] &&
            !mc_hammerhead_interleaved(&p->hh, even)) {
            mc_bus_remap(&m->bus, p->hh_ram_region, w[i].base, span);
        } else {
            mc_bus_map_memory(&m->bus, w[i].base, span, m->ram + w[i].offset, w[i].bank_size, 1);
        }
    }
}

/* RAM in banks of powers of two from 4 to 64 MB, largest first. */
static int ram_banks(uint32_t total, uint32_t *banks, unsigned max) {
    memset(banks, 0, max * sizeof *banks);
    unsigned n = 0;
    for (uint32_t size = 64u * MB; size >= 4u * MB && total; size >>= 1)
        while (total >= size && n < max) banks[n++] = size, total -= size;
    return total == 0 && n > 0;
}

/* --- the VRAM where its controller puts it ---------------------------------------- */

static void map_vram(pci1 *p) {
    mc_ppc *m = p->m;
    uint32_t base = 0, size = 0, vram_size = 0;
    uint8_t *vram = NULL;
    if (p->board->video == VIDEO_CONTROL) {
        if (mc_control_decoding(&p->control)) base = mc_pci_config_base(&p->control.cfg, 2);
        if (base) base += MC_CONTROL_FB, size = MC_CONTROL_APERTURE - MC_CONTROL_FB;
        vram = p->control.vram, vram_size = p->control.vram_size;
    }
    if (base == p->vram_base) return;
    if (p->vram_base) mc_bus_remap(&m->bus, mc_ppc_region(m, p->vram_base >= 0xF0000000u ? "chaos" : "pci"), p->vram_base, MC_CONTROL_APERTURE - MC_CONTROL_FB);
    p->vram_base = base;
    if (base) mc_bus_map_memory(&m->bus, base, size, vram, vram_size, 1);
}

/* The Livadix Display Card's BAR0 (VRAM) as memory on the bus when memory
 * decoding is on, so the shell and guest reach it without a PCI cycle. */
static void map_card_vram(pci1 *p) {
    mc_ppc *m = p->m;
    uint32_t base = 0;
    if (p->card_fitted && mc_livadix_card_pci_decoding(&p->card)) base = mc_pci_config_base(&p->card.cfg, 0);
    if (base == p->card_vram_base) return;
    if (p->card_vram_base)
        mc_bus_remap(&m->bus, mc_ppc_region(m, p->card_vram_base >= 0xF0000000u ? "pci-high" : "pci"), p->card_vram_base,
                     p->card.vram_size);
    p->card_vram_base = base;
    if (base) mc_bus_map_memory(&m->bus, base, p->card.vram_size, p->card.vram, p->card.vram_size, 1);
}

static void card_config_write(void *dev, unsigned reg, unsigned size, uint32_t value) {
    pci1 *p = dev;
    mc_livadix_card_function(&p->card).config_write(&p->card, reg, size, value);
    map_card_vram(p);
}
static uint32_t card_config_read(void *dev, unsigned reg, unsigned size) {
    pci1 *p = dev;
    return mc_livadix_card_function(&p->card).config_read(&p->card, reg, size);
}
static int card_mem_read(void *dev, uint32_t address, unsigned size, uint32_t *value) {
    pci1 *p = dev;
    return mc_livadix_card_function(&p->card).mem_read(&p->card, address, size, value);
}
static int card_mem_write(void *dev, uint32_t address, unsigned size, uint32_t value) {
    pci1 *p = dev;
    return mc_livadix_card_function(&p->card).mem_write(&p->card, address, size, value);
}

static void control_config_write(void *dev, unsigned reg, unsigned size, uint32_t value) {
    pci1 *p = dev;
    mc_control_function(&p->control).config_write(&p->control, reg, size, value);
    map_vram(p);
}
static uint32_t control_config_read(void *dev, unsigned reg, unsigned size) {
    pci1 *p = dev;
    return mc_control_function(&p->control).config_read(&p->control, reg, size);
}
static int control_mem_read(void *dev, uint32_t address, unsigned size, uint32_t *value) {
    pci1 *p = dev;
    return mc_control_function(&p->control).mem_read(&p->control, address, size, value);
}
static int control_mem_write(void *dev, uint32_t address, unsigned size, uint32_t value) {
    pci1 *p = dev;
    return mc_control_function(&p->control).mem_write(&p->control, address, size, value);
}

/* --- PCI memory space ------------------------------------------------------------- */

/* $8000 0000-$EFFF FFFF and $F900 0000-$FEFF FFFF: Chaos's functions (Open
 * Firmware gives Control's BARs a 256 MB window of Chaos's: the chaos
 * node's ranges)†, else the Bandit whose address mask opens the window. */
static uint32_t pcimem_read(void *dev, uint32_t offset, unsigned size) {
    pci1 *p = dev;
    const uint32_t address = 0x80000000u + offset;
    uint32_t v = size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFFu : 0xFFu;
    if (p->board->video == VIDEO_CONTROL && mc_bandit_mem_read(&p->chaos, address, size, &v)) return v;
    for (unsigned i = 0; i < p->board->bandits; i++)
        if (mc_bandit_decodes(&p->bandit[i], address)) {
            mc_bandit_mem_read(&p->bandit[i], address, size, &v);
            break;
        }
    return v;
}
static void pcimem_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pci1 *p = dev;
    const uint32_t address = 0x80000000u + offset;
    if (p->board->video == VIDEO_CONTROL && mc_bandit_mem_write(&p->chaos, address, size, value)) return;
    for (unsigned i = 0; i < p->board->bandits; i++)
        if (mc_bandit_decodes(&p->bandit[i], address)) {
            mc_bandit_mem_write(&p->bandit[i], address, size, value);
            return;
        }
}
static int pcimem_ops_stable(void *dev, uint32_t offset, unsigned size) {
    pci1 *p = dev; uint32_t off;
    return mc_pci_config_hit(&p->gc.cfg, 0, 0x80000000u + offset, &off) &&
           mc_gc_ops.stable(&p->gc, off, size);
}
static const mc_dev_ops pcimem_ops = {pcimem_read, pcimem_write, NULL, NULL, pcimem_ops_stable};

/* --- Grand Central's devices ------------------------------------------------------ */

static uint32_t via_read(void *dev, uint32_t offset, unsigned size) {
    pci1 *p = dev;
    /* Grand Central exposes a VIA-compatible synchronous interface.
     * One 783.36 kHz E cycle per access (via.h, Guide chapter 4) keeps
     * the host handshake observable to the 2.097 MHz Cuda firmware. */
    p->m->bus.cost_due += MC_VIA_ECLOCK;
    mc_adbctl_sync(&p->cuda);
    return mc_via_ops.read(&p->via, offset, size);
}
static void via_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pci1 *p = dev;
    p->m->bus.cost_due += MC_VIA_ECLOCK;
    mc_adbctl_sync(&p->cuda);
    mc_via_ops.write(&p->via, offset, size, value);
}
static const mc_dev_ops via_ops = {via_read, via_write, NULL, NULL, NULL};

/* Grand Central + $1A000: the board's identification, a 16-bit register.
 * Bit 8 is high for normal startup: the original Serial Test Manager
 * (ROM 9630C68B $FFF201FC-$FFF20218) reads with lwbrx and enters its
 * interactive console when this diagnostic strap is low. The cold PRAM
 * path may skip that check; it must also boot after PRAM has been saved.
 * The firmware takes its machine-family pins from this register
 * (Open Firmware's root node reads it little-endian and takes bits 15-11,
 * ROM file $34052C). */
static uint32_t board_id_read(void *dev, uint32_t offset, unsigned size) {
    const pci1 *p = dev;
    const uint16_t id = p->board->gc_id;
    const uint32_t v = (uint32_t)id >> (8 * (offset & 1u));
    return size == 1 ? v & 0xFFu : v & 0xFFFFu;
}
static void readonly_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { (void)dev, (void)offset, (void)size, (void)value; }
static const mc_dev_ops board_id_ops = {board_id_read, readonly_write, NULL, NULL, NULL};

/* Grand Central + $1E000: the 8500's and 8600's AV connector
 * sense. Apple TIL 18473 and 18559 describe the detection circuit on
 * the AV ribbon. Control's ROM 9630C68B ndrv tests the big-endian
 * word at file $2701D4-$2701FC with mask $C000. When both bits are
 * high, the driver marks video out absent. Both TV outputs are unloaded. */
static uint32_t av_sense_read(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)size;
    return offset == 0 ? 0xC0u : 0;
}

static const mc_dev_ops av_sense_ops = {av_sense_read, readonly_write, NULL, NULL, NULL};

/* The SCC's four ports every $10: B control, B data, A control, A data;
 * scc.c takes the Macintosh's order (B control +0, A control +2, B data +4,
 * A data +6). */
static uint32_t scc_offset(uint32_t offset) {
    static const uint8_t order[4] = {0, 4, 2, 6};
    return order[(offset >> 4) & 3u];
}
static uint32_t scc_read(void *dev, uint32_t offset, unsigned size) { return mc_scc_ops.read(dev, scc_offset(offset), size); }
static void scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { mc_scc_ops.write(dev, scc_offset(offset), size, value); }
static const mc_dev_ops scc_ops = {scc_read, scc_write, NULL, NULL, NULL};

/* The 53C94's DMA: DACK cycles on DBDMA channel 0's stream. */
static int scsi_dma_read(void *dev, uint8_t *byte) {
    uint32_t v = 0;
    if (!mc_ncr53c96_dma_read(dev, 1, &v)) return MC_DBDMA_NONE;
    *byte = (uint8_t)v;
    return MC_DBDMA_BYTE;
}
static int scsi_dma_write(void *dev, uint8_t byte) { return mc_ncr53c96_dma_write(dev, 1, byte) == 1; }

static int swim3_dma_read(void *dev, uint8_t *byte) { return mc_swim3_dma_read(dev, byte) ? MC_DBDMA_BYTE : MC_DBDMA_NONE; }
static int swim3_dma_write(void *dev, uint8_t byte) { return mc_swim3_dma_write(dev, byte); }

/* The codec behind the AWACS port. */
static void codec_control(void *ctx, uint32_t word) { mc_awac_control(ctx, (uint16_t)word); }
static uint32_t codec_status(void *ctx) {
    const mc_awac *a = ctx;
    return (uint32_t)mc_awac_status(a, 0) | (uint32_t)mc_awac_status(a, 1) << 8 | (uint32_t)mc_awac_status(a, 2) << 16 |
           (uint32_t)mc_awac_status(a, 3) << 24;
}

/* --- Ethernet --------------------------------------------------------------------- */

/* Transmit channel 2: the bytes of a frame until its OUTPUT_LAST. */
static int enet_tx_write(void *dev, uint8_t byte) {
    pci1 *p = dev;
    if (p->tx_n < FRAME_MAX) p->tx[p->tx_n++] = byte;
    return 1;
}
static void enet_tx_end(void *dev) {
    pci1 *p = dev;
    if (p->tx_n) mc_am79c940_transmit(&p->enet, p->tx, p->tx_n);
    p->tx_n = 0;
}
/* Receive channel 3: a frame and its four status bytes, then the record's end. */
static int enet_rx_read(void *dev, uint8_t *byte) {
    pci1 *p = dev;
    if (!p->rx_n) return MC_DBDMA_NONE;
    if (p->rx_i >= p->rx_len[p->rx_head]) {
        p->rx_head = (p->rx_head + 1u) % RX_FRAMES;
        p->rx_n--;
        p->rx_i = 0;
        return MC_DBDMA_END;
    }
    *byte = p->rx[p->rx_head][p->rx_i++];
    return MC_DBDMA_BYTE;
}
static int enet_dma_receive(void *ctx, const uint8_t *frame, size_t length, const uint8_t status[4]) {
    pci1 *p = ctx;
    if (p->rx_n >= RX_FRAMES || length + 4 > FRAME_MAX) return 0;
    const unsigned slot = (p->rx_head + p->rx_n) % RX_FRAMES;
    memcpy(p->rx[slot], frame, length);
    memcpy(p->rx[slot] + length, status, 4);
    p->rx_len[slot] = (uint16_t)(length + 4);
    p->rx_n++;
    mc_gc_dma_kick(&p->gc, 3);
    return 1;
}
static void enet_transmit(void *ctx, const uint8_t *frame, size_t length) {
    pci1 *p = ctx;
    if (p->net.transmit) p->net.transmit(p->net.ctx, frame, length);
}

/* --- wiring ----------------------------------------------------------------------- */

static void board_line(void *board, void *dev, int line, int state) {
    pci1 *p = board;
    if (dev == &p->gc) {
        if (line == MC_GC_LINE_INT) mc_ppc_set_interrupt(p->m, state);
    } else if (dev == &p->hh || dev == &p->plat) {
        if (line == MC_HAMMERHEAD_LINE_MAP && state) map_ram(p);
        else if (dev == &p->plat && line == MC_PLATINUM_LINE_VBL) mc_gc_set_source(&p->gc, MC_GC_IRQ_DISPLAY, state);
    } else if (dev == &p->via) {
        if (line == MC_VIA_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_VIA, state);
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&p->cuda, (uint8_t)state);
    } else if (dev == &p->cuda) {
        if (line == MC_ADBCTL_LINE_NMI) mc_gc_set_source(&p->gc, MC_GC_IRQ_NMI, state);
        else if (line == MC_ADBCTL_LINE_POWER) {
            if (state) p->powered = 1;
            else if (p->powered) mc_ppc_power_off(p->m);
        } else if (line == MC_ADBCTL_LINE_RESET) {
            /* Cuda's restart: the processor and the board start over at once
             * (as on the PDM boards)†. */
            if (state && !p->in_reset) {
                p->in_reset = 1;
                if (p->m->desc->ops->reset) p->m->desc->ops->reset(p);
                ppc_reset(&p->m->cpu);
                ppc_jit_cpu_reset(p->m->jit);
                cpu_straps(p);
            } else if (!state) {
                p->in_reset = 0;
            }
        }
    } else if (dev == &p->control) {
        if (line == MC_CONTROL_LINE_VBL) mc_gc_set_source(&p->gc, 26, state);
    } else if (dev == &p->card) {
        /* Bandit 1 slot 13 (A1): Open Firmware's interrupt number 23. */
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_gc_set_source(&p->gc, 23, state);
    } else if (dev == &p->mesh) {
        if (line == MC_MESH_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_SCSI1, state);
        else if (line == MC_MESH_LINE_DMA_STATUS) mc_gc_dma_bits(&p->gc, 10, 0xE0, (uint8_t)state);
        else if (line == MC_MESH_LINE_DRQ && state) mc_gc_dma_kick(&p->gc, 10);
    } else if (dev == &p->scsi) {
        if (line == MC_NCR53C96_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_SCSI0, state);
        else if (line == MC_NCR53C96_LINE_DRQ) {
            /* DREQ also on channel 0's status bit s5: the native SIM polls
             * it there before it starts the channel (its globals name
             * the status byte $F300 8004 and bit 5; ROM $FFEB66C0). */
            mc_gc_dma_bits(&p->gc, 0, 0x20, state ? 0x20 : 0);
            if (state) mc_gc_dma_kick(&p->gc, 0);
        }
    } else if (dev == &p->swim3) {
        if (line == MC_SWIM3_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_FLOPPY, state);
        else if (line == MC_SWIM3_LINE_DRQ && state) mc_gc_dma_kick(&p->gc, 1);
    } else if (dev == &p->awacs) {
        if (line == MC_AWACS_LINE_OUT_DRQ && state) mc_gc_dma_kick(&p->gc, 8);
        else if (line == MC_AWACS_LINE_IN_DRQ && state) mc_gc_dma_kick(&p->gc, 9);
    } else if (dev == &p->scc) {
        if (line == MC_SCC_LINE_IRQ) {
            mc_gc_set_source(&p->gc, MC_GC_IRQ_SCCA, state);
        }
    } else if (dev == &p->enet) {
        if (line == MC_AM79C940_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_ENET, state);
        /* Original PCI driver: OUTPUT_LAST waits for s5, then LOAD_QUAD
         * reads XMTFS and IR. The MACE must receive the record before that
         * wait so its completion can release the channel (CHRP I/O 15.7.1). */
        else if (line == MC_AM79C940_LINE_TX_STATUS) mc_gc_dma_bits(&p->gc, 2, 0x20u, state ? 0x20u : 0);
    }
}

/* --- storage ---------------------------------------------------------------------- */

static mc_scsi_bus *disk_bus(pci1 *p) { return &p->scsi_bus[0]; }

static mc_vdisk *make_vdisk(mc_disk *const *images, unsigned n, char *error, size_t capacity) {
    size_t size = 0;
    const uint8_t *driver = mc_guest_livadixdisk(&size);
    const mc_vdisk_drivers drivers = {driver, size, NULL, 0};
    return mc_vdisk_create(images, n, &drivers, error, capacity);
}

static void drop_disks(pci1 *p) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !p->vdisk[id]) continue;
        mc_scsi_bus_attach(disk_bus(p), id, NULL);
        mc_vdisk_destroy(p->vdisk[id]);
        p->vdisk[id] = NULL;
    }
}

static int layout_disks(pci1 *p, char *error, size_t capacity) {
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
        mc_scsi_bus_attach(disk_bus(p), id, mc_scsi_unit_target(&p->unit[id]));
    }
    return 1;
}

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    pci1 *p = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    p->vdisk[CD_ID] = NULL;
    mc_media_eject(&p->m->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    pci1 *p = board;
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

static void startup_cd(pci1 *p) {
    uint8_t *pram = p->nvram.data + MC_NVRAM_PRAM;
    const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
    pram[0x78] = pram[0x79] = 0;
    pram[0x7A] = (uint8_t)(refnum >> 8);
    pram[0x7B] = (uint8_t)refnum;
    p->nvram.dirty = 1;
}

static void board_poll(void *board) {
    pci1 *p = board;
    const uint8_t *startup = p->nvram.data + MC_NVRAM_PRAM + 0x78;
    /* Cold firmware initializes classic PRAM as well as OF's options.
     * An explicit host CD selection must survive those default writes. */
    if (p->m->config.boot_priority == MC_BOOT_CDROM &&
        (startup[0] || startup[1] || startup[2] != 0xFF || startup[3] != (uint8_t)-(33 + CD_ID))) startup_cd(p);
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

/* --- serial, network, sound ------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }
static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    pci1 *p = ctx;
    if (p->serial.transmit) p->serial.transmit(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    pci1 *p = ctx;
    if (p->serial.outputs) p->serial.outputs(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void board_serial_host(void *board, const mc_serial_host *host) {
    pci1 *p = board;
    if (host) p->serial = *host;
    else memset(&p->serial, 0, sizeof p->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    pci1 *p = board;
    const size_t received = port < 2 ? mc_scc_receive(&p->scc, port_channel(port), data, count) : count;
    return received;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    pci1 *p = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&p->scc, port_channel(port), dcd);
    mc_scc_set_cts(&p->scc, port_channel(port), cts);
}
static void board_net_host(void *board, const mc_net_host *host) {
    pci1 *p = board;
    if (host) p->net = *host;
    else memset(&p->net, 0, sizeof p->net);
}
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_am79c940_receive(&((pci1 *)board)->enet, frame, length);
}

static uint32_t board_audio_rate(void *board) {
    pci1 *p = board;
    return p->m->config.sound ? mc_awacs_rate(&p->awacs) : 0;
}
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    pci1 *p = board;
    const size_t n = mc_awacs_pull(&p->awacs, stereo, frames);
    mc_awac_output(&p->awac, stereo, n);
    mc_ppc_audio_startup(p->m, stereo, n, p->awacs.played,
                         !p->awacs.out_n && !p->awacs.ring_n && !(p->gc.ch[8].status & MC_DBDMA_ACTIVE));
    return n;
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    pci1 *p = board;
    p->m->startup_sound_done = 0;
    cpu_straps(p);
    if (p->m->config.boot_priority == MC_BOOT_CDROM) {
        startup_cd(p);
    }
    if (p->board->memc == MEMC_PLATINUM) mc_platinum_reset(&p->plat);
    else mc_hammerhead_reset(&p->hh);
    for (unsigned i = 0; i < p->board->bandits; i++) mc_bandit_reset(&p->bandit[i]);
    mc_gc_reset(&p->gc);
    if (p->board->video == VIDEO_CONTROL) {
        mc_bandit_reset(&p->chaos);
        mc_control_reset(&p->control);
        map_vram(p);
    }
    if (p->card_fitted) {
        mc_livadix_card_ops.reset(&p->card);
        map_card_vram(p);
    }
    mc_radacal_reset(&p->dac);
    mc_awacs_reset(&p->awacs);
    mc_via_ops.reset(&p->via);
    mc_swim3_ops.reset(&p->swim3);
    mc_am79c940_ops.reset(&p->enet);
    mc_ncr53c96_reset(&p->scsi);
    if (p->board->mesh) mc_mesh_reset(&p->mesh);
    mc_scc_ops.reset(&p->scc);
    p->tx_n = 0;
    map_ram(p);
    mc_via_set_pa(&p->via, 0xFFu, 0xFFu);
    mc_via_set_pb(&p->via, 0xF7u, 0xF7u);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    pci1 *p = board;
    if (p->card_fitted) {
        mc_livadix_card_view v;
        if (!mc_livadix_card_view_get(&p->card, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = v.palette;
        fb->serial = ++p->fb_serial;
        return 1;
    }
    if (p->board->video == VIDEO_CONTROL) {
        mc_control_view v;
        if (!mc_control_view_get(&p->control, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = p->dac.palette;
        fb->serial = ++p->fb_serial;
        return 1;
    }
    if (p->board->video == VIDEO_PLATINUM) {
        mc_platinum_view v;
        if (!mc_platinum_view_get(&p->plat, &v)) return 0;
        fb->width = v.width;
        fb->height = v.height;
        fb->depth = v.depth;
        fb->row_bytes = v.row_bytes;
        fb->pixels = v.pixels;
        fb->palette = p->dac.palette;
        fb->serial = ++p->fb_serial;
        return 1;
    }
    return 0;
}

static int board_key(void *board, uint8_t code, int down) {
    pci1 *p = board;
    if (code == 0x7F) mc_adbctl_power_key(&p->cuda, down);
    if (!mc_adb_keyboard_key(&p->keyboard, code, down)) return 0;
    mc_adbctl_poke(&p->cuda);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    pci1 *p = board;
    mc_adb_mouse_move(&p->mouse, dx, dy, button);
    mc_adbctl_poke(&p->cuda);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    pci1 *p = board;
    p->host_start = host_seconds;
    mc_adbctl_set_clock(&p->cuda, host_seconds);
}

/* The PRAM file: Cuda's (adbctl.h: its clock and parameter RAM), then the
 * 8 KB NVRAM, which holds Open Firmware's configuration and the Mac OS's
 * parameter RAM at $1300. */
static void save_nvram(pci1 *p) {
    if (!p->nvram_path[0] || !mc_adbctl_nvram_save(&p->cuda, p->nvram_path, p->host_start)) return;
    FILE *f = fopen(p->nvram_path, "ab");
    if (!f) return;
    fwrite(p->nvram.data, 1, MC_NVRAM_SIZE, f);
    fclose(f);
}

/* A new parameter RAM at NVRAM $1300: the ROM's own defaults, as on the
 * PDM boards (pdm.c, fresh_pram): POST takes the start-up sound's volume
 * from its byte 8 ($FFF03D2C, NVRAM $1308). */
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

static int load_nvram(pci1 *p) {
    if (!p->nvram_path[0] || !mc_adbctl_nvram_load(&p->cuda, p->nvram_path)) return 0;
    FILE *f = fopen(p->nvram_path, "rb");
    if (!f) return 0;
    int ok = fseek(f, 8 + 1 + 8 + 256, SEEK_SET) == 0 && fread(p->nvram.data, 1, MC_NVRAM_SIZE, f) == MC_NVRAM_SIZE;
    fclose(f);
    return ok;
}

static void board_destroy(void *board) {
    pci1 *p = board;
    save_nvram(p);
    for (int id = 0; id < 7; id++) {
        if (!p->vdisk[id]) continue;
        mc_scsi_bus_attach(disk_bus(p), id, NULL);
        mc_vdisk_destroy(p->vdisk[id]);
    }
    for (unsigned i = 0; i < 2; i++) mc_scsi_bus_free(&p->scsi_bus[i]);
    mc_control_free(&p->control);
    mc_platinum_free(&p->plat);
    if (p->card_fitted) mc_livadix_card_free(&p->card);
    free(p->cuda.mcu);
    free(p);
}

static uint32_t board_card_blits(void *board) {
    pci1 *p = board;
    return p->card_fitted ? p->card.blt.count : 0;
}

static const mc_board_ops pci1_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive, NULL, board_card_blits,
};

/* The monitor the configuration's screen size names, by its sense lines
 * (control.h; Technical Note HW 30's codes, as pdm_video.c): the Apple 21"
 * color display (sense code 0) at 1152 x 870, the 16" (extended $2D) at 832
 * x 624, else the 13" Hi-Res (code 6, which the extended probe reads as $2B:
 * Open Firmware's table at file $3468BB). Sense 7 / extended $3F: no monitor
 * (the built-in video when the Livadix Display Card owns the screen). */
static void monitor_sense(const mc_config *c, uint8_t *sense, uint8_t *ext, int none) {
    if (none) {
        *sense = 7, *ext = 0x3F;
        return;
    }
    if (c->width == 1152 && c->height == 870) *sense = 0, *ext = 0;
    else if (c->width == 832 && c->height == 624) *sense = 7, *ext = 0x2D;
    else *sense = 6, *ext = 0x2B;
}

/* Control's and Platinum's modes with 4 MB of VRAM (see video_4mb below). */
static unsigned builtin_depths(unsigned w, unsigned h) {
    if ((w == 640 && h == 480) || (w == 832 && h == 624)) return MC_DEPTHS_UPTO(32);
    if (w == 1152 && h == 870) return MC_DEPTHS_UPTO(16);
    return 0;
}

static int want_card(const pci1 *p) {
    const mc_config *c = &p->m->config;
    if (c->video_card == MC_VIDEO_CARD_LIVADIX || p->board->video == VIDEO_NONE) return 1;
    if (c->video_card == MC_VIDEO_CARD_BUILTIN) return 0;
    const unsigned depths = builtin_depths(c->width, c->height);
    return !depths || !(depths & MC_DEPTH_BIT(c->depth));
}

static int fit_card(pci1 *p) {
    mc_ppc *m = p->m;
    size_t rom_size = 0;
    const uint8_t *rom = mc_guest_livadixcard_pci(&rom_size);
    const int mode = mc_livadix_card_mode(m->config.width, m->config.height);
    if (mode < 0 || !rom || !rom_size ||
        !mc_livadix_card_init(&p->card, &m->host, "Livadix Display Card", MC_LIVADIX_CARD_VRAM, (unsigned)mode, rom, rom_size,
                           m->config.video_acceleration))
        return 0;
    mc_livadix_card_set_depth(&p->card, m->config.depth);
    const mc_pci_function fn = {p, card_config_read, card_config_write, card_mem_read, card_mem_write};
    if (!mc_bandit_attach(&p->bandit[0], MC_LIVADIX_PCI_SLOT, &fn)) {
        mc_livadix_card_free(&p->card);
        return 0;
    }
    p->card_fitted = 1;
    return 1;
}

/* --- building --------------------------------------------------------------------- */

static void *fail(pci1 *p, const char *message) {
    snprintf(p->m->error, sizeof p->m->error, "%s", message);
    board_destroy(p);
    return NULL;
}

static void *build(mc_ppc *m, const pci1_board *b) {
    pci1 *p = calloc(1, sizeof *p);
    if (!p) {
        snprintf(m->error, sizeof m->error, "Out of memory");
        return NULL;
    }
    p->m = m;
    p->board = b;
    m->board = p;
    const mc_hw_host *host = &m->host;
    const mc_ppc_port direct = {MC_PPC_PORT_DIRECT, 0, 0, 0};
    uint32_t banks[MC_HAMMERHEAD_BANKS];
    const unsigned nbanks = b->memc == MEMC_PLATINUM ? MC_PLATINUM_BANKS : MC_HAMMERHEAD_BANKS;
    if (!ram_banks(m->ram_size, banks, nbanks)) {
        snprintf(m->error, sizeof m->error, "emulation.ram_mb: %u MiB does not fit the board's banks", (unsigned)(m->ram_size / MB));
        free(p);
        return NULL;
    }
    const int card = want_card(p);
    /* The memory controller. */
    const int sysctl = mc_ppc_region(m, "sysctl");
    if (b->memc == MEMC_PLATINUM) {
        /* The bus clock in the identification's low half (POST's tables at
         * $FFF03AD8 and $FFF03B1C): the 7200/90's 45 MHz (bit 15, code 11),
         * else 50 MHz (code 3, the 7200/75's). */
        const uint16_t id_low = m->rate == 90000000ull * m->div ? 0x8B00u : (uint16_t)b->id;
        if (!mc_platinum_init(&p->plat, host, "Platinum", id_low, b->l2, banks, 4u * MB)) return fail(p, "Out of memory");
        uint8_t sense, ext;
        monitor_sense(&m->config, &sense, &ext, card);
        mc_platinum_set_monitor(&p->plat, sense, ext);
        mc_ppc_attach(m, sysctl, 0, MC_PLATINUM_SIZE, &p->plat, &mc_platinum_ops, direct);
        mc_bus_map_memory(&m->bus, 0xF1000000u, MC_PLATINUM_VRAM_WINDOW, p->plat.vram, p->plat.vram_size, 1);
    } else {
        mc_hammerhead_init(&p->hh, host, "Hammerhead", b->id, b->l2, banks);
        mc_hammerhead_set_straps(&p->hh, b->straps, 3);     /* the 50 MHz bus */
        mc_ppc_attach(m, sysctl, 0, MC_HAMMERHEAD_SIZE, &p->hh, &mc_hammerhead_ops, direct);
        /* The region stays based at zero: callbacks receive physical RAM
         * addresses, while remap selects only separate-bank windows. */
        p->hh_ram_region = mc_bus_add_io(&m->bus, 0, 0, 0x7FFFFFFFu, 0);
        if (!p->hh_ram_region || !mc_bus_attach(&m->bus, p->hh_ram_region, 0, 0x80000000u, p, &hh_ram_ops, 0))
            return fail(p, "Cannot map Hammerhead DIMM views");
    }
    map_ram(p);
    /* The bridges. */
    static const char *const bandit_region[2] = {"bandit", "bandit2"};
    for (unsigned i = 0; i < b->bandits; i++) {
        mc_bandit_init(&p->bandit[i], host, i ? "Bandit 2" : "Bandit", MC_BANDIT_BANDIT, 0xF2000000u + 0x02000000u * i);
        mc_ppc_attach(m, mc_ppc_region(m, bandit_region[i]), 0, MC_BANDIT_SPACE, &p->bandit[i], &mc_bandit_ops, direct);
    }
    mc_ppc_attach(m, mc_ppc_region(m, "pci"), 0, 0x70000000u, p, &pcimem_ops, direct);
    mc_ppc_attach(m, mc_ppc_region(m, "pci-high"), 0, 0x06000000u, p, &pcimem_ops, direct);
    if (b->video == VIDEO_CONTROL) {
        mc_bandit_init(&p->chaos, host, "Chaos", MC_BANDIT_CHAOS, 0xF0000000u);
        mc_ppc_attach(m, mc_ppc_region(m, "chaos"), 0, MC_BANDIT_SPACE, &p->chaos, &mc_bandit_ops, direct);
        if (!mc_control_init(&p->control, host, "Control", b->vram_mb * MB)) return fail(p, "Out of memory");
        uint8_t sense, ext;
        monitor_sense(&m->config, &sense, &ext, card);
        mc_control_set_monitor(&p->control, sense, ext);
        const mc_pci_function cf = {p, control_config_read, control_config_write, control_mem_read, control_mem_write};
        mc_bandit_attach(&p->chaos, MC_CONTROL_DEVICE, &cf);
        mc_planb_init(&p->planb, host, "Plan B");
        const mc_pci_function pf = mc_planb_function(&p->planb);
        mc_bandit_attach(&p->chaos, MC_PLANB_DEVICE, &pf);
    }
    if (card && !fit_card(p)) {
        char msg[128];
        snprintf(msg, sizeof msg, "The Livadix Display Card cannot show %ux%u", (unsigned)m->config.width,
                 (unsigned)m->config.height);
        return fail(p, msg);
    }
    /* Grand Central and its devices. */
    mc_gc_init(&p->gc, host, "Grand Central");
    const mc_pci_function gf = mc_gc_function(&p->gc);
    mc_bandit_attach(&p->bandit[0], MC_GC_DEVICE, &gf);
    mc_gc_attach(&p->gc, 0x1A000u, 0x10u, 0, p, &board_id_ops);
    if (b == &board_8500)
        mc_gc_attach(&p->gc, 0x1E000u, 4u, 0, p, &av_sense_ops);
    mc_via_init(&p->via, host, "VIA", 9);
    mc_gc_attach(&p->gc, MC_GC_VIA, 0x2000u, 0, p, &via_ops);
    mc_scc_init(&p->scc, host);
    mc_scc_set_clocks(&p->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    {
        const mc_scc_port sport = {p, scc_transmit, scc_outputs};
        mc_scc_set_port(&p->scc, &sport);
    }
    mc_gc_attach(&p->gc, MC_GC_SCC, 0x1000u, 0, &p->scc, &scc_ops);
    /* The legacy SCC window at + $12000 in the Macintosh's order (the 68k
     * ROM's serial code: B control + 0, A control + 2, B data + 4, A data
     * + 6; ROM $FFF11A10). */
    mc_gc_attach(&p->gc, 0x12000u, 0x1000u, 0, &p->scc, &mc_scc_ops);
    /* SCSI: MESH on the internal bus, the 53C94 on the external one (on
     * the 7200 the 53C94's bus is the only one). */
    mc_scsi_bus_init(&p->scsi_bus[0]);
    mc_scsi_bus_init(&p->scsi_bus[1]);
    if (b->mesh) {
        /* The MESH ID register reads $E2: the native SCSI SIM takes the chip
         * at + $18000 for a MESH when it reads $E1 or above there, else for
         * a 53C94 (ROM $FFEBBBEC), and version 2 from $E2. */
        mc_mesh_init(&p->mesh, host, "MESH", &p->scsi_bus[0], 0xE2u);
        mc_gc_attach(&p->gc, MC_GC_MESH, 0x100u, 4, &p->mesh, &mc_mesh_ops);
        const mc_dbdma_port mp = {&p->mesh, mc_mesh_dma_read, mc_mesh_dma_write, NULL};
        mc_gc_dma_port(&p->gc, 10, &mp);
    }
    mc_ncr53c96_init(&p->scsi, host, &p->scsi_bus[b->mesh ? 1 : 0], 25000000u);
    mc_gc_attach(&p->gc, MC_GC_53C94, 0x100u, 4, &p->scsi, &mc_ncr53c96_ops);
    {
        const mc_dbdma_port sp = {&p->scsi, scsi_dma_read, scsi_dma_write, NULL};
        mc_gc_dma_port(&p->gc, 0, &sp);
    }
    mc_scsi_cd_init(&p->unit[CD_ID]);
    mc_scsi_unit_on_eject(&p->unit[CD_ID], cd_ejected, p);
    mc_scsi_bus_attach(disk_bus(p), CD_ID, mc_scsi_unit_target(&p->unit[CD_ID]));
    /* The floppy. */
    mc_swim3_init(&p->swim3, host, "SWIM III", 1);
    mc_swim3_set_stride(&p->swim3, 4);
    mc_gc_attach(&p->gc, MC_GC_SWIM3, 0x100u, 0, &p->swim3, &mc_swim3_ops);
    {
        const mc_dbdma_port fp = {&p->swim3, swim3_dma_read, swim3_dma_write, NULL};
        mc_gc_dma_port(&p->gc, 1, &fp);
    }
    /* Sound. */
    mc_awac_init(&p->awac, host, "AWAC");
    /* Mac OS 9.0.4 System, nlib -20537 (GCAwacsPlugin),
     * __SndHWGetType +$1E3C-$1E98: codec status bits 15-12 identify
     * AWACS as 2 (3 is Screamer). HardwareInitialize +$4604-$4658
     * rejects 0 with noHardwareErr. The first PCI boards have AWACS
     * (7500/8500 Developer Note, chapter 2, "AWAC Sound IC"). */
    mc_awac_set_status(&p->awac, 1, 0x20u);
    {
        const mc_awacs_codec codec = {&p->awac, codec_control, codec_status};
        mc_awacs_init(&p->awacs, host, "AWACS", &codec);
    }
    mc_gc_attach(&p->gc, MC_GC_AWACS, MC_AWACS_SIZE, 0, &p->awacs, &mc_awacs_ops);
    {
        const mc_dbdma_port out = {&p->awacs, NULL, mc_awacs_dma_write, NULL};
        const mc_dbdma_port in = {&p->awacs, mc_awacs_dma_read, NULL, NULL};
        mc_gc_dma_port(&p->gc, 8, &out);
        mc_gc_dma_port(&p->gc, 9, &in);
    }
    /* Ethernet. */
    mc_am79c940_init(&p->enet, host, "MACE");
    {
        const mc_am79c940_port e = {p, enet_transmit, enet_dma_receive};
        mc_am79c940_set_port(&p->enet, &e);
        const mc_dbdma_port tx = {p, NULL, enet_tx_write, NULL, enet_tx_end};
        const mc_dbdma_port rx = {p, enet_rx_read, NULL, NULL};
        mc_gc_dma_port(&p->gc, 2, &tx);
        mc_gc_dma_port(&p->gc, 3, &rx);
    }
    mc_gc_attach(&p->gc, MC_GC_MACE, MC_AM79C940_SIZE, 0, &p->enet, &mc_am79c940_ops);
    mc_sonic_prom_init(&p->enet_prom, host, mc_sonic_default_address, 4, 1);
    /* Curio's address PROM marker: the original PCI MACE driver reads
     * raw byte 6 and recognises $AA (9.0.4, $0BB5B6EC-$0BB5B724) before
     * validating local-mac-address. Keep the eight-byte XOR check byte. */
    p->enet_prom.bytes[6] = 0xAA;
    p->enet_prom.bytes[7] ^= 0xAA;
    mc_gc_attach(&p->gc, MC_GC_ENET_ROM, 0x1000u, 0, &p->enet_prom, &mc_sonic_prom_ops);
    /* The DAC and the NVRAM. */
    mc_radacal_init(&p->dac, host, "DAC", b->video == VIDEO_PLATINUM ? MC_DAC_DACULA : MC_DAC_RADACAL);
    mc_gc_attach(&p->gc, MC_GC_DAC, 0x1000u, 4, &p->dac, &mc_radacal_ops);
    mc_nvram_init(&p->nvram, host, "NVRAM", NULL);
    mc_gc_attach(&p->gc, MC_NVRAM_PAGE_OFFSET, MC_NVRAM_WINDOW, 0, &p->nvram, &mc_nvram_page_ops);
    mc_gc_attach(&p->gc, MC_NVRAM_DATA_OFFSET, MC_NVRAM_WINDOW, 0, &p->nvram, &mc_nvram_data_ops);
    /* ADB, the clock and power: Cuda. */
    mc_adb_bus_init(&p->adb);
    mc_adb_keyboard_init(&p->keyboard);
    mc_adb_mouse_init(&p->mouse);
    mc_adb_attach(&p->adb, &p->keyboard.dev);
    mc_adb_attach(&p->adb, &p->mouse.dev);
    mc_adbctl_init(&p->cuda, host, "Cuda", MC_ADBCTL_CUDA, &p->via, &p->adb);
    mc_adbctl_set_part(&p->cuda, "341S0788");
    {
        static const char *const keys[2] = {"cuda-341s0788", NULL};
        if (mc_adbctl_select(&p->cuda, (mc_adbctl_level)m->config.controller, m->config.roms, keys) < 0)
            return fail(p, "emulation.controller: firmware: the Cuda firmware (cuda-341s0788) is not in the roms folders");
    }
    if (!mc_config_nvram_path(&m->config, p->nvram_path, sizeof p->nvram_path)) p->nvram_path[0] = 0;
    if (!load_nvram(p)) {
        mc_adbctl_set_seconds(&p->cuda, 0);
        fresh_pram(p->nvram.data + MC_NVRAM_PRAM);
    }
    if (m->config.boot_priority == MC_BOOT_CDROM) {
        startup_cd(p);
    }
    mc_adbctl_ops.reset(&p->cuda);
    return p;
}

static void *build_7200(mc_ppc *m) { return build(m, &board_7200); }
static void *build_7500(mc_ppc *m) { return build(m, &board_7500); }
static void *build_8500(mc_ppc *m) { return build(m, &board_8500); }
static void *build_9500(mc_ppc *m) { return build(m, &board_9500); }
static void *build_7300(mc_ppc *m) { return build(m, &board_7300); }

/* --- the descriptions ------------------------------------------------------------- */

static const uint32_t pci1_roms[] = {0x9630C68Bu, 0x96CD923Du, 0x960E4BE9u, 0x960FC647u, 0};
static const uint16_t ram_7200[] = {8, 16, 24, 32, 40, 48, 64, 96, 128, 192, 256, 512, 0};
static const uint16_t ram_hh[] = {16, 24, 32, 40, 48, 64, 96, 128, 136, 192, 256, 512, 1024, 0};

#define MAP_COMMON                                                                         \
    {"ram", 0x00000000u, 0x80000000u, 0, MC_REGION_RAM},                                   \
    {"pci", 0x80000000u, 0x70000000u, 0x7FFFFFFFu, MC_REGION_IO},                          \
    {"bandit", 0xF2000000u, 0x02000000u, 0x01FFFFFFu, MC_REGION_IO},                       \
    {"sysctl", 0xF8000000u, 0x01000000u, 0x00FFFFFFu, MC_REGION_IO},                       \
    {"pci-high", 0xF9000000u, 0x06000000u, 0x07FFFFFFu, MC_REGION_IO},                     \
    {"rom", 0xFF000000u, 0x01000000u, 0, MC_REGION_ROM}

static const mc_region map_7200[] = {MAP_COMMON};
static const mc_region map_chaos[] = {MAP_COMMON, {"chaos", 0xF0000000u, 0x02000000u, 0x01FFFFFFu, MC_REGION_IO}};
static const mc_region map_two[] = {MAP_COMMON, {"bandit2", 0xF4000000u, 0x02000000u, 0x01FFFFFFu, MC_REGION_IO}};

static const char *const models_7200[] = {"power-macintosh-7200", "power-macintosh-7200-v1", NULL};
static const char *const models_7500[] = {"power-macintosh-7500", NULL};
static const char *const models_7600[] = {"power-macintosh-7600", NULL};
static const char *const models_8500[] = {"power-macintosh-8500", NULL};
static const char *const models_8600[] = {"power-macintosh-8600", "power-macintosh-8600-300", NULL};
static const char *const models_9500[] = {"power-macintosh-9500", NULL};
static const char *const models_9600[] = {"power-macintosh-9600", "power-macintosh-9600-350", NULL};
static const char *const models_7300[] = {"power-macintosh-7300", NULL};

/* Control's and Platinum's modes with 4 MB of VRAM on the monitors the
 * board connects (monitor_sense): the 13" at 640 x 480 and the 16" at 832 x
 * 624 to 32 bits, the 21" at 1152 x 870 to 16 (the drivers' choices traced). */
static unsigned video_4mb(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return builtin_depths(w, h);
}
/* The Livadix Display Card fits every PCI board (Bandit 1 slot 13); boards
 * without built-in video (9500, 9600) take it for every mode. */
static const mc_hw_caps caps_video = {video_4mb, 1, 0, NULL};
static const mc_hw_caps caps_card = {NULL, 1, 0, NULL};

#define PCI1_DESC(id, mname, model, hz, tb, cpi_, cpid, rams, mp, b, cp)                                                     const mc_ppc_desc mc_pci##id##_desc = {                                                                                      .key = "pci" #id, .models = models_##id, .name = mname, .cpu = model, .cpu_hz = hz, .timebase_hz = tb,                   .cpi = cpi_, .cpi_div = cpid, .roms = pci1_roms, .ram_mb = rams, .map = mp, .nmap = sizeof mp / sizeof *mp,              .unmapped = MC_PPC_UNMAPPED_TEA, .build = b, .ops = &pci1_ops, .caps = cp,                                           }

/* The 601's RTC input: C15M / 2, as on the PDM boards (pdm.c): Open
 * Firmware gives the processor node a timebase-frequency of 7,833,600 Hz
 * ($778800). The PDM boards' flat 8/9 clock per instruction gives the
 * nanokernel the configured 601 clock. The 604's time base counts a
 * quarter of the bus clock, 50 MHz. */
#define RTC_601 7833600u
#define TB_604 12500000u
PCI1_DESC(7200, "Power Macintosh 7200", PPC_MODEL_601, 90000000u, RTC_601, 8, 9, ram_7200, map_7200, build_7200, &caps_video);
PCI1_DESC(7500, "Power Macintosh 7500", PPC_MODEL_601, 100000000u, RTC_601, 8, 9, ram_hh, map_chaos, build_7500, &caps_video);
PCI1_DESC(7600, "Power Macintosh 7600", PPC_MODEL_604, 132000000u, 11000000u, 8, 9, ram_hh, map_chaos, build_7500, &caps_video);
PCI1_DESC(8500, "Power Macintosh 8500", PPC_MODEL_604, 120000000u, 10000000u, 8, 9, ram_hh, map_chaos, build_8500, &caps_video);
PCI1_DESC(8600, "Power Macintosh 8600", PPC_MODEL_604E, 200000000u, TB_604, 8, 9, ram_hh, map_chaos, build_8500, &caps_video);
PCI1_DESC(9500, "Power Macintosh 9500", PPC_MODEL_604, 120000000u, 10000000u, 8, 9, ram_hh, map_two, build_9500, &caps_card);
PCI1_DESC(9600, "Power Macintosh 9600", PPC_MODEL_604E, 200000000u, TB_604, 8, 9, ram_hh, map_two, build_9500, &caps_card);
PCI1_DESC(7300, "Power Macintosh 7300", PPC_MODEL_604E, 168000000u, 12000000u, 8, 9, ram_hh, map_chaos, build_7300, &caps_video);
