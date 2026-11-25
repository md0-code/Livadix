#include "ppc.h"
#include "ppc_model.h"

/* MPC601UM 6.7.3, Tables 6-11/12: one BAT array for instructions and data. */
#define BAT_ADDRESS UINT32_C(0xfffe0000)
#define BAT_VALID UINT32_C(0x00000040)
#define BAT_BSM UINT32_C(0x0000003f)
#define BAT_KS UINT32_C(0x00000008)
#define BAT_KU UINT32_C(0x00000004)
/* MPC601UM 6.8.3.1, Table 6-14, and 6.10.1, Table 6-20. */
#define SR_T UINT32_C(0x80000000)
#define SR_KS UINT32_C(0x40000000)
#define SR_KU UINT32_C(0x20000000)
#define SR_VSID UINT32_C(0x00ffffff)
#define SR_BUID UINT32_C(0x1ff00000)
#define SR_MEMORY_FORCED UINT32_C(0x07f00000)
/* MPC601UM 6.8.3.2, Figure 6-14/Table 6-16. */
#define PTE_VALID UINT32_C(0x80000000)
#define PTE_SECONDARY UINT32_C(0x00000040)
#define PTE_R UINT32_C(0x00000100)
#define PTE_C UINT32_C(0x00000080)
#define PAGE_ADDRESS UINT32_C(0xfffff000)
#define PAGE_OFFSET UINT32_C(0x00000fff)
/* MPC601UM 6.9.1.3/4, Figure 6-19: nine maskable and ten fixed hash bits. */
#define SDR1_ORIGIN UINT32_C(0xffff0000)
#define SDR1_MASK UINT32_C(0x000001ff)
#define HASH_FIXED UINT32_C(0x000003ff)
/* MPC601UM 5.4.3/4, Tables 5-10/11; bit numbers are MSB-first. */
#define FAULT_NOT_FOUND UINT32_C(0x40000000)
#define FAULT_PROTECTION UINT32_C(0x08000000)
#define FAULT_STORE UINT32_C(0x02000000)
#define ISI_TABLE_SEARCH UINT32_C(0x00200000)
/* MPC601UM 2.4.3: HID0[28] selects 601 LE; MSR[LE/ILE] are reserved. */
#define HID0_LM UINT32_C(0x00000008)

static bool unsupported(ppc_cpu *cpu)
{
    cpu->result = PPC_UNIMPLEMENTED;
    return false;
}

static bool memory_fault(ppc_cpu *cpu, uint32_t ea, ppc_access access,
                         uint32_t cause)
{
    if (access == PPC_FETCH) {
        ppc_exception(cpu, PPC_EX_ISI, cause, ea);
    } else {
        cpu->dar = ea;
        cpu->dsisr = cause | (access == PPC_WRITE ? FAULT_STORE : 0);
        ppc_exception(cpu, PPC_EX_DSI, 0, cpu->last_pc);
    }
    return false;
}

static bool bus_fault(ppc_cpu *cpu, uint32_t saved_pc)
{
    /* MPC601UM 5.4.2: TEA is a machine check, not a translation fault.
       The CPU owns ME/checkstop policy. Do not invent DSISR/DAR contents. */
    ppc_exception(cpu, PPC_EX_MACHINE, 0, saved_pc);
    return false;
}

static bool permitted(bool key, unsigned pp, ppc_access access)
{
    /* MPC601UM 6.4, Table 6-7; also PEM 7.5.4, Table 7-18. */
    if (key && pp == 0)
        return false;
    return access != PPC_WRITE || (pp != 3 && !(key && pp == 1));
}

static bool translated(ppc_cpu *cpu, ppc_access access, uint32_t address,
                       uint32_t *pa)
{
    /* MPC601UM 2.4.3.3: fetch BE words, exchange words within a doubleword.
       Data lane selection needs the operand size and is done by load/store. */
    if (access == PPC_FETCH && (cpu->hid[0] & HID0_LM))
        address ^= 4;
    *pa = address;
    return true;
}

bool ppc_translate(ppc_cpu *cpu, uint32_t ea, ppc_access access, uint32_t *pa)
{
    uint32_t sr = cpu->sr[ea >> 28];
    uint32_t enable = access == PPC_FETCH ? PPC_MSR_IR : PPC_MSR_DR;
    uint32_t saved_pc = access == PPC_FETCH ? ea : cpu->last_pc;
    unsigned i;
    int bat_match = -1;
    uint32_t bat_mask = 0;

    cpu->mmu_wim = 0;
    if (access != PPC_READ && access != PPC_WRITE && access != PPC_FETCH)
        return unsupported(cpu);

    if (cpu->model != PPC_MODEL_601) {
        if (!ppc_classic_translate(cpu, ea, access, pa)) return false;
        if (access == PPC_FETCH && ppc_little_endian(cpu)) *pa ^= 4u;
        return true;
    }

    /* MPC601UM 6.5.1/2: instruction bypass precedes T; for data, T precedes
       DT. These orders differ from the later-family PEM 7.3/7.7 model. */
    if (access == PPC_FETCH && !(cpu->msr & enable))
        return translated(cpu, access, ea, pa);
    if (sr & SR_T) {
        if ((sr & SR_BUID) == SR_MEMORY_FORCED) {
            /* MPC601UM 6.10.4, Figure 6-26: bypass BATs, keys and PTEs. */
            uint32_t address = ((sr & 15) << 28) | (ea & UINT32_C(0x0fffffff));
            return translated(cpu, access, address, pa);
        }
        if (access == PPC_FETCH) {
            /* MPC601UM 5.4.4, Table 5-11: unlike the PEM, SRR1[3] is zero. */
            return memory_fault(cpu, ea, access, 0);
        }
        /* MPC601UM 6.10.2 requires two I/O address packets, a BUID and a
           selected key. ppc_bus has only memory callbacks: no physical
           address can honestly stand in for this protocol. */
        return unsupported(cpu);
    }
    if (!(cpu->msr & enable))
        return translated(cpu, access, ea, pa);

    for (i = 0; i < 4; ++i) {
        uint32_t upper = cpu->bat[i][0], lower = cpu->bat[i][1];
        uint32_t bsm = lower & BAT_BSM;
        uint32_t mask = (bsm << 17) | UINT32_C(0x0001ffff);
        if (!(lower & BAT_VALID))
            continue;
        /* MPC601UM 6.7.2/3: malformed masks/alignment and multiple matches
           have undefined results; report unsupported instead of mapping
           an arbitrary block or falling through to a page. */
        if ((bsm & (bsm + 1)) || ((upper | lower) & BAT_ADDRESS & mask))
            return unsupported(cpu);
        if ((ea & ~mask) == (upper & BAT_ADDRESS)) {
            if (bat_match >= 0)
                return unsupported(cpu);
            bat_match = (int)i;
            bat_mask = mask;
        }
    }
    if (bat_match >= 0) {
        uint32_t upper = cpu->bat[bat_match][0];
        uint32_t lower = cpu->bat[bat_match][1];
        bool key = (upper & ((cpu->msr & PPC_MSR_PR) ? BAT_KU : BAT_KS)) != 0;
        if (!permitted(key, upper & 3, access))
            return memory_fault(cpu, ea, access, FAULT_PROTECTION);
        cpu->mmu_wim = upper & UINT32_C(0x70);
        return translated(cpu, access, (lower & BAT_ADDRESS) | (ea & bat_mask), pa);
    }

    /* MPC601UM 6.8.3.1, Table 6-14: SR[3:7] are reserved on the 601.
       In particular, the PEM 7.5.2.1.1 SR[N] facility is NOT a 601 bit. */
    {
        uint32_t vsid = sr & SR_VSID;
        uint32_t page_index = (ea >> 12) & UINT32_C(0x0000ffff);
        uint32_t hash = vsid ^ page_index;
        uint32_t hash_mask = ((cpu->sdr1 & SDR1_MASK) << 10) | HASH_FIXED;
        uint32_t match = PTE_VALID | (vsid << 7) | (page_index >> 10);
        bool key = (sr & ((cpu->msr & PPC_MSR_PR) ? SR_KU : SR_KS)) != 0;
        unsigned secondary;

        /* MPC601UM 6.9.2: eight PTEs in each PTEG, primary before secondary.
           No guest-visible software TLB is required by the existing API. */
        for (secondary = 0; secondary < 2; ++secondary) {
            uint32_t pteg = (cpu->sdr1 & SDR1_ORIGIN) | ((hash & hash_mask) << 6);
            for (i = 0; i < 8; ++i) {
                uint32_t pte_address = pteg + i * 8;
                uint32_t high, low, history;
                bool allowed;
                if (!cpu->bus.read ||
                    !cpu->bus.read(cpu->bus.opaque, pte_address, 4, &high))
                    return bus_fault(cpu, saved_pc);
                if (high != match)
                    continue;
                if (!cpu->bus.read(cpu->bus.opaque, pte_address + 4, 4, &low))
                    return bus_fault(cpu, saved_pc);
                allowed = permitted(key, low & 3, access);
                /* MPC601UM 6.8.4.1/2 and 6.9.2 step 8: a table hit may set
                   R even for a protected access, but never C for a denied
                   store. Preserve PPN, WIM, PP and all other supplied bits. */
                history = low | PTE_R;
                if (allowed && access == PPC_WRITE)
                    history |= PTE_C;
                if (history != low && (!cpu->bus.write ||
                    !cpu->bus.write(cpu->bus.opaque, pte_address + 4, 4, history)))
                    return bus_fault(cpu, saved_pc);
                if (!allowed)
                    return memory_fault(cpu, ea, access, FAULT_PROTECTION);
                cpu->mmu_wim = low & UINT32_C(0x70);
                return translated(cpu, access, (low & PAGE_ADDRESS) | (ea & PAGE_OFFSET), pa);
            }
            hash = ~hash;
            match |= PTE_SECONDARY;
        }
    }
    return memory_fault(cpu, ea, access,
                        FAULT_NOT_FOUND | (access == PPC_FETCH ? ISI_TABLE_SEARCH : 0));
}

bool ppc_alignment(ppc_cpu *cpu, uint32_t ea)
{
    uint32_t op = cpu->last_opcode;
    /* MPC601UM 5.4.6, Table 5-13: encode the faulting instruction, not the
       DSI store/protection flags. Both indexed and immediate forms matter. */
    uint32_t info = ((op >> 21) & 31) << 5 | ((op >> 16) & 31);
    if (op >> 26 == 31) {
        info |= ((op >> 1) & 3) << 15;
        info |= ((op >> 6) & 1) << 14;
        info |= ((op >> 7) & 15) << 10;
    } else {
        info |= ((op >> 26) & 1) << 14;
        info |= ((op >> 27) & 15) << 10;
    }
    cpu->dar = ea;
    cpu->dsisr = info;
    ppc_exception(cpu, PPC_EX_ALIGNMENT, 0, cpu->last_pc);
    return false;
}

bool ppc_preflight(ppc_cpu *cpu, uint32_t ea, unsigned bytes, bool sequence, bool floating)
{
    uint32_t sr = cpu->sr[ea >> 28];
    bool io = (sr & SR_T) != 0;
    bool forced = io && (sr & SR_BUID) == SR_MEMORY_FORCED;
    bool page = (!io && (cpu->msr & PPC_MSR_DR)) || (forced && floating);
    if (cpu->model != PPC_MODEL_601) return ppc_classic_preflight(cpu, ea, bytes, sequence, floating);
    /* MPC601UM 5.4.6: check the ENTIRE instruction before transfers.
       Word-aligned strings/multiples may cross pages; scalars may not,
       even when a single BAT covers both pages. */
    if ((uint64_t)(ea & UINT32_C(0x0fffffff)) + bytes > UINT32_C(0x10000000) ||
        (floating && io && !forced) ||
        (page && (!sequence || (ea & 3)) && (ea & PAGE_OFFSET) + bytes > 4096) ||
        ((cpu->hid[0] & HID0_LM) && (sequence || (bytes && (ea & (bytes - 1))))))
        return ppc_alignment(cpu, ea);
    return true;
}

bool ppc_data_breakpoint(ppc_cpu *cpu, uint32_t ea, unsigned bytes, bool store, bool sequence)
{
    /* MPC601UM 2.3.3.12.4, tables 2-25/26: HID5 DAB is a doubleword EA,
       SA (bits 30-31) enables stores/loads, bit 29 is ignored and the
       compare is independent of MSR[DT]. PEM 2.3.14, 6.4.3 (MPC604EUM,
       MPC750UM 4.5.3 follow it): DABR[BT] must equal MSR[DR]. The 603 and
       603e have no DABR. Any byte of an access in the doubleword matches;
       strings and multiples are sequenced one word at a time and DAR holds
       that word access's EA (601 table 2-26; within PEM table 6-9's range). */
    uint32_t dabr, target;
    unsigned step;
    if (!bytes) return true;
    if (cpu->model == PPC_MODEL_601) dabr = cpu->hid[5];
    else if (ppc_model_information(cpu->model)->features & PPC_MODEL_SOFT_TLB) return true;
    else {
        dabr = cpu->dabr;
        if (((dabr & 4u) != 0) != ((cpu->msr & PPC_MSR_DR) != 0)) return true;
    }
    if (!(dabr & (store ? 2u : 1u))) return true;
    target = dabr & ~UINT32_C(7);
    step = sequence ? 4u : bytes;
    for (unsigned offset = 0; offset < bytes; offset += step) {
        uint32_t first = ea + offset;
        uint32_t last = first + (bytes - offset < step ? bytes - offset : step) - 1u;
        if ((first & ~UINT32_C(7)) == target || (last & ~UINT32_C(7)) == target) {
            cpu->dar = first;
            cpu->dsisr = PPC_DSISR_DABR | (store ? PPC_DSISR_STORE : 0u);
            ppc_exception(cpu, PPC_EX_DSI, 0, cpu->last_pc);
            return false;
        }
    }
    return true;
}

static bool scalar_address(ppc_cpu *cpu, uint32_t ea, unsigned size,
                           ppc_access access, uint32_t *pa)
{
    if (size != 1 && size != 2 && size != 4)
        return unsupported(cpu);
    if (!ppc_preflight(cpu, ea, size, false, false)) return false;
    if (!ppc_translate(cpu, ea, access, pa))
        return false;
    /* MPC601UM 2.4.3.1: byte/halfword/word address XORs are 7/6/4.
       Callbacks always transfer BE numerical values: no byte reversal. */
    if (ppc_little_endian(cpu))
        *pa ^= 8 - size;
    return true;
}

static unsigned beat_size(uint32_t pa, unsigned remaining)
{
    if (!(pa & 3) && remaining >= 4)
        return 4;
    if (!(pa & 1) && remaining >= 2)
        return 2;
    return 1;
}

static bool unaligned_scalar(ppc_cpu *cpu, uint32_t ea, unsigned size,
                             ppc_access access, uint32_t *value)
{
    bool little = ppc_little_endian(cpu);
    uint32_t result = 0;
    if (!ppc_preflight(cpu, ea, size, false, false)) return false;
    /* MPC604eUM 3.1.5: LE accesses scatter across doubleword byte lanes.
       Translating each byte also handles noncontiguous physical page frames. */
    for (unsigned i = 0; i < size; ++i) {
        uint32_t pa, part;
        unsigned shift = 8u * (little ? i : size - i - 1u);
        if (!ppc_translate(cpu, ea + i, access, &pa)) return false;
        if (little) pa ^= 7u;
        if (access == PPC_READ) {
            if (!cpu->bus.read || !cpu->bus.read(cpu->bus.opaque, pa, 1, &part))
                return bus_fault(cpu, cpu->last_pc);
            result |= (part & 255u) << shift;
        } else if (!cpu->bus.write || !cpu->bus.write(cpu->bus.opaque, pa, 1, (*value >> shift) & 255u)) {
            return bus_fault(cpu, cpu->last_pc);
        }
    }
    if (access == PPC_READ) *value = result;
    return true;
}

bool ppc_load(ppc_cpu *cpu, uint32_t ea, unsigned size, uint32_t *value)
{
    uint32_t pa, result = 0;
    unsigned remaining = size;
    if (cpu->model != PPC_MODEL_601 && (size == 2 || size == 4) && (ea & (size - 1u)))
        return unaligned_scalar(cpu, ea, size, PPC_READ, value);
    if (!scalar_address(cpu, ea, size, PPC_READ, &pa))
        return false;
    while (remaining) {
        uint32_t part;
        unsigned beat = beat_size(pa, remaining);
        if (!cpu->bus.read || !cpu->bus.read(cpu->bus.opaque, pa, beat, &part))
            return bus_fault(cpu, cpu->last_pc);
        part &= UINT32_MAX >> ((4 - beat) * 8);
        /* Avoid shifting a 32-bit value by 32 for the single-word case. */
        result = beat == 4 ? part : (result << (beat * 8)) | part;
        remaining -= beat;
        pa += beat;
    }
    *value = result;
    return true;
}

bool ppc_store(ppc_cpu *cpu, uint32_t ea, unsigned size, uint32_t value)
{
    uint32_t pa;
    unsigned remaining = size;
    if (cpu->model != PPC_MODEL_601 && (size == 2 || size == 4) && (ea & (size - 1u)))
        return unaligned_scalar(cpu, ea, size, PPC_WRITE, &value);
    if (!scalar_address(cpu, ea, size, PPC_WRITE, &pa))
        return false;
    while (remaining) {
        unsigned beat = beat_size(pa, remaining);
        uint32_t part = (value >> ((remaining - beat) * 8)) &
                        (UINT32_MAX >> ((4 - beat) * 8));
        /* A TEA on a later beat can leave earlier writes visible (601 UM
           5.4.2 is imprecise). Memory callbacks offer no rollback, and
           replaying MMIO to fake atomicity would be incorrect. */
        if (!cpu->bus.write || !cpu->bus.write(cpu->bus.opaque, pa, beat, part))
            return bus_fault(cpu, cpu->last_pc);
        remaining -= beat;
        pa += beat;
    }
    return true;
}
