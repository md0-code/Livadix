/* The 68030 MMU: transparent translation, the 22-entry ATC, the table
 * search through up to five levels with short and long descriptors, early
 * termination, indirection, limits, function-code lookup, and the PMOVE,
 * PFLUSH, PLOAD and PTEST instructions.
 *
 * Source: MC68030 User's Manual (3rd edition, 1990), section 9: 9.2.1 the
 * translation flow (CPU space untranslated; TTx, then the ATC, then a table
 * search; WP and a write to an entry with M clear), 9.2.2 reset, 9.3
 * transparent translation (FC base and mask, R/W and RWM, read-modify-write
 * cycles matching only with RWM set), 9.4 the ATC (22 entries, fully
 * associative, B, CI, WP, M; the entry's FC in the tag), 9.5.1 descriptor
 * formats (short and long table, page, early termination, invalid and
 * indirect descriptors; DT codes; U, WP, CI, M, S, L/U and LIMIT), 9.5.2 the
 * table search (root selection by FC2 and SRE, FCL indexing by function
 * code, TIA-TID, limit checks, U and M updates, invalid descriptors, limit
 * violations and bus errors making an entry with B set), 9.5.3 early
 * termination and indirection, 9.5.5 protection (S bit of long descriptors),
 * 9.7 the registers (CRP/SRP, TC fields and the consistency check, TTx,
 * MMUSR for PTEST at level 0 and levels 1-7, the FD bit), 9.8 the
 * instructions. M68000 PRM section 6: PMOVE, PFLUSH, PLOAD, PTEST (MC68030
 * forms) and their extension words. The ATC's pseudo-LRU replacement is
 * modelled by a rotating pointer over the valid entries. */
#include "m68k_internal.h"

#define TC_E 0x80000000u
#define TC_SRE 0x02000000u
#define TC_FCL 0x01000000u
#define TT_E 0x8000u
#define TT_CI 0x0400u
#define TT_RW 0x0200u
#define TT_RWM 0x0100u

/* MMUSR (MC68030 UM 9.7.4). */
#define MS_B 0x8000u
#define MS_L 0x4000u
#define MS_S 0x2000u
#define MS_W 0x0800u
#define MS_I 0x0400u
#define MS_M 0x0200u
#define MS_T 0x0040u

static unsigned ps_of(uint32_t tc) { return (tc >> 20) & 15u; }

int m68k_mmu030_active(const m68k_cpu *cpu) { return (cpu->mmu.tc30 & TC_E) != 0; }

/* TTx (MC68030 UM 9.3, 9.7.3): enabled; FC and A31-A24 equal outside their
 * masks; read or write as R/W says unless RWM; a read-modify-write cycle
 * only with RWM. */
static int tt_match(uint32_t tt, uint32_t la, unsigned fc, int write, int rmw) {
    if (!(tt & TT_E)) return 0;
    if ((((fc ^ (tt >> 4)) & 7u) & ~tt & 7u) != 0) return 0;
    if ((((la >> 24) ^ (tt >> 24)) & ~(tt >> 16) & 0xFFu) != 0) return 0;
    if (rmw) return (tt & TT_RWM) != 0;
    if (!(tt & TT_RWM) && ((tt & TT_RW) != 0) == (write != 0)) return 0;
    return 1;
}

static m68k_atc_entry *atc_find(m68k_cpu *cpu, uint32_t la, unsigned fc) {
    const uint32_t tag = la >> ps_of(cpu->mmu.tc30);
    for (unsigned i = 0; i < 22; i++) {
        m68k_atc_entry *e = &cpu->mmu.atc030[i];
        if ((e->st & M68K_ATC_V) && e->tag == tag && e->fc == fc) return e;
    }
    return NULL;
}

static void atc_drop(m68k_cpu *cpu, m68k_atc_entry *e) {
    const unsigned ps = ps_of(cpu->mmu.tc30);
    m68k_mmu_tlb_drop(cpu, e, 0, ps);
    m68k_mmu_tlb_drop(cpu, e, 1, ps);
    e->st = 0;
}

static m68k_atc_entry *atc_victim(m68k_cpu *cpu) {
    m68k_mmu *m = &cpu->mmu;
    for (unsigned i = 0; i < 22; i++)
        if (!(m->atc030[i].st & M68K_ATC_V)) return &m->atc030[i];
    m68k_atc_entry *e = &m->atc030[m->next030];
    m->next030 = (uint8_t)((m->next030 + 1u) % 22u);
    atc_drop(cpu, e);
    return e;
}

static void flush_all(m68k_cpu *cpu) {
    for (unsigned i = 0; i < 22; i++)
        if (cpu->mmu.atc030[i].st & M68K_ATC_V) atc_drop(cpu, &cpu->mmu.atc030[i]);
}

typedef m68k_walk walk;

static int desc_read(m68k_cpu *cpu, walk *w, uint32_t a, uint32_t *v) {
    if (m68k_phys_read32(cpu, a, v)) {
        w->status |= MS_B | MS_I;
        return 1;
    }
    w->last = a;
    return 0;
}

/* A limit check of `index` against a long descriptor's (or root pointer's)
 * L/U and LIMIT (first long word bits 31, 30-16). */
static int limit_ok(uint32_t hi, uint32_t index) {
    const uint32_t limit = (hi >> 16) & 0x7FFFu;
    return (hi & 0x80000000u) ? index >= limit : index <= limit;
}

/* The search. `update`: set U (and M for a write) as a normal access does;
 * `max_levels`: PTEST's level (7 for a full search). The MC68851 searches
 * the same tables (mmu851.c): its descriptors add bits the 68030 leaves
 * unused, recorded in g, lock and sg (MC68851 UM 5.1.5.3: G bit 7 and L bit
 * 5 of page descriptors, SG bit 9 of long descriptors and root pointers). */
#define search m68k_mmu030_search
void m68k_mmu030_search(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int update, unsigned max_levels, walk *w) {
    const uint32_t tc = cpu->mmu.tc30;
    const unsigned s = (fc >> 2) & 1u;
    const uint32_t *root = (s && (tc & TC_SRE)) ? cpu->mmu.srp30 : cpu->mmu.crp;
    unsigned ti[4] = {(tc >> 12) & 15u, (tc >> 8) & 15u, (tc >> 4) & 15u, tc & 15u};
    unsigned nti = 0;
    while (nti < 4 && ti[nti]) nti++;
    const unsigned is = (tc >> 16) & 15u, ps = ps_of(tc);
    memset(w, 0, sizeof *w);
    uint32_t hi = root[0];                  /* status/limit word of the current descriptor */
    uint32_t addr = root[1] & 0xFFFFFFF0u;  /* table address of the current descriptor */
    unsigned dt = hi & 3u;
    int lim = 1;                            /* the current descriptor has a limit (long format or root) */
    unsigned used = is;                     /* logical-address bits consumed so far */
    unsigned level = 0;
    int sviol = 0;
    w->sg = (hi >> 9) & 1u;
    if (dt == 1) {
        /* Root pointer page descriptor: a constant offset (9.7.1); the limit
         * applies to the TIA index whatever FCL is. */
        const uint32_t index = nti ? (la << is) >> (32u - ti[0]) : 0;
        if (nti && !limit_ok(hi, index)) {
            w->status |= MS_L | MS_I;
            return;
        }
        w->pa = (addr + la) & ~((1u << ps) - 1u);
        return;
    }
    if (dt == 0) {
        w->status |= MS_I;
        return;
    }
    if (tc & TC_FCL) lim = 0;               /* the FC level cannot be limited */
    unsigned step = (tc & TC_FCL) ? 0u : 1u;   /* 0: the function-code level comes first */
    unsigned tix = 0;
    uint32_t d0 = 0, d1 = 0;
    for (;;) {
        if (level >= max_levels) {
            w->levels = level;
            return;
        }
        uint32_t index;
        if (step == 0) {
            index = fc & 7u;
        } else {
            if (tix >= nti) break;          /* tables exhausted: the last descriptor was not a page */
            index = (la << used) >> (32u - ti[tix]);
            if (lim && !limit_ok(hi, index)) {
                w->status |= MS_L | MS_I;
                w->levels = level;
                return;
            }
        }
        const uint32_t size = dt == 3 ? 8u : 4u;
        const uint32_t da = addr + index * size;
        if (desc_read(cpu, w, da, &d0)) { w->levels = level; return; }
        if (size == 8 && desc_read(cpu, w, da + 4, &d1)) { w->levels = level; return; }
        w->last = da;
        level++;
        if (step == 0) step = 1;
        else used += ti[tix++];
        const unsigned ndt = d0 & 3u;
        if (ndt == 0) {
            w->status |= MS_I;
            w->levels = level;
            return;
        }
        if (size == 8 && (d0 & 0x200u)) w->sg = 1;    /* SG of a long descriptor (MC68851) */
        if (size == 8 && (d0 & 0x100u) && !s) {   /* S of a long descriptor */
            sviol = 1;
            w->status |= MS_S;
        }
        if (d0 & 4u) w->status |= MS_W;
        if (update && !(d0 & 8u) && !sviol) {
            d0 |= 8u;
            if (m68k_phys_write32(cpu, da, d0)) { w->status |= MS_B | MS_I; w->levels = level; return; }
        }
        if (ndt == 1) {
            /* Page descriptor, early termination if indexes remain: the
             * frame is its address plus the unused logical bits. */
            const uint32_t page = size == 8 ? d1 : d0;
            if (size == 8 && tix < nti && !limit_ok(d0, (la << used) >> (32u - ti[tix]))) {
                w->status |= MS_L | MS_I;
                w->levels = level;
                return;
            }
            const uint32_t below = used >= 32u ? 0 : la & (0xFFFFFFFFu >> used);
            w->pa = ((page & 0xFFFFFF00u) + below) & ~((1u << ps) - 1u);
            if (d0 & 0x10u) w->status |= MS_M;
            if (d0 & 0x40u) w->ci = 1;
            w->g = (d0 >> 7) & 1u;
            w->lock = (d0 >> 5) & 1u;
            if (update && write && !(w->status & (MS_W | MS_S)) && !(d0 & 0x10u)) {
                if (m68k_phys_write32(cpu, da, d0 | 0x10u)) { w->status |= MS_B | MS_I; w->levels = level; return; }
                w->status |= MS_M;
            }
            if (sviol) w->status |= MS_I;
            w->levels = level;
            return;
        }
        hi = d0;
        lim = size == 8;
        addr = (size == 8 ? d1 : d0) & 0xFFFFFFF0u;
        dt = ndt;
        if (tix >= nti) {
            /* A table descriptor where the indexes are exhausted: an indirect
             * descriptor to a page descriptor of that size (9.5.3.2). */
            const uint32_t ia = (size == 8 ? d1 : d0) & 0xFFFFFFFCu;
            if (desc_read(cpu, w, ia, &d0)) { w->levels = level; return; }
            if (ndt == 3 && desc_read(cpu, w, ia + 4, &d1)) { w->levels = level; return; }
            w->last = ia;
            if ((d0 & 3u) != 1u) {
                w->status |= MS_I;
                w->levels = level;
                return;
            }
            if (ndt == 3 && (d0 & 0x200u)) w->sg = 1;
            if (ndt == 3 && (d0 & 0x100u) && !s) { sviol = 1; w->status |= MS_S | MS_I; }
            if (d0 & 4u) w->status |= MS_W;
            uint32_t nd = d0;
            if (update && !sviol) {
                nd |= 8u;
                if (write && !(w->status & MS_W)) nd |= 0x10u;
            }
            if (nd != d0 && m68k_phys_write32(cpu, ia, nd)) { w->status |= MS_B | MS_I; w->levels = level; return; }
            if (nd & 0x10u) w->status |= MS_M;
            if (nd & 0x40u) w->ci = 1;
            w->g = (nd >> 7) & 1u;
            w->lock = (nd >> 5) & 1u;
            w->pa = (ndt == 3 ? d1 : nd) & 0xFFFFFF00u & ~((1u << ps) - 1u);
            w->levels = level;
            return;
        }
    }
    w->status |= MS_I;
    w->levels = level;
}

static void load_entry(m68k_cpu *cpu, m68k_atc_entry *e, uint32_t la, unsigned fc, const walk *w) {
    e->tag = la >> ps_of(cpu->mmu.tc30);
    e->fc = (uint8_t)fc;
    e->pa = w->pa;
    e->st = M68K_ATC_V;
    if (w->status & (MS_I | MS_B | MS_L | MS_S)) e->st |= M68K_ATC_B;
    else e->st |= M68K_ATC_R;
    if (w->status & MS_W) e->st |= M68K_ATC_W;
    if (w->status & MS_M) e->st |= M68K_ATC_M;
    if (w->ci) e->st |= 0x40u;
}

int m68k_mmu030_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill) {
    (void)ifetch;
    fc &= 7u;
    *pa = la;
    *fill = 0;
    if (fc == 7) return 0;
    const int rmw = cpu->locked;
    if (tt_match(cpu->mmu.tt30[0], la, fc, write, rmw) || tt_match(cpu->mmu.tt30[1], la, fc, write, rmw)) {
        /* Only accesses of the matching kind are transparent. */
        const uint32_t t0 = cpu->mmu.tt30[0], t1 = cpu->mmu.tt30[1];
        const int both = tt_match(t0, la, fc, !write, 0) || tt_match(t1, la, fc, !write, 0);
        *fill = 0x80u | (both ? 3u : write ? 2u : 1u);
        return 0;
    }
    if (!(cpu->mmu.tc30 & TC_E)) {
        *fill = 0x80u | 3u;
        return 0;
    }
    m68k_atc_entry *e = atc_find(cpu, la, fc);
    if (!e || (write && (e->st & M68K_ATC_R) && !(e->st & (M68K_ATC_M | M68K_ATC_W)))) {
        walk w;
        if (e) atc_drop(cpu, e);
        e = atc_victim(cpu);
        search(cpu, la, fc, write, 1, 7, &w);
        load_entry(cpu, e, la, fc, &w);
    }
    if (e->st & M68K_ATC_B) return 1;
    if (write && (e->st & M68K_ATC_W)) return 1;
    const unsigned ps = ps_of(cpu->mmu.tc30);
    *pa = e->pa | (la & ((1u << ps) - 1u));
    /* Pages smaller than the soft TLB's stay on the slow path. */
    if (ps >= M68K_PAGE_SHIFT) *fill = 1u | (((e->st & (M68K_ATC_M | M68K_ATC_W)) == M68K_ATC_M) ? 2u : 0u);
    return 0;
}

/* --- the instructions (cpID 0) ------------------------------------------- */

#define VEC_MMU_CONFIG 56u

/* FC operand of PFLUSH, PLOAD, PTEST (PRM): 10XXX immediate, 01DDD Dn,
 * 00000 SFC, 00001 DFC; -1 if reserved. */
static int fc_operand(m68k_cpu *cpu, unsigned f) {
    if (f & 0x10u) return (int)(f & 7u);
    if ((f & 0x18u) == 0x08u) return (int)(D(f & 7u) & 7u);
    if (f == 0) return (int)(cpu->sfc & 7u);
    if (f == 1) return (int)(cpu->dfc & 7u);
    return -1;
}

/* TC consistency (9.7.2, 9.7.5.3): PS at least 8 and PS + IS + the TIx up
 * to the first zero equal to 32. */
static int tc_valid(uint32_t tc) {
    const unsigned ps = ps_of(tc);
    if (ps < 8) return 0;
    unsigned sum = ps + ((tc >> 16) & 15u);
    for (int i = 3; i >= 0; i--) {
        const unsigned t = (tc >> (4u * (unsigned)i)) & 15u;
        if (!t) break;
        sum += t;
    }
    return sum == 32;
}

static void line_f(m68k_cpu *cpu) { m68k_exception_line_f(cpu); }

void m68k_op_pmmu030(m68k_cpu *cpu, uint32_t op) {
    /* F-line cpID 0 in user mode is a privilege violation (9.8). */
    PRIVILEGED();
    const uint32_t ext = fetch16(cpu);
    const unsigned mode = OP_MODE(op), reg = OP_REG(op);
    const int calt = mode >= 2 && mode != 3 && mode != 4 && !(mode == 7 && reg > 1);
    m68k_mmu *m = &cpu->mmu;
    cpu->cyc_extra += m68k_timing_pmmu(cpu, op, m68k_pmmu_kind(ext));     /* MC68030 UM 11.7.2 */
    switch (ext >> 13) {
    case 0: case 2: {                                   /* PMOVE TTx / TC, SRP, CRP */
        const unsigned preg = (ext >> 10) & 7u, tomem = (ext >> 9) & 1u, fd = (ext >> 8) & 1u;
        const int tt = (ext >> 13) == 0;
        if ((ext & 0xFFu) || !calt || (tt ? (preg != 2 && preg != 3) : (preg != 0 && preg != 2 && preg != 3))) {
            line_f(cpu);
            return;
        }
        const uint32_t a = ea_addr(cpu, mode, reg, 4);
        uint32_t *r = tt ? &m->tt30[preg - 2] : preg == 0 ? &m->tc30 : preg == 2 ? m->srp30 : m->crp;
        const unsigned n = (tt || preg == 0) ? 1u : 2u;
        if (tomem) {
            for (unsigned i = 0; i < n; i++) wr32(cpu, a + 4u * i, r[i]);
            return;
        }
        uint32_t v[2];
        for (unsigned i = 0; i < n; i++) v[i] = rd32(cpu, a + 4u * i);
        /* The next instruction's words, fetched before the new TC applies
         * (and before the flushes below, so its ATC entry goes with them). */
        if (!tt && preg == 0 && cpu->model != M68K_68020) m68k_prefetch_hold(cpu);
        m68k_mmu_tlb_flush(cpu);
        if (!fd) flush_all(cpu);
        if (tt) {
            r[0] = v[0] & 0xFFFF8777u;
        } else if (preg == 0) {
            /* A new page size changes every tag: the ATC goes even with FD
             * (the manual leaves stale entries of another size undefined). */
            if ((r[0] ^ v[0]) & 0x00F00000u) flush_all(cpu);
            r[0] = v[0] & 0x83FFFFFFu;
            if ((r[0] & TC_E) && !tc_valid(r[0])) {
                r[0] &= ~TC_E;
                m68k_mmu_changed(cpu);
                m68k_exception(cpu, VEC_MMU_CONFIG, cpu->pc);
                return;
            }
        } else {
            r[0] = v[0] & 0xFFFF0003u;
            r[1] = v[1] & 0xFFFFFFF0u;
            if ((r[0] & 3u) == 0) {
                m68k_mmu_changed(cpu);
                m68k_exception(cpu, VEC_MMU_CONFIG, cpu->pc);
                return;
            }
        }
        m68k_mmu_changed(cpu);
        return;
    }
    case 3: {                                           /* PMOVE MMUSR */
        if ((ext & 0x1DFFu) || !calt) {
            line_f(cpu);
            return;
        }
        const uint32_t a = ea_addr(cpu, mode, reg, 2);
        if (ext & 0x0200u) wr16(cpu, a, m->mmusr30);
        else m->mmusr30 = (uint16_t)rd16(cpu, a);
        return;
    }
    case 1: {                                           /* PFLUSH, PLOAD */
        const unsigned fmode = (ext >> 10) & 7u, mask = (ext >> 5) & 7u;
        if (fmode == 1 && (ext & 0x3FFu) == 0) {       /* PFLUSHA */
            m68k_mmu_tlb_flush(cpu);
            flush_all(cpu);
            return;
        }
        const int fc = fc_operand(cpu, ext & 0x1Fu);
        if (fc < 0) {
            line_f(cpu);
            return;
        }
        if (fmode == 0 && !(ext & 0x01E0u)) {          /* PLOADR/PLOADW FC,<ea> */
            if (!calt) { line_f(cpu); return; }
            const uint32_t la = ea_addr(cpu, mode, reg, 4);
            m68k_atc_entry *e = atc_find(cpu, la, (unsigned)fc);
            if (e) atc_drop(cpu, e);
            walk w;
            search(cpu, la, (unsigned)fc, !(ext & 0x0200u), 1, 7, &w);
            load_entry(cpu, atc_victim(cpu), la, (unsigned)fc, &w);
            return;
        }
        if (fmode != 4 && fmode != 6) { line_f(cpu); return; }
        uint32_t la = 0;
        if (fmode == 6) {
            if (!calt) { line_f(cpu); return; }
            la = ea_addr(cpu, mode, reg, 4);
        }
        const uint32_t tag = la >> ps_of(m->tc30);
        for (unsigned i = 0; i < 22; i++) {
            m68k_atc_entry *e = &m->atc030[i];
            if (!(e->st & M68K_ATC_V) || ((e->fc ^ (unsigned)fc) & mask)) continue;
            if (fmode == 6 && e->tag != tag) continue;
            atc_drop(cpu, e);
        }
        return;
    }
    case 4: {                                           /* PTEST */
        const unsigned level = (ext >> 10) & 7u, areg = (ext >> 8) & 1u, an = (ext >> 5) & 7u;
        const int fc = fc_operand(cpu, ext & 0x1Fu);
        if (fc < 0 || !calt || (level == 0 && (areg || an))) {
            line_f(cpu);
            return;
        }
        const uint32_t la = ea_addr(cpu, mode, reg, 4);
        const int write = !(ext & 0x0200u);
        if (level == 0) {
            unsigned sr = 0;
            if (tt_match(m->tt30[0], la, (unsigned)fc, write, 0) || tt_match(m->tt30[1], la, (unsigned)fc, write, 0)) {
                sr = MS_T;
            } else {
                const m68k_atc_entry *e = atc_find(cpu, la, (unsigned)fc);
                if (!e) sr = MS_I;
                else {
                    if (e->st & M68K_ATC_B) sr |= MS_B | MS_I;
                    if (e->st & M68K_ATC_W) sr |= MS_W;
                    if (e->st & M68K_ATC_M) sr |= MS_M;
                }
            }
            m->mmusr30 = (uint16_t)sr;
            return;
        }
        walk w;
        search(cpu, la, (unsigned)fc, write, 0, level, &w);
        m->mmusr30 = (uint16_t)(w.status | (w.levels & 7u));
        if (areg) A(an) = w.last;
        return;
    }
    default:
        line_f(cpu);
        return;
    }
}
