/* The 68HC05 core against the M68HC05 family reference (instruction set:
 * operations, condition codes, addressing modes, HC05 cycle counts, the
 * stack page, interrupts, SWI/RTI, WAIT/STOP), and the Egret/Cuda chip's
 * peripherals as adbctl_mcu.h describes them (ports and data directions,
 * the core timer's TOF and RTI, the one-second timer, the PLL's two bus
 * clocks). Programs are assembled by hand from the opcode map. */
#include "hw_test.h"
#include "../../src/hw/cpu6805.h"
#include "../../src/hw/adbctl_mcu.h"

#include <stdlib.h>

static uint8_t mem[0x2000];
static unsigned writes;
static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a & 0x1FFFu]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a & 0x1FFFu] = v; writes++; }

static mc_cpu6805 cpu;

/* Loads a program at $0F00 and resets into it. */
static void load(const uint8_t *code, size_t n) {
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x0F00, code, n);
    mem[0x1FFE] = 0x0F;
    mem[0x1FFF] = 0x00;
    mc_cpu6805_init(&cpu, NULL, rd, wr, 0x1FFE);
    mc_cpu6805_reset(&cpu);
}

/* Runs n instructions; returns the cycles. */
static unsigned run(unsigned n) {
    unsigned c = 0;
    while (n--) c += mc_cpu6805_step(&cpu);
    return c;
}

#define CC(f) ((cpu.cc & (f)) != 0)

static void test_reset(void) {
    const uint8_t p[] = {0x9D};
    load(p, sizeof p);
    CHECK_EQ(cpu.pc, 0x0F00);
    CHECK_EQ(cpu.sp, 0xFF);
    CHECK(CC(MC_6805_I));
    CHECK_EQ(cpu.cc & 0xE0, 0xE0);                  /* CCR's three upper bits read 1 */
    CHECK_EQ(run(1), 2);                            /* NOP */
}

/* Every addressing mode of LDA and STA with its HC05 cycle count. */
static void test_addressing(void) {
    const uint8_t p[] = {
        0xA6, 0x11,             /* LDA #$11          2 */
        0xB7, 0x90,             /* STA $90           4 */
        0xC7, 0x01, 0x20,       /* STA $0120         5 */
        0xAE, 0x05,             /* LDX #$05          2 */
        0xD7, 0x01, 0x00,       /* STA $0100,X       6 */
        0xE7, 0x80,             /* STA $80,X         5 */
        0xF7,                   /* STA ,X            4 */
        0x4F,                   /* CLRA              3 */
        0xB6, 0x90,             /* LDA $90           3 */
        0xC6, 0x01, 0x20,       /* LDA $0120         4 */
        0xD6, 0x01, 0x00,       /* LDA $0100,X       5 */
        0xE6, 0x80,             /* LDA $80,X         4 */
        0xF6,                   /* LDA ,X            3 */
        0xD6, 0xFF, 0xFF,       /* LDA $FFFF,X: 16-bit sum wraps to $0004 */
    };
    load(p, sizeof p);
    static const unsigned cycles[] = {2, 4, 5, 2, 6, 5, 4, 3, 3, 4, 5, 4, 3};
    for (size_t i = 0; i < sizeof cycles / sizeof *cycles; i++) CHECK_EQ(run(1), cycles[i]);
    CHECK_EQ(mem[0x90], 0x11);
    CHECK_EQ(mem[0x120], 0x11);
    CHECK_EQ(mem[0x105], 0x11);
    CHECK_EQ(mem[0x85], 0x11);
    CHECK_EQ(mem[0x05], 0x11);
    CHECK_EQ(cpu.a, 0x11);
    mem[0x04] = 0x5A;
    run(1);
    CHECK_EQ(cpu.a, 0x5A);
}

/* Arithmetic and logic flags. */
static void test_alu(void) {
    const uint8_t add[] = {0xA6, 0x0F, 0xAB, 0x01, 0xAB, 0xF0, 0xA9, 0x00};
    load(add, sizeof add);
    run(2);                                         /* $0F + $01: half carry */
    CHECK_EQ(cpu.a, 0x10);
    CHECK(CC(MC_6805_H));
    CHECK(!CC(MC_6805_C));
    run(1);                                         /* $10 + $F0 = $00, carry */
    CHECK_EQ(cpu.a, 0x00);
    CHECK(CC(MC_6805_C) && CC(MC_6805_Z) && !CC(MC_6805_H));
    run(1);                                         /* ADC #0 adds the carry */
    CHECK_EQ(cpu.a, 0x01);
    CHECK(!CC(MC_6805_C) && !CC(MC_6805_Z));

    const uint8_t sub[] = {0xA6, 0x10, 0xA0, 0x20, 0xA2, 0x00, 0xA1, 0xEF, 0xAE, 0x10, 0xA3, 0x11};
    load(sub, sizeof sub);
    run(2);                                         /* $10 - $20: borrow, negative */
    CHECK_EQ(cpu.a, 0xF0);
    CHECK(CC(MC_6805_C) && CC(MC_6805_N));
    run(1);                                         /* SBC #0 takes the borrow */
    CHECK_EQ(cpu.a, 0xEF);
    CHECK(!CC(MC_6805_C));
    run(1);                                         /* CMP equal: Z, A kept */
    CHECK(CC(MC_6805_Z) && !CC(MC_6805_C));
    CHECK_EQ(cpu.a, 0xEF);
    run(2);                                         /* CPX $10 with $11: borrow */
    CHECK(CC(MC_6805_C) && CC(MC_6805_N));

    const uint8_t logic[] = {0xA6, 0xF0, 0xA4, 0x0F, 0xAA, 0x81, 0xA8, 0xFF, 0xA5, 0x80, 0xA5, 0x7E};
    load(logic, sizeof logic);
    run(2);
    CHECK_EQ(cpu.a, 0x00);
    CHECK(CC(MC_6805_Z));
    run(1);
    CHECK_EQ(cpu.a, 0x81);
    CHECK(CC(MC_6805_N));
    run(1);
    CHECK_EQ(cpu.a, 0x7E);
    run(1);                                         /* BIT: Z from A & M, A kept */
    CHECK(CC(MC_6805_Z));
    run(1);
    CHECK(!CC(MC_6805_Z));
    CHECK_EQ(cpu.a, 0x7E);

    const uint8_t mul[] = {0xA6, 0xC8, 0xAE, 0x64, 0x99, 0x42};
    load(mul, sizeof mul);
    run(3);
    CHECK_EQ(run(1), 11);                           /* MUL: X:A = A x X, H and C cleared */
    CHECK_EQ(cpu.x, 0x4E);
    CHECK_EQ(cpu.a, 0x20);
    CHECK(!CC(MC_6805_C) && !CC(MC_6805_H));
}

/* Read-modify-write: operations, flags, the five modes and their cycles. */
static void test_rmw(void) {
    const uint8_t p[] = {
        0xA6, 0x01, 0x40,       /* NEGA: $FF, C set (result not 0) */
        0x40,                   /* NEGA: $01 */
        0x4F, 0x40,             /* CLRA, NEGA: 0, C clear */
        0xA6, 0x55, 0x43,       /* COMA: $AA, C set */
        0x44,                   /* LSRA: $55, C 0, N 0 */
        0x46,                   /* RORA: C (0) in: $2A, C 1 */
        0x46,                   /* RORA: C (1) in: $95, C 0 */
        0x47,                   /* ASRA: $CA, C 1 */
        0x48,                   /* LSLA: $94, C 1 */
        0x49,                   /* ROLA: $29, C 1 */
        0x4A,                   /* DECA: $28 */
        0x4C,                   /* INCA: $29 */
        0x4D,                   /* TSTA */
        0x3C, 0x90,             /* INC $90      5 */
        0x6C, 0x10,             /* INC $10,X    6 */
        0x7C,                   /* INC ,X       5 */
        0x5C,                   /* INCX         3 */
        0x3D, 0x90,             /* TST $90      4 */
        0x6D, 0x10,             /* TST $10,X    5 */
        0x7D,                   /* TST ,X       4 */
        0x3F, 0x90,             /* CLR $90      5 */
    };
    load(p, sizeof p);
    run(2);
    CHECK_EQ(cpu.a, 0xFF);
    CHECK(CC(MC_6805_C) && CC(MC_6805_N));
    run(1);
    CHECK_EQ(cpu.a, 0x01);
    run(2);
    CHECK(!CC(MC_6805_C) && CC(MC_6805_Z));
    run(2);
    CHECK_EQ(cpu.a, 0xAA);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0x55);
    CHECK(!CC(MC_6805_C) && !CC(MC_6805_N));
    run(1);
    CHECK_EQ(cpu.a, 0x2A);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0x95);
    CHECK(!CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0xCA);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0x94);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0x29);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.a, 0x28);
    run(1);
    CHECK_EQ(cpu.a, 0x29);
    CHECK_EQ(run(1), 3);
    cpu.x = 0x80;
    mem[0x90] = 0x7F;
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x90], 0x80);
    CHECK(CC(MC_6805_N));
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x90], 0x81);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x80], 0x01);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.x, 0x81);
    const unsigned w = writes;
    CHECK_EQ(run(1), 4);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(run(1), 4);
    CHECK_EQ(writes, w);                            /* TST writes nothing */
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x90], 0);
    CHECK(CC(MC_6805_Z) && !CC(MC_6805_N));
}

/* The 16 relative branches, taken and not, against each flag. */
static void test_branches(void) {
    static const struct { uint8_t op, cc, taken; } t[] = {
        {0x20, 0, 1}, {0x21, 0, 0},
        {0x22, 0, 1}, {0x22, MC_6805_C, 0}, {0x22, MC_6805_Z, 0}, {0x23, MC_6805_Z, 1}, {0x23, 0, 0},
        {0x24, 0, 1}, {0x24, MC_6805_C, 0}, {0x25, MC_6805_C, 1}, {0x25, 0, 0},
        {0x26, 0, 1}, {0x26, MC_6805_Z, 0}, {0x27, MC_6805_Z, 1}, {0x27, 0, 0},
        {0x28, 0, 1}, {0x28, MC_6805_H, 0}, {0x29, MC_6805_H, 1}, {0x29, 0, 0},
        {0x2A, 0, 1}, {0x2A, MC_6805_N, 0}, {0x2B, MC_6805_N, 1}, {0x2B, 0, 0},
        {0x2C, 0, 1}, {0x2C, MC_6805_I, 0}, {0x2D, MC_6805_I, 1}, {0x2D, 0, 0},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) {
        const uint8_t p[] = {t[i].op, 0x10};
        load(p, sizeof p);
        cpu.cc = (uint8_t)(0xE0 | t[i].cc);
        CHECK_EQ(run(1), 3);
        CHECK_EQ(cpu.pc, t[i].taken ? 0x0F12 : 0x0F02);
    }
    /* BIL/BIH test the /IRQ pin; a backward branch. */
    const uint8_t p[] = {0x2E, 0x02, 0x9D, 0x9D, 0x2F, 0xFA};
    load(p, sizeof p);
    cpu.irq_pin = 0;
    run(1);
    CHECK_EQ(cpu.pc, 0x0F04);
    run(1);
    CHECK_EQ(cpu.pc, 0x0F06);
    cpu.irq_pin = 1;
    cpu.pc = 0x0F04;
    run(1);
    CHECK_EQ(cpu.pc, 0x0F00);
}

/* Bit manipulation and test-and-branch: C takes the bit. */
static void test_bits(void) {
    const uint8_t p[] = {
        0x1A, 0x90,             /* BSET 5,$90 */
        0x1B, 0x90,             /* BCLR 5,$90 */
        0x10, 0x90,             /* BSET 0,$90 */
        0x00, 0x90, 0x05,       /* BRSET 0,$90,+5: taken, C = 1 */
        0x9D, 0x9D, 0x9D, 0x9D, 0x9D,
        0x0F, 0x90, 0x02,       /* BRCLR 7,$90,+2: taken, C = 0 */
        0x9D, 0x9D,
        0x01, 0x90, 0x7F,       /* BRCLR 0,$90: not taken, C = 1 */
    };
    load(p, sizeof p);
    mem[0x90] = 0x00;
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x90], 0x20);
    run(1);
    CHECK_EQ(mem[0x90], 0x00);
    run(1);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.pc, 0x0F0E);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.pc, 0x0F13);
    CHECK(!CC(MC_6805_C));
    run(1);
    CHECK_EQ(cpu.pc, 0x0F16);
    CHECK(CC(MC_6805_C));
}

/* JSR/JMP in every mode, BSR/RTS, the stack page and its wrap. */
static void test_flow(void) {
    const uint8_t p[] = {
        0xBD, 0x80,             /* JSR $80       5 */
        0xCD, 0x10, 0x00,       /* JSR $1000     6 */
        0xAD, 0x08,             /* BSR +8        6 */
        0xAE, 0x10,             /* LDX #$10 */
        0xDD, 0x0F, 0xF0,       /* JSR $0FF0,X   7 */
        0xED, 0x70,             /* JSR $70,X     6 */
        0xFD,                   /* JSR ,X        5 */
    };
    load(p, sizeof p);
    mem[0x80] = 0x81;           /* RTS at $80 */
    mem[0x1000] = 0x81;
    mem[0x0F0F] = 0x81;
    mem[0x10] = 0x81;
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.pc, 0x80);
    CHECK_EQ(cpu.sp, 0xFD);
    CHECK_EQ(mem[0xFF], 0x02);  /* return address low at the top, then high */
    CHECK_EQ(mem[0xFE], 0x0F);
    CHECK_EQ(run(1), 6);        /* RTS */
    CHECK_EQ(cpu.pc, 0x0F02);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x1000);
    run(1);
    CHECK_EQ(run(1), 6);        /* BSR */
    CHECK_EQ(cpu.pc, 0x0F0F);
    run(2);
    CHECK_EQ(run(1), 7);
    CHECK_EQ(cpu.pc, 0x1000);
    run(1);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x80);
    run(1);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.pc, 0x10);
    /* JMP cycles. */
    const uint8_t j[] = {0xBC, 0x80, 0x00};
    load(j, sizeof j);
    mem[0x80] = 0xCC; mem[0x81] = 0x0F; mem[0x82] = 0x00;
    CHECK_EQ(run(1), 2);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.pc, 0x0F00);
    /* The stack pointer stays in $C0-$FF: 33 nested calls wrap. */
    const uint8_t deep[] = {0xAD, 0xFE};    /* BSR to itself */
    load(deep, sizeof deep);
    run(32);
    CHECK_EQ(cpu.sp, 0xFF);                 /* 32 pushes of 2 bytes: back to $FF */
    run(1);
    CHECK_EQ(cpu.sp, 0xFD);
    const uint8_t rsp[] = {0x9C};
    load(rsp, sizeof rsp);
    cpu.sp = 0xC4;
    run(1);
    CHECK_EQ(cpu.sp, 0xFF);
}

/* Interrupts: five bytes stacked (PCL, PCH, X, A, CCR), I set, the vector;
 * RTI; the I mask; SWI; WAIT and STOP clear I and halt until one. */
static void test_interrupts(void) {
    const uint8_t p[] = {0x9A, 0xA6, 0x12, 0xAE, 0x34, 0x83, 0x8F, 0x9D};
    load(p, sizeof p);
    mem[0x1FFC] = 0x10; mem[0x1FFD] = 0x00;     /* SWI */
    mem[0x1FFA] = 0x11; mem[0x1FFB] = 0x00;     /* /IRQ */
    mem[0x1000] = 0x80;                         /* RTI */
    mem[0x1100] = 0x80;
    CHECK_EQ(mc_cpu6805_interrupt(&cpu, 0x1FFA), 0);    /* I set at reset */
    run(3);
    CHECK_EQ(run(1), 10);                       /* SWI */
    CHECK_EQ(cpu.pc, 0x1000);
    CHECK(CC(MC_6805_I));
    CHECK_EQ(cpu.sp, 0xFA);
    CHECK_EQ(mem[0xFF], 0x06);                  /* PCL */
    CHECK_EQ(mem[0xFE], 0x0F);                  /* PCH */
    CHECK_EQ(mem[0xFD], 0x34);                  /* X */
    CHECK_EQ(mem[0xFC], 0x12);                  /* A */
    CHECK_EQ(mem[0xFB] & 0xE8, 0xE0);           /* CCR with I clear, upper bits 1 */
    cpu.a = 0;
    cpu.x = 0;
    CHECK_EQ(run(1), 9);                        /* RTI */
    CHECK_EQ(cpu.pc, 0x0F06);
    CHECK_EQ(cpu.a, 0x12);
    CHECK_EQ(cpu.x, 0x34);
    CHECK(!CC(MC_6805_I));
    CHECK_EQ(run(1), 2);                        /* WAIT */
    CHECK(cpu.waiting);
    CHECK_EQ(run(1), 0);                        /* halted */
    CHECK_EQ(mc_cpu6805_interrupt(&cpu, 0x1FFA), 10);
    CHECK(!cpu.waiting);
    CHECK_EQ(cpu.pc, 0x1100);
    run(1);
    CHECK_EQ(cpu.pc, 0x0F07);
    const uint8_t s[] = {0x9B, 0x8E};
    load(s, sizeof s);
    run(2);
    CHECK(cpu.stopped);
    CHECK(!CC(MC_6805_I));                      /* STOP clears I */
}

static void test_misc(void) {
    const uint8_t p[] = {0xA6, 0x42, 0x97, 0x4F, 0x9F, 0x99, 0x98, 0x9B, 0x9A, 0x31, 0x8D};
    load(p, sizeof p);
    run(2);
    CHECK_EQ(cpu.x, 0x42);
    run(2);
    CHECK_EQ(cpu.a, 0x42);
    run(1);
    CHECK(CC(MC_6805_C));
    run(1);
    CHECK(!CC(MC_6805_C));
    run(1);
    CHECK(CC(MC_6805_I));
    run(1);
    CHECK(!CC(MC_6805_I));
    CHECK_EQ(run(2), 4);                        /* undefined opcodes: two cycles each, counted */
    CHECK_EQ(cpu.illegal, 2);
    CHECK_EQ(cpu.pc, 0x0F0B);
}

/* --- the Egret/Cuda chip ---------------------------------------------------- */

static uint8_t image[MC_ADBCTL_FW_SIZE];
static uint8_t pins_in[3] = {0xFF, 0xFF, 0xFF};
static unsigned out_reports;
static uint8_t out_levels[3], out_driven[3];
static uint8_t chip_pins(void *ctx, unsigned port, uint64_t now) { (void)ctx; (void)now; return pins_in[port]; }
static void chip_outputs(void *ctx, unsigned port, uint8_t levels, uint8_t driven, uint64_t now) {
    (void)ctx; (void)now;
    out_levels[port] = levels;
    out_driven[port] = driven;
    out_reports++;
}

static void chip_load(mc_adbctl_mcu *m, const uint8_t *code, size_t n) {
    memset(image, 0, sizeof image);
    memcpy(image + 0x100, code, n);             /* code at $1000 */
    image[0x10FE] = 0x10; image[0x10FF] = 0x00; /* reset vector */
    mc_adbctl_mcu_init(m, image, 0, NULL, chip_pins, chip_outputs);
}

static void test_chip_ports(void) {
    static mc_adbctl_mcu m;
    const uint8_t p[] = {
        0xA6, 0x0F, 0xB7, 0x04,     /* DDRA = $0F */
        0xA6, 0x05, 0xB7, 0x00,     /* PA = $05 */
        0xB6, 0x00, 0xB7, 0x90,     /* $90 = PA */
        0x20, 0xFE,
    };
    chip_load(&m, p, sizeof p);
    CHECK_EQ(out_driven[0], 0);
    CHECK_EQ(out_levels[0], 0xFF);              /* released pins report 1 */
    pins_in[0] = 0xA0;
    mc_adbctl_mcu_run(&m, 30u * 3825u / 4u);    /* 30 cycles of the slow bus */
    CHECK_EQ(out_driven[0], 0x0F);
    CHECK_EQ(out_levels[0], 0xF5);
    CHECK_EQ(m.ram[0x90], 0xA5);                /* outputs from the latch, inputs from the pins */
    CHECK_EQ(mc_adbctl_mcu_peek(&m, 0x1000), 0xA6);
    pins_in[0] = 0xFF;
}

static void test_chip_timers(void) {
    static mc_adbctl_mcu m;
    const uint8_t p[] = {0x20, 0xFE};           /* BRA * (3 cycles) */
    chip_load(&m, p, sizeof p);
    /* Slow bus: 16.384 kHz, 956.25 C15M periods per cycle. */
    mc_adbctl_mcu_run(&m, 3825u * 3u / 4u);     /* 3 cycles: 2868.75 periods */
    CHECK_EQ(m.cpu.cycles, 3);
    m.pll = 0x4E;                               /* the PLL's 2.097152 MHz */
    const uint64_t c0 = m.cpu.cycles, t0 = mc_adbctl_mcu_now(&m);
    mc_adbctl_mcu_run(&m, t0 + 7471u);          /* 1000 cycles: 7470.7 periods */
    CHECK(m.cpu.cycles - c0 >= 999 && m.cpu.cycles - c0 <= 1003);
    /* TOF every 1024 cycles, RTI every 2^(14+RT); flags clear on 0, keep on 1. */
    m.ctcsr = 0x00;
    m.timer_cycles = 0;
    mc_adbctl_mcu_run(&m, mc_adbctl_mcu_now(&m) + 1030u * 3825u / 512u);
    CHECK(m.ctcsr & 0x80);
    CHECK(!(m.ctcsr & 0x40));
    CHECK_EQ(mc_adbctl_mcu_peek(&m, 0x09), (uint8_t)(m.timer_cycles >> 2));
    /* Writes through the CPU's bus: $08 = $40 clears TOF, keeps RTIF. */
    m.ctcsr |= 0x40;
    m.cpu.write(m.cpu.ctx, 0x08, 0x40);
    CHECK_EQ(m.ctcsr & 0xC0, 0x40);
    m.cpu.write(m.cpu.ctx, 0x08, 0x00);
    CHECK_EQ(m.ctcsr & 0xC0, 0);
    m.timer_cycles = 0;
    m.ctcsr = 0x00;                             /* RT = 0: 16384 cycles */
    mc_adbctl_mcu_run(&m, mc_adbctl_mcu_now(&m) + 16400u * 3825u / 512u);
    CHECK(m.ctcsr & 0x40);
    /* An enabled timer interrupt goes through $1FF8 with I clear. */
    image[0x10F8] = 0x11; image[0x10F9] = 0x00; /* $1FF8 -> $1100 */
    image[0x0200] = 0x20; image[0x0201] = 0xFE; /* $1100: BRA * */
    memcpy(m.rom, image, sizeof image);
    m.cpu.cc &= (uint8_t)~MC_6805_I;
    m.ctcsr = 0x20;                             /* TOIE */
    m.timer_cycles = 0;
    mc_adbctl_mcu_run(&m, mc_adbctl_mcu_now(&m) + 1100u * 3825u / 512u);
    CHECK_EQ(m.cpu.pc, 0x1100);
    /* The one-second flag, from the crystal: its interrupt at $1FF6. */
    static mc_adbctl_mcu s;
    chip_load(&s, p, sizeof p);
    s.ostcsr = 0x10;
    image[0x10F6] = 0x11; image[0x10F7] = 0x00;
    image[0x0200] = 0x20; image[0x0201] = 0xFE;
    memcpy(s.rom, image, sizeof image);
    s.cpu.cc &= (uint8_t)~MC_6805_I;
    mc_adbctl_mcu_run(&s, MC_C15M_HZ - 2000u);
    CHECK_EQ(s.cpu.pc & 0xFF00, 0x1000);
    mc_adbctl_mcu_run(&s, MC_C15M_HZ + 2000u);
    CHECK(s.ostcsr & 0x40);
    CHECK_EQ(s.cpu.pc, 0x1100);
    /* /IRQ: a falling edge, taken once. */
    static mc_adbctl_mcu q;
    chip_load(&q, p, sizeof p);
    image[0x10FA] = 0x11; image[0x10FB] = 0x00;
    image[0x0200] = 0x20; image[0x0201] = 0xFE;
    memcpy(q.rom, image, sizeof image);
    q.cpu.cc &= (uint8_t)~MC_6805_I;
    mc_adbctl_mcu_set_irq(&q, 0);
    mc_adbctl_mcu_run(&q, 20000);
    CHECK_EQ(q.cpu.pc, 0x1100);
    CHECK(!q.irq_latch);
    CHECK(!q.cpu.irq_pin);
}

int main(void) {
    test_reset();
    test_addressing();
    test_alu();
    test_rmw();
    test_branches();
    test_bits();
    test_flow();
    test_interrupts();
    test_misc();
    test_chip_ports();
    test_chip_timers();
    printf("test_6805: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
