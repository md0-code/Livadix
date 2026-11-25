/* Livadix — the Mitsubishi MELPS 740 (740 family) CPU core.
 *
 * Written from the Mitsubishi "Series 740 User's Manual" (software manual,
 * 1989): 2 the registers (A, X, Y, S, the 16-bit PC, the processor status
 * register N V T B D I Z C, bit 7 to bit 0; every register but I
 * indeterminate after reset), 2.3 the stack (pushes store then decrement,
 * pulls increment then load, in the stack page the chip selects), 2.5 the T
 * flag (when set, ADC, AND, CMP, EOR, LDA, ORA and SBC take M(X), the zero
 * page byte X addresses, in place of A), 3.1 the addressing modes (zero page
 * bit, accumulator bit, special page $FFxx, zero page indirect), 3.3 every
 * instruction (operation, flags, encodings), 4 the programming notes, and
 * Appendices 2 and 3 (the machine language instruction table with byte and
 * cycle counts per opcode, and the hexadecimal code table). Cycles are
 * counted in the internal clock phi (Appendix 2, note 9): the T flag adds 3
 * (ADC, AND, EOR, ORA, SBC), 2 (LDA) or 1 (CMP), a taken branch 2.
 *
 * MUL and DIV (Appendix 3 note 1: "special type only", the M37450 family)
 * run only when `muldiv` is set; the M50753 does not have them (1992
 * Single-Chip 8-bit Microcomputers vol. 2, "Support of these instructions
 * depends on the microcomputer type"), nor FST/SLW. The opcodes the code
 * table leaves blank, and MUL/DIV without `muldiv`, are counted in
 * `illegal` and take 2 cycles doing nothing†.
 *
 * Interrupts (M50753 data sheet, "Interrupt"): PCH, PCL, then PS with B
 * clear are pushed, I set, PC from the vector; BRK pushes PC + 2 and PS
 * with B set and takes the lowest-priority vector. The acceptance takes 7
 * cycles, as BRK† (the manuals give no count). WIT stops the clock until an
 * interrupt request; STP also stops the oscillator (the chip restarts it).
 *
 * Decimal mode (D) applies to ADC and SBC; N, V and Z are documented as
 * invalid there (Software manual 4.3): Livadix sets them from the binary
 * result†. Memory is reached through callbacks with the 16-bit address;
 * the chip decodes it. */
#ifndef MC_CPU740_H
#define MC_CPU740_H

#include <stdint.h>

/* Processor status bits (Software manual 2.5). */
enum {
    MC_740_C = 0x01, MC_740_Z = 0x02, MC_740_I = 0x04, MC_740_D = 0x08,
    MC_740_B = 0x10, MC_740_T = 0x20, MC_740_V = 0x40, MC_740_N = 0x80
};

typedef struct mc_cpu740 {
    uint8_t a, x, y, s, p;
    uint16_t pc;
    uint16_t stack_page;        /* $0000 or $0100: the chip's stack page bit */
    uint8_t muldiv;             /* the part has MUL and DIV */
    uint8_t waiting;            /* WIT: the clock stopped until an interrupt */
    uint8_t stopped;            /* STP: the oscillator stopped until an interrupt or reset */
    uint64_t cycles;            /* phi cycles executed */
    uint64_t illegal;           /* undefined opcodes executed */
    void *ctx;
    uint8_t (*read)(void *ctx, uint16_t address);
    void (*write)(void *ctx, uint16_t address, uint8_t value);
} mc_cpu740;

void mc_cpu740_init(mc_cpu740 *c, void *ctx, uint8_t (*read)(void *, uint16_t),
                    void (*write)(void *, uint16_t, uint8_t));
/* Reset: I set, PC from $FFFE (low) and $FFFF (high), WIT and STP left.
 * The other registers keep their values (indeterminate on the chip). */
void mc_cpu740_reset(mc_cpu740 *c);
/* One instruction; returns its phi cycles (0 while WIT or STP holds). */
unsigned mc_cpu740_step(mc_cpu740 *c);
/* Takes an interrupt through `vector` (the address of its low byte) when I
 * is clear; returns the cycles taken, 0 when masked. Leaves WIT and STP. */
unsigned mc_cpu740_interrupt(mc_cpu740 *c, uint16_t vector);
/* The instruction at `pc` as text (for traces); returns its length. */
unsigned mc_cpu740_disasm(const mc_cpu740 *c, uint16_t pc, char *out, unsigned capacity);

#endif
