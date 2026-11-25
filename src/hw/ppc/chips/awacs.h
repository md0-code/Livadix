/* Livadix — Grand Central's sound interface: the AWACS's port behind Grand
 * Central at +$14000, with its DBDMA output (channel 8) and input (channel
 * 9) streams. The codec itself (the control words' registers, its status,
 * the attenuation) is the AWAC model the 6100 family brings; this file is
 * the controller side that clocks words to it and frames to and from it.
 *
 * Sources: Power Macintosh 7500 and 8500 Developer Note (Apple, 1995),
 * "AWAC Sound IC" (a waveform amplifier with a 16-bit stereo codec after
 * the IT&T ASCO 2300 specification); Power Macintosh 7200 Developer Note
 * (1995), "AWACS Sound IC"; Power Macintosh Computers Developer Note
 * (Apple, 1994), chapter 3 "Sound I/O" (16-bit stereo frames, left first).
 * CHRP I/O Device Reference (1996), chapter 15 (DBDMA).
 *
 * As ROM 9630C68B's POST plays the start-up sound ($FFF03D2C-$FFF03E94), registers little-endian at $10 steps:
 *  +$00  sound control: bits 10-8 the sample rate code, set from the sound
 *        resource's header (the start-up sound at $FFE0 0000: code 2, 207,868
 *        bytes of 16-bit stereo, about 2.4 s at 22,050 Hz); the other bits
 *        kept†
 *  +$10  codec control: a write sends the codec a control word (bits 14-12
 *        its register, 11-0 the data: POST writes $4000 + v and $2000 + v,
 *        v = a x $41 for an attenuation a = 2 x (7 - the PRAM volume), then
 *        $1000 + (status bit 3 << 4)); bit 24 reads set while the word is
 *        on its way (POST polls it, $FFF03DD8)
 *  +$20  codec status, read (POST takes bit 3)
 *  +$30  the clipping count†, +$40 byte swap† (kept)
 * Channel 8 plays the output: POST stops it, points it at a list of
 * OUTPUT_MORE commands in ROM and sets run ($FFF03E0C-$FFF03E8C).
 *
 * Livadix's model: the rate codes are 44,100 Hz / 1, 1.5, 2, 2.5, 3, 4, 5, 6
 * (code 2 22,050 Hz, the start-up sound's†); a codec word is busy for one
 * frame time†; output frames are 16-bit big-endian stereo† (left first),
 * taken from a 1 KB FIFO the output channel fills, one millisecond's worth
 * per event; MC_AWACS_LINE_OUT_DRQ (and _IN_DRQ for the input, which
 * delivers silence) pulses when the FIFO wants data. The codec is reached
 * through mc_awacs_codec (none: words are kept, the status reads 0).
 *
 * Livadix's own code. */
#ifndef MC_AWACS_H
#define MC_AWACS_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

enum { MC_AWACS_LINE_OUT_DRQ = 0, MC_AWACS_LINE_IN_DRQ = 1 };
enum { MC_AWACS_CONTROL = 0x00, MC_AWACS_CODEC = 0x10, MC_AWACS_STATUS = 0x20, MC_AWACS_CLIP = 0x30, MC_AWACS_SWAP = 0x40 };

#define MC_AWACS_SIZE 0x1000u
#define MC_AWACS_FIFO 1024u
#define MC_AWACS_RING 16384u            /* frames kept for the host */
#define MC_AWACS_BUSY 0x01000000u

typedef struct mc_awacs_codec {
    void *ctx;
    void (*control)(void *ctx, uint32_t word);
    uint32_t (*status)(void *ctx);
} mc_awacs_codec;

typedef struct mc_awacs {
    mc_dev dev;
    mc_awacs_codec codec;
    uint32_t control, word, swap;
    uint64_t busy_until;
    uint8_t out[MC_AWACS_FIFO];
    unsigned out_n;
    int played;                    /* output DMA has supplied samples since reset */
    unsigned in_n;                  /* silence bytes ready for the input channel */
    int16_t ring[MC_AWACS_RING * 2];
    unsigned ring_head, ring_n;
    uint64_t last;                  /* the time frames were last made */
    uint64_t frac;                  /* frame remainder (x C15M) */
} mc_awacs;

extern const mc_dev_ops mc_awacs_ops;   /* offsets from Grand Central + $14000 */

void mc_awacs_init(mc_awacs *a, const mc_hw_host *host, const char *name, const mc_awacs_codec *codec);
void mc_awacs_reset(mc_awacs *a);
/* The sample rate the control register selects (Hz). */
uint32_t mc_awacs_rate(const mc_awacs *a);
/* The DBDMA streams: output (channel 8) and input (channel 9). */
int mc_awacs_dma_write(void *a, uint8_t byte);
int mc_awacs_dma_read(void *a, uint8_t *byte);
/* Up to `frames` interleaved stereo frames played since the last pull. */
size_t mc_awacs_pull(mc_awacs *a, int16_t *stereo, size_t frames);

#endif
