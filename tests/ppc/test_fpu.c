#include "test_support.h"
#include "ppc_fp_soft.h"

/* MPC601UM 2.5 (representations and rounding), 5.4.7 (FPSCR actions),
   chapter 10 encodings. Expected values below use exact binary fractions. */
#define ONE UINT64_C(0x3ff0000000000000)
#define TWO UINT64_C(0x4000000000000000)
#define INF UINT64_C(0x7ff0000000000000)
#define NEG UINT64_C(0x8000000000000000)
static void expect(ppc_fp_operation op, bool single, uint64_t a, uint64_t b,
                   uint64_t c, uint32_t fpscr, uint64_t bits, uint32_t raised, unsigned cls)
{
    ppc_fp_result r = ppc_fp_compute(op, single, a, b, c, fpscr);
    CHECK(r.write); CHECK(r.bits == bits); CHECK(r.raised == raised); CHECK(r.fprf == cls);
}
static uint32_t aop(unsigned op, unsigned xo, unsigned d, unsigned a, unsigned b, unsigned c, bool rc)
{
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | (rc ? 1u : 0u);
}
static void arithmetic(void)
{
    const uint32_t inexact = PPC_FP_XX | PPC_FP_FI;
    for (unsigned rn = 0; rn < 4; ++rn) {
        expect(PPC_FP_ADD, false, ONE, ONE, 0, rn, TWO, 0, 4);
        expect(PPC_FP_ADD, false, ONE, UINT64_C(0x3ca0000000000000), 0, rn,
            ONE + (rn == 2 ? 1 : 0), inexact | (rn == 2 ? PPC_FP_FR : 0), 4);
        expect(PPC_FP_ADD, false, ONE | NEG, UINT64_C(0xbca0000000000000), 0, rn,
            (ONE | NEG) + (rn == 3 ? 1 : 0), inexact | (rn == 3 ? PPC_FP_FR : 0), 8);
        expect(PPC_FP_SUB, false, ONE, ONE, 0, rn, rn == 3 ? NEG : 0, 0, rn == 3 ? 18 : 2);
        expect(PPC_FP_DIV, false, ONE, UINT64_C(0x4008000000000000), 0, rn,
            UINT64_C(0x3fd5555555555555) + (rn == 2 ? 1 : 0), inexact | (rn == 2 ? PPC_FP_FR : 0), 4);
        expect(PPC_FP_ROUND_SINGLE, false, 0, UINT64_C(0x3ff0000010000000), 0, rn,
            ONE + (rn == 2 ? UINT64_C(0x20000000) : 0), inexact | (rn == 2 ? PPC_FP_FR : 0), 4);
    }
    expect(PPC_FP_ADD, false, ONE + 1, UINT64_C(0x3ca0000000000000), 0, 0,
        ONE + 2, inexact | PPC_FP_FR, 4);
    expect(PPC_FP_MADD, false, ONE + 1, ONE | NEG, UINT64_C(0x3feffffffffffffe), 0,
        UINT64_C(0xb970000000000000), 0, 8);
    expect(PPC_FP_NMADD, false, ONE + 1, ONE | NEG, UINT64_C(0x3feffffffffffffe), 0,
        UINT64_C(0x3970000000000000), 0, 4);
    expect(PPC_FP_MUL, false, ONE, 0, ONE | NEG, 0, ONE | NEG, 0, 8);
    expect(PPC_FP_DIV, false, ONE, INF, 0, 0, 0, 0, 2);
    expect(PPC_FP_MUL, true, TWO, 0, ONE, 0, TWO, 0, 4);
    /* Black-box QEMU 10.2.1/604 clears FR here. Exact rational division
       proves the magnitude increments; UM table 2-1 requires FR on 601. */
    expect(PPC_FP_DIV, false, UINT64_C(0x409972d944031702), UINT64_C(0xbf94f1a7244ee7da), 0, 3,
        UINT64_C(0xc0f370f9f92ee0e8), PPC_FP_XX | PPC_FP_FI | PPC_FP_FR, 8);
}
static void range_and_invalid(void)
{
    const uint64_t maximum = UINT64_C(0x7fefffffffffffff);
    expect(PPC_FP_MUL, false, maximum, 0, TWO, 0, INF, PPC_FP_OX | PPC_FP_XX, 5);
    expect(PPC_FP_MUL, false, maximum, 0, TWO, 1, maximum, PPC_FP_OX | PPC_FP_XX, 4);
    expect(PPC_FP_MUL, false, maximum, 0, TWO, PPC_FP_OE,
        UINT64_C(0x1fffffffffffffff), PPC_FP_OX, 4);
    expect(PPC_FP_DIV, false, UINT64_C(0x0010000000000000), TWO, 0, 0,
        UINT64_C(0x0008000000000000), 0, 20);
    expect(PPC_FP_DIV, false, UINT64_C(0x0010000000000000), TWO, 0, PPC_FP_UE,
        UINT64_C(0x6000000000000000), PPC_FP_UX, 4);
    expect(PPC_FP_DIV, false, 1, TWO, 0, 0, 0, PPC_FP_UX | PPC_FP_XX | PPC_FP_FI, 2);
    expect(PPC_FP_DIV, false, 1, TWO, 0, 2, 1, PPC_FP_UX | PPC_FP_XX | PPC_FP_FI | PPC_FP_FR, 20);
    expect(PPC_FP_SUB, false, INF, INF, 0, 0,
        UINT64_C(0x7ff8000000000000), PPC_FP_VXISI, 17);
    expect(PPC_FP_DIV, false, 0, 0, 0, 0,
        UINT64_C(0x7ff8000000000000), PPC_FP_VXZDZ, 17);
    expect(PPC_FP_DIV, false, INF, INF, 0, 0,
        UINT64_C(0x7ff8000000000000), PPC_FP_VXIDI, 17);
    expect(PPC_FP_MUL, false, INF, 0, 0, 0,
        UINT64_C(0x7ff8000000000000), PPC_FP_VXIMZ, 17);
    expect(PPC_FP_ADD, false, UINT64_C(0x7ff0000000000123), ONE, 0, 0,
        UINT64_C(0x7ff8000000000123), PPC_FP_VXSNAN, 17);
    expect(PPC_FP_DIV, false, ONE, 0, 0, 0, INF, PPC_FP_ZX, 5);
    ppc_fp_result r = ppc_fp_compute(PPC_FP_DIV, false, ONE, 0, 0, PPC_FP_ZE);
    CHECK(!r.write); CHECK(r.raised == PPC_FP_ZX);
    r = ppc_fp_compute(PPC_FP_MUL, false, INF, 0, 0, PPC_FP_VE);
    CHECK(!r.write); CHECK(r.raised == PPC_FP_VXIMZ);
    uint32_t raised;
    CHECK(ppc_fp_compare(ONE, TWO, false, 0, &raised) == 8); CHECK(raised == 0);
    CHECK(ppc_fp_compare(0, NEG, false, 0, &raised) == 2);
    CHECK(ppc_fp_compare(UINT64_C(0x7ff8000000000001), ONE, true, 0, &raised) == 1);
    CHECK(raised == PPC_FP_VXVC);
    CHECK(ppc_fp_compare(UINT64_C(0x7ff0000000000001), ONE, true, PPC_FP_VE, &raised) == 1);
    CHECK(raised == PPC_FP_VXSNAN);
}
static void conversion(void)
{
    static const uint32_t singles[] = {0, 0x80000000u, 1, 0x007fffffu, 0x00800000u,
        0x3f800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc12345u, 0x7f812345u};
    for (unsigned i = 0; i < sizeof(singles) / sizeof(singles[0]); ++i)
        CHECK(ppc_fp_to_single(ppc_fp_from_single(singles[i])) == singles[i]);
    CHECK(ppc_fp_from_single(1) == UINT64_C(0x36a0000000000000));
    for (unsigned rn = 0; rn < 4; ++rn) {
        ppc_fp_result r = ppc_fp_compute(PPC_FP_TO_INT, false, 0, UINT64_C(0x4004000000000000), 0, rn);
        CHECK((uint32_t)r.bits == (rn == 2 ? 3u : 2u));
        CHECK(r.raised == (PPC_FP_XX | PPC_FP_FI | (rn == 2 ? PPC_FP_FR : 0)));
        r = ppc_fp_compute(PPC_FP_TO_INT_ZERO, false, 0, UINT64_C(0xc004000000000000), 0, rn);
        CHECK((uint32_t)r.bits == 0xfffffffeu);
    }
    ppc_fp_result r = ppc_fp_compute(PPC_FP_TO_INT, false, 0, INF, 0, 0);
    CHECK((uint32_t)r.bits == 0x7fffffffu); CHECK(r.raised == PPC_FP_VXCVI);
    r = ppc_fp_compute(PPC_FP_TO_INT, false, 0, UINT64_C(0x7ff8000000000001), 0, PPC_FP_VE);
    CHECK(!r.write); CHECK(r.raised == PPC_FP_VXCVI);
}
static void instruction_state(ppc_cpu *c, test_memory *m)
{
    test_setup(c, m); c->fpr[1] = ONE; c->fpr[2] = TWO;
    CHECK(test_step(c, m, aop(63, 21, 3, 1, 2, 0, true)) == PPC_OK);
    CHECK(c->fpr[3] == UINT64_C(0x4008000000000000)); CHECK(c->fpscr == 0x4000); CHECK(c->cr == 0);
    c->fpscr = PPC_FP_VE | 0x14000u | PPC_FP_FR | PPC_FP_FI;
    c->fpr[1] = INF; c->fpr[2] = INF; c->fpr[3] = ONE; c->msr |= PPC_MSR_FE1;
    CHECK(test_step(c, m, aop(63, 20, 3, 1, 2, 0, true)) == PPC_EXCEPTION);
    CHECK(c->srr0 == 0x1000); CHECK(c->srr1 & 0x00100000u); CHECK(c->fpr[3] == ONE);
    CHECK(c->fpscr == (PPC_FP_VE | 0x14000u | PPC_FP_VXISI | PPC_FP_FX | PPC_FP_FEX | PPC_FP_VX));
    CHECK(((c->cr >> 24) & 15) == 14);
    c->msr = PPC_MSR_ME | PPC_MSR_FP | PPC_MSR_FE0;
    CHECK(test_step(c, m, aop(63, 583, 4, 0, 0, 0, false)) == PPC_OK);
    CHECK(c->fpr[4] == (UINT64_C(0xfff8000000000000) | c->fpscr));
    CHECK(test_step(c, m, aop(63, 72, 5, 0, 3, 0, false)) == PPC_OK); CHECK(c->fpr[5] == ONE);
    test_setup(c, m); c->fpr[1] = ONE; c->fpr[2] = UINT64_C(0x3ca0000000000000);
    CHECK(test_step(c, m, aop(63, 21, 3, 1, 2, 0, true)) == PPC_OK);
    CHECK(c->fpscr == (PPC_FP_FX | PPC_FP_XX | PPC_FP_FI | 0x4000u));
    c->fpscr &= ~PPC_FP_FX;
    CHECK(test_step(c, m, aop(63, 21, 3, 1, 2, 0, false)) == PPC_OK); CHECK(!(c->fpscr & PPC_FP_FX));
    c->fpscr = PPC_FP_FX | PPC_FP_OX | PPC_FP_OE;
    CHECK(test_step(c, m, aop(63, 64, 8, 0, 0, 0, false)) == PPC_OK);
    CHECK(((c->cr >> 20) & 15) == 9); CHECK(c->fpscr == PPC_FP_OE);
    c->fpr[2] = UINT64_MAX;
    CHECK(test_step(c, m, (63u << 26) | (255u << 17) | (2u << 11) | (711u << 1)) == PPC_OK);
    CHECK(!(c->fpscr & PPC_FP_RESERVED));
    test_setup(c, m);
    CHECK(test_step(c, m, aop(63, 22, 3, 0, 1, 0, false)) == PPC_EXCEPTION);
    CHECK(c->srr1 & 0x00080000u);
}
int main(void)
{
    static test_memory memory;
    ppc_cpu cpu;
    arithmetic(); range_and_invalid(); conversion(); instruction_state(&cpu, &memory);
    return test_report("601 FPU");
}
