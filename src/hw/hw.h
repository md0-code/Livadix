/* Livadix — shared chip interface.
 *
 * Every chip model is a plain struct with an mc_dev_ops table. Chips never
 * call the bus, the scheduler or the interrupt router directly: they talk to
 * the machine through the mc_hw_host callbacks they are given at init, so a
 * chip can be unit-tested against a fake host.
 *
 * Time is counted in periods of the 15.6672 MHz master clock (C15M).
 */
#ifndef MC_HW_H
#define MC_HW_H

#include "../core/machine.h"
#include "drivesnd.h"

#include <stdint.h>

#define MC_NEVER          UINT64_MAX

typedef struct mc_dev_ops {
    uint32_t (*read)(void *dev, uint32_t offset, unsigned size);              /* size 1, 2 or 4 */
    void     (*write)(void *dev, uint32_t offset, unsigned size, uint32_t value);
    void     (*event)(void *dev, uint64_t now);                               /* the deadline set with schedule() arrived */
    void     (*reset)(void *dev);
    /* Optional: 1 when a read of `offset` now returns what the last read of
     * it returned and changes nothing, and keeps doing so until the chip's
     * next scheduled event or a write to it (a guest polling loop over such
     * reads can then be advanced to that event at once). */
    int      (*stable)(void *dev, uint32_t offset, unsigned size);
} mc_dev_ops;

/* What the machine provides to a chip. */
typedef struct mc_hw_host {
    void *ctx;
    /* Current guest time in C15M periods. */
    uint64_t (*now)(void *ctx);
    /* Arm the device's single deadline (absolute C15M time); MC_NEVER cancels.
       Re-arming replaces the previous deadline. */
    void (*schedule)(void *ctx, void *dev, uint64_t when);
    /* An output pin of the chip changed (IRQ, DRQ, CA1/CB1 sources...).
       `line` is chip-defined (see each chip's header); state 1 = asserted. */
    void (*line)(void *ctx, void *dev, int line, int state);
    /* Optional (NULL when the machine has no bus master): the chip as bus
       master reads (write 0) or writes (write 1) `length` bytes of physical
       memory at `address`, in address order. Returns 0 on a bus error (the
       bytes before it were transferred). */
    int (*dma)(void *ctx, uint32_t address, void *data, uint32_t length, int write);
    mc_drivesnd *drive_sound;           /* optional mechanical audio observer */
} mc_hw_host;

/* Common header embedded first in every chip struct. */
typedef struct mc_dev {
    const mc_dev_ops  *ops;
    const mc_hw_host  *host;
    const char        *name;
} mc_dev;

static inline void mc_dev_drive(const mc_dev *d, unsigned drive, unsigned event, uint64_t t, uint32_t a, uint32_t b) {
    if (d->host->drive_sound) mc_drivesnd_event(d->host->drive_sound, drive, event, t, a, b);
}

static inline uint64_t mc_dev_now(const mc_dev *d) { return d->host->now(d->host->ctx); }
static inline void mc_dev_schedule(mc_dev *d, uint64_t when) { d->host->schedule(d->host->ctx, d, when); }
static inline void mc_dev_line(mc_dev *d, int line, int state) { d->host->line(d->host->ctx, d, line, state); }
static inline int mc_dev_dma(mc_dev *d, uint32_t address, void *data, uint32_t length, int write) {
    return d->host->dma && d->host->dma(d->host->ctx, address, data, length, write);
}

#endif
