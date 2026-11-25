/* Livadix - Heathrow IDE (heathrow_ide.h). */
#include "heathrow_ide.h"
static uint32_t read(void *p,uint32_t off,unsigned size){
    mc_heathrow_ide *h=p;mc_ata *a=&h->ata;
    if((off&~3u)==0x200){uint32_t v=h->timing>>(8u*(off&3u));return size==4?v:v&((1u<<(8u*size))-1u);}
    if(off>=0x80&&off!=0x160)return 0;
    unsigned reg=off==0x160?MC_ATA_ALT_STATUS:off>>4;
    if(!reg&&size>=2){uint32_t v=mc_ata_data_read16(a);if(size==4)v|=(uint32_t)mc_ata_data_read16(a)<<16;return v;}
    return mc_ata_reg_read(a,reg,size);
}
static void write(void *p,uint32_t off,unsigned size,uint32_t value){
    mc_heathrow_ide *h=p;mc_ata *a=&h->ata;
    if((off&~3u)==0x200){unsigned shift=8u*(off&3u);uint32_t mask=(size==4?0xFFFFFFFFu:(1u<<(8u*size))-1u)<<shift;h->timing=(h->timing&~mask)|((value<<shift)&mask);return;}
    if(off>=0x80&&off!=0x160)return;
    unsigned reg=off==0x160?MC_ATA_DEV_CONTROL:off>>4;
    if(!reg&&size>=2){mc_ata_data_write16(a,(uint16_t)value);if(size==4)mc_ata_data_write16(a,(uint16_t)(value>>16));}
    else mc_ata_reg_write(a,reg,size,value);
}
static void reset(void *p){mc_heathrow_ide *h=p;h->timing=0;mc_ata_reset(&h->ata);}
const mc_dev_ops mc_heathrow_ide_ops={read,write,NULL,reset,NULL};
void mc_heathrow_ide_init(mc_heathrow_ide *h,const mc_hw_host *host){h->timing=0;mc_ata_init(&h->ata,host,4);h->ata.lane_swap=0;}
