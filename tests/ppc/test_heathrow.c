/* Livadix - G3 I/O fixtures from Heathrow/IDE/SPD/Screamer/BMAC headers.
 * Interrupts, bus lanes, clocks and management protocols, independent of
 * the guest boot tests; no other emulator sources. */
#include "ppc/chips/heathrow.h"
#include "ppc/chips/heathrow_ide.h"
#include "ppc/chips/sdram_spd.h"
#include "ppc/chips/screamer.h"
#include "ppc/chips/bmac.h"
#include <stdio.h>
#include <string.h>
static unsigned checks,failures;static uint64_t time_now;
#define CHECK(x) do{++checks;if(!(x)){++failures;fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x);}}while(0)
static uint64_t now(void *p){(void)p;return time_now;}
static void schedule(void *p,void *d,uint64_t t){(void)p;(void)d;(void)t;}
static void line(void *p,void *d,int n,int s){(void)p;(void)d;(void)n;(void)s;}
static const mc_hw_host host={.now=now,.schedule=schedule,.line=line};
static void interrupts(void){
    mc_heathrow h;mc_heathrow_init(&h,&host,"test");const mc_dev_ops *o=&mc_heathrow_ops;
    CHECK(mc_pci_config_read(&h.cfg,0,4)==0x0010106Bu);
    mc_pci_config_write(&h.cfg,0x10,4,0xFFFFFFFFu);CHECK(mc_pci_config_read(&h.cfg,0x10,4)==0xFFF80000u);
    /* Jaguar enables sources without bit 31. VIA is edge-sensitive;
     * PCI source 23 remains asserted until the card lowers its line. */
    o->write(&h,0x24,4,0x00840001u);mc_heathrow_set_source(&h,18,1);CHECK(h.irq);
    CHECK(o->read(&h,0x20,4)==0x40000u);o->write(&h,0x28,4,0x40000u);CHECK(!h.irq);
    CHECK(o->read(&h,0x2C,4)==0x40000u);
    mc_heathrow_set_source(&h,18,0);CHECK(!h.irq);
    mc_heathrow_set_source(&h,23,1);o->write(&h,0x28,4,1u<<23);CHECK(h.irq);
    mc_heathrow_set_source(&h,23,0);CHECK(!h.irq);
    mc_heathrow_set_source(&h,0,1);mc_heathrow_set_source(&h,0,0);CHECK(o->read(&h,0x20,4)==1);
    o->write(&h,0x28,4,0x80000001u);CHECK(!h.irq);CHECK(o->read(&h,0x20,4)==0);
    o->write(&h,0x14,4,1);h.ch[2].host->interrupt(h.ch[2].host->ctx,2);
    CHECK(h.irq);CHECK(o->read(&h,0x10,4)==1);CHECK(o->read(&h,0x20,4)==0);
    o->write(&h,0x18,4,1);o->write(&h,0x28,4,0x80000000u);CHECK(!h.irq);
    h.ch[11].host->interrupt(h.ch[11].host->ctx,11);CHECK(o->read(&h,0x20,4)==4);
    o->write(&h,0x38,4,0x11223344);o->write(&h,0x39,1,0xAA);CHECK(o->read(&h,0x38,4)==0x1122AA44);
    mc_heathrow_reset(&h);CHECK(!h.irq);CHECK(!o->read(&h,0x20,4));CHECK(!o->read(&h,0x38,4));
    /* The classic ROM acknowledges bit 31 before its 68k handler can
     * service VIA, and expects a new CPU latch when VIA drops. */
    o->write(&h,0x24,4,0x80040000u);mc_heathrow_set_source(&h,18,1);CHECK(h.irq);
    o->write(&h,0x28,4,0x80000000u);CHECK(!h.irq);
    mc_heathrow_set_source(&h,18,0);CHECK(h.irq);
    o->write(&h,0x28,4,0x80000000u);CHECK(!h.irq);
}
static void ohare_timer(void) {
    mc_heathrow h;
    mc_ohare_init(&h, &host, "O'Hare");
    const mc_dev_ops *o = &mc_heathrow_ops;
    CHECK(mc_pci_config_read(&h.cfg, 0, 4) == 0x0007106Bu);
    CHECK(o->read(&h, 0x13100, 4) == 0);
    time_now = 500;
    o->write(&h, 0x13100, 4, 0x5Cu);
    CHECK(o->read(&h, 0x13100, 4) == 0x5Cu);
    CHECK(o->read(&h, 0x13101, 1) == 0);
    time_now += 782;
    CHECK(o->read(&h, 0x13100, 1) == 0x2Eu);
    time_now += 782;
    CHECK(o->read(&h, 0x13100, 1) == 0);
    o->write(&h, 0x13100, 1, 0x2Eu);
    CHECK(o->read(&h, 0x13100, 1) == 0x2Eu);
    mc_heathrow_reset(&h);
    CHECK(o->read(&h, 0x13100, 4) == 0);
    CHECK(mc_pci_config_read(&h.cfg, 0, 4) == 0x0007106Bu);
    h.board_id = 0x60;
    CHECK(o->read(&h, 0x34, 1) == 0x60);
    o->write(&h, 0x34, 1, 4);
    CHECK(o->read(&h, 0x34, 1) == 0x24);
    o->write(&h, 0x34, 1, 2);
    CHECK(o->read(&h, 0x34, 1) == 0x42);
    h.board_id = 0x10;
    o->write(&h, 0x34, 1, 1);
    CHECK(o->read(&h, 0x34, 1) == 1);
    o->write(&h, 0x34, 1, 0);
    CHECK(o->read(&h, 0x34, 1) == 0x10);
    /* Gazelle AIO sense $15E: GPIO6 stays released high at reset, as
     * the original ROM's warm POST diagnostic-switch test requires. */
    h.board_id = 0x50;
    o->write(&h, 0x34, 1, 0);CHECK(o->read(&h, 0x34, 1) == 0x50);
    o->write(&h, 0x34, 1, 4);CHECK(o->read(&h, 0x34, 1) == 0x14);
    o->write(&h, 0x34, 1, 2);CHECK(o->read(&h, 0x34, 1) == 0x52);
    o->write(&h, 0x34, 1, 1);CHECK(o->read(&h, 0x34, 1) == 0x41);
    h.ch[6].status = MC_DBDMA_RUN;
    h.ch[6].stopped = 1;
    o->write(&h, 0x860C, 4, 0x1230);
    CHECK(h.ch[6].command == 0x1230 && h.ch[6].status == MC_DBDMA_RUN);
    h.ch[6].status |= MC_DBDMA_ACTIVE;
    o->write(&h, 0x860C, 4, 0x4560);
    CHECK(h.ch[6].command == 0x1230);
    mc_heathrow_init(&h, &host, "Heathrow");
    h.ch[6].status = MC_DBDMA_RUN;
    o->write(&h, 0x860C, 4, 0x1230);
    CHECK(h.ch[6].command == 0); /* generic CHRP policy */
    time_now = 0;
}

static void spd(void){
    mc_sdram_spd s;uint32_t banks[8];uint8_t out[8],cmd[3]={0xA0,2,0xA1};unsigned n=sizeof out;
    CHECK(mc_sdram_spd_init(&s,64u<<20,banks));CHECK(banks[0]==64u<<20&&banks[1]==0);
    CHECK(mc_sdram_spd_transfer(&s,0x25,cmd,3,out,&n));CHECK(n==4&&out[0]==4&&out[1]==12&&out[2]==9&&out[3]==1);
    cmd[0]=0xA2;cmd[2]=0xA3;n=8;CHECK(!mc_sdram_spd_transfer(&s,0x25,cmd,3,out,&n));
    CHECK(mc_sdram_spd_init(&s,768u<<20,banks));for(unsigned i=0;i<6;++i)CHECK(banks[i]==128u<<20);
    for(unsigned i=0;i<3;++i){unsigned sum=0;for(unsigned k=0;k<63;++k)sum+=s.data[i][k];CHECK((sum&255)==s.data[i][63]);CHECK(s.data[i][5]==2&&s.data[i][3]==12&&s.data[i][4]==10&&s.data[i][13]==4);}
    CHECK(!mc_sdram_spd_init(&s,800u<<20,banks));CHECK(!mc_sdram_spd_init(&s,48u<<20,banks));
}
static void ide(void){
    mc_heathrow_ide h;mc_heathrow_ide_init(&h,&host);const mc_dev_ops *o=&mc_heathrow_ide_ops;
    mc_ata_attach_cd(&h.ata,0);CHECK(o->read(&h,0x40,1)==0x14);CHECK(o->read(&h,0x50,1)==0xEB);
    o->write(&h,0x200,4,0x526);CHECK(o->read(&h,0x200,4)==0x526);CHECK(o->read(&h,0x201,1)==5);
    o->write(&h,0x201,1,0xA1);CHECK(o->read(&h,0x200,4)==0xA126);
    o->write(&h,0x160,1,2);CHECK(h.ata.devctl==2);CHECK(o->read(&h,0x160,1)==0);
    o->reset(&h);CHECK(!o->read(&h,0x200,4));CHECK(o->read(&h,0x40,1)==0x14);
}
static void screamer(void){
    mc_screamer s;mc_screamer_init(&s,&host);CHECK(mc_screamer_status(&s)==0x403000u);
    mc_screamer_control(&s,0x6000);CHECK(mc_screamer_status(&s)==0x3000);
    time_now=1001;CHECK(mc_screamer_status(&s)==0x3000);time_now=1002;CHECK(mc_screamer_status(&s)==0x403000u);
}
static void bmac(void){
    mc_bmac b;const uint8_t mac[]={2,0,0,0,0,1};mc_bmac_init(&b,&host,mac);const mc_dev_ops *o=&mc_bmac_ops;
    /* 93C46 READ word12: start + 10 + six-bit address, then data. */
    unsigned command=0x18C;o->write(&b,0x190,2,0);
    for(int bit=8;bit>=0;--bit){unsigned v=1u|(((command>>bit)&1u)<<3);o->write(&b,0x190,2,v);o->write(&b,0x190,2,v|2u);}
    CHECK((o->read(&b,0x190,2)&4u)==0); /* READ dummy zero */
    unsigned value=0;for(unsigned i=0;i<16;++i){o->write(&b,0x190,2,1);o->write(&b,0x190,2,3);value=value<<1|((o->read(&b,0x190,2)>>2)&1u);}CHECK(value==0x8000);
    /* IEEE802.3 clause22: ST01, OP10, PHY0, reg2, Z0 turnaround. */
    unsigned frame=(6u<<10)|2u;o->write(&b,0x180,2,4);
    for(int bit=13;bit>=0;--bit){unsigned v=4u|(((frame>>bit)&1u)<<1);o->write(&b,0x180,2,v);o->write(&b,0x180,2,v|1u);}
    value=0;for(unsigned i=0;i<17;++i){o->write(&b,0x180,2,0);o->write(&b,0x180,2,1);if(!i)CHECK(!(o->read(&b,0x180,2)&8u));else value=value<<1|((o->read(&b,0x180,2)>>3)&1u);}CHECK(value==0x7810);
    uint8_t frame_data[60]={0},byte=0;
    CHECK(mc_bmac_receive(&b,frame_data,sizeof frame_data));
    unsigned received=0,status=0;int r;
    while((r=mc_bmac_dma_read(&b,&byte))==MC_DBDMA_BYTE){if(received>=64)status=status<<8|byte;++received;}
    CHECK(r==MC_DBDMA_END&&received==66&&status==64);
    CHECK(b.rx[60]==0x08&&b.rx[61]==0x89&&b.rx[62]==0x12&&b.rx[63]==0x04);
    CHECK(mc_bmac_dma_read(&b,&byte)==MC_DBDMA_NONE);
    uint8_t odd_frame[61]={0};CHECK(mc_bmac_receive(&b,odd_frame,sizeof odd_frame));
    received=status=0;
    while((r=mc_bmac_dma_read(&b,&byte))==MC_DBDMA_BYTE){if(received>=66)status=status<<8|byte;++received;}
    CHECK(r==MC_DBDMA_END&&received==68&&status==65&&b.rx[65]==0);

    o->write(&b,0x210,2,0xFEFF);CHECK(mc_bmac_dma_write(&b,0xAA));mc_bmac_dma_end(&b);CHECK(b.irq);CHECK(o->read(&b,0x200,2)&0x100);CHECK(!b.irq);
}
int main(void){interrupts();ohare_timer();spd();ide();screamer();bmac();printf("Heathrow: %u checks, %u failures\n",checks,failures);return failures!=0;}
