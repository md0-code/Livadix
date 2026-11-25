/* Livadix: the 68k accelerator of the Power Macintoshes:
 * 68k code on Livadix's own 68k core instead of Apple's emulator, between the
 * emulator's dispatch entries. */
#ifndef MC_M68K_ACCEL_H
#define MC_M68K_ACCEL_H

#include <stdint.h>

#include "ppc_machine.h"

typedef struct mc_m68k_accel mc_m68k_accel;

typedef struct mc_m68k_accel_stats {
    uint64_t runs;              /* take-overs that ran at least one instruction */
    uint64_t declined;          /* entries left to Apple's emulator */
    uint64_t instructions;      /* 68k instructions run */
    uint64_t translated, blocks; /* of them in the 68k translator's blocks, and the blocks it made */
    uint64_t checked, differed; /* the shadow check's instructions compared, and those that differed */
} mc_m68k_accel_stats;

/* Whether a ROM image holds the 68k emulator the accelerator was read from
 * (its fetch and NOP's dispatch entry, where the machine maps them). */
int mc_m68k_accel_knows(const uint8_t *rom, size_t size);
/* For a machine with the translator on; NULL without it. */
mc_m68k_accel *mc_m68k_accel_create(mc_ppc *m);
void mc_m68k_accel_destroy(mc_m68k_accel *x);
/* Whether `pc` is a dispatch entry of the emulator. */
int mc_m68k_accel_entry(const mc_m68k_accel *x, uint32_t pc);
/* From the dispatch entry the PowerPC PC is at: runs 68k instructions for at
 * most `budget` PowerPC steps and returns the steps to charge, the PowerPC
 * registers at the entry of the next instruction; 0: nothing run, the
 * entry is the emulator's. */
uint64_t mc_m68k_accel_run(mc_m68k_accel *x, uint64_t budget);
/* The shadow check: Apple's emulator runs everything, and each instruction
 * the accelerator would run is first predicted on the 68k core (its writes
 * held back) and compared with what the emulator made of it at the next
 * dispatch entry: the registers, PC, flags and the bytes written. The first
 * differences are printed on stderr. */
void mc_m68k_accel_shadow(mc_m68k_accel *x, int on);
void mc_m68k_accel_get_stats(const mc_m68k_accel *x, mc_m68k_accel_stats *s);

#endif
