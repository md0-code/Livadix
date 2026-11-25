#include "test_support.h"

/* MPC601UM chapter 3 tables 3-1, 3-8/9 and 3-41; chapter 10 POWER
   instructions. The duplicated shift descriptions disagree: table 3-9
   supplies the complete long-shift mask cases. No emulator source used. */
static void arithmetic(ppc_cpu *c, test_memory *m)
{
    test_setup(c, m); c->gpr[1] = 0x80000000u;
    CHECK(test_step(c, m, test_x(360 + 512, 3, 1, 0, true)) == PPC_OK);
    CHECK(c->gpr[3] == 0x80000000u); CHECK(c->xer == (PPC_XER_OV | PPC_XER_SO)); CHECK(c->cr >> 28 == 9);
    c->gpr[1] = 7;
    CHECK(test_step(c, m, test_x(488 + 512, 3, 1, 0, true)) == PPC_OK);
    CHECK(c->gpr[3] == 0xfffffff9u); CHECK(c->xer == PPC_XER_SO);
    c->gpr[1] = 4; c->gpr[2] = 9;
    CHECK(test_step(c, m, test_x(264, 3, 1, 2, false)) == PPC_OK); CHECK(c->gpr[3] == 5);
    CHECK(test_step(c, m, (9u << 26) | (3u << 21) | (1u << 16) | 2u) == PPC_OK); CHECK(c->gpr[3] == 0);
    CHECK(test_step(c, m, (9u << 26) | (3u << 21) | (1u << 16) | 7u) == PPC_OK); CHECK(c->gpr[3] == 3);
    c->gpr[1] = 0xfffffffeu; c->gpr[2] = 3;
    CHECK(test_step(c, m, test_x(107, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[3] == UINT32_MAX); CHECK(c->mq == 0xfffffffau); CHECK(c->cr >> 28 == 9);
    c->gpr[1] = 0xfffffff9u;
    CHECK(test_step(c, m, test_x(363, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[3] == 0xfffffffeu); CHECK(c->mq == UINT32_MAX); CHECK(c->cr >> 28 == 9);
    c->gpr[1] = 0; c->mq = 10;
    CHECK(test_step(c, m, test_x(331, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[3] == 3); CHECK(c->mq == 1); CHECK(c->cr >> 28 == 5);
    c->gpr[1] = UINT32_MAX; c->mq = 0xfffffff6u;
    CHECK(test_step(c, m, test_x(331, 3, 1, 2, false)) == PPC_OK);
    CHECK(c->gpr[3] == 0xfffffffdu); CHECK(c->mq == UINT32_MAX);
    c->gpr[1] = 0x80000000u; c->gpr[2] = UINT32_MAX;
    CHECK(test_step(c, m, test_x(363 + 512, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[3] == 0x80000000u); CHECK(c->mq == 0); CHECK(c->xer & PPC_XER_OV);
}
static void masks(ppc_cpu *c, test_memory *m)
{
    test_setup(c, m); c->gpr[3] = 28; c->gpr[2] = 3;
    CHECK(test_step(c, m, test_x(29, 3, 1, 2, true)) == PPC_OK); CHECK(c->gpr[1] == 0xf000000fu);
    c->gpr[3] = 0x12345678; c->gpr[1] = 0xaaaaaaaa; c->gpr[2] = 0x00ff00ff;
    CHECK(test_step(c, m, test_x(541, 3, 1, 2, false)) == PPC_OK); CHECK(c->gpr[1] == 0xaa34aa78);
    c->gpr[3] = 0x80000000u; c->gpr[1] = 0; c->gpr[2] = 3;
    CHECK(test_step(c, m, test_x(537, 3, 1, 2, false)) == PPC_OK); CHECK(c->gpr[1] == 0x10000000u);
    c->gpr[3] = 0x12345678; c->gpr[1] = 0xaaaaaaaa; c->gpr[2] = 8;
    CHECK(test_step(c, m, (22u << 26) | (3u << 21) | (1u << 16) | (2u << 11) | (8u << 6) | (15u << 1)) == PPC_OK);
    CHECK(c->gpr[1] == 0xaa56aaaau);
    for (unsigned selector = 12; selector < 16; ++selector) {
        CHECK(test_step(c, m, test_x(531, 3, selector, 0, false)) == PPC_OK); CHECK(c->gpr[3] == 64);
    }
}
static void shifts(ppc_cpu *c, test_memory *m)
{
    static const struct { unsigned xo; bool right, immediate, merge, keep_mq, large_zero, arithmetic; } forms[] = {
        {153,false,false,false,false,false,false}, {217,false,false,true,false,false,false},
        {184,false,true,false,false,false,false}, {248,false,true,true,false,false,false},
        {216,false,false,true,true,false,false}, {152,false,false,false,false,true,false},
        {952,true,true,false,false,false,true}, {920,true,false,false,false,true,true},
        {665,true,false,false,false,false,false}, {921,true,false,false,false,false,true},
        {729,true,false,true,false,false,false}, {696,true,true,false,false,false,false},
        {760,true,true,true,false,false,false}, {728,true,false,true,true,false,false},
        {664,true,false,false,false,true,false}
    };
    for (unsigned form = 0; form < sizeof(forms) / sizeof(forms[0]); ++form) {
        for (unsigned n = 0; n < (forms[form].immediate ? 32u : 64u); ++n) {
            test_setup(c, m);
            uint32_t source = 0x92345679u, previous = 0xaabbccddu;
            unsigned shift = n & 31u;
            uint32_t rotated = shift ? (forms[form].right ?
                (source >> shift) | (source << (32 - shift)) :
                (source << shift) | (source >> (32 - shift))) : source;
            uint32_t mask = forms[form].right ? UINT32_MAX >> shift : UINT32_MAX << shift;
            uint32_t expected;
            if (forms[form].keep_mq && n >= 32) expected = previous & mask;
            else if (forms[form].merge) expected = (rotated & mask) | (previous & ~mask);
            else {
                if (forms[form].large_zero && n >= 32) mask = 0;
                expected = (rotated & mask) | (forms[form].arithmetic ? ~mask : 0);
            }
            c->gpr[3] = source; c->gpr[2] = n; c->mq = previous;
            CHECK(test_step(c, m, test_x(forms[form].xo, 3, 1, forms[form].immediate ? n : 2u, true)) == PPC_OK);
            CHECK(c->gpr[1] == expected); CHECK(c->mq == (forms[form].keep_mq ? previous : rotated));
            if (forms[form].arithmetic) CHECK(((c->xer & PPC_XER_CA) != 0) == ((rotated & ~mask) != 0));
        }
    }
}
static void strings(ppc_cpu *c, test_memory *m)
{
    test_setup(c, m); c->gpr[1] = 0x2000; c->gpr[2] = 0; c->xer = 0x4407;
    memcpy(m->bytes + 0x2000, "ABCDEFG", 7);
    CHECK(test_step(c, m, test_x(277, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[3] == 0x41424344u); CHECK((c->xer & 127) == 4); CHECK(c->cr >> 28 == 2);
    c->xer = 0x7a07; c->gpr[4] = UINT32_MAX;
    CHECK(test_step(c, m, test_x(277, 3, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[4] == 0x45464700u); CHECK((c->xer & 127) == 7); CHECK(c->cr >> 28 == 0);
    c->xer = 0x4407;
    CHECK(test_step(c, m, test_x(277, 1, 1, 2, true)) == PPC_OK);
    CHECK(c->gpr[1] == 0x2000); CHECK(c->gpr[2] == 0);
    test_setup(c, m); c->sr[0] = 0x80000000u; c->xer = 1;
    CHECK(test_step(c, m, test_x(277, 3, 0, 2, false)) == PPC_EXCEPTION);
    CHECK(c->srr0 == 0x1000); CHECK(c->dsisr == 0x04000000u);
    test_setup(c, m); c->sr[0] = 0x87f00000u; c->xer = 2; c->gpr[2] = 0xfff;
    CHECK(test_step(c, m, test_x(277, 3, 0, 2, false)) == PPC_EXCEPTION);
    CHECK(c->srr0 == 0x1000); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    test_setup(c, m); c->hid[0] |= 8; c->xer = 1;
    test_write(m, 0x1004, 4, test_x(277, 3, 0, 2, false));
    CHECK(ppc_step(c) == PPC_EXCEPTION); CHECK(c->last_exception == PPC_EX_ALIGNMENT);
    CHECK(c->srr0 == 0x1000);
}
int main(void)
{
    static test_memory memory;
    ppc_cpu cpu;
    arithmetic(&cpu, &memory); masks(&cpu, &memory); shifts(&cpu, &memory); strings(&cpu, &memory);
    return test_report("601 POWER subset");
}
