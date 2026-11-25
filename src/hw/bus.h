/* Livadix — the physical bus.
 *
 * The physical address space is a table of 4 KiB pages. A page is
 *   - memory (RAM or ROM): a host pointer in the read table and, for
 *     writable memory, the write table, which the CPU uses directly (its
 *     fast path); a ROM page's write entry is NULL and a write to it is
 *     ignored, as on the hardware;
 *   - a device region: both entries NULL, so every access takes the slow
 *     path, which finds the device window by the region's offset (regions
 *     with sub-page device spacing, like the Quadra's I/O block, hold
 *     several windows and may repeat through their span with a mask);
 *   - unmapped: a bus error (the CPU builds its access-fault frame).
 *
 * Devices on an 8-bit port (the VIAs, the SCC...) see a word or longword
 * access as successive byte cycles, as the I/O bus adapter's dynamic bus
 * sizing presents them (Macintosh Quadra 700 Developer Note, "I/O bus
 * adapter ICs"). */
#ifndef MC_BUS_H
#define MC_BUS_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

#define MC_BUS_PAGE_SHIFT 12u
#define MC_BUS_PAGE_SIZE (1u << MC_BUS_PAGE_SHIFT)
#define MC_BUS_PAGES (1u << (32u - MC_BUS_PAGE_SHIFT))
#define MC_BUS_REGIONS 64
#define MC_BUS_WINDOWS 24

enum { MC_BUS_NONE, MC_BUS_RAM, MC_BUS_ROM, MC_BUS_IO, MC_BUS_FLOAT };

/* Window flags. */
enum {
    MC_WIN_PORT8 = 1,       /* 8-bit device: wider accesses become byte cycles */
    MC_WIN_BUSERR = 2       /* accesses here raise a bus error (an absent chip that does not answer) */
};

typedef struct mc_bus_window {
    uint32_t offset, size;      /* within the region, after its mask */
    void *dev;
    const mc_dev_ops *ops;
    unsigned flags;
} mc_bus_window;

typedef struct mc_bus_region {
    uint8_t kind;
    uint32_t base, size, mask;  /* device offset = (address - base) & mask */
    mc_bus_window win[MC_BUS_WINDOWS];
    unsigned nwin;
    int unclaimed_ok;           /* offsets in no window read 0 and ignore writes (else bus error) */
    uint16_t cost;              /* C15M periods each bus cycle to its devices takes (0: none) */
    uint8_t last;               /* the window of the last access (a lookup hint) */
} mc_bus_region;

typedef struct mc_bus {
    uint8_t **read_map, **write_map;    /* MC_BUS_PAGES entries each */
    uint8_t *page_region;               /* region index per page; 0 = unmapped */
    mc_bus_region region[MC_BUS_REGIONS];
    unsigned nregion;                   /* region 0 is "unmapped" */
    /* Changes whenever the page tables do (mc_bus_map_memory, _unmap,
     * _remap): the PowerPC translator's cached host pages follow it. */
    unsigned generation;
    /* Optional observer of slow-path accesses (traces). */
    void (*observe)(void *ctx, uint32_t address, unsigned size, int write, uint32_t value, int fault);
    void *observe_ctx;
    /* Device access time owed to guest time (C15M periods): the machine
     * takes it after each access (mc_bus_region.cost). */
    uint64_t cost_due;
} mc_bus;

int mc_bus_init(mc_bus *bus);
void mc_bus_free(mc_bus *bus);

/* Maps [base, base + size) to memory: page n of the span shows host page
 * n modulo host_size (so a bank aliases through its window). Writable
 * memory is RAM, the rest ROM. Returns the region index (0 on failure). */
int mc_bus_map_memory(mc_bus *bus, uint32_t base, uint32_t size, uint8_t *host, uint32_t host_size, int writable);
/* Makes [base, base + size) a floating bus: no device answers, yet the
 * cycle completes (reads 0, writes are lost) without a bus error. */
void mc_bus_map_float(mc_bus *bus, uint32_t base, uint64_t size);
/* Makes [base, base + size) unmapped (bus errors). */
void mc_bus_unmap(mc_bus *bus, uint32_t base, uint64_t size);
/* A device region over [base, base + size), offsets taken with `mask`. */
int mc_bus_add_io(mc_bus *bus, uint32_t base, uint64_t size, uint32_t mask, int unclaimed_ok);
/* A device window at region offset `offset`. */
int mc_bus_attach(mc_bus *bus, int region, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops, unsigned flags);
/* Points the pages of [base, base + size) at an existing region again
 * (after a remap). */
void mc_bus_remap(mc_bus *bus, int region, uint32_t base, uint64_t size);

/* The slow path: 0 on success, nonzero for a bus error. size is 1, 2 or 4
 * and the access does not cross a page. */
int mc_bus_read(mc_bus *bus, uint32_t address, unsigned size, uint32_t *value);
int mc_bus_write(mc_bus *bus, uint32_t address, unsigned size, uint32_t value);

/* A device read at `address` that would read the same and change nothing
 * until its chip's next event (mc_dev_ops.stable): 1 with the guest time
 * the read costs in *cost; 0 otherwise. */
int mc_bus_read_stable(const mc_bus *bus, uint32_t address, unsigned size, uint64_t *cost);

/* Reads memory without side effects (no device is called); 0 when the
 * page is not memory. */
int mc_bus_peek(const mc_bus *bus, uint32_t address, unsigned size, uint32_t *value);

#endif
