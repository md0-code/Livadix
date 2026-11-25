/* Livadix — the PowerPC-to-x64 translator (ppc_jit.c).
 *
 * The translator runs a ppc_cpu exactly as ppc_step would, a step at a time
 * (a step is what one ppc_step call does: an instruction retired, an
 * exception taken, a fault): every register, memory byte, instruction and
 * step count is the interpreter's. The machine (src/hw/ppc) charges each
 * step's clocks as for the interpreter's. Livadix's own code. */
#ifndef MC_PPC_JIT_H
#define MC_PPC_JIT_H
#include "ppc.h"

typedef struct ppc_jit ppc_jit;

typedef struct ppc_jit_stats {
    uint64_t blocks, instructions;      /* translated */
    uint64_t helpers;                   /* translated instructions the interpreter runs (call-outs) */
    uint64_t entries;                   /* blocks entered from C (the rest chain through the dispatcher) */
    uint64_t steps;                     /* steps completed or folded inside the translator */
    uint64_t fills, flushes, invalidated, code_flushes, declines;
    uint64_t memory_slow, idle_steps, idle_probes;
    uint64_t cached_gpr_blocks, cached_gpr_uses; /* translated blocks / operands */
} ppc_jit_stats;

/* 1 on hosts with the translator (Windows x64). */
int ppc_jit_available(void);
/* A translator for `cpu`, whose physical pages are bus.h's tables:
 * read_map/write_map hold 2^20 host page pointers each (NULL: not memory,
 * a write_map entry NULL for ROM); `generation`, when not NULL, is a
 * counter that changes whenever the tables do. The translator observes
 * cpu->bus (it wraps the callbacks) to see code and page tables written and
 * device accesses. NULL when unavailable. */
ppc_jit *ppc_jit_create(ppc_cpu *cpu, uint8_t *const *read_map, uint8_t *const *write_map, const unsigned *generation);
void ppc_jit_destroy(ppc_jit *);
/* Runs at most `steps` steps of translated code; returns the steps run,
 * cpu->result being the last one's. 0: the next step is the
 * interpreter's (ppc_step) — a time base access (the machine brings the
 * time base up to date first), an interrupt to take, code outside memory,
 * a block longer than the budget, breakpoints, trace or little-endian
 * mode. A run ends after a step that accessed a device or ended in
 * checkstop or an unimplemented opcode, so the machine sees each device
 * access at its exact step. */
uint64_t ppc_jit_run(ppc_jit *, uint64_t steps);
/* Called before each device access during a run with the steps the run
 * made before the accessing step, so the machine's clock is that step's
 * as the device sees it (the interpreter counts a step after it). */
void ppc_jit_set_clock(ppc_jit *, void (*clock)(void *ctx, uint64_t steps), void *ctx);
/* Physical memory written by a bus master (DMA) or a debugger. */
void ppc_jit_written(ppc_jit *, uint32_t address, uint32_t length);
/* Blocks of at most `max_block` instructions (0: the default, 64): the
 * differential tests' variants. */
void ppc_jit_options(ppc_jit *, unsigned max_block);
/* Drops every translation and cached address translation. */
void ppc_jit_flush(ppc_jit *);
/* A board reset the CPU, possibly during a translated device access. */
void ppc_jit_cpu_reset(ppc_jit *);
void ppc_jit_get_stats(const ppc_jit *, ppc_jit_stats *);
/* Opt-in host profiling; timings never enter guest state. */
void ppc_jit_profile_enable(ppc_jit *);
/* Exact idle skipping (stationary loops proven on the reference CPU), bounded by the machine event budget. */
void ppc_jit_idle_enable(ppc_jit *, int enabled);
/* The 68k accelerator's (hw/ppc/m68k_accel.c). A run ends before the
 * instruction at base + 8n (n below size / 8), so its caller can take over
 * there, except where skip[n] is nonzero (skip may be NULL; it is kept, not
 * copied). ppc_jit_data is the translator's own data translation for the
 * current mode, as an instruction's access makes it: 1 with the host
 * address in *host, or NULL there for a device or for RAM whose PowerPC
 * writes are watched (translated PowerPC code, a page table; a page that is
 * only guarded is given: its guard is for PowerPC writes), and the
 * physical address in *pa; 0 when the translation is
 * refused (the CPU is left as it was, no exception taken). ppc_jit_sync
 * brings the translator's TLB up to the CPU's translation state first, as a
 * run does. ppc_jit_tlb_generation changes whenever host addresses it gave
 * may no longer hold (the translation state moved, a page became code or
 * a page table). */
void ppc_jit_trap(ppc_jit *, uint32_t base, uint32_t size, const uint8_t *skip);
int ppc_jit_data(ppc_jit *, uint32_t ea, int write, uint8_t **host, uint32_t *pa);
void ppc_jit_sync(ppc_jit *);
uint32_t ppc_jit_tlb_generation(const ppc_jit *);
/* On a CPU with a software-loaded TLB (the 603), an entry of its data TLB
 * that changes ends only the host addresses given for pages of that entry's
 * index (bits 12 to 16 of the address): not a move of the generation, but
 * a bit here, bit n for index n, returned and cleared. */
uint32_t ppc_jit_soft_dropped(ppc_jit *);
/* Whether a board reset the CPU (ppc_jit_cpu_reset) since the last call that
 * cleared it (clear nonzero). */
int ppc_jit_reset_seen(ppc_jit *, int clear);
/* A host page whose every write the caller is told of (the 68k accelerator's
 * translated 68k code): its writes leave the inline path, and `written` is
 * called with the page, the offset and the size of each, or with NULL when
 * the guards are lost (the bus remapped) and the caller must assume
 * everything written. */
typedef void (*ppc_jit_guard_fn)(void *ctx, uint8_t *host, uint32_t offset, uint32_t size);
/* `held` is called when a host page's writes leave the inline path (it
 * became translated code, a page table or guarded): a write address
 * ppc_jit_data gave for it no longer holds. With it set, that event does
 * not move ppc_jit_tlb_generation. */
typedef void (*ppc_jit_held_fn)(void *ctx, const uint8_t *host);
void ppc_jit_set_guard(ppc_jit *, ppc_jit_guard_fn written, ppc_jit_held_fn held, void *ctx);
void ppc_jit_guard(ppc_jit *, uint8_t *host);
void ppc_jit_unguard(ppc_jit *, uint8_t *host);
int ppc_jit_profile_write(const ppc_jit *, const char *path);
/* Time base ticks (ppc_clock) until the decrementer exception becomes
 * pending; UINT64_MAX when it already is. */
uint64_t ppc_decrementer_ticks(const ppc_cpu *);
/* One ppc_step through a one-instruction translation, its memory accesses
 * through the CPU's bus callbacks (the suites' translator mode). */
ppc_result ppc_jit_step(ppc_cpu *);
#endif
