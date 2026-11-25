/* Livadix — the Motorola 68HC05 CPU core.
 *
 * Written from the M68HC05 family reference (Motorola, "M68HC05
 * Microcontroller Applications Guide" / "Understanding Small
 * Microcontrollers": programming model, chapter "Instruction set"): the
 * accumulator A, index register X, the 6-bit stack pointer inside the
 * $00C0-$00FF stack page (reset and RSP set it to $FF; pushes decrement and
 * wrap inside the page), the program counter, and the condition code
 * register 111HINZC; the addressing modes inherent, immediate, direct,
 * extended, indexed with no, 8-bit and 16-bit offset, relative and bit
 * test-and-branch; every instruction of the opcode map with its HC05 cycle
 * count; interrupts stacking PCL, PCH, X, A, CCR (five bytes, 10 cycles)
 * with I set and the vector fetched from the top of memory (reset $xFFE,
 * SWI $xFFC, then the external and peripheral vectors below), RTI unstacking
 * them; STOP and WAIT halting until an interrupt.
 *
 * The opcodes the map leaves undefined ($31, $35, $3B, $3E, $41, $45, $4B,
 * $4E, $51, $52, $55, $5B, $5E, $61, $62, $65, $6B, $6E, $71, $72, $75,
 * $7B, $7E, $82, $84-$8D, $90-$96, $9E, $A7, $AC, $AF) are counted in
 * `illegal` and take two cycles doing nothing† (the parts with an illegal
 * opcode reset are not modelled).
 *
 * Memory is reached through the read/write callbacks with a 16-bit address
 * (the chip decodes its own width). The core counts cycles of the internal
 * bus clock; the chip around it turns them into time. */
#ifndef MC_CPU6805_H
#define MC_CPU6805_H

#include <stdint.h>

/* Condition code bits. */
enum { MC_6805_C = 0x01, MC_6805_Z = 0x02, MC_6805_N = 0x04, MC_6805_I = 0x08, MC_6805_H = 0x10 };

typedef struct mc_cpu6805 {
    uint8_t a, x, sp, cc;
    uint16_t pc;
    uint16_t pc_mask;           /* address width of the program counter ($1FFF: 13 bits) */
    uint16_t vector_top;        /* the reset vector's address ($1FFE) */
    uint8_t stopped, waiting;   /* STOP / WAIT executed, no interrupt taken since */
    uint8_t irq_pin;            /* level on /IRQ for BIL/BIH (1 = high) */
    uint64_t cycles;            /* bus cycles executed */
    uint64_t illegal;           /* undefined opcodes executed */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t address);
    void (*write)(void *ctx, uint16_t address, uint8_t value);
} mc_cpu6805;

void mc_cpu6805_init(mc_cpu6805 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                     void (*write)(void *, uint16_t, uint8_t), uint16_t vector_top);
/* Reset: SP = $FF, I set, PC from the reset vector, STOP/WAIT left. */
void mc_cpu6805_reset(mc_cpu6805 *c);
/* One instruction; returns its cycles (0 when halted by STOP or WAIT). */
unsigned mc_cpu6805_step(mc_cpu6805 *c);
/* Takes an interrupt through the vector at `vector` (its high byte's
 * address) if I is clear; returns the cycles (10) or 0 when masked.
 * Leaves STOP and WAIT. */
unsigned mc_cpu6805_interrupt(mc_cpu6805 *c, uint16_t vector);

#endif
