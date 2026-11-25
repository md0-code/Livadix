/* Livadix — the PowerBook Duo's Power Manager, PG&E, at firmware level: Apple's
 * PG&E boot ROM (pmu-pge-boot.bin, 512 bytes) and the main program the Mac
 * ROM downloads into it, on the 68HC05 core (cpu6805.c). Internal to pmu.c,
 * which dispatches to it.
 *
 * The 68HC05 interface this file uses (cpu6805.h):
 *   typedef struct mc_cpu6805 { uint8_t a, x, sp, cc; uint16_t pc, pc_mask,
 *       vector_top; uint8_t stopped, waiting, irq_pin; uint64_t cycles,
 *       illegal; void *ctx; uint8_t (*read)(void *, uint16_t);
 *       void (*write)(void *, uint16_t, uint8_t); } mc_cpu6805;
 *   void mc_cpu6805_init(mc_cpu6805 *, void *ctx, read, write, uint16_t
 *       vector_top);       the reset vector's address; pc_mask then set here
 *   void mc_cpu6805_reset(mc_cpu6805 *);
 *   unsigned mc_cpu6805_step(mc_cpu6805 *);      cycles, 0 while STOP/WAIT
 *   unsigned mc_cpu6805_interrupt(mc_cpu6805 *, uint16_t vector);
 *                                               10 cycles, 0 when masked
 * Nothing else of the core is used.
 *
 * What the boot ROM shows (disassembled with Livadix's work tools): a 16-bit part (reset vector $FFFE -> $FE10, /IRQ
 * $FFFA -> $FFD2, SWI $FFFC; the timer and SCI vectors hold the text
 * "APPL91"), the register page of the MC68HC05C4 layout (ports $00-$03 and
 * their directions $04-$07, SPI control/status/data $0A-$0C) and six more
 * port pairs at $20-$2B; RAM from $40 (the stack at $C0-$FF; both programs
 * also use $0100-$03FF, the main program lives at $7FFC-$FDFF: Livadix makes
 * $0040-$FDFF one RAM†); a COP reset at $0FF0 (written $00 in every loop);
 * the ROM at $FE00-$FFFF.
 * Its command loop exchanges each byte with the Mac through the SPI as
 * master (its clock drives VIA1's CB1, so the VIA's shift register takes or
 * gives the byte): wait for /REQ low ($22 bit 6), write the SPI data
 * register, pull /ACK low ($26 bit 6), wait for the transfer's end (SPSR
 * bit 7), read the data, wait for /REQ high, raise /ACK. Command $E1
 * downloads: four bytes to $7FFC-$7FFF, then the program from $8000 until
 * the address wraps; its checksum over $8002-$FDFF must come out 0; then
 * $8005 runs it. Command $E2 answers $94.
 * The Duo ROMs carry such an image, tagged "BORG", at $AA9C0 (Duo 210 ROM;
 * also in the 270c, 280, 520 and 150 ROMs) with the jump table $8005,
 * $8008, $800B, $800E, $8011, $8014 the boot ROM expects.
 *
 * What the main program shows (the Duo 210 ROM's image, disassembled the
 * same way):
 * - Its own vector table at $FFF0-$FFFB of the image: $FFF4 the one-second
 *   interrupt ($86F7, which clears $0D bit 6), $FFF6 the tick ($86C4,
 *   clears $0D bit 7), $FFF8 the ADB transceiver ($8A7D), $FFFA /IRQ. The
 *   boot ROM clears $1C bit 6 before it enters the program (or calls it
 *   from its /IRQ handler) and sets it in its own sleep loop: Livadix takes
 *   the vectors from what the download wrote while $1C bit 6 is clear, the
 *   boot ROM's ($FFF0 for the one-second and tick interrupts) while it is
 *   set†.
 * - $0D: bit 6 the one-second flag, bit 7 the tick's; bits 4 and 5 enable
 *   them (the main program sets both, the boot ROM's sleep loop bit 4).
 * - $38-$3B: the clock, big-endian seconds ($30 writes them, $38 reads).
 * - Parameter RAM at $0300-$03FF ($31/$39 the first 20 bytes at $0310 and
 *   $0308, $32/$3A any range from $0300).
 * - The ADB transceiver: $18 control (bits 7, 6, 5, 3 enable the flags of
 *   $19; bit 0 sends a reset), $19 status (bit 7 the transmitter empty, 6
 *   the transaction done, 5 no answer, 4 a Service Request, 3 a byte
 *   received), $1A data. The program writes the command and any Listen
 *   data at the transmitter-empty interrupts, then clears $18 bit 7; the
 *   reply comes byte by byte, then the done (or no-answer) flag. Autopoll
 *   is the program's own, on the tick.
 * - The A-D converter: $1E control (bit 5 on, bits 0-2 the channel; bit 7
 *   the conversion done), $1D the result; channels 0-4 (battery, charger).
 * - Power: $22 bit 1 driven (its direction $23 bit 1) high turns the main
 *   power on, low off; $24 bit 3 reads the power back (the program waits
 *   for it). /ACK $26 bit 6. $20 bit 2 driven low holds the processor in
 *   reset (the power-on sequence releases it last; $D0 pulses it). $22 bit
 *   2 driven low is /INT, held until the Mac's $78 reads the cause.
 *
 * - Sleep and shut down both turn the main power off; RAM $91 still $62
 *   marks a sleep (cleared on the way to shut down, $9DEA/$9E31; the boot
 *   ROM's reset takes $62 as a wake, $FE28), which Livadix reports as
 *   MC_PMU_POWER_SLEEP (the Mac keeps its RAM, the processor held).
 * - $0F: bit 3 enables and bit 7 flags a key interrupt (vector $FFF0; the
 *   boot ROM's sleep loop enables it, $FE07, the program's handler $872C
 *   and its sleep-time entry $CA0F check the power key, $97AE); a write
 *   clears the flag; enabling it with a key still held reasserts it
 *   (inferred from $C9AC masking/rearming around the $90C3 scan).
 *   $24 bit 6: the AC adapter (external power: the
 *   start's battery check skipped; asleep or off, the charging loop that
 *   watches the keys, $CA18, $C983).
 * - The power key: $7E in the keyboard table (row 5, column 0), also read
 *   on row 5 with no column driven ($934A).
 * Livadix's choices, †: the E clock (2.097152 MHz), the tick at 64 Hz, the
 * ADB timing (the ADB's 100 us bit cell), /IRQ never asserted (no dock),
 * the AC adapter always connected,
 * the A-D codes, the conversion instantaneous, unknown registers as
 * latches. */
#ifndef MC_PMU_PGE_H
#define MC_PMU_PGE_H

#include "pmu.h"

#include "cpu6805.h"

#define MC_PGE_E_HZ 4194304u
#define MC_PGE_TICK_HZ 64u
#define MC_PGE_BOOT_BASE 0xFE00u
#define MC_PGE_BOOT_SIZE 512u
#define MC_PGE_XRAM_BASE 0x7FFCu        /* where a download starts */
#define MC_PGE_PRAM 0x0300u             /* parameter RAM, 256 bytes */

typedef struct mc_pmu_pge {
    mc_cpu6805 cpu;
    mc_pmu *pmu;
    uint8_t reg[0x40];              /* $00-$3F */
    uint8_t ram[MC_PGE_BOOT_BASE - 0x40];   /* $0040-$FDFF: on-chip and external RAM */
    uint8_t boot[MC_PGE_BOOT_SIZE];
    uint8_t vectors[16];            /* $FFF0-$FFFF as a download wrote them */
    uint8_t pins[0x40];             /* levels the board drives on port bits ($00-$03, $20-$2A data registers) */
    uint8_t ad[8];                  /* the A-D inputs' codes */
    uint8_t spi_in;                 /* the byte the last exchange brought in */
    uint32_t spi_left;              /* E cycles to the exchange's end */
    uint32_t second_left, tick_left;        /* E cycles to the next second and tick */
    uint8_t loaded;                 /* mc_pmu_pge_load gave a program */
    /* The ADB transceiver. */
    uint8_t adb_phase;              /* 0 idle, 1 sending, 2 answering */
    uint8_t adb_out[16], adb_out_n;
    uint8_t adb_in[8], adb_in_n, adb_in_pos;
    uint32_t adb_left;              /* E cycles to its next event (0: none) */
    uint16_t keys[8];               /* per column, the rows of the keys down */
    uint8_t mods;                   /* $01 bits 3-7 of the modifiers down */
    int16_t dx, dy;                 /* the trackball's counts */
    uint8_t buttons;
    uint8_t int_out;                /* $22 bit 2 as driven */
    uint32_t int_left;              /* E cycles to the end of the /INT pulse (0: none) */
    uint32_t again_left;            /* E cycles of /REQ high before /INT pulses again */
    uint64_t base_c15m, base_cycles;
} mc_pmu_pge;

mc_pmu_pge *mc_pmu_pge_new(mc_pmu *p, const uint8_t *boot);
void mc_pmu_pge_free(mc_pmu_pge *g);
void mc_pmu_pge_reset(mc_pmu_pge *g);
void mc_pmu_pge_event(mc_pmu_pge *g, uint64_t now);
void mc_pmu_pge_catch_up(mc_pmu_pge *g, uint64_t now);
void mc_pmu_pge_req(mc_pmu_pge *g, int level);
/* Loads a main program image (from $7FFC, as $E1 would) without the
 * download: for harnesses that take it from a Duo ROM. */
void mc_pmu_pge_load(mc_pmu_pge *g, const uint8_t *image, size_t size);
/* The built-in keyboard (an ADB key code; 0 when it has none) and the
 * trackball. */
int mc_pmu_pge_key(mc_pmu_pge *g, uint8_t code, int down);
void mc_pmu_pge_mouse(mc_pmu_pge *g, int dx, int dy, int button);
/* Parameter RAM ($0300-$03FF) and the clock ($38-$3B). */
void mc_pmu_pge_pram_get(mc_pmu_pge *g, uint8_t *out);
void mc_pmu_pge_pram_put(mc_pmu_pge *g, const uint8_t *in);
uint32_t mc_pmu_pge_seconds(mc_pmu_pge *g);
void mc_pmu_pge_set_seconds(mc_pmu_pge *g, uint32_t seconds);

#endif
