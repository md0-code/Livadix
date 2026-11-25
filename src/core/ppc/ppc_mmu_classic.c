/* MPC603eUM chapter 5; MPC604eUM chapter 6; MPC750UM chapter 5. */
#include "ppc_model.h"

#define SR_T 0x80000000u
#define SR_KS 0x40000000u
#define SR_KP 0x20000000u
#define SR_N 0x10000000u
#define PTE_R 0x100u
#define PTE_C 0x80u

static bool storage_fault(ppc_cpu *c, uint32_t ea, ppc_access access, uint32_t cause) {
    if (access == PPC_FETCH) ppc_exception(c, PPC_EX_ISI, cause, ea);
    else {
        c->dar = ea; c->dsisr = cause | (access == PPC_WRITE ? 0x02000000u : 0u);
        ppc_exception(c, PPC_EX_DSI, 0, c->last_pc);
    }
    return false;
}
static bool bus_read(ppc_cpu *c, uint32_t address, uint32_t *value) {
    if (c->bus.read && c->bus.read(c->bus.opaque, address, 4, value)) return true;
    ppc_exception(c, PPC_EX_MACHINE, 0, c->last_pc); return false;
}
static bool bus_write(ppc_cpu *c, uint32_t address, uint32_t value) {
    if (c->bus.write && c->bus.write(c->bus.opaque, address, 4, value)) return true;
    ppc_exception(c, PPC_EX_MACHINE, 0, c->last_pc); return false;
}
static bool protection(uint32_t pte, bool key, bool store) {
    unsigned pp = pte & 3u;
    if (key && pp == 0) return false;
    return !store || (pp != 3 && (!key || pp != 1));
}
static uint32_t pteg(const ppc_cpu *c, uint32_t hash) {
    uint32_t mask = ((c->sdr1 & 0x1ffu) << 10) | 0x3ffu;
    return (c->sdr1 & 0xffff0000u) | ((hash & mask) << 6);
}
static bool bat_translate(ppc_cpu *c, uint32_t ea, ppc_access access, uint32_t *pa, bool *matched) {
    bool user = (c->msr & PPC_MSR_PR) != 0;
    *matched = false;
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t *bat = access == PPC_FETCH ? c->bat[i] : c->dbat[i];
        uint32_t upper = bat[0], lower = bat[1], bl = (upper >> 2) & 0x7ffu;
        uint32_t mask = (bl << 17) | 0x1ffffu;
        if (!(upper & (user ? 1u : 2u)) || (ea & ~mask) != (upper & 0xfffe0000u & ~mask)) continue;
        *matched = true;
        if (!(lower & 3u) || (access == PPC_WRITE && (lower & 3u) != 2u))
            return storage_fault(c, ea, access, 0x08000000u);
        if (access == PPC_FETCH && (lower & 8u)) return storage_fault(c, ea, access, 0x10000000u);
        c->mmu_wim = lower & 0x78u;
        *pa = (lower & 0xfffe0000u & ~mask) | (ea & mask);
        return true;
    }
    return true;
}

static bool tlb_translate(ppc_cpu *c, uint32_t ea, uint32_t sr, ppc_access access, uint32_t *pa) {
    unsigned which = access == PPC_FETCH ? 0u : 1u, index = (ea >> 12) & 31u;
    uint32_t page = (ea >> 12) & 0xffffu, vsid = sr & 0xffffffu;
    bool key = (sr & ((c->msr & PPC_MSR_PR) ? SR_KP : SR_KS)) != 0;
    bool store = access == PPC_WRITE;
    unsigned miss_way = c->tlb_lru[which][index];
    for (unsigned way = 0; way < 2; ++way) {
        ppc_tlb_entry *entry = &c->tlb[which][index][way];
        if (!entry->valid || entry->vsid != vsid || entry->page != page) continue;
        c->tlb_lru[which][index] = (unsigned char)(way ^ 1u);
        if (!protection(entry->pte, key, store)) return storage_fault(c, ea, access, 0x08000000u);
        if (access == PPC_FETCH && (entry->pte & 8u)) return storage_fault(c, ea, access, 0x10000000u);
        if (store && !(entry->pte & PTE_C)) { miss_way = way; break; }
        c->mmu_wim = entry->pte & 0x78u;
        *pa = (entry->pte & 0xfffff000u) | (ea & 0xfffu);
        return true;
    }
    {
        uint32_t compare = 0x80000000u | (vsid << 7) | ((ea >> 22) & 0x3fu);
        uint32_t hash = vsid ^ page;
        uint32_t cause = (c->cr & 0xf0000000u) | (key ? 0x00080000u : 0u)
            | (access == PPC_FETCH ? 0x00040000u : 0u)
            | (miss_way ? 0x00020000u : 0u) | (store ? 0x00010000u : 0u);
        c->hash1 = pteg(c, hash); c->hash2 = pteg(c, ~hash);
        if (access == PPC_FETCH) { c->imiss = ea; c->icmp = compare; }
        else { c->dmiss = ea; c->dcmp = compare; }
        ppc_exception(c, access == PPC_FETCH ? PPC_EX_ITLB : store ? PPC_EX_DTLB_STORE : PPC_EX_DTLB_LOAD,
                      cause, access == PPC_FETCH ? ea : c->last_pc);
    }
    return false;
}
void ppc_classic_tlb_load(ppc_cpu *c, uint32_t ea, bool instruction) {
    unsigned which = instruction ? 0u : 1u, index = (ea >> 12) & 31u;
    unsigned way = (c->srr1 >> 17) & 1u;
    uint32_t compare = instruction ? c->icmp : c->dcmp;
    uint32_t miss = instruction ? c->imiss : c->dmiss;
    ppc_tlb_entry *entry = &c->tlb[which][index][way];
    entry->vsid = (compare >> 7) & 0xffffffu;
    entry->page = ((compare & 0x3fu) << 10) | ((miss >> 12) & 0x3ffu);
    entry->pte = c->rpa & 0xfffff0fbu;
    entry->valid = (compare & 0x80000000u) != 0;
    c->tlb_lru[which][index] = (unsigned char)(way ^ 1u);
    ++c->tlb_writes;
}
void ppc_classic_tlb_invalidate(ppc_cpu *c, uint32_t ea) {
    unsigned index = (ea >> 12) & 31u;
    for (unsigned which = 0; which < 2; ++which)
        for (unsigned way = 0; way < 2; ++way) c->tlb[which][index][way].valid = false;
    ++c->tlb_writes;
}

static bool classic_translate(ppc_cpu *c, uint32_t ea, ppc_access access, uint32_t *pa, bool software_tlb) {
    uint32_t sr, vsid, page, hash, api;
    bool matched, key;
    c->mmu_wim = 0;
    if (!(c->msr & (access == PPC_FETCH ? PPC_MSR_IR : PPC_MSR_DR))) { *pa = ea; return true; }
    if (!bat_translate(c, ea, access, pa, &matched)) return false;
    if (matched) return true;
    sr = c->sr[ea >> 28];
    if (sr & SR_T) return storage_fault(c, ea, access, access == PPC_FETCH ? 0x10000000u : 0x04000000u);
    if (access == PPC_FETCH && (sr & SR_N)) return storage_fault(c, ea, access, 0x10000000u);
    if (software_tlb && (ppc_model_information(c->model)->features & PPC_MODEL_SOFT_TLB))
        return tlb_translate(c, ea, sr, access, pa);
    vsid = sr & 0xffffffu; page = (ea >> 12) & 0xffffu; hash = vsid ^ page; api = (ea >> 22) & 0x3fu;
    key = (sr & ((c->msr & PPC_MSR_PR) ? SR_KP : SR_KS)) != 0;
    for (unsigned secondary = 0; secondary < 2; ++secondary) {
        uint32_t group = pteg(c, secondary ? ~hash : hash);
        uint32_t tag = 0x80000000u | (vsid << 7) | (secondary << 6) | api;
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t address = group + i * 8u, upper, lower, updated;
            bool allowed;
            if (!bus_read(c, address, &upper)) return false;
            if (upper != tag) continue;
            if (!bus_read(c, address + 4u, &lower)) return false;
            allowed = protection(lower, key, access == PPC_WRITE);
            updated = lower | PTE_R | (allowed && access == PPC_WRITE ? PTE_C : 0u);
            if (updated != lower && !bus_write(c, address + 4u, updated)) return false;
            if (!allowed) return storage_fault(c, ea, access, 0x08000000u);
            if (access == PPC_FETCH && (lower & 8u)) return storage_fault(c, ea, access, 0x10000000u);
            c->mmu_wim = lower & 0x78u;
            *pa = (lower & 0xfffff000u) | (ea & 0xfffu);
            return true;
        }
    }
    return storage_fault(c, ea, access, 0x40000000u);
}

bool ppc_classic_translate(ppc_cpu *c, uint32_t ea, ppc_access access, uint32_t *pa) {
    return classic_translate(c, ea, access, pa, true);
}

/* Debugger only, on a copied CPU with non-mutating memory callbacks:
 * resolve a nonresident 603 page from its hashed table (MPC603eUM ch. 5).
 * Guest execution still takes the software TLB-miss exception above. */
bool ppc_classic_debug_walk(ppc_cpu *c, uint32_t ea, ppc_access access, uint32_t *pa) {
    return classic_translate(c, ea, access, pa, false);
}

bool ppc_classic_preflight(ppc_cpu *c, uint32_t ea, unsigned bytes, bool sequence, bool floating) {
    unsigned alignment = bytes < 4u ? bytes : 4u;
    bool early_le = c->model == PPC_MODEL_603 || c->model == PPC_MODEL_603E || c->model == PPC_MODEL_604;
    if (!bytes) return true;
    /* MPC603eUM 4.5.6 (PID6), MPC604eUM 3.1.5/4.3.6, MPC750UM 4.5.6:
       later scalar page crossings are translated, not rejected as on the 601. */
    if ((sequence && ppc_little_endian(c)) ||
        (((early_le && ppc_little_endian(c)) || floating) && (ea & (alignment - 1u))))
        return ppc_alignment(c, ea);
    return true;
}
