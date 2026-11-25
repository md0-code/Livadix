/* The configuration window's rules for a profile (profile.h). Livadix's own
 * code: the limits come from the models (models.c), the boards
 * (machine.h, mc_machine_video and mc_machine_features), the System's
 * memory needs (compat.c) and the schema (config.c, docs/NOTES.md). */
#include "profile.h"
#include "compat.h"
#include "hfs.h"
#include "machine.h"
#include "models.h"
#include "portable.h"
#include "romdb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* --- the model ------------------------------------------------------------------------ */

const mc_model *mc_profile_model(const mc_config *c, const mc_rom_index *roms) {
    (void)roms;
    return mc_model_entry(c->model);
}

size_t mc_profile_ram_sizes(const mc_model *m, uint32_t *sizes, size_t max) {
    static const uint16_t generic[] = {4, 5, 8, 10, 12, 16, 20, 24, 32, 36, 40, 48, 52, 64, 65, 68, 80, 128, 136, 256, 0};
    uint32_t legal[64];
    size_t nlegal = m ? mc_machine_ram_sizes(m, legal, 64) : 0;
    if (!nlegal)
        for (const uint16_t *s = generic; *s; ++s) legal[nlegal++] = (uint32_t)*s * 1024u;
    size_t n = 0;
    for (size_t i = 0; i < nlegal && n < max; ++i) {
        const uint32_t s = legal[i];
        if (s < 1024u || s > 1024u * 1024u || (m && s > m->ram_max_mb * 1024u)) continue;
        sizes[n++] = s;
    }
    /* ascending, without repeats */
    for (size_t i = 1; i < n; ++i)
        for (size_t k = i; k > 0 && sizes[k - 1] > sizes[k]; --k) { const uint32_t t = sizes[k]; sizes[k] = sizes[k - 1]; sizes[k - 1] = t; }
    size_t out = 0;
    for (size_t i = 0; i < n; ++i) if (!out || sizes[out - 1] != sizes[i]) sizes[out++] = sizes[i];
    if (!out && m && max) sizes[out++] = (m->ram_mb < 4 ? 4 : m->ram_mb) * 1024u;
    return out;
}
uint32_t mc_profile_ram_least(const mc_profile_env *env) { return env->boot && env->boot->bootable ? mc_compat_min_ram(env->boot) : 0; }

/* The smallest size the model takes that is at least want (KiB), else
 * its largest. */
static uint32_t ram_at_least(const mc_model *m, uint32_t want) {
    uint32_t sizes[64];
    /* A board that takes any size keeps the one asked for (config.c). */
    if (m && !mc_machine_ram_sizes(m, sizes, 64)) {
        const uint32_t most = m->ram_max_mb * 1024u;
        return want > most ? most : want < 1024u ? 1024u : want;
    }
    const size_t n = mc_profile_ram_sizes(m, sizes, 64);
    if (!n) return want < 4096u ? 4096u : want;
    for (size_t i = 0; i < n; ++i) if (sizes[i] >= want) return sizes[i];
    return sizes[n - 1];
}

unsigned mc_profile_depths(const mc_config *c, const mc_model *m, unsigned w, unsigned h) {
    unsigned builtin = 0, card = 0;
    mc_machine_video(m, w, h, &builtin, &card);
    return c->video_card == MC_VIDEO_CARD_BUILTIN ? builtin : c->video_card == MC_VIDEO_CARD_LIVADIX ? card : builtin | card;
}
int mc_profile_has_builtin(const mc_model *m) {
    for (unsigned i = 0; i < MC_CONFIG_RESOLUTIONS; ++i) {
        unsigned builtin = 0, card = 0;
        mc_machine_video(m, mc_config_resolutions[i][0], mc_config_resolutions[i][1], &builtin, &card);
        if (builtin) return 1;
    }
    return 0;
}
int mc_profile_has_card(const mc_model *m) {
    unsigned builtin = 0, card = 0;
    mc_machine_video(m, 640, 480, &builtin, &card);
    return card != 0;
}

/* --- presets and rules ------------------------------------------------------------------ */

/* What the System needs (MiB) when the model can have it, else 0: a model
 * that cannot keeps any size it takes. */
static uint32_t ram_needed(const mc_model *m, const mc_profile_env *env) {
    const uint32_t least = mc_profile_ram_least(env);
    return least && ram_at_least(m, least * 1024u) >= least * 1024u ? least : 0;
}

void mc_profile_preset(mc_config *c, const mc_profile_env *env) {
    const mc_model *m = env->model;
    const uint32_t least = ram_needed(m, env);
    mc_config_set_ram_kb(c, ram_at_least(m, (m->ram_mb > least ? m->ram_mb : least) * 1024u));
    c->cpu_hz = m->cpu_hz;
    c->width = m->width;
    c->height = m->height;
    c->depth = m->depth;
    c->look = m->look;
    c->monitor = m->monitor;
    c->video_card = MC_VIDEO_CARD_AUTO;
    c->controller = MC_CONTROLLER_AUTO;
    c->fpu_full = 0;
    c->charger_start = 0;
    c->pmmu = 0;
    c->network_card = MC_NET_CARD_AUTO;
    c->cdrom_bus = MC_BUS_SCSI;
    c->rom[0] = 0;
    c->rom_explicit = c->rom_scanned = 0;
    /* The floppy drive a model without one took away comes back (the
     * rules below take it again where the model has none). */
    if (!c->floppy_count) {
        c->floppy_count = 1;
        c->floppy_off[0] = 0;
        c->floppy_off[1] = 1;
    }
    /* The internal IDE disk of the models that have one holds the first
     * hard disk, as the real machine's internal drive. */
    const int ata = (mc_machine_features(m) & MC_FEATURE_ATA_DISK) != 0;
    for (size_t i = 0; i < c->disk_count; ++i) {
        c->disks[i].bus = ata && i == 0 ? MC_BUS_ATA : MC_BUS_SCSI;
        if (c->disks[i].bus == MC_BUS_ATA) c->disks[i].scsi_id_set = c->disks[i].scsi_id = 0;
    }
    mc_profile_normalise(c, env, MC_CHANGED_MODEL);
}

static unsigned pick_depth(unsigned allowed, unsigned depth) {
    unsigned below = 0, above = 0;
    for (unsigned d = 1; d <= 32; d *= 2) {
        if (!(allowed & MC_DEPTH_BIT(d))) continue;
        if (d <= depth) below = d;
        else if (!above) above = d;
    }
    return below ? below : above;
}
static int resolution_index(unsigned w, unsigned h) {
    for (int i = 0; i < MC_CONFIG_RESOLUTIONS; ++i)
        if (mc_config_resolutions[i][0] == w && mc_config_resolutions[i][1] == h) return i;
    return -1;
}
/* A screen size the display choice shows: the model's own when it does,
 * else the first one. 0 when none. */
static int settle_resolution(mc_config *c, const mc_model *m) {
    if (resolution_index(m->width, m->height) >= 0 && mc_profile_depths(c, m, m->width, m->height)) {
        c->width = m->width;
        c->height = m->height;
        return 1;
    }
    for (int i = 0; i < MC_CONFIG_RESOLUTIONS; ++i)
        if (mc_profile_depths(c, m, mc_config_resolutions[i][0], mc_config_resolutions[i][1])) {
            c->width = mc_config_resolutions[i][0];
            c->height = mc_config_resolutions[i][1];
            return 1;
        }
    return 0;
}
static void normalise_video(mc_config *c, const mc_model *m, int changed) {
    if (resolution_index(c->width, c->height) < 0) { c->width = m->width; c->height = m->height; }
    unsigned builtin = 0, card = 0;
    mc_machine_video(m, c->width, c->height, &builtin, &card);
    if (c->video_card == MC_VIDEO_CARD_LIVADIX && !card) c->video_card = MC_VIDEO_CARD_AUTO;
    if (c->video_card == MC_VIDEO_CARD_BUILTIN && !builtin) {
        /* The size just chosen keeps itself and takes the display that
         * shows it; the built-in video just chosen takes a size it shows. */
        if (changed == MC_CHANGED_RESOLUTION || !settle_resolution(c, m)) c->video_card = MC_VIDEO_CARD_AUTO;
    }
    if (!mc_profile_depths(c, m, c->width, c->height) && !settle_resolution(c, m)) {
        c->video_card = MC_VIDEO_CARD_AUTO;
        settle_resolution(c, m);
    }
    const unsigned allowed = mc_profile_depths(c, m, c->width, c->height);
    if (allowed && !(allowed & MC_DEPTH_BIT(c->depth))) c->depth = pick_depth(allowed, c->depth);
    if (!c->refresh || 60 % c->refresh || c->refresh < 10) c->refresh = 60;
}
static int monitor_ok(const mc_config *c, const mc_model *m, uint32_t monitor) {
    char why[400];
    return mc_config_monitor_check(m, c->width, c->height, c->depth, monitor, why, sizeof why);
}

void mc_profile_normalise(mc_config *c, const mc_profile_env *env, int changed) {
    const mc_model *m = env->model;
    /* Memory: a size the board takes, and what the System needs. */
    mc_config_set_ram_kb(c, ram_at_least(m, mc_config_ram_kb(c) < 1024u ? 1024u : mc_config_ram_kb(c)));
    const uint32_t least = ram_needed(m, env);
    if (least && mc_config_ram_kb(c) < least * 1024u) mc_config_set_ram_kb(c, ram_at_least(m, least * 1024u));
    if (c->cpu_hz < MC_CONFIG_MIN_HZ) c->cpu_hz = MC_CONFIG_MIN_HZ;
    if (c->cpu_hz > MC_CONFIG_MAX_HZ) c->cpu_hz = MC_CONFIG_MAX_HZ;
    /* The full 68881 set only in a 68040's own FPU. */
    if (!(m->fpu && !strncmp(m->cpu, "68040", 5))) c->fpu_full = 0;
    /* The start by the charger only where the ROM knows it. */
    if (strcmp(m->key, "powerbook-100")) c->charger_start = 0;
    /* The 68851 only in the socket of the Macintosh II's boards (the AMU's). */
    if (!mc_model_pmmu_socket(m)) c->pmmu = 0;
    /* The controller's firmware only where the model has one and its dump
     * is in a roms folder. */
    const char *fw = mc_model_controller_firmware(m);
    if (!fw) c->controller = MC_CONTROLLER_AUTO;
    else if (c->controller == MC_CONTROLLER_FIRMWARE && !(env->roms && mc_rom_index_firmware(env->roms, mc_firmware_find(fw))))
        c->controller = MC_CONTROLLER_AUTO;
    /* The screen, then the monitor at it (a monochrome display just chosen
     * takes a mode one of Apple's had). */
    normalise_video(c, m, changed);
    c->look = mc_config_look_fit(m, c->look);
    if (!monitor_ok(c, m, c->monitor)) {
        if (changed == MC_CHANGED_MONITOR && c->monitor == MC_MONITOR_MONO && (m->monitors & MC_MON_MODULAR_MONO)) {
            const int portrait = c->width == 640 && c->height == 870, two_page = c->width == 1152 && c->height == 870;
            if (!(c->width == 640 && c->height == 480) && !portrait && !two_page) { c->width = 640; c->height = 480; }
            normalise_video(c, m, MC_CHANGED_RESOLUTION);
            if (c->depth > 8) c->depth = pick_depth(mc_profile_depths(c, m, c->width, c->height) & MC_DEPTHS_UPTO(8), 8);
        }
        if (!monitor_ok(c, m, c->monitor)) {
            if (monitor_ok(c, m, m->monitor)) c->monitor = m->monitor;
            else
                for (uint32_t k = 0; k < 3; ++k)
                    if (monitor_ok(c, m, k)) { c->monitor = k; break; }
        }
    }
    /* At least one mouse button clicks: the other one than just chosen. */
    if (c->mouse[0] != MC_MOUSE_NORMAL && c->mouse[1] != MC_MOUSE_NORMAL) c->mouse[changed == MC_CHANGED_MOUSE_LEFT ? 1 : 0] = MC_MOUSE_NORMAL;
    /* Storage: the buses the model has, one SCSI ID per disk, one place per
     * image. */
    const unsigned features = mc_machine_features(m);
    for (size_t i = 0; i < c->disk_count; ++i) {
        mc_disk_config *d = &c->disks[i];
        if (d->bus == MC_BUS_ATA && !(features & MC_FEATURE_ATA_DISK)) d->bus = MC_BUS_SCSI;
        if (d->bus == MC_BUS_ATA) d->scsi_id_set = d->scsi_id = 0;
        if (d->scsi_id_set && (d->scsi_id < 0 || d->scsi_id > 6 || d->scsi_id == 3)) d->scsi_id_set = d->scsi_id = 0;
        for (size_t k = 0; k < i; ++k)
            if (d->scsi_id_set && c->disks[k].scsi_id_set && c->disks[k].scsi_id == d->scsi_id) d->scsi_id_set = d->scsi_id = 0;
    }
    for (size_t i = 0; i < c->disk_count; ++i)
        for (size_t k = 0; k < i; ++k)
            if (!mc_path_cmp(c->disks[k].image, c->disks[i].image)) {
                memmove(&c->disks[i], &c->disks[i + 1], (c->disk_count - i - 1) * sizeof *c->disks);
                memset(&c->disks[--c->disk_count], 0, sizeof *c->disks);
                --i;
                break;
            }
    /* A console has no hard disk (config.c). */
    if (m->console) {
        memset(c->disks, 0, sizeof c->disks);
        c->disk_count = 0;
    }
    if (c->cdrom_bus == MC_BUS_ATA && !(features & MC_FEATURE_ATA_CD)) c->cdrom_bus = MC_BUS_SCSI;
    if (c->floppy_count > MC_CONFIG_FLOPPIES) c->floppy_count = MC_CONFIG_FLOPPIES;
    for (size_t i = c->floppy_count; i < MC_CONFIG_FLOPPIES; ++i) { c->floppy_off[i] = 1; memset(&c->floppies[i], 0, sizeof c->floppies[i]); }
    if ((features & MC_FEATURE_NO_FLOPPY) || (c->cdrom_bus == MC_BUS_ATA && (features & MC_FEATURE_MEDIA_BAY))) {
        /* The bay holds the CD-ROM drive: no floppy drive. */
        c->floppy_count = 0;
        for (size_t i = 0; i < MC_CONFIG_FLOPPIES; ++i) { c->floppy_off[i] = 1; memset(&c->floppies[i], 0, sizeof c->floppies[i]); }
    }
    for (size_t i = 0; i < c->floppy_count; ++i) {
        if (c->floppy_off[i]) { memset(&c->floppies[i], 0, sizeof c->floppies[i]); continue; }
        if (!c->floppies[i].image[0]) { c->floppies[i].readonly = 0; continue; }
        int taken = 0;
        for (size_t k = 0; k < c->disk_count; ++k) taken |= !mc_path_cmp(c->disks[k].image, c->floppies[i].image);
        for (size_t k = 0; k < i; ++k) taken |= !mc_path_cmp(c->floppies[k].image, c->floppies[i].image);
        if (taken) memset(&c->floppies[i], 0, sizeof c->floppies[i]);
    }
    /* One drive in the expansion bay where the model has one module. */
    if ((features & MC_FEATURE_ONE_FLOPPY) && c->floppy_count > 1) {
        c->floppy_count = 1;
        c->floppy_off[1] = 1;
        memset(&c->floppies[1], 0, sizeof c->floppies[1]);
    }
    /* The trailing drives not fitted are not listed. */
    while (c->floppy_count && c->floppy_off[c->floppy_count - 1]) c->floppy_count--;
    if (c->cdrom[0]) {
        int taken = 0;
        for (size_t k = 0; k < c->disk_count; ++k) taken |= !c->disks[k].readonly && !mc_path_cmp(c->disks[k].image, c->cdrom);
        for (size_t k = 0; k < c->floppy_count; ++k) taken |= !c->floppies[k].readonly && !mc_path_cmp(c->floppies[k].image, c->cdrom);
        if (taken) c->cdrom[0] = 0;
    }
    /* An Ethernet card only in the slot the model has. */
    if (features & MC_FEATURE_ENET) c->network_card = MC_NET_CARD_AUTO;
    if (c->network_card == MC_NET_CARD_NUBUS && !(features & MC_FEATURE_NUBUS)) c->network_card = MC_NET_CARD_AUTO;
    if (c->network_card == MC_NET_CARD_LC && !(features & MC_FEATURE_LC_SLOT)) c->network_card = MC_NET_CARD_AUTO;
    if (c->close_timeout < 1 || c->close_timeout > 3600) c->close_timeout = 30;
}

/* --- comparing ---------------------------------------------------------------------------- */

static int path_same(const char *a, const char *b) {
    for (;; ++a, ++b) {
        char x = *a == '\\' ? '/' : (char)tolower((unsigned char)*a), y = *b == '\\' ? '/' : (char)tolower((unsigned char)*b);
        if (x != y) return 0;
        if (!x) return 1;
    }
}
static int disks_same(const mc_disk_config *a, const mc_disk_config *b) {
    return path_same(a->image, b->image) && a->readonly == b->readonly && a->scsi_id_set == b->scsi_id_set &&
           (!a->scsi_id_set || a->scsi_id == b->scsi_id) && a->bus == b->bus;
}
static int port_same(const mc_port_config *a, const mc_port_config *b) {
    const unsigned x = a->type == MC_PORT_DISABLED ? MC_PORT_DEFAULT : a->type, y = b->type == MC_PORT_DISABLED ? MC_PORT_DEFAULT : b->type;
    if (x != y) return 0;
    if (x == MC_PORT_OUTPUT) return path_same(a->path, b->path);
    if (x == MC_PORT_SERIAL) return !mc_path_cmp(a->path, b->path);
    return x == MC_PORT_DEFAULT || !strcmp(a->path, b->path);
}

/* The keys the window writes, in the order of the template. */
enum {
    K_RAM, K_MHZ, K_JIT, K_CHIME, K_FPU, K_M68K_ACCEL, K_CONTROLLER, K_CLOSE, K_CLOSE_TIMEOUT, K_CHARGER, K_PMMU,
    K_RESOLUTION, K_DEPTH, K_REFRESH, K_CARD, K_ACCEL, K_LOOK, K_MONITOR, K_FILTER, K_SCALE, K_FULLSCREEN, K_VSYNC,
    K_DISKS, K_FLOPPIES, K_CDROM, K_BOOT, K_SOUND, K_FLOPPY_SOUND, K_LAYOUT, K_COMMAND, K_LEFT, K_RIGHT, K_PORT_A, K_PORT_B,
    K_NETWORK, K_NET_CARD, K_FORWARD, K_SHARE_NAME, K_METADATA, K_PASSWORD, K_FOLDERS, K_COUNT
};
static const char *const key_paths[K_COUNT][3] = {
    {"emulation", "ram_mb"}, {"emulation", "cpu_mhz"}, {"emulation", "jit"}, {"emulation", "startup_sound"},
    {"emulation", "fpu_full"}, {"emulation", "m68k_accelerator"}, {"emulation", "controller"}, {"emulation", "close_action"}, {"emulation", "close_timeout"},
    {"emulation", "charger_start"}, {"emulation", "pmmu"},
    {"video", "resolution"}, {"video", "depth"}, {"video", "refresh"}, {"video", "card"}, {"video", "acceleration"},
    {"video", "look"}, {"video", "monitor"}, {"video", "filter"}, {"video", "scale"}, {"video", "fullscreen"}, {"video", "vsync"},
    {"storage", "disks"}, {"storage", "floppies"}, {"storage", "cdrom"}, {"storage", "boot_priority"}, {"sound", "enabled"}, {"sound", "floppy"},
    {"input", "keyboard", "layout"}, {"input", "keyboard", "command"}, {"input", "mouse", "left"}, {"input", "mouse", "right"},
    {"ports", "a"}, {"ports", "b"}, {"network", "mode"}, {"network", "card"}, {"network", "forward"},
    {"sharing", "name"}, {"sharing", "metadata"}, {"sharing", "password"}, {"sharing", "folders"},
};

static int key_same(int k, const mc_config *a, const mc_config *b) {
    switch (k) {
    case K_RAM: return mc_config_ram_kb(a) == mc_config_ram_kb(b);
    case K_MHZ: return a->cpu_hz == b->cpu_hz;
    case K_JIT: return a->jit == b->jit;
    case K_CHIME: return a->startup_sound == b->startup_sound;
    case K_FPU: return a->fpu_full == b->fpu_full;
    case K_M68K_ACCEL: return a->m68k_accelerator == b->m68k_accelerator;
    case K_CHARGER: return a->charger_start == b->charger_start;
    case K_PMMU: return a->pmmu == b->pmmu;
    case K_CONTROLLER: return a->controller == b->controller;
    case K_CLOSE: return a->close_action == b->close_action;
    case K_CLOSE_TIMEOUT: return a->close_timeout == b->close_timeout;
    case K_RESOLUTION: return a->width == b->width && a->height == b->height;
    case K_DEPTH: return a->depth == b->depth;
    case K_REFRESH: return a->refresh == b->refresh;
    case K_CARD: return a->video_card == b->video_card;
    case K_ACCEL: return a->video_acceleration == b->video_acceleration;
    case K_LOOK: return a->look == b->look;
    case K_MONITOR: return a->monitor == b->monitor;
    case K_FILTER: return a->filter == b->filter;
    case K_SCALE: return a->scale == b->scale;
    case K_FULLSCREEN: return a->fullscreen == b->fullscreen;
    case K_VSYNC: return a->vsync == b->vsync;
    case K_DISKS:
        if (a->disk_count != b->disk_count) return 0;
        for (size_t i = 0; i < a->disk_count; ++i) if (!disks_same(&a->disks[i], &b->disks[i])) return 0;
        return 1;
    case K_FLOPPIES:
        if (a->floppy_count != b->floppy_count) return 0;
        for (size_t i = 0; i < a->floppy_count; ++i)
            if (a->floppy_off[i] != b->floppy_off[i] ||
                (!a->floppy_off[i] && (!path_same(a->floppies[i].image, b->floppies[i].image) || a->floppies[i].readonly != b->floppies[i].readonly)))
                return 0;
        return 1;
    case K_CDROM: return path_same(a->cdrom, b->cdrom) && a->cdrom_bus == b->cdrom_bus;
    case K_BOOT: return a->boot_priority == b->boot_priority;
    case K_SOUND: return a->sound == b->sound;
    case K_FLOPPY_SOUND: return a->floppy_sound == b->floppy_sound;
    case K_LAYOUT: return a->keyboard_layout == b->keyboard_layout;
    case K_COMMAND: return a->command_key == b->command_key;
    case K_LEFT: return a->mouse[0] == b->mouse[0];
    case K_RIGHT: return a->mouse[1] == b->mouse[1];
    case K_PORT_A: return port_same(&a->ports[0], &b->ports[0]);
    case K_PORT_B: return port_same(&a->ports[1], &b->ports[1]);
    case K_NETWORK: return a->network == b->network;
    case K_NET_CARD: return a->network_card == b->network_card;
    case K_FORWARD:
        if (a->forward_count != b->forward_count) return 0;
        for (size_t i = 0; i < a->forward_count; ++i)
            if (a->forwards[i].udp != b->forwards[i].udp || a->forwards[i].address != b->forwards[i].address ||
                a->forwards[i].host_port != b->forwards[i].host_port || a->forwards[i].guest_port != b->forwards[i].guest_port)
                return 0;
        return 1;
    case K_SHARE_NAME: return !strcmp(a->sharing_name, b->sharing_name);
    case K_METADATA: return a->sharing_metadata == b->sharing_metadata;
    case K_PASSWORD: return !strcmp(a->sharing_password, b->sharing_password);
    case K_FOLDERS:
        if (a->share_count != b->share_count) return 0;
        for (size_t i = 0; i < a->share_count; ++i)
            if (strcmp(a->shares[i].name, b->shares[i].name) || !path_same(a->shares[i].path, b->shares[i].path) ||
                a->shares[i].readonly != b->shares[i].readonly)
                return 0;
        return 1;
    default: return 1;
    }
}
int mc_profile_same(const mc_config *a, const mc_config *b) {
    if (a->model != b->model) return 0;
    for (int k = 0; k < K_COUNT; ++k) if (!key_same(k, a, b)) return 0;
    return 1;
}
int mc_profile_needs_restart(const mc_config *running, const mc_config *edited) {
    static const int live[] = {K_CHIME, K_CLOSE, K_CLOSE_TIMEOUT, K_REFRESH, K_LOOK, K_MONITOR, K_FILTER, K_SCALE, K_FULLSCREEN,
                               K_VSYNC, K_SOUND, K_FLOPPY_SOUND, K_COMMAND, K_LEFT, K_RIGHT};
    if (running->model != edited->model) return 1;
    if (!path_same(running->nvram, edited->nvram) || strcmp(running->nvram_id, edited->nvram_id)) return 1;
    /* Sound goes off at once; on again only on a machine built with it. */
    if (edited->sound && !running->sound) return 1;
    /* Disks go into and out of the drives as it runs; the drives stay. */
    if (running->cdrom_bus != edited->cdrom_bus || running->floppy_count != edited->floppy_count) return 1;
    for (size_t i = 0; i < running->floppy_count; ++i) if (running->floppy_off[i] != edited->floppy_off[i]) return 1;
    for (int k = 0; k < K_COUNT; ++k) {
        if (k == K_FLOPPIES || k == K_CDROM) continue;
        int applies_live = 0;
        for (size_t i = 0; i < sizeof live / sizeof *live; ++i) applies_live |= live[i] == k;
        if (!applies_live && !key_same(k, running, edited)) return 1;
    }
    return 0;
}

/* --- reading and writing ------------------------------------------------------------------- */

static const char *const looks[] = {"plain", "crt", "lcd"};
static const char *const monitors[] = {"color", "mono", "lcd-gray"};
static const char *const filters[] = {"nearest", "linear", "sharp"};
static const char *const scales[] = {"fit", "integer"};
static const char *const cards[] = {"auto", "builtin", "livadix"};
static const char *const controllers[] = {"auto", "protocol", "firmware"};
static const char *const closes[] = {"shutdown", "quit"};
static const char *const priorities[] = {"disk", "cdrom"};
static const char *const layouts[] = {"usa", "iso"};
static const char *const commands[] = {"left-alt", "left-control", "left-alt", "right-control", "right-alt"};
static const char *const buttons[] = {"disabled", "normal", "control-click"};
static const char *const networks[] = {"nat", "none"};
static const char *const net_cards[] = {"auto", "none", "nubus", "lc"};
static const char *const metadatas[] = {"streams", "appledouble"};
static const char *name_of(const char *const *names, size_t count, uint32_t v) { return v < count ? names[v] : names[0]; }
#define NAME(table, v) name_of(table, sizeof table / sizeof *table, v)

/* A path as the file writes it: relative to its folder when inside it. */
static int strip_folder(char *p, const char *folder, char *out, size_t capacity) {
    char base[MC_CONFIG_PATH];
    snprintf(base, sizeof base, "%s", folder);
    for (char *s = base; *s; ++s) if (*s == '\\') *s = '/';
    const size_t n = strlen(base);
    if (!n || mc_path_ncmp(p, base, n)) return 0;
    snprintf(out, capacity, "%s", p + n);
    return 1;
}
static void relative(const char *folder, const char *path, char *out, size_t capacity) {
    char p[MC_CONFIG_PATH];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p; *s; ++s) if (*s == '\\') *s = '/';
    if (strip_folder(p, folder, out, capacity)) return;
#ifdef _WIN32
    /* An absolute path (the NVRAM base Livadix makes) inside the folder of a
     * file named without one (livadix.yaml): the folder made absolute. */
    char full[MC_CONFIG_PATH];
    if (_fullpath(full, folder[0] ? folder : ".", sizeof full)) {
        const size_t end = strlen(full);
        if (end && full[end - 1] != '/' && full[end - 1] != '\\' && end + 1 < sizeof full) { full[end] = '/'; full[end + 1] = 0; }
        if (strip_folder(p, full, out, capacity)) return;
    }
#endif
    snprintf(out, capacity, "%s", p);
}
static const char *path_text(const char *folder, const char *path, char *out, size_t capacity) {
    char rel[MC_CONFIG_PATH];
    relative(folder, path, rel, sizeof rel);
    return mc_ydoc_quote_path(rel, out, capacity);
}

/* A key's value as the file writes it: one line (*text), or block lines
 * (lines, *count). empty: the value Livadix has without the key (an empty
 * list, no CD, no port, no password), which a profile still writes when
 * the shared settings give another. */
typedef struct value_text {
    char text[MC_CONFIG_PATH * 2];
    char lines[MC_CONFIG_DISKS * 3][MC_CONFIG_PATH + 64];
    const char *line[MC_CONFIG_DISKS * 3];
    size_t count;
    int block, empty;
} value_text;
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"   /* every caller passes a literal */
#endif
static void add_line(value_text *v, const char *format, const char *a, const char *b) {
    if (v->count >= sizeof v->lines / sizeof *v->lines) return;
    snprintf(v->lines[v->count], sizeof v->lines[0], format, a, b);
    v->line[v->count] = v->lines[v->count];
    v->count++;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
static void format_value(int k, const mc_config *c, const char *folder, value_text *v) {
    char q[MC_CONFIG_PATH + 8], q2[MC_CONFIG_PATH + 8], mhz[32];
    v->count = 0;
    v->block = v->empty = 0;
    v->text[0] = 0;
    switch (k) {
    case K_RAM: mc_config_ram_text(mc_config_ram_kb(c), v->text, sizeof v->text); break;
    case K_MHZ: snprintf(v->text, sizeof v->text, "%s", mc_config_mhz_text(c->cpu_hz, mhz, sizeof mhz)); break;
    case K_JIT: snprintf(v->text, sizeof v->text, "%s", c->jit ? "true" : "false"); break;
    case K_CHIME: snprintf(v->text, sizeof v->text, "%s", c->startup_sound ? "true" : "false"); break;
    case K_FPU: snprintf(v->text, sizeof v->text, "%s", c->fpu_full ? "true" : "false"); break;
    case K_M68K_ACCEL: snprintf(v->text, sizeof v->text, "%s", c->m68k_accelerator ? "true" : "false"); break;
    case K_CHARGER: snprintf(v->text, sizeof v->text, "%s", c->charger_start ? "true" : "false"); break;
    case K_PMMU: snprintf(v->text, sizeof v->text, "%s", c->pmmu ? "true" : "false"); break;
    case K_CONTROLLER: snprintf(v->text, sizeof v->text, "%s", NAME(controllers, c->controller)); break;
    case K_CLOSE: snprintf(v->text, sizeof v->text, "%s", NAME(closes, c->close_action)); break;
    case K_CLOSE_TIMEOUT: snprintf(v->text, sizeof v->text, "%u", (unsigned)c->close_timeout); break;
    case K_RESOLUTION: snprintf(v->text, sizeof v->text, "%ux%u", (unsigned)c->width, (unsigned)c->height); break;
    case K_DEPTH: snprintf(v->text, sizeof v->text, "%u", (unsigned)c->depth); break;
    case K_REFRESH: snprintf(v->text, sizeof v->text, "%u", (unsigned)c->refresh); break;
    case K_CARD: snprintf(v->text, sizeof v->text, "%s", NAME(cards, c->video_card)); break;
    case K_ACCEL: snprintf(v->text, sizeof v->text, "%s", c->video_acceleration ? "true" : "false"); break;
    case K_LOOK: snprintf(v->text, sizeof v->text, "%s", NAME(looks, c->look)); break;
    case K_MONITOR: snprintf(v->text, sizeof v->text, "%s", NAME(monitors, c->monitor)); break;
    case K_FILTER: snprintf(v->text, sizeof v->text, "%s", NAME(filters, c->filter)); break;
    case K_SCALE: snprintf(v->text, sizeof v->text, "%s", NAME(scales, c->scale)); break;
    case K_FULLSCREEN: snprintf(v->text, sizeof v->text, "%s", c->fullscreen ? "true" : "false"); break;
    case K_VSYNC: snprintf(v->text, sizeof v->text, "%s", c->vsync ? "true" : "false"); break;
    case K_DISKS:
        v->block = 1;
        v->empty = !c->disk_count;
        for (size_t i = 0; i < c->disk_count; ++i) {
            const mc_disk_config *d = &c->disks[i];
            add_line(v, "- image: %s%s", path_text(folder, d->image, q, sizeof q), "");
            if (d->readonly) add_line(v, "  readonly: true%s%s", "", "");
            if (d->bus == MC_BUS_ATA) add_line(v, "  bus: ata%s%s", "", "");
            else if (d->scsi_id_set) { char id[16]; snprintf(id, sizeof id, "%d", d->scsi_id); add_line(v, "  scsi_id: %s%s", id, ""); }
        }
        break;
    case K_FLOPPIES:
        v->block = 1;
        v->empty = !c->floppy_count;
        for (size_t i = 0; i < c->floppy_count; ++i) {
            if (c->floppy_off[i]) add_line(v, "- off%s%s", "", "");
            else if (!c->floppies[i].image[0]) add_line(v, "- {}%s%s", "", "");
            else add_line(v, "- {image: %s%s}", path_text(folder, c->floppies[i].image, q, sizeof q), c->floppies[i].readonly ? ", readonly: true" : "");
        }
        break;
    case K_CDROM:
        v->empty = !c->cdrom[0] && c->cdrom_bus == MC_BUS_SCSI;
        if (c->cdrom_bus == MC_BUS_ATA)
            snprintf(v->text, sizeof v->text, c->cdrom[0] ? "{image: %s, bus: ata}" : "{bus: ata}", c->cdrom[0] ? path_text(folder, c->cdrom, q, sizeof q) : "");
        else if (c->cdrom[0]) snprintf(v->text, sizeof v->text, "%s", path_text(folder, c->cdrom, q, sizeof q));
        else snprintf(v->text, sizeof v->text, "{}");
        break;
    case K_BOOT: snprintf(v->text, sizeof v->text, "%s", NAME(priorities, c->boot_priority)); break;
    case K_SOUND: snprintf(v->text, sizeof v->text, "%s", c->sound ? "true" : "false"); break;
    case K_FLOPPY_SOUND: snprintf(v->text, sizeof v->text, "%s", c->floppy_sound ? "true" : "false"); break;
    case K_LAYOUT: snprintf(v->text, sizeof v->text, "%s", NAME(layouts, c->keyboard_layout)); break;
    case K_COMMAND: snprintf(v->text, sizeof v->text, "%s", NAME(commands, c->command_key)); break;
    case K_LEFT: snprintf(v->text, sizeof v->text, "%s", NAME(buttons, c->mouse[0])); break;
    case K_RIGHT: snprintf(v->text, sizeof v->text, "%s", NAME(buttons, c->mouse[1])); break;
    case K_PORT_A:
    case K_PORT_B: {
        const mc_port_config *p = &c->ports[k == K_PORT_B];
        v->empty = p->type == MC_PORT_DEFAULT || p->type == MC_PORT_DISABLED;
        if (p->type == MC_PORT_SERIAL) snprintf(v->text, sizeof v->text, "{serial: %s}", p->path);
        else if (p->type == MC_PORT_PIPE) snprintf(v->text, sizeof v->text, "{pipe: %s}", mc_ydoc_quote_path(p->path, q, sizeof q));
        else if (p->type == MC_PORT_OUTPUT) snprintf(v->text, sizeof v->text, "{output: %s}", path_text(folder, p->path, q, sizeof q));
        else snprintf(v->text, sizeof v->text, "{disabled: true}");
        break;
    }
    case K_NETWORK: snprintf(v->text, sizeof v->text, "%s", NAME(networks, c->network)); break;
    case K_NET_CARD: snprintf(v->text, sizeof v->text, "%s", NAME(net_cards, c->network_card)); break;
    case K_FORWARD: {
        v->empty = !c->forward_count;
        size_t used = (size_t)snprintf(v->text, sizeof v->text, "[");
        for (size_t i = 0; i < c->forward_count && used < sizeof v->text; ++i) {
            char one[48];
            mc_config_forward_text(&c->forwards[i], one, sizeof one);
            used += (size_t)snprintf(v->text + used, sizeof v->text - used, "%s\"%s\"", i ? ", " : "", one);
        }
        if (used < sizeof v->text) snprintf(v->text + used, sizeof v->text - used, "]");
        break;
    }
    case K_SHARE_NAME: snprintf(v->text, sizeof v->text, "%s", mc_ydoc_quote_path(c->sharing_name, q, sizeof q)); break;
    case K_METADATA: snprintf(v->text, sizeof v->text, "%s", NAME(metadatas, c->sharing_metadata)); break;
    case K_PASSWORD:
        v->empty = !c->sharing_password[0];
        snprintf(v->text, sizeof v->text, "%s", c->sharing_password[0] ? mc_ydoc_quote_path(c->sharing_password, q, sizeof q) : "\"\"");
        break;
    case K_FOLDERS:
        v->block = 1;
        v->empty = !c->share_count;
        for (size_t i = 0; i < c->share_count; ++i) {
            char name[2 * MC_CONFIG_SHARE_NAME + 4];
            snprintf(q2, sizeof q2, "%s", path_text(folder, c->shares[i].path, q, sizeof q));
            snprintf(v->lines[v->count], sizeof v->lines[0], "- {name: %s, path: %s%s}", mc_ydoc_quote_path(c->shares[i].name, name, sizeof name), q2,
                     c->shares[i].readonly ? ", readonly: true" : "");
            v->line[v->count] = v->lines[v->count];
            v->count++;
        }
        break;
    default: break;
    }
}

static size_t profile_path(const char *profile, const char **path) {
    size_t n = 0;
    if (profile) { path[n++] = "profiles"; path[n++] = profile; }
    return n;
}
static size_t key_path(const char *profile, int k, const char **path) {
    size_t n = profile_path(profile, path);
    for (int i = 0; i < 3 && key_paths[k][i]; ++i) path[n++] = key_paths[k][i];
    return n;
}
static void folder_of(const char *source, char *out) {
    snprintf(out, MC_CONFIG_PATH, "%s", source);
    char *slash = strrchr(out, '/'), *back = strrchr(out, '\\');
    if (back > slash) slash = back;
    if (slash) slash[1] = 0; else out[0] = 0;
}

/* The profile alone (the others and default: left out), as Livadix reads it,
 * or (values) its values without the model's rules. */
static int read_profile(const mc_ydoc *doc, const char *profile, const char *source, mc_config *c, int values, char *error,
                        size_t capacity) {
    mc_ydoc copy;
    if (!mc_ydoc_copy(&copy, doc)) { snprintf(error, capacity, "Out of memory"); return 0; }
    if (profile) {
        /* The other profiles left out, and default: with them. */
        const char *top[] = {"profiles"}, *def[] = {"default"};
        static char keys[MC_CONFIG_PROFILES][128];
        const size_t n = mc_ydoc_keys(&copy, top, 1, keys, MC_CONFIG_PROFILES);
        for (size_t i = 0; i < n; ++i) {
            if (!strcmp(keys[i], profile)) continue;
            const char *path[] = {"profiles", keys[i]};
            mc_ydoc_remove(&copy, path, 2);
        }
        mc_ydoc_remove(&copy, def, 1);
    }
    size_t length = 0;
    char *text = mc_ydoc_text(&copy, &length);
    mc_ydoc_free(&copy);
    if (!text) { snprintf(error, capacity, "Out of memory"); return 0; }
    const int ok = values ? mc_config_parse_values(text, length, source, profile, c, error, capacity)
                          : mc_config_parse(text, length, source, profile, c, error, capacity);
    free(text);
    return ok;
}
int mc_profile_read(const mc_ydoc *doc, const char *profile, const char *source, const mc_rom_index *roms, mc_config *c,
                    char *error, size_t capacity) {
    (void)roms;            /* the model is the profile's own: no ROM settles it */
    return read_profile(doc, profile, source, c, 0, error, capacity);
}

/* What the profile gets from the shared settings and its model alone. */
static int profile_base(const mc_ydoc *doc, const char *profile, const mc_config *c, const char *source, const mc_rom_index *roms,
                        mc_config *base) {
    mc_ydoc copy;
    if (!mc_ydoc_copy(&copy, doc)) return 0;
    const char *path[8];
    size_t n = profile_path(profile, path);
    const char *model = mc_model_entry(c->model)->key;
    int ok = 1;
    if (profile) {
        ok = mc_ydoc_set(&copy, path, n, "{}");
        path[n] = "model";
        ok = ok && mc_ydoc_set(&copy, path, n + 1, model);
    } else {
        /* Without profiles: the defaults with the model. */
        mc_ydoc_free(&copy);
        char text[128];
        const int length = snprintf(text, sizeof text, "model: %s\n", model);
        char error[256];
        if (!mc_config_parse(text, (size_t)length, source, NULL, base, error, sizeof error)) return 0;
        return 1;
    }
    char error[512];
    (void)roms;
    /* The values the shared settings and the model give, though the model
     * may not take them all (the profile then writes its own). */
    ok = ok && read_profile(&copy, profile, source, base, 1, error, sizeof error);
    mc_ydoc_free(&copy);
    return ok;
}

static int write_value(mc_ydoc *doc, const char **path, size_t n, const value_text *v) {
    if (v->block) return v->count ? mc_ydoc_set_block(doc, path, n, v->line, v->count) : mc_ydoc_set(doc, path, n, "[]");
    return mc_ydoc_set(doc, path, n, v->text);
}

static int write_profile(mc_ydoc *doc, const char *profile, const mc_config *c, const char *source, const mc_rom_index *roms,
                         char *error, size_t capacity) {
    char folder[MC_CONFIG_PATH], problem[512];
    folder_of(source, folder);
    const char *path[8];
    size_t n = profile_path(profile, path);
    /* Every key of the profile on a line of its own. */
    if (!mc_ydoc_expand(doc, path, n)) { snprintf(error, capacity, "Out of memory"); return 0; }
    mc_config current;
    int ok = mc_profile_read(doc, profile, source, roms, &current, problem, sizeof problem);
    const char *model = mc_model_entry(c->model)->key;
    /* A profile that does not read is written anew. */
    if (!ok && profile && !mc_ydoc_set(doc, path, n, "{}")) { snprintf(error, capacity, "The profile cannot be rewritten"); return 0; }
    if (!ok || current.model != c->model) {
        path[n] = "model";
        if (!mc_ydoc_set(doc, path, n + 1, model)) { snprintf(error, capacity, "The model cannot be written"); return 0; }
        /* Another model's ROM file does not follow it. */
        const char *rom[8];
        size_t r = profile_path(profile, rom);
        rom[r++] = "emulation";
        rom[r++] = "rom";
        mc_ydoc_remove(doc, rom, r);
        ok = mc_profile_read(doc, profile, source, roms, &current, problem, sizeof problem);
    }
    /* Retain a stable namespace through renaming and model changes. The
     * store itself adds the model and actual ROM identity on every start. */
    mc_config owned = *c;
    if(profile && strcmp(profile,c->profile)){
        if(!mc_config_new_nvram_identity(&owned)){snprintf(error,capacity,"A fresh NVRAM identity could not be generated");return 0;}
        snprintf(owned.profile,sizeof owned.profile,"%s",profile);
    }
    snprintf(owned.source,sizeof owned.source,"%s",source);
    mc_config_nvram_identity(&owned);
    const char *nv[8]; size_t nn=profile_path(profile,nv);nv[nn++]="emulation";nv[nn++]="nvram_id";
    char nvtext[MC_CONFIG_PATH*2];
    if(!mc_ydoc_set(doc,nv,nn,owned.nvram_id)){snprintf(error,capacity,"The NVRAM identity cannot be written");return 0;}
    if(c->nvram[0]&&!mc_model_builtin(mc_model_entry(c->model))){
        nv[nn-1]="nvram";
        if(!path_text(folder,c->nvram,nvtext,sizeof nvtext)||!mc_ydoc_set(doc,nv,nn,nvtext)){snprintf(error,capacity,"The NVRAM base path cannot be written");return 0;}
    }
    /* What the profile gets from the shared settings and its model alone;
     * when those do not make a configuration of this model (shared memory
     * or a screen it lacks, say), the profile writes every key itself. */
    mc_config base;
    const int has_base = profile_base(doc, profile, c, source, roms, &base);
    if (!has_base && !profile) { snprintf(error, capacity, "The shared settings do not read"); return 0; }
    for (int k = 0; k < K_COUNT; ++k) {
        if (ok && key_same(k, c, &current)) continue;
        const size_t depth = key_path(profile, k, path);
        const int written = mc_ydoc_has(doc, path, depth);
        if (!written && has_base && key_same(k, c, &base)) continue;
        value_text *v = malloc(sizeof *v);
        if (!v) { snprintf(error, capacity, "Out of memory"); return 0; }
        format_value(k, c, folder, v);
        int done;
        /* A profile's key set back to what the shared settings and the model
         * give it goes (the file without profiles keeps its keys); Livadix's
         * value without the key, likewise, else it is written. */
        if (has_base && key_same(k, c, &base) && (profile || v->empty)) done = mc_ydoc_remove(doc, path, depth);
        else done = write_value(doc, path, depth, v);
        free(v);
        if (!done) { snprintf(error, capacity, "%s.%s cannot be written", key_paths[k][0], key_paths[k][1]); return 0; }
    }
    mc_config back;
    if (!mc_profile_read(doc, profile, source, roms, &back, problem, sizeof problem)) {
        snprintf(error, capacity, "The written profile does not read: %s", problem);
        return 0;
    }
    if (!mc_profile_same(&back, c)) {
        for (int k = 0; k < K_COUNT; ++k)
            if (!key_same(k, &back, c)) { snprintf(error, capacity, "%s.%s did not read back as written", key_paths[k][0], key_paths[k][1]); return 0; }
        snprintf(error, capacity, "The model did not read back as written");
        return 0;
    }
    return 1;
}
/* All or nothing: the document changes only when the profile is written
 * and reads back as edited. */
int mc_profile_write(mc_ydoc *doc, const char *profile, const mc_config *c, const char *source, const mc_rom_index *roms,
                     char *error, size_t capacity) {
    mc_ydoc work;
    if (!mc_ydoc_copy(&work, doc)) { snprintf(error, capacity, "Out of memory"); return 0; }
    if (!write_profile(&work, profile, c, source, roms, error, capacity)) { mc_ydoc_free(&work); return 0; }
    mc_ydoc_free(doc);
    *doc = work;
    return 1;
}
