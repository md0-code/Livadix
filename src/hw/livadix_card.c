/* Livadix — the Livadix Display Card (see livadix_card.h). Livadix's own code. */
#include "livadix_card.h"

#include <stdlib.h>
#include <string.h>

const uint16_t mc_livadix_card_modes[MC_LIVADIX_CARD_MODES][2] = {
    {512, 342}, {512, 384}, {640, 400}, {640, 480}, {800, 600}, {832, 624},
    {1024, 768}, {1152, 864}, {1152, 870}, {1280, 1024}, {1600, 1200}, {1920, 1080},
};

int mc_livadix_card_mode(uint32_t width, uint32_t height) {
    for (int i = 0; i < MC_LIVADIX_CARD_MODES; i++)
        if (mc_livadix_card_modes[i][0] == width && mc_livadix_card_modes[i][1] == height) return i;
    return -1;
}

static void update_irq(mc_livadix_card *c) {
    const uint8_t irq = (c->status & MC_LIVADIX_STATUS_VBL) && (c->control & MC_LIVADIX_CONTROL_VBL_IRQ);
    if (irq != c->irq) {
        c->irq = irq;
        mc_dev_line(&c->dev, MC_LIVADIX_CARD_LINE_IRQ, irq);
    }
}

/* The next event: the vertical blank, or the blitter finishing first. */
static void schedule(mc_livadix_card *c) {
    mc_dev_schedule(&c->dev, c->blt.done && c->blt.done < c->next_vbl ? c->blt.done : c->next_vbl);
}

/* The vertical blank: every `period` C15M periods of guest time. */
static void card_event(void *dev, uint64_t now) {
    mc_livadix_card *c = dev;
    if (c->blt.done && c->blt.done <= now) c->blt.done = 0;
    if (c->next_vbl <= now) {
        while (c->next_vbl <= now) {
            c->next_vbl += c->period;
            c->vbl_count++;
        }
        c->status |= MC_LIVADIX_STATUS_VBL;
        update_irq(c);
    }
    schedule(c);
}

/* The blitter (livadix_card.h). Pixels are big-endian, the leftmost in the
 * high bits of a byte at depths under 8. */
#define BLT(r) b->reg[((r) - MC_LIVADIX_REG_BLT_OP) / 4u]

static uint32_t get_pixel(const uint8_t *row, uint32_t x, uint32_t d) {
    switch (d) {
    case 32: row += 4u * x; return (uint32_t)row[0] << 24 | (uint32_t)row[1] << 16 | (uint32_t)row[2] << 8 | row[3];
    case 16: row += 2u * x; return (uint32_t)row[0] << 8 | row[1];
    case 8: return row[x];
    default: {
        const uint32_t bit = x * d, shift = 8u - d - (bit & 7u);
        return (uint32_t)(row[bit >> 3] >> shift) & ((1u << d) - 1u);
    }
    }
}
static void put_pixel(uint8_t *row, uint32_t x, uint32_t d, uint32_t v) {
    switch (d) {
    case 32: row += 4u * x; row[0] = (uint8_t)(v >> 24); row[1] = (uint8_t)(v >> 16); row[2] = (uint8_t)(v >> 8); row[3] = (uint8_t)v; return;
    case 16: row += 2u * x; row[0] = (uint8_t)(v >> 8); row[1] = (uint8_t)v; return;
    case 8: row[x] = (uint8_t)v; return;
    default: {
        const uint32_t bit = x * d, shift = 8u - d - (bit & 7u), mask = ((1u << d) - 1u) << shift;
        row[bit >> 3] = (uint8_t)((row[bit >> 3] & ~mask) | ((v << shift) & mask));
        return;
    }
    }
}

/* Whether a w x h rectangle at (x, y) of the map at `base`, `stride` lies
 * in VRAM. */
static int inside(uint32_t vram_size, uint32_t base, uint32_t stride, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t d) {
    const uint64_t first = (uint64_t)base + (uint64_t)y * stride, end_bit = ((uint64_t)x + w) * d;
    return end_bit <= (uint64_t)stride * 8u && first + (uint64_t)(h - 1) * stride + (end_bit + 7) / 8 <= vram_size;
}

/* The operation on the part x0..x1, y0..y1 (exclusive) of the destination
 * rectangle; a copy reads the source from the scratch buffer, which holds
 * the source rectangle (rows of `sbytes` from pixel `sx0`). */
static void blit_part(mc_livadix_blitter *b, uint8_t *vram, uint32_t op, uint32_t d, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                      size_t sbytes) {
    const uint32_t mask = d == 32 ? 0xFFFFFFFFu : (1u << d) - 1u;
    const uint32_t dst = BLT(MC_LIVADIX_REG_BLT_DST), dstride = BLT(MC_LIVADIX_REG_BLT_DST_STRIDE);
    const uint32_t dx = BLT(MC_LIVADIX_REG_BLT_DX), dy = BLT(MC_LIVADIX_REG_BLT_DY);
    if (op == MC_LIVADIX_BLT_COPY) {
        const uint32_t sx = BLT(MC_LIVADIX_REG_BLT_SX);
        for (uint32_t y = y0; y < y1; y++) {
            const uint8_t *srow = b->scratch + (size_t)(y - dy) * sbytes;
            uint8_t *drow = vram + dst + (size_t)y * dstride;
            if (!(d & 7u)) {
                memcpy(drow + (size_t)x0 * (d / 8u), srow + (size_t)(x0 - dx) * (d / 8u), (size_t)(x1 - x0) * (d / 8u));
                continue;
            }
            /* The scratch rows keep the source's own bit positions. */
            for (uint32_t x = x0; x < x1; x++) put_pixel(drow, x, d, get_pixel(srow, sx % (8u / d) + (x - dx), d));
        }
        return;
    }
    const uint32_t fg = BLT(MC_LIVADIX_REG_BLT_FG) & mask, bg = BLT(MC_LIVADIX_REG_BLT_BG) & mask;
    const uint32_t px = BLT(MC_LIVADIX_REG_BLT_PATX), py = BLT(MC_LIVADIX_REG_BLT_PATY);
    const uint32_t p0 = BLT(MC_LIVADIX_REG_BLT_PAT0), p1 = BLT(MC_LIVADIX_REG_BLT_PAT1);
    for (uint32_t y = y0; y < y1; y++) {
        const uint32_t pr = (y + py) & 7u;
        const uint8_t bits = (uint8_t)((pr < 4 ? p0 : p1) >> (8u * (3u - (pr & 3u))));
        uint8_t *drow = vram + dst + (size_t)y * dstride;
        for (uint32_t x = x0; x < x1; x++) {
            const uint32_t on = bits >> (7u - ((x + px) & 7u)) & 1u;
            if (op == MC_LIVADIX_BLT_FILL) put_pixel(drow, x, d, on ? fg : bg);
            else if (on) put_pixel(drow, x, d, get_pixel(drow, x, d) ^ fg);
        }
    }
}

static int blit(mc_livadix_blitter *b, uint8_t *vram, uint32_t vram_size, uint64_t now) {
    const uint32_t op = BLT(MC_LIVADIX_REG_BLT_OP), d = BLT(MC_LIVADIX_REG_BLT_DEPTH);
    const uint32_t w = BLT(MC_LIVADIX_REG_BLT_W), h = BLT(MC_LIVADIX_REG_BLT_H);
    const uint32_t dst = BLT(MC_LIVADIX_REG_BLT_DST), dstride = BLT(MC_LIVADIX_REG_BLT_DST_STRIDE);
    const uint32_t dx = BLT(MC_LIVADIX_REG_BLT_DX), dy = BLT(MC_LIVADIX_REG_BLT_DY);
    if (!b->accel || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32) || op < MC_LIVADIX_BLT_FILL ||
        op > MC_LIVADIX_BLT_COPY || !w || !h || w > 0x8000u || h > 0x8000u || b->clips > MC_LIVADIX_BLT_CLIPS ||
        !inside(vram_size, dst, dstride, dx, dy, w, h, d))
        return 0;
    size_t sbytes = 0;
    if (op == MC_LIVADIX_BLT_COPY) {
        /* The whole source first, as through a buffer: overlapping
         * rectangles and clip lists in any order copy as if at once. */
        const uint32_t src = BLT(MC_LIVADIX_REG_BLT_SRC), sstride = BLT(MC_LIVADIX_REG_BLT_SRC_STRIDE);
        const uint32_t sx = BLT(MC_LIVADIX_REG_BLT_SX), sy = BLT(MC_LIVADIX_REG_BLT_SY);
        if (!inside(vram_size, src, sstride, sx, sy, w, h, d)) return 0;
        const uint32_t first = sx * d / 8u, last = ((sx + w) * d + 7u) / 8u;
        sbytes = last - first;
        if ((size_t)h * sbytes > b->scratch_size) {
            uint8_t *s = realloc(b->scratch, (size_t)h * sbytes);
            if (!s) return 0;
            b->scratch = s;
            b->scratch_size = (size_t)h * sbytes;
        }
        for (uint32_t y = 0; y < h; y++) memcpy(b->scratch + (size_t)y * sbytes, vram + src + (size_t)(sy + y) * sstride + first, sbytes);
    }
    /* Each clip rectangle met, or the whole rectangle. */
    uint64_t pixels = 0;
    const unsigned n = b->clips ? b->clips : 1u;
    for (unsigned k = 0; k < n; k++) {
        uint32_t x0 = dx, y0 = dy, x1 = dx + w, y1 = dy + h;
        if (b->clips) {
            const uint32_t t = b->clip[k][0] >> 16, l = b->clip[k][0] & 0xFFFFu, bt = b->clip[k][1] >> 16, r = b->clip[k][1] & 0xFFFFu;
            if (t > y0) y0 = t;
            if (l > x0) x0 = l;
            if (bt < y1) y1 = bt;
            if (r < x1) x1 = r;
            if (x0 >= x1 || y0 >= y1) continue;
        }
        blit_part(b, vram, op, d, x0, y0, x1, y1, sbytes);
        pixels += (uint64_t)(x1 - x0) * (y1 - y0);
    }
    b->count++;
    /* The engine's time: 8 bytes of the destination (and of the source, or
     * read back) per VRAM cycle, and the clip rectangles it walks. */
    const uint64_t bytes = (pixels * d + 7u) / 8u;
    const uint64_t cycles = (bytes + 7u) / 8u * (op == MC_LIVADIX_BLT_FILL ? 1u : 2u);
    const uint64_t ns = MC_LIVADIX_BLT_START_NS + cycles * MC_LIVADIX_BLT_CYCLE_NS + (uint64_t)b->clips * MC_LIVADIX_BLT_CLIP_NS;
    b->done = (b->done > now ? b->done : now) + ns * MC_C15M_HZ / 1000000000u + 1u;
    return 1;
}

void mc_livadix_blitter_init(mc_livadix_blitter *b, int accel) {
    memset(b, 0, sizeof *b);
    b->accel = accel != 0;
}

void mc_livadix_blitter_free(mc_livadix_blitter *b) {
    free(b->scratch);
    b->scratch = NULL;
    b->scratch_size = 0;
}

void mc_livadix_blitter_reset(mc_livadix_blitter *b) {
    memset(b->reg, 0, sizeof b->reg);
    memset(b->clip, 0, sizeof b->clip);
    b->clips = 0;
    b->done = 0;
}

int mc_livadix_blitter_reg(uint32_t r) {
    return (r >= MC_LIVADIX_REG_BLT_CAPS && r <= MC_LIVADIX_REG_BLT_CLIPS) ||
           (r >= MC_LIVADIX_REG_CLIP && r < MC_LIVADIX_REG_CLIP + 8u * MC_LIVADIX_BLT_CLIPS);
}

uint32_t mc_livadix_blitter_read(const mc_livadix_blitter *b, uint32_t r, uint64_t now) {
    switch (r) {
    case MC_LIVADIX_REG_BLT_CAPS: return b->accel ? 0x101u : 0u;
    case MC_LIVADIX_REG_BLT_STATUS: return b->done > now;
    case MC_LIVADIX_REG_BLT_COUNT: return b->count;
    case MC_LIVADIX_REG_BLT_CLIPS: return b->clips;
    default:
        if (r >= MC_LIVADIX_REG_BLT_OP && r < MC_LIVADIX_REG_BLT_GO) return BLT(r);
        if (r >= MC_LIVADIX_REG_CLIP && r < MC_LIVADIX_REG_CLIP + 8u * MC_LIVADIX_BLT_CLIPS) return b->clip[(r - MC_LIVADIX_REG_CLIP) / 8u][(r >> 2) & 1u];
        return 0;
    }
}

int mc_livadix_blitter_write(mc_livadix_blitter *b, uint32_t r, uint32_t v, uint8_t *vram, uint32_t vram_size, uint64_t now) {
    if (r == MC_LIVADIX_REG_BLT_GO) return blit(b, vram, vram_size, now);
    if (r == MC_LIVADIX_REG_BLT_CLIPS) b->clips = v;
    else if (r >= MC_LIVADIX_REG_BLT_OP && r < MC_LIVADIX_REG_BLT_GO) BLT(r) = v;
    else if (r >= MC_LIVADIX_REG_CLIP && r < MC_LIVADIX_REG_CLIP + 8u * MC_LIVADIX_BLT_CLIPS)
        b->clip[(r - MC_LIVADIX_REG_CLIP) / 8u][(r >> 2) & 1u] = v;
    return 0;
}

static uint32_t reg_read(mc_livadix_card *c, uint32_t r) {
    if (mc_livadix_blitter_reg(r)) return mc_livadix_blitter_read(&c->blt, r, mc_dev_now(&c->dev));
    switch (r) {
    case MC_LIVADIX_REG_ID: return MC_LIVADIX_CARD_ID;
    case MC_LIVADIX_REG_VERSION: return 1;
    case MC_LIVADIX_REG_VRAM: return c->vram_size;
    case MC_LIVADIX_REG_MONITOR: return c->monitor;
    case MC_LIVADIX_REG_MODES: return MC_LIVADIX_CARD_MODES;
    case MC_LIVADIX_REG_REFRESH: return MC_LIVADIX_CARD_REFRESH_MHZ;
    case MC_LIVADIX_REG_INIT_DEPTH: return c->init_depth;
    case MC_LIVADIX_REG_WIDTH: return c->width;
    case MC_LIVADIX_REG_HEIGHT: return c->height;
    case MC_LIVADIX_REG_DEPTH: return c->depth;
    case MC_LIVADIX_REG_STRIDE: return c->stride;
    case MC_LIVADIX_REG_BASE: return c->base;
    case MC_LIVADIX_REG_CONTROL: return c->control;
    case MC_LIVADIX_REG_STATUS: return c->status;
    case MC_LIVADIX_REG_CLUT_INDEX: return c->clut_index;
    case MC_LIVADIX_REG_CLUT_DATA: {
        const uint8_t v = c->clut[c->clut_index][c->clut_part];
        if (++c->clut_part == 3) {
            c->clut_part = 0;
            c->clut_index++;
        }
        return v;
    }
    case MC_LIVADIX_REG_GAMMA_INDEX: return c->gamma_index;
    case MC_LIVADIX_REG_GAMMA_DATA: {
        const uint8_t v = c->gamma[c->gamma_part][c->gamma_index];
        if (++c->gamma_part == 3) {
            c->gamma_part = 0;
            c->gamma_index++;
        }
        return v;
    }
    default:
        if (r >= MC_LIVADIX_REG_RES && r < MC_LIVADIX_REG_RES + 4u * MC_LIVADIX_CARD_MODES) {
            const unsigned n = (r - MC_LIVADIX_REG_RES) / 4u;
            return (uint32_t)mc_livadix_card_modes[n][0] << 16 | mc_livadix_card_modes[n][1];
        }
        return 0;
    }
}

static void reg_write(mc_livadix_card *c, uint32_t r, uint32_t v) {
    if (mc_livadix_blitter_reg(r)) {
        if (mc_livadix_blitter_write(&c->blt, r, v, c->vram, c->vram_size, mc_dev_now(&c->dev))) schedule(c);
        return;
    }
    switch (r) {
    case MC_LIVADIX_REG_WIDTH: c->width = v & 0xFFFFu; break;
    case MC_LIVADIX_REG_HEIGHT: c->height = v & 0xFFFFu; break;
    case MC_LIVADIX_REG_DEPTH: c->depth = v & 0x3Fu; break;
    case MC_LIVADIX_REG_STRIDE: c->stride = v & 0xFFFFu; break;
    case MC_LIVADIX_REG_BASE: c->base = v & 0x00FFFFFFu; break;
    case MC_LIVADIX_REG_CONTROL: c->control = v & 3u; update_irq(c); break;
    case MC_LIVADIX_REG_STATUS: c->status &= ~v; update_irq(c); break;
    case MC_LIVADIX_REG_CLUT_INDEX: c->clut_index = (uint8_t)v; c->clut_part = 0; break;
    case MC_LIVADIX_REG_CLUT_DATA:
        c->clut[c->clut_index][c->clut_part] = (uint8_t)v;
        if (++c->clut_part == 3) {
            c->clut_part = 0;
            c->clut_index++;
        }
        break;
    case MC_LIVADIX_REG_GAMMA_INDEX: c->gamma_index = (uint8_t)v; c->gamma_part = 0; break;
    case MC_LIVADIX_REG_GAMMA_DATA:
        c->gamma[c->gamma_part][c->gamma_index] = (uint8_t)v;
        if (++c->gamma_part == 3) {
            c->gamma_part = 0;
            c->gamma_index++;
        }
        break;
    default: return;
    }
    c->dirty = 1;
}

static uint32_t card_read(void *dev, uint32_t offset, unsigned size) {
    mc_livadix_card *c = dev;
    const uint32_t v = reg_read(c, offset & 0xFFCu);
    if (size == 1) return (v >> (8u * (3u - (offset & 3u)))) & 0xFFu;
    if (size == 2) return (v >> (8u * (2u - (offset & 2u)))) & 0xFFFFu;
    return v;
}

static void card_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_livadix_card *c = dev;
    /* A narrower write drives its lanes, the others read as zero. */
    if (size == 1) value = (value & 0xFFu) << (8u * (3u - (offset & 3u)));
    else if (size == 2) value = (value & 0xFFFFu) << (8u * (2u - (offset & 2u)));
    reg_write(c, offset & 0xFFCu, value);
}

static void card_reset(void *dev) {
    mc_livadix_card *c = dev;
    mc_pci_config_init(&c->cfg, MC_LIVADIX_PCI_VENDOR, MC_LIVADIX_PCI_DEVICE, 0x01u, MC_LIVADIX_PCI_CLASS, 0x00u);
    c->cfg.reg[MC_PCI_INT_PIN] = 1u;
    mc_pci_config_bar(&c->cfg, 0, c->vram_size, 0);
    mc_pci_config_bar(&c->cfg, 1, 0x1000u, 0);
    mc_pci_config_rom(&c->cfg, MC_LIVADIX_CARD_ROM_WINDOW);
    c->width = mc_livadix_card_modes[c->monitor][0];
    c->height = mc_livadix_card_modes[c->monitor][1];
    c->depth = 1;
    c->stride = (c->width + 31u) / 32u * 4u;
    c->base = 0;
    c->control = MC_LIVADIX_CONTROL_DISPLAY;
    c->status = 0;
    c->clut_index = c->clut_part = c->gamma_index = c->gamma_part = 0;
    /* The CLUT and the gamma tables start as linear ramps. */
    for (unsigned i = 0; i < 256; i++)
        for (unsigned ch = 0; ch < 3; ch++) c->clut[i][ch] = c->gamma[ch][i] = (uint8_t)i;
    c->irq = 0;
    mc_dev_line(&c->dev, MC_LIVADIX_CARD_LINE_IRQ, 0);
    c->next_vbl = mc_dev_now(&c->dev) + c->period;
    mc_livadix_blitter_reset(&c->blt);
    schedule(c);
    c->dirty = 1;
}

/* Every register reads the same until a write or the card's event (the
 * vertical blank, the blitter finishing) but the data ports, which step
 * their index. */
static int card_stable(void *dev, uint32_t offset, unsigned size) {
    (void)dev;
    (void)size;
    const uint32_t r = offset & 0xFFCu;
    return r != MC_LIVADIX_REG_CLUT_DATA && r != MC_LIVADIX_REG_GAMMA_DATA;
}

const mc_dev_ops mc_livadix_card_ops = {card_read, card_write, card_event, card_reset, card_stable};

int mc_livadix_card_init(mc_livadix_card *c, const mc_hw_host *host, const char *name, uint32_t vram_size, unsigned monitor,
                      const uint8_t *rom, size_t rom_size, int accel) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_livadix_card_ops;
    c->dev.host = host;
    c->dev.name = name;
    if (monitor >= MC_LIVADIX_CARD_MODES || rom_size > MC_LIVADIX_CARD_ROM_WINDOW || !vram_size || (vram_size & 0xFFFu) ||
        vram_size > MC_LIVADIX_CARD_REGS_OFFSET)
        return 0;
    c->monitor = monitor;
    c->init_depth = 8;
    c->rom_size = rom_size;
    mc_livadix_blitter_init(&c->blt, accel);
    c->vram_size = vram_size;
    c->vram = calloc(1, vram_size);
    c->rom = calloc(1, MC_LIVADIX_CARD_ROM_WINDOW);
    c->direct = malloc((size_t)vram_size / 2u * 4u + 4u);
    if (!c->vram || !c->rom || !c->direct) {
        mc_livadix_card_free(c);
        return 0;
    }
    if (rom_size) memcpy(c->rom + MC_LIVADIX_CARD_ROM_WINDOW - rom_size, rom, rom_size);
    c->period = (uint64_t)MC_C15M_HZ * 1000u / MC_LIVADIX_CARD_REFRESH_MHZ;
    card_reset(c);
    return 1;
}

void mc_livadix_card_set_depth(mc_livadix_card *c, uint32_t depth) {
    if (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16 || depth == 32) c->init_depth = depth;
}

void mc_livadix_card_free(mc_livadix_card *c) {
    free(c->vram);
    free(c->rom);
    free(c->direct);
    mc_livadix_blitter_free(&c->blt);
    c->vram = c->rom = NULL;
    c->direct = NULL;
}

int mc_livadix_card_attach(mc_livadix_card *c, mc_nubus *nubus, unsigned slot) {
    return mc_nubus_fit(nubus, slot) &&
           mc_nubus_map_memory(nubus, slot, MC_LIVADIX_CARD_VRAM_OFFSET, c->vram_size, c->vram, 1) &&
           mc_nubus_map_device(nubus, slot, MC_LIVADIX_CARD_REGS_OFFSET, 0x1000u, c, &mc_livadix_card_ops) &&
           mc_nubus_map_memory(nubus, slot, MC_LIVADIX_CARD_ROM_OFFSET, MC_LIVADIX_CARD_ROM_WINDOW, c->rom, 0);
}

int mc_livadix_card_pci_decoding(const mc_livadix_card *c) {
    return (c->cfg.reg[MC_PCI_COMMAND] & MC_PCI_CMD_MEMORY) != 0;
}

static uint32_t fn_config_read(void *p, unsigned reg, unsigned size) {
    return mc_pci_config_read(&((mc_livadix_card *)p)->cfg, reg, size);
}
static void fn_config_write(void *p, unsigned reg, unsigned size, uint32_t value) {
    mc_pci_config_write(&((mc_livadix_card *)p)->cfg, reg, size, value);
}

static int fn_mem_read(void *p, uint32_t address, unsigned size, uint32_t *value) {
    mc_livadix_card *c = p;
    uint32_t off;
    if (mc_pci_config_hit(&c->cfg, 1, address, &off)) {
        *value = mc_pci_swap(card_read(c, off, size), size);
        return 1;
    }
    if (mc_pci_config_hit(&c->cfg, 0, address, &off)) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size && off + i < c->vram_size; i++) v |= (uint32_t)c->vram[off + i] << (8 * i);
        *value = v;
        return 1;
    }
    if (mc_pci_config_rom_hit(&c->cfg, address, &off)) {
        const uint8_t *rom = c->rom + MC_LIVADIX_CARD_ROM_WINDOW - c->rom_size;
        uint32_t v = 0;
        for (unsigned i = 0; i < size && off + i < c->rom_size; i++) v |= (uint32_t)rom[off + i] << (8 * i);
        *value = v;
        return 1;
    }
    return 0;
}

static int fn_mem_write(void *p, uint32_t address, unsigned size, uint32_t value) {
    mc_livadix_card *c = p;
    uint32_t off;
    if (mc_pci_config_hit(&c->cfg, 1, address, &off)) {
        card_write(c, off, size, mc_pci_swap(value, size));
        return 1;
    }
    if (mc_pci_config_hit(&c->cfg, 0, address, &off)) {
        for (unsigned i = 0; i < size && off + i < c->vram_size; i++) c->vram[off + i] = (uint8_t)(value >> (8 * i));
        return 1;
    }
    return 0;
}

mc_pci_function mc_livadix_card_function(mc_livadix_card *c) {
    return (mc_pci_function){c, fn_config_read, fn_config_write, fn_mem_read, fn_mem_write};
}

/* The CLUT through the gamma tables: the palette of the indexed depths and
 * each channel's table for the direct ones. */
static void compose(mc_livadix_card *c) {
    c->identity = 1;
    for (unsigned i = 0; i < 256; i++) {
        for (unsigned ch = 0; ch < 3; ch++) {
            c->lut[ch][i] = c->gamma[ch][c->clut[i][ch]];
            if (c->lut[ch][i] != i) c->identity = 0;
        }
        c->palette[i] = (uint32_t)c->lut[0][i] << 16 | (uint32_t)c->lut[1][i] << 8 | c->lut[2][i];
    }
    c->dirty = 0;
}

int mc_livadix_card_view_get(mc_livadix_card *c, mc_livadix_card_view *v) {
    const uint32_t d = c->depth;
    if (!(c->control & MC_LIVADIX_CONTROL_DISPLAY) || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 32) ||
        !c->width || !c->height || (uint64_t)c->width * d > (uint64_t)c->stride * 8u ||
        c->base + (uint64_t)c->stride * c->height > c->vram_size)
        return 0;
    if (c->dirty) compose(c);
    v->width = c->width;
    v->height = c->height;
    v->depth = d;
    v->row_bytes = c->stride;
    v->pixels = c->vram + c->base;
    v->palette = c->palette;
    if (d < 16 || c->identity) return 1;
    /* A direct depth through tables that change it: the picture converted
     * to x-R8-G8-B8. */
    uint8_t *out = (uint8_t *)c->direct;
    for (uint32_t y = 0; y < c->height; y++) {
        const uint8_t *row = c->vram + c->base + (size_t)y * c->stride;
        for (uint32_t x = 0; x < c->width; x++, out += 4) {
            unsigned r, g, b;
            if (d == 16) {
                const unsigned p = (unsigned)row[2 * x] << 8 | row[2 * x + 1];
                r = p >> 10 & 31u;
                g = p >> 5 & 31u;
                b = p & 31u;
                r = r << 3 | r >> 2;
                g = g << 3 | g >> 2;
                b = b << 3 | b >> 2;
            } else {
                r = row[4 * x + 1];
                g = row[4 * x + 2];
                b = row[4 * x + 3];
            }
            out[0] = 0;
            out[1] = c->lut[0][r];
            out[2] = c->lut[1][g];
            out[3] = c->lut[2][b];
        }
    }
    v->depth = 32;
    v->row_bytes = c->width * 4u;
    v->pixels = (const uint8_t *)c->direct;
    return 1;
}
