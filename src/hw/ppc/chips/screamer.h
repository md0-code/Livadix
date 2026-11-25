/* Livadix - Screamer codec behind Heathrow's AWACS-compatible serial port.
 * Apple G3 Developer Note (1998), pp45-46; ROM78F57389 FFF042BC-FFF04404:
 * codec-status ready bit22, type3 bits15..12; register6 calibration, followed
 * by 1041 time-base ticks before checking ready. Common attenuation/control
 * storage uses Livadix's AWAC implementation; no other emulator code used. */
#ifndef MC_SCREAMER_H
#define MC_SCREAMER_H
#include "awac.h"
typedef struct mc_screamer {mc_awac codec;uint64_t ready_at;} mc_screamer;
void mc_screamer_init(mc_screamer *,const mc_hw_host *);
void mc_screamer_control(void *,uint32_t);
uint32_t mc_screamer_status(void *);
void mc_screamer_output(const mc_screamer *,int16_t *,size_t);
#endif
