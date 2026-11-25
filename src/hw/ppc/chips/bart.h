/* Livadix — BART, the NuBus controller of the Power Macintosh 6100 (on its
 * NuBus adapter card), 7100 and 8100: the gateway between the 601's bus
 * and NuBus.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994): chapter
 * 2, "BART NuBus Controller" (the data gateway between NuBus and the CPU
 * bus, a CPU bus master for one- and four-beat transactions, NuBus '90),
 * "Physical Memory Allocations"; chapter 4, "NuBus Interface" (three slots
 * on the 7100 and 8100; on the 6100 one short card on the adapter in the
 * PDS, which carries the BART; no locked NuBus cycles). Enhanced Power
 * Macintosh Developer Note (Apple, 1994), "NuBus Support" and Table 1-3
 * (BART 4 and BART 21; the later ROM adds minor slots 1-E and major slot
 * 8). Designing Cards and Drivers for the Macintosh Family, 3rd ed.,
 * chapter 7 (standard slot space $Fs00 0000-$FsFF FFFF, super slot space
 * $s000 0000-$sFFF FFFF; an undecoded address times out, which the Slot
 * Manager takes for an empty slot), chapter 8 (the declaration ROM at the
 * top of the standard slot space).
 *
 * As ROM 9FEB69B3 drives it (ROM addresses at
 * $FFC0 0000): its registers at $F000 0000 (the PDM decoder tables' entry
 * at $FFC155A0); the start-up code writes $80 to +$00 and, unless the CPU
 * ID reads $3011, $80 to +$11 ($FFCC5278-$FFCC5290); the Slot Manager then
 * reads each slot's top bytes from $FEFF FFFF down and takes a timeout for
 * an empty slot. The Workgroup Server 9150 Developer Note (1995),
 * "ID Register", documents the read-only BART 4/21 ID at +$08. Other
 * register meanings are not documented: Livadix keeps them read back as
 * written†. Without the BART (a 6100 without the adapter) the machine leaves $F000 0000 undecoded: the ROM's write times
 * out and it carries on (observed on Livadix's probe of the ROM).
 *
 * The slots: each card places windows in its standard slot space, as on
 * the 68k machines (nubus.h); everything else of a slot times out. Super
 * slot space is not decoded. Connector slot numbers follow the 9150
 * Developer Note, Figure 2 (slots $A-$D
 * and PDS $E; the 7100/8100 omit $A). Each slot's /NMRQ: the BART gathers
 * them and reports the asserted slots as a bit mask (bit s for slot s) on
 * its line; the AMIC presents them in its pseudo-VIA2 (amic.h).
 *
 * Output line: MC_BART_LINE_SLOTS (state = the mask of asserted slots).
 *
 * Livadix's own code. */
#ifndef MC_BART_H
#define MC_BART_H

#include "hw.h"

#include <stdint.h>

enum { MC_BART_LINE_SLOTS = 0 };

#define MC_BART_ID_4 0x43184001u
#define MC_BART_ID_21 0x43184003u

#define MC_BART_SIZE 0x1000u        /* registers at $F000 0000 */
#define MC_BART_WINDOWS 8u          /* windows per slot */

typedef struct mc_bart_window {
    uint32_t offset, size;      /* in the standard slot space */
    uint8_t *host;              /* memory, or NULL for a device */
    int writable;
    void *dev;
    const mc_dev_ops *ops;
} mc_bart_window;

typedef struct mc_bart_slot {
    int fitted;
    unsigned n;
    mc_bart_window w[MC_BART_WINDOWS];
} mc_bart_slot;

typedef struct mc_bart {
    mc_dev dev;
    uint8_t reg[256];
    uint32_t id;                /* board-selected BART revision; survives reset */
    uint16_t slots;             /* bit s: s is a slot of the machine */
    uint16_t asserted;          /* bit s: slot s asserts its interrupt */
    mc_bart_slot slot[16];
} mc_bart;

extern const mc_dev_ops mc_bart_ops;    /* the registers */

/* `slots`: bit s for each slot s the board has ($9-$E). */
void mc_bart_init(mc_bart *b, const mc_hw_host *host, const char *name, uint16_t slots);
/* Takes slot s for a card (0 when it is not a free slot of the board). */
int mc_bart_fit(mc_bart *b, unsigned slot);
/* A card's windows in its standard slot space (offsets from $Fs00 0000);
 * 0 on failure. */
int mc_bart_map_memory(mc_bart *b, unsigned slot, uint32_t offset, uint32_t size, uint8_t *host, int writable);
int mc_bart_map_device(mc_bart *b, unsigned slot, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops);
/* The first free slot (0 when none). */
unsigned mc_bart_free_slot(const mc_bart *b);
/* Slot s's card asserts or releases /NMRQ. */
void mc_bart_irq(mc_bart *b, unsigned slot, int asserted);

/* An access the CPU makes in NuBus space ($9000 0000-$EFFF FFFF super slot,
 * $F100 0000-$FEFF FFFF standard slot): 1 with the data, 0 on a NuBus
 * timeout. Little more than a decode: NuBus' byte lanes are the CPU's. */
int mc_bart_read(mc_bart *b, uint32_t address, unsigned size, uint32_t *value);
int mc_bart_write(mc_bart *b, uint32_t address, unsigned size, uint32_t value);

#endif
