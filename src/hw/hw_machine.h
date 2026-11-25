/* Livadix — machines built from a description.
 *
 * A machine description (src/hw/machines/) names the board: CPU and FPU,
 * the ROMs it runs, the legal RAM sizes, its physical map (RAM, ROM, I/O
 * block, slot spaces) and its interrupt sources. machine.c builds the
 * common parts from it (CPU, bus, scheduler, interrupt router, guest time)
 * and the board's `build` function creates its chips, attaches them to the
 * regions the map declared and wires their pins. machine.c implements the
 * machine interface (src/core/machine.h) on top, and serves every model
 * without a description with the stub machine. */
#ifndef MC_HW_MACHINE_H
#define MC_HW_MACHINE_H

#include "../core/config.h"
#include "../core/machine.h"
#include "../core/media.h"
#include "../core/cpu/m68k.h"
#include "amu.h"
#include "bus.h"
#include "hw.h"
#include "irq.h"
#include "sched.h"

#include <stddef.h>
#include <stdint.h>

/* Physical regions of the map. */
typedef enum mc_region_kind {
    MC_REGION_RAM,      /* the RAM banks (the board maps them; `size` is the window) */
    MC_REGION_ROM,      /* the ROM, repeated through the window */
    MC_REGION_IO,       /* a device region: chips attach at offsets; repeats with `mask` */
    MC_REGION_EMPTY     /* nothing answers: bus error (empty NuBus slots) */
} mc_region_kind;

typedef struct mc_region {
    const char *name;
    uint32_t base;
    uint64_t size;
    uint32_t mask;          /* MC_REGION_IO: device offset = (address - base) & mask */
    uint8_t kind;
} mc_region;

/* Interrupt sources and their CPU levels. */
typedef struct mc_irq_route {
    const char *source;
    uint8_t level;
} mc_irq_route;

/* The memory management beside the CPU: none, the
 * Macintosh II's AMU (a 24-bit map, amu.h), an MC68851 in the 68020's
 * socket, or the 68030's and 68040's own. */
enum { MC_MMU_NONE, MC_MMU_AMU, MC_MMU_68851, MC_MMU_030, MC_MMU_040 };

typedef struct mc_hw mc_hw;

typedef struct mc_board_ops {
    void (*reset)(void *board);                         /* power-on and the RESET instruction */
    void (*line)(void *board, void *dev, int line, int state);
    int (*framebuffer)(void *board, mc_framebuffer *fb);
    int (*key)(void *board, uint8_t code, int down);
    int (*mouse)(void *board, int dx, int dy, int button);
    void (*set_clock)(void *board, int64_t host_seconds);
    void (*destroy)(void *board);                       /* also saves what persists */
    /* Sound out (machine.h): the rate in Hz (0: none) and the frames made. */
    uint32_t (*audio_rate)(void *board);
    size_t (*audio_pull)(void *board, int16_t *stereo, size_t frames);
    /* An image went into a drive (hw->media already holds it): the board
     * hands it to its chip. 0 with a message when the chip refuses it. */
    int (*insert)(void *board, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity);
    /* After every run: media the guest ejected go back to hw->media. */
    void (*poll)(void *board);
    /* Serial ports (machine.h). */
    void (*serial_host)(void *board, const mc_serial_host *host);
    size_t (*serial_receive)(void *board, unsigned port, const uint8_t *data, size_t count);
    void (*serial_modem)(void *board, unsigned port, int dcd, int cts);
    /* Ethernet (machine.h). */
    void (*net_host)(void *board, const mc_net_host *host);
    int (*net_receive)(void *board, const uint8_t *frame, size_t length);
    /* The machine's controls (machine.h, mc_machine_control); -1 none. */
    int (*control)(void *board, int control, int value);
    /* The Livadix Display Card's blitter operations so far (its BLT_COUNT,
     * livadix_card.h; the tools' count), 0 without the card. NULL: none. */
    uint32_t (*card_blits)(void *board);
    void (*audio_source)(void *board, const mc_audio_source *source);
    /* Optional absolute pointing device, ready after guest enumeration. */
    int (*absolute_ready)(void *board);
    int (*absolute_mouse)(void *board, int x, int y, int button);
} mc_board_ops;

/* What a board offers the configuration editor (machine.h,
 * mc_machine_video and mc_machine_features): the depths its built-in video
 * drives at a screen size (the same choice its build makes), whether the
 * Livadix Display Card fits in a slot, and its features. */
typedef struct mc_hw_caps {
    /* MC_DEPTH_BIT mask of the depths the built-in video shows at width x
     * height (0: none); NULL: no built-in video. */
    unsigned (*builtin)(const mc_model *model, unsigned width, unsigned height);
    uint8_t card;                       /* the Livadix Display Card fits (a NuBus, processor-direct or PCI slot) */
    uint16_t features;                   /* MC_FEATURE_* (machine.h) */
    unsigned (*features_of)(const mc_model *model); /* when the models of a board differ; NULL: features */
} mc_hw_caps;

typedef struct mc_hw_desc {
    const char *key;                    /* "quadra700" */
    const char *const *models;        /* the models it serves, NULL-terminated */
    uint8_t cpu, fpu;                   /* m68k_model, m68k_fpu_type */
    const uint32_t *roms;               /* ROM checksums this board runs, 0-terminated */
    const uint16_t *ram_mb;             /* legal RAM sizes in MiB, 0-terminated */
    const mc_region *map;
    size_t nmap;
    const mc_irq_route *irqs;
    size_t nirq;
    /* Creates the board: its chips, their windows and wiring. Returns the
     * board state (NULL on failure, with the error in hw->error). */
    void *(*build)(mc_hw *hw);
    const mc_board_ops *ops;
    /* MC_MMU_*; with MC_MMU_68851 the CPU gets the PMMU (mmu851.c). */
    uint8_t mmu;
    /* The 24-bit decode of a 68020 board without a PMMU (amu.h): the
     * 32-bit base of each MiB of the 24-bit space; NULL: none. The board
     * switches modes with mc_hw_set_addr24 from the pin or register that
     * selects them (Macintosh II: VIA2 PB3, low for 24-bit; LC: the V8's
     * bit, 5.2e). */
    const uint32_t *map24;
    /* What the board offers the configuration editor; NULL: any mode on
     * the built-in video, no card, no features. */
    const mc_hw_caps *caps;
    /* Legal RAM sizes that are not a whole number of MiB, in KiB,
     * 0-terminated (the Plus's and SE's 2.5 MB); NULL: none. */
    const uint32_t *ram_kb;
} mc_hw_desc;

/* The machine as the board sees it. */
struct mc_hw {
    const mc_hw_desc *desc;
    const mc_model *model;
    mc_config config;
    m68k_cpu *cpu;
    mc_bus bus;
    mc_sched sched;
    mc_irq irq;
    mc_hw_host host;
    int region[32];                     /* bus region of each map entry */
    int irq_source[32];                 /* irq source of each route */
    uint8_t *rom, *ram;
    uint32_t rom_size, ram_size;
    mc_amu amu;                         /* the 24-bit decode (desc->map24) */
    void *board;
    /* Guest time: `time` C15M periods at `cyc` CPU cycles, plus `rem`/rate
     * (rate: the clock, emulation.cpu_mhz in Hz). */
    uint64_t rate, time, cyc, rem;
    /* mc_hw_now's last answer and what it was computed from (devices ask
     * many times per instruction; the division is the costly part). */
    uint64_t now_count, now_time, now_value;
    uint64_t rate_inv;                  /* floor((2^64 - 1) / rate): division by rate as a multiplication */
    uint64_t idle;                      /* cycles spent in STOP */
    /* A DBcc to itself above the model's clock (machine.c, cpu_dbra): the
     * time each of its cycles takes beyond one at cpu_hz, dbra_num /
     * dbra_den C15M periods, and the fraction carried. */
    uint64_t dbra_num, dbra_den, dbra_acc;
    int running;
    int powered_off;                    /* mc_hw_power_off: the run ends, every later run returns MC_RUN_HALTED */
    int reset_held;                     /* mc_hw_hold_reset: the processor waits in reset */
    int reset_pending;                  /* released: the processor and the board restart */
    int cpu_reset_pending;              /* mc_hw_cpu_restart: the processor alone restarts */
    /* The programmer's switch (machine.h, MC_CONTROL_INTERRUPT and _RESET):
     * the reset switch held (mc_hw_reset_switch) and the interrupt switch's
     * irq source where the board leaves it to the machine (the route named
     * "switch", else a level-7 source of its own: the NMI). */
    int switch_reset;
    int switch_irq;
    uint64_t slice_end;                 /* cycle count where the current slice ends */
    /* The 68000's wait states (m68k_bus.wait), set by a board whose memory
     * cycles others share (machines/compact.c); NULL: none. */
    unsigned (*wait)(void *board, uint32_t address, unsigned fc, unsigned kind, uint64_t cycle);
    mc_media media;
    char description[192], error[256];
};

/* The region of map entry `name` (0 when absent). */
int mc_hw_region(const mc_hw *hw, const char *name);
/* The irq source of route `name` (-1 when absent). */
int mc_hw_irq(const mc_hw *hw, const char *name);
/* Current guest time. */
uint64_t mc_hw_now(mc_hw *hw);
/* The page tables changed (a remap): the CPU drops its translations. */
void mc_hw_maps_changed(mc_hw *hw);
/* 24-bit mode on or off on a board with a 24-bit decode (desc->map24):
 * the CPU gets the tables of the new mode and drops its translations (a
 * device write may call it in the middle of an instruction). */
void mc_hw_set_addr24(mc_hw *hw, int on);

/* The board's power went off (its power-off line or its power controller's
 * command): the current instruction is the last. */
void mc_hw_power_off(mc_hw *hw);

/* The board's power controller holds the processor's /RESET (held 1) or
 * releases it (0): while held the processor runs no instruction and guest
 * time passes to the next deadline; on release the board's reset and the
 * processor's run (its reset vectors), after the current instruction. */
void mc_hw_hold_reset(mc_hw *hw, int held);

/* The programmer's reset switch on a board whose /RESET it drives (the
 * compact Macs): pressed (1), the processor waits in reset; released, the
 * board's reset and the processor's run, the RAM kept. */
void mc_hw_reset_switch(mc_hw *hw, int down);

/* The processor alone restarts through its reset vectors (the board's
 * devices keep their state), after the current instruction: a PowerBook's
 * power cycling (machines/powerbook.c). */
void mc_hw_cpu_restart(mc_hw *hw);

/* The built machine behind the interface (NULL for the stub): for tests
 * and traces. */
mc_hw *mc_machine_hw(mc_machine *m);

/* The descriptions. */
extern const mc_hw_desc mc_quadra700_desc;
extern const mc_hw_desc mc_quadra900_desc, mc_quadra950_desc;    /* machines/quadra900.c */
extern const mc_hw_desc mc_iifx_desc;                              /* machines/iifx.c */
extern const mc_hw_desc mc_quadra800_desc, mc_centris610_desc;    /* machines/quadra800.c */
extern const mc_hw_desc mc_quadra630_desc, mc_lc630_desc;         /* machines/quadra630.c */
extern const mc_hw_desc mc_iici_desc;
extern const mc_hw_desc mc_iisi_desc;                              /* machines/iisi.c */
extern const mc_hw_desc mc_lc_desc, mc_lc2_desc, mc_classic2_desc, mc_color_classic_desc, mc_tv_desc; /* machines/lc.c */
extern const mc_hw_desc mc_quadra605_desc;                          /* machines/quadra605.c */
extern const mc_hw_desc mc_lc3_desc, mc_cc2_desc;                   /* machines/lc3.c */
extern const mc_hw_desc mc_iivx_desc, mc_iivi_desc;                 /* machines/iivx.c */
extern const mc_hw_desc mc_macii_desc, mc_macii_fdhd_desc, mc_iix_desc; /* machines/macii.c */
extern const mc_hw_desc mc_quadra840av_desc, mc_quadra660av_desc;   /* machines/quadra840av.c */
extern const mc_hw_desc mc_pb140_desc, mc_pb170_desc, mc_pb160_desc, mc_pb180_desc, mc_duo210_desc, mc_duo270_desc,
    mc_duo280_desc, mc_pb150_desc;                                  /* machines/powerbook.c */
extern const mc_hw_desc mc_pb520_desc, mc_pb190_desc;               /* machines/powerbook500.c */
extern const mc_hw_desc mc_mac128k_desc, mc_mac512ke_desc, mc_macplus_desc; /* machines/compact.c */
extern const mc_hw_desc mc_portable_desc, mc_pb100_desc;            /* machines/portable.c */
extern const mc_hw_desc mc_macse_desc; /* machines/se.c */
const mc_hw_desc *mc_hw_find(const char *model_key);

/* A machine's PRAM file: emulation.nvram, else in the folder nvram beside the
 * configuration, its name with .nvram (a profile's is set by config.c). The
 * path is computed only; the owned store creates directories. Legacy files
 * are never moved or imported implicitly. NULL for Built-in/no file. */
const char *mc_config_nvram_path(const mc_config *c, char *buffer, size_t capacity);

#endif
