/* Livadix — AMIC, the Apple Memory-Mapped I/O Controller of the Power
 * Macintosh 6100, 7100 and 8100: the interrupt controller in front of the
 * 601's single interrupt, the pseudo-VIA2, the DMA channels (SCSI, the MACE
 * Ethernet, the SWIM III floppy, the SCC, sound) and the sound codec's port.
 * VIA1 is a full 6522 (via.h) on the board at $50F0 0000; the video timing
 * the AMIC also carries is pdm_video.h.
 *
 * Sources: Power Macintosh Computers Developer Note (Apple, 1994): chapter
 * 2, "Apple Memory-Mapped I/O Controller" (interrupts through the VIA
 * channels; DMA for Ethernet, SWIM III, the SCC, SCSI and sound; monitor
 * support; no extended 601 transfers), "CPU Bus Arbitration" and Table 2-4
 * (the DMA requesters), "Address Errors" (the AMIC ends an access to an
 * undecoded address after 40 us), "Interrupt Handling" and Figure 2-2 (the
 * 68000 interrupt levels emulated; the pseudo-VIA1 IFR at $50F0 1A00 and IER
 * at $50F0 1C00; the pseudo-VIA2's slot interrupt register $50F2 6002 (bit
 * 6 VBL, bits 5-2 the slots), its enable $50F2 6012, its interrupt flags
 * $50F2 6003 (bit 7 any, 6 SCSI-B IRQ, 5 the floppy controller, 3 SCSI-A
 * IRQ, 2 SCSI-B DRQ, 1 any slot, 0 SCSI-A DRQ) and enables $50F2 6013,
 * written with the 6522's set and clear convention); chapter 3, "Sound I/O"
 * (four 8 KB buffers; 16-bit stereo in and out).
 *
 * The register layout as ROM 9FEB69B3 drives it (ROM
 * addresses at $FFC0 0000; † marks what the ROM does not settle):
 *
 * $50F2 A000, the interrupt controller (named in the nanokernel's
 * configuration record "Boot PDM 601 1.0", $FFF0C094):
 *   +$00 read: bits 5-0 the pending sources: 0 VIA1, 1 the pseudo-VIA2, 2
 *        the SCC, 3 the Ethernet controller, 4 the DMA channels, 5 NMI; the
 *        nanokernel's external interrupt handler writes $C0, reads this
 *        byte and maps bits 5-0 to a 68000 level through its table at
 *        $FFF10740 (VIA1 1, VIA2 2, Ethernet 3, SCC and DMA 4, NMI 7;
 *        $FFF107BC-$FFF107F4); bits 7-6 read back as written. Livadix's
 *        reading†: any change of bits 5-0 raises the 601's INT, and a
 *        write with bit 7 set (the handler's $C0 before its read)
 *        releases it: the nanokernel learns each new level, also when a
 *        source goes away. Held as a level, INT keeps the 601 in the
 *        handler while the 68k code polls with interrupts masked (the
 *        ROM's start-up VIA timer loop); taken only on new sources, the
 *        68k emulator keeps a level that is gone pending and loops in
 *        the level 1 handler (both observed on the ROM);
 *   +$08, +$0A: the DMA channels' interrupt flags, bits 7-0 and 9-8: the
 *        68k level 4 handler reads +$0A then +$08 when bit 2 is clear and
 *        dispatches the highest set bit through a table of ten ($FFC10682-
 *        $FFC106BE); the drivers install bit 6 the floppy ($FFD8EB48), 8
 *        sound out ($FFDCFE04), 9 sound in ($FFDDD414), 5 the Ethernet
 *        ($FFEF0C6C), 0-3 the serial channels ($FFD9BFBC-$FFD9C00C);
 *        Livadix sets bits 4 and 7 for the SCSI channels A and B†. A flag
 *        follows its channel's own interrupt bit and clears with it.
 * $50F2 6000, the pseudo-VIA2 (registers in the low 5 address bits; the
 *        68k code reaches them at +$1C13 and +$1A03, as RBV-style VIA2s,
 *        so bits 12-9 are ignored): $00 port B (written $8F, kept), $02 the
 *        slot flags (active low; bit 6 VBL, bits 5-2 slots, bits 1-0 read
 *        1), $03 the flags, $10 kept ($40 written), $12 slot enables (bits
 *        6-2), $13 enables (bits 6-0); the others kept†. Bit 4 of the
 *        flags is enabled by the ROM ($FFC10254: $90) for a source the
 *        Developer Note does not show: Livadix leaves it to the machine†.
 * $50F3 1000, the DMA channels (the start-up chime resets eight channel
 *        control bytes by setting bit 0: +$1008, +$0C20, +$1028, +$1068,
 *        +$1088, +$1098, +$10A8, +$10B8, $FFCE5CC8):
 *   +$0000 the DMA area's base, bits 31-16 in bytes 0-1 (the chime rounds
 *        its buffer to 256 KB and writes $50F3 1001 then $50F3 1000,
 *        $FFCE5B3A-$FFCE5B58; the area is $28000 bytes long, $FFCE5B74);
 *        the channels' fixed buffers lie in it: sound out at +$10000 and
 *        +$12000 (the POST's and the chime's physical buffers,
 *        $FFF056DC, $FFCE5B64), the others at Livadix's offsets†
 *        (MC_AMIC_AREA_*);
 *   +$0C20 Ethernet transmit control, +$1028 Ethernet receive control
 *        (the PDM MACE driver, $FFEF0694-$FFEF0C52): bit 7 reads the
 *        channel's interrupt and a write with bit 7 clears it; bit 6 of the
 *        receive control: the ring filled (cleared by a write with bit 6);
 *        bits 3 and 1 the enable and run the driver sets ($8A)†; bit 0 a
 *        reset; +$1030 the next receive slot the AMIC fills, +$1034 the
 *        next the driver takes: a ring of $C0 slots of $100 bytes at the
 *        area's start, a frame in consecutive slots after an 8-byte header
 *        (its first word the MACE's status nibble and 12-bit count, its
 *        second the two other status bytes, $FFEF07FE-$FFEF0812,
 *        $FFEF09FA-$FFEF0A40); +$1044 + $10n the byte count of transmit
 *        buffer n, at the area + $14000 + $800n (the driver's logical
 *        $6100 0000 is the area, $FFEF0B3A-$FFEF0B52; it fills buffer 0
 *        only, $FFEF0748, and the start sends it: buffer 1 †); bits 6-5 of
 *        the transmit control read set while the channel can take a frame
 *        (the driver refuses to send, by _Debugger, when both are clear,
 *        $FFEF071A; Livadix's transmissions end at once, so they always read
 *        set†);
 *   +$1000, +$1004 the SCSI A and B DMA addresses, +$1008, +$1009 their
 *        control bytes (the SCSI Manager's PDM set-up, $FFC2E0D0-$FFC2E182;
 *        the POST's AMIC revision test sets bits 3-2 of +$1008 and reads
 *        them back, $FFF032B8): bit 1 runs the channel, bit 6 set from
 *        memory to the device (StartAMIC sets it unless its direction
 *        argument is nonzero, $FFC2A5C0-$FFC2A5D4), bit 4 a
 *        flush the HAL waits on (Wt4AMICComplete, $FFC2A612; Livadix's
 *        channels hold nothing, it reads clear at once); bit 0 a reset,
 *        bit 7 the interrupt†;
 *   +$1060 the floppy's DMA address (its driver writes only bytes 2-3
 *        and reads the full address to find its buffer, $FFD8EB00-
 *        $FFD8EB3C), +$1064 its count (16-bit, $80 for a sector; Livadix
 *        counts 4-byte units†), +$1068 its control (bit 0 reset, bit 1
 *        run, bit 6 memory to device, $FFD8FB1E-$FFD8FB2A, $FFD8FF2E);
 *   +$1080-$10BF the serial channels' registers (the "SerialHALAMIC"
 *        driver): kept†, no serial DMA;
 *   every other byte of the window kept and read back.
 * $50F1 4000, sound (the POST's beeps $FFF0519C-$FFF05338 and the
 *        start-up chime $FFCE59E2-$FFCE5CA8):
 *   +$00 codec port: write $40, the word to +$01 (high) and +$02 (low),
 *        then $C0; bit 7 reads set until the word has gone to the codec
 *        (the ROM polls it, $FFCE5A76); +$04-$07 the codec's status;
 *   +$08, +$09 written 4 and 0: the buffers' size in KB†;
 *   +$0C-$0E the output's progress: nonzero while it plays (the chime
 *        waits for bits 17-6 to read 0 after stopping it, $FFCE5C8C);
 *   +$10 output control: bit 0 runs the output DMA, bits 3-2 the rate
 *        (the chime writes 4: 11025 Hz x 2^n†);
 *   +$11 input control, kept†;
 *   +$14 interrupt flags and enables: bits 7 and 6 the output buffers 1
 *        and 0 played, bits 5 and 4 the input buffers†, a write clears the
 *        flags it has set; bits 3-2 the enables (input, output†) ($F0,
 *        then $0C at the sound driver's start, $FFDCFE30; its handler,
 *        $FFDCFB9C);
 *   +$18 the same flags without enables, polled by the POST and the chime
 *        (bit 6: refill buffer 0, bit 7: buffer 1; written back to clear).
 *        The output plays 16-bit stereo big-endian frames (left first) from
 *        buffer 0, then 1, then 0, as long as bit 0 of +$10 stays set.
 * $50F2 C000 bit 0: 0 makes the POST enter its diagnostic monitor on the
 *        serial port ($FFF04E94); Livadix reads 1.
 *
 * The 60.15 Hz tick to VIA1 CA1 (Figure 2-2: VIA1 IFR bit 1): a square
 * wave of the classic period†.
 *
 * Output lines (mc_hw_host line, 1 = asserted):
 *   MC_AMIC_LINE_CPU   the 601's external interrupt
 *   MC_AMIC_LINE_TICK  VIA1 CA1's 60.15 Hz (the level)
 *
 * The Workgroup Server 9150 Developer Note (1995), "FAT AMIC IC",
 * Figure 2, adds slot bits 1-0 (NuBus $A on bit 1) and identifies
 * FAT AMIC 1/2 by read-only bit 5 of both SCSI DMA control registers.
 *
 * Livadix's own code. */
#ifndef MC_AMIC_H
#define MC_AMIC_H

#include "hw.h"

#include <stddef.h>
#include <stdint.h>

struct mc_awac;

enum { MC_AMIC_LINE_CPU = 0, MC_AMIC_LINE_TICK = 1 };

/* The windows (offsets from each base). */
#define MC_AMIC_INT_SIZE 0x1000u        /* $50F2 A000 */
#define MC_AMIC_VIA2_SIZE 0x2000u       /* $50F2 6000 */
#define MC_AMIC_DMA_SIZE 0x2000u        /* $50F3 1000 */
#define MC_AMIC_SOUND_SIZE 0x1000u      /* $50F1 4000 */
#define MC_AMIC_DIAG_SIZE 0x1000u       /* $50F2 C000 */

/* Interrupt sources of +$00 (bits). */
enum {
    MC_AMIC_SRC_VIA1 = 0, MC_AMIC_SRC_VIA2 = 1, MC_AMIC_SRC_SCC = 2, MC_AMIC_SRC_ENET = 3,
    MC_AMIC_SRC_DMA = 4, MC_AMIC_SRC_NMI = 5
};

/* The pseudo-VIA2's flags. */
enum {
    MC_AMIC_VIA2_SCSI_A_DRQ = 0x01, MC_AMIC_VIA2_SLOT = 0x02, MC_AMIC_VIA2_SCSI_B_DRQ = 0x04,
    MC_AMIC_VIA2_SCSI_A_IRQ = 0x08, MC_AMIC_VIA2_BIT4 = 0x10, MC_AMIC_VIA2_FDC = 0x20, MC_AMIC_VIA2_SCSI_B_IRQ = 0x40
};

/* The DMA flags' bits (+$08/+$0A). */
enum {
    MC_AMIC_DMA_SERIAL0 = 0, MC_AMIC_DMA_SERIAL1 = 1, MC_AMIC_DMA_SERIAL2 = 2, MC_AMIC_DMA_SERIAL3 = 3,
    MC_AMIC_DMA_SCSI_A = 4, MC_AMIC_DMA_ENET = 5, MC_AMIC_DMA_FLOPPY = 6, MC_AMIC_DMA_SCSI_B = 7,
    MC_AMIC_DMA_SND_OUT = 8, MC_AMIC_DMA_SND_IN = 9
};

/* Byte channels the devices feed (DREQ-driven). */
enum { MC_AMIC_CH_SCSI_A = 0, MC_AMIC_CH_SCSI_B = 1, MC_AMIC_CH_FLOPPY = 2, MC_AMIC_BYTE_CHANNELS = 3 };

/* Livadix's offsets of the fixed buffers in the DMA area (sound out from the
 * ROM; the rest †). */
#define MC_AMIC_AREA_SIZE 0x28000u
#define MC_AMIC_AREA_ENET_RX 0x00000u   /* $C0 slots of $100 */
#define MC_AMIC_AREA_SND_OUT 0x10000u   /* two buffers, $2000 apart */
#define MC_AMIC_AREA_ENET_TX 0x14000u   /* two buffers of $800 ($FFEF0B3A-$FFEF0B52) */
#define MC_AMIC_AREA_FLOPPY 0x18000u
#define MC_AMIC_ENET_SLOTS 0xC0u
#define MC_AMIC_ENET_SLOT 0x100u
#define MC_AMIC_ENET_TX_BUF 0x800u

typedef struct mc_amic_port {
    void *ctx;
    int (*read)(void *ctx, uint8_t *byte);      /* device -> memory: 1 with a byte */
    int (*write)(void *ctx, uint8_t byte);      /* memory -> device: 1 when taken */
} mc_amic_port;

typedef struct mc_amic_channel {
    mc_amic_port port;
    uint8_t request;
    uint8_t running;            /* the transfer loop is active (no nesting) */
} mc_amic_channel;

#define MC_AMIC_SOUND_RATE 44100u

enum { MC_AMIC_STANDARD = 0, MC_AMIC_FAT_1 = 1, MC_AMIC_FAT_2 = 2 };

typedef struct mc_amic {
    mc_dev dev;
    uint8_t variant;            /* board-selected AMIC revision; survives reset */
    /* Interrupts. */
    uint8_t sources;            /* +$00 bits 5-0 asserted */
    uint8_t int_ctl;            /* bits 7-6 */
    uint8_t cpu;                /* the line as reported */
    uint8_t latch, seen;        /* a source asserted since the last acknowledge; the sources then */
    uint16_t dma_flags;
    /* Pseudo-VIA2. */
    uint8_t via2_reg[32];
    uint8_t via2_ifr, via2_ier, via2_slots, via2_sier, via2_irq;
    /* DMA. */
    uint8_t dma_reg[MC_AMIC_DMA_SIZE];
    mc_amic_channel ch[MC_AMIC_BYTE_CHANNELS];
    void (*enet_transmit)(void *ctx, const uint8_t *frame, size_t length);
    void *enet_ctx;
    /* Sound. */
    uint8_t snd_reg[0x20];
    uint8_t snd_flags14, snd_flags18;
    uint64_t codec_busy_until;
    struct mc_awac *codec;
    unsigned snd_buf;           /* the output buffer playing */
    uint32_t snd_pos;           /* bytes of it played */
    int snd_played;             /* output DMA has played since reset */
    uint64_t snd_time;          /* guest time the output is made up to */
    uint64_t snd_acc;           /* rate accumulator */
    int16_t snd_frame[2];       /* the source frame being held */
    int16_t *ring;
    size_t ring_size, ring_head, ring_count;
    /* The tick. */
    uint64_t tick_start;
    uint8_t tick;
    uint8_t diag;
} mc_amic;

extern const mc_dev_ops mc_amic_int_ops;     /* $50F2 A000 */
extern const mc_dev_ops mc_amic_via2_ops;    /* $50F2 6000 */
extern const mc_dev_ops mc_amic_dma_ops;     /* $50F3 1000 */
extern const mc_dev_ops mc_amic_sound_ops;   /* $50F1 4000 */
extern const mc_dev_ops mc_amic_diag_ops;    /* $50F2 C000 */

void mc_amic_init(mc_amic *a, const mc_hw_host *host, const char *name);
void mc_amic_free(mc_amic *a);
/* The sound codec on the AMIC's codec port (may be NULL). */
void mc_amic_set_codec(mc_amic *a, struct mc_awac *codec);

/* An interrupt source of +$00 (MC_AMIC_SRC_*; VIA2 and DMA are the AMIC's
 * own and follow their registers). */
void mc_amic_set_source(mc_amic *a, unsigned source, int asserted);
/* A pseudo-VIA2 input (MC_AMIC_VIA2_* other than SLOT; a level). */
void mc_amic_set_via2(mc_amic *a, uint8_t bit, int asserted);
/* The slot lines of the pseudo-VIA2 (bit 6 VBL, bits 5-2 the slots;
 * 1 = asserted) under `mask`. */
void mc_amic_set_slots(mc_amic *a, uint8_t asserted, uint8_t mask);

/* A byte channel's device and its request line. */
void mc_amic_set_port(mc_amic *a, unsigned channel, const mc_amic_port *port);
void mc_amic_request(mc_amic *a, unsigned channel, int asserted);
/* The SWIM III's interrupt line (the floppy channel's control bit 3). */
void mc_amic_floppy_irq(mc_amic *a, int asserted);

/* Ethernet: a received frame (FCS included) with the MACE's four receive
 * status bytes, into the receive ring; 0 when the ring has no room or the
 * channel does not run. */
int mc_amic_enet_receive(mc_amic *a, const uint8_t *frame, size_t length, const uint8_t status[4]);
/* Where transmitted frames go (the MACE's transmit). */
void mc_amic_set_enet_transmit(mc_amic *a, void (*transmit)(void *ctx, const uint8_t *frame, size_t length), void *ctx);

/* The DMA area's base (physical). */
uint32_t mc_amic_dma_base(const mc_amic *a);

/* Sound out at MC_AMIC_SOUND_RATE: the frames made since the last pull,
 * through the codec's output attenuation (mc_psc_pull's contract). */
size_t mc_amic_pull(mc_amic *a, int16_t *stereo, size_t frames);

#endif
