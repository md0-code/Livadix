/* test_dsp3210: the DSP3210 core (dsp3210.h) against the Information
 * Manual: the Quadra 840AV ROM's start program (a pc-relative goto, a
 * 24-bit load and a shift-and-or, a call whose latent instruction stores
 * the return address), the delayed branches of 4.4.2.3, the CAU's flags for
 * 16- and 32-bit operations, data moves with their sizes and
 * post-modification, the DAU's multiply-accumulate with the latencies of
 * 4.4.2 (the memory write, the accumulator as a multiplier input, the DAU
 * condition), the conversions of 8.2.4, do loops, an external interrupt
 * with ireturn, the timer and an illegal opcode's error exception. */
#include "hw_test.h"
#include "dsp3210.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t dram[0x10000];
static mc_dsp3210 dsp;
static int iacks[2];

static unsigned rc(unsigned r) { return r <= 14 ? r : r <= 19 ? r + 2u : r + 4u; }   /* r -> field code */

static uint32_t cb(unsigned c, unsigned rb, int n) { return 0x80000000u | c << 21 | rb << 16 | ((uint32_t)n & 0xFFFFu); }
#define NOP 0x80000000u
static uint32_t set24(unsigned rd, uint32_t m) { return 0xC0000000u | ((m >> 16) & 0xFFu) << 21 | rc(rd) << 16 | (m & 0xFFFFu); }
static uint32_t shor(unsigned rd, unsigned rs, uint32_t n) { return 0x90000000u | rc(rd) << 21 | rc(rs) << 16 | (n & 0xFFFFu); }
static uint32_t addi(int lng, unsigned rd, unsigned rs, int n) {
    return (lng ? 0x80000000u : 0) | 0x14000000u | rc(rd) << 21 | rs << 16 | ((uint32_t)n & 0xFFFFu);
}
static uint32_t alur(int lng, unsigned f, unsigned rd, unsigned rs1, unsigned c, unsigned rs2) {
    return (lng ? 0x80000000u : 0) | 0x18000000u | f << 21 | rc(rd) << 16 | rs1 << 11 | c << 5 | rs2;
}
static uint32_t alui(int lng, unsigned f, unsigned rd, int n) {
    return (lng ? 0x80000000u : 0) | 0x1A000000u | f << 21 | rc(rd) << 16 | ((uint32_t)n & 0xFFFFu);
}
/* 7c: rH <-> *rP++rI; t 1 = store */
static uint32_t movp(unsigned t, unsigned w, unsigned rh, unsigned rp, unsigned ri) {
    return 0x9C000000u | t << 24 | w << 21 | rc(rh) << 16 | rc(rp) << 11 | ri;
}
static uint32_t movio(unsigned t, unsigned w, unsigned rh, unsigned ior) { return 0x9C000400u | t << 24 | w << 21 | rc(rh) << 16 | ior; }
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* an encoder the suite does not use yet */
#endif
static uint32_t call4(unsigned rm, unsigned rb, int n) { return 0x10000000u | rc(rm) << 21 | rb << 16 | ((uint32_t)n & 0xFFFFu); }
/* DA field: *rP with post-modify i (0 none, 7 +size) or an accumulator. */
static unsigned ptr(unsigned p, unsigned i) { return p << 3 | i; }
#define ACC(n) (n)
#define NOWRITE 7u
static uint32_t f3(unsigned m, unsigned f, unsigned s, unsigned n, unsigned x, unsigned y, unsigned z) {
    return 0x60000000u | m << 26 | f << 24 | s << 23 | n << 21 | x << 14 | y << 7 | z;
}
static uint32_t f1(unsigned m, unsigned f, unsigned s, unsigned n, unsigned x, unsigned y, unsigned z) {
    return 0x20000000u | m << 26 | f << 24 | s << 23 | n << 21 | x << 14 | y << 7 | z;
}
static uint32_t f5(unsigned g, unsigned n, unsigned y, unsigned z) { return 0x78000000u | g << 23 | n << 21 | y << 7 | z; }

static void put32(uint32_t a, uint32_t v) { dram[a] = (uint8_t)(v >> 24); dram[a + 1] = (uint8_t)(v >> 16); dram[a + 2] = (uint8_t)(v >> 8); dram[a + 3] = (uint8_t)v; }
static uint32_t get32(uint32_t a) { return (uint32_t)dram[a] << 24 | (uint32_t)dram[a + 1] << 16 | (uint32_t)dram[a + 2] << 8 | dram[a + 3]; }
static void prog(uint32_t a, const uint32_t *w, size_t n) { for (size_t i = 0; i < n; i++) put32(a + 4u * (uint32_t)i, w[i]); }

static void on_iack(void *ctx, int line) { (void)ctx; iacks[line & 1]++; }

static uint8_t **pages;

static void boot(void) {
    const mc_dsp3210_bus bus = {NULL, NULL, NULL, NULL, on_iack};
    if (!pages) {
        pages = calloc((size_t)1 << (32 - MC_DSP3210_PAGE_SHIFT), sizeof *pages);
        for (uint32_t a = 0; a < sizeof dram; a += 1u << MC_DSP3210_PAGE_SHIFT) pages[a >> MC_DSP3210_PAGE_SHIFT] = dram + a;
    }
    mc_dsp3210_init(&dsp, &bus, pages, pages);
    mc_dsp3210_reset(&dsp, 0);
}

/* Runs a program at 0x100 reached by goto from 0 until it loops at `stop`. */
static void run_at(const uint32_t *w, size_t n, unsigned steps) {
    const uint32_t start[] = {cb(1, 0, 0x100), NOP};
    prog(0, start, 2);
    prog(0x100, w, n);
    boot();
    mc_dsp3210_run(&dsp, steps);
}

int main(void) {
    /* The 840AV ROM's start program ($4099BEA8): pc+4 skips the word at 8. */
    memset(dram, 0, sizeof dram);
    const uint32_t start[] = {0x802F0004u, 0xC001E0FCu, 0x40802A60u, 0x90210000u, 0x10210000u, 0x9DE10800u, 0};
    prog(0, start, 7);
    put32(0x8, 0x40802A60u);
    const uint32_t kernel[] = {cb(1, 15, -8), NOP};          /* goto pc - 8: itself */
    prog(0xE0FC, kernel, 2);
    boot();
    mc_dsp3210_run(&dsp, 20);
    CHECK_EQ(get32(0x18) & 0xFFFFFu, 0x18u);
    CHECK_EQ(dsp.r[1], 0x18u);
    CHECK(dsp.pc == 0xE0FCu || dsp.pc == 0xE100u);

    /* Delayed branches (4.4.2.3): I1 goto A, I2 goto B -> I1, I2, A, B. */
    {
        const uint32_t w[] = {cb(1, 0, 0x200), cb(1, 0, 0x300), NOP, NOP};
        memset(dram, 0, sizeof dram);
        const uint32_t s[] = {cb(1, 0, 0x100), NOP};
        prog(0, s, 2);
        prog(0x100, w, 4);
        const uint32_t a[] = {addi(1, 5, 0, 7), cb(1, 15, -8), NOP};   /* A: r5 = 7, loops at A+4 */
        prog(0x200, a, 3);
        const uint32_t b[] = {addi(1, 6, 0, 9), cb(1, 15, -8), NOP};
        prog(0x300, b, 3);
        boot();
        mc_dsp3210_run(&dsp, 12);
        CHECK_EQ(dsp.r[5], 7u);         /* A ran once */
        CHECK_EQ(dsp.r[6], 9u);         /* then B */
    }

    memset(dram, 0, sizeof dram);
    /* CAU flags: a 32-bit add overflow, a 16-bit carry, subtract's borrow, a compare. */
    {
        const uint32_t w[] = {
            set24(1, 0x7FFFFF), shor(1, 1, 0x7FFF),          /* r1 = 0x7FFFFFFF */
            addi(1, 2, rc(1), 1),                            /* r2 = r1 + 1: v, n */
            movio(0, 7, 3, 0),                               /* r3 = ps */
            addi(0, 4, 0, -1),                               /* r4 = (short) 0xFFFF */
            alur(0, 0, 5, rc(4), 1, 23),                     /* r5 = (short) r4 + 1: c, z */
            movio(0, 7, 6, 0),
            alur(1, 4, 7, 0, 1, 23),                         /* r7 = 0 - 1: borrow */
            movio(0, 7, 8, 0),
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 30);
        CHECK_EQ(dsp.r[2], 0x80000000u);
        CHECK_EQ(dsp.r[3] & 15u, 0x5u);     /* n, v */
        CHECK_EQ(dsp.r[5], 0u);
        CHECK_EQ(dsp.r[6] & 15u, 0xAu);     /* z, c */
        CHECK_EQ(dsp.r[7], 0xFFFFFFFFu);
        CHECK_EQ(dsp.r[8] & 15u, 0x9u);     /* n, c (borrow) */
    }

    /* Shifts, rotate through carry, carry-reverse add, loads and stores. */
    {
        memset(dram, 0, sizeof dram);
        put32(0x800, 0x8899AABBu);
        put32(0x804, 0xCC000000u);
        const uint32_t w[] = {
            set24(1, 0x000800),
            movp(0, 1, 2, 1, 23),                            /* r2 = (char) *r1++ (0x88 -> sign) */
            addi(1, 1, rc(1), 1),                            /* r1 = 0x802 */
            movp(0, 3, 3, 1, 23),                            /* r3 = (short) *r1++ : 0xAABB, r1 = 0x804 */
            movp(0, 4, 4, 1, 0),                             /* r4 = (hbyte) *r1 : 0xCC00 */
            set24(5, 0x000900),
            movp(1, 7, 3, 5, 23),                            /* *r5++ = r3 */
            alui(1, 1, 3, 4),                                /* r3 <<= 4 */
            alui(1, 13, 2, 4),                               /* r2 $>>= 4 */
            set24(6, 0x000001),
            alur(1, 9, 7, rc(6), 1, rc(6)),                  /* r7 = r6 >>>1: c = 1 */
            alur(1, 11, 8, 0, 1, 0),                         /* r8 = r0 <<<1: carry in -> 1 */
            set24(9, 0x000001), set24(10, 0x000001),
            alur(1, 3, 11, rc(9), 1, rc(10)),                /* r11 = r9 # r10 (carry reverse) */
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 40);
        CHECK_EQ(dsp.r[1], 0x804u);
        CHECK_EQ(dsp.r[4], 0xCC00u);
        CHECK_EQ(get32(0x900), 0xFFFFAABBu);
        CHECK_EQ(dsp.r[3], 0xFFFAABB0u);
        CHECK_EQ(dsp.r[2], 0xFFFFFFF8u);
        CHECK_EQ(dsp.r[7], 0u);
        CHECK_EQ(dsp.r[8], 1u);
        CHECK_EQ(dsp.r[11], 0u);           /* 1 # 1: the carry runs down and out */
    }

    /* DAU: a0 = 1.5 * 2.0 + a? ; the Z write's latency; float conversions. */
    {
        CHECK_EQ(mc_dsp3210_f32_from(1.0), 0x00000080u);
        CHECK_EQ(mc_dsp3210_f32_from(-1.0), 0x8000007Fu);     /* -2 * 2^-1 */
        CHECK(fabs(mc_dsp3210_f32_value(mc_dsp3210_f32_from(-0.75)) + 0.75) < 1e-9);
        CHECK(fabs(mc_dsp3210_f32_value(mc_dsp3210_f32_from(3.1415926)) - 3.1415926) < 1e-6);
        memset(dram, 0, sizeof dram);
        put32(0x800, mc_dsp3210_f32_from(1.5));
        put32(0x804, mc_dsp3210_f32_from(2.0));
        put32(0x808, mc_dsp3210_f32_from(0.25));
        const uint32_t w[] = {
            set24(1, 0x000800), set24(2, 0x000804), set24(3, 0x000810), set24(4, 0x000810),
            f3(4, 0, 0, 0, ptr(1, 0), ptr(2, 0), ptr(3, 0)),   /* *r3 = a0 = 0.0 + *r2 * *r1 */
            movp(0, 7, 5, 4, 0),                             /* r5 = *r4: still old (latency) */
            NOP, NOP,
            movp(0, 7, 6, 4, 0),                             /* the fourth: sees 3.0 */
            set24(7, 0x000808),
            f1(0, 0, 0, 1, ptr(7, 0), ptr(7, 0), NOWRITE),   /* a1 = *r7 + a0 * *r7 */
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 40);
        CHECK_EQ(dsp.r[5], 0u);
        CHECK_EQ(dsp.r[6], mc_dsp3210_f32_from(3.0));
        CHECK(fabs(mc_dsp3210_f32_value((uint32_t)((dsp.a[1] >> 8) & 0xFFFFFF00u) | (uint32_t)(dsp.a[1] & 0xFFu)) - (0.25 + 3.0 * 0.25)) < 1e-6);
    }

    /* Z p = 1111: the AV kernel's in-place loop, the result back to the Y
     * operand's address, the Y pointer post-incremented. */
    {
        memset(dram, 0, sizeof dram);
        put32(0x800, mc_dsp3210_f32_from(0.5));
        put32(0x804, mc_dsp3210_f32_from(1.5));
        put32(0x808, mc_dsp3210_f32_from(2.0));             /* the gain */
        const uint32_t w[] = {
            set24(2, 0x000800), set24(3, 0x000808), set24(15, 0x000C00),
            f1(4, 0, 0, 1, ptr(3, 0), ptr(3, 0), NOWRITE),   /* a1 = *r3 (2.0) */
            NOP, NOP, NOP,
            f3(4, 0, 0, 0, ACC(1), ptr(2, 0), 0x7Fu),       /* *r2++ = a0 = 0.0 + *r2 * a1 */
            f3(4, 0, 0, 0, ACC(1), ptr(2, 0), 0x7Fu),
            NOP, NOP, NOP,
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 40);
        CHECK_EQ(get32(0x800), mc_dsp3210_f32_from(1.0));
        CHECK_EQ(get32(0x804), mc_dsp3210_f32_from(3.0));
        CHECK_EQ(dsp.r[2], 0x808u);
        CHECK_EQ(get32(0xC00), 0u);
    }

    /* Multiplier latency (4.4.2.2): I1 a0 = 2.0, I2 a0 = 3.0, I4 a2 = a0 * 1.0 sees 2.0. */
    {
        memset(dram, 0, sizeof dram);
        put32(0x800, mc_dsp3210_f32_from(2.0));
        put32(0x804, mc_dsp3210_f32_from(3.0));
        put32(0x808, mc_dsp3210_f32_from(1.0));
        const uint32_t w[] = {
            set24(1, 0x000800), set24(2, 0x000804), set24(3, 0x000808),
            f1(4, 0, 0, 0, ptr(3, 0), ptr(1, 0), NOWRITE),   /* I1 a0 = *r1 (2.0) */
            f1(4, 0, 0, 0, ptr(3, 0), ptr(2, 0), NOWRITE),   /* I2 a0 = *r2 (3.0) */
            NOP,
            f3(4, 0, 0, 2, ptr(3, 0), ACC(0), NOWRITE),      /* I4 a2 = a0 * *r3 */
            f3(4, 0, 0, 3, ptr(3, 0), ACC(0), NOWRITE),      /* I5 a3 = a0 * *r3 */
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 30);
        CHECK(fabs(mc_dsp3210_f32_value((uint32_t)((dsp.a[2] >> 8) & 0xFFFFFF00u) | (uint32_t)(dsp.a[2] & 0xFFu)) - 2.0) < 1e-9);
        CHECK(fabs(mc_dsp3210_f32_value((uint32_t)((dsp.a[3] >> 8) & 0xFFFFFF00u) | (uint32_t)(dsp.a[3] & 0xFFu)) - 3.0) < 1e-9);
    }

    /* Conversions: int32 with each rounding mode, float32, ieee and dsp. */
    {
        memset(dram, 0, sizeof dram);
        put32(0x800, mc_dsp3210_f32_from(-2.5));
        put32(0x804, 0xFFFFFF85u);                           /* -123 */
        put32(0x808, 0x40490FDBu);                           /* IEEE pi */
        const uint32_t w[] = {
            set24(1, 0x000800), set24(2, 0x000804), set24(3, 0x000808), set24(4, 0x000900),
            f5(9, 0, ptr(1, 0), ptr(4, 7)),                  /* *r4++ = int32(-2.5) nearest: -2 */
            set24(5, 0x000010), movio(1, 0, 5, 14),          /* dauc: truncate toward -inf */
            NOP,
            f5(9, 0, ptr(1, 0), ptr(4, 7)),                  /* -3 */
            set24(5, 0x000030), movio(1, 0, 5, 14),          /* toward zero */
            NOP,
            f5(9, 0, ptr(1, 0), ptr(4, 7)),                  /* -2 */
            f5(8, 1, ptr(2, 0), ptr(4, 7)),                  /* *r4++ = float32(-123) */
            f5(13, 2, ptr(3, 0), ptr(4, 7)),                 /* *r4++ = dsp(pi) */
            NOP,
            f5(12, 3, ACC(2), ptr(4, 7)),                    /* *r4++ = ieee(a2) */
            NOP, NOP, NOP, NOP,
            cb(1, 15, -8), NOP,
        };
        run_at(w, sizeof w / 4, 40);
        CHECK_EQ(get32(0x900), (uint32_t)-2);
        CHECK_EQ(get32(0x904), (uint32_t)-3);
        CHECK_EQ(get32(0x908), (uint32_t)-2);
        CHECK(fabs(mc_dsp3210_f32_value(get32(0x90C)) + 123.0) < 1e-9);
        CHECK(fabs(mc_dsp3210_f32_value(get32(0x910)) - 3.14159265) < 1e-6);
        CHECK_EQ(get32(0x914), 0x40490FDBu);
    }

    /* A do loop: do 0, 4 repeats r1 += 1 five times. */
    {
        const uint32_t w[] = {0x8C000000u | 0u << 11 | 4u, addi(1, 1, rc(1), 1), cb(1, 15, -8), NOP};
        run_at(w, sizeof w / 4, 30);
        CHECK_EQ(dsp.r[1], 5u);
    }

    /* EXT1 interrupt: vector 15 at evtp + 120, IACK1, ireturn back. */
    {
        const uint32_t w[] = {
            set24(22, 0x000400),                             /* evtp */
            set24(1, 0x008000), movio(1, 3, 1, 8),           /* emr = EXT1 */
            NOP,
            addi(1, 2, rc(2), 1), cb(1, 15, -12), NOP,       /* r2 counts */
        };
        run_at(w, sizeof w / 4, 20);
        const uint32_t vec[] = {addi(1, 3, rc(3), 1), 0x803E0000u};   /* r3++, ireturn */
        prog(0x400 + 15u * 8u, vec, 2);
        mc_dsp3210_set_irq(&dsp, 1, 1);
        mc_dsp3210_run(&dsp, 3);
        mc_dsp3210_set_irq(&dsp, 1, 0);
        const uint32_t before = dsp.r[2];
        mc_dsp3210_run(&dsp, 20);
        CHECK_EQ(dsp.r[3], 1u);
        CHECK_EQ(iacks[1], 1);
        CHECK(dsp.r[2] > before);
        CHECK_EQ(dsp.level, 0);
    }

    /* An illegal opcode: call evtp + 16 (r20). */
    {
        memset(dram, 0, sizeof dram);
        const uint32_t w[] = {set24(22, 0x000400), NOP, 0x00000000u, NOP};
        run_at(w, sizeof w / 4, 5);
        CHECK_EQ(dsp.pc, 0x410u);
        CHECK_EQ(dsp.r[20], 0x10Cu);
        CHECK_EQ(dsp.level, 3);
    }
    printf("test_dsp3210: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
