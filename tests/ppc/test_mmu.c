#include "test_support.h"

/* MPC601UM 6.4 tables 6-7/8, 6.7.3 tables 6-11/12, 6.8/6.9
   (601 BATs, segments, PTEGs); chapter 5 exception and alignment state. */
static void bat_permissions(ppc_cpu *c, test_memory *m)
{
    uint32_t pa;
    for (unsigned user = 0; user < 2; ++user) {
        for (unsigned key = 0; key < 2; ++key) {
            for (unsigned pp = 0; pp < 4; ++pp) {
                for (unsigned access = 0; access < 3; ++access) {
                    bool allowed = !(key && pp == 0) &&
                        (access != PPC_WRITE || (pp != 3 && !(key && pp == 1)));
                    test_setup(c, m);
                    c->msr |= PPC_MSR_DR | PPC_MSR_IR | (user ? PPC_MSR_PR : 0);
                    c->bat[0][0] = 0x40000000u | (key ? (user ? 4u : 8u) : 0u) | pp;
                    c->bat[0][1] = 0x00020040u;
                    CHECK(ppc_translate(c, 0x40001234, (ppc_access)access, &pa) == allowed);
                    if (allowed) { CHECK(pa == 0x21234); CHECK(m->reads == 0); }
                    else {
                        CHECK(c->last_exception == (access == PPC_FETCH ? PPC_EX_ISI : PPC_EX_DSI));
                        CHECK(c->srr0 == (access == PPC_FETCH ? 0x40001234u : 0x1000u));
                        CHECK((access == PPC_FETCH ? c->srr1 : c->dsisr) & 0x08000000u);
                        if (access != PPC_FETCH) CHECK(c->dar == 0x40001234);
                    }
                }
            }
        }
    }
    test_setup(c, m);
    c->msr |= PPC_MSR_DR;
    c->bat[0][0] = 0x80000002u;
    c->bat[0][1] = 0x40u | 63u;
    CHECK(ppc_translate(c, 0x807fffff, PPC_READ, &pa)); CHECK(pa == 0x7fffff);
    c->bat[0][1] = 0x42u;
    CHECK(!ppc_translate(c, 0x80000000, PPC_READ, &pa)); CHECK(c->result == PPC_UNIMPLEMENTED);
}
static uint32_t pte(ppc_cpu *c, test_memory *m, uint32_t ea, uint32_t vsid,
                    bool secondary, unsigned slot, uint32_t low)
{
    uint32_t index = (ea >> 12) & 65535u;
    uint32_t hash = vsid ^ index;
    uint32_t mask = ((c->sdr1 & 511u) << 10) | 1023u;
    uint32_t group = (c->sdr1 & 0xffff0000u) | (((secondary ? ~hash : hash) & mask) << 6);
    uint32_t address = group + slot * 8;
    test_write(m, address, 4, 0x80000000u | (vsid << 7) | (secondary ? 64u : 0u) | (index >> 10));
    test_write(m, address + 4, 4, low);
    return address;
}
static void hash_pages(ppc_cpu *c, test_memory *m)
{
    const uint32_t ea = 0x12345234u, vsid = 0x00abcdeu;
    uint32_t pa, address, low;
    for (unsigned secondary = 0; secondary < 2; ++secondary) {
        for (unsigned slot = 0; slot < 8; ++slot) {
            test_setup(c, m);
            c->msr |= PPC_MSR_DR; c->sdr1 = 0x10000; c->sr[1] = vsid;
            address = pte(c, m, ea, vsid, secondary != 0, slot, 0x23002);
            CHECK(ppc_translate(c, ea, PPC_READ, &pa)); CHECK(pa == 0x23234);
            test_read(m, address + 4, 4, &low); CHECK(low == 0x23102);
            CHECK(ppc_translate(c, ea, PPC_WRITE, &pa));
            test_read(m, address + 4, 4, &low); CHECK(low == 0x23182);
        }
    }
    test_setup(c, m); c->msr |= PPC_MSR_DR | PPC_MSR_PR; c->sdr1 = 0x10000; c->sr[1] = vsid | 0x20000000u;
    address = pte(c, m, ea, vsid, false, 7, 0x23001);
    CHECK(!ppc_translate(c, ea, PPC_WRITE, &pa)); CHECK(c->dsisr == 0x0a000000u); CHECK(c->dar == ea);
    test_read(m, address + 4, 4, &low); CHECK(low == 0x23101);
    test_setup(c, m); c->msr |= PPC_MSR_IR; c->sdr1 = 0x10000; c->sr[1] = vsid;
    CHECK(!ppc_translate(c, ea, PPC_FETCH, &pa)); CHECK(c->last_exception == PPC_EX_ISI);
    CHECK(c->srr1 == (0x40200000u | PPC_MSR_ME | PPC_MSR_FP | PPC_MSR_IR));
    test_setup(c, m); c->msr |= PPC_MSR_DR; c->sdr1 = 0x10000; c->sr[1] = vsid;
    m->fail_read = true;
    CHECK(!ppc_translate(c, ea, PPC_READ, &pa)); CHECK(c->last_exception == PPC_EX_MACHINE);
    CHECK(c->srr0 == 0x1000); CHECK(!(c->msr & PPC_MSR_ME));
}
static void bypass_and_lanes(ppc_cpu *c, test_memory *m)
{
    uint32_t pa, value;
    test_setup(c, m); c->sr[8] = 0x80000000u;
    CHECK(ppc_translate(c, 0x81234567, PPC_FETCH, &pa)); CHECK(pa == 0x81234567);
    CHECK(!ppc_translate(c, 0x81234567, PPC_READ, &pa)); CHECK(c->result == PPC_UNIMPLEMENTED);
    c->result = PPC_OK; c->sr[8] = 0x87f00002u;
    CHECK(ppc_translate(c, 0x81234567, PPC_READ, &pa)); CHECK(pa == 0x21234567);
    c->msr |= PPC_MSR_IR;
    CHECK(ppc_translate(c, 0x81234567, PPC_FETCH, &pa)); CHECK(pa == 0x21234567);
    test_setup(c, m);
    for (unsigned i = 0; i < 16; ++i) m->bytes[0x2000 + i] = (uint8_t)i;
    CHECK(ppc_load(c, 0x2001, 4, &value)); CHECK(value == 0x01020304u);
    c->hid[0] |= 8u;
    CHECK(ppc_translate(c, 0x2000, PPC_FETCH, &pa)); CHECK(pa == 0x2004);
    CHECK(ppc_load(c, 0x2000, 1, &value)); CHECK(value == 7);
    CHECK(ppc_load(c, 0x2000, 2, &value)); CHECK(value == 0x0607);
    CHECK(ppc_load(c, 0x2000, 4, &value)); CHECK(value == 0x04050607);
    CHECK(!ppc_load(c, 0x2001, 4, &value)); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
}
static void alignment_and_cache(ppc_cpu *c, test_memory *m)
{
    uint32_t value;
    test_setup(c, m); c->gpr[1] = 0x0ffffffe;
    CHECK(test_step(c, m, (32u << 26) | (3u << 21) | (1u << 16)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT); CHECK(c->dar == 0x0ffffffe);
    CHECK(c->dsisr == (3u << 5 | 1u)); CHECK(m->writes == 0);
    test_setup(c, m); c->msr |= PPC_MSR_DR; c->bat[0][0] = 2; c->bat[0][1] = 0x40;
    CHECK(!ppc_load(c, 0xfff, 2, &value)); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->gpr[1] = 0xffb; c->fpr[3] = UINT64_MAX; c->msr |= PPC_MSR_DR;
    c->bat[0][0] = 2; c->bat[0][1] = 0x40;
    CHECK(test_step(c, m, (54u << 26) | (3u << 21) | (1u << 16)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT); CHECK(m->writes == 0);
    test_setup(c, m); c->hid[0] |= 8;
    /* LE fetch exchanges the two words; arrange the opcode in its fetch lane. */
    test_write(m, 0x1004, 4, (46u << 26) | (30u << 21) | (1u << 16)); c->gpr[1] = 0x2000;
    CHECK(ppc_step(c) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->sr[0] = 0x80000000u; c->gpr[1] = 0x2000;
    CHECK(test_step(c, m, test_x(1014, 0, 0, 1, false)) == PPC_OK); CHECK(m->writes == 0);
    test_setup(c, m); c->msr |= PPC_MSR_DR; c->sdr1 = 0x10000;
    pte(c, m, 0x2000, 0, false, 0, 0x2000 | 2u | 0x20u); c->gpr[1] = 0x2000;
    memset(m->bytes + 0x2000, 0xaa, 32);
    CHECK(test_step(c, m, test_x(1014, 0, 0, 1, false)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT); CHECK(m->bytes[0x2000] == 0xaa);
    test_setup(c, m); c->gpr[1] = 0x2001;
    CHECK(test_step(c, m, test_x(20, 3, 0, 1, false)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT); CHECK(!c->reserved);
    CHECK(c->dsisr == (3u << 5));
    test_setup(c, m); c->reserved = true; c->gpr[1] = 0x2001;
    CHECK(test_step(c, m, test_x(150, 3, 0, 1, true)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT); CHECK(!c->reserved); CHECK(m->writes == 0);
    test_setup(c, m); c->reserved = true; c->reservation = 0x2000; c->gpr[1] = 0x2040; c->gpr[3] = 42;
    /* MPC601UM 3.5.7 explicitly permits a different conditional-store EA. */
    CHECK(test_step(c, m, test_x(150, 3, 0, 1, true)) == PPC_OK);
    CHECK(c->cr >> 28 == 2); CHECK(ppc_load(c, 0x2040, 4, &value)); CHECK(value == 42);
    c->reserved = true;
    CHECK(test_step(c, m, 0x44000002u) == PPC_EXCEPTION); CHECK(!c->reserved);
    test_setup(c, m); c->gpr[1] = 0x2000; c->sr[0] = 0x80000000u;
    CHECK(test_step(c, m, (50u << 26) | (3u << 21) | (1u << 16)) == PPC_EXCEPTION);
    CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->gpr[1] = 0xff9; c->sr[0] = 0x87f00000u;
    CHECK(test_step(c, m, (54u << 26) | (3u << 21) | (1u << 16)) == PPC_EXCEPTION);
    CHECK(m->writes == 0); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->gpr[1] = 0xffd; c->msr |= PPC_MSR_DR;
    c->bat[0][0] = 2; c->bat[0][1] = 0x40;
    CHECK(test_step(c, m, (47u << 26) | (30u << 21) | (1u << 16)) == PPC_EXCEPTION);
    CHECK(m->writes == 0); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->gpr[1] = 0xffc; c->gpr[30] = 42; c->gpr[31] = 43; c->msr |= PPC_MSR_DR;
    c->bat[0][0] = 2; c->bat[0][1] = 0x40;
    CHECK(test_step(c, m, (47u << 26) | (30u << 21) | (1u << 16)) == PPC_OK);
    CHECK(ppc_load(c, 0xffc, 4, &value)); CHECK(value == 42);
    CHECK(ppc_load(c, 0x1000, 4, &value)); CHECK(value == 43);
}
int main(void)
{
    static test_memory memory;
    ppc_cpu cpu;
    bat_permissions(&cpu, &memory); hash_pages(&cpu, &memory);
    bypass_and_lanes(&cpu, &memory); alignment_and_cache(&cpu, &memory);
    return test_report("601 MMU");
}
