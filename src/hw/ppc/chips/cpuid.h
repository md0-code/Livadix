/* Livadix — the CPU identification register of the PDM boards (Power
 * Macintosh 6100, 7100, 8100) at $5FFF FFFC.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994), chapter
 * 1, "Machine Identification" and Table 1-5: the 3 low bits of the 32-bit
 * register at $5FFF FFFC identify the model (6100 000, 7100 010, 8100 011).
 *
 * As ROM 9FEB69B3 reads it (ROM addresses at
 * $FFC0 0000): the register answers byte reads, read-only (the 68k code
 * writes $6A, $6F, $65, $20 to its bytes and expects them unchanged,
 * $FFC1001C-$FFC10060); its high word is $A55A and its low word $3010
 * plus the model code; the PowerPC start-up code reads bytes +2 and +3
 * and takes $3012 and $3013 for the 7100/8100 memory layout
 * ($FFF03044-$FFF0305C, hmc.h); the 68k code first tries longword
 * accesses (it writes 0 and $FFFF FFFF and compares, $FFC0FFFA-$FFC1000C)
 * and goes on to the byte test, with the 6100's probes for $3010
 * ($FFC10070), only when a longword reads back what it wrote. Livadix's
 * reading†: a longword access is not the register's, the bus holds the
 * last longword written (before any, the register's value); the 6100
 * finds its ProductInfo record only so.
 *
 * Livadix's own code. */
#ifndef MC_CPUID_H
#define MC_CPUID_H

#include "hw.h"

#include <stdint.h>

#define MC_CPUID_SIZE 4u
#define MC_CPUID_PDM_6100 0xA55A3010u
#define MC_CPUID_PDM_7100 0xA55A3012u
#define MC_CPUID_PDM_8100 0xA55A3013u
/* Workgroup Server 9150 Developer Note (1995), Table 1: low bits 100. */
#define MC_CPUID_PDM_9150 0xA55A3014u

typedef struct mc_cpuid {
    mc_dev dev;
    uint32_t value;
    uint32_t bus;               /* what a longword read returns */
} mc_cpuid;

extern const mc_dev_ops mc_cpuid_ops;
void mc_cpuid_init(mc_cpuid *c, const mc_hw_host *host, const char *name, uint32_t value);

#endif
