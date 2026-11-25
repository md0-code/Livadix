/* Livadix - the Am29F010 flash memory: 128 KiB in eight 16 KiB sectors,
 * written with the JEDEC command sequences.
 *
 * Source: AMD Am29F010 data sheet (publication 16736), "Command
 * Definitions" (table 4), "Autoselect", "Byte Programming", "Chip Erase",
 * "Sector Erase" and "Write Operation Status":
 *  - a command is unlocked by $AA to address $5555 and $55 to $2AAA (the
 *    address bits A14-A0 are compared);
 *  - $F0 returns to reading the array (alone, or after the unlock);
 *  - $90 autoselect: address 0 reads the manufacturer $01, address 1 the
 *    device $20, address 2 of a sector its protection (0);
 *  - $A0 program: the next write clears the bits of one byte that the data
 *    has 0 (a bit never goes from 0 to 1);
 *  - $80, the unlock again, then $10 to $5555 erases the chip, $30 to a
 *    sector's address erases that sector, to $FF.
 * Programming and erasing complete at once here: the status a read gives
 * during them (DQ7 inverted, DQ6 toggling) is never seen, a read after the
 * command gives the array, which is what the data sheet's polling ends on.
 *
 * Livadix's own code. */
#ifndef MC_AM29F010_H
#define MC_AM29F010_H

#include <stdint.h>

#define MC_AM29F010_SIZE 0x20000u
#define MC_AM29F010_SECTOR 0x4000u

typedef struct mc_am29f010 {
    uint8_t data[MC_AM29F010_SIZE];
    uint8_t state;
    uint8_t dirty;      /* the array changed since the owner last cleared this */
} mc_am29f010;

/* An erased chip (every byte $FF), reading the array. */
void mc_am29f010_init(mc_am29f010 *f);
/* Power-on or hardware reset: back to reading the array, the array kept. */
void mc_am29f010_reset(mc_am29f010 *f);
uint8_t mc_am29f010_read(const mc_am29f010 *f, uint32_t address);
void mc_am29f010_write(mc_am29f010 *f, uint32_t address, uint8_t value);

#endif
