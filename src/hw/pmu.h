/* Livadix — the PowerBook Power Manager (PMU): its command protocol over the
 * VIA, at protocol level (pmu.c) or running Apple's firmware (pmu_fw.c).
 *
 * Sources. Guide to the Macintosh Family Hardware, 2nd ed.: chapter 2
 * "Power Manager microprocessor" and chapter 3 "Power Manager IC in the
 * Macintosh Portable computer" (an M50753 with 6 KB of ROM; it sleeps and
 * wakes the machine, is the real-time clock, keeps parameter RAM, is the ADB
 * transceiver, drives the display contrast, watches temperature and
 * battery), chapter 4 Tables 4-4 and 4-13 (the Portable's VIA port A as
 * the eight-bit PMU data bus, PB0 vPMReq and PB1 vPMAck: the processor's
 * request and the PMU's acknowledge), chapter 2 "Real-time clock" (128 bytes
 * of parameter RAM in the PMU). Inside Macintosh: Devices (1994), chapter 6
 * "Power Manager" (idle and sleep states, the wakeup timer, battery and
 * charger status, modem power). Everything below the Guide's description
 * comes from observing Apple's code: the PMU firmware
 * pmu-pb140.bin and pmu-pb160.bin disassembled with Livadix's 740 core, and
 * the PowerBook ROMs' Power Manager driver (PMgrOp) read with a 68k
 * disassembler.
 *
 * The protocol (observed in the ROMs' PMgrOp and both firmwares). The Mac
 * sends a command byte, then (old framing: always; new framing: when its
 * send-count table holds $FF) a count byte, then the data. A reply follows
 * when the receive-count table is not 0: in the old framing a first byte
 * (the command, or a status for $7E/$7F), a count and the data; in the new
 * framing a fixed table entry n means n - 1 data bytes without count, $FF
 * a count and the data, 1 a single status byte. The two tables are the same
 * in every PowerBook ROM (PB 160 ROM $89DAC/$89EAC); which framing a machine
 * uses is bit 17 of its ProductInfo extended-features word ($0DD4): clear on
 * the PowerBook 140-180 (old), set on the Duos, the 150 and the 520 (new).
 *
 * The byte handshake. Parallel (port A of the VIA at the ROM's VIA2 pointer
 * $0CEC; PB2 /REQ out, PB1 /ACK in): to send, the Mac drives port A, pulls
 * /REQ low, waits for /ACK low, raises /REQ, waits for /ACK high; to
 * receive, it waits for /ACK low (the PMU drives port A), pulls /REQ low,
 * reads port A, waits for /ACK high, raises /REQ. Shift register (VIA1's
 * SR clocked by the PMU on CB1; same /REQ and /ACK): to send, the Mac sets
 * shift-out (ACR bit 4), writes SR and pulls /REQ low; to receive, it sets
 * shift-in and pulls /REQ low first; the PMU clocks the byte, pulls /ACK
 * low, the Mac raises /REQ, the PMU raises /ACK. The PB 160/180 PMU drives
 * $07 on port A while busy, which the ROM waits out before a command
 * (extended-features bit 21); the PB 190's ROM waits for /ACK high first.
 * The PMU interrupt /INT is a short low pulse on VIA1 CB1 (the ROM's
 * handler sits in the CB1 slot of Lvl1DT, $01A2) and the ROM answers it
 * with command $78.
 *
 * Wiring per machine (for 5.2g and 5.2h; † where not confirmed by the
 * machine's ROM run):
 *   PowerBook 140, 145, 170: MC_PMU_PB140, old framing, parallel; VIA2
 *     port A data, PB2 /REQ, PB1 /ACK; /INT on VIA1 CB1. ROM 420DBFF3.
 *   PowerBook 160, 165, 165c, 180, 180c: MC_PMU_PB160, old framing, busy
 *     $07, parallel; same pins†. ROM E33B2724.
 *   Duo 210, 230, 250, 270c, 280, 280c: MC_PMU_PGE, new framing, shift
 *     register (VIA1 SR, CB1 clock), /REQ VIA2 PB2, /ACK VIA2 PB1†.
 *   PowerBook 150: MC_PMU_99, new framing, shift register, the MSC's port
 *     B as the Duos'; ROM FDA22562, its PmgrOp counts (mc_pmu_set_counts).
 *   PowerBook 520, 520c, 540, 540c, 190, 190cs: MC_PMU_99, new framing,
 *     shift register (VIA1 SR, CB1 clock), /REQ VIA2 PB2 as written, /ACK
 *     VIA2 PB1, /INT VIA1 CB1; ROMs B6909089 and 4D27039C and their
 *     PmgrOp counts (machines/powerbook500.c).
 *   Macintosh Portable, PowerBook 100: MC_PMU_PORTABLE (firmware
 *     pmu-portable.bin, version $02B5), old framing, parallel; the VIA's
 *     port A data, PB0 /REQ, PB1 /ACK (Guide, Tables 4-4 and 4-13), /INT
 *     on CB1, the one-second output (MC_PMU_LINE_SECOND, a pulse) on CA2
 *     (Table 4-19). Its tick is the Miscellaneous GLU's 60 Hz (Guide,
 *     chapter 3: the 60.15 Hz signal on the M50753's INT1, which the board
 *     gives with mc_pmu_set_tick); 60 ticks make its second. The clock and
 *     the wakeup timer as on the PB 160; no $7E (Shut Down is the ROM's
 *     sleep without its resume signature, machines/portable.c); $78
 *     answers the interrupt's status byte alone and $28 the ADB data
 *     (firmware $EE87, $F076); $10/$18 and $50/$58 on its own pins
 *     (pmu.c).
 *
 * Commands (from the firmware unless marked): $10-$17 power planes on/off
 * (bit 7 of each data byte: clear), $18 read them; $20 ADB command
 * (command, flags: bit 2 autopoll, count, data), $21 ADB off; $30 set and
 * $38 read the clock (PB 160 and later), $31/$39 the 20 bytes of the
 * original parameter RAM (bytes $10-$1F, then $08-$0B), $32/$3A extended
 * parameter RAM (address, count, data); $40-$47 set and $48 read the
 * contrast (0-31), $49 A-D channel 0; $50-$57 and $58 the misc. power bits;
 * $60/$6A battery thresholds, $68/$69 battery status (flags, level,
 * temperature); $78 the interrupt's data; $7E shut down and $7F sleep
 * (signature "MATT"); $80/$82/$88 the wakeup timer (PB 160 and later);
 * $90 modem power, $98 its status; $D0 a reset pulse, $D8 an A-D channel;
 * $E0 write and $E8 read PMU memory, $EA the firmware version, $EC the
 * self-test, $EF reset the PMU. The interrupt data: ($01 << 4 | ADB flags,
 * command, data) for ADB, ($20, flags, level, temperature) for the
 * battery; PMU99 adds a one-second interrupt (bit 7 of the status byte: the
 * PB 520 ROM's handler at $D8E80 chains it to the one-second handler) †.
 *
 * Sleep is paused emulation. The CPU runs while MC_PMU_LINE_POWER is
 * MC_PMU_POWER_ON and MC_PMU_LINE_RESET is 0, from its reset vector each
 * time that becomes true (power-up, and the end of the reset pulse on
 * wake). $7F with the signature (or $7E, shut down) turns POWER to SLEEP
 * (or OFF) five ticks later; the machine stops running the CPU and keeps
 * everything else, guest time included (the clock keeps counting). A host
 * key, mouse or other action calls mc_pmu_wake(): as the firmware's sleep
 * loop does once it sees the key (or, off, the power key's press and
 * release), the PMU turns POWER on with RESET held for four ticks, and the
 * ROM resumes from sleep (or starts).
 *
 * Lines (mc_hw_host line callback): MC_PMU_LINE_ACK (the /ACK level),
 * MC_PMU_LINE_INT (the /INT level, 1 = high), MC_PMU_LINE_DATA (the byte
 * the PMU drives on port A, -1 when it drives nothing), MC_PMU_LINE_POWER,
 * MC_PMU_LINE_RESET (1 while the CPU is held in reset), MC_PMU_LINE_CONTRAST
 * (0-31), MC_PMU_LINE_MODEM (modem power), MC_PMU_LINE_PLANES (the power
 * plane bits of $10/$18). */
#ifndef MC_PMU_H
#define MC_PMU_H

#include "adb.h"
#include "hw.h"

#include <stddef.h>
#include <stdint.h>

enum mc_pmu_model { MC_PMU_PB140, MC_PMU_PB160, MC_PMU_PGE, MC_PMU_99, MC_PMU_PORTABLE };
enum mc_pmu_iface { MC_PMU_PARALLEL, MC_PMU_SHIFT };
enum mc_pmu_level { MC_PMU_PROTOCOL, MC_PMU_FIRMWARE };

enum {
    MC_PMU_LINE_ACK, MC_PMU_LINE_INT, MC_PMU_LINE_DATA, MC_PMU_LINE_POWER, MC_PMU_LINE_RESET,
    MC_PMU_LINE_CONTRAST, MC_PMU_LINE_MODEM, MC_PMU_LINE_PLANES, MC_PMU_LINE_SECOND
};
enum { MC_PMU_POWER_ON, MC_PMU_POWER_SLEEP, MC_PMU_POWER_OFF };

/* The PMU's main tick: the firmware runs its housekeeping (ADB autopoll,
 * battery, interrupts) every 91st of a second (91 ticks to its one-second
 * count while awake), from timer X. */
#define MC_PMU_TICK_HZ 91u
#define MC_PMU_TICK (MC_C15M_HZ / MC_PMU_TICK_HZ)

/* The battery the PMU reports: a plugged-in, charged battery, as A-D codes
 * the firmware reads (channel 1 the battery voltage, 5 the temperature, 7
 * the charger's sense, port IN bit 3 the charger) †. */
#define MC_PMU_BATTERY_LEVEL 0xD8
#define MC_PMU_TEMPERATURE 0x90
#define MC_PMU_CHARGER_SENSE 0x40

typedef struct mc_pmu_fw mc_pmu_fw;
typedef struct mc_pmu_pge mc_pmu_pge;

typedef struct mc_pmu {
    mc_dev dev;
    uint8_t model, iface, level;
    mc_adb_bus *bus;
    /* The VIA side. */
    uint8_t req;                 /* /REQ level from the Mac */
    uint8_t port_in;             /* the byte the Mac drives on port A */
    uint8_t ack, int_level;      /* levels the PMU drives */
    int data_out;                /* byte driven on port A, -1 none */
    uint8_t (*shift)(void *ctx, uint8_t in);    /* shift-register interface (mc_via_shift8) */
    void *shift_ctx;
    uint8_t power, reset;
    /* Protocol level. */
    uint8_t phase;               /* the transfer state */
    uint8_t pending_action;      /* what the deadline does */
    uint64_t due_ack, due_tick, due_int, due_adb, due_busy, due_timeout, due_power;
    uint8_t rx_byte;
    uint8_t cmd, rx_stage;          /* 0 command, 1 count, 2 data */
    uint8_t in[256];
    unsigned in_len, in_need;
    uint8_t out[260];
    unsigned out_len, out_pos;
    uint8_t busy;                /* PB 160: $07 on port A */
    uint8_t ram01;               /* interrupt sources: bit 0 ADB, 1-2 battery, 3 one-second (PMU99) */
    uint8_t adb_flags;           /* firmware $06: 0 host, 1 pending, 2 autopoll, 3 SRQ, 4/5/7 status */
    uint8_t adb_cmd;
    uint8_t adb_out[8];
    unsigned adb_out_len;
    uint8_t adb_reply[8];
    unsigned adb_reply_len;
    uint8_t adb_busy;
    uint8_t planes;              /* firmware P0 latch */
    uint8_t misc;                /* firmware $00 bits */
    uint8_t contrast;            /* 0–31 on older boards; raw PWM on PBX */
    uint8_t pbx_lcd, brightness, contrast_max, contrast_min;
    uint8_t smart_battery;        /* $6F answers a smart battery's record (mc_pmu_set_smart_battery) */
    uint8_t lcd_aux;              /* $4F/5, a separate PWM control */
    /* A-D channel 0: the screen's contrast slider (0-255, mid-scale $80 at
     * start), which the PMU's firmware (PB 140-180) reads to set the
     * contrast (0 gives 31, $80 20, $FF 10) and the ROM reads through $D8
     * at the protocol level (then setting it with $40: 20 at $80). */
    uint8_t slider;
    uint8_t modem;
    uint8_t battery_flags, battery_level, temperature;
    uint8_t threshold[2];        /* $72, $73 */
    uint16_t seconds_ticks;      /* ticks to the firmware's next second ($25; 0 at a cold start: 256) */
    uint8_t tick_in;             /* MC_PMU_PORTABLE: the 60.15 Hz input's level */
    uint8_t charger_start;       /* MC_PMU_PORTABLE: started by the charger (IN bit 5 clear) */
    uint8_t p1_out;              /* MC_PMU_PORTABLE: P1 bit 7 as $50 sets it */
    uint8_t charge_state;        /* MC_PMU_PORTABLE: the charger's state ($21) */
    uint16_t charge_count;       /* MC_PMU_PORTABLE: its counter ($22-$23) */
    uint8_t second_level;        /* MC_PMU_PORTABLE: the one-second output's level */
    uint64_t due_second;         /* when the one-second pulse ends */
    uint8_t sleep_mode;          /* 0 awake, 1 asleep or off, 2 waking, 3 CPU reset on wake, 4 powering up,
                                    5 booting, 6 going to sleep, 7 shutting down, 8 the Portable's
                                    reset button held */
    uint8_t down_ticks;          /* ticks left of the way down */
    uint8_t planes_saved;        /* the planes before sleep */
    uint64_t loop_start;         /* when the firmware's sleep loop runs */
    uint16_t charge_seconds;     /* seconds since the charger was found */
    uint64_t ready_at;           /* a reply's first byte not before (the command's own time) */
    uint8_t wake_time[4];
    uint8_t wake_enabled;
    /* Parameter RAM (128 bytes on the M50753 PMUs, 256 on the later ones†)
     * and the clock (seconds since 1904 at guest time `clock_base`). */
    uint8_t pram[256];
    /* PMU99 external NVRAM, owned and persisted by its board. */
    uint8_t *nvram;
    uint32_t nvram_size;
    uint32_t seconds;
    uint64_t clock_base;
    uint8_t dirty;
    uint8_t version[2];
    /* Firmware level: the M50753 (pmu_fw.c) or the PG&E (pmu_pge.c, with
     * the 68HC05 core). */
    mc_pmu_fw *fw;
    mc_pmu_pge *pge;
    /* The ROM's PmgrOp count tables (256 send counts, then 256 reply
     * counts; $FF a counted transfer), when the board names them: the
     * new framing's protocol level then takes its counts from them. */
    const uint8_t *counts;
    uint8_t bay_wired, bay_id;   /* PMU99 on the PowerBook 190: $4D answers the expansion bay's module ID */
    /* The new framing's autopoll: the devices to poll (bit n: address n). */
    uint16_t adb_map;
} mc_pmu;

/* `firmware` (6144 bytes for the M50753 models; the 512-byte boot ROM for
 * the PG&E, used once the 68HC05 core is built in; NULL for protocol
 * level) selects the level; `bus` holds the ADB devices the PMU is master
 * of. */
void mc_pmu_init(mc_pmu *p, const mc_hw_host *host, const char *name, unsigned model, unsigned iface,
                 mc_adb_bus *bus, const uint8_t *firmware, size_t firmware_size);
void mc_pmu_free(mc_pmu *p);
extern const mc_dev_ops mc_pmu_ops;

/* The VIA side: /REQ, the byte port A shows, and (shift-register models)
 * the function that clocks a byte through VIA1's SR. */
void mc_pmu_set_req(mc_pmu *p, int level);
/* MC_PMU_PORTABLE: the reset button of the programmer's switch, on the
 * M50753's INT2 and P3.0 (low while pressed): the firmware holds the
 * processor in reset (P3.1 low, $F326), waits for the release and restarts
 * its main loop ($E85C, parameter RAM and the clock kept), which lets the
 * processor go. 0 on other models. */
int mc_pmu_reset_switch(mc_pmu *p, int pressed);
/* The contrast slider (A-D channel 0) moved. */
void mc_pmu_set_slider(mc_pmu *p, uint8_t value);
/* MC_PMU_PORTABLE: the board's 60.15 Hz square wave (INT1; the PMU counts
 * its falling edges). */
void mc_pmu_set_tick(mc_pmu *p, int level);
void mc_pmu_set_port(mc_pmu *p, uint8_t value);
void mc_pmu_set_shift(mc_pmu *p, uint8_t (*shift)(void *ctx, uint8_t in), void *ctx);
/* The ROM's PmgrOp count tables (512 bytes, see mc_pmu.counts); NULL for
 * the built-in ones. */
void mc_pmu_set_counts(mc_pmu *p, const uint8_t *counts);
/* The PowerBook G3 Series' batteries: $6F with a battery's number answers
 * its record (see pmu.c). */
void mc_pmu_set_smart_battery(mc_pmu *p);
/* PBX's downloaded PG&E firmware gives the LCD separate PWM controls. */
void mc_pmu_set_pbx_lcd(mc_pmu *p);
/* Wallstreet ROM CBB01212 $FFF078AC-$FFF07A28: $33 writes one byte
 * at a 16-bit address, $3B reads one byte. NULL disconnects the NVRAM. */
void mc_pmu_set_nvram(mc_pmu *p, uint8_t *data, uint32_t size);
/* The PowerBook 190's expansion bay: $4D (one byte sent) answers the
 * module's ID in bits 0-2, as ROM 4D27039C's ReadHWMediaBayID reads it
 * (offset $C1EA0; MediaBayGetType, $C1E00: 0 the floppy module "MB1",
 * 3 an ATA module "MB2", 7 none). */
void mc_pmu_set_bay(mc_pmu *p, uint8_t id);
/* A key, mouse or host action while asleep: power returns and the CPU is
 * reset, as the firmware's wake path does. Returns 0 when not asleep. */
int mc_pmu_wake(mc_pmu *p);
int mc_pmu_asleep(const mc_pmu *p);

/* Parameter RAM and the clock (persisted by the machine). */
unsigned mc_pmu_pram_size(const mc_pmu *p);
void mc_pmu_pram_get(mc_pmu *p, uint8_t *out);
void mc_pmu_pram_put(mc_pmu *p, const uint8_t *in);
uint32_t mc_pmu_seconds(mc_pmu *p);
void mc_pmu_set_seconds(mc_pmu *p, uint32_t seconds);
/* emulation.nvram in Livadix's PRAM file format ("LVDXPRAM", version 1, the
 * clock as seconds since 1904 minus the host's seconds, 256 bytes):
 * `host_seconds` is the host's time in seconds since 1904. */
int mc_pmu_nvram_save(mc_pmu *p, const char *path, int64_t host_seconds);
int mc_pmu_nvram_load(mc_pmu *p, const char *path, int64_t host_seconds);

/* Firmware (romdb.c's table, docs/NOTES.md): runs the given image from now
 * on when it is this model's (pmu-pb140 for MC_PMU_PB140, pmu-pb160 for
 * MC_PMU_PB160, pmu-pge-boot for MC_PMU_PGE, by size and CRC32); returns 0
 * when it is refused. Call before the reset. */
int mc_pmu_use_firmware(mc_pmu *p, const uint8_t *image, size_t size);
/* The PG&E at firmware level: the main program (the image a Duo ROM
 * downloads with $E1, from its "BORG" tag) already in its RAM, as an
 * earlier start left it; the boot ROM then runs it from its reset. */
void mc_pmu_pge_program(mc_pmu *p, const uint8_t *image, size_t size);
/* The PG&E at firmware level scans the built-in keyboard and trackball
 * itself: input goes to it (1) instead of the ADB devices (0). */
int mc_pmu_input_key(mc_pmu *p, uint8_t code, int down);
int mc_pmu_input_mouse(mc_pmu *p, int dx, int dy, int button);
/* emulation.controller (MC_CONTROLLER_* of config.h): runs the first of
 * `keys` (NULL-terminated firmware keys; models.h,
 * mc_model_controller_firmware) the roms folders' index holds and that is
 * this model's. AUTO runs it when present, PROTOCOL never, FIRMWARE
 * requires it. Returns 1 when the firmware runs, 0 for the protocol level,
 * -1 when FIRMWARE found none. */
struct mc_rom_index;
int mc_pmu_select(mc_pmu *p, unsigned level, const struct mc_rom_index *roms, const char *const *keys);

/* Timing of the protocol level (C15M periods), as the firmware measures
 * them on its 740 core (the differential, test_pmu): from /REQ low to /ACK
 * low, from /REQ high to /ACK high, and from a byte's end to the next
 * byte the PMU presents. */
#define MC_PMU_ACK_DELAY ((uint64_t)MC_C15M_HZ * 47u / 1000000u)
#define MC_PMU_RELEASE_DELAY ((uint64_t)MC_C15M_HZ * 12u / 1000000u)
#define MC_PMU_NEXT_DELAY ((uint64_t)MC_C15M_HZ * 43u / 1000000u)
#define MC_PMU_INT_PULSE ((uint64_t)MC_C15M_HZ * 10u / 1000000u)

#endif
