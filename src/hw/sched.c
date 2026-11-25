/* Livadix — the device scheduler: a binary heap of per-device deadlines
 *. Livadix's own code. */
#include "sched.h"

#include <string.h>

void mc_sched_init(mc_sched *s) {
    memset(s, 0, sizeof *s);
}

int mc_sched_slot(const mc_sched *s, const void *dev) {
    for (unsigned i = 0; i < s->count; i++)
        if (s->entry[i].dev == dev) return (int)i;
    return -1;
}

int mc_sched_add(mc_sched *s, void *dev, mc_sched_fn fn) {
    int slot = mc_sched_slot(s, dev);
    if (slot >= 0) return slot;
    if (s->count >= MC_SCHED_MAX) return -1;
    mc_sched_entry *e = &s->entry[s->count];
    e->when = MC_NEVER;
    e->dev = dev;
    e->fn = fn;
    e->pos = -1;
    return (int)s->count++;
}

static uint64_t at(const mc_sched *s, unsigned pos) { return s->entry[s->heap[pos]].when; }

static void place(mc_sched *s, unsigned pos, int slot) {
    s->heap[pos] = slot;
    s->entry[slot].pos = (int)pos;
}

static void sift_up(mc_sched *s, unsigned pos) {
    const int slot = s->heap[pos];
    const uint64_t when = s->entry[slot].when;
    while (pos > 0) {
        const unsigned parent = (pos - 1) / 2;
        if (at(s, parent) <= when) break;
        place(s, pos, s->heap[parent]);
        pos = parent;
    }
    place(s, pos, slot);
}

static void sift_down(mc_sched *s, unsigned pos) {
    const int slot = s->heap[pos];
    const uint64_t when = s->entry[slot].when;
    for (;;) {
        unsigned child = 2 * pos + 1;
        if (child >= s->queued) break;
        if (child + 1 < s->queued && at(s, child + 1) < at(s, child)) child++;
        if (at(s, child) >= when) break;
        place(s, pos, s->heap[child]);
        pos = child;
    }
    place(s, pos, slot);
}

static void unqueue(mc_sched *s, int slot) {
    const int pos = s->entry[slot].pos;
    if (pos < 0) return;
    s->entry[slot].pos = -1;
    const unsigned last = --s->queued;
    if ((unsigned)pos == last) return;
    const int moved = s->heap[last];
    place(s, (unsigned)pos, moved);
    sift_down(s, (unsigned)pos);
    sift_up(s, (unsigned)s->entry[moved].pos);
}

void mc_sched_set(mc_sched *s, int slot, uint64_t when) {
    if (slot < 0 || (unsigned)slot >= s->count) return;
    mc_sched_entry *e = &s->entry[slot];
    /* The same deadline again: nothing moves (the heap's order among equal
     * deadlines then never depends on how often a device re-arms). */
    if (e->pos >= 0 && e->when == when) return;
    if (e->pos >= 0) unqueue(s, slot);
    e->when = when;
    if (when == MC_NEVER) return;
    const unsigned pos = s->queued++;
    place(s, pos, slot);
    sift_up(s, pos);
}

unsigned mc_sched_run(mc_sched *s, uint64_t now) {
    unsigned calls = 0;
    while (s->queued && at(s, 0) <= now) {
        const int slot = s->heap[0];
        mc_sched_entry *e = &s->entry[slot];
        const uint64_t when = e->when;
        unqueue(s, slot);
        e->when = MC_NEVER;
        e->fn(e->dev, when);
        calls++;
    }
    return calls;
}
