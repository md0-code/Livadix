/* Livadix — the AWAC (audio waveform amplifier and converter), the sound
 * codec of the Power Macintosh 6100, 7100 and 8100; written as the codec
 * alone, its host interface left to the controller that clocks its control
 * words (the AMIC here, amic.h), so that the PCI machines' AWACS can build
 * on it.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994): chapter
 * 2, "AWAC Sound Chip" (a 44-pin waveform amplifier with a 16-bit stereo
 * codec, conforming to the IT&T ASCO 2300 Audio-Stereo Codec
 * specification), Table 2-3 (the sound clock); chapter 3, "Sound I/O"
 * (16-bit stereo in and out, the built-in speaker and the line out with
 * their volumes set independently), Table 3-16 and Figures 3-12 to 3-14
 * (frames of 256 bits in four subframes of two 32-bit samples, 20 data and
 * 12 auxiliary bits each; subframe 1 the Macintosh's own sound; the bit
 * clock 256 times the sample rate).
 *
 * The control words as ROM 9FEB69B3 sends them (ROM
 * addresses at $FFC0 0000): 16-bit words whose bits 14-12 address a
 * register and bits 11-0 carry its data; the start-up chime and the POST's
 * beep send $4000 + v, $2000 + v and $1000 + s ($FFCE5A46-$FFCE5B1C,
 * $FFF0551C-$FFF05620), where v = a x $41 for an attenuation a = 2 x (7 -
 * the PRAM volume) in two 4-bit fields (bits 9-6 and 3-0: left and
 * right†), and s carries in bit 7 the status bit 3 the ROM read just
 * before. Livadix's reading†: register 4 the speaker's attenuation, register 2
 * the line out's, steps of 1.5 dB (ASCO 2300's step†); register 1 the input
 * set-up, kept. The status: four bytes the controller reads back, of which
 * the ROM tests byte 2 bit 3; Livadix keeps them as the machine sets them†.
 *
 * Livadix's own code. */
#ifndef MC_AWAC_H
#define MC_AWAC_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

#define MC_AWAC_REGS 8u

typedef struct mc_awac {
    mc_dev dev;
    uint16_t reg[MC_AWAC_REGS];     /* 12-bit data by address */
    uint8_t status[4];
    uint32_t gain[2];               /* left, right: output gain in 1/32768 */
    unsigned out_reg;               /* the register whose attenuation reaches the host */
} mc_awac;

extern const mc_dev_ops mc_awac_ops;
void mc_awac_init(mc_awac *a, const mc_hw_host *host, const char *name);
/* A 16-bit control word from the controller. */
void mc_awac_control(mc_awac *a, uint16_t word);
/* The status bytes (index 0-3). */
uint8_t mc_awac_status(const mc_awac *a, unsigned index);
void mc_awac_set_status(mc_awac *a, unsigned index, uint8_t value);
/* Applies the output attenuation to `frames` interleaved stereo frames. */
void mc_awac_output(const mc_awac *a, int16_t *stereo, size_t frames);

#endif
