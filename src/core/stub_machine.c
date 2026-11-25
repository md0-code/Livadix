/* The stub machine (machine.h): stands in for every model no real machine
 * emulates (src/hw/machine.c decides): now only its own hidden model,
 * MC_MODEL_STUB (models.h), which the tests and gates use. It runs no guest code. Its cycle count and
 * guest time advance as a real machine's would, it shows a test pattern in
 * the configured resolution and depth, lights one cell per ADB key held,
 * draws a cursor where the mouse is (filled while the button is down) and
 * plays a short tone per key press, so the shell, the configuration, the
 * presenter and the sound path can be checked end to end.
 *
 * Livadix's own code; no hardware is described here. */
#include "stub_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The stub's sound: 22,254.5 Hz (C15M / 704), the rate of the Macintosh's
 * sound chips (Guide to the Macintosh Family Hardware, "Sound"); the shell
 * is told the nearest whole rate. */
#define AUDIO_DIVIDER 704u
#define AUDIO_RATE 22255u
#define TONE_FRAMES (AUDIO_RATE / 10u)     /* 100 ms per key press */
#define TONE_HALF_PERIOD 13u               /* about 856 Hz */
/* The pattern animates at the Macintosh's 60.15 Hz tick. */
#define TICK_NUM 6015u
#define TICK_DEN (MC_C15M_HZ * 100ull)

struct mc_stub {
    const mc_model *model;
    char description[192], error[256];
    uint8_t *rom; size_t rom_size;
    /* time */
    uint64_t instructions, time, remainder, rate;
    /* screen */
    uint32_t width, height, depth, row_bytes, colors;
    uint8_t *pixels, *base;
    uint32_t palette[256];
    uint64_t serial, drawn_tick, drawn_input;
    /* input echo */
    uint8_t keys[128];
    int mouse_x, mouse_y, button;
    uint64_t input_serial;
    /* sound */
    int sound;
    uint64_t audio_done, tone_until;
    mc_media media;
};

/* --- the screen ------------------------------------------------------------ */

static void build_palette(mc_stub *m) {
    static const uint32_t gray2[4] = {0xffffff, 0xaaaaaa, 0x555555, 0x000000};
    static const uint32_t colors16[16] = {0xffffff, 0xfcf305, 0xff6402, 0xdd0806, 0xf20884, 0x4600a5, 0x0000d4, 0x02abea,
                                          0x1fb714, 0x006411, 0x562c05, 0x90713a, 0xc0c0c0, 0x808080, 0x404040, 0x000000};
    memset(m->palette, 0, sizeof m->palette);
    switch (m->depth) {
    case 1: m->colors = 2; m->palette[0] = 0xffffff; m->palette[1] = 0; break;
    case 2: m->colors = 4; memcpy(m->palette, gray2, sizeof gray2); break;
    case 4: m->colors = 16; memcpy(m->palette, colors16, sizeof colors16); break;
    case 8:
        /* A 6x6x6 colour cube from white down, then a gray ramp, black last. */
        m->colors = 256;
        for (unsigned i = 0; i < 216; ++i) {
            unsigned r = 5 - i / 36, g = 5 - i / 6 % 6, b = 5 - i % 6;
            m->palette[i] = (r * 51u) << 16 | (g * 51u) << 8 | b * 51u;
        }
        for (unsigned i = 216; i < 255; ++i) { unsigned v = 255u - (i - 215u) * 255u / 40u; m->palette[i] = v << 16 | v << 8 | v; }
        m->palette[255] = 0;
        break;
    default: m->colors = 0; break;
    }
}
static unsigned nearest(const mc_stub *m, uint32_t rgb) {
    unsigned best = 0; long best_distance = -1;
    for (unsigned i = 0; i < m->colors; ++i) {
        long dr = (long)(m->palette[i] >> 16 & 255) - (long)(rgb >> 16 & 255);
        long dg = (long)(m->palette[i] >> 8 & 255) - (long)(rgb >> 8 & 255);
        long db = (long)(m->palette[i] & 255) - (long)(rgb & 255);
        long distance = dr * dr * 3 + dg * dg * 4 + db * db * 2;
        if (best_distance < 0 || distance < best_distance) { best = i; best_distance = distance; }
    }
    return best;
}
/* A rectangle of one colour, clipped to the screen, in the screen's format. */
static void fill(mc_stub *m, uint8_t *buffer, int x, int y, int w, int h, uint32_t rgb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)m->width) w = (int)m->width - x;
    if (y + h > (int)m->height) h = (int)m->height - y;
    if (w <= 0 || h <= 0) return;
    const unsigned d = m->depth;
    uint32_t value = d <= 8 ? nearest(m, rgb) :
                     d == 16 ? (rgb >> 9 & 0x7c00) | (rgb >> 6 & 0x03e0) | (rgb >> 3 & 0x001f) : rgb & 0xffffff;
    for (int row = y; row < y + h; ++row) {
        uint8_t *line = buffer + (size_t)row * m->row_bytes;
        for (int col = x; col < x + w; ++col) {
            if (d < 8) {
                unsigned bit = (unsigned)col * d, shift = 8 - d - bit % 8;
                uint8_t mask = (uint8_t)(((1u << d) - 1) << shift);
                line[bit / 8] = (uint8_t)((line[bit / 8] & ~mask) | (value << shift & mask));
            } else if (d == 8) line[col] = (uint8_t)value;
            else if (d == 16) { line[col * 2] = (uint8_t)(value >> 8); line[col * 2 + 1] = (uint8_t)value; }
            else { uint8_t *p = line + col * 4; p[0] = 0; p[1] = (uint8_t)(value >> 16); p[2] = (uint8_t)(value >> 8); p[3] = (uint8_t)value; }
        }
    }
}
/* The parts that never change: border, colour bars, gray ramp, a block of
 * single-pixel checks (shows how the window filters) and the key grid. */
static void draw_base(mc_stub *m) {
    static const uint32_t bars[8] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
    const int w = (int)m->width, h = (int)m->height;
    fill(m, m->base, 0, 0, w, h, 0x808080);
    for (int i = 0; i < 8; ++i) fill(m, m->base, w * i / 8, 0, w * (i + 1) / 8 - w * i / 8, h / 3, bars[i]);
    for (int i = 0; i < 16; ++i) {
        uint32_t v = 255u - (uint32_t)i * 17u;
        fill(m, m->base, w * i / 16, h / 3, w * (i + 1) / 16 - w * i / 16, h / 6, v << 16 | v << 8 | v);
    }
    const int cx = w * 3 / 4, cy = h / 2 + 8, cs = h / 6;
    for (int y = 0; y < cs; ++y)
        for (int x = 0; x < cs; ++x) fill(m, m->base, cx + x, cy + y, 1, 1, (x + y) & 1 ? 0x000000 : 0xffffff);
    fill(m, m->base, 0, 0, w, 1, 0); fill(m, m->base, 0, h - 1, w, 1, 0);
    fill(m, m->base, 0, 0, 1, h, 0); fill(m, m->base, w - 1, 0, 1, h, 0);
}
/* The key grid: 16 x 8 cells, ADB code = row x 16 + column. */
static void key_cell(const mc_stub *m, unsigned code, int *x, int *y, int *cw, int *ch) {
    *cw = (int)m->width / 32; *ch = (int)m->height / 24;
    *x = 8 + (int)(code % 16) * *cw;
    *y = (int)m->height / 2 + 8 + (int)(code / 16) * *ch;
}
static uint64_t tick_of(const mc_stub *m) { return m->time * TICK_NUM / TICK_DEN; }
static void draw(mc_stub *m) {
    uint64_t tick = tick_of(m);
    if (tick == m->drawn_tick && m->input_serial == m->drawn_input) return;
    m->drawn_tick = tick; m->drawn_input = m->input_serial;
    const int w = (int)m->width, h = (int)m->height;
    memcpy(m->pixels, m->base, (size_t)m->row_bytes * m->height);
    for (unsigned code = 0; code < 128; ++code) {
        int x, y, cw, ch;
        key_cell(m, code, &x, &y, &cw, &ch);
        fill(m, m->pixels, x, y, cw - 1, ch - 1, m->keys[code] ? 0x00ff00 : 0x404040);
    }
    /* A bar crossing the bottom strip, one pixel per tick. */
    int bar = (int)(tick % (uint64_t)w);
    fill(m, m->pixels, 0, h - h / 12, w, h / 12 - 1, 0x000000);
    fill(m, m->pixels, bar, h - h / 12, 4, h / 12 - 1, 0xffffff);
    /* The cursor: a cross, a filled square while the button is down. */
    const int mx = m->mouse_x, my = m->mouse_y;
    if (m->button) fill(m, m->pixels, mx - 5, my - 5, 11, 11, 0xff0000);
    fill(m, m->pixels, mx - 6, my - 1, 13, 3, 0xffffff); fill(m, m->pixels, mx - 1, my - 6, 3, 13, 0xffffff);
    fill(m, m->pixels, mx - 5, my, 11, 1, 0x000000); fill(m, m->pixels, mx, my - 5, 1, 11, 0x000000);
    ++m->serial;
}

int mc_stub_framebuffer(mc_stub *m, mc_framebuffer *fb) {
    if (!m || !fb) return 0;
    draw(m);
    fb->width = m->width; fb->height = m->height; fb->depth = m->depth; fb->row_bytes = m->row_bytes;
    fb->pixels = m->pixels; fb->palette = m->palette; fb->serial = m->serial;
    return 1;
}

void mc_framebuffer_argb(const mc_framebuffer *fb, uint32_t *out) {
    const unsigned d = fb->depth;
    for (uint32_t y = 0; y < fb->height; ++y) {
        const uint8_t *line = fb->pixels + (size_t)y * fb->row_bytes;
        uint32_t *o = out + (size_t)y * fb->width;
        for (uint32_t x = 0; x < fb->width; ++x) {
            uint32_t rgb;
            if (d <= 8) {
                unsigned bit = x * d, value = (unsigned)(line[bit / 8] >> (8 - d - bit % 8)) & ((1u << d) - 1);
                rgb = fb->palette[value];
            } else if (d == 16) {
                unsigned v = (unsigned)line[x * 2] << 8 | line[x * 2 + 1];
                unsigned r = v >> 10 & 31, g = v >> 5 & 31, b = v & 31;
                rgb = (r << 3 | r >> 2) << 16 | (g << 3 | g >> 2) << 8 | (b << 3 | b >> 2);
            } else {
                const uint8_t *p = line + x * 4;
                rgb = (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
            }
            o[x] = 0xff000000u | rgb;
        }
    }
    /* The machine's contrast (toward mid-gray) and brightness, of 256. */
    const unsigned c = fb->contrast && fb->contrast < 256 ? fb->contrast : 256u;
    const unsigned l = fb->level && fb->level < 256 ? fb->level : 256u;
    if (c < 256 || l < 256) {
        uint8_t map[256];
        for (unsigned v = 0; v < 256; v++) map[v] = (uint8_t)(((128u * 256u + ((int)v - 128) * (int)c) / 256u) * l / 256u);
        const size_t n = (size_t)fb->width * fb->height;
        for (size_t i = 0; i < n; i++) {
            const uint32_t v = out[i];
            out[i] = 0xff000000u | (uint32_t)map[v >> 16 & 255u] << 16 | (uint32_t)map[v >> 8 & 255u] << 8 | map[v & 255u];
        }
    }
}

/* --- life cycle and time -------------------------------------------------- */

mc_stub *mc_stub_create(const mc_config *c, const uint8_t *rom, size_t rom_size, char *error, size_t capacity) {
    const mc_model *p = mc_model_entry(c->model);
    if (!p) { if (capacity) snprintf(error, capacity, "Unknown model"); return NULL; }
    mc_stub *m = calloc(1, sizeof *m);
    if (!m) { if (capacity) snprintf(error, capacity, "Out of memory"); return NULL; }
    m->model = p;
    snprintf(m->description, sizeof m->description, "%s: not emulated yet, showing the test pattern", p->name);
    m->rom = rom_size ? malloc(rom_size) : NULL;
    if (rom_size && m->rom) { memcpy(m->rom, rom, rom_size); m->rom_size = rom_size; }
    m->rate = c->cpu_hz;
    m->width = c->width; m->height = c->height; m->depth = c->depth;
    m->row_bytes = (m->width * m->depth + 7) / 8;
    m->pixels = calloc(m->height, m->row_bytes);
    m->base = calloc(m->height, m->row_bytes);
    if ((rom_size && !m->rom) || !m->pixels || !m->base || !m->rate) {
        if (capacity) snprintf(error, capacity, m->rate ? "Out of memory" : "emulation.cpu_mhz must not be 0");
        mc_stub_destroy(m); return NULL;
    }
    build_palette(m);
    draw_base(m);
    m->drawn_tick = UINT64_MAX;
    m->mouse_x = (int)m->width / 2; m->mouse_y = (int)m->height / 2;
    m->sound = c->sound;
    mc_media_init(&m->media, c);
    return m;
}

void mc_stub_destroy(mc_stub *m) {
    if (!m) return;
    mc_media_close(&m->media);
    free(m->rom); free(m->pixels); free(m->base); free(m);
}
const char *mc_stub_description(const mc_stub *m) { return m ? m->description : ""; }
const char *mc_stub_error(const mc_stub *m) { return m ? m->error : ""; }
uint64_t mc_stub_cycles(const mc_stub *m) { return m ? m->instructions : 0; }
uint64_t mc_stub_time(const mc_stub *m) { return m ? m->time : 0; }
uint64_t mc_stub_rate(const mc_stub *m) { return m ? m->rate : 0; }

/* Guest time: time = cycles x C15M / rate (the clock), kept exactly with
 * the remainder (the stub runs no CPU: its cycles are only counted). Steps
 * of at most 2^30 cycles keep the product in range. */
static void advance(mc_stub *m, uint64_t n) {
    while (n) {
        uint64_t step = n > (1u << 30) ? (1u << 30) : n;
        uint64_t total = step * MC_C15M_HZ + m->remainder;
        m->time += total / m->rate; m->remainder = total % m->rate;
        m->instructions += step; n -= step;
    }
}
mc_run_status mc_stub_run(mc_stub *m, uint64_t cycles, uint64_t deadline) {
    if (deadline) {
        if (m->time >= deadline) return MC_RUN_OK;
        /* Cycles until time reaches the deadline. */
        uint64_t ticks = deadline - m->time;
        uint64_t need = ticks > UINT64_MAX / m->rate ? UINT64_MAX : (ticks * m->rate - m->remainder + MC_C15M_HZ - 1) / MC_C15M_HZ;
        if (need < cycles) cycles = need;
    }
    advance(m, cycles);
    return MC_RUN_OK;
}

/* --- input ---------------------------------------------------------------- */

int mc_stub_key(mc_stub *m, uint8_t code, int down) {
    if (code > 127) return 1;
    if (down && !m->keys[code]) m->tone_until = m->time / AUDIO_DIVIDER + TONE_FRAMES;
    m->keys[code] = (uint8_t)(down != 0);
    ++m->input_serial;
    return 1;
}
int mc_stub_mouse(mc_stub *m, int dx, int dy, int button) {
    m->mouse_x += dx; m->mouse_y += dy;
    if (m->mouse_x < 0) m->mouse_x = 0;
    if (m->mouse_y < 0) m->mouse_y = 0;
    if (m->mouse_x >= (int)m->width) m->mouse_x = (int)m->width - 1;
    if (m->mouse_y >= (int)m->height) m->mouse_y = (int)m->height - 1;
    m->button = button != 0;
    ++m->input_serial;
    return 1;
}

/* --- sound ---------------------------------------------------------------- */

uint32_t mc_stub_audio_rate(const mc_stub *m) { return m && m->sound ? AUDIO_RATE : 0; }
size_t mc_stub_audio_pull(mc_stub *m, int16_t *out, size_t frames) {
    if (!m->sound) return 0;
    uint64_t due = m->time / AUDIO_DIVIDER;
    /* Frames the shell never took are dropped beyond one second. */
    if (due - m->audio_done > AUDIO_RATE) m->audio_done = due - AUDIO_RATE;
    size_t n = 0;
    for (; n < frames && m->audio_done < due; ++n, ++m->audio_done) {
        int16_t v = m->audio_done < m->tone_until ? (m->audio_done / TONE_HALF_PERIOD & 1 ? 6000 : -6000) : 0;
        out[n * 2] = out[n * 2 + 1] = v;
    }
    return n;
}

/* --- media ---------------------------------------------------------------- */

unsigned mc_stub_drives(const mc_stub *m, mc_media_kind kind) { return mc_media_drives(&m->media, kind); }
int mc_stub_insert(mc_stub *m, mc_media_kind kind, unsigned index, mc_disk *disk, char *error, size_t capacity) {
    return mc_media_insert(&m->media, kind, index, disk, m->instructions != 0, error, capacity);
}
mc_disk *mc_stub_media(const mc_stub *m, mc_media_kind kind, unsigned index, int *ejected) {
    return mc_media_get(&m->media, kind, index, ejected);
}
int mc_stub_eject(mc_stub *m, mc_media_kind kind, unsigned index) { return mc_media_eject(&m->media, kind, index); }
int mc_stub_reinsert(mc_stub *m) { return mc_media_reinsert(&m->media); }
