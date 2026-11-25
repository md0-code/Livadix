/* Livadix — NuBus slots.
 *
 * The slot spaces of a NuBus machine: slot s answers in its standard (minor)
 * slot space $Fs00 0000-$FsFF FFFF and its super slot space $s000 0000-
 * $sFFF FFFF (Designing Cards and Drivers for the Macintosh Family, 3rd
 * ed., chapter 7 "NuBus Address Space" and "Slot Allocations"). A card
 * places its windows (memory, registers, the declaration ROM at the top of
 * its standard slot space, chapter 8 "The format block") in its slot; every
 * other address of an empty or partly decoded slot stays unmapped and
 * bus-errors, as a NuBus timeout does, which is how the Slot Manager finds
 * empty slots.
 *
 * Slot interrupts: each slot drives its own /NMRQ line, which the Quadra
 * 700 presents active low on a VIA2 port A bit and ORs into /SLOTIRQ on
 * VIA2 CA1 (Macintosh Quadra 700 Developer Note, Table 2-6 and "Direct
 * Slot interrupt handling"). The module keeps the lines' state; the board
 * turns it into its VIA pins through `pins`. Other sources sharing the
 * port (the Quadra 700's Ethernet and built-in video, Table 2-6) use the
 * same lines by their bit number.
 *
 * Livadix's own code. */
#ifndef MC_NUBUS_H
#define MC_NUBUS_H

#include "bus.h"

#include <stdint.h>

#define MC_NUBUS_SLOT_FIRST 0x9u
#define MC_NUBUS_SLOT_LAST 0xEu

typedef struct mc_nubus {
    mc_bus *bus;
    uint8_t slots;              /* bit s: slot s is a NuBus slot of the machine */
    uint8_t fitted;             /* bit s: a card is in slot s (or the machine's own device) */
    uint8_t line_bit[16];       /* the port A bit of each slot's interrupt line, 0xFF for none */
    uint8_t asserted;           /* port A bits whose interrupt source is asserted */
    uint8_t mask;               /* port A bits that carry interrupt lines */
    /* The board's pins: port A levels (active low, under `mask`) and
     * /SLOTIRQ (1 = no interrupt). */
    void (*pins)(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high);
    void *ctx;
} mc_nubus;

/* `slots`: bit s for each NuBus slot s the machine has. */
void mc_nubus_init(mc_nubus *n, mc_bus *bus, uint8_t slots,
                   void (*pins)(void *ctx, uint8_t levels, uint8_t mask, int slotirq_high), void *ctx);
/* Slot s's interrupt line is port A bit `bit` (also for a source that is
 * not a slot: use a slot number above $E). */
void mc_nubus_line(mc_nubus *n, unsigned slot, unsigned bit);
/* A source on port A bit `bit` changed. */
void mc_nubus_irq_bit(mc_nubus *n, unsigned bit, int asserted);
/* Slot s's card asserts or releases its interrupt. */
void mc_nubus_irq(mc_nubus *n, unsigned slot, int asserted);
/* Marks slot s as taken (a machine's own device in a slot's space). */
void mc_nubus_claim(mc_nubus *n, unsigned slot);
/* The first NuBus slot without a card, 0 when every slot is taken. */
unsigned mc_nubus_free_slot(const mc_nubus *n);

/* Standard and super slot space addresses of slot s. */
static inline uint32_t mc_nubus_slot_base(unsigned slot) { return 0xF0000000u | (uint32_t)slot << 24; }
static inline uint32_t mc_nubus_super_base(unsigned slot) { return (uint32_t)slot << 28; }

/* A card's windows in its standard slot space (offsets from $Fs00 0000):
 * memory the CPU reads and writes directly, a read-only image (the
 * declaration ROM), a device region. Returns 0 on failure. */
int mc_nubus_map_memory(mc_nubus *n, unsigned slot, uint32_t offset, uint32_t size, uint8_t *host, int writable);
int mc_nubus_map_device(mc_nubus *n, unsigned slot, uint32_t offset, uint32_t size, void *dev, const mc_dev_ops *ops);
/* Takes slot s for a card. 0 when it is not a free NuBus slot. */
int mc_nubus_fit(mc_nubus *n, unsigned slot);

#endif
