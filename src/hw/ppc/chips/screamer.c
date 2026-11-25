/* Livadix - Screamer (screamer.h). */
#include "screamer.h"
void mc_screamer_init(mc_screamer *s,const mc_hw_host *host){mc_awac_init(&s->codec,host,"Screamer");s->ready_at=0;}
void mc_screamer_control(void *p,uint32_t word){mc_screamer *s=p;mc_awac_control(&s->codec,(uint16_t)word);if(((word>>12)&7u)==6)s->ready_at=mc_dev_now(&s->codec.dev)+MC_C15M_HZ*64u/1000000u;}
uint32_t mc_screamer_status(void *p){mc_screamer *s=p;return 0x3000u|(mc_dev_now(&s->codec.dev)>=s->ready_at?0x00400000u:0);}
void mc_screamer_output(const mc_screamer *s,int16_t *stereo,size_t n){mc_awac_output(&s->codec,stereo,n);}
