/* Livadix — a chip that is not modelled yet (phase 2a): its address range
 * answers as the idle chip's registers would, from a table of register
 * values, and ignores writes. Each use is listed in docs/NOTES.md with what
 * replaces it. */
#ifndef MC_ABSENT_H
#define MC_ABSENT_H

#include "hw.h"

#include <stdint.h>

typedef struct mc_absent_reg {
    uint32_t offset, mask;      /* registers at offset, compared after the mask */
    uint8_t value;
} mc_absent_reg;

typedef struct mc_absent {
    mc_dev dev;
    const mc_absent_reg *regs;  /* first match wins; 0-terminated by a mask of 0 */
    uint8_t fill;               /* every other byte */
} mc_absent;

void mc_absent_init(mc_absent *a, const mc_hw_host *host, const char *name, const mc_absent_reg *regs, uint8_t fill);
extern const mc_dev_ops mc_absent_ops;

#endif
