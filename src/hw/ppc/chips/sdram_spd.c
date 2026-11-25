/* Livadix - SDRAM SPD (sources in sdram_spd.h). */
#include "sdram_spd.h"
#include <string.h>
static void make_spd(uint8_t *p,unsigned rows,unsigned columns,unsigned ranks,
                     unsigned device_width,uint32_t rank) {
    p[0]=128;p[1]=8;p[2]=4;p[3]=(uint8_t)rows;
    p[4]=(uint8_t)columns;p[5]=(uint8_t)ranks;p[6]=64;p[8]=1;
    p[9]=0x75;p[10]=0x54;p[12]=0x80;p[13]=(uint8_t)device_width;p[15]=1;p[16]=0x8F;
    p[17]=4;p[18]=6;p[19]=p[20]=1;p[22]=0x0E;p[23]=0xA0;p[24]=0x60;
    p[27]=20;p[28]=20;p[29]=20;p[30]=50;p[31]=(uint8_t)(rank/(4u*1024u*1024u));
    p[62]=0x12;for(unsigned k=0;k<63;++k)p[63]=(uint8_t)(p[63]+p[k]);
}
int mc_sdram_spd_init(mc_sdram_spd *s,uint32_t bytes,uint32_t banks[8]) {
    memset(s,0,sizeof *s);memset(banks,0,8*sizeof *banks);
    for(unsigned slot=0;slot<3 && bytes;++slot){
        const uint32_t capacity=bytes>256u*1024u*1024u?256u*1024u*1024u:bytes;
        const unsigned ranks=capacity>128u*1024u*1024u?2u:1u;
        const uint32_t rank=capacity/ranks;
        if(rank!=32u*1024u*1024u&&rank!=64u*1024u*1024u&&rank!=128u*1024u*1024u)return 0;
        uint8_t *p=s->data[slot];s->present[slot]=1;
        /* G3 Developer Note Table 4-2: use supported 64-Mbit SDRAMs.
         * x16 / x8 / x4 chips with 12 rows and 8 / 9 / 10 columns give
         * 32 / 64 / 128 MiB ranks. Higher-density x16 chips are sized
         * down by the original firmware, hiding half the configured RAM. */
        const unsigned columns=rank==32u*1024u*1024u?8u:rank==64u*1024u*1024u?9u:10u;
        const unsigned width=rank==32u*1024u*1024u?16u:rank==64u*1024u*1024u?8u:4u;
        make_spd(p,12,columns,ranks,width,rank);
        banks[slot*2]=rank;if(ranks==2)banks[slot*2+1]=rank;
        bytes-=capacity;
    }
    return bytes==0;
}
int mc_sdram_spd_transfer(void *ctx,uint8_t cmd,const uint8_t *data,unsigned n,uint8_t *out,unsigned *length) {
    mc_sdram_spd *s=ctx;if(!n)return 0;
    const unsigned slot=(data[0]&0x0Eu)>>1;
    if((data[0]&0xF0u)!=0xA0u||slot>=3||!s->present[slot])return 0;
    if(cmd==0x25){
        if(n!=3||data[2]!=(uint8_t)(data[0]|1u)||*length<4)return 0;
        s->pointer[slot]=data[1];*length=4;
    }else if(cmd==0x22){
        if(data[0]&1u){if(*length<1)return 0;*length=1;}
        else {if(n<2)return 0;s->pointer[slot]=data[1];*length=0;return 1;}
    }else return 0;
    for(unsigned k=0;k<*length;++k)out[k]=s->data[slot][s->pointer[slot]++];
    return 1;
}

/* PowerBook G3 Series Developer Note, chapters 2 and 4, Table 4-10:
 * a 32 MiB rank uses 1M x16 x4 devices, a 64 MiB rank 2M x8 x4.
 * POST CBB01212 $FFF03850-$FFF03B30 assigns banks 0/1 to Heathrow #1
 * and 2/3 to #2. Both EEPROMs use $50 (wire bytes $A0/$A1). */
int mc_sdram_spd_portable_init(mc_sdram_spd *s,uint32_t bytes,uint32_t banks[8]) {
    const uint32_t unit=32u*1024u*1024u;
    uint32_t bottom,top;
    switch (bytes/unit) {
    case 1: bottom=1;top=0;break;
    case 2: bottom=2;top=0;break;
    case 3: bottom=2;top=1;break;
    case 4: bottom=2;top=2;break;
    case 5: bottom=1;top=4;break;
    case 6: bottom=2;top=4;break;
    default:return 0;
    }
    if (bytes%unit) return 0;
    memset(s,0,sizeof *s);
    memset(banks,0,8*sizeof *banks);
    for (unsigned slot=0;slot<2;slot++) {
        uint32_t size=slot?top:bottom;
        if (!size) continue;
        unsigned ranks=size==4?2u:1u;
        uint32_t rank=size/ranks;
        s->present[slot]=1;
        make_spd(s->data[slot],12,rank==1?8u:9u,ranks,rank==1?16u:8u,rank*unit);
        for (unsigned i=0;i<ranks;i++) banks[2*slot+i]=rank*unit;
    }
    return 1;
}

/* PowerBook G3 Series POST $FFF06FCC-$FFF074A4, Heathrow register $34:
 * bits 16/17 drive SCL/SDA low, bits 20/21 sample the open-drain bus.
 * I2C START, repeated START, STOP and ACK follow UM10204 section 3.1.
 * The separate board configuration EEPROM at $57 is absent here. */
enum { IIC_IDLE, IIC_RX, IIC_TX };

void mc_spd_gpio_init(mc_spd_gpio *p,const uint8_t *data,uint8_t board_sense) {
    memset(p,0,sizeof *p);
    p->data=data;
    p->board_sense=board_sense;
    p->scl=p->sda=1;
}

static void gpio_received(mc_spd_gpio *p) {
    if (p->stage==0) {
        p->ack=p->data && (p->shift&0xfeu)==0xa0u;
        p->next_mode=p->ack?(p->shift&1u?IIC_TX:IIC_RX):IIC_IDLE;
        p->stage=1;
    } else {
        /* Read-only SPD: only the first data byte sets the address. */
        p->ack=p->stage==1;
        if (p->ack) p->pointer=p->shift;
        p->stage=2;
        p->next_mode=p->ack?IIC_RX:IIC_IDLE;
    }
}

static void gpio_drive(mc_spd_gpio *p,uint32_t drive) {
    const uint8_t scl=(drive&0x10000u)==0, sda=(drive&0x20000u)==0;
    if (p->scl && scl && p->sda!=sda) {
        p->slave_low=p->count=p->shift=p->stage=0;
        p->mode=sda?IIC_IDLE:IIC_RX;
    } else if (!p->scl && scl && p->mode!=IIC_IDLE) {
        if (p->count<8 && p->mode==IIC_RX) p->shift=(uint8_t)((p->shift<<1)|sda);
        if (p->count==8 && p->mode==IIC_TX) p->ack=!sda;
        p->count++;
        if (p->count==8 && p->mode==IIC_RX) gpio_received(p);
    } else if (p->scl && !scl && p->mode!=IIC_IDLE) {
        if (p->count==8) {
            p->slave_low=p->mode==IIC_RX && p->ack;
        } else {
            if (p->count==9) {
                p->count=p->slave_low=p->shift=0;
                if (p->mode==IIC_TX) {
                    p->pointer++;
                    p->mode=p->ack?IIC_TX:IIC_IDLE;
                } else p->mode=p->next_mode;
            }
            if (p->mode==IIC_TX) p->slave_low=!(p->data[p->pointer]&(0x80u>>p->count));
        }
    }
    p->scl=scl;
    p->sda=sda;
    p->drive=drive;
}

static uint32_t gpio_read(void *dev,uint32_t off,unsigned size) {
    const mc_spd_gpio *p=dev;
    if (off+size>4) return 0;
    uint32_t value=(p->drive&~0xe0u)|p->board_sense|(p->scl?0x100000u:0u)|(p->sda&&!p->slave_low?0x200000u:0u);
    return (value>>(8u*off)) & (size==4?UINT32_MAX:(1u<<(8u*size))-1u);
}
static void gpio_write(void *dev,uint32_t off,unsigned size,uint32_t value) {
    mc_spd_gpio *p=dev;
    if (off+size>4) return;
    const unsigned shift=8u*off;
    const uint32_t mask=(size==4?UINT32_MAX:(1u<<(8u*size))-1u)<<shift;
    gpio_drive(p,((p->drive&~mask)|((value<<shift)&mask))&0xfffffu);
}
const mc_dev_ops mc_spd_gpio_ops={gpio_read,gpio_write,NULL,NULL,NULL};
