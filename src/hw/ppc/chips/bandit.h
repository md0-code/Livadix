/* Livadix — Bandit, the PowerPC-to-PCI host bridge of the first PCI Power
 * Macintosh generation (343S1126), and Chaos (343S1155), the Bandit-like
 * bridge from the processor bus to the 7500's, 8500's, 7600's and 8600's
 * video bus (VCI) that Control and Plan B sit on.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * chapter 2 "Bandit PCI Bridge IC" (buffering and address translation
 * between the processor bus and the PCI bus, byte order big-endian on the
 * processor bus and little-endian on PCI, the Bandit doing the swapping
 * and address transformations), "Video Bus" (Control and Chaos, a
 * separate bus bridge for video); Power Macintosh 9500 Developer Note
 * (1995), "Bandit Bus Bridge ICs" (two Bandits, one per PCI bus).
 * Designing PCI Cards and Drivers for Power Macintosh Computers (Apple,
 * 1995): chapter 1 "PCI Host Bridge Operation" (configuration cycles
 * through configuration address and data registers, mechanism #1 of the
 * PCI specification; a 23-bit I/O space through a memory-mapped section
 * of each bridge's control space; Table 1-4, a read nobody answers is a
 * machine check, a write is discarded), chapter 2 Table 2-1 (host bridge
 * n's control space at $F000 0000 + n x $200 0000, 32 MB; PCI memory in
 * $8000 0000-$EFFF FFFF and $F900 0000-$FEFF FFFF), chapter 3 Figure 3-1
 * (address invariance: the byte the processor writes at address n is on
 * PCI byte lane n). PCI Local Bus Specification 2.0, 3.7.4.1
 * (configuration mechanism #1, type 0 and type 1 addresses).
 *
 * As ROM 9630C68B uses it (ROM at $FFC0 0000): the
 * configuration address register at control space + $80 0000 ($F280 0000)
 * and the data register at + $C0 0000 ($F2C0 0000; byte n of the selected
 * register at + n), both little-endian through the lanes (POST
 * $FFF03688-$FFF0373C, stwbrx). Open Firmware's bandit FCode (ROM file
 * $3459D4): a type 0 address is 1 << device | register (bits 10-2,
 * function and register), the IDSEL line of device d (11-31) being bit d;
 * a type 1 address bus | device | function | register | 1; the bridge's
 * own function at device 11 ("pci106b,1": vendor $106B, device $0001);
 * register $48 the address mask (map-range: bits 0-15 open the 16 MB window
 * $F000 0000 + n x $100 0000; bits 16-30 open the 256 MB window
 * (n - 16) x $1000 0000 — bit 24 is $8000 0000);
 * register $50 bit 0 cleared for big-endian operation (the node's set-up);
 * the bridge's own memory window, control space + $100 0000 (16 MB:
 * $F300 0000, where POST places Grand Central). Chaos (chaos FCode, ROM
 * file $34608C): the same address, data and window arrangement at
 * $F000 0000; its map-range sets bit (address >> 28) + 16† of a register
 * at $10; its slots are devices 11 (Control), 13 (Plan B) and 14†.
 *
 * Livadix's model: the control space ops (mc_bandit_ops, offset from the
 * control space's base) answer the configuration registers, the I/O space
 * (no function on the boards decodes I/O: master abort) and the memory
 * window; mc_bandit_mem_read/_write serve the address-mask windows, which
 * the board routes to the bridge whose register $48 opens them. Values are
 * byte-swapped between the processor's numeric big-endian values and the
 * functions' little-endian ones. A cycle nobody claims: a read returns all
 * ones and sets the bridge's received-master-abort status bit (the machine
 * check is the board's: mc_bandit_mem_read returns 0); a write is dropped.
 * Bandit's registers $40-$7F other than $48 and $50 are kept as written†;
 * Chaos has no function of its own on its bus here†. The PCI masters'
 * path to memory (DBDMA) is the board's mc_hw_host dma, not modelled
 * through the bridge.
 *
 * Livadix's own code. */
#ifndef MC_BANDIT_H
#define MC_BANDIT_H

#include "hw.h"
#include "pci.h"

#include <stdint.h>

typedef enum { MC_BANDIT_BANDIT, MC_BANDIT_CHAOS } mc_bandit_kind;

#define MC_BANDIT_SPACE 0x02000000u     /* the control space */
#define MC_BANDIT_IO 0x00000000u        /* + I/O space, 8 MB (23 bits) */
#define MC_BANDIT_CONFIG_ADDRESS 0x00800000u
#define MC_BANDIT_CONFIG_DATA 0x00C00000u
#define MC_BANDIT_MEMORY 0x01000000u    /* + the memory window, 16 MB */
#define MC_BANDIT_SELF 11u              /* Bandit's own function's device number */
#define MC_BANDIT_DEVICES 32u

/* Bandit's own registers. */
enum { MC_BANDIT_ADDRESS_MASK = 0x48, MC_BANDIT_MODE = 0x50 };

typedef struct mc_bandit {
    mc_dev dev;
    uint8_t kind;               /* mc_bandit_kind */
    uint32_t base;              /* the control space ($F200 0000 for bridge 1) */
    mc_pci_config cfg;          /* Bandit's own function (device 11) */
    mc_pci_function fn[MC_BANDIT_DEVICES];
    uint32_t config_address;    /* as the bus sees it (little-endian value) */
    uint32_t aborts;            /* master aborts so far (diagnostics) */
} mc_bandit;

extern const mc_dev_ops mc_bandit_ops;

/* A bridge whose control space starts at `base` ($F000 0000, $F200 0000,
 * $F400 0000), reset, its bus empty. */
void mc_bandit_init(mc_bandit *b, const mc_hw_host *host, const char *name, mc_bandit_kind kind, uint32_t base);
void mc_bandit_reset(mc_bandit *b);
/* Puts a function at device number `device` (0-31; not 11 on a Bandit). */
int mc_bandit_attach(mc_bandit *b, unsigned device, const mc_pci_function *fn);
/* 1 when the bridge forwards memory address `address` to its bus: its own
 * window, or an address-mask window it has open. */
int mc_bandit_decodes(const mc_bandit *b, uint32_t address);
/* A processor memory access of 1, 2 or 4 bytes at `address` (big-endian
 * value) forwarded as a PCI memory cycle; 0 when nobody claims it. */
int mc_bandit_mem_read(mc_bandit *b, uint32_t address, unsigned size, uint32_t *value);
int mc_bandit_mem_write(mc_bandit *b, uint32_t address, unsigned size, uint32_t value);
/* A configuration cycle as the bridge makes it from its address register
 * (or a host's own call): device, function, register, little-endian value;
 * a read nobody claims returns all ones. */
uint32_t mc_bandit_config_read(mc_bandit *b, unsigned device, unsigned function, unsigned reg, unsigned size);
void mc_bandit_config_write(mc_bandit *b, unsigned device, unsigned function, unsigned reg, unsigned size, uint32_t value);

#endif
