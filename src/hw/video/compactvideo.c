/* Livadix — the compact Macintosh's video and sound (compactvideo.h). Livadix's
 * own code. */
#include "compactvideo.h"

#include <string.h>

#define VBL_LINE 342u

/* The RAM byte at address `a` of the 4 MB space as the board decodes it
 * (machines/compact.c, mc_compact_map_ram): a power of two repeats; 2.5 MB
 * is a 2 MB row where A21 is low and a 512 KB row where it is high. */
static uint32_t ram_offset(const mc_compactvideo *v, uint32_t a) {
    const uint32_t n = v->ram_size;
    a &= 0x3FFFFFu;
    if (!(n & (n - 1u))) return a & (n - 1u);
    uint32_t row_a = 1;
    while (row_a * 2u <= n) row_a *= 2u;
    return (a & row_a) ? row_a + (a & (n - row_a - 1u)) : (a & (row_a - 1u));
}

static void push(mc_compactvideo *v, int16_t sample) {
    if (v->count == MC_COMPACTVIDEO_RING) {
        v->head = (v->head + 1) % MC_COMPACTVIDEO_RING;
        v->count--;
    }
    v->ring[(v->head + v->count++) % MC_COMPACTVIDEO_RING] = sample;
}

unsigned mc_compactvideo_pwm_place(uint8_t byte) {
    static uint8_t place[64];
    static int ready;
    if (!ready) {
        uint8_t x = 0x1E;
        memset(place, 62, sizeof place);
        for (unsigned i = 0; i < 63; i++) {
            place[x] = (uint8_t)i;
            x = (uint8_t)((x >> 1) | (((x ^ (x >> 1)) & 1u) << 5));
        }
        ready = 1;
    }
    return place[byte & 0x3Fu];
}

int mc_compactvideo_hblank(const mc_compactvideo *v, uint64_t now) {
    const int64_t d = (int64_t)(now - v->frame);
    const uint64_t pos = (uint64_t)((d % (int64_t)MC_COMPACTVIDEO_FRAME + (int64_t)MC_COMPACTVIDEO_FRAME) % (int64_t)MC_COMPACTVIDEO_FRAME);
    return pos % MC_COMPACTVIDEO_LINE >= MC_COMPACTVIDEO_WIDTH;
}

uint32_t mc_compactvideo_speed(const mc_compactvideo *v) {
    const uint32_t base = v->sound_main ? MC_COMPACTVIDEO_SOUND_MAIN : MC_COMPACTVIDEO_SOUND_ALT;
    unsigned sum = 0;
    for (unsigned i = 0; i < MC_COMPACTVIDEO_LINES; i++) sum += mc_compactvideo_pwm_place(v->ram[ram_offset(v, base + 2u * i + 1u)]);
    /* 650 - 350 * place / 44 rpm, x 16. */
    const int64_t speed = (650 * 16 * 44 * (int64_t)MC_COMPACTVIDEO_LINES - 350 * 16 * (int64_t)sum) / (44 * (int64_t)MC_COMPACTVIDEO_LINES);
    return speed > 0 ? (uint32_t)speed : 0;
}

/* The frame's 370 samples: line i's word's high byte, at the volume, while
 * PB7 is low; a high PB7 holds the output at the rest level ($80, the
 * sound off; †). A DC filter (a pole at about 20 Hz) takes the buffer's
 * own rest level out. */
static void make_samples(mc_compactvideo *v) {
    const uint32_t base = v->sound_main ? MC_COMPACTVIDEO_SOUND_MAIN : MC_COMPACTVIDEO_SOUND_ALT;
    uint8_t pb7 = v->pb7_frame;
    unsigned e = 0;
    for (unsigned i = 0; i < MC_COMPACTVIDEO_LINES; i++) {
        const uint64_t t = v->frame + (uint64_t)i * MC_COMPACTVIDEO_LINE;
        while (e < v->edges && v->edge_time[e] <= t) pb7 = v->edge_level[e++];
        const int byte = pb7 ? 128 : v->ram[ram_offset(v, base + 2u * i)];
        const float x = (float)((byte - 128) * 16 * (int)v->volume);
        if (!v->primed) {               /* the filter starts at the rest level */
            v->hp_in = x;
            v->primed = 1;
        }
        const float y = x - v->hp_in + 0.9944f * v->hp_out;
        v->hp_in = x;
        v->hp_out = y;
        const float c = y > 32767.0f ? 32767.0f : y < -32768.0f ? -32768.0f : y;
        if (c > 256.0f || c < -256.0f) v->sounding = 1;
        push(v, (int16_t)c);
    }
}

static void video_event(void *dev, uint64_t now) {
    mc_compactvideo *v = dev;
    (void)now;
    if (!v->vbl) {
        make_samples(v);
        v->vbl = 1;
        mc_dev_line(&v->dev, MC_COMPACTVIDEO_LINE_VBL, 1);
        mc_dev_schedule(&v->dev, v->frame + MC_COMPACTVIDEO_FRAME);
    } else {
        v->vbl = 0;
        v->frame += MC_COMPACTVIDEO_FRAME;
        v->pb7_frame = v->pb7;
        v->edges = 0;
        mc_dev_line(&v->dev, MC_COMPACTVIDEO_LINE_VBL, 0);
        mc_dev_schedule(&v->dev, v->frame + VBL_LINE * MC_COMPACTVIDEO_LINE);
    }
}

static void video_reset(void *dev) {
    mc_compactvideo *v = dev;
    v->frame = mc_dev_now(&v->dev);
    v->vbl = 0;
    v->edges = 0;
    v->pb7_frame = v->pb7;
    mc_dev_line(&v->dev, MC_COMPACTVIDEO_LINE_VBL, 0);
    mc_dev_schedule(&v->dev, v->frame + VBL_LINE * MC_COMPACTVIDEO_LINE);
}

const mc_dev_ops mc_compactvideo_ops = {NULL, NULL, video_event, video_reset, NULL};

void mc_compactvideo_init(mc_compactvideo *v, const mc_hw_host *host, const char *name, const uint8_t *ram, uint32_t ram_size) {
    memset(v, 0, sizeof *v);
    v->dev.ops = &mc_compactvideo_ops;
    v->dev.host = host;
    v->dev.name = name;
    v->ram = ram;
    v->ram_size = ram_size;
    v->page_main = v->sound_main = 1;
    v->pb7 = v->pb7_frame = 1;
    v->palette[0] = 0xFFFFFFFFu;
    v->palette[1] = 0xFF000000u;
}

void mc_compactvideo_set_pa(mc_compactvideo *v, uint8_t pa) {
    v->page_main = (uint8_t)((pa >> 6) & 1u);
    v->sound_main = (uint8_t)((pa >> 3) & 1u);
    v->volume = (uint8_t)(pa & 7u);
}

void mc_compactvideo_set_pb7(mc_compactvideo *v, int level) {
    level = level != 0;
    if (level == v->pb7) return;
    v->pb7 = (uint8_t)level;
    if (v->edges < MC_COMPACTVIDEO_EDGES) {
        v->edge_time[v->edges] = mc_dev_now(&v->dev);
        v->edge_level[v->edges++] = (uint8_t)level;
    }
}

const uint8_t *mc_compactvideo_pixels(const mc_compactvideo *v) {
    return v->ram + ram_offset(v, v->page_main ? MC_COMPACTVIDEO_MAIN : MC_COMPACTVIDEO_ALT);
}

size_t mc_compactvideo_pull(mc_compactvideo *v, int16_t *stereo, size_t frames) {
    size_t n = 0;
    for (; n < frames && v->count; n++) {
        const int16_t s = v->ring[v->head];
        v->head = (v->head + 1) % MC_COMPACTVIDEO_RING;
        v->count--;
        stereo[2 * n] = stereo[2 * n + 1] = s;
    }
    return n;
}
