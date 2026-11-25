/* The configuration window's application (config_app.h). Livadix's own code. */
#include "config_app.h"
#include "../core/compat.h"
#include "../core/disk.h"
#include "../core/machine.h"
#include "../core/models.h"
#include "../core/portable.h"
#include "../core/profile.h"
#include "config_picker.h"
#include "livadix_default_yaml.h"
#include "mc_version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <math.h>

/* MSVC stat uses a 32-bit file size even in an x64 program. */
#ifdef _WIN32
typedef struct _stat64 image_stat;
#define image_stat_read _stat64
#else
typedef struct stat image_stat;
#define image_stat_read stat
#endif

#define TAB_LABELS {"Machine", "Video", "Storage", "Sound", "Input", "Ports", "Network", "Sharing"}
#define ROW 20                         /* the height of a control row */
#define FIELD 22                       /* the height of pop-ups, buttons and fields */

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"   /* every caller passes a literal */
#endif
static void set_status(cfg_app *a, int icon, const char *format, const char *arg) {
    snprintf(a->status, sizeof a->status, format, arg ? arg : "");
    a->status_icon = icon;
}
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
const char *cfg_app_selected(const cfg_app *a) {
    return a->profile_count && a->selected >= 0 && (size_t)a->selected < a->profile_count ? a->names[a->selected] : NULL;
}
static const char *profile_name(const cfg_app *a) { return cfg_app_selected(a); }
static const char *base_name(const char *path) {
    const char *name = path;
    for (const char *s = path; *s; ++s) if (*s == '/' || *s == '\\') name = s + 1;
    return name;
}

/* --- images ------------------------------------------------------------------------------ */

/* What an image holds, read once per file version. */
static const mc_volume_info *volume(cfg_app *a, const char *path) {
    image_stat st;
    const int found = image_stat_read(path, &st) == 0;
    const long long size = found ? (long long)st.st_size : -1, time = found ? (long long)st.st_mtime : 0;
    for (int i = 0; i < CFG_VOLUME_CACHE; ++i)
        if (!strcmp(a->cache[i].path, path) && a->cache[i].size == size && a->cache[i].time == time)
            return a->cache[i].ok ? &a->cache[i].info : NULL;
    const int i = a->cache_next++ % CFG_VOLUME_CACHE;
    snprintf(a->cache[i].path, sizeof a->cache[i].path, "%s", path);
    a->cache[i].size = size;
    a->cache[i].time = time;
    char error[256];
    a->cache[i].ok = found && mc_volume_inspect(path, &a->cache[i].info, error, sizeof error);
    return a->cache[i].ok ? &a->cache[i].info : NULL;
}
static int file_exists(const char *path) {
    image_stat st;
    return image_stat_read(path, &st) == 0;
}
static void scsi_ids(const mc_config *c, int *ids) {
    static const int free_ids[] = {0, 1, 2, 4, 5, 6};
    int taken[8] = {0};
    for (size_t i = 0; i < c->disk_count; ++i) {
        ids[i] = c->disks[i].bus == MC_BUS_ATA ? -2 : c->disks[i].scsi_id_set ? c->disks[i].scsi_id : -1;
        if (ids[i] >= 0) taken[ids[i]] = 1;
    }
    for (size_t i = 0; i < c->disk_count; ++i)
        if (ids[i] == -1)
            for (int k = 0; k < 6; ++k) if (!taken[free_ids[k]]) { ids[i] = free_ids[k]; taken[ids[i]] = 1; break; }
    for (size_t i = 0; i < c->disk_count; ++i) if (ids[i] == -1) ids[i] = 6;   /* beyond six: partitions of the last */
}
/* The startup disk as mc_config_boot_image finds it, from the cached
 * contents of the images. */
static const char *boot_disk(cfg_app *a, const mc_config *c, const mc_volume_info **info) {
    *info = NULL;
    if (c->boot_priority == MC_BOOT_CDROM && c->cdrom[0]) {
        const mc_volume_info *v = volume(a, c->cdrom);
        if (v && v->bootable) { *info = v; return c->cdrom; }
    }
    for (size_t i = 0; i < c->floppy_count; ++i)
        if (!c->floppy_off[i] && c->floppies[i].image[0]) {
            const mc_volume_info *v = volume(a, c->floppies[i].image);
            if (v && v->bootable) { *info = v; return c->floppies[i].image; }
        }
    int ids[MC_CONFIG_DISKS];
    scsi_ids(c, ids);
    for (size_t i = 0; i < c->disk_count; ++i)
        if (ids[i] == -2) {
            const mc_volume_info *v = volume(a, c->disks[i].image);
            if (v && v->bootable) { *info = v; return c->disks[i].image; }
        }
    static const int order[] = {0, 6, 5, 4, 2, 1};
    for (size_t k = 0; k < 6; ++k)
        for (size_t i = 0; i < c->disk_count; ++i) {
            if (ids[i] != order[k]) continue;
            const mc_volume_info *v = volume(a, c->disks[i].image);
            if (v && v->bootable) { *info = v; return c->disks[i].image; }
        }
    return NULL;
}
static mc_profile_env environment(cfg_app *a, const mc_config *c) {
    mc_profile_env env;
    const mc_volume_info *boot = NULL;
    boot_disk(a, c, &boot);
    env.model = mc_model_entry(c->model);
    env.roms = a->roms;
    env.boot = boot;
    return env;
}

/* --- the document ------------------------------------------------------------------------ */

/* The profile list from the document: names, models, the default. */
static void refresh_list(cfg_app *a) {
    const char *top[] = {"profiles"}, *def[] = {"default"};
    a->profile_count = mc_ydoc_keys(&a->doc, top, 1, a->names, MC_CONFIG_PROFILES);
    a->default_index = a->profile_count ? 0 : -1;
    char name[MC_CONFIG_NAME];
    if (mc_ydoc_get(&a->doc, def, 1, name, sizeof name))
        for (size_t i = 0; i < a->profile_count; ++i) if (!strcmp(a->names[i], name)) a->default_index = (int)i;
    /* The models as Livadix reads them, else as written. */
    size_t length = 0;
    char *text = mc_ydoc_text(&a->doc, &length);
    mc_profile_list *list = malloc(sizeof *list);
    char error[512];
    a->doc_error[0] = 0;
    const int read = text && list && mc_config_profiles_text(text, length, a->path, list, error, sizeof error);
    for (size_t i = 0; i < a->profile_count; ++i) {
        const char *path[] = {"profiles", a->names[i], "model"};
        if (read && list->count == a->profile_count) snprintf(a->models[i], sizeof a->models[i], "%s", list->models[i]);
        else if (!mc_ydoc_get(&a->doc, path, 3, a->models[i], sizeof a->models[i])) snprintf(a->models[i], sizeof a->models[i], "%s", MC_MODEL_BUILTIN);
    }
    /* A file whose shared settings do not read cannot be edited (a profile
     * that does not read is that profile's problem; no shared settings at
     * all are the defaults). */
    if (!read && text) {
        mc_ydoc copy;
        if (mc_ydoc_copy(&copy, &a->doc)) {
            mc_ydoc_remove(&copy, top, 1);
            mc_ydoc_remove(&copy, def, 1);
            char keys[1][128];
            char *shared = mc_ydoc_keys(&copy, NULL, 0, keys, 1) ? mc_ydoc_text(&copy, &length) : NULL;
            mc_config c;
            if (shared && !mc_config_parse(shared, length, a->path, NULL, &c, error, sizeof error))
                snprintf(a->doc_error, sizeof a->doc_error, "%s", error);
            free(shared);
            mc_ydoc_free(&copy);
        }
    }
    free(list);
    free(text);
    if (a->selected >= (int)a->profile_count) a->selected = a->profile_count ? (int)a->profile_count - 1 : 0;
}

/* The text fields from the configuration; a field being typed into keeps
 * its text unless force. */
static void sync_texts(cfg_app *a, int force) {
    const mc_config *c = &a->config;
    const uint32_t editing = force ? 0 : a->ui.edit_id;
    if (editing != ui_id("mhz-text", 0)) mc_config_mhz_text(c->cpu_hz, a->mhz_text, sizeof a->mhz_text);
    for (int p = 0; p < 2; ++p)
        if (editing != ui_id("pipe", p))
            snprintf(a->pipe_text[p], sizeof a->pipe_text[p], "%s", c->ports[p].type == MC_PORT_PIPE ? c->ports[p].path : "");
    if (editing != ui_id("forwards", 0)) {
        a->forward_text[0] = 0;
        for (size_t k = 0; k < c->forward_count; ++k) {
            char one[48];
            mc_config_forward_text(&c->forwards[k], one, sizeof one);
            const size_t used = strlen(a->forward_text);
            snprintf(a->forward_text + used, sizeof a->forward_text - used, "%s%s", k ? ", " : "", one);
        }
    }
    if (editing != ui_id("server-name", 0)) snprintf(a->server_text, sizeof a->server_text, "%s", c->sharing_name);
    if (editing != ui_id("share-password", 0)) snprintf(a->password_text, sizeof a->password_text, "%s", c->sharing_password);
    if (a->share_selected >= (int)c->share_count) a->share_selected = (int)c->share_count - 1;
    if (a->share_selected < 0) a->share_selected = 0;
    if (editing != ui_id("share-name", 0))
        snprintf(a->share_name_text, sizeof a->share_name_text, "%s", c->share_count ? c->shares[a->share_selected].name : "");
    if (a->disk_selected >= (int)c->disk_count) a->disk_selected = (int)c->disk_count - 1;
    if (a->disk_selected < 0) a->disk_selected = 0;
}

/* The model, its ROM and the note under the model pop-up. */
static void settle_model(cfg_app *a) {
    a->model = mc_profile_model(&a->config, a->roms);
    mc_config c = a->config;
    char error[512];
    a->rom_ok = mc_config_find_rom(&c, a->roms, error, sizeof error);
    if (a->rom_ok && mc_model_builtin(a->model))
        snprintf(a->rom_note, sizeof a->rom_note, "ROM: %s, Livadix's own: a desktop with the README and the changelog",
                 c.rom[0] ? c.rom : "embedded in livadix.exe");
    else if (a->rom_ok) snprintf(a->rom_note, sizeof a->rom_note, "ROM: %s%s", c.rom, c.rom_scanned ? " (found by its checksum)" : "");
    else snprintf(a->rom_note, sizeof a->rom_note, "%s", error);
}

/* Writes the edited configuration into the document after a change: made
 * valid first, then written and read back as Livadix reads it. */
static int commit(cfg_app *a, int changed) {
    a->changes++;
    a->model = NULL;
    mc_profile_env env = environment(a, &a->config);
    mc_profile_normalise(&a->config, &env, changed);
    char error[512];
    const int written = mc_profile_write(&a->doc, profile_name(a), &a->config, a->path, a->roms, error, sizeof error);
    if (!written) set_status(a, UI_ICON_STOP, "The change could not be written: %s", error);
    mc_config back;
    if (mc_profile_read(&a->doc, profile_name(a), a->path, a->roms, &back, error, sizeof error)) {
        a->config = back;
        a->config_ok = 1;
        a->config_error[0] = 0;
    }
    refresh_list(a);
    settle_model(a);
    sync_texts(a, 0);
    return written;
}

/* The selected profile from the document, exactly as written: nothing of
 * the profile shown before stays. A profile the model's rules change (or
 * that does not read) is written back corrected. */
static void load_selected(cfg_app *a) {
    char error[512];
    a->changes++;
    a->model = NULL;
    a->disk_selected = a->disk_scroll = a->share_selected = a->share_scroll = 0;
    a->config_ok = mc_profile_read(&a->doc, profile_name(a), a->path, a->roms, &a->config, error, sizeof error);
    if (!a->config_ok) {
        snprintf(a->config_error, sizeof a->config_error, "%s", error);
        /* Its model's defaults, until a change writes the profile anew. */
        mc_config_defaults(&a->config);
        snprintf(a->config.source, sizeof a->config.source, "%s", a->path);
        const mc_model *m = a->profile_count ? mc_model_find(a->models[a->selected]) : NULL;
        if (m) a->config.model = (uint32_t)mc_model_index(m);
        mc_profile_env env = environment(a, &a->config);
        mc_profile_preset(&a->config, &env);
    } else {
        a->config_error[0] = 0;
        mc_config fixed = a->config;
        mc_profile_env env = environment(a, &fixed);
        mc_profile_normalise(&fixed, &env, MC_CHANGED_NONE);
        if (!mc_profile_same(&fixed, &a->config)) {
            a->config = fixed;
            commit(a, MC_CHANGED_NONE);
            set_status(a, UI_ICON_NOTE, "Settings %s's model cannot use were corrected (not saved yet)",
                       profile_name(a) ? profile_name(a) : "the configuration");
        }
    }
    settle_model(a);
    a->custom_mhz = a->model && a->config.cpu_hz != a->model->cpu_hz;
    sync_texts(a, 1);
}

static int load(cfg_app *a) {
    mc_ydoc_free(&a->doc);
    free(a->saved);
    a->saved = NULL;
    FILE *f = fopen(a->path, "rb");
    a->exists = f != NULL;
    if (f) fclose(f);
    const int ok = a->exists ? mc_ydoc_load(&a->doc, a->path)
                             : mc_ydoc_parse(&a->doc, (const char *)mc_default_yaml, sizeof mc_default_yaml);
    if (!ok) return 0;
    if (a->exists) a->saved = mc_ydoc_text(&a->doc, NULL);
    refresh_list(a);
    return 1;
}

int cfg_app_init(cfg_app *a, const char *path, const char *program_folder, const mc_rom_index *roms, const cfg_host *host) {
    memset(a, 0, sizeof *a);
    if (host) a->host = *host;
    a->status_icon = -1;
    a->details_open = 1;
    snprintf(a->path, sizeof a->path, "%s", path);
    snprintf(a->folder, sizeof a->folder, "%s", path);
    char *slash = strrchr(a->folder, '/'), *back = strrchr(a->folder, '\\');
    if (back > slash) slash = back;
    if (slash) slash[1] = 0; else a->folder[0] = 0;
    if (roms) a->roms = roms;
    else {
        /* The roms folders, scanned once. */
        mc_config where;
        mc_config_defaults(&where);
        snprintf(where.source, sizeof where.source, "%s", path);
        char folders[2][MC_CONFIG_PATH];
        const size_t count = mc_config_rom_folders(&where, program_folder, folders);
        const char *list[2] = {folders[0], count > 1 ? folders[1] : NULL};
        if (!mc_rom_index_scan(&a->own_roms, list, count)) return 0;
        a->owns_roms = 1;
        a->roms = &a->own_roms;
    }
    if (!load(a)) return 0;
    a->selected = a->profile_count && a->default_index >= 0 ? a->default_index : 0;
    load_selected(a);
    if (!a->exists) set_status(a, UI_ICON_NOTE, "%s does not exist yet: Save writes it with these settings.", a->path);
    return 1;
}
void cfg_app_free(cfg_app *a) {
    cfg_picker_close(a);
    mc_ydoc_free(&a->doc);
    free(a->saved);
    free(a->declined);
    if (a->owns_roms) mc_rom_index_free(&a->own_roms);
}
int cfg_app_dirty(const cfg_app *a) {
    if (!a->exists || !a->saved) return 1;
    char *text = mc_ydoc_text(&a->doc, NULL);
    const int dirty = !text || strcmp(text, a->saved) != 0;
    free(text);
    return dirty;
}
int cfg_app_save(cfg_app *a) {
    if (a->doc_error[0]) { set_status(a, UI_ICON_STOP, "Not saved: %s", a->doc_error); return 0; }
    if (!mc_ydoc_save(&a->doc, a->path)) { set_status(a, UI_ICON_STOP, "Cannot write %s", a->path); return 0; }
    free(a->saved);
    a->saved = mc_ydoc_text(&a->doc, NULL);
    free(a->declined);
    a->declined = NULL;
    a->exists = 1;
    set_status(a, UI_ICON_OK, "Saved %s", a->path);
    return 1;
}
int cfg_app_select(cfg_app *a, const char *name) {
    for (size_t i = 0; i < a->profile_count; ++i)
        if (!strcmp(a->names[i], name)) {
            if ((int)i != a->selected) { a->status[0] = 0; a->status_icon = -1; }
            a->selected = (int)i;
            memset(a->marked, 0, sizeof a->marked);
            a->marked[i] = 1;
            a->anchor = (int)i;
            a->reveal = 1;
            load_selected(a);
            return 1;
        }
    return 0;
}
void cfg_app_set_running(cfg_app *a, const char *profile) {
    a->has_running = profile != NULL;
    snprintf(a->running, sizeof a->running, "%s", profile ? profile : "");
}
void cfg_app_set_problem(cfg_app *a, const char *problem) {
    snprintf(a->problem, sizeof a->problem, "%s", problem ? problem : "");
    snprintf(a->problem_profile, sizeof a->problem_profile, "%s", profile_name(a) ? profile_name(a) : "");
    if (problem && *problem) set_status(a, UI_ICON_STOP, "The Macintosh could not start: %s", problem);
}
int cfg_app_profile(cfg_app *a, const char *profile, mc_config *out) {
    char error[512];
    return mc_profile_read(&a->doc, profile, a->path, a->roms, out, error, sizeof error);
}
/* The window closes: back to the Macintosh, or to none. */
static void leave(cfg_app *a) {
    if (a->host.resume) a->host.resume(a->host.ctx);
    else if (a->host.close) a->host.close(a->host.ctx);
}
/* Whether the running profile as edited differs from the Macintosh running it
 * in what only a new start changes. */
static void restart_running(cfg_app *a);
static int restart_pending(cfg_app *a) {
    if (!a->host.resume || !a->running_from) return 0;
    mc_config edited;
    return cfg_app_profile(a, a->has_running ? a->running : NULL, &edited) && mc_profile_needs_restart(a->running_from, &edited);
}

/* --- media ------------------------------------------------------------------------------- */

static void add_disk(cfg_app *a, const char *path) {
    mc_config *c = &a->config;
    if (c->disk_count >= MC_CONFIG_DISKS) { set_status(a, UI_ICON_WARNING, "At most ten hard disks.%s", ""); return; }
    mc_disk_config *d = &c->disks[c->disk_count++];
    memset(d, 0, sizeof *d);
    snprintf(d->image, sizeof d->image, "%s", path);
    /* The internal IDE disk of the models that have one, while it is free. */
    if (a->model && (mc_machine_features(a->model) & MC_FEATURE_ATA_DISK)) {
        int ata = 0;
        for (size_t i = 0; i + 1 < c->disk_count; ++i) ata |= c->disks[i].bus == MC_BUS_ATA;
        if (!ata) d->bus = MC_BUS_ATA;
    }
    const size_t before = c->disk_count;
    commit(a, MC_CHANGED_STORAGE);
    if (a->config.disk_count < before) { set_status(a, UI_ICON_WARNING, "%s is in a drive already", base_name(path)); return; }
    a->disk_selected = (int)a->config.disk_count - 1;
    set_status(a, UI_ICON_NOTE, "Added the hard disk %s", base_name(path));
}
static void put_floppy(cfg_app *a, int drive, const char *path) {
    mc_config *c = &a->config;
    if (c->cdrom_bus == MC_BUS_ATA) { set_status(a, UI_ICON_WARNING, "The expansion bay holds the CD-ROM drive: no floppy drive.%s", ""); return; }
    while (c->floppy_count <= (size_t)drive) { c->floppy_off[c->floppy_count] = 1; c->floppy_count++; }
    c->floppy_off[drive] = 0;
    snprintf(c->floppies[drive].image, sizeof c->floppies[drive].image, "%s", path);
    c->floppies[drive].readonly = 0;
    a->media_chosen++;
    commit(a, MC_CHANGED_STORAGE);
    if (!a->config.floppies[drive].image[0]) set_status(a, UI_ICON_WARNING, "%s is in a drive already", base_name(path));
    else set_status(a, UI_ICON_NOTE, drive ? "Floppy drive 2: %s" : "Floppy drive 1: %s", base_name(path));
}
static void put_cdrom(cfg_app *a, const char *path) {
    char cue[MC_CONFIG_PATH];
    if (path) path = mc_disk_cue_sheet(path, cue, sizeof cue);
    snprintf(a->config.cdrom, sizeof a->config.cdrom, "%s", path ? path : "");
    a->media_chosen++;
    commit(a, MC_CHANGED_STORAGE);
    if (path && !a->config.cdrom[0]) set_status(a, UI_ICON_WARNING, "%s is a writable disk already", base_name(path));
    else if (path) set_status(a, UI_ICON_NOTE, "CD-ROM: %s", base_name(path));
}
void cfg_app_put_cdrom(cfg_app *a, const char *path) { put_cdrom(a, path && path[0] ? path : NULL); }
static void add_share(cfg_app *a, const char *path) {
    mc_config *c = &a->config;
    if (c->share_count >= MC_CONFIG_SHARES) { set_status(a, UI_ICON_WARNING, "At most eight shared folders.%s", ""); return; }
    mc_share_config *f = &c->shares[c->share_count];
    memset(f, 0, sizeof *f);
    snprintf(f->path, sizeof f->path, "%s", path);
    /* the folder's own name, made unique and short enough */
    const char *name = path;
    for (const char *s = path; *s; ++s) if ((*s == '/' || *s == '\\') && s[1]) name = s + 1;
    char base[MC_CONFIG_SHARE_NAME];
    size_t n = 0, chars = 0;
    for (const char *s = name; *s && *s != '/' && *s != '\\' && n + 1 < sizeof base; ++s) {
        if (((unsigned char)*s & 0xC0) != 0x80 && ++chars > 24) break;
        base[n++] = *s == ':' ? '-' : *s;
    }
    base[n] = 0;
    if (!n) snprintf(base, sizeof base, "Volume");
    snprintf(f->name, sizeof f->name, "%s", base);
    for (int k = 2; k < 100; ++k) {
        int taken = 0;
        for (size_t i = 0; i < c->share_count; ++i) if (!strcmp(c->shares[i].name, f->name)) taken = 1;
        if (!taken) break;
        snprintf(f->name, sizeof f->name, "%s %d", base, k);
    }
    char shown[MC_CONFIG_SHARE_NAME];
    snprintf(shown, sizeof shown, "%s", f->name);
    c->share_count++;
    commit(a, MC_CHANGED_OTHER);
    a->share_selected = (int)a->config.share_count - 1;
    sync_texts(a, 1);
    set_status(a, UI_ICON_NOTE, "Shared the folder %s", shown);
}

/* The result of a file chooser. */
static void picked(cfg_app *a, int kind, int drive, const char *path) {
    switch (kind) {
    case CFG_PICK_DISK: add_disk(a, path); break;
    case CFG_PICK_FLOPPY: put_floppy(a, drive, path); break;
    case CFG_PICK_CDROM: put_cdrom(a, path); break;
    case CFG_PICK_FOLDER: add_share(a, path); break;
    case CFG_PICK_OUTPUT:
        a->config.ports[drive].type = MC_PORT_OUTPUT;
        snprintf(a->config.ports[drive].path, sizeof a->config.ports[drive].path, "%s", path);
        commit(a, MC_CHANGED_OTHER);
        break;
    default: break;
    }
}
/* The file picker (config_picker.c) opens as a dialog of the window. */
static void choose(cfg_app *a, int kind, int drive) { cfg_picker_open(a, kind, drive); }
static void pick_done(cfg_app *a) {
    a->pending_pick = 0;
    if (a->dialog == CFG_DIALOG_PICK) a->dialog = CFG_DIALOG_NONE;
    cfg_picker_close(a);
}
void cfg_app_pick_result(cfg_app *a, const char *path) {
    if (!a->pending_pick) return;
    const int kind = a->pending_pick - 1, drive = a->pending_drive;
    pick_done(a);
    picked(a, kind, drive, path);
}

void cfg_app_drop(cfg_app *a, const char *path) {
    char error[256];
    mc_disk *d = mc_disk_open(path, 0, 512, error, sizeof error);
    const int floppy = d && mc_disk_floppy(d);
    mc_disk_close(d);
    if (floppy) {
        const mc_config *c = &a->config;
        const int first_full = c->floppy_count > 0 && !c->floppy_off[0] && c->floppies[0].image[0];
        put_floppy(a, first_full ? 1 : 0, path);
        return;
    }
    /* A hard disk holds HFS (bare or partitioned); anything else that is
     * whole 2048-byte sectors goes into the CD-ROM drive. */
    const mc_volume_info *v = volume(a, path);
    if (v && (v->kind == MC_IMAGE_HFS || v->kind == MC_IMAGE_PARTITIONED || v->kind == MC_IMAGE_HFS_PLUS)) {
        add_disk(a, path);
        return;
    }
    mc_disk *cd = mc_disk_open(path, 0, 2048, error, sizeof error);
    if (cd) {
        mc_disk_close(cd);
        put_cdrom(a, path);
        return;
    }
    set_status(a, UI_ICON_WARNING, "Not a disk image Livadix can use: %s", path);
}

/* --- profiles ------------------------------------------------------------------------------ */

/* A file without profiles: gains them, its settings shared and a first
 * profile "Default" that keeps them. */
static void ensure_profiles(cfg_app *a) {
    if (a->profile_count) return;
    const char *path[] = {"profiles", "Default"};
    mc_ydoc_set(&a->doc, path, 2, "{}");
    const char *old_id[]={"emulation","nvram_id"};
    mc_ydoc_remove(&a->doc,old_id,2);
    const char *new_id[]={"profiles","Default","emulation","nvram_id"};
    mc_ydoc_set(&a->doc,new_id,4,a->config.nvram_id);
    if(a->config.nvram[0]){
        const char *nv[]={"profiles","Default","emulation","nvram"};char value[MC_CONFIG_PATH*2];
        mc_ydoc_set(&a->doc,nv,4,mc_ydoc_quote_path(a->config.nvram,value,sizeof value));
    }
    const char *def[] = {"default"};
    mc_ydoc_set(&a->doc, def, 1, "Default");
    refresh_list(a);
    a->selected = 0;
}
static int name_taken(const cfg_app *a, const char *name) {
    for (size_t i = 0; i < a->profile_count; ++i) if (!strcmp(a->names[i], name)) return 1;
    return 0;
}
static const char *dialog_check(const cfg_app *a) {
    if (!a->dialog_name[0]) return "A profile needs a name.";
    if (name_taken(a, a->dialog_name) && !(a->dialog == CFG_DIALOG_RENAME && profile_name(a) && !strcmp(profile_name(a), a->dialog_name)))
        return "There is a profile of that name already.";
    return NULL;
}
static void dialog_ok(cfg_app *a) {
    char quoted[MC_CONFIG_NAME + 8];
    mc_config fresh;
    if((a->dialog==CFG_DIALOG_NEW||a->dialog==CFG_DIALOG_DUPLICATE)&&!mc_config_new_nvram_identity(&fresh)){
        set_status(a,UI_ICON_STOP,"A fresh NVRAM identity could not be generated",NULL);return;
    }
    switch (a->dialog) {
    case CFG_DIALOG_NEW: {
        /* A new profile: the Built-in machine and its settings. */
        ensure_profiles(a);
        const char *path[] = {"profiles", a->dialog_name, "model"};
        mc_ydoc_set(&a->doc, path, 3, MC_MODEL_BUILTIN);
        const char *identity[]={"profiles",a->dialog_name,"emulation","nvram_id"};
        mc_ydoc_set(&a->doc,identity,4,fresh.nvram_id);
        refresh_list(a);
        cfg_app_select(a, a->dialog_name);
        mc_profile_env env = environment(a, &a->config);
        mc_profile_preset(&a->config, &env);
        if (commit(a, MC_CHANGED_MODEL)) set_status(a, UI_ICON_NOTE, "New profile %s", a->dialog_name);
        break;
    }
    case CFG_DIALOG_DUPLICATE: {
        char from[MC_CONFIG_NAME];
        snprintf(from, sizeof from, "%s", profile_name(a) ? profile_name(a) : "Default");
        ensure_profiles(a);
        const char *path[] = {"profiles", from};
        mc_ydoc_duplicate(&a->doc, path, 2, a->dialog_name);
        const char *identity[]={"profiles",a->dialog_name,"emulation","nvram_id"};
        mc_ydoc_set(&a->doc,identity,4,fresh.nvram_id);
        refresh_list(a);
        cfg_app_select(a, a->dialog_name);
        set_status(a, UI_ICON_NOTE, "Duplicated as %s", a->dialog_name);
        break;
    }
    case CFG_DIALOG_RENAME: {
        char from[MC_CONFIG_NAME];
        snprintf(from, sizeof from, "%s", profile_name(a));
        /* Freeze the existing identity and base path before its name changes. */
        if(!commit(a,0))break;
        const char *path[] = {"profiles", from};
        mc_ydoc_rename(&a->doc, path, 2, a->dialog_name);
        /* default: follows the name, and so does the running profile. */
        const char *def[] = {"default"};
        if (mc_ydoc_has(&a->doc, def, 1) && a->default_index == a->selected)
            mc_ydoc_set(&a->doc, def, 1, mc_ydoc_quote(a->dialog_name, quoted, sizeof quoted));
        if (a->has_running && !strcmp(a->running, from)) snprintf(a->running, sizeof a->running, "%s", a->dialog_name);
        refresh_list(a);
        cfg_app_select(a, a->dialog_name);
        set_status(a, UI_ICON_NOTE, "Renamed to %s", a->dialog_name);
        break;
    }
    case CFG_DIALOG_DELETE: {
        /* Every selected profile; the one before the first of them shown. */
        static char gone[MC_CONFIG_PROFILES][MC_CONFIG_NAME];
        size_t n = 0;
        int first = -1, was_default = 0;
        for (size_t i = 0; i < a->profile_count; ++i) {
            if (!a->marked[i]) continue;
            if (first < 0) first = (int)i;
            was_default |= (int)i == a->default_index;
            snprintf(gone[n++], sizeof gone[0], "%s", a->names[i]);
        }
        for (size_t i = 0; i < n; ++i) {
            const char *path[] = {"profiles", gone[i]};
            mc_ydoc_remove(&a->doc, path, 2);
        }
        if (was_default) { const char *def[] = {"default"}; mc_ydoc_remove(&a->doc, def, 1); }
        refresh_list(a);
        if (a->profile_count) {
            const int show = first > 0 ? first - 1 : 0;
            a->selected = -1;
            cfg_app_select(a, a->names[show < (int)a->profile_count ? show : 0]);
        }
        if (n == 1) set_status(a, UI_ICON_NOTE, "Deleted the profile %s", gone[0]);
        else {
            char count[16];
            snprintf(count, sizeof count, "%u", (unsigned)n);
            set_status(a, UI_ICON_NOTE, "Deleted %s profiles", count);
        }
        break;
    }
    case CFG_DIALOG_QUIT:
        if (cfg_app_save(a) && a->host.quit) a->host.quit(a->host.ctx);
        break;
    case CFG_DIALOG_CLOSE:
        if (!cfg_app_save(a)) break;
        /* Saved; a change the running Macintosh needs a new start for is
         * asked about as Save asks, and Later then closes. */
        if (restart_pending(a)) { a->restart_closes = 1; a->dialog = CFG_DIALOG_RESTART; return; }
        leave(a);
        break;
    case CFG_DIALOG_RESTART:
        a->dialog = CFG_DIALOG_NONE;
        a->restart_closes = 0;
        restart_running(a);
        return;
    default: break;
    }
    a->dialog = CFG_DIALOG_NONE;
}
static void open_dialog(cfg_app *a, int which) {
    a->dialog = which;
    a->dialog_message[0] = 0;
    if (which == CFG_DIALOG_NEW) snprintf(a->dialog_name, sizeof a->dialog_name, "New profile");
    else if (which == CFG_DIALOG_DUPLICATE) snprintf(a->dialog_name, sizeof a->dialog_name, "%.110s copy", profile_name(a) ? profile_name(a) : "Default");
    else if (which == CFG_DIALOG_RENAME) snprintf(a->dialog_name, sizeof a->dialog_name, "%s", profile_name(a));
    ui_focus(&a->ui, which <= CFG_DIALOG_RENAME ? ui_id("dialog-name", 0) : 0);
    a->ui.edit_id = 0;
}
void cfg_app_quit(cfg_app *a) {
    if (cfg_app_dirty(a)) { a->dialog = CFG_DIALOG_QUIT; return; }
    if (a->host.quit) a->host.quit(a->host.ctx);
}
void cfg_app_close(cfg_app *a) {
    if (a->dialog != CFG_DIALOG_NONE) return;
    char *text = cfg_app_dirty(a) ? mc_ydoc_text(&a->doc, NULL) : NULL;
    const int ask = text && (!a->declined || strcmp(text, a->declined));
    free(text);
    if (!ask) { leave(a); return; }
    snprintf(a->dialog_message, sizeof a->dialog_message, "The changes to %s are not saved. Save them before closing?", base_name(a->path));
    a->dialog = CFG_DIALOG_CLOSE;
}
/* Whether the selected profile can start: it reads, its ROM is there and
 * every image it names exists. */
static const char *start_problem(cfg_app *a) {
    static char why[600];
    if (a->doc_error[0]) return a->doc_error;
    if (!a->rom_ok) return a->rom_note;
    const mc_config *c = &a->config;
    for (size_t i = 0; i < c->disk_count; ++i)
        if (!file_exists(c->disks[i].image)) { snprintf(why, sizeof why, "The hard disk image %s is missing", c->disks[i].image); return why; }
    for (size_t i = 0; i < c->floppy_count; ++i)
        if (!c->floppy_off[i] && c->floppies[i].image[0] && !file_exists(c->floppies[i].image)) {
            snprintf(why, sizeof why, "The floppy image %s is missing", c->floppies[i].image);
            return why;
        }
    if (c->cdrom[0] && !file_exists(c->cdrom)) { snprintf(why, sizeof why, "The CD image %s is missing", c->cdrom); return why; }
    return NULL;
}
static void start(cfg_app *a) {
    const char *problem = start_problem(a);
    if (problem) { set_status(a, UI_ICON_STOP, "%s", problem); return; }
    a->problem[0] = 0;
    if (a->host.start) a->host.start(a->host.ctx, profile_name(a), &a->config);
}
/* The running profile starts again as edited, whichever profile is shown. */
static void restart_running(cfg_app *a) {
    const char *selected = profile_name(a);
    if (selected ? a->has_running && !strcmp(selected, a->running) : !a->has_running) { start(a); return; }
    mc_config config;
    if (!cfg_app_profile(a, a->running, &config)) { set_status(a, UI_ICON_STOP, "The profile %s does not read", a->running); return; }
    a->problem[0] = 0;
    if (a->host.start) a->host.start(a->host.ctx, a->running, &config);
}

/* --- drawing helpers -------------------------------------------------------------------------- */

static const ui_font *sys(void) { return ui_font_get(UI_FONT_SYSTEM); }
/* A label at the left of a control row, right-aligned to x. */
static void caption(cfg_app *a, int x, int y, const char *text) {
    ui_label(&a->ui, x - ui_text_width(sys(), text) - 8, y + 5, text, UI_FONT_SYSTEM, UI_BLACK);
}
static void note(cfg_app *a, int x, int y, const char *text) { ui_label(&a->ui, x, y, text, UI_FONT_SMALL, UI_DARK); }
/* A pop-up of named values: the new choice, or -1. */
static int choice_popup(cfg_app *a, const char *id, ui_rect r, const char *label, const char *const *names, int count, int current,
                        const uint8_t *enabled) {
    int sel = current;
    if (ui_popup(&a->ui, ui_id(id, 0), r, label, names, count, &sel, enabled, NULL)) return sel;
    return -1;
}

/* --- tabs --------------------------------------------------------------------------------------- */

/* The Model pop-up's filters (cfg_app.filter_*): whether m passes all of
 * them but the one numbered skip (0 year, 1 family, 2 processor, 3 kind;
 * -1: none skipped). */
enum { FILTER_YEAR, FILTER_FAMILY, FILTER_CPU, FILTER_FORM, FILTERS };
static int filter_value_ok(const cfg_app *a, const mc_model *m, int which) {
    char cpu[24];
    switch (which) {
    case FILTER_YEAR: return !a->filter_year || m->year == a->filter_year;
    case FILTER_FAMILY: return !a->filter_family || (a->filter_family == 2) == (m->family == MC_CPU_PPC);
    case FILTER_CPU: mc_model_cpu_type(m, cpu, sizeof cpu); return !a->filter_cpu[0] || !strcmp(cpu, a->filter_cpu);
    default: return !a->filter_form || m->form + 1 == a->filter_form;
    }
}
static int filters_pass(const cfg_app *a, const mc_model *m, int skip) {
    for (int k = 0; k < FILTERS; ++k)
        if (k != skip && !filter_value_ok(a, m, k)) return 0;
    return 1;
}
/* The models the Model pop-up can list: those whose ROM is in the roms
 * folders, and the one chosen (the stub only when chosen). */
static int model_listable(const cfg_app *a, const mc_model *m) {
    return mc_model_builtin(m) || (mc_model_index(m) < mc_model_count() && mc_rom_index_rom(a->roms, mc_model_rom(m)) != NULL);
}
/* How many listable models pass every filter. */
static int filtered_count(const cfg_app *a) {
    int n = 0;
    for (size_t i = 0; i < mc_model_count(); ++i) n += model_listable(a, mc_model_entry(i)) && filters_pass(a, mc_model_entry(i), -1);
    return n;
}
static void filter_clear(cfg_app *a, int which) {
    if (which == FILTER_YEAR) a->filter_year = 0;
    else if (which == FILTER_FAMILY) a->filter_family = 0;
    else if (which == FILTER_FORM) a->filter_form = 0;
    else a->filter_cpu[0] = 0;
}
/* The four filter pop-ups, flowed along the row from x (wrapping within w);
 * returns the y under them. A value is listed when a listable model has it;
 * choosing one clears the other filters that would leave no model; *changed
 * then is set. */
static int model_filters(cfg_app *a, int x, int y, int w, int *changed) {
    static const char *const cpus[] = {"68000", "68020", "68030", "68LC040", "68040", "PowerPC 601", "PowerPC 603",
                                       "PowerPC 603e", "PowerPC 604", "PowerPC 604e", "PowerPC 750"};
    enum { MAX_VALUES = 48 };
    static const char *const captions[FILTERS] = {"Year:", "Architecture:", "CPU:", "Type:"};
    static const char *const names[FILTERS] = {"Year", "Architecture", "CPU", "Type"};
    static const int widths[FILTERS] = {90, 100, 120, 110};
    static char texts[FILTERS][MAX_VALUES][24];
    const ui_font *f = ui_font_get(UI_FONT_SYSTEM);
    int fx = x, fy = y;
    for (int which = 0; which < FILTERS; ++which) {
        /* The values: Any, then those the listable models have, in order. */
        int values[MAX_VALUES], count = 1, current = 0;
        const char *items[MAX_VALUES];
        uint8_t enabled[MAX_VALUES];
        values[0] = 0;
        const int candidates = which == FILTER_YEAR ? 1999 : which == FILTER_FAMILY ? 2 : which == FILTER_CPU ? (int)(sizeof cpus / sizeof *cpus) : MC_FORMS;
        for (int v = which == FILTER_YEAR ? 1984 : 1; v <= candidates && count < MAX_VALUES; ++v) {
            int present = 0;
            for (size_t i = 0; i < mc_model_count() && !present; ++i) {
                const mc_model *m = mc_model_entry(i);
                if (!model_listable(a, m)) continue;
                char cpu[24];
                mc_model_cpu_type(m, cpu, sizeof cpu);
                const int has = which == FILTER_YEAR ? m->year == v : which == FILTER_FAMILY ? (v == 2) == (m->family == MC_CPU_PPC)
                              : which == FILTER_CPU ? !strcmp(cpu, cpus[v - 1]) : m->form + 1 == v;
                present = has;
            }
            if (!present) continue;
            values[count] = v;
            enabled[count] = 1;
            if (which == FILTER_YEAR) snprintf(texts[which][count], sizeof texts[0][0], "%d", v);
            else if (which == FILTER_FAMILY) snprintf(texts[which][count], sizeof texts[0][0], "%s", v == 2 ? "PowerPC" : "68k");
            else if (which == FILTER_CPU) snprintf(texts[which][count], sizeof texts[0][0], "%s", cpus[v - 1]);
            else snprintf(texts[which][count], sizeof texts[0][0], "%s", mc_model_form_name((unsigned)v - 1));
            count++;
        }
        snprintf(texts[which][0], sizeof texts[0][0], "Any");
        enabled[0] = 1;
        for (int i = 0; i < count; ++i) items[i] = texts[which][i];
        const int now = which == FILTER_YEAR ? a->filter_year : which == FILTER_FAMILY ? a->filter_family : which == FILTER_FORM ? a->filter_form : 0;
        for (int i = 1; i < count; ++i)
            if (which == FILTER_CPU ? !strcmp(a->filter_cpu, cpus[values[i] - 1]) : values[i] == now) current = i;
        /* The control, flowed along the row. */
        const int tw = ui_text_width(f, captions[which]);
        if (fx > x && fx + tw + 8 + widths[which] > x + w) { fx = x; fy += FIELD + 6; }
        if (fx == x) caption(a, x, fy, captions[which]);
        else { ui_label(&a->ui, fx, fy + 5, captions[which], UI_FONT_SYSTEM, UI_BLACK); fx += tw + 8; }
        const int sel = choice_popup(a, names[which], (ui_rect){fx, fy, widths[which], FIELD}, names[which], items, count, current, enabled);
        if (sel >= 0 && sel != current) {
            const int v = values[sel];
            if (which == FILTER_YEAR) a->filter_year = v;
            else if (which == FILTER_FAMILY) a->filter_family = v;
            else if (which == FILTER_FORM) a->filter_form = v;
            else snprintf(a->filter_cpu, sizeof a->filter_cpu, "%s", v ? cpus[v - 1] : "");
            /* The other filters' values that leave no model with this one go back to Any. */
            for (int k = 0; k < FILTERS && !filtered_count(a); ++k)
                if (k != which) filter_clear(a, k);
            *changed = 1;
        }
        fx += widths[which] + 14;
    }
    return fy + FIELD + 6;
}

/* An icon with its text beside it, the text (wrapped to w) centred
 * vertically on the icon; returns the height used. */
static int icon_note(cfg_app *a, int x, int y, int w, int icon, const char *text, uint32_t color) {
    ui *u = &a->ui;
    const int h = ui_wrapped(u, (ui_rect){x + 20, y, w - 20, 1}, text, UI_FONT_SMALL_BOLD, color);   /* measured */
    const int block = h > 2 ? h - 2 : h, top = block < 16 ? (16 - block) / 2 : 0;
    ui_icon(&u->canvas, icon, x, y + (block > 16 ? (block - 16) / 2 : 0), 0);
    ui_wrapped(u, (ui_rect){x + 20, y + top, w - 20, 0}, text, UI_FONT_SMALL_BOLD, color);
    return block > 16 ? block : 16;
}

static void machine_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    const int x = p.x + 110, w = p.w - 120;
    int y = p.y;
    const mc_model *chosen_model = mc_model_entry(c->model);
    const mc_model *builtin = mc_model_find(MC_MODEL_BUILTIN);
    /* The filters narrow the Model pop-up's list. */
    int filters_changed = 0;
    y = model_filters(a, x, y, w, &filters_changed);
    /* Model: Built-in, then every model whose ROM is in the roms folders
     * and that passes the filters; those without a hardware description are
     * listed, disabled. A chosen model the filters leave out shows as the
     * pop-up's text. */
    enum { MAX_ITEMS = UI_MENU_ITEMS };
    static char labels[MAX_ITEMS][96];
    const char *items[MAX_ITEMS];
    uint8_t enabled[MAX_ITEMS];
    const mc_model *models[MAX_ITEMS];
    int count = 0, current = 0;
    snprintf(labels[0], sizeof labels[0], "%s", builtin->name);
    models[0] = builtin; enabled[0] = 1; items[0] = labels[0]; count = 1;
    int listable = 0, shown = 0;
    current = chosen_model == builtin ? 0 : -1;
    for (size_t i = 0; i <= mc_model_count() && count < MAX_ITEMS; ++i) {
        const mc_model *m = mc_model_entry(i);
        const int chosen = m == chosen_model;
        if (m == builtin) continue;
        const int can_list = model_listable(a, m);
        listable += can_list;
        /* The stub machine (the hidden model) only when it is the one chosen. */
        if (!(can_list && filters_pass(a, m, -1)) && !(chosen && i == mc_model_count())) continue;
        shown += can_list;
        const int has_rom = mc_rom_index_rom(a->roms, mc_model_rom(m)) != NULL;
        const int emulated = mc_machine_emulated(m) || i == mc_model_count();
        if (!has_rom) snprintf(labels[count], sizeof labels[count], "%s (no ROM)", m->name);
        else if (!emulated) snprintf(labels[count], sizeof labels[count], "%s (not yet emulated)", m->name);
        else snprintf(labels[count], sizeof labels[count], "%s", m->name);
        items[count] = labels[count];
        /* A model can be chosen when Livadix can start it. */
        enabled[count] = (uint8_t)((emulated && has_rom) || chosen);
        models[count] = m;
        if (chosen) current = count;
        count++;
    }
    caption(a, x, y, "Model:");
    const int model_w = w > 340 ? 340 : w;
    char outside[96] = "";
    if (current < 0 && chosen_model)
        snprintf(outside, sizeof outside, "%s%s", chosen_model->name,
                 mc_rom_index_rom(a->roms, mc_model_rom(chosen_model)) ? "" : " (no ROM)");
    int sel = current;
    int picked = ui_popup(u, ui_id("model", 0), (ui_rect){x, y, model_w, FIELD}, "Model", items, count, &sel, enabled, outside);
    /* A filter just changed that leaves the model out: the first model it
     * lists that Livadix can start takes its place. */
    if (!picked && filters_changed && current < 0)
        for (int i = 1; i < count && !picked; ++i)
            if (enabled[i] && models[i] != chosen_model) { sel = i; picked = 1; }
    if (picked && sel != current && sel >= 0) {
        /* A model's best settings: nothing of the model before stays. */
        c->model = (uint32_t)mc_model_index(models[sel]);
        a->model = NULL;
        mc_profile_env env = environment(a, c);
        c->model = (uint32_t)mc_model_index(env.model);
        mc_profile_preset(c, &env);
        a->custom_mhz = 0;
        const int written = commit(a, MC_CHANGED_MODEL);
        sync_texts(a, 1);
        if (written) set_status(a, UI_ICON_NOTE, "The %s's own settings", a->model ? a->model->name : "model");
    }
    if (shown < listable && w >= model_w + 120) {
        char filtered[64];
        snprintf(filtered, sizeof filtered, "%d of %d models", shown, listable);
        note(a, x + model_w + 8, y + 7, filtered);
    }
    y += FIELD + 4;
    const mc_model *m = a->model;
    const mc_volume_info *boot = NULL;
    const char *boot_path = boot_disk(a, c, &boot);
    char line[512];
    if (m) {
        snprintf(line, sizeof line, "%s, %s%s, Gestalt type %u", m->name, m->cpu, m->fpu ? " with FPU" : "", (unsigned)m->gestalt);
        note(a, x, y, line);
        y += 12;
        mc_compat_describe(m, line, sizeof line);
        y += ui_wrapped(u, (ui_rect){x, y, w, 0}, line, UI_FONT_SMALL, UI_DARK);
        ui_register(u, ui_id("model-systems", 0), (ui_rect){x, y - 12, w, 12}, line, UI_HIT_TEXT);
    }
    const int rom_lines = a->rom_ok ? 24 : 36;
    ui_wrapped(u, (ui_rect){x, y, w, rom_lines}, a->rom_note, UI_FONT_SMALL, a->rom_ok ? UI_DARK : UI_ERROR);
    y += rom_lines + 2;
    /* Memory: the sizes the model takes; below what the System needs, not. */
    caption(a, x, y, "Memory:");
    static char ram_labels[64][48];
    const char *ram_items[64];
    uint8_t ram_enabled[64];
    uint32_t sizes[64];
    const size_t ram_count = mc_profile_ram_sizes(m, sizes, 64);
    mc_profile_env env = environment(a, c);
    uint32_t least = mc_profile_ram_least(&env);
    /* A model that cannot have what the System needs takes any of its sizes. */
    if (least && ram_count && sizes[ram_count - 1] < least * 1024u) least = 0;
    const uint32_t ram_kb = mc_config_ram_kb(c);
    int ram_current = -1;
    for (size_t i = 0; i < ram_count; ++i) {
        char text[24];
        snprintf(ram_labels[i], sizeof ram_labels[0], "%s MB", mc_config_ram_text(sizes[i], text, sizeof text));
        ram_items[i] = ram_labels[i];
        ram_enabled[i] = (uint8_t)(!least || sizes[i] >= least * 1024u || sizes[i] == ram_kb);
        if (sizes[i] == ram_kb) ram_current = (int)i;
    }
    sel = ram_current;
    if (ui_popup(u, ui_id("ram", 0), (ui_rect){x, y, 200, FIELD}, "Memory", ram_items, (int)ram_count, &sel, ram_enabled, NULL) &&
        sel >= 0 && sizes[sel] != ram_kb) {
        mc_config_set_ram_kb(c, sizes[sel]);
        commit(a, MC_CHANGED_RAM);
    }
    y += FIELD + 4;
    /* Under it: whether the startup disk's System has enough of it. */
    if (boot_path) {
        mc_compat memory;
        if ((m && ram_count && mc_compat_ram_most(boot, sizes[ram_count - 1], m->name, &memory) != MC_COMPAT_OK) ||
            mc_compat_ram(boot, mc_config_ram_kb(c), &memory) != MC_COMPAT_OK) {
            const int h = icon_note(a, x, y, w, UI_ICON_WARNING, memory.reason, UI_ERROR);
            ui_register(u, ui_id("memory-warning", 0), (ui_rect){x, y, w, h}, memory.reason, UI_HIT_TEXT);
            y += h + 2;
        } else {
            snprintf(line, sizeof line, "Enough for %s", boot->system_version >= 0x0760 ? "Mac OS" : "the System");
            y += icon_note(a, x, y, w, UI_ICON_OK, line, UI_DARK) + 2;
        }
    }
    y += 6;
    /* CPU speed: the model's clock, or any from 1 to 10000 MHz. */
    caption(a, x, y, "CPU speed:");
    const uint64_t real = m ? m->cpu_hz : 25000000u;
    char real_text[24];
    mc_config_mhz_text(real, real_text, sizeof real_text);
    int own = !a->custom_mhz;
    snprintf(line, sizeof line, "The model's clock (%s MHz)", real_text);
    if (ui_checkbox(u, ui_id("mhz-model", 0), x, y + 4, line, &own, 0)) {
        a->custom_mhz = !own;
        if (own && c->cpu_hz != real) { c->cpu_hz = real; commit(a, MC_CHANGED_OTHER); }
        sync_texts(a, 1);
    }
    y += ROW + 2;
    /* A logarithmic scale: 1 to 10000 MHz, the real clock marked. */
    double pos = log10((double)c->cpu_hz / 1e6) / 4.0;
    const double mark = log10((double)real / 1e6) / 4.0;
    if (ui_slider(u, ui_id("mhz", 0), (ui_rect){x, y, 240, 24}, &pos, mark, own)) {
        unsigned mhz = (unsigned)(pow(10.0, pos * 4.0) + 0.5);
        if (mhz < 1) mhz = 1;
        if (mhz > 10000) mhz = 10000;
        c->cpu_hz = (uint64_t)mhz * 1000000u;
        commit(a, MC_CHANGED_OTHER);
        sync_texts(a, 1);
    }
    if (ui_edit(u, ui_id("mhz-text", 0), (ui_rect){x + 252, y, 60, FIELD}, a->mhz_text, sizeof a->mhz_text, own)) {
        const uint64_t hz = mc_config_parse_mhz(a->mhz_text);
        if (hz && hz != c->cpu_hz) { c->cpu_hz = hz; commit(a, MC_CHANGED_OTHER); }
    }
    ui_label(u, x + 318, y + 5, "MHz", UI_FONT_SYSTEM, UI_BLACK);
    note(a, x, y + 25, "1");
    note(a, x + 228, y + 25, "10000");
    snprintf(line, sizeof line, "real: %s", real_text);
    note(a, x + (int)(mark * 229) - 8, y + 25, line);
    y += 44;
    if(m && mc_model_ppc(m)){note(a,x,y,"PowerPC clock target; instruction timing is approximate.");y += 14;}
    /* The ADB and power controller: its protocol model or Apple's firmware
     * (emulation.controller), when the model has one with firmware. */
    const char *fw_key = m ? mc_model_controller_firmware(m) : NULL;
    if (fw_key) {
        const mc_firmware *fw = mc_firmware_find(fw_key);
        const mc_rom_file *file = mc_rom_index_firmware(a->roms, fw);
        static const char *const levels[] = {"Automatic", "Protocol model", "Apple's firmware"};
        const uint8_t level_enabled[] = {1, 1, (uint8_t)(file != NULL)};
        caption(a, x, y, "Controller:");
        const int level = choice_popup(a, "controller", (ui_rect){x, y, 170, FIELD}, "Controller", levels, 3, (int)c->controller, level_enabled);
        if (level >= 0 && (uint32_t)level != c->controller) { c->controller = (uint32_t)level; commit(a, MC_CHANGED_OTHER); }
        if (file) snprintf(line, sizeof line, "%s: %s", fw->part, file->name);
        else snprintf(line, sizeof line, "%s: not in roms/ (the protocol model runs)", fw->part);
        ui_wrapped(u, (ui_rect){x + 180, y + 1, w - 180, 24}, line, UI_FONT_SMALL, UI_DARK);
        y += FIELD + 8;
    }
    /* The startup disk and whether the model runs it. */
    if (m && (m->console || mc_model_builtin(m))) {
        /* A console starts its disc, the Built-in machine its own ROM. */
        caption(a, x, y, m->console ? "Startup disc:" : "Startup:");
        char text[MC_CONFIG_PATH + 64];
        if (!m->console) snprintf(text, sizeof text, "Livadix's own ROM; no disk");
        else if (c->cdrom[0]) snprintf(text, sizeof text, "%s", base_name(c->cdrom));
        else snprintf(text, sizeof text, "none: put its CD in the CD-ROM drive (Storage)");
        ui_label(u, x, y + 5, text, UI_FONT_SYSTEM, m->console && !c->cdrom[0] ? UI_ERROR : UI_DARK);
        y += FIELD + 8;
    } else {
        caption(a, x, y, "Startup disk:");
        if (!boot_path) {
            ui_label(u, x, y + 5, "none of the disks holds a System Folder", UI_FONT_SYSTEM, UI_DARK);
            y += FIELD + 8;
        } else {
            char text[512];
            mc_volume_describe(boot, text, sizeof text);
            y += 5 + ui_wrapped(u, (ui_rect){x, y + 5, w, 28}, text, UI_FONT_SMALL, UI_BLACK) + 4;
            mc_compat compat;
            mc_compat_check(m, boot, mc_config_ram_kb(c), &compat);
            y += icon_note(a, x, y, w, compat.status == MC_COMPAT_OK ? UI_ICON_OK : UI_ICON_WARNING, compat.reason,
                           compat.status == MC_COMPAT_OK ? UI_BLACK : UI_ERROR) + 12;
        }
    }
    /* The emulation's switches. */
    caption(a, x, y, "Emulation:");
    y += 4;
    int jit = c->jit, chime = c->startup_sound, fpu = c->fpu_full;
    const int fpu_applies = m && m->fpu && strstr(m->cpu, "68040") == m->cpu;
    if (ui_checkbox(u, ui_id("chime", 0), x, y, "Startup chime", &chime, 0)) { c->startup_sound = chime; commit(a, MC_CHANGED_OTHER); }
    y += ROW;
    if (ui_checkbox(u, ui_id("jit", 0), x, y, "Translation of the Macintosh code to x64 (JIT)", &jit, 0)) { c->jit = jit; commit(a, MC_CHANGED_OTHER); }
    y += ROW;
    if (ui_checkbox(u, ui_id("fpu", 0), x, y, "Full 68881 instruction set in the 68040's FPU", &fpu, !fpu_applies)) {
        c->fpu_full = fpu;
        commit(a, MC_CHANGED_OTHER);
    }
    y += ROW;
    /* emulation.m68k_accelerator: for a Power Macintosh with the translator on. */
    int accelerator = c->m68k_accelerator;
    if (ui_checkbox(u, ui_id("m68k-accelerator", 0), x, y, "68k code run on Livadix's 68k processor (Power Macintosh)", &accelerator,
                    !(m && mc_model_ppc(m) && c->jit))) {
        c->m68k_accelerator = accelerator;
        commit(a, MC_CHANGED_OTHER);
    }
    y += ROW;
    /* emulation.pmmu: the Macintosh II's PMMU socket. */
    int pmmu = c->pmmu;
    if (ui_checkbox(u, ui_id("pmmu", 0), x, y, "A 68851 PMMU in the Macintosh II's socket", &pmmu, !mc_model_pmmu_socket(m))) {
        c->pmmu = pmmu;
        commit(a, MC_CHANGED_OTHER);
    }
    y += ROW;
    /* Closing the window: shut the Macintosh down first, or quit at once
     * (emulation.close_action). */
    int shut = c->close_action == MC_CLOSE_SHUTDOWN;
    if (ui_checkbox(u, ui_id("close", 0), x, y, "Shutdown of the Macintosh before the window closes", &shut, 0)) {
        c->close_action = shut ? MC_CLOSE_SHUTDOWN : MC_CLOSE_QUIT;
        commit(a, MC_CHANGED_OTHER);
    }
}

static void video_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    const mc_model *model = a->model;
    const int x = p.x + 110;
    int y = p.y;
    /* The display: its built-in video, or the card, only what the model has. */
    static const char *const cards[] = {"Automatic", "Built-in video", "Livadix Display Card"};
    const uint8_t card_enabled[] = {1, (uint8_t)mc_profile_has_builtin(model), (uint8_t)mc_profile_has_card(model)};
    caption(a, x, y, "Display:");
    int s = choice_popup(a, "card", (ui_rect){x, y, 220, FIELD}, "Display", cards, 3, (int)c->video_card, card_enabled);
    if (s >= 0 && (uint32_t)s != c->video_card) { c->video_card = (uint32_t)s; commit(a, MC_CHANGED_CARD); }
    y += FIELD + 8;
    /* Resolution and depth: only what a display of the model shows. */
    static char res_labels[MC_CONFIG_RESOLUTIONS][40];
    const char *res_items[MC_CONFIG_RESOLUTIONS];
    uint8_t res_enabled[MC_CONFIG_RESOLUTIONS];
    int res_current = -1;
    mc_config any = *c;
    any.video_card = MC_VIDEO_CARD_AUTO;
    for (int i = 0; i < MC_CONFIG_RESOLUTIONS; ++i) {
        const unsigned w = mc_config_resolutions[i][0], h = mc_config_resolutions[i][1];
        snprintf(res_labels[i], sizeof res_labels[0], "%u x %u", w, h);
        res_items[i] = res_labels[i];
        res_enabled[i] = mc_profile_depths(&any, model, w, h) != 0;
        if (w == c->width && h == c->height) res_current = i;
    }
    caption(a, x, y, "Resolution:");
    s = choice_popup(a, "resolution", (ui_rect){x, y, 220, FIELD}, "Resolution", res_items, MC_CONFIG_RESOLUTIONS, res_current, res_enabled);
    if (s >= 0 && s != res_current) {
        c->width = mc_config_resolutions[s][0];
        c->height = mc_config_resolutions[s][1];
        commit(a, MC_CHANGED_RESOLUTION);
    }
    y += FIELD + 6;
    static char depth_labels[MC_CONFIG_DEPTHS][40];
    static const char *const depth_names[] = {"Black & white", "4 grays or colors", "16", "256", "Thousands", "Millions"};
    const char *depth_items[MC_CONFIG_DEPTHS];
    uint8_t depth_enabled[MC_CONFIG_DEPTHS];
    const unsigned allowed = mc_profile_depths(c, model, c->width, c->height);
    int depth_current = -1;
    for (int i = 0; i < MC_CONFIG_DEPTHS; ++i) {
        snprintf(depth_labels[i], sizeof depth_labels[0], "%u bits (%s)", (unsigned)mc_config_depths[i], depth_names[i]);
        depth_items[i] = depth_labels[i];
        depth_enabled[i] = (allowed & MC_DEPTH_BIT(mc_config_depths[i])) != 0;
        if (mc_config_depths[i] == c->depth) depth_current = i;
    }
    caption(a, x, y, "Depth:");
    s = choice_popup(a, "depth", (ui_rect){x, y, 220, FIELD}, "Depth", depth_items, MC_CONFIG_DEPTHS, depth_current, depth_enabled);
    if (s >= 0 && s != depth_current) { c->depth = mc_config_depths[s]; commit(a, MC_CHANGED_DEPTH); }
    y += FIELD + 6;
    static const char *const refresh_items[] = {"60 Hz", "30 Hz", "20 Hz", "15 Hz", "12 Hz", "10 Hz"};
    static const unsigned refresh_values[] = {60, 30, 20, 15, 12, 10};
    int rc = 0;
    for (int i = 0; i < 6; ++i) if (refresh_values[i] == c->refresh) rc = i;
    caption(a, x, y, "Refresh:");
    s = choice_popup(a, "refresh", (ui_rect){x, y, 120, FIELD}, "Refresh", refresh_items, 6, rc, NULL);
    if (s >= 0 && s != rc) { c->refresh = refresh_values[s]; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 14;
    /* How the window shows the screen. */
    static const char *const looks[] = {"Plain", "CRT", "LCD"};
    caption(a, x, y, "Look:");
    /* CRT only for a model with a CRT (built in or its monitor), LCD only for a PowerBook's panel. */
    uint8_t look_enabled[3] = {1, 1, 1};
    for (uint32_t k = 0; model && k < 3; ++k) look_enabled[k] = (uint8_t)mc_config_look_ok(model, k);
    s = choice_popup(a, "look", (ui_rect){x, y, 120, FIELD}, "Look", looks, 3, (int)c->look, look_enabled);
    if (s >= 0 && (uint32_t)s != c->look) { c->look = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    static const char *const monitors[] = {"Color", "White (monochrome CRT)", "Gray (PowerBook LCD)"};
    caption(a, x + 250, y, "Monitor:");
    /* Only what the model's real hardware offered: a monochrome display of
     * a modular Mac takes one of Apple's monochrome modes when chosen. */
    uint8_t monitor_enabled[3] = {1, 1, 1};
    char monitor_why[400] = "";
    for (uint32_t k = 0; model && k < 3; ++k) {
        char why[400];
        if (mc_config_monitor_check(model, c->width, c->height, c->depth, k, why, sizeof why)) continue;
        if (k == MC_MONITOR_MONO && (model->monitors & MC_MON_MODULAR_MONO)) continue;
        monitor_enabled[k] = 0;
        if (!monitor_why[0]) {
            /* The core's wording names the YAML key; the window says Monitor. */
            char shown[400];
            size_t o = 0;
            for (const char *q = why; *q && o + 8 < sizeof shown;)
                if (!strncmp(q, "video.monitor", 13)) { memcpy(shown + o, "Monitor", 7); o += 7; q += 13; }
                else shown[o++] = *q++;
            shown[o] = 0;
            snprintf(monitor_why, sizeof monitor_why, "%s: %s", monitors[k], shown);
        }
    }
    s = choice_popup(a, "monitor", (ui_rect){x + 250, y, 190, FIELD}, "Monitor", monitors, 3, (int)c->monitor, monitor_enabled);
    if (s >= 0 && (uint32_t)s != c->monitor) { c->monitor = (uint32_t)s; commit(a, MC_CHANGED_MONITOR); }
    y += FIELD + 6;
    static const char *const filters[] = {"Nearest", "Linear", "Sharp"};
    caption(a, x, y, "Filter:");
    s = choice_popup(a, "filter", (ui_rect){x, y, 120, FIELD}, "Filter", filters, 3, (int)c->filter, NULL);
    if (s >= 0 && (uint32_t)s != c->filter) { c->filter = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    static const char *const scales[] = {"Fit the window", "Whole multiples"};
    caption(a, x + 250, y, "Scale:");
    s = choice_popup(a, "scale", (ui_rect){x + 250, y, 150, FIELD}, "Scale", scales, 2, (int)c->scale, NULL);
    if (s >= 0 && (uint32_t)s != c->scale) { c->scale = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 6;
    if (monitor_why[0]) {
        ui_wrapped(u, (ui_rect){x, y, 420, 40}, monitor_why, UI_FONT_SMALL, UI_DARK);
        y += 42;
    }
    y += 6;
    int full = c->fullscreen, vsync = c->vsync, accel = c->video_acceleration;
    if (ui_checkbox(u, ui_id("fullscreen", 0), x, y, "Start in full screen (Alt+Enter switches)", &full, 0)) {
        c->fullscreen = full;
        commit(a, MC_CHANGED_OTHER);
    }
    y += ROW;
    if (ui_checkbox(u, ui_id("vsync", 0), x, y, "Wait for the display's vertical blank (vsync)", &vsync, 0)) {
        c->vsync = vsync;
        commit(a, MC_CHANGED_OTHER);
    }
    y += ROW;
    if (ui_checkbox(u, ui_id("acceleration", 0), x, y, "Accelerated QuickDraw on the Livadix Display Card", &accel, !mc_profile_has_card(model))) {
        c->video_acceleration = accel;
        commit(a, MC_CHANGED_OTHER);
    }
}

/* The disk list's rows. */
typedef struct disk_rows { cfg_app *a; int ids[MC_CONFIG_DISKS]; } disk_rows;
static void disk_row(void *ctx, ui *u, ui_rect r, int index, int selected) {
    disk_rows *d = ctx;
    cfg_app *a = d->a;
    const mc_disk_config *disk = &a->config.disks[index];
    (void)selected;
    ui_icon(&u->canvas, UI_ICON_HARD_DISK, r.x + 4, r.y + 2, 0);
    ui_set_clip(&u->canvas, (ui_rect){r.x, r.y, r.w - 110, r.h});
    ui_label(u, r.x + 24, r.y + 4, base_name(disk->image), UI_FONT_SYSTEM, file_exists(disk->image) ? UI_BLACK : UI_ERROR);
    ui_set_clip(&u->canvas, r);
    char text[32];
    if (d->ids[index] == -2) snprintf(text, sizeof text, "IDE%s", disk->readonly ? ", locked" : "");
    else snprintf(text, sizeof text, "ID %d%s", d->ids[index], disk->readonly ? ", locked" : "");
    ui_label(u, r.x + r.w - 104, r.y + 5, text, UI_FONT_SMALL, UI_DARK);
    const mc_volume_info *v = volume(a, disk->image);
    if (v && v->bootable) {
        mc_compat compat;
        mc_compat_check(a->model, v, mc_config_ram_kb(&a->config), &compat);
        ui_icon(&u->canvas, compat.status == MC_COMPAT_OK ? UI_ICON_OK : UI_ICON_WARNING, r.x + r.w - 22, r.y + 2, 0);
    }
}

static void storage_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    const unsigned features = a->model ? mc_machine_features(a->model) : 0;
    int y = p.y;
    ui_label(u, p.x, y, "Hard disks:", UI_FONT_SYSTEM, UI_BLACK);
    note(a, p.x + 90, y + 2, "drop image files on the window to add them");
    y += 16;
    disk_rows rows = {a, {0}};
    scsi_ids(c, rows.ids);
    static char row_names[MC_CONFIG_DISKS][MC_CONFIG_PATH];
    const char *labels[MC_CONFIG_DISKS];
    for (size_t i = 0; i < c->disk_count; ++i) {
        snprintf(row_names[i], sizeof row_names[i], "%s", base_name(c->disks[i].image));
        labels[i] = row_names[i];
    }
    const int list_w = p.w - 110, list_h = 5 * 20 + 2;
    ui_list(u, ui_id("disks", 0), (ui_rect){p.x, y, list_w, list_h}, 20, (int)c->disk_count, &a->disk_selected, &a->disk_scroll,
            disk_row, &rows, labels, NULL);
    const int bx = p.x + list_w + 10;
    const int sel = a->disk_selected, n = (int)c->disk_count;
    if (ui_button(u, ui_id("disk-add", 0), (ui_rect){bx, y, 90, FIELD}, "Add\xe2\x80\xa6", n >= MC_CONFIG_DISKS ? UI_DISABLED_FLAG : 0))
        choose(a, CFG_PICK_DISK, 0);
    if (ui_button(u, ui_id("disk-remove", 0), (ui_rect){bx, y + 28, 90, FIELD}, "Remove", n ? 0 : UI_DISABLED_FLAG) && n) {
        memmove(c->disks + sel, c->disks + sel + 1, (size_t)(n - sel - 1) * sizeof *c->disks);
        memset(&c->disks[--c->disk_count], 0, sizeof *c->disks);
        commit(a, MC_CHANGED_STORAGE);
        return;
    }
    if (ui_button(u, ui_id("disk-up", 0), (ui_rect){bx, y + 56, 90, FIELD}, "Move Up", sel > 0 && sel < n ? 0 : UI_DISABLED_FLAG) && sel > 0) {
        mc_disk_config t = c->disks[sel]; c->disks[sel] = c->disks[sel - 1]; c->disks[sel - 1] = t;
        a->disk_selected--;
        commit(a, MC_CHANGED_STORAGE);
        return;
    }
    if (ui_button(u, ui_id("disk-down", 0), (ui_rect){bx, y + 84, 90, FIELD}, "Move Down", sel + 1 < n ? 0 : UI_DISABLED_FLAG) && sel + 1 < n) {
        mc_disk_config t = c->disks[sel]; c->disks[sel] = c->disks[sel + 1]; c->disks[sel + 1] = t;
        a->disk_selected++;
        commit(a, MC_CHANGED_STORAGE);
        return;
    }
    y += list_h + 8;
    /* The selected disk: its SCSI ID, or the internal IDE disk. */
    if (n && sel < n) {
        mc_disk_config *d = &c->disks[sel];
        static const char *const id_items[] = {"Automatic", "0", "1", "2", "4", "5", "6", "Internal IDE"};
        static const int id_values[] = {-1, 0, 1, 2, 4, 5, 6, -2};
        uint8_t id_enabled[8];
        int current = 0;
        for (int i = 0; i < 8; ++i) {
            id_enabled[i] = 1;
            for (int k = 0; k < n && i >= 1 && i <= 6; ++k)
                if (k != sel && c->disks[k].bus != MC_BUS_ATA && c->disks[k].scsi_id_set && c->disks[k].scsi_id == id_values[i]) id_enabled[i] = 0;
        }
        id_enabled[7] = (uint8_t)((features & MC_FEATURE_ATA_DISK) != 0);
        if (d->bus == MC_BUS_ATA) current = 7;
        else for (int i = 1; i < 7; ++i) if (d->scsi_id_set && d->scsi_id == id_values[i]) current = i;
        ui_label(u, p.x, y + 5, (features & MC_FEATURE_ATA_DISK) ? "Bus, ID:" : "SCSI ID:", UI_FONT_SYSTEM, UI_BLACK);
        const int s = choice_popup(a, "disk-id", (ui_rect){p.x + 64, y, 120, FIELD}, "SCSI ID", id_items, (features & MC_FEATURE_ATA_DISK) ? 8 : 7,
                                   current, id_enabled);
        if (s >= 0 && s != current) {
            d->bus = s == 7 ? MC_BUS_ATA : MC_BUS_SCSI;
            d->scsi_id_set = s >= 1 && s <= 6;
            d->scsi_id = d->scsi_id_set ? id_values[s] : 0;
            commit(a, MC_CHANGED_STORAGE);
            return;
        }
        int locked = d->readonly;
        if (ui_checkbox(u, ui_id("disk-readonly", 0), p.x + 200, y + 4, "Read-only (never written)", &locked, 0)) {
            d->readonly = locked;
            commit(a, MC_CHANGED_STORAGE);
            return;
        }
        y += FIELD + 6;
        ui_disclosure(u, ui_id("details", 0), p.x, y, "Details", &a->details_open);
        y += 18;
        if (a->details_open) {
            const mc_volume_info *v = volume(a, d->image);
            char line[600], number[16];
            snprintf(line, sizeof line, "%s", d->image);
            ui_wrapped(u, (ui_rect){p.x + 16, y, p.w - 20, 12}, line, UI_FONT_SMALL, UI_DARK);
            y += 12;
            if (!v) {
                ui_label(u, p.x + 16, y, file_exists(d->image) ? "The image cannot be opened." : "The image is missing.", UI_FONT_SYSTEM, UI_ERROR);
            } else {
                snprintf(line, sizeof line, "%s%s, %.1f MB%s%s%s", v->diskcopy ? "DiskCopy 4.2 image, " : "", mc_image_kind_name(v->kind),
                         (double)v->bytes / (1024.0 * 1024.0), v->volume[0] ? ", volume \"" : "", v->volume, v->volume[0] ? "\"" : "");
                ui_label(u, p.x + 16, y, line, UI_FONT_SYSTEM, UI_BLACK);
                y += 16;
                if (v->bootable) {
                    snprintf(line, sizeof line, v->mac_os_x ? "%s %s" : "%s %s, Finder %s", v->mac_os_x ? "Mac OS X" : v->system_version >= 0x0760 ? "Mac OS" : "System",
                             v->system_text[0] ? v->system_text : mc_version_text(v->system_version, number, sizeof number),
                             v->finder_text[0] ? v->finder_text : "?");
                    ui_label(u, p.x + 16, y, line, UI_FONT_SYSTEM, UI_BLACK);
                    y += 16;
                    size_t used = (size_t)snprintf(line, sizeof line, "Enablers: ");
                    if (!v->enabler_count) snprintf(line + used, sizeof line - used, "none");
                    for (size_t i = 0; i < v->enabler_count && used < sizeof line; ++i)
                        used += (size_t)snprintf(line + used, sizeof line - used, "%s%s", i ? ", " : "", v->enablers[i].name);
                    y += ui_wrapped(u, (ui_rect){p.x + 16, y, p.w - 20, 28}, line, UI_FONT_SMALL, UI_BLACK);
                    mc_compat compat;
                    mc_compat_check(a->model, v, mc_config_ram_kb(c), &compat);
                    y += 2 + icon_note(a, p.x + 16, y + 2, p.w - 20, compat.status == MC_COMPAT_OK ? UI_ICON_OK : UI_ICON_WARNING,
                                       compat.reason, compat.status == MC_COMPAT_OK ? UI_BLACK : UI_ERROR);
                } else {
                    ui_label(u, p.x + 16, y, "Not a startup disk (no System Folder).", UI_FONT_SYSTEM, UI_DARK);
                    y += 16;
                }
            }
        }
    }
    /* Floppies, CD, startup device: below. */
    y = p.y + p.h - 3 * (FIELD + 6) - 4;
    const int bay_cd = c->cdrom_bus == MC_BUS_ATA;
    static const char *const floppy_items[] = {"Not fitted", "Empty", "Image\xe2\x80\xa6"};
    for (int i = 0; i < ((features & MC_FEATURE_NO_FLOPPY) ? 0 : (features & MC_FEATURE_ONE_FLOPPY) ? 1 : 2); ++i) {
        const int state = (size_t)i >= c->floppy_count || c->floppy_off[i] ? 0 : c->floppies[i].image[0] ? 2 : 1;
        char label[32];
        snprintf(label, sizeof label, "Floppy %d:", i + 1);
        ui_label(u, p.x, y + 5, label, UI_FONT_SYSTEM, bay_cd ? UI_DISABLED : UI_BLACK);
        ui_icon(&u->canvas, UI_ICON_FLOPPY, p.x + 64, y + 3, 0);
        snprintf(label, sizeof label, "Floppy %d", i + 1);
        const uint8_t enabled[] = {(uint8_t)!bay_cd, (uint8_t)!bay_cd, (uint8_t)!bay_cd};
        const int s = choice_popup(a, i ? "floppy-2" : "floppy-1", (ui_rect){p.x + 86, y, 110, FIELD}, label, floppy_items, 3, state, enabled);
        if (s == 2) choose(a, CFG_PICK_FLOPPY, i);
        else if (s >= 0 && s != state) {
            while (c->floppy_count <= (size_t)i) { c->floppy_off[c->floppy_count] = 1; c->floppy_count++; }
            c->floppy_off[i] = s == 0;
            memset(&c->floppies[i], 0, sizeof c->floppies[i]);
            a->media_chosen++;
            commit(a, MC_CHANGED_STORAGE);
            return;
        }
        if (state == 2) {
            ui_field(u, (ui_rect){p.x + 204, y, p.w - 204 - 200, FIELD}, base_name(c->floppies[i].image));
            int ro = c->floppies[i].readonly;
            if (ui_checkbox(u, ui_id("floppy-readonly", i), p.x + p.w - 190, y + 4, "Read-only", &ro, 0)) {
                c->floppies[i].readonly = ro;
                commit(a, MC_CHANGED_STORAGE);
                return;
            }
            if (ui_button(u, ui_id("floppy-choose", i), (ui_rect){p.x + p.w - 90, y, 90, FIELD}, "Choose\xe2\x80\xa6", 0)) choose(a, CFG_PICK_FLOPPY, i);
        }
        y += FIELD + 6;
    }
    ui_label(u, p.x, y + 5, "CD-ROM:", UI_FONT_SYSTEM, UI_BLACK);
    ui_icon(&u->canvas, UI_ICON_CD, p.x + 64, y + 3, 0);
    const int bay = (features & MC_FEATURE_ATA_CD) != 0;
    const int field_w = p.w - 86 - 290 - (bay ? 70 : 0);
    ui_field(u, (ui_rect){p.x + 86, y, field_w, FIELD}, c->cdrom[0] ? base_name(c->cdrom) : "empty drive");
    if (bay) {
        int in_bay = bay_cd;
        if (ui_checkbox(u, ui_id("cd-bay", 0), p.x + 92 + field_w, y + 4, "In bay", &in_bay, 0)) {
            c->cdrom_bus = in_bay ? MC_BUS_ATA : MC_BUS_SCSI;
            if (!in_bay && !c->floppy_count) { c->floppy_count = 1; c->floppy_off[0] = 0; c->floppy_off[1] = 1; }
            commit(a, MC_CHANGED_STORAGE);
            return;
        }
    }
    if (ui_button(u, ui_id("cd-choose", 0), (ui_rect){p.x + p.w - 280, y, 80, FIELD}, "Choose\xe2\x80\xa6", 0)) choose(a, CFG_PICK_CDROM, 0);
    if (ui_button(u, ui_id("cd-eject", 0), (ui_rect){p.x + p.w - 192, y, 64, FIELD}, "Eject", c->cdrom[0] ? 0 : UI_DISABLED_FLAG) && c->cdrom[0])
        put_cdrom(a, NULL);
    static const char *const boot_items[] = {"Disk first", "CD-ROM first"};
    const int s = choice_popup(a, "boot", (ui_rect){p.x + p.w - 120, y, 120, FIELD}, "Start from", boot_items, 2, (int)c->boot_priority, NULL);
    if (s >= 0 && (uint32_t)s != c->boot_priority) { c->boot_priority = (uint32_t)s; commit(a, MC_CHANGED_STORAGE); }
}

static void sound_tab(cfg_app *a, ui_rect p) {
    int on = a->config.sound;
    if (ui_checkbox(&a->ui, ui_id("sound", 0), p.x + 20, p.y, "Play the Macintosh's sound", &on, 0)) {
        a->config.sound = on;
        commit(a, MC_CHANGED_OTHER);
    }
    note(a, p.x + 37, p.y + 20, "Off: the Macintosh runs as with its speaker unplugged.");
    int floppy = a->config.floppy_sound;
    if (ui_checkbox(&a->ui, ui_id("floppy-sound", 0), p.x + 20, p.y + 54, "Floppy drive sounds", &floppy, 0)) {
        a->config.floppy_sound = floppy;
        commit(a, MC_CHANGED_OTHER);
    }
    note(a, p.x + 37, p.y + 74, "Motor and head movement; independent of the Macintosh speaker.");
}

static void input_tab(cfg_app *a, ui_rect p) {
    mc_config *c = &a->config;
    const int x = p.x + 150;
    int y = p.y;
    static const char *const layouts[] = {"USA (ANSI)", "ISO"};
    caption(a, x, y, "Keyboard:");
    int s = choice_popup(a, "layout", (ui_rect){x, y, 180, FIELD}, "Keyboard", layouts, 2, (int)c->keyboard_layout, NULL);
    if (s >= 0 && (uint32_t)s != c->keyboard_layout) { c->keyboard_layout = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 6;
    static const char *const commands[] = {"Left Alt", "Right Alt", "Left Control", "Right Control"};
    static const uint32_t command_values[] = {MC_COMMAND_LEFT_ALT, MC_COMMAND_RIGHT_ALT, MC_COMMAND_LEFT_CONTROL, MC_COMMAND_RIGHT_CONTROL};
    int current = 0;
    for (int i = 0; i < 4; ++i) if (command_values[i] == c->command_key) current = i;
    caption(a, x, y, "Command key:");
    s = choice_popup(a, "command", (ui_rect){x, y, 180, FIELD}, "Command key", commands, 4, current, NULL);
    if (s >= 0 && s != current) { c->command_key = command_values[s]; commit(a, MC_CHANGED_OTHER); }
#ifdef _WIN32
    note(a, x, y + FIELD + 4, "The other Alt key is Option, Control is Control; the Windows key opens this window.");
#else
    note(a, x, y + FIELD + 4, "The other Alt key is Option, Control is Control; the Super key opens this window.");
#endif
    y += FIELD + 30;
    static const char *const buttons[] = {"Disabled", "Click", "Control-click"};
    for (int b = 0; b < 2; ++b) {
        caption(a, x, y, b ? "Right button:" : "Left button:");
        uint8_t enabled[3] = {1, 1, 1};
        /* At least one button stays a click. */
        if (c->mouse[!b] != MC_MOUSE_NORMAL) enabled[0] = enabled[2] = 0;
        s = choice_popup(a, b ? "right" : "left", (ui_rect){x, y, 180, FIELD}, b ? "Right button" : "Left button", buttons, 3,
                         (int)c->mouse[b], enabled);
        if (s >= 0 && (uint32_t)s != c->mouse[b]) {
            c->mouse[b] = (uint32_t)s;
            commit(a, b ? MC_CHANGED_MOUSE_RIGHT : MC_CHANGED_MOUSE_LEFT);
        }
        y += FIELD + 6;
    }
}

/* network.forward from the comma-separated text; 0 when an entry does not
 * parse (the message in `problem`). */
static int forward_list(const char *text, mc_config *c, char *problem, size_t room) {
    mc_forward_config list[MC_CONFIG_FORWARDS];
    size_t count = 0;
    for (const char *p = text; *p;) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *end = p;
        while (*end && *end != ',' && *end != ' ') end++;
        char one[64];
        snprintf(one, sizeof one, "%.*s", (int)(end - p), p);
        if (count == MC_CONFIG_FORWARDS) { snprintf(problem, room, "At most 16 forwards"); return 0; }
        if (!mc_config_parse_forward(one, &list[count], problem, room)) return 0;
        for (size_t k = 0; k < count; ++k)
            if (list[k].udp == list[count].udp && list[k].host_port == list[count].host_port) {
                snprintf(problem, room, "Two forwards share a host port");
                return 0;
            }
        count++;
        p = end;
    }
    memcpy(c->forwards, list, count * sizeof *list);
    c->forward_count = count;
    return 1;
}

static void network_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    const unsigned features = a->model ? mc_machine_features(a->model) : 0;
    const int x = p.x + 150;
    int y = p.y;
    static const char *const modes[] = {"NAT (the Internet through this computer)", "None (no cable)"};
    caption(a, x, y, "Ethernet:");
    int s = choice_popup(a, "network", (ui_rect){x, y, 300, FIELD}, "Ethernet", modes, 2, (int)c->network, NULL);
    if (s >= 0 && (uint32_t)s != c->network) { c->network = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 4;
    ui_wrapped(u, (ui_rect){x, y, p.w - 160, 60},
               "The Macintosh gets 10.0.2.15 from DHCP or BOOTP; the router and this computer are 10.0.2.2, the DNS "
               "server 10.0.2.3. Nothing is installed and no administrator rights are needed.", UI_FONT_SMALL, UI_DARK);
    y += 52;
    caption(a, x, y, "Forwards:");
    const int disabled = c->network != MC_NETWORK_NAT;
    if (ui_edit(u, ui_id("forwards", 0), (ui_rect){x, y, p.w - 160, FIELD}, a->forward_text, sizeof a->forward_text, disabled)) {
        char problem[128];
        mc_config edited = *c;
        if (forward_list(a->forward_text, &edited, problem, sizeof problem)) {
            *c = edited;
            commit(a, MC_CHANGED_OTHER);
        }
    }
    y += FIELD + 4;
    note(a, x, y, "Ports of this computer that lead to the Macintosh, separated by commas:");
    note(a, x, y + 14, "tcp:8080:80 or udp:5353:53 (on 127.0.0.1), tcp:0.0.0.0:2121:21 (the local network)");
    y += 40;
    static const char *const cards[] = {"Automatic", "None", "EtherTalk NB Card (NuBus)", "Ethernet LC Card (LC slot)"};
    const int builtin = (features & MC_FEATURE_ENET) != 0;
    const uint8_t card_enabled[] = {1, (uint8_t)!builtin, (uint8_t)(!builtin && (features & MC_FEATURE_NUBUS)),
                                    (uint8_t)(!builtin && (features & MC_FEATURE_LC_SLOT))};
    caption(a, x, y, "Card:");
    s = choice_popup(a, "net-card", (ui_rect){x, y, 300, FIELD}, "Card", cards, 4, (int)c->network_card, card_enabled);
    if (s >= 0 && (uint32_t)s != c->network_card) { c->network_card = (uint32_t)s; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 4;
    if (builtin) note(a, x, y, "This model has built-in Ethernet.");
    else {
        note(a, x, y, "For models without built-in Ethernet: Automatic fits the card the model's slot takes when its");
        note(a, x, y + 14, "declaration ROM is in the roms folder (card-ethernet-nb.bin, card-ethernet-lc.bin)");
    }
}

/* The shared folders' rows: the volume's name, the folder, read-only. */
static void share_row(void *ctx, ui *u, ui_rect r, int index, int selected) {
    const cfg_app *a = ctx;
    const mc_share_config *f = &a->config.shares[index];
    (void)selected;
    ui_set_clip(&u->canvas, (ui_rect){r.x, r.y, 150, r.h});
    ui_label(u, r.x + 6, r.y + 4, f->name, UI_FONT_SYSTEM, UI_BLACK);
    ui_set_clip(&u->canvas, (ui_rect){r.x + 156, r.y, r.w - 156 - 70, r.h});
    ui_label(u, r.x + 156, r.y + 5, f->path, UI_FONT_SMALL, UI_DARK);
    ui_set_clip(&u->canvas, r);
    if (f->readonly) ui_label(u, r.x + r.w - 64, r.y + 5, "read-only", UI_FONT_SMALL, UI_DARK);
}

static void sharing_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    int y = p.y;
    ui_label(u, p.x, y, "Shared folders:", UI_FONT_SYSTEM, UI_BLACK);
    note(a, p.x + 110, y + 2, "AppleShare volumes for the Macintosh (Chooser, AppleShare)");
    y += 16;
    static char rows[MC_CONFIG_SHARES][MC_CONFIG_PATH + MC_CONFIG_SHARE_NAME + 16];
    const char *labels[MC_CONFIG_SHARES];
    for (size_t i = 0; i < c->share_count; ++i) {
        snprintf(rows[i], sizeof rows[i], "%s \xe2\x80\x93 %s%s", c->shares[i].name, c->shares[i].path, c->shares[i].readonly ? " (read-only)" : "");
        labels[i] = rows[i];
    }
    const int list_w = p.w - 110, list_h = 4 * 20 + 2;
    const int before = a->share_selected;
    ui_list(u, ui_id("shares", 0), (ui_rect){p.x, y, list_w, list_h}, 20, (int)c->share_count, &a->share_selected, &a->share_scroll, share_row,
            a, labels, NULL);
    if (a->share_selected != before) sync_texts(a, 1);
    const int bx = p.x + list_w + 10, n = (int)c->share_count, sel = a->share_selected;
    if (ui_button(u, ui_id("share-add", 0), (ui_rect){bx, y, 90, FIELD}, "Add\xe2\x80\xa6", n >= MC_CONFIG_SHARES ? UI_DISABLED_FLAG : 0))
        choose(a, CFG_PICK_FOLDER, 0);
    if (ui_button(u, ui_id("share-remove", 0), (ui_rect){bx, y + 28, 90, FIELD}, "Remove", n ? 0 : UI_DISABLED_FLAG) && n) {
        memmove(c->shares + sel, c->shares + sel + 1, (size_t)(n - sel - 1) * sizeof *c->shares);
        memset(&c->shares[--c->share_count], 0, sizeof *c->shares);
        commit(a, MC_CHANGED_OTHER);
        sync_texts(a, 1);
        return;
    }
    y += list_h + 8;
    const int x = p.x + 120;
    if (n && sel < n) {
        caption(a, x, y, "Volume name:");
        if (ui_edit(u, ui_id("share-name", 0), (ui_rect){x, y, 200, FIELD}, a->share_name_text, sizeof a->share_name_text, 0)) {
            int clash = 0;
            size_t chars = 0;
            for (const char *s = a->share_name_text; *s; ++s) chars += ((unsigned char)*s & 0xC0) != 0x80;
            for (int i = 0; i < n; ++i) if (i != sel && !strcmp(c->shares[i].name, a->share_name_text)) clash = 1;
            if (!a->share_name_text[0] || chars > 27 || strchr(a->share_name_text, ':') || clash)
                set_status(a, UI_ICON_WARNING, "A volume name is 1 to 27 characters without ':', one per folder.%s", "");
            else {
                snprintf(c->shares[sel].name, sizeof c->shares[sel].name, "%s", a->share_name_text);
                commit(a, MC_CHANGED_OTHER);
                return;
            }
        }
        int ro = c->shares[sel].readonly;
        if (ui_checkbox(u, ui_id("share-readonly", 0), x + 220, y + 4, "Read-only", &ro, 0)) {
            c->shares[sel].readonly = ro;
            commit(a, MC_CHANGED_OTHER);
            return;
        }
        y += FIELD + 10;
    }
    caption(a, x, y, "Server name:");
    if (ui_edit(u, ui_id("server-name", 0), (ui_rect){x, y, 200, FIELD}, a->server_text, sizeof a->server_text, 0)) {
        size_t chars = 0;
        for (const char *s = a->server_text; *s; ++s) chars += ((unsigned char)*s & 0xC0) != 0x80;
        if (!a->server_text[0] || chars > 31 || strchr(a->server_text, ':'))
            set_status(a, UI_ICON_WARNING, "The server's name is 1 to 31 characters without ':'.%s", "");
        else {
            snprintf(c->sharing_name, sizeof c->sharing_name, "%s", a->server_text);
            commit(a, MC_CHANGED_OTHER);
        }
    }
    y += FIELD + 6;
    static const char *const metas[] = {"NTFS streams", "AppleDouble (._ files)"};
    caption(a, x, y, "Mac metadata:");
    const int m = choice_popup(a, "sharing", (ui_rect){x, y, 220, FIELD}, "Metadata", metas, 2, (int)c->sharing_metadata, NULL);
    if (m >= 0 && (uint32_t)m != c->sharing_metadata) { c->sharing_metadata = (uint32_t)m; commit(a, MC_CHANGED_OTHER); }
    y += FIELD + 6;
    caption(a, x, y, "Password:");
    if (ui_edit(u, ui_id("share-password", 0), (ui_rect){x, y, 120, FIELD}, a->password_text, sizeof a->password_text, 0)) {
        int ok = strlen(a->password_text) <= 8;
        for (const char *s = a->password_text; *s; ++s) if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) ok = 0;
        if (!ok) set_status(a, UI_ICON_WARNING, "The password is at most 8 printable ASCII characters.%s", "");
        else {
            snprintf(c->sharing_password, sizeof c->sharing_password, "%s", a->password_text);
            commit(a, MC_CHANGED_OTHER);
        }
    }
    y += FIELD + 4;
    note(a, x, y, "empty: guests; else any user name with this password");
    y += 18;
    ui_wrapped(u, (ui_rect){x, y, p.w - 130, 60},
               c->network == MC_NETWORK_NAT
                   ? "On the Macintosh: AppleTalk set to Ethernet (the AppleTalk or Network control panel), then the Chooser, AppleShare."
                   : "The file server needs the Ethernet: set it to NAT on the Network tab.",
               UI_FONT_SMALL, c->network == MC_NETWORK_NAT ? UI_DARK : UI_ERROR);
}

static void ports_tab(cfg_app *a, ui_rect p) {
    ui *u = &a->ui;
    mc_config *c = &a->config;
    int y = p.y;
    static const char *const kinds[] = {"Not connected", "Serial port", "Named pipe", "Output file"};
    for (int port = 0; port < 2; ++port) {
        const ui_rect g = ui_group(u, (ui_rect){p.x, y, p.w, 104}, port ? "Printer port (b)" : "Modem port (a)");
        mc_port_config *pc = &c->ports[port];
        const int kind = pc->type == MC_PORT_DISABLED ? MC_PORT_DEFAULT : (int)pc->type;
        const int s = choice_popup(a, port ? "port-b" : "port-a", (ui_rect){g.x + 80, g.y, 160, FIELD}, port ? "Printer port" : "Modem port",
                                   kinds, 4, kind, NULL);
        ui_label(u, g.x, g.y + 5, "Connect to:", UI_FONT_SYSTEM, UI_BLACK);
        if (s >= 0 && s != kind) {
            if (s == 3) { choose(a, CFG_PICK_OUTPUT, port); y += 112; continue; }
            pc->type = (unsigned)s;
#ifdef _WIN32
            if (s == MC_PORT_SERIAL) snprintf(pc->path, sizeof pc->path, "%s", a->com_count ? a->com_ports[0] : "COM1");
            else if (s == MC_PORT_PIPE) snprintf(pc->path, sizeof pc->path, "\\\\.\\pipe\\livadix-%c", port ? 'b' : 'a');
            else pc->path[0] = 0;
#else
            if (s == MC_PORT_SERIAL) snprintf(pc->path, sizeof pc->path, "%s", a->com_count ? a->com_ports[0] : "/dev/ttyS0");
            else if (s == MC_PORT_PIPE) snprintf(pc->path, sizeof pc->path, "/tmp/livadix-%c.sock", port ? 'b' : 'a');
            else pc->path[0] = 0;
#endif
            commit(a, MC_CHANGED_OTHER);
            sync_texts(a, 1);
        }
        const int y2 = g.y + FIELD + 8;
        if (pc->type == MC_PORT_SERIAL) {
            /* The host's COM ports. */
            const char *items[CFG_COM_PORTS + 1];
            int count = 0, current = -1;
#ifdef _WIN32
            for (int i = 0; i < a->com_count; ++i) { items[count] = a->com_ports[i]; if (!_stricmp(a->com_ports[i], pc->path)) current = count; count++; }
#else
            for (int i = 0; i < a->com_count; ++i) { items[count] = a->com_ports[i]; if (!strcmp(a->com_ports[i], pc->path)) current = count; count++; }
#endif
            if (current < 0) { items[count] = pc->path; current = count++; }
            ui_label(u, g.x, y2 + 5, "Port:", UI_FONT_SYSTEM, UI_BLACK);
            int chosen = current;
            if (ui_popup(u, ui_id("com", port), (ui_rect){g.x + 80, y2, 120, FIELD}, port ? "Printer COM" : "Modem COM", items, count, &chosen, NULL, NULL) &&
                chosen != current) {
                snprintf(pc->path, sizeof pc->path, "%s", items[chosen]);
                commit(a, MC_CHANGED_OTHER);
            }
#ifdef _WIN32
            note(a, g.x + 210, y2 + 7, a->com_count ? "the serial ports Windows has" : "Windows reports no serial port");
#else
            note(a, g.x + 210, y2 + 7, a->com_count ? "the serial ports this computer has" : "no serial port found");
#endif
        } else if (pc->type == MC_PORT_PIPE) {
            ui_label(u, g.x, y2 + 5, "Pipe:", UI_FONT_SYSTEM, UI_BLACK);
#ifdef _WIN32
            if (ui_edit(u, ui_id("pipe", port), (ui_rect){g.x + 80, y2, g.w - 90, FIELD}, a->pipe_text[port], sizeof a->pipe_text[port], 0) &&
                !strncmp(a->pipe_text[port], "\\\\.\\pipe\\", 9) && a->pipe_text[port][9]) {
#else
            if (ui_edit(u, ui_id("pipe", port), (ui_rect){g.x + 80, y2, g.w - 90, FIELD}, a->pipe_text[port], sizeof a->pipe_text[port], 0) &&
                a->pipe_text[port][0] == '/' && a->pipe_text[port][1]) {
#endif
                snprintf(pc->path, sizeof pc->path, "%s", a->pipe_text[port]);
                commit(a, MC_CHANGED_OTHER);
            }
        } else if (pc->type == MC_PORT_OUTPUT) {
            ui_label(u, g.x, y2 + 5, "File:", UI_FONT_SYSTEM, UI_BLACK);
            ui_field(u, (ui_rect){g.x + 80, y2, g.w - 190, FIELD}, base_name(pc->path));
            if (ui_button(u, ui_id("output-choose", port), (ui_rect){g.x + g.w - 100, y2, 90, FIELD}, "Choose\xe2\x80\xa6", 0))
                choose(a, CFG_PICK_OUTPUT, port);
        }
        y += 112;
    }
}

/* --- the profile list ------------------------------------------------------------------------- */

static void profile_row(void *ctx, ui *u, ui_rect r, int index, int selected) {
    cfg_app *a = ctx;
    if (!selected && a->marked[index]) ui_fill(&u->canvas, r.x, r.y, r.w, r.h, UI_ACCENT);
    ui_icon(&u->canvas, UI_ICON_MAC, r.x + 3, r.y + 2, 0);
    ui_label(u, r.x + 23, r.y + 2, a->names[index], UI_FONT_SYSTEM, UI_BLACK);
    char line[160];
    const mc_model *m = mc_model_find(a->models[index]);
    const int running = a->has_running && !strcmp(a->names[index], a->running);
    snprintf(line, sizeof line, "%s%s%s", running ? "running, " : "", index == a->default_index ? "\xe2\x80\xa2 default, " : "",
             m ? m->name : "unknown model");
    ui_label(u, r.x + 23, r.y + 16, line, UI_FONT_SMALL, UI_DARK);
}

static size_t marked_count(const cfg_app *a) {
    size_t n = 0;
    for (size_t i = 0; i < a->profile_count; ++i) n += a->marked[i] != 0;
    return n;
}
static void profiles_panel(cfg_app *a, ui_rect r) {
    ui *u = &a->ui;
    const ui_rect g = ui_group(u, r, "Profiles");
    const int buttons = 3 * (FIELD + 6);
    static const char *names[MC_CONFIG_PROFILES];
    for (size_t i = 0; i < a->profile_count; ++i) names[i] = a->names[i];
    if (a->profile_count) {
        if (a->selected >= 0 && (size_t)a->selected < a->profile_count && !a->marked[a->selected]) {
            memset(a->marked, 0, sizeof a->marked);
            a->marked[a->selected] = 1;
        }
        if (a->reveal) {
            /* The profile shown, chosen anywhere but by a click in the list, comes into view. */
            const int rows = (g.h - buttons - 4 - 2) / 30;
            if (a->selected < a->profile_scroll) a->profile_scroll = a->selected;
            else if (rows > 0 && a->selected >= a->profile_scroll + rows) a->profile_scroll = a->selected - rows + 1;
            a->reveal = 0;
        }
        int sel = a->selected;
        const int mods = u->in.mods;
        const int moved = ui_list(u, ui_id("profiles", 0), (ui_rect){g.x, g.y, g.w, g.h - buttons - 4}, 30, (int)a->profile_count, &sel,
                                  &a->profile_scroll, profile_row, a, names, NULL);
        const int clicked = u->list_clicked;
        uint8_t keep[MC_CONFIG_PROFILES];
        if (clicked >= 0 && (mods & (UI_MOD_CTRL | UI_MOD_SHIFT))) {
            /* Ctrl adds or takes out one; Shift selects the range from the
             * last plain or Ctrl click. The one shown is the one clicked, or
             * another selected one. */
            if (mods & UI_MOD_CTRL) {
                memcpy(keep, a->marked, sizeof keep);
                keep[clicked] = !keep[clicked];
            } else {
                memset(keep, 0, sizeof keep);
                const int from = a->anchor < (int)a->profile_count ? a->anchor : clicked;
                for (int i = from < clicked ? from : clicked; i <= (from < clicked ? clicked : from); ++i) keep[i] = 1;
            }
            int show = keep[clicked] ? clicked : keep[a->selected] ? a->selected : -1;
            for (size_t i = 0; show < 0 && i < a->profile_count; ++i) if (keep[i]) show = (int)i;
            if (show < 0) { keep[clicked] = 1; show = clicked; }
            const int anchor = mods & UI_MOD_CTRL ? clicked : a->anchor;
            if (show != a->selected) cfg_app_select(a, a->names[show]);
            memcpy(a->marked, keep, sizeof keep);
            a->anchor = anchor;
        } else if (moved && sel != a->selected) {
            cfg_app_select(a, a->names[sel]);
        } else if (clicked >= 0) {
            memset(a->marked, 0, sizeof a->marked);
            a->marked[clicked] = 1;
            a->anchor = clicked;
        }
    } else {
        ui_fill(&u->canvas, g.x, g.y, g.w, g.h - buttons - 4, UI_WHITE);
        ui_frame(&u->canvas, g.x, g.y, g.w, g.h - buttons - 4, UI_BLACK);
        ui_wrapped(u, (ui_rect){g.x + 6, g.y + 6, g.w - 12, 80}, "This file has no profiles: its settings are one configuration. "
                   "New makes it a profile and adds another.", UI_FONT_SMALL, UI_DARK);
    }
    const int bw = (g.w - 6) / 2, editable = !a->doc_error[0];
    const size_t several = a->profile_count ? marked_count(a) : 0;
    int y = g.y + g.h - buttons + 2;
    if (ui_button(u, ui_id("profile-new", 0), (ui_rect){g.x, y, bw, FIELD}, "New\xe2\x80\xa6", editable ? 0 : UI_DISABLED_FLAG))
        open_dialog(a, CFG_DIALOG_NEW);
    if (ui_button(u, ui_id("profile-duplicate", 0), (ui_rect){g.x + bw + 6, y, bw, FIELD}, "Duplicate\xe2\x80\xa6",
                  editable && several <= 1 ? 0 : UI_DISABLED_FLAG))
        open_dialog(a, CFG_DIALOG_DUPLICATE);
    y += FIELD + 6;
    const int has = a->profile_count > 0 && editable, one = has && several == 1;
    if (ui_button(u, ui_id("profile-rename", 0), (ui_rect){g.x, y, bw, FIELD}, "Rename\xe2\x80\xa6", one ? 0 : UI_DISABLED_FLAG))
        open_dialog(a, CFG_DIALOG_RENAME);
    /* At least one profile stays. */
    if (ui_button(u, ui_id("profile-delete", 0), (ui_rect){g.x + bw + 6, y, bw, FIELD}, "Delete\xe2\x80\xa6",
                  has && several < a->profile_count ? 0 : UI_DISABLED_FLAG))
        open_dialog(a, CFG_DIALOG_DELETE);
    y += FIELD + 6;
    if (ui_button(u, ui_id("profile-default", 0), (ui_rect){g.x, y, g.w, FIELD}, "Make Default",
                  one && a->default_index != a->selected ? 0 : UI_DISABLED_FLAG)) {
        char quoted[MC_CONFIG_NAME + 8];
        const char *def[] = {"default"};
        mc_ydoc_set(&a->doc, def, 1, mc_ydoc_quote(profile_name(a), quoted, sizeof quoted));
        refresh_list(a);
        set_status(a, UI_ICON_NOTE, "%s starts by default", profile_name(a));
    }
}

/* --- dialogs --------------------------------------------------------------------------------- */

static void dialogs(cfg_app *a, ui_rect window) {
    ui *u = &a->ui;
    ui_canvas *c = &u->canvas;
    if (a->dialog == CFG_DIALOG_PICK) {
        char path[MC_CONFIG_PATH];
        const int kind = a->picker.kind, drive = a->picker.drive;
        const int done = cfg_picker_frame(a, window, path, sizeof path);
        if (done == 1) { pick_done(a); picked(a, kind, drive, path); }
        else if (done == -1) pick_done(a);
        else if (done == -2) {
            char name[600];
            snprintf(name, sizeof name, "%s", a->dialog_message);
            pick_done(a);
            snprintf(a->dialog_message, sizeof a->dialog_message,
                     "â%sâ cannot be used: its name has characters this computer's code page does not hold. Rename it or its folder and choose it again.",
                     name);
            a->alert_icon = UI_ICON_STOP;
            a->dialog = CFG_DIALOG_ALERT;
        }
        return;
    }
    const int w = 420, h = a->dialog <= CFG_DIALOG_RENAME ? 150 : 140;
    const ui_rect frame = {window.x + (window.w - w) / 2, window.y + (window.h - h) / 3, w, h};
    const ui_rect in = ui_dialog(u, frame);
    const int bottom = in.y + in.h - FIELD;
    int ok = 0, cancel = 0, other = 0;
    if (a->dialog <= CFG_DIALOG_RENAME) {
        static const char *const prompts[] = {"", "Name of the new profile:", "Name of the copy:", "New name of the profile:"};
        ui_icon(c, UI_ICON_MAC, in.x, in.y, 0);
        ui_label(u, in.x + 24, in.y + 2, prompts[a->dialog], UI_FONT_SYSTEM, UI_BLACK);
        ui_edit(u, ui_id("dialog-name", 0), (ui_rect){in.x + 24, in.y + 24, in.w - 24, FIELD}, a->dialog_name, sizeof a->dialog_name, 0);
        const char *problem = dialog_check(a);
        if (problem) ui_label(u, in.x + 24, in.y + 52, problem, UI_FONT_SMALL, UI_ERROR);
        cancel = ui_button(u, ui_id("dialog-cancel", 0), (ui_rect){in.x + in.w - 170, bottom, 76, FIELD}, "Cancel", 0);
        ok = ui_button(u, ui_id("dialog-ok", 0), (ui_rect){in.x + in.w - 80, bottom, 76, FIELD}, "OK", UI_DEFAULT | (problem ? UI_DISABLED_FLAG : 0));
        if (!problem && u->in.key == UI_KEY_ENTER) ok = 1;
    } else {
        char message[700];
        const char *yes = "OK", *no = NULL, *third = NULL;
        int icon = UI_ICON_WARNING;
        switch (a->dialog) {
        case CFG_DIALOG_DELETE: {
            const size_t n = marked_count(a);
            if (n <= 1)
                snprintf(message, sizeof message, "Delete the profile \xe2\x80\x9c%s\xe2\x80\x9d? Its settings leave the file when you save.",
                         profile_name(a));
            else {
                int used = snprintf(message, sizeof message, "Delete these %u profiles? Their settings leave the file when you save:", (unsigned)n);
                for (size_t i = 0, k = 0; i < a->profile_count && used > 0 && (size_t)used < sizeof message; ++i)
                    if (a->marked[i])
                        used += snprintf(message + used, sizeof message - (size_t)used, "%s \xe2\x80\x9c%s\xe2\x80\x9d", k++ ? "," : "", a->names[i]);
            }
            yes = "Delete"; no = "Cancel";
            break;
        }
        case CFG_DIALOG_QUIT:
            snprintf(message, sizeof message, "The changes to %s are not saved. Save them before quitting?", base_name(a->path));
            yes = "Save"; no = "Cancel"; third = "Don't Save";
            break;
        case CFG_DIALOG_CLOSE:
            snprintf(message, sizeof message, "%s", a->dialog_message);
            yes = "Save"; no = "Cancel"; third = "Don't Save";
            break;
        case CFG_DIALOG_RESTART:
            snprintf(message, sizeof message, "The changes are saved. Those that need it take effect when the Macintosh restarts. "
                     "Restart it now?");
            yes = "Restart"; no = "Later";
            break;
        default:
            snprintf(message, sizeof message, "%s", a->dialog_message);
            icon = a->alert_icon;
            break;
        }
        ui_icon(c, icon, in.x, in.y, 1);
        ui_wrapped(u, (ui_rect){in.x + 44, in.y, in.w - 44, in.h - FIELD - 8}, message, UI_FONT_SYSTEM, UI_BLACK);
        const int yw = ui_text_width(ui_font_get(UI_FONT_SYSTEM), yes) + 28;
        ok = ui_button(u, ui_id("dialog-ok", 0), (ui_rect){in.x + in.w - yw - 4, bottom, yw, FIELD}, yes, UI_DEFAULT);
        if (no) cancel = ui_button(u, ui_id("dialog-cancel", 0), (ui_rect){in.x + in.w - yw - 94, bottom, 80, FIELD}, no, 0);
        if (third) other = ui_button(u, ui_id("dialog-other", 0), (ui_rect){in.x, bottom, 100, FIELD}, third, 0);
        if (u->in.key == UI_KEY_ENTER) ok = 1;
    }
    if (u->in.key == UI_KEY_ESCAPE) cancel = 1;
    if (other && a->dialog == CFG_DIALOG_QUIT) { a->dialog = CFG_DIALOG_NONE; if (a->host.quit) a->host.quit(a->host.ctx); return; }
    if (other && a->dialog == CFG_DIALOG_CLOSE) {
        /* The edits stay, unsaved: the same ones are not asked about again. */
        a->dialog = CFG_DIALOG_NONE;
        free(a->declined);
        a->declined = mc_ydoc_text(&a->doc, NULL);
        leave(a);
        return;
    }
    if (cancel && a->dialog == CFG_DIALOG_RESTART && a->restart_closes) {
        a->dialog = CFG_DIALOG_NONE;
        a->restart_closes = 0;
        leave(a);
        return;
    }
    if (cancel) { a->dialog = CFG_DIALOG_NONE; return; }
    if (ok) {
        if (a->dialog == CFG_DIALOG_ALERT) a->dialog = CFG_DIALOG_NONE;
        else dialog_ok(a);
    }
}

/* --- the frame --------------------------------------------------------------------------------- */

static void frame_once(cfg_app *a, const ui_input *in, ui_rect window) {
    ui *u = &a->ui;
    /* Escape closes an open menu, or leaves a field being typed in, first. */
    const int busy = u->menu.id != 0 || u->edit_id != 0;
    ui_begin(u, in);
    ui_canvas *c = &u->canvas;
    if (window.w <= 0 || window.h <= 0) window = (ui_rect){0, 0, c->w, c->h};
    const int modal = a->dialog != CFG_DIALOG_NONE;
    const int dirty = cfg_app_dirty(a);
    /* Keys of the window: Ctrl+S saves; Escape closes it. */
    int save = 0;
    if (!modal && in->key == UI_KEY_CHAR && (in->mods & UI_MOD_CTRL) && in->ch == 's') save = 1;
    const int escape = !modal && in->key == UI_KEY_ESCAPE && !busy && !u->menu.id && !u->edit_id;
    u->blocked = modal;
    char title[MC_CONFIG_PATH + 64];
    snprintf(title, sizeof title, "Livadix " MC_VERSION " Configuration \xe2\x80\x93 %s%s", base_name(a->path), dirty ? " (changed)" : "");
    const ui_rect content = ui_window(u, window, title);
    /* The close box: as Close. */
    const ui_rect box = {window.x + 9, window.y + 5, 11, 11};
    ui_register(u, ui_id("close-box", 0), box, "Close box", UI_HIT_BUTTON);
    const int closed = !modal && in->pressed && ui_inside(box, in->x, in->y);
    const int left_w = 236, bar = FIELD + 22;
    profiles_panel(a, (ui_rect){content.x + 8, content.y + 6, left_w - 8, content.h - bar - 8});
    /* The editor. */
    const ui_rect area = {content.x + left_w + 8, content.y + 6, content.w - left_w - 16, content.h - bar - 8};
    char heading[MC_CONFIG_NAME + 32];
    snprintf(heading, sizeof heading, "%s", profile_name(a) ? profile_name(a) : "The configuration");
    ui_label(u, area.x + 2, area.y, heading, UI_FONT_SYSTEM, UI_BLACK);
    static const char *const tabs[] = TAB_LABELS;
    ui_rect panel;
    ui_tabs(u, ui_id("tabs", 0), (ui_rect){area.x, area.y + 18, area.w, area.h - 18}, tabs, CFG_TABS, &a->tab, &panel);
    /* What stops the file, the start that failed (on its profile), or the
     * profile itself. */
    const int failed_here = a->problem[0] && !strcmp(a->problem_profile, profile_name(a) ? profile_name(a) : "");
    const char *trouble = a->doc_error[0] ? a->doc_error : failed_here ? a->problem : !a->config_ok ? a->config_error : NULL;
    if (trouble) {
        ui_icon(c, UI_ICON_STOP, panel.x, panel.y, 1);
        char text[1200];
        snprintf(text, sizeof text, "%s%s", failed_here && !a->doc_error[0] ? "The Macintosh could not start: " : "", trouble);
        ui_wrapped(u, (ui_rect){panel.x + 44, panel.y, panel.w - 44, 56}, text, UI_FONT_SYSTEM, UI_ERROR);
        ui_register(u, ui_id("problem", 0), (ui_rect){panel.x + 44, panel.y, panel.w - 44, 56}, text, UI_HIT_TEXT);
        panel.y += 60;
        panel.h -= 60;
    }
    if (!a->doc_error[0]) switch (a->tab) {
    case CFG_TAB_MACHINE: machine_tab(a, panel); break;
    case CFG_TAB_VIDEO: video_tab(a, panel); break;
    case CFG_TAB_STORAGE: storage_tab(a, panel); break;
    case CFG_TAB_SOUND: sound_tab(a, panel); break;
    case CFG_TAB_INPUT: input_tab(a, panel); break;
    case CFG_TAB_NETWORK: network_tab(a, panel); break;
    case CFG_TAB_SHARING: sharing_tab(a, panel); break;
    default: ports_tab(a, panel); break;
    }
    /* The status line and the window's buttons. */
    const int by = content.y + content.h - FIELD - 8;
    ui_fill(c, content.x, by - 10, content.w, 1, UI_SHADOW);
    ui_fill(c, content.x, by - 9, content.w, 1, UI_WHITE);
    int sx = content.x + 10;
    if (a->status[0]) {
        if (a->status_icon >= 0) { ui_icon(c, a->status_icon, sx, by + 3, 0); sx += 20; }
        ui_set_clip(c, (ui_rect){sx, by, content.x + content.w - 444 - sx, FIELD});
        ui_label(u, sx, by + 5, a->status, UI_FONT_SYSTEM, a->status_icon == UI_ICON_STOP ? UI_ERROR : UI_BLACK);
        ui_clear_clip(c);
    }
    const int bx = content.x + content.w - 8;
    const int dirty_now = cfg_app_dirty(a);
    /* Stop (the running Macintosh shuts down and stays off); Start, or Restart
     * for the running profile (it starts as edited, saved or not); Close (the
     * window, the edits kept); Save; Quit (a running Macintosh shuts down as
     * when the host's window closes). */
    if (ui_button(u, ui_id("stop", 0), (ui_rect){bx - 430, by, 70, FIELD}, "Stop", a->host.resume && a->host.stop ? 0 : UI_DISABLED_FLAG) &&
        a->host.resume && a->host.stop)
        a->host.stop(a->host.ctx);
    const char *selected = profile_name(a);
    const int is_running = a->host.resume && a->running_from && (selected ? a->has_running && !strcmp(selected, a->running) : !a->has_running);
    if (ui_button(u, ui_id("start", 0), (ui_rect){bx - 352, by, 90, FIELD}, is_running ? "Restart" : "Start", start_problem(a) ? UI_DISABLED_FLAG : 0))
        start(a);
    const int shut = ui_button(u, ui_id("close-button", 0), (ui_rect){bx - 254, by, 70, FIELD}, "Close", 0);
    if (ui_button(u, ui_id("save", 0), (ui_rect){bx - 172, by, 90, FIELD}, "Save", UI_DEFAULT | (dirty_now && !a->doc_error[0] ? 0 : UI_DISABLED_FLAG)))
        save = 1;
    if (ui_button(u, ui_id("quit", 0), (ui_rect){bx - 70, by, 70, FIELD}, "Quit", 0))
        cfg_app_quit(a);
    /* Enter: the default button, unless a control takes it. */
    if (!modal && u->in.key == UI_KEY_ENTER && dirty_now) save = 1;
    if (save && cfg_app_dirty(a) && cfg_app_save(a) && restart_pending(a)) {
        /* Saved, but the Macintosh runs on as it started: restart it now? */
        a->dialog = CFG_DIALOG_RESTART;
    }
    if (modal) {
        u->blocked = 0;
        /* The window under the dialog, dimmed by a pattern. */
        for (int y = content.y; y < window.y + window.h; y += 2)
            for (int x = window.x + (y / 2) % 2; x < window.x + window.w; x += 2) ui_pixel(c, x, y, UI_SHADOW);
        dialogs(a, window);
    }
    ui_end(u);
    if (shut || closed || escape) cfg_app_close(a);
}
/* A frame; one whose controls changed the configuration while it was drawn
 * (what was drawn before the change shows the old one) is drawn once more,
 * so no frame shows both. */
void cfg_app_frame(cfg_app *a, const ui_input *in, ui_rect window) {
    const unsigned before = a->changes;
    ui_canvas *c = &a->ui.canvas;
    const size_t bytes = (size_t)c->w * c->h * sizeof *c->px;
    uint32_t *under = c->px && bytes ? malloc(bytes) : NULL;
    if (under) memcpy(under, c->px, bytes);
    frame_once(a, in, window);
    if (a->changes == before || !under) { free(under); return; }
    /* Drawn again on what was under the first drawing (a menu it showed
     * outside the window does not stay). */
    memcpy(c->px, under, bytes);
    free(under);
    ui_input idle;
    memset(&idle, 0, sizeof idle);
    idle.x = in->x;
    idle.y = in->y;
    frame_once(a, &idle, window);
}
