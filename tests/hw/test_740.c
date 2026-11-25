/* test_740: the MELPS 740 core (cpu740.c) and the M50753 chip (m50753.c)
 * against their documents. Expected values come from the Series 740
 * Software Manual (1989): Appendix 2 (every opcode's byte and cycle count,
 * typed here from the table, independently of cpu740.c's own table), 3.3
 * (operations and flags) and 2.5 (the T flag); and from the M50753 data
 * sheet (memory map, interrupt priorities and vectors, request bits the
 * program cannot set, timer periods with the 1/n ratio, the A-D converter's
 * 72-cycle conversion, open-drain ports, reset state). */
#include "hw_test.h"
#include "m50753.h"

#include <stdlib.h>
#include <string.h>


static uint8_t mem[65536];
static mc_cpu740 cpu;
static unsigned writes;

static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; mem[a] = v; writes++; }

static void fresh(void) {
    memset(mem, 0, sizeof mem);
    mc_cpu740_init(&cpu, NULL, rd, wr);
    mem[0xFFFE] = 0x00;
    mem[0xFFFF] = 0x80;
    mc_cpu740_reset(&cpu);
    cpu.s = 0xBF;
    cpu.p = 0;
}

/* Places code at $8000 and runs one instruction; returns its cycles. */
static unsigned run1(const uint8_t *code, unsigned n) {
    memcpy(mem + 0x8000, code, n);
    cpu.pc = 0x8000;
    return mc_cpu740_step(&cpu);
}
#define RUN(...) run1((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

/* --- Appendix 2: bytes and cycles of every opcode -------------------------------- */

typedef struct { uint8_t op, bytes, cycles; } spec;
static const spec SPECS[] = {
    /* Load */
    {0xA9,2,2},{0xA5,2,3},{0xB5,2,4},{0xAD,3,4},{0xBD,3,5},{0xB9,3,5},{0xA1,2,6},{0xB1,2,6},
    {0xA2,2,2},{0xA6,2,3},{0xB6,2,4},{0xAE,3,4},{0xBE,3,5},
    {0xA0,2,2},{0xA4,2,3},{0xB4,2,4},{0xAC,3,4},{0xBC,3,5},
    {0x3C,3,4},
    /* Store */
    {0x85,2,4},{0x95,2,5},{0x8D,3,5},{0x9D,3,6},{0x99,3,6},{0x81,2,7},{0x91,2,7},
    {0x86,2,4},{0x96,2,5},{0x8E,3,5},{0x84,2,4},{0x94,2,5},{0x8C,3,6},
    /* Transfer and stack */
    {0xAA,1,2},{0x8A,1,2},{0xA8,1,2},{0x98,1,2},{0xBA,1,2},{0x9A,1,2},
    {0x48,1,3},{0x08,1,3},{0x68,1,4},{0x28,1,4},
    /* Add and subtract */
    {0x69,2,2},{0x65,2,3},{0x75,2,4},{0x6D,3,4},{0x7D,3,5},{0x79,3,5},{0x61,2,6},{0x71,2,6},
    {0xE9,2,2},{0xE5,2,3},{0xF5,2,4},{0xED,3,4},{0xFD,3,5},{0xF9,3,5},{0xE1,2,6},{0xF1,2,6},
    {0x3A,1,2},{0xE6,2,5},{0xF6,2,6},{0xEE,3,6},{0xFE,3,7},
    {0x1A,1,2},{0xC6,2,5},{0xD6,2,6},{0xCE,3,6},{0xDE,3,7},
    {0xE8,1,2},{0xCA,1,2},{0xC8,1,2},{0x88,1,2},
    /* Logic */
    {0x29,2,2},{0x25,2,3},{0x35,2,4},{0x2D,3,4},{0x3D,3,5},{0x39,3,5},{0x21,2,6},{0x31,2,6},
    {0x09,2,2},{0x05,2,3},{0x15,2,4},{0x0D,3,4},{0x1D,3,5},{0x19,3,5},{0x01,2,6},{0x11,2,6},
    {0x49,2,2},{0x45,2,3},{0x55,2,4},{0x4D,3,4},{0x5D,3,5},{0x59,3,5},{0x41,2,6},{0x51,2,6},
    {0x44,2,5},{0x24,2,3},{0x2C,3,4},{0x64,2,3},
    /* Comparison */
    {0xC9,2,2},{0xC5,2,3},{0xD5,2,4},{0xCD,3,4},{0xDD,3,5},{0xD9,3,5},{0xC1,2,6},{0xD1,2,6},
    {0xE0,2,2},{0xE4,2,3},{0xEC,3,4},{0xC0,2,2},{0xC4,2,3},{0xCC,3,4},
    /* Shift and rotate */
    {0x0A,1,2},{0x06,2,5},{0x16,2,6},{0x0E,3,6},{0x1E,3,7},
    {0x4A,1,2},{0x46,2,5},{0x56,2,6},{0x4E,3,6},{0x5E,3,7},
    {0x2A,1,2},{0x26,2,5},{0x36,2,6},{0x2E,3,6},{0x3E,3,7},
    {0x6A,1,2},{0x66,2,5},{0x76,2,6},{0x6E,3,6},{0x7E,3,7},
    {0x82,2,8},
    /* Flags */
    {0x18,1,2},{0x38,1,2},{0xD8,1,2},{0xF8,1,2},{0x58,1,2},{0x78,1,2},{0x12,1,2},{0x32,1,2},{0xB8,1,2},
    /* Other */
    {0xEA,1,2},
};

static void test_appendix2(void) {
    for (size_t i = 0; i < sizeof SPECS / sizeof *SPECS; i++) {
        fresh();
        const uint8_t code[3] = {SPECS[i].op, 0x10, 0x20};
        const unsigned cycles = run1(code, 3);
        if (cycles != SPECS[i].cycles || cpu.pc != 0x8000 + SPECS[i].bytes) {
            fprintf(stderr, "opcode $%02X: %u cycles, %u bytes; expected %u, %u\n", SPECS[i].op, cycles,
                    (unsigned)(cpu.pc - 0x8000), SPECS[i].cycles, SPECS[i].bytes);
            mc_test_failures++;
        }
    }
    /* Bit management, every bit: CLB/SEB A 2 cycles, zz 5; BBC/BBS A 4 and
     * zz 5, plus 2 when the branch is taken. */
    for (unsigned i = 0; i < 8; i++) {
        fresh();
        cpu.a = 0;
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x0B)), 2);
        CHECK_EQ(cpu.a, 1u << i);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x1B)), 2);
        CHECK_EQ(cpu.a, 0);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x0F), 0x40), 5);
        CHECK_EQ(mem[0x40], 1u << i);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x07), 0x40, 0x10), 7);      /* BBS i,$40 taken */
        CHECK_EQ(cpu.pc, 0x8003 + 0x10);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x17), 0x40, 0x10), 5);      /* BBC i,$40 not taken */
        CHECK_EQ(cpu.pc, 0x8003);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x1F), 0x40), 5);
        CHECK_EQ(mem[0x40], 0);
        cpu.a = (uint8_t)~(1u << i);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x13), 0xFE), 6);            /* BBC i,A taken, backwards */
        CHECK_EQ(cpu.pc, 0x8000);
        CHECK_EQ(RUN((uint8_t)(i << 5 | 0x03), 0xFE), 4);            /* BBS i,A not taken */
        CHECK_EQ(cpu.pc, 0x8002);
    }
}

/* --- operations --------------------------------------------------------------- */

static void test_loads_and_flags(void) {
    fresh();
    RUN(0xA9, 0x00);                        /* LDA #0 */
    CHECK(cpu.p & MC_740_Z);
    RUN(0xA9, 0x80);
    CHECK(cpu.p & MC_740_N);
    CHECK(!(cpu.p & MC_740_Z));
    mem[0x0034] = 0x55;
    RUN(0xA6, 0x34);                        /* LDX $34 */
    CHECK_EQ(cpu.x, 0x55);
    mem[0x1234 + 0x55] = 0x77;
    RUN(0xBC, 0x34, 0x12);                  /* LDY $1234,X */
    CHECK_EQ(cpu.y, 0x77);
    RUN(0x3C, 0x99, 0x20);                  /* LDM #$99,$20: immediate first */
    CHECK_EQ(mem[0x20], 0x99);
    /* (zz,X) wraps in page zero; (zz),Y adds Y to the pointer. */
    fresh();
    cpu.x = 0x01;
    mem[0xFF] = 0x00; mem[0x00] = 0x30;
    mem[0x3000] = 0x42;
    RUN(0xA1, 0xFE);                        /* LDA ($FE,X): pointer at $FF/$00 */
    CHECK_EQ(cpu.a, 0x42);
    cpu.y = 0x10;
    mem[0x10] = 0x00; mem[0x11] = 0x30;
    mem[0x3010] = 0x24;
    RUN(0xB1, 0x10);                        /* LDA ($10),Y */
    CHECK_EQ(cpu.a, 0x24);
}

static void test_arithmetic(void) {
    fresh();
    cpu.a = 0x7F;
    RUN(0x69, 0x01);                        /* ADC #1: overflow */
    CHECK_EQ(cpu.a, 0x80);
    CHECK(cpu.p & MC_740_V);
    CHECK(cpu.p & MC_740_N);
    CHECK(!(cpu.p & MC_740_C));
    cpu.a = 0xFF;
    RUN(0x69, 0x01);
    CHECK_EQ(cpu.a, 0x00);
    CHECK(cpu.p & MC_740_C);
    CHECK(cpu.p & MC_740_Z);
    /* SBC: C set means no borrow. */
    cpu.p = MC_740_C;
    cpu.a = 0x10;
    RUN(0xE9, 0x20);
    CHECK_EQ(cpu.a, 0xF0);
    CHECK(!(cpu.p & MC_740_C));
    /* Decimal mode (D set): 2-digit BCD with the carry out of 99. */
    cpu.p = MC_740_D;
    cpu.a = 0x45;
    RUN(0x69, 0x38);
    CHECK_EQ(cpu.a, 0x83);
    CHECK(!(cpu.p & MC_740_C));
    cpu.p = MC_740_D;
    cpu.a = 0x75;
    RUN(0x69, 0x30);
    CHECK_EQ(cpu.a, 0x05);
    CHECK(cpu.p & MC_740_C);
    cpu.p = MC_740_D | MC_740_C;
    cpu.a = 0x10;
    RUN(0xE9, 0x01);
    CHECK_EQ(cpu.a, 0x09);
    CHECK(cpu.p & MC_740_C);
    cpu.p = MC_740_D | MC_740_C;
    cpu.a = 0x00;
    RUN(0xE9, 0x01);
    CHECK_EQ(cpu.a, 0x99);
    CHECK(!(cpu.p & MC_740_C));
    /* CMP, CPX, CPY: C when the register is at least the operand. */
    cpu.p = 0;
    cpu.a = 0x40;
    RUN(0xC9, 0x40);
    CHECK(cpu.p & MC_740_Z);
    CHECK(cpu.p & MC_740_C);
    cpu.x = 0x01;
    RUN(0xE0, 0x02);
    CHECK(!(cpu.p & MC_740_C));
    CHECK(cpu.p & MC_740_N);
    /* INC/DEC A, memory. */
    cpu.a = 0xFF;
    RUN(0x3A);
    CHECK_EQ(cpu.a, 0);
    CHECK(cpu.p & MC_740_Z);
    RUN(0x1A);
    CHECK_EQ(cpu.a, 0xFF);
    mem[0x50] = 0x00;
    RUN(0xC6, 0x50);
    CHECK_EQ(mem[0x50], 0xFF);
}

/* 2.5: with T set, operations act on M(X), the zero page byte X addresses,
 * and leave A; cycles grow by 3 (ADC, AND, EOR, ORA, SBC), 2 (LDA), 1 (CMP). */
static void test_t_flag(void) {
    fresh();
    RUN(0x32);                              /* SET */
    CHECK(cpu.p & MC_740_T);
    cpu.a = 0x11;
    cpu.x = 0x60;
    mem[0x60] = 0x05;
    mem[0x61] = 0x03;
    CHECK_EQ(RUN(0x65, 0x61), 3 + 3);       /* ADC $61: M($60) = 5 + 3 */
    CHECK_EQ(mem[0x60], 0x08);
    CHECK_EQ(cpu.a, 0x11);
    CHECK_EQ(RUN(0x29, 0x0C), 2 + 3);       /* AND #$0C */
    CHECK_EQ(mem[0x60], 0x08);
    CHECK_EQ(RUN(0x09, 0x01), 2 + 3);       /* ORA #1 */
    CHECK_EQ(mem[0x60], 0x09);
    CHECK_EQ(RUN(0x49, 0xFF), 2 + 3);       /* EOR #$FF */
    CHECK_EQ(mem[0x60], 0xF6);
    CHECK(cpu.p & MC_740_N);
    CHECK_EQ(RUN(0xA9, 0x22), 2 + 2);       /* LDA #$22 into M(X) */
    CHECK_EQ(mem[0x60], 0x22);
    CHECK_EQ(cpu.a, 0x11);
    CHECK_EQ(RUN(0xC9, 0x22), 2 + 1);       /* CMP #$22 against M(X) */
    CHECK(cpu.p & MC_740_Z);
    cpu.p |= MC_740_C;
    CHECK_EQ(RUN(0xE9, 0x02), 2 + 3);       /* SBC #2 */
    CHECK_EQ(mem[0x60], 0x20);
    CHECK_EQ(cpu.a, 0x11);
    RUN(0x85, 0x70);                        /* STA ignores T */
    CHECK_EQ(mem[0x70], 0x11);
    RUN(0x12);                              /* CLT */
    CHECK(!(cpu.p & MC_740_T));
    CHECK_EQ(RUN(0x69, 0x01), 2);
}

static void test_shifts_and_bits(void) {
    fresh();
    cpu.a = 0x81;
    RUN(0x0A);                              /* ASL A */
    CHECK_EQ(cpu.a, 0x02);
    CHECK(cpu.p & MC_740_C);
    RUN(0x2A);                              /* ROL A: carry in */
    CHECK_EQ(cpu.a, 0x05);
    CHECK(!(cpu.p & MC_740_C));
    RUN(0x4A);                              /* LSR A */
    CHECK_EQ(cpu.a, 0x02);
    CHECK(cpu.p & MC_740_C);
    RUN(0x6A);                              /* ROR A */
    CHECK_EQ(cpu.a, 0x81);
    mem[0x30] = 0xA5;
    RUN(0x82, 0x30);                        /* RRF: swap nibbles */
    CHECK_EQ(mem[0x30], 0x5A);
    RUN(0x44, 0x30);                        /* COM */
    CHECK_EQ(mem[0x30], 0xA5);
    CHECK(cpu.p & MC_740_N);
    mem[0x31] = 0;
    RUN(0x64, 0x31);                        /* TST */
    CHECK(cpu.p & MC_740_Z);
    mem[0x32] = 0xC0;
    cpu.a = 0x3F;
    RUN(0x24, 0x32);                        /* BIT: N, V from memory, Z from A & M */
    CHECK(cpu.p & MC_740_N);
    CHECK(cpu.p & MC_740_V);
    CHECK(cpu.p & MC_740_Z);
    /* SEB/CLB change no flag. */
    const uint8_t p = cpu.p;
    RUN(0xEF, 0x33);
    CHECK_EQ(mem[0x33], 0x80);
    CHECK_EQ(cpu.p, p);
}

static void test_jumps_and_stack(void) {
    fresh();
    /* JSR pushes the address of its last byte; RTS adds 1. */
    CHECK_EQ(RUN(0x20, 0x00, 0x90), 6);
    CHECK_EQ(cpu.pc, 0x9000);
    CHECK_EQ(cpu.s, 0xBD);
    CHECK_EQ(mem[0xBF], 0x80);
    CHECK_EQ(mem[0xBE], 0x02);
    mem[0x9000] = 0x60;
    cpu.pc = 0x9000;
    CHECK_EQ(mc_cpu740_step(&cpu), 6);
    CHECK_EQ(cpu.pc, 0x8003);
    /* JSR \$FF02: the special page. */
    CHECK_EQ(RUN(0x22, 0x02), 5);
    CHECK_EQ(cpu.pc, 0xFF02);
    cpu.s = 0xBF;
    /* JSR ($zz) and JMP ($zz). */
    mem[0x1F] = 0x34; mem[0x20] = 0x12;
    CHECK_EQ(RUN(0x02, 0x1F), 7);
    CHECK_EQ(cpu.pc, 0x1234);
    CHECK_EQ(mem[0xBF], 0x80);
    CHECK_EQ(mem[0xBE], 0x01);
    cpu.s = 0xBF;
    CHECK_EQ(RUN(0xB2, 0x1F), 4);
    CHECK_EQ(cpu.pc, 0x1234);
    mem[0x4000] = 0x78; mem[0x4001] = 0x56;
    CHECK_EQ(RUN(0x6C, 0x00, 0x40), 5);
    CHECK_EQ(cpu.pc, 0x5678);
    CHECK_EQ(RUN(0x4C, 0x00, 0x70), 3);
    CHECK_EQ(cpu.pc, 0x7000);
    /* BRA, and Bcc: 2 cycles, 4 when taken. */
    CHECK_EQ(RUN(0x80, 0x10), 4);
    CHECK_EQ(cpu.pc, 0x8012);
    cpu.p = MC_740_Z;
    CHECK_EQ(RUN(0xF0, 0x80), 4);
    CHECK_EQ(cpu.pc, 0x8002 - 0x80);
    CHECK_EQ(RUN(0xD0, 0x80), 2);
    /* PHA/PLA/PHP/PLP and the stack page bit. */
    cpu.a = 0x5A;
    RUN(0x48);
    CHECK_EQ(mem[0xBF], 0x5A);
    cpu.a = 0;
    RUN(0x68);
    CHECK_EQ(cpu.a, 0x5A);
    cpu.stack_page = 0x0100;
    cpu.p = MC_740_C | MC_740_T;
    RUN(0x08);
    CHECK_EQ(mem[0x01BF], MC_740_C | MC_740_T);
    cpu.p = 0;
    RUN(0x28);
    CHECK_EQ(cpu.p, MC_740_C | MC_740_T);
    /* TXS/TSX. */
    cpu.x = 0x80;
    RUN(0x9A);
    CHECK_EQ(cpu.s, 0x80);
    RUN(0xBA);
    CHECK_EQ(cpu.x, 0x80);
    CHECK(cpu.p & MC_740_N);
}

/* BRK pushes PC + 2 and PS with B set, takes $FFF4; an interrupt pushes PS
 * with B clear; RTI restores both. */
static void test_brk_and_interrupts(void) {
    fresh();
    mem[0xFFF4] = 0x00; mem[0xFFF5] = 0xA0;
    cpu.p = MC_740_C;
    CHECK_EQ(RUN(0x00, 0xEA), 7);
    CHECK_EQ(cpu.pc, 0xA000);
    CHECK_EQ(mem[0xBF], 0x80);
    CHECK_EQ(mem[0xBE], 0x02);
    CHECK_EQ(mem[0xBD], MC_740_C | MC_740_B);
    CHECK(cpu.p & MC_740_I);
    mem[0xA000] = 0x40;                     /* RTI */
    CHECK_EQ(mc_cpu740_step(&cpu), 6);
    CHECK_EQ(cpu.pc, 0x8002);
    CHECK_EQ(cpu.p, MC_740_C | MC_740_B);
    fresh();
    cpu.pc = 0x8123;
    mem[0xFFFA] = 0x00; mem[0xFFFB] = 0xB0;
    cpu.p = MC_740_I;
    CHECK_EQ(mc_cpu740_interrupt(&cpu, 0xFFFA), 0);    /* masked */
    cpu.p = MC_740_B;
    CHECK_EQ(mc_cpu740_interrupt(&cpu, 0xFFFA), 7);
    CHECK_EQ(cpu.pc, 0xB000);
    CHECK_EQ(mem[0xBD], 0);                 /* B clear */
    CHECK_EQ(mem[0xBE], 0x23);
    CHECK_EQ(mem[0xBF], 0x81);
}

static void test_undefined_and_muldiv(void) {
    fresh();
    CHECK_EQ(RUN(0x04), 2);
    CHECK_EQ(cpu.illegal, 1u);
    CHECK_EQ(cpu.pc, 0x8001);
    CHECK_EQ(RUN(0x62, 0x10), 2);           /* MUL is not an M50753 instruction */
    CHECK_EQ(cpu.illegal, 2u);
    /* On a part with them (Appendix 2): MUL M(S),A <- A x M(zz+X), S - 1. */
    fresh();
    cpu.muldiv = 1;
    cpu.a = 200;
    cpu.x = 1;
    mem[0x11] = 3;
    CHECK_EQ(RUN(0x62, 0x10), 15);
    CHECK_EQ(cpu.a, (600 & 0xFF));
    CHECK_EQ(mem[0xBF], 600 >> 8);
    CHECK_EQ(cpu.s, 0xBE);
    /* DIV A <- (M(zz+X+1),M(zz+X)) / A, M(S) <- ~remainder. */
    cpu.a = 7;
    mem[0x11] = 0x64; mem[0x12] = 0x00;     /* 100 */
    CHECK_EQ(RUN(0xE2, 0x10), 16);
    CHECK_EQ(cpu.a, 14);
    CHECK_EQ(mem[0xBE], (uint8_t)~2);
    /* WIT and STP stop the core. */
    fresh();
    RUN(0xC2);
    CHECK(cpu.waiting);
    CHECK_EQ(mc_cpu740_step(&cpu), 0);
    fresh();
    RUN(0x42);
    CHECK(cpu.stopped);
}

/* --- the M50753 ------------------------------------------------------------------ */

static mc_m50753 chip;
static uint8_t rom[MC_M50753_ROM_SIZE];
static int out_port[16];
static unsigned out_count;

static void chip_out(void *ctx, int port, uint8_t levels) {
    (void)ctx;
    out_port[port] = levels;
    out_count++;
}

/* A ROM whose reset vector points at `code` placed at $E800. */
static void chip_with(const uint8_t *code, size_t n) {
    memset(rom, 0xEA, sizeof rom);
    memcpy(rom, code, n);
    rom[0x17FE] = 0x00; rom[0x17FF] = 0xE8;     /* reset $E800 */
    for (unsigned v = 0xFFF4; v < 0xFFFE; v += 2) {
        rom[v - 0xE800] = 0x00;
        rom[v - 0xE800 + 1] = (uint8_t)(0xF0 + ((v - 0xFFF4) >> 1));  /* $F000, $F100... */
    }
    mc_m50753_init(&chip, rom, NULL, chip_out);
    mc_m50753_reset(&chip);
}
#define CHIP(...) chip_with((const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

static void test_chip_map_and_reset(void) {
    CHIP(0xEA);
    CHECK_EQ(chip.cpu.pc, 0xE800);
    CHECK(chip.cpu.p & MC_740_I);
    /* Figure 14. */
    CHECK_EQ(mc_m50753_read(&chip, 0xE1), 0);
    CHECK_EQ(mc_m50753_read(&chip, 0xFC), 0xFF);
    CHECK_EQ(mc_m50753_read(&chip, 0xFD), 0x01);
    CHECK_EQ(mc_m50753_read(&chip, 0xFE), 0);
    CHECK_EQ(mc_m50753_read(&chip, 0xFF), 0);
    /* RAM $00-$BF, ROM $E800-$FFFF. */
    mc_m50753_write(&chip, 0xBF, 0x12);
    CHECK_EQ(mc_m50753_read(&chip, 0xBF), 0x12);
    CHECK_EQ(mc_m50753_read(&chip, 0xE800), 0xEA);
    mc_m50753_write(&chip, 0xE800, 0x00);
    CHECK_EQ(mc_m50753_read(&chip, 0xE800), 0xEA);
    /* The stack page bit. */
    mc_m50753_write(&chip, 0xFF, 0x10);
    CHECK_EQ(chip.cpu.stack_page, 0x0100);
}

static void test_chip_ports(void) {
    CHIP(0xEA);
    /* Inputs float and read the board's level; outputs read the latch and
     * open-drain: a 0 pulls the pin low, a 1 releases it. */
    mc_m50753_set_pins(&chip, 2, 0x5A, 0xFF);
    CHECK_EQ(mc_m50753_read(&chip, 0xE4), 0x5A);
    out_count = 0;
    mc_m50753_write(&chip, 0xE4, 0x0F);     /* latch only: still inputs */
    CHECK_EQ(out_count, 0u);
    mc_m50753_write(&chip, 0xE5, 0xFF);
    CHECK_EQ(out_count, 1u);
    CHECK_EQ(out_port[2], 0x0F);
    CHECK_EQ(mc_m50753_read(&chip, 0xE4), 0x0F);
    CHECK_EQ(mc_m50753_pins(&chip, 2), 0x0A);    /* the board pulls some low too */
    /* P4 has four bits. */
    mc_m50753_write(&chip, 0xEB, 0xFF);
    mc_m50753_write(&chip, 0xEA, 0xF0);
    CHECK_EQ(out_port[4], 0x00);
    CHECK_EQ(mc_m50753_read(&chip, 0xEB), 0xFF);
    CHECK_EQ(mc_m50753_read(&chip, 0xEA) & 0x0F, 0);
}

/* Timer X with prescaler X: requests every n x m periods of f(XIN)/16
 * (4 phi cycles each), ratio 1/n (Programming notes 1). */
static void test_chip_timers(void) {
    CHIP(0x3C, 0x04, 0xFC, 0x3C, 0x03, 0xFD, 0x80, 0xFE);   /* LDM #4,$FC; LDM #3,$FD; BRA * */
    mc_m50753_step(&chip);
    mc_m50753_step(&chip);
    uint64_t start = chip.cycles;
    while (!(chip.tcr & 0x80u) && chip.cycles - start < 10000) mc_m50753_step(&chip);
    /* The first period runs from the loads; the next ones are whole. */
    CHECK(chip.cycles - start <= 4 * 4 * 3 + 4);
    mc_m50753_write(&chip, 0xFF, 0x00);     /* clear the request (bit 7 written 0) */
    CHECK(!(chip.tcr & 0x80u));
    mc_m50753_write(&chip, 0xFF, 0x80);     /* the program cannot set it */
    CHECK(!(chip.tcr & 0x80u));
    start = chip.cycles;
    while (!(chip.tcr & 0x80u) && chip.cycles - start < 10000) mc_m50753_step(&chip);
    CHECK(chip.cycles - start >= 48 - 4 && chip.cycles - start <= 48 + 4);
    /* The count stop bit. */
    mc_m50753_write(&chip, 0xFF, 0x20);
    start = chip.cycles;
    while (chip.cycles - start < 400) mc_m50753_step(&chip);
    CHECK(!(chip.tcr & 0x80u));
    /* Timers 1 and 2 behind their prescaler; timer 1 requests in bit 5 of
     * $FE, timer 2 in bit 3. */
    mc_m50753_write(&chip, 0xF9, 2);
    mc_m50753_write(&chip, 0xFA, 5);
    mc_m50753_write(&chip, 0xFB, 10);
    start = chip.cycles;
    while (!(chip.icr & 0x20u) && chip.cycles - start < 10000) mc_m50753_step(&chip);
    CHECK(chip.cycles - start >= 40 - 4 && chip.cycles - start <= 40 + 4);
    CHECK(!(chip.icr & 0x08u));
    while (!(chip.icr & 0x08u) && chip.cycles - start < 10000) mc_m50753_step(&chip);
    CHECK(chip.cycles - start >= 80 - 4 && chip.cycles - start <= 80 + 4);
}

/* A-D: a write to $F2 starts the channel of $F3 bits 0-2; the result and
 * (bit 3 set) the request in bit 5 of $FE come 72 cycles later. */
static void test_chip_adc(void) {
    CHIP(0x80, 0xFE);
    chip.analog[5] = 0xA7;
    mc_m50753_write(&chip, 0xF3, 0x0D);
    mc_m50753_write(&chip, 0xF2, 0);
    uint64_t start = chip.cycles;
    while (!(chip.icr & 0x20u) && chip.cycles - start < 1000) mc_m50753_step(&chip);
    CHECK(chip.cycles - start >= MC_M50753_ADC_CYCLES && chip.cycles - start <= MC_M50753_ADC_CYCLES + 4);
    CHECK_EQ(mc_m50753_read(&chip, 0xEF), 0xA7);
    chip.in_port = 0x3C;
    CHECK_EQ(mc_m50753_read(&chip, 0xEE), 0x3C);
}

/* Table 1: priorities and vectors; acceptance clears the request; INT1/INT2
 * request on H-to-L edges; WIT resumes on a request. */
static void test_chip_interrupts(void) {
    CHIP(0x58, 0xC2, 0x80, 0xFE);           /* CLI; WIT; BRA * */
    chip.cpu.s = 0xBF;                      /* the stack in RAM, below the registers */
    mc_m50753_write(&chip, 0xFE, 0x55);     /* every enable */
    mc_m50753_write(&chip, 0xFF, 0x40);
    mc_m50753_step(&chip);                  /* CLI */
    mc_m50753_step(&chip);                  /* WIT */
    CHECK(chip.cpu.waiting);
    mc_m50753_step(&chip);
    CHECK(chip.cpu.waiting);
    mc_m50753_set_int2(&chip, 0);           /* INT2 request (lowest) */
    mc_m50753_set_int1(&chip, 0);           /* INT1 request (highest) */
    CHECK_EQ(chip.icr & 0x82u, 0x82u);
    mc_m50753_step(&chip);
    CHECK_EQ(chip.cpu.pc, 0xF400);          /* INT1: $FFFC */
    CHECK(!(chip.icr & 0x80u));
    CHECK(chip.icr & 0x02u);
    chip.cpu.p &= (uint8_t)~MC_740_I;
    chip.tcr |= 0x80u;                      /* timer X before INT2 */
    mc_m50753_step(&chip);
    CHECK_EQ(chip.cpu.pc, 0xF300);
    chip.cpu.p &= (uint8_t)~MC_740_I;
    mc_m50753_step(&chip);
    CHECK_EQ(chip.cpu.pc, 0xF000);          /* INT2: $FFF4 */
    CHECK_EQ(chip.icr & 0x02u, 0);
    /* A rising edge requests nothing. */
    mc_m50753_set_int1(&chip, 1);
    CHECK(!(chip.icr & 0x80u));
}

int main(void) {
    test_appendix2();
    test_loads_and_flags();
    test_arithmetic();
    test_t_flag();
    test_shifts_and_bits();
    test_jumps_and_stack();
    test_brk_and_interrupts();
    test_undefined_and_muldiv();
    test_chip_map_and_reset();
    test_chip_ports();
    test_chip_timers();
    test_chip_adc();
    test_chip_interrupts();
    printf("test_740: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
