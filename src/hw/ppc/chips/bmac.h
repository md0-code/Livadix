/* Livadix - Heathrow BMAC Ethernet cell.
 * Apple G3 Developer Note (1998), p.45; ROM78F57389 Open Firmware BMAC
 * FCode file33B18E-33C16A: register offsets, FIFO/MAC reset, interrupt
 * mask, MII clock/OE/data pins, 93C46 serial-ROM pins and READ sequence.
 * Its lxt970? word at33BA3F checks PHY register2=7810; management frames
 * use IEEE802.3 Clause22. External cable/link follows the host port.
 * Apple BMacEnet hardware reference: BMacEnetRegisters.h and
 * BMacEnetPrivate.cpp (_receivePackets), apple-oss-distributions /
 * AppleBMacEthernet. Register/packet-format facts only, no driver copied.
 * Livadix's own code; no emulator source used. */
#ifndef MC_BMAC_H
#define MC_BMAC_H
#include "hw.h"
#define MC_BMAC_SIZE 0x1000u
enum{MC_BMAC_LINE_INT,MC_BMAC_LINE_RX,MC_BMAC_LINE_TX};
typedef struct mc_bmac {
 mc_dev dev; uint16_t reg[256],eeprom[64],phy[32];
 uint16_t srom;uint32_t serial;unsigned serial_bits,serial_out;
 uint16_t mii;uint32_t management;unsigned management_bits,management_out;uint16_t management_value;
 uint8_t tx[1600],rx[1608];unsigned tx_n,rx_n,rx_i;int irq,link;
 void *port;void(*transmit)(void *,const uint8_t *,size_t);
} mc_bmac;
extern const mc_dev_ops mc_bmac_ops;
void mc_bmac_init(mc_bmac *,const mc_hw_host *,const uint8_t mac[6]);
void mc_bmac_reset(mc_bmac *);
int mc_bmac_dma_write(void *,uint8_t);
int mc_bmac_dma_read(void *,uint8_t *);
void mc_bmac_dma_end(void *);
int mc_bmac_receive(mc_bmac *,const uint8_t *,size_t);
#endif
