/* Livadix — the LC 475, LC 575, Quadra 605 and Performa 475, 476, 575, 577
 * and 578 board (MEMCjr, PrimeTime and Cuda).
 *
 * Sources: Macintosh LC 575 Developer Note (Apple, 1994): chapter 1,
 * Table 1-1 (the LC 475 beside the LC 575: 68LC040 at 25 and 33 MHz, 4 to
 * 36 MB, 512 KB to 1 MB of VRAM, one expansion slot), "RAM Expansion",
 * Figure 1-4, "RAM SIMMs" (4 MB on the board, one 72-pin SIMM), "Video
 * RAM", Table 1-3 (8 bits with 512 KB, 16 bits with 1 MB of VRAM),
 * "Keyboard" (the power key, Table 1-8), "Expansion Slot" (the slot of the
 * LC 475 and Quadra 605); chapter 2, Figure 2-1 (block diagram: MEMCjr,
 * PrimeTime with SWIM II, VIA1, VIA2 and the sound, Cuda, DFAC II,
 * Antelope, the 53C96 and SCC on the I/O bus), "Custom ICs", "Address
 * Maps" (the Quadra 610 and 650's map; VRAM from $F900 0000), Figure 2-2,
 * "Expansion Card Addresses" (the card as NuBus slot $E), Table 2-1 (the
 * built-in display's 640 x 480 timing); chapter 3, "ROM Software"
 * (machine identification, Gestalt 92, MEMCjr, Cuda with soft power off,
 * the push buttons' level-2 interrupt); chapter 4, Table 4-2 (/SLOTIRQ
 * reported as the slot $E interrupt, level 2). Guide to the Macintosh
 * Family Hardware, 2nd ed., chapter 4 (VIA pin functions, as on the
 * Quadra 700: quadra700.c). Quadra 840AV Developer Note, "Cuda" (cuda.h).
 *
 * Observed from ROM FF7439EE:
 *   - the ROM finds the CPU ID register ($A55A 22xx, primetime.h) and the
 *     ProductInfo record of its low word: the LC 475 and Performa 475/476
 *     $2221 (box 83), the Quadra 605 $2225 (box 88), the LC 575 and
 *     Performa 575/577/578 $222E (box 86); no VIA identity is read;
 *   - the decoder record of the Centris/Quadra 610, 650 and 800 (ROM
 *     offset $A8680): the I/O devices where those machines have them
 *     (VIA1 $50F0 0000, VIA2 $50F0 2000, SCC $50F0 C020, SCSI $50F1 0000,
 *     sound $50F1 4000, SWIM II $50F1 E000; quadra800.c), the MEMCjr at
 *     $50F0 E000 (memcjr.h), PrimeTime's registers at $50F1 8000;
 *   - the video programmed through DAFB's registers at $F980 0000 (dafb.h);
 *   - the Cuda driver on VIA1 PB3-PB5 and the shift register (cuda.h).
 *
 * Livadix's own code. */
#include "../hw_machine.h"
#include "../absent.h"
#include "../adb.h"
#include "../adbctl.h"
#include "../via.h"
#include "../asic/memcjr.h"
#include "../asic/primetime.h"
#include "../enet_card.h"
#include "../livadix_card.h"
#include "../nubus.h"
#include "../asc.h"
#include "../ncr53c96.h"
#include "../scc.h"
#include "../scsi_bus.h"
#include "../scsi_target.h"
#include "../swim.h"
#include "../guest/guest.h"
#include "../storage/vdisk.h"
#include "../video/dafb.h"
#include "../../core/models.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROM_BASE 0x40000000u
#define IO_BASE 0x50000000u
#define IO_IMAGE 0x40000u
#define VRAM_SIZE (1u << 20)        /* VRAM fully populated: 1 MB (Developer Note, "Video RAM") */
#define CARD_SLOT 0xEu
#define VIDEO_BOARD_ID 0x004Du      /* the ROM's board ID for the built-in video (slot $9 PRAM) */              /* the expansion slot answers as NuBus slot $E ("Expansion Card Addresses") */

/* A board of the family: its CPU ID, whether it has the LC 575's built-in
 * display (640 x 480 only, "Built-in Color Display"). */
typedef struct q605_board {
    const char *model;
    uint16_t cpu_id;
    uint8_t builtin_display;
} q605_board;

static const q605_board boards[] = {
    {"lc-475", MC_PRIMETIME_ID_LC475, 0},
    {"performa-475", MC_PRIMETIME_ID_LC475, 0},
    {"performa-476", MC_PRIMETIME_ID_LC475, 0},
    {"quadra-605", MC_PRIMETIME_ID_Q605, 0},
    {"lc-575", MC_PRIMETIME_ID_LC575, 1},
    {"performa-575", MC_PRIMETIME_ID_LC575, 1},
    {"performa-577", MC_PRIMETIME_ID_LC575, 1},
    {"performa-578", MC_PRIMETIME_ID_LC575, 1},
};

static const q605_board *board_of(const mc_model *model) {
    for (size_t i = 0; i < sizeof boards / sizeof *boards; i++)
        if (!strcmp(boards[i].model, model->key)) return &boards[i];
    return &boards[3];
}

typedef struct q605 {
    mc_hw *hw;
    const q605_board *board;
    mc_via via1, via2;
    mc_memcjr memc;
    mc_primetime prime;
    mc_absent comm;             /* the LC 575's communications slot, no card fitted */
    uint8_t buttons_held, buttons_latched;  /* the LC 575's push buttons (button_read) */
    uint64_t buttons_release;               /* when the held ones come up (C15M) */
    mc_scc scc;
    mc_asc asc;
    mc_swim swim;
    mc_scsi_bus scsi_bus;
    mc_ncr53c96 scsi;
    mc_scsi_unit unit[7];       /* SCSI IDs 0-6; the CD-ROM drive at 3 */
    mc_vdisk *vdisk[7];
    mc_dafb dafb;
    mc_nubus nubus;
    mc_livadix_card card;          /* the Livadix Display Card, in the expansion slot when fitted */
    mc_enet_card *enet;          /* the Ethernet card (network.card), NULL for none */
    unsigned card_slot;
    mc_serial_host serial;
    int chime_done;             /* emulation.startup_sound off: the ROM's chime is over */
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adbctl cuda;
    int powered;                /* Cuda turned the power on (firmware level: about 1.5 s after the start) */
    uint64_t fb_serial;
    int overlay;
    int overlay_region;         /* the trap over the ROM's own range while the overlay is on */
    char nvram[1024];
    int64_t host_start;         /* the host's time given at start (mc_machine_set_clock) */
} q605;

static const mc_region q605_map[] = {
    {"rom", ROM_BASE, 0x10000000u, 0, MC_REGION_ROM},
    {"io", IO_BASE, 0x04000000u, IO_IMAGE - 1u, MC_REGION_IO},
    {"cpuid", 0x5FFF0000u, 0x10000u, 0xFFFFu, MC_REGION_IO},
};

static const mc_irq_route q605_irqs[] = {
    {"via1", 1}, {"via2", 2}, {"scc", 4}, {"nmi", 7},
};

/* --- the reset overlay ------------------------------------------------------------- */

static void map_ram(q605 *q);

/* The ROM at 0 from reset until the first access to its own range, as on
 * the Quadra 700 (quadra700.c). */
static void overlay_on(q605 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 1;
    mc_bus_map_memory(&hw->bus, 0, ROM_BASE, hw->rom, hw->rom_size, 0);
    mc_bus_remap(&hw->bus, q->overlay_region, ROM_BASE, 0x10000000u);
    mc_hw_maps_changed(hw);
}

static void overlay_off(q605 *q) {
    mc_hw *hw = q->hw;
    q->overlay = 0;
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x10000000u, hw->rom, hw->rom_size, 0);
    map_ram(q);
}

static uint32_t trap_read(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    if (q->overlay) overlay_off(q);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | q->hw->rom[(offset + i) & (q->hw->rom_size - 1u)];
    return v;
}
static void trap_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)offset;
    (void)size;
    (void)value;
    q605 *q = dev;
    if (q->overlay) overlay_off(q);
}
static const mc_dev_ops trap_ops = {trap_read, trap_write, NULL, NULL};

/* --- RAM ------------------------------------------------------------------------ */

static void map_ram(q605 *q) {
    mc_memcjr_map(&q->memc, &q->hw->bus, q->hw->ram);
    mc_hw_maps_changed(q->hw);
}

static void memc_remap(void *ctx) {
    q605 *q = ctx;
    if (!q->overlay) map_ram(q);
}

/* --- VIA1 and Cuda ----------------------------------------------------------------- */

/* Every CPU access to VIA1 comes after Cuda's own up to now (adbctl.h). */
static uint32_t via1_read(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    mc_adbctl_sync(&q->cuda);
    return mc_via_ops.read(&q->via1, offset, size);
}
static void via1_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q605 *q = dev;
    mc_adbctl_sync(&q->cuda);
    mc_via_ops.write(&q->via1, offset, size, value);
}
static int via1_stable(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    return !q->cuda.firmware && mc_via_ops.stable && mc_via_ops.stable(&q->via1, offset, size);
}
static const mc_dev_ops via1_ops = {via1_read, via1_write, NULL, NULL, via1_stable};

/* The LC 575's front-panel push buttons, a register in PrimeTime's VIA2
 * space at +$1006 (VIA2 base $50F0 2000; ROM FF7439EE, the handler the ROM
 * installs when ProductInfo's features hold bit 27 ($DD4 bit 3), ROM offset
 * $AA1E0): a read gives the buttons pressed since their bit was last
 * cleared, bit 0 volume down, 1 volume up, 2 brightness down, 3 brightness
 * up (the ROM's table at $AA418, for bits 8-11 of the word it reads); a
 * write of a bit clears it, the bit coming back while the button is held
 * (the 20 ms debounce at $AA2F2 writes it and reads again). A press raises
 * VIA2's interrupt flag 5 (level 2, Developer Note, "ROM Software"; the ROM
 * puts the handler in Via2DT[5], $D84, and turns IER bit 5 off while it
 * works, $AA3FE, and on again when the buttons are up, $AA40A). */
static int is_button_register(const q605 *q, uint32_t offset) {
    return q->board->builtin_display && (offset & 0x1FFFu) == 0x1006u;
}

static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    if (is_button_register(q, offset)) return q->buttons_latched;
    return mc_via_ops.read(&q->via2, offset, size);
}

static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q605 *q = dev;
    if (is_button_register(q, offset)) {
        const uint8_t mask = (uint8_t)(value & 0x0Fu);
        q->buttons_latched = (uint8_t)((q->buttons_latched & ~mask) | (q->buttons_held & mask));
        mc_via_set_ifr(&q->via2, MC_VIA_IRQ_T2, q->buttons_latched != 0);
        return;
    }
    mc_via_ops.write(&q->via2, offset, size, value);
}

static int via2_stable(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    if (is_button_register(q, offset)) return 0;
    return mc_via_ops.stable(&q->via2, offset, size);
}

static const mc_dev_ops via2_ops = {via2_read, via2_write, NULL, NULL, via2_stable};

/* A button pressed for 100 ms (board_poll lets it up). */
static void press_button(q605 *q, uint8_t bit) {
    q->buttons_held |= bit;
    q->buttons_latched |= bit;
    q->buttons_release = mc_dev_now(&q->via2.dev) + MC_C15M_HZ / 10u;
    mc_via_set_ifr(&q->via2, MC_VIA_IRQ_T2, 1);
}

/* The machine's controls: the LC 575's volume and brightness buttons. */
static int board_control(void *board, int control, int value) {
    q605 *q = board;
    if (!q->board->builtin_display || (control != MC_CONTROL_BRIGHTNESS && control != MC_CONTROL_VOLUME)) return -1;
    const unsigned bit = (control == MC_CONTROL_VOLUME ? 0u : 2u) + (value > 0 ? 1u : 0u);
    press_button(q, (uint8_t)(1u << bit));
    return 0;
}

/* --- the frame buffer controller ----------------------------------------------------- */

/* The DAFB-compatible registers ($F980 0000-$F980 01FF) on the MEMCjr's
 * six-bit data path: a write stores the six bits written below the
 * window's six, a read loads the window (memcjr.h). The CLUT/DAC and the
 * clock generator above are reached directly. */
static uint32_t video_read(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    const uint32_t v = mc_dafb_ops.read(&q->dafb, offset, size);
    if (offset < 0x200u && size == 4) mc_memcjr_load(&q->memc, v);
    return v;
}
static void video_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q605 *q = dev;
    if (offset < 0x200u && size == 4) value = mc_memcjr_compose(&q->memc, value);
    mc_dafb_ops.write(&q->dafb, offset, size, value);
}
static int video_stable(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    return offset >= 0x200u && mc_dafb_ops.stable && mc_dafb_ops.stable(&q->dafb, offset, size);
}
static const mc_dev_ops video_ops = {video_read, video_write, NULL, NULL, video_stable};

/* --- wiring ------------------------------------------------------------------------ */

static void cuda_hold(q605 *q) {
    const int held = q->cuda.line_state[MC_ADBCTL_LINE_RESET] || !q->cuda.line_state[MC_ADBCTL_LINE_POWER];
    if (held && !q->overlay && q->overlay_region) overlay_on(q);
    mc_hw_hold_reset(q->hw, held);
}

static void slot_irq(q605 *q, unsigned bit, int asserted) { mc_nubus_irq_bit(&q->nubus, bit, asserted); }

static void board_line(void *board, void *dev, int line, int state) {
    q605 *q = board;
    mc_hw *hw = q->hw;
    if (dev == &q->via1) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via1"), state);
        /* PB3-PB5: Cuda's TREQ, BYTEACK and TIP (cuda.h). */
        else if (line == MC_VIA_LINE_PB) mc_adbctl_via_pb(&q->cuda, (uint8_t)state);
        /* PA5 vHeadSel: the floppy drive's SEL line (Guide, Table 4-2). */
        else if (line == MC_VIA_LINE_PA) mc_swim_set_headsel(&q->swim, (state & 0x20) != 0);
    } else if (dev == &q->via2) {
        if (line == MC_VIA_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "via2"), state);
        /* v2VBL: VIA2 Timer 1 squares PB7 at 60.15 Hz into VIA1 CA1. */
        else if (line == MC_VIA_LINE_PB) mc_via_set_ca1(&q->via1, state & 0x80);
    } else if (dev == &q->cuda) {
        /* The processor runs while Cuda powers the system and releases
         * /RESET; a reset brings the overlay back. Cuda's power down
         * (pseudo-command $0A, the ROM's ShutDown) switches the machine
         * off. */
        if (line == MC_ADBCTL_LINE_NMI) mc_irq_set(&hw->irq, mc_hw_irq(hw, "nmi"), state);
        else if (line == MC_ADBCTL_LINE_RESET || line == MC_ADBCTL_LINE_POWER) {
            if (line == MC_ADBCTL_LINE_POWER) {
                if (state) q->powered = 1;
                else if (q->powered) mc_hw_power_off(hw);
            }
            cuda_hold(q);
        }
    } else if (dev == &q->scsi) {
        /* The 53C96 interrupt on VIA2 CB2, low when asserted; its DREQ on
         * VIA2 CA2, high when asserted: this ROM programs CA2 for a rising
         * edge (PCR $26, ROM offset $2EDC; the Quadra 800's ROM a falling
         * one) and after a DMA selection waits for that flag (ROM offset
         * $D1982). */
        if (line == MC_NCR53C96_LINE_IRQ) mc_via_set_cb2(&q->via2, !state);
        else if (line == MC_NCR53C96_LINE_DRQ) mc_via_set_ca2(&q->via2, state);
    } else if (dev == &q->scc) {
        if (line == MC_SCC_LINE_IRQ) mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), state);
        /* /W//REQ on VIA1 PA7 vSCCWrReq, low when asserted (Guide, Table 4-2). */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&q->via1, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &q->asc) {
        /* The sound's /IRQ on VIA2 CB1 (Guide, Table 4-20). */
        if (line == MC_ASC_LINE_IRQ) mc_via_set_cb1(&q->via2, !state);
    } else if (dev == &q->dafb) {
        if (line == MC_DAFB_LINE_VBL) slot_irq(q, 6, state);
    } else if (dev == &q->card) {
        if (line == MC_LIVADIX_CARD_LINE_IRQ) mc_nubus_irq(&q->nubus, q->card_slot, state);
    }
}

/* Slot interrupts: active low on VIA2 port A (slot $E on PA5, the video
 * on PA6), ORed into /SLOTIRQ on CA1, as on the Quadra 700. */
static void slot_pins(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high) {
    q605 *q = ctx;
    mc_via_set_pa(&q->via2, levels, mask);
    mc_via_set_ca1(&q->via2, slotirq_high);
}

/* --- persistence ------------------------------------------------------------------ */

/* A new PRAM: what the ROM writes when it finds the PRAM invalid (the
 * original 20 bytes, the extended PRAM's signature "NuMc", the default
 * startup device at $76-$7B), with 32-bit addressing on ($8A = $05, as
 * Mac OS 8.0 sets it); the values of the Quadra 700's and 800's ROMs
 * (quadra800.c), which this ROM writes too. */
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

static void load_nvram(q605 *q) {
    if (!mc_adbctl_nvram_load(&q->cuda, q->nvram)) {
        fresh_pram(mc_adbctl_pram(&q->cuda));
        mc_adbctl_set_seconds(&q->cuda, 0);
    }
}

/* The depth the built-in video drives a monitor at with its 1 MB of VRAM;
 * 0 for none: 16 bits on the 12", 13", 16" and VGA monitors, 8 on the
 * others (Developer Note, Table 1-4 for the 610-800, Table 1-3 for the LC
 * 475 family), as the ROM offers them with the board's DAC (dafb.h,
 * antelope). */
static uint32_t video_max_depth(mc_dafb_monitor monitor) {
    switch (monitor) {
    case MC_MONITOR_NONE: return 0;
    case MC_MONITOR_RGB12: case MC_MONITOR_HIRES13: case MC_MONITOR_RGB16: case MC_MONITOR_VGA: return 16;
    default: return 8;
    }
}

/* The screen depth video.depth asks for, as if chosen in the Monitors
 * control panel: the built-in video's slot PRAM (slot $9 at extended PRAM
 * $46-$4D: board ID $004D, the depth's mode $80 + log2(depth), the
 * monitor's sResource twice, its code: $02, $06, $05, $0A and $00 for the
 * 12", 13", portrait, 16" and 21" monitors), as this ROM writes it, takes
 * the mode when the record is for the monitor connected. A fresh PRAM is
 * left to the ROM, which starts in its default depth. */
static void set_depth(q605 *q, uint8_t *p) {
    static const struct { uint8_t monitor, code; } codes[] = {
        {MC_MONITOR_RGB12, 0x02}, {MC_MONITOR_HIRES13, 0x06}, {MC_MONITOR_PORTRAIT, 0x05},
        {MC_MONITOR_RGB16, 0x0A}, {MC_MONITOR_RGB21, 0x00},
    };
    const mc_config *c = &q->hw->config;
    int code = -1;
    for (size_t i = 0; i < sizeof codes / sizeof *codes; i++)
        if (codes[i].monitor == q->dafb.monitor) code = codes[i].code;
    if (code < 0 || p[0x10] != 0xA8 || p[0x46] != (VIDEO_BOARD_ID >> 8) || p[0x47] != (VIDEO_BOARD_ID & 0xFF) ||
        p[0x4B] != code)
        return;
    if (c->depth > video_max_depth(q->dafb.monitor)) return;
    unsigned mode = 0x80;
    for (uint32_t d = c->depth; d > 1; d >>= 1) mode++;
    p[0x48] = (uint8_t)mode;
}

/* The same for the Livadix Display Card in slot $E. */
static void set_card_depth(q605 *q, uint8_t *pram) {
    const mc_config *c = &q->hw->config;
    uint8_t *p = pram + 0x46 + 8 * (q->card_slot - 9);
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

static void drop_disks(q605 *q) {
    for (int id = 0; id < 7; id++) {
        if (id == CD_ID || !q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
        q->vdisk[id] = NULL;
    }
}

/* The hard disks on their SCSI IDs, as on the Quadra 800 (quadra800.c). */
static int layout_disks(q605 *q, char *error, size_t capacity) {
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

static void cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    q605 *q = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    q->vdisk[CD_ID] = NULL;
    mc_media_eject(&q->hw->media, MC_MEDIA_CDROM, 0);
}

static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    q605 *q = board;
    if (kind == MC_MEDIA_DISK) return layout_disks(q, error, capacity);
    if (kind == MC_MEDIA_FLOPPY) return mc_swim_insert(&q->swim, drive, disk, 0, error, capacity);
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

static void board_poll(void *board) {
    q605 *q = board;
    if (q->buttons_held && mc_dev_now(&q->via2.dev) >= q->buttons_release) q->buttons_held = 0;
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
    q605 *q = board;
    return q->hw->config.sound ? mc_asc_rate(&q->asc) : 0;
}

static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) {
    q605 *q = board;
    const size_t n = mc_asc_pull(&q->asc, stereo, frames);
    if (!q->hw->config.startup_sound && !q->chime_done) {
        memset(stereo, 0, n * 2 * sizeof *stereo);
        if (q->asc.played && !mc_asc_fifo_level(&q->asc, 0) && !mc_asc_fifo_level(&q->asc, 1) && !q->asc.ring_count) q->chime_done = 1;
    }
    return n;
}

/* --- serial ports -------------------------------------------------------------- */

static int port_channel(unsigned port) { return port == 0 ? MC_SCC_A : MC_SCC_B; }

static void scc_transmit(void *ctx, int channel, uint8_t byte) {
    q605 *q = ctx;
    if (q->serial.transmit) q->serial.transmit(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, byte);
}
static void scc_outputs(void *ctx, int channel, unsigned outputs) {
    q605 *q = ctx;
    if (q->serial.outputs) q->serial.outputs(q->serial.ctx, channel == MC_SCC_A ? 0u : 1u, (outputs & MC_SCC_OUT_DTR) != 0);
}

static void board_serial_host(void *board, const mc_serial_host *host) {
    q605 *q = board;
    if (host) q->serial = *host;
    else memset(&q->serial, 0, sizeof q->serial);
}
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    q605 *q = board;
    return port < 2 ? mc_scc_receive(&q->scc, port_channel(port), data, count) : count;
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) {
    q605 *q = board;
    if (port >= 2) return;
    mc_scc_set_dcd(&q->scc, port_channel(port), dcd);
    mc_scc_set_cts(&q->scc, port_channel(port), cts);
}

/* --- SCSI ------------------------------------------------------------------------ */

static uint32_t scsi_read(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.read(dev, offset >> 4, size); }
static void scsi_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_ncr53c96_ops.write(dev, offset >> 4, size, value);
}
static int scsi_stable(void *dev, uint32_t offset, unsigned size) { return mc_ncr53c96_ops.stable(dev, offset >> 4, size); }
static const mc_dev_ops scsi_ops = {scsi_read, scsi_write, NULL, NULL, scsi_stable};

static uint32_t pdma_read(void *dev, uint32_t offset, unsigned size) {
    q605 *q = dev;
    (void)offset;
    uint32_t v = 0;
    mc_ncr53c96_dma_read(&q->scsi, size, &v);
    return v;
}
static void pdma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    q605 *q = dev;
    (void)offset;
    mc_ncr53c96_dma_write(&q->scsi, size, value);
}
static const mc_dev_ops pdma_ops = {pdma_read, pdma_write, NULL, NULL};

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    q605 *q = board;
    mc_via_ops.reset(&q->via1);
    mc_via_ops.reset(&q->via2);
    mc_primetime_ops.reset(&q->prime);
    mc_dafb_ops.reset(&q->dafb);
    if (q->card_slot) mc_livadix_card_ops.reset(&q->card);
    mc_enet_card_reset(q->enet);
    mc_ncr53c96_reset(&q->scsi);
    mc_scc_ops.reset(&q->scc);
    mc_asc_ops.reset(&q->asc);
    mc_swim_ops.reset(&q->swim);
}

static int board_framebuffer(void *board, mc_framebuffer *fb) {
    q605 *q = board;
    mc_dafb_view v;
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
    fb->serial = ++q->fb_serial;
    /* The LC 575's built-in display: the brightness the Screen driver
     * (.BCScreen in ROM resource DRVR 52) writes to PrimeTime +$501, from
     * PRAM $51 at start and from the brightness buttons ($AA4BC: steps of
     * 4 from $36 to $FF), 0 until it does; shown as that fraction of full
     * (†). */
    if (q->board->builtin_display && !q->card_slot) fb->level = (uint16_t)(q->prime.reg[5] & 0xFFu);
    return 1;
}

static int board_key(void *board, uint8_t code, int down) {
    q605 *q = board;
    /* The keyboard's power key ($7F7F) goes out on ADB as the other keys
     * and also pulls the ADB connector's PSW line low (Developer Note,
     * Table 1-7: "Power on signal"), Cuda's power-key input. */
    if (code == 0x7F) mc_adbctl_power_key(&q->cuda, down);
    if (!mc_adb_keyboard_key(&q->keyboard, code, down)) return 0;
    mc_adbctl_poke(&q->cuda);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    q605 *q = board;
    mc_adb_mouse_move(&q->mouse, dx, dy, button);
    mc_adbctl_poke(&q->cuda);
    return 1;
}

static void board_set_clock(void *board, int64_t host_seconds) {
    q605 *q = board;
    q->host_start = host_seconds;
    mc_adbctl_set_clock(&q->cuda, host_seconds);
}

static void board_destroy(void *board) {
    q605 *q = board;
    if (q->nvram[0]) mc_adbctl_nvram_save(&q->cuda, q->nvram, q->host_start);
    mc_swim_flush(&q->swim);
    for (int id = 0; id < 7; id++) {
        if (!q->vdisk[id]) continue;
        mc_scsi_bus_attach(&q->scsi_bus, id, NULL);
        mc_vdisk_destroy(q->vdisk[id]);
    }
    mc_scsi_bus_free(&q->scsi_bus);
    mc_dafb_free(&q->dafb);
    mc_livadix_card_free(&q->card);
    mc_enet_card_free(q->enet);
    free(q->cuda.mcu);
    free(q);
}

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void *build(mc_hw *hw) {
    q605 *q = calloc(1, sizeof *q);
    if (!q) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    q->hw = hw;
    q->board = board_of(hw->model);
    hw->board = q;
    uint32_t banks[MC_MEMCJR_BANKS];
    if (!mc_memcjr_layout(hw->config.ram_mb, banks)) {
        snprintf(hw->error, sizeof hw->error, "emulation.ram_mb: %u MiB does not fit the %s's banks",
                 (unsigned)hw->config.ram_mb, hw->model->name);
        free(q);
        return NULL;
    }
    const int io = mc_hw_region(hw, "io");
    /* I/O bus cycles take 0.5 us of guest time, as on the Quadra 700. */
    hw->bus.region[io].cost = 8;
    mc_via_init(&q->via1, &hw->host, "VIA1", 9);
    mc_via_init(&q->via2, &hw->host, "VIA2", 9);
    mc_bus_attach(&hw->bus, io, 0x00000, 0x2000, q, &via1_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x02000, 0x2000, q, &via2_ops, MC_WIN_PORT8);
    mc_memcjr_init(&q->memc, &hw->host, "MEMCjr", banks, memc_remap, q);
    mc_bus_attach(&hw->bus, io, 0x0E000, 0x2000, &q->memc, &mc_memcjr_ops, 0);
    mc_primetime_init(&q->prime, &hw->host, "PrimeTime", q->board->cpu_id);
    mc_bus_attach(&hw->bus, io, 0x18000, MC_PRIMETIME_SIZE, &q->prime, &mc_primetime_ops, 0);
    mc_bus_attach(&hw->bus, mc_hw_region(hw, "cpuid"), 0xFFFC, 4, &q->prime, &mc_primetime_cpu_id_ops, 0);
    mc_scc_init(&q->scc, &hw->host);
    mc_scc_set_clocks(&q->scc, MC_C15M_HZ / 2u, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    const mc_scc_port port = {q, scc_transmit, scc_outputs};
    mc_scc_set_port(&q->scc, &port);
    mc_bus_attach(&hw->bus, io, 0x0C000, 0x2000, &q->scc, &mc_scc_ops, MC_WIN_PORT8);
    mc_scsi_bus_init(&q->scsi_bus);
    mc_ncr53c96_init(&q->scsi, &hw->host, &q->scsi_bus, 25000000u);
    mc_bus_attach(&hw->bus, io, 0x10000, 0x0100, &q->scsi, &scsi_ops, MC_WIN_PORT8);
    mc_bus_attach(&hw->bus, io, 0x10100, 0x1F00, q, &pdma_ops, 0);
    mc_scsi_cd_init(&q->unit[CD_ID]);
    mc_scsi_unit_on_eject(&q->unit[CD_ID], cd_ejected, q);
    mc_scsi_bus_attach(&q->scsi_bus, CD_ID, mc_scsi_unit_target(&q->unit[CD_ID]));
    mc_asc_init(&q->asc, &hw->host, MC_ASC_ENHANCED);
    mc_bus_attach(&hw->bus, io, 0x14000, MC_ASC_SIZE, &q->asc, &mc_asc_ops, MC_WIN_PORT8);
    /* The LC 575's communications slot ("Communications Options"; the ROM
     * probes a card at $50F0 9400 and $50F0 9FFC after setting bit 1 of
     * PrimeTime's +$100, ROM offset $A746E): with no card the cycle ends
     * without a bus error and reads 0†, and the ROM finds no card. */
    if (q->board->builtin_display) {
        mc_absent_init(&q->comm, &hw->host, "Comm slot", NULL, 0x00);
        mc_bus_attach(&hw->bus, io, 0x09000, 0x1000, &q->comm, &mc_absent_ops, 0);
    }
    mc_swim2_init(&q->swim, &hw->host, hw->media.floppy_drives);
    mc_bus_attach(&hw->bus, io, 0x1E000, 0x2000, &q->swim, &mc_swim_ops, MC_WIN_PORT8);
    /* Built-in video in slot $9: VRAM from $F900 0000, the controls at
     * $F980 0000. The LC 575's built-in display is the 14" 640 x 480. */
    mc_dafb_monitor monitor = mc_dafb_monitor_for(hw->config.width, hw->config.height);
    if (q->board->builtin_display) monitor = MC_MONITOR_HIRES13;
    const int fits = q->board->builtin_display ? hw->config.width == 640 && hw->config.height == 480
                                               : monitor != MC_MONITOR_NONE;
    /* video.card builtin with a depth the built-in video lacks: its deepest
     * one at that size; a size it lacks: the card (no black screen). */
    const int over = hw->config.depth > video_max_depth(monitor);
    const int card = hw->config.video_card == MC_VIDEO_CARD_LIVADIX || !fits || video_max_depth(monitor) == 0 || (over && hw->config.video_card == MC_VIDEO_CARD_AUTO);
    if (!card && over) hw->config.depth = video_max_depth(monitor);
    if (card) monitor = MC_MONITOR_NONE;
    if (!mc_dafb_init(&q->dafb, &hw->host, "Video", VRAM_SIZE, monitor)) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        free(q);
        return NULL;
    }
    q->dafb.antelope = 1;
    mc_bus_map_float(&hw->bus, 0xF9000000u, 2u << 20);
    mc_bus_map_memory(&hw->bus, 0xF9000000u, VRAM_SIZE, q->dafb.vram, VRAM_SIZE, 1);
    const int video_io = mc_bus_add_io(&hw->bus, 0xF9800000u, 0x1000u, 0xFFFu, 0);
    mc_bus_attach(&hw->bus, video_io, 0, 0x400, q, &video_ops, 0);
    /* The expansion slot as slot $E, the built-in video as slot $9; their
     * interrupts on VIA2 PA5 and PA6. */
    mc_nubus_init(&q->nubus, &hw->bus, (uint8_t)(1u << (0x9 - 8) | 1u << (CARD_SLOT - 8)), slot_pins, q);
    mc_nubus_claim(&q->nubus, 0x9);
    mc_nubus_line(&q->nubus, CARD_SLOT, CARD_SLOT - 9);
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
    /* The Ethernet card (network.card). */
    q->enet = mc_enet_card_fit(hw, &q->nubus, MC_ENET_CARD_LC, mc_nubus_free_slot(&q->nubus));
    if (!q->enet && hw->error[0]) {
        mc_dafb_free(&q->dafb);
        mc_livadix_card_free(&q->card);
        free(q);
        return NULL;
    }
    /* ADB, the clock, PRAM and power: Cuda, with an Apple Extended
     * Keyboard and mouse. */
    mc_adb_bus_init(&q->adb);
    mc_adb_keyboard_init(&q->keyboard);
    mc_adb_mouse_init(&q->mouse);
    mc_adb_attach(&q->adb, &q->keyboard.dev);
    mc_adb_attach(&q->adb, &q->mouse.dev);
    mc_adbctl_init(&q->cuda, &hw->host, "Cuda", MC_ADBCTL_CUDA, &q->via1, &q->adb);
    mc_adbctl_set_part(&q->cuda, "341S0788");
    if (mc_hw_nvram_path(hw, q->nvram, sizeof q->nvram) == NULL) q->nvram[0] = 0;
    load_nvram(q);
    {
        const char *keys[2] = {mc_model_controller_firmware(hw->model), NULL};
        const int level = mc_adbctl_select(&q->cuda, (mc_adbctl_level)hw->config.controller, hw->config.roms, keys);
        if (level < 0) {
            snprintf(hw->error, sizeof hw->error, "emulation.controller: firmware: the Cuda firmware (%s) is not in the roms folders",
                     keys[0] ? keys[0] : "?");
            mc_dafb_free(&q->dafb);
            mc_livadix_card_free(&q->card);
            free(q);
            return NULL;
        }
    }
    /* The power key that starts the machine stays held through Cuda's
     * power-up sequence (its reset pulses, about 1.1 s; firmware 341S0788
     * at $1232) and is released 0.4 s after the processor starts: released
     * earlier, Cuda debounces the release for 10 ms ($1661) right after the
     * reset, while the ROM waits at most about 7 ms for its first answer
     * (ROM offset $A9FA0: 9,167 turns of a 7-cycle loop, MC68040 UM section
     * 10, at 25 MHz, each with a 0.5 us VIA read). */
    q->cuda.key_hold = (uint64_t)MC_C15M_HZ * 3u / 2u;
    uint8_t *pram = mc_adbctl_pram(&q->cuda);
    set_depth(q, pram);
    if (q->card_slot) set_card_depth(q, pram);
    if (hw->config.boot_priority == MC_BOOT_CDROM) {
        const uint16_t refnum = (uint16_t)-(32 + CD_ID + 1);
        pram[0x78] = pram[0x79] = 0;
        pram[0x7A] = (uint8_t)(refnum >> 8);
        pram[0x7B] = (uint8_t)refnum;
    }
    /* Pins other chips drive: VIA1 port A with vSCCWrReq (PA7) high,
     * unconnected inputs high; VIA2 CA2 (the 53C96's DREQ) low. */
    mc_via_set_pa(&q->via1, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via1, 0xF7u, 0xF7u);
    mc_via_set_pa(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_pb(&q->via2, 0xFFu, 0xFFu);
    mc_via_set_ca2(&q->via2, 0);
    q->overlay_region = mc_bus_add_io(&hw->bus, ROM_BASE, 0x10000000u, 0x0FFFFFFFu, 0);
    mc_bus_attach(&hw->bus, q->overlay_region, 0, 0x10000000u, q, &trap_ops, 0);
    overlay_on(q);
    mc_adbctl_ops.reset(&q->cuda);
    cuda_hold(q);
    return q;
}

static void board_net_host(void *board, const mc_net_host *host) { mc_enet_card_net_host(((q605 *)board)->enet, host); }
static int board_net_receive(void *board, const uint8_t *frame, size_t length) {
    return mc_enet_card_net_receive(((q605 *)board)->enet, frame, length);
}

/* The microphone: the DFAC II's input, recorded by the EASC (asc.c). */
static void board_audio_source(void *board, const mc_audio_source *source) {
    mc_asc_set_audio_source(&((q605 *)board)->asc, source);
}

static const mc_board_ops q605_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    board_net_host, board_net_receive, board_control,
    .audio_source = board_audio_source,
};

static const char *const q605_models[] = {"quadra-605", "lc-475", "performa-475", "performa-476", "lc-575",
                                          "performa-575", "performa-577", "performa-578", NULL};
static const uint32_t q605_roms[] = {0xFF7439EEu, 0};
/* RAM (Developer Note, Figure 1-4): 4 MB on the board and one SIMM. */
static const uint16_t q605_ram[] = {4, 5, 6, 8, 12, 20, 36, 0};

/* The configuration editor's view of the video choice build makes
 * (hw_machine.h, mc_hw_caps). */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    const q605_board *b = board_of(model);
    const mc_dafb_monitor monitor = b->builtin_display ? MC_MONITOR_HIRES13 : mc_dafb_monitor_for(w, h);
    const int fits = b->builtin_display ? w == 640 && h == 480 : monitor != MC_MONITOR_NONE;
    return fits ? MC_DEPTHS_UPTO(video_max_depth(monitor)) : 0;
}
static const mc_hw_caps q605_caps = {video_builtin, 1, MC_FEATURE_LC_SLOT, NULL};

/* The MC68LC040, no FPU (Developer Note, "Microprocessor"). */
const mc_hw_desc mc_quadra605_desc = {
    "quadra605", q605_models, M68K_68LC040, M68K_FPU_NONE, q605_roms, q605_ram,
    q605_map, sizeof q605_map / sizeof *q605_map, q605_irqs, sizeof q605_irqs / sizeof *q605_irqs,
    build, &q605_ops, MC_MMU_040, NULL, &q605_caps,
};
