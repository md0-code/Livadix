/* Livadix — the Macintosh SE/30's built-in video (se30video.h).
 *
 * Livadix's own code. */
#include "se30video.h"

#include <string.h>

/* The classic Macintosh's frame: 704 dots of C15M a line, 370 lines
 * (60.15 Hz, Guide, chapter 12). */
#define FRAME (704u * 370u)

static void arm(mc_se30video *v) { mc_dev_schedule(&v->dev, v->frame_start + FRAME); }

static void event(void *dev, uint64_t now) {
    mc_se30video *v = dev;
    while (v->frame_start + FRAME <= now) v->frame_start += FRAME;
    if (v->irq_enabled && !v->irq) {
        v->irq = 1;
        mc_dev_line(&v->dev, MC_SE30VIDEO_LINE_IRQ, 1);
    }
    arm(v);
}

static void reset(void *dev) {
    mc_se30video *v = dev;
    v->main_page = 1;
    v->irq_enabled = 0;
    if (v->irq) {
        v->irq = 0;
        mc_dev_line(&v->dev, MC_SE30VIDEO_LINE_IRQ, 0);
    }
    v->frame_start = mc_dev_now(&v->dev);
    arm(v);
}

const mc_dev_ops mc_se30video_ops = {NULL, NULL, event, reset, NULL};

int mc_se30video_init(mc_se30video *v, const mc_hw_host *host, const char *name, const uint8_t *rom, size_t rom_size) {
    memset(v, 0, sizeof *v);
    if (!rom || rom_size != MC_SE30VIDEO_ROM) return 0;
    v->dev.ops = &mc_se30video_ops;
    v->dev.host = host;
    v->dev.name = name;
    memcpy(v->rom, rom, sizeof v->rom);
    v->main_page = 1;
    v->palette[0] = 0xFFFFFFFFu;
    v->palette[1] = 0xFF000000u;
    return 1;
}

void mc_se30video_map(mc_se30video *v, mc_bus *bus) {
    for (uint32_t mb = 0; mb < 16; mb++) {
        const uint32_t base = MC_SE30VIDEO_BASE + (mb << 20);
        mc_bus_map_memory(bus, base, 0xF0000u, v->vram, MC_SE30VIDEO_VRAM, 1);
        mc_bus_map_memory(bus, base + 0xF0000u, 0x10000u, v->rom, MC_SE30VIDEO_ROM, 0);
    }
}

void mc_se30video_set_page(mc_se30video *v, int main_page) { v->main_page = main_page != 0; }

void mc_se30video_set_sync_enable(mc_se30video *v, int pb6) {
    v->irq_enabled = !pb6;
    if (pb6 && v->irq) {
        v->irq = 0;
        mc_dev_line(&v->dev, MC_SE30VIDEO_LINE_IRQ, 0);
    }
}

void mc_se30video_view_get(const mc_se30video *v, mc_se30video_view *view) {
    view->pixels = v->vram + (v->main_page ? MC_SE30VIDEO_MAIN : MC_SE30VIDEO_ALT);
    view->palette = v->palette;
}
