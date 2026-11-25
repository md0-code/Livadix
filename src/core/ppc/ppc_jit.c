/* The PowerPC-to-x64 translator.
 *
 * Livadix's own design, after the 68k translator's model (src/core/cpu/
 * jit_x64.c). The instruction semantics are
 * those of Livadix's interpreter (ppc_cpu.c and its siblings, written from the
 * PowerPC 601 User's Manual, the Programming Environments Manual and the
 * 603e, 604e and 750 User's Manuals), which is the reference: translated
 * code leaves every register, memory byte, instruction and step count as
 * ppc_step would. Host encodings from the Intel 64 and IA-32 Architectures
 * Software Developer's Manual (x64emit.h, shared with the 68k translator).
 *
 * Shape:
 *   - Steps: a step is what one ppc_step call does (an instruction, an
 *     interrupt or fetch fault taken). The machine charges each its clocks;
 *     a run returns the steps it made.
 *   - Blocks: straight runs of up to 64 instructions inside one 4 KiB page,
 *     keyed by the logical PC and the host page they were read from; their
 *     entry checks the run's budget (r13) against their length, so a block
 *     runs only where the interpreter would run all of it. Conditional
 *     branches leave by side exits; unconditional branches, bclr and bcctr
 *     end a block. Exits go to a dispatcher in generated code: a
 *     direct-mapped table PC -> block, checked against the fetch TLB of the
 *     current mode (TLB-checked entry); a branch to its own block's start
 *     jumps to the entry.
 *   - State: the guest registers stay in the ppc_cpu structure (rbx), read
 *     and written by each translated instruction; CR and XER in their
 *     architected form.
 *   - Inline: the integer, rotate, logical, compare, CR and branch
 *     instructions, the moves of every SPR that is storage (spr_move),
 *     mfmsr, fmr/fneg/fabs/fnabs, and the loads and stores of 1, 2, 4 and
 *     (floating-point) 8 bytes, every form; lfs/stfs convert through the
 *     interpreter's own conversions. Call-outs that need no interpreter in
 *     their ordinary case: multiple and string transfers, dcbz and
 *     reservations on the host page the TLB maps (fast_*), floating-point
 *     arithmetic on host doubles (ppc_fpu_fast). Everything else is a
 *     call-out to the interpreter for that one instruction (ppc_execute):
 *     those instructions' other cases, the 601's POWER instructions, TLB
 *     operations, MSR and segment moves, traps taken and system calls.
 *   - Memory: a soft TLB per access kind (fetch, read, write) and mode
 *     (MSR[IR or DR], MSR[PR]), filled by the interpreter's own translation
 *     (ppc_translate) when an access misses: an aligned access whose page
 *     the TLB maps to host memory is made inline; anything else calls the
 *     interpreter for the instruction (faults, alignment, devices). A fill
 *     translates exactly as the instruction then does (the same R and C
 *     bits, the 603's TLB LRU, which inline accesses also keep).
 *   - Exactness of the cache: the TLB is flushed when the segment
 *     registers, BATs, SDR1 or HID0 change (compared after any MMU
 *     instruction and whenever the interpreter ran steps between runs), on
 *     any TLB instruction, and when a page the hardware table walk read is
 *     written (the page table: its pages are kept out of the inline write
 *     path); fills read the table through the observed bus, so those pages
 *     are known.
 *   - Devices: an access to a page that is not memory goes through the
 *     interpreter and the run ends after that step, so the machine takes the
 *     bus time, the time base and any interrupt or deadline the device
 *     changed at that exact step.
 *   - Self-modifying code (the PCI machines' dynamic-recompilation 68k
 *     emulator writes PowerPC code at run time): pages holding translated
 *     code are kept out of the inline write path; a write through the bus
 *     (the interpreter's stores, call-outs) or by DMA (ppc_jit_written) to
 *     words a block was translated from drops those blocks, and the block
 *     that wrote leaves at the next instruction. The interpreter is
 *     coherent (icbi and isync are no-ops), and so is this.
 *   - Time: the time base and decrementer are not touched by translated
 *     code; mfspr/mtspr of the time base, DEC and the 601's RTC, and mftb,
 *     are left to the interpreter after the machine brought the time base
 *     to that step, and the machine gives a run no more steps than the
 *     decrementer takes to become pending (ppc_decrementer_ticks).
 *   - Fallbacks: breakpoints armed (IABR, DABR, the 601's HID1/HID2/HID5),
 *     trace (MSR[SE], MSR[BE]) and little-endian mode run on the
 *     interpreter, as does code fetched from outside memory.
 *   - Idle skipping: a stationary loop is proven by executing it on the
 *     reference CPU from a state back to that state (every register, every
 *     RAM byte written restored; no device, no time register), and its
 *     whole recurrences inside the machine's next-event/DEC budget are
 *     skipped. A register-only cycle in real mode is learned, so later
 *     runs skip its turns from any of its states. Mac OS 9's
 *     register-rotating idle task is such a loop. */
#include "ppc_jit.h"
#include "ppc_model.h"
#include "ppc_fp_soft.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define MC_PPC_JIT 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../cpu/x64emit.h"
#define JIT_CALL
#define JIT_FP_TO_SINGLE ppc_fp_to_single
#define JIT_FP_FROM_SINGLE ppc_fp_from_single
#elif defined(__linux__) && defined(__x86_64__)
/* Linux x86-64: the generated code keeps the Windows x64 calling convention
 * (arguments in RCX RDX R8 R9, RBX RBP RSI RDI R12-R15 and XMM6-XMM15 kept
 * across a call, 32 bytes of shadow space); the C functions it calls are
 * compiled with that convention (GCC's ms_abi attribute) and the trampoline
 * is entered through it. Code memory comes from mmap (mmap(2)). */
#define MC_PPC_JIT 1
#include <sys/mman.h>
#include <time.h>
#include "../cpu/x64emit.h"
#define JIT_CALL __attribute__((ms_abi))
/* The instruction's extended opcode is `xo` here as in IBM's manuals, the
 * same word as the encoder's operand type (MSVC does not warn of it). */
#pragma GCC diagnostic ignored "-Wshadow"
#define JIT_FP_TO_SINGLE jit_fp_to_single
#define JIT_FP_FROM_SINGLE jit_fp_from_single
#elif (defined(_M_ARM64) || defined(__aarch64__)) && (defined(_WIN32) || defined(__linux__))
/* ARM64 (Windows, Linux): the same translator through a64emit.h, which
 * lowers each x86-64 operation to ARM64 instructions with the same effect;
 * the C functions are called with the platform's convention (AAPCS64). */
#define MC_PPC_JIT 1
#define MC_PPC_A64 1
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <time.h>
#endif
#include "../cpu/a64emit.h"
#define JIT_CALL
#ifdef __GNUC__
/* `xo` is both the extended opcode (IBM's name) and the encoder's operand type. */
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#ifdef _WIN32
#define JIT_FP_TO_SINGLE ppc_fp_to_single
#define JIT_FP_FROM_SINGLE ppc_fp_from_single
#else
#define JIT_FP_TO_SINGLE jit_fp_to_single
#define JIT_FP_FROM_SINGLE jit_fp_from_single
#endif
#else
#define MC_PPC_JIT 0
#endif
#ifndef MC_PPC_A64
#define MC_PPC_A64 0
/* A forward jump bound here (a64emit.h's x_bind also follows the flags). */
#define x_bind(b, at) x_patch((at), (b)->p)
#endif

uint64_t ppc_decrementer_ticks(const ppc_cpu *c)
{
    /* ppc_clock: the 601 counts 128 per tick (MPC601UM 2.2.5.3), the later
       processors one; the exception becomes pending when a delivery takes
       more than DEC holds. */
    if (c->decrementer_pending) return UINT64_MAX;
    return c->model == PPC_MODEL_601 ? (uint64_t)(c->dec / 128u) + 1u : (uint64_t)c->dec + 1u;
}

#if !MC_PPC_JIT

int ppc_jit_available(void) { return 0; }
ppc_jit *ppc_jit_create(ppc_cpu *cpu, uint8_t *const *r, uint8_t *const *w, const unsigned *g)
{
    (void)cpu; (void)r; (void)w; (void)g;
    return NULL;
}
void ppc_jit_destroy(ppc_jit *j) { (void)j; }
uint64_t ppc_jit_run(ppc_jit *j, uint64_t steps) { (void)j; (void)steps; return 0; }
void ppc_jit_written(ppc_jit *j, uint32_t a, uint32_t n) { (void)j; (void)a; (void)n; }
void ppc_jit_flush(ppc_jit *j) { (void)j; }
void ppc_jit_cpu_reset(ppc_jit *j) { (void)j; }
void ppc_jit_options(ppc_jit *j, unsigned max_block) { (void)j; (void)max_block; }
void ppc_jit_set_clock(ppc_jit *j, void (*clock)(void *ctx, uint64_t steps), void *ctx) { (void)j; (void)clock; (void)ctx; }
void ppc_jit_get_stats(const ppc_jit *j, ppc_jit_stats *s) { (void)j; memset(s, 0, sizeof *s); }
void ppc_jit_idle_enable(ppc_jit *j, int enabled) { (void)j; (void)enabled; }
void ppc_jit_trap(ppc_jit *j, uint32_t base, uint32_t size, const uint8_t *skip) { (void)j; (void)base; (void)size; (void)skip; }
int ppc_jit_data(ppc_jit *j, uint32_t ea, int write, uint8_t **host, uint32_t *pa) { (void)j; (void)ea; (void)write; (void)host; (void)pa; return 0; }
void ppc_jit_sync(ppc_jit *j) { (void)j; }
uint32_t ppc_jit_tlb_generation(const ppc_jit *j) { (void)j; return 0; }
uint32_t ppc_jit_soft_dropped(ppc_jit *j) { (void)j; return 0; }
int ppc_jit_reset_seen(ppc_jit *j, int clear) { (void)j; (void)clear; return 0; }
void ppc_jit_set_guard(ppc_jit *j, ppc_jit_guard_fn written, ppc_jit_held_fn held, void *ctx) { (void)j; (void)written; (void)held; (void)ctx; }
void ppc_jit_guard(ppc_jit *j, uint8_t *host) { (void)j; (void)host; }
void ppc_jit_unguard(ppc_jit *j, uint8_t *host) { (void)j; (void)host; }
void ppc_jit_profile_enable(ppc_jit *j) { (void)j; }
int ppc_jit_profile_write(const ppc_jit *j, const char *p) { (void)j; (void)p; return 0; }
ppc_result ppc_jit_step(ppc_cpu *c) { return ppc_step(c); }

#else

#define CODE_SIZE (64u << 20)
#if MC_PPC_A64
#define CODE_RESERVE (768u << 10)   /* room one block's translation may need (ARM64: about four times x86's) */
#else
#define CODE_RESERVE (192u << 10)   /* room one block's translation may need */
#endif
#define MAX_INSNS 64u
#define TLB_SIZE 512u               /* entries per set */
#define FAST_SIZE 32768u
#define HASH_SIZE 65536u
#define PAGE_SLOTS 65536u
#define MAX_BLOCKS 131072u
#define PAGE_MASK UINT32_C(0xfffff000)
#define IDLE_SLOTS 4096u            /* code pages whose candidate runs are counted */
#define IDLE_STEPS 4096u            /* the longest recurrence a probe looks for */
#define IDLE_BUDGET 4096u           /* runs shorter than this are not looked at */

enum { K_FETCH, K_READ, K_WRITE };
enum { EXIT_DISPATCH = 1, EXIT_BUDGET, EXIT_HELPER, EXIT_SYSTEM, EXIT_MODE };
enum { P_CODE = 1, P_WATCHED = 2, P_GUARDED = 4 };

/* A soft TLB entry (32 bytes, read by translated code): tag = the logical
 * page | 1 when the page is host memory taken inline, | 3 when known but not
 * inline (a device, or a page whose writes are watched); host = addend +
 * logical address. */
typedef struct jtlb {
    uint32_t tag;
    uint8_t lru_val, pad[3];
    uint8_t *lru_ptr;           /* the 603's TLB LRU byte the access sets, else a dummy */
    intptr_t addend;
    uint8_t *host;              /* the host page */
} jtlb;

/* The dispatcher's table (32 bytes): pc, its block's length and code, and
 * the fetch TLB's addend the block was translated under. */
typedef struct jfast {
    uint32_t pc, need;
    intptr_t addend;
    const uint8_t *code;
    uintptr_t pad;
} jfast;

typedef struct jblock {
    uint32_t pc;
    uint16_t first, count;      /* words [first, first + count) of its page */
    uint8_t *host;
    intptr_t addend;
    uint8_t *code;
    struct jblock *hnext, *pnext;
} jblock;

typedef struct jpage {
    uint8_t *host;
    unsigned flags;
    uint32_t words[32];         /* the words translated code was read from */
    jblock *blocks;
} jpage;

/* One step of a learned idle cycle: the registers before it, and the word
 * it executes with where that was read. */
typedef struct jcycle_step {
    uint32_t pc, cr, xer, lr, ctr, word;
    const uint8_t *at;
    uint32_t gpr[32];
} jcycle_step;

typedef struct snap { uint32_t sr[16], bat[8], dbat[8], sdr1, hid0; } snap;

typedef uint32_t (JIT_CALL *enter_fn)(ppc_cpu *, ppc_jit *, const uint8_t *, int64_t);

typedef struct jprofile {
    uint32_t pc, op;
    uint64_t calls, ticks;
    unsigned kind;
} jprofile;

struct ppc_jit {
    ppc_cpu *cpu;
    ppc_bus bus;                /* the machine's, observed */
    uint8_t *const *read_map, *const *write_map;
    const unsigned *generation;
    unsigned seen_generation;
    int wrapped, soft;
    ppc_model model;
    uint8_t *code, *code_start, *code_free, *code_end;
    enter_fn enter;
    const uint8_t *dispatch, *epilogue;
    jtlb *cur_fetch, *cur_read, *cur_write;     /* the current mode's sets (enter loads them) */
    int64_t left;               /* the budget at the last exit, or as of the step a call-out makes */
    int64_t give;               /* the budget the current block run started with */
    uint64_t run_base;          /* steps of this ppc_jit_run before it */
    void (*clock)(void *, uint64_t);
    void *clock_ctx;
    uint8_t io, smc, flushed, filling, outside, running, dummy, cpu_reset;
    uint8_t lm;                 /* the 601's HID0[LM] the code was translated under */
    uint32_t trap_base, trap_size;      /* runs end before base + 8n (ppc_jit_trap) */
    const uint8_t *trap_skip;           /* but not where skip[n] */
    uint32_t tlb_generation;            /* ppc_jit_tlb_generation */
    uint32_t soft_dropped;              /* ppc_jit_soft_dropped */
    uint32_t soft_seen;                 /* the CPU's tlb_writes when soft_tlb was taken */
    ppc_cpu undo;                       /* the CPU before a translation ppc_jit_data asked for */
    ppc_jit_guard_fn guard;             /* ppc_jit_set_guard */
    ppc_jit_held_fn held;
    void *guard_ctx;
    unsigned tlb_used;
    snap snap;
    ppc_tlb_entry soft_tlb[2][32][2];
    jblock *blocks;
    unsigned nblocks, npages, max_block;
    jpage **pages;
    ppc_jit_stats stats;
    jprofile *profile;
    unsigned profile_size, profile_used, profile_stalled;
    uint64_t profile_dropped;
    int idle_enabled, idle_learning, idle_reject;
    unsigned idle_bytes;
    struct { uint8_t *at; uint8_t value; } idle_writes[4096];
    uint32_t idle_pc[1024], idle_hits[1024];    /* runs starting at a branch to itself or a trap */
    uint32_t idle_seen[4096];   /* hashes of the registers quiet runs ended in */
    struct {
        uint32_t page;          /* the code page | 1 */
        uint16_t hits, wait;    /* candidates since the last probe; probed at every wait-th, 0: a probe paid */
        uint8_t armed;          /* a run from the page stayed in translated code */
    } idle[IDLE_SLOTS];
    unsigned idle_reads;        /* RAM reads of the current probe step (its fetch is one) */
    struct {
        unsigned period;        /* steps; 0: no cycle learned */
        uint32_t gprs;          /* the GPRs its instructions can name (bit r) */
        uint64_t instructions;  /* retired in one period */
        ppc_cpu base;           /* the state it was learned from (its MSR) */
        jcycle_step *step;      /* [IDLE_STEPS] */
    } cycle;
    uint64_t enter_ticks, compile_ticks, run_ticks;
    jblock *hash[HASH_SIZE];
    jtlb tlb[3][4][TLB_SIZE];
    uint32_t tlb_pa[3][4 * TLB_SIZE];   /* each entry's physical page */
    uint8_t tlb_wim[3][4 * TLB_SIZE];   /* and its translation's WIMG bits (mmu_wim) */
    jfast fast[FAST_SIZE];
};

#define CPU(f) XM(XBX, (int32_t)offsetof(ppc_cpu, f))
static xo gpr_operand(xbuf *, unsigned);
#define GPR(r) gpr_operand(b, (r))
#define FPR(r) XM(XBX, (int32_t)(offsetof(ppc_cpu, fpr) + 8u * (r)))
#define JIT(f) XM(XBP, (int32_t)offsetof(ppc_jit, f))

int ppc_jit_available(void) { return 1; }

#ifdef _WIN32
static uint64_t host_ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
#else
static uint64_t host_ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}
/* The single-precision conversions as the generated code calls them. */
static JIT_CALL uint32_t jit_fp_to_single(uint64_t bits) { return ppc_fp_to_single(bits); }
static JIT_CALL uint64_t jit_fp_from_single(uint32_t bits) { return ppc_fp_from_single(bits); }
#endif

static unsigned profile_hash(uint32_t pc, uint32_t op, unsigned kind, unsigned size)
{
    uint32_t key = pc ^ (op * 0x9e3779b1u) ^ (kind * 7919u);
    key = (key ^ (key >> 16)) * 0x85ebca6bu;
    return (key ^ (key >> 13)) & (size - 1u);
}

static int profile_grow(ppc_jit *j)
{
    const unsigned size = j->profile_size ? j->profile_size * 2u : 65536u;
    if (size <= j->profile_size) { j->profile_stalled = 1; return 0; }
    jprofile *rows = calloc(size, sizeof *rows);
    if (!rows) { j->profile_stalled = 1; return 0; }
    for (unsigned i = 0; i < j->profile_size; ++i) {
        const jprofile *p = &j->profile[i];
        if (!p->calls) continue;
        unsigned at = profile_hash(p->pc, p->op, p->kind, size);
        while (rows[at].calls) at = (at + 1u) & (size - 1u);
        rows[at] = *p;
    }
    free(j->profile); j->profile = rows; j->profile_size = size;
    return 1;
}

static void profile_add(ppc_jit *j, unsigned kind, uint32_t pc, uint32_t op, uint64_t start)
{
    if (!j->profile) return;
    if (!j->profile_stalled && j->profile_used >= j->profile_size * 3u / 4u) profile_grow(j);
    unsigned at = profile_hash(pc, op, kind, j->profile_size);
    for (unsigned n = 0; n < j->profile_size; ++n, at = (at + 1u) & (j->profile_size - 1u)) {
        jprofile *p = &j->profile[at];
        if (p->calls && (p->pc != pc || p->op != op || p->kind != kind)) continue;
        if (!p->calls) {
            if (j->profile_used >= j->profile_size * 3u / 4u) break;
            ++j->profile_used; p->pc = pc; p->op = op; p->kind = kind;
        }
        ++p->calls; p->ticks += host_ticks() - start;
        return;
    }
    ++j->profile_dropped;
}

void ppc_jit_profile_enable(ppc_jit *j)
{
    if (j && !j->profile) profile_grow(j);
}

static int profile_order(const void *a, const void *b)
{
    const jprofile *x = a, *y = b;
    return x->ticks < y->ticks ? 1 : x->ticks > y->ticks ? -1 : 0;
}

int ppc_jit_profile_write(const ppc_jit *j, const char *path)
{
#ifdef _WIN32
    LARGE_INTEGER hz;
#else
    const struct { uint64_t QuadPart; } hz = {1000000000u};
#endif
    if (!j || !j->profile) return 0;
    jprofile *rows = malloc(j->profile_size * sizeof *rows);
    if (!rows) return 0;
    memcpy(rows, j->profile, j->profile_size * sizeof *rows);
    qsort(rows, j->profile_size, sizeof *rows, profile_order);
    FILE *f = fopen(path, "w");
    if (!f) { free(rows); return 0; }
#ifdef _WIN32
    QueryPerformanceFrequency(&hz);
#endif
    fprintf(f, "run_s,enter_s,compile_s,steps,helpers,memory_slow,declines,entries\n");
    fprintf(f, "%.9f,%.9f,%.9f,%llu,%llu,%llu,%llu,%llu\n",
            (double)j->run_ticks / hz.QuadPart, (double)j->enter_ticks / hz.QuadPart,
            (double)j->compile_ticks / hz.QuadPart, (unsigned long long)j->stats.steps,
            (unsigned long long)j->stats.helpers, (unsigned long long)j->stats.memory_slow,
            (unsigned long long)j->stats.declines, (unsigned long long)j->stats.entries);
    fprintf(f, "kind,pc,opcode,calls,host_s\n");
    for (unsigned i = 0; i < j->profile_size; ++i) {
        const jprofile *p = &rows[i];
        if (!p->calls) continue;
        fprintf(f, "%s,%08X,%08X,%llu,%.9f\n", p->kind ? "memory" : "helper",
                p->pc, p->op, (unsigned long long)p->calls, (double)p->ticks / hz.QuadPart);
    }
    fprintf(f, "dropped,0,0,%llu,0\n", (unsigned long long)j->profile_dropped);
    free(rows);
    return fclose(f) == 0;
}

/* --- decoding ---------------------------------------------------------------------- */

/* Instructions that read or write the time base, DEC or the 601's RTC: the
 * interpreter runs them once the machine brought the time base to them. */
static int is_decline(uint32_t op)
{
    unsigned xo = (op >> 1) & 1023u, spr = ((op >> 16) & 31u) | ((op >> 6) & 992u);
    if (op >> 26 != 31) return 0;
    if (xo == 371) return 1;
    if (xo != 339 && xo != 467) return 0;
    switch (spr) {
    case 4: case 5: case 6: case 20: case 21: case 22: case 268: case 269: case 284: case 285: return 1;
    default: return 0;
    }
}

typedef struct meminfo { uint8_t size, store, update, indexed, sign, rev, fp; } meminfo;

/* The scalar loads and stores the interpreter's memory() makes (ppc_cpu.c). */
static int mem_decode(uint32_t op, meminfo *m)
{
    static const uint8_t sizes[24] = {4, 4, 1, 1, 4, 4, 1, 1, 2, 2, 2, 2, 2, 2, 0, 0, 4, 4, 8, 8, 4, 4, 8, 8};
    unsigned p = op >> 26, xo = (op >> 1) & 1023u;
    memset(m, 0, sizeof *m);
    if (p >= 32 && p <= 55 && p != 46 && p != 47) {
        m->size = sizes[p - 32];
        m->update = p & 1u;
        m->store = (p >= 36 && p <= 39) || p == 44 || p == 45 || p >= 52;
        m->sign = p == 42 || p == 43;
        m->fp = p >= 48;
        return 1;
    }
    if (p != 31) return 0;
    m->indexed = 1;
    switch (xo) {
    case 23: case 55: m->size = 4; m->update = xo == 55; return 1;
    case 87: case 119: m->size = 1; m->update = xo == 119; return 1;
    case 151: case 183: m->size = 4; m->store = 1; m->update = xo == 183; return 1;
    case 215: case 247: m->size = 1; m->store = 1; m->update = xo == 247; return 1;
    case 279: case 311: m->size = 2; m->update = xo == 311; return 1;
    case 343: case 375: m->size = 2; m->sign = 1; m->update = xo == 375; return 1;
    case 407: case 439: m->size = 2; m->store = 1; m->update = xo == 439; return 1;
    case 534: m->size = 4; m->rev = 1; return 1;
    case 790: m->size = 2; m->rev = 1; return 1;
    case 662: m->size = 4; m->rev = 1; m->store = 1; return 1;
    case 918: m->size = 2; m->rev = 1; m->store = 1; return 1;
    case 535: case 567: m->size = 4; m->fp = 1; m->update = xo == 567; return 1;
    case 599: case 631: m->size = 8; m->fp = 1; m->update = xo == 631; return 1;
    case 663: case 695: m->size = 4; m->fp = 1; m->store = 1; m->update = xo == 695; return 1;
    case 727: case 759: m->size = 8; m->fp = 1; m->store = 1; m->update = xo == 759; return 1;
    default: return 0;
    }
}

static unsigned mem_align(const meminfo *m) { return m->size == 8 ? 7u : m->size - 1u; }

/* --- modes, breakpoints ----------------------------------------------------------------- */

static int usable(const ppc_cpu *c)
{
    if (ppc_power_waiting(c)) return 0;
    if (c->msr & (PPC_MSR_SE | PPC_MSR_BE)) return 0;
    if (ppc_little_endian(c)) return 0;
    if (c->model == PPC_MODEL_601) {
        unsigned mode = (c->hid[1] >> 28) & 7u, response = (c->hid[1] >> 22) & 3u;
        if ((response == 1u || response == 2u) && (mode == 2u || mode == 6u || mode == 7u)) return 0;
        return !(c->hid[5] & 3u);
    }
    if (c->iabr & 2u) return 0;
    return (ppc_model_information(c->model)->features & PPC_MODEL_SOFT_TLB) || !(c->dabr & 3u);
}

static unsigned data_mode(const ppc_cpu *c) { return (c->msr & PPC_MSR_DR ? 2u : 0u) | (c->msr & PPC_MSR_PR ? 1u : 0u); }
static unsigned fetch_mode(const ppc_cpu *c) { return (c->msr & PPC_MSR_IR ? 2u : 0u) | (c->msr & PPC_MSR_PR ? 1u : 0u); }

static void set_modes(ppc_jit *j)
{
    const unsigned fm = fetch_mode(j->cpu), dm = data_mode(j->cpu);
    j->cur_fetch = j->tlb[K_FETCH][fm];
    j->cur_read = j->tlb[K_READ][dm];
    j->cur_write = j->tlb[K_WRITE][dm];
}

/* --- the soft TLB ----------------------------------------------------------------------- */

static void tlb_flush(ppc_jit *j)
{
    for (unsigned s = 0; s < 12; ++s)
        if (j->tlb_used & (1u << s)) memset(j->tlb[s / 4u][s % 4u], 0, sizeof j->tlb[0][0]);
    j->tlb_used = 0;
    ++j->tlb_generation;
    ++j->stats.flushes;
}

static void take_snap(const ppc_cpu *c, snap *s)
{
    memcpy(s->sr, c->sr, sizeof s->sr);
    memcpy(s->bat, c->bat, sizeof s->bat);
    memcpy(s->dbat, c->dbat, sizeof s->dbat);
    s->sdr1 = c->sdr1;
    s->hid0 = c->hid[0];
}

static void flush_code(ppc_jit *j);

static int lm_mode(const ppc_cpu *c) { return c->model == PPC_MODEL_601 && (c->hid[0] & 8u); }

/* The 603's TLB entry [which][index][*] changed (tlbld, tlbli, tlbie): every
 * translation made through it is of a page with that index (soft_way), in
 * the fetch sets for the ITLB, the read and write sets for the DTLB. */
static void soft_drop(ppc_jit *j, unsigned which, unsigned index)
{
    const unsigned first = which ? K_READ : K_FETCH, last = which ? K_WRITE : K_FETCH;
    if (which) j->soft_dropped |= UINT32_C(1) << index;
    for (unsigned kind = first; kind <= last; ++kind)
        for (unsigned m = 0; m < 4; ++m) {
            if (!(j->tlb_used & (1u << (kind * 4u + m)))) continue;
            for (unsigned i = index; i < TLB_SIZE; i += 32u) j->tlb[kind][m][i].tag = 0;
        }
}

/* The translation state against the one the TLB was filled under. */
static void mmu_check(ppc_jit *j)
{
    snap now;
    int changed;
    take_snap(j->cpu, &now);
    changed = memcmp(&now, &j->snap, sizeof now) != 0;
    if (j->lm != (uint8_t)lm_mode(j->cpu)) {
        /* MPC601UM 5.4.6: translated loads and stores hold HID0[LM]'s rule. */
        j->lm = (uint8_t)lm_mode(j->cpu);
        flush_code(j);
    }
    if (j->soft && j->soft_seen != j->cpu->tlb_writes && (j->soft_seen = j->cpu->tlb_writes, memcmp(j->soft_tlb, j->cpu->tlb, sizeof j->soft_tlb))) {
        if (!changed)
            for (unsigned which = 0; which < 2; ++which)
                for (unsigned index = 0; index < 32; ++index)
                    for (unsigned way = 0; way < 2; ++way) {
                        const ppc_tlb_entry *was = &j->soft_tlb[which][index][way], *is = &j->cpu->tlb[which][index][way];
                        if (was->vsid != is->vsid || was->page != is->page || was->pte != is->pte || was->valid != is->valid)
                            soft_drop(j, which, index);
                    }
        memcpy(j->soft_tlb, j->cpu->tlb, sizeof j->soft_tlb);
    }
    if (changed) {
        j->snap = now;
        tlb_flush(j);
    }
}

/* The 603's TLB entry ppc_classic_translate hits for `ea` (its LRU byte is
 * set on every access through it); -1 when it goes another way. */
static int soft_way(const ppc_cpu *c, uint32_t ea, ppc_access access, unsigned *which, unsigned *index)
{
    bool user = (c->msr & PPC_MSR_PR) != 0;
    uint32_t sr = c->sr[ea >> 28];
    if (!(c->msr & (access == PPC_FETCH ? PPC_MSR_IR : PPC_MSR_DR))) return -1;
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t *bat = access == PPC_FETCH ? c->bat[i] : c->dbat[i];
        uint32_t upper = bat[0], bl = (upper >> 2) & 0x7ffu, mask = (bl << 17) | 0x1ffffu;
        if ((upper & (user ? 1u : 2u)) && (ea & ~mask) == (upper & 0xfffe0000u & ~mask)) return -1;
    }
    if (sr & 0x80000000u) return -1;
    *which = access == PPC_FETCH ? 0u : 1u;
    *index = (ea >> 12) & 31u;
    for (unsigned way = 0; way < 2; ++way) {
        const ppc_tlb_entry *e = &c->tlb[*which][*index][way];
        if (e->valid && e->vsid == (sr & 0xffffffu) && e->page == ((ea >> 12) & 0xffffu)) return (int)way;
    }
    return -1;
}

static jpage *page_find(ppc_jit *j, const uint8_t *host);
static void purge_writes(ppc_jit *j, const uint8_t *host);

/* The interpreter's translation of `ea` for this step, into TLB entry t.
 * 0: the translation faulted (the step's exception taken) or failed. */
static int fill(ppc_jit *j, jtlb *t, uint32_t ea, unsigned kind)
{
    ppc_cpu *c = j->cpu;
    const ppc_access access = kind == K_FETCH ? PPC_FETCH : kind == K_READ ? PPC_READ : PPC_WRITE;
    const uint32_t page = ea & PAGE_MASK;
    const unsigned set = kind * 4u + (kind == K_FETCH ? fetch_mode(c) : data_mode(c));
    uint32_t pa;
    uint8_t *host;
    unsigned which, index;
    int way;
    j->filling = 1;
    const bool ok = ppc_translate(c, ea, access, &pa);
    j->filling = 0;
    ++j->stats.fills;
    if (!ok) return 0;
    j->tlb_used |= 1u << set;
    j->tlb_pa[kind][(set & 3u) * TLB_SIZE + (unsigned)(t - j->tlb[kind][set & 3u])] = pa & PAGE_MASK;
    j->tlb_wim[kind][(set & 3u) * TLB_SIZE + (unsigned)(t - j->tlb[kind][set & 3u])] = (uint8_t)c->mmu_wim;
    host = (kind == K_WRITE ? j->write_map : j->read_map)[pa >> 12];
    if (host && kind == K_WRITE) {
        const jpage *p = page_find(j, host);
        if (p && p->flags) host = NULL;
    }
    t->host = host;
    t->addend = host ? (intptr_t)host - (intptr_t)page : 0;
    t->lru_ptr = &j->dummy;
    t->lru_val = 0;
    if (j->soft && (way = soft_way(c, ea, access, &which, &index)) >= 0) {
        t->lru_ptr = &c->tlb_lru[which][index];
        t->lru_val = (uint8_t)(way ^ 1);
    }
    t->tag = page | (host ? 1u : 3u);
    return 1;
}

/* --- pages: translated code and the page table ------------------------------------------ */

static unsigned page_slot(const uint8_t *host)
{
    uint64_t v = (uint64_t)(uintptr_t)host >> 12;
    v ^= v >> 17;
    return (unsigned)(v * UINT64_C(0x9E3779B97F4A7C15) >> 48) & (PAGE_SLOTS - 1u);
}

static jpage *page_find(ppc_jit *j, const uint8_t *host)
{
    for (unsigned i = page_slot(host), n = 0; n < PAGE_SLOTS; i = (i + 1u) & (PAGE_SLOTS - 1u), ++n) {
        jpage *p = j->pages[i];
        if (!p) return NULL;
        if (p->host == host) return p;
    }
    return NULL;
}

static void reset_pages(ppc_jit *j)
{
    for (unsigned i = 0; i < PAGE_SLOTS; ++i) {
        free(j->pages[i]);
        j->pages[i] = NULL;
    }
    j->npages = 0;
    j->cycle.period = 0;        /* its words were read through the old map */
    if (j->guard) j->guard(j->guard_ctx, NULL, 0, 0);
    flush_code(j);
    tlb_flush(j);
    j->flushed = 1;
}

static jpage *page_get(ppc_jit *j, uint8_t *host)
{
    jpage *p = page_find(j, host);
    unsigned i;
    if (p) return p;
    if (j->npages >= PAGE_SLOTS / 2u) reset_pages(j);
    p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->host = host;
    for (i = page_slot(host); j->pages[i]; i = (i + 1u) & (PAGE_SLOTS - 1u)) {}
    j->pages[i] = p;
    ++j->npages;
    return p;
}

/* The TLB's write entries of host page `host` go (they are filled again,
 * inline or not as the page's flags now say). */
static void drop_writes(ppc_jit *j, const uint8_t *host)
{
    for (unsigned m = 0; m < 4; ++m) {
        if (!(j->tlb_used & (1u << (K_WRITE * 4u + m)))) continue;
        for (unsigned i = 0; i < TLB_SIZE; ++i)
            if (j->tlb[K_WRITE][m][i].host == host) j->tlb[K_WRITE][m][i].tag = 0;
    }
}

static void purge_writes(ppc_jit *j, const uint8_t *host)
{
    if (j->held) j->held(j->guard_ctx, host);
    else ++j->tlb_generation;
    for (unsigned m = 0; m < 4; ++m) {
        if (!(j->tlb_used & (1u << (K_WRITE * 4u + m)))) continue;
        for (unsigned i = 0; i < TLB_SIZE; ++i)
            if (j->tlb[K_WRITE][m][i].host == host) j->tlb[K_WRITE][m][i].tag = 0;
    }
}

/* A page the hardware table walk read: its writes change translations. */
static void watch(ppc_jit *j, uint8_t *host)
{
    jpage *p = page_get(j, host);
    if (!p) { reset_pages(j); return; }
    if (!(p->flags & P_WATCHED)) {
        p->flags |= P_WATCHED;
        purge_writes(j, host);
    }
}

static unsigned block_hash(uint32_t pc, const uint8_t *host)
{
    uint64_t v = ((uint64_t)(uintptr_t)host >> 12) * UINT64_C(0x9E3779B97F4A7C15) ^ (pc >> 2);
    return (unsigned)(v ^ (v >> 29)) & (HASH_SIZE - 1u);
}

static void kill(ppc_jit *j, jblock *k)
{
    jblock **h = &j->hash[block_hash(k->pc, k->host)];
    jfast *f = &j->fast[(k->pc >> 2) & (FAST_SIZE - 1u)];
    while (*h && *h != k) h = &(*h)->hnext;
    if (*h) *h = k->hnext;
    if (f->code == k->code) f->pc = 0xffffffffu;
    ++j->stats.invalidated;
}

/* Bytes [offset, offset + size) of host page `host` were written. */
static void written(ppc_jit *j, uint8_t *host, uint32_t offset, uint32_t size)
{
    jpage *p = page_find(j, host);
    if (!p || !size) return;
    if (p->flags & P_CODE) {
        unsigned w0 = offset >> 2, w1 = (offset + size - 1u) >> 2, w;
        if (w1 > 1023u) w1 = 1023u;
        for (w = w0; w <= w1 && !(p->words[w >> 5] & (1u << (w & 31u))); ++w) {}
        if (w <= w1) {
            jblock **link = &p->blocks;
            memset(p->words, 0, sizeof p->words);
            while (*link) {
                jblock *k = *link;
                if (k->first <= w1 && (unsigned)k->first + k->count > w0) {
                    *link = k->pnext;
                    kill(j, k);
                    continue;
                }
                for (unsigned i = k->first; i < (unsigned)k->first + k->count; ++i) p->words[i >> 5] |= 1u << (i & 31u);
                link = &k->pnext;
            }
            if (!p->blocks) {
                /* no translated code left: its writes may be inline again */
                p->flags &= ~(unsigned)P_CODE;
                if (!p->flags) drop_writes(j, host);
            }
            j->smc = 1;
        }
    }
    if ((p->flags & P_WATCHED) && !j->filling) {
        tlb_flush(j);
        j->flushed = 1;
    }
    if ((p->flags & P_GUARDED) && j->guard) j->guard(j->guard_ctx, host, offset, size);
}

/* --- the observed bus ------------------------------------------------------------------- */

/* A device access: the machine's clock to this step's. */
static void device_access(ppc_jit *j)
{
    j->io = 1;
    if (j->clock && j->running) j->clock(j->clock_ctx, j->run_base + (uint64_t)(j->give - j->left));
}

static bool wrap_read(void *ctx, uint32_t address, unsigned size, uint32_t *value)
{
    ppc_jit *j = ctx;
    uint8_t *host = j->read_map[address >> 12];
    if (!host) device_access(j);
    else if (j->filling) watch(j, host);
    else ++j->idle_reads;
    return j->bus.read && j->bus.read(j->bus.opaque, address, size, value);
}

static bool wrap_write(void *ctx, uint32_t address, unsigned size, uint32_t value)
{
    ppc_jit *j = ctx;
    uint8_t *host = j->write_map[address >> 12];
    bool ok;
    if (host && j->idle_learning) {
        for (unsigned b = 0; b < size; ++b) {
            uint8_t *at = host + (address & 4095u) + b;
            unsigned k;
            for (k = 0; k < j->idle_bytes && j->idle_writes[k].at != at; ++k) {}
            if (k == j->idle_bytes) {
                if (k == 4096u) { j->idle_reject = 1; break; }
                j->idle_writes[k].at = at;
                j->idle_writes[k].value = *at;
                ++j->idle_bytes;
            }
        }
    }
    if (!host) device_access(j);
    ok = j->bus.write && j->bus.write(j->bus.opaque, address, size, value);
    if (host) written(j, host, address & 0xfffu, size);
    return ok;
}

void ppc_jit_set_clock(ppc_jit *j, void (*clock)(void *ctx, uint64_t steps), void *ctx)
{
    if (!j) return;
    j->clock = clock;
    j->clock_ctx = ctx;
}

void ppc_jit_written(ppc_jit *j, uint32_t address, uint32_t length)
{
    if (!j || !j->read_map) return;
    while (length) {
        const uint32_t offset = address & 0xfffu;
        uint32_t n = 4096u - offset;
        uint8_t *host = j->write_map[address >> 12];
        if (n > length) n = length;
        if (host) written(j, host, offset, n);
        address += n;
        length -= n;
        if (!address) break;
    }
}

/* --- call-outs from translated code ------------------------------------------------------ */

static void step_begin(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    c->result = PPC_OK;
    c->pc = c->last_pc = pc;
    c->last_opcode = op;
    c->last_exception = 0;
    j->io = j->smc = j->flushed = 0;
}

static uint32_t step_flags(const ppc_jit *j)
{
    return (j->cpu->result != PPC_OK ? 1u : 0u) | j->io | j->smc | j->flushed;
}

/* One instruction by the interpreter; nonzero: the block leaves after it. */
static JIT_CALL uint32_t helper(ppc_jit *j, uint32_t op, uint32_t pc)
{
    const uint64_t start = j->profile ? host_ticks() : 0;
    ++j->stats.helpers;
    step_begin(j, op, pc);
    ppc_execute(j->cpu, op);
    profile_add(j, 0, pc, op, start);
    return step_flags(j);
}

/* A load or store the inline path did not make (unaligned, a TLB miss, a
 * device, a watched page, the FPU off): cached aligned integer device
 * reads use the observed bus below; other cases use the interpreter. On a
 * TLB miss of an access the interpreter will translate (aligned, no alignment
 * or FPU exception first), its translation fills the TLB first: the same
 * call, with the same effects, that the instruction makes, which then
 * repeats it without any. */
static JIT_CALL uint32_t mem_slow(ppc_jit *j, uint32_t op, uint32_t pc, uint32_t ea)
{
    ppc_cpu *c = j->cpu;
    meminfo mi;
    uint32_t flags;
    const uint64_t start = j->profile ? host_ticks() : 0;
    ++j->stats.memory_slow;
    mem_decode(op, &mi);
    step_begin(j, op, pc);
    const int mapped_span = (ea & 4095u) + mi.size <= 4096u;
    const int scalar_unaligned = !mi.fp && mapped_span &&
        (c->model != PPC_MODEL_601 || !(c->hid[0] & 8u)); /* MPC601UM 5.4.6, HID0[LM]. */
    if ((!(ea & mem_align(&mi)) || scalar_unaligned) && (!mi.fp || (c->msr & PPC_MSR_FP)) &&
        !(mi.fp && c->model == PPC_MODEL_601 && (c->sr[ea >> 28] & 0x80000000u))) {
        const unsigned kind = mi.store ? K_WRITE : K_READ;
        jtlb *t = &j->tlb[kind][data_mode(c)][(ea >> 12) & (TLB_SIZE - 1u)];
        if ((t->tag | 2u) != ((ea & PAGE_MASK) | 3u)) {
            c->pc = pc + 4u;
            if (!fill(j, t, ea, kind)) {
                profile_add(j, 1, pc, op, start);
                return 1u;
            }
            c->pc = pc;
        }
    }
    /* A cached device translation already passed the interpreter's
     * protection checks. Ordinary aligned integer reads still issue the
     * same sized bus cycle, through the observed bus (clock/side effects/
     * machine checks), without decoding and walking the MMU twice again.
     * Breakpoints, tracing and little-endian modes never enter this path. */
    const unsigned mode = data_mode(c), at = (ea >> 12) & (TLB_SIZE - 1u);
    const unsigned kind = mi.store ? K_WRITE : K_READ;
    jtlb *cached = &j->tlb[kind][mode][at];
    jtlb *read = &j->tlb[K_READ][mode][at];
    if (!mi.fp && !mi.update && !mi.rev && !(ea & (mi.size - 1u)) &&
        cached->tag == ((ea & PAGE_MASK) | 1u)) {
        /* A successful fill already made all permission/R/C effects.
         * Ordinary host memory has the same conditions as the inline path;
         * watched code, page tables and guarded writes have tag 3 instead. */
        uint8_t *host = (uint8_t *)(cached->addend + (intptr_t)(uintptr_t)ea);
        uint32_t value = c->gpr[(op >> 21) & 31u];
        c->mmu_wim = j->tlb_wim[kind][mode * TLB_SIZE + at];
        *cached->lru_ptr = cached->lru_val;
        c->pc = pc + 4u;
        if (mi.store) {
            for (unsigned i = 0; i < mi.size; ++i)
                host[i] = (uint8_t)(value >> ((mi.size - i - 1u) * 8u));
        } else {
            value = 0;
            for (unsigned i = 0; i < mi.size; ++i) value = value << 8 | host[i];
            c->gpr[(op >> 21) & 31u] = mi.sign ? (uint32_t)(int32_t)(int16_t)value : value;
        }
        ++c->instructions;
    } else if (!mi.store && !mi.fp && !mi.update && !mi.rev &&
        !(ea & (mi.size - 1u)) && read->tag == ((ea & PAGE_MASK) | 3u)) {
        uint32_t value = 0;
        const unsigned slot = mode * TLB_SIZE + at;
        const uint32_t pa = j->tlb_pa[K_READ][slot] | (ea & 4095u);
        c->mmu_wim = j->tlb_wim[K_READ][slot];
        *read->lru_ptr = read->lru_val;
        c->pc = pc + 4u;
        if (!c->bus.read || !c->bus.read(c->bus.opaque, pa, mi.size, &value))
            ppc_exception(c, PPC_EX_MACHINE, 0, c->last_pc);
        else {
            value &= UINT32_MAX >> ((4u - mi.size) * 8u);
            c->gpr[(op >> 21) & 31u] = mi.sign ? (uint32_t)(int32_t)(int16_t)value : value;
            if (c->result == PPC_OK) ++c->instructions;
        }
    } else ppc_execute(c, op);
    profile_add(j, 1, pc, op, start);
    flags = step_flags(j);
    if (!flags) --c->instructions;      /* the block counts it with its own */
    return flags;
}

/* --- the multiple-access instructions ------------------------------------------------------ */

/* These make the interpreter's accesses on the host page when the TLB maps
 * the whole span inline, which is when the interpreter's own translations
 * of it have no effect left but the 603's LRU byte (as the inline loads and
 * stores); anything else is the interpreter's (helper). Data breakpoints
 * and little-endian mode never reach translated code (usable). */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint8_t *fast_span(ppc_jit *j, uint32_t ea, unsigned bytes, unsigned kind, unsigned *slot)
{
    const unsigned mode = data_mode(j->cpu), at = (ea >> 12) & (TLB_SIZE - 1u);
    jtlb *t = &j->tlb[kind][mode][at];
    if ((ea & 4095u) + bytes > 4096u || t->tag != ((ea & PAGE_MASK) | 1u)) return NULL;
    *t->lru_ptr = t->lru_val;
    *slot = mode * TLB_SIZE + at;
    return (uint8_t *)(t->addend + (intptr_t)(uintptr_t)ea);
}

/* lmw, stmw (multiple() of ppc_cpu.c). */
static JIT_CALL uint32_t fast_multiple(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    const unsigned reg = (op >> 21) & 31u, a = (op >> 16) & 31u, store = (op >> 26) == 47u;
    const uint32_t ea = (a ? c->gpr[a] : 0u) + (uint32_t)(int32_t)(int16_t)(op & 0xffffu);
    unsigned slot;
    uint8_t *p;
    if ((ea & 3u) || lm_mode(c) || !(p = fast_span(j, ea, (32u - reg) * 4u, store ? K_WRITE : K_READ, &slot)))
        return helper(j, op, pc);
    for (unsigned r = reg; r < 32u; ++r, p += 4) {
        if (store) put32(p, c->gpr[r]);
        else c->gpr[r] = be32(p);
    }
    ++c->instructions;
    return 0;
}

/* lswi, lswx, stswi, stswx (string_access() of ppc_cpu.c). */
static JIT_CALL uint32_t fast_string(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    const unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u, xo = (op >> 1) & 1023u;
    const int indexed = xo == 533u || xo == 661u, store = xo == 661u || xo == 725u;
    const unsigned bytes = indexed ? c->xer & 127u : b ? b : 32u;
    const uint32_t ea = (a ? c->gpr[a] : 0u) + (indexed ? c->gpr[b] : 0u);
    uint32_t temp[32];
    bool changed[32] = {false};
    unsigned slot;
    uint8_t *p = NULL;
    if (lm_mode(c) || (bytes && !(p = fast_span(j, ea, bytes, store ? K_WRITE : K_READ, &slot))))
        return helper(j, op, pc);
    memcpy(temp, c->gpr, sizeof temp);
    for (unsigned i = 0; i < bytes; ++i) {
        const unsigned r = (d + i / 4u) & 31u, shift = 24u - (i & 3u) * 8u;
        if (store) p[i] = (uint8_t)(c->gpr[r] >> shift);
        else {
            if ((a && r == a) || (indexed && r == b)) continue;
            if (!(i & 3u)) temp[r] = 0;
            temp[r] |= (uint32_t)p[i] << shift;
            changed[r] = true;
        }
    }
    if (!store) for (unsigned r = 0; r < 32u; ++r) if (changed[r]) c->gpr[r] = temp[r];
    ++c->instructions;
    return 0;
}

/* dcbz (extended() of ppc_cpu.c): a direct-store segment's and a
 * write-through or cache-inhibited page's are the interpreter's. */
static JIT_CALL uint32_t fast_dcbz(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    const unsigned a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    const uint32_t address = (a ? c->gpr[a] : 0u) + c->gpr[b];
    unsigned slot;
    uint8_t *p;
    if (((c->model == PPC_MODEL_601 || (c->msr & PPC_MSR_DR)) && (c->sr[address >> 28] & 0x80000000u)) ||
        !(p = fast_span(j, address & ~31u, 32u, K_WRITE, &slot)) || (j->tlb_wim[K_WRITE][slot] & 0x60u))
        return helper(j, op, pc);
    memset(p, 0, 32);
    ++c->instructions;
    return 0;
}

/* lwarx (extended() of ppc_cpu.c). */
static JIT_CALL uint32_t fast_lwarx(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    const unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    const uint32_t ea = (a ? c->gpr[a] : 0u) + c->gpr[b];
    unsigned slot;
    uint8_t *p;
    if ((ea & 3u) || !(p = fast_span(j, ea, 4u, K_READ, &slot))) return helper(j, op, pc);
    c->gpr[d] = be32(p);
    c->reservation = (j->tlb_pa[K_READ][slot] | (ea & 4095u)) & ~31u;
    c->reserved = true;
    ++c->instructions;
    return 0;
}

/* stwcx. (extended() of ppc_cpu.c). */
static JIT_CALL uint32_t fast_stwcx(ppc_jit *j, uint32_t op, uint32_t pc)
{
    ppc_cpu *c = j->cpu;
    const unsigned s = (op >> 21) & 31u, a = (op >> 16) & 31u, b = (op >> 11) & 31u;
    const uint32_t ea = (a ? c->gpr[a] : 0u) + c->gpr[b];
    unsigned slot;
    uint8_t *p;
    bool success;
    if (!(op & 1u) || (ea & 3u) || !(p = fast_span(j, ea, 4u, K_WRITE, &slot))) return helper(j, op, pc);
    success = c->reserved && ppc_reservation_matches(c, j->tlb_pa[K_WRITE][slot] | (ea & 4095u));
    c->reserved = false;
    if (success) put32(p, c->gpr[s]);
    c->cr = (c->cr & UINT32_C(0x0fffffff)) | ((success ? 2u : 0u) | (c->xer >> 31)) << 28;
    ++c->instructions;
    return 0;
}

/* Floating-point arithmetic, rounding, conversion and compare: the host's
 * for ordinary operands (ppc_fpu_fast), else the interpreter's. */
static JIT_CALL uint32_t fast_fp(ppc_jit *j, uint32_t op, uint32_t pc)
{
    if (!ppc_fpu_fast(j->cpu, op)) return helper(j, op, pc);
    ++j->cpu->instructions;
    return 0;
}

/* --- generation ------------------------------------------------------------------------- */

enum { C_SLOW, C_EXIT, C_PRIVILEGED };

typedef struct cold {
    uint8_t kind;
    uint8_t *sites[6];
    unsigned nsites;
    uint32_t op, pc;
    unsigned steps, inl;
    const uint8_t *resume;
} cold;

typedef struct gen {
    ppc_jit *j;
    xbuf b;
    uint32_t block_pc, pc, op;
    unsigned k, inl;
    const uint8_t *entry;
    int nofast, cache_reg;
    unsigned cache_until;
    cold cold[MAX_INSNS * 2u];
    unsigned ncold;
} gen;

/* A read-only GPR in the straight-line prefix lives in nonvolatile RDI.
 * Its architected copy is always current, including at faults and side
 * exits. Helpers end the cache; no dirty state crosses a call or block. */
static gen *buffer_gen(xbuf *b) { return (gen *)((uint8_t *)b - offsetof(gen, b)); }
static xo gpr_operand(xbuf *b, unsigned reg)
{
    gen *g = buffer_gen(b);
    if (g->cache_reg == (int)reg) {
        ++g->j->stats.cached_gpr_uses;
        return XR(XDI);
    }
    return XM(XBX, (int32_t)(offsetof(ppc_cpu, gpr) + 4u * reg));
}

static void choose_gpr_cache(gen *g, const uint32_t *words, unsigned max)
{
    unsigned reads[32] = {0};
    uint32_t writes = 0;
    unsigned until = 0;
    g->cache_reg = -1;
    g->cache_until = 0;
    if (g->nofast) return;
    for (unsigned k = 0; k < max; ++k) {
        const uint32_t op = words[k];
        const unsigned primary = op >> 26, d = (op >> 21) & 31u;
        const unsigned a = (op >> 16) & 31u, b = (op >> 11) & 31u, xo = (op >> 1) & 1023u;
        meminfo mi;
        int end = 0;
        if (mem_decode(op, &mi)) {
            if (mi.fp || (mi.update && (!a || (!mi.store && a == d)))) break;
            if (a) ++reads[a];
            if (mi.indexed) ++reads[b];
            if (mi.store) ++reads[d]; else writes |= 1u << d;
            if (mi.update) writes |= 1u << a;
        } else if (primary == 7 || primary == 8 || (primary >= 12 && primary <= 15)) {
            if (a || primary < 14) ++reads[a];
            writes |= 1u << d;
        } else if (primary == 10 || primary == 11) ++reads[a];
        else if (primary >= 24 && primary <= 29) { ++reads[d]; writes |= 1u << a; }
        else if (primary == 20 || primary == 21 || primary == 23) {
            ++reads[d]; if (primary == 20) ++reads[a]; if (primary == 23) ++reads[b];
            writes |= 1u << a;
        } else if (primary == 16) { /* conditional branch: no GPR writes */ }
        else if (primary == 18) end = 1;
        else if (primary == 31 && (xo == 266 || xo == 40)) {
            ++reads[a]; ++reads[b]; writes |= 1u << d;
        } else if (primary == 31 && (xo == 28 || xo == 444 || xo == 316 || xo == 124 || xo == 24 || xo == 536)) {
            ++reads[d]; ++reads[b]; writes |= 1u << a;
        } else if (primary == 31 && (xo == 824 || xo == 26 || xo == 922 || xo == 954)) {
            ++reads[d]; writes |= 1u << a;
        } else if (primary == 31 && (xo == 0 || xo == 32)) { ++reads[a]; ++reads[b]; }
        else if (primary == 31 && xo == 83) writes |= 1u << d;
        else if (primary == 31 && xo == 146) { ++reads[d]; end = 1; }
        else break;
        until = k + 1u;
        if (end) break;
    }
    unsigned best = 2;
    for (unsigned reg = 0; reg < 32; ++reg)
        if (!(writes & (1u << reg)) && reads[reg] > best) { g->cache_reg = (int)reg; best = reads[reg]; }
    g->cache_until = until;
}

static cold *new_cold(gen *g, unsigned kind)
{
    cold *s = &g->cold[g->ncold++];
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    s->op = g->op;
    s->pc = g->pc;
    s->steps = g->k + 1u;
    s->inl = g->inl;
    return s;
}

static void emit_counts(xbuf *b, unsigned steps, unsigned inl, int may_reset)
{
    if (steps) x_alu_i(b, ALU_SUB, 8, XR(X13), (int32_t)steps);
    if (inl) {
        /* A reset during a device instruction discards the preceding
         * instructions, whose deferred count must not enter reset state. */
        uint8_t *reset = NULL;
        if (may_reset) {
            x_test_i(b, 1, JIT(cpu_reset), 1);
            reset = x_jcc(b, CC_NE);
        }
        x_alu_i(b, ALU_ADD, 8, CPU(instructions), (int32_t)inl);
        if (reset) x_bind(b, reset);
    }
}

/* Leaves for `target` (in eax when dynamic) after `steps` steps, `inl` of
 * them inline; the last one was (lpc, lop). */
static void emit_goto(gen *g, int dynamic, uint32_t target, unsigned steps, unsigned inl, uint32_t lpc, uint32_t lop)
{
    xbuf *b = &g->b;
    if (dynamic) x_load(b, 4, XCX, XR(XAX));
    emit_counts(b, steps, inl, 0);
    x_mov_mi(b, 4, CPU(last_pc), lpc);
    x_mov_mi(b, 4, CPU(last_opcode), lop);
    x_mov_mi(b, 4, CPU(last_exception), 0);
    x_mov_mi(b, 4, CPU(result), PPC_OK);
    if (!dynamic) x_mov_ri(b, XCX, target);
    x_store(b, 4, CPU(pc), XCX);
    if (!dynamic && target == g->block_pc) x_jmp_to(b, g->entry);
    else x_jmp_to(b, g->j->dispatch);
}

/* Leaves for the run loop's state checks (EXIT_SYSTEM) after the inline
 * instruction (lpc, lop), the block's last. */
static void emit_leave_reason(gen *g, unsigned steps, unsigned inl, uint32_t lpc, uint32_t lop, unsigned why)
{
    xbuf *b = &g->b;
    emit_counts(b, steps, inl, 0);
    x_mov_mi(b, 4, CPU(last_pc), lpc);
    x_mov_mi(b, 4, CPU(last_opcode), lop);
    x_mov_mi(b, 4, CPU(last_exception), 0);
    x_mov_mi(b, 4, CPU(result), PPC_OK);
    x_mov_mi(b, 4, CPU(pc), lpc + 4u);
    x_mov_mi(b, 1, JIT(io), 0);
    x_mov_ri(b, XAX, why);
    x_jmp_to(b, g->j->epilogue);
}

static void emit_leave(gen *g, unsigned steps, unsigned inl, uint32_t lpc, uint32_t lop)
{
    emit_leave_reason(g, steps, inl, lpc, lop, EXIT_SYSTEM);
}

static void emit_exit(xbuf *b, const ppc_jit *j, unsigned steps, unsigned inl, unsigned why)
{
    emit_counts(b, steps, inl, 1);
    x_mov_ri(b, XAX, why);
    x_jmp_to(b, j->epilogue);
}

/* A call-out for the instruction (op, pc), the block's `k`-th: j->left is
 * the budget as of its step (device accesses tell the machine). */
static void emit_call(xbuf *b, const void *fn, uint32_t op, uint32_t pc, unsigned k)
{
    buffer_gen(b)->cache_reg = -1;
    x_lea(b, 8, XAX, XM(X13, -(int32_t)k));
    x_store(b, 8, JIT(left), XAX);
    x_load(b, 8, XCX, XR(XBP));
    x_mov_ri(b, XDX, op);
    x_mov_ri(b, X8, pc);
    x_call_abs(b, fn);
}

/* The interpreter for this instruction; `end`: the block ends after it. */
static int emit_helper(gen *g, int end)
{
    xbuf *b = &g->b;
    emit_call(b, (const void *)helper, g->op, g->pc, g->k);
    if (end) {
        emit_exit(b, g->j, g->k + 1u, g->inl, EXIT_SYSTEM);
        return 1;
    }
    x_test(b, 4, XR(XAX), XAX);
    {
        cold *s = new_cold(g, C_EXIT);
        s->sites[s->nsites++] = x_jcc(b, CC_NE);
    }
    return 0;
}

/* One of the multiple-access call-outs above. */
static int emit_fast(gen *g, const void *fn)
{
    xbuf *b = &g->b;
    cold *s;
    if (g->nofast) return emit_helper(g, 0);
    emit_call(b, fn, g->op, g->pc, g->k);
    x_test(b, 4, XR(XAX), XAX);
    s = new_cold(g, C_EXIT);
    s->sites[s->nsites++] = x_jcc(b, CC_NE);
    return 0;
}

static void load_ra0(xbuf *b, int reg, unsigned a)
{
    if (a) x_load(b, 4, reg, GPR(a));
    else x_alu(b, ALU_XOR, 4, XR(reg), XR(reg));
}

/* CR field f from the flags of a compare (signed: L/G, else B/A) and
 * XER[SO]; clobbers ecx, edx. */
static void emit_cr_insert(xbuf *b, unsigned f, int reg)
{
    const unsigned shift = 28u - 4u * f;
    if (shift) x_shift_i(b, SH_SHL, 4, XR(reg), shift);
    x_load(b, 4, XCX, CPU(cr));
    x_alu_i(b, ALU_AND, 4, XR(XCX), (int32_t)~(UINT32_C(15) << shift));
    x_alu(b, ALU_OR, 4, XR(XCX), XR(reg));
    x_store(b, 4, CPU(cr), XCX);
}

static void emit_cr_flags(xbuf *b, unsigned f, int sign)
{
    x_mov_ri(b, XDX, 2);
    x_mov_ri(b, XCX, 4);
    x_cmov(b, sign ? CC_G : CC_A, 4, XDX, XR(XCX));
    x_mov_ri(b, XCX, 8);
    x_cmov(b, sign ? CC_L : CC_B, 4, XDX, XR(XCX));
    x_load(b, 4, XCX, CPU(xer));
    x_shift_i(b, SH_SHR, 4, XR(XCX), 31);
    x_alu(b, ALU_OR, 4, XR(XDX), XR(XCX));
    emit_cr_insert(b, f, XDX);
}

/* CR0 from the result in `reg` (ppc_record). */
static void emit_record(xbuf *b, int reg)
{
    x_test(b, 4, XR(reg), reg);
    emit_cr_flags(b, 0, 1);
}

/* XER[CA] from r8b, XER[OV] and [SO] from r9b (ppc_overflow). */
static void emit_xer(xbuf *b, int ca, int ov)
{
    x_load(b, 4, XCX, CPU(xer));
    if (ca) {
        x_alu_i(b, ALU_AND, 4, XR(XCX), (int32_t)~PPC_XER_CA);
        x_movzx(b, XDX, XR(X8), 1);
        x_shift_i(b, SH_SHL, 4, XR(XDX), 29);
        x_alu(b, ALU_OR, 4, XR(XCX), XR(XDX));
    }
    if (ov) {
        x_alu_i(b, ALU_AND, 4, XR(XCX), (int32_t)~PPC_XER_OV);
        x_movzx(b, XDX, XR(X9), 1);
        x_shift_i(b, SH_SHL, 4, XR(XDX), 30);
        x_alu(b, ALU_OR, 4, XR(XCX), XR(XDX));
        x_shift_i(b, SH_SHL, 4, XR(XDX), 1);
        x_alu(b, ALU_OR, 4, XR(XCX), XR(XDX));
    }
    x_store(b, 4, CPU(xer), XCX);
}

#if !MC_PPC_A64
static void x_bswap64(xbuf *b, int r)
{
    x8(b, 0x48u | ((r & 8) ? 1u : 0u));
    x8(b, 0x0F);
    x8(b, 0xC8u + (unsigned)(r & 7));
}
/* STC, CLC, CDQ and BSR EAX, r/m32 (a64emit.h has its own). */
static void x_stc(xbuf *b) { x8(b, 0xF9); }
static void x_clc(xbuf *b) { x8(b, 0xF8); }
static void x_cdq(xbuf *b) { x8(b, 0x99); }
static void x_bsr(xbuf *b, int dst, xo src) { x_op2(b, 4, 0xBD, (unsigned)dst, 1, src); }
#endif

static uint32_t rot_mask(unsigned first, unsigned last)
{
    uint32_t left = UINT32_MAX >> first, right = UINT32_MAX << (31u - last);
    return first <= last ? left & right : left | right;
}

/* The add/subtract family of ppc_cpu.c's extended(): ~x (subtracting) + a
 * source + a carry in, with CA, OV and CR0 as it sets them. */
static void emit_add(gen *g, unsigned base)
{
    xbuf *b = &g->b;
    const uint32_t op = g->op;
    const unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, rb = (op >> 11) & 31u;
    const int oe = (op & 0x400u) != 0, rc = (op & 1u) != 0;
    const int sub = base == 8 || base == 40 || base == 104 || base == 136 || base == 200 || base == 232;
    const int ca = !(base == 40 || base == 104 || base == 266);
    x_load(b, 4, XAX, GPR(a));
    if (sub) x_grp3(b, 2, 4, XR(XAX));
    if (base == 8 || base == 40 || base == 104) x_stc(b);
    else if (base == 10 || base == 266) x_clc(b);
    else x_bt_i(b, 4, 4, CPU(xer), 29);                             /* CF = XER[CA] */
    if (base == 104 || base == 200 || base == 202) x_alu_i(b, ALU_ADC, 4, XR(XAX), 0);
    else if (base == 232 || base == 234) x_alu_i(b, ALU_ADC, 4, XR(XAX), -1);
    else x_alu_rm(b, ALU_ADC, 4, XAX, GPR(rb));
    if (ca) x_setcc(b, CC_B, XR(X8));
    if (oe) x_setcc(b, CC_O, XR(X9));
    x_store(b, 4, GPR(d), XAX);
    if (ca || oe) emit_xer(b, ca, oe);
    if (rc) emit_record(b, XAX);
}

static void emit_divide(gen *g, int sign)
{
    xbuf *b = &g->b;
    const uint32_t op = g->op;
    const unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, rb = (op >> 11) & 31u;
    uint8_t *zero, *zero2 = NULL, *ok = NULL, *done;
    x_load(b, 4, XAX, GPR(a));
    x_load(b, 4, XCX, GPR(rb));
    x_alu(b, ALU_XOR, 4, XR(X9), XR(X9));
    x_test(b, 4, XR(XCX), XCX);
    zero = x_jcc(b, CC_E);
    if (sign) {
        x_alu_i(b, ALU_CMP, 4, XR(XCX), -1);
        ok = x_jcc(b, CC_NE);
        x_alu_i(b, ALU_CMP, 4, XR(XAX), INT32_MIN);
        zero2 = x_jcc(b, CC_E);
        x_bind(b, ok);
        x_cdq(b);
        x_grp3(b, 7, 4, XR(XCX));                                   /* idiv ecx */
    } else {
        x_alu(b, ALU_XOR, 4, XR(XDX), XR(XDX));
        x_grp3(b, 6, 4, XR(XCX));                                   /* div ecx */
    }
    done = x_jmp(b);
    x_bind(b, zero);
    if (zero2) x_bind(b, zero2);
    x_alu(b, ALU_XOR, 4, XR(XAX), XR(XAX));
    x_mov_ri(b, X9, 1);
    x_bind(b, done);
    x_store(b, 4, GPR(d), XAX);
    if (op & 0x400u) emit_xer(b, 0, 1);
    if (op & 1u) emit_record(b, XAX);
}

/* rA from rS and rB (or SH): the logical and shift X-forms. */
static int emit_logic(gen *g, unsigned xo)
{
    xbuf *b = &g->b;
    const uint32_t op = g->op;
    const unsigned s = (op >> 21) & 31u, a = (op >> 16) & 31u, rb = (op >> 11) & 31u;
    int ca = 0;
    switch (xo) {
    case 28: case 124: case 316: case 284: case 444: case 476:
        x_load(b, 4, XAX, GPR(s));
        if (!(xo == 444 && s == rb))
            x_alu_rm(b, xo == 28 || xo == 476 ? ALU_AND : xo == 316 || xo == 284 ? ALU_XOR : ALU_OR, 4, XAX, GPR(rb));
        if (xo == 124 || xo == 284 || xo == 476) x_grp3(b, 2, 4, XR(XAX));
        break;
    case 60: case 412:
        x_load(b, 4, XCX, GPR(rb));
        x_grp3(b, 2, 4, XR(XCX));
        x_load(b, 4, XAX, GPR(s));
        x_alu(b, xo == 60 ? ALU_AND : ALU_OR, 4, XR(XAX), XR(XCX));
        break;
    case 24: case 536:
        x_load(b, 4, XAX, GPR(s));
        x_load(b, 4, XCX, GPR(rb));
        x_shift_cl(b, xo == 24 ? SH_SHL : SH_SHR, 4, XR(XAX));
        x_alu(b, ALU_XOR, 4, XR(XDX), XR(XDX));
        x_test_i(b, 4, XR(XCX), 32);
        x_cmov(b, CC_NE, 4, XAX, XR(XDX));
        break;
    case 792:
        x_load(b, 4, XAX, GPR(s));
        x_load(b, 4, XCX, GPR(rb));
        x_alu_i(b, ALU_AND, 4, XR(XCX), 63);
        x_mov_ri(b, XDX, 1);
        x_shift_cl(b, SH_SHL, 4, XR(XDX));
        x_alu_i(b, ALU_SUB, 4, XR(XDX), 1);
        x_mov_ri(b, X8, UINT32_MAX);
        x_alu_i(b, ALU_CMP, 4, XR(XCX), 32);
        x_cmov(b, CC_AE, 4, XDX, XR(X8));
        x_alu(b, ALU_AND, 4, XR(XDX), XR(XAX));
        x_setcc(b, CC_NE, XR(X8));
        x_test(b, 4, XR(XAX), XAX);
        x_setcc(b, CC_S, XR(X9));
        x_alu(b, ALU_AND, 1, XR(X8), XR(X9));
        x_alu_i(b, ALU_CMP, 4, XR(XCX), 31);
        x_mov_ri(b, XDX, 31);
        x_cmov(b, CC_A, 4, XCX, XR(XDX));
        x_shift_cl(b, SH_SAR, 4, XR(XAX));
        ca = 1;
        break;
    case 824:
        x_load(b, 4, XAX, GPR(s));
        if (rb) {
            x_load(b, 4, XDX, XR(XAX));
            x_alu_i(b, ALU_AND, 4, XR(XDX), (int32_t)((UINT32_C(1) << rb) - 1u));
            x_setcc(b, CC_NE, XR(X8));
            x_test(b, 4, XR(XAX), XAX);
            x_setcc(b, CC_S, XR(X9));
            x_alu(b, ALU_AND, 1, XR(X8), XR(X9));
            x_shift_i(b, SH_SAR, 4, XR(XAX), rb);
        } else x_mov_ri(b, X8, 0);
        ca = 1;
        break;
    case 26:
        x_load(b, 4, XCX, GPR(s));
        x_bsr(b, XAX, XR(XCX));
        x_mov_ri(b, XDX, UINT32_MAX);
        x_cmov(b, CC_E, 4, XAX, XR(XDX));
        x_grp3(b, 3, 4, XR(XAX));
        x_alu_i(b, ALU_ADD, 4, XR(XAX), 31);
        break;
    case 922: x_movsx(b, XAX, GPR(s), 2); break;
    case 954: x_movsx(b, XAX, GPR(s), 1); break;
    default: return 0;
    }
    x_store(b, 4, GPR(a), XAX);
    if (ca) emit_xer(b, 1, 0);
    if (op & 1u) emit_record(b, XAX);
    return 1;
}

/* A load or store: inline when aligned and the TLB maps the page to host
 * memory (for writes: no translated code, no page table there), else
 * mem_slow. */
static void emit_mem(gen *g, const meminfo *mi)
{
    xbuf *b = &g->b;
    const uint32_t op = g->op;
    const unsigned d = (op >> 21) & 31u, a = (op >> 16) & 31u, rb = (op >> 11) & 31u;
    const int32_t simm = (int16_t)(op & 0xffffu);
    const int set = mi->store ? X15 : X14;
    const unsigned align = mem_align(mi);
    cold *s = new_cold(g, C_SLOW);
    int32_t last = 0;
    xo host;
    load_ra0(b, XCX, a);
    if (mi->indexed) x_alu_rm(b, ALU_ADD, 4, XCX, GPR(rb));
    else if (simm) x_alu_i(b, ALU_ADD, 4, XR(XCX), simm);
    /* MPC601UM 5.4.6; MPC603eUM 4.5.6; MPC604eUM 3.1.5; MPC750UM 4.5.6:
     * ordinary BE integer accesses may be unaligned, in one cached page
     * (the tag compared is the last byte's page); with the 601's HID0[LM]
     * (the code is translated under it), and floating point, aligned only. */
    if (align && (mi->fp || g->j->lm)) {
        x_test_i(b, 4, XR(XCX), align);
        s->sites[s->nsites++] = x_jcc(b, CC_NE);
    } else if (align) last = (int32_t)mi->size - 1;
    if (mi->fp) {
        x_test_i(b, 4, CPU(msr), PPC_MSR_FP);
        s->sites[s->nsites++] = x_jcc(b, CC_E);
    }
    if (mi->fp && mi->store && mi->size == 4) {
        x_load(b, 4, XDI, XR(XCX));
        x_load(b, 8, XCX, FPR(d));
        x_call_abs(b, (const void *)JIT_FP_TO_SINGLE);
        x_load(b, 4, X10, XR(XAX));
        x_load(b, 4, XCX, XR(XDI));
    }
    x_load(b, 4, XAX, XR(XCX));
    x_shift_i(b, SH_SHR, 4, XR(XAX), 7);
    x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)((TLB_SIZE - 1u) << 5));
    if (last) x_lea(b, 4, XDX, XM(XCX, last));
    else x_load(b, 4, XDX, XR(XCX));
    x_alu_i(b, ALU_AND, 4, XR(XDX), (int32_t)PAGE_MASK);
    x_alu_i(b, ALU_OR, 4, XR(XDX), 1);
    x_alu_rm(b, ALU_CMP, 4, XDX, XMI(set, XAX, 1, 0));
    s->sites[s->nsites++] = x_jcc(b, CC_NE);
    if (g->j->soft) {
        x_load(b, 8, XDX, XMI(set, XAX, 1, 8));
        x_load(b, 1, X8, XMI(set, XAX, 1, 4));
        x_store(b, 1, XM(XDX, 0), X8);
    }
    x_load(b, 8, XAX, XMI(set, XAX, 1, 16));
    host = XMI(XAX, XCX, 1, 0);
    if (!mi->store) {
        if (mi->fp && mi->size == 8) {
            x_load(b, 8, XDX, host);
            x_bswap64(b, XDX);
            x_store(b, 8, FPR(d), XDX);
        } else if (mi->fp) {
            x_load(b, 4, XDX, host);
            x_bswap(b, XDX);
            x_load(b, 4, XDI, XR(XCX));
            x_load(b, 4, XCX, XR(XDX));
            x_call_abs(b, (const void *)JIT_FP_FROM_SINGLE);
            x_store(b, 8, FPR(d), XAX);
            x_load(b, 4, XCX, XR(XDI));
        } else {
            if (mi->size == 4) {
                x_load(b, 4, XDX, host);
                if (!mi->rev) x_bswap(b, XDX);
            } else if (mi->size == 2) {
                x_movzx(b, XDX, host, 2);
                if (!mi->rev) x_shift_i(b, SH_ROR, 2, XR(XDX), 8);
                if (mi->sign) x_movsx(b, XDX, XR(XDX), 2);
            } else x_movzx(b, XDX, host, 1);
            x_store(b, 4, GPR(d), XDX);
        }
    } else if (mi->fp && mi->size == 8) {
        x_load(b, 8, XDX, FPR(d));
        x_bswap64(b, XDX);
        x_store(b, 8, host, XDX);
    } else if (mi->fp) {
        x_bswap(b, X10);
        x_store(b, 4, host, X10);
    } else {
        x_load(b, 4, XDX, GPR(d));
        if (mi->size == 4) {
            if (!mi->rev) x_bswap(b, XDX);
        } else if (mi->size == 2 && !mi->rev) x_shift_i(b, SH_ROR, 2, XR(XDX), 8);
        x_store(b, mi->size, host, XDX);
    }
    if (mi->update) x_store(b, 4, GPR(a), XCX);
    s->resume = b->p;
}

/* bc, bclr, bcctr: the CTR decrement and conditions of branch_condition()
 * (ppc_cpu.c); jumps to the returned sites when not taken. */
static unsigned emit_condition(xbuf *b, unsigned bo, unsigned bi, uint8_t **sites)
{
    unsigned n = 0;
    if (!(bo & 4u)) {
        x_alu_i(b, ALU_SUB, 4, CPU(ctr), 1);
        sites[n++] = x_jcc(b, (bo & 2u) ? CC_NE : CC_E);
    }
    if (!(bo & 16u)) {
        x_bt_i(b, 4, 4, CPU(cr), 31u - bi);
        sites[n++] = x_jcc(b, (bo & 8u) ? CC_AE : CC_B);
    }
    return n;
}

/* tw, twi (trap() of ppc_cpu.c): the trap itself is the interpreter's.
 * 0: it always traps. */
static int emit_trap(gen *g, int immediate)
{
    static const struct { unsigned bit, cc; } tests[5] = {{16, CC_L}, {8, CC_G}, {4, CC_E}, {2, CC_B}, {1, CC_A}};
    xbuf *b = &g->b;
    const unsigned to = (g->op >> 21) & 31u, a = (g->op >> 16) & 31u, rb = (g->op >> 11) & 31u;
    cold *s;
    if ((to & 28u) == 28u || (to & 7u) == 7u) return 0;
    if (!to) return 1;
    s = new_cold(g, C_PRIVILEGED);
    x_load(b, 4, XAX, GPR(a));
    if (immediate) x_alu_i(b, ALU_CMP, 4, XR(XAX), (int16_t)(g->op & 0xffffu));
    else x_alu_rm(b, ALU_CMP, 4, XAX, GPR(rb));
    for (unsigned i = 0; i < 5; ++i)
        if (to & tests[i].bit) s->sites[s->nsites++] = x_jcc(b, tests[i].cc);
    return 1;
}

/* SPRs that are storage (spr_read/spr_write of ppc_cpu.c, ppc_model_spr_read/
 * _write of ppc_model.c): the field's offset, the mask a write keeps, and
 * whether a write changes what the run loop checks between blocks (the
 * translation state, breakpoints, the 601's HID0[LM]). MPC601UM 3.3.6
 * tables 3-40/41; PEM 2.3; MPC603eUM 2.1.2; MPC604eUM 2.1.2; MPC750UM 2.1.2.
 * The time base, DEC and the 601's RTC are the interpreter's (is_decline);
 * so are SPRs a model does not have. -1: not storage. */
#define SPR_AT(f) (int)offsetof(ppc_cpu, f)
static int spr_move(ppc_model model, unsigned spr, int store, uint32_t *mask, int *end)
{
    const unsigned features = ppc_model_information(model)->features;
    const int six = model == PPC_MODEL_601, soft = (features & PPC_MODEL_SOFT_TLB) != 0;
    const int l2 = (features & PPC_MODEL_L2) != 0, pmc2 = (features & PPC_MODEL_PMC2) != 0, pmc4 = (features & PPC_MODEL_PMC4) != 0;
    *mask = UINT32_MAX;
    *end = 0;
    switch (spr) {
    case 0: return six ? SPR_AT(mq) : -1;
    case 18: return SPR_AT(dsisr);
    case 19: return SPR_AT(dar);
    case 25: *end = store; return SPR_AT(sdr1);
    case 26: return SPR_AT(srr0);
    case 27: return SPR_AT(srr1);
    case 282: return SPR_AT(ear);
    case 287: return store ? -1 : SPR_AT(pvr);
    case 936: return l2 && !store ? SPR_AT(mmcr[0]) : -1;
    case 937: case 938: return l2 && !store ? SPR_AT(pmc) + 4 * (int)(spr - 937u) : -1;
    case 939: return l2 && !store ? SPR_AT(sia) : -1;
    case 940: return l2 && !store ? SPR_AT(mmcr[1]) : -1;
    case 941: case 942: return l2 && !store ? SPR_AT(pmc) + 4 * (int)(spr - 939u) : -1;
    case 952: return pmc2 ? SPR_AT(mmcr[0]) : -1;
    case 953: case 954: return pmc2 ? SPR_AT(pmc) + 4 * (int)(spr - 953u) : -1;
    case 955: return pmc2 ? SPR_AT(sia) : -1;
    case 956: return pmc4 ? SPR_AT(mmcr[1]) : -1;
    case 957: case 958: return pmc4 ? SPR_AT(pmc) + 4 * (int)(spr - 955u) : -1;
    case 959: return model == PPC_MODEL_604 || model == PPC_MODEL_604E ? SPR_AT(sda) : -1;
    case 976: return soft ? SPR_AT(dmiss) : -1;
    case 977: *mask = 0xffffffbfu; return soft ? SPR_AT(dcmp) : -1;
    case 978: return soft && !store ? SPR_AT(hash1) : -1;
    case 979: return soft && !store ? SPR_AT(hash2) : -1;
    case 980: return soft ? SPR_AT(imiss) : -1;
    case 981: *mask = 0xffffffbfu; return soft ? SPR_AT(icmp) : -1;
    case 982: *mask = 0xfffff1fbu; return soft ? SPR_AT(rpa) : -1;
    case 1008: *end = store; if (!six) *mask = ~0x00000c00u; return SPR_AT(hid[0]);
    case 1009: *end = store; return six || !store ? SPR_AT(hid[1]) : -1;
    case 1010: *end = store; return six ? SPR_AT(hid[2]) : SPR_AT(iabr);
    case 1013: *end = store; return six ? SPR_AT(hid[5]) : soft ? -1 : SPR_AT(dabr);
    case 1017: *mask = ~1u; return l2 ? SPR_AT(l2cr) : -1;
    case 1019: *mask = 0x1ffu; return l2 ? SPR_AT(ictc) : -1;
    case 1020: case 1021: *mask = 0x3f800007u; return l2 ? SPR_AT(thrm) + 4 * (int)(spr - 1020u) : -1;
    case 1022: *mask = 0x3fffu; return l2 ? SPR_AT(thrm[2]) : -1;
    case 1023: *end = store; return six ? SPR_AT(hid[15]) : -1;
    default:
        if (spr >= 272 && spr <= 275) return SPR_AT(sprg) + 4 * (int)(spr - 272u);
        if (spr >= 528 && spr <= 535) { *end = store; return SPR_AT(bat) + 4 * (int)(spr - 528u); }
        if (!six && spr >= 536 && spr <= 543) { *end = store; return SPR_AT(dbat) + 4 * (int)(spr - 536u); }
        return -1;
    }
}

/* Returns 1 when the block ends with this instruction. */
static int emit_insn(gen *g)
{
    xbuf *b = &g->b;
    const uint32_t op = g->op, pc = g->pc;
    const unsigned primary = op >> 26, d = (op >> 21) & 31u, a = (op >> 16) & 31u, rb = (op >> 11) & 31u;
    const unsigned xo = (op >> 1) & 1023u, base = xo & 511u;
    const int32_t simm = (int16_t)(op & 0xffffu);
    const uint32_t uimm = op & 0xffffu;
    const int rc = (op & 1u) != 0;
    meminfo mi;

    if (mem_decode(op, &mi)) {
        if (g->nofast || (mi.update && (!a || (!mi.store && !mi.fp && a == d)))) return emit_helper(g, 0);
        emit_mem(g, &mi);
        ++g->inl;
        return 0;
    }
    switch (primary) {
    case 3:
        if (emit_trap(g, 1)) break;
        return emit_helper(g, 0);
    case 46: case 47: return emit_fast(g, (const void *)fast_multiple);
    case 59: return emit_fast(g, (const void *)fast_fp);
    case 63:
        if ((xo == 72 || xo == 40 || xo == 136 || xo == 264) && !a && !rc && !g->nofast) {
            /* fmr, fneg, fnabs, fabs (ppc_fpu): the bits moved; with the FPU off,
             * or NI on the models that then zero a denormalized operand, the
             * interpreter's. */
            cold *s = new_cold(g, C_PRIVILEGED);
            x_test_i(b, 4, CPU(msr), PPC_MSR_FP);
            s->sites[s->nsites++] = x_jcc(b, CC_E);
            if (g->j->model >= PPC_MODEL_604) {
                x_test_i(b, 4, CPU(fpscr), PPC_FP_NI);
                s->sites[s->nsites++] = x_jcc(b, CC_NE);
            }
            x_load(b, 8, XAX, FPR(rb));
            if (xo != 72) x_bt_i(b, xo == 40 ? 7 : xo == 136 ? 5 : 6, 8, XR(XAX), 63);
            x_store(b, 8, FPR(d), XAX);
            break;
        }
        return emit_fast(g, (const void *)fast_fp);
    case 7:
        x_load(b, 4, XAX, GPR(a));
        x_mov_ri(b, XCX, (uint32_t)simm);
        x_imul(b, 4, XAX, XR(XCX));
        x_store(b, 4, GPR(d), XAX);
        break;
    case 8:
        x_load(b, 4, XAX, GPR(a));
        x_grp3(b, 2, 4, XR(XAX));
        x_stc(b);
        x_alu_i(b, ALU_ADC, 4, XR(XAX), simm);
        x_setcc(b, CC_B, XR(X8));
        x_store(b, 4, GPR(d), XAX);
        emit_xer(b, 1, 0);
        break;
    case 10: case 11:
        if (op & 0x00200000u) return emit_helper(g, 0);
        x_load(b, 4, XAX, GPR(a));
        x_alu_i(b, ALU_CMP, 4, XR(XAX), primary == 10 ? (int32_t)uimm : simm);
        emit_cr_flags(b, d >> 2, primary == 11);
        break;
    case 12: case 13:
        x_load(b, 4, XAX, GPR(a));
        x_alu_i(b, ALU_ADD, 4, XR(XAX), simm);
        x_setcc(b, CC_B, XR(X8));
        x_store(b, 4, GPR(d), XAX);
        emit_xer(b, 1, 0);
        if (primary == 13) emit_record(b, XAX);
        break;
    case 14: case 15: {
        const int32_t imm = primary == 14 ? simm : (int32_t)((uint32_t)simm << 16);
        if (a) {
            x_load(b, 4, XAX, GPR(a));
            if (imm) x_alu_i(b, ALU_ADD, 4, XR(XAX), imm);
            x_store(b, 4, GPR(d), XAX);
        } else x_mov_mi(b, 4, GPR(d), (uint32_t)imm);
        break;
    }
    case 16: {
        uint32_t target = (op & 2u ? 0u : pc) + (uint32_t)(int32_t)(int16_t)(op & 0xfffcu);
        uint8_t *sites[2];
        unsigned n;
        if ((d & 0x14u) == 0x14u) {
            if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
            emit_goto(g, 0, target, g->k + 1u, g->inl + 1u, pc, op);
            ++g->inl;
            return 1;
        }
        n = emit_condition(b, d, a, sites);
        if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
        emit_goto(g, 0, target, g->k + 1u, g->inl + 1u, pc, op);
        for (unsigned i = 0; i < n; ++i) x_bind(b, sites[i]);
        if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
        break;
    }
    case 17: return emit_helper(g, 1);
    case 18: {
        uint32_t disp = op & 0x03fffffcu;
        if (disp & 0x02000000u) disp |= UINT32_C(0xfc000000);
        if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
        emit_goto(g, 0, (op & 2u ? 0u : pc) + disp, g->k + 1u, g->inl + 1u, pc, op);
        ++g->inl;
        return 1;
    }
    case 19:
        switch (xo) {
        case 0:
            x_load(b, 4, XAX, CPU(cr));
            x_shift_i(b, SH_SHR, 4, XR(XAX), 28u - (a >> 2) * 4u);
            x_alu_i(b, ALU_AND, 4, XR(XAX), 15);
            emit_cr_insert(b, d >> 2, XAX);
            break;
        case 16: case 528: {
            uint8_t *sites[2];
            unsigned n;
            const int always = (d & 0x14u) == 0x14u;
            if (xo == 528 && !(d & 4u)) return emit_helper(g, 0);
            x_load(b, 4, XAX, xo == 16 ? CPU(lr) : CPU(ctr));
            x_alu_i(b, ALU_AND, 4, XR(XAX), ~3);
            n = emit_condition(b, d, a, sites);
            if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
            emit_goto(g, 1, 0, g->k + 1u, g->inl + 1u, pc, op);
            ++g->inl;
            if (always) return 1;
            for (unsigned i = 0; i < n; ++i) x_bind(b, sites[i]);
            if (op & 1u) x_mov_mi(b, 4, CPU(lr), pc + 4u);
            return 0;
        }
        case 50: return emit_helper(g, 1);
        case 150: break;                                            /* isync: coherent */
        case 33: case 129: case 193: case 225: case 257: case 289: case 417: case 449:
            x_load(b, 4, XAX, CPU(cr));
            x_load(b, 4, XCX, XR(XAX));
            x_shift_i(b, SH_SHR, 4, XR(XCX), 31u - a);
            x_load(b, 4, XDX, XR(XAX));
            x_shift_i(b, SH_SHR, 4, XR(XDX), 31u - rb);
            if (xo == 129 || xo == 417) x_grp3(b, 2, 4, XR(XDX));
            x_alu(b, xo == 129 || xo == 225 || xo == 257 ? ALU_AND : xo == 193 || xo == 289 ? ALU_XOR : ALU_OR,
                  4, XR(XCX), XR(XDX));
            if (xo == 33 || xo == 225 || xo == 289) x_grp3(b, 2, 4, XR(XCX));
            x_alu_i(b, ALU_AND, 4, XR(XCX), 1);
            x_shift_i(b, SH_SHL, 4, XR(XCX), 31u - d);
            x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)~(UINT32_C(1) << (31u - d)));
            x_alu(b, ALU_OR, 4, XR(XAX), XR(XCX));
            x_store(b, 4, CPU(cr), XAX);
            break;
        default: return emit_helper(g, 0);
        }
        break;
    case 20: case 21: case 23: {
        const uint32_t m = rot_mask((op >> 6) & 31u, (op >> 1) & 31u);
        x_load(b, 4, XAX, GPR(d));
        if (primary == 23) {
            x_load(b, 4, XCX, GPR(rb));
            x_shift_cl(b, SH_ROL, 4, XR(XAX));
        } else if (rb) x_shift_i(b, SH_ROL, 4, XR(XAX), rb);
        if (m != UINT32_MAX) x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)m);
        if (primary == 20) {
            x_load(b, 4, XCX, GPR(a));
            x_alu_i(b, ALU_AND, 4, XR(XCX), (int32_t)~m);
            x_alu(b, ALU_OR, 4, XR(XAX), XR(XCX));
        }
        x_store(b, 4, GPR(a), XAX);
        if (rc) emit_record(b, XAX);
        break;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: {
        const uint32_t imm = primary & 1u ? uimm << 16 : uimm;
        x_load(b, 4, XAX, GPR(d));
        if (imm || primary >= 28)
            x_alu_i(b, primary <= 25 ? ALU_OR : primary <= 27 ? ALU_XOR : ALU_AND, 4, XR(XAX), (int32_t)imm);
        x_store(b, 4, GPR(a), XAX);
        if (primary >= 28) emit_record(b, XAX);
        break;
    }
    case 31:
        switch (base) {
        case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202: case 232: case 234: case 266:
            emit_add(g, base);
            ++g->inl;
            return 0;
        case 235:
            x_load(b, 4, XAX, GPR(a));
            x_imul(b, 4, XAX, GPR(rb));
            x_setcc(b, CC_O, XR(X9));
            x_store(b, 4, GPR(d), XAX);
            if (op & 0x400u) emit_xer(b, 0, 1);
            if (rc) emit_record(b, XAX);
            ++g->inl;
            return 0;
        case 459: case 491:
            emit_divide(g, base == 491);
            ++g->inl;
            return 0;
        default: break;
        }
        if (emit_logic(g, xo)) break;
        switch (xo) {
        case 0: case 32:
            if (op & 0x00200000u) return emit_helper(g, 0);
            x_load(b, 4, XAX, GPR(a));
            x_alu_rm(b, ALU_CMP, 4, XAX, GPR(rb));
            emit_cr_flags(b, d >> 2, xo == 0);
            break;
        case 11: case 75:
            x_load(b, 4, XAX, GPR(a));
            x_grp3(b, xo == 11 ? 4 : 5, 4, GPR(rb));
            x_store(b, 4, GPR(d), XDX);
            if (rc) emit_record(b, XDX);
            break;
        case 4:
            if (emit_trap(g, 0)) break;
            return emit_helper(g, 0);
        case 19:
            x_load(b, 4, XAX, CPU(cr));
            x_store(b, 4, GPR(d), XAX);
            break;
        case 83: {
            cold *s = new_cold(g, C_PRIVILEGED);
            x_test_i(b, 4, CPU(msr), PPC_MSR_PR);
            s->sites[s->nsites++] = x_jcc(b, CC_NE);
            x_load(b, 4, XAX, CPU(msr));
            x_store(b, 4, GPR(d), XAX);
            break;
        }
        case 512:
            x_load(b, 4, XAX, CPU(xer));
            x_shift_i(b, SH_SHR, 4, XR(XAX), 28);
            x_alu_i(b, ALU_AND, 4, CPU(xer), 0x1fffffff);
            emit_cr_insert(b, d >> 2, XAX);
            break;
        case 20: return emit_fast(g, (const void *)fast_lwarx);
        case 150: return emit_fast(g, (const void *)fast_stwcx);
        case 533: case 597: case 661: case 725: return emit_fast(g, (const void *)fast_string);
        case 1014: return emit_fast(g, (const void *)fast_dcbz);
        case 144: {
            uint32_t m = 0;
            for (unsigned f = 0; f < 8; ++f) if (op & (1u << (19u - f))) m |= UINT32_C(15) << (28u - 4u * f);
            x_load(b, 4, XAX, GPR(d));
            x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)m);
            x_load(b, 4, XCX, CPU(cr));
            x_alu_i(b, ALU_AND, 4, XR(XCX), (int32_t)~m);
            x_alu(b, ALU_OR, 4, XR(XCX), XR(XAX));
            x_store(b, 4, CPU(cr), XCX);
            break;
        }
        case 339: case 467: {
            const unsigned spr = a | (rb << 5);
            if (spr != 1 && spr != 8 && spr != 9) {
                uint32_t mask;
                int end;
                const int offset = spr_move(g->j->model, spr, xo == 467, &mask, &end);
                if (offset < 0) return emit_helper(g, xo == 467);
                if (spr & 16u) {
                    cold *s = new_cold(g, C_PRIVILEGED);
                    x_test_i(b, 4, CPU(msr), PPC_MSR_PR);
                    s->sites[s->nsites++] = x_jcc(b, CC_NE);
                }
                if (xo == 339) {
                    x_load(b, 4, XAX, XM(XBX, offset));
                    x_store(b, 4, GPR(d), XAX);
                } else {
                    x_load(b, 4, XAX, GPR(d));
                    if (mask != UINT32_MAX) x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)mask);
                    x_store(b, 4, XM(XBX, offset), XAX);
                    if (end) {
                        emit_leave(g, g->k + 1u, g->inl + 1u, pc, op);
                        ++g->inl;
                        return 1;
                    }
                }
                break;
            }
            if (xo == 339) {
                x_load(b, 4, XAX, spr == 1 ? CPU(xer) : spr == 8 ? CPU(lr) : CPU(ctr));
                if (spr == 1 && g->j->model != PPC_MODEL_601) x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)0xe000007fu);
                x_store(b, 4, GPR(d), XAX);
            } else {
                x_load(b, 4, XAX, GPR(d));
                if (spr == 1)
                    x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)(g->j->model == PPC_MODEL_601 ? 0xe000ff7fu : 0xe000007fu));
                x_store(b, 4, spr == 1 ? CPU(xer) : spr == 8 ? CPU(lr) : CPU(ctr), XAX);
            }
            break;
        }
        case 54: case 86: case 246: case 278: case 598: case 854: case 982:
            break;                                                  /* cache and ordering: coherent */
        case 146: {
            /* MPC601UM / MPC604EUM / MPC750UM MSR masks. The 603's
             * TGPR register-bank exchange remains on its reference path.
             * IR/DR/PR select separate JIT TLB sets; MSR writes do not
             * alter SR/BAT/SDR1 or require comparing those snapshots. */
            if (g->j->soft) return emit_helper(g, 1);
            cold *s = new_cold(g, C_PRIVILEGED);
            x_test_i(b, 4, CPU(msr), PPC_MSR_PR);
            s->sites[s->nsites++] = x_jcc(b, CC_NE);
            x_load(b, 4, XAX, GPR(d));
            x_alu_i(b, ALU_AND, 4, XR(XAX), (int32_t)ppc_model_information(g->j->model)->msr_mask);
            x_store(b, 4, CPU(msr), XAX);
            emit_leave_reason(g, g->k + 1u, g->inl + 1u, pc, op, EXIT_MODE);
            ++g->inl;
            return 1;
        }
        case 210: case 242: case 306: case 370: case 566: case 978: case 1010:
            return emit_helper(g, 1);
        default: return emit_helper(g, 0);
        }
        break;
    default: return emit_helper(g, 0);
    }
    ++g->inl;
    return 0;
}

static void emit_cold(gen *g)
{
    xbuf *b = &g->b;
    for (unsigned i = 0; i < g->ncold; ++i) {
        cold *s = &g->cold[i];
        for (unsigned k = 0; k < s->nsites; ++k) x_bind(b, s->sites[k]);
        if (s->kind == C_SLOW) {
            x_load(b, 4, X9, XR(XCX));
            emit_call(b, (const void *)mem_slow, s->op, s->pc, s->steps - 1u);
            x_test(b, 4, XR(XAX), XAX);
            x_jcc_to(b, CC_E, s->resume);
        }
        if (s->kind == C_PRIVILEGED)
            emit_call(b, (const void *)helper, s->op, s->pc, s->steps - 1u);
        emit_exit(b, g->j, s->steps, s->inl, EXIT_HELPER);
    }
}

/* Translates up to `max` instructions (the first not a time base access);
 * returns how many. */
static unsigned generate(gen *g, const uint32_t *words, unsigned max)
{
    xbuf *b = &g->b;
    uint8_t *need_at, *budget;
    uint32_t lpc = 0, lop = 0;
    unsigned n = 0, k;
    int ended = 0;
    g->entry = b->p;
    g->inl = 0;
    g->ncold = 0;
    need_at = x_alu_i32(b, ALU_CMP, 8, XR(X13), 0);
    budget = x_jcc(b, CC_L);
    if (g->j->soft && !g->nofast) {
        /* the 603's fetch translation sets its TLB's LRU byte (the entry the
           dispatcher or ppc_jit_run checked) */
        const int32_t at = (int32_t)(((g->block_pc >> 12) & (TLB_SIZE - 1u)) * sizeof(jtlb));
        x_load(b, 8, X9, XM(XSI, at + 8));
        x_load(b, 1, X10, XM(XSI, at + 4));
        x_store(b, 1, XM(X9, 0), X10);
    }
    choose_gpr_cache(g, words, max);
    if (g->cache_reg >= 0) {
        x_load(b, 4, XDI, XM(XBX, (int32_t)(offsetof(ppc_cpu, gpr) + 4u * (unsigned)g->cache_reg)));
        ++g->j->stats.cached_gpr_blocks;
    }
    for (k = 0; k < max && !ended; ++k) {
        if (k && is_decline(words[k])) break;
        if (k >= g->cache_until) g->cache_reg = -1;
        g->k = k;
        g->pc = g->block_pc + 4u * k;
        g->op = words[k];
        ended = emit_insn(g);
        lpc = g->pc;
        lop = g->op;
        n = k + 1u;
        ++g->j->stats.instructions;
    }
    if (!ended) emit_goto(g, 0, g->block_pc + 4u * n, n, g->inl, lpc, lop);
    emit_cold(g);
    x_bind(b, budget);
    x_mov_mi(b, 4, CPU(pc), g->block_pc);
    x_mov_ri(b, XAX, EXIT_BUDGET);
    x_jmp_to(b, g->j->epilogue);
    memcpy(need_at, &n, 4);
    return n;
}

/* --- blocks ----------------------------------------------------------------------------- */

static void flush_code(ppc_jit *j)
{
    j->code_free = j->code_start;
    j->nblocks = 0;
    memset(j->hash, 0, sizeof j->hash);
    memset(j->fast, 0xff, sizeof j->fast);
    for (unsigned i = 0; i < PAGE_SLOTS; ++i) {
        jpage *p = j->pages[i];
        if (!p) continue;
        p->flags &= ~(unsigned)P_CODE;
        p->blocks = NULL;
        memset(p->words, 0, sizeof p->words);
    }
    ++j->stats.code_flushes;
}

static jblock *translate(ppc_jit *j, uint32_t pc, uint8_t *host, intptr_t addend)
{
    static gen g;
    uint32_t words[MAX_INSNS];
    const unsigned offset = pc & 0xfffu;
    unsigned max = (4096u - offset) / 4u, n;
    jblock *k;
    jpage *p;
    if (max > j->max_block) max = j->max_block;
    for (unsigned i = 0; i < max; ++i) words[i] = be32(host + offset + 4u * i);
    if ((size_t)(j->code_end - j->code_free) < CODE_RESERVE || j->nblocks >= MAX_BLOCKS) {
        flush_code(j);
        tlb_flush(j);
    }
    p = page_get(j, host);
    if (!p) return NULL;
    if (!(p->flags & P_CODE)) {
        p->flags |= P_CODE;
        purge_writes(j, host);
    }
    memset(&g.b, 0, sizeof g.b);
    g.j = j;
    g.b.p = j->code_free;
    g.b.end = j->code_end;
    g.block_pc = pc;
    g.nofast = 0;
    n = generate(&g, words, max);
#if MC_PPC_A64
    if (g.b.bad && !g.b.overflow) {
        /* A flag test the ARM64 flags could not answer: the block again,
         * each instruction through the interpreter's helper. */
        memset(&g.b, 0, sizeof g.b);
        g.b.p = j->code_free;
        g.b.end = j->code_end;
        g.nofast = 1;
        n = generate(&g, words, max);
        if (g.b.bad) g.b.overflow = 1;
    }
#endif
    if (g.b.overflow) {
        flush_code(j);
        return NULL;
    }
    k = &j->blocks[j->nblocks++];
    k->pc = pc;
    k->first = (uint16_t)(offset >> 2);
    k->count = (uint16_t)n;
    k->host = host;
    k->addend = addend;
    k->code = (uint8_t *)g.entry;
    {
        jblock **h = &j->hash[block_hash(pc, host)];
        k->hnext = *h;
        *h = k;
    }
    k->pnext = p->blocks;
    p->blocks = k;
    for (unsigned i = k->first; i < (unsigned)k->first + n; ++i) p->words[i >> 5] |= 1u << (i & 31u);
    j->code_free = (uint8_t *)(((uintptr_t)g.b.p + 15u) & ~(uintptr_t)15u);
#if MC_PPC_A64
    a64_sync(g.entry, (size_t)(g.b.p - g.entry));
#endif
    ++j->stats.blocks;
    return k;
}

static jblock *find(ppc_jit *j, uint32_t pc, const uint8_t *host)
{
    for (jblock *k = j->hash[block_hash(pc, host)]; k; k = k->hnext)
        if (k->pc == pc && k->host == host) return k;
    return NULL;
}

/* --- the stubs: entry, epilogue, dispatcher ----------------------------------------------- */

static void build_stubs(ppc_jit *j)
{
    xbuf b = {j->code, j->code + 4096, 0};
    uint8_t *miss[3];
#if MC_PPC_A64
    /* epilogue: RAX's X4 = the exit's reason, returned in X0; the budget
       left to j->left; X19-X28, X29, X30 restored (AAPCS64) */
    j->epilogue = b.p;
    x_store(&b, 8, JIT(left), X13);
    a64_mov(&b, 1, 0, A64R(XAX));
    for (int i = 0; i < 5; i++) a64_ldp(&b, 19u + 2u * (unsigned)i, 20u + 2u * (unsigned)i, 2 + 2 * i);
    a64_ldp_post(&b, 29, 30, 12);
    x_ret(&b);
    /* enter(cpu, jit, code, budget): the arguments in X0-X3, RCX RDX R8 R9's */
    b.p = (uint8_t *)(((uintptr_t)b.p + 15u) & ~(uintptr_t)15u);
    j->enter = (enter_fn)(void *)b.p;
    a64_stp_pre(&b, 29, 30, -12);
    for (int i = 0; i < 5; i++) a64_stp(&b, 19u + 2u * (unsigned)i, 20u + 2u * (unsigned)i, 2 + 2 * i);
#else
    static const int saved[8] = {XBX, XBP, XSI, XDI, X12, X13, X14, X15};
    /* epilogue: eax = the exit's reason; the budget left to j->left */
    j->epilogue = b.p;
    x_store(&b, 8, JIT(left), X13);
    x_alu_i(&b, ALU_ADD, 8, XR(XSP), 40);
    for (int i = 7; i >= 0; --i) x_pop(&b, saved[i]);
    x_ret(&b);
    /* enter(cpu, jit, code, budget) */
    b.p = (uint8_t *)(((uintptr_t)b.p + 15u) & ~(uintptr_t)15u);
    j->enter = (enter_fn)(void *)b.p;
    for (int i = 0; i < 8; ++i) x_push(&b, saved[i]);
    x_alu_i(&b, ALU_SUB, 8, XR(XSP), 40);
#endif
    x_load(&b, 8, XBX, XR(XCX));
    x_load(&b, 8, XBP, XR(XDX));
    x_load(&b, 8, X13, XR(X9));
    x_load(&b, 8, XSI, JIT(cur_fetch));
    x_load(&b, 8, X14, JIT(cur_read));
    x_load(&b, 8, X15, JIT(cur_write));
    x_lea(&b, 8, X12, JIT(fast));
    x_jmp_m(&b, XR(X8));
    /* dispatch: ecx = pc (stored) */
    b.p = (uint8_t *)(((uintptr_t)b.p + 15u) & ~(uintptr_t)15u);
    j->dispatch = b.p;
    x_load(&b, 4, XAX, XR(XCX));
    x_shift_i(&b, SH_SHR, 4, XR(XAX), 2);
    x_alu_i(&b, ALU_AND, 4, XR(XAX), FAST_SIZE - 1u);
    x_shift_i(&b, SH_SHL, 4, XR(XAX), 5);
    x_lea(&b, 8, XDX, XMI(X12, XAX, 1, 0));
    x_alu_mr(&b, ALU_CMP, 4, XM(XDX, 0), XCX);
    miss[0] = x_jcc(&b, CC_NE);
    x_load(&b, 4, XAX, XR(XCX));
    x_shift_i(&b, SH_SHR, 4, XR(XAX), 12);
    x_alu_i(&b, ALU_AND, 4, XR(XAX), TLB_SIZE - 1u);
    x_shift_i(&b, SH_SHL, 4, XR(XAX), 5);
    x_load(&b, 4, X8, XR(XCX));
    x_alu_i(&b, ALU_AND, 4, XR(X8), (int32_t)PAGE_MASK);
    x_alu_i(&b, ALU_OR, 4, XR(X8), 1);
    x_alu_mr(&b, ALU_CMP, 4, XMI(XSI, XAX, 1, 0), X8);
    miss[1] = x_jcc(&b, CC_NE);
    x_load(&b, 8, X8, XMI(XSI, XAX, 1, 16));
    x_alu_rm(&b, ALU_CMP, 8, X8, XM(XDX, 8));
    miss[2] = x_jcc(&b, CC_NE);
    x_jmp_m(&b, XM(XDX, 16));
    for (int i = 0; i < 3; ++i) x_bind(&b, miss[i]);
    x_mov_ri(&b, XAX, EXIT_DISPATCH);
    x_jmp_to(&b, j->epilogue);
    j->code_start = (uint8_t *)(((uintptr_t)b.p + 63u) & ~(uintptr_t)63u);
    j->code_free = j->code_start;
#if MC_PPC_A64
    a64_sync(j->code, (size_t)(b.p - j->code));
#endif
}

static ppc_jit *jit_new(ppc_cpu *cpu, uint8_t *const *read_map, uint8_t *const *write_map, const unsigned *generation)
{
    ppc_jit *j = calloc(1, sizeof *j);
    if (!j) return NULL;
#ifdef _WIN32
    j->code = VirtualAlloc(NULL, CODE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
    j->code = mmap(NULL, CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (j->code == MAP_FAILED) j->code = NULL;
#endif
    j->blocks = calloc(MAX_BLOCKS, sizeof *j->blocks);
    j->pages = calloc(PAGE_SLOTS, sizeof *j->pages);
    j->cycle.step = calloc(IDLE_STEPS, sizeof *j->cycle.step);
    if (!j->code || !j->blocks || !j->pages || !j->cycle.step) {
#ifdef _WIN32
        if (j->code) VirtualFree(j->code, 0, MEM_RELEASE);
#else
        if (j->code) munmap(j->code, CODE_SIZE);
#endif
        free(j->blocks);
        free(j->pages);
        free(j->cycle.step);
        free(j);
        return NULL;
    }
    j->code_end = j->code + CODE_SIZE;
    j->cpu = cpu;
    j->model = cpu->model;
    j->soft = (ppc_model_information(cpu->model)->features & PPC_MODEL_SOFT_TLB) != 0;
    j->read_map = read_map;
    j->write_map = write_map;
    j->generation = generation;
    j->seen_generation = generation ? *generation : 0;
    j->outside = 1;
    j->max_block = MAX_INSNS;
    j->idle_enabled = 1;
    j->lm = (uint8_t)lm_mode(cpu);
    memset(j->fast, 0xff, sizeof j->fast);
    build_stubs(j);
    set_modes(j);
    take_snap(cpu, &j->snap);
    memcpy(j->soft_tlb, cpu->tlb, sizeof j->soft_tlb);
    j->soft_seen = cpu->tlb_writes;
    return j;
}

ppc_jit *ppc_jit_create(ppc_cpu *cpu, uint8_t *const *read_map, uint8_t *const *write_map, const unsigned *generation)
{
    ppc_jit *j;
    if (!cpu || !read_map || !write_map) return NULL;
    j = jit_new(cpu, read_map, write_map, generation);
    if (!j) return NULL;
    j->bus = cpu->bus;
    cpu->bus.opaque = j;
    cpu->bus.read = wrap_read;
    cpu->bus.write = wrap_write;
    j->wrapped = 1;
    return j;
}

void ppc_jit_destroy(ppc_jit *j)
{
    if (!j) return;
    if (j->wrapped && j->cpu->bus.opaque == j) j->cpu->bus = j->bus;
    for (unsigned i = 0; i < PAGE_SLOTS; ++i) free(j->pages[i]);
    free(j->pages);
    free(j->blocks);
    free(j->cycle.step);
    free(j->profile);
#ifdef _WIN32
    VirtualFree(j->code, 0, MEM_RELEASE);
#else
    munmap(j->code, CODE_SIZE);
#endif
    free(j);
}

void ppc_jit_options(ppc_jit *j, unsigned max_block)
{
    if (!j) return;
    j->max_block = max_block && max_block < MAX_INSNS ? max_block : MAX_INSNS;
    flush_code(j);
}

void ppc_jit_flush(ppc_jit *j)
{
    if (!j) return;
    flush_code(j);
    tlb_flush(j);
}

void ppc_jit_cpu_reset(ppc_jit *j)
{
    if (!j) return;
    j->cpu_reset = 1;
    j->cycle.period = 0;
    j->soft_seen = j->cpu->tlb_writes - 1u;  /* the reset CPU's TLB is compared again */
    tlb_flush(j);
    j->flushed = 1;
}

void ppc_jit_get_stats(const ppc_jit *j, ppc_jit_stats *s)
{
    if (j) *s = j->stats;
    else memset(s, 0, sizeof *s);
}

void ppc_jit_idle_enable(ppc_jit *j, int enabled)
{
    if (j) j->idle_enabled = enabled != 0;
}

void ppc_jit_trap(ppc_jit *j, uint32_t base, uint32_t size, const uint8_t *skip)
{
    if (!j) return;
    j->trap_base = base;
    j->trap_size = size;
    j->trap_skip = skip;
    flush_code(j);
}

void ppc_jit_sync(ppc_jit *j)
{
    if (!j) return;
    /* the bus remapped (a board's control register) since the last run */
    if (j->generation && *j->generation != j->seen_generation) {
        j->seen_generation = *j->generation;
        reset_pages(j);
    }
    mmu_check(j);
}

uint32_t ppc_jit_tlb_generation(const ppc_jit *j) { return j ? j->tlb_generation : 0; }

uint32_t ppc_jit_soft_dropped(ppc_jit *j)
{
    uint32_t dropped;
    if (!j) return 0;
    dropped = j->soft_dropped;
    j->soft_dropped = 0;
    return dropped;
}

int ppc_jit_reset_seen(ppc_jit *j, int clear)
{
    int seen;
    if (!j) return 0;
    seen = j->cpu_reset != 0;
    if (clear) j->cpu_reset = 0;
    return seen;
}

void ppc_jit_set_guard(ppc_jit *j, ppc_jit_guard_fn written_fn, ppc_jit_held_fn held, void *ctx)
{
    if (!j) return;
    j->guard = written_fn;
    j->held = held;
    j->guard_ctx = ctx;
    ++j->tlb_generation;
}

void ppc_jit_guard(ppc_jit *j, uint8_t *host)
{
    jpage *p;
    if (!j || !host) return;
    p = page_get(j, host);
    if (!p) return;             /* the table was full: reset_pages told the caller */
    if (!(p->flags & P_GUARDED)) {
        p->flags |= P_GUARDED;
        /* The inline write entries of the page go; what ppc_jit_data gives
         * for it does not change (a guarded page stays writable to the
         * caller), so the owner is not told. */
        drop_writes(j, host);
    }
}

void ppc_jit_unguard(ppc_jit *j, uint8_t *host)
{
    jpage *p;
    if (!j || !host) return;
    p = page_find(j, host);
    if (p && (p->flags & P_GUARDED)) {
        p->flags &= ~(unsigned)P_GUARDED;
        purge_writes(j, host);      /* its write entries are filled again, inline unless something else holds them */
    }
}

int ppc_jit_data(ppc_jit *j, uint32_t ea, int write, uint8_t **host, uint32_t *pa)
{
    ppc_cpu *c = j->cpu;
    const unsigned kind = write ? K_WRITE : K_READ, mode = data_mode(c), at = (ea >> 12) & (TLB_SIZE - 1u);
    jtlb *t = &j->tlb[kind][mode][at];
    if ((t->tag | 2u) != ((ea & PAGE_MASK) | 3u)) {
        c->result = PPC_OK;
        c->last_exception = 0;
        j->io = 0;
        j->run_base = 0;
        j->give = j->left = 0;
        j->undo = *c;
        if (!fill(j, t, ea, kind)) {
            *c = j->undo;
            return 0;
        }
        c->result = j->undo.result;
        c->last_exception = j->undo.last_exception;
    }
    *t->lru_ptr = t->lru_val;
    *host = (t->tag & 2u) ? NULL : (uint8_t *)(t->addend + (intptr_t)(uintptr_t)ea);
    *pa = j->tlb_pa[kind][mode * TLB_SIZE + at] | (ea & 4095u);
    if (!*host && write && j->write_map) {
        uint8_t *page = j->write_map[*pa >> 12];
        const jpage *p = page ? page_find(j, page) : NULL;
        if (p && !(p->flags & ~(unsigned)P_GUARDED)) *host = page + (ea & 4095u);
    }
    return 1;
}

/* Instructions after which the translation state is compared again. */
static int mmu_op(uint32_t op)
{
    const unsigned xo = (op >> 1) & 1023u;
    if (op >> 26 != 31) return 0;
    return xo == 146 || xo == 210 || xo == 242 || xo == 306 || xo == 370 || xo == 467 || xo == 566 || xo == 978 || xo == 1010;
}

/* The whole architected state of two CPUs, the last step's fields aside. */
static int same_cpu(const ppc_cpu *x, const ppc_cpu *y)
{
    ppc_cpu a, b;
    if (memcmp(x->gpr, y->gpr, sizeof x->gpr) || x->msr != y->msr || x->cr != y->cr || x->lr != y->lr || x->ctr != y->ctr)
        return 0;
    a = *x; b = *y;
    memset(&a.bus, 0, sizeof a.bus); memset(&b.bus, 0, sizeof b.bus);
    a.instructions = b.instructions = 0;
    a.exceptions = b.exceptions = a.tlb_writes = b.tlb_writes = 0;     /* counted, as the instructions */
    a.last_pc = b.last_pc = a.last_opcode = b.last_opcode = 0;
    a.last_exception = b.last_exception = a.mmu_wim = b.mmu_wim = 0;
    return !memcmp(&a, &b, sizeof a);
}

/* Everything but the registers a learned cycle records per step. */
static int same_rest(const ppc_cpu *x, const ppc_cpu *y)
{
    ppc_cpu a = *x, b = *y;
    a.pc = b.pc; a.cr = b.cr; a.xer = b.xer; a.lr = b.lr; a.ctr = b.ctr;
    memcpy(a.gpr, b.gpr, sizeof a.gpr);
    return same_cpu(&a, &b);
}

/* One step of the reference CPU inside a probe, the run's step `at`. 0: the
 * probe ends (before a step that is not its to make, or after one that
 * left the loop's conditions; *steps counts it when it was made). */
static int probe_step(ppc_jit *j, uint64_t at, uint64_t *steps, jcycle_step *record)
{
    ppc_cpu *c = j->cpu;
    uint32_t op;
    set_modes(j);
    jtlb *t = &j->cur_fetch[(c->pc >> 12) & (TLB_SIZE - 1u)];
    j->io = 0;
    j->run_base = at;
    j->give = j->left = 0;
    if ((t->tag | 2u) != ((c->pc & PAGE_MASK) | 3u)) {
        c->result = PPC_OK;
        c->last_pc = c->pc;
        c->last_exception = 0;
        if (!fill(j, t, c->pc, K_FETCH)) { ++*steps; return 0; }
    }
    if ((t->tag & 2u) || is_decline(op = be32(t->host + (c->pc & 4095u)))) return 0;
    if (record) {
        record->pc = c->pc; record->cr = c->cr; record->xer = c->xer; record->lr = c->lr; record->ctr = c->ctr;
        record->word = op; record->at = t->host + (c->pc & 4095u);
        memcpy(record->gpr, c->gpr, sizeof record->gpr);
    }
    j->idle_reads = 0;
    ppc_step(c);
    if (mmu_op(op) || c->result != PPC_OK) mmu_check(j); /* It may have changed this address space. */
    ++*steps;
    return !(j->io || j->idle_reject || c->result == PPC_CHECKSTOP || c->result == PPC_UNIMPLEMENTED);
}

/* Instructions whose every input and effect is among the PC, the GPRs their
 * register fields name, CR, XER, LR and CTR (MPC601UM 10, PEM 8): integer
 * add, subtract, logical, shift, rotate and compare, the CR instructions,
 * branches, and the moves of LR, CTR and XER. Multiplies and divides are
 * left out: the 601 leaves their by-product in MQ (MPC601UM 10). */
static int cycle_op(uint32_t op)
{
    const unsigned primary = op >> 26, xo = (op >> 1) & 1023u, spr = ((op >> 16) & 31u) | ((op >> 6) & 992u);
    switch (primary) {
    case 8: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 18:
    case 20: case 21: case 23: case 24: case 25: case 26: case 27: case 28: case 29:
        return primary != 10 && primary != 11 ? 1 : !(op & 0x00200000u);
    case 19:
        return xo == 0 || xo == 16 || xo == 528 || xo == 33 || xo == 129 || xo == 193 || xo == 225 || xo == 257 ||
               xo == 289 || xo == 417 || xo == 449;
    case 31:
        switch (xo & 511u) {
        case 8: case 10: case 40: case 104: case 136: case 138: case 200: case 202: case 232: case 234: case 266: return 1;
        default: break;
        }
        switch (xo) {
        case 0: case 32: return !(op & 0x00200000u);
        case 28: case 60: case 124: case 284: case 316: case 412: case 444: case 476:
        case 24: case 536: case 792: case 824: case 26: case 922: case 954:
        case 19: case 144: case 512: return 1;
        case 339: case 467: return spr == 1 || spr == 8 || spr == 9;
        default: return 0;
        }
    default: return 0;
    }
}

static int idle_pending(const ppc_cpu *c) { return (c->msr & PPC_MSR_EE) && (c->external_irq || c->decrementer_pending); }

/* Learns the cycle the CPU is at the start of, by a further turn of it on
 * the reference CPU: a cycle of `period` steps in real mode, each an
 * instruction of cycle_op's that takes no exception and leaves everything
 * but its registers as it was. Its steps then depend on the fetched words,
 * the PC, CR, XER, LR, CTR, the MSR and the GPRs a register field of its
 * instructions names, and on nothing else of the machine. A later run that
 * finds itself in any state of it, with the same words there, is at the
 * start of the same `period` steps back to that state (idle_cycle) without
 * the reference CPU making them again. */
static void idle_learn(ppc_jit *j, unsigned period, uint64_t base, uint64_t *steps)
{
    ppc_cpu *c = j->cpu;
    const ppc_cpu start = *c;
    int ok = 1;
    uint32_t gprs = 0;
    j->cycle.period = 0;
    for (unsigned i = 0; i < period; ++i) {
        if (!usable(c) || idle_pending(c)) return;
        if ((c->msr & (PPC_MSR_IR | PPC_MSR_DR)) || !same_rest(&start, c)) ok = 0;
        if (!probe_step(j, base + *steps, steps, &j->cycle.step[i])) return;
        if (c->result != PPC_OK || j->idle_bytes || j->idle_reads != 1u || !cycle_op(j->cycle.step[i].word)) ok = 0;
        gprs |= 1u << ((j->cycle.step[i].word >> 21) & 31u) | 1u << ((j->cycle.step[i].word >> 16) & 31u) |
                1u << ((j->cycle.step[i].word >> 11) & 31u);
    }
    if (!ok || c->pc != start.pc || !same_cpu(&start, c)) return;
    j->cycle.base = start;
    j->cycle.instructions = c->instructions - start.instructions;
    j->cycle.gprs = gprs;
    j->cycle.period = period;
}

/* The steps of the learned cycle's whole turns inside `budget`, when the
 * CPU is in one of its states and its instructions are still there. */
static uint64_t idle_cycle(ppc_jit *j, uint64_t budget)
{
    ppc_cpu *c = j->cpu;
    const unsigned period = j->cycle.period;
    const jcycle_step *s = j->cycle.step;
    unsigned i;
    if (!period || budget < period || c->msr != j->cycle.base.msr) return 0;
    for (i = 0; i < period; ++i) {
        unsigned r;
        if (s[i].pc != c->pc || s[i].cr != c->cr || s[i].xer != c->xer || s[i].lr != c->lr || s[i].ctr != c->ctr) continue;
        for (r = 0; r < 32u && (!(j->cycle.gprs >> r & 1u) || s[i].gpr[r] == c->gpr[r]); ++r) {}
        if (r == 32u) break;
    }
    if (i == period) return 0;
    for (i = 0; i < period; ++i)
        if (be32(s[i].at) != s[i].word) { j->cycle.period = 0; return 0; }
    const uint64_t turns = budget / period;
    c->instructions += turns * j->cycle.instructions;
    j->stats.idle_steps += turns * period;
    return turns * period;
}

/* A stationary loop is folded only after the reference CPU has executed it
 * from this state back to this state: the same PC with every architected
 * register equal and every RAM byte written in between restored, however
 * many iterations that takes (Mac OS 9's idle task rotates its registers:
 * 27 iterations, 1,215 instructions). No time-register instruction or
 * device access is crossed, and no interrupt is pending. The steps up to the
 * machine's next event are then the same deterministic CPU/RAM computation
 * repeated: whole recurrences inside the budget are skipped, the normal
 * machine loop charges their exact steps, updates TB/DEC, and runs devices
 * at the original deadlines. No firmware address or call is substituted.
 * CPU semantics: MPC601UM ch. 5/10, PEM ch. 6/8.
 * Returns the steps taken (interpreted and skipped); *skipped: those skipped. */
static uint64_t idle_probe(ppc_jit *j, uint64_t budget, uint64_t base, uint64_t *skipped)
{
    ppc_cpu *c = j->cpu, initial = *c;
    uint64_t steps = 0;
    const uint64_t limit = budget < IDLE_STEPS ? budget : IDLE_STEPS;
    int pure = 1;               /* real mode, no exception, no memory but the fetches */
    *skipped = 0;
    j->idle_learning = 1;
    j->idle_reject = 0;
    j->idle_bytes = 0;
    ++j->stats.idle_probes;
    while (steps < limit && usable(c) && !idle_pending(c)) {
        if (c->msr & (PPC_MSR_IR | PPC_MSR_DR)) pure = 0;
        if (!probe_step(j, base + steps, &steps, NULL)) break;
        if (c->result != PPC_OK || j->idle_reads != 1u) pure = 0;
        if (c->pc == initial.pc && same_cpu(&initial, c)) {
            unsigned k;
            for (k = 0; k < j->idle_bytes && *j->idle_writes[k].at == j->idle_writes[k].value; ++k) {}
            if (k != j->idle_bytes) continue;
            const uint64_t period = steps;
            if (pure && !j->idle_bytes && budget - steps >= period) {
                idle_learn(j, (unsigned)period, base, &steps);
                /* a turn that ended early or elsewhere proves nothing from here */
                if (j->io || j->idle_reject || c->pc != initial.pc || !same_cpu(&initial, c)) break;
                for (k = 0; k < j->idle_bytes && *j->idle_writes[k].at == j->idle_writes[k].value; ++k) {}
                if (k != j->idle_bytes) break;
            }
            const uint64_t repeats = (budget - steps) / period;
            *skipped = repeats * period;
            c->instructions += repeats * (c->instructions - initial.instructions) / (steps / period);
            c->exceptions += (uint32_t)(repeats * (uint32_t)(c->exceptions - initial.exceptions) / (steps / period));
            c->tlb_writes += (uint32_t)(repeats * (uint32_t)(c->tlb_writes - initial.tlb_writes) / (steps / period));
            j->stats.idle_steps += *skipped;
            steps += *skipped;
            break;
        }
    }
    j->idle_learning = 0;
    mmu_check(j);
    return steps;
}

/* --- running ---------------------------------------------------------------------------- */

uint64_t ppc_jit_run(ppc_jit *j, uint64_t budget)
{
    ppc_cpu *c = j->cpu;
    uint64_t steps = 0;
    const uint64_t run_start = j->profile ? host_ticks() : 0;
    const uint64_t calls = j->stats.helpers + j->stats.memory_slow;
    const uint32_t first_page = (c->pc & PAGE_MASK) | 1u;
    unsigned entered = 0;
    if (budget > (UINT64_C(1) << 40)) budget = UINT64_C(1) << 40;
    if (j->generation && *j->generation != j->seen_generation) {
        j->seen_generation = *j->generation;
        reset_pages(j);
    }
    if (j->outside) {
        mmu_check(j);
        j->outside = 0;
    }
    j->running = 1;
    j->cpu_reset = 0;
    while (steps < budget) {
        uint32_t pc = c->pc, word;
        jtlb *t;
        jblock *k;
        jfast *f;
        int64_t give;
        uint32_t why;
        if ((c->msr & PPC_MSR_EE) && (c->external_irq || c->decrementer_pending)) break;
        if (!usable(c)) break;
        if (pc - j->trap_base < j->trap_size && !((pc - j->trap_base) & 7u) &&
            !(j->trap_skip && j->trap_skip[(pc - j->trap_base) >> 3])) break;           /* the caller's (ppc_jit_trap) */
        set_modes(j);
        t = &j->cur_fetch[(pc >> 12) & (TLB_SIZE - 1u)];
        if ((t->tag | 2u) != ((pc & PAGE_MASK) | 3u)) {
            /* the step's fetch translation, as ppc_step makes it */
            c->result = PPC_OK;
            c->last_pc = pc;
            c->last_exception = 0;
            j->io = 0;
            j->run_base = steps;
            j->give = j->left = 0;
            if (!fill(j, t, pc, K_FETCH)) {
                ++steps;
                if (c->result == PPC_UNIMPLEMENTED || c->result == PPC_CHECKSTOP || j->io) break;
                continue;
            }
        }
        if (t->tag & 2u) break;                                     /* not memory: the interpreter's */
        word = be32(t->host + (pc & 0xfffu));
        if (is_decline(word)) {
            ++j->stats.declines;
            break;
        }
        if (j->idle_enabled && !steps && budget >= IDLE_BUDGET) {
            /* A run is a candidate when it starts in a code page from which
             * two earlier runs never left translated code (the budget to the
             * last block in one entry, no call-out: a loop that touches no
             * device and no unmapped or watched memory) and ended in the same
             * registers, in a page a probe
             * paid in, or, now and then, at a branch to itself or a trap
             * (MPC601UM 10: b ., twi 31,r31,n). A probe that does not pay
             * disarms the page until such a run again, and the page is then
             * probed at every 2nd, 4th ... 64th candidate. This selects candidates, it never proves one
             * idle. */
            const unsigned slot = (pc >> 12) & (IDLE_SLOTS - 1u);
            const uint32_t page = (pc & PAGE_MASK) | 1u;
            const uint64_t turned = j->cycle.period ? idle_cycle(j, budget) : 0;
            if (turned) {
                steps += turned;
                continue;
            }
            if (word == 0x48000000u || (word & 0xffff0000u) == 0x0fff0000u) {
                const unsigned at = (pc >> 2) & 1023u;
                if (j->idle_pc[at] != pc) j->idle_pc[at] = pc, j->idle_hits[at] = 0;
                if ((++j->idle_hits[at] & 4095u) == 8u) {
                    if (j->idle[slot].page != page) {
                        j->idle[slot].page = page;
                        j->idle[slot].hits = 0;
                        j->idle[slot].wait = 1;
                    }
                    j->idle[slot].armed = 1;
                }
            }
            if (j->idle[slot].page == page && (!j->idle[slot].wait || j->idle[slot].armed) &&
                ++j->idle[slot].hits >= j->idle[slot].wait) {
                uint64_t skipped;
                const uint64_t taken = idle_probe(j, budget - steps, steps, &skipped);
                j->idle[slot].hits = 0;
                j->idle[slot].armed = 0;
                /* The reference CPU is much slower than translated code: a probe
                 * costs some thousand translated steps, and only one that skipped
                 * many times what it interpreted pays (wait 0). */
                j->idle[slot].wait = skipped >= 32u * (taken - skipped) + 8192u ? 0u :
                                     j->idle[slot].wait < 64u ? (uint16_t)(j->idle[slot].wait ? j->idle[slot].wait * 2u : 2u) : 64u;
                steps += taken;
                if (c->result == PPC_UNIMPLEMENTED || c->result == PPC_CHECKSTOP || j->io) break;
                if (taken) continue;
            }
        }
        k = find(j, pc, t->host);
        if (!k) {
            const uint64_t start = j->profile ? host_ticks() : 0;
            k = translate(j, pc, t->host, t->addend);
            if (j->profile) j->compile_ticks += host_ticks() - start;
            if (!k) break;
        }
        if (t->tag != ((pc & PAGE_MASK) | 1u)) continue;            /* a translation flushed the TLB */
        if (k->count > budget - steps) break;
        f = &j->fast[(pc >> 2) & (FAST_SIZE - 1u)];
        f->pc = pc;
        f->need = k->count;
        f->addend = k->addend;
        f->code = k->code;
        give = (int64_t)(budget - steps);
        j->run_base = steps;
        j->give = j->left = give;
        ++j->stats.entries;
        ++entered;
        const uint64_t start = j->profile ? host_ticks() : 0;
        why = j->enter(c, j, k->code, give);
        if (j->profile) j->enter_ticks += host_ticks() - start;
        steps += (uint64_t)(give - j->left);
        if (why == EXIT_HELPER || why == EXIT_SYSTEM) {
            if (c->result == PPC_UNIMPLEMENTED || c->result == PPC_CHECKSTOP || j->io) break;
            if (why == EXIT_SYSTEM) mmu_check(j);
        }
    }
    j->running = 0;
    if (budget >= IDLE_BUDGET && budget - steps < MAX_INSNS && entered <= 1u && calls == j->stats.helpers + j->stats.memory_slow) {
        /* The run stayed in translated code. A loop that will come back to a
         * state ends its runs in few different ones (a delay loop counting
         * down never ends two alike): the page is armed when this run ends
         * in registers an earlier such run ended in. */
        uint32_t h = c->pc ^ c->msr * 0x9E3779B1u;
        h = (h ^ c->cr) * 0x85EBCA6Bu; h = (h ^ c->xer) * 0xC2B2AE35u; h = (h ^ c->lr) * 0x27D4EB2Fu; h = (h ^ c->ctr) * 0x165667B1u;
        for (unsigned r = 0; r < 32u; ++r) h = (h ^ c->gpr[r]) * 0x9E3779B1u + (h >> 15);
        h |= 1u;
        if (j->idle_seen[h >> 20] != h) j->idle_seen[h >> 20] = h;
        else {
            const unsigned slot = (first_page >> 12) & (IDLE_SLOTS - 1u);
            if (j->idle[slot].page != first_page) {
                j->idle[slot].page = first_page;
                j->idle[slot].hits = 0;
                j->idle[slot].wait = 1;
            }
            j->idle[slot].armed = 1;
        }
    }
    if (j->profile) j->run_ticks += host_ticks() - run_start;
    j->stats.steps += steps;
    j->outside = 1;
    return steps;
}

/* --- one step (the suites' translator mode) ----------------------------------------------- */

ppc_result ppc_jit_step(ppc_cpu *c)
{
    static ppc_jit *j;
    static gen g;
    uint32_t pa, op;
    if (!j && !(j = jit_new(c, NULL, NULL, NULL))) return ppc_step(c);
    if (!usable(c) || ((c->msr & PPC_MSR_EE) && (c->external_irq || c->decrementer_pending))) return ppc_step(c);
    c->result = PPC_OK;
    c->last_pc = c->pc;
    c->last_exception = 0;
    if (!ppc_translate(c, c->pc, PPC_FETCH, &pa)) return c->result;
    if (!c->bus.read || !c->bus.read(c->bus.opaque, pa, 4, &op)) {
        ppc_exception(c, PPC_EX_MACHINE, 0, c->pc);
        return c->result;
    }
    c->last_opcode = op;
    if (is_decline(op)) return ppc_execute(c, op);
    j->cpu = c;
    j->model = c->model;
    memset(&g.b, 0, sizeof g.b);
    g.j = j;
    g.b.p = j->code_start;
    g.b.end = j->code_end;
    g.block_pc = c->pc;
    g.nofast = 1;
    generate(&g, &op, 1);
    j->enter(c, j, g.entry, 1);
    return c->result;
}

#endif
