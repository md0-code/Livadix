/* The MC68851 PMMU beside a 68020: the subset Macintosh software uses.
 *
 * Source: MC68851 Paged Memory Management Unit User's Manual (second
 * edition, M68851UM/AD REV 2; sections cited below) and the M68000 Family
 * Programmer's Reference Manual's MC68851 instruction pages, the MC68020
 * User's Manual section 8 (coprocessor interface: pre- and post-instruction
 * exceptions, the format $0 and $2 frames, F-line when no coprocessor
 * answers) and 6.5 (stack frames).
 *
 * The translation tables are the 68030's (MC68851 UM 5.1: the same root
 * pointers, TC fields, short and long table, page, indirect and invalid
 * descriptors, early termination, limits, function-code lookup), so the
 * table search is mmu030.c's; this file adds what the 68851 does
 * differently:
 *   - its ATC: 64 entries, fully associative, tagged with the logical page,
 *     the four-bit function code and the task alias unless the entry is
 *     shared globally (SG); a locked entry (L) is not replaced (5.2);
 *   - the root pointer table: eight recent CRP values, each a task alias;
 *     loading CRP selects or replaces one and flushes the replaced alias's
 *     entries; PCSR shows the alias and the flush (5.3, 6.1.2);
 *   - PSR's G and C bits (6.1.8); SRP and DRP loads flush the supervisor
 *     and DMA entries (6.1.1);
 *   - a read-modify-write cycle with no ATC entry, or one whose M bit is
 *     clear, is refused without a table search (6.3.1.7);
 *   - the instructions through the coprocessor interface, cpID 0: PMOVE
 *     (TC, CRP, SRP, DRP, PSR, PCSR; any addressing mode, Dn and An for
 *     the registers of 32 bits or less), PFLUSHA, PFLUSH(S) by function
 *     code and address with a four-bit mask, PFLUSHR, PLOAD, PTEST, PBcc,
 *     PDBcc, PScc, PTRAPcc, PSAVE and PRESTORE (appendix A); every one
 *     privileged (6.2); the post-instruction exceptions (configuration
 *     error 56, illegal operation 57) stack format $2 with the next
 *     instruction's PC (6.3.2, MC68020 UM 6.5.3).
 * Not modelled (line F, the F-line emulator exception the 68851 itself
 * returns for commands it does not recognise, 6.3.2.1): the access-level
 * mechanism (PMOVE to or from CAL, VAL, SCC and AC, PVALID; RAL and WAL
 * fields are ignored, as with AC's ALC field zero, its reset value, 6.1.7)
 * and the breakpoint registers (BADx, BACx). Neither Apple's ROMs nor the
 * System's MMU code use them. The ATC's pseudo-LRU
 * replacement is modelled, as the 68030's, by a rotating pointer. */
#include "m68k_internal.h"

#define TC_E 0x80000000u
#define VEC_MMU_CONFIG 56u          /* MC68851 UM 6.3.2.3 */
#define VEC_MMU_ILLEGAL 57u         /* MC68851 UM 6.3.2.4 */

/* ATC entry bits beyond the shared M68K_ATC_* ones (MC68851 UM 5.2.1). */
#define ST_CI 0x0040u
#define ST_SG 0x1000u
#define ST_LOCK 0x2000u
#define ST_GATE 0x4000u

/* PSR (MC68851 UM 6.1.8); B, L, S, W, I, M at the 68030 MMUSR's places. */
#define PS_B 0x8000u
#define PS_I 0x0400u
#define PS_W 0x0800u
#define PS_M 0x0200u
#define PS_G 0x0100u
#define PS_C 0x0080u

/* Root pointer bits kept (MC68851 UM 6.1.1, Figure 6-1): L/U, LIMIT, SG,
 * DT in the upper long word; the table address and four unused bits the
 * system may use in the lower. */
#define RP_HI 0xFFFF0203u

int m68k_mmu851_active(const m68k_cpu *cpu) { return cpu->pmmu.fitted && (cpu->mmu.tc30 & TC_E); }

static unsigned ps_of(const m68k_cpu *cpu) { return (cpu->mmu.tc30 >> 20) & 15u; }

/* --- the ATC ------------------------------------------------------------- */

/* A match: the logical page and the function code, and the current task
 * alias unless the entry is shared globally (MC68851 UM 5.2.1.1). */
static m68k_atc_entry *find(m68k_cpu *cpu, uint32_t la, unsigned fc) {
    const uint32_t tag = la >> ps_of(cpu);
    for (unsigned i = 0; i < 64; i++) {
        m68k_atc_entry *e = &cpu->pmmu.atc[i];
        if ((e->st & M68K_ATC_V) && e->tag == tag && e->fc == fc && ((e->st & ST_SG) || e->pad == cpu->pmmu.ta)) return e;
    }
    return NULL;
}

static void drop(m68k_cpu *cpu, m68k_atc_entry *e) {
    const unsigned ps = ps_of(cpu);
    m68k_mmu_tlb_drop(cpu, e, 0, ps);
    m68k_mmu_tlb_drop(cpu, e, 1, ps);
    e->st = 0;
}

static unsigned locked(const m68k_cpu *cpu) {
    unsigned n = 0;
    for (unsigned i = 0; i < 64; i++)
        if ((cpu->pmmu.atc[i].st & (M68K_ATC_V | ST_LOCK)) == (M68K_ATC_V | ST_LOCK)) n++;
    return n;
}

/* An invalid entry, else the next unlocked one (MC68851 UM 5.2.1.3). */
static m68k_atc_entry *victim(m68k_cpu *cpu) {
    for (unsigned i = 0; i < 64; i++)
        if (!(cpu->pmmu.atc[i].st & M68K_ATC_V)) return &cpu->pmmu.atc[i];
    for (unsigned n = 0; n < 64; n++) {
        m68k_atc_entry *e = &cpu->pmmu.atc[cpu->pmmu.next];
        cpu->pmmu.next = (uint8_t)((cpu->pmmu.next + 1u) & 63u);
        if (!(e->st & ST_LOCK)) {
            drop(cpu, e);
            return e;
        }
    }
    m68k_atc_entry *e = &cpu->pmmu.atc[cpu->pmmu.next];
    drop(cpu, e);
    return e;
}

/* An entry from a table search; its L bit only while fewer than 63 entries
 * are locked (MC68851 UM 5.1.5.3.9, 5.2.1.2). */
static void load_entry(m68k_cpu *cpu, m68k_atc_entry *e, uint32_t la, unsigned fc, const m68k_walk *w) {
    e->tag = la >> ps_of(cpu);
    e->fc = (uint8_t)fc;
    e->pad = cpu->pmmu.ta;
    e->pa = w->pa;
    uint16_t st = M68K_ATC_V;
    st |= (w->status & 0xE400u) ? M68K_ATC_B : M68K_ATC_R;   /* B, L, S or I: no translation */
    if (w->status & PS_W) st |= M68K_ATC_W;
    if (w->status & PS_M) st |= M68K_ATC_M;
    if (w->ci) st |= ST_CI;
    if (w->sg) st |= ST_SG;
    if (w->g) st |= ST_GATE;
    if (w->lock && locked(cpu) < 63) st |= ST_LOCK;
    e->st = st;
}

static void flush_all(m68k_cpu *cpu) {
    for (unsigned i = 0; i < 64; i++)
        if (cpu->pmmu.atc[i].st & M68K_ATC_V) drop(cpu, &cpu->pmmu.atc[i]);
}

/* The entries of task alias `ta` that are not shared globally. */
static void flush_alias(m68k_cpu *cpu, unsigned ta) {
    for (unsigned i = 0; i < 64; i++) {
        m68k_atc_entry *e = &cpu->pmmu.atc[i];
        if ((e->st & M68K_ATC_V) && !(e->st & ST_SG) && e->pad == ta) drop(cpu, e);
    }
}

int m68k_mmu851_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill) {
    (void)ifetch;
    fc &= 7u;
    *pa = la;
    *fill = 0;
    if (fc == 7) return 0;              /* CPU space: not translated (MC68851 UM 5.2.2.1) */
    if (!m68k_mmu851_active(cpu)) {
        *fill = 0x80u | 3u;
        return 0;
    }
    m68k_atc_entry *e = find(cpu, la, fc);
    if (cpu->locked) {
        /* A read-modify-write cycle searches nothing: refused without an
         * entry, or with one whose M bit is clear (MC68851 UM 6.3.1.7). */
        if (!e || !(e->st & M68K_ATC_M)) return 1;
    } else if (!e || (write && (e->st & M68K_ATC_R) && !(e->st & (M68K_ATC_M | M68K_ATC_W)))) {
        /* A miss, or the first write through an entry with M clear: the
         * search sets the descriptor's M bit (MC68851 UM 5.2.1.2). */
        m68k_walk w;
        if (e) drop(cpu, e);
        e = victim(cpu);
        m68k_mmu030_search(cpu, la, fc, write, 1, 7, &w);
        load_entry(cpu, e, la, fc, &w);
    }
    if (e->st & M68K_ATC_B) return 1;
    if (write && (e->st & M68K_ATC_W)) return 1;
    const unsigned ps = ps_of(cpu);
    *pa = e->pa | (la & ((1u << ps) - 1u));
    if (ps >= M68K_PAGE_SHIFT) *fill = 1u | (((e->st & (M68K_ATC_M | M68K_ATC_W)) == M68K_ATC_M) ? 2u : 0u);
    return 0;
}

/* --- the registers ----------------------------------------------------- */

/* TC consistency (MC68851 UM 6.1.3, 6.1.3.4): PS bit 3 set, and PS + IS +
 * TIA + TIB + TIC + TID = 32. */
static int tc_valid(uint32_t tc) {
    const unsigned ps = (tc >> 20) & 15u;
    if (!(ps & 8u)) return 0;
    return ps + ((tc >> 16) & 15u) + ((tc >> 12) & 15u) + ((tc >> 8) & 15u) + ((tc >> 4) & 15u) + (tc & 15u) == 32u;
}

/* A write to TC; 1 when the consistency check failed (TC is then loaded
 * with E clear, 6.1.3). A value with E clear flushes the ATC (6.1.3); so
 * does a new value while E stays set, whose tags would no longer be the
 * same pages (the manual requires E clear before a change). */
static int write_tc(m68k_cpu *cpu, uint32_t v) {
    m68k_mmu *m = &cpu->mmu;
    v &= 0x83FFFFFFu;
    const uint32_t old = m->tc30;
    int bad = 0;
    if ((v & TC_E) && !(old & TC_E) && !tc_valid(v)) {
        v &= ~TC_E;
        bad = 1;
    }
    if (!(v & TC_E) || v != old) {
        m68k_mmu_tlb_flush(cpu);
        flush_all(cpu);
    }
    m->tc30 = v;
    m68k_mmu_changed(cpu);
    return bad;
}

/* CRP: the root pointer table (MC68851 UM 5.3.1). The same value selects
 * its task alias; the same address and L/U with another limit or DT
 * replaces that entry; otherwise an invalid entry, else the next by the
 * replacement pointer. A replaced entry's alias loses its ATC entries
 * (shared ones stay) and PCSR's F bit says so. */
static void load_crp(m68k_cpu *cpu, uint32_t hi, uint32_t lo) {
    m68k_mmu *m = &cpu->mmu;
    hi &= RP_HI;
    m->crp[0] = hi;
    m->crp[1] = lo;
    int at = -1, replace = 0;
    for (unsigned i = 0; i < 8 && at < 0; i++)
        if ((cpu->pmmu.rpt_valid >> i & 1u) && cpu->pmmu.rpt[i][0] == hi && ((cpu->pmmu.rpt[i][1] ^ lo) & 0xFFFFFFF0u) == 0) at = (int)i;
    if (at >= 0) {
        cpu->pmmu.pcsr_f = 0;
    } else {
        for (unsigned i = 0; i < 8 && at < 0; i++)
            if ((cpu->pmmu.rpt_valid >> i & 1u) && ((cpu->pmmu.rpt[i][1] ^ lo) & 0xFFFFFFF0u) == 0 && ((cpu->pmmu.rpt[i][0] ^ hi) & 0x80000000u) == 0) {
                at = (int)i;
                replace = 1;
            }
        for (unsigned i = 0; i < 8 && at < 0; i++)
            if (!(cpu->pmmu.rpt_valid >> i & 1u)) at = (int)i;
        if (at < 0) {
            at = cpu->pmmu.rpt_next;
            cpu->pmmu.rpt_next = (uint8_t)((cpu->pmmu.rpt_next + 1u) & 7u);
            replace = 1;
        }
        cpu->pmmu.rpt[at][0] = hi;
        cpu->pmmu.rpt[at][1] = lo;
        cpu->pmmu.rpt_valid |= (uint8_t)(1u << at);
        cpu->pmmu.pcsr_f = 1;
        if (replace) flush_alias(cpu, (unsigned)at);
    }
    cpu->pmmu.ta = (uint8_t)at;
    /* Entries of the other aliases stop matching. */
    m68k_mmu_tlb_flush(cpu);
    m68k_mmu_changed(cpu);
}

/* SRP: every supervisor entry goes, shared ones too (MC68851 UM 6.1.1). */
static void load_srp(m68k_cpu *cpu, uint32_t hi, uint32_t lo) {
    cpu->mmu.srp30[0] = hi & RP_HI;
    cpu->mmu.srp30[1] = lo;
    for (unsigned i = 0; i < 64; i++) {
        m68k_atc_entry *e = &cpu->pmmu.atc[i];
        if ((e->st & M68K_ATC_V) && (e->fc & 4u)) drop(cpu, e);
    }
    m68k_mmu_changed(cpu);
}

static uint16_t pcsr(const m68k_cpu *cpu) {
    return (uint16_t)((cpu->pmmu.pcsr_f ? 0x8000u : 0) | (locked(cpu) >= 63 ? 0x4000u : 0) | (cpu->pmmu.ta & 7u));
}

void m68k_set_pmmu(m68k_cpu *cpu, int fitted) {
    if (cpu->model != M68K_68020) return;
    cpu->pmmu.fitted = (uint8_t)(fitted != 0);
    cpu->mmu.tc30 &= ~TC_E;
    m68k_pflusha(cpu);
    m68k_mmu_changed(cpu);
}

/* --- the instructions ---------------------------------------------------- */

static unsigned ea_cls(unsigned mode, unsigned reg) {
    if (mode < 7) return 1u << mode;
    if (reg <= 4) return 1u << (7 + reg);
    return 0;
}

/* The FC field of PFLUSH, PLOAD and PTEST (appendix A): 1DDDD four bits
 * immediate, 01RRR the low four bits of Dn, 00000 SFC, 00001 DFC; -1 for
 * the other encodings. */
static int fc_operand(m68k_cpu *cpu, unsigned f) {
    if (f & 0x10u) return (int)(f & 15u);
    if ((f & 0x18u) == 0x08u) return (int)(D(f & 7u) & 15u);
    if (f == 0) return (int)(cpu->sfc & 7u);
    if (f == 1) return (int)(cpu->dfc & 7u);
    return -1;
}

/* A post-instruction exception (MC68851 UM 6.3.2.3-6.3.2.5): format $2,
 * the next instruction's PC and this one's address (MC68020 UM 6.5.3). */
static void post_exception(m68k_cpu *cpu, unsigned vector) { m68k_exception_fmt2(cpu, vector, cpu->pc, cpu->ipc); }

/* PMOVE's operand: `size` 2, 4 or 8 bytes, from any mode (immediate data
 * from the instruction stream; the low word of Dn or An for a word). */
static void read_operand(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned size, uint32_t v[2]) {
    if (mode == 0 || mode == 1) {
        v[0] = cpu->r[mode * 8 + reg] & (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
        return;
    }
    if (mode == 7 && reg == 4) {
        v[0] = size == 2 ? fetch16(cpu) : fetch32(cpu);
        if (size == 8) v[1] = fetch32(cpu);
        return;
    }
    const uint32_t a = ea_addr(cpu, mode, reg, size);
    v[0] = size == 2 ? rd16(cpu, a) : rd32(cpu, a);
    if (size == 8) v[1] = rd32(cpu, a + 4);
}
/* A word into An is sign-extended, as MOVEA does. */
static void write_operand(m68k_cpu *cpu, unsigned mode, unsigned reg, unsigned size, const uint32_t v[2]) {
    if (mode == 0) {
        set_dn(cpu, reg, v[0], size == 2 ? 2u : 4u);
        return;
    }
    if (mode == 1) {
        A(reg) = size == 2 ? sz_sext(v[0], 2) : v[0];
        return;
    }
    const uint32_t a = ea_addr(cpu, mode, reg, size);
    if (size == 2) {
        wr16(cpu, a, v[0]);
        return;
    }
    wr32(cpu, a, v[0]);
    if (size == 8) wr32(cpu, a + 4, v[1]);
}

/* PMOVE (appendix A, formats 1 and 3). 0 when the encoding is not one the
 * model executes (line F). */
static int op_pmove(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op), cls = ea_cls(mode, reg);
    const unsigned preg = (ext >> 10) & 7u, tomem = (ext >> 9) & 1u;
    m68k_mmu *m = &cpu->mmu;
    unsigned size;
    if ((ext >> 13) == 2) {
        /* Format 1: TC, DRP, SRP, CRP (CAL, VAL, SCC, AC: not modelled). */
        if ((ext & 0x01FFu) || preg >= 4) return 0;
        size = preg == 0 ? 4u : 8u;
    } else {
        /* Format 3: PSR, and PCSR to memory only (format 2, BADx/BACx:
         * not modelled). */
        if ((ext & 0x01FFu) || preg > 1 || (preg == 1 && !tomem)) return 0;
        size = 2;
    }
    unsigned allowed = tomem ? EA_ALT : EA_ALL;
    if (size == 8) allowed &= ~(EA_DN | EA_AN);
    if (!(allowed & cls)) return 0;
    if (!cpu->s) {
        m68k_exception_privilege(cpu);
        return 1;
    }
    uint32_t v[2] = {0, 0};
    if (tomem) {
        if (size == 2) v[0] = preg == 0 ? m->mmusr30 : pcsr(cpu);
        else if (preg == 0) v[0] = m->tc30;
        else {
            const uint32_t *r = preg == 1 ? cpu->pmmu.drp : preg == 2 ? m->srp30 : m->crp;
            v[0] = r[0];
            v[1] = r[1];
        }
        write_operand(cpu, mode, reg, size, v);
        return 1;
    }
    read_operand(cpu, mode, reg, size, v);
    if (size == 2) {
        m->mmusr30 = (uint16_t)v[0];
        return 1;
    }
    if (preg == 0) {
        if (write_tc(cpu, v[0])) post_exception(cpu, VEC_MMU_CONFIG);
        return 1;
    }
    /* A root pointer with DT invalid is refused and not loaded (6.1.1.4,
     * 6.3.2.3). */
    if ((v[0] & 3u) == 0) {
        post_exception(cpu, VEC_MMU_CONFIG);
        return 1;
    }
    if (preg == 3) load_crp(cpu, v[0], v[1]);
    else if (preg == 2) load_srp(cpu, v[0], v[1]);
    else {
        /* DRP translates FC3 = 1 accesses, which the 68020 never makes. */
        cpu->pmmu.drp[0] = v[0] & RP_HI;
        cpu->pmmu.drp[1] = v[1];
    }
    return 1;
}

/* PFLUSHA, PFLUSH(S), PLOAD (ext 001xxx; PVALID, mode 010, not modelled). */
static int op_pflush(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op), cls = ea_cls(mode, reg);
    const unsigned fmode = (ext >> 10) & 7u, mask = (ext >> 5) & 15u;
    if (fmode == 1) {
        if (ext != 0x2400u) return 0;
        if (!cpu->s) { m68k_exception_privilege(cpu); return 1; }
        m68k_mmu_tlb_flush(cpu);
        flush_all(cpu);
        return 1;
    }
    const int fc = fc_operand(cpu, ext & 0x1Fu);
    if (fc < 0) return 0;
    if (fmode == 0) {
        /* PLOADR (R/W 1) / PLOADW FC,<ea>: control alterable. */
        if ((ext & 0x01E0u) || !(cls & EA_CALT)) return 0;
        if (!cpu->s) { m68k_exception_privilege(cpu); return 1; }
        const uint32_t la = ea_addr(cpu, mode, reg, 4);
        if (!m68k_mmu851_active(cpu)) {
            post_exception(cpu, VEC_MMU_ILLEGAL);
            return 1;
        }
        m68k_atc_entry *e = find(cpu, la, (unsigned)fc);
        if (e) drop(cpu, e);
        m68k_walk w;
        m68k_mmu030_search(cpu, la, (unsigned)fc, !(ext & 0x0200u), 1, 7, &w);
        load_entry(cpu, victim(cpu), la, (unsigned)fc, &w);
        return 1;
    }
    if (fmode < 4) return 0;
    /* 100/101 by function code, 110/111 and address; the odd modes take
     * shared entries too (PFLUSHS). Others: the current alias only. */
    const int with_ea = fmode >= 6, shared = fmode & 1u;
    if (with_ea && !(cls & EA_CALT)) return 0;
    if (!cpu->s) { m68k_exception_privilege(cpu); return 1; }
    const uint32_t tag = with_ea ? ea_addr(cpu, mode, reg, 4) >> ps_of(cpu) : 0;
    for (unsigned i = 0; i < 64; i++) {
        m68k_atc_entry *e = &cpu->pmmu.atc[i];
        if (!(e->st & M68K_ATC_V) || ((e->fc ^ (unsigned)fc) & mask)) continue;
        if ((e->st & ST_SG) ? !shared : e->pad != cpu->pmmu.ta) continue;
        if (with_ea && e->tag != tag) continue;
        drop(cpu, e);
    }
    return 1;
}

/* PFLUSHR <ea>: a previous CRP value, from memory or immediate. */
static int op_pflushr(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (!(ea_cls(mode, reg) & EA_MEM)) return 0;
    if (!cpu->s) { m68k_exception_privilege(cpu); return 1; }
    uint32_t v[2];
    read_operand(cpu, mode, reg, 8, v);
    for (unsigned i = 0; i < 8; i++) {
        if (!(cpu->pmmu.rpt_valid >> i & 1u) || cpu->pmmu.rpt[i][0] != (v[0] & RP_HI) || ((cpu->pmmu.rpt[i][1] ^ v[1]) & 0xFFFFFFF0u)) continue;
        cpu->pmmu.rpt_valid &= (uint8_t)~(1u << i);
        flush_alias(cpu, i);
        break;
    }
    return 1;
}

/* PTEST: level 0 asks the ATC, 1-7 search that deep without updating U or
 * M; An gets the last descriptor's address (appendix A, 6.1.8). */
static int op_ptest(m68k_cpu *cpu, uint32_t op, uint32_t ext) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const unsigned level = (ext >> 10) & 7u, areg = (ext >> 8) & 1u, an = (ext >> 5) & 7u;
    const int fc = fc_operand(cpu, ext & 0x1Fu);
    if (fc < 0 || !(ea_cls(mode, reg) & EA_CALT) || (level == 0 && (ext & 0x01E0u))) return 0;
    if (!cpu->s) { m68k_exception_privilege(cpu); return 1; }
    const uint32_t la = ea_addr(cpu, mode, reg, 4);
    if (!m68k_mmu851_active(cpu)) {
        post_exception(cpu, VEC_MMU_ILLEGAL);
        return 1;
    }
    m68k_mmu *m = &cpu->mmu;
    if (level == 0) {
        unsigned sr = 0;
        const m68k_atc_entry *e = find(cpu, la, (unsigned)fc);
        if (!e) sr = PS_I;
        else {
            if (e->st & M68K_ATC_B) sr |= PS_B | PS_I;
            if (e->st & M68K_ATC_W) sr |= PS_W;
            if (e->st & M68K_ATC_M) sr |= PS_M;
            if (e->st & ST_GATE) sr |= PS_G;
            if (e->st & ST_SG) sr |= PS_C;
        }
        m->mmusr30 = (uint16_t)sr;
        return 1;
    }
    m68k_walk w;
    m68k_mmu030_search(cpu, la, (unsigned)fc, !(ext & 0x0200u), 0, level, &w);
    unsigned sr = w.status | (w.levels & 7u);
    if (!(w.status & PS_I)) sr |= (w.g ? PS_G : 0) | (w.sg ? PS_C : 0);
    m->mmusr30 = (uint16_t)sr;
    if (areg) A(an) = w.last;
    return 1;
}

/* The conditions on PSR (appendix A, PBcc): B, L, S, A, W, I, G, C set
 * (even codes) or clear (odd); -1 for codes above $0F. */
static int condition(const m68k_cpu *cpu, unsigned c) {
    static const uint16_t bit[8] = {0x8000u, 0x4000u, 0x2000u, 0x1000u, 0x0800u, 0x0400u, 0x0100u, 0x0080u};
    if (c > 15u) return -1;
    const int set = (cpu->mmu.mmusr30 & bit[c >> 1]) != 0;
    return (c & 1u) ? !set : set;
}

/* PSAVE and PRESTORE (appendix A; the state frames of MC68851 UM 6.2.7.3,
 * Figures 6-10 to 6-13, first edition): Livadix's 68851 is always idle
 * between instructions, so PSAVE writes the idle frame ($C020: 36 bytes:
 * PSR, SCC, CAL, VAL, SRP, CRP, three internal long words, written zero).
 * PRESTORE takes the reset frame (length 0: nothing restored) and the
 * idle, mid-coprocessor and breakpoint frames (PSR, SRP and CRP from
 * their places); any other format word is a format error. */
#define FRAME_IDLE 0xC020u
static void op_psave(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (!(ea_cls(mode, reg) & (EA_CALT | EA_PD))) { m68k_exception_line_f(cpu); return; }
    PRIVILEGED();
    const uint32_t a = ea_addr(cpu, mode, reg, mode == 4 ? 36u : 4u);
    const m68k_mmu *m = &cpu->mmu;
    const uint32_t frame[9] = {FRAME_IDLE << 16, (uint32_t)m->mmusr30 << 16, m->srp30[1], m->srp30[0], m->crp[1], m->crp[0], 0, 0, 0};
    cpu->restart_writes = 1;
    for (unsigned i = 0; i < 9; i++) wr32(cpu, a + 4u * i, frame[i]);
    cpu->restart_writes = 0;
}
static void op_prestore(m68k_cpu *cpu, uint32_t op) {
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    if (!(ea_cls(mode, reg) & (EA_CTRL | EA_PI))) { m68k_exception_line_f(cpu); return; }
    PRIVILEGED();
    const uint32_t a = mode == 3 ? A(reg) : ea_addr(cpu, mode, reg, 4);
    const uint32_t first = rd32(cpu, a), fmt = first >> 16, len = fmt & 0xFFu;
    unsigned psr_at = 0;
    if (len != 0) {
        if ((fmt >> 8) != 0xC0u || (len != 0x20u && len != 0x28u && len != 0x48u)) {
            m68k_exception_restart(cpu, VEC_FORMAT);
            return;
        }
        psr_at = len == 0x20u ? 0x04u : len == 0x28u ? 0x0Cu : 0x2Cu;
    }
    uint32_t v[5] = {0, 0, 0, 0, 0};
    if (len) for (unsigned i = 0; i < 5; i++) v[i] = rd32(cpu, a + psr_at + 4u * i);
    if (mode == 3) {
        undo_push(cpu, 8 + reg, A(reg));
        A(reg) = a + 4u + len;
    }
    if (!len) return;
    cpu->mmu.mmusr30 = (uint16_t)(v[0] >> 16);
    load_srp(cpu, v[2], v[1]);
    load_crp(cpu, v[4], v[3]);
}

void m68k_op_pmmu851(m68k_cpu *cpu, uint32_t op) {
    const unsigned type = (op >> 6) & 7u;
    if (!cpu->pmmu.fitted) {
        /* No coprocessor answers: line F (MC68020 UM 8.16.5); cpSAVE and
         * cpRESTORE are privileged before the coprocessor is asked. */
        if ((type == 4 || type == 5) && !cpu->s) m68k_exception_privilege(cpu);
        else m68k_exception_line_f(cpu);
        return;
    }
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    switch (type) {
    case 0: {                                   /* general: the command word decides */
        const uint32_t ext = fetch16(cpu);
        int done = 0;
        /* The MC68030's times for the same instructions (m68k_timing.c). */
        cpu->cyc_extra += m68k_timing_pmmu(cpu, op, m68k_pmmu_kind(ext));
        switch (ext >> 13) {
        case 1: done = op_pflush(cpu, op, ext); break;
        case 2: case 3: done = op_pmove(cpu, op, ext); break;
        case 4: done = op_ptest(cpu, op, ext); break;
        case 5: done = ext == 0xA000u && op_pflushr(cpu, op); break;
        default: break;
        }
        if (!done) m68k_exception_line_f(cpu);
        return;
    }
    case 1: {                                   /* PScc, PDBcc, PTRAPcc */
        const uint32_t ext = fetch16(cpu);
        const int c = (ext & 0xFFC0u) ? -1 : condition(cpu, ext & 0x3Fu);
        if (mode == 1) {                        /* PDBcc Dn,<label> */
            const uint32_t at = cpu->pc;        /* the displacement word (PRM, cpDBcc) */
            const uint32_t disp = sz_sext(fetch16(cpu), 2);
            if (c < 0) { m68k_exception_line_f(cpu); return; }
            PRIVILEGED();
            if (!c) {
                const uint32_t w = (D(reg) - 1u) & 0xFFFFu;
                D(reg) = (D(reg) & 0xFFFF0000u) | w;
                if (w != 0xFFFFu) jump(cpu, at + disp);
            }
            return;
        }
        if (mode == 7 && reg >= 2 && reg <= 4) {    /* PTRAPcc: a word, a long word or no operand */
            if (reg == 2) (void)fetch16(cpu);
            else if (reg == 3) (void)fetch32(cpu);
            if (c < 0) { m68k_exception_line_f(cpu); return; }
            PRIVILEGED();
            if (c) m68k_exception_fmt2(cpu, VEC_TRAPCC, cpu->pc, cpu->ipc);
            return;
        }
        if (c < 0 || !(ea_cls(mode, reg) & EA_DALT)) { m68k_exception_line_f(cpu); return; }
        PRIVILEGED();
        ea_write(cpu, mode, reg, 1, c ? 0xFFu : 0);
        return;
    }
    case 2: case 3: {                           /* PBcc.W, PBcc.L */
        const uint32_t disp = type == 2 ? sz_sext(fetch16(cpu), 2) : fetch32(cpu);
        const int c = condition(cpu, op & 0x3Fu);
        if (c < 0) { m68k_exception_line_f(cpu); return; }
        PRIVILEGED();
        if (c) jump(cpu, cpu->ipc + 2u + disp);
        return;
    }
    case 4: op_psave(cpu, op); return;
    case 5: op_prestore(cpu, op); return;
    default: m68k_exception_line_f(cpu); return;
    }
}
