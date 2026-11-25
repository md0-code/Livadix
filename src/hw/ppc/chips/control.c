/* Livadix — Control, the 7500's and 8500's video controller (control.h).
 *
 * Livadix's own code. */
#include "control.h"

#include <stdlib.h>
#include <string.h>

static void set_line(mc_control_chip *c) {
    const int irq = c->vbl && (c->reg[MC_CONTROL_INT >> 4] & 4u) && !c->irq_ack;
    if (irq != c->irq) {
        c->irq = irq;
        mc_dev_line(&c->dev, MC_CONTROL_LINE_VBL, irq);
    }
}

static void control_event(void *p, uint64_t now) {
    mc_control_chip *c = p;
    if (!c->vbl) {
        c->vbl = 1;
        c->irq_ack = 0;
        c->reg[MC_CONTROL_STATUS >> 4] |= 4u;
        mc_dev_schedule(&c->dev, now + c->frame / 50u);
    } else {
        c->vbl = 0;
        c->reg[MC_CONTROL_STATUS >> 4] &= ~4u;
        mc_dev_schedule(&c->dev, now + c->frame - c->frame / 50u);
    }
    set_line(c);
}

void mc_control_reset(mc_control_chip *c) {
    /* PCI 2.0 6.2; the IDs from the record before the FCode (file $346624). */
    mc_pci_config_init(&c->cfg, 0x106Bu, 0x0003u, 0x00u, 0x038000u, 0x00u);
    mc_pci_config_writable(&c->cfg, MC_PCI_BAR0, 4);       /* written by the chaos node† */
    mc_pci_config_bar(&c->cfg, 1, MC_CONTROL_REGS, 0);
    mc_pci_config_bar(&c->cfg, 2, MC_CONTROL_APERTURE, 0);
    memset(c->reg, 0, sizeof c->reg);
    c->vbl = 0;
    c->irq_ack = 0;
    set_line(c);
    if (c->dev.host) mc_dev_schedule(&c->dev, mc_dev_now(&c->dev) + c->frame);
}

int mc_control_init(mc_control_chip *c, const mc_hw_host *host, const char *name, uint32_t vram_size) {
    memset(c, 0, sizeof *c);
    c->dev.ops = &mc_control_ops;
    c->dev.host = host;
    c->dev.name = name;
    if (vram_size != (2u << 20) && vram_size != (4u << 20)) return 0;
    c->vram = calloc(1, vram_size);
    if (!c->vram) return 0;
    c->vram_size = vram_size;
    c->frame = MC_C15M_HZ / 60u;
    mc_control_reset(c);
    return 1;
}

void mc_control_free(mc_control_chip *c) {
    free(c->vram);
    c->vram = NULL;
}

/* The sense lines (2-0) as the monitor answers the lines the register
 * pulls low (control.h, $150). */
static uint32_t sense_lines(const mc_control_chip *c) {
    const unsigned released = (c->reg[MC_CONTROL_SENSE >> 4] >> 3) & 7u, e = c->sense_ext;
    unsigned lines = c->sense & 7u;
    if (released == 7u) return lines;
    if (c->sense < 6) return lines & released;
    if (!(released & 4u)) lines = (e >> 4) & 3u;                       /* line 2 low: lines 1, 0 */
    else if (!(released & 2u)) lines = ((e >> 3) & 1u) << 2 | ((e >> 2) & 1u);   /* line 1 low: lines 2, 0 */
    else lines = (e & 3u) << 1;                                          /* line 0 low: lines 2, 1 */
    return lines & released;
}

void mc_control_set_monitor(mc_control_chip *c, uint8_t sense, uint8_t sense_ext) {
    c->sense = sense;
    c->sense_ext = sense_ext;
}

static uint32_t reg_read(void *p, uint32_t offset, unsigned size) {
    mc_control_chip *c = p;
    offset &= MC_CONTROL_REGS - 1u;
    if ((offset & 15u) >= 4) return 0;
    uint32_t r = c->reg[offset >> 4];
    if ((offset & ~15u) == MC_CONTROL_SENSE) r = (r & 0x3Fu) | sense_lines(c) << 6;
    const uint32_t v = r >> (8 * (offset & 3u));
    return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
}

static void reg_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    mc_control_chip *c = p;
    offset &= MC_CONTROL_REGS - 1u;
    if ((offset & 15u) >= 4) return;
    const unsigned r = offset >> 4, shift = 8 * (offset & 3u);
    const uint32_t m = (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1u) << shift;
    uint32_t v = (c->reg[r] & ~m) | ((value << shift) & m);
    if (r < 0x12) v &= 0xFFFu;                              /* the timing counters: 12 bits */
    if (r == MC_CONTROL_STATUS >> 4) return;                /* status: read only */
    c->reg[r] = v;
    if (r == MC_CONTROL_INT >> 4 && (v & 8u)) c->irq_ack = 1;
    if (r == MC_CONTROL_INT >> 4) set_line(c);
}

static void control_reset_op(void *p) { mc_control_reset(p); }

const mc_dev_ops mc_control_ops = {reg_read, reg_write, control_event, control_reset_op, NULL};

int mc_control_view_get(const mc_control_chip *c, mc_control_view *v) {
    const uint32_t h_end = c->reg[10], h_start = c->reg[11], v_end = c->reg[2], v_start = c->reg[3];
    const uint32_t stride = c->reg[MC_CONTROL_STRIDE >> 4];
    if (h_end <= h_start || v_end <= v_start || !stride) return 0;
    const uint32_t width = (h_end - h_start) * 2u, height = (v_end - v_start) / 2u;
    uint32_t depth = 0;
    for (uint32_t k = 1; k <= 2 && !depth; k++) {
        if (stride % k || stride / k <= 0x20u) continue;
        const uint32_t row = stride / k - 0x20u;
        if (row % width) continue;
        const uint32_t bpp = row / width;
        if (bpp == 1 || bpp == 2 || bpp == 4) depth = bpp * 8u;
    }
    if (!depth) return 0;
    const uint32_t row_bytes = width * depth / 8u + 0x20u;
    /* The picture starts $10 bytes into the frame buffer $130 points at:
     * the display driver's screen base is the aperture + $130's value +
     * $10 (its 640 x 480 set-up writes $200 and the 68k world's ScrnBase
     * reads $9080 0210)†. */
    /* With $160 bit 6 clear (the arrangement the 8500's driver sets, $19
     * where the 7500's sets $51) the driver draws at the aperture + $60 0000
     * beyond what $130 says: the picture starts there†. */
    const uint32_t bank = (c->reg[MC_CONTROL_MODE >> 4] & 0x40u) ? 0 : 0x600000u;
    const uint32_t start = (c->reg[MC_CONTROL_START >> 4] + 0x10u + bank) & (c->vram_size - 1u);
    if ((uint64_t)start + (uint64_t)row_bytes * height > c->vram_size) return 0;
    v->width = width;
    v->height = height;
    v->depth = depth;
    v->row_bytes = row_bytes;
    v->pixels = c->vram + start;
    return 1;
}

/* --- the function on Chaos's bus -------------------------------------------------- */

/* Control answers its BARs while its command register's memory bit is set
 * or its configuration register $10 holds anything: Open Firmware's chaos
 * node writes $0200 0000 there (the bytes 00 00 00 02) and then assigns
 * BARs $14 and $18 without ever writing the command register (ROM
 * 9630C68B; the run's configuration cycles)†. */
int mc_control_decoding(const mc_control_chip *c) {
    return (c->cfg.reg[MC_PCI_COMMAND] & MC_PCI_CMD_MEMORY) || mc_pci_config_read(&c->cfg, MC_PCI_BAR0, 4);
}

static int hit(const mc_control_chip *c, unsigned n, uint32_t address, uint32_t *offset) {
    if (!mc_control_decoding(c)) return 0;
    const uint32_t base = mc_pci_config_base(&c->cfg, n);
    if (!base || address - base >= c->cfg.bar_size[n]) return 0;
    *offset = address - base;
    return 1;
}

static uint32_t fn_config_read(void *p, unsigned reg, unsigned size) { return mc_pci_config_read(&((mc_control_chip *)p)->cfg, reg, size); }
static void fn_config_write(void *p, unsigned reg, unsigned size, uint32_t value) { mc_pci_config_write(&((mc_control_chip *)p)->cfg, reg, size, value); }

static int fn_mem_read(void *p, uint32_t address, unsigned size, uint32_t *value) {
    mc_control_chip *c = p;
    uint32_t off;
    if (hit(c, 1, address, &off)) { *value = reg_read(c, off, size); return 1; }
    if (!hit(c, 2, address, &off)) return 0;
    uint32_t v = 0;
    if (off >= MC_CONTROL_FB)                               /* bytes in address order, little-endian value */
        for (unsigned i = 0; i < size; i++) v |= (uint32_t)c->vram[(off - MC_CONTROL_FB + i) & (c->vram_size - 1u)] << (8 * i);
    *value = v;
    return 1;
}

static int fn_mem_write(void *p, uint32_t address, unsigned size, uint32_t value) {
    mc_control_chip *c = p;
    uint32_t off;
    if (hit(c, 1, address, &off)) { reg_write(c, off, size, value); return 1; }
    if (!hit(c, 2, address, &off)) return 0;
    if (off >= MC_CONTROL_FB)
        for (unsigned i = 0; i < size; i++) c->vram[(off - MC_CONTROL_FB + i) & (c->vram_size - 1u)] = (uint8_t)(value >> (8 * i));
    return 1;
}

mc_pci_function mc_control_function(mc_control_chip *c) {
    return (mc_pci_function){c, fn_config_read, fn_config_write, fn_mem_read, fn_mem_write};
}
