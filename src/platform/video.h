/* The presenter: a window and how the picture is put on it.
 *
 * Frames arrive as ARGB8888 of any size (the guest screen at its own resolution, or a
 * larger rendering of it) and are shown at the configured display mode, scale,
 * shape and filtering, through OpenGL 3.3 (plain or the CRT shader) or, when
 * GL cannot be had, through the SDL renderer without the shader. Every
 * option can change while Livadix runs. Nothing here knows what is in the
 * picture. */

#ifndef MC_VIDEO_H
#define MC_VIDEO_H

#include "settings.h"

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct mc_video mc_video;

/* `screen_w` x `screen_h` is the guest screen's resolution and `aspect_w:aspect_h`
 * the shape it had on a monitor; both fix the layout whatever the size of
 * the frames shown. `hidden` opens no visible window (headless runs). */
mc_video *mc_video_open(const char *title, const mc_settings *s, int screen_w, int screen_h, int aspect_w,
                        int aspect_h, bool hidden, char *err, size_t errlen);
void mc_video_close(mc_video *v);

void mc_video_present(mc_video *v, const uint32_t *frame, int w, int h);
void mc_video_set_geometry(mc_video *v, int w, int h);
/* An ARGB image (alpha 0 transparent) drawn over the whole window after the
 * picture, which is shown as it is (look, scale, filter); the caller keeps it
 * until it is cleared with NULL. */
void mc_video_set_overlay(mc_video *v, const uint32_t *argb, int w, int h);
/* Where an overlay of w x h falls in an output: the largest rectangle of its
 * shape, centred. */
SDL_Rect mc_video_overlay_rect(int w, int h, int out_w, int out_h);

/* Live changes; the settings stay the caller's to save. */
/* Full screen is always borderless, on the desktop at its own resolution:
 * no mode switch, instant toggling, and the only kind every platform has. */
/* The window's icon: `size` x `size` ARGB pixels (copied). */
void mc_video_set_icon(mc_video *v, const uint32_t *argb, int size);
void mc_video_set_fullscreen(mc_video *v, bool on);
bool mc_video_fullscreen(const mc_video *v);
void mc_video_set_filter(mc_video *v, int filter);
void mc_video_set_integer_scale(mc_video *v, int scale);
void mc_video_set_aspect(mc_video *v, int aspect);
void mc_video_set_vsync(mc_video *v, bool on);
/* Returns false when the CRT shader is unavailable (no OpenGL 3.3). */
bool mc_video_set_crt(mc_video *v, bool on);
bool mc_video_crt_available(const mc_video *v);
/* How the picture is shown: plain; a CRT (the CRT shader's preset: curvature,
 * scanlines, aperture stripe, glow); or an LCD (sharp, flat, a faint grid
 * between pixels shown two or more output pixels wide); in full colour or
 * in one phosphor colour. All but plain in colour need the shader: false
 * when it is unavailable, and the picture stays plain. */
enum { MC_VIDEO_LOOK_PLAIN, MC_VIDEO_LOOK_CRT, MC_VIDEO_LOOK_LCD };
enum { MC_VIDEO_PHOSPHOR_COLOR, MC_VIDEO_PHOSPHOR_WHITE, MC_VIDEO_PHOSPHOR_GRAY };
bool mc_video_set_look(mc_video *v, int look, int phosphor);

/* A window position (mouse event coordinates) in screen pixels; false when it
 * is outside the picture (the result is clamped to the picture). */
bool mc_video_to_screen(const mc_video *v, int wx, int wy, int *gx, int *gy);

/* Save the next presented frame as a BMP: as it appears in the window, or
 * `raw` as the frame itself at its own size. */
void mc_video_request_shot(mc_video *v, const char *path, bool raw);
/* The path the last shot went to, empty when it failed; cleared by reading. */
bool mc_video_take_shot_result(mc_video *v, char *path, size_t len);

SDL_Window *mc_video_window(mc_video *v);

#endif /* MC_VIDEO_H */
