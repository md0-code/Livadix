/* The configuration window (docs/NOTES.md, "The configuration
 * window"): drawn by livadix.exe over the Macintosh's screen while the
 * emulation pauses (desktop.c). This part holds the state and draws every
 * frame into a canvas from the frame's input; the host (desktop.c,
 * config_host.c) gives it the window, the host's folders and the starts.
 *
 * The selected profile is edited as the configuration Livadix starts with
 * (an mc_config): every change goes through mc_profile_normalise (profile.h),
 * so the profile stays valid for its model, and is written into livadix.yaml
 * as the keys that differ (mc_profile_write), comments and other profiles
 * kept. The document is saved on Save; Start starts the selected profile as
 * edited, saved or not; Close leaves the window, the edits kept. */

#ifndef MC_CONFIG_APP_H
#define MC_CONFIG_APP_H

#include "../core/config.h"
#include "../core/hfs.h"
#include "../core/romdb.h"
#include "../core/yamledit.h"
#include "ui.h"

enum { CFG_TAB_MACHINE, CFG_TAB_VIDEO, CFG_TAB_STORAGE, CFG_TAB_SOUND, CFG_TAB_INPUT, CFG_TAB_PORTS, CFG_TAB_NETWORK, CFG_TAB_SHARING, CFG_TABS };
enum { CFG_DIALOG_NONE, CFG_DIALOG_NEW, CFG_DIALOG_DUPLICATE, CFG_DIALOG_RENAME, CFG_DIALOG_DELETE, CFG_DIALOG_QUIT, CFG_DIALOG_CLOSE, CFG_DIALOG_RESTART,
       CFG_DIALOG_ALERT, CFG_DIALOG_PICK };
/* What the file picker (config_picker.c) is for. */
enum { CFG_PICK_DISK, CFG_PICK_FLOPPY, CFG_PICK_CDROM, CFG_PICK_OUTPUT, CFG_PICK_FOLDER };

struct mc_model;
struct cfg_dir_entry;

/* The file picker: Livadix's own, drawn in the window on every host. */
typedef struct cfg_picker {
    int kind, drive;                   /* CFG_PICK_*, and the floppy drive or port */
    char folder[MC_CONFIG_PATH];       /* the folder shown, UTF-8 */
    struct cfg_dir_entry *entries;     /* its entries (config_host.h) */
    int count, readable;
    int *shown, shown_count;           /* the entries the filter shows */
    const char **labels;               /* their names, for scripts */
    int selected, scroll, all_files;
    char name[256];                    /* Save as (CFG_PICK_OUTPUT) */
    char places[16][64], place_paths[16][MC_CONFIG_PATH];
    int place_count;
} cfg_picker;

typedef struct cfg_host {
    void *ctx;
    /* Start: the Macintosh running now stops, its disks closed, and the
     * profile starts with this configuration. */
    void (*start)(void *ctx, const char *profile, const mc_config *config);
    /* The window closes and the Macintosh goes on, with the running profile's
     * changes that apply as it runs; the others wait for a new start
     * (desktop.c). NULL: nothing runs to go back to. */
    void (*resume)(void *ctx);
    /* Livadix quits. */
    void (*quit)(void *ctx);
    /* The running Macintosh shuts down (as closing the window does) and stays
     * off: the window shows the Livadix logo. NULL: no Stop. */
    void (*stop)(void *ctx);
    /* The window closes with no Macintosh running. NULL: it stays. */
    void (*close)(void *ctx);
} cfg_host;

#define CFG_VOLUME_CACHE 24
#define CFG_COM_PORTS 32

typedef struct cfg_app {
    ui ui;
    cfg_host host;
    char path[MC_CONFIG_PATH];         /* the file edited */
    char folder[MC_CONFIG_PATH];       /* its folder, with its separator ("" for the working one) */
    mc_ydoc doc;
    char *saved;                       /* the text as last loaded or saved */
    char *declined;                    /* the unsaved text Close was told not to save: not asked about again */
    int restart_closes;                /* the restart question came from Close: Later closes the window */
    unsigned changes;                  /* counts the edits and loads: a frame that makes one is drawn again */
    int exists;                        /* the file is on disk */
    const mc_rom_index *roms;          /* the roms folders (own_roms, or the host's) */
    mc_rom_index own_roms;
    int owns_roms;
    char doc_error[512];               /* the file does not read at all: why */
    /* The profiles: count 0 for a file without profiles:. */
    size_t profile_count;
    int default_index;
    char names[MC_CONFIG_PROFILES][MC_CONFIG_NAME];
    char models[MC_CONFIG_PROFILES][64];
    int selected, profile_scroll;
    uint8_t marked[MC_CONFIG_PROFILES]; /* the profiles selected together (Ctrl- and Shift-click), the shown one among them */
    int anchor;                        /* where a Shift-click's range starts */
    int reveal;                        /* the list scrolls to the shown profile */
    /* The selected profile as Livadix starts it. */
    mc_config config;
    int config_ok;                     /* it read from the file (else config holds its model's defaults) */
    char config_error[512];
    const struct mc_model *model;      /* the model it builds (auto's choice settled) */
    int rom_ok;
    char rom_note[512];
    int custom_mhz;                    /* the CPU speed is not the model's clock */
    /* The Model pop-up's filters (0 or "": any): the year, the family
     * (1 68k, 2 PowerPC), the processor (mc_model_cpu_type) and the kind
     * (MC_FORM_* + 1). The window's own, not saved. */
    int filter_year, filter_family, filter_form;
    char filter_cpu[24];
    /* The profile the Macintosh runs (has_running 0: none runs) and a
     * start that failed. */
    int has_running;
    char running[MC_CONFIG_NAME];
    /* The running profile's settings as the Macintosh started with them
     * (NULL: none): what a change that needs a new start is measured from. */
    const mc_config *running_from;
    char problem[1024];
    char problem_profile[MC_CONFIG_NAME]; /* the profile that could not start */
    /* The editor. */
    int tab;
    int disk_selected, disk_scroll, details_open;
    char mhz_text[24], pipe_text[2][256], forward_text[512];
    int share_selected, share_scroll;
    char share_name_text[MC_CONFIG_SHARE_NAME], server_text[MC_CONFIG_SHARE_NAME], password_text[16];
    unsigned media_chosen;             /* counts the floppy and CD images chosen, ejected or dropped */
    int pending_pick;                  /* the picker open: CFG_PICK_* + 1 (a script may answer it), and its drive */
    int pending_drive;
    cfg_picker picker;
    char last_folder[MC_CONFIG_PATH];  /* where the picker last chose (UTF-8) */
    /* Dialogs. */
    int dialog;
    char dialog_name[MC_CONFIG_NAME];
    char dialog_message[512];
    int alert_icon;
    char status[512];
    int status_icon;                   /* UI_ICON_* or -1 */
    /* Image contents, by path. */
    struct { char path[MC_CONFIG_PATH]; long long size, time; int ok; mc_volume_info info; } cache[CFG_VOLUME_CACHE];
    int cache_next;
    char com_ports[CFG_COM_PORTS][16];
    int com_count;
} cfg_app;

/* Opens path (a missing file starts from the template Livadix writes, unsaved)
 * with the roms folders' index (NULL: the app scans roms/ beside
 * program_folder and beside the file itself). */
int cfg_app_init(cfg_app *app, const char *path, const char *program_folder, const mc_rom_index *roms, const cfg_host *host);
void cfg_app_free(cfg_app *app);
/* One frame: the input, then everything drawn into app->ui.canvas (whose
 * pixels and size the caller sets); the window fills `window` (0 x 0: the
 * whole canvas), over whatever the canvas holds. */
void cfg_app_frame(cfg_app *app, const ui_input *in, ui_rect window);
/* Whether the document differs from the file. */
int cfg_app_dirty(const cfg_app *app);
int cfg_app_save(cfg_app *app);
/* A file dropped on the window: a floppy image into a floppy drive, a
 * disk or partitioned image as a hard disk, else a CD image. */
void cfg_app_drop(cfg_app *app, const char *path);
/* The selected profile's CD image (NULL or "": an empty drive). */
void cfg_app_put_cdrom(cfg_app *app, const char *path);
/* The answer to the open file picker, as a script gives it (`pick PATH`). */
void cfg_app_pick_result(cfg_app *app, const char *path);
/* Selects a profile by name; 0 when there is none. */
int cfg_app_select(cfg_app *app, const char *name);
/* Livadix is to quit: asks about unsaved changes first. */
void cfg_app_quit(cfg_app *app);
/* The window is to close (its Close, the close box, Escape, the host's key),
 * the edits kept: warns about unsaved changes first, once for the same ones. */
void cfg_app_close(cfg_app *app);
/* The profile the Macintosh runs (NULL: none runs), and a start that failed
 * (NULL: none), shown above the tabs. */
void cfg_app_set_running(cfg_app *app, const char *profile);
void cfg_app_set_problem(cfg_app *app, const char *problem);
/* A profile as edited, as Livadix starts it; 0 when it does not read. */
int cfg_app_profile(cfg_app *app, const char *profile, mc_config *out);
/* The name of the selected profile (NULL for a file without profiles:). */
const char *cfg_app_selected(const cfg_app *app);

#endif /* MC_CONFIG_APP_H */
