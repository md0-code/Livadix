/* Text, panels and short messages, drawn into an ARGB frame. */

#ifndef MC_OVERLAY_H
#define MC_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>

int mc_text_width(const char *s);
void mc_text_draw(uint32_t *fb, int w, int h, int x, int y, const char *s, uint32_t color);
/* A rectangle blended over the frame; alpha 0..256. */
void mc_panel(uint32_t *fb, int w, int h, int x, int y, int pw, int ph, uint32_t color, int alpha);

/* A one-line message at the bottom left for a couple of seconds. */
typedef struct mc_osd {
    char text[128];
    uint32_t until_ms;
} mc_osd;

void mc_osd_show(mc_osd *o, uint32_t now_ms, const char *fmt, ...);
void mc_osd_draw(const mc_osd *o, uint32_t now_ms, uint32_t *fb, int w, int h);

#endif /* MC_OVERLAY_H */
