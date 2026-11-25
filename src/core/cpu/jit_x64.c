/* The x64 translator.
 *
 * Livadix's own design. The instruction semantics are those of Livadix's
 * interpreter (m68k_ops.c, written from the M68000 Family Programmer's
 * Reference Manual), which is the reference: a translated block leaves
 * every register, flag, memory byte, instruction and cycle count as the
 * interpreter would. Host encodings from the Intel 64 and IA-32
 * Architectures Software Developer's Manual (x64emit.h).
 *
 * Shape:
 *   - Blocks: straight runs of guest instructions inside one 4 KiB page,
 *     keyed by the logical PC and the host page they were read from. A
 *     block's entry checks the slice's budget against its cycles and that
 *     the program page table still maps its logical page to that host page
 *     (TLB-checked entry); conditional branches inside it leave by side
 *     exits. Untranslated instructions (privileged, FPU, MMU, exceptions)
 *     end a block; the interpreter runs them.
 *   - Exits are chained: a direct branch's exit is patched to jump to the
 *     target block's entry once it exists; indirect jumps (RTS, JMP (An))
 *     look the target up in a direct-mapped table.
 *   - State: D0-D7/A0-A7 live in the CPU structure; the registers a block
 *     uses most are held in host registers for its length. The flags are
 *     kept in the interpreter's form (values N, Z, V, C, X derive from);
 *     a flag no exit can see before the next instruction rewrites it is not
 *     computed, and a condition test right after the instruction that set
 *     the flags uses the host's flags directly.
 *   - Memory: the soft TLB's tables, inline. An access the tables do not
 *     map either calls the slow path (callout on: MMIO call-out
 *     2.3 refinement 1; the instructions done so far are counted first so
 *     the device sees exact guest time) or leaves the block before the
 *     instruction changed anything, and the interpreter runs it. A read the
 *     slow path refuses leaves the block the same way and takes the access
 *     error; a refused write waits in a writeback slot as in the
 *     interpreter. After a slow-path call the block leaves at the next
 *     instruction boundary when an interrupt is due, the slice's end moved
 *     or translated code was written (refinement 2).
 *   - Self-modifying code: pages holding translated code have their write
 *     mappings held back from the fast path (mmu.c, table kind 3); a write
 *     through the slow path to bytes a block was translated from drops the
 *     page's blocks.
 *   - Idle skipping: a loop that only reads memory and branches back (a
 *     guest busy-wait) runs once; its state then repeats exactly until an
 *     interrupt or device event, so the rest of the slice's whole
 *     iterations are counted at once. DBRA to itself is counted the same
 *     way. Guest time stays a function of the cycle count.
 *   - Cycles (m68k_timing.c): a block sums its instructions' counts on each
 *     path at translation time (a branch taken or not, MOVEM's registers,
 *     a full-format extension word); its entry asks the budget for the
 *     largest sum any path may charge, so a block runs only where the
 *     interpreter would run all of it; an instruction the interpreter runs
 *     for the block charges its own. */
#include "m68k_internal.h"

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define MC_JIT_X64 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include "x64emit.h"
#define JIT_CALL
#elif defined(__linux__) && defined(__x86_64__)
/* Linux x86-64: the generated code keeps the Windows x64 calling convention
 * (arguments in RCX RDX R8 R9, RBX RBP RSI RDI R12-R15 and XMM6-XMM15 kept
 * across a call, 32 bytes of shadow space); the C functions it calls are
 * compiled with that convention (GCC's ms_abi attribute) and the trampoline
 * is entered through it. Code memory comes from mmap (mmap(2)). */
#define MC_JIT_X64 1
#include <cpuid.h>
#include <sys/mman.h>
#include <x86intrin.h>
#include "x64emit.h"
#define JIT_CALL __attribute__((ms_abi))
#elif (defined(_M_ARM64) || defined(__aarch64__)) && (defined(_WIN32) || defined(__linux__))
/* ARM64 (Windows, Linux): the same translator; a64emit.h lowers each of
 * its x86-64 operations to ARM64 instructions with the same effect, the
 * generated code calls the C functions with the platform's own convention
 * (AAPCS64), and code made or changed is synchronised with instruction
 * fetch (a64_sync). */
#define MC_JIT_X64 1
#define MC_JIT_A64 1
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <sys/mman.h>
#endif
#include "a64emit.h"
#define JIT_CALL
#else
#define MC_JIT_X64 0
#endif
#ifndef MC_JIT_A64
#define MC_JIT_A64 0
/* A forward jump bound here (a64emit.h's x_bind also follows the flags). */
#define x_bind(b, at) x_patch((at), (b)->p)
#endif

#if !MC_JIT_X64

int m68k_jit_available(void) { return 0; }
int m68k_jit_enable(m68k_cpu *cpu, int on) { (void)cpu; (void)on; return 0; }
void m68k_jit_options(m68k_cpu *cpu, int callout, int idle_skip, unsigned max_block) { (void)cpu; (void)callout; (void)idle_skip; (void)max_block; }
void m68k_jit_threshold(m68k_cpu *cpu, unsigned runs) { (void)cpu; (void)runs; }
void m68k_jit_flush(m68k_cpu *cpu) { (void)cpu; }
void m68k_jit_invalidate(m68k_cpu *cpu, uint32_t address, uint32_t size) { (void)cpu; (void)address; (void)size; }
void m68k_jit_get_stats(const m68k_cpu *cpu, m68k_jit_stats *stats) { (void)cpu; memset(stats, 0, sizeof *stats); }
void m68k_jit_set_block_hook(m68k_cpu *cpu, m68k_jit_block_hook hook, void *ctx) { (void)cpu; (void)hook; (void)ctx; }
void m68k_jit_set_refuse_hook(m68k_cpu *cpu, m68k_jit_refuse_hook hook, void *ctx) { (void)cpu; (void)hook; (void)ctx; }
int m68k_jit_exec(m68k_cpu *cpu) { (void)cpu; return 0; }
void m68k_jit_written(m68k_cpu *cpu, const uint8_t *page, uint32_t offset, unsigned size) { (void)cpu; (void)page; (void)offset; (void)size; }
int m68k_jit_is_code(m68k_cpu *cpu, const uint8_t *page) { (void)cpu; (void)page; return 0; }
void m68k_jit_free(m68k_cpu *cpu) { (void)cpu; }
void m68k_jit_trap(m68k_cpu *cpu) { (void)cpu; }
void m68k_jit_wlog(m68k_cpu *cpu, const uint8_t *p, unsigned size) { (void)cpu; (void)p; (void)size; }
void m68k_jit_wlog_end(m68k_cpu *cpu) { (void)cpu; }

#else

/* --- the translator's state ------------------------------------------------ */

#define CODE_SIZE (128u << 20)
#define MAX_BLOCKS 262144u
#define HASH_SIZE 65536u
#define FAST_SIZE 65536u
#define HEAT_SIZE 65536u
#define PAGE_SLOTS 65536u
#define MAX_INSNS 64u
#if MC_JIT_A64
#define CODE_RESERVE 262144u    /* room a block's translation may need (ARM64: about four times x86's) */
#else
#define CODE_RESERVE 65536u     /* room a block's translation may need */
#endif

enum { EXIT_NORMAL, EXIT_INTERP, EXIT_CHECK, EXIT_FAULT, EXIT_CHAIN };

typedef struct jblock {
    uint32_t pc, end;           /* guest range [pc, end) */
    const uint8_t *host;        /* host page the code was read from */
    uint8_t *code;              /* entry; NULL: the interpreter runs the instruction at pc */
    struct jblock *hnext;       /* hash chain */
    struct jblock *pnext;       /* blocks of the same host page */
    uint8_t **sites;            /* chain sites (rel32 fields) that jump to code */
    uint32_t nsites, capsites;
    int32_t need;               /* counts (cycles when timed) the entry asks the budget to hold */
    uint16_t ninsn;
    uint8_t dead;
    uint8_t nbytes;             /* no code: the instruction bytes that decided it, rechecked when found */
    uint8_t bytes[10];
} jblock;

typedef struct jpage {
    const uint8_t *host;        /* NULL empty; TOMB deleted */
    jblock *blocks;
    uint8_t *code;              /* a byte per byte of the page: nonzero where some block was translated from (+3 bytes of slack) */
} jpage;
#define CODE_MAP_SIZE (M68K_PAGE_SIZE + 4u)

/* A direct-mapped cache, host page -> its code map, that translated writes
 * consult inline for a page whose write mapping is held back (it lives in
 * the code region, within reach of RIP-relative addressing). */
typedef struct jwcache {
    const uint8_t *host;
    const uint8_t *code;
} jwcache;
#define WCACHE_SIZE 16384u
static unsigned wcache_index(const uint8_t *host) { return (unsigned)((uintptr_t)host >> M68K_PAGE_SHIFT) & (WCACHE_SIZE - 1); }
#define TOMB ((const uint8_t *)1)

typedef struct jfast {
    uint32_t pc, need;
    uint8_t *code;              /* (both read by translated code: offsets 0 and 8) */
    const uint8_t *host;        /* the host page the block was read from */
    uintptr_t pad;
} jfast;

/* Idle loops through traps (m68k_jit_trap): the anchors seen (A-line
 * instructions), the state a logged iteration started from and the bytes
 * it wrote with their values before. */
#define LOOP_SLOTS 16u
#define LOOP_MAX (1u << 20)     /* instructions an iteration may take */
#define WLOG_SLOTS 4096u
#define WLOG_MAX 1024u          /* distinct bytes an iteration may write */

typedef struct jloop {
    uint32_t pc;
    uint16_t sr;
    uint8_t verified;           /* an iteration from here repeated: the next visit logs at once */
    uint8_t fails;
    uint32_t backoff;           /* visits to let pass after a failure */
    uint32_t r[16];
} jloop;

/* Everything an iteration's course depends on in the CPU, besides memory
 * (the replacement counters are compared apart: they may advance). */
typedef struct jsnap {
    uint32_t r[16];
    uint32_t usp, isp, msp, vbr, sfc, dfc, cacr, caar, tc, itt0, itt1, dtt0, dtt1, mmusr, urp, srp;
    uint32_t tc30, crp[2], srp30[2], tt30[2];
    uint32_t mmu_searches, bus_reads, bus_writes, irqs_taken, dbra_runs;
    uint16_t sr, mmusr30;
    uint8_t s, m, ipl, nmi_seen, ea_ext020, next030, wb_n, status, events, pad[3];
    uint8_t ctr[2];
    m68k_atc_entry atc[2][16][4];
    m68k_atc_entry atc030[22];
    m68k_fpu fpu;
} jsnap;

typedef struct jit_state {
    uint8_t *code, *code_end, *code_free, *code_base;
    jwcache *wcache;            /* WCACHE_SIZE entries at the start of the code region */
    int (JIT_CALL *enter)(m68k_cpu *cpu, uint8_t *code);
    uint8_t *epilogue;
    uint8_t *thunk_read, *thunk_write;  /* the slow-path calls: temporaries kept, maps reloaded */
    jblock *blocks;
    unsigned nblocks;
    jblock *hash[HASH_SIZE];
    jfast fast[FAST_SIZE];
    jpage pages[PAGE_SLOTS];
    unsigned npages, ntomb;
    int callout, idle_skip;
    unsigned threshold;         /* runs before a PC is translated */
    uint8_t heat[HEAT_SIZE];    /* runs found so far of untranslated PCs, by hash */
    unsigned max_block;
    int movbe;
    m68k_jit_stats stats;
    jloop loops[LOOP_SLOTS];
    jloop *log_loop;            /* the anchor of the iteration being logged (NULL: none) */
    uint64_t log_start, log_cycles;
    jsnap log_snap;
    const uint8_t *wkey[WLOG_SLOTS];
    uint8_t wold[WLOG_SLOTS];
    uint16_t wused[WLOG_MAX];
    unsigned wn;
    int wover;
    uint8_t **wlog_map;         /* maps nothing: the write table and, two tables on, the held-back one */
    uint8_t kind[65536];        /* translation family of each opcode word on this model (0: not translated) */
    m68k_model model;
    m68k_jit_block_hook hook;
    void *hook_ctx;
    m68k_jit_refuse_hook refuse;
    void *refuse_ctx;
} jit_state;

#define J(cpu) ((jit_state *)(cpu)->jit)
#define OFF(field) ((int32_t)offsetof(m68k_cpu, field))
#define ROFF(n) ((int32_t)(offsetof(m68k_cpu, r) + 4u * (n)))

/* --- code pages ---------------------------------------------------------------- */

static unsigned page_hash(const uint8_t *p) { return (unsigned)(((uintptr_t)p >> 12) * 2654435761u) & (PAGE_SLOTS - 1); }

static jpage *page_find(jit_state *j, const uint8_t *host) {
    unsigned i = page_hash(host);
    for (unsigned k = 0; k < PAGE_SLOTS; k++, i = (i + 1) & (PAGE_SLOTS - 1)) {
        jpage *p = &j->pages[i];
        if (p->host == host) return p;
        if (!p->host) return NULL;
    }
    return NULL;
}

static jpage *page_get(jit_state *j, const uint8_t *host, int *created) {
    *created = 0;
    jpage *p = page_find(j, host);
    if (p) return p;
    if (j->npages + j->ntomb >= PAGE_SLOTS / 2) return NULL;
    unsigned i = page_hash(host);
    for (;; i = (i + 1) & (PAGE_SLOTS - 1)) {
        p = &j->pages[i];
        if (!p->host || p->host == TOMB) break;
    }
    uint8_t *map = calloc(CODE_MAP_SIZE, 1);
    if (!map) return NULL;
    if (p->host == TOMB) j->ntomb--;
    p->host = host;
    p->blocks = NULL;
    p->code = map;
    j->npages++;
    *created = 1;
    return p;
}

int m68k_jit_is_code(m68k_cpu *cpu, const uint8_t *page) {
    jit_state *j = J(cpu);
    return j && j->npages && page_find(j, page) != NULL;
}

/* --- blocks ----------------------------------------------------------------------- */

static unsigned block_hash(uint32_t pc) { return ((pc >> 1) * 2654435761u) >> 16 & (HASH_SIZE - 1); }
static unsigned fast_index(uint32_t pc) { return (pc >> 1) & (FAST_SIZE - 1); }

static void block_kill(jit_state *j, jblock *b);

static jblock *block_find(jit_state *j, uint32_t pc, const uint8_t *host) {
    for (jblock *b = j->hash[block_hash(pc)]; b; b = b->hnext) {
        if (b->pc != pc || b->host != host) continue;
        /* A block with no code is not on its page's list (its page is not
         * write-protected for it): its bytes are compared instead. */
        if (!b->code && memcmp(b->bytes, host + (pc & (M68K_PAGE_SIZE - 1)), b->nbytes) != 0) {
            block_kill(j, b);
            return NULL;
        }
        return b;
    }
    return NULL;
}

/* Makes a block unreachable: out of the hash and the fast table, every
 * chained jump to it sent back to its exit sequence. Its code stays until
 * the cache is emptied (a block may be running when this happens). */
static void block_kill(jit_state *j, jblock *b) {
    if (b->dead) return;
    b->dead = 1;
    jblock **pp = &j->hash[block_hash(b->pc)];
    while (*pp && *pp != b) pp = &(*pp)->hnext;
    if (*pp) *pp = b->hnext;
    jfast *f = &j->fast[fast_index(b->pc)];
    if (b->code && f->code == b->code) {
        f->pc = 1;
        f->code = NULL;
    }
    for (uint32_t i = 0; i < b->nsites; i++) {
#if MC_JIT_A64
        x_unpatch(b->sites[i]);
        a64_sync(b->sites[i], 4);
#else
        const int32_t zero = 0;
        memcpy(b->sites[i], &zero, 4);
#endif
    }
    free(b->sites);
    b->sites = NULL;
    b->nsites = b->capsites = 0;
    j->stats.invalidations++;
}

static void page_drop(m68k_cpu *cpu, jit_state *j, jpage *p) {
    for (jblock *b = p->blocks; b; b = b->pnext) block_kill(j, b);
    m68k_mmu_tlb_protect(cpu, p->host, 0);
    jwcache *w = &j->wcache[wcache_index(p->host)];
    if (w->host == p->host) {
        w->host = NULL;
        w->code = NULL;
    }
    p->host = TOMB;
    p->blocks = NULL;
    free(p->code);
    p->code = NULL;
    j->npages--;
    j->ntomb++;
}

void m68k_jit_written(m68k_cpu *cpu, const uint8_t *page, uint32_t offset, unsigned size) {
    jit_state *j = J(cpu);
    if (!j || !j->npages) return;
    jpage *p = page_find(j, page);
    if (!p) return;
    /* The translated writes' cache of code maps: this page now. */
    jwcache *wc = &j->wcache[wcache_index(page)];
    wc->host = page;
    wc->code = p->code;
    const uint32_t off = offset & (M68K_PAGE_SIZE - 1);
    int hit = 0;
    for (unsigned k = 0; k < size && off + k < M68K_PAGE_SIZE; k++) hit |= p->code[off + k];
    if (!hit) return;
    page_drop(cpu, j, p);
    /* A block running now leaves after its instruction. */
    cpu->jit_pending = 1;
}

/* Empties the code cache: every block, page record and chain. */
static void flush_all(m68k_cpu *cpu, jit_state *j) {
    for (unsigned i = 0; i < j->nblocks; i++) free(j->blocks[i].sites);
    j->nblocks = 0;
    memset(j->hash, 0, sizeof j->hash);
    for (unsigned i = 0; i < PAGE_SLOTS; i++)
        if (j->pages[i].host && j->pages[i].host != TOMB) free(j->pages[i].code);
    memset(j->wcache, 0, WCACHE_SIZE * sizeof *j->wcache);
    for (unsigned i = 0; i < FAST_SIZE; i++) {
        j->fast[i].pc = 1;
        j->fast[i].code = NULL;
    }
    memset(j->pages, 0, sizeof j->pages);
    j->npages = j->ntomb = 0;
    if (cpu->mmu.tlb) m68k_mmu_tlb_protect(cpu, NULL, 0);
    j->code_free = j->code_base;
    j->stats.flushes++;
}

void m68k_jit_flush(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    if (j) flush_all(cpu, j);
}

void m68k_jit_invalidate(m68k_cpu *cpu, uint32_t address, uint32_t size) {
    if (!cpu->jit || !size) return;
    uint8_t *const *map = cpu->bus.read_map[1];
    if (!map) return;
    const uint32_t last = address + size - 1;
    for (uint32_t page = address >> M68K_PAGE_SHIFT;; page++) {
        const uint8_t *host = map[page & (M68K_PAGE_COUNT - 1)];
        const uint32_t lo = page == address >> M68K_PAGE_SHIFT ? address & (M68K_PAGE_SIZE - 1) : 0;
        const uint32_t hi = page == last >> M68K_PAGE_SHIFT ? last & (M68K_PAGE_SIZE - 1) : M68K_PAGE_SIZE - 1;
        if (host) m68k_jit_written(cpu, host, lo, hi - lo + 1);
        if (page == last >> M68K_PAGE_SHIFT) break;
    }
}

/* --- decoding ------------------------------------------------------------------------- */

/* Translation families. */
enum {
    K_NONE, K_MOVE, K_MOVEA, K_MOVEQ, K_LEA, K_PEA, K_CLR, K_TST, K_NEG, K_NOT, K_EXT, K_SWAP, K_EXG,
    K_ALU_ER, K_ALU_RE, K_ALU_A, K_ALU_I, K_QUICK, K_ADDX, K_CMPM, K_MUL, K_BIT, K_SHIFT,
    K_BCC, K_DBCC, K_SCC, K_JMP, K_JSR, K_RTS, K_LINK, K_UNLK, K_MOVEM, K_NOP,
    K_FROM_SR,  /* MOVE from SR to a data register */
    K_MOVE16,   /* MOVE16 (68040): one 16-byte line between mapped pages */
    K_TO_SR,    /* MOVE to SR, ORI/ANDI/EORI to SR: S, M and the trace bits kept */
    K_TO_CCR,   /* MOVE to CCR, ORI/ANDI/EORI to CCR */
    K_BF,       /* the bit field instructions but BFFFO: helper_bf, in place */
    K_INPLACE,  /* the interpreter's handler in place (helper_inplace): register-only shifts and
                 * rotates by a register or through X, the divides and long multiplies of a
                 * register or an immediate */
    K_INTERP    /* any other opcode word: the interpreter's handler, called from the block, which ends there */
};

/* Flags in CCR bit order. */
enum { F_C = 1, F_V = 2, F_Z = 4, F_N = 8, F_X = 16, F_NZVC = 15, F_ALL = 31 };

enum { EK_NONE, EK_D, EK_A, EK_IND, EK_POST, EK_PRE, EK_DISP, EK_IDX, EK_ABS, EK_IMM };

typedef struct ead {
    uint8_t k, reg;
    uint8_t xreg;               /* index register 0-15, 0xFF none */
    uint8_t xlong, xscale;
    uint8_t base;               /* EK_IDX: 0 An, 1 none, 2 constant (in abs) */
    uint8_t ext020;             /* the index word used a scale or the full format */
    uint8_t full, pcmode;       /* EK_IDX: the full format (its word in extw); the mode is PC-relative */
    uint16_t extw;
    int32_t disp;
    uint32_t abs;               /* absolute address, PC-relative address, immediate, or EK_IDX constant base */
    uint8_t mi;                 /* EK_IDX memory indirect (PRM 2.2.10-2.2.13): 0 none, 1 pre-indexed or no index, 2 post-indexed */
    int32_t od;                 /* its outer displacement */
} ead;

typedef struct insn {
    uint32_t pc, next, target;
    uint16_t op;
    uint8_t kind, size, sub, reg, cc;
    uint8_t fr, fw, live;       /* flags read, written; written flags some exit or reader sees */
    uint8_t mem;                /* touches memory: may leave the block at its start or after it */
    uint8_t ends;               /* the block ends after it */
    uint8_t inv;                /* a Bcc the block follows when taken (a loop's path): leaves when not */
    uint32_t imm;
    ead s, d;
} insn;

typedef struct dctx {
    const uint8_t *host;
    uint32_t pc;                /* address of the next word to read */
    int bad;
} dctx;

/* The next instruction word; past the page's end the instruction is the
 * interpreter's. */
static uint32_t word(dctx *d) {
    const uint32_t off = d->pc & (M68K_PAGE_SIZE - 1);
    if (d->bad || ((d->pc + 2) & (M68K_PAGE_SIZE - 1)) < off) {
        if (!d->bad && off == M68K_PAGE_SIZE - 2) {
            d->pc += 2;
            d->bad = 2;     /* the last word of the page: readable, nothing after it */
            return ld16(d->host + off);
        }
        d->bad = 1;
        return 0;
    }
    d->pc += 2;
    return ld16(d->host + off);
}
static uint32_t lword(dctx *d) {
    const uint32_t hi = word(d);
    return hi << 16 | word(d);
}

/* An effective address (PRM 2.2) with its extension words; reserved
 * full-format encodings are left to the interpreter. */
static void decode_ea(dctx *d, unsigned mode, unsigned reg, unsigned size, ead *e) {
    memset(e, 0, sizeof *e);
    e->reg = (uint8_t)reg;
    e->xreg = 0xFF;
    switch (mode) {
    case 0: e->k = EK_D; return;
    case 1: e->k = EK_A; return;
    case 2: e->k = EK_IND; return;
    case 3: e->k = EK_POST; return;
    case 4: e->k = EK_PRE; return;
    case 5: e->k = EK_DISP; e->disp = (int16_t)word(d); return;
    default: break;
    }
    if (mode == 7) {
        switch (reg) {
        case 0: e->k = EK_ABS; e->abs = (uint32_t)(int32_t)(int16_t)word(d); return;
        case 1: e->k = EK_ABS; e->abs = lword(d); return;
        case 2: { const uint32_t at = d->pc; e->k = EK_ABS; e->abs = at + (uint32_t)(int32_t)(int16_t)word(d); return; }
        case 4:
            e->k = EK_IMM;
            e->abs = size == 4 ? lword(d) : size == 2 ? word(d) : (word(d) & 0xFFu);
            return;
        case 3: break;
        default: d->bad = 1; return;
        }
    }
    /* Indexed (PRM 2.2.6-2.2.13): base An, or the address of the extension word. */
    const uint32_t at = d->pc;
    const uint32_t ext = word(d);
    e->k = EK_IDX;
    e->base = mode == 7 ? 2 : 0;
    e->pcmode = mode == 7;
    e->extw = (uint16_t)ext;
    e->full = (ext & 0x0100u) != 0;
    if (mode == 7) e->abs = at;
    e->ext020 = (ext & 0x0700u) != 0;
    e->xreg = (uint8_t)(ext >> 12);
    e->xlong = (ext & 0x0800u) != 0;
    e->xscale = (uint8_t)(1u << ((ext >> 9) & 3u));
    if (!(ext & 0x0100u)) {
        e->disp = (int8_t)ext;
        return;
    }
    const unsigned bdsize = (ext >> 4) & 3u, iis = ext & 7u;
    if (bdsize == 0 || (ext & 0x0008u) || ((ext & 0x0040u) ? iis > 3 : iis == 4)) {
        d->bad = 1;     /* reserved (illegal in the interpreter) */
        return;
    }
    if (bdsize == 2) e->disp = (int16_t)word(d);
    else if (bdsize == 3) e->disp = (int32_t)lword(d);
    if (iis) {
        /* Memory indirect: I/IS 1-3 pre-indexed (or, the index suppressed,
         * no index), 5-7 post-indexed; the outer displacement null, word
         * or long (PRM Table 2-2). */
        e->mi = (uint8_t)(!(ext & 0x0040u) && iis >= 5 ? 2 : 1);
        if ((iis & 3u) == 2) e->od = (int16_t)word(d);
        else if ((iis & 3u) == 3) e->od = (int32_t)lword(d);
    }
    if (ext & 0x0080u) {
        e->base = 1;
        e->abs = 0;
    }
    if (ext & 0x0040u) e->xreg = 0xFF;
}

static unsigned cc_reads(unsigned cc) {
    static const uint8_t r[16] = {0, 0, F_C | F_Z, F_C | F_Z, F_C, F_C, F_Z, F_Z, F_V, F_V, F_N, F_N,
                                  F_N | F_V, F_N | F_V, F_N | F_V | F_Z, F_N | F_V | F_Z};
    return r[cc & 15u];
}

static int is_mem(const ead *e) { return e->k >= EK_IND && e->k <= EK_ABS; }

/* Size field of the S3 encodings (bits 7-6). */
static unsigned size67(unsigned op) { return ((op >> 6) & 3u) == 0 ? 1u : ((op >> 6) & 3u) == 1 ? 2u : 4u; }

/* An instruction for the interpreter's handler: its length is the
 * handler's business (it fetches the words itself), and the block ends
 * with it. `next` covers the opcode word, the bytes the block depends on. */
static int as_interp(insn *in) {
    in->kind = K_INTERP;
    in->next = in->pc + 2;
    in->mem = 1;
    in->ends = 1;
    in->fr = F_ALL;
    in->fw = F_ALL;
    in->live = 0;
    memset(&in->s, 0, sizeof in->s);
    memset(&in->d, 0, sizeof in->d);
    return 1;
}

static int decode_body(jit_state *j, dctx *d, insn *in, unsigned op);

/* The bytes of extension words of an effective address (brief index format
 * only); -1 when not known here. */
static int ea_ext_len(dctx *d, uint32_t at, unsigned mode, unsigned reg, unsigned size) {
    switch (mode) {
    case 0: case 1: case 2: case 3: case 4: return 0;
    case 5: return 2;
    case 6: break;
    default:
        switch (reg) {
        case 0: case 2: return 2;
        case 1: return 4;
        case 3: break;
        case 4: return size == 4 ? 4 : 2;
        default: return -1;
        }
    }
    /* Indexed: the brief format (bit 8 clear) is one word. */
    const uint32_t off = at & (M68K_PAGE_SIZE - 1);
    if (off > M68K_PAGE_SIZE - 2) return -1;
    return (ld16(d->host + off) & 0x0100u) ? -1 : 2;
}

/* The length of an instruction the interpreter runs inside a block, for the
 * families seen in the guests' hot code that never transfer control but by
 * an exception: the SR instructions, MOVEC, MOVE USP, CAS, MOVE16, the cache
 * instructions, bit fields, the word divides, the long multiplies and
 * divides, shifts, the FPU's general, FMOVE and FMOVEM instructions. 0 when
 * the block ends after it. The whole instruction
 * must be inside the page. */
static unsigned interp_len(dctx *d, unsigned op) {
    const uint32_t pc = d->pc - 2;
    const unsigned mode = (op >> 3) & 7u, reg = op & 7u;
    int len = 0;
    if ((op & 0xFFF0u) == 0x40C0u || (op & 0xFFF8u) == 0x46C0u || op == 0x40E7u || op == 0x46DFu ||
        (op & 0xFFF0u) == 0x4E60u || (op & 0xFF00u) == 0xF400u)
        len = 2;
    else if (op == 0x007Cu || op == 0x027Cu || op == 0x0A7Cu || op == 0x46FCu || op == 0x4E7Au || op == 0x4E7Bu ||
             (op & 0xFFF8u) == 0xF620u)
        len = 4;
    else if ((op & 0xF9C0u) == 0x08C0u && (op & 0x0600u) && mode >= 2 && op != 0x0CFCu && op != 0x0EFCu) {
        const int e = ea_ext_len(d, pc + 4, mode, reg, 4);     /* CAS */
        len = e < 0 ? 0 : 4 + e;
    } else if ((op & 0xF8C0u) == 0xE8C0u && mode != 1 && mode != 3 && mode != 4) {
        const int e = ea_ext_len(d, pc + 4, mode, reg, 4);     /* bit fields */
        len = e < 0 ? 0 : 4 + e;
    } else if ((op & 0xF0C0u) == 0x80C0u && mode != 1) {
        const int e = ea_ext_len(d, pc + 2, mode, reg, 2);     /* DIVU.W, DIVS.W */
        len = e < 0 ? 0 : 2 + e;
    } else if ((op & 0xFF80u) == 0x4C00u && mode != 1) {
        const int e = ea_ext_len(d, pc + 4, mode, reg, 4);     /* MULx.L, DIVx.L */
        len = e < 0 ? 0 : 4 + e;
    } else if ((op & 0xF000u) == 0xE000u && (op & 0x00C0u) != 0x00C0u) {
        len = 2;                                                /* shifts and rotates of a register */
    } else if ((op & 0xF8C0u) == 0xE0C0u && mode >= 2) {
        const int e = ea_ext_len(d, pc + 2, mode, reg, 2);     /* of memory */
        len = e < 0 ? 0 : 2 + e;
    } else if ((op & 0xFFC0u) == 0xF200u) {
        /* The FPU's general and FMOVE/FMOVEM instructions (cpID 1, type 0):
         * the opcode, the command word, the operand's words. */
        const uint32_t off = (pc + 2) & (M68K_PAGE_SIZE - 1);
        if (off <= M68K_PAGE_SIZE - 2) {
            const unsigned ext = ld16(d->host + off), opclass = ext >> 13, rx = (ext >> 10) & 7u;
            static const uint8_t fmt_bytes[8] = {4, 4, 12, 12, 2, 8, 2, 0};     /* L S X P W D B; 7 FMOVECR */
            if (opclass == 0 || (opclass == 2 && rx == 7)) {
                len = 4;
            } else if (opclass == 2 || opclass == 3 || opclass >= 4) {
                if (mode == 0 || (mode == 1 && opclass >= 4 && opclass <= 5)) {
                    len = 4;
                } else if (mode == 7 && reg == 4) {
                    len = opclass == 2 ? 4 + fmt_bytes[rx] : 0;
                } else if (mode >= 2) {
                    const int e = ea_ext_len(d, pc + 4, mode, reg, 4);
                    len = e < 0 ? 0 : 4 + e;
                }
            }
        }
    }
    if (!len) return 0;
    const uint32_t off = pc & (M68K_PAGE_SIZE - 1);
    if (off + (unsigned)len > M68K_PAGE_SIZE) return 0;
    return (unsigned)len;
}

/* Decodes one instruction at d->pc; 0 when not even its opcode word is in
 * the page (the block ends before it). */
static int decode(jit_state *j, dctx *d, insn *in) {
    memset(in, 0, sizeof *in);
    in->pc = d->pc;
    if (d->bad) return 0;
    const unsigned op = word(d);
    if (d->bad == 1) return 0;
    in->op = (uint16_t)op;
    const int bad_before = d->bad;
    if (j->kind[op] != K_INTERP && decode_body(j, d, in, op)) return 1;
    /* Left to the interpreter: an encoding the translator does not handle,
     * or extension words past the page's end. */
    d->bad = bad_before;
    d->pc = in->pc + 2;
    as_interp(in);
    /* The block goes on after an instruction that leaves the PC at the next
     * one when it does not trap, if its length is known here. */
    const unsigned len = interp_len(d, op);
    if (len) {
        in->next = in->pc + len;
        in->ends = 0;
        d->pc = in->next;
        if (!(in->next & (M68K_PAGE_SIZE - 1))) d->bad = 2;     /* the page's end: nothing after it here */
    }
    return 1;
}

static int decode_body(jit_state *j, dctx *d, insn *in, unsigned op) {
    const unsigned kind = j->kind[op];
    const unsigned mode = (op >> 3) & 7u, reg = op & 7u, reg9 = (op >> 9) & 7u;
    in->kind = (uint8_t)kind;
    switch (kind) {
    case K_MOVE: {
        const unsigned sz = (op >> 12) == 1 ? 1u : (op >> 12) == 3 ? 2u : 4u;
        in->size = (uint8_t)sz;
        decode_ea(d, mode, reg, sz, &in->s);
        decode_ea(d, (op >> 6) & 7u, reg9, sz, &in->d);
        in->fw = F_NZVC;
        break;
    }
    case K_MOVEA:
        in->size = (op >> 12) == 3 ? 2 : 4;
        in->reg = (uint8_t)reg9;
        decode_ea(d, mode, reg, in->size, &in->s);
        break;
    case K_MOVEQ: in->reg = (uint8_t)reg9; in->imm = (uint32_t)(int32_t)(int8_t)op; in->fw = F_NZVC; break;
    case K_LEA: in->reg = (uint8_t)reg9; decode_ea(d, mode, reg, 4, &in->s); break;
    case K_PEA: decode_ea(d, mode, reg, 4, &in->s); break;
    case K_CLR: case K_TST: case K_NEG: case K_NOT:
        in->size = (uint8_t)size67(op);
        decode_ea(d, mode, reg, in->size, &in->d);
        in->fw = kind == K_NEG ? F_ALL : F_NZVC;
        break;
    case K_EXT: in->reg = (uint8_t)reg; in->sub = (uint8_t)((op >> 6) & 7u); in->fw = F_NZVC; break;   /* 2: EXT.W, 3: EXT.L, 7: EXTB.L */
    case K_SWAP: in->reg = (uint8_t)reg; in->fw = F_NZVC; break;
    case K_FROM_SR:
        /* To a data register (sub 0) or to memory (sub 1, at d). */
        in->reg = (uint8_t)reg;
        in->fr = F_ALL;
        if (mode != 0) {
            in->sub = 1;
            decode_ea(d, mode, reg, 2, &in->d);
        }
        break;
    case K_INPLACE:
        /* The words after the opcode: the long forms' extension word, an
         * immediate source (helper_inplace reads them again). */
        if ((op & 0xF000u) == 0x4000u) {
            in->imm = word(d);
            if (mode == 7) (void)lword(d);
        } else if ((op & 0xF000u) == 0x8000u && mode == 7) {
            in->imm = word(d);
        }
        in->fr = F_ALL;
        in->fw = F_ALL;
        break;
    case K_BF:
        /* The extension word in imm; the field's <ea> (control or Dn) at d. */
        in->imm = word(d);
        if (mode != 0) decode_ea(d, mode, reg, 1, &in->d);
        else in->reg = (uint8_t)reg;
        /* A memory-indirect <ea> reads its pointer before helper_bf may
         * decline: the interpreter's, whose own read is the only one. */
        if (in->d.mi) return 0;
        in->fw = F_NZVC;
        break;
    case K_TO_SR: case K_TO_CCR:
        /* sub: 0 MOVE from <ea> (at s), 1 ORI, 2 ANDI, 3 EORI #imm. */
        if ((op & 0xFF00u) == 0x4600u || (op & 0xFF00u) == 0x4400u) {
            decode_ea(d, mode, reg, 2, &in->s);
        } else {
            in->sub = (uint8_t)((op & 0x0F00u) == 0x0000u ? 1 : (op & 0x0F00u) == 0x0200u ? 2 : 3);
            in->imm = word(d);
            in->fr = F_ALL;
        }
        in->fw = F_ALL;
        if (kind == K_TO_SR) in->fr = F_ALL;
        break;
    case K_EXG: in->reg = (uint8_t)reg9; in->sub = (uint8_t)((op >> 3) & 0x1Fu); break;
    case K_ALU_ER: {
        /* OR, SUB, CMP, AND, ADD <ea>,Dn */
        const unsigned line = op >> 12;
        in->sub = (uint8_t)(line == 8 ? ALU_OR : line == 9 ? ALU_SUB : line == 0xB ? ALU_CMP : line == 0xC ? ALU_AND : ALU_ADD);
        in->size = (uint8_t)size67(op);
        in->reg = (uint8_t)reg9;
        decode_ea(d, mode, reg, in->size, &in->s);
        in->fw = (in->sub == ALU_ADD || in->sub == ALU_SUB) ? F_ALL : F_NZVC;
        break;
    }
    case K_ALU_RE: {
        /* OR, SUB, EOR, AND, ADD Dn,<ea> */
        const unsigned line = op >> 12;
        in->sub = (uint8_t)(line == 8 ? ALU_OR : line == 9 ? ALU_SUB : line == 0xB ? ALU_XOR : line == 0xC ? ALU_AND : ALU_ADD);
        in->size = (uint8_t)size67(op);
        in->reg = (uint8_t)reg9;
        decode_ea(d, mode, reg, in->size, &in->d);
        in->fw = (in->sub == ALU_ADD || in->sub == ALU_SUB) ? F_ALL : F_NZVC;
        break;
    }
    case K_ALU_A: {
        /* SUBA, CMPA, ADDA <ea>,An */
        const unsigned line = op >> 12;
        in->sub = (uint8_t)(line == 9 ? ALU_SUB : line == 0xB ? ALU_CMP : ALU_ADD);
        in->size = (op & 0x0100u) ? 4 : 2;
        in->reg = (uint8_t)reg9;
        decode_ea(d, mode, reg, in->size, &in->s);
        in->fw = in->sub == ALU_CMP ? F_NZVC : 0;
        break;
    }
    case K_ALU_I: {
        /* ORI, ANDI, SUBI, ADDI, EORI, CMPI #<data>,<ea> */
        static const uint8_t ops[8] = {ALU_OR, ALU_AND, ALU_SUB, ALU_ADD, 0, ALU_XOR, ALU_CMP, 0};
        in->sub = ops[(op >> 9) & 7u];
        in->size = (uint8_t)size67(op);
        in->imm = in->size == 4 ? lword(d) : in->size == 2 ? word(d) : (word(d) & 0xFFu);
        decode_ea(d, mode, reg, in->size, &in->d);
        in->fw = (in->sub == ALU_ADD || in->sub == ALU_SUB) ? F_ALL : F_NZVC;
        break;
    }
    case K_QUICK:
        in->sub = (op & 0x0100u) ? ALU_SUB : ALU_ADD;
        in->size = (uint8_t)size67(op);
        in->imm = ((reg9 - 1u) & 7u) + 1u;
        decode_ea(d, mode, reg, in->size, &in->d);
        in->fw = in->d.k == EK_A ? 0 : F_ALL;
        break;
    case K_ADDX:
        in->sub = (op >> 12) == 0xD ? ALU_ADC : ALU_SBB;
        in->size = (uint8_t)size67(op);
        in->reg = (uint8_t)reg9;
        in->imm = reg;
        in->fr = F_X | F_Z;
        in->fw = F_ALL;
        break;
    case K_CMPM:
        in->size = (uint8_t)size67(op);
        decode_ea(d, 3, reg, in->size, &in->s);
        decode_ea(d, 3, reg9, in->size, &in->d);
        in->fw = F_NZVC;
        break;
    case K_MUL:
        in->sub = (op & 0x0100u) ? 1 : 0;   /* MULS */
        in->size = 2;
        in->reg = (uint8_t)reg9;
        decode_ea(d, mode, reg, 2, &in->s);
        in->fw = F_NZVC;
        break;
    case K_BIT:
        in->sub = (uint8_t)((op >> 6) & 3u);    /* BTST, BCHG, BCLR, BSET */
        if (op & 0x0100u) {
            in->reg = (uint8_t)reg9;            /* bit number in Dn */
            in->imm = 0xFFFFFFFFu;
        } else {
            in->imm = word(d) & 0xFFu;
        }
        in->size = mode == 0 ? 4 : 1;
        decode_ea(d, mode, reg, 1, &in->d);
        in->fw = F_Z;
        break;
    case K_SHIFT: {
        const unsigned type = (op >> 3) & 3u;
        in->size = (uint8_t)size67(op);
        in->reg = (uint8_t)reg;
        in->sub = (uint8_t)type;                /* 0 AS, 1 LS, 3 RO */
        in->cc = (op & 0x0100u) ? 1 : 0;        /* left */
        in->imm = ((reg9 - 1u) & 7u) + 1u;
        in->fw = type == 3 ? F_NZVC : F_ALL;
        break;
    }
    case K_BCC: {
        const uint32_t base = d->pc;
        uint32_t disp = (uint32_t)(int32_t)(int8_t)op;
        if (disp == 0) disp = (uint32_t)(int32_t)(int16_t)word(d);
        else if (disp == 0xFFFFFFFFu) disp = lword(d);
        in->cc = (uint8_t)((op >> 8) & 15u);
        in->target = base + disp;
        /* An odd target is the interpreter's (address error, and the 68040
         * takes it even for a branch not taken). */
        if (disp & 1u) return 0;
        in->fr = (uint8_t)cc_reads(in->cc);
        break;
    }
    case K_DBCC: {
        const uint32_t base = d->pc;
        const uint32_t disp = (uint32_t)(int32_t)(int16_t)word(d);
        in->cc = (uint8_t)((op >> 8) & 15u);
        in->reg = (uint8_t)reg;
        in->target = base + disp;
        if (disp & 1u) return 0;
        in->fr = (uint8_t)cc_reads(in->cc);
        break;
    }
    case K_SCC:
        in->cc = (uint8_t)((op >> 8) & 15u);
        in->size = 1;
        decode_ea(d, mode, reg, 1, &in->d);
        in->fr = (uint8_t)cc_reads(in->cc);
        break;
    case K_JMP: case K_JSR:
        decode_ea(d, mode, reg, 4, &in->s);
        break;
    case K_RTS: case K_NOP: break;
    case K_LINK: in->reg = (uint8_t)reg; in->imm = (uint32_t)(int32_t)(int16_t)word(d); break;
    case K_UNLK: in->reg = (uint8_t)reg; break;
    case K_MOVE16:
        /* (Ax)+,(Ay)+: Ax in reg, Ay in cc (sub 4); the absolute forms:
         * Ay in reg, the address in imm, the form in sub (op_move16_abs). */
        if ((op & 0xFFF8u) == 0xF620u) {
            in->reg = (uint8_t)reg;
            in->cc = (uint8_t)((word(d) >> 12) & 7u);
            in->sub = 4;
        } else {
            in->reg = (uint8_t)reg;
            in->sub = (uint8_t)((op >> 3) & 3u);
            in->imm = lword(d);
        }
        break;
    case K_MOVEM:
        in->size = (op & 0x0040u) ? 4 : 2;
        in->sub = (op & 0x0400u) ? 1 : 0;       /* 1: memory to registers */
        in->imm = word(d);
        decode_ea(d, mode, reg, in->size, &in->s);
        /* The mapped-page transfer may decline at run time. Leave memory-
         * indirect forms to the interpreter before reading their pointer,
         * so a decline cannot repeat an MMU or device access. */
        if (in->s.mi) return 0;
        break;
    default: return 0;
    }
    if (d->bad == 1) return 0;
    in->next = d->pc;
    switch (kind) {
    case K_LEA: case K_JMP: in->mem = 0; break;      /* only the address */
    case K_PEA: case K_JSR: case K_RTS: case K_LINK: case K_UNLK: case K_MOVEM: case K_CMPM: case K_MOVE16: case K_BF: case K_INPLACE: in->mem = 1; break;
    case K_BCC: in->mem = in->cc == 1; break;
    default: in->mem = (uint8_t)(is_mem(&in->s) || is_mem(&in->d)); break;
    }
    if (in->s.mi || in->d.mi) in->mem = 1;         /* a memory-indirect address reads its pointer */
    in->ends =(uint8_t)(kind == K_JMP || kind == K_JSR || kind == K_RTS || (kind == K_BCC && (in->cc == 0 || in->cc == 1)));
    return 1;
}

/* Which family each opcode word belongs to, by the interpreter's own decode
 * (m68k_decode names the pattern it chose for the model), so a word is
 * translated only where the interpreter runs the same instruction. */
static unsigned family(const char *name, unsigned op) {
    static const struct { const char *name; unsigned kind; } names[] = {
        {"MOVE.B", K_MOVE}, {"MOVE.W", K_MOVE}, {"MOVE.L", K_MOVE}, {"MOVEA.W", K_MOVEA}, {"MOVEA.L", K_MOVEA},
        {"MOVEQ", K_MOVEQ}, {"LEA", K_LEA}, {"PEA", K_PEA},
        {"CLR.B", K_CLR}, {"CLR.W", K_CLR}, {"CLR.L", K_CLR}, {"TST.B", K_TST}, {"TST.W", K_TST}, {"TST.L", K_TST},
        {"NEG.B", K_NEG}, {"NEG.W", K_NEG}, {"NEG.L", K_NEG}, {"NOT.B", K_NOT}, {"NOT.W", K_NOT}, {"NOT.L", K_NOT},
        {"EXT.W", K_EXT}, {"EXT.L", K_EXT}, {"EXTB.L", K_EXT}, {"SWAP", K_SWAP}, {"EXG", K_EXG},
        {"ADDA.W", K_ALU_A}, {"ADDA.L", K_ALU_A}, {"SUBA.W", K_ALU_A}, {"SUBA.L", K_ALU_A}, {"CMPA.W", K_ALU_A}, {"CMPA.L", K_ALU_A},
        {"ORI.B", K_ALU_I}, {"ORI.W", K_ALU_I}, {"ORI.L", K_ALU_I}, {"ANDI.B", K_ALU_I}, {"ANDI.W", K_ALU_I}, {"ANDI.L", K_ALU_I},
        {"SUBI.B", K_ALU_I}, {"SUBI.W", K_ALU_I}, {"SUBI.L", K_ALU_I}, {"ADDI.B", K_ALU_I}, {"ADDI.W", K_ALU_I}, {"ADDI.L", K_ALU_I},
        {"EORI.B", K_ALU_I}, {"EORI.W", K_ALU_I}, {"EORI.L", K_ALU_I}, {"CMPI.B", K_ALU_I}, {"CMPI.W", K_ALU_I}, {"CMPI.L", K_ALU_I},
        {"ADDQ.B", K_QUICK}, {"ADDQ.W", K_QUICK}, {"ADDQ.L", K_QUICK}, {"SUBQ.B", K_QUICK}, {"SUBQ.W", K_QUICK}, {"SUBQ.L", K_QUICK},
        {"ADDX.B", K_ADDX}, {"ADDX.W", K_ADDX}, {"ADDX.L", K_ADDX}, {"SUBX.B", K_ADDX}, {"SUBX.W", K_ADDX}, {"SUBX.L", K_ADDX},
        {"CMPM.B", K_CMPM}, {"CMPM.W", K_CMPM}, {"CMPM.L", K_CMPM}, {"MULU.W", K_MUL}, {"MULS.W", K_MUL},
        {"BTST", K_BIT}, {"BCHG", K_BIT}, {"BCLR", K_BIT}, {"BSET", K_BIT},
        {"shift.B", K_SHIFT}, {"shift.W", K_SHIFT}, {"shift.L", K_SHIFT},
        {"Bcc", K_BCC}, {"DBcc", K_DBCC}, {"Scc", K_SCC}, {"JMP", K_JMP}, {"JSR", K_JSR}, {"RTS", K_RTS},
        {"LINK.W", K_LINK}, {"UNLK", K_UNLK}, {"MOVEM.W", K_MOVEM}, {"MOVEM.L", K_MOVEM}, {"NOP", K_NOP},
        {"MOVE from SR", K_FROM_SR}, {"MOVE16", K_MOVE16},
        {"DIVU.W", K_INPLACE}, {"DIVS.W", K_INPLACE}, {"MULx.L", K_INPLACE}, {"DIVx.L", K_INPLACE},
        {"MOVE to SR", K_TO_SR}, {"ORI to SR", K_TO_SR}, {"ANDI to SR", K_TO_SR}, {"EORI to SR", K_TO_SR},
        {"MOVE to CCR", K_TO_CCR}, {"ORI to CCR", K_TO_CCR}, {"ANDI to CCR", K_TO_CCR}, {"EORI to CCR", K_TO_CCR},
        {"BFTST", K_BF}, {"BFEXTU", K_BF}, {"BFCHG", K_BF}, {"BFEXTS", K_BF}, {"BFCLR", K_BF}, {"BFSET", K_BF}, {"BFINS", K_BF},
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (strcmp(name, names[i].name) != 0) continue;
        const unsigned k = names[i].kind;
        /* Same names, encodings left to the interpreter. */
        if (k == K_SHIFT && (((op >> 3) & 3u) == 2 || (op & 0x20u))) return K_INPLACE;     /* ROX, and counts in a register */
        if (k == K_INPLACE && (op & 0x38u) && (op & 0x3Fu) != 0x3Cu) return 0;            /* a divisor or factor in memory */
        if (k == K_ADDX && (op & 0x0008u)) return 0;                                 /* the -(An) forms */
        if (k == K_EXG) {
            const unsigned m = (op >> 3) & 0x1Fu;
            if (m != 0x08 && m != 0x09 && m != 0x11) return 0;
        }
        return k;
    }
    /* OR, SUB, CMP, AND, ADD, EOR share their names between directions. */
    static const char *const alu[] = {"OR.B", "OR.W", "OR.L", "SUB.B", "SUB.W", "SUB.L", "CMP.B", "CMP.W", "CMP.L",
                                      "AND.B", "AND.W", "AND.L", "ADD.B", "ADD.W", "ADD.L", "EOR.B", "EOR.W", "EOR.L"};
    for (size_t i = 0; i < sizeof alu / sizeof alu[0]; i++) {
        if (strcmp(name, alu[i]) != 0) continue;
        if (name[0] == 'E') return K_ALU_RE;
        if (name[0] == 'C') return K_ALU_ER;
        return (op & 0x0100u) ? K_ALU_RE : K_ALU_ER;
    }
    return 0;
}

static void build_kinds(jit_state *j) {
    /* Per model, once (the decode tables never change). */
    static uint8_t kinds[M68K_MODEL_COUNT][65536];
    static uint8_t built[M68K_MODEL_COUNT];
    if (!built[j->model]) {
        for (unsigned op = 0; op < 65536; op++) {
            const m68k_opinfo info = m68k_decode(j->model, (uint16_t)op);
            const unsigned k = info.valid && info.name ? family(info.name, op) : 0;
        kinds[j->model][op] = (uint8_t)(k ? k : K_INTERP);
        }
        built[j->model] = 1;
    }
    memcpy(j->kind, kinds[j->model], sizeof j->kind);
}

/* --- code generation ------------------------------------------------------------------ */

/* Host registers: RBX the CPU, R12/R13 the read and write page tables of
 * the current privilege, RBP RSI RDI R14 R15 guest registers held for the
 * block; RAX RCX RDX R8-R11 temporaries. */
static const int cache_regs[5] = {XBP, XSI, XDI, X14, X15};
static const int temp_regs[7] = {XAX, XCX, XDX, X8, X9, X10, X11};

enum { ST_READ, ST_WRITE };
typedef struct stub {
    uint8_t kind, size;
    uint16_t insn, dirty;
    uint16_t live;              /* host registers to preserve across the call */
    int8_t addr, val, dst;      /* host registers (addr -1: constant in cval) */
    int8_t t, o, w;             /* a write's scratch registers (free in the stub) */
    uint32_t cval, imm;
    uint8_t has_imm;
    uint8_t *sites[3];
    uint8_t *ret;
} stub;

enum { EX_CHAIN, EX_CHECK, EX_INTERP, EX_FAULT, EX_SPIN };
typedef struct exitrec {
    uint8_t kind;
    uint16_t done, dirty;       /* instructions completed; cached registers to store */
    int32_t cyc;                /* their counts (cycles when timed) on this exit's path */
    uint32_t pc;
    uint8_t *sites[8];
    unsigned nsites;
} exitrec;

#define MAX_STUBS 256
#define MAX_EXITS 320

typedef struct cg {
    xbuf b;
    jit_state *j;
    m68k_cpu *cpu;
    insn *ins;
    unsigned n, cur;
    int8_t hreg[16];            /* guest register -> host register, -1 */
    uint16_t dirty;             /* cached guest registers changed so far */
    uint16_t tmask;             /* temporaries in use (by host register number) */
    int32_t delta[16];          /* (An)+ / -(An) steps of the current instruction, not yet committed */
    uint16_t stepped;           /* registers the current instruction steps or pushes through */
    uint8_t eflags;             /* the host's flags hold the guest's N Z V C for the next instruction */
    uint8_t eflags_in;          /* ... for the current instruction */
    uint8_t *entry;             /* the block's entry */
    uint8_t *loop_top;          /* after the entry's checks and loads: where a jump back to the start goes on */
    uint8_t *budget_imm;        /* the entry's budget comparison, patched to the final length */
    int stop_interp;            /* the block ends before an instruction the interpreter runs */
    stub stubs[MAX_STUBS];
    unsigned nstubs;
    exitrec exits[MAX_EXITS];
    unsigned nexits;
    int fail;
    int spin;                   /* the block is a busy-wait loop (idle skipping) */
    int spin_ctr;               /* its DBcc's counter register, -1 for a Bcc */
    int has_slow;               /* the current instruction has slow-path stubs */
    /* Counts (cycles when timed): pre[i] what the block's path charges
     * before instruction i (an instruction the interpreter runs for the
     * block charges its own, counted 0 here), qpre[i] a bound of it (each
     * instruction's largest count, at least 1); c_cur and b_cur the current
     * instruction's count on the path and its bound; need the block's bound,
     * what the entry asks the budget to hold. */
    int32_t pre[MAX_INSNS + 1], qpre[MAX_INSNS + 1];
    int32_t c_cur, b_cur, need;
    uint8_t *step_imm[MAX_INSNS];   /* helper_step's bound operand, patched at the end */
    uint8_t step_at[MAX_INSNS];
    unsigned nstep;
} cg;

static int talloc(cg *g) {
    for (unsigned i = 0; i < 7; i++) {
        const int r = temp_regs[i];
        if (!(g->tmask & (1u << r))) {
            g->tmask |= (uint16_t)(1u << r);
            return r;
        }
    }
    g->fail = 1;
    return XAX;
}
static int talloc_specific(cg *g, int r) {
    if (g->tmask & (1u << r)) g->fail = 1;
    g->tmask |= (uint16_t)(1u << r);
    return r;
}
static void tfree(cg *g, int r) {
    for (unsigned i = 0; i < 7; i++)
        if (temp_regs[i] == r) g->tmask &= (uint16_t)~(1u << r);
}

static xo greg(cg *g, unsigned n) { return g->hreg[n] >= 0 ? XR(g->hreg[n]) : XM(XBX, ROFF(n)); }

/* A guest register's value (with the pending step of An) in a register:
 * the cached host register itself when the instruction does not step that
 * register (its value is then still the read one when used) (*temp 0). */
static int gval(cg *g, unsigned n, int *temp) {
    if (g->hreg[n] >= 0 && !g->delta[n] && !(g->stepped & (1u << n))) {
        *temp = 0;
        return g->hreg[n];
    }
    const int t = talloc(g);
    x_load(&g->b, 4, t, greg(g, n));
    if (g->delta[n]) x_alu_i(&g->b, ALU_ADD, 4, XR(t), g->delta[n]);
    *temp = 1;
    return t;
}
static void mark_dirty(cg *g, unsigned n) {
    if (g->hreg[n] >= 0) g->dirty |= (uint16_t)(1u << n);
}
/* Writes the low `size` bytes of host register r into guest register n. */
static void gset(cg *g, unsigned n, int r, unsigned size) {
    x_mov(&g->b, size, greg(g, n), XR(r));
    mark_dirty(g, n);
}
static void gset_imm(cg *g, unsigned n, uint32_t v, unsigned size) {
    x_mov_mi(&g->b, size, greg(g, n), v);
    mark_dirty(g, n);
}

/* Stores the cached registers in `dirty` to the CPU structure. */
static void flush_regs(cg *g, uint16_t dirty) {
    for (unsigned n = 0; n < 16; n++)
        if ((dirty & (1u << n)) && g->hreg[n] >= 0) x_store(&g->b, 4, XM(XBX, ROFF(n)), g->hreg[n]);
}

/* `k` instructions done, `c` counts charged. */
static void account(cg *g, unsigned k, int32_t c) {
    if (k) x_alu_i(&g->b, ALU_ADD, 8, XM(XBX, OFF(instructions)), (int32_t)k);
    if (c) {
        x_alu_i(&g->b, ALU_ADD, 8, XM(XBX, OFF(cycles)), c);
        x_alu_i(&g->b, ALU_SUB, 8, XM(XBX, OFF(budget)), c);
    }
}

/* The counts on the block's path before instruction `done` (the current
 * instruction's own included when `done` is after it). */
static int32_t cyc_at(const cg *g, unsigned done) {
    return done > g->cur ? g->pre[g->cur] + g->c_cur : g->pre[done];
}

/* An exit record, shared by identical exits. */
static exitrec *exit_to_c(cg *g, unsigned kind, uint32_t pc, unsigned done, int32_t cyc, uint16_t dirty) {
    for (unsigned i = 0; i < g->nexits; i++) {
        exitrec *e = &g->exits[i];
        if (e->kind == kind && e->pc == pc && e->done == done && e->cyc == cyc && e->dirty == dirty && e->nsites < 8) return e;
    }
    if (g->nexits >= MAX_EXITS) {
        g->fail = 1;
        g->nexits = 0;
    }
    exitrec *e = &g->exits[g->nexits++];
    memset(e, 0, sizeof *e);
    e->kind = (uint8_t)kind;
    e->pc = pc;
    e->done = (uint16_t)done;
    e->cyc = cyc;
    e->dirty = dirty;
    return e;
}
static exitrec *exit_to(cg *g, unsigned kind, uint32_t pc, unsigned done, uint16_t dirty) {
    return exit_to_c(g, kind, pc, done, cyc_at(g, done), dirty);
}
/* A jump (cc 16: unconditional) to an exit. */
static void exit_jump(cg *g, exitrec *e, unsigned cc) {
    uint8_t *site = cc == 16 ? x_jmp(&g->b) : x_jcc(&g->b, cc);
    if (e->nsites < 8) e->sites[e->nsites++] = site;
    else g->fail = 1;
}

static stub *stub_new(cg *g, unsigned kind, unsigned size) {
    if (g->nstubs >= MAX_STUBS) {
        g->fail = 1;
        g->nstubs = 0;
    }
    stub *s = &g->stubs[g->nstubs++];
    memset(s, 0, sizeof *s);
    s->kind = (uint8_t)kind;
    s->size = (uint8_t)size;
    s->insn = (uint16_t)g->cur;
    s->dirty = g->dirty;
    s->addr = s->val = s->dst = -1;
    g->has_slow = 1;
    return s;
}

/* --- memory ----------------------------------------------------------------- */

/* An address operand: a host register, or a constant. */
typedef struct addr_t {
    int reg;        /* -1: constant */
    int temp;
    uint32_t c;
} addr_t;

static void addr_free(cg *g, addr_t *a) {
    if (a->reg >= 0 && a->temp) tfree(g, a->reg);
    a->reg = -1;
    a->temp = 0;
}

/* The page pointer and offset of an access, testing that the page is mapped
 * and the access does not cross it (else to the stub). The memory operand
 * of the access goes to *m (in registers t and o). */
static void page_lookup(cg *g, const addr_t *a, unsigned size, int map_reg, int t, int o, stub *s, xo *m) {
    xbuf *b = &g->b;
    if (a->reg < 0) {
        const uint32_t page = a->c >> M68K_PAGE_SHIFT, off = a->c & (M68K_PAGE_SIZE - 1);
        x_load(b, 8, t, XM(map_reg, (int32_t)(page * 8u)));
        x_test(b, 8, XR(t), t);
        s->sites[0] = x_jcc(b, CC_E);
        *m = XM(t, (int32_t)off);
        return;
    }
    x_load(b, 4, t, XR(a->reg));
    x_shift_i(b, SH_SHR, 4, XR(t), M68K_PAGE_SHIFT);
    x_load(b, 8, t, XMI(map_reg, t, 8, 0));
    x_test(b, 8, XR(t), t);
    s->sites[0] = x_jcc(b, CC_E);
    x_load(b, 4, o, XR(a->reg));
    x_alu_i(b, ALU_AND, 4, XR(o), M68K_PAGE_SIZE - 1);
    if (size == 2) {
        x_alu_i(b, ALU_CMP, 4, XR(o), M68K_PAGE_SIZE - 1);
        s->sites[1] = x_jcc(b, CC_E);
    } else if (size == 4) {
        x_alu_i(b, ALU_CMP, 4, XR(o), M68K_PAGE_SIZE - 4);
        s->sites[1] = x_jcc(b, CC_A);
    }
    *m = XMI(t, o, 1, 0);
}

/* A constant address whose access crosses a page end goes through a
 * register (the slow path splits it). */
static void addr_reg_if_crossing(cg *g, addr_t *a, unsigned size) {
    if (a->reg >= 0 || (a->c & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - size) return;
    a->reg = talloc(g);
    a->temp = 1;
    x_mov_ri(&g->b, a->reg, a->c);
}

/* Reads `size` bytes at a; the value zero-extended in a new temporary. */
static int mem_read(cg *g, addr_t *a, unsigned size) {
    xbuf *b = &g->b;
    addr_reg_if_crossing(g, a, size);
    stub *s = stub_new(g, ST_READ, size);
    const int t = talloc(g), o = talloc(g);
    s->live = (uint16_t)(g->tmask & ~(1u << t) & ~(1u << o));
    s->addr = (int8_t)a->reg;
    s->cval = a->c;
    s->dst = (int8_t)o;
    xo m;
    page_lookup(g, a, size, X12, t, o, s, &m);
    if (size == 1) {
        x_movzx(b, o, m, 1);
    } else if (size == 2) {
        if (g->j->movbe) {
            x_movbe_load(b, 2, o, m);
            x_movzx(b, o, XR(o), 2);
        } else {
            x_movzx(b, o, m, 2);
            x_shift_i(b, SH_ROL, 2, XR(o), 8);
        }
    } else if (g->j->movbe) {
        x_movbe_load(b, 4, o, m);
    } else {
        x_load(b, 4, o, m);
        x_bswap(b, o);
    }
    s->ret = b->p;
    tfree(g, t);
    return o;
}

/* The store itself: the low `size` bytes of v (or the immediate) at m, in
 * the guest's byte order (w: a scratch register). */
static void store_value(cg *g, xo m, unsigned size, int v, int has_imm, uint32_t imm, int w) {
    xbuf *b = &g->b;
    if (has_imm) {
        const uint32_t sw = size == 4 ? m68k_bswap32(imm) : size == 2 ? m68k_bswap16((uint16_t)imm) : imm;
        x_mov_mi(b, size, m, sw);
    } else if (size == 1) {
        x_store(b, 1, m, v);
    } else if (g->j->movbe) {
        x_movbe_store(b, size, m, v);
    } else {
        x_load(b, 4, w, XR(v));
        if (size == 2) x_shift_i(b, SH_ROL, 2, XR(w), 8);
        else x_bswap(b, w);
        x_store(b, size, m, w);
    }
}

/* Writes the low `size` bytes of register v (or the immediate) at a. */
static void mem_write(cg *g, addr_t *a, unsigned size, int v, int has_imm, uint32_t imm) {
    xbuf *b = &g->b;
    addr_reg_if_crossing(g, a, size);
    stub *s = stub_new(g, ST_WRITE, size);
    const int t = talloc(g), o = talloc(g), w = talloc(g);
    s->live = (uint16_t)(g->tmask & ~(1u << t) & ~(1u << o) & ~(1u << w));
    s->addr = (int8_t)a->reg;
    s->cval = a->c;
    s->val = (int8_t)(has_imm ? -1 : v);
    s->has_imm = (uint8_t)has_imm;
    s->imm = imm;
    s->t = (int8_t)t;
    s->o = (int8_t)o;
    s->w = (int8_t)w;
    xo m;
    page_lookup(g, a, size, X13, t, o, s, &m);
    store_value(g, m, size, v, has_imm, imm, w);
    s->ret = b->p;
    tfree(g, t);
    tfree(g, o);
    tfree(g, w);
}

/* --- effective addresses ----------------------------------------------------------------- */

static unsigned jea_step(unsigned reg, unsigned size) { return (reg == 7 && size == 1) ? 2u : size; }

/* The address of a memory operand; the (An)+ / -(An) steps go to the
 * pending deltas, committed at the end of the instruction. */
static addr_t ea_address(cg *g, const ead *e, unsigned size) {
    xbuf *b = &g->b;
    addr_t a = {-1, 0, 0};
    const unsigned an = 8u + e->reg;
    switch (e->k) {
    case EK_IND: a.reg = gval(g, an, &a.temp); break;
    case EK_POST:
        a.reg = gval(g, an, &a.temp);
        g->delta[an] += (int32_t)jea_step(e->reg, size);
        break;
    case EK_PRE:
        g->delta[an] -= (int32_t)jea_step(e->reg, size);
        a.reg = gval(g, an, &a.temp);
        break;
    case EK_DISP: {
        int tmp;
        const int r = gval(g, an, &tmp);
        const int t = tmp ? r : talloc(g);
        x_lea(b, 4, t, XM(r, e->disp));
        a.reg = t;
        a.temp = 1;
        break;
    }
    case EK_IDX: {
        if (e->ext020) x_mov_mi(b, 1, XM(XBX, OFF(ea_ext020)), 1);
        int t = -1, post = -1;
        if (e->xreg != 0xFF) {
            const unsigned x = e->xreg;
            t = talloc(g);
            x_load(b, 4, t, greg(g, x));
            if (g->delta[x]) x_alu_i(b, ALU_ADD, 4, XR(t), g->delta[x]);
            if (!e->xlong) x_movsx(b, t, XR(t), 2);
        }
        if (e->mi == 2) {           /* the index joins after the indirection */
            post = t;
            t = -1;
        }
        if (e->mi) {
            /* The intermediate address (the base, the base displacement and
             * a pre-index), the long read there, then the post-index and
             * the outer displacement (PRM 2.2.10-2.2.13). */
            addr_t in = {-1, 0, 0};
            if (e->base == 0) {
                int tmp;
                const int r = gval(g, an, &tmp);
                if (t < 0) {
                    const int u = tmp ? r : talloc(g);
                    x_lea(b, 4, u, XM(r, e->disp));
                    t = u;
                } else {
                    x_lea(b, 4, t, XMI(r, t, e->xscale, e->disp));
                    if (tmp) tfree(g, r);
                }
                in.reg = t;
                in.temp = 1;
            } else {
                const uint32_t c = (e->base == 2 ? e->abs : 0u) + (uint32_t)e->disp;
                if (t < 0) {
                    in.c = c;
                } else {
                    if (e->xscale > 1) x_lea(b, 4, t, XMI(-1, t, e->xscale, (int32_t)c));
                    else x_alu_i(b, ALU_ADD, 4, XR(t), (int32_t)c);
                    in.reg = t;
                    in.temp = 1;
                }
            }
            const int v = mem_read(g, &in, 4);
            addr_free(g, &in);
            if (post >= 0) {
                x_lea(b, 4, v, XMI(v, post, e->xscale, e->od));
                tfree(g, post);
            } else if (e->od) {
                x_alu_i(b, ALU_ADD, 4, XR(v), e->od);
            }
            a.reg = v;
            a.temp = 1;
            break;
        }
        if (e->base == 0) {
            int tmp;
            const int r = gval(g, an, &tmp);
            if (t < 0) {
                const int u = tmp ? r : talloc(g);
                x_lea(b, 4, u, XM(r, e->disp));
                t = u;
            } else {
                x_lea(b, 4, t, XMI(r, t, e->xscale, e->disp));
                if (tmp) tfree(g, r);
            }
        } else {
            const uint32_t c = (e->base == 2 ? e->abs : 0u) + (uint32_t)e->disp;
            if (t < 0) {
                a.c = c;
                break;
            }
            if (e->xscale > 1) x_lea(b, 4, t, XMI(-1, t, e->xscale, (int32_t)c));
            else x_alu_i(b, ALU_ADD, 4, XR(t), (int32_t)c);
        }
        a.reg = t;
        a.temp = 1;
        break;
    }
    case EK_ABS: a.c = e->abs; break;
    default: g->fail = 1; break;
    }
    return a;
}

/* A source operand: a host register (guest register or temporary), or an
 * immediate. */
typedef struct val_t {
    int reg;        /* -1: immediate */
    int temp;
    uint32_t imm;
} val_t;

static void val_free(cg *g, val_t *v) {
    if (v->reg >= 0 && v->temp) tfree(g, v->reg);
    v->reg = -1;
    v->temp = 0;
}

static val_t ea_value(cg *g, const ead *e, unsigned size) {
    val_t v = {-1, 0, 0};
    if (e->k == EK_D || e->k == EK_A) {
        v.reg = gval(g, e->k == EK_D ? e->reg : 8u + e->reg, &v.temp);
        return v;
    }
    if (e->k == EK_IMM) {
        v.imm = e->abs;
        return v;
    }
    addr_t a = ea_address(g, e, size);
    v.reg = mem_read(g, &a, size);
    v.temp = 1;
    addr_free(g, &a);
    return v;
}

/* A value in a register (an immediate goes to a temporary). */
static void val_reg(cg *g, val_t *v) {
    if (v->reg >= 0) return;
    v->reg = talloc(g);
    v->temp = 1;
    x_mov_ri(&g->b, v->reg, v->imm);
}

/* Commits the pending (An)+ / -(An) steps. */
static void commit_steps(cg *g) {
    for (unsigned n = 8; n < 16; n++) {
        if (!g->delta[n]) continue;
        x_alu_i(&g->b, ALU_ADD, 4, greg(g, n), g->delta[n]);
        mark_dirty(g, n);
        g->delta[n] = 0;
    }
}

/* --- flags ------------------------------------------------------------------------------- */

static unsigned live(cg *g) { return g->ins[g->cur].live; }

/* N and Z from the low `size` bytes of r. */
static void flags_nz(cg *g, int r, unsigned size, unsigned lv) {
    xbuf *b = &g->b;
    if (!(lv & (F_N | F_Z))) return;
    int t = r;
    if (size < 4) {
        t = talloc(g);
        x_load(b, 4, t, XR(r));
        x_shift_i(b, SH_SHL, 4, XR(t), 32u - 8u * size);
    }
    if (lv & F_N) x_store(b, 4, XM(XBX, OFF(fn)), t);
    if (lv & F_Z) x_store(b, 4, XM(XBX, OFF(fz)), t);
    if (t != r) tfree(g, t);
}
static void flags_vc_clear(cg *g, unsigned lv) {
    xbuf *b = &g->b;
    if ((lv & (F_V | F_C)) == (F_V | F_C) && OFF(fc) == OFF(fv) + 4) {
        x_mov_mi(b, 8, XM(XBX, OFF(fv)), 0);
        return;
    }
    if (lv & F_V) x_mov_mi(b, 4, XM(XBX, OFF(fv)), 0);
    if (lv & F_C) x_mov_mi(b, 4, XM(XBX, OFF(fc)), 0);
}
static void jflags_logic(cg *g, int r, unsigned size) {
    const unsigned lv = live(g);
    flags_nz(g, r, size, lv);
    flags_vc_clear(g, lv);
}
/* C (and X) and V from the host's flags after ADD/SUB/NEG/ADC/SBB on the
 * guest's size: captured into byte registers, stored later. */
typedef struct cv_t { int c, v; } cv_t;
static cv_t flags_cv_capture(cg *g, int with_x) {
    xbuf *b = &g->b;
    const unsigned lv = live(g);
    cv_t cv = {-1, -1};
    if (lv & (F_C | (with_x ? F_X : 0))) {
        cv.c = talloc(g);
        x_setcc(b, CC_B, XR(cv.c));
    }
    if (lv & F_V) {
        cv.v = talloc(g);
        x_setcc(b, CC_O, XR(cv.v));
    }
    return cv;
}
static void flags_cv_store(cg *g, cv_t *cv, int with_x) {
    xbuf *b = &g->b;
    const unsigned lv = live(g);
    if (cv->c >= 0) {
        x_movzx(b, cv->c, XR(cv->c), 1);
        if (lv & F_C) x_store(b, 4, XM(XBX, OFF(fc)), cv->c);
        if (with_x && (lv & F_X)) x_store(b, 4, XM(XBX, OFF(fx)), cv->c);
        tfree(g, cv->c);
    }
    if (cv->v >= 0) {
        x_movzx(b, cv->v, XR(cv->v), 1);
        x_shift_i(b, SH_SHL, 4, XR(cv->v), 31);
        x_store(b, 4, XM(XBX, OFF(fv)), cv->v);
        tfree(g, cv->v);
    }
    cv->c = cv->v = -1;
}

/* The x86 condition for a guest condition when the host's flags hold N Z V C
 * (PRM 3, conditional tests). */
static const uint8_t cc_x86[16] = {0xFF, 0xFF, CC_A, CC_BE, CC_AE, CC_B, CC_NE, CC_E, CC_NO, CC_O, CC_NS, CC_S, CC_GE, CC_L, CC_G, CC_LE};

/* Sets the host's flags so that x86 condition *xcc means the guest
 * condition cc is true; from the stored flags unless the previous
 * instruction left them in the host's. Returns 0 for "always", 1 for
 * "never", 2 when *xcc is to be tested. */
static int cond_test(cg *g, unsigned cc, unsigned *xcc) {
    xbuf *b = &g->b;
    cc &= 15u;
    if (cc == 0) return 0;
    if (cc == 1) return 1;
    if (g->eflags_in) {
        *xcc = cc_x86[cc];
        return 2;
    }
    switch (cc) {
    case 6: case 7:
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fz)), 0);
        *xcc = cc == 7 ? CC_E : CC_NE;
        return 2;
    case 10: case 11:
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fn)), 0);
        *xcc = cc == 11 ? CC_S : CC_NS;
        return 2;
    case 8: case 9:
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fv)), 0);
        *xcc = cc == 9 ? CC_S : CC_NS;
        return 2;
    case 4: case 5:
        x_test_i(b, 1, XM(XBX, OFF(fc)), 1);
        *xcc = cc == 5 ? CC_NE : CC_E;
        return 2;
    case 12: case 13: {
        const int t = talloc(g);
        x_load(b, 4, t, XM(XBX, OFF(fn)));
        x_alu_rm(b, ALU_XOR, 4, t, XM(XBX, OFF(fv)));
        tfree(g, t);
        *xcc = cc == 13 ? CC_S : CC_NS;
        return 2;
    }
    case 2: case 3: {
        /* LS: C or Z */
        const int t = talloc(g), u = talloc(g);
        x_load(b, 4, t, XM(XBX, OFF(fc)));
        x_alu_i(b, ALU_AND, 4, XR(t), 1);
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fz)), 0);
        x_setcc(b, CC_E, XR(u));
        x_alu_mr(b, ALU_OR, 1, XR(t), u);
        tfree(g, t);
        tfree(g, u);
        *xcc = cc == 3 ? CC_NE : CC_E;
        return 2;
    }
    default: {
        /* LE: Z or N != V */
        const int t = talloc(g), u = talloc(g);
        x_load(b, 4, t, XM(XBX, OFF(fn)));
        x_alu_rm(b, ALU_XOR, 4, t, XM(XBX, OFF(fv)));
        x_shift_i(b, SH_SHR, 4, XR(t), 31);
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fz)), 0);
        x_setcc(b, CC_E, XR(u));
        x_alu_mr(b, ALU_OR, 1, XR(t), u);
        tfree(g, t);
        tfree(g, u);
        *xcc = cc == 15 ? CC_NE : CC_E;
        return 2;
    }
    }
}

/* The flags the next instruction tests right away (0: none). */
static unsigned next_tests(cg *g) {
    if (g->cur + 1 >= g->n) return 0;
    const insn *nx = &g->ins[g->cur + 1];
    if ((nx->kind == K_BCC || nx->kind == K_DBCC || nx->kind == K_SCC) && (nx->cc & 15u) > 1) return cc_reads(nx->cc);
    return 0;
}

static void op_src(cg *g, unsigned op, unsigned size, int dst, const val_t *s) {
    if (s->reg < 0) x_alu_i(&g->b, op, size, XR(dst), (int32_t)s->imm);
    else x_alu_rm(&g->b, op, size, dst, XR(s->reg));
}
/* The host's flags for a condition test that follows: a compare d - s, or
 * a test of a result whose V and C are clear; `nz_only`: they are not, so
 * only an N or Z test can use it. */
static void reflag_cmp(cg *g, unsigned size, int d, const val_t *s) {
    if (!next_tests(g)) return;
    op_src(g, ALU_CMP, size, d, s);
    g->eflags = 1;
}
static void reflag_test(cg *g, unsigned size, int r, int nz_only) {
    const unsigned t = next_tests(g);
    if (!t || (nz_only && (t & ~(unsigned)(F_N | F_Z)))) return;
    x_test(&g->b, size, XR(r), r);
    g->eflags = 1;
}

/* --- instructions -------------------------------------------------------------------- */

/* The destination of a read-modify-write: a data register or memory. */
typedef struct dest_t {
    int is_reg;
    unsigned n;
    addr_t a;
} dest_t;

static int dest_read(cg *g, const ead *e, unsigned size, dest_t *d) {
    d->is_reg = e->k == EK_D;
    d->n = e->reg;
    d->a.reg = -1;
    d->a.temp = 0;
    d->a.c = 0;
    if (d->is_reg) {
        const int t = talloc(g);
        x_load(&g->b, 4, t, greg(g, e->reg));
        return t;
    }
    d->a = ea_address(g, e, size);
    return mem_read(g, &d->a, size);
}
static void dest_write(cg *g, dest_t *d, unsigned size, int r) {
    if (d->is_reg) {
        gset(g, d->n, r, size);
        return;
    }
    mem_write(g, &d->a, size, r, 0, 0);
    addr_free(g, &d->a);
}

static void t_move(cg *g, const insn *in) {
    val_t v = ea_value(g, &in->s, in->size);
    if (in->d.k == EK_D) {
        val_reg(g, &v);
        commit_steps(g);
        gset(g, in->d.reg, v.reg, in->size);
        jflags_logic(g, v.reg, in->size);
        reflag_test(g, in->size, v.reg, 0);
        val_free(g, &v);
        return;
    }
    addr_t a = ea_address(g, &in->d, in->size);
    if (v.reg < 0) {
        mem_write(g, &a, in->size, -1, 1, v.imm);
        addr_free(g, &a);
        commit_steps(g);
        const unsigned lv = live(g);
        const uint32_t sh = in->size == 4 ? v.imm : v.imm << (32u - 8u * in->size);
        if (lv & F_N) x_mov_mi(&g->b, 4, XM(XBX, OFF(fn)), sh);
        if (lv & F_Z) x_mov_mi(&g->b, 4, XM(XBX, OFF(fz)), sh);
        flags_vc_clear(g, lv);
        return;
    }
    mem_write(g, &a, in->size, v.reg, 0, 0);
    addr_free(g, &a);
    commit_steps(g);
    jflags_logic(g, v.reg, in->size);
    reflag_test(g, in->size, v.reg, 0);
    val_free(g, &v);
}

static void t_movea(cg *g, const insn *in) {
    val_t v = ea_value(g, &in->s, in->size);
    const int t = talloc(g);
    if (v.reg < 0) x_mov_ri(&g->b, t, in->size == 2 ? (uint32_t)(int32_t)(int16_t)v.imm : v.imm);
    else if (in->size == 2) x_movsx(&g->b, t, XR(v.reg), 2);
    else x_load(&g->b, 4, t, XR(v.reg));
    val_free(g, &v);
    commit_steps(g);
    gset(g, 8u + in->reg, t, 4);
    tfree(g, t);
}

static void t_moveq(cg *g, const insn *in) {
    gset_imm(g, in->reg, in->imm, 4);
    const unsigned lv = live(g);
    if (lv & F_N) x_mov_mi(&g->b, 4, XM(XBX, OFF(fn)), in->imm);
    if (lv & F_Z) x_mov_mi(&g->b, 4, XM(XBX, OFF(fz)), in->imm);
    flags_vc_clear(g, lv);
}

static void t_lea(cg *g, const insn *in) {
    addr_t a = ea_address(g, &in->s, 4);
    if (a.reg < 0) gset_imm(g, 8u + in->reg, a.c, 4);
    else gset(g, 8u + in->reg, a.reg, 4);
    addr_free(g, &a);
}

/* A7 - 4 -> A7 (pending); v -> (A7). */
static void push(cg *g, int v, int has_imm, uint32_t imm) {
    g->delta[15] -= 4;
    addr_t a = {-1, 0, 0};
    a.reg = gval(g, 15, &a.temp);
    mem_write(g, &a, 4, v, has_imm, imm);
    addr_free(g, &a);
}

static void t_pea(cg *g, const insn *in) {
    addr_t a = ea_address(g, &in->s, 4);
    if (a.reg < 0) push(g, -1, 1, a.c);
    else push(g, a.reg, 0, 0);
    addr_free(g, &a);
    commit_steps(g);
}

static void t_clr(cg *g, const insn *in) {
    if (in->d.k == EK_D) {
        gset_imm(g, in->d.reg, 0, in->size);
    } else {
        addr_t a = ea_address(g, &in->d, in->size);
        mem_write(g, &a, in->size, -1, 1, 0);
        addr_free(g, &a);
        commit_steps(g);
    }
    const unsigned lv = live(g);
    if (lv & F_N) x_mov_mi(&g->b, 4, XM(XBX, OFF(fn)), 0);
    if (lv & F_Z) x_mov_mi(&g->b, 4, XM(XBX, OFF(fz)), 0);
    flags_vc_clear(g, lv);
}

static void t_tst(cg *g, const insn *in) {
    val_t v = ea_value(g, &in->d, in->size);
    val_reg(g, &v);
    commit_steps(g);
    jflags_logic(g, v.reg, in->size);
    reflag_test(g, in->size, v.reg, 0);
    val_free(g, &v);
}

static void t_neg_not(cg *g, const insn *in) {
    dest_t d;
    const int r = dest_read(g, &in->d, in->size, &d);
    if (in->kind == K_NOT) {
        x_grp3(&g->b, 2, in->size, XR(r));
        dest_write(g, &d, in->size, r);
        commit_steps(g);
        jflags_logic(g, r, in->size);
        reflag_test(g, in->size, r, 0);
    } else {
        x_grp3(&g->b, 3, in->size, XR(r));
        cv_t cv = flags_cv_capture(g, 1);
        dest_write(g, &d, in->size, r);
        commit_steps(g);
        flags_nz(g, r, in->size, live(g));
        flags_cv_store(g, &cv, 1);
        reflag_test(g, in->size, r, 1);
    }
    tfree(g, r);
}

static void t_ext(cg *g, const insn *in) {
    const int t = talloc(g);
    const unsigned size = in->sub == 2 ? 2u : 4u;
    x_movsx(&g->b, t, greg(g, in->reg), in->sub == 3 ? 2u : 1u);
    gset(g, in->reg, t, size);
    jflags_logic(g, t, size);
    reflag_test(g, size, t, 0);
    tfree(g, t);
}

static void t_swap(cg *g, const insn *in) {
    const int t = talloc(g);
    x_load(&g->b, 4, t, greg(g, in->reg));
    x_shift_i(&g->b, SH_ROL, 4, XR(t), 16);
    gset(g, in->reg, t, 4);
    jflags_logic(g, t, 4);
    reflag_test(g, 4, t, 0);
    tfree(g, t);
}

/* MOVE SR,Dn: privileged (in user mode the block leaves before it and the
 * interpreter takes the exception); SR = the system byte and the CCR as
 * ccr_of composes it from the flags (all in memory here: it reads them). */
/* The CCR from the flags in the interpreter's form (ccr_of), in t (c a
 * scratch register). */
static void ccr_compose(cg *g, int t, int c) {
    xbuf *b = &g->b;
    x_load(b, 4, t, XM(XBX, OFF(fx)));
    x_alu_i(b, ALU_AND, 4, XR(t), 1);
    x_shift_i(b, SH_SHL, 4, XR(t), 4);
    x_load(b, 4, c, XM(XBX, OFF(fn)));
    x_shift_i(b, SH_SHR, 4, XR(c), 31);
    x_shift_i(b, SH_SHL, 4, XR(c), 3);
    x_alu_rm(b, ALU_OR, 4, t, XR(c));
    x_load(b, 4, c, XM(XBX, OFF(fv)));
    x_shift_i(b, SH_SHR, 4, XR(c), 31);
    x_shift_i(b, SH_SHL, 4, XR(c), 1);
    x_alu_rm(b, ALU_OR, 4, t, XR(c));
    x_load(b, 4, c, XM(XBX, OFF(fc)));
    x_alu_i(b, ALU_AND, 4, XR(c), 1);
    x_alu_rm(b, ALU_OR, 4, t, XR(c));
    x_alu_rm(b, ALU_XOR, 4, c, XR(c));
    x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(fz)), 0);
    x_setcc(b, CC_E, XR(c));
    x_shift_i(b, SH_SHL, 4, XR(c), 2);
    x_alu_rm(b, ALU_OR, 4, t, XR(c));
}
/* The flags set from the CCR in t, as ccr_set does (c a scratch register). */
static void ccr_store(cg *g, int t, int c) {
    xbuf *b = &g->b;
    x_load(b, 4, c, XR(t));
    x_shift_i(b, SH_SHR, 4, XR(c), 4);
    x_alu_i(b, ALU_AND, 4, XR(c), 1);
    x_store(b, 4, XM(XBX, OFF(fx)), c);
    x_load(b, 4, c, XR(t));
    x_alu_i(b, ALU_AND, 4, XR(c), 8);
    x_shift_i(b, SH_SHL, 4, XR(c), 28);
    x_store(b, 4, XM(XBX, OFF(fn)), c);
    x_load(b, 4, c, XR(t));
    x_shift_i(b, SH_SHR, 4, XR(c), 2);
    x_alu_i(b, ALU_AND, 4, XR(c), 1);
    x_alu_i(b, ALU_XOR, 4, XR(c), 1);
    x_store(b, 4, XM(XBX, OFF(fz)), c);
    x_load(b, 4, c, XR(t));
    x_alu_i(b, ALU_AND, 4, XR(c), 2);
    x_shift_i(b, SH_SHL, 4, XR(c), 30);
    x_store(b, 4, XM(XBX, OFF(fv)), c);
    x_load(b, 4, c, XR(t));
    x_alu_i(b, ALU_AND, 4, XR(c), 1);
    x_store(b, 4, XM(XBX, OFF(fc)), c);
    g->eflags = 0;
}

/* MOVE from SR (privileged from the 68010 on: the interpreter takes the
 * exception), to a data register or to memory. */
static void t_from_sr(cg *g, const insn *in) {
    xbuf *b = &g->b;
    x_alu_i(b, ALU_CMP, 1, XM(XBX, OFF(s)), 0);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
    const int t = talloc(g), c = talloc(g);
    ccr_compose(g, t, c);
    x_movzx(b, c, XM(XBX, OFF(sr_hi)), 2);
    x_alu_rm(b, ALU_OR, 4, t, XR(c));
    tfree(g, c);
    if (!in->sub) {
        gset(g, in->reg, t, 2);
    } else {
        addr_t a = ea_address(g, &in->d, 2);
        mem_write(g, &a, 2, t, 0, 0);
        addr_free(g, &a);
        commit_steps(g);
    }
    tfree(g, t);
}

/* The new CCR or SR: MOVE's operand, or the old one ORed, ANDed or XORed
 * with the immediate (the CCR forms' immediate taken to its five bits as
 * op_ori_ccr and op_eori_ccr do). */
static int sr_operand(cg *g, const insn *in, int sr) {
    xbuf *b = &g->b;
    const int t = talloc(g), c = talloc(g);
    if (in->sub == 0) {
        val_t v = ea_value(g, &in->s, 2);
        val_reg(g, &v);
        x_movzx(b, t, XR(v.reg), 2);
        val_free(g, &v);
    } else {
        ccr_compose(g, t, c);
        if (sr) {
            x_movzx(b, c, XM(XBX, OFF(sr_hi)), 2);
            x_alu_rm(b, ALU_OR, 4, t, XR(c));
        }
        const uint32_t imm = in->kind == K_TO_CCR && in->sub != 2 ? in->imm & 0x1Fu : in->imm & 0xFFFFu;
        x_alu_i(b, in->sub == 1 ? ALU_OR : in->sub == 2 ? ALU_AND : ALU_XOR, 4, XR(t), (int32_t)imm);
    }
    tfree(g, c);
    return t;
}

static void t_to_ccr(cg *g, const insn *in) {
    const int t = sr_operand(g, in, 0);
    commit_steps(g);
    const int c = talloc(g);
    ccr_store(g, t, c);
    tfree(g, c);
    tfree(g, t);
}

/* The bit field instructions (op_bitfield, which this follows step for
 * step) for a block: the operation and its extension word in jit_arg, the
 * memory form's <ea> in jit_a; registers and flags in the CPU structure.
 * A memory field goes through the page tables when its bytes lie in one
 * page that is mapped (for writing, when the instruction writes it): 1
 * otherwise, nothing done, and the interpreter runs the instruction. The
 * counts the operands add (a register offset or width, a field across a
 * long word on the 68040 or five bytes on the 68020/030) are charged here. */
static JIT_CALL int helper_bf(m68k_cpu *cpu) {
    const uint32_t op = cpu->jit_arg & 0xFFFFu, ext = cpu->jit_arg >> 16;
    const unsigned kind = (op >> 8) & 7u;
    const int32_t offset = (ext & 0x0800u) ? (int32_t)cpu->r[(ext >> 6) & 7u] : (int32_t)((ext >> 6) & 31u);
    unsigned width = (ext & 0x0020u) ? (cpu->r[ext & 7u] & 31u) : (ext & 31u);
    if (width == 0) width = 32;
    const unsigned dn = (ext >> 12) & 7u;
    const uint32_t ones = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
    const unsigned mode = (op >> 3) & 7u;
    const int writes = kind == 2 || kind == 4 || kind == 6 || kind == 7;
    uint32_t field;
    uint64_t data = 0;
    uint32_t addr = 0;
    unsigned nbytes = 0, bitoff = 0;
    uint8_t *p = NULL;
    if (mode == 0) {
        const uint32_t v = cpu->r[op & 7u], n = (unsigned)offset & 31u;
        field = (n ? (v << n) | (v >> (32u - n)) : v) >> (32u - width);
    } else {
        addr = cpu->jit_a + (uint32_t)(offset >> 3);
        bitoff = (unsigned)offset & 7u;
        nbytes = (bitoff + width + 7u) / 8u;
        const uint32_t off = addr & (M68K_PAGE_SIZE - 1);
        if (off + nbytes > M68K_PAGE_SIZE) return 1;
        uint8_t *page = (writes ? cpu->wmap : (uint8_t *const *)cpu->rmap)[addr >> M68K_PAGE_SHIFT];
        if (!page) return 1;
        p = page + off;
        uint64_t raw = 0;
        for (unsigned i = 0; i < nbytes; i++) raw = raw << 8 | p[i];
        data = raw << (64u - 8u * nbytes);
        field = (uint32_t)((data << bitoff) >> (64u - width));
    }
    {
        const int span = mode != 0 && (cpu->model >= M68K_68040 ? (addr & 3u) * 8u + bitoff + width > 32u : nbytes == 5);
        const unsigned flags = ((ext & 0x0820u) ? M68K_TQ_REGARG : 0u) | (span ? M68K_TQ_SPAN : 0u);
        if (flags) {
            const int64_t extra = m68k_timing_query(cpu, (uint16_t)op, flags);
            cpu->cycles += (uint64_t)extra;
            cpu->budget -= extra;
        }
    }
    uint32_t newfield = field, flagval = field;
    switch (kind) {
    case 1: cpu->r[dn] = field; break;
    case 2: newfield = ~field & ones; break;
    case 3: cpu->r[dn] = width == 32 ? field : (uint32_t)((int32_t)(field << (32u - width)) >> (32u - width)); break;
    case 4: newfield = 0; break;
    case 6: newfield = ones; break;
    case 7: newfield = cpu->r[dn] & ones; flagval = newfield; break;
    default: break;
    }
    cpu->fn = flagval << (32u - width);
    cpu->fz = flagval;
    cpu->fv = 0;
    cpu->fc = 0;
    if (writes) {
        if (mode == 0) {
            const unsigned r = op & 7u, n = (unsigned)offset & 31u;
            uint32_t t = n ? (cpu->r[r] << n) | (cpu->r[r] >> (32u - n)) : cpu->r[r];
            const uint32_t top = width == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> width);
            t = (t & ~top) | (width == 32 ? newfield : newfield << (32u - width));
            cpu->r[r] = n ? (t >> n) | (t << (32u - n)) : t;
        } else {
            const unsigned shiftpos = 64u - bitoff - width;
            const uint64_t fm = (uint64_t)ones << shiftpos;
            data = (data & ~fm) | ((uint64_t)newfield << shiftpos);
            const uint64_t out = data >> (64u - 8u * nbytes);
            for (unsigned i = 0; i < nbytes; i++) p[i] = (uint8_t)(out >> (8u * (nbytes - 1u - i)));
        }
    }
    return 0;
}

/* An instruction the interpreter's handler runs in place, for one that
 * touches no data memory (K_INPLACE): its words are in the block's page,
 * so nothing aborts it. A divide by zero is left to the interpreter (1:
 * nothing done, the block leaves before the instruction); the handler's
 * extra counts (a long count, a signed long multiply or divide) are
 * charged here, the table's by the block. */
static JIT_CALL int helper_inplace(m68k_cpu *cpu) {
    const uint32_t op = cpu->jit_arg, pc = cpu->jit_a;
    const uint8_t *page = cpu->pmap[pc >> M68K_PAGE_SHIFT];
    const uint8_t *w = page + (pc & (M68K_PAGE_SIZE - 1));
    if ((op & 0xF0C0u) == 0x80C0u) {
        const uint32_t divisor = (op & 0x38u) ? ld16(w + 2) : cpu->r[op & 7u] & 0xFFFFu;
        if (!divisor) return 1;
    } else if ((op & 0xFFC0u) == 0x4C40u) {
        const uint32_t divisor = (op & 0x38u) ? ld32(w + 4) : cpu->r[op & 7u];
        if (!divisor) return 1;
    }
    cpu->ipc = pc;
    cpu->pc = pc + 2;
    cpu->op = (uint16_t)op;
    cpu->undo_n = 0;
    cpu->table[op](cpu, op);
    const int64_t x = cpu->cyc_extra;
    cpu->cyc_extra = 0;
    cpu->cycles += (uint64_t)x;
    cpu->budget -= x;
    return 0;
}

static void t_inplace(cg *g, const insn *in) {
    xbuf *b = &g->b;
    flush_regs(g, g->dirty);
    g->dirty = 0;
    x_mov_mi(b, 4, XM(XBX, OFF(jit_a)), in->pc);
    x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), in->op);
    x_load(b, 8, XCX, XR(XBX));
    x_call_abs(b, (const void *)helper_inplace);
    x_test(b, 4, XR(XAX), XAX);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, 0), CC_NE);
    for (unsigned n = 0; n < 16; n++)
        if (g->hreg[n] >= 0) x_load(b, 4, g->hreg[n], XM(XBX, ROFF(n)));
    g->eflags = 0;
}

/* A bit field instruction (not BFFFO): helper_bf with the registers in the
 * CPU structure; when it declines (a memory field not in one mapped page)
 * the block leaves before the instruction and the interpreter runs it. */
static void t_bf(cg *g, const insn *in) {
    xbuf *b = &g->b;
    if (in->d.k != EK_NONE) {
        addr_t a = ea_address(g, &in->d, 1);
        if (a.reg >= 0) x_store(b, 4, XM(XBX, OFF(jit_a)), a.reg);
        else x_mov_mi(b, 4, XM(XBX, OFF(jit_a)), a.c);
        addr_free(g, &a);
        commit_steps(g);
    }
    flush_regs(g, g->dirty);
    g->dirty = 0;
    x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), (uint32_t)in->op | in->imm << 16);
    x_load(b, 8, XCX, XR(XBX));
    x_call_abs(b, (const void *)helper_bf);
    x_test(b, 4, XR(XAX), XAX);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, 0), CC_NE);
    for (unsigned n = 0; n < 16; n++)
        if (g->hreg[n] >= 0) x_load(b, 4, g->hreg[n], XM(XBX, ROFF(n)));
    g->eflags = 0;
}

/* The write of SR (m68k_write_sr_insn's, value in jit_v): nonzero when the
 * block must leave after the instruction (S or M changed, so the stack,
 * the maps and the privilege; or an event: an interrupt the new mask lets
 * through, tracing). */
static JIT_CALL int helper_sr(m68k_cpu *cpu) {
    const uint8_t s = cpu->s, m = cpu->m;
    m68k_set_sr(cpu, (uint16_t)cpu->jit_v);
    cpu->flow = 1;              /* as m68k_write_sr_insn */
    return cpu->s != s || cpu->m != m || cpu->events != 0;
}

/* MOVE to SR and ORI/ANDI/EORI to SR in supervisor mode (else the
 * interpreter takes the privilege violation): the operand read here, the
 * write itself by the interpreter's routine; the block leaves after it when
 * that changed the stack or maps or raised an event. */
static void t_to_sr(cg *g, const insn *in) {
    xbuf *b = &g->b;
    x_alu_i(b, ALU_CMP, 1, XM(XBX, OFF(s)), 0);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
    const int t = sr_operand(g, in, 1);
    commit_steps(g);
    x_store(b, 4, XM(XBX, OFF(jit_v)), t);
    tfree(g, t);
    flush_regs(g, g->dirty);
    g->dirty = 0;
    x_load(b, 8, XCX, XR(XBX));
    x_call_abs(b, (const void *)helper_sr);
    x_test(b, 4, XR(XAX), XAX);
    exit_jump(g, exit_to(g, EX_CHECK, in->next, g->cur + 1, 0), CC_NE);
    g->eflags = 0;
}

static void t_exg(cg *g, const insn *in) {
    unsigned x, y;
    if (in->sub == 0x08) { x = in->reg; y = in->op & 7u; }
    else if (in->sub == 0x09) { x = 8u + in->reg; y = 8u + (in->op & 7u); }
    else { x = in->reg; y = 8u + (in->op & 7u); }
    const int t = talloc(g), u = talloc(g);
    x_load(&g->b, 4, t, greg(g, x));
    x_load(&g->b, 4, u, greg(g, y));
    gset(g, x, u, 4);
    gset(g, y, t, 4);
    tfree(g, t);
    tfree(g, u);
}

/* OR, AND, SUB, ADD, CMP <ea>,Dn */
static void t_alu_er(cg *g, const insn *in) {
    val_t s = ea_value(g, &in->s, in->size);
    commit_steps(g);
    const int d = talloc(g);
    x_load(&g->b, 4, d, greg(g, in->reg));
    if (in->sub == ALU_CMP) {
        const int r = talloc(g);
        x_load(&g->b, 4, r, XR(d));
        op_src(g, ALU_SUB, in->size, r, &s);
        cv_t cv = flags_cv_capture(g, 0);
        flags_nz(g, r, in->size, live(g));
        flags_cv_store(g, &cv, 0);
        reflag_cmp(g, in->size, d, &s);
        tfree(g, r);
    } else if (in->sub == ALU_ADD || in->sub == ALU_SUB) {
        op_src(g, in->sub, in->size, d, &s);
        cv_t cv = flags_cv_capture(g, 1);
        gset(g, in->reg, d, in->size);
        flags_nz(g, d, in->size, live(g));
        flags_cv_store(g, &cv, 1);
        reflag_test(g, in->size, d, 1);
    } else {
        op_src(g, in->sub, in->size, d, &s);
        gset(g, in->reg, d, in->size);
        jflags_logic(g, d, in->size);
        reflag_test(g, in->size, d, 0);
    }
    tfree(g, d);
    val_free(g, &s);
}

/* <op> s,<ea> (a data register or memory): OR, AND, SUB, ADD, EOR, CMP. */
static void rmw_alu(cg *g, const insn *in, unsigned op, const val_t *s) {
    dest_t dd;
    const int d = dest_read(g, &in->d, in->size, &dd);
    if (op == ALU_CMP) {
        const int r = talloc(g);
        x_load(&g->b, 4, r, XR(d));
        op_src(g, ALU_SUB, in->size, r, s);
        cv_t cv = flags_cv_capture(g, 0);
        addr_free(g, &dd.a);
        commit_steps(g);
        flags_nz(g, r, in->size, live(g));
        flags_cv_store(g, &cv, 0);
        reflag_cmp(g, in->size, d, s);
        tfree(g, r);
        tfree(g, d);
        return;
    }
    op_src(g, op, in->size, d, s);
    if (op == ALU_ADD || op == ALU_SUB) {
        cv_t cv = flags_cv_capture(g, 1);
        dest_write(g, &dd, in->size, d);
        commit_steps(g);
        flags_nz(g, d, in->size, live(g));
        flags_cv_store(g, &cv, 1);
        reflag_test(g, in->size, d, 1);
    } else {
        dest_write(g, &dd, in->size, d);
        commit_steps(g);
        jflags_logic(g, d, in->size);
        reflag_test(g, in->size, d, 0);
    }
    tfree(g, d);
}

static void t_alu_re(cg *g, const insn *in) {
    val_t s = {-1, 0, 0};
    s.reg = gval(g, in->reg, &s.temp);
    rmw_alu(g, in, in->sub, &s);
    val_free(g, &s);
}

static void t_alu_i(cg *g, const insn *in) {
    val_t s = {-1, 0, in->imm};
    rmw_alu(g, in, in->sub, &s);
}

static void t_quick(cg *g, const insn *in) {
    if (in->d.k == EK_A) {
        /* ADDQ/SUBQ to An: the whole register, no flags. */
        x_alu_i(&g->b, in->sub, 4, greg(g, 8u + in->d.reg), (int32_t)in->imm);
        mark_dirty(g, 8u + in->d.reg);
        return;
    }
    val_t s = {-1, 0, in->imm};
    rmw_alu(g, in, in->sub, &s);
}

/* ADDA, SUBA, CMPA <ea>,An: the source sign-extended to 32 bits. */
static void t_alu_a(cg *g, const insn *in) {
    val_t s = ea_value(g, &in->s, in->size);
    commit_steps(g);
    const int t = talloc(g);
    if (s.reg < 0) x_mov_ri(&g->b, t, in->size == 2 ? (uint32_t)(int32_t)(int16_t)s.imm : s.imm);
    else if (in->size == 2) x_movsx(&g->b, t, XR(s.reg), 2);
    else x_load(&g->b, 4, t, XR(s.reg));
    val_free(g, &s);
    const unsigned an = 8u + in->reg;
    if (in->sub == ALU_CMP) {
        const int d = talloc(g), r = talloc(g);
        x_load(&g->b, 4, d, greg(g, an));
        x_load(&g->b, 4, r, XR(d));
        x_alu_rm(&g->b, ALU_SUB, 4, r, XR(t));
        cv_t cv = flags_cv_capture(g, 0);
        flags_nz(g, r, 4, live(g));
        flags_cv_store(g, &cv, 0);
        if (next_tests(g)) {
            x_alu_rm(&g->b, ALU_CMP, 4, d, XR(t));
            g->eflags = 1;
        }
        tfree(g, d);
        tfree(g, r);
    } else {
        x_alu_mr(&g->b, in->sub, 4, greg(g, an), t);
        mark_dirty(g, an);
    }
    tfree(g, t);
}

/* ADDX, SUBX Dy,Dx: X in, Z only cleared. */
static void t_addx(cg *g, const insn *in) {
    xbuf *b = &g->b;
    const int d = talloc(g), s = talloc(g);
    x_load(b, 4, d, greg(g, in->reg));
    x_load(b, 4, s, greg(g, in->imm));
    x_bt_i(b, 4, 4, XM(XBX, OFF(fx)), 0);
    x_alu_rm(b, in->sub, in->size, d, XR(s));
    tfree(g, s);
    cv_t cv = flags_cv_capture(g, 1);
    gset(g, in->reg, d, in->size);
    const unsigned lv = live(g);
    if (lv & (F_N | F_Z)) {
        const int t = talloc(g);
        x_load(b, 4, t, XR(d));
        if (in->size < 4) x_shift_i(b, SH_SHL, 4, XR(t), 32u - 8u * in->size);
        if (lv & F_Z) x_alu_mr(b, ALU_OR, 4, XM(XBX, OFF(fz)), t);
        if (lv & F_N) x_store(b, 4, XM(XBX, OFF(fn)), t);
        tfree(g, t);
    }
    flags_cv_store(g, &cv, 1);
    tfree(g, d);
}

static void t_cmpm(cg *g, const insn *in) {
    val_t s = ea_value(g, &in->s, in->size);
    val_t dv = ea_value(g, &in->d, in->size);
    commit_steps(g);
    const int r = talloc(g);
    x_load(&g->b, 4, r, XR(dv.reg));
    op_src(g, ALU_SUB, in->size, r, &s);
    cv_t cv = flags_cv_capture(g, 0);
    flags_nz(g, r, in->size, live(g));
    flags_cv_store(g, &cv, 0);
    reflag_cmp(g, in->size, dv.reg, &s);
    tfree(g, r);
    val_free(g, &s);
    val_free(g, &dv);
}

/* MULU.W, MULS.W: 16 x 16 -> 32 */
static void t_mul(cg *g, const insn *in) {
    val_t s = ea_value(g, &in->s, 2);
    commit_steps(g);
    const int a = talloc(g), c = talloc(g);
    if (s.reg < 0) x_mov_ri(&g->b, a, in->sub ? (uint32_t)(int32_t)(int16_t)s.imm : (s.imm & 0xFFFFu));
    else if (in->sub) x_movsx(&g->b, a, XR(s.reg), 2);
    else x_movzx(&g->b, a, XR(s.reg), 2);
    val_free(g, &s);
    if (in->sub) x_movsx(&g->b, c, greg(g, in->reg), 2);
    else x_movzx(&g->b, c, greg(g, in->reg), 2);
    x_imul(&g->b, 4, a, XR(c));
    tfree(g, c);
    gset(g, in->reg, a, 4);
    jflags_logic(g, a, 4);
    reflag_test(g, 4, a, 0);
    tfree(g, a);
}

/* BTST, BCHG, BCLR, BSET: Z = the bit's complement; Dn modulo 32, memory a
 * byte modulo 8. */
static void t_bit(cg *g, const insn *in) {
    xbuf *b = &g->b;
    const int is_dn = in->d.k == EK_D;
    /* The mask 1 << bit. */
    const int m = talloc(g);
    if (in->imm == 0xFFFFFFFFu) {
        const int c = talloc_specific(g, XCX);
        x_load(b, 4, c, greg(g, in->reg));
        x_alu_i(b, ALU_AND, 4, XR(c), is_dn ? 31 : 7);
        x_mov_ri(b, m, 1);
        x_shift_cl(b, SH_SHL, 4, XR(m));
        tfree(g, c);
    } else {
        x_mov_ri(b, m, 1u << (in->imm & (is_dn ? 31u : 7u)));
    }
    if (in->sub == 0) {
        /* BTST */
        val_t v = {-1, 0, 0};
        if (is_dn) v.reg = gval(g, in->d.reg, &v.temp);
        else v = ea_value(g, &in->d, 1);
        val_reg(g, &v);
        commit_steps(g);
        x_alu_rm(b, ALU_AND, 4, m, XR(v.reg));
        if (live(g) & F_Z) x_store(b, 4, XM(XBX, OFF(fz)), m);
        const unsigned t = next_tests(g);
        if (t && !(t & ~(unsigned)F_Z)) {
            x_test(b, 4, XR(m), m);
            g->eflags = 1;
        }
        val_free(g, &v);
        tfree(g, m);
        return;
    }
    dest_t dd;
    int v;
    if (is_dn) {
        dd.is_reg = 1;
        dd.n = in->d.reg;
        dd.a.reg = -1;
        dd.a.temp = 0;
        dd.a.c = 0;
        v = talloc(g);
        x_load(b, 4, v, greg(g, in->d.reg));
    } else {
        v = dest_read(g, &in->d, 1, &dd);
    }
    const int z = talloc(g);
    x_load(b, 4, z, XR(v));
    x_alu_rm(b, ALU_AND, 4, z, XR(m));
    if (in->sub == 1) {
        x_alu_rm(b, ALU_XOR, 4, v, XR(m));
    } else if (in->sub == 2) {
        x_grp3(b, 2, 4, XR(m));
        x_alu_rm(b, ALU_AND, 4, v, XR(m));
    } else {
        x_alu_rm(b, ALU_OR, 4, v, XR(m));
    }
    tfree(g, m);
    dest_write(g, &dd, is_dn ? 4u : 1u, v);
    commit_steps(g);
    if (live(g) & F_Z) x_store(b, 4, XM(XBX, OFF(fz)), z);
    tfree(g, z);
    tfree(g, v);
}

/* AS, LS, RO by an immediate count 1-8 on Dn (PRM 4, ASL/ASR, LSL/LSR,
 * ROL/ROR), as the interpreter's shift(). */
static void t_shift(cg *g, const insn *in) {
    xbuf *b = &g->b;
    const unsigned size = in->size, bits = 8u * size, cnt = in->imm, type = in->sub, left = in->cc;
    const unsigned lv = live(g);
    const int v = talloc(g), r = talloc(g);
    x_load(b, 4, v, greg(g, in->reg));
    if (size < 4) x_movzx(b, v, XR(v), size);
    x_load(b, 4, r, XR(v));
    /* The bit shifted out last: C (and X for AS and LS). */
    int c = -1;
    if ((lv & (F_C | F_X)) && type != 3) {
        c = talloc(g);
        x_bt_i(b, 4, 4, XR(v), left ? bits - cnt : cnt - 1);
        x_setcc(b, CC_B, XR(c));
        x_movzx(b, c, XR(c), 1);
    }
    int ov = -1;
    switch (type) {
    case 0:
        if (left) {
            /* V: the most significant bit changed during the shift: the
             * top cnt+1 bits of the operand are not all equal. */
            if (lv & F_V) {
                ov = talloc(g);
                const int t2 = talloc(g);
                x_load(b, 4, ov, XR(v));
                x_shift_i(b, SH_SHL, 4, XR(ov), 32u - bits);
                x_load(b, 4, t2, XR(ov));
                x_shift_i(b, SH_SAR, 4, XR(t2), 31u - cnt);
                x_shift_i(b, SH_SAR, 4, XR(ov), 31);
                x_alu_rm(b, ALU_CMP, 4, t2, XR(ov));
                x_setcc(b, CC_NE, XR(ov));
                x_movzx(b, ov, XR(ov), 1);
                x_shift_i(b, SH_SHL, 4, XR(ov), 31);
                tfree(g, t2);
            }
            x_shift_i(b, SH_SHL, 4, XR(r), cnt);
        } else {
            x_shift_i(b, SH_SHL, 4, XR(r), 32u - bits);
            const unsigned n = cnt + 32u - bits;
            x_shift_i(b, SH_SAR, 4, XR(r), n > 31 ? 31 : n);
        }
        break;
    case 1:
        x_shift_i(b, left ? SH_SHL : SH_SHR, 4, XR(r), cnt);
        break;
    default: {
        const unsigned n = cnt & (bits - 1u);
        if (n) x_shift_i(b, left ? SH_ROL : SH_ROR, size, XR(r), n);
        break;
    }
    }
    if (size < 4) x_movzx(b, r, XR(r), size);
    gset(g, in->reg, r, size);
    flags_nz(g, r, size, lv);
    if (lv & F_V) {
        if (ov >= 0) x_store(b, 4, XM(XBX, OFF(fv)), ov);
        else x_mov_mi(b, 4, XM(XBX, OFF(fv)), 0);
    }
    if (type == 3) {
        if (lv & F_C) {
            /* ROL: C = bit 0 of the result; ROR: its msb. */
            const int t = talloc(g);
            x_load(b, 4, t, XR(r));
            if (left) x_alu_i(b, ALU_AND, 4, XR(t), 1);
            else x_shift_i(b, SH_SHR, 4, XR(t), bits - 1u);
            x_store(b, 4, XM(XBX, OFF(fc)), t);
            tfree(g, t);
        }
    } else if (c >= 0) {
        if (lv & F_C) x_store(b, 4, XM(XBX, OFF(fc)), c);
        if (lv & F_X) x_store(b, 4, XM(XBX, OFF(fx)), c);
    }
    if (c >= 0) tfree(g, c);
    if (ov >= 0) tfree(g, ov);
    reflag_test(g, size, r, 1);
    tfree(g, v);
    tfree(g, r);
}

static void t_scc(cg *g, const insn *in) {
    xbuf *b = &g->b;
    unsigned xcc = 0;
    const int k = cond_test(g, in->cc, &xcc);
    const int t = talloc(g);
    if (k == 0) {
        x_mov_ri(b, t, 0xFF);
    } else if (k == 1) {
        x_mov_ri(b, t, 0);
    } else {
        x_setcc(b, xcc, XR(t));
        x_movzx(b, t, XR(t), 1);
        x_grp3(b, 3, 4, XR(t));     /* 1 -> $FF..FF, 0 -> 0 */
    }
    if (in->d.k == EK_D) {
        gset(g, in->d.reg, t, 1);
    } else {
        addr_t a = ea_address(g, &in->d, 1);
        mem_write(g, &a, 1, t, 0, 0);
        addr_free(g, &a);
        commit_steps(g);
    }
    tfree(g, t);
}

static void t_link(cg *g, const insn *in) {
    /* SP - 4 -> SP; An -> (SP); SP -> An; SP + d -> SP. */
    const unsigned an = 8u + in->reg;
    g->delta[15] -= 4;
    addr_t a = {-1, 0, 0};
    a.reg = gval(g, 15, &a.temp);
    g->delta[15] = 0;
    int tmp = 0;
    const int v = in->reg == 7 ? a.reg : gval(g, an, &tmp);
    mem_write(g, &a, 4, v, 0, 0);
    if (tmp) tfree(g, v);
    if (in->reg != 7) gset(g, an, a.reg, 4);
    const int t = talloc(g);
    x_lea(&g->b, 4, t, XM(a.reg, (int32_t)in->imm));
    gset(g, 15, t, 4);
    tfree(g, t);
    addr_free(g, &a);
}

static void t_unlk(cg *g, const insn *in) {
    /* An -> SP; (SP) -> An; SP + 4 -> SP (then An, which wins for A7). */
    const unsigned an = 8u + in->reg;
    addr_t a = {-1, 0, 0};
    a.reg = gval(g, an, &a.temp);
    const int v = mem_read(g, &a, 4);
    const int t = talloc(g);
    x_lea(&g->b, 4, t, XM(a.reg, 4));
    gset(g, 15, t, 4);
    tfree(g, t);
    gset(g, an, v, 4);
    tfree(g, v);
    addr_free(g, &a);
}

/* MOVEM: only when the whole transfer is in one mapped page (else the
 * interpreter runs it, with its restart rules). */
static void t_movem(cg *g, const insn *in) {
    xbuf *b = &g->b;
    const unsigned mask = in->imm, size = in->size;
    unsigned count = 0;
    for (unsigned i = 0; i < 16; i++) count += (mask >> i) & 1u;
    const unsigned total = count * size;
    const unsigned an = 8u + in->s.reg;
    if (!total) {
        /* An empty list: (An)+ and -(An) leave An as it was. */
        if (in->s.k != EK_PRE && in->s.k != EK_POST) {
            addr_t a = ea_address(g, &in->s, size);
            addr_free(g, &a);
        }
        return;
    }
    addr_t a = {-1, 0, 0};
    if (in->s.k == EK_PRE || in->s.k == EK_POST) a.reg = gval(g, an, &a.temp);
    else a = ea_address(g, &in->s, size);
    if (a.reg < 0) {
        a.reg = talloc(g);
        a.temp = 1;
        x_mov_ri(b, a.reg, a.c);
    }
    /* The lowest address of the transfer. */
    const int st = talloc(g);
    if (in->s.k == EK_PRE) x_lea(b, 4, st, XM(a.reg, -(int32_t)total));
    else x_load(b, 4, st, XR(a.reg));
    /* First and last byte in one page, and that page mapped. */
    const int t = talloc(g), o = talloc(g);
    x_load(b, 4, o, XR(st));
    x_alu_i(b, ALU_AND, 4, XR(o), M68K_PAGE_SIZE - 1);
    x_alu_i(b, ALU_CMP, 4, XR(o), (int32_t)(M68K_PAGE_SIZE - total));
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_A);
    x_load(b, 4, t, XR(st));
    x_shift_i(b, SH_SHR, 4, XR(t), M68K_PAGE_SHIFT);
    x_load(b, 8, t, XMI(in->sub ? X12 : X13, t, 8, 0));
    x_test(b, 8, XR(t), t);
    if (in->sub) {
        exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
    } else {
        /* A page whose write mapping is held back for its code (as in
         * emit_stub): the stores go there when none of the bytes written is
         * code. */
        uint8_t *mapped = x_jcc(b, CC_NE);
        x_load(b, 4, t, XR(st));
        x_shift_i(b, SH_SHR, 4, XR(t), M68K_PAGE_SHIFT);
        x_load(b, 8, t, XMI(X13, t, 8, (int32_t)(2u * M68K_PAGE_COUNT * 8u)));
        x_test(b, 8, XR(t), t);
        exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
        const int i = talloc(g), m = talloc(g);
        x_load(b, 8, i, XR(t));
        x_shift_i(b, SH_SHR, 8, XR(i), M68K_PAGE_SHIFT);
        x_alu_i(b, ALU_AND, 4, XR(i), WCACHE_SIZE - 1);
        x_shift_i(b, SH_SHL, 4, XR(i), 4);
        x_lea_rip(b, m, (const uint8_t *)g->j->wcache);
        x_alu_rm(b, ALU_CMP, 8, t, XMI(m, i, 1, 0));
        exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_NE);
        x_load(b, 8, m, XMI(m, i, 1, 8));
        for (unsigned k = 0; k < total;) {
            const unsigned left = total - k, sz = left >= 8 ? 8u : left >= 4 ? 4u : left >= 2 ? 2u : 1u;
            x_alu_i(b, ALU_CMP, sz, XMI(m, o, 1, (int32_t)k), 0);
            exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_NE);
            k += sz;
        }
        tfree(g, i);
        tfree(g, m);
        x_bind(b, mapped);
    }
    x_alu_rm(b, ALU_ADD, 8, t, XR(o));      /* t = host address of the lowest byte */
    tfree(g, o);
    const int w = talloc(g);
    unsigned off = 0;
    if (!in->sub) {
        /* Registers to memory: ascending addresses hold ascending
         * registers (for -(An) the list is reversed and stored downwards,
         * which is the same layout). */
        for (unsigned r = 0; r < 16; r++) {
            const unsigned bit = in->s.k == EK_PRE ? 15u - r : r;
            if (!(mask & (1u << bit))) continue;
            if (in->s.k == EK_PRE && r == an) x_lea(b, 4, w, XM(a.reg, -(int32_t)size));   /* An's initial value minus the size */
            else x_load(b, 4, w, greg(g, r));
            if (size == 2) {
                x_shift_i(b, SH_ROL, 2, XR(w), 8);
                x_store(b, 2, XM(t, (int32_t)off), w);
            } else {
                x_bswap(b, w);
                x_store(b, 4, XM(t, (int32_t)off), w);
            }
            off += size;
        }
        if (in->s.k == EK_PRE) gset(g, an, st, 4);
    } else {
        for (unsigned r = 0; r < 16; r++) {
            if (!(mask & (1u << r))) continue;
            if (size == 2) {
                x_movzx(b, w, XM(t, (int32_t)off), 2);
                x_shift_i(b, SH_ROL, 2, XR(w), 8);
                x_movsx(b, w, XR(w), 2);
            } else {
                x_load(b, 4, w, XM(t, (int32_t)off));
                x_bswap(b, w);
            }
            gset(g, r, w, 4);
            off += size;
        }
        if (in->s.k == EK_POST) {
            x_alu_i(b, ALU_ADD, 4, XR(st), (int32_t)total);
            gset(g, an, st, 4);
        }
    }
    tfree(g, w);
    tfree(g, t);
    tfree(g, st);
    addr_free(g, &a);
}

/* MOVE16 (op_move16_pp, op_move16_abs): both lines aligned down to 16, so
 * each lies inside a page; when the source's page is mapped for reading
 * and the destination's for writing (a page holding code has its write
 * mapping held back) the 16 bytes move host to host, else the interpreter
 * runs the instruction, with its slow paths and restart rule. */
static void t_move16(cg *g, const insn *in) {
    xbuf *b = &g->b;
    const int src = talloc(g), dst = talloc(g);
    if (in->sub == 4) {
        x_load(b, 4, src, greg(g, 8u + in->reg));
        x_load(b, 4, dst, greg(g, 8u + in->cc));
    } else if (in->sub == 0 || in->sub == 2) {
        x_load(b, 4, src, greg(g, 8u + in->reg));
        x_mov_ri(b, dst, in->imm);
    } else {
        x_mov_ri(b, src, in->imm);
        x_load(b, 4, dst, greg(g, 8u + in->reg));
    }
    x_alu_i(b, ALU_AND, 4, XR(src), ~15);
    x_alu_i(b, ALU_AND, 4, XR(dst), ~15);
    /* The host addresses: page table entry plus offset. */
    const int sp = talloc(g), dp = talloc(g);
    x_load(b, 4, sp, XR(src));
    x_shift_i(b, SH_SHR, 4, XR(sp), M68K_PAGE_SHIFT);
    x_load(b, 8, sp, XMI(X12, sp, 8, 0));
    x_test(b, 8, XR(sp), sp);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
    x_load(b, 4, dp, XR(dst));
    x_shift_i(b, SH_SHR, 4, XR(dp), M68K_PAGE_SHIFT);
    x_load(b, 8, dp, XMI(X13, dp, 8, 0));
    x_test(b, 8, XR(dp), dp);
    exit_jump(g, exit_to(g, EX_INTERP, in->pc, g->cur, g->dirty), CC_E);
    x_alu_i(b, ALU_AND, 4, XR(src), M68K_PAGE_SIZE - 1);
    x_alu_i(b, ALU_AND, 4, XR(dst), M68K_PAGE_SIZE - 1);
    {
        /* Two quadwords through a free temporary. */
        const int v = talloc(g);
        x_load(b, 8, v, XMI(sp, src, 1, 0));
        x_store(b, 8, XMI(dp, dst, 1, 0), v);
        x_load(b, 8, v, XMI(sp, src, 1, 8));
        x_store(b, 8, XMI(dp, dst, 1, 8), v);
        tfree(g, v);
    }
    tfree(g, sp);
    tfree(g, dp);
    tfree(g, src);
    tfree(g, dst);
    /* The post-increments, Ax then Ay (the same register twice: 32). */
    const int t = talloc(g);
    if (in->sub == 4) {
        x_load(b, 4, t, greg(g, 8u + in->reg));
        x_alu_i(b, ALU_ADD, 4, XR(t), 16);
        gset(g, 8u + in->reg, t, 4);
        x_load(b, 4, t, greg(g, 8u + in->cc));
        x_alu_i(b, ALU_ADD, 4, XR(t), 16);
        gset(g, 8u + in->cc, t, 4);
    } else if (in->sub <= 1) {
        x_load(b, 4, t, greg(g, 8u + in->reg));
        x_alu_i(b, ALU_ADD, 4, XR(t), 16);
        gset(g, 8u + in->reg, t, 4);
    }
    tfree(g, t);
}

/* --- block exits ----------------------------------------------------------------------- */

/* A direct exit to `target` after `done` instructions; chained later. */
static void exit_chain(cg *g, unsigned cc, uint32_t target, unsigned done) {
    exit_jump(g, exit_to(g, EX_CHAIN, target, done, g->dirty), cc);
}
/* ... after the current instruction, which counts `own` on this path. */
static void exit_chain_c(cg *g, unsigned cc, uint32_t target, int32_t own) {
    exit_jump(g, exit_to_c(g, EX_CHAIN, target, g->cur + 1, g->pre[g->cur] + own, g->dirty), cc);
}

/* After an instruction with slow paths: leave when a helper asked for it.
 * MOVZX and JRCXZ leave the host's flags for a condition test that follows
 * (RCX is free between instructions). */
static void check_pending_to(cg *g, uint32_t pc_after) {
    if (!g->has_slow) return;
    x_movzx(&g->b, XCX, XM(XBX, OFF(jit_pending)), 1);
#if MC_JIT_A64
    uint8_t *none = x_jrcxz_fwd(&g->b);
    exit_jump(g, exit_to(g, EX_CHECK, pc_after, g->cur + 1, g->dirty), 16);
    x_bind(&g->b, none);
#else
    x_jrcxz(&g->b, 5);
    exit_jump(g, exit_to(g, EX_CHECK, pc_after, g->cur + 1, g->dirty), 16);
#endif
}
static void check_pending(cg *g) { check_pending_to(g, g->ins[g->cur].next); }

/* The jump to the address in EAX, everything stored and counted: leave
 * when a helper asked for it or the address is odd (the interpreter takes
 * the address error), else the target's block through the fast table. */
static void lookup_jump(cg *g, int pending) {
    xbuf *b = &g->b;
    x_test_i(b, 1, XR(XAX), 1);
    uint8_t *odd = x_jcc(b, CC_NE);
    if (pending) {
        x_alu_i(b, ALU_CMP, 1, XM(XBX, OFF(jit_pending)), 0);
        uint8_t *cont = x_jcc(b, CC_E);
        x_mov_ri(b, XAX, EXIT_CHECK);
        x_jmp_to(b, g->j->epilogue);
        x_bind(b, cont);
    }
    /* The fast table: entry (pc >> 1) mod FAST_SIZE, 16 bytes each. */
    x_load(b, 4, XCX, XR(XAX));
    x_shift_i(b, SH_SHR, 4, XR(XCX), 1);
    x_alu_i(b, ALU_AND, 4, XR(XCX), FAST_SIZE - 1);
    x_shift_i(b, SH_SHL, 4, XR(XCX), 5);
    x_mov_ri64(b, XDX, (uint64_t)(uintptr_t)g->j->fast);
    x_alu_rm(b, ALU_CMP, 4, XAX, XMI(XDX, XCX, 1, 0));
    uint8_t *miss = x_jcc(b, CC_NE);
    x_jmp_m(b, XMI(XDX, XCX, 1, 8));
    x_bind(b, miss);
    x_mov_ri(b, XAX, EXIT_NORMAL);
    x_jmp_to(b, g->j->epilogue);
    x_bind(b, odd);
    x_mov_ri(b, XAX, EXIT_CHECK);
    x_jmp_to(b, g->j->epilogue);
}

/* The indirect jump to the address in register r after `done` instructions. */
static void exit_indirect(cg *g, int r, unsigned done) {
    xbuf *b = &g->b;
    if (r != XAX) x_load(b, 4, XAX, XR(r));
    x_store(b, 4, XM(XBX, OFF(pc)), XAX);
    flush_regs(g, g->dirty);
    account(g, done, cyc_at(g, done));
    x_mov_mi(b, 4, XM(XBX, OFF(ipc)), g->ins[g->cur].pc);
    lookup_jump(g, g->has_slow);
}

static JIT_CALL void helper_step(m68k_cpu *cpu);
static JIT_CALL void helper_line_a(m68k_cpu *cpu);

/* An instruction the translator leaves to the interpreter: its handler,
 * called here (m68k_jit_step: fetch, handler, count, faults taken), then on
 * to wherever it left the PC. */
static void t_interp(cg *g, const insn *in) {
    xbuf *b = &g->b;
    flush_regs(g, g->dirty);
    g->dirty = 0;
    account(g, g->cur, g->pre[g->cur]);
    x_mov_mi(b, 4, XM(XBX, OFF(pc)), in->pc);
    /* The counts the block may still charge after this one (helper_step):
     * patched once the block's bound is known. */
#if MC_JIT_A64
    uint8_t *step_at = x_mov_mi_patch(b, XM(XBX, OFF(jit_arg)), 0);
#else
    x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), 0);
    uint8_t *step_at = b->p - 4;
#endif
    if (g->nstep < MAX_INSNS) {
        g->step_imm[g->nstep] = step_at;
        g->step_at[g->nstep++] = (uint8_t)(g->cur + 1);
    } else {
        g->fail = 1;
    }
    x_load(b, 8, XCX, XR(XBX));
    x_call_abs(b, (const void *)((in->op & 0xF000u) == 0xA000u ? helper_line_a : helper_step));
    x_load(b, 4, XAX, XM(XBX, OFF(pc)));
    if (in->ends) {
        lookup_jump(g, 1);
        return;
    }
    /* At the next instruction with nothing pending: the block goes on, its
     * registers reloaded (the handler may have changed any, and SR.S the
     * maps), the count taken back to the block's start (its exits count
     * from there). */
    x_alu_i(b, ALU_CMP, 4, XR(XAX), (int32_t)in->next);
    uint8_t *away = x_jcc(b, CC_NE);
    x_alu_i(b, ALU_CMP, 1, XM(XBX, OFF(jit_pending)), 0);
    uint8_t *on = x_jcc(b, CC_E);
    x_bind(b, away);
    lookup_jump(g, 1);
    x_bind(b, on);
    x_load(b, 8, X12, XM(XBX, OFF(rmap)));
    x_load(b, 8, X13, XM(XBX, OFF(wmap)));
    for (unsigned n = 0; n < 16; n++)
        if (g->hreg[n] >= 0) x_load(b, 4, g->hreg[n], XM(XBX, ROFF(n)));
    x_alu_i(b, ALU_SUB, 8, XM(XBX, OFF(instructions)), (int32_t)(g->cur + 1));
    if (g->pre[g->cur]) {
        /* (this instruction charged its own count: the block's path counts
         * it 0) */
        x_alu_i(b, ALU_SUB, 8, XM(XBX, OFF(cycles)), g->pre[g->cur]);
        x_alu_i(b, ALU_ADD, 8, XM(XBX, OFF(budget)), g->pre[g->cur]);
    }
    g->eflags = 0;
}

/* Counts of the branches (m68k_timing.c): the table holds Bcc not taken,
 * BRA and BSR; taken is bcc_taken; DBcc's table count is cc true, its
 * branch dbcc_branch, its counter's end dbcc_expired. On the block's path
 * (c_cur) a Bcc is not taken, or taken when the block follows it (inv). */
static int32_t taken_cost(const cg *g, const insn *in) {
    return in->cc <= 1 ? (int32_t)g->cpu->ctab[in->op] : (int32_t)g->cpu->tm->bcc_taken;
}

static void t_bcc(cg *g, const insn *in) {
    const unsigned done = g->cur + 1;
    if (in->cc == 1) {                      /* BSR */
        push(g, -1, 1, in->next);
        commit_steps(g);
        check_pending_to(g, in->target);
        exit_chain(g, 16, in->target, done);
        return;
    }
    unsigned xcc = 0;
    const int k = cond_test(g, in->cc, &xcc);
    const int32_t nt = (int32_t)g->cpu->ctab[in->op], tk = taken_cost(g, in);
    if (in->inv) {
        /* The block goes on at the target; not taken, it leaves. */
        if (k == 1) exit_chain_c(g, 16, in->next, nt);
        else if (k == 2) exit_chain_c(g, xcc ^ 1u, in->next, nt);
        return;
    }
    const int back = g->spin && in->target == g->ins[0].pc && g->cur == g->n - 1;
    if (k == 0 || k == 2) {
        const unsigned cc = k == 0 ? 16u : xcc;
        if (back) exit_jump(g, exit_to_c(g, EX_SPIN, in->target, done, g->pre[g->cur] + tk, g->dirty), cc);
        else exit_chain_c(g, cc, in->target, tk);
    }
}

static void t_dbcc(cg *g, const insn *in) {
    xbuf *b = &g->b;
    unsigned xcc = 0;
    const int k = cond_test(g, in->cc, &xcc);
    if (k == 0) return;                     /* condition true: falls through */
    uint8_t *skip = k == 2 ? x_jcc(b, xcc) : NULL;
    /* Dn.w - 1; taken unless it became -1 (a borrow from 0). */
    x_alu_i(b, ALU_SUB, 2, greg(g, in->reg), 1);
    mark_dirty(g, in->reg);
    const m68k_timing *tm = g->cpu->tm;
    if (g->spin && in->target == g->ins[0].pc && g->cur == g->n - 1)
        exit_jump(g, exit_to_c(g, EX_SPIN, in->target, g->cur + 1, g->pre[g->cur] + tm->dbcc_branch, g->dirty), CC_AE);
    else
        exit_chain_c(g, CC_AE, in->target, tm->dbcc_branch);
    /* The counter's end: its count differs from cc true's (the path's). */
    const int32_t d = (int32_t)tm->dbcc_expired - (int32_t)g->cpu->ctab[in->op];
    if (d) {
        x_alu_i(b, ALU_ADD, 8, XM(XBX, OFF(cycles)), d);
        x_alu_i(b, ALU_SUB, 8, XM(XBX, OFF(budget)), d);
    }
    if (skip) x_bind(b, skip);
}

static void t_rts(cg *g) {
    addr_t a = {-1, 0, 0};
    a.reg = gval(g, 15, &a.temp);
    const int v = mem_read(g, &a, 4);
    addr_free(g, &a);
    x_alu_i(&g->b, ALU_ADD, 4, greg(g, 15), 4);
    mark_dirty(g, 15);
    exit_indirect(g, v, g->cur + 1);
    tfree(g, v);
}

static void t_jmp(cg *g, const insn *in) {
    addr_t a = ea_address(g, &in->s, 4);
    if (a.reg < 0 && (a.c & 1u)) {
        g->fail = 1;                /* an odd target: the interpreter's address error */
        return;
    }
    if (in->kind == K_JSR) {
        if (a.reg >= 0 && !a.temp) {
            /* A held guest register (A7 itself for JSR (A7)): the target is
             * its value before the push. */
            const int t = talloc(g);
            x_load(&g->b, 4, t, XR(a.reg));
            a.reg = t;
            a.temp = 1;
        }
        push(g, -1, 1, in->next);
        commit_steps(g);
    }
    if (a.reg < 0) {
        check_pending_to(g, a.c);
        exit_chain(g, 16, a.c, g->cur + 1);
    } else {
        exit_indirect(g, a.reg, g->cur + 1);
        addr_free(g, &a);
    }
}

static void translate_insn(cg *g, const insn *in) {
    switch (in->kind) {
    case K_MOVE: t_move(g, in); break;
    case K_MOVEA: t_movea(g, in); break;
    case K_MOVEQ: t_moveq(g, in); break;
    case K_LEA: t_lea(g, in); break;
    case K_PEA: t_pea(g, in); break;
    case K_CLR: t_clr(g, in); break;
    case K_TST: t_tst(g, in); break;
    case K_NEG: case K_NOT: t_neg_not(g, in); break;
    case K_EXT: t_ext(g, in); break;
    case K_SWAP: t_swap(g, in); break;
    case K_FROM_SR: t_from_sr(g, in); break;
    case K_TO_SR: t_to_sr(g, in); break;
    case K_TO_CCR: t_to_ccr(g, in); break;
    case K_BF: t_bf(g, in); break;
    case K_INPLACE: t_inplace(g, in); break;
    case K_EXG: t_exg(g, in); break;
    case K_ALU_ER: t_alu_er(g, in); break;
    case K_ALU_RE: t_alu_re(g, in); break;
    case K_ALU_A: t_alu_a(g, in); break;
    case K_ALU_I: t_alu_i(g, in); break;
    case K_QUICK: t_quick(g, in); break;
    case K_ADDX: t_addx(g, in); break;
    case K_CMPM: t_cmpm(g, in); break;
    case K_MUL: t_mul(g, in); break;
    case K_BIT: t_bit(g, in); break;
    case K_SHIFT: t_shift(g, in); break;
    case K_SCC: t_scc(g, in); break;
    case K_LINK: t_link(g, in); break;
    case K_UNLK: t_unlk(g, in); break;
    case K_MOVEM: t_movem(g, in); break;
    case K_MOVE16: t_move16(g, in); break;
    case K_NOP: break;
    case K_BCC: t_bcc(g, in); break;
    case K_DBCC: t_dbcc(g, in); break;
    case K_RTS: t_rts(g); break;
    case K_JMP: case K_JSR: t_jmp(g, in); break;
    case K_INTERP: t_interp(g, in); break;
    default: g->fail = 1; break;
    }
}

/* --- the cold code ------------------------------------------------------------------------ */

static JIT_CALL void helper_spin(m68k_cpu *cpu, uint8_t *tries);

/* A busy-wait loop whose iterations stopped repeating this many times in a
 * row is left to run (the record costs more than it finds). */
#define SPIN_TRIES 32

static void emit_exit(cg *g, exitrec *e) {
    xbuf *b = &g->b;
    for (unsigned i = 0; i < e->nsites; i++) x_bind(b, e->sites[i]);
    flush_regs(g, e->dirty);
    account(g, e->done, e->cyc);
    /* IPC: the last instruction done, as the interpreter leaves it (an
     * exception taken next may stack it: an address error at an odd vector). */
    if (e->done && e->kind != EX_FAULT) x_mov_mi(b, 4, XM(XBX, OFF(ipc)), g->ins[e->done - 1].pc);
    switch (e->kind) {
    case EX_CHAIN: {
        if (e->pc == g->ins[0].pc && g->loop_top) {
            /* Back to the block's own start: with the budget for another
             * run, on from after the entry's loads (the registers are in
             * place and stored, the page cannot have changed without a
             * call-out leaving). */
            x_alu_i(b, ALU_CMP, 8, XM(XBX, OFF(budget)), g->need);
            x_jcc_to(b, CC_GE, g->loop_top);
        }
        /* The patchable jump: rel32 0 falls through to the exit. */
        uint8_t *site = x_jmp(b);
        x_bind(b, site);
        x_mov_mi(b, 4, XM(XBX, OFF(pc)), e->pc);
        x_lea_rip(b, XDX, site);
        x_store(b, 8, XM(XBX, OFF(jit_site)), XDX);
        x_mov_ri(b, XAX, EXIT_CHAIN);
        x_jmp_to(b, g->j->epilogue);
        break;
    }
    case EX_SPIN: {
        /* The busy-wait loop's back edge: the helper counts as many more
         * iterations as pass unchanged (helper_spin), then the loop goes on
         * from its entry. */
#if MC_JIT_A64
        uint8_t *tries = x_data_slot(b, 0);         /* the loop's count of failed tries, jumped over */
#else
        x8(b, 0xEB);                                /* JMP over the loop's count of failed tries */
        x8(b, 1);
        uint8_t *tries = b->p;
        x8(b, 0);
#endif
        x_lea_rip(b, XDX, tries);
        x_alu_i(b, ALU_CMP, 1, XM(XDX, 0), SPIN_TRIES);
        uint8_t *plain = x_jcc(b, CC_AE);
        x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), (int32_t)(g->n | (uint32_t)(g->spin_ctr + 1) << 16));
        x_mov_mi(b, 4, XM(XBX, OFF(jit_cyc)), (uint32_t)e->cyc);   /* an iteration's counts */
        x_load(b, 8, XCX, XR(XBX));
        x_call_abs(b, (const void *)helper_spin);
        x_bind(b, plain);
        x_mov_mi(b, 4, XM(XBX, OFF(pc)), e->pc);
        x_jmp_to(b, g->entry);
        break;
    }
    default:
        x_mov_mi(b, 4, XM(XBX, OFF(pc)), e->pc);
        if (e->kind == EX_FAULT) x_mov_mi(b, 4, XM(XBX, OFF(ipc)), e->pc);
        x_mov_ri(b, XAX, e->kind == EX_CHECK ? EXIT_CHECK : e->kind == EX_FAULT ? EXIT_FAULT : EXIT_INTERP);
        x_jmp_to(b, g->j->epilogue);
        break;
    }
}

static JIT_CALL void helper_read(m68k_cpu *cpu);
static JIT_CALL void helper_write(m68k_cpu *cpu);

static void emit_stub(cg *g, stub *s) {
    xbuf *b = &g->b;
    const insn *in = &g->ins[s->insn];
    if (s->kind == ST_WRITE && s->addr >= 0 && g->j->callout) {
        /* A page whose write mapping is held back because it holds code
         * (mmu.c, kind 3): when the bytes written are not code, the store
         * goes straight to the page, as the interpreter's fast path writes
         * it; otherwise the slow path, which drops the translations. */
        x_bind(b, s->sites[0]);
        const int t = s->t, o = s->o, w = s->w;
        uint8_t *slow[5];
        x_load(b, 4, o, XR(s->addr));
        x_shift_i(b, SH_SHR, 4, XR(o), M68K_PAGE_SHIFT);
        x_load(b, 8, t, XMI(X13, o, 8, (int32_t)(2u * M68K_PAGE_COUNT * 8u)));
        x_test(b, 8, XR(t), t);
        slow[0] = x_jcc(b, CC_E);
        x_load(b, 8, o, XR(t));
        x_shift_i(b, SH_SHR, 8, XR(o), M68K_PAGE_SHIFT);
        x_alu_i(b, ALU_AND, 4, XR(o), WCACHE_SIZE - 1);
        x_shift_i(b, SH_SHL, 4, XR(o), 4);
        x_lea_rip(b, w, (const uint8_t *)g->j->wcache);
        x_alu_rm(b, ALU_CMP, 8, t, XMI(w, o, 1, 0));
        slow[1] = x_jcc(b, CC_NE);
        x_load(b, 8, w, XMI(w, o, 1, 8));
        x_load(b, 4, o, XR(s->addr));
        x_alu_i(b, ALU_AND, 4, XR(o), M68K_PAGE_SIZE - 1);
        slow[2] = NULL;
        if (s->size == 2) {
            x_alu_i(b, ALU_CMP, 4, XR(o), M68K_PAGE_SIZE - 1);
            slow[2] = x_jcc(b, CC_E);
        } else if (s->size == 4) {
            x_alu_i(b, ALU_CMP, 4, XR(o), M68K_PAGE_SIZE - 4);
            slow[2] = x_jcc(b, CC_A);
        }
        x_alu_i(b, ALU_CMP, s->size, XMI(w, o, 1, 0), 0);     /* code bytes among those written */
        slow[3] = x_jcc(b, CC_NE);
        store_value(g, XMI(t, o, 1, 0), s->size, s->val, s->has_imm, s->imm, w);
        x_jmp_to(b, s->ret);
        for (unsigned i = 0; i < 4; i++) x_bind(b, slow[i]);
        s->sites[0] = NULL;
    }
    for (unsigned i = 0; i < 3; i++) x_bind(b, s->sites[i]);
    if (!g->j->callout) {
        /* Leave before the instruction; the interpreter runs it. */
        flush_regs(g, s->dirty);
        account(g, s->insn, g->pre[s->insn]);
        if (s->insn) x_mov_mi(b, 4, XM(XBX, OFF(ipc)), g->ins[s->insn - 1].pc);
        x_mov_mi(b, 4, XM(XBX, OFF(pc)), in->pc);
        x_mov_ri(b, XAX, EXIT_INTERP);
        x_jmp_to(b, g->j->epilogue);
        return;
    }
    /* The access's arguments go in the CPU structure; the shared thunk keeps
     * the temporaries and calls the helper, which counts the instructions
     * done so far for the device (and takes them back after). */
    if (s->addr >= 0) x_store(b, 4, XM(XBX, OFF(jit_a)), s->addr);
    else x_mov_mi(b, 4, XM(XBX, OFF(jit_a)), s->cval);
    if (s->kind == ST_WRITE) {
        /* The helper keeps the operand's low bytes. */
        if (s->has_imm) x_mov_mi(b, 4, XM(XBX, OFF(jit_v)), s->imm);
        else x_store(b, 4, XM(XBX, OFF(jit_v)), s->val);
    }
    /* The size and the instructions done; the counts done and the ones the
     * block may still charge from this instruction on. */
    x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), s->size | (unsigned)s->insn << 8);
    x_mov_mi(b, 4, XM(XBX, OFF(jit_cyc)), (uint32_t)g->pre[s->insn]);
    x_mov_mi(b, 4, XM(XBX, OFF(jit_need)), (uint32_t)(g->need - g->qpre[s->insn]));
    x_mov_mi(b, 4, XM(XBX, OFF(ipc)), in->pc);
#if MC_JIT_A64
    x_call_rel(b, s->kind == ST_READ ? g->j->thunk_read : g->j->thunk_write);
#else
    x8(b, 0xE8);
    x_patch(b->p, s->kind == ST_READ ? g->j->thunk_read : g->j->thunk_write);
    b->p += 4;
#endif
    if (s->kind == ST_READ) {
        x_alu_i(b, ALU_CMP, 4, XM(XBX, OFF(jit_fault)), 0);
        exit_jump(g, exit_to_c(g, EX_FAULT, in->pc, s->insn, g->pre[s->insn], s->dirty), CC_NE);
        x_load(b, 4, s->dst, XM(XBX, OFF(jit_val)));
    }
    x_jmp_to(b, s->ret);
}

/* --- the helpers blocks call -------------------------------------------------------------- */

/* Whether the block must leave after this instruction: an interrupt due,
 * the slice's end moved inside the block, or the block's own page
 * remapped. */
static void after_call(m68k_cpu *cpu, const uint8_t *before) {
    if (cpu->events || cpu->budget < (int64_t)cpu->jit_need || cpu->pmap[cpu->ipc >> M68K_PAGE_SHIFT] != before)
        cpu->jit_pending = 1;
}
static JIT_CALL void helper_read(m68k_cpu *cpu) {
    const uint8_t *before = cpu->pmap[cpu->ipc >> M68K_PAGE_SHIFT];
    const uint32_t done = cpu->jit_arg >> 8;
    const int64_t cyc = cpu->jit_cyc;
    cpu->instructions += done;
    cpu->cycles += (uint64_t)cyc;
    cpu->budget -= cyc;
    uint32_t v = 0;
    const uint32_t reads = cpu->bus_reads;
    cpu->jit_fault = (uint32_t)m68k_try_read(cpu, cpu->jit_a, cpu->jit_arg & 0xFFu, &v);
    cpu->jit_val = v;
    /* The polling record: one read through the bus (a device), or
     * something else (memory through the slow path: not a pure iteration). */
    if (cpu->bus_reads == reads + 1 && cpu->poll_n < 8 && !cpu->jit_fault) {
        cpu->poll_pa[cpu->poll_n] = cpu->bus_pa;
        cpu->poll_size[cpu->poll_n] = (uint8_t)(cpu->jit_arg & 0xFFu);
        cpu->poll_at[cpu->poll_n] = (uint32_t)cyc;
        cpu->poll_n++;
    } else {
        cpu->poll_bad = 1;
    }
    after_call(cpu, before);
    cpu->instructions -= done;
    cpu->cycles -= (uint64_t)cyc;
    cpu->budget += cyc;
}
/* The back edge of a busy-wait loop (is_spin). The iteration just run did
 * nothing through the slow path but device reads the chips call stable
 * (no table search, no memory through the slow path): every further
 * iteration does the same, reads the same, until an interrupt or the next
 * device event, which come only at the slice's end or at a deadline the
 * machine finds among the device accesses (m68k_bus.poll). Those whole
 * iterations are counted at once, with what each would leave: the ATC's
 * replacement counter steps of the device reads, the machine's device
 * time. Then the record starts again for the next iteration. */
static JIT_CALL void helper_spin(m68k_cpu *cpu, uint8_t *tries) {
    jit_state *j = J(cpu);
    const unsigned len = cpu->jit_arg & 0xFFFFu;
    const int ctr = (int)(cpu->jit_arg >> 16) - 1;
    const int64_t cyc = cpu->jit_cyc;       /* an iteration's counts */
    /* The iteration repeats when it ends where it started: the registers
     * (but the DBcc's counter, which nothing else in the loop uses, one
     * lower) and the flags it reads before setting (which it never sets). */
    int same = !cpu->poll_bad && cpu->mmu_searches == cpu->poll_searches && len;
    for (unsigned r = 0; r < 16 && same; r++)
        if ((int)r != ctr && cpu->r[r] != cpu->spin_snap[r]) same = 0;
    int64_t most = 0;
    if (same && !cpu->events && cyc > 0) {
        most = cpu->budget / cyc;
        /* A DBcc loop: until its counter would end it (the last iteration
         * runs as such). */
        if (ctr >= 0 && (int64_t)(cpu->r[ctr] & 0xFFFFu) < most) most = cpu->r[ctr] & 0xFFFFu;
    }
    int64_t k = 0;
    if (most > 0) {
        if (cpu->poll_n == 0) {
            k = most;
        } else if (cpu->bus.poll) {
            m68k_poll p;
            p.length = (unsigned)cyc;
            p.n = cpu->poll_n;
            p.budget = most * cyc;
            for (unsigned m = 0; m < p.n; m++) {
                p.address[m] = cpu->poll_pa[m];
                p.size[m] = cpu->poll_size[m];
                p.at[m] = cpu->poll_at[m];
            }
            k = cpu->bus.poll(cpu->bus.ctx, &p);
        }
    }
    if (k > 0) {
        const uint8_t step = (uint8_t)(cpu->mmu.ctr[0] - cpu->poll_ctr);
        cpu->mmu.ctr[0] = (uint8_t)(cpu->mmu.ctr[0] + (uint64_t)k * step);
        if (ctr >= 0) cpu->r[ctr] = (cpu->r[ctr] & 0xFFFF0000u) | ((cpu->r[ctr] - (uint32_t)k) & 0xFFFFu);
        cpu->instructions += (uint64_t)k * len;
        cpu->cycles += (uint64_t)(k * cyc);
        cpu->budget -= k * cyc;
        j->stats.fast_forwarded += (uint64_t)k * len;
        if (*tries) *tries = 0;
    } else if (*tries < SPIN_TRIES) {
        ++*tries;
    }
    cpu->poll_n = 0;
    cpu->poll_bad = 0;
    cpu->poll_ctr = cpu->mmu.ctr[0];
    cpu->poll_searches = cpu->mmu_searches;
}

/* --- idle loops through traps ------------------------------------------------------------- */

/* An idle loop the guest runs through system calls (the Finder's event loop
 * at rest: WaitNextEvent and the traps under it) is no busy-wait a block
 * can see, yet its iterations leave the state they found: the same
 * registers, the same memory (what an iteration writes, it writes back), no
 * device touched. From such a state the next iteration is the same again,
 * until an interrupt or a device event, which come only at the slice's end
 * (the machine's next deadline). Livadix's observer, for the
 * translator: an A-line instruction is an anchor; the second visit of an
 * anchor with the registers of the first starts a logged iteration (every
 * write through the slow path, the prior value of each byte written kept);
 * at the next visit the iteration is compared with where it started, in
 * the CPU and in every byte written, and when it repeats, the whole
 * iterations the slice still holds are counted at once, with the steps of
 * the replacement counters they would make. */

static void snap_take(const m68k_cpu *cpu, jsnap *s) {
    memset(s, 0, sizeof *s);
    memcpy(s->r, cpu->r, sizeof s->r);
    s->usp = cpu->usp;
    s->isp = cpu->isp;
    s->msp = cpu->msp;
    s->vbr = cpu->vbr;
    s->sfc = cpu->sfc;
    s->dfc = cpu->dfc;
    s->cacr = cpu->cacr;
    s->caar = cpu->caar;
    s->tc = cpu->tc;
    s->itt0 = cpu->itt0;
    s->itt1 = cpu->itt1;
    s->dtt0 = cpu->dtt0;
    s->dtt1 = cpu->dtt1;
    s->mmusr = cpu->mmusr;
    s->urp = cpu->urp;
    s->srp = cpu->srp;
    s->tc30 = cpu->mmu.tc30;
    memcpy(s->crp, cpu->mmu.crp, sizeof s->crp);
    memcpy(s->srp30, cpu->mmu.srp30, sizeof s->srp30);
    memcpy(s->tt30, cpu->mmu.tt30, sizeof s->tt30);
    s->mmu_searches = cpu->mmu_searches;
    s->bus_reads = cpu->bus_reads;
    s->bus_writes = cpu->bus_writes;
    s->irqs_taken = cpu->irqs_taken;
    s->dbra_runs = cpu->dbra_runs;
    s->sr = m68k_sr(cpu);
    s->mmusr30 = cpu->mmu.mmusr30;
    s->s = cpu->s;
    s->m = cpu->m;
    s->ipl = cpu->ipl;
    s->nmi_seen = cpu->nmi_seen;
    s->ea_ext020 = cpu->ea_ext020;
    s->next030 = cpu->mmu.next030;
    s->wb_n = cpu->wb_n;
    s->status = (uint8_t)cpu->status;
    s->events = (uint8_t)cpu->events;
    s->ctr[0] = cpu->mmu.ctr[0];
    s->ctr[1] = cpu->mmu.ctr[1];
    memcpy(s->atc, cpu->mmu.atc, sizeof s->atc);
    memcpy(s->atc030, cpu->mmu.atc030, sizeof s->atc030);
    memcpy(&s->fpu, &cpu->fpu, sizeof s->fpu);
}

static unsigned wlog_hash(const uint8_t *p) { return (unsigned)(((uintptr_t)p * 0x9E3779B97F4A7C15ull) >> 52) & (WLOG_SLOTS - 1); }

void m68k_jit_wlog(m68k_cpu *cpu, const uint8_t *p, unsigned size) {
    jit_state *j = J(cpu);
    if (!j || !j->log_loop) return;
    for (unsigned k = 0; k < size; k++) {
        unsigned h = wlog_hash(p + k);
        while (j->wkey[h] && j->wkey[h] != p + k) h = (h + 1) & (WLOG_SLOTS - 1);
        if (j->wkey[h]) continue;
        if (j->wn >= WLOG_MAX) {
            j->wover = 1;
            return;
        }
        j->wkey[h] = p + k;
        j->wold[h] = p[k];
        j->wused[j->wn++] = (uint16_t)h;
    }
}

/* Ends the logging: the write table back. */
static void log_stop(m68k_cpu *cpu, jit_state *j) {
    for (unsigned i = 0; i < j->wn; i++) j->wkey[j->wused[i]] = NULL;
    j->wn = 0;
    j->wover = 0;
    j->log_loop = NULL;
    cpu->wlog = 0;
    m68k_select_maps(cpu);
}

static void loop_failed(jloop *l) {
    l->verified = 0;
    if (l->fails < 12) l->fails++;
    l->backoff = 1u << l->fails;
}

void m68k_jit_wlog_end(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    if (!j) {
        cpu->wlog = 0;
        m68k_select_maps(cpu);
        return;
    }
    if (j->log_loop) loop_failed(j->log_loop);
    log_stop(cpu, j);
}

/* The iteration logged since the anchor's last visit has ended at it:
 * whether it left everything as it found it; the counters' steps in
 * steps[]. */
static int loop_repeats(m68k_cpu *cpu, jit_state *j, uint8_t steps[2]) {
    if (j->wover) return 0;
    static jsnap now;
    snap_take(cpu, &now);
    steps[0] = (uint8_t)(now.ctr[0] - j->log_snap.ctr[0]);
    steps[1] = (uint8_t)(now.ctr[1] - j->log_snap.ctr[1]);
    now.ctr[0] = j->log_snap.ctr[0];
    now.ctr[1] = j->log_snap.ctr[1];
    if (memcmp(&now, &j->log_snap, sizeof now) != 0) return 0;
    for (unsigned i = 0; i < j->wn; i++) {
        const unsigned h = j->wused[i];
        if (*j->wkey[h] != j->wold[h]) return 0;
    }
    return 1;
}

void m68k_jit_trap(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    if (!j) return;
    if (!j->idle_skip || cpu->on_exception || cpu->events || cpu->wb_n) {
        if (j->log_loop) {
            loop_failed(j->log_loop);
            log_stop(cpu, j);
        }
        return;
    }
    const uint32_t pc = cpu->ipc;
    jloop *l = &j->loops[(pc >> 1) & (LOOP_SLOTS - 1)];
    if (j->log_loop) {
        jloop *at = j->log_loop;
        const uint64_t len = cpu->instructions - j->log_start;
        const int64_t cyc = (int64_t)(cpu->cycles - j->log_cycles);
        if (at != l || at->pc != pc) {
            if (len > LOOP_MAX) {
                loop_failed(at);
                log_stop(cpu, j);
            }
            return;
        }
        uint8_t steps[2];
        const int same = len && loop_repeats(cpu, j, steps);
        log_stop(cpu, j);
        if (!same) {
            loop_failed(l);
            return;
        }
        l->verified = 1;
        l->fails = 0;
        /* The whole iterations the slice holds, this instruction's own
         * count kept. */
        const int64_t k = cyc > 0 ? (cpu->budget - 1) / cyc : 0;
        if (k > 0) {
            cpu->instructions += (uint64_t)k * len;
            cpu->cycles += (uint64_t)(k * cyc);
            cpu->budget -= k * cyc;
            cpu->mmu.ctr[0] = (uint8_t)(cpu->mmu.ctr[0] + (uint64_t)k * steps[0]);
            cpu->mmu.ctr[1] = (uint8_t)(cpu->mmu.ctr[1] + (uint64_t)k * steps[1]);
            j->stats.fast_forwarded += (uint64_t)k * len;
        }
        return;
    }
    if (l->pc == pc) {
        if (l->backoff) {
            l->backoff--;
            return;
        }
        if (l->verified || (l->sr == m68k_sr(cpu) && !memcmp(l->r, cpu->r, sizeof l->r))) {
            /* The iteration from here is logged. */
            snap_take(cpu, &j->log_snap);
            j->log_start = cpu->instructions;
            j->log_cycles = cpu->cycles;
            j->log_loop = l;
            j->wn = 0;
            j->wover = 0;
            cpu->wlog_map = j->wlog_map;
            cpu->wlog = 1;
            m68k_select_maps(cpu);
            return;
        }
    } else {
        l->pc = pc;
        l->verified = 0;
        l->fails = 0;
        l->backoff = 0;
    }
    l->sr = m68k_sr(cpu);
    memcpy(l->r, cpu->r, sizeof l->r);
}

/* The host's time stamp: the x86 TSC, or ARM64's virtual counter (CNTVCT_EL0). */
#if MC_JIT_A64 && defined(_MSC_VER)
#define jit_ticks() ((uint64_t)_ReadStatusReg(ARM64_CNTVCT))
#elif MC_JIT_A64
static inline uint64_t jit_ticks(void) {
    uint64_t t;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t));
    return t;
}
#else
#define jit_ticks() __rdtsc()
#endif

/* LIVADIX_JIT_PROFILE (a developer's tool): the instructions blocks hand to
 * the interpreter (helper_step), counted by opcode word with the host's
 * time stamp cycles they take, printed by family at exit: where
 * translating more would pay. */
static struct {
    int on;                         /* -1 not yet asked */
    unsigned last;
    uint64_t start, total;
    uint64_t count[65536], cycles[65536];
} prof = {-1};

static void prof_dump(void) {
    enum { NAMES = 512 };
    static const char *name[NAMES];
    static uint64_t count[NAMES], cycles[NAMES];
    unsigned n = 0;
    for (unsigned op = 0; op < 65536; op++) {
        if (!prof.count[op]) continue;
        const m68k_opinfo info = m68k_decode(M68K_68040, (uint16_t)op);
        const char *nm = info.name ? info.name : "?";
        unsigned k = 0;
        while (k < n && strcmp(name[k], nm)) k++;
        if (k == n) {
            if (n == NAMES) continue;
            name[n] = nm;
            count[n] = cycles[n] = 0;
            n++;
        }
        count[k] += prof.count[op];
        cycles[k] += prof.cycles[op];
    }
    fprintf(stderr, "jit profile: %.1f Mcycles of %.1f in instructions blocks handed to the interpreter%c",
            prof.total / 1e6, (jit_ticks() - prof.start) / 1e6, 10);
    for (unsigned r = 0; r < 30; r++) {
        unsigned best = n;
        for (unsigned k = 0; k < n; k++)
            if (cycles[k] && (best == n || cycles[k] > cycles[best])) best = k;
        if (best == n) break;
        fprintf(stderr, "  %-16s %10llu %9.1f Mcycles%c", name[best], (unsigned long long)count[best], cycles[best] / 1e6, 10);
        cycles[best] = 0;
    }
}

static JIT_CALL void helper_step(m68k_cpu *cpu) {
    if (prof.on < 0) {
        prof.on = getenv("LIVADIX_JIT_PROFILE") != NULL;
        if (prof.on) {
            atexit(prof_dump);
            prof.start = jit_ticks();
        }
    }
    uint64_t t0 = 0;
    if (prof.on) {
        const uint8_t *page = cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT];
        const uint32_t off = cpu->pc & (M68K_PAGE_SIZE - 1);
        prof.last = page && off < M68K_PAGE_SIZE - 1 ? (unsigned)(page[off] << 8 | page[off + 1]) : 0;
        t0 = jit_ticks();
    }
    const uint8_t *before = cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT];
    const uint32_t at = cpu->pc;
    m68k_jit_step(cpu);
    if (prof.on) {
        const uint64_t d = jit_ticks() - t0;
        prof.total += d;
        prof.count[prof.last]++;
        prof.cycles[prof.last] += d;
    }
    /* Something to take now, the block's own page remapped, or the slice
     * shortened below what the block still has: the block leaves. */
    if (cpu->events || cpu->pmap[at >> M68K_PAGE_SHIFT] != before || cpu->budget < (int64_t)cpu->jit_arg)
        cpu->jit_pending = 1;
}
/* Whether a supervisor data access of `size` bytes at `a` would be made
 * without an abort: through the soft TLB's table of its kind, or, without
 * the MMU's tables, through the bus's maps. */
static int sup_mapped(const m68k_cpu *cpu, uint32_t a, unsigned size, int store) {
    if ((a & (M68K_PAGE_SIZE - 1)) > M68K_PAGE_SIZE - size) return 0;
    const m68k_mmu *m = &cpu->mmu;
    if (m->mode) return m->tlb && !cpu->locked && m->tlb[(size_t)TLB_TABLE(store ? 2u : 1u, 1u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)] != NULL;
    uint8_t *const *map = store ? cpu->bus.write_map[1] : cpu->bus.read_map[1];
    return map && map[a >> M68K_PAGE_SHIFT] != NULL;
}

/* A line-A trap for a block: helper_step's work without the decode and the
 * setjmp when nothing in the exception can abort (supervisor mode, its
 * format $0 frame and the vector where the tables map them); helper_step
 * otherwise. */
static JIT_CALL void helper_line_a(m68k_cpu *cpu) {
    const uint32_t sp = cpu->r[15] - 8u;
    if (!cpu->s || cpu->before || prof.on < 0 || !sup_mapped(cpu, sp, 8, 1) || !sup_mapped(cpu, cpu->vbr + 0x28u, 4, 0)) {
        helper_step(cpu);
        return;
    }
    if (prof.on) {
        const uint8_t *page = cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT];
        const uint32_t off = cpu->pc & (M68K_PAGE_SIZE - 1);
        prof.last = page && off < M68K_PAGE_SIZE - 1 ? (unsigned)(page[off] << 8 | page[off + 1]) : 0;
    }
    const uint64_t t0 = prof.on ? jit_ticks() : 0;
    const uint8_t *before = cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT];
    const uint32_t at = cpu->pc;
    const uint32_t op = ld16(before + (at & (M68K_PAGE_SIZE - 1)));
    cpu->ipc = at;
    cpu->pc = at + 2;
    cpu->undo_n = 0;
    cpu->op = (uint16_t)op;
    m68k_exception_line_a(cpu);
    m68k_retire(cpu, op);
    if (cpu->events || cpu->pmap[at >> M68K_PAGE_SHIFT] != before || cpu->budget < (int64_t)cpu->jit_arg)
        cpu->jit_pending = 1;
    if (prof.on) {
        const uint64_t d = jit_ticks() - t0;
        prof.total += d;
        prof.count[prof.last]++;
        prof.cycles[prof.last] += d;
    }
}

static JIT_CALL void helper_write(m68k_cpu *cpu) {
    const uint8_t *before = cpu->pmap[cpu->ipc >> M68K_PAGE_SHIFT];
    const uint32_t done = cpu->jit_arg >> 8;
    const int64_t cyc = cpu->jit_cyc;
    cpu->instructions += done;
    cpu->cycles += (uint64_t)cyc;
    cpu->budget -= cyc;
    const unsigned size = cpu->jit_arg & 0xFFu;
    m68k_slow_write(cpu, cpu->jit_a, size, cpu->fc_data, cpu->jit_v & sz_mask(size));
    after_call(cpu, before);
    cpu->instructions -= done;
    cpu->cycles -= (uint64_t)cyc;
    cpu->budget += cyc;
}

/* --- translating a block ----------------------------------------------------------------- */

/* Flag liveness and the registers held in host registers. */
static void analyse(cg *g) {
    insn *ins = g->ins;
    const unsigned n = g->n;
    /* An exit at an instruction's start (a slow path leaving before it) or
     * after it (a pending check, a branch, the block's end) sees every
     * flag. */
    unsigned lv = F_ALL;
    for (unsigned i = n; i-- > 0;) {
        insn *in = &ins[i];
        if (in->mem || in->kind == K_BCC || in->kind == K_DBCC) lv = F_ALL;
        in->live = (uint8_t)(lv & in->fw);
        lv = (lv & ~(unsigned)in->fw) | in->fr;
        if (in->mem) lv = F_ALL;
    }
    unsigned uses[16] = {0};
    for (unsigned i = 0; i < n; i++) {
        const insn *in = &ins[i];
        const ead *es[2] = {&in->s, &in->d};
        for (unsigned k = 0; k < 2; k++) {
            const ead *e = es[k];
            if (e->k == EK_D) uses[e->reg] += 2;
            else if (e->k >= EK_A && e->k <= EK_DISP) uses[8u + e->reg] += 2;
            else if (e->k == EK_IDX && e->base == 0) uses[8u + e->reg] += 2;
            if (e->k == EK_IDX && e->xreg != 0xFF) uses[e->xreg] += 1;
        }
        switch (in->kind) {
        case K_MOVEA: case K_LEA: case K_ALU_A: uses[8u + in->reg] += 2; break;
        case K_MOVEQ: case K_ALU_ER: case K_ALU_RE: case K_MUL: case K_EXT: case K_SWAP: case K_SHIFT: case K_DBCC:
            uses[in->reg] += 2;
            break;
        case K_PEA: case K_JSR: case K_RTS: uses[15] += 2; break;
        case K_LINK: case K_UNLK: uses[15] += 2; uses[8u + in->reg] += 2; break;
        default: break;
        }
    }
    memset(g->hreg, -1, sizeof g->hreg);
    for (unsigned c = 0; c < 5; c++) {
        unsigned best = 16, most = 3;
        for (unsigned r = 0; r < 16; r++)
            if (g->hreg[r] < 0 && uses[r] > most) {
                most = uses[r];
                best = r;
            }
        if (best == 16) break;
        g->hreg[best] = (int8_t)cache_regs[c];
    }
}

/* A busy-wait loop: the last instruction branches back to the first, the
 * loop writes no memory, and one iteration's results depend only on the
 * memory it reads and on registers and flags the loop never writes: no
 * register is read in an iteration before the iteration writes it and then
 * written, no flag is read before the iteration sets it if the loop sets
 * it at all. Its state after one iteration is then the state after any
 * number of them, until something outside the CPU changes memory. */
#define MAX_LOOP 16

static uint32_t ea_regs(const ead *e) {
    switch (e->k) {
    case EK_D: return 1u << e->reg;
    case EK_A: case EK_IND: case EK_DISP: return 1u << (8u + e->reg);
    case EK_IDX: return (e->base == 0 ? 1u << (8u + e->reg) : 0u) | (e->xreg != 0xFF ? 1u << e->xreg : 0u);
    default: return 0;
    }
}
/* What a busy-wait loop may do: read memory and registers, compute in data
 * registers, compare and branch; nothing that writes memory, steps an
 * address register or leaves to the interpreter. */
static int spin_kind(const insn *in) {
    if (in->s.k == EK_POST || in->s.k == EK_PRE || in->d.k == EK_POST || in->d.k == EK_PRE) return 0;
    switch (in->kind) {
    case K_TST: case K_NOP: case K_MOVEQ: case K_SHIFT: case K_SWAP: case K_EXT: case K_DBCC: return 1;
    case K_BIT: return in->sub == 0;
    case K_ALU_I: return in->sub == ALU_CMP || in->d.k == EK_D;
    case K_ALU_ER: return 1;
    case K_ALU_A: return in->sub == ALU_CMP;
    case K_MOVE: return in->d.k == EK_D;
    case K_BCC: return in->cc != 1;
    default: return 0;
    }
}

/* A busy-wait loop (idle skipping): a block of such instructions whose last
 * one branches back to its start (Bcc, or DBcc whose counter nothing else
 * in the loop uses), reading no flag before it sets it unless the loop
 * never sets it. Whether an iteration repeats is then seen at its end
 * (helper_spin): the registers are where they were at its start. */
static int is_spin(const insn *ins, unsigned n, int *ctr) {
    if (n < 1 || n > MAX_LOOP) return 0;
    const insn *last = &ins[n - 1];
    if (last->target != ins[0].pc) return 0;
    if (last->kind == K_DBCC) *ctr = last->reg;
    else if (last->kind == K_BCC && last->cc != 1) *ctr = -1;
    else return 0;
    uint32_t used = 0;
    unsigned fset = 0, fread_first = 0;
    for (unsigned i = 0; i < n; i++) {
        const insn *in = &ins[i];
        if (!spin_kind(in) || (in->kind == K_DBCC && i != n - 1)) return 0;
        if (i != n - 1)
            used |= ea_regs(&in->s) | ea_regs(&in->d) | 1u << (in->reg & 15u) | 1u << (in->reg & 7u) | 1u << (8u + (in->reg & 7u));
        fread_first |= in->fr & ~fset;
        fset |= in->fw;
    }
    if (*ctr >= 0 && (used >> *ctr) & 1u) return 0;
    return (fread_first & fset) == 0;
}

/* A loop through forward branches (idle skipping): from `start` the
 * shortest path of busy-wait instructions back to it, taking conditional
 * branches either way inside the page; the branches it takes are followed
 * (insn.inv). Depth-first, within a budget of decodes. */
typedef struct loopfind {
    jit_state *j;
    const uint8_t *host;
    uint32_t start;
    insn path[MAX_LOOP];
    unsigned best_n, budget;
} loopfind;

static void loop_walk(loopfind *f, insn *best, uint32_t pc, unsigned n) {
    if (n >= MAX_LOOP || (f->best_n && n + 1 >= f->best_n) || !f->budget) return;
    f->budget--;
    if ((pc >> M68K_PAGE_SHIFT) != (f->start >> M68K_PAGE_SHIFT)) return;
    dctx d = {f->host, pc, 0};
    insn *in = &f->path[n];
    if (!decode(f->j, &d, in) || !spin_kind(in)) return;
    if ((in->kind == K_BCC || in->kind == K_DBCC) && in->target == f->start) {
        memcpy(best, f->path, (n + 1) * sizeof *best);
        f->best_n = n + 1;
        return;
    }
    if (in->kind == K_DBCC) return;
    if (in->kind == K_BCC) {
        const int fwd = in->target > in->pc && (in->target >> M68K_PAGE_SHIFT) == (f->start >> M68K_PAGE_SHIFT);
        if (in->cc == 0) {
            if (!fwd) return;
            in->inv = 1;
            in->ends = 0;
            loop_walk(f, best, in->target, n + 1);
            return;
        }
        loop_walk(f, best, in->next, n + 1);
        if (!fwd) return;
        in = &f->path[n];
        in->inv = 1;
        loop_walk(f, best, in->target, n + 1);
        in->inv = 0;
        return;
    }
    if (in->ends) return;
    loop_walk(f, best, in->next, n + 1);
}

static unsigned find_loop(jit_state *j, const uint8_t *host, uint32_t pc, insn *ins) {
    static loopfind f;
    f.j = j;
    f.host = host;
    f.start = pc;
    f.best_n = 0;
    f.budget = 64;
    loop_walk(&f, ins, pc, 0);
    return f.best_n;
}

/* An instruction's count on the block's path (what the interpreter would
 * charge for it there: m68k_timing.c) and its bound, the largest it may
 * charge on any path (at least 1). An instruction the interpreter runs for
 * the block charges its own (0 here). */
static int32_t jcost(const cg *g, const insn *in, int32_t *bound) {
    const m68k_cpu *cpu = g->cpu;
    if (in->kind == K_INTERP) {
        *bound = 1;
        return 0;
    }
    int32_t c = (int32_t)cpu->ctab[in->op];
    if (in->s.k == EK_IDX && in->s.full) c += m68k_timing_full(cpu, in->op, 0, in->s.extw, in->s.pcmode);
    if (in->d.k == EK_IDX && in->d.full) c += m68k_timing_full(cpu, in->op, in->kind == K_MOVE ? 1u : 0u, in->d.extw, in->d.pcmode);
    if (in->kind == K_MOVEM) c += m68k_timing_movem(cpu->tm, in->op, in->imm);
    int32_t bd = c;
    if (in->kind == K_INPLACE) {
        /* helper_inplace charges what the handler adds: a shift's long
         * count, a signed long multiply or divide. */
        int32_t most = 0;
        if ((in->op & 0xF000u) == 0xE000u) most = m68k_timing_query(cpu, in->op, M68K_TQ_BIG);
        else if ((in->op & 0xFF80u) == 0x4C00u && (in->imm & 0x0800u)) most = m68k_timing_query(cpu, in->op, M68K_TQ_SIGNED);
        if (most > 0) bd += most;
    }
    if (in->kind == K_BF) {
        /* helper_bf charges a register operand's and a spanning field's
         * counts itself: the bound holds the most it may add. */
        const unsigned reg = (in->imm & 0x0820u) ? M68K_TQ_REGARG : 0u;
        int32_t most = m68k_timing_query(cpu, in->op, reg | (in->d.k != EK_NONE ? M68K_TQ_SPAN : 0u));
        const int32_t other = reg ? m68k_timing_query(cpu, in->op, reg) : 0;
        if (other > most) most = other;
        if (most > 0) bd += most;
    }
    if (in->kind == K_BCC) {
        const int32_t tk = taken_cost(g, in);
        if (tk > bd) bd = tk;
        if (in->inv) c = tk;
    } else if (in->kind == K_DBCC) {
        if ((int32_t)cpu->tm->dbcc_branch > bd) bd = cpu->tm->dbcc_branch;
        if ((int32_t)cpu->tm->dbcc_expired > bd) bd = cpu->tm->dbcc_expired;
    }
    *bound = bd < 1 ? 1 : bd;
    return c;
}

static int emit_block(jit_state *j, cg *g, uint32_t pc0, const uint8_t *host) {
    xbuf *b = &g->b;
    analyse(g);
    g->b.p = j->code_free;
    g->b.end = j->code_end;
    g->b.overflow = 0;
#if MC_JIT_A64
    g->b.bad = 0;
#endif
    g->entry = b->p;
    /* Entry: enough budget for the whole block, and the logical page still
     * maps to the host page the code came from; otherwise the dispatcher
     * decides (nothing done). */
    g->budget_imm = x_alu_i32(b, ALU_CMP, 8, XM(XBX, OFF(budget)), (int32_t)g->n);
    exitrec *ef = exit_to_c(g, EX_CHECK, pc0, 0, 0, 0);     /* nothing done, nothing counted */
    exit_jump(g, ef, CC_L);
    x_load(b, 8, XAX, XM(XBX, OFF(pmap)));
    x_mov_ri64(b, XCX, (uint64_t)(uintptr_t)host);
    x_alu_rm(b, ALU_CMP, 8, XCX, XM(XAX, (int32_t)((pc0 >> M68K_PAGE_SHIFT) * 8u)));
    exit_jump(g, ef, CC_NE);
    x_load(b, 8, X12, XM(XBX, OFF(rmap)));
    x_load(b, 8, X13, XM(XBX, OFF(wmap)));
    for (unsigned n = 0; n < 16; n++)
        if (g->hreg[n] >= 0) x_load(b, 4, g->hreg[n], XM(XBX, ROFF(n)));
    g->loop_top = g->spin ? NULL : b->p;
    if (g->spin) {
        /* A busy-wait loop's iteration starts its record. */
        x_mov_mi(b, 2, XM(XBX, OFF(poll_n)), 0);    /* poll_n, poll_bad */
        x_movzx(b, XAX, XM(XBX, (int32_t)offsetof(m68k_cpu, mmu) + (int32_t)offsetof(m68k_mmu, ctr)), 1);
        x_store(b, 1, XM(XBX, OFF(poll_ctr)), XAX);
        x_load(b, 4, XAX, XM(XBX, OFF(mmu_searches)));
        x_store(b, 4, XM(XBX, OFF(poll_searches)), XAX);
        for (unsigned r = 0; r < 16; r += 2) {
            x_load(b, 8, XAX, XM(XBX, ROFF(r)));
            x_store(b, 8, XM(XBX, OFF(spin_snap) + 4 * (int32_t)r), XAX);
        }
    }
    g->dirty = 0;
    g->eflags = 0;
    g->nstep = 0;
    unsigned done = 0;
    int32_t pcyc = 0, qcyc = 0;
    for (g->cur = 0; g->cur < g->n; g->cur++) {
        const insn *in = &g->ins[g->cur];
        uint8_t *mark = b->p;
        const unsigned nst = g->nstubs, nex = g->nexits, nsp = g->nstep;
        g->pre[g->cur] = pcyc;
        g->qpre[g->cur] = qcyc;
        g->c_cur = jcost(g, in, &g->b_cur);
        const uint16_t dirty = g->dirty;
        const uint8_t eflags = g->eflags;
        g->has_slow = 0;
        g->fail = 0;
        g->tmask = 0;
        memset(g->delta, 0, sizeof g->delta);
        g->stepped = 0;
        if (in->s.k == EK_POST || in->s.k == EK_PRE) g->stepped |= (uint16_t)(1u << (8u + in->s.reg));
        if (in->d.k == EK_POST || in->d.k == EK_PRE) g->stepped |= (uint16_t)(1u << (8u + in->d.reg));
        if (in->kind == K_PEA || in->kind == K_JSR || in->kind == K_LINK || (in->kind == K_BCC && in->cc == 1))
            g->stepped |= 1u << 15;
        g->eflags_in = g->eflags;
        g->eflags = 0;
#if MC_JIT_A64
        g->b.bad = 0;
#endif
        translate_insn(g, in);
#if MC_JIT_A64
        if (g->b.bad) g->fail = 1;      /* a flag test the ARM64 flags could not answer: the interpreter's */
#endif
        if (!g->fail) {
            for (unsigned r = 8; r < 16; r++)
                if (g->delta[r]) g->fail = 1;   /* a step left uncommitted: not translated */
            if (g->tmask) g->fail = 1;          /* a temporary left allocated: not translated */
        }
        if (g->fail) {
            /* Not translatable here after all: the interpreter's handler
             * runs it, and the block ends with it. */
            b->p = mark;
            g->nstubs = nst;
            g->nexits = nex;
            g->nstep = nsp;
            g->dirty = dirty;
            g->tmask = 0;
            g->eflags = eflags;
            g->fail = 0;
            as_interp(&g->ins[g->cur]);
            g->has_slow = 0;
            memset(g->delta, 0, sizeof g->delta);
            g->c_cur = 0;
            g->b_cur = 1;
            t_interp(g, &g->ins[g->cur]);
            qcyc += 1;
            done = g->cur + 1;
            break;
        }
        if (!in->ends) check_pending(g);
        pcyc += g->c_cur;
        qcyc += g->b_cur;
        done = g->cur + 1;
        if (in->ends) break;
    }
    if (done == 0) return 0;
    g->n = done;
    g->pre[done] = pcyc;
    g->qpre[done] = qcyc;
    /* The entry asks the budget for the block's bound; each instruction the
     * interpreter runs for the block, for what may follow it. */
    g->need = qcyc;
    memcpy(g->budget_imm, &g->need, 4);
    for (unsigned i = 0; i < g->nstep; i++) {
        const int32_t rest = g->need - g->qpre[g->step_at[i]];
        memcpy(g->step_imm[i], &rest, 4);
    }
    const insn *last = &g->ins[done - 1];
    if (!last->ends) {
        g->cur = done - 1;
        g->c_cur = g->pre[done] - g->pre[done - 1];
        if (g->stop_interp) exit_jump(g, exit_to(g, EX_INTERP, last->next, done, g->dirty), 16);
        else exit_chain(g, 16, last->next, done);
    }
    /* Cold code: slow paths, then exits. */
    for (unsigned i = 0; i < g->nstubs; i++) emit_stub(g, &g->stubs[i]);
    for (unsigned i = 0; i < g->nexits; i++) emit_exit(g, &g->exits[i]);
#if MC_JIT_A64
    if (b->bad) return -1;
#endif
    if (b->overflow || g->fail) return -1;
    j->code_free = b->p;
    while ((uintptr_t)j->code_free & 15u) *j->code_free++ = 0xCC;
    return 1;
}

/* DBcc Dn,* to itself, its condition false and Dn.w above 0 (jit_arg:
 * the register; jit_cyc: an execution's counts): as many branching
 * executions as the interpreter would run in the budget left (each needs
 * the budget above 0 at its start), at most Dn.w (the counter's last
 * execution is left to the interpreter); the machine charges their time
 * (m68k_bus.dbra) and may end the slice sooner. */
static JIT_CALL void helper_dbra_self(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    const unsigned reg = cpu->jit_arg & 7u;
    const int64_t t = cpu->jit_cyc;
    const uint32_t w = D(reg) & 0xFFFFu;
    int64_t c = (cpu->budget + t - 1) / t;
    if (c > (int64_t)w) c = w;
    if (cpu->bus.dbra) {
        cpu->dbra_runs++;
        c = cpu->bus.dbra(cpu->bus.ctx, c, (unsigned)t);
    }
    cpu->instructions += (uint64_t)c;
    cpu->cycles += (uint64_t)(c * t);
    cpu->budget -= c * t;
    j->stats.fast_forwarded += (uint64_t)c;
    D(reg) = (D(reg) & 0xFFFF0000u) | ((w - (uint32_t)c) & 0xFFFFu);
}

/* DBcc Dn,* (to itself): as many executions as the slice holds, at once. */
static int emit_dbra_self(jit_state *j, cg *g, uint32_t pc0, const uint8_t *host, const insn *in) {
    xbuf *b = &g->b;
    g->b.p = j->code_free;
    g->b.end = j->code_end;
    g->b.overflow = 0;
#if MC_JIT_A64
    g->b.bad = 0;
#endif
    g->entry = b->p;
    g->n = 1;
    g->cur = 0;
    g->eflags_in = 0;
    g->need = 1;
    x_alu_i(b, ALU_CMP, 8, XM(XBX, OFF(budget)), 1);
    uint8_t *e1 = x_jcc(b, CC_L);
    x_load(b, 8, XAX, XM(XBX, OFF(pmap)));
    x_mov_ri64(b, XCX, (uint64_t)(uintptr_t)host);
    x_alu_rm(b, ALU_CMP, 8, XCX, XM(XAX, (int32_t)((pc0 >> M68K_PAGE_SHIFT) * 8u)));
    uint8_t *e2 = x_jcc(b, CC_NE);
    unsigned xcc = 0;
    const int k = cond_test(g, in->cc, &xcc);
    g->tmask = 0;
    uint8_t *ctrue = NULL;
    if (k == 0) ctrue = x_jmp(b);
    else if (k == 2) ctrue = x_jcc(b, xcc);
    /* Dn.w = 0: the counter's end, an execution of its own count: the
     * interpreter's. */
    x_alu_i(b, ALU_CMP, 2, XM(XBX, ROFF(in->reg)), 0);
    uint8_t *last = x_jcc(b, CC_E);
    x_mov_mi(b, 4, XM(XBX, OFF(jit_arg)), in->reg);
    x_mov_mi(b, 4, XM(XBX, OFF(jit_cyc)), g->cpu->tm->dbcc_branch);
    x_load(b, 8, XCX, XR(XBX));
    x_call_abs(b, (const void *)helper_dbra_self);
    x_mov_mi(b, 4, XM(XBX, OFF(ipc)), pc0);
    x_mov_mi(b, 4, XM(XBX, OFF(pc)), pc0);
    x_mov_ri(b, XAX, EXIT_NORMAL);
    x_jmp_to(b, j->epilogue);
    if (ctrue) {
        /* Condition true: one execution, falling through. */
        x_bind(b, ctrue);
        account(g, 1, (int32_t)g->cpu->ctab[in->op]);
        x_mov_mi(b, 4, XM(XBX, OFF(ipc)), pc0);
        x_mov_mi(b, 4, XM(XBX, OFF(pc)), in->next);
        x_mov_ri(b, XAX, EXIT_NORMAL);
        x_jmp_to(b, j->epilogue);
    }
    x_bind(b, last);
    x_mov_mi(b, 4, XM(XBX, OFF(pc)), pc0);
    x_mov_ri(b, XAX, EXIT_INTERP);
    x_jmp_to(b, j->epilogue);
    x_bind(b, e1);
    x_bind(b, e2);
    x_mov_mi(b, 4, XM(XBX, OFF(pc)), pc0);
    x_mov_ri(b, XAX, EXIT_CHECK);
    x_jmp_to(b, j->epilogue);
#if MC_JIT_A64
    if (b->bad) return -1;
#endif
    if (b->overflow) return -1;
    j->code_free = b->p;
    while ((uintptr_t)j->code_free & 15u) *j->code_free++ = 0xCC;
    return 1;
}

static cg *the_cg;
static insn the_insns[MAX_INSNS];

/* Translates the block at pc (host page `host`): NULL when the cache is full. */
static jblock *translate(jit_state *j, m68k_cpu *cpu, uint32_t pc, const uint8_t *host) {
    if (j->nblocks >= MAX_BLOCKS || (size_t)(j->code_end - j->code_free) < CODE_RESERVE) return NULL;
    if (!the_cg) the_cg = malloc(sizeof *the_cg);
    cg *g = the_cg;
    if (!g) return NULL;
    insn *ins = the_insns;
    /* The stub and exit tables are cleared as they are used. */
    memset(g, 0, offsetof(cg, stubs));
    g->nstubs = g->nexits = 0;
    g->fail = g->spin = g->has_slow = 0;
    g->spin_ctr = -1;
    g->j = j;
    g->cpu = cpu;
    g->ins = ins;
    dctx d = {host, pc, 0};
    unsigned n = 0;
    /* A page its owner keeps from translation: no instruction, the interpreter's. */
    const unsigned max = j->refuse && j->refuse(j->refuse_ctx, host) ? 0u : j->max_block ? j->max_block : MAX_INSNS;
    if (j->idle_skip && max) {
        n = find_loop(j, host, pc, ins);
        if (n > max) n = 0;
    }
    if (!n) while (n < max) {
        if (!decode(j, &d, &ins[n])) {
            g->stop_interp = d.bad != 2;    /* else the page's end: the next block is in the next page */
            break;
        }
        n++;
        /* A DBcc to itself whose time the machine charges starts a block
         * of its own (emit_dbra_self, or the interpreter's). */
        if (cpu->bus.dbra && n > 1 && ins[n - 1].kind == K_DBCC && ins[n - 1].target == ins[n - 1].pc) {
            n--;
            g->stop_interp = 0;
            break;
        }
        if (ins[n - 1].ends) break;
        /* A branch back to the block's start closes a loop: the block is
         * its body (a busy-wait is recognised as such, is_spin). */
        if ((ins[n - 1].kind == K_BCC || ins[n - 1].kind == K_DBCC) && ins[n - 1].target == pc) break;
    }
    g->n = n;
    int r = 0;
    if (n && j->idle_skip && ins[0].kind == K_DBCC && ins[0].target == pc) {
        r = emit_dbra_self(j, g, pc, host, &ins[0]);
        g->n = 1;
    } else if (n && cpu->bus.dbra && ins[0].kind == K_DBCC && ins[0].target == pc) {
        r = 0;      /* charged per execution: the interpreter runs it */
    } else if (n) {
        g->spin_ctr = -1;
        g->spin = j->idle_skip && is_spin(ins, n, &g->spin_ctr);
        r = emit_block(j, g, pc, host);
    }
    if (r < 0) return NULL;
#if MC_JIT_A64
    if (r > 0) a64_sync(g->entry, (size_t)(j->code_free - g->entry));
#endif
    if (r > 0 && j->hook) j->hook(j->hook_ctx, pc, g->n, g->entry, (size_t)(j->code_free - g->entry));
    jblock *blk = &j->blocks[j->nblocks++];
    memset(blk, 0, sizeof *blk);
    blk->pc = pc;
    blk->host = host;
    blk->ninsn = (uint16_t)(r > 0 ? g->n : 0);
    blk->need = r > 0 ? g->need : 0;
    blk->code = r > 0 ? g->entry : NULL;
    blk->end = blk->ninsn ? ins[g->n - 1].next : pc + 2;
    blk->hnext = j->hash[block_hash(pc)];
    j->hash[block_hash(pc)] = blk;
    if (!blk->code) {
        /* The interpreter runs the instruction here, while its bytes stay. */
        const uint32_t off = pc & (M68K_PAGE_SIZE - 1);
        blk->nbytes = (uint8_t)(M68K_PAGE_SIZE - off < sizeof blk->bytes ? M68K_PAGE_SIZE - off : sizeof blk->bytes);
        memcpy(blk->bytes, host + off, blk->nbytes);
        return blk;
    }
    jfast *f = &j->fast[fast_index(pc)];
    f->pc = pc;
    f->code = blk->code;
    f->host = host;
    f->need = (uint32_t)blk->need;
    /* The page's code words; its writes leave the fast path. */
    int created;
    jpage *pg = page_get(j, host, &created);
    if (!pg) {
        block_kill(j, blk);
        return NULL;
    }
    for (uint32_t a = pc & (M68K_PAGE_SIZE - 1); a <= ((blk->end - 1) & (M68K_PAGE_SIZE - 1)); a++) pg->code[a] = 1;
    jwcache *wc = &j->wcache[wcache_index(host)];
    wc->host = host;
    wc->code = pg->code;
    blk->pnext = pg->blocks;
    pg->blocks = blk;
    if (created) m68k_mmu_tlb_protect(cpu, host, 1);
    j->stats.blocks++;
    j->stats.translated_instructions += blk->ninsn;
    return blk;
}

/* --- the dispatcher ------------------------------------------------------------------------ */

static void add_site(jblock *b, uint8_t *site) {
    if (b->nsites == b->capsites) {
        const uint32_t cap = b->capsites ? b->capsites * 2 : 4;
        uint8_t **s = realloc(b->sites, cap * sizeof *s);
        if (!s) return;
        b->sites = s;
        b->capsites = cap;
    }
    b->sites[b->nsites++] = site;
}

int m68k_jit_exec(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    cpu->jit_pending = 0;
    for (;;) {
        const uint32_t pc = cpu->pc;
        const uint8_t *host = cpu->pmap[pc >> M68K_PAGE_SHIFT];
        if (!host) return 0;            /* the interpreter's fetch fills the page table */
        jfast *f = &j->fast[fast_index(pc)];
        uint8_t *entry;
        if (f->pc == pc && f->host == host && f->code) {
            if (cpu->budget < (int64_t)f->need) return 0;
            entry = f->code;
        } else {
            /* Not yet: the interpreter runs it until it has run often. A
             * count below the threshold also says no PC of its slot was
             * translated (a slot that reached it stays at 255). */
            uint8_t *h = &j->heat[(pc >> 1 ^ pc >> 17) & (HEAT_SIZE - 1)];
            if (*h + 1u < j->threshold) {
                ++*h;
                return 0;
            }
            jblock *b = block_find(j, pc, host);
            if (!b) {
                *h = 255;
                b = translate(j, cpu, pc, host);
                if (!b) {
                    flush_all(cpu, j);
                    b = translate(j, cpu, pc, host);
                    if (!b) return 0;
                }
            }
            if (!b->code || cpu->budget < b->need) return 0;
            f->pc = pc;
            f->code = b->code;
            f->host = host;
            f->need = (uint32_t)b->need;
            entry = b->code;
        }
        const uint64_t before = cpu->instructions;
        const int code = j->enter(cpu, entry);
        j->stats.exits++;
        j->stats.run_instructions += cpu->instructions - before;
        j->stats.exit_kinds[code & 7]++;
        cpu->jit_pending = 0;
        switch (code) {
        case EXIT_INTERP: return 0;
        case EXIT_FAULT: m68k_jit_fault(cpu); return 1;
        case EXIT_CHECK: return 1;
        case EXIT_CHAIN: {
            /* Chain the exit to the target's block. */
            uint8_t *site = (uint8_t *)cpu->jit_site;
            const uint8_t *th = cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT];
            if (th && site && !(cpu->pc & 1u)) {
                jblock *t = block_find(j, cpu->pc, th);
                if (!t && !j->threshold) t = translate(j, cpu, cpu->pc, th);
                if (t && t->code && !t->dead) {
                    x_patch(site, t->code);
#if MC_JIT_A64
                    a64_sync(site, 4);
#endif
                    add_site(t, site);
                    j->stats.chains++;
                }
            }
            break;
        }
        default: break;
        }
        if (cpu->events || cpu->budget <= 0 || (cpu->pc & 1u)) return 1;
    }
}

/* --- set-up ------------------------------------------------------------------------------------- */

#ifndef _WIN32
#define WLOG_MAP_SIZE (3u * (size_t)M68K_PAGE_COUNT * sizeof(uint8_t *))
/* Zeroed pages, reserved without being backed until written. */
static void *host_map(size_t size, int protection) {
    void *p = mmap(NULL, size, protection, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
#endif

static int has_movbe(void) {
#if MC_JIT_A64
    return 1;       /* a64emit.h's MOVBE: a load or store and REV */
#elif defined(_WIN32)
    int r[4];
    __cpuid(r, 1);
    return (r[2] >> 22) & 1;
#else
    unsigned a, b, c, d;
    return __get_cpuid(1, &a, &b, &c, &d) && ((c >> 22) & 1);
#endif
}

int m68k_jit_available(void) { return 1; }

static jit_state *jit_create(m68k_cpu *cpu) {
    jit_state *j = calloc(1, sizeof *j);
    if (!j) return NULL;
#ifdef _WIN32
    j->code = VirtualAlloc(NULL, CODE_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    j->wlog_map = VirtualAlloc(NULL, 3u * (size_t)M68K_PAGE_COUNT * sizeof(uint8_t *), MEM_COMMIT | MEM_RESERVE, PAGE_READONLY);
#else
    j->code = host_map(CODE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    j->wlog_map = host_map(WLOG_MAP_SIZE, PROT_READ);
#endif
    j->blocks = calloc(MAX_BLOCKS, sizeof *j->blocks);
    if (!j->code || !j->blocks || !j->wlog_map) {
#ifdef _WIN32
        if (j->code) VirtualFree(j->code, 0, MEM_RELEASE);
        if (j->wlog_map) VirtualFree(j->wlog_map, 0, MEM_RELEASE);
#else
        if (j->code) munmap(j->code, CODE_SIZE);
        if (j->wlog_map) munmap(j->wlog_map, WLOG_MAP_SIZE);
#endif
        free(j->blocks);
        free(j);
        return NULL;
    }
    j->code_end = j->code + CODE_SIZE;
    j->model = cpu->model;
    j->callout = 1;
    j->idle_skip = 1;
    j->threshold = 32;
    memset(j->heat, 0, sizeof j->heat);
    j->movbe = has_movbe();
    build_kinds(j);
    for (unsigned i = 0; i < FAST_SIZE; i++) j->fast[i].pc = 1;
#if MC_JIT_A64
    /* The trampoline, int enter(m68k_cpu *cpu, uint8_t *code): X19-X28,
     * X29 and X30 saved (AAPCS64, 96 bytes, SP 16-byte aligned), RBX's X19
     * the CPU; the epilogue returns RAX's X4 in X0. */
    xbuf b = {j->code, j->code_end, 0};
    j->enter = (int (JIT_CALL *)(m68k_cpu *, uint8_t *))(void *)b.p;
    a64_stp_pre(&b, 29, 30, -12);
    for (int i = 0; i < 5; i++) a64_stp(&b, 19u + 2u * (unsigned)i, 20u + 2u * (unsigned)i, 2 + 2 * i);
    x_load(&b, 8, XBX, XR(XCX));
    x_jmp_m(&b, XR(XDX));
    j->epilogue = b.p;
    a64_mov(&b, 1, 0, A64R(XAX));
    for (int i = 0; i < 5; i++) a64_ldp(&b, 19u + 2u * (unsigned)i, 20u + 2u * (unsigned)i, 2 + 2 * i);
    a64_ldp_post(&b, 29, 30, 12);
    x_ret(&b);
    /* The slow-path thunks, entered by BL: the temporaries' X0-X6 (and X7)
     * and X29 X30 kept (80 bytes), the helper with X0 = the CPU, the
     * page-table registers reloaded. */
    for (unsigned k = 0; k < 2; k++) {
        while ((uintptr_t)b.p & 15u) *b.p++ = 0xCC;
        if (k == 0) j->thunk_read = b.p;
        else j->thunk_write = b.p;
        a64_stp_pre(&b, 29, 30, -10);
        for (int i = 0; i < 4; i++) a64_stp(&b, 2u * (unsigned)i, 2u * (unsigned)i + 1u, 2 + 2 * i);
        x_load(&b, 8, XCX, XR(XBX));
        x_call_abs(&b, k == 0 ? (const void *)helper_read : (const void *)helper_write);
        for (int i = 0; i < 4; i++) a64_ldp(&b, 2u * (unsigned)i, 2u * (unsigned)i + 1u, 2 + 2 * i);
        x_load(&b, 8, X12, XM(XBX, OFF(rmap)));
        x_load(&b, 8, X13, XM(XBX, OFF(wmap)));
        a64_ldp_post(&b, 29, 30, 10);
        x_ret(&b);
    }
#else
    /* The trampoline, int enter(m68k_cpu *cpu, uint8_t *code): the
     * callee-saved registers (Windows x64: RBX RBP RSI RDI R12-R15) pushed,
     * 32 bytes of shadow space for the helpers' calls, RSP 16-byte aligned. */
    xbuf b = {j->code, j->code_end, 0};
    j->enter = (int (JIT_CALL *)(m68k_cpu *, uint8_t *))(void *)b.p;
    static const int saved[8] = {XBX, XBP, XSI, XDI, X12, X13, X14, X15};
    for (unsigned i = 0; i < 8; i++) x_push(&b, saved[i]);
    x_alu_i(&b, ALU_SUB, 8, XR(XSP), 40);
    x_load(&b, 8, XBX, XR(XCX));
    x_jmp_m(&b, XR(XDX));
    j->epilogue = b.p;
    x_alu_i(&b, ALU_ADD, 8, XR(XSP), 40);
    for (int i = 7; i >= 0; i--) x_pop(&b, saved[i]);
    x_ret(&b);
    /* The slow-path thunks: the temporaries kept (seven pushes after the
     * return address leave RSP 16-byte aligned), shadow space, the helper
     * with RCX = the CPU, the page-table registers reloaded. */
    for (unsigned k = 0; k < 2; k++) {
        while ((uintptr_t)b.p & 15u) *b.p++ = 0xCC;
        if (k == 0) j->thunk_read = b.p;
        else j->thunk_write = b.p;
        for (unsigned i = 0; i < 7; i++) x_push(&b, temp_regs[i]);
        x_alu_i(&b, ALU_SUB, 8, XR(XSP), 32);
        x_load(&b, 8, XCX, XR(XBX));
        x_call_abs(&b, k == 0 ? (const void *)helper_read : (const void *)helper_write);
        x_alu_i(&b, ALU_ADD, 8, XR(XSP), 32);
        for (int i = 6; i >= 0; i--) x_pop(&b, temp_regs[i]);
        x_load(&b, 8, X12, XM(XBX, OFF(rmap)));
        x_load(&b, 8, X13, XM(XBX, OFF(wmap)));
        x_ret(&b);
    }
#endif
    while ((uintptr_t)b.p & 15u) *b.p++ = 0xCC;
    j->wcache = (jwcache *)(void *)b.p;
    memset(j->wcache, 0, WCACHE_SIZE * sizeof *j->wcache);
    j->code_base = j->code_free = b.p + WCACHE_SIZE * sizeof *j->wcache;
#if MC_JIT_A64
    a64_sync(j->code, (size_t)(b.p - j->code));
#endif
    return j;
}

int m68k_jit_enable(m68k_cpu *cpu, int on) {
    if (cpu->model == M68K_68000) on = 0;   /* the interpreter's bus-cycle model (m68k.h) */
    if (!on) {
        m68k_jit_free(cpu);
        m68k_mmu_changed(cpu);
        return 0;
    }
    if (cpu->jit) return 1;
    jit_state *j = jit_create(cpu);
    if (!j) return 0;
    cpu->jit = j;
    m68k_mmu_changed(cpu);
    return 1;
}

void m68k_jit_options(m68k_cpu *cpu, int callout, int idle_skip, unsigned max_block) {
    jit_state *j = J(cpu);
    if (!j) return;
    j->callout = callout;
    j->idle_skip = idle_skip;
    j->max_block = max_block > MAX_INSNS ? MAX_INSNS : max_block;
    flush_all(cpu, j);
}

void m68k_jit_threshold(m68k_cpu *cpu, unsigned runs) {
    jit_state *j = J(cpu);
    if (j) j->threshold = runs > 255 ? 255 : runs;
}

void m68k_jit_get_stats(const m68k_cpu *cpu, m68k_jit_stats *stats) {
    const jit_state *j = cpu->jit;
    if (j) *stats = j->stats;
    else memset(stats, 0, sizeof *stats);
}

void m68k_jit_set_refuse_hook(m68k_cpu *cpu, m68k_jit_refuse_hook hook, void *ctx) {
    jit_state *j = J(cpu);
    if (!j) return;
    j->refuse = hook;
    j->refuse_ctx = ctx;
}

void m68k_jit_set_block_hook(m68k_cpu *cpu, m68k_jit_block_hook hook, void *ctx) {
    jit_state *j = J(cpu);
    if (!j) return;
    j->hook = hook;
    j->hook_ctx = ctx;
}

void m68k_jit_free(m68k_cpu *cpu) {
    jit_state *j = J(cpu);
    if (!j) return;
    if (cpu->mmu.tlb) m68k_mmu_tlb_protect(cpu, NULL, 0);
    for (unsigned i = 0; i < j->nblocks; i++) free(j->blocks[i].sites);
    for (unsigned i = 0; i < PAGE_SLOTS; i++)
        if (j->pages[i].host && j->pages[i].host != TOMB) free(j->pages[i].code);
    if (j->log_loop) {
        cpu->wlog = 0;
        m68k_select_maps(cpu);
    }
#ifdef _WIN32
    VirtualFree(j->code, 0, MEM_RELEASE);
    VirtualFree(j->wlog_map, 0, MEM_RELEASE);
#else
    munmap(j->code, CODE_SIZE);
    munmap(j->wlog_map, WLOG_MAP_SIZE);
#endif
    free(j->blocks);
    free(j);
    cpu->jit = NULL;
}

#endif /* MC_JIT_X64 */
