/* Livadix - Heathrow (Apple 343S1201), PCI I/O and 64-source interrupts.
 * Apple Power Macintosh G3 Computers Developer Note (1998), ch. 2
 * "Heathrow IC" (p.45): MESH, IDE, SCC, SWIM3, VIA/Cuda, Screamer and
 * 8 KiB NVRAM; CHRP I/O Device Reference ch.15 (DBDMA, dbdma.h).
 * ROM 78F57389, file $33DFAE: PCI 106B:0010, 512 KiB BAR; file
 * $33B0AA-$33C750 device tree: MESH +10000/DMA8000/IRQ12, SWIM3
 * +15000/DMA8100/IRQ19, SCC +13000/DMA8400-8700/IRQ15-16, sound
 * +14000/DMA8800-8900/IRQ17, IDE +20000/DMA8B00/IRQ13, IDE +21000/
 * DMA8C00/IRQ14, VIA +16000/IRQ18, NVRAM +60000 (byte stride16).
 * POST $FFF031DC-$FFF032D8: PCI dev16, BAR F3000000; interrupt mask24,
 * clear28, feature control38. Extended interrupt bank at +10..1C is
 * used by Heathrow's native interrupt tree. AppleHeathrow/Heathrow.h
 * (kTypeLevelMask) and Heathrow.cpp (enableVector, handleInterrupt):
 * sources 20..28 are level-sensitive, all others latch edges; enabled
 * pending sources assert INT without a bank-0 bit-31 master gate.
 * The classic nanokernel's bit-31 latch protocol is retained when enabled
 * by its mask write (G3 ROM FFF168xx); this mode distinction is inferred
 * from the original ROM and Darwin driver register sequences.
 * Apple Logic Board Design LPX-40 Developer Note, chapter 2 (O'Hare):
 * the earlier PCI I/O controller shares the SCC and DBDMA layout.
 * The Power Macintosh 6400's Open Firmware identifies it as 106B:0007.
 * Mac OS 8.0's LocalTalk HAL Wait100 writes $5C to the high CPU byte
 * at +$13100 and waits until that byte counts down to zero (CHRP I/O
 * 9.8.5, 921.6 kHz). Original RestartTxDMA polls ACTIVE alone, writes
 * CommandPtr and then RUN/WAKE. Its idle-pointer behavior is inferred
 * from that driver sequence, independently of the generic CHRP engine.
 * O'Hare motherboard sense (ROM 6F5724C0 $FFC1500E-$FFC150C8):
 * GPIO +$34 bits 0..2 drive the three corresponding input lines 4..6
 * low. board_id supplies their released levels. The 6400 has bits5/6
 * high (sense $1AB), the 5400 bit4 high (sense $054).
 * Built on Livadix's own Grand Central channel/window implementation.
 * No other emulator source read. */
#ifndef MC_HEATHROW_H
#define MC_HEATHROW_H
#include "hw.h"
#include "dbdma.h"
#include "pci.h"
#define MC_HEATHROW_SIZE 0x80000u
#define MC_HEATHROW_CHANNELS 13u
#define MC_HEATHROW_WINDOWS 24u
#define MC_HEATHROW_DBDMA 0x8000u
enum { MC_HEATHROW_LINE_INT = 0 };
typedef struct mc_heathrow_window {
    uint32_t offset,size; unsigned shift; void *dev; const mc_dev_ops *ops;
} mc_heathrow_window;
typedef struct mc_heathrow {
    mc_dev dev; mc_pci_config cfg;
    uint32_t mask[2],latched[2],level[2],active[2];
    uint32_t features,board_id;
    uint32_t feature_in;   /* input pins read in the feature control register */
    uint64_t serial_timer_end;
    int ohare;
    uint8_t gpio_drive;
    int pending_int,irq,pending,running,rerun;
    mc_dbdma_host dma_host; mc_dbdma ch[MC_HEATHROW_CHANNELS];
    mc_heathrow_window win[MC_HEATHROW_WINDOWS]; unsigned nwin;
    void *unmapped_ctx;
    void (*unmapped)(void *,uint32_t,unsigned,int,uint32_t);
} mc_heathrow;
extern const mc_dev_ops mc_heathrow_ops;
void mc_heathrow_init(mc_heathrow *,const mc_hw_host *,const char *);
void mc_ohare_init(mc_heathrow *,const mc_hw_host *,const char *);
void mc_heathrow_reset(mc_heathrow *);
mc_pci_function mc_heathrow_function(mc_heathrow *);
int mc_heathrow_attach(mc_heathrow *,uint32_t,uint32_t,unsigned,void *,const mc_dev_ops *);
void mc_heathrow_dma_port(mc_heathrow *,unsigned,const mc_dbdma_port *);
void mc_heathrow_dma_bits(mc_heathrow *,unsigned,uint8_t,uint8_t);
void mc_heathrow_dma_kick(mc_heathrow *,unsigned);
void mc_heathrow_set_source(mc_heathrow *,unsigned,int);
#endif
