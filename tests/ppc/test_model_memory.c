/* MPC603eUM 4.5.6/5.5; MPC604eUM 3.1.5/4.3.6; MPC750UM 4.5.6/5. */
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
static void page(uint32_t ea, uint32_t pa, unsigned pp) {
    uint32_t vsid = cpu.sr[ea >> 28] & 0xffffffu;
    uint32_t compare = 0x80000000u | (vsid << 7) | ((ea >> 22) & 63u);
    if (ppc_model_information(cpu.model)->features & PPC_MODEL_SOFT_TLB) {
        cpu.dmiss = ea; cpu.dcmp = compare; cpu.rpa = pa | pp | 0x80u; cpu.srr1 = 0;
        ppc_classic_tlb_load(&cpu, ea, false);
    } else {
        uint32_t hash = vsid ^ ((ea >> 12) & 0xffffu);
        uint32_t group = 0x10000u | ((hash & 0x3ffu) << 6);
        test_write(&memory, group, 4, compare);
        test_write(&memory, group + 4u, 4, pa | pp);
    }
}
static void alignment(ppc_model model) {
    uint32_t value = 0;
    bool early = model == PPC_MODEL_603 || model == PPC_MODEL_603E || model == PPC_MODEL_604;
    setup(model);
    for (unsigned i = 0; i < 32; ++i) memory.bytes[i] = (uint8_t)('A' + i);
    CHECK(ppc_load(&cpu, 3, 2, &value) && value == 0x4445u);
    CHECK(ppc_load(&cpu, 6, 4, &value) && value == 0x4748494au);
    CHECK(ppc_store(&cpu, 7, 4, 0x11223344u));
    CHECK(memory.bytes[7] == 0x11 && memory.bytes[10] == 0x44);
    setup(model); cpu.msr |= PPC_MSR_LE;
    for (unsigned i = 0; i < 32; ++i) memory.bytes[i] = (uint8_t)('A' + i);
    CHECK(ppc_load(&cpu, 3, 2, &value) == !early);
    if (early) CHECK(cpu.last_exception == PPC_EX_ALIGNMENT && memory.reads == 0);
    else {
        CHECK(value == 0x4445u);
        CHECK(ppc_load(&cpu, 6, 4, &value) && value == 0x4f504142u);
        CHECK(ppc_store(&cpu, 6, 4, 0x11223344u));
        CHECK(memory.bytes[1] == 0x44 && memory.bytes[0] == 0x33);
        CHECK(memory.bytes[15] == 0x22 && memory.bytes[14] == 0x11);
    }
    setup(model); cpu.msr |= PPC_MSR_LE;
    CHECK(!ppc_preflight(&cpu, 0, 16, true, false));
    CHECK(cpu.last_exception == PPC_EX_ALIGNMENT);
    setup(model);
    CHECK(!ppc_preflight(&cpu, 2, 8, false, true));
    CHECK(cpu.last_exception == PPC_EX_ALIGNMENT);
    setup(model);
    CHECK(ppc_preflight(&cpu, 0xffc, 8, false, true));
    setup(model); cpu.gpr[2] = 3;
    CHECK(test_step(&cpu, &memory, test_x(20, 5, 0, 2, false)) == PPC_EXCEPTION);
    CHECK(cpu.last_exception == PPC_EX_ALIGNMENT);
    setup(model); cpu.gpr[2] = 3;
    CHECK(test_step(&cpu, &memory, (46u << 26) | (5u << 21) | (2u << 16)) == PPC_EXCEPTION);
    CHECK(cpu.last_exception == PPC_EX_ALIGNMENT);
    setup(model); cpu.msr |= PPC_MSR_LE; cpu.gpr[2] = 0x2000;
    test_write(&memory, 0x2000, 4, 0x01234567u); test_write(&memory, 0x2004, 4, 0x89abcdefu);
    /* LE instruction fetch exchanges the words of an aligned doubleword. */
    test_write(&memory, 0x1004, 4, (50u << 26) | (5u << 21) | (2u << 16));
    CHECK(test_step(&cpu, &memory, (50u << 26) | (5u << 21) | (2u << 16)) == PPC_OK);
    CHECK(cpu.fpr[5] == UINT64_C(0x0123456789abcdef));
    cpu.fpr[5] = UINT64_C(0x1122334455667788);
    test_write(&memory, 0x1004, 4, (54u << 26) | (5u << 21) | (2u << 16));
    CHECK(test_step(&cpu, &memory, (54u << 26) | (5u << 21) | (2u << 16)) == PPC_OK);
    CHECK(memory.bytes[0x2000] == 0x11 && memory.bytes[0x2007] == 0x88);
}
static void crossing(ppc_model model) {
    const uint32_t ea = 0x30000ffeu;
    uint32_t value = 0;
    setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = 1; cpu.sdr1 = 0x10000;
    page(ea, 0x21000, 2); page(ea + 2u, 0x30000, 2);
    memory.bytes[0x21ffe] = 0xaa; memory.bytes[0x21fff] = 0xbb;
    memory.bytes[0x30000] = 0xcc; memory.bytes[0x30001] = 0xdd;
    CHECK(ppc_load(&cpu, ea, 4, &value) && value == 0xaabbccddu);
    CHECK(ppc_store(&cpu, ea, 4, 0x11223344u));
    CHECK(memory.bytes[0x21ffe] == 0x11 && memory.bytes[0x21fff] == 0x22);
    CHECK(memory.bytes[0x30000] == 0x33 && memory.bytes[0x30001] == 0x44);
    setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = 0x60000001u; cpu.sdr1 = 0x10000;
    page(ea, 0x21000, 2); page(ea + 2u, 0x30000, 0);
    value = 0xdeadbeefu;
    CHECK(!ppc_load(&cpu, ea, 4, &value) && value == 0xdeadbeefu);
    CHECK(cpu.last_exception == PPC_EX_DSI && cpu.dar == ea + 2u && cpu.dsisr == 0x08000000u);
    setup(model); cpu.msr |= PPC_MSR_DR; cpu.sr[3] = 0x60000001u; cpu.sdr1 = 0x10000;
    page(ea, 0x21000, 2); page(ea + 2u, 0x30000, 0);
    CHECK(!ppc_store(&cpu, ea, 4, 0x11223344u));
    CHECK(cpu.last_exception == PPC_EX_DSI && cpu.dar == ea + 2u && cpu.dsisr == 0x0a000000u);
    CHECK(memory.bytes[0x30000] == 0 && memory.bytes[0x30001] == 0);
    if (model == PPC_MODEL_604E || model == PPC_MODEL_750) {
        setup(model); cpu.msr |= PPC_MSR_DR | PPC_MSR_LE; cpu.sr[3] = 1; cpu.sdr1 = 0x10000;
        page(ea, 0x21000, 2); page(ea + 2u, 0x30000, 2);
        memory.bytes[0x21ff9] = 0x11; memory.bytes[0x21ff8] = 0x22;
        memory.bytes[0x30007] = 0x33; memory.bytes[0x30006] = 0x44;
        CHECK(ppc_load(&cpu, ea, 4, &value) && value == 0x44332211u);
        CHECK(ppc_store(&cpu, ea, 4, 0xaabbccddu));
        CHECK(memory.bytes[0x21ff9] == 0xdd && memory.bytes[0x21ff8] == 0xcc);
        CHECK(memory.bytes[0x30007] == 0xbb && memory.bytes[0x30006] == 0xaa);
    }
}
static void fetch_fault(ppc_model model) {
    uint32_t pa;
    setup(model); cpu.msr |= PPC_MSR_IR; cpu.sr[3] = 0x10000001u;
    CHECK(!ppc_translate(&cpu, 0x30001234u, PPC_FETCH, &pa));
    CHECK(cpu.srr0 == 0x30001234u && cpu.last_exception == PPC_EX_ISI);
    if (ppc_model_information(model)->features & PPC_MODEL_SOFT_TLB) {
        setup(model); cpu.msr |= PPC_MSR_IR; cpu.sr[3] = 1;
        CHECK(!ppc_translate(&cpu, 0x30001234u, PPC_FETCH, &pa));
        CHECK(cpu.srr0 == 0x30001234u && cpu.last_exception == PPC_EX_ITLB);
    }
}
int main(void) {
    for (ppc_model model = PPC_MODEL_603; model < PPC_MODEL_COUNT; model = (ppc_model)(model + 1)) {
        alignment(model); crossing(model); fetch_fault(model);
    }
    return test_report("PowerPC later memory");
}
