/* Livadix's own extended-precision arithmetic for the FPU (fpu.c): the
 * 68881/68882 extended format, exact intermediate results and the rounding
 * the MC68881/MC68882 User's Manual describes (4.5.5.2 underflow, round,
 * overflow; 3.6 data format details; 6.1.4-6.1.5 overflow and underflow
 * results), plus a 128-bit working format (fpb) for the transcendental
 * functions and decimal conversions (fpu_trans.c).
 *
 * Extended format (MC68881 UM 3.6): sign, 15-bit biased exponent (bias
 * 16383), explicit integer bit, 63-bit fraction; value = 2^(e - 16383) x j.f
 * for every exponent below the maximum, so a denormal (e = 0, j = 0) is
 * 2^-16383 x 0.f. */
#ifndef MC_FPU_SOFT_H
#define MC_FPU_SOFT_H

#include "m68k.h"

/* EXC byte bits as the FPSR holds them (MC68881 UM 2.3.3). */
enum {
    FX_INEX1 = 0x0100u, FX_INEX2 = 0x0200u, FX_DZ = 0x0400u, FX_UNFL = 0x0800u,
    FX_OVFL = 0x1000u, FX_OPERR = 0x2000u, FX_SNAN = 0x4000u, FX_BSUN = 0x8000u
};
/* Rounding modes (FPCR RND, MC68881 UM 2.2.2). */
enum { RND_N, RND_Z, RND_M, RND_P };

/* An unpacked value: k says the class; for FX_NORM the value is
 * (m + lo / 2^64) x 2^(e - 63) with bit 63 of m set (lo holds the bits below
 * m, its bit 0 sticky); for FX_NAN, m is the 64-bit mantissa as stored. */
enum { FX_ZERO, FX_NORM, FX_INF, FX_NAN };
typedef struct fpx {
    uint64_t m, lo;
    int32_t e;
    uint8_t s, k;
} fpx;

/* A rounding precision: mantissa bits and the normalised exponent range
 * (unbiased). */
typedef struct fpfmt { int bits; int32_t emin, emax; } fpfmt;
extern const fpfmt FMT_X, FMT_D, FMT_S;

fpx fx_zero(int sign);
fpx fx_inf(int sign);
fpx fx_default_nan(void);       /* the nonsignaling NaN with all mantissa bits set */
int fx_is_snan(fpx v);          /* NaN with the quiet bit (62) clear */
fpx fx_quiet(fpx v);

/* Registers and extended memory operands. Unnormalized and denormalized
 * values come out normalized; NaN mantissas are kept. */
fpx fx_from_x(m68k_fx80 r);
/* An extended image whose exponent carries the 68040 frame's bit 15 (the
 * ROM's package normalizes denormals into it, MC68040 UM 9.8 ETE/FPTE). */
fpx fx_from_x_e15(m68k_fx80 r, int e15);
/* Packs a value already rounded to extended (exponents below -16383 become
 * denormals, exactly). */
m68k_fx80 fx_to_x(fpx v);
/* 32-bit and 64-bit binary formats. */
fpx fx_from_s(uint32_t v);
fpx fx_from_d(uint64_t v);
uint32_t fx_to_s(fpx v);        /* v rounded to FMT_S */
uint64_t fx_to_d(fpx v);        /* v rounded to FMT_D */
fpx fx_from_int(int64_t v);
fpx fx_from_u64(uint64_t v);
fpx fx_from_u128(uint64_t hi, uint64_t lo);   /* the 128-bit integer hi:lo, exactly */

/* Rounds to `f` in `mode` (FPCR RND): sets INEX2, UNFL (the intermediate
 * was too small for a normalized number of the format), OVFL; returns the
 * rounded value, normalized, ready for fx_to_x. */
fpx fx_round(fpx v, const fpfmt *f, unsigned mode, unsigned *exc);
/* Rounds to an integer (FINT, FINTRZ, conversions). */
fpx fx_round_int(fpx v, unsigned mode, unsigned *exc);
/* To a signed integer of `bits` (8, 16, 32): OPERR and the largest integer
 * of the operand's sign on overflow; INEX2 when inexact. */
int32_t fx_to_int(fpx v, int bits, unsigned mode, unsigned *exc);

/* Exact operations on finite or infinite non-NaN operands; the caller
 * rounds. OPERR and DZ as the MC68881 UM operation tables give them, with
 * the default NaN as the result. */
fpx fx_add(fpx a, fpx b, unsigned mode, unsigned *exc);   /* a + b; an exact zero sum is -0 only in RM */
fpx fx_mul(fpx a, fpx b, unsigned *exc);
fpx fx_div(fpx a, fpx b, unsigned *exc);    /* a / b */
fpx fx_sqrt(fpx a, unsigned *exc);
/* FMOD (round == 0: truncated quotient) and FREM (round == 1: nearest);
 * *q: the quotient's sign and low 7 bits as the FPSR quotient byte. */
fpx fx_rem(fpx a, fpx b, int nearest, unsigned *exc, unsigned *q);
fpx fx_scale(fpx a, int32_t n);             /* a x 2^n, exact (unrounded) */
int fx_cmp_mag(fpx a, fpx b);               /* |a| vs |b| for normalized values: -1, 0, 1 */

/* --- the 128-bit working format ----------------------------------------- */

/* (h:l) x 2^(e - 127) with bit 63 of h set; h == 0 means zero. */
typedef struct fpb { uint64_t h, l; int32_t e; int s; } fpb;
fpb fb_from_x(fpx v);
fpx fb_to_x(fpb v);             /* to an fpx with the low bits sticky, for fx_round */
fpb fb_from_u64(uint64_t v);
fpb fb_make(int s, int32_t e, uint64_t h, uint64_t l);   /* (h:l) x 2^(e-127), normalized */
fpb fb_from_int(int64_t v);
fpb fb_add(fpb a, fpb b);
fpb fb_sub(fpb a, fpb b);
fpb fb_mul(fpb a, fpb b);
fpb fb_div(fpb a, fpb b);
fpb fb_sqrt(fpb a);
fpb fb_ldexp(fpb a, int32_t n);
fpb fb_neg(fpb a);
int fb_cmp(fpb a, fpb b);       /* signed comparison: -1, 0, 1 */
int fb_is_zero(fpb a);
int64_t fb_floor_int(fpb a);    /* floor for |a| < 2^62 */
fpb fb_frac(fpb a);             /* a - trunc(a) */

#endif /* MC_FPU_SOFT_H */
