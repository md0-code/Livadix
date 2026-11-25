/* The presenter's settings (the platform layer).
 *
 * Livadix reads its settings from livadix.yaml (src/core/config.c); the shell
 * fills this structure from the `video` section and hands it to the
 * presenter, which needs nothing else. */

#ifndef MC_SETTINGS_H
#define MC_SETTINGS_H

#include <stdbool.h>

#define MC_PATH_MAX 1024

typedef enum mc_filter { MC_FILTER_NEAREST, MC_FILTER_LINEAR, MC_FILTER_SHARP } mc_filter;
typedef enum mc_aspect { MC_ASPECT_ORIGINAL, MC_ASPECT_SQUARE } mc_aspect;

typedef struct mc_settings {
    bool fullscreen;            /* borderless, on the desktop */
    int window_scale;           /* the window at start: the picture times this */
    int integer_scale;          /* 0: fill the window; N: whole multiples up to N */
    int aspect;                 /* mc_aspect */
    int filter;                 /* mc_filter */
    bool crt;
    bool vsync;
} mc_settings;

#endif /* MC_SETTINGS_H */
