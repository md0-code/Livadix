/* Livadix — the MCA, the Memory Controller and Arbiter of the Quadra 840AV and
 * Centris 660AV: the decoding of the eight DRAM banks.
 *
 * Quadra 840AV and Centris 660AV Developer Note (Apple, 1993), chapter 2,
 * "Random-Access Memory" (72-pin SIMMs of one or two banks of up to 16 MB;
 * 4 MB soldered and two SIMM slots on the 660AV, four SIMM slots and no
 * soldered RAM on the 840AV), "Memory Controller and Arbiter" (eight banks
 * of DRAM of up to 16 MB, ROM control, bus arbitration and time-outs),
 * "ROM and RAM Management" (the system software programs the MCA for the
 * RAM present; the ROM at 0 after reset until the first access to its own
 * range), Table 2-1 (bank organisations of 1 to 16 MB).
 *
 * The register layout is as ROM 5BF10FD1 programs it: the MCA answers at $50F3 0000-$50F3 07FF, every
 * register a longword of which only bit 31 is significant (one bit per
 * register; the ROM writes $00000000 or $FFFFFFFF, or a value rotated so the
 * bit wanted is in bit 31).
 *   - $400-$418: timing bits the ROM sets by model (ROM offset $F3E6);
 *   - $420-$42C: the DRAM organisation of the bank pairs (0 and 2, 1 and 3,
 *     4 and 6, 5 and 7: the ROM's table at $AE070), chosen by sizing the
 *     banks both ways (ROM offset $ADCF4); kept, not modelled (every bank
 *     answers in full either way);
 *   - $438, $43C, $440, $444: inputs, the board's identification in bit 31
 *     (bits 0-3 of the value the ROM compares with its ProductInfo records'
 *     byte $34: $F the Quadra 840AV, $B the Centris 660AV; ROM offset
 *     $F6EC);
 *   - $450 + $28n, bank n (0-7): seven registers with the bank's base in
 *     megabytes (bit 0 first), then three with its size code (bit 0 first):
 *     0 the bank off, c the bank answering for 1 MB << (c - 1) from its base
 *     (5: 16 MB). To size the banks the ROM puts bank n at n x 16 MB with
 *     code 5 and finds each bank's contents repeating through its window
 *     (ROM offset $ADB0C); then it packs the banks from 0, the largest first,
 *     each with its own size code (ROM offset $ADFF4).
 * A bank smaller than its window repeats through it; an empty bank and the
 * RAM space no bank covers float (reads 0, writes are lost). Every register
 * reads back what was written, the identification inputs excepted. */
#ifndef MC_MCA_H
#define MC_MCA_H

#include "../bus.h"
#include "../hw.h"

#include <stdint.h>

#define MC_MCA_BANKS 8
#define MC_MCA_SIZE 0x800u
#define MC_MCA_REGS (MC_MCA_SIZE / 4u)

typedef struct mc_mca {
    mc_dev dev;
    uint32_t reg[MC_MCA_REGS];
    uint8_t id;                             /* the identification inputs ($438-$444: bits 0-3) */
    uint32_t bank_size[MC_MCA_BANKS];       /* the populated banks (bytes, a power of two, or 0) */
    void (*remap)(void *ctx);               /* the decoding changed */
    void *ctx;
} mc_mca;

#define MC_MCA_ID_840AV 0x0Fu
#define MC_MCA_ID_660AV 0x0Bu

/* `bank_size`: the RAM of each bank, in order through hw->ram. */
void mc_mca_init(mc_mca *m, const mc_hw_host *host, const char *name, uint8_t id,
                 const uint32_t bank_size[MC_MCA_BANKS], void (*remap)(void *ctx), void *ctx);
extern const mc_dev_ops mc_mca_ops;
/* Maps the RAM banks into [0, $4000 0000) as the registers decode them. */
void mc_mca_map(const mc_mca *m, mc_bus *bus, uint8_t *ram);
/* Bank n's base (bytes) and window (bytes, 0: off) as programmed. */
uint32_t mc_mca_bank_base(const mc_mca *m, unsigned n);
uint32_t mc_mca_bank_window(const mc_mca *m, unsigned n);
/* Splits `ram_mb` into banks as the board's sockets hold it: `onboard_mb`
 * (0 or 4) soldered in bank 0, the rest in up to `simm_banks` SIMM banks
 * after it, each a power of two of 1 to 16 MB, the largest first. 0 when it
 * does not fit. */
int mc_mca_layout(uint32_t ram_mb, uint32_t onboard_mb, unsigned simm_banks, uint32_t bank_size[MC_MCA_BANKS]);

#endif
