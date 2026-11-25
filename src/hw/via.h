/* Livadix — the 6522 Versatile Interface Adapter.
 *
 * Written from the Rockwell R6522 VIA data sheet (register map, port and
 * handshake operation, Timer 1 one-shot and free-running modes with PB7,
 * Timer 2 one-shot and pulse counting, the shift register's eight modes,
 * IFR/IER) and the Guide to the Macintosh Family Hardware, 2nd ed.,
 * chapter 4 (registers at base + n x $200, the E clock of 783.36 kHz =
 * C15M/20: one count every 1.2766 us).
 *
 * The chip talks to the machine through its mc_hw_host: the line callback
 * reports MC_VIA_LINE_IRQ (1 = /IRQ asserted) and the outputs
 * MC_VIA_LINE_PA / _PB (state = the 8 pin levels the VIA drives: ORx where
 * DDRx is 1, 1 elsewhere; PB7 is the timer output when ACR7 is set),
 * MC_VIA_LINE_CA2 / _CB2 (output levels in the output modes) and
 * MC_VIA_LINE_CB1 (the shift clock it drives in the internally clocked shift
 * modes, unused by Macintosh wiring). Inputs come in through the
 * mc_via_set_* functions. Counters are computed from guest time when read;
 * the scheduler is armed only for the next flag or PB7 edge. */
#ifndef MC_VIA_H
#define MC_VIA_H

#include "hw.h"

#include <stdint.h>

enum {
    MC_VIA_LINE_IRQ,
    MC_VIA_LINE_PA,
    MC_VIA_LINE_PB,
    MC_VIA_LINE_CA2,
    MC_VIA_LINE_CB2,
    MC_VIA_LINE_CB1
};

/* Registers (R6522 data sheet, "Register select"). On a Macintosh register n
 * is at base + n x $200. */
enum {
    MC_VIA_ORB, MC_VIA_ORA, MC_VIA_DDRB, MC_VIA_DDRA,
    MC_VIA_T1CL, MC_VIA_T1CH, MC_VIA_T1LL, MC_VIA_T1LH,
    MC_VIA_T2CL, MC_VIA_T2CH, MC_VIA_SR, MC_VIA_ACR,
    MC_VIA_PCR, MC_VIA_IFR, MC_VIA_IER, MC_VIA_ORA_NH
};

/* Interrupt flag bits. */
enum {
    MC_VIA_IRQ_CA2 = 0x01, MC_VIA_IRQ_CA1 = 0x02, MC_VIA_IRQ_SR = 0x04, MC_VIA_IRQ_CB2 = 0x08,
    MC_VIA_IRQ_CB1 = 0x10, MC_VIA_IRQ_T2 = 0x20, MC_VIA_IRQ_T1 = 0x40
};

/* One E-clock period in C15M periods. */
#define MC_VIA_ECLOCK 20u

typedef struct mc_via {
    mc_dev dev;
    unsigned shift;             /* register select = offset >> shift (9 on a Macintosh) */
    uint8_t ora, orb, ddra, ddrb, acr, pcr, ifr, ier;
    uint8_t pa_in, pb_in;       /* levels other chips drive on the port pins */
    uint8_t ira, irb;           /* input latches (ACR0/ACR1) */
    uint8_t ca1, ca2, cb1, cb2; /* input pin levels */
    uint8_t ca2_out, cb2_out;   /* output levels in CA2/CB2 output modes */
    uint8_t ca2_pulse, cb2_pulse;
    uint8_t irq_out;
    uint8_t pa_out, pb_out;     /* last levels reported */
    /* Timer 1: the counter was loaded with t1_n at t1_start (C15M). */
    uint16_t t1_latch, t1_n;
    uint64_t t1_start;
    uint8_t t1_armed;           /* one-shot: a flag is still due */
    uint8_t pb7;                /* timer output level */
    /* Timer 2. */
    uint8_t t2_latch_lo;
    uint16_t t2_n;
    uint64_t t2_start;
    uint8_t t2_armed;
    uint16_t t2_count;          /* pulse counting mode: the counter */
    /* Shift register. */
    uint8_t sr, sr_count;       /* bits shifted since the last SR access */
    uint64_t sr_start;          /* internally clocked modes: when shifting began */
    uint8_t sr_running;
    uint64_t next_event;
} mc_via;

void mc_via_init(mc_via *via, const mc_hw_host *host, const char *name, unsigned shift);
extern const mc_dev_ops mc_via_ops;

/* Input pins. `mask` selects the port bits another chip drives. */
void mc_via_set_pa(mc_via *via, uint8_t value, uint8_t mask);
void mc_via_set_pb(mc_via *via, uint8_t value, uint8_t mask);
void mc_via_set_ca1(mc_via *via, int level);
void mc_via_set_ca2(mc_via *via, int level);
void mc_via_set_cb1(mc_via *via, int level);
void mc_via_set_cb2(mc_via *via, int level);
/* An interrupt source a machine adds to the VIA's own: sets (on) or clears
 * the IFR bits of `bits` (a chip of the board using a bit the VIA's own
 * source leaves alone, such as the LC 575's push buttons on Timer 2's). */
void mc_via_set_ifr(mc_via *via, uint8_t bits, int on);
/* An external device clocks 8 bits through the shift register on CB1 (SR
 * modes 0, 3 and 7): in the shift-in modes `in` is shifted in MSB first
 * from CB2; returns the 8 bits shifted out on CB2. CB1 ends high. */
uint8_t mc_via_shift8(mc_via *via, uint8_t in);
/* Shift register mode (ACR4-2). */
static inline unsigned mc_via_sr_mode(const mc_via *via) { return (via->acr >> 2) & 7u; }

/* The current counters (for tests and traces). */
uint16_t mc_via_t1(mc_via *via);
uint16_t mc_via_t2(mc_via *via);

#endif
