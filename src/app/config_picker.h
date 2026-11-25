/* The configuration window's file picker (config_picker.c): Livadix's own,
 * the same on every host.
 *
 * Livadix's own code. */
#ifndef MC_CONFIG_PICKER_H
#define MC_CONFIG_PICKER_H

#include "config_app.h"

/* Opens the picker for kind (CFG_PICK_*) and the floppy drive or port as a
 * dialog of the window (CFG_DIALOG_PICK). */
void cfg_picker_open(cfg_app *app, int kind, int drive);
/* One frame of the open picker, centred in window: 1 with the chosen path
 * in path (as the configuration holds it), -1 when cancelled, -2 when the
 * chosen name cannot be used on this host (the name in dialog_message),
 * 0 while it stays open. */
int cfg_picker_frame(cfg_app *app, ui_rect window, char *path, size_t capacity);
void cfg_picker_close(cfg_app *app);

#endif /* MC_CONFIG_PICKER_H */
