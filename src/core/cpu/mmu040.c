/* The 68040 MMU: transparent translation, the instruction and data ATCs,
 * the three-level table search with U and M updates, PFLUSH and PTEST.
 *
 * Source: MC68040 User's Manual (1993), section 3: 3.1 programming model
 * (URP/SRP, TCR E and P, the TTR fields and matching, MMUSR), 3.2 logical
 * address translation (root/pointer/page indexes, table and page descriptor
 * formats, UDT/PDT encodings, indirect descriptors, invalid descriptors
 * creating an ATC entry with R clear), 3.2.5 table search accesses (U set in
 * every descriptor met, M set for a write without write protection or a
 * supervisor violation, Table 3-1), 3.2.5 Table 3-2 (MOVES to FC 2/6 become
 * data references; FC 0, 3, 4, 7 are untranslated alternate-space
 * accesses), 3.2.6 protection (S and W), 3.3 ATCs (four-way, 64 entries, set
 * from LA15-12 or LA16-13, FC2 and G in the tag, M clear forcing a new table
 * search on a write, a 2-bit counter choosing the entry to replace), 3.4
 * transparent translation (independent of TCR E; TT0's status wins when
 * both match), 3.6 reset (E bits cleared, ATCs kept), 3.7 PFLUSH and PTEST.
 * M68000 PRM 6: PFLUSH opmodes, PTEST R/W and the MMUSR it writes. */
#include "m68k_internal.h"

/* MMUSR bits (MC68040 UM 3.1.4). */
#define MMUSR_R 0x001u
#define MMUSR_T 0x002u
#define MMUSR_B 0x800u

#define TC_E 0x8000u
#define TC_P 0x4000u
#define TT_E 0x8000u
#define TT_W 0x0004u

static unsigned page_shift(const m68k_cpu *cpu) { return (cpu->tc & TC_P) ? 13u : 12u; }

/* Translation is visible to the fast path when the tables are on or a
 * transparent block is write protected (the TTRs work whatever E is). */
int m68k_mmu040_active(const m68k_cpu *cpu) {
    if (cpu->tc & TC_E) return 1;
    const uint32_t tt[4] = {cpu->itt0, cpu->itt1, cpu->dtt0, cpu->dtt1};
    for (unsigned i = 0; i < 4; i++)
        if ((tt[i] & TT_E) && (tt[i] & TT_W)) return 1;
    return 0;
}

/* A TTR matches when enabled, its S field admits FC2 and A31-A24 equal the
 * base outside the mask (MC68040 UM 3.1.3, 3.4). */
static int tt_match(uint32_t tt, uint32_t la, unsigned s) {
    if (!(tt & TT_E)) return 0;
    const unsigned sf = (tt >> 13) & 3u;
    if (sf == 0 && s) return 0;
    if (sf == 1 && !s) return 0;
    const uint32_t base = tt >> 24, mask = (tt >> 16) & 0xFFu;
    return (((la >> 24) ^ base) & ~mask & 0xFFu) == 0;
}
static int tt_lookup(const m68k_cpu *cpu, uint32_t la, unsigned s, int ifetch, uint32_t *tt) {
    const uint32_t t0 = ifetch ? cpu->itt0 : cpu->dtt0, t1 = ifetch ? cpu->itt1 : cpu->dtt1;
    if (tt_match(t0, la, s)) { *tt = t0; return 1; }
    if (tt_match(t1, la, s)) { *tt = t1; return 1; }
    return 0;
}

/* Whether a data translation of `la` in supervisor (s 1) or user space
 * reaches the ATC (and steps its replacement counter): the tables on and no
 * transparent translation matching (m68k_mmu040_translate). */
int m68k_mmu040_data_steps(const m68k_cpu *cpu, uint32_t la, unsigned s) {
    uint32_t tt;
    return (cpu->tc & TC_E) && !tt_lookup(cpu, la, s, 0, &tt);
}

static unsigned set_of(const m68k_cpu *cpu, uint32_t la) { return (la >> page_shift(cpu)) & 15u; }

static m68k_atc_entry *atc_lookup(m68k_cpu *cpu, int unit, uint32_t la, unsigned s) {
    const uint32_t tag = la >> page_shift(cpu);
    m68k_atc_entry *set = cpu->mmu.atc[unit][set_of(cpu, la)];
    for (unsigned w = 0; w < 4; w++)
        if ((set[w].st & M68K_ATC_V) && set[w].tag == tag && ((set[w].fc >> 2) & 1u) == s) return &set[w];
    return NULL;
}

/* A slot for a new entry: an invalid one, else the one the replacement
 * counter points at; its soft-TLB pages go. */
static m68k_atc_entry *atc_victim(m68k_cpu *cpu, int unit, uint32_t la) {
    m68k_atc_entry *set = cpu->mmu.atc[unit][set_of(cpu, la)];
    for (unsigned w = 0; w < 4; w++)
        if (!(set[w].st & M68K_ATC_V)) return &set[w];
    m68k_atc_entry *e = &set[cpu->mmu.ctr[unit] & 3u];
    m68k_mmu_tlb_drop(cpu, e, unit, page_shift(cpu));
    return e;
}

/* The table search (MC68040 UM Figures 3-9, 3-10): fills `e` and returns 0,
 * or MMUSR_B for a bus error on a descriptor access (e gets R clear). */
static unsigned search(m68k_cpu *cpu, uint32_t la, unsigned s, int write, m68k_atc_entry *e) {
    const int p8k = (cpu->tc & TC_P) != 0;
    uint32_t wp = 0, d, a;
    e->tag = la >> page_shift(cpu);
    e->fc = (uint8_t)(s << 2);
    e->pa = 0;
    e->st = M68K_ATC_V;
    /* Root level: RI = LA31-25. */
    a = ((s ? cpu->srp : cpu->urp) & 0xFFFFFE00u) | ((la >> 23) & 0x1FCu);
    if (m68k_phys_read32(cpu, a, &d)) goto bus_error;
    if (!(d & 2u)) return 0;                        /* UDT 00/01: invalid */
    if (!(d & 8u) && m68k_phys_write32(cpu, a, d | 8u)) goto bus_error;
    wp |= d & 4u;
    /* Pointer level: PI = LA24-18. */
    a = (d & 0xFFFFFE00u) | ((la >> 16) & 0x1FCu);
    if (m68k_phys_read32(cpu, a, &d)) goto bus_error;
    if (!(d & 2u)) return 0;
    if (!(d & 8u) && m68k_phys_write32(cpu, a, d | 8u)) goto bus_error;
    wp |= d & 4u;
    /* Page level: PGI = LA17-12 (4K) or LA17-13 (8K). */
    a = p8k ? (d & 0xFFFFFF80u) | ((la >> 11) & 0x7Cu) : (d & 0xFFFFFF00u) | ((la >> 10) & 0xFCu);
    if (m68k_phys_read32(cpu, a, &d)) goto bus_error;
    if ((d & 3u) == 2u) {                           /* indirect */
        a = d & 0xFFFFFFFCu;
        if (m68k_phys_read32(cpu, a, &d)) goto bus_error;
        if ((d & 3u) == 2u) return 0;               /* an indirect to an indirect is invalid */
    }
    if ((d & 3u) == 0) return 0;                    /* PDT 00: invalid */
    wp |= d & 4u;
    uint32_t nd = d | 8u;                           /* U */
    if (write && !wp && !((d & 0x80u) && !s)) nd |= 0x10u;   /* M (Table 3-1) */
    if (nd != d && m68k_phys_write32(cpu, a, nd)) goto bus_error;
    e->pa = nd & (p8k ? 0xFFFFE000u : 0xFFFFF000u);
    e->st = (uint16_t)(M68K_ATC_V | M68K_ATC_R | (nd & (M68K_ATC_G | M68K_ATC_U | M68K_ATC_S | M68K_ATC_CM | M68K_ATC_M)) |
                       (wp ? M68K_ATC_W : 0));
    return 0;
bus_error:
    e->st = M68K_ATC_V | M68K_ATC_B;
    return MMUSR_B;
}

int m68k_mmu040_translate(m68k_cpu *cpu, uint32_t la, unsigned fc, int write, int ifetch, uint32_t *pa, unsigned *fill) {
    const unsigned s = (fc >> 2) & 1u;
    const unsigned f = fc & 7u;
    *pa = la;
    /* Alternate spaces: physical, untranslated (Table 3-2). */
    if (f == 0 || f == 3 || f == 4 || f == 7) {
        *fill = 0;
        return 0;
    }
    uint32_t tt;
    if (tt_lookup(cpu, la, s, ifetch, &tt)) {
        if (write && (tt & TT_W)) return 1;
        *fill = 0x80u | 1u | ((tt & TT_W) ? 0u : 2u);
        return 0;
    }
    if (!(cpu->tc & TC_E)) {
        *fill = 0x80u | 3u;
        return 0;
    }
    const int unit = ifetch ? 1 : 0;
    cpu->mmu.ctr[unit]++;
    m68k_atc_entry *e = atc_lookup(cpu, unit, la, s);
    /* A write through an entry with M clear searches again to set M
     * (MC68040 UM 3.3, M-bit). */
    if (!e || (write && (e->st & M68K_ATC_R) && !(e->st & (M68K_ATC_M | M68K_ATC_W)) && !((e->st & M68K_ATC_S) && !s))) {
        if (e) m68k_mmu_tlb_drop(cpu, e, unit, page_shift(cpu));
        else e = atc_victim(cpu, unit, la);
        search(cpu, la, s, write, e);
    }
    if (!(e->st & M68K_ATC_R)) return 1;
    if ((e->st & M68K_ATC_S) && !s) return 1;
    if (write && (e->st & M68K_ATC_W)) return 1;
    *pa = e->pa | (la & ((1u << page_shift(cpu)) - 1u));
    *fill = 1u | (((e->st & (M68K_ATC_M | M68K_ATC_W)) == M68K_ATC_M) ? 2u : 0u);
    return 0;
}

/* PFLUSH (MC68000 PRM 6, MC68040 form): opmode 0 PFLUSHN (An), 1 PFLUSH (An),
 * 2 PFLUSHAN, 3 PFLUSHA; both ATCs; FC2 from DFC. */
void m68k_mmu040_pflush(m68k_cpu *cpu, unsigned opmode, uint32_t la) {
    const unsigned s = (cpu->dfc >> 2) & 1u, shift = page_shift(cpu);
    for (int unit = 0; unit < 2; unit++)
        for (unsigned set = 0; set < 16; set++)
            for (unsigned w = 0; w < 4; w++) {
                m68k_atc_entry *e = &cpu->mmu.atc[unit][set][w];
                if (!(e->st & M68K_ATC_V)) continue;
                if (!(opmode & 1u) && (e->st & M68K_ATC_G)) continue;
                if (opmode < 2 && (e->tag != la >> shift || ((e->fc >> 2) & 1u) != s)) continue;
                m68k_mmu_tlb_drop(cpu, e, unit, shift);
                e->st = 0;
            }
}

/* PTEST (MC68000 PRM 6, MC68040 form): the ATC the DFC names (program space:
 * instruction) loses its entry for the address, a fresh search fills it,
 * and MMUSR reports (MC68040 UM 3.1.4). */
void m68k_mmu040_ptest(m68k_cpu *cpu, uint32_t la, int write) {
    const unsigned fc = cpu->dfc & 7u, s = (fc >> 2) & 1u;
    const int unit = (fc & 3u) == 2u;
    uint32_t tt;
    m68k_atc_entry *e = atc_lookup(cpu, unit, la, s);
    if (e) {
        m68k_mmu_tlb_drop(cpu, e, unit, page_shift(cpu));
        e->st = 0;
    }
    if (tt_lookup(cpu, la, s, unit, &tt)) {
        cpu->mmusr = MMUSR_T | MMUSR_R;
        return;
    }
    e = atc_victim(cpu, unit, la);
    if (search(cpu, la, s, write, e)) {
        cpu->mmusr = MMUSR_B;
        return;
    }
    if (!(e->st & M68K_ATC_R)) {
        cpu->mmusr = 0;
        return;
    }
    cpu->mmusr = (e->pa & 0xFFFFF000u) | (e->st & (M68K_ATC_G | M68K_ATC_U | M68K_ATC_S | M68K_ATC_CM | M68K_ATC_M | M68K_ATC_W)) |
                 MMUSR_R;
}
