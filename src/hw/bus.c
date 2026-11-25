/* Livadix — the physical bus. Livadix's own code. */
#include "bus.h"

#include <stdlib.h>
#include <string.h>

int mc_bus_init(mc_bus *bus) {
    memset(bus, 0, sizeof *bus);
    bus->read_map = calloc(MC_BUS_PAGES, sizeof *bus->read_map);
    bus->write_map = calloc(MC_BUS_PAGES, sizeof *bus->write_map);
    bus->page_region = calloc(MC_BUS_PAGES, 1);
    bus->nregion = 1;   /* region 0: unmapped */
    if (!bus->read_map || !bus->write_map || !bus->page_region) {
        mc_bus_free(bus);
        return 0;
    }
    return 1;
}

void mc_bus_free(mc_bus *bus) {
    free(bus->read_map);
    free(bus->write_map);
    free(bus->page_region);
    bus->read_map = bus->write_map = NULL;
    bus->page_region = NULL;
}

static int new_region(mc_bus *bus, uint8_t kind, uint32_t base, uint64_t size, uint32_t mask) {
    if (bus->nregion >= MC_BUS_REGIONS) return 0;
    mc_bus_region *r = &bus->region[bus->nregion];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    r->base = base;
    r->size = size > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size;
    r->mask = mask;
    return (int)bus->nregion++;
}

int mc_bus_map_memory(mc_bus *bus, uint32_t base, uint32_t size, uint8_t *host, uint32_t host_size, int writable) {
    if (!host_size || (host_size & (MC_BUS_PAGE_SIZE - 1))) return 0;
    /* Memory pages share one region per kind: only the kind matters. */
    const uint8_t kind = writable ? MC_BUS_RAM : MC_BUS_ROM;
    int r = 0;
    for (unsigned i = 1; i < bus->nregion && !r; i++)
        if (bus->region[i].kind == kind) r = (int)i;
    if (!r) r = new_region(bus, kind, 0, 0, 0xFFFFFFFFu);
    if (!r) return 0;
    for (uint64_t off = 0; off < size; off += MC_BUS_PAGE_SIZE) {
        const uint32_t page = (uint32_t)((base + off) >> MC_BUS_PAGE_SHIFT);
        uint8_t *p = host + (off % host_size);
        bus->read_map[page] = p;
        bus->write_map[page] = writable ? p : NULL;
        bus->page_region[page] = (uint8_t)r;
    }
    bus->generation++;
    return r;
}

void mc_bus_map_float(mc_bus *bus, uint32_t base, uint64_t size) {
    int r = 0;
    for (unsigned i = 1; i < bus->nregion && !r; i++)
        if (bus->region[i].kind == MC_BUS_FLOAT) r = (int)i;
    if (!r) r = new_region(bus, MC_BUS_FLOAT, 0, 0, 0);
    if (r) mc_bus_remap(bus, r, base, size);
}

void mc_bus_unmap(mc_bus *bus, uint32_t base, uint64_t size) {
    for (uint64_t off = 0; off < size; off += MC_BUS_PAGE_SIZE) {
        const uint32_t page = (uint32_t)((base + off) >> MC_BUS_PAGE_SHIFT);
        bus->read_map[page] = bus->write_map[page] = NULL;
        bus->page_region[page] = 0;
    }
    bus->generation++;
}

void mc_bus_remap(mc_bus *bus, int region, uint32_t base, uint64_t size) {
    for (uint64_t off = 0; off < size; off += MC_BUS_PAGE_SIZE) {
        const uint32_t page = (uint32_t)((base + off) >> MC_BUS_PAGE_SHIFT);
        bus->read_map[page] = bus->write_map[page] = NULL;
        bus->page_region[page] = (uint8_t)region;
    }
    bus->generation++;
}

int mc_bus_add_io(mc_bus *bus, uint32_t base, uint64_t size, uint32_t mask, int unclaimed_ok) {
    const int r = new_region(bus, MC_BUS_IO, base, size, mask);
    if (!r) return 0;
    bus->region[r].unclaimed_ok = unclaimed_ok;
    mc_bus_remap(bus, r, base, size);
    return r;
}

int mc_bus_attach(mc_bus *bus, int region, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops, unsigned flags) {
    if (region <= 0 || (unsigned)region >= bus->nregion) return 0;
    mc_bus_region *r = &bus->region[region];
    if (r->nwin >= MC_BUS_WINDOWS) return 0;
    mc_bus_window *w = &r->win[r->nwin++];
    w->offset = offset;
    w->size = size;
    w->dev = dev;
    w->ops = ops;
    w->flags = flags;
    return 1;
}

static const mc_bus_window *window(const mc_bus_region *r, uint32_t offset) {
    /* The window of the last access first: devices are polled. */
    const mc_bus_window *last = &r->win[r->last];
    if (offset - last->offset < last->size && r->last < r->nwin) return last;
    for (unsigned i = 0; i < r->nwin; i++)
        if (offset - r->win[i].offset < r->win[i].size) {
            ((mc_bus_region *)r)->last = (uint8_t)i;
            return &r->win[i];
        }
    return NULL;
}

static uint32_t load(const uint8_t *p, unsigned size) {
    return size == 1 ? p[0] : size == 2 ? (uint32_t)p[0] << 8 | p[1]
                                        : (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

int mc_bus_peek(const mc_bus *bus, uint32_t a, unsigned size, uint32_t *value) {
    const uint8_t *p = bus->read_map[a >> MC_BUS_PAGE_SHIFT];
    if (!p) return 0;
    *value = load(p + (a & (MC_BUS_PAGE_SIZE - 1)), size);
    return 1;
}

/* Bus cycles an access makes: one, or one per byte on an 8-bit port. */
static unsigned cycles(const mc_bus_window *w, unsigned size) { return (w->flags & MC_WIN_PORT8) ? size : 1u; }

int mc_bus_read_stable(const mc_bus *bus, uint32_t a, unsigned size, uint64_t *cost) {
    const uint32_t page = a >> MC_BUS_PAGE_SHIFT;
    if (bus->read_map[page]) return 0;
    const unsigned ri = bus->page_region[page];
    const mc_bus_region *r = &bus->region[ri];
    if (!ri || r->kind != MC_BUS_IO) return 0;
    const uint32_t off = (a - r->base) & r->mask;
    const mc_bus_window *w = window(r, off);
    if (!w || (w->flags & MC_WIN_BUSERR) || !w->ops->stable) return 0;
    if (!w->ops->stable(w->dev, off - w->offset, size)) return 0;
    *cost = (uint64_t)r->cost * cycles(w, size);
    return 1;
}

static int io_read(mc_bus *bus, const mc_bus_region *r, uint32_t a, unsigned size, uint32_t *value) {
    const uint32_t off = (a - r->base) & r->mask;
    const mc_bus_window *w = window(r, off);
    if (!w) {
        if (!r->unclaimed_ok) return 1;
        *value = 0;
        return 0;
    }
    if (w->flags & MC_WIN_BUSERR) return 1;
    bus->cost_due += (uint64_t)r->cost * cycles(w, size);
    if ((w->flags & MC_WIN_PORT8) && size > 1) {
        uint32_t v = 0;
        for (unsigned i = 0; i < size; i++) v = v << 8 | (w->ops->read(w->dev, off - w->offset + i, 1) & 0xFFu);
        *value = v;
        return 0;
    }
    *value = w->ops->read(w->dev, off - w->offset, size);
    return 0;
}

static int io_write(mc_bus *bus, const mc_bus_region *r, uint32_t a, unsigned size, uint32_t value) {
    const uint32_t off = (a - r->base) & r->mask;
    const mc_bus_window *w = window(r, off);
    if (!w) return r->unclaimed_ok ? 0 : 1;
    if (w->flags & MC_WIN_BUSERR) return 1;
    bus->cost_due += (uint64_t)r->cost * cycles(w, size);
    if ((w->flags & MC_WIN_PORT8) && size > 1) {
        for (unsigned i = 0; i < size; i++)
            w->ops->write(w->dev, off - w->offset + i, 1, value >> (8u * (size - 1u - i)) & 0xFFu);
        return 0;
    }
    w->ops->write(w->dev, off - w->offset, size, value);
    return 0;
}

int mc_bus_read(mc_bus *bus, uint32_t a, unsigned size, uint32_t *value) {
    const uint32_t page = a >> MC_BUS_PAGE_SHIFT;
    int fault = 0;
    const uint8_t *p = bus->read_map[page];
    if (p) {
        *value = load(p + (a & (MC_BUS_PAGE_SIZE - 1)), size);
    } else {
        const unsigned ri = bus->page_region[page];
        const mc_bus_region *r = &bus->region[ri];
        if (ri && r->kind == MC_BUS_IO) fault = io_read(bus, r, a, size, value);
        else if (ri && r->kind == MC_BUS_FLOAT) *value = 0;
        else fault = 1;
        if (fault) *value = 0;
    }
    if (bus->observe && !p) bus->observe(bus->observe_ctx, a, size, 0, *value, fault);
    return fault;
}

int mc_bus_write(mc_bus *bus, uint32_t a, unsigned size, uint32_t value) {
    const uint32_t page = a >> MC_BUS_PAGE_SHIFT;
    int fault = 0;
    uint8_t *p = bus->write_map[page];
    const unsigned ri = bus->page_region[page];
    if (p) {
        p += a & (MC_BUS_PAGE_SIZE - 1);
        for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(value >> (8u * (size - 1u - i)));
        return 0;
    }
    const mc_bus_region *r = &bus->region[ri];
    if (ri && (r->kind == MC_BUS_ROM || r->kind == MC_BUS_FLOAT)) fault = 0;     /* ROM writes are ignored */
    else if (ri && r->kind == MC_BUS_IO) fault = io_write(bus, r, a, size, value);
    else fault = 1;
    if (bus->observe) bus->observe(bus->observe_ctx, a, size, 1, value, fault);
    return fault;
}
