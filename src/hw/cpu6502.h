/* Livadix — the 65C02 CPU core (the processor inside Apple's I/O Processor).
 *
 * Written from the CMOS 6502 data sheets: Rockwell R65C00/21 and R65C29
 * family "R65C02" data sheet (instruction set, opcode matrix with bytes and
 * cycles, the bit manipulation instructions RMB, SMB, BBR, BBS) and the
 * Western Design Center W65C02S data sheet (programming model, addressing
 * modes, Table "Operation, Operation Codes and Status Register", the notes
 * on cycle counts: +1 for an indexed read crossing a page, +1 for a taken
 * branch and +1 more when it crosses a page, +1 for ADC and SBC in decimal
 * mode, shifts and rotates absolute,X in 6 cycles unless a page is crossed;
 * the reserved opcodes as NOPs of 1, 2 or 3 bytes; WAI and STP), and the
 * MOS MCS6500 Microcomputer Family Programming Manual for the NMOS base set
 * (sections 2-8: the instructions, the flags, BCD arithmetic, the stack in
 * page 1, the interrupt sequences and the vectors at $FFFA NMI, $FFFC
 * RESET, $FFFE IRQ/BRK).
 *
 * Programming model: A, X, Y, the stack pointer S (stack at $0100-$01FF,
 * pushes post-decrement), PC and P = N V 1 B D I Z C. CMOS behaviour kept:
 * N and Z valid after decimal ADC/SBC; D cleared by RESET, BRK and every
 * interrupt; JMP (abs) reads its pointer across a page end; the zero-page
 * indirect modes wrap inside page 0.
 *
 * The IOP's variant: Apple's IOP kernels use
 * the CMOS additions (BRA, PHX/PHY/PLX/PLY, STZ, TSB/TRB, (zp), INC A and
 * DEC A, BIT #imm, JMP (abs,X)); the whole R65C02 set is implemented,
 * including RMB/SMB/BBR/BBS. $CB and $DB are WAI and STP on the W65C02S
 * and one-byte NOPs on the R65C02 (`variant`); the IOP core is taken as an
 * R65C02-class part†.
 *
 * Bus model: one read or write per operand access; the dummy reads and
 * writes of the real bus cycles are not made (no IOP register the kernels
 * touch with a read-modify-write has a read side effect). */
#ifndef MC_CPU6502_H
#define MC_CPU6502_H

#include <stdint.h>

/* Status register bits. */
enum {
    MC_6502_C = 0x01, MC_6502_Z = 0x02, MC_6502_I = 0x04, MC_6502_D = 0x08,
    MC_6502_B = 0x10, MC_6502_U = 0x20, MC_6502_V = 0x40, MC_6502_N = 0x80
};

/* $CB and $DB: NOPs on the R65C02, WAI and STP on the W65C02S. */
typedef enum { MC_6502_R65C02 = 0, MC_6502_W65C02S = 1 } mc_6502_variant;

typedef struct mc_cpu6502 {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint8_t variant;            /* mc_6502_variant */
    uint8_t irq;                /* /IRQ level: 1 = asserted */
    uint8_t nmi;                /* an NMI edge is pending */
    uint8_t waiting, stopped;   /* WAI / STP executed */
    uint64_t cycles;            /* clock cycles executed */
    uint32_t used[8];           /* opcodes executed, one bit each (coverage) */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t address);
    void (*write)(void *ctx, uint16_t address, uint8_t value);
} mc_cpu6502;

void mc_cpu6502_init(mc_cpu6502 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                     void (*write)(void *, uint16_t, uint8_t), mc_6502_variant variant);
/* /RES: S decremented by 3 without writes, I set, D cleared, PC from
 * $FFFC; leaves WAI and STP. Returns its 7 cycles. */
unsigned mc_cpu6502_reset(mc_cpu6502 *c);
/* One instruction, or the interrupt sequence when an NMI edge is pending or
 * /IRQ is asserted with I clear (7 cycles: PCH, PCL and P with B clear
 * pushed, I set, D cleared, PC from the vector). Returns the cycles; 0 when
 * halted by WAI (until an interrupt) or STP (until reset). */
unsigned mc_cpu6502_step(mc_cpu6502 *c);
/* The /IRQ level (1 = asserted). An asserted /IRQ ends WAI even with I set
 * (execution then continues after the WAI). */
void mc_cpu6502_set_irq(mc_cpu6502 *c, int asserted);
/* A falling edge on /NMI. */
void mc_cpu6502_nmi(mc_cpu6502 *c);

#endif
