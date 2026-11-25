/* The soft TLB shared by the 68030, 68040 and MC68851 models: the page tables the
 * instructions read and write through while translation is on, filled from
 * the MMU's translations and emptied whenever those may change.
 *
 * The tables are a cache of the chip's own state, never a superset of it: a
 * page enters them only from a valid ATC entry or a transparent
 * translation, and leaves when that entry is replaced or flushed (PFLUSH,
 * PTEST, ATC replacement) or when an MMU register is written. Writes are
 * mapped only for pages whose ATC entry already has M set and no write
 * protection, so the first write to a page takes the slow path, which sets M
 * in the descriptor as the hardware does (MC68040 UM 3.2.5, Table 3-1;
 * MC68030 UM 9.5). Livadix's own design.
 *
 * With the translator on, the tables also serve while translation is off
 * (mode 2: logical = physical, every page filled from the bus's maps on its
 * first slow access), and the write mappings of pages that hold translated
 * code are held back in a table of their own (kind 3): the fast path misses
 * them, the slow path writes through them without a new translation, so
 * the translator sees every write to its code and the ATCs see exactly the
 * accesses they see under the interpreter. */
#include "m68k_internal.h"

#include <stdlib.h>

#define LIST_CAP 65536u

static int is_040(const m68k_cpu *cpu) { return cpu->model == M68K_68040 || cpu->model == M68K_68LC040; }

/* Physical accesses (table searches): the bus's supervisor maps, else its
 * callbacks; nonzero for a bus error. */
int m68k_phys_read32(m68k_cpu *cpu, uint32_t pa, uint32_t *v) {
    uint8_t *const *map = cpu->bus.read_map[1];
    const uint8_t *p = map ? map[pa >> M68K_PAGE_SHIFT] : NULL;
    if (p && (pa & 3u) == 0) {
        *v = ld32(p + (pa & (M68K_PAGE_SIZE - 1)));
        return 0;
    }
    if (!cpu->bus.read) return 1;
    return cpu->bus.read(cpu->bus.ctx, pa, 4, M68K_FC_SUPER_DATA, v);
}
int m68k_phys_write32(m68k_cpu *cpu, uint32_t pa, uint32_t v) {
    uint8_t *const *map = cpu->bus.write_map[1];
    uint8_t *p = map ? map[pa >> M68K_PAGE_SHIFT] : NULL;
    if (p && (pa & 3u) == 0) {
        st32(p + (pa & (M68K_PAGE_SIZE - 1)), v);
        if (cpu->jit) m68k_jit_written(cpu, p, pa & (M68K_PAGE_SIZE - 1), 4);
        return 0;
    }
    if (!cpu->bus.write) return 1;
    const int fault = cpu->bus.write(cpu->bus.ctx, pa, 4, M68K_FC_SUPER_DATA, v);
    if (!fault && cpu->jit) {
        /* A descriptor's U and M bits written through the callback: code
         * the translator took from the page's read map goes (as
         * piece_write; a table walk over a handler, jit_diff). */
        uint8_t *const *rmap = cpu->bus.read_map[1];
        const uint8_t *r = rmap ? rmap[pa >> M68K_PAGE_SHIFT] : NULL;
        if (r) m68k_jit_written(cpu, r, pa & (M68K_PAGE_SIZE - 1), 4);
    }
    return fault;
}

static uint8_t **table(m68k_cpu *cpu, unsigned t) { return cpu->mmu.tlb + (size_t)t * M68K_PAGE_COUNT; }

static void clear_all(m68k_cpu *cpu) {
    memset(cpu->mmu.tlb, 0, sizeof(uint8_t *) * TLB_TABLES * M68K_PAGE_COUNT);
    cpu->mmu.nlist = 0;
}

void m68k_mmu_tlb_drop(m68k_cpu *cpu, const m68k_atc_entry *e, int instruction, unsigned page_shift) {
    if (!cpu->mmu.tlb || !(e->st & M68K_ATC_V)) return;
    const unsigned s = (e->fc & 4u) ? 1u : 0u;
    const uint32_t first = (e->tag << page_shift) >> M68K_PAGE_SHIFT;
    const uint32_t n = page_shift > M68K_PAGE_SHIFT ? 1u << (page_shift - M68K_PAGE_SHIFT) : 1u;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t page = (first + i) & (M68K_PAGE_COUNT - 1);
        if (instruction) {
            table(cpu, TLB_TABLE(0, s))[page] = NULL;
        } else {
            table(cpu, TLB_TABLE(1, s))[page] = NULL;
            table(cpu, TLB_TABLE(2, s))[page] = NULL;
            table(cpu, TLB_TABLE(3, s))[page] = NULL;
            table(cpu, TLB_TABLE(4, s))[page] = NULL;
            table(cpu, TLB_TABLE(5, s))[page] = NULL;
        }
    }
}

/* Every page the tables map: those of the ATC entries and those on the list. */
void m68k_mmu_tlb_flush(m68k_cpu *cpu) {
    m68k_mmu *m = &cpu->mmu;
    if (!m->tlb) return;
    if (is_040(cpu)) {
        const unsigned shift = (cpu->tc & 0x4000u) ? 13u : 12u;
        for (unsigned u = 0; u < 2; u++)
            for (unsigned set = 0; set < 16; set++)
                for (unsigned w = 0; w < 4; w++) m68k_mmu_tlb_drop(cpu, &m->atc[u][set][w], (int)u, shift);
    } else {
        /* The 68030 and the MC68851 have one ATC for both kinds of access. */
        const unsigned shift = (m->tc30 >> 20) & 15u;
        const int n = cpu->model == M68K_68020 ? 64 : 22;
        m68k_atc_entry *atc = cpu->model == M68K_68020 ? cpu->pmmu.atc : m->atc030;
        for (int i = 0; i < n; i++) {
            m68k_mmu_tlb_drop(cpu, &atc[i], 0, shift);
            m68k_mmu_tlb_drop(cpu, &atc[i], 1, shift);
        }
    }
    for (uint32_t i = 0; i < m->nlist; i++) table(cpu, m->list[i] >> 20)[m->list[i] & 0xFFFFFu] = NULL;
    m->nlist = 0;
}

void m68k_mmu_tlb_fill(m68k_cpu *cpu, uint32_t la, unsigned fc, int ifetch, uint32_t pa, unsigned fill) {
    m68k_mmu *m = &cpu->mmu;
    if (!m->tlb || !(fill & 3u)) return;
    const unsigned s = (fc & 4u) ? 1u : 0u;
    const uint32_t lpage = la >> M68K_PAGE_SHIFT, ppage = pa >> M68K_PAGE_SHIFT;
    uint8_t *const *rmap = cpu->bus.read_map[s], *const *wmap = cpu->bus.write_map[s];
    uint8_t *rp = rmap ? rmap[ppage] : NULL, *wp = wmap ? wmap[ppage] : NULL;
    unsigned t[2], n = 0;
    if (ifetch) {
        if ((fill & 1u) && rp) { table(cpu, TLB_TABLE(0, s))[lpage] = rp; t[n++] = TLB_TABLE(0, s); }
    } else if (!rp && !wp) {
        /* Not memory: the physical page, for the slow path. The 68030's
         * translation has other effects than a counter: not cached. */
        if (!is_040(cpu) && cpu->mmu.mode == 1) return;
        uint8_t *dev = (uint8_t *)(uintptr_t)((ppage << M68K_PAGE_SHIFT) | TLB_DEV_VALID | ((fill & 0x80u) ? 0u : TLB_DEV_ATC));
        if (fill & 1u) { table(cpu, TLB_TABLE(4, s))[lpage] = dev; t[n++] = TLB_TABLE(4, s); }
        if (fill & 2u) { table(cpu, TLB_TABLE(5, s))[lpage] = dev; t[n++] = TLB_TABLE(5, s); }
    } else {
        if ((fill & 1u) && rp) { table(cpu, TLB_TABLE(1, s))[lpage] = rp; t[n++] = TLB_TABLE(1, s); }
        if ((fill & 2u) && wp) {
            const unsigned k = TLB_TABLE(cpu->jit && m68k_jit_is_code(cpu, wp) ? 3u : 2u, s);
            table(cpu, k)[lpage] = wp;
            t[n++] = k;
        }
    }
    if (!(fill & 0x80u)) return;
    for (unsigned i = 0; i < n; i++) {
        if (m->nlist == LIST_CAP) {
            clear_all(cpu);
            return;
        }
        m->list[m->nlist++] = t[i] << 20 | lpage;
    }
}

/* One write mapping between the fast path's table and the held-back one. */
static int move_write(m68k_cpu *cpu, unsigned s, uint32_t page, const uint8_t *host, int protect) {
    uint8_t **from = table(cpu, TLB_TABLE(protect ? 2u : 3u, s)), **to = table(cpu, TLB_TABLE(protect ? 3u : 2u, s));
    uint8_t *p = from[page];
    if (!p || (host && p != host)) return 0;
    to[page] = p;
    from[page] = NULL;
    return 1;
}

static void protect_entry(m68k_cpu *cpu, const m68k_atc_entry *e, unsigned page_shift, const uint8_t *host, int protect) {
    if (!(e->st & M68K_ATC_V)) return;
    const unsigned s = (e->fc & 4u) ? 1u : 0u;
    const uint32_t first = (e->tag << page_shift) >> M68K_PAGE_SHIFT;
    const uint32_t n = page_shift > M68K_PAGE_SHIFT ? 1u << (page_shift - M68K_PAGE_SHIFT) : 1u;
    for (uint32_t i = 0; i < n; i++) move_write(cpu, s, (first + i) & (M68K_PAGE_COUNT - 1), host, protect);
}

/* The pages the tables may map are those of the data ATC entries and those
 * on the list (the same walk as m68k_mmu_tlb_flush). */
void m68k_mmu_tlb_protect(m68k_cpu *cpu, const uint8_t *host, int protect) {
    m68k_mmu *m = &cpu->mmu;
    if (!m->tlb) return;
    if (is_040(cpu)) {
        const unsigned shift = (cpu->tc & 0x4000u) ? 13u : 12u;
        for (unsigned set = 0; set < 16; set++)
            for (unsigned w = 0; w < 4; w++) protect_entry(cpu, &m->atc[0][set][w], shift, host, protect);
    } else {
        const unsigned shift = (m->tc30 >> 20) & 15u;
        const int n = cpu->model == M68K_68020 ? 64 : 22;
        const m68k_atc_entry *atc = cpu->model == M68K_68020 ? cpu->pmmu.atc : m->atc030;
        for (int i = 0; i < n; i++) protect_entry(cpu, &atc[i], shift, host, protect);
    }
    const unsigned from0 = TLB_TABLE(protect ? 2u : 3u, 0), from1 = TLB_TABLE(protect ? 2u : 3u, 1);
    for (uint32_t i = 0; i < m->nlist; i++) {
        const unsigned t = m->list[i] >> 20, s = t & 1u;
        const uint32_t page = m->list[i] & 0xFFFFFu;
        if ((t == from0 || t == from1) && move_write(cpu, s, page, host, protect))
            m->list[i] = TLB_TABLE(protect ? 3u : 2u, s) << 20 | page;
    }
}

int m68k_mmu_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill) {
    if (is_040(cpu)) return m68k_mmu040_translate(cpu, la, fc, write, ifetch, pa, fill);
    if (cpu->model == M68K_68020) return m68k_mmu851_translate(cpu, la, fc, write, ifetch, pa, fill);
    return m68k_mmu030_translate(cpu, la, fc, write, ifetch, pa, fill);
}

void m68k_mmu_changed(m68k_cpu *cpu) {
    m68k_mmu *m = &cpu->mmu;
    const int active = is_040(cpu) ? m68k_mmu040_active(cpu)
                       : cpu->model == M68K_68030 ? m68k_mmu030_active(cpu) : m68k_mmu851_active(cpu);
    if ((active || cpu->jit) && !m->tlb) {
        m->tlb = calloc(TLB_TABLES * (size_t)M68K_PAGE_COUNT, sizeof(uint8_t *));
        m->list = malloc(LIST_CAP * sizeof(uint32_t));
        m->nlist = 0;
    }
    /* What the tables held may no longer hold: empty them whether or not
     * translation stays on. */
    if (m->tlb) {
        if (m->nlist == LIST_CAP) clear_all(cpu);
        else m68k_mmu_tlb_flush(cpu);
    }
    /* Mode 1: the MMU translates; 2: the translator wants the tables while
     * logical = physical; 0: the bus's own maps. */
    m->mode = (uint8_t)(!m->tlb ? 0 : active ? 1 : cpu->jit ? 2 : 0);
    m68k_select_maps(cpu);
}

void m68k_mmu_forget(m68k_cpu *cpu, uint32_t address) {
    const uint32_t page = address >> M68K_PAGE_SHIFT;
    if (!cpu->mmu.tlb) return;
    for (unsigned s = 0; s < 2; s++)
        for (unsigned k = 0; k < 6; k++) table(cpu, TLB_TABLE(k, s))[page] = NULL;
}

void m68k_pflusha(m68k_cpu *cpu) {
    m68k_mmu_tlb_flush(cpu);
    memset(cpu->mmu.atc, 0, sizeof cpu->mmu.atc);
    memset(cpu->mmu.atc030, 0, sizeof cpu->mmu.atc030);
    memset(cpu->pmmu.atc, 0, sizeof cpu->pmmu.atc);
}

void m68k_free(m68k_cpu *cpu) {
    m68k_jit_free(cpu);
    free(cpu->mmu.tlb);
    free(cpu->mmu.list);
    cpu->mmu.tlb = NULL;
    cpu->mmu.list = NULL;
    cpu->mmu.mode = 0;
}
