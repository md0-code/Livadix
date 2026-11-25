/* Livadix - Heathrow IDE cell. G3 ROM 78F57389, ATA native driver
 * FFEE430C timing register +200, FFEE4384 alternate status/control +160,
 * FFEE4520/FFEE46BC data word transfers; ATA-2 task file (ata.h).
 * PCI lane order is little endian; Grackle converts CPU accesses. */
#ifndef MC_HEATHROW_IDE_H
#define MC_HEATHROW_IDE_H
#include "../../ata.h"
typedef struct mc_heathrow_ide { mc_ata ata; uint32_t timing; } mc_heathrow_ide;
extern const mc_dev_ops mc_heathrow_ide_ops;
void mc_heathrow_ide_init(mc_heathrow_ide *,const mc_hw_host *);
#endif
