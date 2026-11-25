/* The window: runs a machine paced to wall time, shows its screen through
 * the presenter, plays its sound and gives it the keyboard, the mouse and
 * dropped media. */
#ifndef MC_DESKTOP_H
#define MC_DESKTOP_H

#include "../core/machine.h"

typedef struct mc_desktop_options {
    int hidden;                 /* no visible window (headless checks) */
    uint64_t frames;            /* stop after this many presented frames; 0: run until closed */
    const char *screenshot;     /* save the last frame as a BMP */
    int unthrottled;            /* run as fast as the host can: each frame one 1/refresh s of guest time */
    const char *wav;            /* record the sound to this WAV file instead of playing it */
    /* Checks: a close request (as the window's close button makes) when
     * this many frames have passed, for each of the first close_count. */
    uint64_t close_at[4];
    unsigned close_count;
    /* Checks: ADB key transitions (bit 7 a release) after given frames. */
    uint64_t key_at[8];
    uint8_t key_code[8];
    unsigned key_count;
    /* Tools (test harnesses): called after
     * every run of the machine; returning 0 ends the run. */
    int (*after_run)(void *ctx, mc_machine *machine);
    void *ctx;
    /* Tools: when set, the frames presented and the wall time spent
     * converting and presenting them (performance-counter ticks). */
    uint64_t *presents, *present_ticks;
    /* Tools: when set, the guest time (C15M periods) the pacing dropped
     * after lags longer than its catch-up. */
    uint64_t *dropped;
    /* Tools: the hidden window's size in multiples of the screen (0: 1), as
     * a large window or full screen presents it. */
    int window_scale;
    /* Tools: called right after each frame is presented, on the window
     * loop's thread (a check can wait there as a display or a remote
     * desktop makes a present wait, and read what was presented). */
    void (*after_present)(void *ctx, mc_machine *machine, const uint32_t *pixels, int w, int h);
} mc_desktop_options;

/* 0 on a normal end; otherwise error says why. */
int mc_desktop_run(mc_machine *machine, const mc_config *config, const mc_desktop_options *options, char *error,
                   size_t capacity);

/* livadix.exe's window with the configuration window in it (docs/NOTES.md,
 * "The configuration window"): the Windows key opens it over the Macintosh's
 * screen and pauses the Macintosh; closing it applies the changes, starting
 * the Macintosh again when they need it; Start starts another profile. */
struct mc_rom_index;
typedef struct mc_desktop_session {
    void *ctx;
    /* Builds the machine of a configuration with its ROM and media (the
     * session owns it then); NULL with the reason. */
    mc_machine *(*build)(void *ctx, const mc_config *config, char *error, size_t capacity);
    const char *yaml;                   /* the configuration file */
    const char *program_folder;         /* livadix.exe's folder (roms/ beside it) */
    const struct mc_rom_index *roms;    /* the roms folders' index */
    const char *script;                 /* checks: commands for the configuration window, or NULL */
    int width, height;                  /* hidden runs: the configuration window's canvas (0: 820 x 600) */
} mc_desktop_session;
/* Runs the machine built from config (the session's from then on), or,
 * when none could start (machine NULL), the configuration window showing
 * why (problem). profile: the profile it runs (NULL: the file has none). */
int mc_desktop_session_run(mc_machine *machine, const mc_config *config, const char *profile, const char *problem,
                           const mc_desktop_options *options, const mc_desktop_session *session, char *error, size_t capacity);

/* The ADB key code of an SDL key (-1 for none), with the configured Command
 * key (MC_COMMAND_*). Exposed for the tests. */
int mc_desktop_adb_key(int sdl_scancode, uint32_t command_key);

#endif /* MC_DESKTOP_H */
