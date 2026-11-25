/* The configuration window's services from the host (config_host.c): the
 * folders the file picker shows, the serial ports, a canvas saved as a BMP,
 * and the scripts the checks drive the window with. */

#ifndef MC_CONFIG_HOST_H
#define MC_CONFIG_HOST_H

#include "config_app.h"

#include <stdio.h>

/* The file picker's view of the host (config_picker.c). Paths are UTF-8. */
typedef struct cfg_dir_entry {
    char name[256];
    int dir;
    long long size, time;              /* bytes; seconds since 1970 */
} cfg_dir_entry;
/* The entries of a folder, folders first, by name without case, hidden ones
 * left out; their count (*out to free), -1 when it cannot be read. */
int cfg_host_list(const char *folder, cfg_dir_entry **out);
/* The places the picker offers besides a folder's parents: Desktop, Home and
 * the drives (Windows); Desktop, Home, the root and the mounted media (Linux). */
int cfg_host_places(char names[][64], char paths[][MC_CONFIG_PATH], int max);
int cfg_host_absolute(const char *path, char *out, size_t capacity);
int cfg_host_is_folder(const char *path);
char cfg_host_separator(void);
/* A picked path as the configuration holds it for kind (CFG_PICK_*), and
 * back: on Windows the files in the Windows code page (the core opens them
 * through the C library), a shared folder in UTF-8; 0 when it cannot be. */
int cfg_host_native_path(const char *utf8, int kind, char *out, size_t capacity);
int cfg_host_utf8_path(const char *native, int kind, char *out, size_t capacity);
/* The serial ports Windows has (HKLM\HARDWARE\DEVICEMAP\SERIALCOMM); on
 * Linux the tty devices /dev/ttyUSB*, /dev/ttyACM* and /dev/ttyS*. */
void cfg_host_serial_ports(cfg_app *app);
/* A 24-bit BMP of the canvas. */
int cfg_host_save_bmp(const ui_canvas *canvas, const char *path);

/* A script: one command per line (docs/NOTES.md, "Command line"). */
typedef struct cfg_script {
    FILE *f;
    int line;
    int done;
} cfg_script;
/* The words of a line; "quoted words" keep their spaces. Returns how many. */
int cfg_script_words(char *line, char **out, int max);
/* The UI_KEY_* of a key's name ("tab", "shift+tab", "ctrl+s"), the
 * modifiers into mods; a single character as its negative; 0 unknown. */
int cfg_script_key(const char *name, int *mods);

#endif /* MC_CONFIG_HOST_H */
