/* Livadix — National DP8390 NIC (Network Interface Controller).
 *
 * Written from the DP8390D/NS32490D NIC datasheet in National's Local Area
 * Network Databook ("NIC DS" below): 7.0 "Buffer Management" (the receive
 * buffer ring of 256-byte pages between PSTART and PSTOP, CURR and BNRY,
 * the four-byte receive header: receive status, next packet pointer,
 * receive byte count low then high; buffer overflow, OVW), 8.0 "Packet
 * Reception" (address recognition: the physical address PAR0-5, broadcast
 * with AB, multicast through the 64-bit hash of MAR0-7 with AM, every
 * physical address with PRO; the hash is the upper six bits of the CRC
 * over the destination address), 9.0 "Packet Transmission" (TPSR, TBCR,
 * CR TXP, TSR), 10.0 "Remote DMA", 11.0 "Internal Registers" (the register
 * map of pages 0, 1 and 2, CR, ISR, IMR, DCR, TCR, TSR, RCR, RSR, the
 * tally counters CNTR0-2) and 13.0 "Loopback Diagnostics".
 *
 * Model: the registers of pages 0-2; STP and STA (stopping sets ISR RST at
 * once, nothing being in flight on this network); transmit of TBCR bytes
 * from page TPSR of the buffer memory (to the network, or nowhere in a
 * loopback mode of TCR LB1-0) taking its wire time, then TSR PTX and ISR
 * PTX; a frame from the network takes its wire time in a queue of
 * MC_DP8390_WIRE frames, passes the address filter (RCR SEP and AR accept
 * nothing more, there being no errored or runt frames) and goes into the
 * ring with its FCS behind a header (status, next page, count low, count
 * high) as the local DMA stores it, then ISR PRX; with no room in the ring
 * the frame is dropped with ISR OVW and MPA counted (CNTR2); in monitor
 * mode (RCR MON) frames are checked and counted only. Remote DMA moves
 * bytes between the buffer memory and the remote DMA port (the data port
 * at register $10 on boards that wire one) and raises RDC. No collisions,
 * no CRC or frame alignment errors.
 *
 * The buffer memory is the board's (`mem`, `mem_size` bytes from local
 * address 0): the local DMA writes and reads it in byte order, the order
 * the NIC DS gives for the receive header (status first); the board wires
 * the bytes so the host sees that order (the NB card).
 */
#ifndef MC_DP8390_H
#define MC_DP8390_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_DP8390_LINE_IRQ = 0 };   /* INT, active high */

/* Command register (11.0 "Command Register"). */
enum {
    MC_DP8390_CR_STP = 0x01, MC_DP8390_CR_STA = 0x02, MC_DP8390_CR_TXP = 0x04, MC_DP8390_CR_RD = 0x38,
    MC_DP8390_CR_PS = 0xC0
};
/* Interrupt status and mask (ISR, IMR). */
enum {
    MC_DP8390_ISR_PRX = 0x01, MC_DP8390_ISR_PTX = 0x02, MC_DP8390_ISR_RXE = 0x04, MC_DP8390_ISR_TXE = 0x08,
    MC_DP8390_ISR_OVW = 0x10, MC_DP8390_ISR_CNT = 0x20, MC_DP8390_ISR_RDC = 0x40, MC_DP8390_ISR_RST = 0x80
};
/* Receive configuration (RCR) and status (RSR). */
enum {
    MC_DP8390_RCR_SEP = 0x01, MC_DP8390_RCR_AR = 0x02, MC_DP8390_RCR_AB = 0x04, MC_DP8390_RCR_AM = 0x08,
    MC_DP8390_RCR_PRO = 0x10, MC_DP8390_RCR_MON = 0x20
};
enum { MC_DP8390_RSR_PRX = 0x01, MC_DP8390_RSR_MPA = 0x10, MC_DP8390_RSR_PHY = 0x20, MC_DP8390_RSR_DIS = 0x40 };
/* Transmit configuration (TCR) and status (TSR). */
enum { MC_DP8390_TCR_LB = 0x06 };
enum { MC_DP8390_TSR_PTX = 0x01 };

#define MC_DP8390_WIRE 16
#define MC_DP8390_FRAME_MAX 1514

typedef struct mc_dp8390_port {
    void *ctx;
    void (*transmit)(void *ctx, const uint8_t *frame, size_t length);
} mc_dp8390_port;

typedef struct mc_dp8390_frame {
    uint16_t length;            /* with the FCS */
    uint8_t data[MC_DP8390_FRAME_MAX + 4];
} mc_dp8390_frame;

typedef struct mc_dp8390 {
    mc_dev dev;
    uint8_t cr, isr, imr, dcr, tcr, rcr, tsr, rsr, ncr;
    uint8_t pstart, pstop, bnry, tpsr, curr;
    uint16_t tbcr, rsar, rbcr, crda, clda;
    uint8_t par[6], mar[8];
    uint8_t cntr[3];
    int irq;
    uint8_t *mem;
    uint32_t mem_size;
    /* Transmission in progress: its end (guest time), MC_NEVER when none. */
    uint64_t tx_end;
    mc_dp8390_frame wire[MC_DP8390_WIRE];
    unsigned wire_head, wire_count;
    uint64_t wire_end;
    mc_dp8390_port port;
    uint8_t tx[MC_DP8390_FRAME_MAX + 4];
} mc_dp8390;

/* A NIC after a hardware reset, with the board's buffer memory. */
void mc_dp8390_init(mc_dp8390 *n, const mc_hw_host *host, uint8_t *mem, uint32_t mem_size);
extern const mc_dev_ops mc_dp8390_ops;
/* Register r (0-15) of the page CR selects; r 16 is the remote DMA port. */
uint8_t mc_dp8390_read(mc_dp8390 *n, unsigned r);
void mc_dp8390_write(mc_dp8390 *n, unsigned r, uint8_t v);
void mc_dp8390_set_port(mc_dp8390 *n, const mc_dp8390_port *port);
/* A frame from the network (destination address to the end of the data,
 * no FCS). 0 when the wire's queue is full (offer it again later). */
int mc_dp8390_receive(mc_dp8390 *n, const uint8_t *frame, size_t length);
/* The multicast hash bit (0-63) of a destination address (NIC DS 8.0). */
unsigned mc_dp8390_hash(const uint8_t address[6]);

#endif
