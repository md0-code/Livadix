/* Livadix — F108, the memory controller and I/O support IC of the Quadra 630 /
 * LC 630 and LC 580 families: the RAM banks' decoding (this file) and the
 * IDE interface (the task file's window, byte lanes and INTRQ; f108_ata).
 *
 * Macintosh LC 630 and Quadra 630 Developer Note (1994), chapter 2, "F108
 * IC" (control logic for the ROM and RAM, a SCSI controller "just like an
 * NCR 53C96" clocked at 16.5 MHz, SCC circuitry "just like the 8530" with an
 * 8 MHz PCLK, the IDE hard disk interface controller; the memory control
 * logic "similar to that in the MEMCjr IC used in the Macintosh Quadra
 * 605"), "RAM Addresses" (the first 1 GB of the address space reserved for
 * RAM, 4 to 36 MB, sized by the ROM at startup), chapter 1 (4 MB built in,
 * one 72-pin SIMM).
 *
 * The register layout is what ROMs 06684214 and 064DC91D program
 * (ROM offsets $4952-$4982 and
 * $A014A-$A0636): longword registers at $50F0E000 + 4n. A register carries
 * only its low 6 bits on a cycle; the rest goes through the register at
 * +$7C: the ROM writes a value rotated right 6 bits to +$7C, then the value
 * itself to the register, which takes its bits 6-31 from +$7C and bits 0-5
 * from the cycle; a read of a register returns bits 0-5 and leaves the
 * value rotated right 6 bits in +$7C for the next read. Registers: +$04 to
 * +$14 the starts of the five RAM banks in megabytes (bits 0-9; bit 10 is
 * kept by the ROM when it rewrites them), +$2C the last megabyte of RAM
 * (bits 0-9), +$30 and +$34 timing values (written $A55A00DC and $A55A01E7
 * at 33 MHz; kept). To size the banks the ROM puts bank n at n x 64 MB with
 * the end at the top and looks for each bank's contents repeating through
 * its window; then it packs them. Bank n answers from its start up to the
 * next bank's start (the last up to the end), repeating its contents; an
 * empty bank and the space above the end float (reads 0, writes are lost).
 * Every other register reads back what was written. */
#ifndef MC_F108_H
#define MC_F108_H

#include "../ata.h"
#include "../bus.h"
#include "../hw.h"

#include <stdint.h>

#define MC_F108_BANKS 5
#define MC_F108_REGS 32
#define MC_F108_UNIT (1u << 20)         /* the bank registers' unit: 1 MB */

typedef struct mc_f108 {
    mc_dev dev;
    uint32_t reg[MC_F108_REGS];         /* reg[31] is the +$7C latch */
    uint32_t bank_size[MC_F108_BANKS];  /* the populated banks (bytes, a power of two, or 0) */
    void (*remap)(void *ctx);           /* the decoding changed */
    void *ctx;
} mc_f108;

/* `bank_size`: the RAM of each bank, in order through hw->ram. */
void mc_f108_init(mc_f108 *f, const mc_hw_host *host, const char *name, const uint32_t bank_size[MC_F108_BANKS],
                  void (*remap)(void *ctx), void *ctx);
extern const mc_dev_ops mc_f108_ops;    /* offsets from $50F0E000 */
/* Maps the RAM banks into [0, $40000000) as the registers decode them. */
void mc_f108_map(const mc_f108 *f, mc_bus *bus, uint8_t *ram);
/* Splits `ram_mb` into banks: `onboard_mb` soldered in bank 0, the rest in
 * the SIMM banks from bank 1 (a 72-pin SIMM is one or two banks of 4, 8 or
 * 16 MB; `simm_banks` banks in all), the largest first. 0 when it does not
 * fit. */
int mc_f108_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_F108_BANKS]);

/* --- the IDE interface and the slot interrupt register ---------------------
 *
 * The window at $5001 A000 (the decoder table's IDE base), as the ROM's ATA
 * Manager addresses it (ROM offsets $CEE64-$CEF3E, $CFB36-$CFB6E): bus n's
 * task file at +n x $80, register r (the ATA Manager's register selector,
 * the ATA Device Software Guide, Table 3-3) at +4r, so the alternate status
 * and device control at +$38; one bus. The data register is the IDE data
 * bus byte-swapped onto the I/O bus (Developer Note, "IDE Hard Disk": DD0-7
 * on IOD24-31, DD8-15 on IOD16-23): the first byte of a sector is the high
 * byte of the word the 68040 reads (ata.h, lane_swap).
 *
 * The byte at +$101 gathers interrupts for the VIA2 slot interrupt handler
 * (ROM offset $8BC2C, installed through low memory $D74; its dispatch
 * table at ROM offset $6F14): bit 6 the built-in video's VBL (dispatched as
 * slot 0), bit 5 the IDE INTRQ (slot $D), bit 4 an input dispatched as slot
 * $C, bits 2 and 3 inputs dispatched as slots $A and $B when bits 0 and 1
 * (written) enable them; the ATA Manager's CheckATAInt reads bit 5 (ROM
 * offset $D0416). Bits 0-1 read back what was written; the rest are the
 * inputs' levels. An enabled input pulls /SLOTIRQ, VIA2 CA1, low (the line
 * MC_F108_LINE_SLOTIRQ, 1 = asserted). */

enum { MC_F108_LINE_SLOTIRQ };
enum { MC_F108_INT_VIDEO = 0x40, MC_F108_INT_IDE = 0x20 };

typedef struct mc_f108_ide {
    mc_dev dev;
    mc_ata *ata;
    uint8_t control;            /* bits 0-1 */
    uint8_t inputs;             /* bits 2-6 */
    uint8_t out;
} mc_f108_ide;

void mc_f108_ide_init(mc_f108_ide *i, const mc_hw_host *host, const char *name, mc_ata *ata);
extern const mc_dev_ops mc_f108_ide_ops;    /* offsets from $5001 A000 */
/* An interrupt input (MC_F108_INT_*) changed. */
void mc_f108_ide_set(mc_f108_ide *i, uint8_t bit, int state);

#endif
