/* Livadix: the 68k accelerator of the Power Macintoshes.
 *
 * Under Mac OS a Power Macintosh spends most of its instructions in Apple's
 * 68LC040 emulator. This runs the 68k code on Livadix's own 68k core
 * (src/core/cpu) instead, from one of the emulator's dispatch entries to the
 * next 68k instruction that is not the accelerator's to run, and hands back
 * there with the emulator's registers as the emulator would have left them.
 *
 * Livadix's own design. The emulator's interface (the dispatch table at
 * $6808 0000, D0-D7 in r8-r15, A0-A6 in r16-r22, A7 in r1, the address
 * after the opcode in r24 and its word fetched ahead in r27, N and Z in CR0,
 * V and C in XER, X in r26) is what ROM 9FEB69B3's code does, read from the
 * ROM and sampled on the running machine; no document describes it. 68k
 * semantics are the 68k core's. */
#include "m68k_accel.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/cpu/m68k.h"
#include "../../core/ppc/ppc_jit.h"

#define TABLE UINT32_C(0x68080000)      /* the dispatch table: 65,536 entries of two instructions */
#define TABLE_SIZE UINT32_C(0x00080000)
#define NOP_ENTRY (TABLE + 0x4E71u * 8u) /* NOP's entry: eieio, then a branch to the fetch that ends a handler */
/* PowerPC steps charged for one 68k instruction: what Apple's emulator
 * takes, measured with check ppc-68k-speed's program (the guest time of its
 * rounds with the emulator against the 68k instructions the accelerator ran
 * for them): 10 for the 6100's emulator, 11 for the later ROMs' (the
 * dynamic recompiler's version), whose dispatch costs more. */
#define STEPS (x->steps_per)
#define STEPS_PLAIN 10u
#define STEPS_RECOMPILER 11u
#define RECOMPILER_WORD 0x7EE903A6u     /* mtctr r23, before the later emulators' fetch: the recompiler's hook */

typedef struct state { uint32_t r[16], pc; uint8_t ccr; } state;
#define HELD 4096u                      /* the shadow check's held-back bytes */
#define MORE 512u                       /* the instructions it runs on to meet a fused sequence's end */
#define MAPPED 16384u                   /* pages given to the 68k core at a time */
#define BUSY 16384u                     /* host pages whose PowerPC writes are counted */
#define WRITTEN 65536u                  /* host pages in the 68k core's write table, by host page */
#define BUSY_WRITES 65536u              /* writes after which a page's 68k code is left to the interpreter */

#if defined(_MSC_VER)
#pragma warning(disable : 4324) /* jmp_buf's alignment pads the structure */
#endif
struct mc_m68k_accel {
    mc_ppc *m;
    m68k_cpu cpu;
    /* The 68k core's page tables: the host page of each page of RAM the
     * PowerPC MMU has translated for it, a read and a write, as the
     * translator's TLB gave them (ppc_jit_data); every other access comes
     * through the callbacks. Emptied when the translator's TLB generation or
     * the PowerPC's mode moves. */
    uint8_t **rd, **wr;
    uint32_t *mapped;           /* the pages set, bit 0: in wr */
    unsigned nmapped;
    uint32_t generation, mode;
    jmp_buf leave;              /* from the exception hook or an abort: the instruction is not the accelerator's */
    m68k_before before;         /* the 68k registers before the instruction the interpreter is running */
    uint64_t first;             /* the core's instruction count at the take-over */
    uint32_t start_r[16];       /* and its registers */
    uint8_t skip[65536];        /* opcodes whose entries are never taken over */
    /* Pages with translated 68k code that PowerPC code keeps writing (data
     * beside the code): each write costs the PowerPC translator its slow
     * path, so past BUSY_WRITES the page's 68k code goes back to the
     * interpreter and its guard is lifted. The number is the best of those
     * tried on the 7.5.3 and 9.0.4 boots (16 to 100 million): lower leaves
     * hot code to the interpreter, higher pays more in slow writes than the
     * translation saves. */
    struct { const uint8_t *host; uint32_t writes; } busy[BUSY];
    /* The 68k page each host page is written through (wr), so that the entry
     * can be taken back when the PowerPC translator holds the host page's
     * writes; one per slot, the earlier one taken back when two meet. */
    struct { const uint8_t *host; uint32_t page; } written[WRITTEN];
    uint64_t start, steps;      /* the run's first clock count and the steps it charges */
    int known, starting, io, reset;
    uint32_t fetch;             /* the emulator's fetch (fetch_of) */
    uint32_t steps_per;         /* STEPS */
    uint32_t jump;              /* and its jump to the 68k address in r24 (jump_of) */
    int inside;                 /* the 68k core is running an instruction of a run */
    /* The shadow check: each instruction predicted on the 68k core with its
     * writes held back, then compared with what Apple's emulator made of it. */
    int shadow, predicted, refused;
    state prediction;
    uint32_t shadow_op, shadow_pc, shadow_exceptions;
    uint64_t shadow_at;         /* the PowerPC instruction count at the prediction */
    state before_state;
    struct { uint32_t address; uint8_t value; } held[HELD];
    unsigned nheld, reports;
    uint32_t *by_op;            /* differences by opcode */
    mc_m68k_accel_stats stats;
};

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static unsigned written_slot(const uint8_t *host) { return (unsigned)((uintptr_t)host >> 12) & (WRITTEN - 1u); }

/* Entry i of `mapped` leaves the 68k core's tables. */
static void unmap_one(mc_m68k_accel *x, unsigned i)
{
    const uint32_t page = x->mapped[i] >> 1;
    if (x->mapped[i] & 1u) {
        if (x->wr[page]) {
            const unsigned at = written_slot(x->wr[page]);
            if (x->written[at].host == x->wr[page] && x->written[at].page == page) x->written[at].host = NULL;
        }
        x->wr[page] = NULL;
    } else x->rd[page] = NULL;
}

static void unmap(mc_m68k_accel *x)
{
    for (unsigned i = 0; i < x->nmapped; ++i) unmap_one(x, i);
    x->nmapped = 0;
    m68k_set_maps(&x->cpu, (uint8_t *const *)x->rd, (uint8_t *const *)x->wr, (uint8_t *const *)x->rd, (uint8_t *const *)x->wr);
}

/* The 603's data TLB entries of the indexes in `dropped` changed
 * (ppc_jit_soft_dropped): the pages given through them go. */
static void unmap_indexes(mc_m68k_accel *x, uint32_t dropped)
{
    unsigned kept = 0;
    for (unsigned i = 0; i < x->nmapped; ++i) {
        const uint32_t page = x->mapped[i] >> 1;
        if (dropped >> (page & 31u) & 1u) {
            unmap_one(x, i);
            m68k_mmu_forget(&x->cpu, page << 12);
        } else x->mapped[kept++] = x->mapped[i];
    }
    x->nmapped = kept;
}

/* A device sees the guest time of the instruction that reaches it. */
static void device(mc_m68k_accel *x)
{
    x->io = 1;
    x->cpu.budget = 0;          /* the run ends after this instruction */
    x->m->cycles = x->start + (x->cpu.instructions - x->first + 1u) * STEPS * x->m->desc->cpi;
}

/* The write entry of slot `at` leaves the 68k core's tables. */
static void take_back(mc_m68k_accel *x, unsigned at)
{
    const uint32_t page = x->written[at].page;
    if (x->wr[page] == x->written[at].host) {
        x->wr[page] = NULL;
        m68k_mmu_forget(&x->cpu, page << 12);
    }
    x->written[at].host = NULL;
}

/* The host page of a page of RAM, for the 68k core's own accesses. */
static void map(mc_m68k_accel *x, uint32_t address, uint8_t *host, int write)
{
    const uint32_t page = address >> 12;
    uint8_t **table = write ? x->wr : x->rd;
    if (table[page]) return;
    if (x->nmapped == MAPPED) unmap(x);
    table[page] = host - (address & 4095u);
    if (write) {
        const unsigned at = written_slot(table[page]);
        if (x->written[at].host && (x->written[at].host != table[page] || x->written[at].page != page)) take_back(x, at);
        x->written[at].host = table[page];
        x->written[at].page = page;
    }
    x->mapped[x->nmapped++] = page << 1 | (write ? 1u : 0u);
    m68k_mmu_forget(&x->cpu, address);
}

/* A device the access reached reset the processor (a board's reset line):
 * the PowerPC state is the reset CPU's now, nothing of the 68k run goes
 * back into it, and the rest of the instruction is dropped. */
static void after_device(mc_m68k_accel *x)
{
    if (!ppc_jit_reset_seen(x->m->jit, 0)) return;
    x->reset = 1;
    longjmp(x->leave, 1);
}

static int acc_peek(mc_m68k_accel *x, uint32_t address, uint32_t *value);

/* Whether the instruction the core is running, which is about to read a
 * device, is sure to complete after the read. An instruction abandoned
 * after it (a write the PowerPC MMU refuses: a page fault, or on the 603
 * any page its TLB does not hold) is run again by Apple's emulator, which
 * reads the device again: a word of a disk's data register lost. So the
 * read is made only for an instruction that accesses no memory after it
 * (MC68040 UM, PRM: the forms below), or for a MOVE whose destination's
 * pages were asked for first; any other is the emulator's from its start. */
static int reads_device(mc_m68k_accel *x)
{
    const uint32_t *r = x->before.r, pc = x->cpu.ipc;
    uint32_t op, w, address;
    uint8_t *host;
    uint32_t pa;
    unsigned top, mode, size, words;
    if (acc_peek(x, pc, &op)) return 0;
    top = op >> 12;
    mode = (op >> 6) & 7u;
    if (top >= 1u && top <= 3u) {       /* MOVE, MOVEA */
        const unsigned sm = (op >> 3) & 7u, sr = op & 7u, dr = (op >> 9) & 7u;
        if (mode <= 1u) return 1;
        size = top == 1u ? 1u : top == 3u ? 2u : 4u;
        if (sm >= 2u && sm <= 4u) words = 0;
        else if (sm == 5u || (sm == 7u && sr == 0u)) words = 1;
        else if (sm == 7u && sr == 1u) words = 2;
        else return 0;
        if ((sm == 3u || sm == 4u) && sr == dr && mode != 7u) return 0;
        switch (mode) {
        case 2: case 3: address = r[8u + dr]; break;
        case 4: address = r[8u + dr] - (size == 1u && dr == 7u ? 2u : size); break;
        case 5:
            if (acc_peek(x, pc + 2u + 2u * words, &w)) return 0;
            address = r[8u + dr] + (uint32_t)(int32_t)(int16_t)w;
            break;
        case 7:
            if (dr > 1u || acc_peek(x, pc + 2u + 2u * words, &w)) return 0;
            address = (uint32_t)(int32_t)(int16_t)w;
            if (dr == 1u) {
                uint32_t low;
                if (acc_peek(x, pc + 4u + 2u * words, &low)) return 0;
                address = w << 16 | low;
            }
            break;
        default: return 0;
        }
        return ppc_jit_data(x->m->jit, address, 1, &host, &pa) && ppc_jit_data(x->m->jit, address + size - 1u, 1, &host, &pa);
    }
    if ((op & 0xFF00u) == 0x4A00u && mode != 3u) return 1;          /* TST */
    if ((op & 0xFF00u) == 0x0C00u && mode != 3u) return 1;          /* CMPI */
    if ((op & 0xFFC0u) == 0x0800u || (op & 0xF1C0u) == 0x0100u) return 1;    /* BTST, MOVEP to a register */
    if ((op & 0xFFC0u) == 0x44C0u) return 1;                        /* MOVE to CCR */
    if (top == 0xBu) return mode <= 3u || mode == 7u;               /* CMP, CMPA */
    if (top == 0x9u || top == 0xDu || top == 0xCu) return mode <= 3u || mode == 7u;  /* SUB, ADD, AND to a register, SUBA, ADDA, MULU, MULS */
    if (top == 0x8u) return mode <= 2u;                             /* OR to a register (a division may trap) */
    return 0;
}

static int acc_read(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t *value)
{
    mc_m68k_accel *x = ctx;
    ppc_cpu *c = &x->m->cpu;
    uint8_t *host;
    uint32_t pa, v = 0;
    (void)fc;
    if (x->starting) { *value = 0; return 0; }
    if (x->shadow && size == 1) {
        for (unsigned i = x->nheld; i-- > 0;)
            if (x->held[i].address == address) { *value = x->held[i].value; return 0; }
    }
    if ((address & 4095u) + size > 4096u || (x->shadow && size > 1)) {
        for (unsigned i = 0; i < size; ++i) {
            uint32_t b;
            if (acc_read(ctx, address + i, 1, fc, &b)) return 1;
            v = v << 8 | b;
        }
        *value = v;
        return 0;
    }
    if (!ppc_jit_data(x->m->jit, address, 0, &host, &pa)) {
        /* refused (a page fault; on the 603 a page its TLB does not hold): the
         * instruction is the emulator's, left without the 68k core's own
         * bus error path */
        if (x->inside) longjmp(x->leave, 1);
        return 1;
    }
    if (host) {
        v = size == 1 ? host[0] : size == 2 ? be16(host) : be32(host);
        if (!x->shadow) map(x, address, host, 0);
    } else if (x->shadow) { x->refused = 1; return 1; }  /* a device is read once, by the emulator */
    else {
        if (x->inside && !reads_device(x)) longjmp(x->leave, 1);
        device(x);
        if (!c->bus.read(c->bus.opaque, pa, size, &v)) return 1;
        after_device(x);
    }
    *value = v;
    return 0;
}

static int acc_write(void *ctx, uint32_t address, unsigned size, unsigned fc, uint32_t value)
{
    mc_m68k_accel *x = ctx;
    ppc_cpu *c = &x->m->cpu;
    uint8_t *host;
    uint32_t pa;
    (void)fc;
    if (x->starting) return 0;
    if (x->shadow) {
        for (unsigned i = 0; i < size; ++i) {
            if (x->nheld == HELD) { x->refused = 1; return 1; }
            x->held[x->nheld].address = address + i;
            x->held[x->nheld++].value = (uint8_t)(value >> (8u * (size - 1u - i)));
        }
        return 0;
    }
    if ((address & 4095u) + size > 4096u) {
        for (unsigned i = 0; i < size; ++i)
            if (acc_write(ctx, address + i, 1, fc, (value >> (8u * (size - 1u - i))) & 255u)) return 1;
        return 0;
    }
    if (!ppc_jit_data(x->m->jit, address, 1, &host, &pa)) longjmp(x->leave, 1);
    if (host) {
        map(x, address, host, 1);
        if (size == 1) host[0] = (uint8_t)value;
        else if (size == 2) { host[0] = (uint8_t)(value >> 8); host[1] = (uint8_t)value; }
        else { host[0] = (uint8_t)(value >> 24); host[1] = (uint8_t)(value >> 16); host[2] = (uint8_t)(value >> 8); host[3] = (uint8_t)value; }
        /* translated 68k code there (the page guarded) is the 68k translator's to drop */
        m68k_jit_host_written(&x->cpu, host - (address & 4095u), address & 4095u, size);
    } else if (x->m->bus.write_map[pa >> 12]) {
        /* RAM that holds translated code or a page table: through the observed
         * bus, which drops the code or the translations; host pages given
         * out before may then no longer hold */
        if (!c->bus.write(c->bus.opaque, pa, size, value)) return 1;
        if (ppc_jit_tlb_generation(x->m->jit) != x->generation) {
            unmap(x);
            x->generation = ppc_jit_tlb_generation(x->m->jit);
            x->io = 1;
        }
        {
            const uint32_t dropped = ppc_jit_soft_dropped(x->m->jit);
            if (dropped) { unmap_indexes(x, dropped); x->io = 1; }
        }
    } else {
        device(x);
        if (!c->bus.write(c->bus.opaque, pa, size, value)) return 1;
        after_device(x);
    }
    return 0;
}

static int acc_iack(void *ctx, unsigned level) { (void)ctx; (void)level; return -1; }
static void acc_reset(void *ctx) { (void)ctx; }

/* Any exception: the instruction is Apple's emulator's. */
static void acc_exception(void *ctx, m68k_cpu *cpu, unsigned vector, uint32_t pc)
{
    mc_m68k_accel *x = ctx;
    (void)cpu; (void)vector; (void)pc;
    if (x->starting) return;
    longjmp(x->leave, 1);
}

static unsigned busy_slot(const uint8_t *host) { return (unsigned)((uintptr_t)host >> 12) & (BUSY - 1u); }

/* Asked by the 68k translator before it translates from a host page. */
static int acc_refuse(void *ctx, const uint8_t *host)
{
    const mc_m68k_accel *x = ctx;
    const unsigned at = busy_slot(host);
    return x->busy[at].host == host && x->busy[at].writes >= BUSY_WRITES;
}

/* The 68k translator made a block from the page of `pc`: PowerPC writes to
 * that page must be heard of. */
static void acc_block(void *ctx, uint32_t pc, unsigned instructions, const uint8_t *code, size_t size)
{
    mc_m68k_accel *x = ctx;
    (void)instructions; (void)code; (void)size;
    ppc_jit_guard(x->m->jit, x->rd[pc >> 12]);
}

/* The PowerPC translator holds a host page's writes from now on. */
static void acc_held(void *ctx, const uint8_t *host)
{
    mc_m68k_accel *x = ctx;
    const unsigned at = written_slot(host);
    if (x->written[at].host == host) take_back(x, at);
}

/* A PowerPC write (the emulator's, native code's, DMA) reached a page with translated 68k code. */
static void acc_guard(void *ctx, uint8_t *host, uint32_t offset, uint32_t size)
{
    mc_m68k_accel *x = ctx;
    m68k_jit_host_written(&x->cpu, host, offset, size);
    if (host) {
        const unsigned at = busy_slot(host);
        if (x->busy[at].host != host) { x->busy[at].host = host; x->busy[at].writes = 0; }
        if (++x->busy[at].writes == BUSY_WRITES) {
            m68k_jit_host_written(&x->cpu, host, 0, 4096);
            ppc_jit_unguard(x->m->jit, host);
        }
    } else memset(x->busy, 0, sizeof x->busy);
}

/* The emulator's own, known from the opcode alone: line A and line F
 * (traps, Mixed Mode, the FPU's absence), the instructions that are
 * privileged or trap in the 68k core's user state (MC68040 UM, PRM), and
 * CHK, whose undefined flags the emulator sets its own way. The rest is
 * tried, and anything that raises an exception after all is abandoned. */
static int own(uint32_t op)
{
    if ((op >> 12) == 0xAu || (op >> 12) == 0xFu) return 0;
    if ((op & 0xF140u) == 0x4100u) return 0;                        /* CHK */
    if ((op & 0xFFC0u) == 0x40C0u || (op & 0xFFC0u) == 0x46C0u) return 0;    /* MOVE from SR, MOVE to SR */
    if (op == 0x007Cu || op == 0x027Cu || op == 0x0A7Cu) return 0;  /* ORI, ANDI, EORI to SR */
    if ((op & 0xFF00u) == 0x0E00u) return 0;                        /* MOVES */
    if ((op & 0xFFF0u) == 0x4E40u || (op & 0xFFF0u) == 0x4E60u) return 0;    /* TRAP, MOVE USP */
    if (op >= 0x4E70u && op <= 0x4E7Bu && op != 0x4E71u && op != 0x4E75u && op != 0x4E77u) return 0;
                                                                    /* RESET, STOP, RTE, RTD, TRAPV, MOVEC */
    if (op == 0x4AFCu || (op & 0xFFF8u) == 0x4848u) return 0;       /* ILLEGAL, BKPT */
    return 1;
}

/* CR2's LT: the emulator has an event to take (an interrupt, a trace) once
 * the instruction at this entry is done, so that instruction is its own. */
static int event(const ppc_cpu *c) { return (c->cr & 0x00800000u) != 0; }

/* The fetch NOP's entry branches to (a relative `b`), when the entry is
 * eieio and that branch: the emulator's own fetch. 0 otherwise. */
static uint32_t fetch_of(uint32_t first, uint32_t second)
{
    if (first != 0x7C0006ACu || (second & 0xFC000003u) != 0x48000000u) return 0;
    return NOP_ENTRY + 4u + (uint32_t)((int32_t)(second << 6) >> 6);
}

/* The routine JMP (A0)'s entry branches to once r24 is A0 (`addi r24,r16,0`,
 * then a relative `b`): it loads the opcode at r24 and the word after it
 * and goes to the opcode's entry, looking for a pending event only when
 * both are loaded. 0 when the entry is not that. */
#define JMP_ENTRY (TABLE + 0x4ED0u * 8u)
#define JUMP_WORD 0xAB780000u           /* lha r27,0(r24): the jump's first instruction */
static uint32_t jump_of(uint32_t first, uint32_t second)
{
    if (first != 0x3B100000u || (second & 0xFC000003u) != 0x48000000u) return 0;
    return JMP_ENTRY + 4u + (uint32_t)((int32_t)(second << 6) >> 6);
}

static const uint32_t fetch_words[4] = {0x537D1B78u, 0x7FA803A6u, 0xAF780002u, 0x4CA80020u};

int mc_m68k_accel_knows(const uint8_t *rom, size_t size)
{
    const size_t base = 0x300000u;      /* the ROM offset of logical $6800 0000 (6100 and PCI ROMs) */
    uint32_t fetch;
    if (!rom || size < base + (TABLE - UINT32_C(0x68000000)) + TABLE_SIZE) return 0;
    fetch = fetch_of(be32(rom + base + (NOP_ENTRY - UINT32_C(0x68000000))), be32(rom + base + (NOP_ENTRY - UINT32_C(0x68000000)) + 4u));
    if (!fetch || fetch - UINT32_C(0x68000000) + 16u > TABLE - UINT32_C(0x68000000)) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (be32(rom + base + (fetch - UINT32_C(0x68000000)) + 4u * i) != fetch_words[i]) return 0;
    fetch = jump_of(be32(rom + base + (JMP_ENTRY - UINT32_C(0x68000000))), be32(rom + base + (JMP_ENTRY - UINT32_C(0x68000000)) + 4u));
    if (!fetch || fetch - UINT32_C(0x68000000) + 4u > TABLE - UINT32_C(0x68000000)) return 0;
    return be32(rom + base + (fetch - UINT32_C(0x68000000))) == JUMP_WORD;
}

mc_m68k_accel *mc_m68k_accel_create(mc_ppc *m)
{
    mc_m68k_accel *x;
    m68k_bus bus;
    if (!m || !m->jit) return NULL;
    x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->rd = calloc(M68K_PAGE_COUNT, sizeof *x->rd);
    x->wr = calloc(M68K_PAGE_COUNT, sizeof *x->wr);
    x->mapped = calloc(MAPPED, sizeof *x->mapped);
    if (!x->rd || !x->wr || !x->mapped) { free(x->rd); free(x->wr); free(x->mapped); free(x); return NULL; }
    x->m = m;
    x->steps_per = STEPS_PLAIN;
    memset(&bus, 0, sizeof bus);
    bus.ctx = x;
    bus.read_map[0] = bus.read_map[1] = (uint8_t *const *)x->rd;
    bus.write_map[0] = bus.write_map[1] = (uint8_t *const *)x->wr;
    bus.read = acc_read; bus.write = acc_write; bus.iack = acc_iack; bus.reset = acc_reset;
    x->starting = 1;
    m68k_init(&x->cpu, M68K_68LC040, &bus);
    m68k_reset(&x->cpu);
    m68k_set_sr(&x->cpu, 0);            /* user state: privileged instructions raise their exception */
    x->cpu.on_exception = acc_exception;
    x->cpu.hook_ctx = x;
    x->cpu.before = &x->before;
    x->starting = 0;
    /* The 68k translator, with every slow access left to the interpreter (so
     * anything abandoned is abandoned there, the registers kept) and no idle
     * skipping (guest time is the machine's). */
    if (m68k_jit_enable(&x->cpu, 1)) {
        m68k_jit_options(&x->cpu, 0, 0, 0);
        m68k_jit_set_block_hook(&x->cpu, acc_block, x);
        m68k_jit_set_refuse_hook(&x->cpu, acc_refuse, x);
        ppc_jit_set_guard(m->jit, acc_guard, acc_held, x);
    }
    /* The PowerPC translator runs through the entries that are the emulator's by their opcode. */
    for (uint32_t op = 0; op < 65536u; ++op) x->skip[op] = (uint8_t)!own(op);
    ppc_jit_trap(m->jit, TABLE, TABLE_SIZE, x->skip);
    return x;
}

void mc_m68k_accel_destroy(mc_m68k_accel *x)
{
    if (!x) return;
    ppc_jit_set_guard(x->m->jit, NULL, NULL, NULL);
    if (x->by_op) {
        for (unsigned op = 0; op < 65536; ++op)
            if (x->by_op[op]) fprintf(stderr, "68k shadow: opcode %04X differed %u times\n", op, (unsigned)x->by_op[op]);
        free(x->by_op);
    }
    m68k_free(&x->cpu);
    free(x->rd);
    free(x->wr);
    free(x->mapped);
    free(x);
}

void mc_m68k_accel_get_stats(const mc_m68k_accel *x, mc_m68k_accel_stats *s)
{
    m68k_jit_stats js;
    if (!x) { memset(s, 0, sizeof *s); return; }
    *s = x->stats;
    m68k_jit_get_stats(&x->cpu, &js);
    s->translated = js.run_instructions;
    s->blocks = js.blocks;
}

int mc_m68k_accel_entry(const mc_m68k_accel *x, uint32_t pc)
{
    return x && pc - TABLE < TABLE_SIZE && !(pc & 7u);
}

/* Whether the entry the PowerPC PC is at starts the instruction in memory.
 * Some entries finish their instruction through another's (MOVE CCR,-(A7)
 * steps A7 and goes on at MOVE CCR,(A7)'s): there the entry is not the
 * opcode at r24 - 2, and what is left of the instruction is the emulator's. */
/* A word of 68k code for the hand-back; nonzero when it cannot be read.
 * Never a device, and never abandoning anything. */
static int acc_peek(mc_m68k_accel *x, uint32_t address, uint32_t *value)
{
    uint8_t *host;
    uint32_t pa, v = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (!ppc_jit_data(x->m->jit, address + i, 0, &host, &pa) || !host) return 1;
        v = v << 8 | *host;
    }
    *value = v;
    return 0;
}

static int starts(mc_m68k_accel *x, uint32_t op, uint32_t pc)
{
    const uint8_t *page = x->rd[pc >> 12];
    uint32_t w;
    if (page && !x->shadow && (pc & 4095u) != 4095u) return be16(page + (pc & 4095u)) == op;
    return !acc_read(x, pc, 2, 0, &w) && w == op;
}

/* The emulator this was read from: its fetch, and NOP's entry. */
static int known(mc_m68k_accel *x)
{
    uint32_t w0, w1, w;
    if (x->known) return x->known > 0;
    x->known = -1;
    if (acc_read(x, NOP_ENTRY, 4, 0, &w0) || acc_read(x, NOP_ENTRY + 4u, 4, 0, &w1) || !(x->fetch = fetch_of(w0, w1))) return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (acc_read(x, x->fetch + 4u * i, 4, 0, &w) || w != fetch_words[i]) return 0;
    if (acc_read(x, JMP_ENTRY, 4, 0, &w0) || acc_read(x, JMP_ENTRY + 4u, 4, 0, &w1) || !(x->jump = jump_of(w0, w1))) return 0;
    if (acc_read(x, x->jump, 4, 0, &w) || w != JUMP_WORD) return 0;
    x->steps_per = !acc_read(x, x->fetch - 4u, 4, 0, &w) && w == RECOMPILER_WORD ? STEPS_RECOMPILER : STEPS_PLAIN;
    x->known = 1;
    return 1;
}

/* The translator's host pages hold while its TLB generation and the PowerPC's mode do. */
static void settle(mc_m68k_accel *x)
{
    const ppc_cpu *c = &x->m->cpu;
    const uint32_t generation = ppc_jit_tlb_generation(x->m->jit), mode = c->msr & (PPC_MSR_DR | PPC_MSR_PR);
    uint32_t dropped;
    ppc_jit_sync(x->m->jit);
    dropped = ppc_jit_soft_dropped(x->m->jit);
    if (generation != x->generation || mode != x->mode || ppc_jit_tlb_generation(x->m->jit) != generation) {
        unmap(x);
        x->generation = ppc_jit_tlb_generation(x->m->jit);
        x->mode = mode;
        x->known = 0;           /* the emulator is read again: a System may map another at the same place */
    } else if (dropped) unmap_indexes(x, dropped);
}

static uint8_t ccr_of(const ppc_cpu *c)
{
    return (uint8_t)((c->cr & 0x80000000u ? 8u : 0u) | (c->cr & 0x20000000u ? 4u : 0u) |
                     (c->xer & 0x40000000u ? 2u : 0u) | (c->xer & 0x20000000u ? 1u : 0u) |
                     (c->gpr[26] & 0x20000000u ? 16u : 0u));
}

static void state_of(const ppc_cpu *c, state *s)
{
    for (unsigned i = 0; i < 8; ++i) s->r[i] = c->gpr[8 + i];
    for (unsigned i = 0; i < 7; ++i) s->r[8 + i] = c->gpr[16 + i];
    s->r[15] = c->gpr[1];
    s->pc = c->gpr[24] - 2u;
    s->ccr = ccr_of(c);
}

/* More PowerPC instructions than this between two entries: the emulator did
 * more than the instruction's handler (the dynamic recompiler ran a block of
 * translated 68k code from a branch, or an interrupt's 68k handler ran), and
 * the comparison is void. Its handlers take at most a few hundred (MOVEM of
 * every register, a long division). */
#define SHADOW_STEPS 1000u

/* The check compares at every dispatch entry, so the PowerPC translator stops at them all. */
void mc_m68k_accel_shadow(mc_m68k_accel *x, int on)
{
    if (!x) return;
    x->shadow = on != 0;
    ppc_jit_trap(x->m->jit, TABLE, TABLE_SIZE, on ? NULL : x->skip);
}

/* At a dispatch entry, with Apple's emulator doing all the work: what it
 * made of the instruction predicted at the entry before, then the
 * prediction of this one. An exception taken in between (counted by the CPU)
 * voids the comparison. */
static void shadow(mc_m68k_accel *x)
{
    ppc_cpu *c = &x->m->cpu;
    m68k_cpu *k = &x->cpu;
    const uint32_t op = (c->pc - TABLE) >> 3;
    state now;
    m68k_abandon(k);
    state_of(c, &now);
    settle(x);
    /* An entry an instruction passes through on its way: not a boundary. */
    if (!known(x) || !starts(x, op, now.pc)) return;
    if (x->predicted && c->exceptions == x->shadow_exceptions && c->instructions - x->shadow_at <= SHADOW_STEPS &&
        (memcmp(now.r, x->before_state.r, sizeof now.r) || now.pc != x->before_state.pc || now.ccr != x->before_state.ccr)) {
        state *p = &x->prediction;
        int differs;
        unsigned bad;
        /* The emulator runs some sequences as one (a push and RTS, MOVEM,
         * UNLK and RTS, a loop of block moves): the core is given up to MORE
         * more instructions to arrive where the emulator did. */
        for (unsigned more = 0;; ++more) {
            uint32_t next;
            differs = memcmp(p->r, now.r, sizeof now.r) || p->pc != now.pc || p->ccr != now.ccr;
            bad = HELD;
            for (unsigned i = 0; !differs && i < x->nheld && bad == HELD; ++i) {
                uint8_t *host;
                uint32_t pa;
                if (p->r[15] - x->held[i].address - 1u < 256u) continue;      /* below the stack pointer: a push the emulator folded away */
                if (ppc_jit_data(x->m->jit, x->held[i].address, 0, &host, &pa)) {
                    uint8_t *writable;
                    /* a byte of ROM keeps its value whatever is written */
                    if (host && *host != x->held[i].value && ppc_jit_data(x->m->jit, x->held[i].address, 1, &writable, &pa) && writable) bad = i;
                }
            }
            if ((!differs && bad == HELD) || more == MORE) break;
            x->refused = 0;
            if (setjmp(x->leave) || acc_read(x, k->pc, 2, 0, &next) || !own(next) || m68k_run(k, 1) != M68K_OK || x->refused) break;
            memcpy(p->r, k->r, sizeof k->r);
            p->pc = k->pc;
            p->ccr = m68k_ccr(k);
        }
        ++x->stats.checked;
        if (differs || bad != HELD) {
            ++x->stats.differed;
            if (!x->by_op) x->by_op = calloc(65536, sizeof *x->by_op);
            if (x->by_op) ++x->by_op[x->shadow_op];
            if (x->by_op && x->by_op[x->shadow_op] <= 2u && x->reports < 200) {
                ++x->reports;
                fprintf(stderr, "68k shadow: %04X at %08X:", (unsigned)x->shadow_op, (unsigned)x->shadow_pc);
                for (unsigned r = 0; r < 16; ++r)
                    if (p->r[r] != now.r[r])
                        fprintf(stderr, " %c%u %08X/%08X", r < 8 ? 'D' : 'A', r & 7u, (unsigned)p->r[r], (unsigned)now.r[r]);
                if (p->pc != now.pc) fprintf(stderr, " PC %08X/%08X", (unsigned)p->pc, (unsigned)now.pc);
                if (p->ccr != now.ccr) fprintf(stderr, " CCR %02X/%02X", p->ccr, now.ccr);
                if (bad != HELD) fprintf(stderr, " byte at %08X %02X", (unsigned)x->held[bad].address, x->held[bad].value);
                fprintf(stderr, " (the core's/the emulator's) [%llu PPC, srr0 %08X, pc %08X lr %08X r29 %08X; before:", (unsigned long long)(c->instructions - x->shadow_at), (unsigned)c->srr0, (unsigned)c->pc, (unsigned)c->lr, (unsigned)c->gpr[29]);
                for (unsigned r = 0; r < 16; ++r)
                    if (x->before_state.r[r] != now.r[r])
                        fprintf(stderr, " %c%u %08X", r < 8 ? 'D' : 'A', r & 7u, (unsigned)x->before_state.r[r]);
                fprintf(stderr, "]\n");
            }
        }
    }
    x->predicted = 0;
    if (own(op) && !event(c) && (c->msr & PPC_MSR_DR)) {
        memcpy(k->r, now.r, sizeof now.r);
        k->pc = now.pc;
        m68k_set_ccr(k, now.ccr);
        x->nheld = 0;
        x->refused = 0;
        if (!setjmp(x->leave) && m68k_run(k, 1) == M68K_OK && !x->refused) {
            memcpy(x->prediction.r, k->r, sizeof k->r);
            x->prediction.pc = k->pc;
            x->prediction.ccr = m68k_ccr(k);
            x->predicted = 1;
            x->shadow_op = op;
            x->shadow_pc = now.pc;
            x->shadow_exceptions = c->exceptions;
            x->shadow_at = c->instructions;
            x->before_state = now;
        }
    }
}

uint64_t mc_m68k_accel_run(mc_m68k_accel *x, uint64_t budget)
{
    mc_ppc *m = x->m;
    ppc_cpu *c = &m->cpu;
    m68k_cpu *k = &x->cpu;
    uint64_t made;
    volatile int abandoned = 0;
    volatile uint32_t start_pc = 0;
    const uint32_t first = (c->pc - TABLE) >> 3;
    if (x->shadow) { shadow(x); return 0; }
    if (budget < STEPS || !own(first) || event(c) || !(c->msr & PPC_MSR_DR) ||
        ((c->msr & PPC_MSR_EE) && (c->external_irq || c->decrementer_pending))) return 0;
    x->io = 0;
    x->reset = 0;
    x->start = m->cycles;
    ppc_jit_reset_seen(m->jit, 1);
    settle(x);
    if (!known(x) || !starts(x, first, c->gpr[24] - 2u)) return 0;
    for (unsigned i = 0; i < 8; ++i) k->r[i] = c->gpr[8 + i];
    for (unsigned i = 0; i < 7; ++i) k->r[8 + i] = c->gpr[16 + i];
    k->r[15] = c->gpr[1];
    k->pc = c->gpr[24] - 2u;
    m68k_set_ccr(k, ccr_of(c));
    x->first = k->instructions;
    start_pc = k->pc;
    memcpy(x->start_r, k->r, sizeof x->start_r);
    for (;;) {
        const uint64_t done = (k->instructions - x->first) * STEPS;
        if (done + STEPS > budget) break;
        x->io = 0;
        if (!setjmp(x->leave)) {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
            ((unsigned __int64 *)x->leave)[0] = 0;  /* no frame: longjmp does not unwind (nothing between is to be) */
#endif
            x->inside = 1;
            m68k_run(k, (int64_t)((budget - done) / STEPS));
            x->inside = 0;
        } else {
            x->inside = 0;
            /* An exception, or a write the PowerPC MMU refuses, in the instruction
             * the interpreter was running: it is the emulator's, from its start. */
            memcpy(k->r, x->before.r, sizeof k->r);
            k->fn = x->before.fn; k->fz = x->before.fz; k->fv = x->before.fv; k->fc = x->before.fc; k->fx = x->before.fx;
            k->pc = k->ipc;
            m68k_abandon(k);
            abandoned = 1;
            break;
        }
        if (!x->io) break;
        /* The last instruction reached a device: what the machine does after
         * such a step, and on with what the slice still holds. */
        budget = (k->instructions - x->first) * STEPS + mc_ppc_accel_resume(m, (k->instructions - x->first) * STEPS);
        /* the access may have remapped the bus or changed the translation state */
        settle(x);
    }
    made = k->instructions - x->first;
    if (x->reset) {
        /* The instruction that reset the machine counts; the CPU is not touched. */
        if (!abandoned) m68k_abandon(k);
        return (made + (abandoned ? 1u : 0u)) * STEPS;
    }
    /* The core may have counted the abandoned instruction (an illegal one is
     * counted before its exception): nothing ran if it was the first. */
    if (abandoned) {
        const int moved = k->pc != start_pc || memcmp(k->r, x->start_r, sizeof k->r) != 0;
        made = !moved ? 0u : made > 1u ? made - 1u : 1u;
    }
    if (!made) { ++x->stats.declined; return 0; }
    x->steps = made * STEPS;
    for (unsigned i = 0; i < 8; ++i) c->gpr[8 + i] = k->r[i];
    for (unsigned i = 0; i < 7; ++i) c->gpr[16 + i] = k->r[8 + i];
    c->gpr[1] = k->r[15];
    {
        /* Hand back as the emulator's fetch leaves things: at the next
         * instruction's entry with the word after its opcode fetched; where
         * that word cannot be read, at the fetch itself, which then takes the
         * fault as its own; where the opcode cannot be read either, at the
         * emulator's jump to r24, which loads both words (taking the faults)
         * before it looks for a pending event, so that an event's PC is
         * never taken with one of them still to load. */
        uint32_t w0 = 0, w1 = 0;
        const int have0 = !acc_peek(x, k->pc, &w0), have1 = have0 && !acc_peek(x, k->pc + 2u, &w1);
        c->gpr[29] = TABLE + (have0 ? w0 * 8u : 0u);
        if (have1) {
            c->gpr[24] = k->pc + 2u;
            c->gpr[27] = (uint32_t)(int32_t)(int16_t)w1;
            c->lr = c->ctr = c->pc = c->gpr[29];
        } else if (have0) {
            c->gpr[24] = k->pc;
            c->gpr[27] = (uint32_t)(int32_t)(int16_t)w0;
            c->pc = x->fetch;
        } else {
            c->gpr[24] = k->pc;
            c->pc = x->jump;
        }
    }
    {
        const unsigned ccr = m68k_ccr(k), n = ccr >> 3 & 1u, z = ccr >> 2 & 1u, v = ccr >> 1 & 1u;
        c->cr = (c->cr & 0x0fffffffu) | (n ? 0x80000000u : 0u) | (z ? 0x20000000u : 0u) | (!n && !z ? 0x40000000u : 0u) |
                (v ? 0x10000000u : 0u);
        c->xer = (c->xer & 0x1fffffffu) | (v ? 0xC0000000u : 0u) | (ccr & 1u ? 0x20000000u : 0u);
        c->gpr[26] = ccr & 16u ? 0x20000000u : 0u;
    }
    c->instructions += x->steps;
    m->cycles = x->start;
    ++x->stats.runs;
    x->stats.instructions += made;
    return x->steps;
}
