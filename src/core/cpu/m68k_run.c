/* The CPU's run loop, exception processing, interrupts, access faults, the
 * status register and the control registers.
 *
 * Sources:
 *   PRM (M68000 Family Programmer's Reference Manual): 1.3 supervisor
 *   programming model (SR, USP/ISP/MSP, VBR, SFC/DFC, CACR, CAAR); 2.2.6-
 *   2.2.13 indexed and memory-indirect modes and the brief/full extension
 *   word formats (2.1); section 4 MOVEC, RTE, STOP.
 *   MC68040 User's Manual: section 3 (MMU registers TC, ITT0/1, DTT0/1,
 *   MMUSR, URP, SRP and their implemented bits), section 4 (CACR DE/IE),
 *   section 7 (misaligned operands split into aligned cycles), section 8
 *   (exception processing: vector table, priorities, the format $0, $1, $2,
 *   $3, $4 and $7 stack frames, the access error SSW and writeback status
 *   words, trace, interrupts and the throwaway frame, format error).
 *   MC68030 User's Manual: section 6 (CACR bits), section 8 (format $A/$B
 *   bus fault frames and SSW).
 *   MC68020 User's Manual: CACR (E, F, CE, C), the same frame set as 030.
 */
#include "m68k_internal.h"

#include <stdio.h>

static const char *const model_names[M68K_MODEL_COUNT] = {"68020", "68030", "68040", "68LC040", "68000"};
const char *m68k_model_name(m68k_model model) { return model < M68K_MODEL_COUNT ? model_names[model] : "?"; }

static int is_040(const m68k_cpu *cpu) { return cpu->model == M68K_68040 || cpu->model == M68K_68LC040; }

/* --- the status register (PRM 1.3.1) ------------------------------------ */

/* Implemented SR bits on the 68020/030/040: T1 T0 S M I2-I0 X N Z V C;
 * the 68000's: T S I2-I0 X N Z V C (MC68000 UM 2.3). */
#define SR_IMPLEMENTED 0xF71Fu
#define SR_IMPLEMENTED_000 0xA71Fu

uint16_t m68k_sr(const m68k_cpu *cpu) { return (uint16_t)(cpu->sr_hi | ccr_of(cpu)); }
uint8_t m68k_ccr(const m68k_cpu *cpu) { return (uint8_t)ccr_of(cpu); }
void m68k_set_ccr(m68k_cpu *cpu, uint8_t ccr) { ccr_set(cpu, ccr); }

/* Stands in for a bus without page tables: every access takes the slow path
 * without a test in the fast one. */
static uint8_t *empty_map[M68K_PAGE_COUNT];

/* The maps of the current privilege: the bus's own while logical =
 * physical, the soft TLB's while the MMU translates (mmu.c). */
void m68k_select_maps(m68k_cpu *cpu) {
    const unsigned s = cpu->s ? 1u : 0u;
    if (cpu->mmu.mode) {
        uint8_t **t = cpu->mmu.tlb;
        cpu->pmap = t + (size_t)TLB_TABLE(0, s) * M68K_PAGE_COUNT;
        cpu->rmap = t + (size_t)TLB_TABLE(1, s) * M68K_PAGE_COUNT;
        cpu->wmap = cpu->wlog ? cpu->wlog_map : t + (size_t)TLB_TABLE(2, s) * M68K_PAGE_COUNT;
    } else {
        cpu->rmap = cpu->bus.read_map[s] ? cpu->bus.read_map[s] : empty_map;
        cpu->wmap = cpu->bus.write_map[s] ? cpu->bus.write_map[s] : empty_map;
        cpu->pmap = cpu->rmap;
    }
    cpu->fc_data = cpu->s ? M68K_FC_SUPER_DATA : M68K_FC_USER_DATA;
    cpu->fc_prog = cpu->s ? M68K_FC_SUPER_PROGRAM : M68K_FC_USER_PROGRAM;
}
#define select_maps m68k_select_maps

/* A7 is the active stack pointer: USP (S=0), ISP (S=1, M=0), MSP (S=1, M=1). */
static void store_a7(m68k_cpu *cpu) {
    if (!cpu->s) cpu->usp = A(7);
    else if (cpu->m) cpu->msp = A(7);
    else cpu->isp = A(7);
}
static void load_a7(m68k_cpu *cpu) {
    A(7) = !cpu->s ? cpu->usp : cpu->m ? cpu->msp : cpu->isp;
}

void m68k_update_events(m68k_cpu *cpu) {
    uint32_t ev = cpu->events & (EV_WRITEBACK | EV_STOP | EV_EXIT | EV_TRACE_NOW);
    const unsigned mask = (cpu->sr_hi >> 8) & 7u;
    if (cpu->ipl > mask || (cpu->ipl == 7 && !cpu->nmi_seen)) ev |= EV_IRQ;
    if (cpu->sr_hi & (SR_T1 | SR_T0)) ev |= EV_TRACE;
    cpu->events = ev;
}

static void write_sr(m68k_cpu *cpu, uint16_t sr) {
    sr &= cpu->model == M68K_68000 ? SR_IMPLEMENTED_000 : SR_IMPLEMENTED;
    store_a7(cpu);
    cpu->sr_hi = sr & 0xFF00u;
    cpu->s = (sr & SR_S) != 0;
    cpu->m = (sr & SR_M) != 0;
    ccr_set(cpu, sr & 0xFFu);
    load_a7(cpu);
    select_maps(cpu);
    m68k_update_events(cpu);
}

void m68k_set_sr(m68k_cpu *cpu, uint16_t sr) { write_sr(cpu, sr); }

uint32_t m68k_get_usp(const m68k_cpu *cpu) { return !cpu->s ? A(7) : cpu->usp; }
uint32_t m68k_get_isp(const m68k_cpu *cpu) { return (cpu->s && !cpu->m) ? A(7) : cpu->isp; }
uint32_t m68k_get_msp(const m68k_cpu *cpu) { return (cpu->s && cpu->m) ? A(7) : cpu->msp; }
void m68k_set_usp(m68k_cpu *cpu, uint32_t v) { if (!cpu->s) A(7) = v; else cpu->usp = v; }
void m68k_set_isp(m68k_cpu *cpu, uint32_t v) { if (cpu->s && !cpu->m) A(7) = v; else cpu->isp = v; }
void m68k_set_msp(m68k_cpu *cpu, uint32_t v) { if (cpu->s && cpu->m) A(7) = v; else cpu->msp = v; }

/* --- set-up ------------------------------------------------------------- */

const m68k_handler *m68k_table_for(m68k_model model);

void m68k_init(m68k_cpu *cpu, m68k_model model, const m68k_bus *bus) {
    m68k_tables_init();
    memset(cpu, 0, sizeof *cpu);
    cpu->model = model;
    cpu->bus = *bus;
    cpu->abortp = &cpu->abort;
    cpu->table = m68k_table_for(model);
    cpu->tm = m68k_timing_of(model, 0);
    cpu->ctab = cpu->tm->op;
    cpu->sr_hi = 0x2700;
    cpu->s = 1;
    select_maps(cpu);
    m68k_set_fpu(cpu, model == M68K_68020 ? M68K_FPU_68881 : model == M68K_68030 ? M68K_FPU_68882
                      : model == M68K_68040 ? M68K_FPU_68040 : M68K_FPU_NONE, 0);
}

void m68k_set_maps(m68k_cpu *cpu, uint8_t *const *read_user, uint8_t *const *write_user,
                   uint8_t *const *read_super, uint8_t *const *write_super) {
    cpu->bus.read_map[0] = read_user;
    cpu->bus.write_map[0] = write_user;
    cpu->bus.read_map[1] = read_super;
    cpu->bus.write_map[1] = write_super;
    m68k_mmu_changed(cpu);
}

/* --- bus access outside instructions (exception processing, reset) ------ */

/* One aligned piece through the page table or the bus; nonzero on a bus error. */
static int piece_read(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t *v) {
    uint8_t *const *map = cpu->bus.read_map[(fc & 4u) ? 1 : 0];
    const uint8_t *p = map ? map[a >> M68K_PAGE_SHIFT] : NULL;
    if (p) {
        p += a & (M68K_PAGE_SIZE - 1);
        *v = size == 1 ? p[0] : size == 2 ? ld16(p) : ld32(p);
        return 0;
    }
    if (!cpu->bus.read) return 1;
    cpu->bus_reads++;
    cpu->bus_pa = a;
    return cpu->bus.read(cpu->bus.ctx, a, size, fc, v);
}
static void store_piece(uint8_t *p, unsigned size, uint32_t v) {
    if (size == 1) p[0] = (uint8_t)v;
    else if (size == 2) st16(p, v);
    else st32(p, v);
}
static int piece_write(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t v) {
    uint8_t *const *map = cpu->bus.write_map[(fc & 4u) ? 1 : 0];
    uint8_t *p = map ? map[a >> M68K_PAGE_SHIFT] : NULL;
    if (p) {
        if (cpu->wlog) m68k_jit_wlog(cpu, p + (a & (M68K_PAGE_SIZE - 1)), size);
        store_piece(p + (a & (M68K_PAGE_SIZE - 1)), size, v);
        /* The translator drops what it translated from these bytes. */
        if (cpu->jit) m68k_jit_written(cpu, p, a & (M68K_PAGE_SIZE - 1), size);
        return 0;
    }
    if (!cpu->bus.write) return 1;
    cpu->bus_writes++;
    const int fault = cpu->bus.write(cpu->bus.ctx, a, size, fc, v);
    if (!fault && cpu->jit) {
        /* A bus that writes its memory through the callback (no write map)
         * may still serve the page's reads, and code, from its read map. */
        uint8_t *const *rmap = cpu->bus.read_map[(fc & 4u) ? 1 : 0];
        const uint8_t *r = rmap ? rmap[a >> M68K_PAGE_SHIFT] : NULL;
        if (r) m68k_jit_written(cpu, r, a & (M68K_PAGE_SIZE - 1), size);
    }
    return fault;
}

/* The aligned bus cycles of an operand (MC68040 UM section 7, misaligned
 * operands): a word at an odd address is two bytes; a long at offset 1 or 3
 * is byte, word, byte; at offset 2, two words. An access crossing a page
 * that the page table cannot serve whole is split the same way. */
typedef struct pieces { uint32_t addr[3]; uint8_t size[3]; uint8_t n; } pieces;
static void split(uint32_t a, unsigned size, pieces *p) {
    p->n = 0;
    if (size == 1 || (size == 2 && !(a & 1u)) || (size == 4 && !(a & 3u))) {
        p->addr[0] = a; p->size[0] = (uint8_t)size; p->n = 1;
    } else if (size == 2) {
        p->addr[0] = a; p->size[0] = 1; p->addr[1] = a + 1; p->size[1] = 1; p->n = 2;
    } else if ((a & 3u) == 2) {
        p->addr[0] = a; p->size[0] = 2; p->addr[1] = a + 2; p->size[1] = 2; p->n = 2;
    } else {
        p->addr[0] = a; p->size[0] = 1; p->addr[1] = a + 1; p->size[1] = 2; p->addr[2] = a + 3; p->size[2] = 1;
        p->n = 3;
    }
}

/* Where an access failed: 0 none, 1 the bus refused a cycle, 2 the MMU
 * refused the translation; `at` is the logical address of that piece. */
typedef struct refusal { int kind; uint32_t at; } refusal;

/* An aligned piece at a logical address: translated while the MMU is on
 * (filling the soft TLB for later accesses), then performed physically.
 * `write` asks for write permission (a write, or the read of a read-modify-
 * write). Instruction fetches are the accesses in program space. */
/* CPU space (function code 7, only MOVES makes such a data cycle) on the
 * 68020 and 68030 is not memory: A19-A16 give the cycle's type and type
 * 2 is a coprocessor's interface registers, its cpID in A15-A13 (MC68030
 * UM 7.4 "CPU space cycles", Figure 7-41; MC68881/MC68882 UM 7.2, the
 * coprocessor interface registers, the response CIR at $00). The FPU
 * (cpID 1) answers when fitted, and the MC68851 (cpID 0) in the 68020's
 * socket; the response CIR reads the null primitive ($0802, MC68881/
 * MC68882 UM 7.4.2) and writes are taken. No other device answers a CPU
 * space cycle on a Macintosh, so it ends in a bus error: the Universal
 * ROMs find an FPU by reading cpID 1's response CIR with MOVES and SFC = 7
 * (the IIsi's ROM 36B7FB6C at $40803A82, clearing the hardware flags' FPU
 * bit when it bus-errors). Returns -1 when the cycle is not one of these. */
static int cpu_space_piece(m68k_cpu *cpu, uint32_t a, unsigned fc, int store, uint32_t *v) {
    if ((fc & 7u) != M68K_FC_CPU || is_040(cpu)) return -1;
    if (((a >> 16) & 0xFu) != 2u) return ((a >> 16) & 0xFu) == 0xFu ? -1 : 1;
    const unsigned cpid = (a >> 13) & 7u;
    const int fitted = (cpid == 1 && cpu->fpu.type != M68K_FPU_NONE) || (cpid == 0 && cpu->pmmu.fitted);
    if (!fitted) return 1;
    if (!store) *v = (a & 0x1Fu) == 0 ? 0x0802u : 0;
    return 0;
}

static int lpiece(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, int write, int store, uint32_t *v) {
    uint32_t pa = a;
    const int cs = cpu_space_piece(cpu, a, fc, store, v);
    if (cs >= 0) {
        if (cs == 0 && !store && size == 1) *v &= 0xFFu;
        return cs;
    }
    if (cpu->mmu.mode) {
        const unsigned space = fc & 3u;
        if (space == 1u && write == store) {
            /* A device page translated before: the physical page without a
             * new translation, the ATC's replacement counter advanced as the
             * translation (an ATC hit) would. */
            const uintptr_t dev = (uintptr_t)cpu->mmu.tlb[(size_t)TLB_TABLE(store ? 5u : 4u, (fc & 4u) ? 1u : 0u) * M68K_PAGE_COUNT +
                                                         (a >> M68K_PAGE_SHIFT)];
            if (dev) {
                if (dev & TLB_DEV_ATC) cpu->mmu.ctr[0]++;
                pa = (uint32_t)(dev & ~(uintptr_t)(M68K_PAGE_SIZE - 1)) | (a & (M68K_PAGE_SIZE - 1));
                if (store) return piece_write(cpu, pa, size, fc, *v) ? 1 : 0;
                return piece_read(cpu, pa, size, fc, v) ? 1 : 0;
            }
        }
        unsigned fill = 0x83u;      /* mode 2: logical = physical, readable and writable as the bus maps them */
        const int ifetch = space == 2u;
        if (cpu->mmu.mode == 1) {
            cpu->mmu_searches++;
            if (m68k_mmu_translate(cpu, a, fc, write, ifetch, &pa, &fill)) return 2;
        }
        if (space == 1u || space == 2u) m68k_mmu_tlb_fill(cpu, a, fc, ifetch, pa, fill);
    }
    if (store) return piece_write(cpu, pa, size, fc, *v) ? 1 : 0;
    return piece_read(cpu, pa, size, fc, v) ? 1 : 0;
}

static int access_read_rf(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, int lock, uint32_t *value, refusal *r) {
    pieces p;
    split(a, size, &p);
    uint32_t v = 0;
    for (unsigned i = 0; i < p.n; i++) {
        uint32_t part = 0;
        const int k = lpiece(cpu, p.addr[i], p.size[i], fc, lock, 0, &part);
        if (k) {
            r->kind = k;
            r->at = p.addr[i];
            return k;
        }
        v = (v << (8u * p.size[i])) | part;
    }
    *value = v;
    return 0;
}
static int access_write_rf(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t value, refusal *r) {
    pieces p;
    split(a, size, &p);
    unsigned left = size;
    for (unsigned i = 0; i < p.n; i++) {
        left -= p.size[i];
        uint32_t part = (value >> (8u * left)) & sz_mask(p.size[i]);
        const int k = lpiece(cpu, p.addr[i], p.size[i], fc, 1, 1, &part);
        if (k) {
            r->kind = k;
            r->at = p.addr[i];
            return k;
        }
    }
    return 0;
}
/* A data access the fast path did not make (it crosses a page, or comes
 * from exception processing) through the soft TLB, when the table of its
 * kind maps the page of every aligned piece: the pieces are made there,
 * with the one other effect each piece's translation has (68040: an ATC
 * hit steps the data ATC's replacement counter, unless transparent
 * translation or TCR E = 0 keeps it from the ATC). A mapped page has a
 * valid ATC entry (or a transparent translation) allowing the access (for
 * a write, M set and no protection) and no translated code (else its write
 * mapping is held back), so the translation would hit, search nothing and
 * the translator would find nothing to drop: the result is the slow
 * path's. 0 when a page is not mapped (nothing was done). */
static int soft_access(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, int store, uint32_t *v) {
    const m68k_mmu *m = &cpu->mmu;
    if (!m->mode || !m->tlb || (fc & 3u) != 1u || cpu->locked) return 0;
    const int counts = m->mode == 1;
    if (counts && !is_040(cpu)) return 0;
    const unsigned s = (fc & 4u) ? 1u : 0u;
    uint8_t *const *t = m->tlb + (size_t)TLB_TABLE(store ? 2u : 1u, s) * M68K_PAGE_COUNT;
    pieces p;
    split(a, size, &p);
    uint8_t *host[3];
    unsigned steps = 0;
    for (unsigned i = 0; i < p.n; i++) {
        host[i] = t[p.addr[i] >> M68K_PAGE_SHIFT];
        if (!host[i]) return 0;
        if (counts) steps += (unsigned)m68k_mmu040_data_steps(cpu, p.addr[i], s);
    }
    cpu->mmu.ctr[0] = (uint8_t)(cpu->mmu.ctr[0] + steps);
    if (store) {
        unsigned left = size;
        for (unsigned i = 0; i < p.n; i++) {
            left -= p.size[i];
            if (cpu->wlog) m68k_jit_wlog(cpu, host[i] + (p.addr[i] & (M68K_PAGE_SIZE - 1)), p.size[i]);
            store_piece(host[i] + (p.addr[i] & (M68K_PAGE_SIZE - 1)), p.size[i], (*v >> (8u * left)) & sz_mask(p.size[i]));
        }
        return 1;
    }
    uint32_t r = 0;
    for (unsigned i = 0; i < p.n; i++) {
        const uint8_t *q = host[i] + (p.addr[i] & (M68K_PAGE_SIZE - 1));
        r = (r << (8u * p.size[i])) | (p.size[i] == 1 ? q[0] : p.size[i] == 2 ? ld16(q) : ld32(q));
    }
    *v = r;
    return 1;
}

static int access_read(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t *value) {
    if (soft_access(cpu, a, size, fc, 0, value)) return 0;
    refusal r;
    return access_read_rf(cpu, a, size, fc, 0, value, &r);
}
static int access_write(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t value) {
    if (soft_access(cpu, a, size, fc, 1, &value)) return 0;
    refusal r;
    return access_write_rf(cpu, a, size, fc, value, &r);
}

/* --- access faults ------------------------------------------------------ */

/* SSW SIZE field, 68040 (MC68040 UM, format $7 frame): 00 long, 01 byte,
 * 10 word, 11 line. */
static unsigned size_code_040(unsigned size) { return size == 1 ? 1u : size == 2 ? 2u : size == 16 ? 3u : 0u; }
/* 68020/030 SSW SIZE: 01 byte, 10 word, 11 three bytes, 00 long (MC68030 UM
 * section 8, the special status word of the bus fault frames). */
static unsigned size_code_030(unsigned size) { return size == 1 ? 1u : size == 2 ? 2u : size == 3 ? 3u : 0u; }

/* TT of the transfer (MC68040 UM Table 3-2): 10 for the alternate spaces
 * (function codes 0, 3, 4, 7), 00 for normal accesses. */
static unsigned tt_of(unsigned fc) {
    fc &= 7u;
    return (fc == 0 || fc == 3 || fc == 4 || fc == 7) ? 2u : 0u;
}

/* The fault record of the access at `a` refused as `r` says. FA is the
 * access's first byte whichever piece failed; MA marks an MMU refusal of the
 * second page of an access spanning two (MC68040 UM 8.4.6.2, 8.4.6.4). */
static void record_fault(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, int write, const refusal *r) {
    m68k_fault *f = &cpu->fault;
    const int atc = r->kind == 2;
    const int misaligned = atc && ((r->at ^ a) >> M68K_PAGE_SHIFT) != 0;
    f->address = a;
    f->ea = a;
    f->size = (uint8_t)size;
    f->fc = (uint8_t)fc;
    f->write = (uint8_t)write;
    f->atc = (uint8_t)atc;
    f->misaligned = (uint8_t)misaligned;
    f->locked = cpu->locked;
    if (is_040(cpu)) {
        /* MA (11), ATC (10), LK (9), RW (8, 1 = read), SIZE (6-5), TT
         * (4-3), TM (2-0) = the function code. */
        f->ssw = (uint16_t)((misaligned ? 0x0800u : 0) | (atc ? 0x0400u : 0) | (cpu->locked ? 0x0200u : 0) |
                            (write ? 0 : 0x0100u) | (size_code_040(size) << 5) | (tt_of(fc) << 3) | (fc & 7u));
    } else {
        /* DF (8) for a data cycle, RM (7) for the read of a read-modify-
         * write (MC68020 UM 6.4.1, Figure 6-2; the MC68851
         * refuses such a cycle without a table search, MC68851 UM
         * 6.3.1.7), RW (6, 1 = read), SIZE (5-4), FC (2-0). */
        const int program = (fc & 3u) == 2;
        f->ssw = (uint16_t)((program ? 0 : 0x0100u) | (cpu->locked && !write ? 0x0080u : 0) | (write ? 0 : 0x0040u) |
                            (size_code_030(size) << 4) | (fc & 7u));
    }
}

/* The write mapping the translator holds back for a page with translated
 * code (mmu.c, table kind 3), for an access the fast path would have made
 * whole: it is made here, as the fast path would make it (no translation),
 * and the translator sees it. NULL when there is none. */
static uint8_t *held_page(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc) {
    if (!cpu->mmu.mode || fc != cpu->fc_data || (a & (M68K_PAGE_SIZE - 1)) > M68K_PAGE_SIZE - size) return NULL;
    return cpu->mmu.tlb[(size_t)TLB_TABLE(3, cpu->s ? 1u : 0u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)];
}

/* The read a 68020/030 bus error handler completed itself (m68k.h,
 * dib_valid): 1 with the data input buffer's value in *v. */
static int dib_take(m68k_cpu *cpu, uint32_t a, unsigned size, uint32_t *v) {
    if (M68K_LIKELY(!cpu->dib_valid)) return 0;
    if (cpu->instructions != cpu->dib_count) {
        cpu->dib_valid = 0;
        return 0;
    }
    if (a != cpu->dib_address || size != cpu->dib_size) return 0;
    cpu->dib_valid = 0;
    *v = cpu->dib_value & sz_mask(size);
    return 1;
}

/* The 68030 prefetches the two words after the instruction it executes
 * (MC68030 UM 11.1, the instruction pipe), so the instruction after a PMOVE
 * to TC comes from the words fetched with the translation it replaced:
 * the LC II's ROM (35C28F5F at $40A416B2) enables translation with code
 * running at $00A4xxxx, which the new tables do not map (or, with 10 MB,
 * map to RAM at $84xxxx), and leaves by the JMP (A5) already in the pipe.
 * Livadix has no pipe: the words are read here before the change and stand in
 * for the fetch of those addresses, whatever the new translation gives
 * there, during the next instruction only. */
void m68k_prefetch_hold(m68k_cpu *cpu) {
    uint32_t v = 0;
    cpu->pf_until = 0;
    if (access_read(cpu, cpu->pc, 4, cpu->fc_prog, &v)) return;
    cpu->pf_addr = cpu->pc;
    cpu->pf_data = v;
    cpu->pf_until = cpu->instructions + 2u;
}

uint32_t m68k_slow_read(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc) {
    uint32_t v = 0;
    refusal r;
    if (dib_take(cpu, a, size, &v)) return v;
    if (cpu->instructions < cpu->pf_until && (fc & 3u) == 2u && size == 2 && (a - cpu->pf_addr) <= 2u)
        return (a == cpu->pf_addr ? cpu->pf_data >> 16 : cpu->pf_data) & 0xFFFFu;
    if (soft_access(cpu, a, size, fc, 0, &v)) return v;
    if (access_read_rf(cpu, a, size, fc, cpu->locked, &v, &r) == 0) return v;
    record_fault(cpu, a, size, fc, 0, &r);
    m68k_abort(cpu, ABORT_ACCESS);
}

int m68k_try_read(m68k_cpu *cpu, uint32_t a, unsigned size, uint32_t *v) {
    refusal r;
    *v = 0;
    if (dib_take(cpu, a, size, v)) return 0;
    const unsigned fc = cpu->fc_data;
    if (cpu->mmu.mode && !(a & (size - 1u)) && (fc & 3u) == 1u) {
        /* One aligned piece of a device page translated before: lpiece's
         * device path, without the split. */
        const uintptr_t dev = (uintptr_t)cpu->mmu.tlb[(size_t)TLB_TABLE(4u, (fc & 4u) ? 1u : 0u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)];
        if (dev) {
            if (dev & TLB_DEV_ATC) cpu->mmu.ctr[0]++;
            const uint32_t pa = (uint32_t)(dev & ~(uintptr_t)(M68K_PAGE_SIZE - 1)) | (a & (M68K_PAGE_SIZE - 1));
            if (piece_read(cpu, pa, size, fc, v) == 0) return 0;
            *v = 0;
            r.kind = 1;
            r.at = a;
            record_fault(cpu, a, size, fc, 0, &r);
            return 1;
        }
    }
    if (soft_access(cpu, a, size, fc, 0, v)) return 0;
    if (access_read_rf(cpu, a, size, fc, 0, v, &r) == 0) return 0;
    record_fault(cpu, a, size, cpu->fc_data, 0, &r);
    return 1;
}

uint32_t m68k_slow_read_locked(m68k_cpu *cpu, uint32_t a, unsigned size) {
    if (cpu->wlog && (a & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - size) {
        /* Writes are being logged: the write table rd_locked stands in for. */
        const uint8_t *p = cpu->mmu.tlb[(size_t)TLB_TABLE(2, cpu->s ? 1u : 0u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)];
        if (p) {
            p += a & (M68K_PAGE_SIZE - 1);
            return size == 1 ? p[0] : size == 2 ? ld16(p) : ld32(p);
        }
    }
    const uint8_t *held = held_page(cpu, a, size, cpu->fc_data);
    if (held) {
        const uint32_t off = a & (M68K_PAGE_SIZE - 1);
        return size == 1 ? held[off] : size == 2 ? ld16(held + off) : ld32(held + off);
    }
    cpu->locked = 1;
    const uint32_t v = m68k_slow_read(cpu, a, size, cpu->fc_data);
    cpu->locked = 0;
    return v;
}

void m68k_slow_write(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t value) {
    if (cpu->wlog && fc == cpu->fc_data && (a & (M68K_PAGE_SIZE - 1)) <= M68K_PAGE_SIZE - size) {
        /* Writes are being logged: what the fast path would have written
         * through the write table it stands in for. */
        uint8_t *p = cpu->mmu.tlb[(size_t)TLB_TABLE(2, cpu->s ? 1u : 0u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)];
        if (p) {
            p += a & (M68K_PAGE_SIZE - 1);
            m68k_jit_wlog(cpu, p, size);
            store_piece(p, size, value);
            return;
        }
    }
    uint8_t *held = held_page(cpu, a, size, fc);
    if (held) {
        const uint32_t off = a & (M68K_PAGE_SIZE - 1);
        if (cpu->wlog) m68k_jit_wlog(cpu, held + off, size);
        store_piece(held + off, size, value);
        m68k_jit_written(cpu, held, off, size);
        return;
    }
    refusal r = {0, a};
    if (cpu->wb_n == 0) {
        const uintptr_t dev = cpu->mmu.mode && !(a & (size - 1u)) && (fc & 3u) == 1u
                                  ? (uintptr_t)cpu->mmu.tlb[(size_t)TLB_TABLE(5u, (fc & 4u) ? 1u : 0u) * M68K_PAGE_COUNT + (a >> M68K_PAGE_SHIFT)]
                                  : 0;
        if (dev) {
            /* One aligned piece of a device page translated before: lpiece's
             * device path, without the split. */
            if (dev & TLB_DEV_ATC) cpu->mmu.ctr[0]++;
            const uint32_t pa = (uint32_t)(dev & ~(uintptr_t)(M68K_PAGE_SIZE - 1)) | (a & (M68K_PAGE_SIZE - 1));
            if (piece_write(cpu, pa, size, fc, value & sz_mask(size)) == 0) return;
            r.kind = 1;
        } else if (soft_access(cpu, a, size, fc, 1, &value) || access_write_rf(cpu, a, size, fc, value, &r) == 0) {
            return;
        }
    }
    if (cpu->in_exception) m68k_abort(cpu, ABORT_HALT);
    if (cpu->restart_writes) {
        record_fault(cpu, a, size, fc, 1, &r);
        m68k_abort(cpu, ABORT_ACCESS);
    }
    /* The write waits in a writeback slot; once one write has faulted, the
     * instruction's later writes queue behind it (MC68040 UM 8.4.6.7: a
     * write the bus refused is in WB1 with up to two more in WB2 and WB3; a
     * write the MMU refused is in WB2 with one more in WB3; the handler
     * completes them in that order). */
    if (cpu->wb_n == 0) {
        record_fault(cpu, a, size, fc, 1, &r);
        cpu->wb_atc = (uint8_t)(r.kind == 2);
    }
    if (cpu->wb_n < 2) {
        m68k_writeback *w = &cpu->wb[cpu->wb_n++];
        w->address = a;
        w->data = value;
        w->status = (uint16_t)(0x80u | (size_code_040(size) << 5) | (tt_of(fc) << 3) | (fc & 7u));
    }
    cpu->events |= EV_WRITEBACK;
}

/* MOVES (MC68040 UM Table 3-2): function codes 2 and 6 are converted to the
 * data spaces 1 and 5 on the 68040; the 68020/030 keep them. */
static unsigned moves_fc(const m68k_cpu *cpu, unsigned fc) {
    fc &= 7u;
    if (is_040(cpu) && (fc & 3u) == 2u) fc--;
    return fc;
}
uint32_t m68k_moves_read(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc) {
    return m68k_slow_read(cpu, a, size, moves_fc(cpu, fc));
}
void m68k_moves_write(m68k_cpu *cpu, uint32_t a, unsigned size, unsigned fc, uint32_t value) {
    m68k_slow_write(cpu, a, size, moves_fc(cpu, fc), value);
}

M68K_NORETURN void m68k_abort(m68k_cpu *cpu, int kind) {
    cpu->abort_kind = kind;
    longjmp(*cpu->abortp, kind);
}

/* --- exception processing (MC68040 UM section 8) ------------------------ */

static uint32_t sup_read32(m68k_cpu *cpu, uint32_t a) {
    uint32_t v;
    if (access_read(cpu, a, 4, M68K_FC_SUPER_DATA, &v)) m68k_abort(cpu, ABORT_HALT);
    return v;
}
static void sup_write(m68k_cpu *cpu, uint32_t a, unsigned size, uint32_t v) {
    if (access_write(cpu, a, size, M68K_FC_SUPER_DATA, v)) m68k_abort(cpu, ABORT_HALT);
}

/* A frame's writes (word SR, long PC, word format/vector offset, the extra
 * words) in one stroke when the soft TLB maps the frame's page, the frame
 * lies inside it and A7 is even: each write would be soft_access's, with
 * the same pieces (split: the PC's long write two words when A7 + 2 is
 * not a long word's address), an ATC step each, and the same write log.
 * 0 otherwise. */
static int frame_fast(m68k_cpu *cpu, uint32_t sp, const uint8_t *frame, unsigned len) {
    const m68k_mmu *m = &cpu->mmu;
    if (!m->mode || !m->tlb || cpu->locked || (sp & 1u) || (sp & (M68K_PAGE_SIZE - 1)) + len > M68K_PAGE_SIZE) return 0;
    const int counts = m->mode == 1;
    if (counts && !is_040(cpu)) return 0;
    uint8_t *host = m->tlb[(size_t)TLB_TABLE(2u, 1u) * M68K_PAGE_COUNT + (sp >> M68K_PAGE_SHIFT)];
    if (!host) return 0;
    if (counts) {
        const unsigned parts = len / 2u - 1u + (((sp + 2u) & 3u) ? 1u : 0u);
        cpu->mmu.ctr[0] = (uint8_t)(cpu->mmu.ctr[0] + (unsigned)m68k_mmu040_data_steps(cpu, sp, 1u) * parts);
    }
    uint8_t *p = host + (sp & (M68K_PAGE_SIZE - 1));
    if (cpu->wlog) {
        m68k_jit_wlog(cpu, p, 2);
        m68k_jit_wlog(cpu, p + 2, 4);
        m68k_jit_wlog(cpu, p + 6, 2);
        for (unsigned i = 8; i < len; i += 2) m68k_jit_wlog(cpu, p + i, 2);
    }
    memcpy(p, frame, len);
    return 1;
}

/* Writes a frame at A7: SR, PC, format/vector offset, then `extra` bytes
 * (big-endian, already laid out). */
static void push_frame(m68k_cpu *cpu, uint16_t sr, uint32_t pc, unsigned format, unsigned vector,
                       const uint8_t *extra, unsigned n) {
    const uint32_t sp = A(7) - 8u - n;
    uint8_t frame[8 + 64];
    if (n <= 64) {
        const uint16_t fv = (uint16_t)((format << 12) | (vector * 4u));
        frame[0] = (uint8_t)(sr >> 8); frame[1] = (uint8_t)sr;
        frame[2] = (uint8_t)(pc >> 24); frame[3] = (uint8_t)(pc >> 16); frame[4] = (uint8_t)(pc >> 8); frame[5] = (uint8_t)pc;
        frame[6] = (uint8_t)(fv >> 8); frame[7] = (uint8_t)fv;
        if (n) memcpy(frame + 8, extra, n);
        if (frame_fast(cpu, sp, frame, 8u + n)) {
            A(7) = sp;
            return;
        }
    }
    sup_write(cpu, sp, 2, sr);
    sup_write(cpu, sp + 2, 4, pc);
    sup_write(cpu, sp + 6, 2, (format << 12) | (vector * 4u));
    for (unsigned i = 0; i < n; i += 2) sup_write(cpu, sp + 8 + i, 2, (uint32_t)(extra[i] << 8 | extra[i + 1]));
    A(7) = sp;
}

static void begin_exception(m68k_cpu *cpu, unsigned vector, uint32_t pc, uint16_t *old_sr) {
    if (cpu->on_exception) cpu->on_exception(cpu->hook_ctx, cpu, vector, pc);
    cpu->in_exception = 1;
    *old_sr = m68k_sr(cpu);
    /* S set, both trace bits cleared (MC68040 UM section 8, exception
     * processing sequence); M is kept: the frame goes on the active stack. */
    write_sr(cpu, (uint16_t)((*old_sr | SR_S) & ~(SR_T1 | SR_T0)));
    cpu->events &= ~(uint32_t)EV_STOP;
}
static void end_exception(m68k_cpu *cpu, unsigned vector) {
    cpu->pc = sup_read32(cpu, cpu->vbr + vector * 4u);
    cpu->in_exception = 0;
}

/* Exception processing's count by vector (m68k_timing.c); divide by zero,
 * CHK, CHK2, TRAPcc and TRAPV are their instructions' (the taken rows). */
static unsigned exception_count(const m68k_timing *tm, unsigned vector) {
    switch (vector) {
    case VEC_ACCESS: return tm->x_access;
    case VEC_ADDRESS: return tm->x_address;
    case VEC_TRACE: return tm->x_trace;
    case VEC_ZERO_DIVIDE: case VEC_CHK: case VEC_TRAPCC: return 0;
    default: return tm->x_trap;
    }
}

static void take(m68k_cpu *cpu, unsigned vector, unsigned format, uint32_t pc, const uint8_t *extra, unsigned n) {
    uint16_t old;
    cpu->cyc_extra += (int32_t)exception_count(cpu->tm, vector);
    begin_exception(cpu, vector, pc, &old);
    push_frame(cpu, old, pc, format, vector, extra, n);
    end_exception(cpu, vector);
}

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

void m68k_exception(m68k_cpu *cpu, unsigned vector, uint32_t pc) { take(cpu, vector, 0, pc, NULL, 0); }

/* Format $2 (MC68040 UM section 8; MC68020/030 the same six words): the
 * address field holds the instruction's address for CHK, CHK2, TRAPcc,
 * TRAPV, divide by zero and trace, the faulted address for an address error. */
void m68k_exception_fmt2(m68k_cpu *cpu, unsigned vector, uint32_t pc, uint32_t address) {
    uint8_t x[4];
    be32(x, address);
    take(cpu, vector, 2, pc, x, 4);
}

/* Exceptions that stop the instruction before it executes cancel its trace
 * (MC68040 UM section 8, exception priorities: illegal, unimplemented and
 * privilege violations are not traced). */
static void cancel_trace(m68k_cpu *cpu) { cpu->events &= ~(uint32_t)EV_TRACE_NOW; }

/* Illegal, line A, line F and privilege violations: format $0 with the
 * instruction's own address, so the handler sees the opcode at the stacked
 * PC. Timed, the instruction counts the exception's time alone (MC68030 UM
 * 11.6.17: illegal, line A/F traps and privilege violations as rows of
 * their own). */
static void at_instruction(m68k_cpu *cpu, unsigned vector) {
    cancel_trace(cpu);
    cpu->cyc_extra = cpu->tm->unit ? 0 : -(int32_t)cpu->ctab[cpu->op];
    cpu->pc = cpu->ipc;
    m68k_exception(cpu, vector, cpu->ipc);
}
void m68k_exception_illegal(m68k_cpu *cpu) { at_instruction(cpu, VEC_ILLEGAL); }
void m68k_exception_privilege(m68k_cpu *cpu) { at_instruction(cpu, VEC_PRIVILEGE); }
void m68k_exception_line_a(m68k_cpu *cpu) {
    if (cpu->jit) m68k_jit_trap(cpu);
    at_instruction(cpu, VEC_LINE_A);
}
void m68k_exception_line_f(m68k_cpu *cpu) { at_instruction(cpu, VEC_LINE_F); }
/* 68LC040 floating-point instruction: line F with a format $4 frame: the
 * next instruction's PC stacked, the calculated effective address (0 for
 * register and immediate operands) and the faulted instruction's PC
 * (MC68040 UM A.5.1; fpu.c decodes the instruction to find them). */
void m68k_exception_fp_unavailable(m68k_cpu *cpu, uint32_t ea) {
    uint8_t x[8];
    cancel_trace(cpu);
    be32(x, ea);
    be32(x + 4, cpu->ipc);
    take(cpu, VEC_LINE_F, 4, cpu->pc, x, 8);
}

void m68k_unimplemented(m68k_cpu *cpu) {
    m68k_abort(cpu, ABORT_UNIMPLEMENTED);
}

/* The access error (vector 2). 68040: format $7, 30 words. 68020/030: format
 * $B (long bus fault, 46 words) with the fields this design keeps; the
 * instruction restarts from the stacked PC (internal state words zero). */
static void access_error(m68k_cpu *cpu, uint32_t pc, int trace_continuation) {
    const m68k_fault *f = &cpu->fault;
    if (is_040(cpu)) {
        uint8_t x[52];
        memset(x, 0, sizeof x);
        uint16_t ssw = f->ssw;
        if (trace_continuation) ssw |= 0x2000u;     /* CT */
        be32(x + 0x00, f->ea);                       /* $08 EA */
        be16(x + 0x04, ssw);                         /* $0C SSW */
        /* $0E WB3S, $10 WB2S, $12 WB1S; $14 FA; $18 WB3A, $1C WB3D,
         * $20 WB2A, $24 WB2D, $28 WB1A, $2C WB1D/PD0, $30-$3B PD1-PD3. */
        be32(x + 0x0C, f->address);
        /* Slots, as offsets in x (frame offset - 8): WBnS at $06/$08/$0A for
         * n = 3/2/1, WBnA and WBnD at $10 (3), $18 (2), $20 (1). WB1D is
         * memory aligned (Table 8-5: the data rotated to the byte lanes of
         * its address); WB2D and WB3D are register aligned. */
        static const uint8_t st_at[4] = {0, 0x0A, 0x08, 0x06}, a_at[4] = {0, 0x20, 0x18, 0x10};
        const unsigned first = cpu->wb_atc ? 2u : 1u;
        for (unsigned i = 0; i < cpu->wb_n && i < 2; i++) {
            const unsigned slot = first + i;
            const m68k_writeback *w = &cpu->wb[i];
            uint32_t d = w->data;
            if (slot == 1) {
                const unsigned code = (w->status >> 5) & 3u, size = code == 1 ? 1u : code == 2 ? 2u : 4u;
                const unsigned rot = 8u * (w->address & 3u);
                d <<= 32u - 8u * size;
                if (rot) d = (d >> rot) | (d << (32u - rot));
            }
            be16(x + st_at[slot], w->status);
            be32(x + a_at[slot], w->address);
            be32(x + a_at[slot] + 4, d);
        }
        cpu->wb_n = 0;
        take(cpu, VEC_ACCESS, 7, pc, x, 52);
    } else {
        uint8_t x[84];
        memset(x, 0, sizeof x);
        be16(x + 0x02, f->ssw);                      /* $0A SSW */
        be32(x + 0x08, f->address);                  /* $10 data cycle fault address */
        if (f->write) be32(x + 0x10, cpu->wb_n ? cpu->wb[0].data : 0);   /* $18 data output buffer */
        be32(x + 0x1C, f->address);                  /* $24 stage B address */
        cpu->wb_n = 0;
        take(cpu, VEC_ACCESS, 0xB, pc, x, 84);
    }
}

/* Address error: a branch to an odd address (MC68040 UM section 8, "Address
 * Error Exception": the 68040 has no address error for misaligned data, only
 * for an odd instruction address). Stacked PC: the instruction that made the
 * branch; address field: the odd address. 68020/030: format $A short frame. */
static void address_error(m68k_cpu *cpu) {
    const uint32_t target = cpu->pc;
    if (is_040(cpu)) {
        /* The referenced address with bit 0 cleared (MC68040 UM 8.4.3:
         * "address is the reference address - 1"; 8.4.6.7). */
        m68k_exception_fmt2(cpu, VEC_ADDRESS, cpu->ipc, target & ~1u);
    } else {
        uint8_t x[24];
        memset(x, 0, sizeof x);
        be16(x + 0x02, (uint16_t)(0x4000u | 0x0040u | (2u << 4) | cpu->fc_prog));   /* FB, RW, word, FC */
        be32(x + 0x08, target);
        take(cpu, VEC_ADDRESS, 0xA, cpu->ipc, x, 24);
    }
}

static void take_interrupt(m68k_cpu *cpu, unsigned level) {
    int v = cpu->bus.iack ? cpu->bus.iack(cpu->bus.ctx, level) : M68K_IACK_AUTOVECTOR;
    const unsigned vector = v == M68K_IACK_AUTOVECTOR ? VEC_AUTOVECTOR + level
                            : v == M68K_IACK_SPURIOUS ? VEC_SPURIOUS
                                                      : (unsigned)v & 0xFFu;
    uint16_t old;
    if (cpu->on_exception) cpu->on_exception(cpu->hook_ctx, cpu, vector, cpu->pc);
    cpu->irqs_taken++;
    cpu->cyc_extra += (int32_t)cpu->tm->x_int + (cpu->m ? (int32_t)cpu->tm->x_int_m : 0);
    cpu->in_exception = 1;
    old = m68k_sr(cpu);
    write_sr(cpu, (uint16_t)(((old | SR_S) & ~(SR_T1 | SR_T0 | SR_MASK)) | (level << 8)));
    cpu->events &= ~(uint32_t)EV_STOP;
    push_frame(cpu, old, cpu->pc, 0, vector, NULL, 0);
    if (cpu->m) {
        /* M set: the format $0 frame went on the master stack; clear M and
         * stack a format $1 throwaway frame on the interrupt stack, whose SR
         * still has M set so the handler's RTE returns to the master stack
         * (MC68040 UM section 8, "Interrupt Exception"). */
        const uint16_t with_m = m68k_sr(cpu);
        write_sr(cpu, (uint16_t)(with_m & ~SR_M));
        push_frame(cpu, with_m, cpu->pc, 1, vector, NULL, 0);
    }
    end_exception(cpu, vector);
}

void m68k_set_irq(m68k_cpu *cpu, unsigned level) {
    level &= 7u;
    if (level < 7) cpu->nmi_seen = 0;
    cpu->ipl = (uint8_t)level;
    m68k_update_events(cpu);
}

/* Between instructions: pending access error, trace, interrupts, odd PC,
 * STOP. Returns nonzero to leave the run loop. */
static int between_body(m68k_cpu *cpu) {
    const uint32_t ev = cpu->events;
    if (ev & EV_EXIT) {
        cpu->events &= ~(uint32_t)EV_EXIT;
        return 1;
    }
    if (ev & EV_WRITEBACK) {
        const int traced = (ev & EV_TRACE_NOW) != 0;
        cpu->events &= ~(uint32_t)(EV_WRITEBACK | EV_TRACE_NOW);
        access_error(cpu, cpu->pc, traced);
    } else if (ev & EV_TRACE_NOW) {
        cpu->events &= ~(uint32_t)EV_TRACE_NOW;
        if (cpu->trace_pending == 1 || (cpu->trace_pending == 2 && cpu->flow))
            m68k_exception_fmt2(cpu, VEC_TRACE, cpu->pc, cpu->ipc);
    }
    if (cpu->events & EV_IRQ) {
        const unsigned mask = (cpu->sr_hi >> 8) & 7u;
        if (cpu->ipl == 7 && !cpu->nmi_seen) {
            cpu->nmi_seen = 1;
            take_interrupt(cpu, 7);
        } else if (cpu->ipl > mask && cpu->ipl < 7) {
            take_interrupt(cpu, cpu->ipl);
        }
        m68k_update_events(cpu);
    }
    if (cpu->pc & 1u) {
        address_error(cpu);
        if (cpu->pc & 1u) {     /* an odd vector: no way forward */
            cpu->status = M68K_HALTED;
            return 1;
        }
    }
    if (cpu->events & EV_STOP) {
        cpu->status = M68K_STOPPED;
        return 1;
    }
    return 0;
}
/* ... and the exceptions it processed counted. */
static int between(m68k_cpu *cpu) {
    const int r = between_body(cpu);
    if (cpu->cyc_extra) {
        m68k_charge(cpu, cpu->cyc_extra);
        cpu->cyc_extra = 0;
    }
    return r;
}

/* Arms the trace for the instruction about to run: T1 traces every
 * instruction, T0 only a change of flow (PRM 1.3.1). */
static void arm_trace(m68k_cpu *cpu) {
    cpu->trace_pending = (cpu->sr_hi & SR_T1) ? 1 : 2;
    cpu->flow = 0;
    cpu->events |= EV_TRACE_NOW;
}

static void undo(m68k_cpu *cpu) {
    for (unsigned i = cpu->undo_n; i-- > 0;) cpu->r[cpu->undo_reg[i & 3u]] = cpu->undo_val[i & 3u];
    cpu->undo_n = 0;
    cpu->pc = cpu->ipc;
}

/* For instructions that decide on an exception after computing their
 * effective address: the address-register steps are undone, the PC kept. */
void m68k_undo_steps(m68k_cpu *cpu) {
    const uint32_t pc = cpu->pc;
    undo(cpu);
    cpu->pc = pc;
}
/* A pre-instruction exception (MC68040 UM 8.3, group 2; MC68881 UM 6.1.13):
 * the instruction is undone and its own address stacked, so RTE restarts it. */
void m68k_exception_restart(m68k_cpu *cpu, unsigned vector) {
    undo(cpu);
    cancel_trace(cpu);
    m68k_exception(cpu, vector, cpu->ipc);
}
/* Any frame: `extra` holds the words after the format/vector word. */
void m68k_exception_frame(m68k_cpu *cpu, unsigned vector, unsigned format, uint32_t pc, const uint8_t *extra, unsigned n) {
    take(cpu, vector, format, pc, extra, n);
}

/* A translated block's read was refused (m68k_try_read recorded the fault)
 * before its instruction changed anything: the access error, as
 * handle_abort takes it for the interpreter. */
/* A faulted instruction counts as a step (one count untimed; timed only
 * its exception's time, the access error's): its exception was processed
 * in its place. */
static void fault_retire(m68k_cpu *cpu) {
    m68k_charge(cpu, (int64_t)cpu->tm->unit + cpu->cyc_extra);
    cpu->cyc_extra = 0;
}

void m68k_jit_fault(m68k_cpu *cpu) {
    cpu->undo_n = 0;
    cpu->pc = cpu->ipc;
    cancel_trace(cpu);
    cpu->wb_n = 0;
    cpu->events &= ~(uint32_t)EV_WRITEBACK;
    cpu->cyc_extra = 0;
    cpu->instructions++;
    access_error(cpu, cpu->ipc, 0);
    fault_retire(cpu);
}

static void handle_abort(m68k_cpu *cpu) {
    const int kind = cpu->abort_kind;
    cpu->in_exception = 0;
    cpu->restart_writes = 0;
    cpu->locked = 0;
    switch (kind) {
    case ABORT_ACCESS:
        /* What the instruction added before its fault is dropped. */
        undo(cpu);
        cancel_trace(cpu);
        cpu->wb_n = 0;
        cpu->events &= ~(uint32_t)EV_WRITEBACK;
        cpu->cyc_extra = 0;
        cpu->instructions++;
        access_error(cpu, cpu->ipc, 0);
        fault_retire(cpu);
        break;
    case ABORT_ILLEGAL:
        undo(cpu);
        cpu->cyc_extra = 0;
        cpu->instructions++;
        m68k_exception_illegal(cpu);
        m68k_charge(cpu, (int64_t)cpu->ctab[cpu->op] + cpu->cyc_extra);
        cpu->cyc_extra = 0;
        break;
    case ABORT_UNIMPLEMENTED:
        undo(cpu);
        cpu->status = M68K_UNIMPLEMENTED;
        cpu->unimplemented_op = (uint32_t)(cpu->pmap && cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT]
                                               ? ld16(cpu->pmap[cpu->pc >> M68K_PAGE_SHIFT] + (cpu->pc & (M68K_PAGE_SIZE - 1)))
                                               : 0xFFFFFFFFu);
        cpu->events |= EV_EXIT;
        break;
    default:
        cpu->status = M68K_HALTED;
        cpu->events |= EV_EXIT;
        break;
    }
}

/* One instruction for a translated block (an instruction it leaves to the
 * interpreter): as the run loop runs it, with an abort point of its own so
 * a fault or halt never unwinds through translated code. */
void m68k_jit_step(m68k_cpu *cpu) {
    jmp_buf here;
    jmp_buf *outer = cpu->abortp;
    cpu->abortp = &here;
    if (setjmp(here) == 0) {
        cpu->ipc = cpu->pc;
        cpu->undo_n = 0;
        if (M68K_UNLIKELY(cpu->before != NULL)) {
            m68k_before *b = cpu->before;
            memcpy(b->r, cpu->r, sizeof b->r);
            b->fn = cpu->fn; b->fz = cpu->fz; b->fv = cpu->fv; b->fc = cpu->fc; b->fx = cpu->fx;
        }
        const uint32_t op = fetch16(cpu);
        cpu->op = (uint16_t)op;
        cpu->table[op](cpu, op);
        m68k_retire(cpu, op);
    } else {
        handle_abort(cpu);
    }
    cpu->abortp = outer;
}

m68k_status m68k_run000(m68k_cpu *cpu, int64_t counts);

int m68k_step(m68k_cpu *cpu, jmp_buf *abort) {
    cpu->abortp = abort;
    cpu->ipc = cpu->pc;
    cpu->undo_n = 0;
    const uint32_t op = fetch16(cpu);
    cpu->op = (uint16_t)op;
    cpu->table[op](cpu, op);
    m68k_retire(cpu, op);
    return !(cpu->events | (cpu->pc & 1u));
}

void m68k_jit_host_written(m68k_cpu *cpu, const uint8_t *page, uint32_t offset, unsigned size) {
    if (!page) m68k_jit_flush(cpu);
    else m68k_jit_written(cpu, page, offset, size);
}

void m68k_abandon(m68k_cpu *cpu) {
    cpu->in_exception = 0;
    cpu->restart_writes = 0;
    cpu->locked = 0;
    cpu->wb_n = 0;
    cpu->undo_n = 0;
    cpu->cyc_extra = 0;
    cpu->events = 0;
    m68k_update_events(cpu);
}

m68k_status m68k_run(m68k_cpu *cpu, int64_t counts) {
    if (cpu->model == M68K_68000) return m68k_run000(cpu, counts);
    if (cpu->status == M68K_HALTED) return M68K_HALTED;
    cpu->status = M68K_OK;
    cpu->budget = counts;
    cpu->abortp = &cpu->abort;
    if (setjmp(cpu->abort) != 0) handle_abort(cpu);
    /* What is pending after an instruction (its trace, a writeback access
     * error, an odd PC, an interrupt) is taken before the run returns, so a
     * slice ends on a clean boundary. */
    for (;;) {
        if (M68K_UNLIKELY(cpu->events | (cpu->pc & 1u))) {
            if (between(cpu) || cpu->budget <= 0) break;
            if (cpu->events & EV_TRACE) arm_trace(cpu);
        } else if (cpu->budget <= 0) {
            break;
        }
        /* Translated blocks run while nothing is pending (no trace armed,
         * no interrupt due); the interpreter takes what they leave. */
        if (cpu->jit && !cpu->events && (m68k_jit_exec(cpu) || cpu->budget <= 0)) continue;
        cpu->ipc = cpu->pc;
        cpu->undo_n = 0;
        if (M68K_UNLIKELY(cpu->before != NULL)) {
            m68k_before *b = cpu->before;
            memcpy(b->r, cpu->r, sizeof b->r);
            b->fn = cpu->fn; b->fz = cpu->fz; b->fv = cpu->fv; b->fc = cpu->fc; b->fx = cpu->fx;
        }
        const uint32_t op = fetch16(cpu);
        cpu->op = (uint16_t)op;
        cpu->table[op](cpu, op);
        m68k_retire(cpu, op);
    }
    if (cpu->status == M68K_OK && (cpu->events & EV_EXIT)) cpu->events &= ~(uint32_t)EV_EXIT;
    /* A logged iteration of an idle loop ends with the slice: the machine's
     * devices act between slices. */
    if (cpu->wlog) m68k_jit_wlog_end(cpu);
    return cpu->status;
}

/* --- reset --------------------------------------------------------------- */

void m68k_reset(m68k_cpu *cpu) {
    cpu->events = 0;
    cpu->status = M68K_OK;
    cpu->wb_n = 0;
    cpu->in_exception = 0;
    cpu->trace_pending = 0;
    cpu->vbr = 0;
    cpu->cacr = 0;
    /* Reset clears the E bits of TCR and the TTRs; P and the ATCs stay
     * (MC68040 UM 3.6.1). The 68030 clears TC E and the TTx E bits
     * (MC68030 UM 9.7). */
    m68k_mmu_tlb_flush(cpu);
    cpu->tc &= ~0x8000u;
    cpu->itt0 &= ~0x8000u;
    cpu->itt1 &= ~0x8000u;
    cpu->dtt0 &= ~0x8000u;
    cpu->dtt1 &= ~0x8000u;
    cpu->mmu.tc30 &= ~0x80000000u;
    cpu->mmu.tt30[0] &= ~0x8000u;
    cpu->mmu.tt30[1] &= ~0x8000u;
    cpu->s = 1;
    cpu->m = 0;
    cpu->sr_hi = 0x2700;
    ccr_set(cpu, 0);
    m68k_mmu_changed(cpu);
    m68k_fpu_reset(cpu);
    uint32_t sp = 0, pc = 0;
    if (access_read(cpu, 0, 4, M68K_FC_SUPER_PROGRAM, &sp) || access_read(cpu, 4, 4, M68K_FC_SUPER_PROGRAM, &pc)) {
        cpu->status = M68K_HALTED;
        return;
    }
    A(7) = sp;
    cpu->isp = sp;
    cpu->pc = pc;
    m68k_update_events(cpu);
}

/* --- indexed and memory-indirect addressing (PRM 2.2.6-2.2.13, 2.1) ------ */

uint32_t m68k_ea_index(m68k_cpu *cpu, uint32_t base, int pc_relative) {
    const uint32_t at = cpu->pc;
    const uint32_t ext = fetch16(cpu);
    if (ext & 0x0700u) cpu->ea_ext020 = 1;
    uint32_t xn = cpu->r[ext >> 12];
    if (!(ext & 0x0800u)) xn = (uint32_t)(int32_t)(int16_t)xn;
    xn <<= (ext >> 9) & 3u;                    /* scale 1, 2, 4, 8 */
    if (!(ext & 0x0100u)) return base + xn + (uint32_t)(int32_t)(int8_t)ext;   /* brief format */
    /* Full format: BS (7), IS (6), BD SIZE (5-4), I/IS (2-0); bit 3 is 0. */
    const unsigned bdsize = (ext >> 4) & 3u, iis = ext & 7u;
    if (bdsize == 0 || (ext & 0x0008u)) m68k_abort(cpu, ABORT_ILLEGAL);
    if ((ext & 0x0040u) ? iis > 3 : iis == 4) m68k_abort(cpu, ABORT_ILLEGAL);
    /* Its time over the brief format's the table holds (m68k_timing.c). */
    if (!cpu->tm->unit) cpu->cyc_extra += m68k_timing_full(cpu, cpu->op, m68k_timing_slot(cpu, at), (uint16_t)ext, pc_relative);
    uint32_t bd = 0;
    if (bdsize == 2) bd = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    else if (bdsize == 3) bd = fetch32(cpu);
    if (ext & 0x0080u) base = 0;               /* base suppressed */
    if (ext & 0x0040u) xn = 0;                 /* index suppressed */
    if (iis == 0) return base + bd + xn;       /* no memory indirection */
    uint32_t od = 0;
    if ((iis & 3u) == 2) od = (uint32_t)(int32_t)(int16_t)fetch16(cpu);
    else if ((iis & 3u) == 3) od = fetch32(cpu);
    const unsigned fc = pc_relative ? cpu->fc_prog : cpu->fc_data;
    (void)fc;
    if (iis & 4u) return rd32(cpu, base + bd) + xn + od;   /* postindexed */
    return rd32(cpu, base + bd + xn) + od;                 /* preindexed (or no index) */
}

/* --- control registers (PRM 4, MOVEC; MC68040 UM sections 3 and 4) ------- */

/* CACR bits that read back: 68020 E, F; 68030 EI FI IBE ED FD DBE WA (the
 * clear bits CI, CEI, CD, CED are write-only); 68040 DE, IE. */
static uint32_t cacr_mask(m68k_model model) {
    switch (model) {
    case M68K_68020: return 0x00000003u;
    case M68K_68030: return 0x00003313u;
    default: return 0x80008000u;
    }
}

int m68k_movec_read(m68k_cpu *cpu, unsigned code, uint32_t *v) {
    const int e40 = is_040(cpu);
    switch (code) {
    case 0x000: *v = cpu->sfc; return 1;
    case 0x001: *v = cpu->dfc; return 1;
    case 0x002: *v = cpu->cacr; return 1;
    case 0x800: *v = m68k_get_usp(cpu); return 1;
    case 0x801: *v = cpu->vbr; return 1;
    case 0x803: *v = m68k_get_msp(cpu); return 1;
    case 0x804: *v = m68k_get_isp(cpu); return 1;
    case 0x802: if (e40) return 0; *v = cpu->caar; return 1;
    case 0x003: if (!e40) return 0; *v = cpu->tc; return 1;
    case 0x004: if (!e40) return 0; *v = cpu->itt0; return 1;
    case 0x005: if (!e40) return 0; *v = cpu->itt1; return 1;
    case 0x006: if (!e40) return 0; *v = cpu->dtt0; return 1;
    case 0x007: if (!e40) return 0; *v = cpu->dtt1; return 1;
    case 0x805: if (!e40) return 0; *v = cpu->mmusr; return 1;
    case 0x806: if (!e40) return 0; *v = cpu->urp; return 1;
    case 0x807: if (!e40) return 0; *v = cpu->srp; return 1;
    default: return 0;
    }
}

int m68k_movec_write(m68k_cpu *cpu, unsigned code, uint32_t v) {
    const int e40 = is_040(cpu);
    switch (code) {
    case 0x000: cpu->sfc = v & 7u; return 1;
    case 0x001: cpu->dfc = v & 7u; return 1;
    case 0x002: cpu->cacr = v & cacr_mask(cpu->model); return 1;
    case 0x800: m68k_set_usp(cpu, v); return 1;
    case 0x801: cpu->vbr = v; return 1;
    case 0x803: m68k_set_msp(cpu, v); return 1;
    case 0x804: m68k_set_isp(cpu, v); return 1;
    case 0x802: if (e40) return 0; cpu->caar = v; return 1;
    /* 68040 MMU registers, implemented bits (MC68040 UM section 3): TC E P;
     * ITTn/DTTn base, mask, E, S field, U1 U0, CM, W; MMUSR PA B G U1 U0 S
     * CM M W T R; URP/SRP 512-byte aligned. */
    case 0x003: case 0x004: case 0x005: case 0x006: case 0x007: case 0x806: case 0x807:
        if (!e40) return 0;
        /* The soft TLB empties before the translation changes; a new page
         * size also invalidates the ATCs, whose tags it changes (the manual
         * leaves that undefined and asks software to flush). Writes do not
         * flush the ATCs otherwise (MC68040 UM 3.7.4). */
        m68k_mmu_tlb_flush(cpu);
        switch (code) {
        case 0x003:
            if ((cpu->tc ^ v) & 0x4000u) memset(cpu->mmu.atc, 0, sizeof cpu->mmu.atc);
            cpu->tc = v & 0x0000C000u;
            break;
        case 0x004: cpu->itt0 = v & 0xFFFFE364u; break;
        case 0x005: cpu->itt1 = v & 0xFFFFE364u; break;
        case 0x006: cpu->dtt0 = v & 0xFFFFE364u; break;
        case 0x007: cpu->dtt1 = v & 0xFFFFE364u; break;
        case 0x806: cpu->urp = v & 0xFFFFFE00u; break;
        default: cpu->srp = v & 0xFFFFFE00u; break;
        }
        m68k_mmu_changed(cpu);
        return 1;
    case 0x805: if (!e40) return 0; cpu->mmusr = v & 0xFFFFFFF7u; return 1;
    default: return 0;
    }
}

/* --- the instructions that live with the exception logic ----------------- */

/* Valid RTE frame formats and their sizes in bytes: 68040 $0, $1, $2, $3,
 * $4, $7 (MC68040 UM section 8); 68020/030 $0, $1, $2, $9, $A, $B (MC68030
 * UM section 8). Anything else is a format error. */
static unsigned frame_size(const m68k_cpu *cpu, unsigned format) {
    if (is_040(cpu)) {
        switch (format) {
        case 0x0: case 0x1: return 8;
        case 0x2: case 0x3: return 12;
        /* Format $4 exists only on the 68LC040; the 68040 "does not generate
         * or recognize" it (MC68040 UM 8.4.5, A.5.2). */
        case 0x4: return cpu->model == M68K_68LC040 ? 16 : 0;
        case 0x7: return 60;
        default: return 0;
        }
    }
    switch (format) {
    case 0x0: case 0x1: return 8;
    case 0x2: return 12;
    case 0x9: return 20;
    case 0xA: return 32;
    case 0xB: return 92;
    default: return 0;
    }
}

void m68k_op_rte(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIVILEGED();
    for (;;) {
        const uint32_t sp = A(7);
        const uint32_t sr = rd16(cpu, sp);
        const uint32_t pc = rd32(cpu, sp + 2);
        const uint32_t fv = rd16(cpu, sp + 6);
        const unsigned format = fv >> 12;
        const unsigned size = frame_size(cpu, format);
        if (!size) {
            m68k_exception(cpu, VEC_FORMAT, cpu->ipc);
            return;
        }
        cpu->cyc_extra += (int32_t)cpu->tm->rte[format];   /* by frame (m68k_timing.c) */
        uint32_t ssw = 0;
        if (format == 7) ssw = rd16(cpu, sp + 12);
        if (format == 0xA || format == 0xB) {
            /* 68020/030 bus fault frames (MC68030 UM 8.2.1, 8.4.4): DF set
             * with RW clear asks the processor to rerun the faulted write
             * from the data output buffer, at the fault address with the
             * SSW's size and function code. A refused rerun takes a new bus
             * error over this frame, which stays intact. Read faults restart
             * their instruction from the stacked PC instead (m68k.h); with
             * DF cleared by the handler, the restarted instruction's read
             * takes the data input buffer at +$2C (m68k.h, dib_valid). */
            ssw = rd16(cpu, sp + 0x0A);
            cpu->dib_valid = 0;
            if (format == 0xB && !(ssw & 0x0100u) && (ssw & 0x0040u) && (ssw & 3u) != 2u) {
                const unsigned code = (ssw >> 4) & 3u;
                cpu->dib_address = rd32(cpu, sp + 0x10);
                cpu->dib_value = rd32(cpu, sp + 0x2C);
                cpu->dib_size = (uint8_t)(code == 1 ? 1u : code == 2 ? 2u : code == 3 ? 3u : 4u);
                cpu->dib_count = cpu->instructions + 1u;
                cpu->dib_valid = cpu->dib_size != 3;
            }
            if ((ssw & 0x0100u) && !(ssw & 0x0040u)) {
                const uint32_t fa = rd32(cpu, sp + 0x10), dob = rd32(cpu, sp + 0x18);
                const unsigned code = (ssw >> 4) & 3u, wsize = code == 1 ? 1u : code == 2 ? 2u : code == 3 ? 3u : 4u;
                cpu->restart_writes = 1;
                if (wsize == 3) {
                    m68k_slow_write(cpu, fa, 2, ssw & 7u, (dob >> 8) & 0xFFFFu);
                    m68k_slow_write(cpu, fa + 2, 1, ssw & 7u, dob & 0xFFu);
                } else {
                    m68k_slow_write(cpu, fa, wsize, ssw & 7u, dob & sz_mask(wsize));
                }
                cpu->restart_writes = 0;
            }
        }
        A(7) = sp + size;
        write_sr(cpu, (uint16_t)sr);
        if (format == 1) continue;              /* throwaway: the next frame is on the new stack */
        jump(cpu, pc);
        if (format == 7 && (ssw & 0x2000u) && (cpu->sr_hi & (SR_T1 | SR_T0))) {
            /* CT: the trace of the faulted instruction is still due. */
            cpu->trace_pending = 1;
            cpu->events |= EV_TRACE_NOW;
        }
        return;
    }
}

void m68k_op_stop(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIVILEGED();
    const uint32_t sr = fetch16(cpu);
    write_sr(cpu, (uint16_t)sr);
    cpu->flow = 1;
    /* A trace pending on STOP is taken instead of stopping (PRM, STOP). */
    if (!(cpu->events & EV_TRACE_NOW)) cpu->events |= EV_STOP;
}

void m68k_op_reset(m68k_cpu *cpu, uint32_t op) {
    (void)op;
    PRIVILEGED();
    if (cpu->bus.reset) cpu->bus.reset(cpu->bus.ctx);
}

void m68k_op_movec(m68k_cpu *cpu, uint32_t op) {
    PRIVILEGED();
    const uint32_t ext = fetch16(cpu);
    const unsigned code = ext & 0xFFFu, reg = ext >> 12;
    if (op & 1u) {
        if (!m68k_movec_write(cpu, code, cpu->r[reg])) m68k_exception_illegal(cpu);
        else if (code <= 2) cpu->cyc_extra += m68k_timing_query(cpu, (uint16_t)op, M68K_TQ_CRB);  /* SFC, DFC, CACR */
    } else {
        uint32_t v;
        if (!m68k_movec_read(cpu, code, &v)) m68k_exception_illegal(cpu);
        else cpu->r[reg] = v;
    }
}

/* SR-writing instructions share this: privilege checked by the caller. */
void m68k_write_sr_insn(m68k_cpu *cpu, uint32_t sr) {
    write_sr(cpu, (uint16_t)sr);
    cpu->flow = 1;
}
