/* The machine interface: everything the shell (src/app, src/platform) knows
 * of an emulated Macintosh.
 *
 * The core has no SDL and no wall clock. Guest time is a count of periods of
 * the Macintosh's 15.6672 MHz master clock (C15M), derived from the CPU's
 * clock cycles (each instruction takes its cycles for the processor) and
 * the configured `emulation.cpu_mhz`, so a run is deterministic at any host
 * speed; the shell paces it to wall time.
 *
 * Models with a machine description (src/hw/machines/) run the real
 * machine (src/hw/machine.c); every other model is served by the stub
 * machine (stub_machine.c): it shows a test pattern at the configured
 * resolution and depth and echoes input (keys, mouse, a tone per key press),
 * so the shell, the configuration and the presenter work end to end. */

#ifndef MC_MACHINE_H
#define MC_MACHINE_H

#include "config.h"
#include "disk.h"
#include "models.h"

#include <stddef.h>
#include <stdint.h>

#define MC_C15M_HZ 15667200u

typedef struct mc_machine mc_machine;

typedef enum mc_run_status {
    MC_RUN_OK,          /* ran the cycles asked, or reached the deadline */
    MC_RUN_HALTED,      /* the Macintosh switched its power off (mc_machine_powered_off) */
    MC_RUN_ERROR        /* stopped: mc_machine_error says why */
} mc_run_status;

/* Whether a real machine (CPU and chips) emulates this model; when not
 * (every stock model is emulated: the hidden model MC_MODEL_STUB, models.h),
 * mc_machine_create builds the stub machine. */
int mc_machine_emulated(const mc_model *model);
/* The RAM sizes an emulated model takes, in KiB (whole MiB and the sizes
 * that are not, such as the Plus's 2.5 MB), at most `max` into `kb`;
 * returns how many (0: not an emulated model). Not sorted. */
size_t mc_machine_ram_sizes(const mc_model *model, uint32_t *kb, size_t max);

/* Screen depths as a mask: bit 0 for 1 bit, then 2, 4, 8, 16, 32. */
#define MC_DEPTH_BIT(d) (1u << ((d) <= 1 ? 0 : (d) == 2 ? 1 : (d) == 4 ? 2 : (d) == 8 ? 3 : (d) == 16 ? 4 : 5))
#define MC_DEPTHS_UPTO(d) ((d) ? MC_DEPTH_BIT(d) * 2u - 1u : 0u)
/* The depths (MC_DEPTH_BIT mask) the model shows at width x height: on its
 * built-in video, and on the Livadix Display Card (0 when the model has no
 * slot for it; else every depth that fits its 2 MiB). A model without a
 * hardware description shows any mode that fits 2 MiB on the stub. */
void mc_machine_video(const mc_model *model, unsigned width, unsigned height, unsigned *builtin, unsigned *card);
/* What else the model has, for the configuration editor. */
enum {
    MC_FEATURE_ONE_FLOPPY = 256, /* one selectable floppy mechanism */
    MC_FEATURE_NO_FLOPPY = 128, /* no built-in floppy mechanism */
    MC_FEATURE_ATA_DISK = 1,    /* an internal IDE (ATA) hard disk: storage.disks bus ata */
    MC_FEATURE_ATA_CD = 2,      /* the expansion bay's ATAPI CD-ROM drive: storage.cdrom bus ata */
    MC_FEATURE_ENET = 4,        /* built-in Ethernet (network.card does not apply) */
    MC_FEATURE_NUBUS = 8,       /* a NuBus slot for the EtherTalk NB Card (network.card nubus) */
    MC_FEATURE_LC_SLOT = 16,    /* an LC processor-direct slot for the Ethernet LC Card (network.card lc) */
    MC_FEATURE_MEDIA_BAY = 64, /* the ATAPI CD occupies the floppy media bay */
    MC_FEATURE_FIXED_SCREEN = 32 /* a built-in screen and no slot: its own mode only */
};
unsigned mc_machine_features(const mc_model *model);

/* A machine for the configuration's model with its ROM image (copied).
 * Media are inserted afterwards with mc_machine_insert. */
mc_machine *mc_machine_create(const mc_config *config, const uint8_t *rom, size_t rom_size, char *error, size_t capacity);
/* Whether emulation.m68k_accelerator has a ROM it works on: a Power
 * Macintosh ROM whose 68k emulator is one the accelerator knows. */
int mc_machine_m68k_accelerator_known(const uint8_t *rom, size_t rom_size);
/* Saves what persists (PRAM and the clock, emulation.nvram) and frees. */
void mc_machine_destroy(mc_machine *m);
/* Same teardown, with persistence failures reported to the caller. */
int mc_machine_close(mc_machine *m, char *error, size_t capacity);
/* The host's local time when the machine starts, in seconds since 1 January
 * 1904 (the Macintosh epoch). The machine's clock is this plus the
 * adjustment saved with its PRAM, then advances with guest time. Without
 * it the clock starts from the saved value alone, so a run is
 * reproducible. Call before the first run. */
void mc_machine_set_clock(mc_machine *m, int64_t host_seconds);
/* What runs: the model's name, or the stub's description. */
const char *mc_machine_description(const mc_machine *m);
const char *mc_machine_error(const mc_machine *m);

/* Run at least one instruction and about `cycles` CPU clock cycles (the
 * last instruction may end past them), stopping early once guest time
 * reaches `deadline` (C15M periods; 0 for none). */
mc_run_status mc_machine_run(mc_machine *m, uint64_t cycles, uint64_t deadline);
/* CPU clock cycles run so far, those spent waiting in STOP included; the
 * instructions run (0 for the stub machine). */
uint64_t mc_machine_cycles(const mc_machine *m);
uint64_t mc_machine_instructions(const mc_machine *m);
/* Whether the Macintosh switched its own power off: the end of its System's
 * Shut Down on a model with soft power. Every run
 * after that returns MC_RUN_HALTED at once. */
int mc_machine_powered_off(const mc_machine *m);
/* Guest memory as a supervisor data read sees it (through the MMU when it
 * translates): the shell observes the running System's low memory and
 * structures with it (src/app/shutdown.c). 0 when nothing is there; the stub
 * machine has none. */
int mc_machine_peek(mc_machine *m, uint32_t address, unsigned size, uint32_t *value);
uint64_t mc_machine_time(const mc_machine *m);
/* CPU clock cycles per second of guest time (emulation.cpu_mhz in Hz). */
uint64_t mc_machine_rate(const mc_machine *m);

/* The screen as the guest's video hardware holds it: `depth` bits per pixel,
 * big-endian, rows `row_bytes` apart; 1 to 8 bits index `palette`
 * (0x00RRGGBB), 16 bits are x-R5-G5-B5 and 32 bits x-R8-G8-B8. `serial`
 * changes whenever pixels, palette or geometry change. */
typedef struct mc_framebuffer {
    uint32_t width, height, depth, row_bytes;
    const uint8_t *pixels;
    const uint32_t *palette;
    uint64_t serial;
    /* The display's brightness as the machine sets it, 1-255 of full; 0
     * full (most machines leave it). mc_framebuffer_argb applies it. */
    uint16_t level;
    /* The display's contrast, 1-255 of full (the picture drawn toward
     * mid-gray); 0 full. */
    uint16_t contrast;
    /* Physical guest pixel format when presentation includes a hardware
     * cursor in a different format. Zero means depth/row_bytes above. */
    uint32_t native_depth, native_row_bytes;
} mc_framebuffer;
/* 0 when the machine shows nothing (no display, or the display is off). */
int mc_machine_framebuffer(mc_machine *m, mc_framebuffer *fb);
/* The framebuffer as ARGB8888, width x height pixels into out. */
void mc_framebuffer_argb(const mc_framebuffer *fb, uint32_t *out);

/* Input. Keys are ADB key codes (0..127) as the Apple Extended Keyboard
 * sends them; mouse motion is in screen pixels, button 1 for down. Both
 * return 0 when the machine's input queue is full. */
int mc_machine_key(mc_machine *m, uint8_t adb_code, int down);
int mc_machine_mouse(mc_machine *m, int dx, int dy, int button);
/* Absolute hardware input in guest screen pixels; never patches guest RAM.
 * Ready only after the guest has configured and polled the device. Input
 * returns readiness, retaining button releases while the device is suspended. */
int mc_machine_absolute_ready(mc_machine *m);
int mc_machine_absolute_mouse(mc_machine *m, int x, int y, int button);
/* The pointer straight to screen pixel (x, y) of the main screen, as the
 * running System's cursor globals hold it (MTemp, RawMouse and CrsrNew,
 * Inside Macintosh: Imaging With QuickDraw, "Cursor Utilities"). For
 * Classic Mac OS only: how a Remote Desktop session and the checks place
 * its pointer; other guests have no such globals.
 * Returns 0 when nothing was written (the stub machine, no memory there). */
int mc_machine_mouse_to(mc_machine *m, int x, int y);

/* Sound: the shell pulls interleaved stereo 16-bit frames at
 * mc_machine_audio_rate (Hz; 0 when the machine has no sound). Returns the
 * frames written, at most `frames`: those the guest produced so far. */
uint32_t mc_machine_audio_rate(const mc_machine *m);
/* Live independent speaker and mechanical floppy sound switches. */
void mc_machine_set_sound(mc_machine *m, int speaker, int floppy);
size_t mc_machine_audio_pull(mc_machine *m, int16_t *stereo, size_t frames);
/* A codec's signed stereo input at its selected rate. NULL supplies silence.
 * enable is optional: a nonzero rate starts input, zero stops it. */
typedef struct mc_audio_source {
    void *ctx;
    void (*sample)(void *ctx, unsigned rate, int16_t frame[2]);
    void (*enable)(void *ctx, unsigned rate);
} mc_audio_source;
/* Returns 0 when the machine has no connected sound-input codec. */
int mc_machine_set_audio_source(mc_machine *m, const mc_audio_source *source);

/* Serial ports: port 0 is the modem port (SCC channel A), 1 the printer
 * port (channel B). The machine calls `transmit` for every character the
 * Macintosh sends and `outputs` when its handshake output (DTR, HSKo)
 * changes; bytes arriving from the host go in with mc_machine_serial_receive,
 * which returns how many the line took (the rest are offered again later).
 * DCD and CTS (HSKi) are the host side's modem inputs. */
typedef struct mc_serial_host {
    void *ctx;
    void (*transmit)(void *ctx, unsigned port, uint8_t byte);
    void (*outputs)(void *ctx, unsigned port, int dtr);
} mc_serial_host;
void mc_machine_set_serial(mc_machine *m, const mc_serial_host *host);
size_t mc_machine_serial_receive(mc_machine *m, unsigned port, const uint8_t *data, size_t count);
void mc_machine_serial_modem(mc_machine *m, unsigned port, int dcd, int cts);

/* Ethernet: the machine calls `transmit` with every frame its Ethernet
 * controller sends (destination address to the end of the data, without
 * the FCS); frames from the network go in with mc_machine_net_receive,
 * which returns 0 when the controller's wire is busy (offer the frame again
 * later) and 1 when the frame was taken (or the machine has no Ethernet).
 * Without a host (NULL) the cable is unplugged: no carrier. */
typedef struct mc_net_host {
    void *ctx;
    void (*transmit)(void *ctx, const uint8_t *frame, size_t length);
} mc_net_host;
void mc_machine_set_net(mc_machine *m, const mc_net_host *host);
int mc_machine_net_receive(mc_machine *m, const uint8_t *frame, size_t length);

/* Media. Hard disks go in before the first run; floppies and CDs at any
 * time. On success the machine owns the image (closed on replacement or
 * destruction). An ejected medium stays with its drive until another is
 * inserted, so mc_machine_reinsert can put it back. */
typedef enum mc_media_kind { MC_MEDIA_DISK, MC_MEDIA_FLOPPY, MC_MEDIA_CDROM } mc_media_kind;
/* Drives of a kind: hard disk slots, fitted floppy drives, CD-ROM drives. */
unsigned mc_machine_drives(const mc_machine *m, mc_media_kind kind);
int mc_machine_insert(mc_machine *m, mc_media_kind kind, unsigned drive, mc_disk *disk, char *error, size_t capacity);
/* The image in a drive (NULL when empty) and whether it was ejected. */
mc_disk *mc_machine_media(const mc_machine *m, mc_media_kind kind, unsigned drive, int *ejected);
int mc_machine_eject(mc_machine *m, mc_media_kind kind, unsigned drive);
/* The most recently ejected medium back into its drive; 0 when none. */
int mc_machine_reinsert(mc_machine *m);

/* The machine's own controls, which the host has no physical counterpart
 * for (docs/NOTES.md, "Machine controls"):
 *   MC_CONTROL_KEYSWITCH  the Quadra 900/950 front-panel key: value 0 Off,
 *                         1 On, 2 Secure (ADB and the floppy drive off);
 *   MC_CONTROL_BRIGHTNESS, MC_CONTROL_CONTRAST  a panel's or display's
 *                         brightness and contrast: value -1 or +1 a step
 *                         (the buttons or sliders of the machine);
 *   MC_CONTROL_VOLUME     front-panel volume buttons: -1 or +1 a press;
 *   MC_CONTROL_INTERRUPT, MC_CONTROL_RESET  the programmer's switch's
 *                         interrupt and reset buttons: value 1 pressed, 0
 *                         released (the interrupt: the NMI, level 7, or the
 *                         Portable's level 4, on every emulated model; the
 *                         reset: the 68000 Macs, the processor and the board
 *                         restarting on its release, the Portable's and
 *                         PowerBook 100's through their Power Manager).
 * Returns the keyswitch's new position, 0 or more for the others when the
 * machine took the step (its own software then acts on it), -1 when the
 * machine has no such control. */
typedef enum mc_control {
    MC_CONTROL_KEYSWITCH, MC_CONTROL_BRIGHTNESS, MC_CONTROL_CONTRAST, MC_CONTROL_VOLUME, MC_CONTROL_INTERRUPT, MC_CONTROL_RESET
} mc_control;
int mc_machine_control(mc_machine *m, mc_control control, int value);

#endif /* MC_MACHINE_H */
