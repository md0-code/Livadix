/* Livadix — the Mitsubishi M50753 single-chip microcomputer (the PowerBook
 * Power Manager's microcontroller).
 *
 * Written from the M50753-XXXSP/FP data sheet (Mitsubishi Semiconductors
 * Data Book, Single-Chip 8-Bit Microcomputers, 1989, pp. 2-199..2-222):
 * Figure 1 the memory map (RAM $0000-$00BF, 192 bytes; the special function
 * registers $00E0-$00FF; ROM $E800-$FFFF, 6144 bytes; vectors $FFF4-$FFFF),
 * "Stack pointer" (stack page bit 4 of $00FF), "Interrupt" (Table 1: INT1
 * $FFFC, timer X $FFFA, timer 1 or A-D $FFF8, timer 2 or serial I/O $FFF6,
 * INT2 or BRK $FFF4; Figure 3: the request and enable bits of $00FE and
 * $00FF; request bits are set on H-to-L edges of INT1/INT2, on a timer
 * reaching zero and at the end of an A-D conversion, cleared on acceptance,
 * and the program can clear but not set them), "Timer" (timer X with its
 * prescaler X, timers 1 and 2 behind a shared prescaler, all down-counting
 * at the oscillator divided by 16 and reloaded after the zero cycle; Figure
 * 5: timer X modes timer, pulse output, event counter and pulse-width
 * measurement, count stop bit 5; "Programming notes" (1): the frequency
 * ratio of timer and prescaler is 1/n), "Serial I/O" (Figure 6: the 4-bit
 * mode register $00F6, the serial register $00F7, P34-P37 as SIN, SOUT,
 * CLK, SRDY), "A-D converter" (Figures 9, 10: channel select bits 0-2 and
 * interrupt source bit 3 of $00F3, a write to $00F2 starts the conversion,
 * the result in $00EF; tCONV 72 us at f(XIN) = 4 MHz; port IN at $00EE,
 * unreadable during a conversion), "PWM" (Figures 11, 12: bit 0 of $00F5
 * routes timers 1 and 2 to the N-channel PWM output, high for timer 1's
 * period and low for timer 2's), "Reset circuit" (Figure 14: directions 0,
 * serial mode 0, prescaler X $FF, timer X $01, interrupt and timer control
 * 0, I set, A-D control 0, PWM control 0), "I/O ports" (P0-P3 eight bits,
 * P4 four bits, N-channel open drain: a written latch shows on an output
 * pin, an input pin floats and reads its level, reading an output reads the
 * latch), "Clock generating circuit" (phi = f(XIN)/4; STP stops the
 * oscillator and loads prescaler X $FF and timer X $01, released by an
 * interrupt request after timer X's overflow; WIT stops phi until one).
 *
 * The chip runs the 740 core of cpu740.c in phi cycles. A board around it
 * gives the oscillator frequency, drives the pins and analog inputs, and is
 * told of output changes through `out` (port 0-4, or MC_M50753_PIN_*). */
#ifndef MC_M50753_H
#define MC_M50753_H

#include "cpu740.h"

#include <stdint.h>

#define MC_M50753_ROM_BASE 0xE800u
#define MC_M50753_ROM_SIZE 6144u
#define MC_M50753_RAM_SIZE 192u
/* tCONV in phi cycles (72 us at f(XIN) = 4 MHz, phi = 1 MHz). */
#define MC_M50753_ADC_CYCLES 72u

/* Output pins reported through `out` besides ports 0-4. */
enum { MC_M50753_PIN_PWM = 8, MC_M50753_PIN_CNTR = 9 };

typedef struct mc_m50753 {
    mc_cpu740 cpu;
    uint8_t ram[MC_M50753_RAM_SIZE];
    uint8_t rom[MC_M50753_ROM_SIZE];
    /* Ports P0-P4: latch, direction (1 = output), the level the board drives
     * on each pin (1 = high or released), the levels last reported. */
    uint8_t latch[5], ddr[5], ext[5], reported[5];
    uint8_t in_port;                /* port IN as digital levels */
    uint8_t analog[8];              /* IN0-IN7 as A-D codes (VREF = 255) */
    uint8_t int1, int2, cntr;       /* input pin levels */
    /* Special function registers. */
    uint8_t icr, tcr;               /* $FE interrupt control, $FF timer control */
    uint8_t adc_control, adc_result, adc_busy;
    uint32_t adc_left;              /* phi cycles until the conversion ends */
    uint8_t pwm_control, sio_mode, sio_data, sio_count;
    uint32_t sio_left;
    /* Timers: latches and counters; the /16 divider runs on phi / 4. */
    uint8_t prex_latch, tx_latch, pre12_latch, t1_latch, t2_latch;
    uint16_t prex, tx, pre12, t1, t2;
    uint8_t div16;                  /* phi cycles into the current f(XIN)/16 period (4 per period) */
    uint8_t cntr_out, pwm_out, pwm_phase;
    uint32_t stp_left;              /* STP: phi cycles until the clock restarts */
    uint64_t cycles;                /* phi cycles run */
    /* The board. */
    void *ctx;
    void (*out)(void *ctx, int port, uint8_t levels);
} mc_m50753;

/* The ROM image is copied in; `out` may be NULL. */
void mc_m50753_init(mc_m50753 *m, const uint8_t rom[MC_M50753_ROM_SIZE], void *ctx,
                    void (*out)(void *, int, uint8_t));
/* /RESET: the registers of Figure 14, then the reset vector. */
void mc_m50753_reset(mc_m50753 *m);
/* Runs at least one instruction (or one idle phi cycle under WIT/STP),
 * timers and interrupts included; returns the phi cycles taken. */
unsigned mc_m50753_step(mc_m50753 *m);
/* Input pins. */
void mc_m50753_set_pins(mc_m50753 *m, int port, uint8_t levels, uint8_t mask);
void mc_m50753_set_int1(mc_m50753 *m, int level);
void mc_m50753_set_int2(mc_m50753 *m, int level);
void mc_m50753_set_cntr(mc_m50753 *m, int level);
/* The level each pin of a port shows (1 high or released): an output's
 * latch (open drain: a 1 releases), else what the board drives. */
uint8_t mc_m50753_pins(const mc_m50753 *m, int port);
/* Memory as the CPU sees it (for tests and traces). */
uint8_t mc_m50753_read(mc_m50753 *m, uint16_t address);
void mc_m50753_write(mc_m50753 *m, uint16_t address, uint8_t value);

#endif
