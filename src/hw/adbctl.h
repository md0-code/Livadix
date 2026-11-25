/* Livadix — Egret and Cuda, the microcontrollers that run ADB, the clock,
 * parameter RAM, power and reset on the IIsi, the LC family, Classic II,
 * LC III, IIvx/IIvi, Quadra 900/950 (Egret) and Color Classic, the LC 520
 * family and TV, LC 475/575/605, 630/580 and the AV Quadras (Cuda).
 *
 * Two levels share this interface:
 *   - protocol level (default): the packets, commands and timing modelled
 *     here (adbctl.c, with the link layers in egret.c and cuda.c);
 *   - firmware level: Apple's firmware (roms/egret-*.bin, cuda-*.bin)
 *     on the 68HC05 core (adbctl_mcu.c), its ports wired to the same VIA
 *     lines, the ADB line (adb_wire.c), the power and reset lines.
 *
 * Sources: Apple's Developer Notes (Macintosh IIsi: "ADB microcontroller",
 * "Real-time clock and parameter RAM", "Power control", "Keyboard reset and
 * NMI"; Quadra 840AV: "Cuda", Table 2-2 CudaClk 32.768 kHz), the Guide to
 * the Macintosh Family Hardware, 2nd ed., chapter 8 (ADB), and, as the
 * binding evidence, what Apple's firmware does and what the Mac ROMs send
 * and expect.
 *
 * Packets (both): the host sends [type, command, data...]; type 0 is an
 * ADB command, 1 a pseudo-command (the controller's own), others are
 * refused. Replies and unsolicited packets go to the host as [type, flags,
 * command, data...]: type 0 for ADB (flags: $40 autopoll, $02 timeout, $01
 * Service Request), 1 for pseudo-commands (flags 0), 2 for errors
 * ([2, error, type, command]: 1 bad type, 2 bad command, 3 bad length,
 * 4 bad PRAM address, 5 IIC failure), 3 the one-second tick (Cuda's one
 * byte), 4 Cuda's power key.
 *
 * Wiring a machine (see egret.h and cuda.h for the VIA pins of each):
 *   - mc_adbctl_init with VIA1 and the ADB bus (devices attached as for
 *     adb_via.c); the controller drives VIA1 PB3 and clocks CB1/CB2 itself;
 *   - mc_adbctl_via_pb from the machine's VIA1 MC_VIA_LINE_PB report;
 *   - mc_adbctl_sync before every access of the CPU to VIA1 (the firmware
 *     level runs in slices; this brings it up to the CPU's time first, so
 *     the CPU sees the shift register and lines as the firmware left them);
 *   - mc_adbctl_set_part with the machine's part ("341S0789"...), and
 *     the strap field for Egret's model strap (port A bit 5)†;
 *   - mc_adbctl_poke when an ADB device gets new data (input);
 *   - the lines MC_ADBCTL_LINE_RESET (1 = the 68k's /RESET held), _POWER
 *     (1 = the system powered), _NMI (1 = /NMI asserted);
 *   - the parameter RAM and clock (mc_adbctl_pram, mc_adbctl_seconds) for
 *     emulation.nvram, with mc_adbctl_nvram_load/save. */
#ifndef MC_ADBCTL_H
#define MC_ADBCTL_H

#include "adb.h"
#include "adb_wire.h"
#include "adbctl_mcu.h"
#include "hw.h"
#include "via.h"

#include <stddef.h>
#include <stdint.h>

typedef enum { MC_ADBCTL_EGRET, MC_ADBCTL_CUDA } mc_adbctl_kind;

enum { MC_ADBCTL_LINE_RESET, MC_ADBCTL_LINE_POWER, MC_ADBCTL_LINE_NMI };

/* The levels a controller can run at (emulation.controller). */
typedef enum { MC_ADBCTL_AUTO, MC_ADBCTL_PROTOCOL, MC_ADBCTL_FIRMWARE } mc_adbctl_level;

#define MC_ADBCTL_MAX_PACKET 40

typedef struct mc_adbctl {
    mc_dev dev;
    mc_adbctl_kind kind;
    mc_via *via;
    mc_adb_bus *adb;
    uint8_t firmware;           /* running Apple's firmware */
    uint8_t via_pb;             /* VIA1 port B as the VIA drives it */
    uint8_t xcvr;               /* level driven on VIA1 PB3 */
    uint8_t line_state[3];
    uint8_t power_key;          /* the power key held (1) */
    uint8_t power_boot;         /* firmware: the key held for the cold start, until the power comes on */
    uint8_t strap;              /* Egret's model strap (port A bit 5)† */
    /* A device on Cuda's IIC bus that acknowledges what is written to it
     * (the Color Classic's DFAC2): pseudo-command $22 succeeds at the
     * protocol level (cuda.c) and the firmware's transfers are acknowledged
     * (adbctl.c, iic_master); 0: no device, $22 fails. */
    uint8_t iic_device;
    /* Optional physical IIC-device bus. Protocol transactions are delivered
     * with the bytes Cuda would clock; the board supplies actual devices.
     * ROM 78F57389 FFF05484-FFF055C0: $22 and $25 combined random read. */
    void *iic_ctx;
    int (*iic_transfer)(void *,uint8_t,const uint8_t *,unsigned,uint8_t *,unsigned *);

    uint8_t iic_scl, iic_sda, iic_ack, iic_bits, iic_active;
    uint8_t dirty;              /* PRAM or clock written since mc_adbctl_clean */
    /* --- protocol level --------------------------------------------------- */
    /* The controller's state at its firmware addresses ($90-$1FF: flags,
     * clock $AB-$AE, alarm $AF-$B2, parameter RAM $100-$1FF), so reads and
     * writes of its address space (pseudo-commands $02, $08) see it. */
    uint8_t mem[0x200];
    uint32_t seconds;           /* the clock: `seconds` at guest time `base` */
    uint64_t base;
    uint64_t next_tick;         /* the next one-second mark */
    uint64_t next_poll;         /* the next autopoll */
    uint64_t next_rti;          /* Cuda's power-key timeout steps */
    uint8_t keys;               /* Control, Command, power, Option held */
    /* Link layer (egret.c, cuda.c). */
    uint8_t link;               /* link state */
    uint8_t ack;                /* the handshake level last seen */
    uint64_t link_at;           /* the link's pending deadline */
    uint8_t rx[MC_ADBCTL_MAX_PACKET];
    unsigned rx_n;
    uint8_t tx[MC_ADBCTL_MAX_PACKET];
    unsigned tx_n, tx_i;
    uint16_t stream;            /* streaming reply: the next address ($FFFF: none) */
    uint8_t stream_kind;        /* 1 memory, 2 parameter RAM */
    uint64_t reply_at;          /* when the reply is ready (ADB time) */
    uint8_t reply_pending;
    uint8_t search;             /* autopoll: Service Request search pending */
    uint8_t restart_due;        /* restart after the link goes idle */
    uint64_t reset_until;       /* /RESET held until */
    uint64_t nmi_until;
    /* --- firmware level --------------------------------------------------- */
    mc_adbctl_mcu *mcu;
    mc_adb_wire wire;
    uint8_t keep_ram;           /* cold start: the battery-backed RAM keeps its contents */
    uint64_t key_hold;          /* the start's power key: held this long (C15M) after the power comes on */
    uint64_t key_release;       /* ... until this time (0: not yet known) */
    unsigned slice;             /* C15M periods per firmware run slice */
    uint64_t hold;              /* the firmware runs at least to here (adbctl.c, fw_run) */
    const struct mc_adbctl_timing *timing;  /* the part's timing (NULL: the kind's) */
    /* Caboose (the Quadra 900/950's Egret): its own pseudo-commands $20
     * (2 bytes) and $21 (1 byte), which the Quadra 950 ROM and the System
     * send (ROM offset $85588: $20 with 01 00 once the ROM has installed
     * its keyswitch handler, $854D0), kept; and the keyswitch's packet. */
    uint8_t caboose;
    uint8_t caboose_cmd[2][2];
    /* A packet to send unsolicited (mc_adbctl_post), its length. */
    uint8_t post[4];
    uint8_t post_n;
} mc_adbctl;

void mc_adbctl_init(mc_adbctl *c, const mc_hw_host *host, const char *name, mc_adbctl_kind kind,
                    mc_via *via1, mc_adb_bus *adb);
extern const mc_dev_ops mc_adbctl_ops;

/* Firmware: identifies an image by size and CRC32 (romdb.c, docs/NOTES.md);
 * returns the part's name or NULL. */
const char *mc_adbctl_firmware_name(const uint8_t *image, size_t size, mc_adbctl_kind *kind);
/* Runs the given image (a recognised one of the right kind) from now on;
 * returns 0 when it is refused. Keeps the parameter RAM and the clock. */
int mc_adbctl_use_firmware(mc_adbctl *c, const uint8_t *image, size_t size);
/* emulation.controller (its values are MC_CONTROLLER_*, in this order):
 * runs the first of `keys` (NULL-terminated firmware keys, e.g.
 * "cuda-341s0788"; models.h, mc_model_controller_firmware) that the roms
 * folders' index holds, whatever its file name (romdb.h,
 * mc_rom_index_firmware), and that is this kind's firmware. AUTO runs it
 * when present, PROTOCOL never, FIRMWARE requires it. Returns 1 when the
 * firmware runs, 0 for the protocol level, -1 when FIRMWARE found none. */
struct mc_rom_index;
int mc_adbctl_select(mc_adbctl *c, mc_adbctl_level level, const struct mc_rom_index *roms, const char *const *keys);

/* VIA1 port B as the VIA drives it (from its MC_VIA_LINE_PB report). */
void mc_adbctl_via_pb(mc_adbctl *c, uint8_t pb);
/* An ADB device has new data. */
void mc_adbctl_poke(mc_adbctl *c);
/* The power key (on the keyboard or the case). */
void mc_adbctl_power_key(mc_adbctl *c, int down);
/* An unsolicited packet (at most 4 bytes) sent when the link is free. */
void mc_adbctl_post(mc_adbctl *c, const uint8_t *p, unsigned n);
/* Brings the controller up to now (the firmware runs to the current time). */
void mc_adbctl_sync(mc_adbctl *c);

/* Parameter RAM (256 bytes) and the clock (seconds since 1904). */
uint8_t *mc_adbctl_pram(mc_adbctl *c);
uint32_t mc_adbctl_seconds(mc_adbctl *c);
void mc_adbctl_set_seconds(mc_adbctl *c, uint32_t seconds);
/* Livadix's PRAM file ("LVDXPRAM", version 1, the clock minus the host's time
 * at start, the 256 bytes: the format of the Quadra 700's RTC). `host_start`
 * is the host's time (seconds since 1904) the clock is relative to. */
int mc_adbctl_nvram_load(mc_adbctl *c, const char *path);
int mc_adbctl_nvram_save(mc_adbctl *c, const char *path, int64_t host_start);
/* After nvram_load: the clock becomes the saved offset plus the host's time. */
void mc_adbctl_set_clock(mc_adbctl *c, int64_t host_seconds);

/* --- inside the module (adbctl.c, egret.c, cuda.c) ------------------------ */
uint64_t mc_adbctl_now(const mc_adbctl *c);
void mc_adbctl_set_line(mc_adbctl *c, int line, int state);
void mc_adbctl_set_xcvr(mc_adbctl *c, int level);
/* The host's packet in rx[] is complete: runs it and prepares the reply. */
void mc_adbctl_packet(mc_adbctl *c);
/* Byte i of the packet going out; *last is set on its final byte. */
uint8_t mc_adbctl_tx_byte(mc_adbctl *c, unsigned i, int *last);
/* With the link idle: an unsolicited packet (tick, autopoll, power key) to
 * send now? Fills tx[] and returns 1. */
int mc_adbctl_unsolicited(mc_adbctl *c);
/* The controller's next own deadline (ticks, polls, timeouts). */
uint64_t mc_adbctl_own_deadline(const mc_adbctl *c);
void mc_adbctl_reschedule(mc_adbctl *c);
/* Link layers. */
void mc_egret_link(mc_adbctl *c);
void mc_cuda_link(mc_adbctl *c);
/* Kind-specific pseudo-commands: returns the expected data length (-1 bad
 * command, -2 two or more bytes, -3 one to four) for `cmd`. */
int mc_egret_pseudo_length(const mc_adbctl *c, uint8_t cmd);
int mc_cuda_pseudo_length(const mc_adbctl *c, uint8_t cmd);
/* Runs a kind-specific pseudo-command; returns 1 when handled. */
int mc_egret_pseudo(mc_adbctl *c, uint8_t cmd, const uint8_t *data, unsigned n);
int mc_cuda_pseudo(mc_adbctl *c, uint8_t cmd, const uint8_t *data, unsigned n);

/* Timing (microseconds of guest time), from the firmware on the
 * differential harness (test_adbctl_diff.c: 341S0788 and 341S0851 at their
 * 2.097 MHz bus). */
struct mc_adbctl_timing {
    uint16_t rx_first;          /* the host's first byte: TIP / ViaFull to its clocks done */
    uint16_t rx_byte;           /* each further byte */
    uint16_t process;           /* the host's packet's end to the reply's idle clocks */
    uint16_t attention;         /* TREQ / XcvrSes asserted to its idle clocks */
    uint16_t tx_first;          /* TIP / SysSes asserted to the first byte's clocks */
    uint16_t tx_byte;           /* the host's handshake to the next byte's clocks */
    uint16_t adb_answer;        /* added to an ADB transaction's own time: answered, */
    uint16_t adb_timeout;       /* not answered */
};
typedef struct mc_adbctl_timing mc_adbctl_timing;

const mc_adbctl_timing *mc_adbctl_timing_of(const mc_adbctl *c);
/* The part a machine has (its timing at the protocol level), e.g.
 * "341S0789"; NULL for the kind's default. */
void mc_adbctl_set_part(mc_adbctl *c, const char *part);
#define MC_ADBCTL_US(n) ((uint64_t)MC_C15M_HZ / 1000000u * (n) + (uint64_t)MC_C15M_HZ % 1000000u * (n) / 1000000u)

#endif
