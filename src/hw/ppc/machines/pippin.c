/* Livadix - Pippin Aspen board bring-up. Apple TIL 18537 and the original
 * KINKA 1.0 (2BEF21B7/2BF65931) POST, NanoKernel and FCode.
 * The RAM sizing decode and temporary service window are provisional. */
#include "../ppc_machine.h"
#include "../chips/grand_central.h"
#include "../chips/mesh.h"
#include "../chips/awac.h"
#include "../chips/awacs.h"
#include "../../adbctl.h"
#include "../../via.h"
#include "../../scc.h"
#include "../../am29f010.h"
#include "../../scsi_target.h"
#include "../../storage/vdisk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct pippin_board;
/* Taos' frame timer: a device of its own for the machine's event queue. */
typedef struct taos_timer { mc_dev dev; struct pippin_board *board; } taos_timer;

typedef struct pippin_board {
    mc_ppc *machine;
    taos_timer taos;
    uint64_t frame_start;        /* C15M time of the last vertical blanking's start */
    uint8_t vbl_pending, vbl_irq;
    mc_gc gc;
    mc_pci_function gc_pci;
    mc_via via;
    mc_adb_bus adb;
    mc_adbctl cuda;
    mc_mesh mesh;
    mc_awac codec;
    mc_awacs sound;
    mc_scc scc;
    mc_serial_host serial;
    mc_scsi_bus scsi;
    uint32_t address;
    int powered;
    uint8_t control[0x1000];
    uint8_t early[0x1000];
    /* The frame buffer, and above it the ROM's work area: its pointers at
     * $F00DFFF0-$F00DFFFF and, from $F00E0000 to $F0100000, its copy of
     * the flash memory (KINKA retail $FFCCCD1E-$FFCCCD60). */
    uint8_t vram[0x200000];
    mc_am29f010 flash;
    uint32_t palette_raw[256], palette[256];
    uint64_t fb_serial;
    mc_scsi_unit cd;
    mc_vdisk *cd_image;
    mc_adb_keyboard keyboard;
    mc_adb_applejack applejack;
    int64_t host_start;
    int clock_set;
    char nvram_path[MC_CONFIG_PATH];
} pippin_board;

static uint32_t asp_read(void *dev, uint32_t offset, unsigned size) {
    pippin_board *p = dev;
    if (offset == 0x800000 && size == 4) return p->address;
    if (offset >= 0xC00000 && offset < 0xC00008 && (p->address & ~0xFCu) == 0x2800) {
        const unsigned reg = (p->address & 0xFCu) + (offset & 3u);
        return mc_pci_swap(p->gc_pci.config_read(&p->gc, reg, size), size);
    }
    uint32_t value;
    if (p->gc_pci.mem_read(&p->gc, 0xF2000000u + offset, size, &value)) return mc_pci_swap(value, size);
    return size == 4 ? UINT32_MAX : (1u << (8u * size)) - 1u;
}

static void asp_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    if (offset == 0x800000 && size == 4) p->address = value;
    else if (offset >= 0xC00000 && offset < 0xC00008 && (p->address & ~0xFCu) == 0x2800) {
        const unsigned reg = (p->address & 0xFCu) + (offset & 3u);
        p->gc_pci.config_write(&p->gc, reg, size, mc_pci_swap(value, size));
    } else p->gc_pci.mem_write(&p->gc, 0xF2000000u + offset, size, mc_pci_swap(value, size));
}

/* Taos' vertical blanking interrupt, as the original display driver uses
 * it (KINKA retail, the driver's code fragment at file $24B000-$24E000):
 *  - the Open Firmware node "taos" (file $344B26) gives interrupt $1C,
 *    Grand Central's source 28;
 *  - ClearInternalVBLInterrupts (file $24D97C) reads +$78 and, when its
 *    bit $00200000 is set, writes that bit to +$80: +$78 is the pending
 *    status, +$80 clears what is written as 1;
 *  - the palette routine (file $24C30C) does this only while +$20 has
 *    $04000000 (the timing generator runs), waits for +$68's $02000000 to
 *    read 0, clears the interrupt again and then loads the colors: that
 *    bit is taken as 1 while the active lines are drawn.
 * The frame is 640 x 480 at 60 Hz; 525 lines with 45 of blanking is the
 * VGA timing, not read from the registers. */
enum { TAOS_VBL = 0x00200000u, TAOS_TIMING = 0x04000000u, TAOS_ACTIVE = 0x02000000u };
#define TAOS_FRAME (MC_C15M_HZ / 60u)
#define TAOS_BLANKING (TAOS_FRAME * 45u / 525u)

static uint32_t early_word(const pippin_board *p, uint32_t offset) {
    return (uint32_t)p->early[offset] << 24 | (uint32_t)p->early[offset + 1] << 16 |
           (uint32_t)p->early[offset + 2] << 8 | p->early[offset + 3];
}
static void taos_update(pippin_board *p) {
    const uint8_t irq = p->vbl_pending && (early_word(p, 0x20) & TAOS_TIMING);
    if (irq == p->vbl_irq) return;
    p->vbl_irq = irq;
    mc_gc_set_source(&p->gc, 28, irq);
}
static void taos_event(void *dev, uint64_t now) {
    pippin_board *p = ((taos_timer *)dev)->board;
    while (p->frame_start + TAOS_FRAME <= now) p->frame_start += TAOS_FRAME;
    p->vbl_pending = 1;
    taos_update(p);
    mc_dev_schedule(&p->taos.dev, p->frame_start + TAOS_FRAME);
}
static const mc_dev_ops taos_timer_ops = {NULL, NULL, taos_event, NULL, NULL};

/* Taos' registers at $F0800000 (KINKA's first writes at $FFF030EC-$FFF03110). */
static uint32_t early_read(void *dev, uint32_t offset, unsigned size) {
    pippin_board *p = dev;
    uint32_t value = 0;
    if (offset + size > sizeof p->early) return 0;
    for (unsigned i = 0; i < size; i++) {
        uint8_t byte = p->early[offset + i];
        if (offset + i == 0x79 && p->vbl_pending) byte |= TAOS_VBL >> 16;
        if (offset + i == 0x68 && (early_word(p, 0x20) & TAOS_TIMING) &&
            mc_dev_now(&p->taos.dev) - p->frame_start >= TAOS_BLANKING) byte |= TAOS_ACTIVE >> 24;
        /* KINKA retail $FFC14B30-$FFC14B3E samples this active-low
         * CD mechanism input. Low invokes .AppleCD control 11 followed
         * by Cuda restart ($FFC14CB0-$FFC14D8A). The host drive is closed;
         * this is an input pin, not a writable register latch. */
        if (offset + i == 0x68) byte |= 0x08;
        value = (value << 8) | byte;
    }
    return value;
}

static void early_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    if (offset >= sizeof p->early || size > sizeof p->early - offset) return;
    for (unsigned i = 0; i < size; i++) {
        const uint8_t byte = (uint8_t)(value >> (8u * (size - i - 1u)));
        if (offset + i == 0x81 && (byte & (TAOS_VBL >> 16))) p->vbl_pending = 0;
        p->early[offset + i] = byte;
    }
    taos_update(p);
}
static const mc_dev_ops early_ops = {early_read, early_write, NULL, NULL, NULL};

/* Original Taos DoDriverIO's palette loop stores RGB0 longwords at
 * $F1000000 + 4*index (KINKA retail $FFE4C534-$FFE4C558). */
static uint32_t palette_read(void *dev, uint32_t offset, unsigned size) {
    const pippin_board *p = dev;
    uint32_t value = 0;
    if (offset >= 1024 || size > 1024 - offset) return 0;
    for (unsigned i = 0; i < size; i++) {
        unsigned at = offset + i;
        value = (value << 8) | ((p->palette_raw[at / 4] >> (24 - 8 * (at & 3))) & 255u);
    }
    return value;
}

static void palette_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    if (offset >= 1024 || size > 1024 - offset) return;
    for (unsigned i = 0; i < size; i++) {
        unsigned at = offset + i, shift = 24 - 8 * (at & 3);
        uint32_t byte = (value >> (8 * (size - i - 1))) & 255u;
        p->palette_raw[at / 4] = (p->palette_raw[at / 4] & ~(255u << shift)) | (byte << shift);
        p->palette[at / 4] = p->palette_raw[at / 4] >> 8;
    }
}
static const mc_dev_ops palette_ops = {palette_read, palette_write, NULL, NULL, NULL};

static uint32_t regs_read(void *dev, uint32_t offset, unsigned size) {
    const pippin_board *p = dev;
    /* Both KINKA ROMs identify the console by this signature. */
    if (offset == 0x10 && size == 4) return 0x40010000u;
    uint32_t value = 0;
    if (offset + size > sizeof p->control) return 0;
    for (unsigned i = 0; i < size; i++) value = (value << 8) | p->control[offset + i];
    return value;
}

/* KINKA $FFF03EB0-$FFF04080 sizes four banks in 16 MiB windows.
 * The 5 MiB of system RAM precede the two expansion banks. The 2 MiB
 * setting uses two 1 MiB banks, which the ROM can size individually.
 * During POST, the $C01000/$C00000, $400000 and $000000 probes distinguish
 * 16, 8, 4 and 1 MiB banks. Normal accesses see only populated RAM. */
static void map_ram(pippin_board *p, int sizing) {
    const uint32_t expansion = p->machine->ram_size - 0x500000u;
    const uint32_t sizes[] = {0x400000u, 0x100000u,
        expansion == 0x200000u ? 0x100000u : expansion,
        expansion == 0x200000u ? 0x100000u : 0};
    uint32_t host_offset = 0;
    mc_bus *bus = &p->machine->bus;
    for (unsigned bank = 0; bank < 4; bank++) {
        const uint32_t base = bank * 0x1000000u;
        const uint32_t size = sizes[bank];
        uint8_t *host = p->machine->ram + host_offset;
        if (sizing && size) {
            for (uint32_t offset = 0; offset < 0x1000000u; offset += 0x1000u) {
                uint32_t page = offset & (size - 1u);
                if (size != 0x1000000u) page &= ~0x1000u;
                if (size == 0x400000u) page |= (offset >> 10) & 0x1000u;
                if (size == 0x800000u) page |= (offset >> 11) & 0x1000u;
                mc_bus_map_memory(bus, base + offset, 0x1000u, host + page, 0x1000u, 1);
            }
        } else {
            /* KINKA $FFF100D8 probes MemTop after installing the OF client
             * vector at $00200000. Only POST's sizing decode may alias:
             * mirroring the $00600000 hole here destroys that live vector. */
            if (size) mc_bus_map_memory(bus, base, size, host, size, 1);
            if (size < 0x1000000u) mc_bus_map_float(bus, base + size, 0x1000000u - size);
        }
        host_offset += size;
    }
}

static void regs_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    if (offset + size > sizeof p->control) return;
    for (unsigned i = 0; i < size; i++) p->control[offset + i] = (uint8_t)(value >> (8u * (size - i - 1u)));
    if (offset == 0x18 && size == 4) map_ram(p, value == 0xAA000000u);
}

/* Grand Central's VIA cell is synchronous: one 783.36 kHz E cycle per
 * access (via.h, Guide chapter 4), as on the PCI Power Macintoshes. The
 * System's Microseconds ($88044-$88064 in RAM on the Pippin's System
 * 7.5.2) reads T2's high byte, the interrupt flags and then, with a high
 * byte not 0, the low byte, which clears T2's flag: accesses closer than
 * an E cycle let it read a counter past 0 before the flag and then lose
 * the Time Manager's interrupt. */
static uint32_t via_read(void *dev, uint32_t offset, unsigned size) {
    pippin_board *p = dev;
    p->machine->bus.cost_due += MC_VIA_ECLOCK;
    mc_adbctl_sync(&p->cuda);
    return mc_via_ops.read(&p->via, offset, size);
}

static void via_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    p->machine->bus.cost_due += MC_VIA_ECLOCK;
    mc_adbctl_sync(&p->cuda);
    mc_via_ops.write(&p->via, offset, size, value);
}

/* Grand Central's SCC cell (CHRP I/O Device Reference, section 9): the
 * four ports every $10 at + $13000 (B control, B data, A control, A data)
 * and the Macintosh's order at + $12000, which the 68k serial and
 * LocalTalk drivers of the Pippin's System use through SCCRd/SCCWr. */
static uint32_t scc_offset(uint32_t offset) {
    static const uint8_t order[4] = {0, 4, 2, 6};
    return order[(offset >> 4) & 3u];
}
static uint32_t scc_read(void *dev, uint32_t offset, unsigned size) { return mc_scc_ops.read(dev, scc_offset(offset), size); }
static void scc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) { mc_scc_ops.write(dev, scc_offset(offset), size, value); }
static const mc_dev_ops scc_ops = {scc_read, scc_write, NULL, NULL, NULL};

/* The 128 KiB flash memory at $40000000, one byte every four addresses
 * (KINKA retail $FFCCCCC0-$FFCCD456: the command addresses $5555 and $2AAA
 * are written at $40015554 and $4000AAA8, the identification is read at
 * $40000000 and $40000004 and must be 01 20, an Am29F010, with eight
 * sectors of $4000). Its first 8 KiB are the Macintosh's NVRAM (the
 * parameter RAM at $1300, checked by "NuMc" at $130C), the other 120 KiB
 * the storage of the titles (Apple Pippin Standard File note). The ROM
 * copies it to $F00E0000 at start-up and writes back the sectors that
 * differ. */
static uint32_t flash_read(void *dev, uint32_t offset, unsigned size) {
    const pippin_board *p = dev;
    uint32_t value = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t at = offset + i;
        value = value << 8 | ((at & 3u) ? 0xFFu : mc_am29f010_read(&p->flash, at >> 2));
    }
    return value;
}
static void flash_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    pippin_board *p = dev;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t at = offset + i;
        if (!(at & 3u)) mc_am29f010_write(&p->flash, at >> 2, (uint8_t)(value >> (8u * (size - i - 1u))));
    }
}
static const mc_dev_ops flash_ops = {flash_read, flash_write, NULL, NULL, NULL};

static int asp_ops_stable(void *dev, uint32_t offset, unsigned size) {
    pippin_board *p = dev; uint32_t off;
    return mc_pci_config_hit(&p->gc.cfg, 0, 0xF2000000u + offset, &off) &&
           mc_gc_ops.stable(&p->gc, off, size);
}
static const mc_dev_ops asp_ops = {asp_read, asp_write, NULL, NULL, asp_ops_stable};
static const mc_dev_ops regs_ops = {regs_read, regs_write, NULL, NULL, NULL};
static const mc_dev_ops via_ops = {via_read, via_write, NULL, NULL, NULL};

static void board_reset(void *dev) {
    pippin_board *p = dev;
    p->machine->startup_sound_done = 0;
    p->address = 0;
    memset(p->early, 0, sizeof p->early);
    memset(p->control, 0, sizeof p->control);
    p->vbl_pending = 0;
    taos_update(p);
    p->frame_start = mc_dev_now(&p->taos.dev);
    mc_dev_schedule(&p->taos.dev, p->frame_start + TAOS_FRAME);
    map_ram(p, 0);
    mc_gc_reset(&p->gc);
    mc_via_ops.reset(&p->via);
    mc_via_set_pa(&p->via, 0xFF, 0xFF);
    mc_via_set_pb(&p->via, 0xF7, 0xF7);
    mc_mesh_reset(&p->mesh);
    mc_awacs_reset(&p->sound);
    mc_scc_ops.reset(&p->scc);
    mc_am29f010_reset(&p->flash);
}

static void line(void *dev, void *chip, int signal, int state) {
    pippin_board *p = dev;
    if (chip == &p->gc && signal == MC_GC_LINE_INT) mc_ppc_set_interrupt(p->machine, state);
    if (chip == &p->cuda) {
        if (signal == MC_ADBCTL_LINE_RESET) {
            /* Retail ROM $FFC0ABD0-$FFC0AD2A sends restart ($01 $11)
             * and waits for the board to reset. Cuda itself stays powered. */
            mc_ppc *m = p->machine;
            if (state && !m->reset_held) {
                m->reset_held = 1;
                board_reset(p);
                ppc_reset(&m->cpu);
                ppc_jit_cpu_reset(m->jit);
            } else if (!state) m->reset_held = 0;
        } else if (signal == MC_ADBCTL_LINE_POWER) {
            if (state) p->powered = 1;
            else if (p->powered) mc_ppc_power_off(p->machine);
        } else if (signal == MC_ADBCTL_LINE_NMI) mc_gc_set_source(&p->gc, MC_GC_IRQ_NMI, state);
    }
    if (chip == &p->scc && signal == MC_SCC_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_SCCA, state);
    if (chip == &p->via && signal == MC_VIA_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_VIA, state);
    if (chip == &p->via && signal == MC_VIA_LINE_PB) mc_adbctl_via_pb(&p->cuda, (uint8_t)state);
    if (chip == &p->mesh && signal == MC_MESH_LINE_IRQ) mc_gc_set_source(&p->gc, MC_GC_IRQ_SCSI1, state);
    if (chip == &p->sound && state) mc_gc_dma_kick(&p->gc, signal == MC_AWACS_LINE_OUT_DRQ ? 8 : 9);
    if (chip == &p->mesh && signal == MC_MESH_LINE_DMA_STATUS) mc_gc_dma_bits(&p->gc, MC_GC_IRQ_DMA_SCSI1, 0xE0, (uint8_t)state);
    if (chip == &p->mesh && signal == MC_MESH_LINE_DRQ && state) mc_gc_dma_kick(&p->gc, MC_GC_IRQ_DMA_SCSI1);
}

/* The PRAM file: Cuda's (adbctl.h: its clock and parameter RAM), then the
 * flash memory. A file without the flash memory (or none) leaves it erased,
 * and the ROM starts with its own defaults. */
#define CUDA_RECORD (8 + 1 + 8 + 256)
static void load_nvram(pippin_board *p) {
    mc_am29f010_init(&p->flash);
    if (!mc_adbctl_nvram_load(&p->cuda, p->nvram_path)) {
        mc_adbctl_set_seconds(&p->cuda, 0);
        return;
    }
    FILE *f = fopen(p->nvram_path, "rb");
    if (!f) return;
    if (fseek(f, CUDA_RECORD, SEEK_SET) || fread(p->flash.data, 1, MC_AM29F010_SIZE, f) != MC_AM29F010_SIZE)
        mc_am29f010_init(&p->flash);
    fclose(f);
}

static void save_nvram(pippin_board *p) {
    if (!p->nvram_path[0] || !mc_adbctl_nvram_save(&p->cuda, p->nvram_path, p->host_start)) return;
    FILE *f = fopen(p->nvram_path, "ab");
    if (!f) return;
    fwrite(p->flash.data, 1, MC_AM29F010_SIZE, f);
    fclose(f);
}

static void destroy(void *dev) {
    pippin_board *p = dev;
    save_nvram(p);
    mc_scsi_bus_free(&p->scsi);
    mc_vdisk_destroy(p->cd_image);
    free(p->cuda.mcu);
    free(p);
}

static void codec_control(void *dev, uint32_t word) { mc_awac_control(dev, (uint16_t)word); }
static uint32_t codec_status(void *dev) {
    const mc_awac *codec = dev;
    return (uint32_t)mc_awac_status(codec, 0) | (uint32_t)mc_awac_status(codec, 1) << 8 |
           (uint32_t)mc_awac_status(codec, 2) << 16 | (uint32_t)mc_awac_status(codec, 3) << 24;
}

static void cd_ejected(void *, mc_scsi_unit *, mc_vdisk *);

/* The two serial ports (GeoPort modem on channel A, printer on B). */
static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }
static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    pippin_board *p = ctx;
    if (p->serial.transmit) p->serial.transmit(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    pippin_board *p = ctx;
    if (p->serial.outputs) p->serial.outputs(p->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}
static void serial_host(void *board, const mc_serial_host *host) {
    pippin_board *p = board;
    if (host) p->serial = *host;
    else memset(&p->serial, 0, sizeof p->serial);
}
static size_t serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    pippin_board *p = board;
    return port < 2 ? mc_scc_receive(&p->scc, port_channel(port), data, count) : count;
}
static void serial_modem(void *board, unsigned port, int dcd, int cts) {
    pippin_board *p = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&p->scc, port_channel(port), dcd);
    mc_scc_set_cts(&p->scc, port_channel(port), cts);
}

static void *build(mc_ppc *m) {
    pippin_board *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->machine = m;
    m->board = p;
    const mc_hw_host *host = &m->host;
    const mc_ppc_port port = {MC_PPC_PORT_DIRECT, 4, 0, 0};
    p->taos.dev = (mc_dev){&taos_timer_ops, host, "Taos"};
    p->taos.board = p;

    mc_bus_attach(&m->bus, mc_ppc_region(m, "flash"), 0, 4u * MC_AM29F010_SIZE, p, &flash_ops, 0);
    mc_bus_attach(&m->bus, mc_ppc_region(m, "early"), 0, 0x1000, p, &early_ops, 0);
    mc_ppc_attach(m, mc_ppc_region(m, "aspen"), 0, 0x2000000, p, &asp_ops, port);
    mc_ppc_attach(m, mc_ppc_region(m, "memory-video"), 0, 0x1000, p, &regs_ops, port);
    mc_bus_attach(&m->bus, mc_ppc_region(m, "palette"), 0, 0x1000, p, &palette_ops, 0);
    mc_bus_map_memory(&m->bus, 0xF0000000u, sizeof p->vram, p->vram, sizeof p->vram, 1);
    mc_gc_init(&p->gc, host, "Grand Central");
    p->gc_pci = mc_gc_function(&p->gc);
    mc_via_init(&p->via, host, "VIA", 9);
    mc_gc_attach(&p->gc, MC_GC_VIA, 0x2000u, 0, p, &via_ops);
    mc_scc_init(&p->scc, host);
    mc_scc_set_clocks(&p->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port sport = {p, scc_transmit, scc_outputs};
    mc_scc_set_port(&p->scc, &sport);
    mc_gc_attach(&p->gc, MC_GC_SCC, 0x1000u, 0, &p->scc, &scc_ops);
    mc_gc_attach(&p->gc, 0x12000u, 0x1000u, 0, &p->scc, &mc_scc_ops);
    mc_adb_bus_init(&p->adb);
    mc_adb_keyboard_init(&p->keyboard);
    mc_adb_applejack_init(&p->applejack);
    mc_adb_attach(&p->adb, &p->keyboard.dev);
    mc_adb_attach(&p->adb, &p->applejack.dev);
    mc_adbctl_init(&p->cuda, host, "Cuda", MC_ADBCTL_CUDA, &p->via, &p->adb);
    if (!mc_config_nvram_path(&m->config, p->nvram_path, sizeof p->nvram_path)) p->nvram_path[0] = 0;
    load_nvram(p);
    mc_scsi_bus_init(&p->scsi);
    mc_scsi_cd_init_pippin(&p->cd);
    mc_scsi_unit_on_eject(&p->cd, cd_ejected, p);
    mc_scsi_bus_attach(&p->scsi, 3, mc_scsi_unit_target(&p->cd));
    mc_mesh_init(&p->mesh, host, "MESH", &p->scsi, 0xE2);
    mc_gc_attach(&p->gc, MC_GC_MESH, 0x100, 4, &p->mesh, &mc_mesh_ops);
    const mc_dbdma_port mesh_port = {&p->mesh, mc_mesh_dma_read, mc_mesh_dma_write, NULL, NULL};
    mc_gc_dma_port(&p->gc, MC_GC_IRQ_DMA_SCSI1, &mesh_port);
    mc_awac_init(&p->codec, host, "AWAC");
    mc_awac_set_status(&p->codec, 1, 0x20);
    const mc_awacs_codec codec = {&p->codec, codec_control, codec_status};
    mc_awacs_init(&p->sound, host, "AWACS", &codec);
    mc_gc_attach(&p->gc, MC_GC_AWACS, MC_AWACS_SIZE, 0, &p->sound, &mc_awacs_ops);
    const mc_dbdma_port audio_out = {&p->sound, NULL, mc_awacs_dma_write, NULL, NULL};
    const mc_dbdma_port audio_in = {&p->sound, mc_awacs_dma_read, NULL, NULL, NULL};
    mc_gc_dma_port(&p->gc, 8, &audio_out);
    mc_gc_dma_port(&p->gc, 9, &audio_in);
    mc_adbctl_ops.reset(&p->cuda);
    return p;
}

static int insert_cd(void *dev, mc_media_kind kind, unsigned drive, mc_disk *disk,
                     char *error, size_t capacity) {
    pippin_board *p = dev;
    if (kind != MC_MEDIA_CDROM || drive != 0) {
        if (capacity) snprintf(error, capacity, "The Pippin has only its CD-ROM drive");
        return 0;
    }
    if (!disk) {
        mc_vdisk_destroy(mc_scsi_unit_remove(&p->cd));
        p->cd_image = NULL;
        return 1;
    }
    mc_vdisk *next = mc_vdisk_create_device(disk, error, capacity);
    if (!next) return 0;
    mc_vdisk_destroy(mc_scsi_unit_remove(&p->cd));
    mc_scsi_cd_insert(&p->cd, next);
    p->cd_image = next;
    return 1;
}

static void cd_ejected(void *dev, mc_scsi_unit *unit, mc_vdisk *disk) {
    pippin_board *p = dev;
    (void)unit;
    mc_vdisk_destroy(disk);
    p->cd_image = NULL;
    mc_media_eject(&p->machine->media, MC_MEDIA_CDROM, 0);
}

static uint32_t audio_rate(void *dev) { return mc_awacs_rate(&((pippin_board *)dev)->sound); }
static size_t audio_pull(void *dev, int16_t *stereo, size_t frames) {
    pippin_board *p = dev;
    const size_t n = mc_awacs_pull(&p->sound, stereo, frames);
    mc_ppc_audio_startup(p->machine, stereo, n, p->sound.played,
                         !p->sound.out_n && !p->sound.ring_n && !(p->gc.ch[8].status & MC_DBDMA_ACTIVE));
    return n;
}

/* The Applejack controller is the Pippin's pointing device (adb.h): the
 * host's mouse moves its trackball and presses Left Fire, and these keys
 * of the host's keyboard are its other switches instead of keys of the ADB
 * keyboard (Extended Keyboard codes): the arrows the directional pad, F1
 * to F4 green, red, blue and yellow, F5 to F7 the square, circle and
 * diamond buttons, F8 Right Fire. */
static uint32_t applejack_switch(uint8_t code) {
    switch (code) {
    case 0x3E: return MC_APPLEJACK_UP;
    case 0x3D: return MC_APPLEJACK_DOWN;
    case 0x3B: return MC_APPLEJACK_LEFT;
    case 0x3C: return MC_APPLEJACK_RIGHT;
    case 0x7A: return MC_APPLEJACK_GREEN;
    case 0x78: return MC_APPLEJACK_RED;
    case 0x63: return MC_APPLEJACK_BLUE;
    case 0x76: return MC_APPLEJACK_YELLOW;
    case 0x60: return MC_APPLEJACK_SQUARE;
    case 0x61: return MC_APPLEJACK_CIRCLE;
    case 0x62: return MC_APPLEJACK_DIAMOND;
    case 0x64: return MC_APPLEJACK_RIGHT_FIRE;
    default: return 0;
    }
}
static int key(void *dev, uint8_t code, int down) {
    pippin_board *p = dev;
    if (code == 0x7F) mc_adbctl_power_key(&p->cuda, down);
    const uint32_t button = applejack_switch(code);
    if (button) mc_adb_applejack_switch(&p->applejack, button, down);
    else if (!mc_adb_keyboard_key(&p->keyboard, code, down)) return 0;
    mc_adbctl_poke(&p->cuda);
    return 1;
}
static int mouse(void *dev, int dx, int dy, int button) {
    pippin_board *p = dev;
    mc_adb_applejack_move(&p->applejack, dx, dy);
    mc_adb_applejack_switch(&p->applejack, MC_APPLEJACK_LEFT_FIRE, button);
    mc_adbctl_poke(&p->cuda);
    return 1;
}
static void set_clock(void *dev, int64_t host_seconds) {
    pippin_board *p = dev;
    p->host_start = host_seconds;
    p->clock_set = 1;
    mc_adbctl_set_clock(&p->cuda, host_seconds);
}
/* The original Taos register descriptors encode base[18:0]<<13,
 * stride-in-longwords<<22 and depth-select<<31. Vertical active start
 * and end are $54/$58, each <<20 (the $1C timing count includes blanking).
 * Display enable is bit $04000000 of register $20; do not present an
 * uninitialized buffer before the native driver programs a valid mode. */
static int framebuffer(void *dev, mc_framebuffer *fb) {
    pippin_board *p = dev;
    if (!(early_word(p, 0x20) & TAOS_TIMING)) return 0;
    uint32_t base = early_word(p, 0) >> 13;
    uint32_t stride = (early_word(p, 4) >> 22) * 4;
    uint32_t depth = early_word(p, 8) & 0x80000000u ? 16 : 8;
    uint32_t width = stride * 8 / depth;
    uint32_t start = early_word(p, 0x54) >> 20;
    uint32_t end = early_word(p, 0x58) >> 20;
    if (end <= start) return 0;
    uint32_t height = end - start;
    if (!width || width > 2048 || height > 2048 ||
        base >= sizeof p->vram || (uint64_t)stride * height > sizeof p->vram - base) return 0;
    *fb = (mc_framebuffer){.width = width, .height = height, .depth = depth,
        .row_bytes = stride, .pixels = p->vram + base, .palette = p->palette,
        .serial = ++p->fb_serial};
    return 1;
}
static const mc_board_ops ops = {
    .reset = board_reset, .line = line, .destroy = destroy, .insert = insert_cd,
    .framebuffer = framebuffer,
    .key = key, .mouse = mouse, .set_clock = set_clock,
    .audio_rate = audio_rate, .audio_pull = audio_pull,
    .serial_host = serial_host, .serial_receive = serial_receive, .serial_modem = serial_modem,
};
static unsigned builtin(const mc_model *model, unsigned width, unsigned height) {
    (void)model;
    return width == 640 && height == 480 ? MC_DEPTH_BIT(16) : 0;
}
static const mc_hw_caps caps = {.builtin = builtin, .features = MC_FEATURE_FIXED_SCREEN | MC_FEATURE_NO_FLOPPY};
static const uint32_t roms[] = {0x2BEF21B7u, 0x2BF65931u, 0};
/* 6 MiB retains the configuration from earlier Livadix releases. */
static const uint16_t ram[] = {5, 6, 7, 9, 13, 21, 0};
static const mc_region map[] = {
    {"ram", 0, 0x4000000u, 0, MC_REGION_RAM},
    {"flash", 0x40000000u, 4u * MC_AM29F010_SIZE, 4u * MC_AM29F010_SIZE - 1u, MC_REGION_IO},
    {"early", 0xF0800000u, 0x1000u, 0xFFFu, MC_REGION_IO},
    {"palette", 0xF1000000u, 0x1000u, 0xFFFu, MC_REGION_IO},
    {"aspen", 0xF2000000u, 0x2000000u, 0x1FFFFFFu, MC_REGION_IO},
    {"memory-video", 0xF8000000u, 0x1000u, 0xFFFu, MC_REGION_IO},
    {"rom", 0xFF000000u, 0x1000000u, 0, MC_REGION_ROM},
};
static const char *const models[] = {"pippin", "pippin-dev", NULL};
const mc_ppc_desc mc_pippin_desc = {
    .key = "pippin", .models = models, .name = "Bandai Pippin", .cpu = PPC_MODEL_603,
    .cpu_hz = 66000000u, .timebase_hz = 8250000u, .cpi = 8, .cpi_div = 9,
    .roms = roms, .ram_mb = ram, .map = map, .nmap = sizeof map / sizeof *map,
    .unmapped = MC_PPC_UNMAPPED_TEA, .build = build, .ops = &ops, .caps = &caps,
};
