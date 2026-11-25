/* Livadix — the PSC, the Peripheral Subsystem Controller of the Quadra 840AV
 * and Centris 660AV: the I/O decode, the interrupt controller, VIA2's
 * functions, the DMA channels and the Singer codec's sound buffers.
 *
 * Quadra 840AV and Centris 660AV Developer Note (Apple, 1993), chapter 2:
 * "Peripheral Subsystem Controller" (nine DMA channels, the I/O decode, all
 * internal system interrupts, the VIA inputs, 16-byte buffers for the Singer
 * sound codec), "PSC Functions", Table 2-10 (the channels: SCSI, ENetRd,
 * ENetWr, FDC, SCCA, SCCB, SCCATx, SndIn, SndOut), Table 2-11 (priorities),
 * Table 2-2 (the PSC's clocks: C22_5792M and C24_576M for Singer at 44.1 and
 * 48 kHz), "Sound I/O"; chapter 10 "DMA Serial Driver" (SCCA is channel 4,
 * SCCB 5, SCCATx 6; DMA transfers start on longword boundaries).
 *
 * The register layout is as ROM 5BF10FD1 drives it:
 *   - $50F3 1000 + $100 + $10L, L = 3-6: interrupt level L's flags (byte);
 *     +4 its enables, written as a VIA's IER (bit 7 set or clear). The
 *     ROM's level handlers read flags & enables (ROM offsets $109D2 level 3
 *     bit 0; $10916 level 4 bits 0-3, bit 3 the DMA channels; $108B6 level 5
 *     bits 0-1; $10846 level 6 bits 0-2) and dispatch through a table of
 *     nine pairs (ExpandMem's +$210); the serial driver takes level 4 bits
 *     1 and 2 ($10A38 writes $86);
 *   - +$200 sound control (bit 8 plays the output buffer: $104 to start the
 *     chime, $004 to stop, ROM offset $C5C88), +$204 Singer's control
 *     ($45500F, the volume in bits 4-7), +$208 Singer's status, +$20C the
 *     output DMA's progress (bits 6-21: nonzero while a buffer plays, the
 *     chime waits for it to reach zero before giving the next buffer and
 *     for it to leave zero after, $C5CB0), +$210 the input buffer, +$214 the
 *     output buffer's address, +$218 its length in 8-byte units ($3C0 for
 *     the chime's $1E00-byte buffers); the samples 16-bit big-endian stereo,
 *     left first, at 44.1 kHz (the chime's own data at ROM offset $C5D24);
 *   - +$804: the DMA channels' interrupt flags (the level 4 bit 3 handler,
 *     $10988, finds the highest set bit of the top byte);
 *   - +$C00 + $10n: DMA channel n's control word: written as the VIA's IER
 *     (bit 15 set or clear); bit 0 the register set in use, bit 9 a flush
 *     (clears itself), bit 10 pause, bit 11 a reset (the channel paused and
 *     idle: reads $C400), bit 14 (read) the channel stopped; FDC is channel
 *     3 ($C30), SCSI 0 ($C00), SCCB 5 ($C50; the serial DMA at $1850F6);
 *   - +$1000 + $20n + $10s: channel n's register set s: +0 the memory
 *     address, +4 the byte count, +8 the command word, written as the
 *     control word: bit 8 cleared before use, bit 9 the direction (set:
 *     from the device to memory), bit 10 the set's terminal count, bit 11
 *     armed (cleared by the PSC when the set is done). The SCSI HAL
 *     ($1F100) pauses the channel, fills the set the control word names,
 *     arms it and unpauses.
 *   - VIA2 at $50F0 2000 (registers every $200, as a VIA's): the PSC's own
 *     VIA2 functions; interrupt flags (+$1A00) and enables (+$1C00, written
 *     as a VIA's IER), its output to level 2; the inputs: bit 0 the SCSI
 *     controller's DREQ, bit 3 its interrupt (the SCSI Manager enables $88
 *     and polls the flags at $50F0 3A00, ROM offset $1F81E), bit 5 the
 *     floppy controller (its driver enables $A0, $18FDBC), bit 1 the slots;
 *     port A at +$1E00 and +$0200 reads the slots' interrupts, active low
 *     (bit s - 9 for slot s, and bit 6 Civic's VBL, which the ROM's slot
 *     dispatcher takes for slot 0, ROM offsets $155A8 and $1566E, where
 *     the video driver installs its handler).
 *   - the Ethernet channels as the built-in Ethernet driver (ROM offsets
 *     $1E0D78-$1E2600) drives them: ENetRd's sets each name a buffer of
 *     2 KB slots, the count register the slots ($1E18B8: command $100,
 *     address, count, $9800), one frame a slot, its four receive status
 *     bytes in words at +0 to +6 and the frame from +$10 ($1E1FAA: the
 *     length from the low byte at +0 and bits 11-8 of the word at +2, the
 *     status from bits 7-4 of its low byte)†; bit 8 of a set's command
 *     marks a frame done and bit 8 of the channel's control word reads
 *     while a set has it ($1E2142-$1E2166: the handler clears it with
 *     $1100 and reads the count left); ENetWr's sets each one frame, a byte
 *     count ($1E1CA8), the driver waiting for bit 10 before it refills a
 *     set ($1E1E56); bit 13 of the control word (an error, $1E2146) never
 *     reads set; the channels take their sets in turn†;
 *   - VIA1 CA1's 60.15 Hz: the ROM programs no VIA2 timer for it (the
 *     Quadra 700 squares VIA2 PB7 into VIA1 CA1); the PSC drives it†.
 *
 *   - +$21C (byte) the DSP's control, as the ROM's AV sound code drives it
 *     ($4099BEF4: $01 starts the DSP3210 program it put at physical 0,
 *     $81 on a timeout stops it again): bit 7 selects setting (1) or
 *     clearing (0) the bits 2-0 written; bit 0 holds the DSP in reset
 *     (MC_PSC_LINE_DSP_RESET); the reset value holds it†.
 *
 * Lines (mc_hw_host line): MC_PSC_LINE_IRQ3..6 and MC_PSC_LINE_VIA2 (1 =
 * asserted), MC_PSC_LINE_TICK (VIA1 CA1's level), MC_PSC_LINE_DSP_RESET (1
 * holds the DSP3210). */
#ifndef MC_PSC_H
#define MC_PSC_H

#include "../hw.h"

#include <stddef.h>
#include <stdint.h>

enum {
    MC_PSC_LINE_IRQ3, MC_PSC_LINE_IRQ4, MC_PSC_LINE_IRQ5, MC_PSC_LINE_IRQ6,
    MC_PSC_LINE_VIA2, MC_PSC_LINE_TICK, MC_PSC_LINE_DSP_RESET, MC_PSC_LINE_DSP_FRAME
};

#define MC_PSC_SIZE 0x2000u         /* from $50F3 1000 */
#define MC_PSC_VIA2_SIZE 0x2000u    /* from $50F0 2000 */
#define MC_PSC_CHANNELS 9

/* The channels (Table 2-10, the serial driver's numbers). */
enum {
    MC_PSC_CH_SCSI = 0, MC_PSC_CH_ENET_RD = 1, MC_PSC_CH_ENET_WR = 2, MC_PSC_CH_FDC = 3,
    MC_PSC_CH_SCCA = 4, MC_PSC_CH_SCCB = 5, MC_PSC_CH_SCCA_TX = 6, MC_PSC_CH_SND_IN = 7, MC_PSC_CH_SND_OUT = 8
};

/* Interrupt sources: level (3-6) and bit, or VIA2 and bit. */
typedef enum mc_psc_source {
    MC_PSC_SRC_ENET = 0x30,         /* level 3 bit 0 */
    MC_PSC_SRC_L4_0 = 0x40,
    MC_PSC_SRC_SCC_A = 0x41,        /* level 4 bit 1 */
    MC_PSC_SRC_SCC_B = 0x42,        /* level 4 bit 2 */
    MC_PSC_SRC_L5_0 = 0x50,
    MC_PSC_SRC_L5_1 = 0x51,
    MC_PSC_SRC_L6_0 = 0x60,
    MC_PSC_SRC_L6_1 = 0x61,
    MC_PSC_SRC_L6_2 = 0x62,
    MC_PSC_SRC_VIA2_SCSI_DRQ = 0x20,    /* VIA2 bit 0 */
    MC_PSC_SRC_VIA2_SLOT = 0x21,        /* VIA2 bit 1 */
    MC_PSC_SRC_VIA2_SCSI_IRQ = 0x23,    /* VIA2 bit 3 */
    MC_PSC_SRC_VIA2_FDC = 0x25          /* VIA2 bit 5 */
} mc_psc_source;

/* A DMA channel's device: one byte at a time while it requests. */
typedef struct mc_psc_port {
    void *ctx;
    int (*read)(void *ctx, uint8_t *byte);      /* device -> memory: 1 with a byte */
    int (*write)(void *ctx, uint8_t byte);      /* memory -> device: 1 when taken */
    /* ENetWr only (optional): each armed set is one frame, handed whole. */
    void (*frame)(void *ctx, const uint8_t *data, size_t length);
} mc_psc_port;

typedef struct mc_psc_channel {
    uint16_t control;
    struct { uint32_t address, count; uint16_t command; } set[2];
    mc_psc_port port;
    uint8_t request;                /* the device asks for transfers */
    uint8_t running;                /* bytes are moving (a nested request waits for the loop) */
} mc_psc_channel;

typedef struct mc_psc {
    mc_dev dev;
    uint8_t flags[4], enables[4];   /* levels 3-6 */
    uint8_t irq[4];
    uint32_t dma_flags;             /* +$804 */
    mc_psc_channel ch[MC_PSC_CHANNELS];
    /* VIA2 */
    uint8_t via2_inputs, via2_ier, via2_irq, via2_slots;   /* slots: port A levels (1 = idle) */
    uint8_t via2_reg[16];
    /* the 60.15 Hz tick */
    uint8_t tick;
    uint64_t next_tick;
    /* sound */
    uint16_t snd_control;
    uint32_t snd_singer, snd_in, snd_out, snd_next;
    uint16_t snd_length;
    uint8_t snd_pending;            /* a next buffer was given while one plays */
    uint32_t snd_left;              /* bytes left of the buffer playing (0: idle) */
    uint64_t snd_time;              /* guest time the output is made up to */
    uint32_t snd_rem;
    int16_t *ring;                  /* the samples made, stereo */
    size_t ring_size, ring_head, ring_count;
    uint8_t played;                 /* a sound was played to the end */
    uint8_t dsp_control;            /* +$21C */
    /* The DSP's sound frames: while +$200 bit 6 is set, a frame interrupt
     * (MC_PSC_LINE_DSP_FRAME, the DSP's IR1N) every +$218 sample times.
     * The output and input buffers at +$214 and +$210 hold two frames of
     * stereo 16-bit samples each, played (+$200 bit 8) and recorded (bit 7)
     * in turn (the AV kernel writes 240 samples a frame to each half of
     * +$214)†. */
    uint8_t frame_req, frame_missed;
    uint64_t next_frame;
    uint64_t frame_count, frame_played; /* frames since they started, host samples played since */
    uint32_t frame_out, frame_in;       /* the frame buffers (+$214, +$210) */
    mc_audio_source source;             /* the sound input */
    unsigned source_rate;
    uint8_t other[MC_PSC_SIZE];     /* registers kept and read back */
} mc_psc;

void mc_psc_init(mc_psc *p, const mc_hw_host *host, const char *name);
void mc_psc_free(mc_psc *p);
extern const mc_dev_ops mc_psc_ops;         /* offsets from $50F3 1000 */
extern const mc_dev_ops mc_psc_via2_ops;    /* offsets from $50F0 2000 */

/* An interrupt source's level (1 asserted). */
void mc_psc_set_source(mc_psc *p, mc_psc_source source, int asserted);
/* The slots' interrupts on VIA2 port A (bit s - 9 for slot s, 1 asserted). */
void mc_psc_set_slots(mc_psc *p, uint8_t asserted_bits);
/* A channel's device and its request line. */
void mc_psc_set_port(mc_psc *p, unsigned channel, const mc_psc_port *port);
void mc_psc_request(mc_psc *p, unsigned channel, int asserted);

/* ENetRd: a received frame (FCS included) with the MACE's four receive
 * status bytes, into the next 2 KB slot of the armed set (the status bytes
 * in words at +0 to +6, each byte in both halves; the frame from +$10; the
 * count register counts slots). 0 when no set takes it. */
#define MC_PSC_ENET_SLOT 2048u
#define MC_PSC_FRAME_MAX 1536u
int mc_psc_enet_receive(mc_psc *p, const uint8_t *frame, size_t length, const uint8_t status[4]);

/* The DSP acknowledges its frame interrupt (IACK1) and rings the host
 * (its BIO0 output changed: level 5 bit 0). */
void mc_psc_dsp_ack(mc_psc *p);
void mc_psc_dsp_doorbell(mc_psc *p);

/* Sound out: 44.1 kHz; frames made since the last pull. */
#define MC_PSC_SOUND_RATE 44100u
size_t mc_psc_pull(mc_psc *p, int16_t *stereo, size_t frames);
/* The sound input the DSP's frames record (NULL: silence). */
void mc_psc_set_audio_source(mc_psc *p, const mc_audio_source *source);

#endif
