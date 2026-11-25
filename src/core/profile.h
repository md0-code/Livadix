/* The configuration window's rules for a profile (docs/NOTES.md,
 * "The configuration window"): a profile is edited as an mc_config (the
 * values Livadix starts with), kept valid for its model after every change
 * (mc_profile_normalise), reset to a model's best settings when the model
 * is chosen (mc_profile_preset), and written into livadix.yaml as the keys
 * that differ from what the file already gives it (mc_profile_write),
 * keeping comments and the other profiles. mc_profile_needs_restart tells
 * the settings a running Macintosh takes at once from those it needs a
 * new start for. */

#ifndef MC_PROFILE_H
#define MC_PROFILE_H

#include "config.h"
#include "yamledit.h"

struct mc_model;
struct mc_rom_index;
struct mc_volume_info;

typedef struct mc_profile_env {
    const struct mc_model *model;          /* the model the profile builds (auto's choice settled) */
    const struct mc_rom_index *roms;       /* the roms folders (controller firmware); NULL: none */
    const struct mc_volume_info *boot;     /* what the startup disk holds; NULL: unknown */
} mc_profile_env;

/* What a change touched, so that the setting just chosen wins over the
 * others it conflicts with. */
enum {
    MC_CHANGED_NONE, MC_CHANGED_MODEL, MC_CHANGED_RAM, MC_CHANGED_RESOLUTION, MC_CHANGED_DEPTH, MC_CHANGED_CARD,
    MC_CHANGED_MONITOR, MC_CHANGED_MOUSE_LEFT, MC_CHANGED_MOUSE_RIGHT, MC_CHANGED_STORAGE, MC_CHANGED_OTHER
};

/* The model a configuration builds: its model, or for model: auto the one
 * the roms folders settle (mc_config_find_rom), else the best emulated. */
const struct mc_model *mc_profile_model(const mc_config *c, const struct mc_rom_index *roms);
/* Every setting that depends on the model, set to the model's best: its
 * memory (at least what the startup disk's System needs, in a size the
 * board takes), clock, screen, look and monitor, the automatic display,
 * controller and network card, the 68040's FPU as the chip, the internal
 * IDE disk where the model has one. The disks, floppies, CD and every
 * other setting stay. */
void mc_profile_preset(mc_config *c, const mc_profile_env *env);
/* Makes every setting valid with the others for the model: memory a size
 * the board takes and at least what the System needs, a screen mode the
 * display shows, a monitor the model had, firmware that is there, buses
 * and cards the model has, one image in one place, a mouse button that
 * clicks. `changed` (MC_CHANGED_*) keeps the setting just chosen. */
void mc_profile_normalise(mc_config *c, const mc_profile_env *env, int changed);

/* The memory sizes the model takes (KiB, at most max), ascending. */
size_t mc_profile_ram_sizes(const struct mc_model *model, uint32_t *sizes, size_t max);
/* The least memory the startup disk's System needs (0: unknown). */
uint32_t mc_profile_ram_least(const mc_profile_env *env);
/* The depths (MC_DEPTH_BIT mask) the configured display shows at w x h. */
unsigned mc_profile_depths(const mc_config *c, const struct mc_model *model, unsigned w, unsigned h);
/* Whether the model has built-in video, and a slot for the Livadix Display
 * Card, at any resolution. */
int mc_profile_has_builtin(const struct mc_model *model);
int mc_profile_has_card(const struct mc_model *model);

/* Every value Livadix takes from a profile, compared (paths without regard to
 * letter case or the slash; an unconnected and a disabled port alike). */
int mc_profile_same(const mc_config *a, const mc_config *b);
/* 1 when going from the running configuration to the edited one needs the
 * Macintosh started again; 0 when every change applies to it as it runs
 * (look, monitor, filter, scale, full screen, vsync, refresh, sound, the
 * keyboard's Command key, the mouse buttons, closing, the startup chime,
 * the floppy and CD images in their drives). */
int mc_profile_needs_restart(const mc_config *running, const mc_config *edited);

/* The profile named (NULL: the file without profiles:) as Livadix reads it,
 * the others left out so that one broken profile does not hide the rest;
 * with roms, model: auto settled as Livadix settles it at start (its model
 * and that model's values for the keys the file does not write). source:
 * the file the document stands for (relative paths). */
int mc_profile_read(const mc_ydoc *doc, const char *profile, const char *source, const struct mc_rom_index *roms, mc_config *c,
                    char *error, size_t capacity);
/* Writes c into the document as that profile: the model, then each key
 * whose value differs from what the document gives the profile now, set
 * in the profile (or taken out when the shared settings and the model give
 * it); then reads it back and checks that Livadix reads c. 0 with a message. */
int mc_profile_write(mc_ydoc *doc, const char *profile, const mc_config *c, const char *source, const struct mc_rom_index *roms,
                     char *error, size_t capacity);

#endif /* MC_PROFILE_H */
