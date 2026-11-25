/* Livadix - SDRAM DIMM serial presence-detect EEPROMs.
 * Apple Power Macintosh G3 Developer Note (1998), pp.64-67: three 168-pin
 * SDRAM DIMMs, mandatory JEDEC21-C SPD EEPROM, Table4-2 row/column/banks.
 * Micron 32/64/128MB x64 SR 168-pin SDRAM UDIMM datasheet (Rev.L,2008),
 * Table21 SPD byte definitions; serial EEPROM sequential/random reads.
 * Cuda transaction framing from ROM 78F57389 FFF05484-FFF05620.
 * Synthesized module records describe the actual configured physical RAM;
 * no guest memory or ROM is changed. Livadix's own device implementation. */
#ifndef MC_SDRAM_SPD_H
#define MC_SDRAM_SPD_H
#include "../../hw.h"
#include <stdint.h>
typedef struct mc_sdram_spd {uint8_t data[3][256],pointer[3],present[3];} mc_sdram_spd;
/* Populate modules and return the six physical rank capacities. */
int mc_sdram_spd_init(mc_sdram_spd *,uint32_t bytes,uint32_t banks[8]);
int mc_sdram_spd_transfer(void *,uint8_t,const uint8_t *,unsigned,uint8_t *,unsigned *);
/* Each PowerBook slot has a separate Heathrow GPIO I2C bus. */
typedef struct mc_spd_gpio {
    const uint8_t *data;
    uint32_t drive;
    uint8_t board_sense, pointer, scl, sda, slave_low, mode, next_mode, stage, count, shift, ack;
} mc_spd_gpio;
int mc_sdram_spd_portable_init(mc_sdram_spd *,uint32_t bytes,uint32_t banks[8]);
void mc_spd_gpio_init(mc_spd_gpio *,const uint8_t *data,uint8_t board_sense);
extern const mc_dev_ops mc_spd_gpio_ops;
#endif
