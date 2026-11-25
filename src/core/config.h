/* livadix.yaml (docs/NOTES.md). libyaml reads the YAML syntax; this
 * module enforces Livadix's schema and resolves the model and ROM. */

#ifndef MC_CONFIG_H
#define MC_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define MC_CONFIG_PATH 1024
#define MC_CONFIG_DISKS 10
#define MC_CONFIG_FLOPPIES 2
#define MC_CONFIG_NAME 128          /* a profile name, with its NUL */
#define MC_CONFIG_PROFILES 256

typedef struct mc_disk_config {
    char image[MC_CONFIG_PATH];
    int readonly;
    int scsi_id_set, scsi_id;          /* hard disks: storage.disks scsi_id */
    int bus;                           /* hard disks: storage.disks bus, MC_BUS_* */
} mc_disk_config;

/* The bus a hard disk is on (storage.disks bus): SCSI, or ATA where the
 * machine has it. */
enum { MC_BUS_SCSI, MC_BUS_ATA };

enum { MC_PORT_DEFAULT, MC_PORT_SERIAL, MC_PORT_PIPE, MC_PORT_OUTPUT, MC_PORT_DISABLED };
/* network.mode: the Ethernet's cable (docs/NOTES.md, "Network"). */
enum { MC_NETWORK_NAT, MC_NETWORK_NONE };
/* network.card: the Ethernet card of a model without built-in Ethernet
 * (docs/NOTES.md, "Network"). */
enum { MC_NET_CARD_AUTO, MC_NET_CARD_NONE, MC_NET_CARD_NUBUS, MC_NET_CARD_LC };
#define MC_CONFIG_FORWARDS 16
/* network.forward: a host port leading to a Macintosh port. */
typedef struct mc_forward_config {
    int udp;
    uint32_t address;                  /* the host address listened on (host byte order) */
    uint16_t host_port, guest_port;
} mc_forward_config;
typedef struct mc_port_config { unsigned type; char path[MC_CONFIG_PATH]; } mc_port_config;
/* sharing: the file server on the Ethernet (docs/NOTES.md, "Sharing"). */
#define MC_CONFIG_SHARES 8
#define MC_CONFIG_SHARE_NAME 64        /* UTF-8 bytes with the NUL */
enum { MC_SHARING_STREAMS, MC_SHARING_APPLEDOUBLE };
typedef struct mc_share_config {
    char name[MC_CONFIG_SHARE_NAME];   /* the volume's name on the Macintosh */
    char path[MC_CONFIG_PATH];
    int readonly;
} mc_share_config;

enum { MC_LOOK_PLAIN, MC_LOOK_CRT, MC_LOOK_LCD };
enum { MC_MONITOR_COLOR, MC_MONITOR_MONO, MC_MONITOR_LCD_GRAY };
enum { MC_CONFIG_FILTER_NEAREST, MC_CONFIG_FILTER_LINEAR, MC_CONFIG_FILTER_SHARP };
enum { MC_CONFIG_SCALE_FIT, MC_CONFIG_SCALE_INTEGER };
/* The ADB keyboard the Macintosh sees. */
enum { MC_KEYBOARD_ANSI, MC_KEYBOARD_ISO };
/* The host key that acts as Command (input.keyboard.command; the other
 * Alt keys are Option, the other Control keys Control). The Windows keys
 * open the configuration window. */
enum { MC_COMMAND_LEFT_ALT, MC_COMMAND_LEFT_CONTROL, MC_COMMAND_RIGHT_CONTROL = 3, MC_COMMAND_RIGHT_ALT };
/* What a host mouse button does. */
enum { MC_MOUSE_DISABLED, MC_MOUSE_NORMAL, MC_MOUSE_CONTROL_CLICK };
enum { MC_BOOT_DISK, MC_BOOT_CDROM };
/* video.card: which display shows the screen (docs/NOTES.md). */
enum { MC_VIDEO_CARD_AUTO, MC_VIDEO_CARD_BUILTIN, MC_VIDEO_CARD_LIVADIX };
/* emulation.controller: how Egret, Cuda (and the PMU) run (docs/NOTES.md). */
enum { MC_CONTROLLER_AUTO, MC_CONTROLLER_PROTOCOL, MC_CONTROLLER_FIRMWARE };
/* emulation.close_action: what closing the window does (docs/NOTES.md). */
enum { MC_CLOSE_SHUTDOWN, MC_CLOSE_QUIT };

typedef struct mc_config {
    /* model */
    uint32_t model;                    /* index in models.c */
    uint32_t model_keys;               /* keys the file wrote, which win over the model's */
    /* emulation */
    char rom[MC_CONFIG_PATH];
    int rom_explicit, rom_scanned;     /* written as rom:, or found by checksum under another name */
    uint32_t ram_mb;                   /* whole MiB (of a size that is not: the MiB below it) */
    uint32_t ram_kb;                   /* a size that is not a whole number of MiB, in KiB (ram_mb: 2.5
                                          gives 2560 here, 2 in ram_mb); 0 otherwise. mc_config_ram_kb */
    uint64_t cpu_hz;                   /* emulation.cpu_mhz, in Hz */
    int jit, startup_sound;
    int fpu_full;                      /* a 68040's FPU runs the whole 68881 set itself */
    int m68k_accelerator;              /* a Power Macintosh's 68k code on Livadix's 68k core */
    int charger_start;                 /* the PowerBook 100 started by its charger (its battery-charging screen) */
    int pmmu;                          /* a 68851 PMMU in the Macintosh II's socket (in place of the AMU) */
    uint32_t controller;               /* MC_CONTROLLER_* */
    uint32_t close_action;             /* MC_CLOSE_* */
    uint32_t close_timeout;            /* seconds a shutdown may take before Livadix quits anyway */
    char nvram[MC_CONFIG_PATH];
    int nvram_private;                /* internal: board sees a private session file; never a YAML key */
    char nvram_id[33];                /* persistent profile identity, independent of its display name */
    /* video */
    uint32_t width, height, depth, refresh;
    uint32_t look, monitor, filter, scale;
    int fullscreen, vsync;
    uint32_t video_card;               /* MC_VIDEO_CARD_* */
    int video_acceleration;            /* the Livadix Display Card's blitter and its QuickDraw acceleration */
    /* storage: floppy entries without an image are empty drives; floppy_off
     * marks a drive that is not fitted */
    mc_disk_config disks[MC_CONFIG_DISKS];
    size_t disk_count;
    mc_disk_config floppies[MC_CONFIG_FLOPPIES];
    size_t floppy_count;
    int floppy_off[MC_CONFIG_FLOPPIES];
    char cdrom[MC_CONFIG_PATH];
    int cdrom_bus;                     /* storage.cdrom bus, MC_BUS_*: ata is the PowerBook 190's expansion-bay ATAPI drive */
    uint32_t boot_priority;
    /* sound */
    int sound, floppy_sound;
    /* input */
    uint32_t keyboard_layout, command_key, mouse[2];
    /* ports */
    mc_port_config ports[2];
    /* network */
    uint32_t network;                  /* MC_NETWORK_* */
    uint32_t network_card;             /* MC_NET_CARD_* */
    mc_forward_config forwards[MC_CONFIG_FORWARDS];
    size_t forward_count;
    /* sharing */
    char sharing_name[MC_CONFIG_SHARE_NAME];
    uint32_t sharing_metadata;         /* MC_SHARING_* */
    char sharing_password[16];
    mc_share_config shares[MC_CONFIG_SHARES];
    size_t share_count;
    /* the file this was read from, and the profile (empty when the file
     * has no profiles:) */
    char source[MC_CONFIG_PATH];
    char profile[MC_CONFIG_NAME];
    /* the roms folders' index mc_config_find_rom used (romdb.h); firmware
     * and declaration ROMs are found through it */
    const struct mc_rom_index *roms;
} mc_config;

/* The screen sizes video.resolution accepts and the depths video.depth does. */
#define MC_CONFIG_RESOLUTIONS 12
extern const uint16_t mc_config_resolutions[MC_CONFIG_RESOLUTIONS][2];
#define MC_CONFIG_DEPTHS 6
extern const uint8_t mc_config_depths[MC_CONFIG_DEPTHS];
/* Video memory of the display that shows any configured mode (the Livadix
 * Display Card). */
#define MC_CONFIG_VRAM (2u * 1024u * 1024u)

/* The profiles of a file: their names in order, each one's model key
 * ("auto" when it names none) and the default's index. count 0: the file
 * has no profiles:, its top level is the one configuration. */
typedef struct mc_profile_list {
    size_t count;
    int default_index;
    char names[MC_CONFIG_PROFILES][MC_CONFIG_NAME];
    char models[MC_CONFIG_PROFILES][64];
} mc_profile_list;

void mc_config_defaults(mc_config *config);
/* Stable profile identity, generated when absent; no filesystem changes. */
void mc_config_nvram_identity(mc_config *config);
/* Fresh identity for a new or copied profile; 0 if host randomness fails. */
int mc_config_new_nvram_identity(mc_config *config);
/* Reads a file and the configuration of its default profile (default:,
 * else the first under profiles:; a file without profiles: is one
 * configuration). Every profile is checked; errors name the line (and the
 * profile). */
int mc_config_load(const char *path, mc_config *config, char *error, size_t capacity);
/* The same for the profile named profile (NULL: the default). */
int mc_config_load_profile(const char *path, const char *profile, mc_config *config, char *error, size_t capacity);
/* The same from text; source is the file it stands for (relative paths). */
int mc_config_parse(const char *text, size_t length, const char *source, const char *profile, mc_config *config, char *error,
                    size_t capacity);
/* The profiles of a file, checked as mc_config_load does. */
int mc_config_profiles(const char *path, mc_profile_list *list, char *error, size_t capacity);
int mc_config_profiles_text(const char *text, size_t length, const char *source, mc_profile_list *list, char *error, size_t capacity);
/* The roms folders of a loaded configuration, each with its separator:
 * roms/ in program_folder (the executable's, with its separator; NULL for
 * none), then roms/ beside the configuration. Returns how many (1 or 2). */
size_t mc_config_rom_folders(const mc_config *config, const char *program_folder, char folders[2][MC_CONFIG_PATH]);
/* Settles the ROM file of a loaded configuration from the index of its roms
 * folders (romdb.h, mc_rom_index_scan): a rom: file must hold the model's
 * ROM; without one, the index's file of that ROM, identified by its
 * checksum whatever its name (rom_scanned when it is not the standard
 * name). The Built-in model uses the embedded image when no rom: is given,
 * leaving rom empty; an explicit file is still checked normally. */
/* mc_config_parse without the rules a model imposes (its maximum memory,
 * its screens and monitors, its drives): what the keys and the model give,
 * for comparing (profile.c). */
int mc_config_parse_values(const char *text, size_t length, const char *source, const char *profile, mc_config *out, char *error,
                           size_t capacity);
int mc_config_find_rom(mc_config *config, const struct mc_rom_index *roms, char *error, size_t capacity);
struct mc_model;

/* emulation.cpu_mhz: a decimal number of MHz from 1 to 10000 with up to six
 * decimals (Hz), as Hz; 0 when the text is not one. */
#define MC_CONFIG_MIN_HZ 1000000ull
#define MC_CONFIG_MAX_HZ 10000000000ull
uint64_t mc_config_parse_mhz(const char *text);
/* Hz as the MHz text the file takes: "33", "33.33", "15.6672". */
const char *mc_config_mhz_text(uint64_t hz, char *buffer, size_t capacity);

/* emulation.ram_mb: the memory in KiB (ram_kb when it belongs to ram_mb,
 * else ram_mb's whole MiB); setting it keeps both fields together. */
uint32_t mc_config_ram_kb(const mc_config *config);
void mc_config_set_ram_kb(mc_config *config, uint32_t kb);
/* emulation.ram_mb as the file writes it: a whole number of MiB up to
 * 1024, or one with up to three decimals that is a whole number of KiB
 * ("2.5"); returns the KiB, 0 when the text is not one. */
uint32_t mc_config_parse_ram(const char *text);
/* KiB as that text: "4", "2.5". */
const char *mc_config_ram_text(uint32_t kb, char *buffer, size_t capacity);

/* Names of the choice values, for messages and --inspect. */
const char *mc_config_look_name(uint32_t look);
const char *mc_config_monitor_name(uint32_t monitor);
/* Whether the model's real hardware offered the monitor at this video
 * mode (docs/NOTES.md, "video.monitor"). When not, returns 0 and
 * writes the reason to why. */
int mc_config_monitor_check(const struct mc_model *model, unsigned width, unsigned height, unsigned depth, uint32_t monitor,
                             char *why, size_t capacity);
/* The looks the model's real screen allows: plain always, crt for a model
 * with a CRT (built in, or the monitor of a modular Mac), lcd for a
 * PowerBook's panel. mc_config_look_fit returns look when allowed, else
 * the model's own screen's look. */
int mc_config_look_ok(const struct mc_model *model, uint32_t look);
uint32_t mc_config_look_fit(const struct mc_model *model, uint32_t look);
const char *mc_config_filter_name(uint32_t filter);
const char *mc_config_scale_name(uint32_t scale);
const char *mc_config_video_card_name(uint32_t card);
const char *mc_config_controller_name(uint32_t controller);
const char *mc_config_network_name(uint32_t network);
const char *mc_config_network_card_name(uint32_t card);
/* A forward as network.forward writes it ("tcp:8080:80"; the address only
 * when it is not 127.0.0.1). */
void mc_config_forward_text(const mc_forward_config *f, char *text, size_t capacity);
/* Parses one network.forward entry: "tcp:HOSTPORT:GUESTPORT", or with the
 * host address "udp:0.0.0.0:HOSTPORT:GUESTPORT". 0 with a message. */
int mc_config_parse_forward(const char *text, mc_forward_config *f, char *error, size_t capacity);
const char *mc_config_close_action_name(uint32_t action);
const char *mc_config_sharing_metadata_name(uint32_t metadata);

#endif /* MC_CONFIG_H */
