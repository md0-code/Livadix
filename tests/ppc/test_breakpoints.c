/* MPC601UM 2.3.3.12.2-4 (tables 2-23..26), 5.4.3, 5.4.12 and the addendum
   (tables 5-24..27); MPC603EUM 2.1.2.6, 4.5.15; MPC604EUM 2.1.2.1, 4.5.14;
   MPC750UM 2.1.2.1, 4.5.3, 4.5.14, table 4-3; PEM 2.3.14, 6.4.3 table 6-9. */
#include "test_support.h"
#include "ppc_model.h"

static ppc_cpu cpu;
static test_memory memory;
#define C (&cpu)
#define DATA 0x2000u

static void setup(ppc_model model)
{
    ppc_bus bus = { &memory, test_read, test_write };
    memset(&memory, 0, sizeof(memory));
    ppc_init_model(&cpu, bus, model);
    cpu.msr = PPC_MSR_ME | PPC_MSR_FP;
    cpu.pc = cpu.last_pc = 0x1000;
}
static ppc_result step(uint32_t op) { return test_step(&cpu, &memory, op); }
static uint32_t dform(unsigned op, unsigned d, unsigned a, int imm)
{
    return (op << 26) | (d << 21) | (a << 16) | ((uint32_t)imm & 0xffffu);
}
static uint32_t mtspr(unsigned spr, unsigned s) { return test_x(467, s, spr & 31u, spr >> 5, false); }
static uint32_t mfspr(unsigned spr, unsigned d) { return test_x(339, d, spr & 31u, spr >> 5, false); }
static uint32_t word(uint32_t address)
{
    uint32_t v = 0;
    test_read(&memory, address, 4, &v);
    return v;
}
/* A DSI from the data breakpoint with nothing else changed. */
static void expect_dabr(uint32_t dar, bool store)
{
    CHECK(cpu.last_exception == PPC_EX_DSI);
    CHECK(cpu.dar == dar);
    CHECK(cpu.dsisr == (PPC_DSISR_DABR | (store ? PPC_DSISR_STORE : 0u)));
    CHECK(cpu.srr0 == 0x1000 && cpu.pc == 0x300);
}

static void run_mode_601(void)
{
    static const uint32_t responses[] = { 0x00400000u, 0x00800000u };
    /* Instruction address compare, limited (010) and full (110), with both
       documented trap encodings of HID1[8-9]. */
    for (unsigned mode = 0; mode < 2; ++mode) for (unsigned r = 0; r < 2; ++r) {
        setup(PPC_MODEL_601);
        cpu.hid[1] = (mode ? 0x60000000u : 0x20000000u) | responses[r];
        cpu.hid[2] = 0x1000u | 3u;
        CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION);
        CHECK(cpu.last_exception == PPC_EX_RUN_MODE && cpu.pc == 0x2000);
        CHECK(cpu.srr0 == 0x1000 && cpu.srr1 == (PPC_MSR_ME | PPC_MSR_FP));
        CHECK(cpu.gpr[3] == 0 && cpu.msr == PPC_MSR_ME && cpu.instructions == 0);
    }
    /* MSR[IP] selects the high vector base; SRR1 holds the whole MSR. */
    setup(PPC_MODEL_601); cpu.msr |= PPC_MSR_IP | PPC_MSR_EE | PPC_MSR_SE;
    cpu.hid[1] = 0x60800000u; cpu.hid[2] = 0x1000u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION && cpu.pc == 0xfff02000u);
    CHECK(cpu.srr1 == (PPC_MSR_ME | PPC_MSR_FP | PPC_MSR_IP | PPC_MSR_EE | PPC_MSR_SE));
    CHECK(cpu.msr == (PPC_MSR_ME | PPC_MSR_IP));
    /* Another address, normal run mode and hard stop: none traps, the
       instruction executes. */
    setup(PPC_MODEL_601); cpu.hid[1] = 0x60800000u; cpu.hid[2] = 0x1004u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x00800000u; cpu.hid[2] = 0x1000u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x60000000u; cpu.hid[2] = 0x1000u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    /* Full branch target compare (111): b, a taken bc, blr and bcctr match
       on their target before the branch executes; a not-taken bc, a
       mismatching target and a non-branch at the address do not. */
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u;
    CHECK(step(0x48000100u) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_RUN_MODE);
    CHECK(cpu.srr0 == 0x1000 && cpu.pc == 0x2000);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u; cpu.cr = 0x20000000u;
    CHECK(step(dform(16, 12, 2, 0x100)) == PPC_EXCEPTION && cpu.srr0 == 0x1000);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u;
    CHECK(step(dform(16, 12, 2, 0x100)) == PPC_OK && cpu.pc == 0x1004);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u; cpu.ctr = 1;
    CHECK(step(dform(16, 16, 0, 0x100)) == PPC_OK && cpu.pc == 0x1004 && cpu.ctr == 0);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u; cpu.lr = 0x1100u;
    CHECK(step(0x4e800020u) == PPC_EXCEPTION && cpu.srr0 == 0x1000 && cpu.lr == 0x1100);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1100u; cpu.ctr = 0x1100u;
    CHECK(step(0x4e800420u) == PPC_EXCEPTION && cpu.srr0 == 0x1000);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1104u;
    CHECK(step(0x48000100u) == PPC_OK && cpu.pc == 0x1100);
    setup(PPC_MODEL_601); cpu.hid[1] = 0x70800000u; cpu.hid[2] = 0x1000u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    /* The 601's MSR[SE] trace is the run mode exception, SRR1[0-15] clear. */
    setup(PPC_MODEL_601); cpu.msr |= PPC_MSR_SE;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_RUN_MODE);
    CHECK(cpu.srr0 == 0x1004 && cpu.gpr[3] == 1 && cpu.pc == 0x2000);
    /* SPR 1010 and 1013 are HID2 and HID5. */
    setup(PPC_MODEL_601); cpu.gpr[3] = 0x12345678u;
    CHECK(step(mtspr(1010, 3)) == PPC_OK && cpu.hid[2] == 0x12345678u);
    CHECK(step(mtspr(1013, 3)) == PPC_OK && cpu.hid[5] == 0x12345678u);
    CHECK(step(mfspr(1013, 4)) == PPC_OK && cpu.gpr[4] == 0x12345678u);
}

static void data_601(void)
{
    /* Loads only (SA = 01): the doubleword at DATA; DT and bit 29 ignored. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 4u | 1u; cpu.gpr[4] = DATA;
    test_write(&memory, DATA + 4, 4, 0xcafef00du);
    CHECK(step(dform(32, 3, 4, 4)) == PPC_EXCEPTION); expect_dabr(DATA + 4, false);
    CHECK(cpu.gpr[3] == 0);
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 1u; cpu.gpr[4] = DATA; cpu.gpr[3] = 7;
    CHECK(step(dform(36, 3, 4, 4)) == PPC_OK && word(DATA + 4) == 7);
    CHECK(step(dform(34, 3, 4, 8)) == PPC_OK);
    CHECK(step(dform(34, 3, 4, -1)) == PPC_OK);
    CHECK(step(dform(34, 3, 4, 7)) == PPC_EXCEPTION); expect_dabr(DATA + 7, false);
    /* An access touching the doubleword from below reports its own EA. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 1u; cpu.gpr[4] = DATA;
    CHECK(step(dform(32, 3, 4, -2)) == PPC_EXCEPTION); expect_dabr(DATA - 2, false);
    /* Load with update: rA not updated. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 1u; cpu.gpr[4] = DATA;
    CHECK(step(dform(33, 3, 4, 4)) == PPC_EXCEPTION && cpu.gpr[4] == DATA);
    /* Stores only (SA = 10): memory and rA unchanged, DSISR[6] set. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 2u; cpu.gpr[4] = DATA; cpu.gpr[3] = 7;
    CHECK(step(dform(37, 3, 4, 0)) == PPC_EXCEPTION); expect_dabr(DATA, true);
    CHECK(word(DATA) == 0 && cpu.gpr[4] == DATA);
    CHECK(step(dform(32, 3, 4, 0)) == PPC_OK);
    /* Disabled (SA = 00). */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA; cpu.gpr[4] = DATA;
    CHECK(step(dform(36, 3, 4, 0)) == PPC_OK && step(dform(32, 3, 4, 0)) == PPC_OK);
    /* Multiples: word by word, DAR = the matching word's EA, no register
       loaded; stores inhibited. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 3u; cpu.gpr[4] = DATA - 8;
    for (unsigned r = 29; r < 32; ++r) cpu.gpr[r] = r;
    CHECK(step(dform(46, 29, 4, 0)) == PPC_EXCEPTION); expect_dabr(DATA, false);
    CHECK(cpu.gpr[29] == 29 && cpu.gpr[31] == 31);
    CHECK(step(dform(47, 29, 4, 0)) == PPC_EXCEPTION); expect_dabr(DATA, true);
    CHECK(word(DATA - 8) == 0 && word(DATA) == 0);
    /* A string word straddling into the doubleword. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 1u; cpu.gpr[4] = DATA - 6;
    CHECK(step(test_x(597, 5, 4, 8, false)) == PPC_EXCEPTION); expect_dabr(DATA - 2, false);
    /* A zero-length string never matches. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 3u; cpu.gpr[4] = DATA; cpu.xer = 0;
    CHECK(step(test_x(533, 5, 0, 4, false)) == PPC_OK);
    /* lwarx matches; stwcx. only while it holds the reservation. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 3u; cpu.gpr[4] = DATA;
    CHECK(step(test_x(20, 3, 0, 4, false)) == PPC_EXCEPTION); expect_dabr(DATA, false);
    CHECK(!cpu.reserved);
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 2u; cpu.gpr[4] = DATA; cpu.gpr[3] = 9;
    CHECK(step(test_x(150, 3, 0, 4, true)) == PPC_OK && word(DATA) == 0 && (cpu.cr >> 28) == 0);
    cpu.reserved = true; cpu.reservation = DATA;
    CHECK(step(test_x(150, 3, 0, 4, true)) == PPC_EXCEPTION); expect_dabr(DATA, true);
    CHECK(word(DATA) == 0);
    /* Cache control (dcbz) never matches; floating-point accesses do. */
    setup(PPC_MODEL_601); cpu.hid[5] = DATA | 3u; cpu.gpr[4] = DATA;
    test_write(&memory, DATA, 4, 0xffffffffu);
    CHECK(step(test_x(1014, 0, 0, 4, false)) == PPC_OK && word(DATA) == 0);
    CHECK(step(dform(54, 1, 4, -8)) == PPC_OK);
    CHECK(step(dform(50, 1, 4, 0)) == PPC_EXCEPTION); expect_dabr(DATA, false);
}

static void later(ppc_model model)
{
    bool soft = (ppc_model_information(model)->features & PPC_MODEL_SOFT_TLB) != 0;
    /* IABR: word compare, BE enables, taken before the instruction. */
    setup(model); cpu.iabr = 0x1000u | 2u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_IABR);
    CHECK(cpu.srr0 == 0x1000 && cpu.pc == 0x1300 && (cpu.srr1 & 0xffffu) == (PPC_MSR_ME | PPC_MSR_FP));
    CHECK(cpu.gpr[3] == 0 && cpu.instructions == 0);
    setup(model); cpu.msr |= PPC_MSR_IP; cpu.iabr = 0x1000u | 2u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION && cpu.pc == 0xfff01300u);
    setup(model); cpu.iabr = 0x1000u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    setup(model); cpu.iabr = 0x1004u | 2u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_OK && cpu.gpr[3] == 1);
    /* TE (bit 31) against MSR[IR]: ignored by the 603/603e only. */
    setup(model); cpu.iabr = 0x1000u | 3u;
    CHECK(step(dform(14, 3, 0, 1)) == (soft ? PPC_EXCEPTION : PPC_OK));
    CHECK(cpu.gpr[3] == (soft ? 0u : 1u));
    /* The breakpoint precedes the single-step trace. */
    setup(model); cpu.msr |= PPC_MSR_SE; cpu.iabr = 0x1000u | 2u;
    CHECK(step(dform(14, 3, 0, 1)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_IABR);
    /* SPR 1010 reads back. */
    setup(model); cpu.gpr[3] = 0x00002002u;
    CHECK(step(mtspr(1010, 3)) == PPC_OK && cpu.iabr == 0x00002002u);
    CHECK(step(mfspr(1010, 4)) == PPC_OK && cpu.gpr[4] == 0x00002002u);
    if (soft) {
        /* No DABR on the 603 and 603e: SPR 1013 is illegal, no match. */
        setup(model);
        CHECK(step(mtspr(1013, 3)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_PROGRAM);
        setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA;
        CHECK(step(dform(32, 3, 4, 0)) == PPC_OK);
        return;
    }
    /* DABR: DR (bit 31) loads, DW (bit 30) stores, BT = MSR[DR]. */
    setup(model); cpu.gpr[3] = DATA | 1u;
    CHECK(step(mtspr(1013, 3)) == PPC_OK && cpu.dabr == (DATA | 1u));
    cpu.gpr[4] = DATA;
    CHECK(step(dform(32, 3, 4, 4)) == PPC_EXCEPTION); expect_dabr(DATA + 4, false);
    CHECK(cpu.gpr[3] == (DATA | 1u));
    setup(model); cpu.dabr = DATA | 1u; cpu.gpr[4] = DATA; cpu.gpr[3] = 5;
    CHECK(step(dform(36, 3, 4, 4)) == PPC_OK && word(DATA + 4) == 5);
    CHECK(step(dform(34, 3, 4, 8)) == PPC_OK);
    setup(model); cpu.dabr = DATA | 2u; cpu.gpr[4] = DATA; cpu.gpr[3] = 5;
    CHECK(step(dform(37, 3, 4, 2)) == PPC_EXCEPTION); expect_dabr(DATA + 2, true);
    CHECK(word(DATA) == 0 && cpu.gpr[4] == DATA);
    CHECK(step(dform(32, 3, 4, 0)) == PPC_OK);
    /* BT set while MSR[DR] is clear: no match. */
    setup(model); cpu.dabr = DATA | 4u | 3u; cpu.gpr[4] = DATA;
    CHECK(step(dform(32, 3, 4, 0)) == PPC_OK && step(dform(36, 3, 4, 0)) == PPC_OK);
    /* An unaligned word reaching into the doubleword (later models do it
       in hardware), lmw/stmw, stfiwx, lwarx; dcbz does not match. */
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA;
    CHECK(step(dform(32, 3, 4, -2)) == PPC_EXCEPTION); expect_dabr(DATA - 2, false);
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA - 8;
    CHECK(step(dform(47, 29, 4, 0)) == PPC_EXCEPTION); expect_dabr(DATA, true);
    CHECK(word(DATA - 8) == 0);
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA; cpu.fpr[1] = 0x1234;
    CHECK(step(test_x(983, 1, 0, 4, false)) == PPC_EXCEPTION); expect_dabr(DATA, true);
    CHECK(word(DATA) == 0);
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA;
    CHECK(step(test_x(20, 3, 0, 4, false)) == PPC_EXCEPTION); expect_dabr(DATA, false);
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA;
    test_write(&memory, DATA, 4, 0xffffffffu);
    CHECK(step(test_x(1014, 0, 0, 4, false)) == PPC_OK && word(DATA) == 0);
    /* Alignment ranks above the breakpoint (MPC750UM table 4-3). */
    setup(model); cpu.dabr = DATA | 3u; cpu.gpr[4] = DATA + 1;
    CHECK(step(dform(46, 29, 4, 0)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_ALIGNMENT);
}

int main(void)
{
    run_mode_601();
    data_601();
    for (ppc_model model = PPC_MODEL_603; model < PPC_MODEL_COUNT; model = (ppc_model)(model + 1))
        later(model);
    return test_report("PowerPC breakpoints");
}
