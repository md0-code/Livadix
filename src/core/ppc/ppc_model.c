/* Motorola MPC603eUM, MPC604eUM and MPC750UM, chapters 2 and 4. */
#include "ppc_model.h"
#include <string.h>

static const ppc_model_info models[PPC_MODEL_COUNT] = {
    { "601",  0x00010001u, 0x0000fd70u, 0 },
    { "603",  0x00030100u, 0x87c7ff7fu, PPC_MODEL_SOFT_TLB },
    { "603e", 0x00060100u, 0x87c7ff7fu, PPC_MODEL_SOFT_TLB },
    { "604",  0x00040100u, 0x87c5ff7fu, PPC_MODEL_PMC2 },
    { "604e", 0x00090100u, 0x87c5ff7fu, PPC_MODEL_PMC2 | PPC_MODEL_PMC4 },
    { "750",  0x00080200u, 0x87c5ff7fu, PPC_MODEL_PMC2 | PPC_MODEL_PMC4 | PPC_MODEL_L2 }
};

const ppc_model_info *ppc_model_information(ppc_model model) {
    return &models[model >= PPC_MODEL_601 && model < PPC_MODEL_COUNT ? model : PPC_MODEL_601];
}
bool ppc_model_parse(const char *name, ppc_model *model) {
    for (unsigned i = 0; i < PPC_MODEL_COUNT; ++i) {
        if (!strcmp(name, models[i].name)) { *model = (ppc_model)i; return true; }
    }
    return false;
}
bool ppc_little_endian(const ppc_cpu *c) {
    return c->model == PPC_MODEL_601 ? (c->hid[0] & 8u) != 0 : (c->msr & PPC_MSR_LE) != 0;
}
/* MPC603EUM 9.3, MPC750UM 10.2: HID0 selects doze or nap;
 * MPC604EUM 7.2.13: POW alone requests doze (RUN remains asserted).
 * Time base/decrementer and device events continue in these modes. */
bool ppc_power_waiting(const ppc_cpu *c) {
    if (!(c->msr & PPC_MSR_POW) || c->model == PPC_MODEL_601) return false;
    if ((c->msr & PPC_MSR_EE) && (c->external_irq || c->decrementer_pending)) return false;
    if (c->model == PPC_MODEL_604 || c->model == PPC_MODEL_604E) return true;
    const uint32_t mode = c->hid[0] & 0x00e00000u;
    return mode == 0x00800000u || mode == 0x00400000u;
}
void ppc_init_model(ppc_cpu *c, ppc_bus bus, ppc_model model) {
    memset(c, 0, sizeof(*c));
    c->model = model >= PPC_MODEL_601 && model < PPC_MODEL_COUNT ? model : PPC_MODEL_601;
    c->bus = bus;
    ppc_reset(c);
}
void ppc_model_reset(ppc_cpu *c) {
    c->msr = PPC_MSR_IP;
    c->pvr = models[c->model].pvr;
    c->dec = UINT32_MAX;
}
void ppc_model_set_msr(ppc_cpu *c, uint32_t value, bool returning) {
    const ppc_model_info *info = &models[c->model];
    if (returning) value = (value & 0x87c0ffffu) | (c->msr & (PPC_MSR_POW | PPC_MSR_ILE));
    value &= info->msr_mask;
    if (returning && (info->features & PPC_MODEL_SOFT_TLB)) value &= ~PPC_MSR_TGPR;
    if ((info->features & PPC_MODEL_SOFT_TLB) && ((value ^ c->msr) & PPC_MSR_TGPR)) {
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t old = c->gpr[i]; c->gpr[i] = c->tgpr[i]; c->tgpr[i] = old;
        }
    }
    c->msr = value;
}
void ppc_model_exception(ppc_cpu *c, uint32_t vector, uint32_t saved_pc, uint32_t cause) {
    uint32_t old = c->msr;
    uint32_t next = old & (0x87c00000u | PPC_MSR_ILE | PPC_MSR_ME | PPC_MSR_IP);
    bool miss = vector == PPC_EX_ITLB || vector == PPC_EX_DTLB_LOAD || vector == PPC_EX_DTLB_STORE;
    if (vector == PPC_EX_MACHINE && !(old & PPC_MSR_ME)) {
        c->result = PPC_CHECKSTOP; c->pc = saved_pc; c->reserved = false; return;
    }
    c->srr0 = saved_pc;
    c->srr1 = (old & 0x87c0ffffu) | cause;
    if (miss) { c->srr1 = (old & 0xffffu) | cause; next |= PPC_MSR_TGPR; }
    if (old & PPC_MSR_ILE) next |= PPC_MSR_LE;
    if (vector == PPC_EX_MACHINE) next &= ~PPC_MSR_ME;
    ppc_model_set_msr(c, next, false);
    c->pc = ((old & PPC_MSR_IP) ? 0xfff00000u : 0u) + vector;
    c->last_exception = vector; c->reserved = false; c->result = PPC_EXCEPTION;
}

bool ppc_model_spr_read(ppc_cpu *c, unsigned s, uint32_t *value) {
    unsigned flags = models[c->model].features;
    if (s >= 528 && s <= 535) { *value = c->bat[(s - 528u) / 2u][s & 1u]; return true; }
    if (s >= 536 && s <= 543) { *value = c->dbat[(s - 536u) / 2u][s & 1u]; return true; }
    switch (s) {
    case 1: *value = c->xer & 0xe000007fu; return true;
    case 8: *value = c->lr; return true;
    case 9: *value = c->ctr; return true;
    case 18: *value = c->dsisr; return true;
    case 19: *value = c->dar; return true;
    case 22: *value = c->dec; return true;
    case 25: *value = c->sdr1; return true;
    case 26: *value = c->srr0; return true;
    case 27: *value = c->srr1; return true;
    case 268: *value = (uint32_t)c->time_base; return true;
    case 269: *value = (uint32_t)(c->time_base >> 32); return true;
    case 272: case 273: case 274: case 275: *value = c->sprg[s - 272u]; return true;
    case 282: *value = c->ear; return true;
    case 287: *value = c->pvr; return true;
    case 976: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->dmiss; return true; } break;
    case 977: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->dcmp; return true; } break;
    case 978: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->hash1; return true; } break;
    case 979: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->hash2; return true; } break;
    case 980: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->imiss; return true; } break;
    case 981: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->icmp; return true; } break;
    case 982: if (flags & PPC_MODEL_SOFT_TLB) { *value = c->rpa; return true; } break;
    case 936: if (flags & PPC_MODEL_L2) { *value = c->mmcr[0]; return true; } break;
    case 937: case 938: if (flags & PPC_MODEL_L2) { *value = c->pmc[s - 937u]; return true; } break;
    case 939: if (flags & PPC_MODEL_L2) { *value = c->sia; return true; } break;
    case 940: if (flags & PPC_MODEL_L2) { *value = c->mmcr[1]; return true; } break;
    case 941: case 942: if (flags & PPC_MODEL_L2) { *value = c->pmc[s - 939u]; return true; } break;
    case 952: if (flags & PPC_MODEL_PMC2) { *value = c->mmcr[0]; return true; } break;
    case 953: case 954: if (flags & PPC_MODEL_PMC2) { *value = c->pmc[s - 953u]; return true; } break;
    case 955: if (flags & PPC_MODEL_PMC2) { *value = c->sia; return true; } break;
    case 956: if (flags & PPC_MODEL_PMC4) { *value = c->mmcr[1]; return true; } break;
    case 957: case 958: if (flags & PPC_MODEL_PMC4) { *value = c->pmc[s - 955u]; return true; } break;
    case 959: if (c->model == PPC_MODEL_604 || c->model == PPC_MODEL_604E) { *value = c->sda; return true; } break;
    case 1008: *value = c->hid[0]; return true;
    case 1009: *value = c->hid[1]; return true;
    case 1010: *value = c->iabr; return true;
    case 1013: if (!(flags & PPC_MODEL_SOFT_TLB)) { *value = c->dabr; return true; } break;
    case 1017: if (flags & PPC_MODEL_L2) { *value = c->l2cr; return true; } break;
    case 1019: if (flags & PPC_MODEL_L2) { *value = c->ictc; return true; } break;
    case 1020: case 1021: case 1022:
        if (flags & PPC_MODEL_L2) { *value = c->thrm[s - 1020u]; return true; } break;
    case 1023: if (c->model == PPC_MODEL_604 || c->model == PPC_MODEL_604E) { *value = 0; return true; } break;
    default: break;
    }
    return false;
}

bool ppc_model_spr_write(ppc_cpu *c, unsigned s, uint32_t value) {
    unsigned flags = models[c->model].features;
    if (s >= 528 && s <= 535) { c->bat[(s - 528u) / 2u][s & 1u] = value; return true; }
    if (s >= 536 && s <= 543) { c->dbat[(s - 536u) / 2u][s & 1u] = value; return true; }
    switch (s) {
    case 1: c->xer = value & 0xe000007fu; return true;
    case 8: c->lr = value; return true;
    case 9: c->ctr = value; return true;
    case 18: c->dsisr = value; return true;
    case 19: c->dar = value; return true;
    case 22:
        if (!(c->dec & 0x80000000u) && (value & 0x80000000u)) c->decrementer_pending = true;
        c->dec = value; return true;
    case 25: c->sdr1 = value; return true;
    case 26: c->srr0 = value; return true;
    case 27: c->srr1 = value; return true;
    case 272: case 273: case 274: case 275: c->sprg[s - 272u] = value; return true;
    case 282: c->ear = value; return true;
    case 284: c->time_base = (c->time_base & UINT64_C(0xffffffff00000000)) | value; return true;
    case 285: c->time_base = ((uint64_t)value << 32) | (uint32_t)c->time_base; return true;
    case 976: if (flags & PPC_MODEL_SOFT_TLB) { c->dmiss = value; return true; } break;
    case 977: if (flags & PPC_MODEL_SOFT_TLB) { c->dcmp = value & 0xffffffbfu; return true; } break;
    case 980: if (flags & PPC_MODEL_SOFT_TLB) { c->imiss = value; return true; } break;
    case 981: if (flags & PPC_MODEL_SOFT_TLB) { c->icmp = value & 0xffffffbfu; return true; } break;
    case 982: if (flags & PPC_MODEL_SOFT_TLB) { c->rpa = value & 0xfffff1fbu; return true; } break;
    case 952: if (flags & PPC_MODEL_PMC2) { c->mmcr[0] = value; return true; } break;
    case 953: case 954: if (flags & PPC_MODEL_PMC2) { c->pmc[s - 953u] = value; return true; } break;
    case 955: if (flags & PPC_MODEL_PMC2) { c->sia = value; return true; } break;
    case 956: if (flags & PPC_MODEL_PMC4) { c->mmcr[1] = value; return true; } break;
    case 957: case 958: if (flags & PPC_MODEL_PMC4) { c->pmc[s - 955u] = value; return true; } break;
    case 959: if (c->model == PPC_MODEL_604 || c->model == PPC_MODEL_604E) { c->sda = value; return true; } break;
    case 1008:
        /* Cache flash-invalidate requests complete immediately in the coherent interpreter. */
        c->hid[0] = value & ~0x00000c00u; return true;
    case 1009: return true; /* Read-only PLL configuration, sampled by the machine. */
    case 1010: c->iabr = value; return true;
    case 1013: if (!(flags & PPC_MODEL_SOFT_TLB)) { c->dabr = value; return true; } break;
    case 1017:
        if (flags & PPC_MODEL_L2) { c->l2cr = value & ~1u; return true; } break;
    case 1019: if (flags & PPC_MODEL_L2) { c->ictc = value & 0x1ffu; return true; } break;
    case 1020: case 1021:
        if (flags & PPC_MODEL_L2) { c->thrm[s - 1020u] = value & 0x3f800007u; return true; } break;
    case 1022: if (flags & PPC_MODEL_L2) { c->thrm[2] = value & 0x3fffu; return true; } break;
    case 1023: if (c->model == PPC_MODEL_604 || c->model == PPC_MODEL_604E) return true; break;
    default: break;
    }
    return false;
}

static bool privileged(ppc_cpu *c) {
    if (!(c->msr & PPC_MSR_PR)) return true;
    ppc_exception(c, PPC_EX_PROGRAM, 0x00040000u, c->last_pc); return false;
}
bool ppc_model_decode(ppc_cpu *c, uint32_t op) {
    unsigned xo = (op >> 1) & 1023u, d = (op >> 21) & 31u, b = (op >> 11) & 31u;
    if ((op >> 26) == 9 || (op >> 26) == 22) {
        ppc_exception(c, PPC_EX_PROGRAM, 0x00080000u, c->last_pc); return true;
    }
    if (op >> 26 != 31) return false;
    switch (xo) {
    case 360: case 872: case 488: case 1000: case 264: case 776:
    case 107: case 619: case 331: case 843: case 363: case 875:
    case 531: case 277: case 29: case 541: case 537:
    case 153: case 217: case 184: case 248: case 216: case 152:
    case 952: case 920: case 665: case 921: case 729: case 696:
    case 760: case 728: case 664:
        ppc_exception(c, PPC_EX_PROGRAM, 0x00080000u, c->last_pc); return true;
    case 371: {
        unsigned tbr = ((op >> 16) & 31u) | ((op >> 6) & 0x3e0u);
        if (tbr != 268 && tbr != 269) ppc_exception(c, PPC_EX_PROGRAM, 0x00080000u, c->last_pc);
        else c->gpr[d] = tbr == 268 ? (uint32_t)c->time_base : (uint32_t)(c->time_base >> 32);
        return true;
    }
    case 978: case 1010:
        if (!(models[c->model].features & PPC_MODEL_SOFT_TLB)) {
            ppc_exception(c, PPC_EX_PROGRAM, 0x00080000u, c->last_pc); return true;
        }
        if (privileged(c)) ppc_classic_tlb_load(c, c->gpr[b], xo == 1010);
        return true;
    case 306:
        if (privileged(c)) ppc_classic_tlb_invalidate(c, c->gpr[b]);
        return true;
    case 566: privileged(c); return true;
    case 983: {
        uint32_t ea, value, pa;
        unsigned a = (op >> 16) & 31u;
        if (!(c->msr & PPC_MSR_FP)) { ppc_exception(c, PPC_EX_FPU, 0, c->last_pc); return true; }
        ea = (a ? c->gpr[a] : 0u) + c->gpr[b]; value = (uint32_t)c->fpr[d];
        if (ea & 3u) ppc_alignment(c, ea);
        else if (ppc_translate(c, ea, PPC_WRITE, &pa) && ppc_data_breakpoint(c, ea, 4, true, false))
            ppc_store(c, ea, 4, value);
        return true;
    }
    default: return false;
    }
}
