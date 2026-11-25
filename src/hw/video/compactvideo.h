/* Livadix — the video and sound of the Macintosh 128K, 512K and Plus: the
 * 512 x 342 one-bit screen and the sound buffer, both fetched from the top
 * of main RAM.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., chapter 12
 * ("Video": 512 x 342 pixels at the 15.6672 MHz dot clock, 704 dot times a
 * line of which 512 visible, 370 lines a frame of which 342 visible, 60.15
 * frames a second; the main screen buffer $5900 bytes below the top of RAM
 * ($3F A700 in the 4 MB space the RAM repeats through), the alternate
 * $8000 below it, VIA1 PA6 selecting the main buffer when high; vertical
 * blanking on VIA1 CA1; a set bit a black pixel) and chapter 13 ("Sound":
 * one word fetched from the sound buffer in each line's horizontal
 * blanking, its high byte the sample, its low byte the disk-speed PWM
 * value, 370 words a frame at 22.2545 kHz; the main buffer $300 below the
 * top of RAM ($3F FD00), the alternate $5F00 below it, VIA1 PA3 selecting
 * the main buffer when high; PA2-PA0 the volume; PB7 low enabling the
 * sound, which Timer 1's PB7 output can square), chapter 3 ("Video and
 * sound timing": the line's 512 dots of the visible part, then 192 of
 * horizontal blanking, which VIA1 PB6 shows on the 128K, 512K and Plus:
 * mc_compactvideo_hblank). The disk-speed bytes: each frame's 370 speed
 * values drive the 400K drive's motor (Guide, chapter 13, "Disk speed";
 * the drive's speed control line, Inside Macintosh III, "The Disk
 * Interface"). The PWM counter is a six-bit shift register (x' = x >> 1,
 * bit 5 = bit 0 xor bit 1); a speed byte's duty is its place in that
 * sequence counted from $1E, which is how ROM 4D1F8172 turns its speed
 * values into bytes (offset $186AE: 440 less the value, in tenths, steps
 * from $1E; ROM 28BA61CE, offset $1C1A: 399 less the value from $0B, three
 * steps further on) and fewer steps run the motor faster (its calibration,
 * $41917C and $4182E2, lowers the value when the drive runs fast). The
 * buffer's mean place is the motor's speed (mc_compactvideo_speed): 650 rpm
 * at place 0 down to 300 at place 44, linear (a drive of the 394-590 rpm
 * zones the ROMs calibrate to†), at once (no inertia†).
 *
 * Livadix's own code. One event at the start of vertical blanking makes the
 * frame's 370 samples (each at its line's level of PB7, from the edges
 * logged since the last frame) and lowers MC_COMPACTVIDEO_LINE_VBL's
 * level (state 1 = blanking), one at its end raises it. The samples go to
 * a ring the machine's sound output pulls from. */
#ifndef MC_COMPACTVIDEO_H
#define MC_COMPACTVIDEO_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

#define MC_COMPACTVIDEO_WIDTH 512u
#define MC_COMPACTVIDEO_HEIGHT 342u
#define MC_COMPACTVIDEO_ROW_BYTES 64u
#define MC_COMPACTVIDEO_LINE 704u                   /* dot times (C15M periods) a line */
#define MC_COMPACTVIDEO_LINES 370u
#define MC_COMPACTVIDEO_FRAME (MC_COMPACTVIDEO_LINE * MC_COMPACTVIDEO_LINES)
/* The buffers in the 4 MB space the RAM repeats through. */
#define MC_COMPACTVIDEO_MAIN 0x3FA700u
#define MC_COMPACTVIDEO_ALT 0x3F2700u
#define MC_COMPACTVIDEO_SOUND_MAIN 0x3FFD00u
#define MC_COMPACTVIDEO_SOUND_ALT 0x3FA100u
#define MC_COMPACTVIDEO_RING 8192u
#define MC_COMPACTVIDEO_EDGES 64u

enum { MC_COMPACTVIDEO_LINE_VBL };

typedef struct mc_compactvideo {
    mc_dev dev;
    const uint8_t *ram;
    uint32_t ram_size;              /* a power of two, or 2.5 MB (two rows, compact.c) */
    uint8_t page_main;              /* VIA1 PA6 */
    uint8_t sound_main;             /* VIA1 PA3 */
    uint8_t volume;                 /* VIA1 PA2-PA0 */
    uint8_t pb7;                    /* VIA1 PB7: high disables the sound */
    uint8_t vbl;
    uint64_t frame;                 /* the time line 0 of the current frame began */
    /* PB7's edges in the current frame. */
    uint64_t edge_time[MC_COMPACTVIDEO_EDGES];
    uint8_t edge_level[MC_COMPACTVIDEO_EDGES];
    unsigned edges;
    uint8_t pb7_frame;              /* PB7 at line 0 */
    int16_t ring[MC_COMPACTVIDEO_RING];
    unsigned head, count;
    float hp_in, hp_out;            /* the output's DC filter */
    uint8_t primed;
    uint8_t sounding;               /* a sample other than the rest level was made */
    uint32_t palette[2];
} mc_compactvideo;

void mc_compactvideo_init(mc_compactvideo *v, const mc_hw_host *host, const char *name, const uint8_t *ram, uint32_t ram_size);
extern const mc_dev_ops mc_compactvideo_ops;
/* VIA1 port A's pins (PA6, PA3, PA2-PA0) and PB7. */
void mc_compactvideo_set_pa(mc_compactvideo *v, uint8_t pa);
void mc_compactvideo_set_pb7(mc_compactvideo *v, int level);
/* Horizontal blanking at guest time `now`: 1 in the 192 dots after each
 * line's visible 512 (VIA1 PB6 on the 128K, 512K and Plus, high while
 * blanking†). */
int mc_compactvideo_hblank(const mc_compactvideo *v, uint64_t now);
/* The 400K drive motor's speed the sound buffer's speed bytes set now
 * (rpm x 16). */
uint32_t mc_compactvideo_speed(const mc_compactvideo *v);
/* A speed byte's place in the PWM counter's sequence from $1E (0-62; a
 * byte whose six bits are 0 leaves it stopped: 62). */
unsigned mc_compactvideo_pwm_place(uint8_t byte);
/* The screen shown (the buffer PA6 selects). */
const uint8_t *mc_compactvideo_pixels(const mc_compactvideo *v);
/* Sound out: the rate (C15M / 704, rounded) and the samples made, as
 * stereo frames. */
#define MC_COMPACTVIDEO_RATE ((MC_C15M_HZ + MC_COMPACTVIDEO_LINE / 2u) / MC_COMPACTVIDEO_LINE)
size_t mc_compactvideo_pull(mc_compactvideo *v, int16_t *stereo, size_t frames);

#endif
