/* Livadix — the device scheduler: one deadline per
 * device, kept in a binary heap ordered by time. The run loop shortens a CPU
 * slice to the earliest deadline; nothing polls per instruction.
 *
 * Time is in C15M periods (hw.h). */
#ifndef MC_SCHED_H
#define MC_SCHED_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

#define MC_SCHED_MAX 32

typedef void (*mc_sched_fn)(void *dev, uint64_t now);

typedef struct mc_sched_entry {
    uint64_t when;      /* MC_NEVER when idle */
    void *dev;
    mc_sched_fn fn;
    int pos;            /* index in the heap, -1 when not queued */
} mc_sched_entry;

typedef struct mc_sched {
    mc_sched_entry entry[MC_SCHED_MAX];
    int heap[MC_SCHED_MAX];   /* entry indices, earliest first */
    unsigned count;           /* registered entries */
    unsigned queued;          /* entries in the heap */
} mc_sched;

void mc_sched_init(mc_sched *s);
/* Registers a device and the function its deadline calls; returns its slot
 * (-1 when full). A device registered twice keeps its first slot. */
int mc_sched_add(mc_sched *s, void *dev, mc_sched_fn fn);
/* The slot of a registered device, -1 when unknown. */
int mc_sched_slot(const mc_sched *s, const void *dev);
/* Arms (or with MC_NEVER cancels) a slot's deadline. */
void mc_sched_set(mc_sched *s, int slot, uint64_t when);
/* The earliest deadline, MC_NEVER when none. */
static inline uint64_t mc_sched_next(const mc_sched *s) {
    return s->queued ? s->entry[s->heap[0]].when : MC_NEVER;
}
/* Calls, in time order, every deadline at or before `now`; each is
 * disarmed before its function runs (which may arm it again). Returns the
 * number of calls. */
unsigned mc_sched_run(mc_sched *s, uint64_t now);

#endif
