/* PEM 3.7.4 and fres/frsqrte/fsel; MPC603eUM 2.3.4.2;
   MPC604eUM tables 2-12/13; MPC750UM tables 2-20/21. */
#include "test_support.h"
#include "ppc_fp_soft.h"
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
static uint32_t fp(unsigned xo, unsigned d, unsigned a, unsigned b, unsigned c, bool single) {
    return ((single ? 59u : 63u) << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1);
}
static ppc_result execute(uint32_t op) { return test_step(&cpu, &memory, op); }
static double number(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}
static void selection(ppc_model model) {
    static const uint64_t inputs[] = {
        0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
        UINT64_C(0xbff0000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000001)
    };
    for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        setup(model); cpu.fpr[1] = inputs[i]; cpu.fpr[2] = 0x0123456789abcdef;
        cpu.fpr[3] = UINT64_C(0xfedcba9876543210); cpu.fpscr = PPC_FP_FX | PPC_FP_FEX | PPC_FP_VX | PPC_FP_VXSNAN | PPC_FP_VE;
        cpu.msr |= PPC_MSR_FE0; cpu.cr = 0xa1234567;
        CHECK(execute(fp(23, 4, 1, 2, 3, false) | 1u) == PPC_OK);
        CHECK(cpu.fpr[4] == cpu.fpr[i <= 2 || i == 4 ? 3 : 2]);
        CHECK(cpu.fpscr == (PPC_FP_FX | PPC_FP_FEX | PPC_FP_VX | PPC_FP_VXSNAN | PPC_FP_VE));
        CHECK(((cpu.cr >> 24) & 15u) == 14u);
    }
}
static void special_estimates(ppc_model model) {
    static const uint64_t inputs[] = { 0, UINT64_C(0x8000000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff8000000001234), UINT64_C(0x7ff0000000001234), UINT64_C(0xbff0000000000000) };
    for (unsigned sqrt = 0; sqrt < 2; ++sqrt) for (unsigned i = 0; i < sizeof(inputs) / sizeof(inputs[0]); ++i) {
        uint32_t cause = i < 2 ? PPC_FP_ZX : i == 5 ? PPC_FP_VXSNAN : sqrt && (i == 3 || i == 6) ? PPC_FP_VXSQRT : 0;
        uint64_t expected = i < 2 ? inputs[i] | UINT64_C(0x7ff0000000000000) : i >= 4 && i <= 5
            ? inputs[i] | UINT64_C(0x0008000000000000) : sqrt && (i == 3 || i == 6)
            ? UINT64_C(0x7ff8000000000000) : i == 3 ? UINT64_C(0x8000000000000000) : i == 6 ? UINT64_C(0xbff0000000000000) : 0;
        if (!sqrt && (i == 4 || i == 5)) expected &= ~UINT64_C(0x1fffffff);
        setup(model); cpu.fpr[2] = inputs[i]; cpu.fpr[3] = 0x1234;
        CHECK(execute(fp(sqrt ? 26 : 24, 3, 0, 2, 0, !sqrt)) == PPC_OK);
        CHECK(cpu.fpr[3] == expected);
        CHECK((cpu.fpscr & PPC_FP_STICKY) == cause);
        CHECK(!(cpu.fpscr & (PPC_FP_FR | PPC_FP_FI)));
        if (cause) {
            setup(model); cpu.fpr[2] = inputs[i]; cpu.fpr[3] = 0x1234;
            cpu.fpscr = cause == PPC_FP_ZX ? PPC_FP_ZE : PPC_FP_VE;
            cpu.msr |= PPC_MSR_FE0;
            CHECK(execute(fp(sqrt ? 26 : 24, 3, 0, 2, 0, !sqrt)) == PPC_EXCEPTION);
            CHECK(cpu.fpr[3] == 0x1234 && cpu.last_exception == PPC_EX_PROGRAM);
            CHECK(cpu.fpscr & PPC_FP_FEX);
        }
    }
    setup(model); CHECK(execute(fp(26, 3, 1, 2, 0, false)) == PPC_EXCEPTION);
    setup(model); CHECK(execute(fp(22, 3, 0, 2, 0, false)) == PPC_EXCEPTION);
    setup(model); cpu.msr &= ~PPC_MSR_FP;
    CHECK(execute(fp(24, 3, 0, 2, 0, true)) == PPC_EXCEPTION && cpu.last_exception == PPC_EX_FPU);
}
static void accuracy(ppc_model model) {
    const double tolerance = 1.0 / 4096.0;
    for (unsigned i = 0; i < 256; ++i) {
        uint64_t bits = ((uint64_t)(1u + (i * 37u) % 2046u) << 52) | ((UINT64_C(0x9e3779b97f4a7c15) * i) & UINT64_C(0x000fffffffffffff));
        double input = number(bits), estimate, product;
        setup(model); cpu.fpr[2] = bits;
        CHECK(execute(fp(26, 3, 0, 2, 0, false)) == PPC_OK);
        estimate = number(cpu.fpr[3]); product = estimate * input * estimate;
        CHECK(product >= (1.0 - tolerance) * (1.0 - tolerance) && product <= (1.0 + tolerance) * (1.0 + tolerance));
        CHECK(cpu.fpscr == 0x4000u);
        bits = ((uint64_t)(940u + i % 167u) << 52) | ((uint64_t)i << 44);
        if (i & 1u) bits |= UINT64_C(0x8000000000000000);
        setup(model); cpu.fpr[2] = bits;
        CHECK(execute(fp(24, 3, 0, 2, 0, true)) == PPC_OK);
        product = number(cpu.fpr[3]) * number(bits);
        CHECK(product >= 1.0 - tolerance && product <= 1.0 + tolerance);
        CHECK(!(cpu.fpscr & PPC_FP_STICKY));
    }
    for (unsigned i = 0; i < 52; ++i) {
        uint64_t bits = UINT64_C(1) << i;
        double product;
        setup(model); cpu.fpr[2] = bits;
        CHECK(execute(fp(26, 3, 0, 2, 0, false)) == PPC_OK);
        product = number(cpu.fpr[3]) * number(bits) * number(cpu.fpr[3]);
        CHECK(product >= (1.0 - tolerance) * (1.0 - tolerance) && product <= (1.0 + tolerance) * (1.0 + tolerance));
    }
}
/* Non-IEEE mode edge cases: MPC603eUM 2.3.4.2 (denormal results become
   signed zero); MPC604EUM tables 2-12/13/32/33 and MPC750UM 2-20/21/40/41
   (denormal operands zeroed, NaNs as in IEEE mode, loads and stores
   unaffected). QEMU 10.2.1 does not keep FPSCR[NI], so these are manual
   fixtures only. */
static void non_ieee_edges(ppc_model model) {
    const bool operands = model >= PPC_MODEL_604;
    const uint64_t denormal = UINT64_C(0x800fffffffffffff), one = UINT64_C(0x3ff0000000000000);
    /* A flushed result keeps the sign and is classified as a zero. */
    setup(model); cpu.fpscr = PPC_FP_NI;
    cpu.fpr[1] = UINT64_C(0x8010000000000000); cpu.fpr[2] = UINT64_C(0x3fe0000000000000);
    CHECK(execute(fp(25, 3, 1, 0, 2, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == UINT64_C(0x8000000000000000));
    CHECK((cpu.fpscr & PPC_FP_FPRF) == (0x12u << 12) && (cpu.fpscr & PPC_FP_NI));
    /* Single precision: a result below 2^-126 is zero (fmuls, frsp). */
    setup(model); cpu.fpscr = PPC_FP_NI;
    cpu.fpr[1] = UINT64_C(0x3810000000000000); cpu.fpr[2] = UINT64_C(0x3fe0000000000000);
    CHECK(execute(fp(25, 3, 1, 0, 2, true)) == PPC_OK && cpu.fpr[3] == 0);
    CHECK((cpu.fpscr & PPC_FP_FPRF) == (0x2u << 12));
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[2] = UINT64_C(0xb800000000000000);
    CHECK(execute(fp(12, 3, 0, 2, 0, false)) == PPC_OK && cpu.fpr[3] == UINT64_C(0x8000000000000000));
    /* Without NI the same product is a denormal. */
    setup(model);
    cpu.fpr[1] = UINT64_C(0x3810000000000000); cpu.fpr[2] = UINT64_C(0x3fe0000000000000);
    CHECK(execute(fp(25, 3, 1, 0, 2, true)) == PPC_OK && cpu.fpr[3] == UINT64_C(0x3800000000000000));
    /* A denormal operand: divided by one it is zero on the 604/750, a
       denormal result (then zero) on the 603e: both give -0. */
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[1] = denormal; cpu.fpr[2] = one;
    CHECK(execute(fp(18, 3, 1, 2, 0, false)) == PPC_OK && cpu.fpr[3] == UINT64_C(0x8000000000000000));
    /* Dividing by a denormal: zero divisor (ZX, -inf) on the 604/750; the
       603e computes with the denormal. */
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[1] = one; cpu.fpr[2] = denormal;
    CHECK(execute(fp(18, 3, 1, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == (operands ? UINT64_C(0xfff0000000000000) : UINT64_C(0xffd0000000000001)));
    CHECK(((cpu.fpscr & PPC_FP_ZX) != 0) == operands);
    /* NaNs are unaffected: a signalling NaN is quieted and flagged. */
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[1] = UINT64_C(0x7ff0000000000123); cpu.fpr[2] = denormal;
    CHECK(execute(fp(21, 3, 1, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == UINT64_C(0x7ff8000000000123) && (cpu.fpscr & PPC_FP_VXSNAN));
    /* Register moves and fsel see zeroed operands on the 604/750 only. */
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[2] = denormal;
    CHECK(execute(fp(72, 3, 0, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == (operands ? UINT64_C(0x8000000000000000) : denormal));
    CHECK(execute(fp(264, 3, 0, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == (operands ? 0 : UINT64_C(0x000fffffffffffff)));
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[1] = denormal; cpu.fpr[2] = one; cpu.fpr[4] = UINT64_C(0x4000000000000000);
    CHECK(execute(fp(23, 3, 1, 2, 4, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == (operands ? UINT64_C(0x4000000000000000) : one));
    CHECK(!(cpu.fpscr & PPC_FP_STICKY));
    /* Floating-point loads and stores move denormals unchanged. */
    setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[2] = denormal; cpu.gpr[4] = 0x2000;
    CHECK(execute((54u << 26) | (2u << 21) | (4u << 16)) == PPC_OK);
    CHECK(execute((50u << 26) | (5u << 21) | (4u << 16)) == PPC_OK && cpu.fpr[5] == denormal);
    test_write(&memory, 0x2008, 4, 0x00000001u);
    CHECK(execute((48u << 26) | (6u << 21) | (4u << 16) | 8u) == PPC_OK);
    CHECK(cpu.fpr[6] == UINT64_C(0x36a0000000000000));
    CHECK(execute((52u << 26) | (6u << 21) | (4u << 16) | 16u) == PPC_OK);
    { uint32_t v = 0; test_read(&memory, 0x2010, 4, &v); CHECK(v == 1u); }
    /* NI survives mtfsf and is reported by mffs. */
    setup(model); cpu.fpscr = PPC_FP_NI | 1u;
    CHECK(execute(fp(583, 7, 0, 0, 0, false)) == PPC_OK && (uint32_t)cpu.fpr[7] == (PPC_FP_NI | 1u));
    cpu.fpr[8] = 0;
    CHECK(execute(fp(711, 0, 0, 8, 0, false) | (1u << 17)) == PPC_OK && cpu.fpscr == 0);
}
static void status_and_ni(ppc_model model) {
    setup(model);
    CHECK(execute(fp(38, 29, 0, 0, 0, false)) == PPC_OK && cpu.fpscr == PPC_FP_NI);
    CHECK(execute(fp(70, 29, 0, 0, 0, false)) == PPC_OK && cpu.fpscr == 0);
    setup(model); cpu.fpr[1] = UINT64_C(0x7fefffffffffffff); cpu.fpr[2] = UINT64_C(0x4000000000000000);
    CHECK(execute(fp(25, 3, 1, 0, 2, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == UINT64_C(0x7ff0000000000000));
    CHECK((cpu.fpscr & (PPC_FP_OX | PPC_FP_XX | PPC_FP_FI)) == (PPC_FP_OX | PPC_FP_XX | PPC_FP_FI));
    setup(model); cpu.fpscr = 3;
    cpu.fpr[1] = UINT64_C(0x409972d944031702); cpu.fpr[2] = UINT64_C(0xbf94f1a7244ee7da);
    CHECK(execute(fp(18, 3, 1, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == UINT64_C(0xc0f370f9f92ee0e8));
    CHECK((cpu.fpscr & (PPC_FP_XX | PPC_FP_FI | PPC_FP_FR)) == (PPC_FP_XX | PPC_FP_FI | PPC_FP_FR));
    setup(model); cpu.fpscr = PPC_FP_NI;
    cpu.fpr[1] = UINT64_C(0x0010000000000000); cpu.fpr[2] = UINT64_C(0x800fffffffffffff);
    CHECK(execute(fp(21, 3, 1, 2, 0, false)) == PPC_OK);
    CHECK(cpu.fpr[3] == (model >= PPC_MODEL_604 ? UINT64_C(0x0010000000000000) : 0));
    CHECK(cpu.fpscr & PPC_FP_NI);
    setup(model); cpu.fpr[1] = UINT64_C(0x0010000000000000); cpu.fpr[2] = UINT64_C(0x800fffffffffffff);
    CHECK(execute(fp(21, 3, 1, 2, 0, false)) == PPC_OK && cpu.fpr[3] == 1);
    if (model >= PPC_MODEL_604) {
        setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[2] = UINT64_C(0x7ff0000000000000);
        CHECK(execute(fp(14, 3, 0, 2, 0, false)) == PPC_OK);
        CHECK((uint32_t)cpu.fpr[3] == 0x8000u && (cpu.fpscr & PPC_FP_VXCVI));
        setup(model); cpu.fpscr = PPC_FP_NI; cpu.fpr[1] = 1;
        CHECK(execute(fp(0, 0, 1, 2, 0, false)) == PPC_OK);
        CHECK((cpu.cr >> 28) == 2u);
    }
    non_ieee_edges(model);
}
int main(void) {
    for (ppc_model model = PPC_MODEL_603; model < PPC_MODEL_COUNT; model = (ppc_model)(model + 1)) {
        selection(model); special_estimates(model); accuracy(model); status_and_ni(model);
    }
    return test_report("PowerPC later floating point");
}
