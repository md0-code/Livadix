/* The 680x0 CPU: a table-driven interpreter for the 68020, 68030, 68040 and
 * 68LC040 integer units, and the 68000 (m68k_000.c, its own bus-cycle
 * sequenced handlers).
 *
 * Written from the M68000 Family Programmer's Reference Manual (PRM, 1992
 * revision), the MC68040, MC68030 and MC68020 User's Manuals and the MC68851
 * PMMU User's Manual (sections cited where a behaviour is implemented). No
 * other emulator was consulted.
 *
 * Shape:
 *   - Decode: per model, a 65,536-entry table maps each opcode word to an
 *     instruction description (name, size, handler) generated at start-up
 *     from the PRM's encoding patterns (m68k_table.c). Encodings a model does
 *     not have decode as illegal, line A or line F. The table is the decode
 *     step a later JIT reuses.
 *   - Execute: one handler per instruction family, specialised by size and,
 *     for the hot forms, by addressing mode (m68k_ops.c).
 *   - Flags are kept as the values they derive from and the CCR is assembled
 *     only when read (m68k_ccr).
 *   - Memory goes through the machine's bus (m68k_bus): host page tables for
 *     RAM and ROM (4 KiB pages; NULL entries take the slow path) and
 *     callbacks for everything else, with the function code of each access.
 *   - Access faults are restartable: see "Access faults" below.
 *
 * The core keeps no wall clock and uses no host library beyond the C runtime.
 */
#ifndef MC_M68K_H
#define MC_M68K_H

#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

typedef enum m68k_model {
    M68K_68020,
    M68K_68030,
    M68K_68040,
    M68K_68LC040,       /* the 68040 without its FPU: FP opcodes raise line F (format $4) */
    M68K_68000,         /* the 68000 and 68HC000 (m68k_000.c): 24-bit bus, group 0 frames, no VBR, MOVEC, FPU or MMU */
    M68K_MODEL_COUNT
} m68k_model;

/* Function codes FC2-FC0 (MC68040 UM section 7, "Bus Operation": TT/TM
 * encodings; PRM section 1, supervisor and user address spaces). */
enum {
    M68K_FC_USER_DATA = 1,
    M68K_FC_USER_PROGRAM = 2,
    M68K_FC_SUPER_DATA = 5,
    M68K_FC_SUPER_PROGRAM = 6,
    M68K_FC_CPU = 7
};

#define M68K_PAGE_SHIFT 12u
#define M68K_PAGE_SIZE (1u << M68K_PAGE_SHIFT)
#define M68K_PAGE_COUNT (1u << (32u - M68K_PAGE_SHIFT))

/* Values the interrupt-acknowledge callback returns besides a vector number. */
#define M68K_IACK_AUTOVECTOR (-1)
#define M68K_IACK_SPURIOUS (-2)

/* The machine's bus.
 *
 * read_map/write_map: 1 M entries each, one per 4 KiB page of the 32-bit
 * address space, [0] for user and [1] for supervisor accesses. An entry
 * points at the host bytes of that page in big-endian guest order, or is
 * NULL. A NULL table means every access takes the slow path. Pages are a
 * cache: read/write must handle every address, mapped or not.
 *
 * read/write: size 1, 2 or 4 bytes; fc is the function code. Return 0 on
 * success, nonzero for a bus error (the CPU then builds the model's access
 * fault frame). An access the CPU splits (misaligned, crossing a page) calls
 * them once per piece.
 *
 * iack: the interrupt acknowledge cycle for a level 1-7; returns a vector
 * number (0-255), M68K_IACK_AUTOVECTOR or M68K_IACK_SPURIOUS. NULL means
 * autovectors.
 *
 * reset: the RESET instruction asserted the reset line (the machine resets
 * its devices; the CPU itself is not reset). May be NULL. */
/* A guest polling loop the translator would advance at once
 * (m68k_bus.poll): its iteration's device reads, in order. Counts are the
 * CPU's (m68k_run): clock cycles when the CPU is timed (m68k_set_timing),
 * instructions otherwise. */
typedef struct m68k_poll {
    unsigned length;                /* counts per iteration */
    unsigned n;                     /* device reads per iteration */
    uint32_t address[8];            /* physical */
    uint8_t size[8];
    uint32_t at[8];                 /* counts from the iteration's start to the instruction that reads */
    int64_t budget;                 /* counts left in the slice */
} m68k_poll;

typedef struct m68k_bus {
    void *ctx;
    uint8_t *const *read_map[2];
    uint8_t *const *write_map[2];
    int (*read)(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t *value);
    int (*write)(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t value);
    int (*iack)(void *ctx, unsigned level);
    void (*reset)(void *ctx);
    /* Optional: how many more whole iterations of a polling loop pass, from
     * the loop's start, before anything its device reads see can change or
     * the slice would end at one of them (the machine's deadline test at
     * each device access); the machine adds their device time. 0 when a
     * read is not stable. The CPU adds the instructions. */
    int64_t (*poll)(void *ctx, const m68k_poll *p);
    /* Optional: `n` executions of a DBcc that branches to itself and
     * decrements (its condition false), of `counts` each (cycles when
     * timed), none counted yet, are about to run: the machine charges their
     * guest time and returns how many of them run before the slice must end
     * (1 to n; it shortens the budget to end the slice after them). The
     * interpreter asks for each, the translator for a whole run of them at
     * once. */
    int64_t (*dbra)(void *ctx, int64_t n, unsigned counts);
    /* Optional, 68000 only: wait states. Called before every bus cycle the
     * 68000 runs (instruction fetches, operand and stack accesses, the
     * interrupt acknowledge excluded) with its 24-bit address, the function
     * code, `kind` 0 read, 1 write, 2 read-modify-write (TAS), and the clock
     * cycle the bus cycle starts at (the CPU's `cycles` count, timed or
     * not, plus what the instruction ran before it); returns the clock
     * cycles the bus cycle is stretched by (a machine's video and sound
     * DMA taking RAM cycles). Each cycle is counted in the instruction's
     * time, so guest time follows. */
    unsigned (*wait)(void *ctx, uint32_t address, unsigned fc, unsigned kind, uint64_t cycle);
} m68k_bus;

typedef enum m68k_status {
    M68K_OK,            /* ran the instructions asked */
    M68K_STOPPED,       /* STOP: waiting for an interrupt above the mask */
    M68K_HALTED,        /* double fault: the processor halted (MC68040 UM
                           section 8, "Access Fault Exception") */
    M68K_UNIMPLEMENTED  /* an opcode this build does not execute; PC is at it
                           (none at present) */
} m68k_status;

/* Access faults (MC68040 UM section 8, "Access Fault Exception" and the
 * format $7 access error stack frame). A read or instruction fetch that
 * the bus refuses aborts the instruction: the CPU restores the registers the
 * instruction changed (PC, and the address registers (An)+/-(An) stepped:
 * handlers commit every other result after their last read), records the
 * fault below and builds the access-error frame with the instruction's
 * address, so RTE re-runs it. A refused write does not abort: as on the
 * 68040, the instruction completes, the write (and any later write of the
 * same instruction, at most two in all) waits in a writeback slot and the
 * access error is taken at the instruction boundary with the next
 * instruction's PC; the handler performs the writebacks. Instructions that
 * write more than two operands (MOVEM, MOVE16, MOVEP) write nothing their
 * restart could not repeat, so a refused write restarts them like a read.
 * A translation the MMU refuses (invalid descriptor, write protection,
 * supervisor-only page, bus error during the table search) is an access
 * fault of the same kind, with the SSW's ATC bit set; a refused write then
 * waits in WB2 instead of WB1 (MC68040 UM 8.4.6.7). */
typedef struct m68k_fault {
    uint32_t address;   /* fault address (FA) */
    uint32_t ea;        /* effective address of the access (EA field) */
    uint16_t ssw;       /* special status word as the model stacks it */
    uint8_t size, fc, write, atc, misaligned, locked;
} m68k_fault;

typedef struct m68k_writeback {
    uint32_t address, data;
    uint16_t status;    /* WBxS: V, SIZE, TT, TM (MC68040 UM, format $7 frame) */
} m68k_writeback;

/* Memory management (MC68040 UM section 3; MC68030 UM section 9).
 *
 * The page tables the instructions use (rmap/wmap/pmap) are either the
 * bus's own (logical = physical: translation off, no write-protected
 * transparent block) or a soft TLB: six tables (program, read, write for
 * user and supervisor) filled on demand from the translation the MMU model
 * computes, and emptied whenever that translation may change (an ATC entry
 * replaced or flushed, a register written, the bus's maps replaced). A
 * table entry exists only while the ATC entry or transparent translation it
 * came from is valid, so the fast path never sees a translation the chip
 * would not use. */
typedef struct m68k_atc_entry {
    uint32_t tag;       /* logical page (68040: LA >> 12 or 13; 68030: LA >> page shift) */
    uint32_t pa;        /* physical page base */
    uint16_t st;        /* M68K_ATC_* */
    uint8_t fc;         /* 68040: FC2; 68030: the function code */
    uint8_t pad;
} m68k_atc_entry;

enum {
    M68K_ATC_R = 0x0001,    /* resident (68040); 68030: valid translation */
    M68K_ATC_W = 0x0004,    /* write protected */
    M68K_ATC_M = 0x0010,    /* modified */
    M68K_ATC_CM = 0x0060,   /* cache mode (68040); 68030 uses bit 6 for CI */
    M68K_ATC_S = 0x0080,    /* supervisor only */
    M68K_ATC_U = 0x0300,    /* U1 U0 */
    M68K_ATC_G = 0x0400,    /* global */
    M68K_ATC_B = 0x0800,    /* bus error during the table search */
    M68K_ATC_V = 0x8000     /* entry valid */
};

typedef struct m68k_mmu {
    uint8_t mode;               /* 0: the bus's maps; 1: the soft TLB; 2: the soft TLB, logical = physical (translator on) */
    uint8_t ctr[2];             /* 68040 replacement counters (data, instruction) */
    uint8_t next030;            /* 68030 replacement pointer */
    m68k_atc_entry atc[2][16][4];   /* 68040 ATCs: [0] data, [1] instruction; 16 sets of 4 */
    m68k_atc_entry atc030[22];      /* 68030 ATC: 22 entries, fully associative */
    uint8_t **tlb;              /* 8 x M68K_PAGE_COUNT (TLB_TABLE); NULL until first needed */
    uint32_t *list;             /* soft-TLB entries not owned by an ATC entry */
    uint32_t nlist;
    /* 68030 registers (MC68030 UM section 9.7); the MC68851 beside a 68020
     * keeps its TC, CRP, SRP and PSR here too (the formats agree: MC68851
     * UM 6.1.1, 6.1.3, 6.1.8) */
    uint32_t tc30, crp[2], srp30[2], tt30[2];
    uint16_t mmusr30;
} m68k_mmu;

/* The MC68851 beside a 68020 (mmu851.c), beyond the registers it shares
 * with the 68030 (m68k_mmu): fitted or not; the current task alias, the
 * root pointer table (8 CRP values, their valid bits, the replacement
 * pointer), PCSR's F bit, the DMA root pointer and the 64-entry ATC with
 * its replacement pointer (MC68851 UM 5.2, 5.3, 6.1.1, 6.1.2). */
typedef struct m68k_pmmu {
    uint8_t fitted, ta, rpt_valid, rpt_next, next, pcsr_f;
    uint32_t drp[2];
    uint32_t rpt[8][2];
    m68k_atc_entry atc[64];
} m68k_pmmu;

/* Floating-point units. */
typedef enum m68k_fpu_type {
    M68K_FPU_NONE,      /* no FPU: coprocessor instructions take line F */
    M68K_FPU_68881,
    M68K_FPU_68882,
    M68K_FPU_68040      /* the 68040's own FPU */
} m68k_fpu_type;

typedef struct m68k_fx80 { uint64_t m; uint16_t se; } m68k_fx80;   /* sign, 15-bit exponent, 64-bit mantissa */

typedef struct m68k_fpu {
    m68k_fx80 fp[8];
    uint32_t fpcr, fpsr, fpiar;
    uint8_t type, full;         /* m68k_fpu_type; full: the 68040 runs the whole 68881 set */
    uint8_t state;              /* FPS_NULL, FPS_IDLE, FPS_UNIMP, FPS_BUSY (68040), FPS_EXC (68881/2: exception pending) */
    uint8_t pending;            /* an exception waits for the next FP instruction (68881/2: EXC PEND) */
    uint8_t pend_vec;           /* its vector (0: from FPSR EXC and FPCR ENABLE) */
    uint8_t resume;             /* 68040: a restored busy frame finishes its instruction */
    uint8_t pend_concurrent;    /* 68882: the pending exception came from an arithmetic instruction (fpu.c, op_fmove_ctl) */
    uint8_t ete15, fpte15;      /* 68040: the restored frame's 16th exponent bits */
    /* State frame contents (MC68040 UM 9.8, Figure 9-10; MC68881 UM 6.4.2). */
    uint16_t cmdreg1b, cmdreg3b;
    uint8_t stag, dtag, e1, e3, t, wbte15, wbm66, wbm1, wbm0, sbit;
    m68k_fx80 etemp, fptemp, wbtemp;
    uint32_t fpiarcu;
    uint16_t cmdcond;           /* 68881/2: command/condition register image */
    m68k_fx80 excop;            /* 68881/2: exceptional operand */
    /* 68881/2: an FSAVE made while instructions == busy_at (the instruction
     * right after an arithmetic one) finds that instruction still executing
     * and saves a busy frame: its command word in cmdreg1b, its operands in
     * etemp (source) and fptemp (destination); fpu.c, save_frame. */
    uint64_t busy_at;
} m68k_fpu;

/* Cache maintenance seen by the CPU (CINV, CPUSH, CACR clear and invalidate
 * bits): `caches` 1 data, 2 instruction, 3 both; `scope` 1 line, 2 page,
 * 3 all; `address` physical. Livadix's memory is always coherent, so nothing
 * here has a data effect; a translator that caches decoded code hooks it. */
typedef void (*m68k_cache_hook)(void *ctx, unsigned caches, unsigned scope, uint32_t address);

typedef struct m68k_cpu m68k_cpu;
typedef void (*m68k_handler)(m68k_cpu *cpu, uint32_t op);
/* The registers and flags before an instruction the interpreter runs
 * (m68k_cpu.before): for a caller that abandons instructions (the Power
 * Macintoshes' 68k accelerator). Its PC is `ipc`. */
typedef struct m68k_before { uint32_t r[16], fn, fz, fv, fc, fx; } m68k_before;
typedef struct m68k_timing m68k_timing;

/* Called for every exception the CPU takes, before the frame is built (for
 * tests and traces). */
typedef void (*m68k_exception_hook)(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc);

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) /* jmp_buf's alignment pads the structure */
#endif
struct m68k_cpu {
    /* Hot state first. r[0..7] = D0-D7, r[8..15] = A0-A7; A7 is the active
     * stack pointer (USP, ISP or MSP per SR.S and SR.M). */
    uint32_t r[16];
    uint32_t pc;
    /* Flags, kept as the values they derive from:
     * N = fn bit 31, Z = (fz == 0), V = fv bit 31, C = fc bit 0, X = fx bit 0. */
    uint32_t fn, fz, fv, fc, fx;
    /* The rest of SR: T1, T0, S, M and the interrupt mask, in place. */
    uint16_t sr_hi;
    uint8_t s, m;               /* SR.S, SR.M */
    uint8_t *const *rmap;       /* page tables for the current privilege */
    uint8_t *const *wmap;
    uint8_t *const *pmap;       /* instruction fetches */
    const m68k_handler *table;  /* this model's dispatch table */
    uint32_t events;            /* nonzero: something to do between instructions */
    uint32_t ipc;               /* address of the instruction executing */
    /* Counts: an instruction costs ctab[opcode] plus what it adds while it
     * runs (cyc_extra: a branch taken, MOVEM's registers, an exception);
     * exceptions between instructions add theirs. Timed (m68k_set_timing),
     * the counts are the model's clock cycles (m68k_timing.c); otherwise one
     * per instruction. */
    int64_t budget;             /* counts left in this run */
    uint64_t cycles;            /* counts since creation */
    uint64_t instructions;      /* instructions completed since creation */
    const uint16_t *ctab;       /* each opcode's count */
    const m68k_timing *tm;      /* what instructions add */
    int32_t cyc_extra;          /* what the instruction executing added */
    uint16_t op;                /* its opcode word */
    uint8_t fc_data, fc_prog;   /* function codes for the current privilege */

    /* Undo record of the instruction executing (address-register steps). */
    uint8_t undo_n, flow, trace_pending, in_exception;
    /* Set by instructions with more writes than the writeback slots hold
     * (MOVEM, MOVE16, MOVEP): a refused write restarts them instead. */
    uint8_t restart_writes;
    /* 68020/030: a read bus fault whose handler cleared DF in the format $B
     * frame before its RTE (MC68030 UM 8.2.1: the processor does not rerun
     * the read and takes the data input buffer instead). The instruction is
     * restarted from the stacked PC; its read of `dib_address` with the
     * frame's size, made while `instructions` is `dib_count` (the restarted
     * instruction), takes `dib_value` without a bus cycle. */
    uint8_t dib_valid, dib_size;
    uint32_t dib_address, dib_value;
    uint64_t dib_count;
    uint8_t undo_reg[4];
    uint32_t undo_val[4];

    /* Supervisor state (PRM 1.3 "Supervisor Programming Model"; MC68040 UM
 * section 1, programming models). */
    uint32_t usp, isp, msp, vbr;
    uint32_t sfc, dfc, cacr, caar;
    uint32_t tc, itt0, itt1, dtt0, dtt1, mmusr, urp, srp;   /* 68040 MMU registers */

    /* Interrupts. */
    uint8_t ipl, nmi_seen;

    m68k_model model;
    m68k_status status;
    uint32_t unimplemented_op;  /* the opcode that stopped the run (M68K_UNIMPLEMENTED) */

    /* Access faults. */
    m68k_fault fault;
    m68k_writeback wb[3];
    uint8_t wb_n;
    uint8_t wb_atc;             /* the first pending write failed translation (WB2) rather than the bus (WB1) */
    uint8_t locked;             /* a read-modify-write (TAS, CAS, CAS2) is reading */
    int abort_kind;
    jmp_buf abort;
    jmp_buf *abortp;            /* where m68k_abort jumps: the run loop's, or a translated block's step */

    m68k_bus bus;
    m68k_exception_hook on_exception;
    m68k_before *before;        /* NULL, or where the interpreter keeps the registers of each instruction it starts */
    void *hook_ctx;
    m68k_cache_hook on_cache;
    void *cache_ctx;

    m68k_mmu mmu;
    m68k_fpu fpu;
    /* Set when an index extension word used a scale or the full format (a
     * 68020 addition); for tests that compare with 68000 behaviour. */
    uint8_t ea_ext020;

    /* The translator (jit_x64.c; NULL: the interpreter alone) and the words
     * its blocks and their helpers exchange: a block leaves after the
     * instruction running when jit_pending is set (a device raised the
     * interrupt level or moved the slice's end, a write hit translated
     * code); jit_a, jit_v and jit_arg carry an access to the slow path,
     * jit_val and jit_fault its result; jit_site is the chain site of the
     * last block exit. */
    void *jit;
    uint8_t jit_pending;
    uint32_t jit_a, jit_v, jit_arg, jit_val, jit_fault;
    int32_t jit_cyc, jit_need;  /* counts a block charged before the call; counts it may still charge */
    uintptr_t jit_site;
    /* What the slow path did, for the translator's polling loops: table
     * searches made (translations not served by the soft TLB), reads through
     * the bus's callbacks and the last one's physical address; and the
     * current iteration's record: its device reads, whether it did anything
     * else through the slow path, the counts at its start. */
    uint32_t mmu_searches, bus_reads, bus_pa;
    uint8_t poll_n, poll_bad, poll_ctr;
    uint32_t poll_searches;
    uint32_t poll_pa[8];
    uint8_t poll_size[8];
    uint32_t poll_at[8];
    uint32_t spin_snap[16];         /* the registers at the iteration's start */
    /* Idle loops through traps (jit_x64.c, m68k_jit_trap): while `wlog` is
     * set, every write is logged (the fast path's write table is then
     * `wlog_map`, which maps nothing, so every write reaches the slow
     * path); and what an iteration must not do, counted: device writes,
     * interrupts taken, delay loops charged by the machine. */
    uint8_t wlog;
    uint8_t *const *wlog_map;
    uint32_t bus_writes, irqs_taken, dbra_runs;
    m68k_pmmu pmmu;
    /* The two instruction words the 68030 had prefetched when a PMOVE to
     * TC changed the translation (m68k_prefetch_hold): fetched with the
     * old translation, they serve the next instruction's fetches of those
     * addresses, whatever the new one maps there. Valid while `instructions` < pf_until. */
    uint32_t pf_addr, pf_data;
    uint64_t pf_until;
    /* The 68000 (m68k_000.c): the instruction executing's prefetch: the
     * address of the next word to fetch and the last word fetched (IRC);
     * the clock cycles it has run; the last group 0 fault (address, the
     * first word of its frame); whether the instruction began traced. */
    struct {
        uint32_t pf, t, fa;
        uint16_t irc, ird, fstat;
        uint8_t traced, group0;
    } x0;
};
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/* Builds the dispatch tables once (idempotent; not thread-safe on the first
 * call). m68k_init calls it. */
void m68k_tables_init(void);

/* A powered-off CPU of `model` on `bus`; call m68k_reset before running.
 * The FPU defaults to the model's usual one: 68881 beside a 68020, 68882
 * beside a 68030, the 68040's own, none on the 68LC040. */
void m68k_init(m68k_cpu *cpu, m68k_model model, const m68k_bus *bus);
/* Releases what the CPU allocated (the soft TLB); the structure itself
 * belongs to the caller. */
void m68k_free(m68k_cpu *cpu);
/* The FPU beside the CPU. `full` (68040 only): execute the instructions and
 * data types the 68040 leaves to software (transcendentals, FMOVECR, packed
 * decimal, denormalized operands) directly instead of trapping to the
 * system's emulation package, as the real chip does. */
void m68k_set_fpu(m68k_cpu *cpu, m68k_fpu_type type, int full);
/* The reset exception (MC68040 UM section 8, "Reset Exception"; MC68000 UM 6.3.1): SR = $2700, VBR = 0, caches and
 * translation disabled, ISP and PC from vectors 0 and 1 (read through the
 * bus at address 0). */
void m68k_reset(m68k_cpu *cpu);
/* Replaces the page tables (the MMU step and machines that remap memory). */
void m68k_set_maps(m68k_cpu *cpu, uint8_t *const *read_user, uint8_t *const *write_user,
                   uint8_t *const *read_super, uint8_t *const *write_super);

/* Invalidates every ATC entry (68030, 68040 and the MC68851), as PFLUSHA
 * does; for harnesses that edit translation tables from outside. */
void m68k_pflusha(m68k_cpu *cpu);

/* The MC68851 PMMU in the 68020's coprocessor socket (`fitted` 1), or none
 * (the Macintosh II's AMU or nothing: coprocessor 0 instructions take line
 * F). 68020 only; translation stays off until the guest enables it. */
void m68k_set_pmmu(m68k_cpu *cpu, int fitted);

/* The x64 translator (jit_x64.c): guest code is
 * translated to host code by blocks and runs with the same guest-visible
 * results and instruction and cycle counts as the interpreter. m68k_jit_enable
 * returns 1 when translation is on (0: not available on this host, or
 * `on` is 0). Options: `callout` 1 (default) lets a block call the slow
 * path of an access that misses the page tables (device registers, a TLB
 * miss) instead of leaving the block before the instruction; `idle_skip`
 * 1 (default) runs a guest busy-wait loop (a loop that only reads memory
 * and branches, or DBRA to itself) to the end of the slice in one step, as
 * many iterations as the slice holds, exactly as the interpreter would;
 * `max_block` limits a block's instructions (0: the default). */
int m68k_jit_available(void);
/* The 68000 always runs on the interpreter (its bus-cycle model: address
 * errors, prefetch and wait states per bus cycle); m68k_jit_enable returns
 * 0 for it. */
int m68k_jit_enable(m68k_cpu *cpu, int on);
void m68k_jit_options(m68k_cpu *cpu, int callout, int idle_skip, unsigned max_block);
/* Code is translated once the dispatcher has found it `runs` times
 * (default 32): code that runs fewer times costs less interpreted than
 * translated. 0 translates at once (the differential tests). */
void m68k_jit_threshold(m68k_cpu *cpu, unsigned runs);
/* Forgets every translation, or those of the bytes at a physical address
 * (through the bus's read maps): for harnesses that write guest memory
 * behind the CPU's back (writes the CPU makes are seen by the translator). */
void m68k_jit_flush(m68k_cpu *cpu);
void m68k_jit_invalidate(m68k_cpu *cpu, uint32_t address, uint32_t size);
/* Counters of the translator (for reports and tests). */
typedef struct m68k_jit_stats {
    uint64_t blocks, translated_instructions, flushes, invalidations, exits, chains, fast_forwarded;
    uint64_t run_instructions;      /* instructions run inside translated blocks (fast-forwarded ones too) */
    uint64_t exit_kinds[5];         /* returns to the dispatcher: normal, interpret, check, fault, chain */
} m68k_jit_stats;
void m68k_jit_get_stats(const m68k_cpu *cpu, m68k_jit_stats *stats);
/* Called with every block translated: its guest address and instruction
 * count, its host code (for diagnosis: a disassembler can read it). */
typedef void (*m68k_jit_block_hook)(void *ctx, uint32_t pc, unsigned instructions, const uint8_t *code, size_t size);
void m68k_jit_set_block_hook(m68k_cpu *cpu, m68k_jit_block_hook hook, void *ctx);
/* Asked before a block is translated from host page `host`: nonzero leaves
 * that code to the interpreter (a page whose owner would rather not have
 * translations to keep track of). */
typedef int (*m68k_jit_refuse_hook)(void *ctx, const uint8_t *host);
void m68k_jit_set_refuse_hook(m68k_cpu *cpu, m68k_jit_refuse_hook hook, void *ctx);

/* Runs instructions while the run has counts left (`counts`; untimed one per
 * instruction, an instruction that takes an exception, the restarted
 * access fault included, counting as one; timed the cycles of each
 * instruction and exception: the last instruction may end past them);
 * returns the status. What the last instruction left pending (its trace, a
 * writeback access error, an odd PC, an interrupt now due) is processed
 * before returning, so m68k_run(cpu, 0) only settles those. `instructions`
 * in the CPU counts every instruction step since creation, `cycles` the
 * counts. */
m68k_status m68k_run(m68k_cpu *cpu, int64_t counts);
/* One instruction, for a caller that takes over what m68k_run does around
 * it (the Power Macintoshes' 68k accelerator): an abort (an access fault, an
 * illegal instruction) jumps to `abort`, which the caller has set with
 * setjmp, the instruction abandoned as it stands; the exception hook is
 * called for an exception as ever. Returns 0 when the instruction left
 * something for the boundary (a refused write, an odd PC, STOP), which the
 * caller must not let stand: it is not processed. */
int m68k_step(m68k_cpu *cpu, jmp_buf *abort);
/* After m68k_step's abort, its exception hook leaving, or its 0: drops what
 * the abandoned instruction left inside the CPU (a pending writeback, a
 * locked read-modify-write, a restart in progress). The caller restores
 * the registers. */
void m68k_abandon(m68k_cpu *cpu);
/* The soft TLB forgets what it holds of the page of `address`, so that its
 * next access looks at the bus's maps again (a page the bus's owner has
 * just put in them). */
void m68k_mmu_forget(m68k_cpu *cpu, uint32_t address);
/* Bytes [offset, offset + size) of host page `page` were written behind the
 * CPU's back: translations made from them are dropped. NULL: all of them. */
void m68k_jit_host_written(m68k_cpu *cpu, const uint8_t *page, uint32_t offset, unsigned size);

/* Instruction timing (m68k_timing.c). Timed, the
 * counts are the model's clock cycles from Motorola's tables (MC68040 UM
 * 10, MC68030 UM 11 instruction-cache case, MC68020 UM 10 cache case,
 * MC68881/MC68882 UM 8; the 68000's from its bus cycles, MC68000 UM 8, in
 * m68k_000.c); untimed (the default after m68k_init) one per
 * instruction. */
void m68k_set_timing(m68k_cpu *cpu, int timed);
int m68k_timed(const m68k_cpu *cpu);
/* The timed count of an opcode word without what it adds while running
 * (a branch not taken, MOVEM with no register, the brief index format). */
unsigned m68k_cycles_of(m68k_model model, uint16_t op);

/* The interrupt level presented on IPL2-IPL0 (0-7). Level 7 is taken on the
 * transition to 7 regardless of the mask (MC68040 UM section 8, "Interrupt
 * Exceptions"). */
void m68k_set_irq(m68k_cpu *cpu, unsigned level);

uint16_t m68k_sr(const m68k_cpu *cpu);
uint8_t m68k_ccr(const m68k_cpu *cpu);
/* Writes SR with the model's implemented bits; switches A7 when S or M
 * changes. */
void m68k_set_sr(m68k_cpu *cpu, uint16_t sr);
void m68k_set_ccr(m68k_cpu *cpu, uint8_t ccr);
/* The stack pointers whatever SR selects. */
uint32_t m68k_get_usp(const m68k_cpu *cpu);
uint32_t m68k_get_isp(const m68k_cpu *cpu);
uint32_t m68k_get_msp(const m68k_cpu *cpu);
void m68k_set_usp(m68k_cpu *cpu, uint32_t v);
void m68k_set_isp(m68k_cpu *cpu, uint32_t v);
void m68k_set_msp(m68k_cpu *cpu, uint32_t v);

/* MOVEC control register access by its PRM code ($000-$007, $800-$807);
 * returns 0 when the model has no such register. Reads see what MOVEC would
 * read (implemented bits only). */
int m68k_movec_read(m68k_cpu *cpu, unsigned code, uint32_t *value);
int m68k_movec_write(m68k_cpu *cpu, unsigned code, uint32_t value);

/* The decode step. */
typedef struct m68k_opinfo {
    const char *name;       /* mnemonic, with size suffix where it has one */
    m68k_handler handler;
    uint8_t valid;          /* 0: illegal, line A or line F on this model */
} m68k_opinfo;
m68k_opinfo m68k_decode(m68k_model model, uint16_t op);
/* Patterns that decode the same opcode with equal priority (must be 0). */
unsigned m68k_table_conflicts(m68k_model model);

const char *m68k_model_name(m68k_model model);

#endif /* MC_M68K_H */
