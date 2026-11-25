/* Livadix — the AT&T DSP3210 digital signal processor of the Quadra 840AV and
 * Centris/Quadra 660AV.
 *
 * Sources: AT&T DSP3210 Digital Signal Processor Information Manual
 * (September 1991): chapter 3 (the data types, Figure 3-4's memory maps,
 * Figure 3-5's I/O memory map), chapter 4 (the instruction set, the
 * latencies of 4.4.2 and the detailed descriptions of 4.6), chapter 7
 * (reset, interrupts, error exceptions), chapter 8 (the CAU and the DAU, the
 * floating-point formats, precision and conversions), chapter 9 (the timer
 * and the bit I/O) and chapter 10 (the instruction and register encodings).
 *
 * The model: one instruction per step (four CKI cycles), the bus without
 * wait states; the pipeline's visible effects as 4.4.2 states them: one
 * latent instruction after goto, call and return (none after ireturn); a
 * DA instruction's memory write read back from the fourth instruction on;
 * an accumulator as a multiplier input as the instruction three before
 * left it; a DAU condition tested by a CA instruction as the last DA
 * instruction at least four before set it. The DAU's arithmetic runs in
 * host doubles, normalized and truncated to the 40-bit accumulator's 32-bit
 * mantissa (an exact sum wider than 53 bits is rounded first†). The serial
 * port and its DMA controller keep their registers only†; the boot ROM's
 * words read 0† (the Macintosh starts the DSP from external memory).
 *
 * The bus: the board gives the DSP its physical page tables (4 KiB pages,
 * host pointers to big-endian bytes, as mc_bus keeps them) and callbacks
 * for every other address. */
#ifndef MC_DSP3210_H
#define MC_DSP3210_H

#include <stddef.h>
#include <stdint.h>

typedef struct mc_dsp3210_bus {
    void *ctx;
    /* An access outside DRAM and the chip: 1, 2 or 4 bytes, the value
     * big-endian in the low bits; nonzero = a bus error. */
    int (*read)(void *ctx, uint32_t address, unsigned size, uint32_t *value);
    int (*write)(void *ctx, uint32_t address, unsigned size, uint32_t value);
    /* The BIO pins the DSP drives changed: their levels (outputs only). */
    void (*bio)(void *ctx, uint8_t levels, uint8_t outputs);
    /* IACK0/IACK1: the DSP takes the interrupt of IR0N (0) or IR1N (1). */
    void (*iack)(void *ctx, int line);
    /* Optional, for tracing: every data write the DSP makes to memory. */
    void (*watch)(void *ctx, uint32_t address, unsigned size, uint32_t value);
} mc_dsp3210_bus;

enum { MC_DSP3210_PENDING = 16, MC_DSP3210_HIST = 8 };

typedef struct mc_dsp3210_shadow {
    uint64_t a[4];
    uint8_t dauc, n, z, v, c, dflags;
    int do_active, do_left, do_len, do_count;
    uint32_t do_start;
} mc_dsp3210_shadow;

typedef struct mc_dsp3210 {
    /* CAU: r[0] is 0; r[1..22] r1-r22 (r20 the error pc, r21 sp, r22 evtp) */
    uint32_t r[23];
    uint32_t pc, pcsh;
    uint32_t delay_target;
    int delay;                  /* a branch's target waits for its latent instruction */
    uint8_t n, z, v, c;
    /* DAU: 40-bit accumulators as raw bits (39..8 the mantissa s.f+guard, 7..0 the exponent) */
    uint64_t a[4];
    struct { uint64_t value; uint64_t step; } ahist[4][MC_DSP3210_HIST];
    unsigned ahead[4];
    uint8_t dflags;             /* N Z U V (bits 0-3) of the last DA instruction that set flags */
    struct { uint8_t flags; uint64_t step; } fhist[MC_DSP3210_HIST];
    unsigned fhead;
    uint8_t ctr, dauc;
    /* control */
    uint16_t pcw, emr;
    int level;                  /* 0 base, 1 interrupt, 2 error (maskable), 3 error (non-maskable) */
    int servicing;              /* the external interrupt being served (0, 1), -1 none */
    int lock;                   /* the instruction just run locks out an interrupt before the next */
    mc_dsp3210_shadow sh;
    /* DA memory writes in flight (4.4.2.1): seen by reads from `due` on */
    struct { uint32_t address, value; unsigned size; uint64_t due; } pend[MC_DSP3210_PENDING];
    unsigned npend;
    uint64_t steps;
    /* do loops */
    int do_active, do_left, do_len, do_count;
    uint32_t do_start;
    /* peripherals */
    uint32_t ioc, idp, odp, ibuf, obuf, timer, timer_init;
    uint16_t icnt, ocnt;
    uint8_t tcon, bioc, dmac, bio_out, bio_in;
    uint16_t irq_latched;       /* emr-position requests: bit 8 EXT0, 9 TIMER, 15 EXT1 */
    uint8_t irn;                /* bit 0 IR0N, bit 1 IR1N asserted */
    int wait, held, stopped;
    /* memory */
    uint8_t ram0[4096], ram1[4096];
    uint8_t *const *read_map, *const *write_map;     /* 4 KiB pages, NULL entries go to the bus */
    mc_dsp3210_bus bus;
    int fault;                  /* an error exception raised by the current instruction (vector + 1) */
    uint32_t ea;                /* the last X/Y memory operand's address and pointer (0: none) */
    unsigned ea_p;
    uint32_t trace[32];         /* the last instructions' addresses (diagnostics) */
    unsigned trace_pos;
} mc_dsp3210;

#define MC_DSP3210_PAGE_SHIFT 12u
void mc_dsp3210_init(mc_dsp3210 *d, const mc_dsp3210_bus *bus, uint8_t *const *read_map, uint8_t *const *write_map);
/* RESTN: held keeps the DSP in reset; the rising edge (held 0) starts the
 * reset sequence with pcw[13:10] from the BIO pins 4-7 (bio_in). */
void mc_dsp3210_reset(mc_dsp3210 *d, int held);
/* IR0N (line 0) or IR1N (line 1): 1 = asserted (low). */
void mc_dsp3210_set_irq(mc_dsp3210 *d, int line, int asserted);
void mc_dsp3210_set_bio(mc_dsp3210 *d, uint8_t levels);
/* Runs up to n instructions; returns the instruction cycles spent (0: held,
 * stopped or waiting for an interrupt). */
unsigned mc_dsp3210_run(mc_dsp3210 *d, unsigned n);
/* Whether the DSP will do anything before an interrupt or a reset. */
int mc_dsp3210_idle(const mc_dsp3210 *d);

/* The 32-bit floating-point format (3.3.3) to a host double and back
 * (normalized, truncated); for the unit tests. */
double mc_dsp3210_f32_value(uint32_t f);
uint32_t mc_dsp3210_f32_from(double x);

#endif
