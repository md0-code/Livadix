/* Livadix — the Macintosh real-time clock chip (RTC) with 256 bytes of
 * parameter RAM.
 *
 * Serial protocol from Inside Macintosh Volume III, "The Real-Time Clock"
 * (pp. III-36..38: enable low for a whole transaction, commands and data
 * MSB first, the processor raising the clock after setting each bit and
 * reading each data bit after lowering the clock; the command table: seconds
 * registers z0000001, z0000101, z0001001, z0001101, test register 00110001,
 * write-protect register 00110101, z010aa01 and z1aaaa01 for the 20 bytes of
 * the original parameter RAM; write-protect bit 7) and the Guide to the
 * Macintosh Family Hardware, 2nd ed., "Real-time clock (RTC)" (256 bytes of
 * parameter RAM in the Macintosh II family, the one-second interrupt on
 * VIA1 CA2, rtcData/rtcClk/rtcEnb on VIA1 PB0-2). The two-byte extended
 * command (z0111aaa 0aaaaa00) that reaches all 256 bytes is as the Quadra
 * 700 ROM issues it, and the seconds registers answer z001aa01 as well as
 * z000aa01 (the ROM's ReadDateTime reads the clock once through each form
 * and keeps it only when both agree), both as observed.
 *
 * Lines: MC_RTC_LINE_DATA (the level the chip drives on rtcData while it
 * sends a byte; 1 = released or high) and MC_RTC_LINE_SECOND (the
 * one-second signal: low for half a second each second). */
#ifndef MC_RTC_H
#define MC_RTC_H

#include "hw.h"

#include <stdint.h>

enum { MC_RTC_LINE_DATA, MC_RTC_LINE_SECOND };

typedef struct mc_rtc {
    mc_dev dev;
    uint8_t pram[256];
    uint8_t wp;                 /* write-protect register */
    uint8_t test;
    /* The seconds counter: `seconds` at guest time `base` (C15M). */
    uint32_t seconds;
    uint64_t base;
    uint8_t second_level;
    /* Serial state. */
    uint8_t enb, clk;
    uint8_t shift, bits;        /* byte being received and its bit count */
    uint8_t phase;              /* 0 command, 1 extended address, 2 data in, 3 data out, 4 done */
    uint8_t cmd, out, out_bits;
    uint16_t address;           /* 0-255 parameter RAM, 0x100+n seconds byte n, 0x200 test, 0x201 write-protect */
    uint8_t data_out;           /* level driven on rtcData */
    uint8_t dirty;              /* parameter RAM or clock written since the last mc_rtc_clean */
} mc_rtc;

void mc_rtc_init(mc_rtc *rtc, const mc_hw_host *host, const char *name);
extern const mc_dev_ops mc_rtc_ops;

/* VIA1 PB0-2 as the processor drives them. data is the level on rtcData
 * when the processor drives it (DDRB bit 0 set). */
void mc_rtc_pins(mc_rtc *rtc, int enb, int clk, int data);
/* What the chip drives on rtcData: 1 when released. */
static inline int mc_rtc_data(const mc_rtc *rtc) { return rtc->data_out; }

/* The clock, in seconds since 1 January 1904. */
uint32_t mc_rtc_seconds(const mc_rtc *rtc);
void mc_rtc_set_seconds(mc_rtc *rtc, uint32_t seconds);

#endif
