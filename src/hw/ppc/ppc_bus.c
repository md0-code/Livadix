/* Livadix — devices on the PowerPC bus (ppc_machine.h).
 *
 * The 601's 64-bit data bus carries the byte at address n modulo 8 on byte
 * lane n, big-endian (MPC601UM 9.3.2.3, table 9-2); the
 * CPU core makes every access naturally aligned, of 1, 2 or 4 bytes (ppc.h),
 * so an access covers lanes [a mod 8, a mod 8 + size). A device answers in
 * one of three ways (mc_ppc_port):
 *
 *  - DIRECT: the chip takes the access as made (a 32-bit register file, or
 *    a chip that decodes its own widths);
 *  - SIZED: a bridge between the CPU bus and the chip's narrower port makes
 *    a wider access successive port-width cycles in address order, as the
 *    68040 machines' I/O bus adapters do (bus.h);
 *  - LANES: the chip is wired to some byte lanes only; an access gets the
 *    chip's bytes on the lanes they share and `fill` on the others, and a
 *    write reaches the chip with the shared lanes' bytes only.
 *
 * Livadix's own code. */
#include "ppc_machine.h"

#include <string.h>

static uint32_t sized_read(void *p, uint32_t offset, unsigned size) {
    const mc_ppc_adapter *a = p;
    uint32_t v = 0;
    if (size <= a->width) return a->ops->read(a->dev, offset, size);
    for (unsigned i = 0; i < size; i += a->width) {
        const uint32_t part = a->ops->read(a->dev, offset + i, a->width);
        v = a->width == 2 ? v << 16 | (part & 0xFFFFu) : v << 8 | (part & 0xFFu);
    }
    return v;
}

static void sized_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    const mc_ppc_adapter *a = p;
    if (size <= a->width) {
        a->ops->write(a->dev, offset, size, value);
        return;
    }
    for (unsigned i = 0; i < size; i += a->width) {
        const unsigned shift = 8u * (size - i - a->width);
        a->ops->write(a->dev, offset + i, a->width, value >> shift & (a->width == 2 ? 0xFFFFu : 0xFFu));
    }
}

static int sized_stable(void *p, uint32_t offset, unsigned size) {
    const mc_ppc_adapter *a = p;
    if (!a->ops->stable) return 0;
    for (unsigned i = 0; i < size; i += a->width)
        if (!a->ops->stable(a->dev, offset + i, size < a->width ? size : a->width)) return 0;
    return 1;
}

/* The lanes an access at `offset` of `size` bytes shares with the chip:
 * [*lo, *hi), and the shift of that run within the access's value. */
static int shared(const mc_ppc_adapter *a, uint32_t offset, unsigned size, unsigned *lo, unsigned *hi, unsigned *shift) {
    const unsigned first = offset & 7u, last = first + size, lane = a->lane, end = lane + a->width;
    *lo = first > lane ? first : lane;
    *hi = last < end ? last : end;
    *shift = 8u * (last - *hi);
    return *lo < *hi;
}

static uint32_t lanes_read(void *p, uint32_t offset, unsigned size) {
    const mc_ppc_adapter *a = p;
    unsigned lo, hi, shift;
    uint64_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | a->fill;
    if (!shared(a, offset, size, &lo, &hi, &shift)) return (uint32_t)v;
    const uint64_t mask = ((UINT64_C(1) << (8u * (hi - lo))) - 1u) << shift;
    const uint64_t part = a->ops->read(a->dev, (offset & ~7u) + lo, hi - lo);
    return (uint32_t)((v & ~mask) | (part << shift & mask));
}

static void lanes_write(void *p, uint32_t offset, unsigned size, uint32_t value) {
    const mc_ppc_adapter *a = p;
    unsigned lo, hi, shift;
    if (!shared(a, offset, size, &lo, &hi, &shift)) return;
    const uint32_t mask = (uint32_t)((UINT64_C(1) << (8u * (hi - lo))) - 1u);
    a->ops->write(a->dev, (offset & ~7u) + lo, hi - lo, value >> shift & mask);
}

static int lanes_stable(void *p, uint32_t offset, unsigned size) {
    const mc_ppc_adapter *a = p;
    unsigned lo, hi, shift;
    if (!shared(a, offset, size, &lo, &hi, &shift)) return 1;   /* nothing answers: nothing changes */
    return a->ops->stable && a->ops->stable(a->dev, (offset & ~7u) + lo, hi - lo);
}

static const mc_dev_ops sized_ops = {sized_read, sized_write, NULL, NULL, sized_stable};
static const mc_dev_ops lanes_ops = {lanes_read, lanes_write, NULL, NULL, lanes_stable};

int mc_ppc_attach(mc_ppc *m, int region, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops, mc_ppc_port port) {
    const unsigned w = port.width;
    if (!ops || !ops->read || !ops->write || (offset & 7u)) return 0;
    if (port.kind == MC_PPC_PORT_DIRECT) return mc_bus_attach(&m->bus, region, offset, size, dev, ops, 0);
    if (w != 1 && w != 2 && w != 4) return 0;
    if (port.kind == MC_PPC_PORT_SIZED) {
        if (w == 4) return mc_bus_attach(&m->bus, region, offset, size, dev, ops, 0);
        if (w == 1) return mc_bus_attach(&m->bus, region, offset, size, dev, ops, MC_WIN_PORT8);
    } else if (port.kind != MC_PPC_PORT_LANES || port.lane > 7u || port.lane % w) {
        return 0;
    }
    if (m->nadapter >= MC_PPC_ADAPTERS) return 0;
    mc_ppc_adapter *a = &m->adapter[m->nadapter];
    memset(a, 0, sizeof *a);
    a->dev = dev;
    a->ops = ops;
    a->kind = port.kind;
    a->width = (uint8_t)w;
    a->lane = port.lane;
    a->fill = port.fill;
    if (!mc_bus_attach(&m->bus, region, offset, size, a, port.kind == MC_PPC_PORT_SIZED ? &sized_ops : &lanes_ops, 0)) return 0;
    m->nadapter++;
    return 1;
}

int mc_ppc_dma(mc_ppc *m, uint32_t address, void *data, uint32_t length, int write) {
    uint8_t *p = data;
    int ok = 1;
    while (length && ok) {
        const uint32_t page = address >> MC_BUS_PAGE_SHIFT, offset = address & (MC_BUS_PAGE_SIZE - 1);
        uint32_t n = MC_BUS_PAGE_SIZE - offset;
        if (n > length) n = length;
        uint8_t *host = write ? m->bus.write_map[page] : (uint8_t *)m->bus.read_map[page];
        if (host) {
            if (write) {
                memcpy(host + offset, p, n);
                ppc_jit_written(m->jit, address, n);    /* translated code, page tables */
            } else memcpy(p, host + offset, n);
        } else {
            /* PCI bus masters issue aligned words to system-space I/O.
             * In particular a DBDMA STORE_QUAD must reach ChannelControl
             * or WaitSelect as one access (CHRP I/O 15.7.3). The port
             * adapter handles narrower peripherals and the bridge's lanes. */
            uint32_t k = 0;
            while (k < n && ok) {
                unsigned width = 1;
                if (!((address + k) & 3u) && n - k >= 4) width = 4;
                else if (!((address + k) & 1u) && n - k >= 2) width = 2;
                uint32_t v = 0;
                if (write) {
                    for (unsigned byte = 0; byte < width; byte++) v = (v << 8) | p[k + byte];
                    ok = !mc_bus_write(&m->bus, address + k, width, v);
                } else {
                    ok = !mc_bus_read(&m->bus, address + k, width, &v);
                    for (unsigned byte = 0; ok && byte < width; byte++)
                        p[k + byte] = (uint8_t)(v >> (8u * (width - 1u - byte)));
                }
                k += width;
            }
        }
        address += n;
        p += n;
        length -= n;
    }
    return ok;
}
