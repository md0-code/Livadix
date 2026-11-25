/* Livadix — the PowerPC machine back end.
 *
 * A PowerPC Macintosh built from a description, the third back end of the
 * machine interface (src/core/machine.h) beside the stub and the 68k
 * hardware machine (src/hw/machine.c dispatches to it). It reuses the 68k
 * machines' parts where the hardware is the same: the physical bus of 4 KiB
 * pages (bus.h: RAM and ROM pages, device regions and windows), the
 * scheduler (sched.h), the chip interface (hw.h: mc_dev_ops, mc_hw_host,
 * guest time in C15M periods), the board operations and regions of
 * hw_machine.h, the media (media.h). What is PowerPC's own:
 *
 *  - the CPU (src/core/ppc) on the bus through numerical big-endian
 *    accesses of 1, 2 or 4 bytes (a doubleword is two words, ppc.h); an
 *    access no page or window answers is a transfer error (TEA): the CPU
 *    takes the machine check, or checkstops with MSR[ME] clear (MPC601UM
 *    5.4.2, 9.4.3.2);
 *  - how a device sits on the 64-bit data bus (ppc_bus.c, mc_ppc_attach):
 *    as the access comes, behind a bridge that sizes it into byte or
 *    halfword cycles, or on fixed byte lanes;
 *  - one external interrupt input: the board's interrupt controller (AMIC
 *    on the 6100, Grand Central on the first PCI machines) drives it with
 *    mc_ppc_set_interrupt;
 *  - guest time from the processor clock, and the time base and
 *    decrementer (the 601's RTC and DEC) counted from guest time.
 *
 * Livadix's own code. */
#ifndef MC_PPC_MACHINE_H
#define MC_PPC_MACHINE_H

#include "../hw_machine.h"
#include "ppc.h"
#include "ppc_jit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mc_ppc mc_ppc;

/* What an access no page or window answers does. */
enum {
    MC_PPC_UNMAPPED_TEA,    /* a transfer error: the machine check (the hardware's behaviour) */
    MC_PPC_UNMAPPED_STOP    /* the same, and the run stops with the access reported (bring-up) */
};

typedef struct mc_ppc_desc {
    const char *key;                    /* "pdm" */
    const char *const *models;          /* catalogue models it serves, NULL-terminated (none: NULL) */
    const char *name;                   /* shown when no model names it */
    uint8_t cpu;                        /* ppc_model */
    uint32_t cpu_hz;                    /* the processor clock when no model gives one */
    /* Time base input: the 601's RTC oscillator (7.8125 MHz, MPC601UM
     * 2.2.5.3), on the later processors a quarter of the bus clock. */
    uint32_t timebase_hz;
    /* Processor clocks per instruction: the interpreter's flat model,
     * cpi / cpi_div clocks (cpi_div 0 is 1). The 601 folds branches out of
     * the instruction stream at no cost (MPC601UM 7.3.1.1), which its
     * boards give as 8/9: the nanokernel times eight addi and a bdnz as
     * eight clocks (ROM $FFF03650). */
    uint32_t cpi;
    uint32_t cpi_div;
    const uint32_t *roms;               /* ROM checksums the board runs, 0-terminated */
    const uint16_t *ram_mb;             /* legal RAM sizes in MiB, 0-terminated */
    /* The physical map (hw_machine.h's regions): RAM is mapped at its
     * region's base, installed size, before build (the board's memory
     * controller may map it again); ROM repeats through each ROM region;
     * IO regions take device windows (mc_ppc_attach); EMPTY stays
     * unmapped. */
    const mc_region *map;
    size_t nmap;
    uint8_t unmapped;                   /* MC_PPC_UNMAPPED_* */
    /* Creates the board's chips, attaches and wires them; NULL with
     * m->error on failure. NULL: a board of RAM and ROM only. */
    void *(*build)(mc_ppc *m);
    const mc_board_ops *ops;            /* NULL: none */
    const mc_hw_caps *caps;             /* NULL: no built-in video, no card, no features */
} mc_ppc_desc;

/* A device presented through a port the bus windows cannot express
 * alone (ppc_bus.c). */
typedef struct mc_ppc_adapter {
    void *dev;
    const mc_dev_ops *ops;
    uint8_t kind, width, lane, fill;
} mc_ppc_adapter;
#define MC_PPC_ADAPTERS 64

/* The first access no page or window answered. */
typedef struct mc_ppc_probe {
    int hit;
    uint32_t address, value, pc;
    unsigned size;
    int write, fetch;
} mc_ppc_probe;

struct mc_ppc {
    const mc_ppc_desc *desc;
    const mc_model *model;              /* NULL: a description run without a catalogue model */
    mc_config config;
    struct mc_nvram *nvram_session;
    ppc_cpu cpu;
    mc_bus bus;
    mc_sched sched;
    mc_hw_host host;
    mc_media media;
    int region[32];                     /* bus region of each map entry (0: none) */
    uint8_t *rom, *ram;
    uint32_t rom_size, ram_size;
    void *board;
    /* Guest time (C15M periods) from processor clocks at `rate` Hz:
     * now = time + ((cycles - cyc) x C15M + rem) / rate. */
    uint64_t rate, cycles, cyc, time, rem;
    uint32_t div;                       /* the description's cpi_div: clocks count in 1/div */
    uint64_t slice_end;
    int running;
    /* Time base ticks delivered to the CPU, and the processor clock count
     * at which the next one is due. */
    uint64_t tb_ticks, tb_due;
    int interrupt;                      /* the external interrupt input */
    int reset_held;                     /* board RESET: clocks/devices run, instructions do not */
    int stop;                           /* end the run after this instruction */
    uint32_t poll_pc, poll_address, poll_value;
    unsigned poll_size;
    int poll_disabled;                /* diagnostic: same as JIT idle skipping */
    uint64_t poll_steps;
    int startup_sound_done;            /* first sound drained since board reset */
    int powered_off;
    int failed;                         /* stopped for good: checkstop, unknown opcode, reported probe */
    mc_ppc_probe probe;
    mc_ppc_adapter adapter[MC_PPC_ADAPTERS];
    ppc_jit *jit;                       /* the translator (emulation.jit); NULL: the interpreter alone */
    struct mc_m68k_accel *accel;        /* the 68k accelerator (m68k_accel.h); NULL: Apple's emulator alone */
    uint64_t jit_start;                 /* the clock count its current run started at */
    unsigned nadapter;
    char description[64];
    char error[256];
};

/* The descriptions (machines/), and the one serving a catalogue model. */
extern const mc_ppc_desc mc_pdm_desc;
/* Native PBX boards, with emulator limitations in docs/NOTES.md. */
extern const mc_ppc_desc mc_pbx2300_desc, mc_pbx1400c_desc, mc_pbx1400cs_desc;
/* Standalone G3/Grackle ROM fixture, no catalogue model enabled. */
extern const mc_ppc_desc mc_g3_desc;
extern const mc_ppc_desc mc_wallstreet_desc, mc_pdq_desc, mc_pippin_desc;
extern const mc_ppc_desc mc_psx5400_desc, mc_psx6400_desc;
extern const mc_ppc_desc mc_psx5500_desc, mc_psx6500_desc, mc_psx_tam_desc;
extern const mc_ppc_desc mc_pdm6100_desc, mc_pdm7100_desc, mc_pdm8100_desc;
extern const mc_ppc_desc mc_pdm6150_desc, mc_pdm8150_desc, mc_pdm7100_80_desc;
extern const mc_ppc_desc mc_pdm8100_100_desc, mc_pdm8100_110_desc, mc_pdm9150_80_desc, mc_pdm9150_120_desc;
extern const mc_ppc_desc mc_pci7200_desc, mc_pci7500_desc, mc_pci7600_desc, mc_pci8500_desc, mc_pci8600_desc;
extern const mc_ppc_desc mc_pci9500_desc, mc_pci9600_desc, mc_pci7300_desc;
extern const mc_ppc_desc mc_capella5200_desc, mc_capella5300_desc, mc_capella6200_desc, mc_capella6300_desc;
const mc_ppc_desc *mc_ppc_find(const char *model_key);
/* As mc_ppc_find, and also boards still in bring-up or without a display
 * card, for the tools. This does not mark a model emulated. */
const mc_ppc_desc *mc_ppc_find_board(const char *model_key);

/* A machine from a description. The configuration gives RAM, clock and
 * media; its model, when the catalogue has the description's, gives the
 * name. NULL with the message in `error`. */
mc_ppc *mc_ppc_create(const mc_ppc_desc *desc, const mc_config *config, const uint8_t *rom, size_t rom_size, char *error, size_t capacity);
void mc_ppc_destroy(mc_ppc *m);
/* Runs about `cycles` processor clocks, stopping early at guest time
 * `deadline` (0: none), as mc_machine_run. */
mc_run_status mc_ppc_run(mc_ppc *m, uint64_t cycles, uint64_t deadline);
/* The 68k accelerator on or off; 0 when it cannot be had (no translator). */
int mc_ppc_m68k_accel(mc_ppc *m, int on);
/* For the accelerator, inside its run, after an instruction that reached a
 * device, `steps` steps into the run: what the run loop does after such a
 * step (the bus time, the time base), then the steps the run may still
 * make from there; 0 when it must end (an interrupt or deadline is due,
 * the machine stopped). */
uint64_t mc_ppc_accel_resume(mc_ppc *m, uint64_t steps);
uint64_t mc_ppc_now(mc_ppc *m);

/* --- for boards and chips ----------------------------------------------------------- */

/* How a device sits on the 64-bit data bus (MPC601UM 9.3.2.3, table 9-2:
 * byte lane n carries the byte at address n modulo 8, big-endian). */
typedef enum mc_ppc_port_kind {
    MC_PPC_PORT_DIRECT,     /* the device takes each access as made: 1, 2 or 4 bytes at its offset */
    MC_PPC_PORT_SIZED,      /* a bridge splits a wider access into `width`-byte cycles in address order */
    MC_PPC_PORT_LANES       /* the device drives lanes [lane, lane + width) of each doubleword only */
} mc_ppc_port_kind;

typedef struct mc_ppc_port {
    uint8_t kind;           /* mc_ppc_port_kind */
    uint8_t width;          /* the device's port in bytes: 1, 2 or 4 (SIZED, LANES) */
    uint8_t lane;           /* LANES: its first byte lane, 0-7, a multiple of width */
    uint8_t fill;           /* LANES: what a lane no device drives reads */
} mc_ppc_port;

/* Attaches a chip at `offset` (a multiple of 8) of bus region `region`
 * (mc_ppc_region: an IO map entry), `size` bytes, presented through `port`.
 * 0 when the region is full or the port is malformed. */
int mc_ppc_attach(mc_ppc *m, int region, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops, mc_ppc_port port);
/* The bus region of the map entry named `name`, 0 when none. */
int mc_ppc_region(const mc_ppc *m, const char *name);
/* The processor's external interrupt input INT (MPC601UM 9.7.1: a level,
 * held until the handler clears its source; the board's interrupt
 * controller drives it). */
void mc_ppc_set_interrupt(mc_ppc *m, int asserted);
/* A chip as bus master (mc_hw_host.dma): RAM and ROM pages in blocks,
 * anything else a byte at a time; 0 on a transfer error. DBDMA's 64-bit
 * transfers go through here. */
int mc_ppc_dma(mc_ppc *m, uint32_t address, void *data, uint32_t length, int write);
/* Ends the run after the current instruction (power off, a board's stop). */
void mc_ppc_power_off(mc_ppc *m);

/* --- the machine interface's third back end (src/hw/machine.c) ---------------------- */

const char *mc_ppc_description(const mc_ppc *m);
const char *mc_ppc_error(const mc_ppc *m);
uint64_t mc_ppc_cycles(const mc_ppc *m);
uint64_t mc_ppc_instructions(const mc_ppc *m);
int mc_ppc_peek(mc_ppc *m, uint32_t address, unsigned size, uint32_t *value);
/* The physical address of a logical one under the current translation, as
 * a supervisor read, without touching the processor or the page table. */
int mc_ppc_physical(mc_ppc *m, uint32_t address, uint32_t *physical);
int mc_ppc_poke(mc_ppc *m, uint32_t address, unsigned size, uint32_t value);
size_t mc_ppc_ram_sizes(const mc_ppc_desc *desc, uint32_t *kb, size_t max);
void mc_ppc_set_clock(mc_ppc *m, int64_t host_seconds);
int mc_ppc_framebuffer(mc_ppc *m, mc_framebuffer *fb);
/* The Livadix Display Card's blitter operations so far, 0 without the card. */
uint32_t mc_ppc_card_blits(mc_ppc *m);
int mc_ppc_key(mc_ppc *m, uint8_t code, int down);
int mc_ppc_mouse(mc_ppc *m, int dx, int dy, int button);
/* Silence only the first hardware sound, while it still runs in the guest. */
void mc_ppc_audio_startup(mc_ppc *, int16_t *, size_t, int played, int drained);
uint32_t mc_ppc_audio_rate(const mc_ppc *m);
size_t mc_ppc_audio_pull(mc_ppc *m, int16_t *stereo, size_t frames);
int mc_ppc_set_audio_source(mc_ppc *m, const mc_audio_source *source);
void mc_ppc_set_serial(mc_ppc *m, const mc_serial_host *host);
size_t mc_ppc_serial_receive(mc_ppc *m, unsigned port, const uint8_t *data, size_t count);
void mc_ppc_serial_modem(mc_ppc *m, unsigned port, int dcd, int cts);
void mc_ppc_set_net(mc_ppc *m, const mc_net_host *host);
int mc_ppc_net_receive(mc_ppc *m, const uint8_t *frame, size_t length);
int mc_ppc_insert(mc_ppc *m, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity);
int mc_ppc_reinsert(mc_ppc *m);
int mc_ppc_control(mc_ppc *m, mc_control control, int value);

#endif
