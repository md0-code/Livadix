/* OHCI and HID protocols: see ohci.h. */
#include "ohci.h"
#include <string.h>

enum {
    REV=0x00, CONTROL=0x04, COMMAND=0x08, STATUS=0x0c,
    ENABLE=0x10, DISABLE=0x14, HCCA=0x18, PERIOD=0x1c,
    CONTROL_HEAD=0x20, CONTROL_CURRENT=0x24, BULK_HEAD=0x28,
    BULK_CURRENT=0x2c, DONE=0x30, INTERVAL=0x34, REMAINING=0x38,
    NUMBER=0x3c, PERIOD_START=0x40, LS_THRESHOLD=0x44,
    RH_A=0x48, RH_B=0x4c, RH_STATUS=0x50, PORT=0x54
};
/* OpenHCI 1.0a 7.1-7.4: register fields. */
enum { SO=1, WDH=2, SF=4, UE=16, FNO=32, RHSC=64 };
#define R(u,off) ((u)->reg[(off)/4u])
#define OPERATIONAL 0x80u
#define POINTER_MASK 0xfffffff0u
#define MIE 0x80000000u

/* HID 1.11 6.2.2: Mouse application, Pointer physical collection,
 * one button, in-range status and two 16-bit absolute axes, logical range 0..32767. */
static const uint8_t pointer_descriptor[] = {
    0x05,0x01, 0x09,0x02, 0xa1,0x01, 0x09,0x01, 0xa1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x01, 0x15,0x00, 0x25,0x01,
    0x75,0x01, 0x95,0x01, 0x81,0x02,
    0x05,0x0d, 0x09,0x32, 0x81,0x02,
    0x75,0x06, 0x95,0x01, 0x81,0x03,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x15,0x00, 0x26,0xff,0x7f,
    0x75,0x10, 0x95,0x02, 0x81,0x02,
    0x75,0x08, 0x95,0x03, 0x81,0x03, 0xc0,0xc0
};
/* USB 1.1 9.6, HID 1.11 6.2.1 and appendix B. A boot-compatible
 * mouse selects Jaguar's legacy cursor personality. Report protocol
 * carries absolute coordinates; boot protocol has its separate three-byte
 * relative format, never absolute coordinates interpreted as deltas. */
static const uint8_t device_descriptor[] = {
    18,1, 0x10,0x01, 0,0,0,8, 0x43,0x4d, 2,0, 0,1, 1,2,0,1
};
static const uint8_t configuration_descriptor[] = {
    9,2,34,0,1,1,0,0x80,25,
    9,4,0,0,1,3,1,2,0,
    9,0x21,0x11,1,0,1,0x22,(uint8_t)sizeof pointer_descriptor,0,
    7,5,0x81,3,8,0,8
};

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static unsigned le16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }
static void put32(uint8_t *p,uint32_t v) {
    for(unsigned i=0;i<4;i++) p[i]=(uint8_t)(v>>(i*8));
}
static void irq(mc_ohci *u) {
    mc_dev_line(&u->dev,0,(R(u,ENABLE)&MIE) && (R(u,STATUS)&R(u,ENABLE)&0x7fffffffu));
}
static int words(mc_ohci *u,uint32_t address,uint32_t *out,unsigned count) {
    uint8_t bytes[16];
    if(count>4 || !mc_dev_dma(&u->dev,address,bytes,count*4,0)) { R(u,STATUS)|=UE; return 0; }
    for(unsigned i=0;i<count;i++) out[i]=le32(bytes+i*4);
    return 1;
}
static int store(mc_ohci *u,uint32_t address,uint32_t value) {
    uint8_t bytes[4];put32(bytes,value);
    if(!mc_dev_dma(&u->dev,address,bytes,4,1)) { R(u,STATUS)|=UE; return 0; }
    return 1;
}
static void schedule(mc_ohci *u) {
    uint64_t next=u->next_frame;
    if(u->reset_due<next) next=u->reset_due;
    if(u->resume_due<next)next=u->resume_due;
    mc_dev_schedule(&u->dev,next);
}
static void start_frame(mc_ohci *u,uint64_t now) {
    u->frame_fraction+=MC_C15M_HZ;
    u->next_frame=now+u->frame_fraction/1000u;
    u->frame_fraction%=1000u;
}
static void device_reset(mc_ohci *u) {
    u->address=u->configuration=u->idle=u->halted=u->pending=0;
    u->protocol=1;u->response_size=u->response_offset=u->response_limit=0;
    u->control_stall=u->interrupt_polled=0;u->report_pending=1;
    u->report_read=u->report_count=0;
}
static void reset(void *p) {
    mc_ohci *u=p;
    memset(u->reg,0,sizeof u->reg);
    R(u,REV)=0x10; R(u,INTERVAL)=0x2edfu; R(u,LS_THRESHOLD)=0x628u;
    /* One always-powered port with a fixed pointer, no over-current
     * input. 7.4.1/7.4.4: a non-removable device sets CSC after reset. */
    R(u,RH_A)=0x02001201u;R(u,RH_B)=0x00020002u;R(u,PORT)=0x10101u;R(u,STATUS)=RHSC;
    u->next_frame=u->reset_due=u->resume_due=MC_NEVER;u->frame_fraction=0;u->done_delay=7;
    device_reset(u);schedule(u);irq(u);
}
void mc_ohci_reset(mc_ohci *u) { reset(u); }
/* 7.1.3: HCR suspends the host without resetting the root hub or its
 * downstream device. Address, configuration and a pending port reset stay. */
static void software_reset(mc_ohci *u) {
    const uint32_t root_a=R(u,RH_A),root_b=R(u,RH_B),root_status=R(u,RH_STATUS),port=R(u,PORT);
    const uint32_t routing=R(u,CONTROL)&0x100u;
    memset(u->reg,0,sizeof u->reg);
    R(u,REV)=0x10;R(u,INTERVAL)=0x2edfu;R(u,LS_THRESHOLD)=0x628u;
    R(u,CONTROL)=routing|0xc0u;R(u,RH_A)=root_a;R(u,RH_B)=root_b;
    R(u,RH_STATUS)=root_status;R(u,PORT)=port;
    u->next_frame=MC_NEVER;u->frame_fraction=0;u->done_delay=7;
    schedule(u);irq(u);
}
static void response(mc_ohci *u,const uint8_t *data,unsigned count) {
    if(count>sizeof u->response) count=sizeof u->response;
    memcpy(u->response,data,count);u->response_size=count;
}
static int setup(mc_ohci *u,const uint8_t *data,unsigned length) {
    if(length!=8) return -2;
    const unsigned type=data[0],request=data[1],value=le16(data+2),index=le16(data+4);
    u->response_size=u->response_offset=0;u->response_limit=le16(data+6);
    u->pending=0;u->control_stall=0;
    if((type&0x60u)==0) {
        switch(request) {
        case 0: /* GET_STATUS, USB 1.1 9.4.5 */
            u->response[0]=(uint8_t)(((type&31u)==2 && index==0x81)?u->halted:0);
            u->response[1]=0;u->response_size=2;break;
        case 1: /* CLEAR_FEATURE(ENDPOINT_HALT) */
            if(type==2 && value==0 && index==0x81) u->halted=0;
            else u->control_stall=1;
            break;
        case 3: /* SET_FEATURE(ENDPOINT_HALT) */
            if(type==2 && value==0 && index==0x81) u->halted=1;
            else u->control_stall=1;
            break;
        case 5: /* SET_ADDRESS takes effect after its status stage (9.4.6). */
            if(type || value>127) u->control_stall=1;
            else {u->pending=5;u->pending_value=(uint8_t)value;}
            break;
        case 6: /* GET_DESCRIPTOR */
            switch(value>>8) {
            case 1: response(u,device_descriptor,sizeof device_descriptor);break;
            case 2: response(u,configuration_descriptor,sizeof configuration_descriptor);break;
            case 3:
                if((value&255u)==0) {const uint8_t lang[]={4,3,9,4};response(u,lang,sizeof lang);}
                else {
                    const char *text=(value&255u)==1?"Livadix":(value&255u)==2?"Livadix absolute pointer":NULL;
                    if(!text) {u->control_stall=1;break;}
                    const unsigned n=(unsigned)strlen(text);u->response_size=2+n*2;
                    u->response[0]=(uint8_t)u->response_size;u->response[1]=3;
                    for(unsigned i=0;i<n;i++){u->response[2+i*2]=(uint8_t)text[i];u->response[3+i*2]=0;}
                }
                break;
            case 0x21: response(u,configuration_descriptor+18,9);break;
            case 0x22: response(u,pointer_descriptor,sizeof pointer_descriptor);break;
            default:u->control_stall=1;break;
            }
            break;
        case 8: u->response[0]=u->configuration;u->response_size=1;break;
        case 9:
            if(type || value>1) u->control_stall=1;
            else {u->pending=9;u->pending_value=(uint8_t)value;}
            break;
        case 10: u->response[0]=0;u->response_size=1;break; /* GET_INTERFACE */
        case 11: if(value || index) u->control_stall=1;break;
        default:u->control_stall=1;break;
        }
    } else if((type&0x7fu)==0x21u && index==0) { /* HID 1.11 7.2 */
        switch(request) {
        case 1:
            if(u->protocol)response(u,u->report,sizeof u->report);
            else {const uint8_t boot[3]={(uint8_t)(u->report[0]&1u),0,0};response(u,boot,sizeof boot);}
            break;
        case 2: u->response[0]=u->idle;u->response_size=1;break;
        case 3: u->response[0]=u->protocol;u->response_size=1;break;
        case 10:u->idle=(uint8_t)(value>>8);break;
        case 11:
            if(value<=1){u->protocol=(uint8_t)value;u->report_pending=1;u->interrupt_polled=0;u->report_read=u->report_count=0;}
            else u->control_stall=1;
            break;
        default:u->control_stall=1;break;
        }
    } else u->control_stall=1;
    if(u->response_size>u->response_limit)u->response_size=u->response_limit;
    return u->control_stall?-2:8;
}
static void status_complete(mc_ohci *u) {
    if(u->pending==5) u->address=u->pending_value;
    if(u->pending==9) {u->configuration=u->pending_value;u->report_pending=1;u->interrupt_polled=0;u->report_read=u->report_count=0;}
    u->pending=0;
}
/* A USB transaction: byte count, -1 NAK, -2 STALL, -3 no response.
 * The sole device occupies root port 1, with endpoints 0 and 0x81. */
static int packet(mc_ohci *u,unsigned address,unsigned endpoint,unsigned direction,uint8_t *data,unsigned count,uint64_t now) {
    if(!(R(u,PORT)&2u) || (R(u,PORT)&0x14u) || address!=u->address) return -3;
    if(endpoint==0) {
        if(direction==0) return setup(u,data,count);
        if(u->control_stall) return -2;
        if(direction==2) {
            unsigned left=u->response_size-u->response_offset;
            if(left>count)left=count;
            memcpy(data,u->response+u->response_offset,left);u->response_offset+=left;
            if(!count || (!left && !u->response_limit))status_complete(u);
            return (int)left;
        }
        if(!count)status_complete(u);
        return (int)count;
    }
    if(endpoint!=1 || direction!=2 || u->configuration!=1) return -2;
    if(u->halted) return -2;
    const unsigned length=u->protocol?sizeof u->report:3u;
    if(count<length)return -2;
    u->interrupt_polled=1;
    if(!u->report_count && !u->report_pending && (!u->idle || now-u->last_report<(uint64_t)u->idle*MC_C15M_HZ/250u))return -1;
    const uint8_t *sample=u->report_count?u->report_queue[u->report_read]:u->report;
    if(u->protocol)memcpy(data,sample,sizeof u->report);
    else {data[0]=sample[0]&1u;data[1]=data[2]=0;}
    if(u->report_count){u->report_read=(u->report_read+1u)&31u;u->report_count--;}
    u->report_pending=0;u->last_report=now;
    return (int)length;
}
/* OpenHCI 4.3.1.3.1: a TD may span two non-contiguous physical pages. */
static unsigned buffer_size(uint32_t start,uint32_t end) {
    if(!start)return 0;
    if((start&0xfffff000u)==(end&0xfffff000u))return end>=start?end-start+1u:0;
    return 4096u-(start&4095u)+(end&4095u)+1u;
}
static int buffer(mc_ohci *u,uint32_t start,uint32_t end,uint8_t *bytes,unsigned count,int write) {
    unsigned first=4096u-(start&4095u);if(first>count)first=count;
    if(first && !mc_dev_dma(&u->dev,start,bytes,first,write))return 0;
    return count==first || mc_dev_dma(&u->dev,end&0xfffff000u,bytes+first,count-first,write);
}
static uint32_t advance(uint32_t start,uint32_t end,unsigned count) {
    const unsigned first=4096u-(start&4095u);
    return count<first?start+count:(end&0xfffff000u)+(count-first);
}
/* At most 256 EDs and 256 transactions per ED in one visit. Invalid or
 * cyclic guest lists cannot spin the host. NAK preserves the TD (4.3.1.3.7). */
static int list(mc_ohci *u,uint32_t head,uint64_t now) {
    unsigned budget=256;int pending=0;
    for(uint32_t at=head&POINTER_MASK;at && budget--; ) {
        uint32_t ed[4];if(!words(u,at,ed,4))return 0;
        const uint32_t next_ed=ed[3]&POINTER_MASK;
        if(!(ed[0]&0xc000u) && !(ed[2]&1u)) {
            unsigned transactions=256;
            while((ed[2]&POINTER_MASK)!=(ed[1]&POINTER_MASK) && transactions--) {
                const uint32_t td_at=ed[2]&POINTER_MASK;
                uint32_t td[4];if(!td_at || !words(u,td_at,td,4))return 0;
                const unsigned left=buffer_size(td[1],td[3]);
                unsigned mps=(ed[0]>>16)&0x7ffu;if(mps>64)mps=64;
                unsigned count=left<mps?left:mps;
                const unsigned epdir=(ed[0]>>11)&3u, direction=epdir?epdir:(td[0]>>19)&3u;
                uint8_t data[64];
                if(direction!=2 && count && !buffer(u,td[1],td[3],data,count,0)){R(u,STATUS)|=UE;return 0;}
                const unsigned capacity=direction==2 && left?mps:count;
                const int n=packet(u,ed[0]&127u,(ed[0]>>7)&15u,direction,data,capacity,now);
                if(n==-1)break;
                if(n==-3 && ((td[0]>>26)&3u)<2u) {
                    td[0]+=1u<<26;if(!store(u,td_at,td[0]))return 0;break;
                }
                if(n==-3)td[0]=(td[0]&~0x0c000000u)|(3u<<26);
                unsigned cc=n==-2?4u:n==-3?5u:n>=0 && (unsigned)n>left?8u:0u;
                const unsigned transferred=n<0?0u:(unsigned)n>left?left:(unsigned)n;
                if(direction==2 && transferred && !buffer(u,td[1],td[3],data,transferred,1)){R(u,STATUS)|=UE;return 0;}
                unsigned toggle=(td[0]&0x02000000u)?(td[0]>>24)&1u:(ed[2]>>1)&1u;
                const int short_packet=n>=0 && left && (unsigned)n<mps;
                if(n>=0) {
                    toggle^=1u;td[0]=(td[0]&~0x0f000000u)|0x02000000u|(toggle<<24);
                    if(transferred==left)td[1]=0;
                    else td[1]=advance(td[1],td[3],transferred);
                    if(short_packet && transferred<left && !(td[0]&0x40000u))cc=9;
                }
                if(cc || !td[1] || short_packet) {
                    const uint32_t next_td=td[2]&POINTER_MASK;
                    td[0]=(td[0]&~0xf0000000u)|(cc<<28);
                    td[2]=R(u,DONE);R(u,DONE)=td_at;
                    const unsigned delay=cc?0u:(td[0]>>21)&7u;
                    if(delay<u->done_delay)u->done_delay=delay;
                    if(!store(u,td_at,td[0]) || !store(u,td_at+4,td[1]) || !store(u,td_at+8,td[2]))return 0;
                    ed[2]=next_td|(cc?1u:0u)|(toggle<<1);
                    if(!store(u,at+8,ed[2]))return 0;
                    if(cc)break;
                } else {
                    if(!store(u,td_at,td[0]) || !store(u,td_at+4,td[1]))return 0;
                    ed[2]=(ed[2]&~2u)|(toggle<<1);
                    if(!store(u,at+8,ed[2]))return 0;
                }
            }
        }
        if(!(ed[0]&0xc000u) && !(ed[2]&1u) && (ed[2]&POINTER_MASK)!=(ed[1]&POINTER_MASK))pending=1;
        at=next_ed;
    }
    return pending;
}
static void event(void *p,uint64_t now) {
    mc_ohci *u=p;
    if(u->reset_due<=now) {
        u->reset_due=MC_NEVER;R(u,PORT)=(R(u,PORT)&~0x10u)|0x100002u;R(u,STATUS)|=RHSC;
    }
    if(u->resume_due<=now) {
        u->resume_due=MC_NEVER;R(u,PORT)=(R(u,PORT)&~4u)|0x40000u;R(u,STATUS)|=RHSC;
    }
    if(u->next_frame<=now && (R(u,CONTROL)&0xc0u)==OPERATIONAL && !(R(u,STATUS)&UE)) {
        R(u,NUMBER)=(R(u,NUMBER)+1u)&0xffffu;R(u,STATUS)|=SF;
        if(!(R(u,NUMBER)&0x7fffu))R(u,STATUS)|=FNO;
        const uint32_t hcca=R(u,HCCA)&0xffffff00u;
        if((mc_pci_config_read(&u->cfg,MC_PCI_COMMAND,2)&MC_PCI_CMD_MASTER) && hcca) {
            /* 4.4: the frame number occupies only the first halfword. */
            uint8_t frame[2]={(uint8_t)R(u,NUMBER),(uint8_t)(R(u,NUMBER)>>8)};
            if(!mc_dev_dma(&u->dev,hcca+0x80,frame,2,1))R(u,STATUS)|=UE;
            if(!(R(u,STATUS)&UE) && (R(u,CONTROL)&4u)) {uint32_t periodic=0;if(words(u,hcca+(R(u,NUMBER)&31u)*4,&periodic,1))list(u,periodic,now);}
            if(!(R(u,STATUS)&UE) && (R(u,CONTROL)&0x10u) && (R(u,COMMAND)&2u)) {
                R(u,CONTROL_CURRENT)=R(u,CONTROL_HEAD);
                if(!list(u,R(u,CONTROL_HEAD),now))R(u,COMMAND)&=~2u;
                R(u,CONTROL_CURRENT)=0;
            }
            if(!(R(u,STATUS)&UE) && (R(u,CONTROL)&0x20u) && (R(u,COMMAND)&4u)) {
                R(u,BULK_CURRENT)=R(u,BULK_HEAD);
                if(!list(u,R(u,BULK_HEAD),now))R(u,COMMAND)&=~4u;
                R(u,BULK_CURRENT)=0;
            }
            if(!(R(u,STATUS)&UE) && R(u,DONE) && !u->done_delay && !(R(u,STATUS)&WDH)) {
                if(store(u,hcca+0x84,R(u,DONE)|((R(u,STATUS)&R(u,ENABLE)&~WDH)?1u:0u))) {
                    R(u,DONE)=0;R(u,STATUS)|=WDH;u->done_delay=7;
                }
            }
            if(u->done_delay && u->done_delay<7)u->done_delay--;
        }
        if(R(u,STATUS)&UE){R(u,CONTROL)=(R(u,CONTROL)&~0xc0u)|0xc0u;u->next_frame=MC_NEVER;}
        else start_frame(u,now);
    } else if((R(u,CONTROL)&0xc0u)!=OPERATIONAL || (R(u,STATUS)&UE))u->next_frame=MC_NEVER;
    schedule(u);irq(u);
}
static uint32_t read(void *p,uint32_t offset,unsigned size) {
    mc_ohci *u=p;uint32_t value=0;const uint32_t base=offset&~3u;
    if(base<sizeof u->reg) value=base==DISABLE?R(u,ENABLE):R(u,base);
    if(base==REMAINING && u->next_frame!=MC_NEVER) {
        const uint64_t now=mc_dev_now(&u->dev),left=u->next_frame>now?u->next_frame-now:0;
        value=(R(u,INTERVAL)&0x80000000u)|(uint32_t)(left*12000000u/MC_C15M_HZ);
    }
    value>>=(offset&3u)*8u;
    return size==4?value:value&((1u<<(size*8u))-1u);
}
static void write(void *p,uint32_t offset,unsigned size,uint32_t value) {
    mc_ohci *u=p;const uint32_t base=offset&~3u,shift=(offset&3u)*8u;
    if(base>=sizeof u->reg)return;
    const uint32_t mask=size==4?0xffffffffu:((1u<<(size*8u))-1u)<<shift;
    value=(value<<shift)&mask;
    const uint64_t now=mc_dev_now(&u->dev);
    switch(base) {
    case CONTROL:
        R(u,CONTROL)=(R(u,CONTROL)&~mask)|(value&0x7ffu);
        if((R(u,CONTROL)&0xc0u)==OPERATIONAL) {if(u->next_frame==MC_NEVER)start_frame(u,now);}
        else u->next_frame=MC_NEVER;
        break;
    case COMMAND:
        if(value&1u){software_reset(u);return;}
        R(u,COMMAND)|=value&6u;break;
    case STATUS:R(u,STATUS)&=~value;break;
    case ENABLE:R(u,ENABLE)|=value&0xc000007fu;break;
    case DISABLE:R(u,ENABLE)&=~value;break;
    case HCCA:R(u,HCCA)=((R(u,HCCA)&~mask)|value)&0xffffff00u;break;
    case CONTROL_HEAD:case CONTROL_CURRENT:case BULK_HEAD:case BULK_CURRENT:
        R(u,base)=((R(u,base)&~mask)|value)&POINTER_MASK;break;
    case INTERVAL:case PERIOD_START:case LS_THRESHOLD:
        R(u,base)=(R(u,base)&~mask)|value;break;
    case RH_STATUS:
        if(value&0x80000000u)R(u,RH_STATUS)&=~0x8000u;
        if(value&0x8000u)R(u,RH_STATUS)|=0x8000u;
        break;
    case PORT:
        R(u,PORT)&=~(value&0x1f0000u);
        if(value&1u)R(u,PORT)&=~2u;
        if(value&2u)R(u,PORT)|=2u;
        if(value&4u)R(u,PORT)|=4u;
        if((value&8u) && (R(u,PORT)&4u))u->resume_due=now+(uint64_t)MC_C15M_HZ*23u/1000u;
        if(value&0x10u) {device_reset(u);R(u,PORT)|=0x10u;u->resume_due=MC_NEVER;u->reset_due=now+MC_C15M_HZ/100u;}
        break;
    default:break;
    }
    schedule(u);irq(u);
}
static const mc_dev_ops ops={read,write,event,reset,NULL};
static uint32_t config_read(void *p,unsigned offset,unsigned size) {
    return mc_pci_config_read(&((mc_ohci *)p)->cfg,offset,size);
}
static void config_write(void *p,unsigned offset,unsigned size,uint32_t value) {
    mc_pci_config_write(&((mc_ohci *)p)->cfg,offset,size,value);
}
static int mem_read(void *p,uint32_t address,unsigned size,uint32_t *value) {
    mc_ohci *u=p;uint32_t offset;
    if(!mc_pci_config_hit(&u->cfg,0,address,&offset))return 0;
    *value=read(u,offset,size);return 1;
}
static int mem_write(void *p,uint32_t address,unsigned size,uint32_t value) {
    mc_ohci *u=p;uint32_t offset;
    if(!mc_pci_config_hit(&u->cfg,0,address,&offset))return 0;
    write(u,offset,size,value);return 1;
}
void mc_ohci_init(mc_ohci *u,const mc_hw_host *host) {
    memset(u,0,sizeof *u);u->dev=(mc_dev){&ops,host,"Livadix USB"};
    /* PCI 2.0 class 0C/03/10 identifies OHCI, with no vendor quirks. */
    mc_pci_config_init(&u->cfg,0x4d43,2,0x10,0x0c0310,0);
    mc_pci_config_bar(&u->cfg,0,0x1000,0);u->cfg.reg[MC_PCI_INT_PIN]=1;
    u->report[0]=2;u->report[1]=u->report[3]=0;u->report[2]=u->report[4]=0x40;
    reset(u);
}
mc_pci_function mc_ohci_function(mc_ohci *u) {
    return (mc_pci_function){u,config_read,config_write,mem_read,mem_write};
}
int mc_ohci_pointer_ready(const mc_ohci *u) {
    return u->configuration==1 && u->protocol==1 && u->interrupt_polled && !u->halted && (R(u,CONTROL)&0xc0u)==OPERATIONAL && (R(u,PORT)&2u) && !(R(u,PORT)&0x14u);
}
/* Coalesce adjacent motion only. A press and release in the same host
 * event batch must reach separate USB polls, at their original positions. */
static void enqueue_report(mc_ohci *u,const uint8_t *sample,int edge) {
    if(!edge && u->report_count) {
        const unsigned last=(u->report_read+u->report_count-1u)&31u;
        if(!u->report_edge[last]){memcpy(u->report_queue[last],sample,8);return;}
    }
    if(u->report_count==32) {
        /* A stalled guest can accumulate an unbounded host burst. Drop
         * an old motion sample first; an all-edge queue drops one whole
         * click cycle, preserving alternating button state and release. */
        unsigned drop=0;
        while(drop<32 && u->report_edge[(u->report_read+drop)&31u])drop++;
        if(drop<32) {
            for(unsigned i=drop;i+1<32;i++) {
                const unsigned a=(u->report_read+i)&31u,b=(a+1u)&31u;
                memcpy(u->report_queue[a],u->report_queue[b],8);u->report_edge[a]=u->report_edge[b];
            }
            u->report_count--;
        } else {u->report_read=(u->report_read+2u)&31u;u->report_count-=2;}
    }
    const unsigned tail=(u->report_read+u->report_count)&31u;
    memcpy(u->report_queue[tail],sample,8);u->report_edge[tail]=(uint8_t)edge;u->report_count++;
}
int mc_ohci_pointer(mc_ohci *u,int x,int y,unsigned width,unsigned height,int button) {
    if(width<2 || height<2)return 0;
    const int ready=mc_ohci_pointer_ready(u);
    if(x<0)x=0;
    if(y<0)y=0;
    if((unsigned)x>=width)x=(int)width-1;
    if((unsigned)y>=height)y=(int)height-1;
    const unsigned px=(unsigned)((uint64_t)(unsigned)x*32767u/(width-1u));
    const unsigned py=(unsigned)((uint64_t)(unsigned)y*32767u/(height-1u));
    const uint8_t report[8]={(uint8_t)(2u|(unsigned)(button!=0)),(uint8_t)px,(uint8_t)(px>>8),(uint8_t)py,(uint8_t)(py>>8),0,0,0};
    if(memcmp(u->report,report,sizeof report)) {
        if(ready)enqueue_report(u,report,(report[0]&1u)!=(u->report[0]&1u));
        else {u->report_read=u->report_count=0;u->report_pending=1;}
        memcpy(u->report,report,sizeof report);
    }
    return ready;
}
