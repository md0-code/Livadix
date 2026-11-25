/* Livadix â€” the Egret and Cuda microcontrollers at the firmware level: Apple's
 * firmware on a 68HC05 core (cpu6805.c) with the peripherals of these
 * custom 68HC05 parts.
 *
 * The parts' data sheets are not public; the peripheral set below is the
 * M68HC05 family's (ports with data direction registers, the core timer
 * with its timer overflow and real-time interrupts, a PLL clock
 * synthesizer from a 32.768 kHz crystal) as Apple's firmware uses it,
 * every address and bit from the firmware's own code:
 *
 *   $00-$02  ports A, B, C        $04-$06  their data direction registers
 *   $07      PLL control: bit 6 selects the PLL bus clock (2.097152 MHzâ€ ),
 *            clear the crystal's 16.384 kHzâ€ ; bits 4, 3, 2, 1 the PLL's
 *            enable and settings (kept, not modelled)
 *   $08      core timer control/status: bit 7 TOF, bit 6 RTIF (written 0
 *            clears, 1 keeps), bit 5 TOIE, bit 4 RTIE, bits 1-0 the RTI rate
 *   $09      core timer counter (bus clock / 4; TOF every 1024 bus cycles,
 *            RTI every 2^(14 + rate) bus cyclesâ€ , as the M68HC05 core timer)
 *   $12      one-second timer: bit 6 its flag (set each second from the
 *            crystal, written 0 clears), bit 4 its interrupt enable
 *   $90-$FF  RAM (stack $C0-$FF)  $100-$1FF RAM (parameter RAM)
 *   $0F00-$1FFF  the firmware image (4352 bytes); vectors at $1FF6
 *            (one-second), $1FF8 (core timer), $1FFA (/IRQ), $1FFC (SWI),
 *            $1FFE (reset)
 *
 * Other addresses read 0â€ . Ports read the output latch where the DDR bit
 * is set and the pin level elsewhere; the levels outside come from the
 * board's `pins` callback, evaluated at the 6805's own time, and every
 * change of what the chip drives goes to `outputs` (a pin whose DDR bit is
 * clear is released: reported as 1 in `levels` and 0 in `driven`).
 *
 * The chip keeps its own time: it runs, cycle by cycle, up to a guest time
 * (C15M periods) when asked (mc_adbctl_mcu_run). */
#ifndef MC_ADBCTL_MCU_H
#define MC_ADBCTL_MCU_H

#include "cpu6805.h"

#include <stddef.h>
#include <stdint.h>

#define MC_ADBCTL_FW_BASE 0x0F00u
#define MC_ADBCTL_FW_SIZE 4352u

/* Bus clock: the 32.768 kHz crystal's PLL (bit 6 of $07) or the crystal
 * divided by 2 (bus cycles in units of C15M/512: 15667200 / 2097152 =
 * 3825/512, 15667200 / 16384 = 3825/4). */
#define MC_ADBCTL_FAST_UNITS 3825u
#define MC_ADBCTL_SLOW_UNITS (3825u * 128u)

typedef struct mc_adbctl_mcu {
    mc_cpu6805 cpu;
    uint8_t rom[MC_ADBCTL_FW_SIZE];
    uint8_t ram[0x200];             /* $0000-$01FF: RAM at $90-$1FF */
    uint8_t latch[4], ddr[4];       /* ports A-C (and $03, kept) */
    uint8_t pll, ctcsr, ostcsr;
    uint8_t io[0x20];               /* other I/O addresses as written */
    uint64_t units;                 /* the chip's time in C15M/512 units */
    uint64_t timer_cycles;          /* bus cycles seen by the core timer */
    uint64_t next_second;           /* C15M time of the next one-second flag */
    uint8_t irq_latch;              /* a falling edge on /IRQ not taken yet (the level is cpu.irq_pin) */
    uint8_t keep_ram;               /* writes to $100-$1FF and $AB-$AE ignored (battery-backed contents kept) */
    uint8_t out_levels[3], out_driven[3];
    void *ctx;
    uint8_t (*pins)(void *ctx, unsigned port, uint64_t now);
    void (*outputs)(void *ctx, unsigned port, uint8_t levels, uint8_t driven, uint64_t now);
} mc_adbctl_mcu;

/* Builds the chip around a firmware image (MC_ADBCTL_FW_SIZE bytes) and
 * resets it at guest time `now`. */
void mc_adbctl_mcu_init(mc_adbctl_mcu *m, const uint8_t *image, uint64_t now, void *ctx,
                        uint8_t (*pins)(void *, unsigned, uint64_t),
                        void (*outputs)(void *, unsigned, uint8_t, uint8_t, uint64_t));
/* Power-on reset (the battery inserted): registers to their reset values,
 * the CPU from the reset vector. RAM is left as it is. */
void mc_adbctl_mcu_reset(mc_adbctl_mcu *m, uint64_t now);
/* Runs instructions until the chip's time reaches `until` (C15M). */
void mc_adbctl_mcu_run(mc_adbctl_mcu *m, uint64_t until);
/* The chip's time in C15M periods. */
static inline uint64_t mc_adbctl_mcu_now(const mc_adbctl_mcu *m) { return m->units / 512u; }
/* The /IRQ pin (1 = high): a falling edge requests the interrupt at $1FFA
 * (edge-sensitive†); BIH/BIL read the level. */
void mc_adbctl_mcu_set_irq(mc_adbctl_mcu *m, int level);
/* Memory as the CPU sees it (for traces and tests). */
uint8_t mc_adbctl_mcu_peek(mc_adbctl_mcu *m, uint16_t address);

#endif
