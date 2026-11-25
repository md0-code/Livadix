/* Livadix — the AMD Am79C940 MACE (Media Access Controller for Ethernet), the
 * Ethernet controller of the Quadra 840AV and Centris 660AV.
 *
 * Am79C940 MACE data sheet (AMD, 1992): "Register Summary" and "User
 * Accessible Registers": 32 byte registers, RCVFIFO 0 (read) and XMTFIFO 1
 * (write), XMTFC 2 (transmit frame control: DRTRY, DXMTFCS, APADXMT),
 * XMTFS 3 (transmit frame status: XMTSV bit 7 valid, UFLO, LCOL, MORE,
 * ONE, DEFER, LCAR, RTRY), XMTRC 4 (retry count), RCVFC 5 (receive frame
 * control: LLRCV, M/R, ASTRPRCV bit 0), RCVFS 6 (the four receive status
 * bytes: RCVCNT low; OFLO, CLSN, FRAM, FCS and RCVCNT bits 11-8; RNTPC;
 * RCVCC), FIFOFC 7, IR 8 (interrupt register, cleared by reading it: JAB,
 * BABL, CERR, RCVCCO, RNTPCO, MPCO, RCVINT bit 1, XMTINT bit 0), IMR 9 (the
 * same bits, 1 masks), PR 10 (poll: XMTSV, TDTREQ, RDTREQ), BIUCC 11
 * (SWRST bit 0, self-clearing), FIFOCC 12, MACCC 13 (PROM bit 7, ENXMT bit
 * 1, ENRCV bit 0, DRCVBC bit 2), PLSCC 14, PHYCC 15 (LNKFL bit 7), CHIPID
 * 16-17 ($x940, x the version), IAC 18 (ADDRCHG bit 7, PHYADDR bit 2,
 * LOGADDR bit 1: the next PADR or LADRF access starts at byte 0), LADRF 20
 * (eight bytes in turn), PADR 21 (six bytes in turn), MPC 24, RNTPC 26,
 * RCVCC 27 (counters), UTR 29 (LOOP bits 2-1: 00 none, 01 external, 10
 * internal, 11 internal through the MENDEC); "Receive Frame Status"
 * (RCVCNT counts the frame with its FCS; ASTRPRCV strips the pad and FCS
 * of an 802.3 frame whose length field is under 46).
 *
 * On the AV boards: the registers a
 * byte every $10 from $50F1 C000 (the built-in Ethernet driver at ROM
 * offsets $1E0D78-$1E2600 writes IMR at +$90, BIUCC +$B0, MACCC +$D0, IAC
 * +$120, LADRF +$140, PADR +$150, UTR +$1D0, reads CHIPID at +$100/+$110,
 * IR +$80, XMTFS +$30, XMTRC +$40); the frames move through the PSC's
 * ENetRd and ENetWr channels (psc.h), the interrupt on the PSC's level 3
 * bit 0; the address PROM at $50F0 8000, byte i at +$10i+1.
 *
 * This model: a frame the PSC hands over is sent (to the network, or back
 * to the receiver in a loopback mode) and raises XMTINT with XMTFS valid; a
 * frame received passes the address filter (PADR, broadcast unless DRCVBC,
 * any multicast once LADRF has a bit set†, everything under PROM) and goes
 * to the PSC with its status bytes, or waits while the PSC has no slot.
 * CHIPID reads $3940† (the version nibble 3). No collisions, no
 * programmed-I/O FIFO path. */
#ifndef MC_AM79C940_H
#define MC_AM79C940_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

/* TX_STATUS is asserted while XMTFS.XMTSV is valid, independently of IMR.
 * Grand Central connects this completion indication to transmit DBDMA s5. */
enum { MC_AM79C940_LINE_IRQ = 0, MC_AM79C940_LINE_TX_STATUS = 1 };

#define MC_AM79C940_SIZE 0x200u     /* 32 registers every $10 */

typedef struct mc_am79c940_port {
    void *ctx;
    /* A frame to the network (no FCS). */
    void (*transmit)(void *ctx, const uint8_t *frame, size_t length);
    /* A received frame (FCS included) and its status bytes to the DMA:
     * 1 when taken. */
    int (*receive)(void *ctx, const uint8_t *frame, size_t length, const uint8_t status[4]);
} mc_am79c940_port;

typedef struct mc_am79c940 {
    mc_dev dev;
    uint8_t reg[32];
    uint8_t padr[6], ladrf[8];
    unsigned padr_i, ladrf_i, rcvfs_i;
    uint8_t rcvfs[4];
    uint8_t irq, tx_status;
    mc_am79c940_port port;
} mc_am79c940;

void mc_am79c940_init(mc_am79c940 *m, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_am79c940_ops;
void mc_am79c940_set_port(mc_am79c940 *m, const mc_am79c940_port *port);
/* A frame from the DMA to send (no FCS). */
void mc_am79c940_transmit(mc_am79c940 *m, const uint8_t *frame, size_t length);
/* A frame from the network (no FCS): 1 when taken or dropped, 0 when it
 * must wait for a DMA slot. */
int mc_am79c940_receive(mc_am79c940 *m, const uint8_t *frame, size_t length);

#endif
