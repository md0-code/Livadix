/* Livadix - BMAC (register/pin evidence in bmac.h). */
#include "bmac.h"
#include "dbdma.h"
#include <string.h>
static void update(mc_bmac *b){int irq=(b->reg[0x20]&~b->reg[0x21])!=0;if(irq!=b->irq){b->irq=irq;mc_dev_line(&b->dev,MC_BMAC_LINE_INT,irq);}}
void mc_bmac_reset(mc_bmac *b){memset(b->reg,0,sizeof b->reg);b->reg[0x21]=0xFFFF;b->reg[0x17]=0xB0;b->tx_n=b->rx_n=b->rx_i=0;b->serial_bits=b->serial_out=b->management_bits=b->management_out=0;b->srom=b->mii=0;update(b);}
static uint8_t reverse(uint8_t x){uint8_t v=0;for(unsigned k=0;k<8;++k){v=(uint8_t)((v<<1)|(x&1u));x>>=1;}return v;}
void mc_bmac_init(mc_bmac *b,const mc_hw_host *host,const uint8_t mac[6]){
 memset(b,0,sizeof *b);b->dev.ops=&mc_bmac_ops;b->dev.host=host;b->dev.name="BMAC";
 for(unsigned k=0;k<3;++k)b->eeprom[10+k]=(uint16_t)(reverse(mac[k*2+1])<<8|reverse(mac[k*2]));
 b->phy[0]=0x1000;b->phy[1]=0x7809;b->phy[2]=0x7810;b->phy[4]=0x01E1;mc_bmac_reset(b);
}
/* Microwire READ: CS=1, CLK=2, DI=8, DO=4, one start/op/address
 * command (110aaaaaa), then sixteen data clocks, as OF ewrite-addr. */
static void serial_write(mc_bmac *b,uint16_t v){
 if(!(v&1u)){b->serial_bits=b->serial_out=0;b->serial=0;}
 else if(!(b->srom&2u)&&(v&2u)){
  if(b->serial_bits<9){b->serial=(b->serial<<1)|((v>>3)&1u);++b->serial_bits;}
  else ++b->serial_out;
 }
 b->srom=v;
}
static uint16_t serial_read(const mc_bmac *b){
 const unsigned index=b->serial_out?b->serial_out-1:0;
 const uint16_t value=b->eeprom[b->serial&63u];
 /* 93C46 READ emits a dummy zero after the address, then MSB first.
  * Apple BMacEnet 9.0.4 checks that dummy bit before clocking data
  * (guest driver 003B0510-003B0584); a one means no EEPROM. */
 const unsigned bit=b->serial_bits>=9?(!b->serial_out?0u:index<16?(value>>(15-index))&1u:1u):1u;
 return (uint16_t)((b->srom&~4u)|(bit<<2));
}
static void mii_write(mc_bmac *b,uint16_t v){
 if(!(b->mii&1u)&&(v&1u)){
  if(v&4u){
   const unsigned bit=(v>>1)&1u;
   if(!b->management_bits&&bit){}else{b->management=(b->management<<1)|bit;++b->management_bits;}
   if(b->management_bits==14&&((b->management>>10)&15u)==6u){
    unsigned reg=b->management&31u;unsigned phy=(b->management>>5)&31u;
    b->management_value=phy?0xFFFF:b->phy[reg];
    if(reg==1&&b->link)b->management_value|=0x24;
    b->management_out=0;
   }
   if(b->management_bits==32){
    unsigned reg=(b->management>>18)&31u;unsigned phy=(b->management>>23)&31u;
    if(!phy&&((b->management>>28)&15u)==5u)b->phy[reg]=(uint16_t)b->management&(reg?0xFFFFu:0x7FFFu);
    b->management_bits=0;b->management=0;
   }
  }else ++b->management_out;
 }
 if((v&4u)&&!(b->mii&4u)){b->management_bits=0;b->management=0;}
 b->mii=v;
}
static uint16_t mii_read(const mc_bmac *b){
 unsigned bit=1;
 if(b->management_bits==14){/* The released MDIO pin has its Z interval on the falling edge
   * after REGAD; the next read clock samples TA=0, then 16 data bits
   * (IEEE802.3 clause22, Apple BMacEnetMII turnaround sequence). */
  if(b->management_out==1)bit=0;else if(b->management_out>=2&&b->management_out<18)bit=(b->management_value>>(17-b->management_out))&1u;}
 return (uint16_t)((b->mii&~8u)|(bit<<3));
}
static uint32_t read_reg(void *p,uint32_t off,unsigned size){
 mc_bmac *b=p;off&=0xFFFu;uint32_t v;
 if((off&~15u)==0x190)v=serial_read(b);
 else if((off&~15u)==0x180)v=mii_read(b);
 else{v=b->reg[off>>4];if((off&~15u)==0x200){b->reg[0x20]=0;update(b);}}
 v>>=8u*(off&3u);return size==4?v:v&((1u<<(8u*size))-1u);
}
static void write_reg(void *p,uint32_t off,unsigned size,uint32_t value){
 mc_bmac *b=p;off&=0xFFFu;(void)size;uint16_t v=(uint16_t)value;
 if((off&~15u)==0x190){serial_write(b,v);return;}
 if((off&~15u)==0x180){mii_write(b,v);return;}
 if((off&~15u)==0x420){b->tx_n=0;b->reg[0x42]=0;return;}
 if((off&~15u)==0x620){b->rx_i=b->rx_n=0;b->reg[0x62]=0;return;}
 b->reg[off>>4]=v;update(b);
}
int mc_bmac_dma_write(void *p,uint8_t byte){mc_bmac *b=p;if(!b->tx_n)mc_dev_line(&b->dev,MC_BMAC_LINE_TX,0);if(b->tx_n<sizeof b->tx)b->tx[b->tx_n++]=byte;return 1;}
void mc_bmac_dma_end(void *p){mc_bmac *b=p;if(b->tx_n&&b->transmit)b->transmit(b->port,b->tx,b->tx_n);b->tx_n=0;b->reg[0x20]|=0x100;update(b);mc_dev_line(&b->dev,MC_BMAC_LINE_TX,1);}
int mc_bmac_dma_read(void *p,uint8_t *byte){mc_bmac *b=p;if(!b->rx_n)return MC_DBDMA_NONE;if(b->rx_i==b->rx_n){b->rx_n=b->rx_i=0;return MC_DBDMA_END;}*byte=b->rx[b->rx_i++];return MC_DBDMA_BYTE;}
int mc_bmac_receive(mc_bmac *b,const uint8_t *frame,size_t n){if(n>1600||b->rx_n)return 0;memcpy(b->rx,frame,n);
 /* IEEE802.3 minimum frame is 64 bytes including its four-byte FCS.
  * Host ports supply frames without FCS or minimum-frame padding. */
 if(n<60){memset(b->rx+n,0,60-n);n=60;}
 /* BMAC delivers the Ethernet FCS and then a BE 16-bit length/status.
  * Mac OS 9 BMacEnet receive: 003B6E14 requires >=64 bytes;
  * 003B70DC locates the two-byte trailer, 003B7104 masks length.
  * IEEE802.3 CRC-32 is sent least-significant byte first. */
 uint32_t crc=0xFFFFFFFFu;
 for(size_t k=0;k<n;++k){crc^=b->rx[k];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1u)?0xEDB88320u:0);}
 crc=~crc;for(unsigned k=0;k<4;++k)b->rx[n+k]=(uint8_t)(crc>>(8*k));
 n+=4;
 /* The receive status occupies its own 16-bit FIFO word. An odd
  * frame/FCS is padded before that word; the length excludes this pad.
  * The native driver reads the trailer with lhz (003B70F0). */
 size_t end=(n+1u)&~(size_t)1u;if(end!=n)b->rx[n]=0;
 b->rx[end]=(uint8_t)(n>>8);b->rx[end+1]=(uint8_t)n;b->rx_n=(unsigned)end+2;b->rx_i=0;mc_dev_line(&b->dev,MC_BMAC_LINE_RX,1);mc_dev_line(&b->dev,MC_BMAC_LINE_RX,0);return 1;}
static void reset_op(void *p){mc_bmac_reset(p);}
const mc_dev_ops mc_bmac_ops={read_reg,write_reg,NULL,reset_op,NULL};
