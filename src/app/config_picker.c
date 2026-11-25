/* The file picker of the configuration window (config_picker.h): Livadix's own,
 * drawn with the window's toolkit on every host, in the manner of the Mac OS
 * 8 Standard File dialog: a pop-up of the folder's enclosing folders (and the
 * host's places), the folder's contents with folders first, a filter, and
 * Open, Choose or Save. The host gives the folders (config_host.c).
 *
 * Livadix's own code. */
#include "config_picker.h"
#include "config_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD 22
#define ROW 18
#define ANCESTORS 48

static const char *const titles[] = {"Add a hard disk image", "Choose a floppy image", "Choose a CD image", "Save the port's output as",
                                     "Share a folder with the Macintosh"};
/* The extensions each kind shows ("hf*": hf and what follows). */
static const char *const filters[] = {"img hfs hf* dsk hda toast iso", "img image dsk dc42", "iso toast cdr img cue bin", "", ""};
static const char *const filter_names[] = {"Disk images", "Floppy images", "CD images", "All files", "Folders"};

static int is_separator(char c) { return c == '/' || c == '\\'; }

/* Whether name's extension is one of kind's. */
static int matches(int kind, const char *name) {
    const char *dot = strrchr(name, '.');
    if (!filters[kind][0]) return 1;
    if (!dot || !dot[1]) return 0;
    for (const char *s = filters[kind]; *s;) {
        const char *end = strchr(s, ' ');
        size_t n = end ? (size_t)(end - s) : strlen(s);
        const int prefix = n && s[n - 1] == '*';
        if (prefix) n--;
        size_t k = 0;
        for (; k < n; ++k) {
            char c = dot[1 + k];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            if (c != s[k]) break;
        }
        if (k == n && (prefix || !dot[1 + n])) return 1;
        s += n + (prefix ? 1 : 0) + (end ? 1 : 0);
    }
    return 0;
}

static void join(char *out, size_t capacity, const char *folder, const char *name) {
    const size_t n = strlen(folder);
    snprintf(out, capacity, "%s%s%s", folder, n && !is_separator(folder[n - 1]) ? (char[2]){cfg_host_separator(), 0} : "", name);
}

/* The folder above path ("" at a root). */
static void parent_of(const char *path, char *out, size_t capacity) {
    snprintf(out, capacity, "%s", path);
    size_t n = strlen(out);
    while (n > 1 && is_separator(out[n - 1])) out[--n] = 0;     /* "C:\x\" -> "C:\x" */
    while (n && !is_separator(out[n - 1])) --n;
    if (!n) { out[0] = 0; return; }
    /* keep the root's own separator: "/" and "C:\" */
    if (n == 1 || (n == 3 && out[1] == ':')) out[n] = 0;
    else out[n - 1] = 0;
}

static void refilter(cfg_picker *p) {
    p->shown_count = 0;
    for (int i = 0; i < p->count; ++i) {
        const cfg_dir_entry *e = &p->entries[i];
        if (e->dir || (p->kind != CFG_PICK_FOLDER && (p->all_files || matches(p->kind, e->name)))) {
            p->shown[p->shown_count] = i;
            p->labels[p->shown_count++] = e->name;
        }
    }
    if (p->selected >= p->shown_count) p->selected = p->shown_count - 1;
}

static void show_folder(cfg_app *a, const char *folder) {
    cfg_picker *p = &a->picker;
    free(p->entries);
    free(p->shown);
    free(p->labels);
    p->entries = NULL;
    p->shown = NULL;
    p->labels = NULL;
    if (!cfg_host_absolute(folder, p->folder, sizeof p->folder)) snprintf(p->folder, sizeof p->folder, "%s", folder);
    /* no separator at the end but a root's own ("/", "C:\") */
    for (size_t n = strlen(p->folder); n > 1 && is_separator(p->folder[n - 1]) && !(n == 3 && p->folder[1] == ':');)
        p->folder[--n] = 0;
    p->count = cfg_host_list(p->folder, &p->entries);
    p->readable = p->count >= 0;
    if (p->count < 0) p->count = 0;
    p->shown = malloc(sizeof *p->shown * (size_t)(p->count + 1));
    p->labels = malloc(sizeof *p->labels * (size_t)(p->count + 1));
    if (!p->shown || !p->labels) p->count = 0;
    p->selected = -1;
    p->scroll = 0;
    refilter(p);
}

/* Where the picker opens: the folder of what the setting holds, else where
 * it last chose, else the configuration's own folder. */
static void start_folder(cfg_app *a, int kind, int drive, char *out, size_t capacity) {
    const mc_config *c = &a->config;
    const char *value = "";
    if (kind == CFG_PICK_DISK && c->disk_count) value = c->disks[a->disk_selected >= 0 && (size_t)a->disk_selected < c->disk_count ? a->disk_selected : 0].image;
    else if (kind == CFG_PICK_FLOPPY && drive >= 0 && drive < MC_CONFIG_FLOPPIES) value = c->floppies[drive].image;
    else if (kind == CFG_PICK_CDROM) value = c->cdrom;
    else if (kind == CFG_PICK_OUTPUT && drive >= 0 && drive < 2 && c->ports[drive].type == MC_PORT_OUTPUT) value = c->ports[drive].path;
    else if (kind == CFG_PICK_FOLDER && c->share_count) value = c->shares[a->share_selected >= 0 && (size_t)a->share_selected < c->share_count ? a->share_selected : 0].path;
    if (value[0]) {
        char utf8[MC_CONFIG_PATH], full[MC_CONFIG_PATH * 2], folder[MC_CONFIG_PATH * 2];
        if (cfg_host_utf8_path(value, kind, utf8, sizeof utf8)) {
            const int absolute = is_separator(utf8[0]) || (utf8[0] && utf8[1] == ':');
            if (absolute) snprintf(full, sizeof full, "%s", utf8);
            else join(full, sizeof full, a->folder[0] ? a->folder : ".", utf8);
            parent_of(full, folder, sizeof folder);
            if (folder[0] && cfg_host_is_folder(folder) && strlen(folder) < capacity) {
                snprintf(out, capacity, "%s", folder);
                return;
            }
        }
    }
    if (a->last_folder[0] && cfg_host_is_folder(a->last_folder)) { snprintf(out, capacity, "%s", a->last_folder); return; }
    snprintf(out, capacity, "%s", a->folder[0] ? a->folder : ".");
}

void cfg_picker_open(cfg_app *a, int kind, int drive) {
    cfg_picker *p = &a->picker;
    char folder[MC_CONFIG_PATH];
    start_folder(a, kind, drive, folder, sizeof folder);
    p->kind = kind;
    p->drive = drive;
    p->all_files = 0;
    p->name[0] = 0;
    if (kind == CFG_PICK_OUTPUT) snprintf(p->name, sizeof p->name, "%s", drive ? "printer.txt" : "modem.txt");
    p->place_count = cfg_host_places(p->places, p->place_paths, 16);
    show_folder(a, folder);
    a->pending_pick = kind + 1;
    a->pending_drive = drive;
    a->dialog = CFG_DIALOG_PICK;
    ui_focus(&a->ui, kind == CFG_PICK_OUTPUT ? ui_id("pick-name", 0) : ui_id("pick-list", 0));
}

void cfg_picker_close(cfg_app *a) {
    cfg_picker *p = &a->picker;
    free(p->entries);
    free(p->shown);
    free(p->labels);
    p->entries = NULL;
    p->shown = NULL;
    p->labels = NULL;
    p->count = p->shown_count = 0;
}

static void size_text(long long size, char *out, size_t capacity) {
    if (size < 1024) snprintf(out, capacity, "%lld bytes", size);
    else if (size < 1024 * 1024) snprintf(out, capacity, "%lld K", (size + 1023) / 1024);
    else if (size < 1024LL * 1024 * 1024) snprintf(out, capacity, "%.1f MB", (double)size / (1024.0 * 1024.0));
    else snprintf(out, capacity, "%.1f GB", (double)size / (1024.0 * 1024.0 * 1024.0));
}

static void row(void *ctx, ui *u, ui_rect r, int index, int selected) {
    cfg_app *a = ctx;
    const cfg_picker *p = &a->picker;
    const cfg_dir_entry *e = &p->entries[p->shown[index]];
    (void)selected;
    static const int kind_icon[] = {UI_ICON_HARD_DISK, UI_ICON_FLOPPY, UI_ICON_CD, UI_ICON_DOCUMENT, UI_ICON_FOLDER};
    const int icon = e->dir ? UI_ICON_FOLDER : matches(p->kind, e->name) && p->kind != CFG_PICK_OUTPUT ? kind_icon[p->kind] : UI_ICON_DOCUMENT;
    ui_icon(&u->canvas, icon, r.x + 4, r.y + 1, 0);
    char size[32] = "";
    if (!e->dir) size_text(e->size, size, sizeof size);
    const int sw = size[0] ? ui_text_width(ui_font_get(UI_FONT_SMALL), size) : 0;
    ui_set_clip(&u->canvas, (ui_rect){r.x + 24, r.y, r.w - 24 - sw - 16, r.h});
    ui_label(u, r.x + 24, r.y + 3, e->name, UI_FONT_SYSTEM, UI_BLACK);
    ui_clear_clip(&u->canvas);
    if (size[0]) ui_label(u, r.x + r.w - sw - 6, r.y + 4, size, UI_FONT_SMALL, UI_DARK);
}

/* The path of what is chosen, as the configuration holds it; 1 when it can be. */
static int chosen(cfg_app *a, const char *utf8, char *path, size_t capacity) {
    if (cfg_host_native_path(utf8, a->picker.kind, path, capacity)) return 1;
    snprintf(a->dialog_message, sizeof a->dialog_message, "%s", utf8);
    return 0;
}

int cfg_picker_frame(cfg_app *a, ui_rect window, char *path, size_t capacity) {
    ui *u = &a->ui;
    cfg_picker *p = &a->picker;
    const int w = 560, h = 410;
    const ui_rect frame = {window.x + (window.w - w) / 2, window.y + (window.h - h) / 3, w, h};
    const ui_rect in = ui_dialog(u, frame);
    ui_icon(&u->canvas, p->kind == CFG_PICK_FOLDER ? UI_ICON_FOLDER : UI_ICON_MAC, in.x, in.y, 0);
    ui_label(u, in.x + 24, in.y + 2, titles[p->kind], UI_FONT_SYSTEM, UI_BLACK);

    /* The folder and the folders around it: a pop-up as Standard File's. */
    char ancestors[ANCESTORS][MC_CONFIG_PATH];
    const char *items[ANCESTORS + 17];
    uint8_t enabled[ANCESTORS + 17];
    char names[ANCESTORS][96];
    int n = 0;
    for (char at[MC_CONFIG_PATH], up[MC_CONFIG_PATH] = ""; n < ANCESTORS;) {
        snprintf(at, sizeof at, "%s", n ? up : p->folder);
        if (!at[0]) break;
        snprintf(ancestors[n], MC_CONFIG_PATH, "%s", at);
        size_t len = strlen(at);
        while (len > 1 && is_separator(at[len - 1]) && !(len == 3 && at[1] == ':')) at[--len] = 0;
        const char *last = at + len;
        while (last > at && !is_separator(last[-1])) --last;
        snprintf(names[n], sizeof names[n], "%s", *last ? last : at);
        if (len == 3 && at[1] == ':') snprintf(names[n], sizeof names[n], "%c:", at[0]);
        items[n] = names[n];
        enabled[n] = 1;
        parent_of(at, up, sizeof up);
        ++n;
        if (!strcmp(up, at)) break;
    }
    int count = n;
    if (p->place_count) {
        items[count] = "-";
        enabled[count++] = 0;
        for (int k = 0; k < p->place_count; ++k) {
            items[count] = p->places[k];
            enabled[count++] = 1;
        }
    }
    int where = 0;
    const int lw = in.w - 24;
    if (ui_popup(u, ui_id("pick-folder", 0), (ui_rect){in.x + 24, in.y + 22, 280, FIELD}, "Folder", items, count, &where, enabled, NULL)) {
        if (where < n) show_folder(a, ancestors[where]);
        else if (where > n) show_folder(a, p->place_paths[where - n - 1]);
    }
    /* Up one folder. */
    char up[MC_CONFIG_PATH];
    parent_of(p->folder, up, sizeof up);
    if (ui_button(u, ui_id("pick-up", 0), (ui_rect){in.x + 312, in.y + 22, 70, FIELD}, "Up", up[0] && strcmp(up, p->folder) ? 0 : UI_DISABLED_FLAG))
        show_folder(a, up);

    /* The folder's contents. */
    const int list_y = in.y + 52, list_h = 236;
    int activated = 0;
    ui_list(u, ui_id("pick-list", 0), (ui_rect){in.x + 24, list_y, lw, list_h}, ROW, p->shown_count, &p->selected, &p->scroll, row, a,
            p->labels, &activated);
    if (!p->readable) ui_label(u, in.x + 34, list_y + 8, "This folder cannot be read.", UI_FONT_SYSTEM, UI_ERROR);
    else if (!p->shown_count) ui_label(u, in.x + 34, list_y + 8, p->kind == CFG_PICK_FOLDER ? "No folders" : "No matching files", UI_FONT_SMALL, UI_DARK);
    /* A typed letter selects the next entry it begins. */
    if (u->focus == ui_id("pick-list", 0) && !u->edit_id && u->in.key == UI_KEY_CHAR && !(u->in.mods & (UI_MOD_CTRL | UI_MOD_ALT)) &&
        p->shown_count) {
        int ch = u->in.ch;
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        for (int k = 1; k <= p->shown_count; ++k) {
            const int i = (p->selected + k + p->shown_count) % p->shown_count;
            int first = (unsigned char)p->entries[p->shown[i]].name[0];
            if (first >= 'A' && first <= 'Z') first += 32;
            if (first == ch) {
                p->selected = i;
                if (i < p->scroll) p->scroll = i;
                if (i >= p->scroll + list_h / ROW) p->scroll = i - list_h / ROW + 1;
                break;
            }
        }
    }
    if (u->focus == ui_id("pick-list", 0) && !u->edit_id && u->in.key == UI_KEY_BACKSPACE && up[0] && strcmp(up, p->folder))
        show_folder(a, up);

    int y = list_y + list_h + 8;
    ui_set_clip(&u->canvas, (ui_rect){in.x + 24, y, lw, 14});
    ui_label(u, in.x + 24, y, p->folder, UI_FONT_SMALL, UI_DARK);
    ui_clear_clip(&u->canvas);
    y += 20;
    if (p->kind == CFG_PICK_OUTPUT) {
        ui_label(u, in.x + 24, y + 4, "Save as:", UI_FONT_SYSTEM, UI_BLACK);
        ui_edit(u, ui_id("pick-name", 0), (ui_rect){in.x + 100, y, lw - 76, FIELD}, p->name, sizeof p->name, 0);
    } else if (p->kind != CFG_PICK_FOLDER) {
        ui_label(u, in.x + 24, y + 4, "Show:", UI_FONT_SYSTEM, UI_BLACK);
        const char *shows[2] = {filter_names[p->kind], "All files"};
        int show = p->all_files;
        if (ui_popup(u, ui_id("pick-show", 0), (ui_rect){in.x + 100, y, 180, FIELD}, "Show", shows, 2, &show, NULL, NULL)) {
            p->all_files = show;
            p->selected = -1;
            refilter(p);
        }
    }

    /* The buttons: Open a folder; Open, Choose or Save what is chosen. */
    const cfg_dir_entry *sel = p->selected >= 0 && p->selected < p->shown_count ? &p->entries[p->shown[p->selected]] : NULL;
    /* Save as: a name saves (an empty one opens the folder selected) */
    const int open_folder = p->kind == CFG_PICK_OUTPUT ? !p->name[0] && sel && sel->dir : p->kind != CFG_PICK_FOLDER && sel && sel->dir;
    const char *label = p->kind == CFG_PICK_FOLDER ? "Choose" : p->kind == CFG_PICK_OUTPUT && !open_folder ? "Save" : "Open";
    const int can = p->kind == CFG_PICK_FOLDER || open_folder || (p->kind == CFG_PICK_OUTPUT ? p->name[0] != 0 : sel != NULL);
    const int bottom = in.y + in.h - FIELD;
    const int cancel = ui_button(u, ui_id("pick-cancel", 0), (ui_rect){in.x + in.w - 190, bottom, 90, FIELD}, "Cancel", 0) ||
                       u->in.key == UI_KEY_ESCAPE;
    int ok = ui_button(u, ui_id("pick-ok", 0), (ui_rect){in.x + in.w - 92, bottom, 90, FIELD}, label, UI_DEFAULT | (can ? 0 : UI_DISABLED_FLAG));
    if (can && u->in.key == UI_KEY_ENTER && !activated) ok = 1;
    if (cancel) return -1;
    char full[MC_CONFIG_PATH * 2];
    if (activated && sel && sel->dir) {             /* a double click opens a folder, whatever the kind */
        join(full, sizeof full, p->folder, sel->name);
        show_folder(a, full);
        return 0;
    }
    if (!ok && !activated) return 0;
    if (open_folder) {
        join(full, sizeof full, p->folder, sel->name);
        show_folder(a, full);
        return 0;
    }
    if (p->kind == CFG_PICK_FOLDER) {
        if (sel && sel->dir) join(full, sizeof full, p->folder, sel->name);
        else snprintf(full, sizeof full, "%s", p->folder);
    } else if (p->kind == CFG_PICK_OUTPUT) {
        /* a typed path goes there: a folder opens, a file is the output */
        const int absolute = is_separator(p->name[0]) || (p->name[0] && p->name[1] == ':');
        if (absolute) snprintf(full, sizeof full, "%s", p->name);
        else join(full, sizeof full, p->folder, p->name);
        if (cfg_host_is_folder(full)) { show_folder(a, full); p->name[0] = 0; return 0; }
    } else if (sel) join(full, sizeof full, p->folder, sel->name);
    else return 0;
    if (strlen(full) >= MC_CONFIG_PATH || !chosen(a, full, path, capacity)) return -2;
    snprintf(a->last_folder, sizeof a->last_folder, "%s", p->kind == CFG_PICK_FOLDER ? full : p->folder);
    if (p->kind == CFG_PICK_FOLDER) parent_of(full, a->last_folder, sizeof a->last_folder);
    return 1;
}
