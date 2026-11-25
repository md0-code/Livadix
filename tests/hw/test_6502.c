/* The 65C02 core against its data sheets (R65C02 and W65C02S opcode
 * matrices: every opcode's length and base cycle count; the operations and
 * flags of the MCS6500 programming manual; the CMOS rules: decimal-mode
 * N/Z and its extra cycle, D cleared by interrupts, JMP (abs) across a page,
 * the page-crossing and branch penalties, the reserved NOPs) and the
 * interrupt sequences. Programs are assembled by hand from the opcode
 * matrix. */
#include "hw_test.h"
#include "../../src/hw/cpu6502.h"

static uint8_t mem[0x10000];
static unsigned reads, writes;
static uint8_t rd(void *ctx, uint16_t a) { (void)ctx; reads++; return mem[a]; }
static void wr(void *ctx, uint16_t a, uint8_t v) { (void)ctx; writes++; mem[a] = v; }

static mc_cpu6502 cpu;

/* Clears memory, puts `code` at $0200, the vectors at $FFFA-$FFFF
 * (NMI $0300, RESET $0200, IRQ/BRK $0400) and resets. */
static void load(const uint8_t *code, size_t n, mc_6502_variant variant) {
    memset(mem, 0, sizeof mem);
    memcpy(mem + 0x0200, code, n);
    mem[0xFFFA] = 0x00; mem[0xFFFB] = 0x03;
    mem[0xFFFC] = 0x00; mem[0xFFFD] = 0x02;
    mem[0xFFFE] = 0x00; mem[0xFFFF] = 0x04;
    mc_cpu6502_init(&cpu, NULL, rd, wr, variant);
    mc_cpu6502_reset(&cpu);
}

static unsigned run(unsigned n) {
    unsigned c = 0;
    while (n--) c += mc_cpu6502_step(&cpu);
    return c;
}

#define P(f) ((cpu.p & (f)) != 0)

/* Bytes and base cycles of every opcode (high nibble = bytes, low = cycles):
 * W65C02S data sheet, opcode matrix, and the R65C02's ($CB, $DB one-byte
 * one-cycle NOPs). Base = no page crossed, branch not taken, binary mode. */
static const uint8_t matrix[256] = {
    /*        0     1     2     3     4     5     6     7     8     9     A     B     C     D     E     F */
    /* 0 */ 0x17, 0x26, 0x22, 0x11, 0x25, 0x23, 0x25, 0x25, 0x13, 0x22, 0x12, 0x11, 0x36, 0x34, 0x36, 0x35,
    /* 1 */ 0x22, 0x25, 0x25, 0x11, 0x25, 0x24, 0x26, 0x25, 0x12, 0x34, 0x12, 0x11, 0x36, 0x34, 0x36, 0x35,
    /* 2 */ 0x36, 0x26, 0x22, 0x11, 0x23, 0x23, 0x25, 0x25, 0x14, 0x22, 0x12, 0x11, 0x34, 0x34, 0x36, 0x35,
    /* 3 */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x26, 0x25, 0x12, 0x34, 0x12, 0x11, 0x34, 0x34, 0x36, 0x35,
    /* 4 */ 0x16, 0x26, 0x22, 0x11, 0x23, 0x23, 0x25, 0x25, 0x13, 0x22, 0x12, 0x11, 0x33, 0x34, 0x36, 0x35,
    /* 5 */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x26, 0x25, 0x12, 0x34, 0x13, 0x11, 0x38, 0x34, 0x36, 0x35,
    /* 6 */ 0x16, 0x26, 0x22, 0x11, 0x23, 0x23, 0x25, 0x25, 0x14, 0x22, 0x12, 0x11, 0x36, 0x34, 0x36, 0x35,
    /* 7 */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x26, 0x25, 0x12, 0x34, 0x14, 0x11, 0x36, 0x34, 0x36, 0x35,
    /* 8 */ 0x23, 0x26, 0x22, 0x11, 0x23, 0x23, 0x23, 0x25, 0x12, 0x22, 0x12, 0x11, 0x34, 0x34, 0x34, 0x35,
    /* 9 */ 0x22, 0x26, 0x25, 0x11, 0x24, 0x24, 0x24, 0x25, 0x12, 0x35, 0x12, 0x11, 0x34, 0x35, 0x35, 0x35,
    /* A */ 0x22, 0x26, 0x22, 0x11, 0x23, 0x23, 0x23, 0x25, 0x12, 0x22, 0x12, 0x11, 0x34, 0x34, 0x34, 0x35,
    /* B */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x24, 0x25, 0x12, 0x34, 0x12, 0x11, 0x34, 0x34, 0x34, 0x35,
    /* C */ 0x22, 0x26, 0x22, 0x11, 0x23, 0x23, 0x25, 0x25, 0x12, 0x22, 0x12, 0x11, 0x34, 0x34, 0x36, 0x35,
    /* D */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x26, 0x25, 0x12, 0x34, 0x13, 0x11, 0x34, 0x34, 0x37, 0x35,
    /* E */ 0x22, 0x26, 0x22, 0x11, 0x23, 0x23, 0x25, 0x25, 0x12, 0x22, 0x12, 0x11, 0x34, 0x34, 0x36, 0x35,
    /* F */ 0x22, 0x25, 0x25, 0x11, 0x24, 0x24, 0x26, 0x25, 0x12, 0x34, 0x14, 0x11, 0x34, 0x34, 0x37, 0x35,
};

/* Every opcode once: length (PC advance) and base cycles. Operand bytes
 * are 0, X = Y = 0, flags set so the conditional branches fall through and
 * zero page $00 so BBR/BBS do too. */
static void test_matrix(void) {
    for (unsigned op = 0; op < 256; op++) {
        if (op == 0x00 || op == 0x20 || op == 0x40 || op == 0x4C || op == 0x60 || op == 0x6C || op == 0x7C) continue;
        const uint8_t code[] = {(uint8_t)op, 0x00, 0x00, 0xEA};
        load(code, sizeof code, MC_6502_R65C02);
        cpu.s = 0xF0;
        cpu.p = MC_6502_U | MC_6502_B | MC_6502_I;
        switch (op) {
        case 0x10: cpu.p |= MC_6502_N; break;
        case 0x50: cpu.p |= MC_6502_V; break;
        case 0x90: cpu.p |= MC_6502_C; break;
        case 0xD0: cpu.p |= MC_6502_Z; break;
        default: break;
        }
        if ((op & 0x0F) == 0x0F) mem[0x00] = (op & 0x80) ? 0x00 : 0xFF;
        const unsigned cycles = mc_cpu6502_step(&cpu);
        const unsigned want_len = matrix[op] >> 4, want_cycles = matrix[op] & 15u;
        if (cpu.pc != 0x0200 + want_len || cycles != want_cycles)
            fprintf(stderr, "opcode %02X: length %u cycles %u, want %u %u\n", op, cpu.pc - 0x0200u, cycles, want_len, want_cycles);
        CHECK_EQ(cpu.pc, 0x0200 + want_len);
        CHECK_EQ(cycles, want_cycles);
    }
    /* The bits of opcodes executed. */
    CHECK(cpu.used[0xFF >> 5] & (1u << (0xFF & 31)));
}

static void test_reset(void) {
    const uint8_t p[] = {0xEA};
    load(p, sizeof p, MC_6502_R65C02);
    CHECK_EQ(cpu.pc, 0x0200);
    CHECK(P(MC_6502_I));
    CHECK(!P(MC_6502_D));
    CHECK_EQ(cpu.s, 0xFA);              /* S decremented by 3 from $FD */
    cpu.p |= MC_6502_D;
    mc_cpu6502_reset(&cpu);
    CHECK(!P(MC_6502_D));               /* CMOS: reset clears D */
}

/* Loads, stores and every address mode with indexing and wrap. */
static void test_modes(void) {
    const uint8_t p[] = {
        0xA2, 0x05,             /* LDX #$05 */
        0xA0, 0x10,             /* LDY #$10 */
        0xB5, 0xFE,             /* LDA $FE,X  -> $03 (zero page wrap) */
        0x85, 0x40,             /* STA $40 */
        0xA1, 0x20,             /* LDA ($20,X) -> pointer at $25 */
        0x85, 0x41,             /* STA $41 */
        0xB1, 0x30,             /* LDA ($30),Y */
        0x85, 0x42,             /* STA $42 */
        0xB2, 0x30,             /* LDA ($30) */
        0x85, 0x43,             /* STA $43 */
        0xB6, 0xF8,             /* LDX $F8,Y  -> $08 */
        0x96, 0x50,             /* STX $50,Y  -> $60 */
        0xBD, 0x00, 0x10,       /* LDA $1000,X (X = 2) */
        0xB2, 0xFF,             /* LDA ($FF): pointer low at $FF, high at $00 */
    };
    load(p, sizeof p, MC_6502_R65C02);
    mem[0x03] = 0x33;
    mem[0x25] = 0x00; mem[0x26] = 0x30;  mem[0x3000] = 0x44;
    mem[0x30] = 0xF8; mem[0x31] = 0x30;  mem[0x3108] = 0x55; mem[0x30F8] = 0x66;
    mem[0x08] = 0x02;
    mem[0x1002] = 0x77;
    mem[0xFF] = 0x34; mem[0x00] = 0x12; mem[0x1234] = 0x88;
    CHECK_EQ(run(3), 2 + 2 + 4);
    CHECK_EQ(cpu.a, 0x33);
    run(1);
    CHECK_EQ(mem[0x40], 0x33);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.a, 0x44);
    run(1);
    CHECK_EQ(run(1), 6);                /* ($30),Y: $30F8 + $10 crosses a page */
    CHECK_EQ(cpu.a, 0x55);
    run(1);
    CHECK_EQ(run(1), 5);                /* (zp) */
    CHECK_EQ(cpu.a, 0x66);
    run(1);
    CHECK_EQ(run(1), 4);
    CHECK_EQ(cpu.x, 0x02);
    CHECK_EQ(run(1), 4);
    CHECK_EQ(mem[0x60], 0x02);
    CHECK_EQ(run(1), 4);                /* $1000,X no crossing */
    CHECK_EQ(cpu.a, 0x77);
    run(1);
    CHECK_EQ(cpu.a, 0x88);

    /* Page crossing on absolute indexed reads, none on stores. */
    const uint8_t q[] = {
        0xA2, 0x01,             /* LDX #1 */
        0xBD, 0xFF, 0x10,       /* LDA $10FF,X: 5 */
        0x9D, 0xFF, 0x10,       /* STA $10FF,X: 5 */
        0xA0, 0x01,             /* LDY #1 */
        0xB9, 0xFF, 0x10,       /* LDA $10FF,Y: 5 */
        0x99, 0x00, 0x10,       /* STA $1000,Y: 5 */
        0xBE, 0xFF, 0x20,       /* LDX $20FF,Y: 5 */
        0xBC, 0xFF, 0x20,       /* LDY $20FF,X (X = mem[$2100]) */
    };
    load(q, sizeof q, MC_6502_R65C02);
    mem[0x1100] = 0x9A;
    mem[0x2100] = 0x01;
    run(1);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.a, 0x9A);
    CHECK_EQ(run(1), 5);
    run(1);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x1001], 0x9A);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.x, 0x01);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.y, 0x01);
}

static void test_arith(void) {
    const uint8_t p[] = {
        0x18, 0xA9, 0x50, 0x69, 0x50,   /* CLC; LDA #$50; ADC #$50 -> $A0, V N */
        0x38, 0xA9, 0x50, 0xE9, 0xB0,   /* SEC; LDA #$50; SBC #$B0 -> $A0, V N, borrow */
        0x18, 0xA9, 0xFF, 0x69, 0x01,   /* CLC; LDA #$FF; ADC #$01 -> 0, C Z */
        0x38, 0xA9, 0x00, 0xE9, 0x01,   /* SEC; LDA #0; SBC #1 -> $FF, C clear */
        0xF8,                           /* SED */
        0x18, 0xA9, 0x58, 0x69, 0x46,   /* CLC; LDA #$58; ADC #$46 -> $04, C (BCD 58+46=104) */
        0x38, 0xA9, 0x12, 0xE9, 0x34,   /* SEC; LDA #$12; SBC #$34 -> $78, borrow */
        0x18, 0xA9, 0x99, 0x69, 0x01,   /* CLC; LDA #$99; ADC #$01 -> $00, C, Z (CMOS: valid) */
        0x38, 0xA9, 0x81, 0xE9, 0x01,   /* SEC; LDA #$81; SBC #$01 -> $80, N (CMOS: valid) */
        0xD8,                           /* CLD */
        0xA9, 0x40, 0xC9, 0x40,         /* LDA #$40; CMP #$40: Z C */
        0xC9, 0x41,                     /* CMP #$41: N, no C */
        0xA2, 0x10, 0xE0, 0x08,         /* LDX #$10; CPX #$08: C */
        0xA0, 0x80, 0xC0, 0x80,         /* LDY #$80; CPY #$80: Z C */
    };
    load(p, sizeof p, MC_6502_R65C02);
    CHECK_EQ(run(3), 2 + 2 + 2);
    CHECK_EQ(cpu.a, 0xA0);
    CHECK(P(MC_6502_V)); CHECK(P(MC_6502_N)); CHECK(!P(MC_6502_C)); CHECK(!P(MC_6502_Z));
    run(3);
    CHECK_EQ(cpu.a, 0xA0);
    CHECK(P(MC_6502_V)); CHECK(P(MC_6502_N)); CHECK(!P(MC_6502_C));
    run(3);
    CHECK_EQ(cpu.a, 0x00);
    CHECK(P(MC_6502_C)); CHECK(P(MC_6502_Z)); CHECK(!P(MC_6502_V));
    run(3);
    CHECK_EQ(cpu.a, 0xFF);
    CHECK(!P(MC_6502_C)); CHECK(P(MC_6502_N));
    run(1);
    run(2);
    CHECK_EQ(run(1), 3);                /* decimal ADC: one more cycle */
    CHECK_EQ(cpu.a, 0x04);
    CHECK(P(MC_6502_C));
    run(2);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.a, 0x78);
    CHECK(!P(MC_6502_C));
    run(3);
    CHECK_EQ(cpu.a, 0x00);
    CHECK(P(MC_6502_C)); CHECK(P(MC_6502_Z));
    run(3);
    CHECK_EQ(cpu.a, 0x80);
    CHECK(P(MC_6502_N)); CHECK(P(MC_6502_C)); CHECK(!P(MC_6502_Z));
    run(1);
    run(2);
    CHECK(P(MC_6502_Z)); CHECK(P(MC_6502_C));
    run(1);
    CHECK(P(MC_6502_N)); CHECK(!P(MC_6502_C)); CHECK(!P(MC_6502_Z));
    run(2);
    CHECK(P(MC_6502_C)); CHECK(!P(MC_6502_Z));
    run(2);
    CHECK(P(MC_6502_C)); CHECK(P(MC_6502_Z));
}

static void test_logic_bits(void) {
    const uint8_t p[] = {
        0xA9, 0xF0, 0x29, 0x3C,         /* LDA #$F0; AND #$3C -> $30 */
        0x09, 0x01,                     /* ORA #$01 -> $31 */
        0x49, 0xFF,                     /* EOR #$FF -> $CE N */
        0x24, 0x10,                     /* BIT $10 ($C0): N V from memory, Z from A&M */
        0x89, 0x01,                     /* BIT #$01: only Z changes */
        0xA9, 0x0F, 0x04, 0x11,         /* LDA #$0F; TSB $11 ($F1): Z clear, $11 = $FF */
        0x14, 0x12,                     /* TRB $12 ($30): Z set, $12 = $30 */
        0x0C, 0x00, 0x30,               /* TSB $3000 */
        0x1C, 0x01, 0x30,               /* TRB $3001 */
        0x77, 0x13,                     /* RMB7 $13 */
        0x87, 0x14,                     /* SMB0 $14 */
        0x0F, 0x14, 0x05,               /* BBR0 $14: not taken */
        0x8F, 0x14, 0x02,               /* BBS0 $14: taken (+2) */
        0xEA, 0xEA,
        0xFF, 0x13, 0x02,               /* BBS7 $13: not taken (bit 7 cleared) */
    };
    load(p, sizeof p, MC_6502_R65C02);
    mem[0x10] = 0xC0; mem[0x11] = 0xF1; mem[0x12] = 0x30; mem[0x13] = 0xFF; mem[0x14] = 0x00;
    mem[0x3000] = 0x80; mem[0x3001] = 0x8F;
    run(2);
    CHECK_EQ(cpu.a, 0x30);
    run(1);
    CHECK_EQ(cpu.a, 0x31);
    run(1);
    CHECK_EQ(cpu.a, 0xCE);
    CHECK(P(MC_6502_N));
    CHECK_EQ(run(1), 3);
    CHECK(P(MC_6502_N)); CHECK(P(MC_6502_V)); CHECK(!P(MC_6502_Z));
    CHECK_EQ(run(1), 2);
    CHECK(P(MC_6502_Z)); CHECK(P(MC_6502_N)); CHECK(P(MC_6502_V));
    run(1);
    CHECK_EQ(run(1), 5);
    CHECK(!P(MC_6502_Z));
    CHECK_EQ(mem[0x11], 0xFF);
    CHECK_EQ(run(1), 5);
    CHECK(P(MC_6502_Z));
    CHECK_EQ(mem[0x12], 0x30);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x3000], 0x8F);
    CHECK(P(MC_6502_Z));
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x3001], 0x80);
    CHECK(!P(MC_6502_Z));
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x13], 0x7F);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x14], 0x01);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.pc, 0x021F);
    CHECK_EQ(run(1), 6);                /* taken, same page */
    CHECK_EQ(cpu.pc, 0x0224);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(cpu.pc, 0x0227);
}

static void test_rmw(void) {
    const uint8_t p[] = {
        0xA9, 0x81, 0x0A,               /* LDA #$81; ASL A -> $02 C */
        0x2A,                           /* ROL A -> $05 */
        0x4A,                           /* LSR A -> $02 C */
        0x6A,                           /* ROR A -> $81 */
        0x1A,                           /* INC A -> $82 */
        0x3A, 0x3A,                     /* DEC A x2 -> $80 */
        0xE6, 0x20,                     /* INC $20 ($FF) -> 0 Z */
        0xC6, 0x21,                     /* DEC $21 (1) -> 0 */
        0xA2, 0x01,                     /* LDX #1 */
        0x1E, 0xFF, 0x30,               /* ASL $30FF,X: crosses -> 7 */
        0x1E, 0x00, 0x30,               /* ASL $3000,X: 6 */
        0xFE, 0x00, 0x30,               /* INC $3000,X: 7 */
        0x76, 0x30,                     /* ROR $30,X: $01 with C -> $80, C */
        0x4E, 0x02, 0x30,               /* LSR $3002 */
    };
    load(p, sizeof p, MC_6502_R65C02);
    mem[0x20] = 0xFF; mem[0x21] = 0x01; mem[0x3100] = 0x40; mem[0x3001] = 0x81; mem[0x31] = 0x01; mem[0x3002] = 0x01;
    run(2);
    CHECK_EQ(cpu.a, 0x02); CHECK(P(MC_6502_C));
    run(1);
    CHECK_EQ(cpu.a, 0x05); CHECK(!P(MC_6502_C));
    run(1);
    CHECK_EQ(cpu.a, 0x02); CHECK(P(MC_6502_C));
    run(1);
    CHECK_EQ(cpu.a, 0x81); CHECK(!P(MC_6502_C)); CHECK(P(MC_6502_N));
    CHECK_EQ(run(1), 2);
    CHECK_EQ(cpu.a, 0x82);
    run(2);
    CHECK_EQ(cpu.a, 0x80);
    CHECK_EQ(run(1), 5);
    CHECK_EQ(mem[0x20], 0); CHECK(P(MC_6502_Z));
    run(1);
    CHECK_EQ(mem[0x21], 0); CHECK(P(MC_6502_Z));
    run(1);
    CHECK_EQ(run(1), 7);
    CHECK_EQ(mem[0x3100], 0x80); CHECK(!P(MC_6502_C)); CHECK(P(MC_6502_N));
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x3001], 0x02); CHECK(P(MC_6502_C));
    CHECK_EQ(run(1), 7);
    CHECK_EQ(mem[0x3001], 0x03);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x31], 0x80); CHECK(P(MC_6502_C)); CHECK(P(MC_6502_N));   /* C from the ASL */
    CHECK_EQ(run(1), 6);
    CHECK_EQ(mem[0x3002], 0x00); CHECK(P(MC_6502_C));
}

static void test_branches(void) {
    const uint8_t p[] = {
        0x18,                   /* CLC */
        0x90, 0x02,             /* BCC +2: taken, 3 */
        0xEA, 0xEA,
        0xB0, 0x10,             /* BCS: not taken, 2 */
        0x80, 0x01,             /* BRA +1: 3 */
        0xEA,
        0x4C, 0xF0, 0x02,       /* JMP $02F0 */
    };
    load(p, sizeof p, MC_6502_R65C02);
    mem[0x02F0] = 0x80; mem[0x02F1] = 0x20;   /* BRA +$20 from $02F2 -> $0312: 4 (page crossed) */
    mem[0x0312] = 0xD0; mem[0x0313] = 0xEC;   /* BNE -$14 -> $0300: taken, crosses back? $0314-$14 = $0300 same page: 3 */
    run(1);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.pc, 0x0205);
    CHECK_EQ(run(1), 2);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.pc, 0x020A);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.pc, 0x02F0);
    CHECK_EQ(run(1), 4);
    CHECK_EQ(cpu.pc, 0x0312);
    cpu.p &= (uint8_t)~MC_6502_Z;
    CHECK_EQ(run(1), 3);
    CHECK_EQ(cpu.pc, 0x0300);
}

static void test_flow(void) {
    const uint8_t p[] = {
        0x20, 0x10, 0x02,       /* JSR $0210 */
        0x6C, 0xFF, 0x02,       /* JMP ($02FF): CMOS reads $02FF and $0300 */
    };
    load(p, sizeof p, MC_6502_R65C02);
    cpu.s = 0xFF;
    mem[0x0210] = 0x60;             /* RTS */
    mem[0x02FF] = 0x34; mem[0x0300] = 0x12;
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x0210);
    CHECK_EQ(cpu.s, 0xFD);
    CHECK_EQ(mem[0x01FF], 0x02);    /* the return address less one, high byte first */
    CHECK_EQ(mem[0x01FE], 0x02);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x0203);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x1234);

    /* JMP (abs,X). */
    const uint8_t q[] = {0xA2, 0x04, 0x7C, 0x00, 0x30};
    load(q, sizeof q, MC_6502_R65C02);
    mem[0x3004] = 0x78; mem[0x3005] = 0x56;
    run(1);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x5678);

    /* Stack: pushes and pulls of every register, PHP with B and bit 5. */
    const uint8_t r[] = {
        0xA9, 0x11, 0xA2, 0x22, 0xA0, 0x33,
        0x48, 0xDA, 0x5A, 0x08,         /* PHA PHX PHY PHP */
        0xA9, 0x00, 0xA2, 0x00, 0xA0, 0x00,
        0x68,                           /* PLA <- the pushed P */
        0x7A, 0xFA,                     /* PLY, PLX */
        0x28,                           /* PLP <- the byte under them */
        0xBA, 0x9A,                     /* TSX TXS */
    };
    load(r, sizeof r, MC_6502_R65C02);
    cpu.s = 0xFF;
    run(3);
    CHECK_EQ(run(1), 3);
    run(2);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(mem[0x01FC] & 0x30, 0x30);
    run(3);
    CHECK_EQ(run(1), 4);
    CHECK_EQ(cpu.a & 0x30, 0x30);
    run(1);
    CHECK_EQ(cpu.y, 0x33);
    run(1);
    CHECK_EQ(cpu.x, 0x22);
    mem[0x01FF] = 0xC3;
    run(1);
    CHECK_EQ(cpu.p, 0xC3 | 0x30);
    run(1);
    CHECK_EQ(cpu.x, 0xFF);
}

static void test_interrupts(void) {
    /* BRK: PC + 2 and P with B pushed, I set, D cleared; RTI back. */
    const uint8_t p[] = {0xF8, 0x00, 0xEA, 0xEA};
    load(p, sizeof p, MC_6502_R65C02);
    cpu.s = 0xFF;
    mem[0x0400] = 0x40;             /* RTI */
    run(1);
    cpu.p &= (uint8_t)~MC_6502_I;
    CHECK_EQ(run(1), 7);
    CHECK_EQ(cpu.pc, 0x0400);
    CHECK(P(MC_6502_I));
    CHECK(!P(MC_6502_D));
    CHECK_EQ(mem[0x01FF], 0x02);
    CHECK_EQ(mem[0x01FE], 0x03);    /* the byte after the signature */
    CHECK(mem[0x01FD] & MC_6502_B);
    CHECK(mem[0x01FD] & MC_6502_D);
    CHECK_EQ(run(1), 6);
    CHECK_EQ(cpu.pc, 0x0203);
    CHECK(P(MC_6502_D));
    CHECK(!P(MC_6502_I));

    /* IRQ: level, masked by I; B clear on the stack. */
    const uint8_t q[] = {0xEA, 0x58, 0xEA, 0xEA};
    load(q, sizeof q, MC_6502_R65C02);
    cpu.s = 0xFF;
    mem[0x0400] = 0x78;             /* SEI */
    mc_cpu6502_set_irq(&cpu, 1);
    CHECK_EQ(run(1), 2);            /* masked */
    CHECK_EQ(run(1), 2);            /* CLI */
    CHECK_EQ(run(1), 7);            /* taken before the next instruction */
    CHECK_EQ(cpu.pc, 0x0400);
    CHECK_EQ(mem[0x01FE], 0x02);
    CHECK(!(mem[0x01FD] & MC_6502_B));
    CHECK(mem[0x01FD] & MC_6502_U);
    CHECK(P(MC_6502_I));
    mc_cpu6502_set_irq(&cpu, 0);

    /* NMI: an edge, taken with I set. */
    load(q, sizeof q, MC_6502_R65C02);
    mem[0x0300] = 0xEA;
    mc_cpu6502_nmi(&cpu);
    CHECK_EQ(run(1), 7);
    CHECK_EQ(cpu.pc, 0x0300);
    CHECK_EQ(run(1), 2);
    CHECK_EQ(cpu.pc, 0x0301);       /* taken once */

    /* WAI and STP on the W65C02S; NOPs on the R65C02. */
    const uint8_t w[] = {0xCB, 0xEA, 0xDB, 0xEA};
    load(w, sizeof w, MC_6502_W65C02S);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(run(1), 0);            /* waiting */
    mc_cpu6502_set_irq(&cpu, 1);    /* I is set: resumes after WAI without the interrupt */
    run(1);
    CHECK_EQ(run(1), 2);
    CHECK_EQ(cpu.pc, 0x0202);
    mc_cpu6502_set_irq(&cpu, 0);
    CHECK_EQ(run(1), 3);
    CHECK_EQ(run(1), 0);            /* stopped */
    mc_cpu6502_reset(&cpu);
    CHECK_EQ(cpu.pc, 0x0200);
    load(w, sizeof w, MC_6502_R65C02);
    CHECK_EQ(run(1), 1);
    CHECK_EQ(cpu.pc, 0x0201);
}

int main(void) {
    test_matrix();
    test_reset();
    test_modes();
    test_arith();
    test_logic_bits();
    test_rmw();
    test_branches();
    test_flow();
    test_interrupts();
    printf("test_6502: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
