/* Livadix — the Macintosh SE, SE FDHD and Classic boards.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed. (1990): chapter
 * 3, Figure 3-3 (the SE's address map: RAM from 0 through the 4 MB space it
 * repeats in; the 256 KB ROM at $40 0000, repeated through $4F FFFF; the
 * 5380 at $58 0000, read at even and written at odd addresses; the SCC read
 * at $90 0000 (A0 = 0) and written at $B0 0000 (A0 = 1); the IWM at
 * $D0 0000 (A0 = 1); the VIA at $E8 0000 (A0 = 0); /VPA from $E0 0000 up;
 * at power-up the ROM overlay: the ROM at 0, the RAM at $60 0000, until the
 * first access to $40 0000-$5F FFFF, chapter 3 "Macintosh SE computer"
 * start-up steps), Table 3-2 (the VIA and the 5380's IRQ at level 1 through
 * the BBU, the SCC at level 2, its /IPL1 taking /IPL0 off), the BBU (the
 * 5380's pseudo-DMA handshake: the BBU holds /DTACK until DRQ, so a byte
 * moves per access, as compact.c's window does); chapter 4, Table 4-3 (VIA
 * port A: PA7 vSCCWrReq, PA6 vPage2, PA5 vHeadSel, PA4 vDriveSel, PA3 vSync
 * (no alternate sound buffer), PA2-PA0 the volume) and Table 4-12 (port B:
 * PB7 vSndEnb, PB6 vH4, low enabling the 5380's IRQ, PB5/PB4 the ADB
 * transceiver's ST1/ST0, PB3 /ADB.INT, PB2-PB0 the clock chip); chapter 5,
 * "RAM access rate in the Macintosh SE computer" (one long-word video fetch
 * in each four RAM cycles of a visible line: compact.c's wait states with
 * four slots); chapter 8 (ADB, the transceiver of adb_via.h); chapter 9
 * (the SE's IWM with 800K drives, the SE FDHD's SWIM with SuperDrives).
 * RAM: the SE's and SE FDHD's four SIMM sockets hold 1, 2, 2.5 or 4 MB; the
 * Classic has 1 MB on its board and an expansion card for 2, 2.5 or 4 MB.
 *
 * Observed from the ROMs: ROMs B2E362A8 and B306E171
 * keep the 5380's base at $5F F000 and its pseudo-DMA and handshake address
 * at $5F F200 (ROM offset $4B4), the VIA's at $EF E1FE, the SCC's at
 * $9F FFF8 and $BF FFF9, the IWM's at $DF E1FF; the Sony driver raises PA4
 * before it enables the lower internal drive, lowers it for the upper one
 * (ROM B2E362A8 offset $35334), the external drive on the IWM's second
 * enable. ROM A49F9914 is the SE FDHD's code in its first 256 KB and a
 * System 6.0.3 ROM disk after it, which the ROM starts from when Command,
 * Option, X and O are held at start-up; its additions (ROM offset $3F770)
 * read VIA PA3 as an input and, low, set BoxFlag ($CB3) to 11, the Classic
 * (Gestalt 'mach' 17), and then leave PA4 alone in the Sony driver: the
 * Classic's board pulls PA3 low and has one internal drive.
 *
 * Livadix's own code. */
#include "compact.h"
#include "../adb.h"
#include "../adb_via.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAM_SPACE 0x400000u
#define ROM_BASE 0x400000u
#define OVERLAY_RAM 0x600000u

typedef struct se_board {
    compact_board base;         /* the model, its drives, the 5380 */
    uint8_t swim;               /* the SWIM (SE FDHD, Classic) or the IWM (SE) */
    uint8_t classic;            /* PA3 pulled low, one internal drive (no vDriveSel) */
} se_board;

static const se_board se_boards[] = {
    {{"mac-se", MC_DRIVE_800K, 0, 1}, 0, 0},
    {{"mac-se-fdhd", MC_DRIVE_SUPERDRIVE, 0, 1}, 1, 0},
    {{"mac-classic", MC_DRIVE_SUPERDRIVE, 0, 1}, 1, 1},
};

typedef struct se {
    compact c;                  /* first: compact.c's operations take the board as a compact */
    const se_board *board;
    mc_adb_bus adb;
    mc_adb_keyboard keyboard;
    mc_adb_mouse mouse;
    mc_adb_via adb_via;
    uint8_t scsi_irq;
    uint8_t pb;                 /* VIA port B as last driven */
} se;

/* The 68000's 24-bit address bus: each MiB of the 24-bit space is itself. */
static const uint32_t se_map24[16] = {
    0x000000u, 0x100000u, 0x200000u, 0x300000u, 0x400000u, 0x500000u, 0x600000u, 0x700000u,
    0x800000u, 0x900000u, 0xA00000u, 0xB00000u, 0xC00000u, 0xD00000u, 0xE00000u, 0xF00000u,
};

/* The chips' windows (Figure 3-3). The regions' names are compact.c's. */
static const mc_region se_regions[] = {
    {"scc_r", 0x800000u, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"scc_w", 0xA00000u, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"iwm", 0xC00000u, 0x200000u, 0x1FFFFFu, MC_REGION_IO},
    {"via", 0xE00000u, 0x100000u, 0xFFFFFu, MC_REGION_IO},
    {"scsi", 0x580000u, 0x80000u, 0x7FFFFu, MC_REGION_IO},
};

static const mc_irq_route se_irqs[] = {
    {"via", 1}, {"scc", 2},
};

/* --- the memory map --------------------------------------------------------------- */

static void map_ram(se *m) {
    mc_hw *hw = m->c.hw;
    if (m->c.overlay) {
        mc_bus_map_memory(&hw->bus, 0, RAM_SPACE, hw->rom, hw->rom_size, 0);
        mc_compact_map_ram(&m->c, OVERLAY_RAM, 0x200000u);
    } else {
        mc_compact_map_ram(&m->c, 0, RAM_SPACE);
        mc_bus_map_float(&hw->bus, OVERLAY_RAM, 0x200000u);
    }
    mc_hw_maps_changed(hw);
}

static void map_fixed(se *m) {
    mc_hw *hw = m->c.hw;
    /* The ROM repeats through $40 0000-$4F FFFF; $50 0000-$57 FFFF and
     * $F0 0000 up hold nothing. */
    mc_bus_map_memory(&hw->bus, ROM_BASE, 0x100000u, hw->rom, hw->rom_size, 0);
    mc_bus_map_float(&hw->bus, 0x500000u, 0x80000u);
    mc_bus_map_float(&hw->bus, 0xF00000u, 0x100000u);
}

/* The BBU leaves the overlay at the first access to $40 0000-$5F FFFF. The
 * wait-state hook sees every bus cycle's address before the access. */
static unsigned board_wait(void *board, uint32_t a, unsigned fc, unsigned kind, uint64_t cycle) {
    se *m = board;
    if (m->c.overlay && a >= ROM_BASE && a < OVERLAY_RAM && kind != 3) {
        m->c.overlay = 0;
        map_ram(m);
    }
    return mc_compact_wait(board, a, fc, kind, cycle);
}

/* --- wiring ------------------------------------------------------------------------ */

static void set_irqs(se *m) {
    mc_hw *hw = m->c.hw;
    const int level1 = m->c.via_irq || (m->scsi_irq && !(m->pb & 0x40u));
    mc_irq_set(&hw->irq, mc_hw_irq(hw, "via"), level1);
    mc_irq_set(&hw->irq, mc_hw_irq(hw, "scc"), m->c.scc_irq);
}

static void via_port_a(se *m, uint8_t pa) {
    mc_swim_set_headsel(&m->c.iwm, (pa & 0x20) != 0);
    if (!m->board->classic) mc_swim_set_upper(&m->c.iwm, !(pa & 0x10));
    /* PA3 is vSync here: the sound comes from the main buffer only. */
    mc_compactvideo_set_pa(&m->c.video, (uint8_t)(pa | 0x08u));
}

static void via_port_b(se *m, uint8_t pb) {
    const int data = (m->c.via.ddrb & 1u) ? (pb & 1u) : 1;
    m->pb = pb;
    mc_rtc_pins(&m->c.rtc, pb & 4u, pb & 2u, data);
    mc_compactvideo_set_pb7(&m->c.video, pb & 0x80);
    mc_adb_via_state(&m->adb_via, (pb >> 4) & 3u);
    set_irqs(m);
}

static uint8_t adb_shift(void *ctx, uint8_t in) { return mc_via_shift8(&((se *)ctx)->c.via, in); }

static void board_line(void *board, void *dev, int line, int state) {
    se *m = board;
    if (dev == &m->c.via) {
        if (line == MC_VIA_LINE_IRQ) { m->c.via_irq = (uint8_t)(state != 0); set_irqs(m); }
        else if (line == MC_VIA_LINE_PA) via_port_a(m, (uint8_t)state);
        else if (line == MC_VIA_LINE_PB) via_port_b(m, (uint8_t)state);
    } else if (dev == &m->c.scc) {
        if (line == MC_SCC_LINE_IRQ) { m->c.scc_irq = (uint8_t)(state != 0); set_irqs(m); }
        /* /W//REQ on PA7, low when asserted. */
        else if (line == MC_SCC_LINE_WREQ) mc_via_set_pa(&m->c.via, state ? 0 : 0x80u, 0x80u);
    } else if (dev == &m->c.rtc) {
        if (line == MC_RTC_LINE_DATA) mc_via_set_pb(&m->c.via, state ? 1u : 0u, 0x01u);
        else if (line == MC_RTC_LINE_SECOND) mc_via_set_ca2(&m->c.via, state);
    } else if (dev == &m->c.video) {
        /* Vertical blanking: CA1 low while blanking. */
        if (line == MC_COMPACTVIDEO_LINE_VBL) mc_via_set_ca1(&m->c.via, !state);
    } else if (dev == &m->c.scsi) {
        if (line == MC_NCR5380_LINE_IRQ) { m->scsi_irq = (uint8_t)(state != 0); set_irqs(m); }
    } else if (dev == &m->adb_via) {
        if (line == MC_ADB_VIA_LINE_INT) mc_via_set_pb(&m->c.via, state ? 0x08u : 0, 0x08u);
    }
}

/* --- board operations ------------------------------------------------------------- */

static void board_reset(void *board) {
    se *m = board;
    mc_via_ops.reset(&m->c.via);
    mc_ncr5380_reset(&m->c.scsi);
    mc_scc_ops.reset(&m->c.scc);
    mc_swim_ops.reset(&m->c.iwm);
    mc_compactvideo_ops.reset(&m->c.video);
    mc_adb_via_ops.reset(&m->adb_via);
    m->scsi_irq = 0;
    /* The VIA's ports are inputs again (pulled high); the BBU starts with
     * the overlay. */
    via_port_a(m, 0xFF);
    via_port_b(m, 0xFF);
    m->c.overlay = 1;
    map_ram(m);
    mc_hw_set_addr24(m->c.hw, 1);
    /* The RESET instruction restarts the processor too, as on the Plus
     * (compact.c; Apple's ShutDownMgr.a: "On the Mac Plus and Mac SE, a
     * RESET will cause the 68000 to get a Reset ... and Re-Boot through the
     * reset vector"). */
    if (m->c.hw->running) mc_hw_cpu_restart(m->c.hw);
}

static int board_key(void *board, uint8_t code, int down) {
    se *m = board;
    if (!mc_adb_keyboard_key(&m->keyboard, code, down)) return 0;
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

static int board_mouse(void *board, int dx, int dy, int button) {
    se *m = board;
    mc_adb_mouse_move(&m->mouse, dx, dy, button);
    mc_adb_via_poke(&m->adb_via);
    return 1;
}

/* compact.c's operations for the rest. */
static int board_framebuffer(void *board, mc_framebuffer *fb) { return mc_compact_ops.framebuffer(board, fb); }
static void board_set_clock(void *board, int64_t host_seconds) { mc_compact_ops.set_clock(board, host_seconds); }
static void board_destroy(void *board) { mc_compact_ops.destroy(board); }
static uint32_t board_audio_rate(void *board) { return mc_compact_ops.audio_rate(board); }
static size_t board_audio_pull(void *board, int16_t *stereo, size_t frames) { return mc_compact_ops.audio_pull(board, stereo, frames); }
static int board_insert(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity) {
    return mc_compact_ops.insert(board, kind, drive, disk, error, capacity);
}
static void board_poll(void *board) { mc_compact_ops.poll(board); }
static void board_serial_host(void *board, const mc_serial_host *host) { mc_compact_ops.serial_host(board, host); }
static size_t board_serial_receive(void *board, unsigned port, const uint8_t *data, size_t count) {
    return mc_compact_ops.serial_receive(board, port, data, count);
}
static void board_serial_modem(void *board, unsigned port, int dcd, int cts) { mc_compact_ops.serial_modem(board, port, dcd, cts); }
static int board_control(void *board, int control, int value) { return mc_compact_ops.control(board, control, value); }

const char *mc_hw_nvram_path(const mc_hw *hw, char *buffer, size_t capacity);

static void scsi_cd_ejected(void *ctx, mc_scsi_unit *unit, mc_vdisk *media) {
    compact *m = ctx;
    (void)unit;
    mc_vdisk_destroy(media);
    m->vdisk[MC_COMPACT_CD_ID] = NULL;
    mc_media_eject(&m->hw->media, MC_MEDIA_CDROM, 0);
}

static void *build(mc_hw *hw) {
    const se_board *b = NULL;
    for (size_t i = 0; i < sizeof se_boards / sizeof *se_boards; i++)
        if (!strcmp(se_boards[i].base.model, hw->model->key)) b = &se_boards[i];
    if (!b) {
        snprintf(hw->error, sizeof hw->error, "No SE board for %s", hw->model->name);
        return NULL;
    }
    se *m = calloc(1, sizeof *m);
    if (!m) {
        snprintf(hw->error, sizeof hw->error, "Out of memory");
        return NULL;
    }
    compact *c = &m->c;
    c->hw = hw;
    c->board = &b->base;
    m->board = b;
    hw->board = m;
    c->ram_size = mc_config_ram_kb(&hw->config) << 10;
    hw->ram_size = c->ram_size;
    c->video_slots = 4;
    hw->wait = board_wait;
    /* The chips on their byte lanes (compact.c). */
    mc_via_init(&c->via, &hw->host, "VIA", 9);
    mc_compact_attach(c, &c->via_port, "via", &c->via, &mc_via_ops, 0, &mc_compact_lane_ops);
    mc_scc_init(&c->scc, &hw->host);
    mc_scc_set_clocks(&c->scc, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ, MC_SCC_MAC_CLOCK_HZ);
    mc_compact_attach(c, &c->scc_read_port, "scc_r", &c->scc, &mc_scc_ops, 0, &mc_compact_lane_ops);
    mc_compact_attach(c, &c->scc_write_port, "scc_w", &c->scc, &mc_scc_ops, 1, &mc_compact_lane_ops);
    if (b->swim) mc_swim_init(&c->iwm, &hw->host, hw->media.floppy_drives);
    else mc_iwm_init(&c->iwm, &hw->host, hw->media.floppy_drives, MC_DRIVE_800K);
    mc_compact_attach(c, &c->iwm_port, "iwm", &c->iwm, &mc_swim_ops, 1, &mc_compact_lane_ops);
    mc_scsi_bus_init(&c->scsi_bus);
    mc_ncr5380_init(&c->scsi, &hw->host, &c->scsi_bus, 0);
    mc_compact_attach(c, &c->scsi_port, "scsi", c, &mc_compact_scsi_ops, -1, &mc_compact_lane_ops);
    mc_scsi_cd_init(&c->unit[MC_COMPACT_CD_ID]);
    mc_scsi_unit_on_eject(&c->unit[MC_COMPACT_CD_ID], scsi_cd_ejected, c);
    mc_scsi_bus_attach(&c->scsi_bus, MC_COMPACT_CD_ID, mc_scsi_unit_target(&c->unit[MC_COMPACT_CD_ID]));
    map_fixed(m);
    mc_compactvideo_init(&c->video, &hw->host, "Video", hw->ram, c->ram_size);
    /* ADB: the transceiver on the VIA, an Apple Extended Keyboard and a mouse. */
    mc_adb_bus_init(&m->adb);
    mc_adb_keyboard_init(&m->keyboard);
    mc_adb_mouse_init(&m->mouse);
    mc_adb_attach(&m->adb, &m->keyboard.dev);
    mc_adb_attach(&m->adb, &m->mouse.dev);
    mc_adb_via_init(&m->adb_via, &hw->host, "ADB", &m->adb, adb_shift, m);
    mc_rtc_init(&c->rtc, &hw->host, "RTC");
    if (mc_hw_nvram_path(hw, c->nvram, sizeof c->nvram) == NULL) c->nvram[0] = 0;
    mc_compact_load_nvram(c);
    /* Pins other chips drive: /W//REQ released, /ADB.INT, vH4 and the
     * clock's data high; the Classic's PA3 low (its ROM reads it as an
     * input and takes the machine for a Classic, BoxFlag 11). */
    mc_via_set_pa(&c->via, b->classic ? 0xF7u : 0xFFu, 0xFFu);
    mc_via_set_pb(&c->via, 0xFFu, 0xFFu);
    m->pb = 0xFF;
    c->overlay = 1;
    map_ram(m);
    mc_rtc_ops.reset(&c->rtc);
    mc_adb_via_ops.reset(&m->adb_via);
    return m;
}

static const mc_board_ops se_ops = {
    board_reset, board_line, board_framebuffer, board_key, board_mouse, board_set_clock, board_destroy,
    board_audio_rate, board_audio_pull, board_insert, board_poll,
    board_serial_host, board_serial_receive, board_serial_modem,
    NULL, NULL, board_control,
};

/* RAM: 1, 2, 2.5 or 4 MB (the SE's two 1 MB and two 256 KB SIMMs for
 * 2.5; the Classic's 1 MB on the board, its card's 1 MB and two SIMM
 * sockets: compact.c's two rows†). */
static const uint16_t se_ram[] = {1, 2, 4, 0};
static const uint32_t se_ram_kb[] = {2560, 0};

static const char *const se_models[] = {"mac-se", "mac-se-fdhd", "mac-classic", NULL};
static const uint32_t se_roms[] = {0xB2E362A8u, 0xB306E171u, 0xA49F9914u, 0};

/* The 512 x 342 screen in one bit; the SE's processor-direct slot takes no
 * card Livadix offers. */
static unsigned video_builtin(const mc_model *model, unsigned w, unsigned h) {
    (void)model;
    return w == MC_COMPACTVIDEO_WIDTH && h == MC_COMPACTVIDEO_HEIGHT ? MC_DEPTH_BIT(1) : 0;
}
static const mc_hw_caps se_caps = {video_builtin, 0, 0, NULL};

const mc_hw_desc mc_macse_desc = {
    "macse", se_models, M68K_68000, M68K_FPU_NONE, se_roms, se_ram,
    se_regions, sizeof se_regions / sizeof *se_regions, se_irqs, sizeof se_irqs / sizeof *se_irqs,
    build, &se_ops, MC_MMU_NONE, se_map24, &se_caps, se_ram_kb,
};
