/* Livadix — Sonora (sonora.h).
 *
 * Livadix's own code. */
#include "sonora.h"

#include <string.h>

/* The 60.15 Hz tick to VIA1 CA1: the classic frame of 370
 * lines of 704 C15M periods, as on the IIci's RBV (rbv.c); the LC III's
 * 12-inch mode has the same 16.626 ms frame (LC III Developer Note,
 * Figure 1-3). */
#define TICK_PERIOD (704u * 370u)

typedef struct monitor_info {
    uint32_t width, height;
    uint8_t code;               /* standard sense code (7: extended) */
    int8_t ext;                 /* extended code (mc_sonora_sense); -1 none */
    uint32_t dots, lines, blank, clock;     /* dots per line, lines per frame, blanking lines, dot clock (Hz) */
} monitor_info;

/* LC III Developer Note, Table 1-6 (sense codes) and Figures 1-3 to 1-9
 * (timing: dots per line and lines per frame of each mode, its blanking
 * lines and dot clock). */
static const monitor_info monitors[MC_SONORA_MONITORS] = {
    /* No monitor: the timing still runs, at the mode the ROM programs
     * first (the 13-inch's, mode 6): the ROM waits for a frame before it
     * senses the monitor (ROM ECBBC41C offset $4484)†. */
    [MC_SONORA_NONE] = {0, 0, 7, 0x3F, 864, 525, 45, 30240000},
    [MC_SONORA_RGB12] = {512, 384, 2, -1, 640, 407, 23, 15667200},
    [MC_SONORA_HIRES13] = {640, 480, 6, -1, 864, 525, 45, 30240000},
    [MC_SONORA_PORTRAIT] = {640, 870, 1, -1, 832, 918, 48, 57283200},
    [MC_SONORA_VGA] = {640, 480, 7, 0x17, 800, 525, 45, 25175000},
    [MC_SONORA_RGB16] = {832, 624, 7, 0x2D, 1152, 667, 43, 57283200},
};

mc_sonora_monitor mc_sonora_monitor_for(uint32_t width, uint32_t height) {
    if (width == 512 && height == 384) return MC_SONORA_RGB12;
    if (width == 640 && height == 480) return MC_SONORA_HIRES13;
    if (width == 640 && height == 870) return MC_SONORA_PORTRAIT;
    if (width == 832 && height == 624) return MC_SONORA_RGB16;
    return MC_SONORA_NONE;
}

/* Table 1-4: every mode to 8 bits, 16 bits at 512 x 384 and 640 x 400;
 * the frame buffer must fit the VRAM (640 x 480 x 16 and 640 x 870 x 8
 * need the 768 KB). */
uint32_t mc_sonora_max_depth(mc_sonora_monitor monitor, uint32_t vram_size) {
    const monitor_info *m = &monitors[monitor];
    if (monitor == MC_SONORA_NONE) return 0;
    uint32_t best = 0;
    for (uint32_t d = 1; d <= 16; d <<= 1) {
        if (d == 16 && monitor != MC_SONORA_RGB12 && monitor != MC_SONORA_HIRES13 && monitor != MC_SONORA_VGA) break;
        if ((uint64_t)m->width * m->height * d / 8u <= vram_size) best = d;
    }
    return best;
}

/* --- sense lines ----------------------------------------------------------------- */

/* The levels on the three lines (Technical Note HW 30): a line Sonora
 * drives (its bit 0 in $28002) reads low; a monitor grounds the lines its
 * code has at 0; an extended-sense monitor (code 7) ties pairs of lines,
 * so driving one low pulls the other low where the pair's bit is 0. */
uint8_t mc_sonora_sense(const mc_sonora *s) {
    const monitor_info *m = &monitors[s->monitor];
    const uint8_t driven = (uint8_t)(~s->vsense & 7u);
    uint8_t out = 0;
    for (unsigned line = 0; line < 3; line++) {
        unsigned v = 1;
        if (driven >> line & 1u) v = 0;
        else if (!(m->code >> line & 1u)) v = 0;
        else if (m->ext >= 0) {
            for (unsigned low = 0; low < 3; low++) {
                if (!(driven >> low & 1u) || low == line) continue;
                /* The pair without line `low`, as the ROM's PrimaryInit
                 * assembles the code (ROM ECBBC41C offset $F9F10): (1,0)
                 * when 2 is low in bits 5-4, (2,0) when 1 is low in bits
                 * 3-2, (2,1) when 0 is low in bits 1-0; the higher-numbered
                 * line is the left bit. */
                const unsigned pair = (unsigned)m->ext >> (2u * low) & 3u;
                const unsigned other_hi = low == 2 ? 1u : 2u;
                const unsigned bit = line == other_hi ? (pair >> 1) & 1u : pair & 1u;
                if (!bit) v = 0;
            }
        }
        out |= (uint8_t)(v << line);
    }
    return out;
}

/* --- interrupts ------------------------------------------------------------------- */

static void update_irq(mc_sonora *s) {
    if (s->slots & s->sier & 0x7Fu) s->ifr |= MC_SONORA_IRQ_SLOT;
    else s->ifr = (uint8_t)(s->ifr & ~MC_SONORA_IRQ_SLOT);
    const uint8_t irq = (s->ifr & s->ier & 0x7Fu) != 0;
    if (irq != s->irq) {
        s->irq = irq;
        mc_dev_line(&s->dev, MC_SONORA_LINE_IRQ, irq);
    }
}

void mc_sonora_set_irq(mc_sonora *s, uint8_t bit, int asserted) {
    if (asserted) s->ifr |= bit;
    else s->ifr &= (uint8_t)~bit;
    update_irq(s);
}

void mc_sonora_set_slots(mc_sonora *s, uint8_t asserted, uint8_t mask) {
    s->slots = (uint8_t)((s->slots & ~mask) | (asserted & mask & 0x3Fu));
    update_irq(s);
}

/* --- time ------------------------------------------------------------------------- */

/* The video's line (slot flag bit 6, active low) is asserted through each
 * vertical blanking interval while the video runs: the ROM waits for a
 * frame by writing $40 to $02 and polling bit 6 low (ROM ECBBC41C offsets
 * $4484, $FDE80). */
static void timing(mc_sonora *s) {
    const monitor_info *m = &monitors[s->monitor];
    if (!m->clock) {
        s->frame = s->blank = 0;
        return;
    }
    s->frame = (uint64_t)MC_C15M_HZ * m->dots * m->lines / m->clock;
    s->blank = (uint64_t)MC_C15M_HZ * m->dots * m->blank / m->clock;
}

static int running(const mc_sonora *s) { return s->frame != 0; }

static void schedule(mc_sonora *s) {
    uint64_t next = s->tick_start + TICK_PERIOD / 2u;
    if (running(s)) {
        const uint64_t f = s->frame_start + (s->vbl ? s->blank : s->frame);
        if (f < next) next = f;
    }
    mc_dev_schedule(&s->dev, next);
}

static void advance(mc_sonora *s, uint64_t now) {
    if (now >= s->tick_start + TICK_PERIOD / 2u) {
        const uint64_t n = (now - s->tick_start) / (TICK_PERIOD / 2u);
        s->tick_start += n * (TICK_PERIOD / 2u);
        if (n & 1u) {
            s->tick ^= 1u;
            mc_dev_line(&s->dev, MC_SONORA_LINE_TICK, s->tick);
        }
    }
    if (running(s)) {
        s->frame_start += (now - s->frame_start) / s->frame * s->frame;
        const uint8_t vbl = (now - s->frame_start) < s->blank;
        if (vbl != s->vbl) {
            s->vbl = vbl;
            s->slots = (uint8_t)(vbl ? s->slots | 0x40u : s->slots & ~0x40u);
            update_irq(s);
        }
    }
}

static void sonora_event(void *dev, uint64_t now) {
    mc_sonora *s = dev;
    advance(s, now);
    schedule(s);
}

/* --- the pseudo-VIA2 -------------------------------------------------------------- */

static uint8_t read_reg(mc_sonora *s, uint32_t reg) {
    advance(s, mc_dev_now(&s->dev));
    switch (reg) {
    case MC_SONORA_DATAB: return s->datab;
    case MC_SONORA_RAMCFG: return s->ramcfg;
    /* Active low, as VIA2's port A (Guide, Table 4-9); bit 7 reads high. */
    case MC_SONORA_SIFR: return (uint8_t)(~s->slots | 0x80u);
    case MC_SONORA_IFR: {
        const uint8_t f = s->ifr & 0x7Fu;
        return (uint8_t)(f | ((f & s->ier) ? 0x80u : 0));
    }
    case MC_SONORA_VRAMCFG: return s->vramcfg;
    case MC_SONORA_SPEED: return s->speed;
    case MC_SONORA_MONP: return s->monp;
    case MC_SONORA_SIER: return (uint8_t)(s->sier | 0x80u);
    case MC_SONORA_IER: return (uint8_t)(s->ier | 0x80u);
    default: return 0;
    }
}

static void write_reg(mc_sonora *s, uint32_t reg, uint8_t v) {
    advance(s, mc_dev_now(&s->dev));
    switch (reg) {
    case MC_SONORA_DATAB: s->datab = v; break;
    /* The RAM configuration: the ROM writes $7F before it sizes memory and
     * the sizes it found after (ROM ECBBC41C offsets $A013A, $A03C0-$A0402:
     * bit 6 the board's RAM, bits 2-0 the SIMM, log2 of its megabytes plus
     * one, one or two banks alike). */
    case MC_SONORA_RAMCFG:
        s->ramcfg = v;
        if (s->remap) s->remap(s->remap_ctx);
        break;
    case MC_SONORA_SIFR: break;
    case MC_SONORA_IFR: s->ifr &= (uint8_t)~(v & 0x7Fu); break;
    case MC_SONORA_VRAMCFG: s->vramcfg = v; break;
    case MC_SONORA_SPEED: s->speed = v; break;
    case MC_SONORA_MONP: s->monp = v; break;
    /* The POST's register test (ROM ECBBC41C offset $48856, its masks at
     * $4892A): the slot enables are bits 6-3 (the video and slots $C-$E),
     * the enables bits 6-0. */
    case MC_SONORA_SIER:
        if (v & 0x80u) s->sier |= v & 0x78u;
        else s->sier &= (uint8_t)~(v & 0x78u);
        break;
    case MC_SONORA_IER:
        if (v & 0x80u) s->ier |= v & 0x7Fu;
        else s->ier &= (uint8_t)~(v & 0x7Fu);
        break;
    default: break;
    }
    update_irq(s);
}

static uint32_t sonora_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | read_reg(dev, (offset + i) & 0x1Fu);
    return v;
}

static void sonora_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) write_reg(dev, (offset + i) & 0x1Fu, (uint8_t)(value >> (8 * (size - 1 - i))));
}

static void sonora_reset(void *dev) {
    mc_sonora *s = dev;
    s->ier = s->sier = 0;
    s->ifr &= MC_SONORA_IRQ_SCSI_DRQ | MC_SONORA_IRQ_SCSI | MC_SONORA_IRQ_SOUND;
    s->datab = 0xFF;
    s->monp = 0x40;
    /* After reset the board's RAM alone (the ROM's first write, $40). */
    s->ramcfg = 0x40;
    s->vramcfg = 0;
    s->vctl = 0;
    s->vdepth = 0;
    s->vsense = 7;
    if (s->remap) s->remap(s->remap_ctx);
    update_irq(s);
}

const mc_dev_ops mc_sonora_ops = {sonora_read, sonora_write, sonora_event, sonora_reset, NULL};

/* --- video registers ------------------------------------------------------------- */

static uint8_t video_reg_read(mc_sonora *s, uint32_t reg) {
    switch (reg) {
    case 0: return s->vctl;
    case 1: return s->vdepth;
    /* The sense lines in bits 6-4 (the ROM's PrimaryInit reads them after
     * driving $07, $03, $05 and $06, ROM ECBBC41C offsets $FA1D2, $F9F22). */
    case 2: return (uint8_t)(mc_sonora_sense(s) << 4 | (s->vsense & 7u));
    default: return 0;
    }
}

static void video_reg_write(mc_sonora *s, uint32_t reg, uint8_t v) {
    switch (reg) {
    case 0: s->vctl = v; break;
    case 1: s->vdepth = v & 7u; break;
    case 2: s->vsense = v & 7u; break;
    default: break;
    }
}

static uint32_t video_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | video_reg_read(dev, (offset + i) & 3u);
    return v;
}

static void video_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) video_reg_write(dev, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_sonora_video_ops = {video_read, video_write, NULL, NULL, NULL};

/* --- the CLUT DAC ------------------------------------------------------------------ */

static void palette_entry(mc_sonora *s, unsigned i) {
    s->palette[i] = 0xFF000000u | (uint32_t)s->clut[i][0] << 16 | (uint32_t)s->clut[i][1] << 8 | s->clut[i][2];
}

/* The address at $00, red, green and blue through $01 (the ROM's
 * PrimaryInit sets 1-bit white and black at addresses $7F and $FF, ROM
 * ECBBC41C offset $FA3CA), the depth in bits 2-0 of $02 (the driver's
 * SetMode, $FDEB0). */
static uint8_t dac_reg_read(mc_sonora *s, uint32_t reg) {
    switch (reg) {
    case 0: return s->dac_addr;
    case 1: {
        const uint8_t v = s->clut[s->dac_addr][s->dac_rgb];
        if (++s->dac_rgb == 3) {
            s->dac_rgb = 0;
            s->dac_addr++;
        }
        return v;
    }
    case 2: return s->dac_ctl;
    default: return 0;
    }
}

static void dac_reg_write(mc_sonora *s, uint32_t reg, uint8_t v) {
    switch (reg) {
    case 0: s->dac_addr = v; s->dac_rgb = 0; break;
    case 1:
        s->clut[s->dac_addr][s->dac_rgb] = v;
        if (++s->dac_rgb == 3) {
            palette_entry(s, s->dac_addr);
            s->dac_rgb = 0;
            s->dac_addr++;
        }
        break;
    case 2: s->dac_ctl = v; break;
    default: break;
    }
}

static uint32_t dac_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | dac_reg_read(dev, (offset + i) & 3u);
    return v;
}

static void dac_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) dac_reg_write(dev, (offset + i) & 3u, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_sonora_dac_ops = {dac_read, dac_write, NULL, NULL, NULL};

/* --- configuration bytes ------------------------------------------------------------ */

static uint32_t cfg_read(void *dev, uint32_t offset, unsigned size) {
    const mc_sonora *s = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | s->cfg[(offset + i) & 1u];
    return v;
}

static void cfg_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_sonora *s = dev;
    for (unsigned i = 0; i < size; i++) s->cfg[(offset + i) & 1u] = (uint8_t)(value >> (8 * (size - 1 - i)));
}

const mc_dev_ops mc_sonora_cfg_ops = {cfg_read, cfg_write, NULL, NULL, NULL};

/* --- RAM ----------------------------------------------------------------------------- */

void mc_sonora_ram_layout(const mc_sonora *s, uint32_t board_size, uint32_t simm_size,
                          void (*map)(void *ctx, uint32_t base, uint32_t window, uint32_t offset, uint32_t size), void *ctx) {
    const unsigned code = s->ramcfg & 7u;
    /* A SIMM of 2, 8 or 32 MB has two banks (LC III Developer Note, Table
     * 1-2). */
    const uint32_t mb = simm_size >> 20;
    const uint32_t bank1 = (mb == 2 || mb == 8 || mb == 32) ? simm_size / 2u : simm_size;
    if (code == 7) {
        if (s->ramcfg & 0x40u) map(ctx, 0, 1u << 24, 0, board_size);
        if (bank1) map(ctx, 1u << 24, 1u << 24, board_size, bank1);
        if (simm_size > bank1) map(ctx, 2u << 24, 1u << 24, board_size + bank1, simm_size - bank1);
        return;
    }
    uint32_t at = 0;
    if (s->ramcfg & 0x40u) {
        map(ctx, 0, board_size, 0, board_size);
        at = board_size;
    }
    if (code && simm_size) map(ctx, at, 1u << (19 + code), board_size, simm_size);
}

/* --- setup and view ------------------------------------------------------------- */

void mc_sonora_init(mc_sonora *s, const mc_hw_host *host, const char *name, mc_sonora_monitor monitor, uint8_t *vram,
                    uint32_t vram_size, void (*remap)(void *ctx), void *remap_ctx) {
    memset(s, 0, sizeof *s);
    s->dev.ops = &mc_sonora_ops;
    s->dev.host = host;
    s->dev.name = name;
    s->monitor = monitor;
    s->vram = vram;
    s->vram_size = vram_size;
    timing(s);
    for (unsigned i = 0; i < 256; i++) palette_entry(s, i);
    sonora_reset(s);
    s->remap = remap;
    s->remap_ctx = remap_ctx;
    const uint64_t now = mc_dev_now(&s->dev);
    s->tick_start = s->frame_start = now;
    schedule(s);
}

int mc_sonora_view_get(mc_sonora *s, mc_sonora_view *view) {
    const monitor_info *m = &monitors[s->monitor];
    if (!running(s) || s->monitor == MC_SONORA_NONE || (s->vctl & 0x80u) || s->vdepth > 4) return 0;
    const uint32_t depth = 1u << s->vdepth;
    view->width = m->width;
    view->height = m->height;
    view->depth = depth;
    view->row_bytes = m->width * depth / 8u;
    if ((uint64_t)view->row_bytes * view->height > s->vram_size) return 0;
    /* A pixel of d bits reaches the CLUT in its top bits, the rest set. */
    if (depth <= 8) {
        const unsigned shift = 8u - depth, low = (1u << shift) - 1u;
        for (unsigned i = 0; i < (1u << depth); i++) {
            uint32_t c = s->palette[(i << shift | low) & 0xFFu];
            /* The portrait display is monochrome: it shows the CLUT's blue
             * output, the channel the ROM's gray tables drive (as on the
             * IIci, rbv.c)†. */
            if (s->monitor == MC_SONORA_PORTRAIT) c = 0xFF000000u | (c & 0xFFu) * 0x010101u;
            s->view_palette[i] = c;
        }
    }
    view->pixels = s->vram;
    view->palette = s->view_palette;
    return 1;
}
