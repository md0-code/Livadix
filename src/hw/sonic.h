/* Livadix — National DP83932 SONIC Ethernet controller.
 *
 * Written from the DP83932B datasheet in National's 1992 Local Area Network
 * Databook ("SONIC DS" below): 1.2.1 address recognition and the CAM, 1.3
 * byte ordering (BMODE = 1, big-endian), 1.7 loopback, 2.6 the FCS, 3.3
 * descriptor alignment, 3.4 receive buffer management (the RRA, RBA and
 * RDA, EOBC, end-of-packet processing and the overflow conditions of
 * 3.4.7), 3.5 transmit buffer management (the TDA and its fragments), 4.1
 * the CAM and the Load CAM command, 4.2 Tables 4-1 and 4-2 (register
 * numbers RA5-RA0), 4.3.1-4.3.13 (every register, its bits and its
 * hardware and software reset values, the tally counters of 4.3.11). The
 * Quadra 700 carries one at $5000A000 with its Ethernet address PROM at
 * $50008000 (Quadra 700 Developer Note, Figure 1-2); its interrupt reaches
 * the CPU through VIA2 PA0 and /SLOTIRQ at level 2 (Table 2-6 and "Direct
 * Slot interrupt handling"; level 3 only in A/UX's interrupt mapping, Table
 * 2-7).
 *
 * Model: every user register with its reset values; the command register's
 * software reset, timer, receiver enable/disable, transmit, halt, Read RRA
 * and Load CAM commands; the general purpose timer counting at half the 10
 * MHz transmit clock (4.3.12) with its TC interrupt. The descriptor areas
 * and buffers are read and written as bus master through the host's dma
 * accessor (hw.h), in 16- or 32-bit mode as DCR DW selects (3.3).
 *
 * The network: a frame transmitted goes to the port (mc_sonic_set_port)
 * whole, without its FCS; with a port the carrier is present (PTX), without
 * one every packet completes "no carrier sense" (NCRS, CRSL) and is lost.
 * A frame from the network (mc_sonic_receive) takes its time on the 10
 * Mbit/s wire (preamble, the frame with its FCS, the interframe gap) in a
 * queue of MC_SONIC_WIRE frames; at its end the SONIC filters its
 * destination (CAM entries enabled in CE, broadcast with BRD, all multicast
 * with AMC, all physical with PRO, RJCM inverting the CAM match) and
 * buffers it into the RBA with its FCS (3.4.6), or counts it missed (MPT)
 * when the receiver is disabled or has no descriptor or buffer. MAC, ENDEC
 * and transceiver loopback (RCR LB1-LB0) receive the station's own
 * transmissions (LBK). Runts, CRC and alignment errors and collisions never
 * occur on this network; HBL, RFO, FU and BR are never set.
 *
 * Register offsets: register n is at n << reg_shift (mc_sonic_init); on the
 * Quadra 700 the SONIC's RA5-RA0 sit on A7-A2 (32-bit spacing, the 16-bit
 * register in the low half of the long word: the ROM's long accesses at
 * $50F0A000 + 4 n). Any access size reads the register's 16 bits (a byte
 * read: odd offsets the low byte, even the high byte).
 */
#ifndef MC_SONIC_H
#define MC_SONIC_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_SONIC_LINE_IRQ = 0 };   /* INT: VIA2 PA0, level 2, on the Quadra 700 (Developer Note, Tables 2-6, 2-7) */

/* Register numbers, RA5-RA0 (SONIC DS Tables 4-1, 4-2). */
enum {
    MC_SONIC_CR = 0x00, MC_SONIC_DCR = 0x01, MC_SONIC_RCR = 0x02, MC_SONIC_TCR = 0x03,
    MC_SONIC_IMR = 0x04, MC_SONIC_ISR = 0x05, MC_SONIC_UTDA = 0x06, MC_SONIC_CTDA = 0x07,
    MC_SONIC_TPS = 0x08, MC_SONIC_TFC = 0x09, MC_SONIC_TSA0 = 0x0A, MC_SONIC_TSA1 = 0x0B,
    MC_SONIC_TFS = 0x0C, MC_SONIC_URDA = 0x0D, MC_SONIC_CRDA = 0x0E, MC_SONIC_CRBA0 = 0x0F,
    MC_SONIC_CRBA1 = 0x10, MC_SONIC_RBWC0 = 0x11, MC_SONIC_RBWC1 = 0x12, MC_SONIC_EOBC = 0x13,
    MC_SONIC_URRA = 0x14, MC_SONIC_RSA = 0x15, MC_SONIC_REA = 0x16, MC_SONIC_RRP = 0x17,
    MC_SONIC_RWP = 0x18, MC_SONIC_TRBA0 = 0x19, MC_SONIC_TRBA1 = 0x1A, MC_SONIC_TBWC0 = 0x1B,
    MC_SONIC_TBWC1 = 0x1C, MC_SONIC_ADDR0 = 0x1D, MC_SONIC_ADDR1 = 0x1E, MC_SONIC_LLFA = 0x1F,
    MC_SONIC_TTDA = 0x20, MC_SONIC_CEP = 0x21, MC_SONIC_CAP2 = 0x22, MC_SONIC_CAP1 = 0x23,
    MC_SONIC_CAP0 = 0x24, MC_SONIC_CE = 0x25, MC_SONIC_CDP = 0x26, MC_SONIC_CDC = 0x27,
    MC_SONIC_SR = 0x28, MC_SONIC_WT0 = 0x29, MC_SONIC_WT1 = 0x2A, MC_SONIC_RSC = 0x2B,
    MC_SONIC_CRCT = 0x2C, MC_SONIC_FAET = 0x2D, MC_SONIC_MPT = 0x2E, MC_SONIC_MDT = 0x2F,
    MC_SONIC_DCR2 = 0x3F
};

/* Command register bits (4.3.1). */
enum {
    MC_SONIC_CR_HTX = 0x0001, MC_SONIC_CR_TXP = 0x0002, MC_SONIC_CR_RXDIS = 0x0004, MC_SONIC_CR_RXEN = 0x0008,
    MC_SONIC_CR_STP = 0x0010, MC_SONIC_CR_ST = 0x0020, MC_SONIC_CR_RST = 0x0080, MC_SONIC_CR_RRRA = 0x0100,
    MC_SONIC_CR_LCAM = 0x0200
};
/* Data configuration: 32-bit data width (4.3.2). */
enum { MC_SONIC_DCR_DW = 0x0020 };
/* Receive control bits (4.3.3): filters, loopback, then the status of the
 * last packet. */
enum {
    MC_SONIC_RCR_PRX = 0x0001, MC_SONIC_RCR_LBK = 0x0002, MC_SONIC_RCR_FAER = 0x0004, MC_SONIC_RCR_CRCR = 0x0008,
    MC_SONIC_RCR_COL = 0x0010, MC_SONIC_RCR_CRS = 0x0020, MC_SONIC_RCR_LPKT = 0x0040, MC_SONIC_RCR_BC = 0x0080,
    MC_SONIC_RCR_MC = 0x0100, MC_SONIC_RCR_LB = 0x0600, MC_SONIC_RCR_AMC = 0x0800, MC_SONIC_RCR_PRO = 0x1000,
    MC_SONIC_RCR_BRD = 0x2000, MC_SONIC_RCR_RNT = 0x4000, MC_SONIC_RCR_ERR = 0x8000
};
/* Interrupt status/mask bits (4.3.5, 4.3.6). */
enum {
    MC_SONIC_INT_RFO = 0x0001, MC_SONIC_INT_MP = 0x0002, MC_SONIC_INT_FAE = 0x0004, MC_SONIC_INT_CRC = 0x0008,
    MC_SONIC_INT_RBAE = 0x0010, MC_SONIC_INT_RBE = 0x0020, MC_SONIC_INT_RDE = 0x0040, MC_SONIC_INT_TC = 0x0080,
    MC_SONIC_INT_TXER = 0x0100, MC_SONIC_INT_TXDN = 0x0200, MC_SONIC_INT_PKTRX = 0x0400, MC_SONIC_INT_PINT = 0x0800,
    MC_SONIC_INT_LCD = 0x1000, MC_SONIC_INT_HBL = 0x2000, MC_SONIC_INT_BR = 0x4000
};
/* Transmit control/status bits (4.3.4). */
enum {
    MC_SONIC_TCR_PTX = 0x0001, MC_SONIC_TCR_BCM = 0x0002, MC_SONIC_TCR_PMB = 0x0008, MC_SONIC_TCR_CRSL = 0x0080,
    MC_SONIC_TCR_NCRS = 0x0100, MC_SONIC_TCR_CRCI = 0x2000, MC_SONIC_TCR_PINTR = 0x8000
};
/* DCR2: reject on CAM match (4.3.7). */
enum { MC_SONIC_DCR2_RJCM = 0x0001 };

/* The Silicon Revision register's value (4.3.13: revisions count up from 0
 * with each silicon revision). The DP83932B's number is not in the
 * datasheet; 4 is assumed† (no driver observed reads it). */
#define MC_SONIC_REVISION 0x0004u

/* Frames waiting on the wire, and the longest frame (1514 bytes without
 * the FCS, IEEE 802.3, SONIC DS 2.0). */
#define MC_SONIC_WIRE 16
#define MC_SONIC_FRAME_MAX 1514

/* The network beside the SONIC: every frame it transmits (destination
 * address to the end of the data, no FCS). */
typedef struct mc_sonic_port {
    void *ctx;
    void (*transmit)(void *ctx, const uint8_t *frame, size_t length);
} mc_sonic_port;

typedef struct mc_sonic_frame {
    uint16_t length;            /* with the FCS */
    uint8_t loopback;
    uint8_t data[MC_SONIC_FRAME_MAX + 4];
} mc_sonic_frame;

typedef struct mc_sonic {
    mc_dev dev;
    uint16_t reg[64];
    uint16_t cam[16][3];        /* CAP0, CAP1, CAP2 of each entry (4.1) */
    unsigned reg_shift;
    int irq;
    /* General purpose timer (4.3.12): value at `timer_base` while running. */
    int timer_running;
    uint32_t timer_value;
    uint64_t timer_base;
    uint32_t timer_rem;
    /* Receive state: the last link field read had EOL (CRDA then holds
     * that field's address, 3.4.6.1); no buffer (the last RBA used up with
     * RBE set, 3.4.7). */
    int rx_eol, rx_no_buffer;
    /* The wire: frames in arrival order; the head's last bit arrives at
     * wire_end (guest time). */
    mc_sonic_frame wire[MC_SONIC_WIRE];
    unsigned wire_head, wire_count;
    uint64_t wire_end;
    mc_sonic_port port;
    uint8_t tx[MC_SONIC_FRAME_MAX + 4];
} mc_sonic;

/* A SONIC after a hardware reset; registers at n << reg_shift (2 on the
 * Quadra 700). */
void mc_sonic_init(mc_sonic *s, const mc_hw_host *host, unsigned reg_shift);
extern const mc_dev_ops mc_sonic_ops;
/* The network (NULL: none, no carrier). */
void mc_sonic_set_port(mc_sonic *s, const mc_sonic_port *port);
/* A frame arrives from the network: destination address to the end of the
 * data, without the FCS (the SONIC appends it); shorter than 60 bytes it is
 * padded. 0 when the wire's queue is full (offer it again later). */
int mc_sonic_receive(mc_sonic *s, const uint8_t *frame, size_t length);
/* A register's current value, by number. */
uint16_t mc_sonic_reg(mc_sonic *s, unsigned reg);
/* The IEEE 802.3 CRC-32 (SONIC DS 2.6) of a frame, as transmitted: the FCS
 * bytes are its low byte first. */
uint32_t mc_sonic_crc32(const uint8_t *data, size_t length);

/* The Ethernet address PROM beside the SONIC (Quadra 700: $50008000).
 * Byte i is at offset i << shift; bytes 0-5 hold the station address, each
 * byte with its bits reversed when `bit_reversed`; byte 6 is 0† and byte 7
 * makes the eight bytes XOR to FFh; zeros after. Observed from the Quadra
 * 700 ROM's Ethernet driver: it reads the eight bytes
 * at consecutive addresses (shift 0), refuses the PROM unless they XOR to
 * FFh, and reverses the bits of bytes 0-5 to make the address. */
typedef struct mc_sonic_prom {
    mc_dev dev;
    uint8_t bytes[16];
    unsigned shift;
} mc_sonic_prom;

/* The default station address, 08:00:07:4D:41:43: in Apple's block
 * (08:00:07), as a real Quadra's; the Ethernet drivers of Mac OS refuse an
 * address that is not Apple's (08:00:07 or 00:05:02; observed). It only ever meets the NAT's virtual network. */
extern const uint8_t mc_sonic_default_address[6];
void mc_sonic_prom_init(mc_sonic_prom *p, const mc_hw_host *host, const uint8_t address[6], unsigned shift,
                        int bit_reversed);
extern const mc_dev_ops mc_sonic_prom_ops;

#endif
