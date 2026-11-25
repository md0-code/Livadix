/* Livadix — the PDM boards' built-in video (pdm_video.h).
 *
 * Livadix's own code. */
#include "pdm_video.h"

#include <string.h>

typedef struct monitor_info {
    uint32_t width, height;
    uint8_t code;               /* standard sense code (7: extended) */
    int8_t ext;                 /* extended code; -1 none */
    uint32_t dots, lines, blank, clock;     /* dots per line, lines per frame, blanking lines, dot clock (Hz) */
    uint32_t max_depth;
} monitor_info;

/* Developer Note, Table 3-10 (dot clocks, H line length, V screen length,
 * V sync space) and Table 3-8 (depths); the sense codes of Technical Note
 * HW 30. With no monitor the 13-inch timing runs†. */
static const monitor_info monitors[MC_PDM_MONITORS] = {
    [MC_PDM_MONITOR_NONE] = {0, 0, 7, 0x3F, 896, 525, 45, 31334400, 0},
    [MC_PDM_MONITOR_RGB12] = {512, 384, 2, -1, 640, 407, 23, 15667200, 16},
    [MC_PDM_MONITOR_RGB13] = {640, 480, 6, -1, 896, 525, 45, 31334400, 16},
    [MC_PDM_MONITOR_PORTRAIT] = {640, 870, 1, -1, 832, 918, 48, 57283200, 8},
    [MC_PDM_MONITOR_RGB16] = {832, 624, 7, 0x2D, 1152, 667, 43, 57283200, 8},
    [MC_PDM_MONITOR_VGA] = {640, 480, 7, 0x17, 800, 525, 45, 25175000, 8},
};

mc_pdm_monitor mc_pdm_video_monitor_for(uint32_t width, uint32_t height) {
    if (width == 512 && height == 384) return MC_PDM_MONITOR_RGB12;
    if (width == 640 && height == 480) return MC_PDM_MONITOR_RGB13;
    if (width == 640 && height == 870) return MC_PDM_MONITOR_PORTRAIT;
    if (width == 832 && height == 624) return MC_PDM_MONITOR_RGB16;
    return MC_PDM_MONITOR_NONE;
}

uint32_t mc_pdm_video_max_depth(mc_pdm_monitor monitor) {
    return monitor < MC_PDM_MONITORS ? monitors[monitor].max_depth : 0;
}

/* The levels on the three lines (Technical Note HW 30): a line driven low
 * reads low; a monitor grounds the lines its code has at 0; an
 * extended-sense monitor (code 7) ties pairs of lines, so driving one low
 * pulls another low where the pair's bit is 0: (1,0) when line 2 is low in
 * bits 5-4 of the code, (2,0) when 1 is low in bits 3-2, (2,1) when 0 is
 * low in bits 1-0 (the order the ROM assembles the code in,
 * $FFCCDBDE-$FFCCDC2E). */
uint8_t mc_pdm_video_sense(const mc_pdm_video *v) {
    const monitor_info *m = &monitors[v->monitor];
    const uint8_t driven = (uint8_t)(~v->sense & 7u);
    uint8_t out = 0;
    for (unsigned line = 0; line < 3; line++) {
        unsigned level = 1;
        if (driven >> line & 1u) level = 0;
        else if (!(m->code >> line & 1u)) level = 0;
        else if (m->ext >= 0) {
            for (unsigned low = 0; low < 3; low++) {
                if (!(driven >> low & 1u) || low == line) continue;
                const unsigned pair = (unsigned)m->ext >> (2u * low) & 3u;
                const unsigned hi = low == 2 ? 1u : 2u;
                const unsigned bit = line == hi ? (pair >> 1) & 1u : pair & 1u;
                if (!bit) level = 0;
            }
        }
        out |= (uint8_t)(level << line);
    }
    return out;
}

/* --- time ------------------------------------------------------------------------- */

static void timing(mc_pdm_video *v) {
    const monitor_info *m = &monitors[v->monitor];
    v->frame = (uint64_t)MC_C15M_HZ * m->dots * m->lines / m->clock;
    v->blank = (uint64_t)MC_C15M_HZ * m->dots * m->blank / m->clock;
}

static void advance(mc_pdm_video *v, uint64_t now) {
    if (now < v->frame_start) return;
    v->frame_start += (now - v->frame_start) / v->frame * v->frame;
    const uint8_t vbl = now - v->frame_start < v->blank;
    if (vbl != v->vbl) {
        v->vbl = vbl;
        mc_dev_line(&v->dev, MC_PDM_VIDEO_LINE_VBL, vbl);
    }
}

static void schedule(mc_pdm_video *v) {
    mc_dev_schedule(&v->dev, v->frame_start + (v->vbl ? v->blank : v->frame));
}

static void video_event(void *dev, uint64_t now) {
    mc_pdm_video *v = dev;
    advance(v, now);
    schedule(v);
}

/* --- registers -------------------------------------------------------------------- */

static uint8_t reg_read(mc_pdm_video *v, unsigned reg) {
    switch (reg) {
    case 0: return v->ctl;
    case 1: return v->depth;
    case 2: return (uint8_t)(mc_pdm_video_sense(v) << 4 | (v->sense & 7u));
    default: return 0;
    }
}

static void reg_write(mc_pdm_video *v, unsigned reg, uint8_t value) {
    switch (reg) {
    case 0: v->ctl = value; break;
    case 1: v->depth = value & 7u; break;
    case 2: v->sense = value & 7u; break;
    default: break;
    }
}

static uint32_t video_read(void *dev, uint32_t offset, unsigned size) {
    mc_pdm_video *v = dev;
    advance(v, mc_dev_now(&v->dev));
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | reg_read(v, (offset + i) & 3u);
    return r;
}

static void video_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_pdm_video *v = dev;
    for (unsigned i = 0; i < size; i++) reg_write(v, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void video_reset(void *dev) {
    mc_pdm_video *v = dev;
    v->ctl = 0x80;
    v->depth = 0;
    v->sense = 7;
}

const mc_dev_ops mc_pdm_video_ops = {video_read, video_write, video_event, video_reset, NULL};

/* --- setup and view ------------------------------------------------------------- */

void mc_pdm_video_init(mc_pdm_video *v, const mc_hw_host *host, const char *name, mc_pdm_monitor monitor) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_pdm_video_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->monitor = monitor < MC_PDM_MONITORS ? monitor : MC_PDM_MONITOR_NONE;
    timing(v);
    video_reset(v);
    v->frame_start = mc_dev_now(&v->dev);
    v->vbl = 1;
    mc_dev_line(&v->dev, MC_PDM_VIDEO_LINE_VBL, 1);
    schedule(v);
}

void mc_pdm_video_set_base(mc_pdm_video *v, uint32_t base) { v->base = base; }

int mc_pdm_video_view_get(mc_pdm_video *v, mc_pdm_video_view *view) {
    const monitor_info *m = &monitors[v->monitor];
    if (v->monitor == MC_PDM_MONITOR_NONE || (v->ctl & 0x80u) || v->depth > 4) return 0;
    const uint32_t depth = 1u << v->depth;
    if (depth > m->max_depth) return 0;
    view->width = m->width;
    view->height = m->height;
    view->depth = depth;
    view->row_bytes = m->width * depth / 8u;
    view->base = v->base;
    return 1;
}
