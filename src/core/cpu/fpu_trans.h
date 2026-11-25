/* Transcendental functions, the constant ROM and packed decimal conversion
 * for the FPU (fpu_trans.c). */
#ifndef MC_FPU_TRANS_H
#define MC_FPU_TRANS_H

#include "fpu_soft.h"

/* The monadic functions, by their FPU opmode (MC68881 UM 4.8). */
enum {
    FT_SINH = 0x02, FT_LOGNP1 = 0x06, FT_ETOXM1 = 0x08, FT_TANH = 0x09, FT_ATAN = 0x0A, FT_ASIN = 0x0C,
    FT_ATANH = 0x0D, FT_SIN = 0x0E, FT_TAN = 0x0F, FT_ETOX = 0x10, FT_TWOTOX = 0x11, FT_TENTOX = 0x12,
    FT_LOGN = 0x14, FT_LOG10 = 0x15, FT_LOG2 = 0x16, FT_COSH = 0x19, FT_ACOS = 0x1C, FT_COS = 0x1D
};

/* f(x) for a non-NaN x, unrounded (the caller rounds); OPERR and DZ with
 * their default results per the instruction tables. */
fpx ft_eval(unsigned op, fpx x, unsigned *exc);
/* The FMOVECR constant at `offset`, unrounded; *exact when it is exactly
 * representable. */
fpx ft_constant(unsigned offset, int *exact);
/* Packed decimal (three long words, most significant first) to extended,
 * rounded to extended in `mode`: INEX1 when inexact. */
fpx ft_from_packed(const uint32_t w[3], unsigned mode, unsigned *exc);
/* Extended to packed decimal with k-factor `k` (-64..63): OPERR for k > 17
 * or a four-digit exponent, INEX2 when inexact. */
void ft_to_packed(fpx x, int k, unsigned mode, unsigned *exc, uint32_t w[3]);

#endif /* MC_FPU_TRANS_H */
