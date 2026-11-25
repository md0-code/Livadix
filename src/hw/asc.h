/* Livadix — Apple Sound Chip (ASC) and Enhanced ASC (EASC).
 *
 * The chip occupies 4 KiB (Quadra 700: $50014000, I/O block offset $14000;
 * Quadra 700 Developer Note, Figure 1-2; the ROM's address table at ROM
 * offset $3598). Offsets within it, with the source of each. "ROM" is the
 * Quadra 700 ROM 420DBFF3 (Apple's code, read as disassembly; addresses
 * are where it runs, $4080 0000 + ROM offset):
 *
 *   $000-$3FF  FIFO A: every byte written anywhere here is appended to it
 *              (Guide ch. 13; the ROM's start beep writes FIFO A at +0 and
 *              FIFO B at +$400, $408BE2C4); in wavetable mode (2) the 2 KiB
 *              are plain RAM, voices 0-1 here (Guide ch. 13; the ROM's sound
 *              RAM test writes and reads back all 2 KiB in mode 2 on the
 *              EASC too, $40847EE4)
 *   $400-$7FF  FIFO B; wave RAM voices 2-3
 *   $800       version (read-only): $00 ASC, $B0 EASC (the
 *              ROM compares it with $B0 to choose its EASC code,
 *              $40845C76, $40847E66, $40849374)
 *   $801       mode: 0 off, 1 FIFO, 2 wavetable; two bits read back (Guide
 *              ch. 13; the ROM's register test, table at $40848068: mask
 *              $03)
 *   $802       control: bit 1 stereo on the ASC (Guide ch. 13); the ROM
 *              never sets it for the EASC, which plays both FIFOs always
 *   $803       FIFO mode: bit 7 clears both FIFOs (the ROM sets then clears
 *              it before each use, $40804324, $40845C90); the original ASC
 *              reads it back (the IIci ROM's register test)
 *   $804       status. ASC: interrupt flags, cleared by the read (Guide ch.
 *              13): bits 0/2 FIFO A/B fell below half, bits 1/3 FIFO A/B
 *              ran empty or filled up (System 7.5.3's Sound Manager fills
 *              both FIFOs until bit 3 reads set); a write sets them (the
 *              IIci ROM's register test reads back $01-$08)†. EASC: the FIFOs' state, read without side effect other
 *              than the interrupt below: bit 0 FIFO A below half, bit 1 FIFO
 *              A full or empty, bits 2/3 the same for FIFO B (the ROM's
 *              start beep fills FIFO A until bit 1 sets, $40845F2E, after
 *              bit 0 asked for data, $40845E3C, and ends when both are set,
 *              $40845E10; its chime polls bit 2 before every byte, bit 3 for
 *              the end, $408BE2B8, $408BE2D2; $0F with both empty,
 *              $40849386); FIFO A recording: bit 0 below half, bit 1 full
 *              (System 7.5.5's EASC interrupt handler takes recorded data
 *              when bit 0 is clear on revision $B0, asc.c)
 *   $805       wavetable control (ASC; stored)†
 *   $806       volume: bits 7-5, 0 (silent) to 7 (Guide ch. 13; register
 *              test mask $E0)
 *   $807       clock rate: ASC 0 = C15M/704, 2 = 22050 Hz, 3 = 44100 Hz†;
 *              the EASC stores it (the ROM clears it, $40847FFC)
 *   $80A       EASC play/record: bit 0 record (register test mask $01; the
 *              ROM's record test, $40849526); the ROM writes 2 to play
 *   $80F       test (stored)
 *   $810-$82F  wavetable: voice n phase at $810+8n, increment at $814+8n
 *              (big-endian, 24 bits used: 9 integer, 15 fraction bits†)
 *   $F00-$F3F  EASC per-FIFO registers, FIFO A at $F00, B at $F20:
 *              +4/+5   rate increment, FIFO bytes per 44.1 kHz output sample
 *                      in 1/$10000 (the ROM computes rate x 65535 / 44100
 *                      from a sound's sample rate, $408BE274, and writes
 *                      $812F for 22,254.5 Hz, $40845CC4)
 *              +6/+7   left and right volume, $7F full (the ROM plays FIFO A
 *                      left with $7F/$00 and FIFO B right with $00/$7F,
 *                      $408042F0, $40845CFC)
 *              +8      FIFO control: read back whole (register test mask
 *                      $FF, values $00-$03 and $80-$83); the ROM sets bit 7
 *                      for every playback and bits 0-1 by the sound's
 *                      compression, 1 for compression ID 5, 2 for ID 6
 *                      ($408BE28C); mode 2 is decoded (asc.c, decode), mode
 *                      1 plays the bytes as they are†
 *              +9      bit 0 masks the FIFO's interrupt (register test mask
 *                      $FF; the ROM's interrupt test expects FIFO B's events
 *                      with $F29 = 0 and $F09 = 1 and the reverse, $4084944C,
 *                      $40849498; the ROM's start leaves both at 1 and the
 *                      System's sound driver unmasks them)
 *              +$10-$17 the decoder's four prediction filters, a pair of
 *                      signed bytes each (the ROM writes $0000, $003C, $CC73,
 *                      $C962, $40845CD0)
 *   $F40-$F4F  the PowerBook 520's and 190's sound block (ROMs B6909089
 *              and 4D27039C): $F40 bits 0-1 the sample rate of its 16-bit
 *              samples, read back and rewritten by the ROMs ($400E0AC6,
 *              $400E0AF4 on the 520, $4000FE0C on the 190), $F44, $F48,
 *              $F4A read back (asc.c)
 *   $1000-$17FF, $1800-$1FFF  (the 520's and 190's 8 KiB window) FIFO A
 *              and FIFO B taking 16-bit samples (the 190's chime, System
 *              7.5.3's and Mac OS 8.0's Sound Managers, asc.c)
 *
 * EASC interrupt (the ROM's interrupt test, $40849384-$408495FE: it takes
 * an interrupt on VIA2 CB1 when the unmasked FIFO fills up, another when it
 * falls below half, polls for empty, and when recording into a FIFO it just
 * cleared and unmasked, an interrupt before the FIFO is half full): each
 * FIFO's events (below half, empty, full; recording, half full and full; a
 * clear) are latched and read-cleared through $804, and an unmasked FIFO
 * with an event latched asserts /IRQ.
 *
 * Output: one sample per tick of the sample clock, C15M/704 = 22,254.5 Hz
 * on the ASC, 44.1 kHz on the EASC (its rate reference
 * above), computed lazily from guest time on every access; the scheduled
 * deadline only serves the IRQ line. Rendered frames go to an output ring
 * the machine drains with mc_asc_pull.
 */
#ifndef MC_ASC_H
#define MC_ASC_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

/* Output line: the FIFO interrupt, 1 = asserted (the chip's active-low /IRQ
 * pulled low). On the Quadra 700 it reaches VIA2 CB1. */
#define MC_ASC_LINE_IRQ 0

#define MC_ASC_SIZE 0x1000u
#define MC_ASC_FIFO 1024u
#define MC_ASC_RING 16384u      /* output frames buffered for the host */
#define MC_ASC_WFIFO 1024u      /* samples in each FIFO of the PowerBook sound block's 16-bit ports (asc.c) */

/* MC_ASC_V8: the ASC of the V8 family's integrated controllers (Sonora,
 * VASP: "functionally identical to that provided by the V8 gate array",
 * Macintosh LC III and IIvx Developer Notes, "Sound Control"): the
 * original ASC, whose FIFOs, when the mode leaves FIFO mode, stop with an
 * event on each ($804 bits 0-3 latched): the IIvx ROM's start beep writes
 * a byte to each FIFO and waits for bit 1 (FIFO A ran empty), and after
 * the last byte sets the mode to 0 and waits for bit 0 (ROM 4957EB49
 * offsets $85EF8, $85F06, $85F0E); the LC III's and IIvx's register
 * tests write $00-$08 to $804 and read them back after a mode test that
 * ends in mode 0 (ROM ECBBC41C offset $48418, its table at $4858E)†. */
typedef enum mc_asc_type { MC_ASC_ORIGINAL, MC_ASC_ENHANCED, MC_ASC_V8 } mc_asc_type;

/* FIFO A keeps its bytes in ram[0..1023], FIFO B in ram[1024..2047]: the
 * 2 KiB of the chip are one RAM seen as FIFOs or as wave tables (Guide to the
 * Macintosh Family Hardware, 2nd ed., ch. 13, "ASC sound modes"). */
typedef struct mc_asc_fifo {
    uint16_t rd, wr, count;
    uint32_t acc;               /* EASC rate converter: $8000 = one byte */
    int16_t out;                /* the sample playing (held on underrun) */
    /* EASC decoding: position in the 28-sample group, its header, the byte
     * holding the next nibble, the two previous samples. */
    uint8_t xa_pos, xa_hdr, xa_byte;
    int s1, s2;
    uint16_t wrd, wwr, wcount;  /* the sound block's 16-bit FIFO (wram) */
} mc_asc_fifo;

typedef struct mc_asc {
    mc_dev dev;
    uint8_t type;               /* mc_asc_type */
    uint8_t mode, control, fifo_mode, irq_status, wt_control, volume, clock, play_rec, test;
    uint32_t phase[4], incr[4]; /* wavetable voices */
    uint8_t xreg[0x40];         /* EASC $F00-$F3F as written */
    /* The PowerBook 520's and 190's sound block (asc.c): present (set by
     * the board after mc_asc_init), $F40-$F4F as written, and (bit c)
     * whether FIFO c plays what its 16-bit port ($1000, $1800) was given. */
    uint8_t block, wide;
    uint8_t breg[0x10];
    uint8_t whi;                /* a byte write's high half of a 16-bit sample */
    int16_t wram[2][MC_ASC_WFIFO];
    /* Singer's stereo input uses the two 16-bit ports while FIFO B can
     * continue to signal playback. The input interrupt is FIFO A's. */
    uint8_t singer;
    /* The ASC of the LC family's V8, Eagle and Spice records into FIFO A
     * ($80A bit 0, as the EASC; set by the board after mc_asc_init). */
    uint8_t input;
    uint16_t capture_rd[2], capture_wr[2], capture_count[2];
    mc_audio_source source;
    uint32_t capture_phase;
    int16_t capture[2][MC_ASC_WFIFO];
    uint16_t capture_word[2];  /* low byte saved for a split port read */
    uint8_t ram[2 * MC_ASC_FIFO];   /* the chip's 2 KiB: FIFOs or wave tables */
    mc_asc_fifo fifo[2];
    int irq;                    /* line state last reported */
    uint8_t irq_events;         /* the EASC events behind it (asc.c, update_irq) */
    /* Sample clock: ticks(now) = base_ticks + (now - base_time) * num / den. */
    uint64_t base_time, base_ticks, ticks;
    uint64_t num, den;
    int16_t ring[MC_ASC_RING][2];
    size_t ring_rd, ring_count;
    uint64_t dropped;           /* frames lost to a full ring */
    uint64_t played;            /* FIFO bytes taken for output since power-up */
} mc_asc;

extern const mc_dev_ops mc_asc_ops;

/* Powers the chip up in its reset state on `host`. */
void mc_asc_init(mc_asc *asc, const mc_hw_host *host, mc_asc_type type);

/* The sound input (Singer's, or what FIFO A records on the EASC and the LC
 * family's ASC); NULL supplies silence. The optional enable callback
 * follows the sample gate: $80A bit 0 set and, on the EASC, FIFO mode
 * (Singer: a valid $F40 rate). It receives the input's rate, or 0 on
 * stop/reset. Replacing an active source stops the old source before
 * starting the new one. */
void mc_asc_set_audio_source(mc_asc *asc, const mc_audio_source *source);

/* The host's sound hand-off. Brings the chip up to the current guest time,
 * then copies at most `frames` interleaved stereo int16 frames (left, right)
 * into `stereo`, oldest first, and returns how many: exactly the frames the
 * sample clock produced since the last pull. Fewer than asked is an
 * underrun: the caller pads (the chip never invents frames ahead of guest
 * time). Frames the host leaves in the ring beyond MC_ASC_RING are dropped
 * oldest first (counted in `dropped`). Volume (register $806, and on the
 * EASC the per-FIFO volumes) is already applied. */
size_t mc_asc_pull(mc_asc *asc, int16_t *stereo, size_t frames);

/* The output rate in Hz, rounded (22255 for C15M/704); the exact clock is
 * mc_asc_rate_exact's num/den samples per C15M period. */
uint32_t mc_asc_rate(const mc_asc *asc);
void mc_asc_rate_exact(const mc_asc *asc, uint64_t *num, uint64_t *den);

/* Bytes waiting in FIFO A (0) or B (1), as of the current guest time
 * (the sound block's 16-bit FIFO: two per sample). */
unsigned mc_asc_fifo_level(mc_asc *asc, int fifo);

#endif
