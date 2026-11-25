/* Livadix — the built-in video's DAC on Grand Central's bus: RaDACal (the
 * 7500's, 8500's, 7600's and 8600's, with Control) and DACula (the 7200's,
 * with Platinum), the colour table and the DAC's control registers.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * "Video Subsystem ICs" (RaDACal, a high-performance DAC for the monitor;
 * it also generates the video timing and supports the hardware cursor);
 * Power Macintosh 7200 Developer Note (1995), the block diagram (DACula).
 *
 * As ROM 9630C68B reaches it: four byte ports at
 * Grand Central + $1B000 + $10 n (Open Firmware's control FCode constant
 * $F301B000, file $3466D7; the Control and Platinum display drivers, files
 * $26BD50 and $27AB60):
 *  +$00 address: the colour table index, or the control register index
 *  +$10 an 8-entry overlay or cursor colour table, R, G, B per entry from
 *       the address (the drivers write black, red, green, blue, yellow,
 *       cyan, magenta, white)†
 *  +$20 the control register the address selects (the drivers write $20,
 *       $21, $11, $10, $22; Open Firmware $21 = 1, $10 = $11 = 0); the 7200's
 *       driver reads register $40 and takes $84 for its DAC
 *       ($FFE7F0D4-$FFE7F0E0)
 *  +$30 the colour table: R, G, B for the entry at the address, which then
 *       advances.
 *
 * Livadix's model: 256 colour entries and 8 overlay entries, 256 control
 * registers kept as written, register $40 reading the variant's ID (DACula
 * $84; RaDACal's not in the ROM: kept as written†). The register index is
 * the ops' offset (the board attaches the window with shift 4).
 *
 * Livadix's own code. */
#ifndef MC_RADACAL_H
#define MC_RADACAL_H

#include "hw.h"

#include <stdint.h>

typedef enum { MC_DAC_RADACAL, MC_DAC_DACULA } mc_dac_kind;
enum { MC_DAC_ADDRESS, MC_DAC_OVERLAY, MC_DAC_CONTROL, MC_DAC_CLUT };
#define MC_DAC_ID_REGISTER 0x40u
#define MC_DACULA_ID 0x84u

typedef struct mc_radacal {
    mc_dev dev;
    uint8_t kind;
    uint8_t address, component;     /* the entry and its next byte (0 R, 1 G, 2 B) */
    uint8_t reg[256];
    uint8_t clut[256][3];
    uint8_t overlay[8][3];
    uint32_t palette[256];          /* x-R8-G8-B8 for the view */
    uint32_t serial;                /* counts colour table changes */
} mc_radacal;

extern const mc_dev_ops mc_radacal_ops;

void mc_radacal_init(mc_radacal *d, const mc_hw_host *host, const char *name, mc_dac_kind kind);
void mc_radacal_reset(mc_radacal *d);

#endif
