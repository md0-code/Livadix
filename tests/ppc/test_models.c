/* Manual fixtures: MPC603eUM chapters 2, 4, 5; MPC604eUM chapters 2,
   4, 5; MPC750UM chapters 2, 4, 5. No external implementation was read. */
#include "test_support.h"
#include "ppc_model.h"

static ppc_cpu cpu;
static test_memory memory;

static void setup(ppc_model model) {
    ppc_bus bus = { &memory, test_read, test_write };
    memset(&memory, 0, sizeof(memory));
    ppc_init_model(&cpu, bus, model);
    cpu.pc = cpu.last_pc = 0x1000;
    cpu.msr = PPC_MSR_ME | PPC_MSR_FP;
}
static uint32_t spr_op(unsigned xo, unsigned reg, unsigned spr) {
    return test_x(xo, reg, spr & 31u, spr >> 5, false);
}
static ppc_result execute(uint32_t op) { return test_step(&cpu, &memory, op); }
static void registers(ppc_model model) {
    ppc_model parsed;
    setup(model);
    CHECK(ppc_model_parse(ppc_model_information(model)->name, &parsed));
    CHECK(parsed == model);
    CHECK(!ppc_model_parse("unknown", &parsed));
    cpu.hid[0] = UINT32_MAX; cpu.time_base = UINT64_MAX;
    ppc_reset(&cpu);
    CHECK(cpu.model == model && cpu.pc == 0xfff00100u);
    CHECK(cpu.msr == PPC_MSR_IP && cpu.dec == UINT32_MAX);
    CHECK(cpu.hid[0] == 0 && cpu.hid[1] == 0 && cpu.time_base == 0);
    CHECK(cpu.pvr == ppc_model_information(model)->pvr);
    cpu.msr = PPC_MSR_ME | PPC_MSR_FP;
    cpu.gpr[5] = 0x12345678u;
    CHECK(execute(spr_op(467, 5, 536)) == PPC_OK);
    CHECK(cpu.dbat[0][0] == 0x12345678u && cpu.bat[0][0] == 0);
    CHECK(execute(spr_op(339, 6, 536)) == PPC_OK && cpu.gpr[6] == cpu.gpr[5]);
    cpu.gpr[5] = UINT32_MAX;
    CHECK(execute(test_x(146, 5, 0, 0, false)) == PPC_OK);
    CHECK(cpu.msr == ppc_model_information(model)->msr_mask);
    setup(model);
    CHECK(execute(spr_op(339, 6, 0)) == PPC_EXCEPTION);
    CHECK(cpu.last_exception == PPC_EX_PROGRAM && (cpu.srr1 & 0x80000u));
    setup(model); cpu.msr |= PPC_MSR_PR;
    CHECK(execute(spr_op(339, 6, 287)) == PPC_EXCEPTION);
    CHECK(cpu.srr1 & 0x40000u);
    setup(model); cpu.gpr[6] = 0xa5a5a5a5u;
    CHECK(execute((9u << 26) | (6u << 21) | 2u) == PPC_EXCEPTION);
    CHECK(cpu.gpr[6] == 0xa5a5a5a5u && cpu.last_exception == PPC_EX_PROGRAM);
    setup(model);
    CHECK(execute(test_x(370, 0, 0, 0, false)) == PPC_EXCEPTION);
    setup(model);
    CHECK(execute(test_x(566, 0, 0, 0, false)) == PPC_OK);
}
static void clocks_and_exceptions(ppc_model model) {
    setup(model);
    cpu.time_base = UINT64_C(0x12345678fffffffe); cpu.dec = 1;
    ppc_clock(&cpu, 2);
    CHECK(cpu.time_base == UINT64_C(0x1234567900000000));
    CHECK(cpu.dec == UINT32_MAX && cpu.decrementer_pending);
    CHECK(execute(spr_op(371, 7, 269)) == PPC_OK && cpu.gpr[7] == 0x12345679u);
    CHECK(execute(spr_op(371, 7, 268)) == PPC_OK && cpu.gpr[7] == 0);
    cpu.gpr[8] = 0xabcdef01u;
    CHECK(execute(spr_op(467, 8, 284)) == PPC_OK);
    CHECK(cpu.time_base == UINT64_C(0x12345679abcdef01));
    CHECK(execute(spr_op(467, 8, 285)) == PPC_OK);
    CHECK(cpu.time_base == UINT64_C(0xabcdef01abcdef01));
    cpu.msr |= PPC_MSR_EE;
    CHECK(execute(0x60000000u) == PPC_EXCEPTION);
    CHECK(cpu.last_exception == PPC_EX_DECREMENTER && cpu.srr0 == 0x1000);
    setup(model); cpu.dec = 0x80000010u;
    ppc_clock(&cpu, 0x80000011u);
    CHECK(cpu.dec == UINT32_MAX && cpu.decrementer_pending);
    setup(model); cpu.msr |= PPC_MSR_ILE | PPC_MSR_RI | PPC_MSR_POW | PPC_MSR_EE;
    cpu.reserved = true;
    ppc_exception(&cpu, PPC_EX_EXTERNAL, 0, 0x12345678u);
    CHECK(cpu.srr0 == 0x12345678u && (cpu.srr1 & PPC_MSR_RI));
    CHECK((cpu.msr & (PPC_MSR_LE | PPC_MSR_ILE)) == (PPC_MSR_LE | PPC_MSR_ILE));
    CHECK(!(cpu.msr & (PPC_MSR_RI | PPC_MSR_POW | PPC_MSR_EE)) && !cpu.reserved);
    setup(model); cpu.msr |= PPC_MSR_BE;
    CHECK(execute(0x48000004u) == PPC_EXCEPTION);
    CHECK(cpu.last_exception == PPC_EX_TRACE && cpu.srr0 == 0x1004);
    setup(model); cpu.msr &= ~PPC_MSR_ME;
    ppc_exception(&cpu, PPC_EX_MACHINE, 0, cpu.last_pc);
    CHECK(cpu.result == PPC_CHECKSTOP && cpu.pc == cpu.last_pc);
}
static void bats(ppc_model model) {
    uint32_t pa = 0;
    setup(model); cpu.msr |= PPC_MSR_IR | PPC_MSR_DR;
    cpu.sr[3] = 0x90000000u;
    cpu.bat[0][0] = 0x30000002u; cpu.bat[0][1] = 0x00020002u;
    cpu.dbat[0][0] = 0x30000002u; cpu.dbat[0][1] = 0x00040002u;
    CHECK(ppc_translate(&cpu, 0x30001234u, PPC_FETCH, &pa) && pa == 0x21234u);
    CHECK(ppc_translate(&cpu, 0x30001234u, PPC_READ, &pa) && pa == 0x41234u);
    cpu.dbat[0][1] = 0x40001u;
    CHECK(!ppc_translate(&cpu, 0x30001234u, PPC_WRITE, &pa));
    CHECK(cpu.last_exception == PPC_EX_DSI && cpu.dar == 0x30001234u && cpu.dsisr == 0x0a000000u);
    setup(model); cpu.msr |= PPC_MSR_DR | PPC_MSR_PR;
    cpu.dbat[0][0] = 0x30000001u; cpu.dbat[0][1] = 0x20002u;
    CHECK(ppc_translate(&cpu, 0x30001234u, PPC_WRITE, &pa) && pa == 0x21234u);
    for (unsigned bl = 0; bl <= 2047; bl = bl ? (bl << 1) | 1u : 1u) {
        uint32_t mask = (bl << 17) | 0x1ffffu;
        setup(model); cpu.msr |= PPC_MSR_DR;
        cpu.dbat[0][0] = 0x40000002u | (bl << 2); cpu.dbat[0][1] = 2;
        CHECK(ppc_translate(&cpu, 0x40000000u | mask, PPC_READ, &pa));
        CHECK(pa == mask);
        if (bl == 2047) break;
    }
    setup(model); cpu.msr |= PPC_MSR_IR; cpu.sr[3] = 0x10000001u;
    CHECK(!ppc_translate(&cpu, 0x30001000u, PPC_FETCH, &pa));
    CHECK(cpu.last_exception == PPC_EX_ISI && (cpu.srr1 & 0x10000000u));
    setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = 0x80000000u;
    CHECK(!ppc_translate(&cpu, 0x30001000u, PPC_READ, &pa));
    CHECK(cpu.dsisr == 0x04000000u && cpu.last_exception == PPC_EX_DSI);
}
static void software_tlb(ppc_model model) {
    const uint32_t ea = 0x30001234u, vsid = 0x12345u;
    uint32_t pa = 0;
    setup(model); cpu.msr |= PPC_MSR_DR;
    cpu.sr[3] = vsid; cpu.sdr1 = 0x10000; cpu.cr = 0xa1234567u;
    for (unsigned i = 0; i < 4; ++i) cpu.gpr[i] = 0xa0u + i;
    CHECK(!ppc_translate(&cpu, ea, PPC_READ, &pa));
    CHECK(cpu.last_exception == PPC_EX_DTLB_LOAD && cpu.srr0 == 0x1000);
    CHECK(cpu.srr1 == (0xa0000000u | PPC_MSR_DR | PPC_MSR_ME | PPC_MSR_FP));
    CHECK(cpu.msr == (PPC_MSR_TGPR | PPC_MSR_ME));
    CHECK(cpu.dmiss == ea && cpu.dcmp == (0x80000000u | (vsid << 7)));
    CHECK(cpu.hash1 == (0x10000u | (((vsid ^ 1u) & 0x3ffu) << 6)));
    CHECK(cpu.hash2 == (0x10000u | ((~(vsid ^ 1u) & 0x3ffu) << 6)));
    CHECK(memory.reads == 0 && memory.writes == 0);
    for (unsigned i = 0; i < 4; ++i) CHECK(cpu.gpr[i] == 0 && cpu.tgpr[i] == 0xa0u + i);
    cpu.rpa = 0x20002u; cpu.gpr[2] = ea;
    CHECK(execute(test_x(978, 0, 0, 2, false)) == PPC_OK);
    CHECK(cpu.tlb[1][1][0].valid && cpu.tlb[1][1][0].page == 1);
    CHECK(execute(0x4c000064u) == PPC_OK);
    CHECK((cpu.msr & PPC_MSR_DR) && !(cpu.msr & PPC_MSR_TGPR));
    for (unsigned i = 0; i < 4; ++i) CHECK(cpu.gpr[i] == 0xa0u + i);
    CHECK(ppc_translate(&cpu, ea, PPC_READ, &pa) && pa == 0x20234u);
    CHECK(!ppc_translate(&cpu, ea, PPC_WRITE, &pa));
    CHECK(cpu.last_exception == PPC_EX_DTLB_STORE && (cpu.srr1 & 0x10000u));
    CHECK(!(cpu.srr1 & 0x20000u));
    cpu.rpa |= 0x80u; cpu.gpr[2] = ea;
    CHECK(execute(test_x(978, 0, 0, 2, false)) == PPC_OK);
    CHECK(execute(0x4c000064u) == PPC_OK);
    CHECK(ppc_translate(&cpu, ea, PPC_WRITE, &pa) && pa == 0x20234u);
    cpu.gpr[2] = ea;
    CHECK(execute(test_x(306, 0, 0, 2, false)) == PPC_OK);
    CHECK(!cpu.tlb[1][1][0].valid && !cpu.tlb[1][1][1].valid);
    setup(model); cpu.msr |= PPC_MSR_IR; cpu.sr[3] = 0x40000001u;
    cpu.pc = cpu.last_pc = 0x30002340u;
    CHECK(!ppc_translate(&cpu, cpu.pc, PPC_FETCH, &pa));
    CHECK(cpu.last_exception == PPC_EX_ITLB && (cpu.srr1 & 0x000c0000u) == 0x000c0000u);
    cpu.rpa = 0x20002u; cpu.gpr[2] = cpu.imiss;
    CHECK(execute(test_x(1010, 0, 0, 2, false)) == PPC_OK);
    CHECK(cpu.tlb[0][2][0].valid && !cpu.tlb[1][2][0].valid);
    setup(model); cpu.msr |= PPC_MSR_DR;
    cpu.dmiss = 0x003ff000u; cpu.dcmp = 0x8000018fu; cpu.rpa = 0x20082u;
    cpu.srr1 = 0x20000u;
    ppc_classic_tlb_load(&cpu, cpu.dmiss, false);
    CHECK(cpu.tlb[1][31][1].page == 0x3fffu && cpu.tlb[1][31][1].vsid == 3);
    cpu.tlb[0][31][0].valid = cpu.tlb[0][31][1].valid = cpu.tlb[1][31][0].valid = true;
    ppc_classic_tlb_invalidate(&cpu, 0x1f000u);
    for (unsigned w = 0; w < 2; ++w) for (unsigned way = 0; way < 2; ++way)
        CHECK(!cpu.tlb[w][31][way].valid);
}
static void hardware_hash(ppc_model model) {
    const uint32_t ea = 0x30004000u, vsid = 17u;
    uint32_t pa, value;
    for (unsigned secondary = 0; secondary < 2; ++secondary) {
        uint32_t hash = secondary ? ~(vsid ^ 4u) : vsid ^ 4u;
        uint32_t pte = 0x10000u | ((hash & 0x3ffu) << 6);
        pte += 7u * 8u;
        setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = vsid; cpu.sdr1 = 0x10000u;
        test_write(&memory, pte, 4, 0x80000000u | (vsid << 7) | (secondary << 6));
        test_write(&memory, pte + 4u, 4, 0x20002u);
        CHECK(ppc_translate(&cpu, ea, PPC_READ, &pa) && pa == 0x20000u);
        test_read(&memory, pte + 4u, 4, &value); CHECK(value == 0x20102u);
        CHECK(ppc_translate(&cpu, ea, PPC_WRITE, &pa) && pa == 0x20000u);
        test_read(&memory, pte + 4u, 4, &value); CHECK(value == 0x20182u);
    }
    for (unsigned key = 0; key < 2; ++key) for (unsigned pp = 0; pp < 4; ++pp)
        for (unsigned store = 0; store < 2; ++store) {
            bool allowed = !(key && pp == 0) && !(store && (pp == 3 || (key && pp == 1)));
            uint32_t pte = 0x10000u | (((vsid ^ 4u) & 0x3ffu) << 6);
            setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = vsid | (key ? 0x60000000u : 0u);
            cpu.sdr1 = 0x10000u;
            test_write(&memory, pte, 4, 0x80000000u | (vsid << 7));
            test_write(&memory, pte + 4u, 4, 0x20000u | pp);
            CHECK(ppc_translate(&cpu, ea, store ? PPC_WRITE : PPC_READ, &pa) == allowed);
            if (!allowed) CHECK(cpu.dsisr == (0x08000000u | (store ? 0x02000000u : 0u)));
        }
    setup(model); cpu.msr |= PPC_MSR_DR;
    CHECK(!ppc_translate(&cpu, ea, PPC_READ, &pa));
    CHECK(cpu.dsisr == 0x40000000u && cpu.last_exception == PPC_EX_DSI);
    setup(model); cpu.msr |= PPC_MSR_DR; memory.fail_read = true;
    CHECK(!ppc_translate(&cpu, ea, PPC_READ, &pa));
    CHECK(cpu.last_exception == PPC_EX_MACHINE);
}
static void diagnostics(ppc_model model) {
    setup(model); cpu.gpr[5] = 0x12345678u;
    if (model == PPC_MODEL_604 || model == PPC_MODEL_604E) {
        CHECK(execute(spr_op(467, 5, 959)) == PPC_OK);
        CHECK(execute(spr_op(339, 6, 959)) == PPC_OK && cpu.gpr[6] == 0x12345678u);
    } else {
        CHECK(execute(spr_op(467, 5, 959)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_PROGRAM);
        CHECK(execute(spr_op(339, 6, 959)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_PROGRAM);
    }
    setup(model); cpu.gpr[5] = UINT32_MAX;
    CHECK(execute(spr_op(467, 5, 1009)) == PPC_OK && cpu.hid[1] == 0);
    CHECK(execute(spr_op(467, 5, 1008)) == PPC_OK && !(cpu.hid[0] & 0xc00u));
    if (model == PPC_MODEL_750) {
        cpu.gpr[5] = 0x00200001u;
        CHECK(execute(spr_op(467, 5, 1017)) == PPC_OK && cpu.l2cr == 0x00200000u);
        cpu.gpr[5] = UINT32_MAX;
        CHECK(execute(spr_op(467, 5, 1019)) == PPC_OK && cpu.ictc == 0x1ffu);
        for (unsigned i = 0; i < 4; ++i) cpu.pmc[i] = 0x100u + i;
        cpu.mmcr[0] = 0x12345678u; cpu.mmcr[1] = 0xabcdef01u; cpu.sia = 0x87654321u;
        cpu.msr |= PPC_MSR_PR;
        CHECK(execute(spr_op(339, 5, 936)) == PPC_OK && cpu.gpr[5] == cpu.mmcr[0]);
        CHECK(execute(spr_op(339, 5, 940)) == PPC_OK && cpu.gpr[5] == cpu.mmcr[1]);
        CHECK(execute(spr_op(339, 5, 939)) == PPC_OK && cpu.gpr[5] == cpu.sia);
        for (unsigned i = 0; i < 4; ++i) {
            unsigned number = i < 2 ? 937u + i : 939u + i;
            CHECK(execute(spr_op(339, 5, number)) == PPC_OK && cpu.gpr[5] == cpu.pmc[i]);
        }
        CHECK(execute(spr_op(467, 5, 936)) == PPC_EXCEPTION && (cpu.srr1 & 0x80000u));
    }
}
int main(void) {
    for (ppc_model model = PPC_MODEL_603; model < PPC_MODEL_COUNT; model = (ppc_model)(model + 1)) {
        registers(model); clocks_and_exceptions(model); bats(model); diagnostics(model);
        if (ppc_model_information(model)->features & PPC_MODEL_SOFT_TLB) software_tlb(model);
        else hardware_hash(model);
    }
    return test_report("PowerPC later models");
}
