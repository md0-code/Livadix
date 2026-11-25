/* Livadix — Grand Central (343S1125), the I/O controller of the first PCI
 * Power Macintosh generation: a PCI function holding the system interrupt
 * collection, the DBDMA engine of every I/O stream and the address decode
 * of the I/O devices behind it (Curio's 53C94, MACE and SCC, MESH, SWIM III,
 * the AWACS sound interface, Cuda's VIA, NVRAM and the DAC on its 16-bit
 * bus).
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * chapter 2 "Grand Central I/O Subsystem IC" (the Cuda support (VIA
 * registers), central system interrupt collection, SWIM III, a DMA
 * controller serving its devices and Curio's; bus interfaces for Curio,
 * Cuda, MESH and AWAC; a 16-bit bus to NVRAM and the Sixty6; the 33 MHz PCI
 * clock); Power Macintosh 7200 and 9500 Developer Notes (1995), the same
 * section ("provides DBDMA support for all I/O transfers"). Designing PCI
 * Cards and Drivers for Power Macintosh Computers (Apple, 1995), Listing
 * 8-1 (the 9500's device tree: gc@10 on the first Bandit, 53c94@10000,
 * mace@11000, escc@13000 and @13020, awacs@14000, swim3@15000,
 * via-cuda@16000, mesh@18000). CHRP I/O Device Reference (1996), chapter 15
 * (DBDMA, dbdma.h), 15.9 (a device's DBDMA interrupt shares its line).
 *
 * As ROM 9630C68B uses it (ROM at $FFC0 0000, file
 * offsets as "file"):
 *  - configuration: POST enables device 16 (IDSEL AD16) with BAR0 at
 *    $F300 0000 and command $16 (memory, bus master, memory write and
 *    invalidate) ($FFF036C4-$FFF0371C); Open Firmware's gc node (file
 *    $3436E1) gives the BAR 1 MB ($10 0000). The device ID ($0002),
 *    revision and class code are not in the ROM†.
 *  - interrupts: registers at +$20 events, +$24 mask, +$28 clear, +$2C
 *    levels, little-endian; POST writes the mask's bit 31 alone
 *    ($FFF03730); the Mac OS interrupt tree (file $267970,
 *    "InterruptTreeTNT") enables sources by setting mask bits, acknowledges
 *    sources 0-10 by writing their bit to +$28 (file $2697F4-$269A40), the
 *    others not (level sources). Source n is event bit n: 0-10 the DBDMA
 *    channels, 12 53C94†, 13 MESH, 14 MACE, 15 SCC channel A, 16 SCC
 *    channel B, 17 AWACS, 18 VIA (Cuda), 19 SWIM III, 20 NMI†, 27 Sixty6,
 *    30 the display (Platinum's VBL) (Open Firmware nodes' interrupt
 *    lists, file $3441B5-$345981).
 *  - DBDMA channels at +$8000 + $100 n: 0 53C94, 1 SWIM III, 2/3 MACE
 *    transmit/receive, 4/5 SCC A transmit/receive, 6/7 SCC B, 8/9 AWACS
 *    out/in†, 10 MESH (the nodes' reg pairs).
 *
 * Livadix's model: the event register holds the latched edges of sources
 * 0-10 and 20† and the current level of the others; the clear register
 * drops latched edges; the levels register reads the inputs and the
 * latched edges (the nanokernel derives the 68k level from mask & levels,
 * $FFF15700-$FFF157CC). The processor interrupt is latched whenever the
 * enabled events (events & mask, sources 0-30) change, a drop included,
 * and cleared by a write of bit 31 to the clear register, which the
 * nanokernel's entry makes; it is asserted while that latch and mask bit 31
 * (the master enable POST sets) are set†. A channel's request arriving
 * while the channels run (a port raising DREQ as it gives a byte) runs
 * them once more instead of re-entering. The
 * devices' windows: the board attaches each device's ops at its offset;
 * an access reaches the device at (offset - its base) >> shift, with the
 * little-endian value the PCI bus carries. Offsets nothing answers read 0.
 *
 * Output line (mc_hw_host line): MC_GC_LINE_INT, the processor's external
 * interrupt (1 = asserted).
 *
 * Livadix's own code. */
#ifndef MC_GRAND_CENTRAL_H
#define MC_GRAND_CENTRAL_H

#include "hw.h"
#include "dbdma.h"
#include "pci.h"

#include <stdint.h>

enum { MC_GC_LINE_INT = 0 };

#define MC_GC_SIZE 0x100000u        /* BAR0 */
#define MC_GC_DEVICE 16u            /* its device number on the Bandit (gc@10) */
#define MC_GC_CHANNELS 11u
#define MC_GC_WINDOWS 16u

/* Interrupt registers (little-endian). */
enum { MC_GC_EVENTS = 0x20, MC_GC_MASK = 0x24, MC_GC_CLEAR = 0x28, MC_GC_LEVELS = 0x2C };
#define MC_GC_DBDMA 0x8000u

/* Sources. */
enum {
    MC_GC_IRQ_DMA_SCSI0 = 0, MC_GC_IRQ_DMA_FLOPPY = 1, MC_GC_IRQ_DMA_ENET_TX = 2, MC_GC_IRQ_DMA_ENET_RX = 3,
    MC_GC_IRQ_DMA_SCCA_TX = 4, MC_GC_IRQ_DMA_SCCA_RX = 5, MC_GC_IRQ_DMA_SCCB_TX = 6, MC_GC_IRQ_DMA_SCCB_RX = 7,
    MC_GC_IRQ_DMA_AUDIO_OUT = 8, MC_GC_IRQ_DMA_AUDIO_IN = 9, MC_GC_IRQ_DMA_SCSI1 = 10,
    MC_GC_IRQ_SCSI0 = 12, MC_GC_IRQ_SCSI1 = 13, MC_GC_IRQ_ENET = 14, MC_GC_IRQ_SCCA = 15, MC_GC_IRQ_SCCB = 16,
    MC_GC_IRQ_AUDIO = 17, MC_GC_IRQ_VIA = 18, MC_GC_IRQ_FLOPPY = 19, MC_GC_IRQ_NMI = 20,
    MC_GC_IRQ_SIXTY6 = 27, MC_GC_IRQ_DISPLAY = 30
};
/* Device offsets (Listing 8-1; ROM). */
enum {
    MC_GC_53C94 = 0x10000, MC_GC_MACE = 0x11000, MC_GC_SCC = 0x13000, MC_GC_AWACS = 0x14000,
    MC_GC_SWIM3 = 0x15000, MC_GC_VIA = 0x16000, MC_GC_MESH = 0x18000, MC_GC_ENET_ROM = 0x19000,
    MC_GC_DAC = 0x1B000, MC_GC_SIXTY6 = 0x1C000, MC_GC_NVRAM = 0x1D000
};

typedef struct mc_gc_window {
    uint32_t offset, size;
    unsigned shift;
    void *dev;
    const mc_dev_ops *ops;
} mc_gc_window;

typedef struct mc_gc {
    mc_dev dev;
    mc_pci_config cfg;
    uint32_t mask, latched, level;
    uint32_t active;                /* events & mask (sources 0-30) at the last update */
    int pending_int;                /* the processor's interrupt latched, until acknowledged */
    int irq;
    mc_dbdma_host dma_host;
    mc_dbdma ch[MC_GC_CHANNELS];
    mc_gc_window win[MC_GC_WINDOWS];
    unsigned nwin;
    int pending;                    /* a channel has work for the next event */
    int running, rerun;             /* the channels are running; a request came meanwhile */
} mc_gc;

extern const mc_dev_ops mc_gc_ops;  /* offsets within BAR0 (little-endian values) */

void mc_gc_init(mc_gc *g, const mc_hw_host *host, const char *name);
void mc_gc_reset(mc_gc *g);
/* The PCI function for the bridge (mc_bandit_attach at MC_GC_DEVICE). */
mc_pci_function mc_gc_function(mc_gc *g);
/* A device at `offset` of the BAR, `size` bytes, reached at
 * (offset - base) >> shift. */
int mc_gc_attach(mc_gc *g, uint32_t offset, uint32_t size, unsigned shift, void *dev, const mc_dev_ops *ops);
/* A device's DBDMA stream (channel 0-10). */
void mc_gc_dma_port(mc_gc *g, unsigned channel, const mc_dbdma_port *port);
/* A channel's hardwired status bits s7-s0 (a device's line the board
 * wires there; mc_dbdma_set_bits). */
void mc_gc_dma_bits(mc_gc *g, unsigned channel, uint8_t mask, uint8_t value);
/* A device's DMA request: the channel may move data now. */
void mc_gc_dma_kick(mc_gc *g, unsigned channel);
/* An interrupt source's input (0-31; 1 = asserted). */
void mc_gc_set_source(mc_gc *g, unsigned source, int state);

#endif
