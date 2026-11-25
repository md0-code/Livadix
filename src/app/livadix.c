/* livadix.exe: the command line, the configuration, the ROM and the media, then
 * the window (desktop.c). */
#include "../core/config.h"
#include "../core/disk.h"
#include "../core/machine.h"
#include "../core/models.h"
#include "../core/romdb.h"
#include "../core/compat.h"
#include "../core/hfs.h"
#include "desktop.h"
#include "livadix_default_yaml.h"
#include "mc_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#else
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <SDL.h>
#endif

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *data = NULL;
    long length = -1;
    if (!fseek(f, 0, SEEK_END)) length = ftell(f);
    if (length > 0 && length <= 64L * 1024 * 1024 && !fseek(f, 0, SEEK_SET) && (data = malloc((size_t)length)) != NULL &&
        fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data) *size = (size_t)length;
    return data;
}

/* Without --config, use livadix.yaml when present, else the embedded
 * template in memory. The configuration window's Save writes it later. */
static const char *default_configuration(int *embedded) {
    static const char name[] = "livadix.yaml";
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesA(name);
    *embedded = attributes == INVALID_FILE_ATTRIBUTES &&
                (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND);
#else
    struct stat st;
    *embedded = stat(name, &st) != 0 && (errno == ENOENT || errno == ENOTDIR);
#endif
    return name;
}

static void list_models(void) {
    printf("%-31s %-32s %-8s %-30s %7s %7s %-14s %-11s %7s %s\n", "model", "name", "ROM", "CPU", "MHz", "RAM MiB",
           "display", "look", "gestalt", "status");
    for (size_t i = 0; i < mc_model_count(); ++i) {
        const mc_model *p = mc_model_entry(i);
        char ram[16], display[24], look[24], mhz[24];
        if (p->ram_kb) snprintf(ram, sizeof ram, "%uK", (unsigned)p->ram_kb);
        else snprintf(ram, sizeof ram, "%u-%u", (unsigned)p->ram_mb, (unsigned)p->ram_max_mb);
        snprintf(display, sizeof display, "%ux%ux%u", (unsigned)p->width, (unsigned)p->height, (unsigned)p->depth);
        snprintf(look, sizeof look, "%s %s", mc_config_look_name(p->look), mc_config_monitor_name(p->monitor));
        printf("%-31s %-32s %08X %-30s %7s %7s %-14s %-11s %7u %s\n", p->key, p->name, (unsigned)p->rom, p->cpu,
               mc_config_mhz_text(p->cpu_hz, mhz, sizeof mhz), ram, display, look, (unsigned)p->gestalt,
               mc_machine_emulated(p) ? "emulated" : "not yet");
    }
}

/* The roms folders' files and what each was recognised as. */
static void print_rom_files(const mc_rom_index *ix) {
    char what[512];
    for (size_t k = 0; k < ix->folder_count; ++k) {
        const size_t length = strlen(ix->folders[k]);
        size_t n = 0;
        for (size_t i = 0; i < ix->count; ++i) n += !strncmp(ix->files[i].path, ix->folders[k], length);
        printf("ROM folder %s: %zu file(s)\n", ix->folders[k], n);
        for (size_t i = 0; i < ix->count; ++i)
            if (!strncmp(ix->files[i].path, ix->folders[k], length))
                printf("  %s: %s\n", ix->files[i].name, mc_rom_file_describe(ix, &ix->files[i], what, sizeof what));
    }
}

/* The known images with their standard names, then the files of the roms
 * folders (beside livadix.exe and beside the configuration). */
static void list_roms(const mc_rom_index *ix) {
    printf("%-24s %-8s %8s  %-7s %s\n", "standard name", "checksum", "size KiB", "date", "models");
    for (size_t i = 0; i < mc_romdb_count(); ++i) {
        const mc_known_rom *r = mc_romdb_entry(i);
        printf("%-24s %08X %8u  %-7s %s\n", r->file, (unsigned)r->checksum, (unsigned)(r->size / 1024), r->date, r->models);
    }
    printf("\n%-24s %-8s %8s  %s\n", "standard name", "CRC32", "bytes", "part (machines)");
    for (size_t i = 0; i < mc_firmware_count(); ++i) {
        const mc_firmware *f = mc_firmware_entry(i);
        printf("%-24s %08x %8u  %s (%s)\n", f->file, (unsigned)f->crc32, (unsigned)f->size, f->part, f->models);
    }
    printf("\n");
    print_rom_files(ix);
}

/* The host's local time in seconds since 1 January 1904, the Macintosh
 * epoch (2,082,844,800 seconds before 1970). */
static int64_t host_mac_time(void) {
    const time_t now = time(NULL);
    struct tm local = *localtime(&now), utc = *gmtime(&now);
    utc.tm_isdst = local.tm_isdst;
    const int64_t offset = (int64_t)difftime(mktime(&local), mktime(&utc));
    return (int64_t)now + offset + 2082844800ll;
}

/* The profiles of a file, the default marked. */
static int list_profiles(const char *yaml, int embedded) {
    mc_profile_list list;
    char error[1024];
    const int ok = embedded ? mc_config_profiles_text((const char *)mc_default_yaml, sizeof mc_default_yaml, yaml,
                                                      &list, error, sizeof error)
                            : mc_config_profiles(yaml, &list, error, sizeof error);
    if (!ok) { fprintf(stderr, "%s: %s\n", yaml, error); return 2; }
    if (!list.count) { printf("%s has no profiles: its settings are one configuration\n", yaml); return 0; }
    printf("  %-48s %s\n", "profile (* the default)", "model");
    for (size_t i = 0; i < list.count; ++i)
        printf("%c %-48s %s\n", (int)i == list.default_index ? '*' : ' ', list.names[i], list.models[i]);
    return 0;
}

/* The command line's argument i as UTF-8 (a profile's name may hold any
 * character); NULL when it cannot be had. */
static const char *argument_utf8(int i, char *buffer, size_t capacity) {
#ifdef _WIN32
    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    const char *result = NULL;
    if (wide && i < count && WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, buffer, (int)capacity, NULL, NULL)) result = buffer;
    if (wide) LocalFree(wide);
    return result;
#else
    /* argv holds the bytes as typed, UTF-8 in a UTF-8 locale: the caller uses it. */
    (void)i;
    (void)buffer;
    (void)capacity;
    return NULL;
#endif
}

#ifdef _WIN32
#define HOST_MENU_KEY "the Windows key"
#else
#define HOST_MENU_KEY "the Super key"
#endif

static int usage(void) {
    printf("Livadix %s, an emulator of 68k and PowerPC Macintosh hardware that runs Apple's own ROMs and system software\n\n"
           "livadix [--config FILE.yaml] [--profile NAME] [--hidden] [--frames N] [--screenshot FILE.bmp] [--unthrottled]\n"
           "     [--wav FILE.wav] [--close-after N[,N...]] [--key CODE@N]... [--script FILE]\n"
           "livadix --inspect [--config FILE.yaml] [--profile NAME]\n"
           "livadix --list-profiles [--config FILE.yaml] | --list-models | --list-roms [--config FILE.yaml] | --version\n\n"
           "Without --config, livadix.yaml in the working directory is used; when missing, defaults run in memory.\n"
           "--profile starts that profile of the file instead of its default: one (else its first).\n"
           "--inspect checks the configuration, ROM and media without starting the Macintosh.\n"
           "--hidden runs without a visible window; --frames N stops after N frames; --screenshot saves the last one.\n"
           "--unthrottled runs as fast as the host can; --wav records the sound to a file instead of playing it.\n"
           "--close-after N closes the window after N frames, as its close button does (more counts: closed again);\n"
           "--key CODE@N sends ADB key CODE (hex, + 80 for a release) after N frames.\n"
           "--script FILE drives the configuration window for checks.\n"
           "Closing the window shuts the Macintosh down first (emulation.close_action); closing again quits at once.\n"
           "In the window: " HOST_MENU_KEY " opens the configuration window (the Macintosh pauses); Left Alt is\n"
           "Command, Right Alt Option; click to capture the mouse, Ctrl+Alt releases it; Alt+Enter toggles full screen;\n"
           "drop a floppy or CD image on the window to insert it; Ctrl+Alt+R reinserts an ejected disk;\n"
           "Ctrl+Alt+K turns a Quadra 900/950's keyswitch, Ctrl+Alt+F1/F2 brightness, F3/F4 contrast, F5/F6 volume.\n", MC_VERSION);
    return 0;
}

/* What an image holds, in words (hfs.h). */
static const char *contents(const char *path) {
    static char text[512];
    mc_volume_info info;
    char error[256];
    if (!mc_volume_inspect(path, &info, error, sizeof error)) snprintf(text, sizeof text, "%s", error);
    else mc_volume_describe(&info, text, sizeof text);
    return text;
}

/* The startup disk and whether the model runs its System: printed, never
 * blocking (docs/NOTES.md, "Compatibility"). */
static void print_compatibility(const mc_config *c, const mc_model *model) {
    if (model->console) {       /* it starts the disc in its drive, whatever System that holds */
        if (c->cdrom[0]) printf("Startup disc: %s\n", c->cdrom);
        else puts("Startup disc: none; the console starts from a CD (storage.cdrom)");
        return;
    }
    if (mc_model_builtin(model)) { puts("Startup: Livadix's own ROM (no System): a desktop with the README and the changelog"); return; }
    mc_volume_info boot;
    const char *path = mc_config_boot_image(c, &boot);
    if (!path) {
        mc_volume_info unread;
        char text[512];
        const char *hfsplus = mc_config_unexamined_image(c, &unread);
        if (hfsplus) printf("Startup disk: %s; %s; compatibility not checked\n", hfsplus, mc_volume_describe(&unread, text, sizeof text));
        else puts("Startup disk: none of the images holds a System Folder");
        return;
    }
    char text[512];
    printf("Startup disk: %s; %s\n", path, mc_volume_describe(&boot, text, sizeof text));
    mc_compat compat;
    if (mc_compat_check(model, &boot, mc_config_ram_kb(c), &compat) == MC_COMPAT_OK) printf("Compatibility: OK: %s\n", compat.reason);
    else if (compat.status == MC_COMPAT_UNKNOWN) printf("Compatibility: not checked: %s\n", compat.reason);
    else printf("Compatibility warning: %s\n", compat.reason);
}

/* Opens every configured image (read-only for inspection) into the machine,
 * or, without one, only checks and prints them. */
static int attach_media(const mc_config *c, mc_machine *m, int print, char *error, size_t capacity) {
    char problem[256];
    for (size_t i = 0; i < c->disk_count; ++i) {
        mc_disk *d = mc_disk_open(c->disks[i].image, m ? !c->disks[i].readonly : 0, 512, problem, sizeof problem);
        if (!d) { snprintf(error, capacity, "%s: %s", c->disks[i].image, problem); return 0; }
        if (print) printf("Disk %zu: %s, %llu sectors, %s; %s\n", i + 1, c->disks[i].image, (unsigned long long)mc_disk_sectors(d),
                          c->disks[i].readonly ? "read-only" : "writable", contents(c->disks[i].image));
        if (!m) mc_disk_close(d);
        else if (!mc_machine_insert(m, MC_MEDIA_DISK, (unsigned)i, d, problem, sizeof problem)) {
            mc_disk_close(d); snprintf(error, capacity, "%s: %s", c->disks[i].image, problem); return 0;
        }
    }
    unsigned drive = 0;
    for (size_t i = 0; i < c->floppy_count; ++i) {
        if (c->floppy_off[i]) { if (print) printf("Floppy %zu: not fitted\n", i + 1); continue; }
        unsigned index = drive++;
        if (!c->floppies[i].image[0]) { if (print) printf("Floppy %zu: empty drive\n", i + 1); continue; }
        mc_disk *d = mc_disk_open(c->floppies[i].image, m ? !c->floppies[i].readonly : 0, 512, problem, sizeof problem);
        if (!d) { snprintf(error, capacity, "%s: %s", c->floppies[i].image, problem); return 0; }
        if (!mc_disk_floppy(d)) {
            mc_disk_close(d);
            snprintf(error, capacity, "%s: a floppy image is 400, 720, 800 or 1440 KiB, raw or DiskCopy 4.2", c->floppies[i].image);
            return 0;
        }
        if (print) printf("Floppy %zu: %s, %llu sectors, %s; %s\n", i + 1, c->floppies[i].image, (unsigned long long)mc_disk_sectors(d),
                          c->floppies[i].readonly ? "read-only" : "writable", contents(c->floppies[i].image));
        if (!m) mc_disk_close(d);
        else if (!mc_machine_insert(m, MC_MEDIA_FLOPPY, index, d, problem, sizeof problem)) {
            mc_disk_close(d); snprintf(error, capacity, "%s: %s", c->floppies[i].image, problem); return 0;
        }
    }
    if (c->cdrom[0]) {
        mc_disk *d = mc_disk_open(c->cdrom, 0, 2048, problem, sizeof problem);
        if (!d) { snprintf(error, capacity, "%s: %s", c->cdrom, problem); return 0; }
        if (print) printf("CD-ROM: %s, %llu sectors of 2048 bytes, read-only%s; %s\n", c->cdrom, (unsigned long long)mc_disk_sectors(d),
                          c->cdrom_bus == MC_BUS_ATA ? ", on the ATA bus" : "", contents(c->cdrom));
        if (!m) mc_disk_close(d);
        else if (!mc_machine_insert(m, MC_MEDIA_CDROM, 0, d, problem, sizeof problem)) {
            mc_disk_close(d); snprintf(error, capacity, "%s: %s", c->cdrom, problem); return 0;
        }
    } else if (print) printf("CD-ROM: empty drive%s\n", c->cdrom_bus == MC_BUS_ATA ? " on the ATA bus" : "");
    return 1;
}

/* The ROM of a configuration (found in the roms folders' index, read and
 * checked) with what it is printed; NULL with the reason. */
static uint8_t *load_rom(mc_config *config, const mc_rom_index *roms, size_t *size, char *error, size_t capacity) {
    if (!mc_config_find_rom(config, roms, error, capacity)) return NULL;
    const int embedded = mc_model_builtin(mc_model_entry(config->model)) && !config->rom_explicit;
    uint8_t *rom;
    if (embedded) {
        const uint8_t *data = mc_rom_builtin(size);
        rom = malloc(*size);
        if (rom) memcpy(rom, data, *size);
    } else rom = read_file(config->rom, size);
    if (!rom) { snprintf(error, capacity, embedded ? "Out of memory" : "Cannot read ROM %s", config->rom); return NULL; }
    mc_rom_info info;
    const int valid = mc_rom_validate(rom, *size, &info);
    const mc_known_rom *known = mc_romdb_identify(rom, *size);
    printf("ROM: %s%s\n", embedded ? "embedded in livadix.exe" : config->rom, config->rom_scanned ? " (found by its checksum)" : "");
    printf("ROM checksum: %08X stored, %08X computed; %s; %s ROM (%s), %u KiB\n", (unsigned)info.stored_checksum,
           (unsigned)info.computed_checksum, valid ? "valid" : "INVALID", known ? known->date : "unknown",
           known ? known->models : "?", (unsigned)(*size / 1024));
    if (!valid) {
        snprintf(error, capacity, "The ROM image is damaged: its checksum does not match its contents");
        free(rom);
        return NULL;
    }
    const mc_model *model = mc_model_entry(config->model);
    printf("Model: %s (%s), %s, Gestalt machine type %u; %s\n", model->key, model->name, model->cpu,
           (unsigned)model->gestalt, mc_machine_emulated(model) ? "emulated" : "not emulated yet (test pattern)");
    print_compatibility(config, model);
    return rom;
}

/* The machine of a configuration with its media (mc_desktop_session's
 * build): at the first start and at every start from the configuration
 * window. */
typedef struct session_ctx {
    const mc_rom_index *roms;
    mc_config built;                   /* the configuration as the last machine was built (its ROM settled) */
} session_ctx;
static mc_machine *build(void *ctx, const mc_config *config, char *error, size_t capacity) {
    session_ctx *x = ctx;
    mc_config c = *config;
    size_t size = 0;
    uint8_t *rom = load_rom(&c, x->roms, &size, error, capacity);
    if (!rom) return NULL;
    mc_machine *machine = mc_machine_create(&c, rom, size, error, capacity);
    free(rom);
    if (!machine) return NULL;
    mc_machine_set_clock(machine, host_mac_time());
    if (!attach_media(&c, machine, 0, error, capacity)) { mc_machine_destroy(machine); return NULL; }
    x->built = c;
    return machine;
}

/* --inspect: the configuration, its ROM and every image, printed. */
static int inspect_configuration(mc_config *c, const mc_rom_index *roms, const char *yaml) {
    char error[1024];
    size_t size = 0;
    uint8_t *rom = load_rom(c, roms, &size, error, sizeof error);
    if (!rom) {
        /* A ROM that is not found names the file; one that cannot be read or is damaged speaks for itself. */
        if (!strncmp(error, "The ROM image", 13) || !strncmp(error, "Cannot read ROM", 15)) fprintf(stderr, "%s\n", error);
        else fprintf(stderr, "%s: %s\n", yaml, error);
        return 1;
    }
    const int accelerator_known = mc_machine_m68k_accelerator_known(rom, size);
    free(rom);
    const mc_model *model = mc_model_entry(c->model);
    /* The FPU the model has: none, the 68881 beside a 68020, the 68882
     * beside a 68030, the 68040's own (as the chip, or the full set), the
     * PowerPC's own. */
    const int c040 = !strncmp(model->cpu, "68040", 5), c020 = !strncmp(model->cpu, "68020", 5);
    const char *fpu = mc_model_ppc(model) ? "the PowerPC's own"
                      : !model->fpu || !strncmp(model->cpu, "68LC040", 7) ? "none"
                      : c040 ? (c->fpu_full ? "the 68040's own, the full 68881 set" : "the 68040's own, as the chip")
                      : c020 ? "68881" : "68882";
    char mhz[24], real[24];
    mc_config_mhz_text(c->cpu_hz, mhz, sizeof mhz);
    mc_config_mhz_text(model->cpu_hz, real, sizeof real);
    char ram[32], ram_text[24];
    if (model->ram_kb) snprintf(ram, sizeof ram, "%u KiB", (unsigned)model->ram_kb);
    else snprintf(ram, sizeof ram, "%s MiB", mc_config_ram_text(mc_config_ram_kb(c), ram_text, sizeof ram_text));
    printf("Emulation: %s RAM, CPU at %s MHz (the %s's clock: %s MHz), JIT %s, startup sound %s, FPU %s; closing the window: %s",
           ram, mhz, model->name, real, (!strncmp(model->cpu, "68000", 5) || !strncmp(model->cpu, "68HC000", 7)) ? "off (the 68000 runs on the interpreter)" : c->jit ? "on" : "off", c->startup_sound ? "on" : "off",
           fpu, mc_config_close_action_name(c->close_action));
    if (c->close_action == MC_CLOSE_SHUTDOWN) printf(" (at most %u s)", (unsigned)c->close_timeout);
    printf("\n");
    if (c->m68k_accelerator)
        printf("68k accelerator: %s\n", !mc_model_ppc(model) ? "not used (a 68k Macintosh)"
                                         : !c->jit ? "not used (it needs emulation.jit)"
                                         : accelerator_known ? "on" : "not available (this ROM's 68k emulator is not one Livadix knows)");
    {
        char list[48] = "", why[400];
        for (uint32_t k = 0; k < 3; ++k)
            if (mc_config_monitor_check(model, c->width, c->height, c->depth, k, why, sizeof why))
                snprintf(list + strlen(list), sizeof list - strlen(list), "%s%s", list[0] ? ", " : "", mc_config_monitor_name(k));
        printf("Monitors this model allows at this video mode: %s\n", list);
    }
    printf("Video: %ux%u, %u bits, %u Hz, card %s; %s look, %s; %s filter, %s scale; %s; vsync %s\n", (unsigned)c->width,
           (unsigned)c->height, (unsigned)c->depth, (unsigned)c->refresh, mc_config_video_card_name(c->video_card),
           mc_config_look_name(c->look), mc_config_monitor_name(c->monitor), mc_config_filter_name(c->filter),
           mc_config_scale_name(c->scale), c->fullscreen ? "full screen" : "windowed", c->vsync ? "on" : "off");
    if (!attach_media(c, NULL, 1, error, sizeof error)) { fprintf(stderr, "%s\n", error); return 1; }
    printf("Boot priority: %s; sound %s\n", c->boot_priority == MC_BOOT_CDROM ? "cdrom" : "disk", c->sound ? "on" : "off");
    printf("Network: %s", mc_config_network_name(c->network));
    if (c->network == MC_NETWORK_NAT) printf(" (the Macintosh at 10.0.2.15, router and host 10.0.2.2, DNS 10.0.2.3)");
    for (size_t k = 0; c->network == MC_NETWORK_NAT && k < c->forward_count; ++k) {
        char text[48];
        mc_config_forward_text(&c->forwards[k], text, sizeof text);
        printf("%s%s", k ? ", " : "; forwards ", text);
    }
    printf("; card %s\n", mc_config_network_card_name(c->network_card));
    for (size_t k = 0; k < c->share_count; ++k)
        printf("%s%s (%s%s)", k ? ", " : "Sharing: ", c->shares[k].name, c->shares[k].path, c->shares[k].readonly ? ", read-only" : "");
    if (c->share_count)
        printf(" as \"%s\", metadata %s, %s%s\n", c->sharing_name, mc_config_sharing_metadata_name(c->sharing_metadata),
               c->sharing_password[0] ? "with a password" : "guests", c->network == MC_NETWORK_NAT ? "" : " (needs network.mode nat)");
    puts("Inspection passed.");
    return 0;
}

static int livadix_main(int argc, char **argv) {
    int inspect = 0, roms_list = 0, profiles_list = 0, config_failed = 0, embedded_config = 0;
    const char *yaml = NULL, *profile = NULL, *script = NULL;
    char profile_name[MC_CONFIG_NAME * 4];
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    mc_desktop_options options;
    memset(&options, 0, sizeof options);
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--inspect")) inspect = 1;
        else if (!strcmp(argv[i], "--list-models")) { list_models(); return 0; }
        else if (!strcmp(argv[i], "--list-roms")) roms_list = 1;
        else if (!strcmp(argv[i], "--version")) { printf("Livadix %s\n", MC_VERSION); return 0; }
        else if (!strcmp(argv[i], "--help")) return usage();
        else if (!strcmp(argv[i], "--config") && i + 1 < argc) yaml = argv[++i];
        else if (!strcmp(argv[i], "--profile") && i + 1 < argc) {
            ++i;
            profile = argument_utf8(i, profile_name, sizeof profile_name);
            if (!profile) profile = argv[i];
        }
        else if (!strcmp(argv[i], "--list-profiles")) profiles_list = 1;
        else if (!strcmp(argv[i], "--hidden")) options.hidden = 1;
        else if (!strcmp(argv[i], "--script") && i + 1 < argc) script = argv[++i];
        else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) options.screenshot = argv[++i];
        else if (!strcmp(argv[i], "--unthrottled")) options.unthrottled = 1;
        else if (!strcmp(argv[i], "--wav") && i + 1 < argc) options.wav = argv[++i];
        else if (!strcmp(argv[i], "--close-after") && i + 1 < argc) {
            char *at = argv[++i], *end = NULL;
            options.close_count = 0;
            do {
                const uint64_t n = strtoull(at, &end, 10);
                if (!n || end == at || options.close_count == 4 || (*end && *end != ',')) {
                    fputs("--close-after needs up to four positive whole numbers, separated by commas\n", stderr);
                    return 2;
                }
                options.close_at[options.close_count++] = n;
                at = end + 1;
            } while (*end);
        }
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) {
            unsigned code = 0;
            unsigned long long at = 0;
            char tail = 0;
            if (options.key_count == 8 || sscanf(argv[++i], "%x@%llu%c", &code, &at, &tail) != 2 || code > 0xFF || !at) {
                fputs("--key needs CODE@N: an ADB key code in hex (+ 80 for a release) and a frame count (at most eight)\n", stderr);
                return 2;
            }
            options.key_at[options.key_count] = at;
            options.key_code[options.key_count++] = (uint8_t)code;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            char *end = NULL;
            options.frames = strtoull(argv[++i], &end, 10);
            if (!options.frames || !end || *end) { fputs("--frames needs a positive whole number\n", stderr); return 2; }
        } else { fprintf(stderr, "Unknown or incomplete argument: %s (livadix --help)\n", argv[i]); return 2; }
    }
    if (inspect && (options.hidden || options.frames || options.screenshot || options.unthrottled || options.wav || options.close_count ||
                    options.key_count || script)) {
        fputs("--inspect starts no window: --hidden, --frames, --screenshot, --unthrottled, --wav, --close-after, --key and --script do not apply\n",
              stderr);
        return 2;
    }
    char error[1024] = "";
    if (profiles_list) {
        if (!yaml) yaml = default_configuration(&embedded_config);
        return list_profiles(yaml, embedded_config);
    }
    char program_folder[1024] = "";
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, program_folder, sizeof program_folder);
    if (!n || n >= sizeof program_folder) program_folder[0] = 0;
#else
    const ssize_t n = readlink("/proc/self/exe", program_folder, sizeof program_folder);
    if (n <= 0 || (size_t)n >= sizeof program_folder) program_folder[0] = 0;
    else program_folder[n] = 0;
#endif
    char *slash = strrchr(program_folder, '\\'), *forward = strrchr(program_folder, '/');
    if (forward && (!slash || forward > slash)) slash = forward;
    if (slash) slash[1] = 0; else program_folder[0] = 0;
    mc_config config;
    if (roms_list) {
        /* The configuration only places the second roms folder. */
        mc_config_defaults(&config);
        snprintf(config.source, sizeof config.source, "%s", yaml ? yaml : "livadix.yaml");
    } else {
        if (!yaml) yaml = default_configuration(&embedded_config);
        const int loaded = embedded_config ? mc_config_parse((const char *)mc_default_yaml, sizeof mc_default_yaml,
                                                             yaml, profile, &config, error, sizeof error)
                                           : mc_config_load_profile(yaml, profile, &config, error, sizeof error);
        if (!loaded) {
            /* A window shows the configuration window with the problem;
             * checks and scripts get the message and the exit code. */
            if (inspect || (options.hidden && !script)) { fprintf(stderr, "%s: %s\n", yaml, error); return 2; }
            config_failed = 1;
            char problem[1200];
            snprintf(problem, sizeof problem, "%s: %s", yaml, error);
            snprintf(error, sizeof error, "%s", problem);
            mc_config_defaults(&config);
            snprintf(config.source, sizeof config.source, "%s", yaml);
        }
        if (config.profile[0]) printf("Profile: %s\n", config.profile);
    }
    /* The roms folders, scanned once: every file identified by its content. */
    char folders[2][MC_CONFIG_PATH];
    const size_t folder_count = mc_config_rom_folders(&config, program_folder[0] ? program_folder : NULL, folders);
    const char *folder_list[2] = {folders[0], folder_count > 1 ? folders[1] : NULL};
    static mc_rom_index roms;
    if (!mc_rom_index_scan(&roms, folder_list, folder_count)) { fputs("Out of memory\n", stderr); return 1; }
    if (roms_list) { list_roms(&roms); mc_rom_index_free(&roms); return 0; }
    if (inspect) {
        print_rom_files(&roms);
        const int status = inspect_configuration(&config, &roms, yaml);
        mc_rom_index_free(&roms);
        return status;
    }
    /* The Macintosh, or the configuration window saying why it cannot start. */
    static session_ctx ctx;
    ctx.roms = &roms;
    mc_machine *machine = NULL;
    int code = 2;
    if (!config_failed) {
        code = 1;
        machine = build(&ctx, &config, error, sizeof error);
        if (machine) config = ctx.built;
    }
    if (!machine && options.hidden && !script) {
        fprintf(stderr, "%s\n", error);
        mc_rom_index_free(&roms);
        return code;
    }
    const mc_desktop_session session = {&ctx, build, yaml, program_folder[0] ? program_folder : NULL, &roms, script, 0, 0};
    const char *running = config.profile[0] ? config.profile : config_failed && profile ? profile : NULL;
    char run_error[1024] = "";
    int status = mc_desktop_session_run(machine, &config, running, machine ? NULL : error, &options, &session, run_error, sizeof run_error);
    if (status) fprintf(stderr, "%s\n", run_error);
    mc_rom_index_free(&roms);
    return status ? 3 : 0;
}

/* livadix.exe is a Windows program without a console window of its own:
 * messages go to the console it was started from, or to the pipes a script
 * gave it. Started from Explorer, a profile that cannot start opens the
 * configuration window (desktop.c); a failure before the window shows its
 * messages in a message box instead. On Linux the program has its terminal's
 * streams as any command line program. */
#ifndef _WIN32
/* Started from a desktop launcher, the error stream goes nowhere a person
 * sees (/dev/null or the session's log): a failure's messages are kept and
 * shown in a message box, as on Windows. */
int main(int argc, char **argv) {
    char errors[] = "/tmp/livadix-errors-XXXXXX";
    int captured = 0;
    struct stat st;
    if (!isatty(STDERR_FILENO) && (fstat(STDERR_FILENO, &st) != 0 || S_ISCHR(st.st_mode) || S_ISSOCK(st.st_mode))) {
        const int fd = mkstemp(errors);
        if (fd >= 0) {
            fflush(stderr);
            captured = dup2(fd, STDERR_FILENO) >= 0;
            close(fd);
            if (!captured) remove(errors);
        }
    }
    const int status = livadix_main(argc, argv);
    if (captured) {
        fflush(stderr);
        FILE *f = fopen(errors, "rb");
        char text[4096];
        const size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        remove(errors);
        text[n] = 0;
        if (status && n) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Livadix", text, NULL);
    }
    return status;
}
#else
int main(int argc, char **argv) {
    char errors[MAX_PATH] = "";
    if (!GetStdHandle(STD_OUTPUT_HANDLE) && !GetStdHandle(STD_ERROR_HANDLE)) {
        FILE *ignored;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen_s(&ignored, "CONOUT$", "w", stdout);
            freopen_s(&ignored, "CONOUT$", "w", stderr);
        } else {
            char folder[MAX_PATH];
            if (GetTempPathA(sizeof folder, folder) && GetTempFileNameA(folder, "mce", 0, errors) &&
                freopen_s(&ignored, errors, "w", stderr) != 0)
                errors[0] = 0;
        }
    }
    const int status = livadix_main(argc, argv);
    if (errors[0]) {
        fflush(stderr);
        FILE *closed;
        freopen_s(&closed, "NUL", "w", stderr);
        FILE *f = fopen(errors, "rb");
        char text[4096];
        const size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
        if (f) fclose(f);
        remove(errors);
        text[n] = 0;
        if (status && n) MessageBoxA(NULL, text, "Livadix", MB_OK | MB_ICONERROR);
    }
    return status;
}
#endif
