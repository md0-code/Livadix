/* A flat test bus for the CPU tests: RAM from address 0, every page mapped
 * for reading; writes take the slow path so they can be recorded. Addresses
 * are masked (24 bits for the 68000 suite), outside RAM is a bus error, and a
 * fault window can be armed to make any range bus-error. */
#ifndef MC_CPU_TEST_H
#define MC_CPU_TEST_H

#include "m68k.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct testbus {
    uint8_t *ram;
    uint32_t size;          /* power of two */
    uint32_t mask;          /* address mask applied before RAM decode */
    uint8_t **map;          /* read pages */
    uint8_t **wmap;         /* write pages (NULL entries when writes are tracked) */
    int track;              /* record written addresses */
    uint32_t *dirty;
    size_t ndirty, cap;
    uint32_t fault_lo, fault_hi;   /* [lo, hi) bus-errors when hi > lo */
    int fault_writes_only;
    unsigned resets;
    int iack;               /* what the interrupt acknowledge answers */
    unsigned last_fc;       /* function code of the last slow-path access */
    long reads, writes;     /* bus calls so far */
    long fail_read_at, fail_write_at;   /* refuse that read / write call (-1: none) */
    unsigned iack_level;
    int logging;            /* record every access's address (alog) and direction (awrite) */
    uint32_t *alog;
    uint8_t *awrite;
    size_t nalog, alogcap;
} testbus;

static inline void tb_log(testbus *b, uint32_t a, int write) {
    if (!b->logging) return;
    if (b->nalog == b->alogcap) {
        b->alogcap = b->alogcap ? b->alogcap * 2 : 256;
        b->alog = realloc(b->alog, b->alogcap * sizeof *b->alog);
        b->awrite = realloc(b->awrite, b->alogcap);
    }
    b->alog[b->nalog] = a;
    b->awrite[b->nalog++] = (uint8_t)write;
}

static inline int tb_faults(testbus *b, uint32_t a, int write) {
    if (b->fault_hi > b->fault_lo && a >= b->fault_lo && a < b->fault_hi) return !b->fault_writes_only || write;
    return 0;
}

static inline int tb_read(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t *v) {
    testbus *b = ctx;
    b->last_fc = fc;
    tb_log(b, a, 0);
    if (b->reads++ == b->fail_read_at) return 1;
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t x = (a + i) & b->mask;
        if (x >= b->size || tb_faults(b, a + i, 0)) return 1;
        r = (r << 8) | b->ram[x];
    }
    *v = r;
    return 0;
}

static inline int tb_write(void *ctx, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    testbus *b = ctx;
    b->last_fc = fc;
    tb_log(b, a, 1);
    if (b->writes++ == b->fail_write_at) return 1;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t x = (a + i) & b->mask;
        if (x >= b->size || tb_faults(b, a + i, 1)) return 1;
    }
    for (unsigned i = 0; i < size; i++) {
        const uint32_t x = (a + i) & b->mask;
        b->ram[x] = (uint8_t)(v >> (8u * (size - 1u - i)));
        if (b->track) {
            if (b->ndirty == b->cap) {
                b->cap = b->cap ? b->cap * 2 : 1024;
                b->dirty = realloc(b->dirty, b->cap * sizeof *b->dirty);
            }
            b->dirty[b->ndirty++] = x;
        }
    }
    return 0;
}

static inline int tb_iack(void *ctx, unsigned level) {
    testbus *b = ctx;
    b->iack_level = level;
    return b->iack;
}

static inline void tb_reset(void *ctx) { ((testbus *)ctx)->resets++; }

/* RAM of `size` bytes (a power of two, at least one page); `mask` applied to
 * every address; `track`: writes through the slow path and recorded. */
static inline void tb_init(testbus *b, uint32_t size, uint32_t mask, int track) {
    memset(b, 0, sizeof *b);
    b->ram = calloc(size, 1);
    b->size = size;
    b->mask = mask;
    b->track = track;
    b->iack = M68K_IACK_AUTOVECTOR;
    b->fail_read_at = b->fail_write_at = -1;
    b->map = calloc(M68K_PAGE_COUNT, sizeof *b->map);
    b->wmap = calloc(M68K_PAGE_COUNT, sizeof *b->wmap);
    for (uint64_t p = 0; p < M68K_PAGE_COUNT; p++) {
        const uint32_t a = ((uint32_t)p << M68K_PAGE_SHIFT) & mask;
        if (a < size) {
            b->map[p] = b->ram + a;
            if (!track) b->wmap[p] = b->ram + a;
        }
    }
}

static inline void tb_unmap(testbus *b, uint32_t lo, uint32_t hi) {
    for (uint32_t p = lo >> M68K_PAGE_SHIFT; p <= (hi - 1) >> M68K_PAGE_SHIFT; p++) b->map[p] = b->wmap[p] = NULL;
}

static inline void tb_free(testbus *b) {
    free(b->ram);
    free(b->map);
    free(b->wmap);
    free(b->dirty);
    free(b->alog);
    free(b->awrite);
}

static inline m68k_bus tb_bus(testbus *b) {
    m68k_bus bus;
    memset(&bus, 0, sizeof bus);
    bus.ctx = b;
    bus.read_map[0] = bus.read_map[1] = b->map;
    bus.write_map[0] = bus.write_map[1] = b->wmap;
    bus.read = tb_read;
    bus.write = tb_write;
    bus.iack = tb_iack;
    bus.reset = tb_reset;
    return bus;
}

static inline uint32_t tb_get(testbus *b, uint32_t a, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = (v << 8) | b->ram[(a + i) & b->mask & (b->size - 1)];
    return v;
}
static inline void tb_put(testbus *b, uint32_t a, unsigned size, uint32_t v) {
    for (unsigned i = 0; i < size; i++) b->ram[(a + i) & b->mask & (b->size - 1)] = (uint8_t)(v >> (8u * (size - 1u - i)));
}

int mc_cpu_unit(void);
int mc_cpu_sst(const char *dir, const char *only);
int mc_cpu_bench(double *mips);
int mc_cpu_fault_sweep(void);
int mc_cpu_mmu_unit(void);
int mc_cpu_68851_unit(void);
int mc_cpu_fpu_unit(void);
int mc_cpu_timing_unit(void);
int mc_cpu_fpu_eval(void);
int mc_cpu_jit_diff(unsigned cases, uint64_t seed, unsigned max_block);

#endif /* MC_CPU_TEST_H */
