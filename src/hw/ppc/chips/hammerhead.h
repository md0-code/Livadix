/* Livadix — Hammerhead (343S1142), the memory controller of the 7500, 8500,
 * 9500, 7300, 7600, 8600 and 9600: the system bus controller, the DRAM banks, the
 * ROM and the second-level cache, its registers at $F800 0000.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * chapter 2 "Hammerhead Memory Controller IC" (system bus, DRAM, ROM and L2
 * cache controller; a 128-bit DRAM array; up to 1.5 GB), "Second-Level
 * Cache" (the cache SIMM's presence and size read at start-up from two pins;
 * none: external cache disabled); Power Macintosh 9500 Developer Note
 * (1995), "Hammerhead Memory Controller IC" (a bank base register for each
 * bank of RAM, its base address and control bits; banks of equal size
 * interleaved); Designing PCI Cards and Drivers for Power Macintosh
 * Computers (Apple, 1995), Table 2-1 ($F800 0000-$F8FF FFFF system control).
 *
 * As ROM 9630C68B uses it (ROM at $FFC0 0000): the
 * registers are at $10 steps; all but the first are bytes on the bus's
 * first byte lane (words read and written with the value in bits 31-24,
 * or the byte itself).
 *  $000  identification, a word: POST takes a memory controller whose upper
 *        halfword is $3001 for Platinum (platinum.h) and any other for
 *        Hammerhead ($FFF03104); bits 11-8 a bus clock code ($FFF03BA0),
 *        bits 15 and 6 select its clock table ($FFF03AB8)
 *  $020  the board's straps, read only: bit 4 selects a timing row
 *        ($FFF03FFC); bits 7-5 the family Open Firmware's root node tells
 *        apart: 4 the 7500 and 8500, 2 the 9500 (file $3404F8-$3405C7)
 *  $030  bits 7-5 the bus clock, read only, which POST takes with a 601 (0
 *        and 4 44 MHz, 1 and 5 33, 2 and 6 40, 3 and 7 50: the table at
 *        $FFF038AC) and bits 6-5 of which pick the timing row ($FFF03FD4);
 *        $050-$080 written from the row; $090 read and
 *        written back ($FFF04018)
 *  $0B0  bit 3 (bit 4 in IBM order): a wake from sleep rather than a cold
 *        start ($FFF031E8); $0C0 written $80 on that path ($FFF03300)
 *  $0E0  bit 7 the L2 cache present, bits 1-0 its size (0 512 KB, 1 256 KB,
 *        2 1 MB, 3 4 MB†) ($FFF03BC8); $0F0 cache control (POST writes $80,
 *        then $70 to $0E0, then 0 to $0F0)
 *  $1C0 + $20 n, $1D0 + $20 n, banks n = 0-25: the bank's base address in
 *        4 MB units, its bit 8 in bit 0 of the first register and bits 7-0
 *        in the second; bit 1 of the first a mode POST sets to size a bank
 *        whose first sizing is not a power of two, bit 2 two equal banks
 *        interleaved ($FFF0402C: bank n at n x 64 MB before sizing;
 *        $FFF04060: each bank sized within its 64 MB by writing at its top
 *        and finding the alias; then the banks packed from 0; $FFF04278).
 *
 * Livadix's model: the identification word, the cache's presence and size are
 * the board's (construction parameters); the other registers are kept as
 * written. A bank decodes from its base to the next higher base of a
 * populated bank, at most 64 MB, its DIMMs repeating through it (an access
 * past the installed size aliases, which is what the sizing finds); an
 * address no populated bank decodes floats. The board maps memory from
 * mc_hammerhead_windows and remaps when MC_HAMMERHEAD_LINE_MAP pulses (a
 * bank register was written). An even bank's bit 2 joins its equal-sized
 * partner into a window up to 128 MB; the partner's base is then ignored.
 * Backing storage uses alternating eight-byte DIMM lanes (the 128-bit
 * array's two 64-bit transfers), including while the banks are separate.
 *
 * Livadix's own code. */
#ifndef MC_HAMMERHEAD_H
#define MC_HAMMERHEAD_H

#include "hw.h"

#include <stdint.h>

enum { MC_HAMMERHEAD_LINE_MAP = 0 };

#define MC_HAMMERHEAD_SIZE 0x1000u
#define MC_HAMMERHEAD_BANKS 26u
#define MC_HAMMERHEAD_BANK_WINDOW 0x04000000u       /* 64 MB */

enum { MC_HAMMERHEAD_ID = 0x000, MC_HAMMERHEAD_STRAPS = 0x020, MC_HAMMERHEAD_WAKE = 0x0B0, MC_HAMMERHEAD_L2 = 0x0E0, MC_HAMMERHEAD_BANK = 0x1C0 };
enum { MC_HAMMERHEAD_L2_512K, MC_HAMMERHEAD_L2_256K, MC_HAMMERHEAD_L2_1M, MC_HAMMERHEAD_L2_4M, MC_HAMMERHEAD_L2_NONE };

/* A run of memory a bank answers: [base, base + size), DIMM bytes from
 * `offset` of canonical RAM repeating every `bank_size` bytes (a combined
 * window includes both equal DIMMs). */
typedef struct mc_mem_window {
    uint32_t base, size;
    uint32_t offset, bank_size;
    unsigned bank;
} mc_mem_window;

typedef struct mc_hammerhead {
    mc_dev dev;
    uint32_t id;                    /* the identification word */
    uint8_t l2;                     /* MC_HAMMERHEAD_L2_* */
    uint8_t reg[MC_HAMMERHEAD_SIZE / 16];
    uint32_t bank_size[MC_HAMMERHEAD_BANKS];    /* installed bytes, 0: empty */
    uint8_t straps;                 /* $020, read only: the board's straps */
    uint8_t bus_code;               /* $030 bits 7-5: the bus clock's code */
} mc_hammerhead;

extern const mc_dev_ops mc_hammerhead_ops;     /* offsets from $F800 0000 */

/* A controller identifying as `id`, its cache `l2`, the banks' installed
 * sizes (MC_HAMMERHEAD_BANKS entries, powers of two from 4 MB, or 0). */
void mc_hammerhead_init(mc_hammerhead *h, const mc_hw_host *host, const char *name, uint32_t id, unsigned l2, const uint32_t *bank_size);
void mc_hammerhead_reset(mc_hammerhead *h);
/* The board's straps: $020 (read only; bits 7-5 the family, bit 4 a
 * timing row) and $030 bits 7-5, the bus clock (see the header's notes). */
void mc_hammerhead_set_straps(mc_hammerhead *h, uint8_t straps, uint8_t bus_code);
/* The windows of `n` banks at `base` with `size` bytes installed (0:
 * empty): a populated bank answers from its base to the next higher base
 * of a populated bank, at most `limit` bytes (the lower-numbered of two at
 * one base); its RAM is the banks' sizes laid end to end in bank order.
 * Shared with Platinum. Returns the count, in address order. */
unsigned mc_mem_bank_windows(const uint32_t *base, const uint32_t *size, unsigned n, uint32_t limit, mc_mem_window *w, unsigned max);

/* Bank n's base address as its registers hold it. */
uint32_t mc_hammerhead_base(const mc_hammerhead *h, unsigned n);
/* Whether even bank n combines its equal-sized adjacent partner. */
int mc_hammerhead_interleaved(const mc_hammerhead *h, unsigned n);
/* The DIMM bank and canonical paired-lane RAM offset answering `address`;
 * 0 when it floats. Separate views of equal DIMMs retain this layout. */
int mc_hammerhead_decode(const mc_hammerhead *h, uint32_t address, unsigned *bank, uint32_t *offset);
/* The memory the banks answer, in address order; returns the count. */
unsigned mc_hammerhead_windows(const mc_hammerhead *h, mc_mem_window *w, unsigned max);

#endif
