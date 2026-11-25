/* Livadix — Apple Sound Chip (ASC) and Enhanced ASC (EASC); see asc.h.
 *
 * Sources: Guide to the Macintosh Family Hardware, 2nd ed., ch. 13 "Sound"
 * ("ASC sound system": 2 KiB of on-chip RAM as two 1 KiB FIFOs or four
 * 512-byte wave tables; mono and stereo single-voice and four-voice modes;
 * 24-bit sampling increments; 22.255 and 44.1 kHz rates; eight volume
 * steps, 0 silent); Macintosh Quadra 700 Developer Note ("Sound ICs": the
 * Enhanced ASC keeps the ASC's modes and adds recording). The EASC's
 * registers as the Quadra 700 ROM (420DBFF3) programs and tests them:
 * asc.h lists each with the ROM code it was read from.
 * Sample clock C15M/704.
 */
#include "asc.h"

#include <string.h>

enum {
    R_VERSION = 0x800, R_MODE = 0x801, R_CONTROL = 0x802, R_FIFO_MODE = 0x803, R_IRQ = 0x804,
    R_WTCTL = 0x805, R_VOLUME = 0x806, R_CLOCK = 0x807, R_PLAYREC = 0x80A, R_TEST = 0x80F,
    R_WAVE = 0x810, R_WAVE_END = 0x830, R_EXT = 0xF00, R_EXT_END = 0xF40, R_SBLOCK = 0xF40, R_SBLOCK_END = 0xF50,
    R_PORT16 = 0x1000
};

/* EASC per-FIFO register offsets within its $20-byte group (asc.h). */
enum { X_RATE = 4, X_LVOL = 6, X_RVOL = 7, X_FCTL = 8, X_ICTL = 9, X_XA = 0x10 };

#define VERSION_ASC 0x00        /* the original ASC */
#define VERSION_EASC 0xB0       /* the ROM's test for an EASC (asc.h) */
#define ONE 0x10000u            /* rate accumulator: one FIFO byte */
#define HALF (MC_ASC_FIFO / 2)
#define EASC_HZ 44100u          /* the EASC's rate reference (asc.h, $F04) */

/* Status bits of $804. */
#define ST_HALF(c) (1u << (2 * (c)))
#define ST_EDGE(c) (2u << (2 * (c)))

static int easc(const mc_asc *a) { return a->type == MC_ASC_ENHANCED; }
static uint8_t *xgroup(mc_asc *a, int c) { return a->xreg + 0x20 * c; }
/* The PowerBook 520's and 190's sound block, beside the EASC (ROMs
 * B6909089 and 4D27039C as they program it†):
 *   $F40  mode, read back as written (the ROMs and the System change it by
 *         read-modify-write: btst/ori/andi at $4000F8D6, $4000FE0C):
 *         bits 0-1 the 16-bit ports' sample rate, 44,100, 22,050, 11,025
 *         Hz or none (the table at $4000FD94 the Sound Manager's output
 *         component reads its rate from, $4000FD7E); bits 5-6 the power
 *         state ($40006422: bit 5 on, bit 6 off); bits 2-3 stored (the
 *         190's chime sets them, the System never does).
 *   $F44  long, $F48 and $F4A words: read back as written (the ROM's
 *         buffer address, the ROM's and the output component's mute bit
 *         10 of $F48 and the volume ramp of $F4A).
 *   $1000, $1800: FIFO A's and FIFO B's 16-bit ports, one big-endian
 *         sample per word. The 190's chime (a sound with an extended
 *         header, 22,050 Hz, two channels, 16 bits; $4012FA66) writes a
 *         word to each per frame while $804 shows FIFO B below half; the
 *         520's ROM a word of 0 to each before its chime; System 7.5.3's
 *         output component, on each FIFO B below-half interrupt, 512
 *         frames, so each FIFO holds 1024 samples, below half under 512.
 * A FIFO plays from its 16-bit port after that port was written, from
 * the EASC's byte port (and its $F04/$F24 rate) after that one was (Mac
 * OS 8.0's Sound Manager on the 520 sends 8-bit sounds as bytes to +$000
 * and +$400, RAM code at $452EB4). The
 * hardware keeps the 16-bit samples in a RAM buffer (the ROM puts its
 * address in $F44); Livadix keeps them in the chip (wram). The PowerBook 1400
 * and Duo 2300c Developer Notes, ch. 2 "Singer IC", also describe Whitney's
 * main-memory sound buffers, but give no layout. Their original ROMs write
 * $00010000 and subsequently $01FC0000 at 32 MiB to $F44; that establishes
 * the programmed value, not sample addressing within the buffer. */
static int wide(const mc_asc *a, int c) { return a->block && ((a->wide >> c) & 1u); }
static unsigned fifo_size(const mc_asc *a, int c) { return wide(a, c) ? MC_ASC_WFIFO : MC_ASC_FIFO; }
static unsigned fifo_count(const mc_asc *a, int c) { return wide(a, c) ? a->fifo[c].wcount : a->fifo[c].count; }
/* FIFO bytes per output sample, in 1/$10000: the ASC takes one per tick;
 * the EASC's per-FIFO increment ($F04/$F24). */
static uint32_t rate_incr(mc_asc *a, int c) {
    if (!easc(a)) return ONE;
    if (wide(a, c)) return (a->breg[0] & 3) == 3 ? 0 : ONE >> (a->breg[0] & 3);   /* of the 44.1 kHz output */
    const uint8_t *x = xgroup(a, c);
    return (uint32_t)x[X_RATE] << 8 | x[X_RATE + 1];
}
/* The EASC always plays both FIFOs (A and B each mixed to both sides by
 * their volumes); the ASC's FIFO B plays in stereo only. */
static int stereo(const mc_asc *a) { return easc(a) || (a->control & 2) != 0; }
/* $80A bit 0 records into FIFO A: the EASC's, and the ASC of the LC
 * family's controllers (asc.h, input: the Color Classic's sound input
 * driver under System 7.5.5 sets it and clears the FIFOs, as on the
 * EASC). */
static int records(const mc_asc *a) { return easc(a) || a->input; }
static int recording(const mc_asc *a) { return records(a) && (a->play_rec & 1); }
static int singer_recording(const mc_asc *a) { return a->singer && recording(a); }

static uint32_t singer_record_incr(const mc_asc *a) {
    /* Singer's 11.025/22.05/44.1 kHz input rates (1400 Developer Note,
     * Sound System) use the portable block's $F40 rate selection. */
    const unsigned rate = a->breg[0] & 3u;
    return rate < 3 ? ONE >> rate : 0;
}

static int fifo_running(const mc_asc *a) { return a->mode == 1; }
static int wavetable(const mc_asc *a) { return a->mode == 2; }

/* The rate FIFO A records at: on the EASC a byte per FIFO A rate step of
 * the 44.1 kHz reference ($F04, asc.h); on the LC family's ASC a byte per
 * tick of its sample clock. */
static unsigned record_rate(const mc_asc *a) {
    if (!easc(a)) return (unsigned)(a->clock == 2 ? 22050u : a->clock == 3 ? 44100u : (MC_C15M_HZ + 352u) / 704u);
    return (unsigned)(((uint64_t)EASC_HZ * rate_incr((mc_asc *)a, 0) + ONE / 2) / ONE);
}
/* The rate the sound input runs at: Singer's, or FIFO A's recording (the
 * EASC's in FIFO mode), 0 when nothing records. */
static unsigned singer_input_rate(const mc_asc *a) {
    if (!recording(a) || (easc(a) && !fifo_running(a))) return 0;
    if (!a->singer) return record_rate(a);
    const unsigned rate = a->breg[0] & 3u;
    return rate < 3 ? EASC_HZ >> rate : 0;
}
static void input_changed(mc_asc *a, unsigned before) {
    const unsigned after = singer_input_rate(a);
    if (before != after && a->source.enable) a->source.enable(a->source.ctx, after);
}

static uint64_t now(const mc_asc *a) { return mc_dev_now(&a->dev); }

static uint64_t ticks_at(const mc_asc *a, uint64_t t) {
    if (t <= a->base_time) return a->base_ticks;
    return a->base_ticks + (t - a->base_time) * a->num / a->den;
}
static uint64_t time_of_tick(const mc_asc *a, uint64_t k) {
    if (k <= a->base_ticks) return a->base_time;
    uint64_t span = (k - a->base_ticks) * a->den;
    return a->base_time + (span + a->num - 1) / a->num;
}

/* Clock select of $807 (Guide ch. 13: 22.255 kHz, the PWM rate, or 44.1 kHz;
 * code values †). The EASC's output runs at its 44.1 kHz rate reference. */
static void set_clock(mc_asc *a) {
    a->num = 1; a->den = 704;
    if (easc(a)) { a->num = EASC_HZ; a->den = MC_C15M_HZ; }
    else if (a->clock == 2) { a->num = 22050; a->den = MC_C15M_HZ; }
    else if (a->clock == 3) { a->num = 44100; a->den = MC_C15M_HZ; }
}
static void rebase(mc_asc *a, uint64_t t) { a->base_time = t; a->base_ticks = a->ticks; }

/* The EASC's status is the FIFOs' state: in playback, below half (bit 0/2)
 * and full or empty (bit 1/3); FIFO A recording, below half (bit 0) and
 * full (bit 1). */
static uint8_t easc_status(const mc_asc *a) {
    uint8_t st = 0;
    for (int c = 0; c < 2; c++) {
        const unsigned n = fifo_count(a, c), size = fifo_size(a, c);
        if (c == 0 && recording(a)) {
            if (singer_recording(a)) {
                /* The B0-revision PBX ROM's input service at $40090090
                 * branches to the callback when $804 bit 0 is clear. */
                if (a->capture_count[0] < MC_ASC_WFIFO / 2) st |= ST_HALF(0);
                if (a->capture_count[0] == MC_ASC_WFIFO) st |= ST_EDGE(0);
            } else {
                /* As Singer's: bit 0 while less than half has come in.
                 * System 7.5.5's EASC interrupt handler (the LC 475's, at
                 * $9441E in RAM) calls FIFO A's routine on revision $B0
                 * only with bit 0 clear, and the sound input driver
                 * empties the FIFO from there. */
                if (n < size / 2) st |= ST_HALF(0);
                if (n == size) st |= ST_EDGE(0);
            }
        } else {
            if (n < size / 2) st |= ST_HALF(c);
            if (n == 0 || n == size) st |= ST_EDGE(c);
        }
    }
    return st;
}
/* EASC interrupt: a FIFO's events (falling below half and running empty,
 * filling up; recording, reaching half and full; a clear) are latched until
 * $804 is read; a FIFO whose interrupt is not masked ($F09/$F29 bit 0
 * clear) asserts /IRQ while it has one latched. */
/* The EASC's unmasked latched events. */
static uint8_t easc_events(const mc_asc *a) {
    return (uint8_t)(((a->xreg[X_ICTL] & 1) ? 0 : a->irq_status & 3) | ((a->xreg[0x20 + X_ICTL] & 1) ? 0 : a->irq_status & 12));
}
/* An EASC event latched while /IRQ is already asserted interrupts again:
 * the line is released and asserted, an edge for VIA2 CB1. The LC 475's
 * sound input (System 7.5.5) unmasks FIFO A with older events latched,
 * starts recording, clears the FIFOs and VIA2's CB1 flag (ROM FF7439EE
 * $4080A05A) and then waits for the half-full interrupt without reading
 * $804†. */
static void update_irq(mc_asc *a) {
    const uint8_t events = easc(a) ? easc_events(a) : 0;
    const int level = easc(a) ? events != 0 : (a->irq_status & 0x0F) != 0;
    const uint8_t fresh = (uint8_t)(events & ~a->irq_events);
    a->irq_events = events;
    if (level && a->irq && fresh) {
        mc_dev_line(&a->dev, MC_ASC_LINE_IRQ, 0);
        mc_dev_line(&a->dev, MC_ASC_LINE_IRQ, 1);
    } else if (level != a->irq) {
        a->irq = level;
        mc_dev_line(&a->dev, MC_ASC_LINE_IRQ, level);
    }
}

static void emit(mc_asc *a, int l, int r) {
    int v = a->volume >> 5;               /* eight steps, 0 silent (Guide ch. 13) */
    l = l * v / 7; r = r * v / 7;
    if (l > 32767) l = 32767; else if (l < -32768) l = -32768;
    if (r > 32767) r = 32767; else if (r < -32768) r = -32768;
    if (a->ring_count == MC_ASC_RING) {
        a->ring_rd = (a->ring_rd + 1) % MC_ASC_RING; a->ring_count--; a->dropped++;
    }
    size_t at = (a->ring_rd + a->ring_count) % MC_ASC_RING;
    a->ring[at][0] = (int16_t)l; a->ring[at][1] = (int16_t)r;
    a->ring_count++;
}

/* The next byte of FIFO c (0 on an underrun, which `ok` reports). */
static uint8_t take(mc_asc *a, int c, int *ok) {
    mc_asc_fifo *f = &a->fifo[c];
    *ok = f->count != 0;
    if (!*ok) return 0;
    const uint8_t b = a->ram[c * MC_ASC_FIFO + f->rd];
    f->rd = (uint16_t)((f->rd + 1) % MC_ASC_FIFO);
    f->count--;
    a->played++;
    if (f->count == HALF - 1) a->irq_status |= ST_HALF(c);   /* fell below half: refill */
    if (f->count == 0) a->irq_status |= ST_EDGE(c);           /* ran empty */
    return b;
}

/* The next sample of the sound block's 16-bit FIFO c, if it has one. */
static int take16(mc_asc *a, int c, int16_t *v) {
    mc_asc_fifo *f = &a->fifo[c];
    if (!f->wcount) return 0;
    *v = a->wram[c][f->wrd];
    f->wrd = (uint16_t)((f->wrd + 1) % MC_ASC_WFIFO);
    f->wcount--;
    a->played += 2;
    if (f->wcount == MC_ASC_WFIFO / 2 - 1) a->irq_status |= ST_HALF(c);
    if (f->wcount == 0) a->irq_status |= ST_EDGE(c);
    return 1;
}

/* EASC decoding, FIFO control ($F08) bits 0-1 = 2: groups of 15 bytes, a
 * header (bits 5-4 the prediction filter, bits 3-0 the shift) and 28 4-bit
 * samples, low nibble first; each sample is the nibble shifted to the top
 * of 16 bits and right by the shift, plus the filter's prediction from the
 * two previous samples, K0 x s1 + K1 x s2 in 1/64ths, with K0 and K1 the
 * second and first signed byte of the filter's pair at $F10 + 2f. The
 * layout is what the ROM's chime (compression ID 6, 120-bit packets, which
 * the ROM sends with this mode, $408BE298) decodes to: a clean decaying
 * chord with this order and filter assignment, noise with the others. */
static int decode(mc_asc *a, int c, int16_t *out) {
    mc_asc_fifo *f = &a->fifo[c];
    int ok = 1;
    if (f->xa_pos == 0) {
        /* A group starts only with its header and first data byte both in
         * the FIFO: taking the header alone on an underrun would lose it
         * (the next byte read as the header garbles the rest of the
         * stream): the Quadra 700 ROM's chime garbled on the left when a
         * sample tick fell between its first bytes to FIFO A and to B. */
        if (f->count < 2) return 0;
        f->xa_hdr = take(a, c, &ok);
        if (!ok) return 0;
    }
    int nib;
    if (!(f->xa_pos & 1)) {
        f->xa_byte = take(a, c, &ok);
        if (!ok) return 0;
        nib = f->xa_byte & 15;
    } else nib = f->xa_byte >> 4;
    f->xa_pos = (uint8_t)((f->xa_pos + 1) % 28);
    if (nib >= 8) nib -= 16;
    const uint8_t *k = xgroup(a, c) + X_XA + 2 * ((f->xa_hdr >> 4) & 3);
    const int k0 = (int8_t)k[1], k1 = (int8_t)k[0];
    int v = (nib * 4096) >> (f->xa_hdr & 15);
    v += (f->s1 * k0 + f->s2 * k1 + 32) >> 6;
    if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
    f->s2 = f->s1;
    f->s1 = v;
    *out = (int16_t)v;
    return 1;
}

/* One output sample's worth of FIFO consumption for channel c. */
static void consume(mc_asc *a, int c) {
    mc_asc_fifo *f = &a->fifo[c];
    f->acc += rate_incr(a, c);
    while (f->acc >= ONE) {
        f->acc -= ONE;
        if (wide(a, c)) {
            int16_t v;
            if (take16(a, c, &v)) f->out = v;   /* underrun: the last sample holds */
            continue;
        }
        if (easc(a) && (xgroup(a, c)[X_FCTL] & 3) == 2) {
            int16_t v;
            if (decode(a, c, &v)) f->out = v;
            continue;
        }
        int ok;
        const uint8_t b = take(a, c, &ok);
        if (ok) f->out = (int16_t)(((int)b - 128) * 256);   /* underrun: the last sample holds */
    }
}

/* Recording ($80A bit 0): FIFO A fills from the sound input at its rate, a
 * byte per sample in offset binary as the FIFOs play ($80 silence), the
 * input's two channels mixed (the DFAC's mono input). Without a source it
 * records silence. */
static void record(mc_asc *a) {
    mc_asc_fifo *f = &a->fifo[0];
    f->acc += rate_incr(a, 0);
    if (f->acc < ONE) return;
    f->acc -= ONE;
    if (f->count == MC_ASC_FIFO) return;
    int16_t frame[2] = {0, 0};
    if (a->source.sample) a->source.sample(a->source.ctx, record_rate(a), frame);
    a->ram[f->wr] = (uint8_t)((((int)frame[0] + frame[1]) / 2 >> 8) + 128);
    f->wr = (uint16_t)((f->wr + 1) % MC_ASC_FIFO);
    f->count++;
    if (f->count == HALF) a->irq_status |= ST_HALF(0);
    if (f->count == MC_ASC_FIFO) a->irq_status |= ST_EDGE(0);
}


static void singer_record(mc_asc *a) {
    const uint32_t inc = singer_record_incr(a);
    a->capture_phase += inc;
    if (a->capture_phase < ONE) return;
    a->capture_phase -= ONE;
    int16_t frame[2] = {0, 0};
    if (a->source.sample) a->source.sample(a->source.ctx, EASC_HZ >> (a->breg[0] & 3u), frame);
    for (unsigned c = 0; c < 2; c++) {
        if (a->capture_count[c] == MC_ASC_WFIFO) continue;
        a->capture[c][a->capture_wr[c]] = frame[c];
        a->capture_wr[c] = (uint16_t)((a->capture_wr[c] + 1u) % MC_ASC_WFIFO);
        a->capture_count[c]++;
        if (c == 0) {
            if (a->capture_count[0] == MC_ASC_WFIFO / 2) a->irq_status |= ST_HALF(0);
            if (a->capture_count[0] == MC_ASC_WFIFO) a->irq_status |= ST_EDGE(0);
        }
    }
}

static void clear_capture(mc_asc *a) {
    a->capture_phase = 0;
    for (unsigned c = 0; c < 2; c++) {
        a->capture_rd[c] = a->capture_wr[c] = a->capture_count[c] = a->capture_word[c] = 0;
    }
}

static uint16_t singer_input(mc_asc *a, unsigned c) {
    if (!a->capture_count[c]) return 0;
    const uint16_t v = (uint16_t)a->capture[c][a->capture_rd[c]];
    a->capture_rd[c] = (uint16_t)((a->capture_rd[c] + 1u) % MC_ASC_WFIFO);
    a->capture_count[c]--;
    return v;
}

static void tick(mc_asc *a) {
    int l = 0, r = 0;
    if (wavetable(a)) {
        /* Four voices from 512-byte tables, summed (Guide ch. 13, "ASC sound
         * modes"); the 24-bit phase holds 9 integer bits †. Stereo sums
         * voices 0+1 to the left and 2+3 to the right †. */
        int s[4];
        for (int n = 0; n < 4; n++) {
            s[n] = (int)a->ram[n * 512 + ((a->phase[n] >> 15) & 0x1FF)] - 128;
            a->phase[n] = (a->phase[n] + a->incr[n]) & 0xFFFFFFu;
        }
        if (stereo(a)) { l = (s[0] + s[1]) << 7; r = (s[2] + s[3]) << 7; }
        else l = r = (s[0] + s[1] + s[2] + s[3]) << 6;
        emit(a, l, r);
        return;
    }
    if (!fifo_running(a)) {
        /* The LC family's ASC records with the mode left at 0: the Color
         * Classic's driver sets only $80A and clears the FIFOs. */
        if (!easc(a) && recording(a)) record(a);
        emit(a, 0, 0);                       /* off: silence */
        return;
    }
    if (singer_recording(a)) singer_record(a);
    else if (recording(a)) record(a);
    else consume(a, 0);
    if (stereo(a)) consume(a, 1);
    const int sa = recording(a) ? 0 : a->fifo[0].out;   /* FIFO A is filling, not playing */
    if (easc(a)) {
        /* Each FIFO to both sides by its left and right volume, $7F full
         * (the ROM's FIFO A left, FIFO B right). */
        const int sb = a->fifo[1].out;
        const uint8_t *xa = xgroup(a, 0), *xb = xgroup(a, 1);
        l = (sa * xa[X_LVOL] + sb * xb[X_LVOL]) / 0x7F;
        r = (sa * xa[X_RVOL] + sb * xb[X_RVOL]) / 0x7F;
    } else {
        l = sa;
        r = stereo(a) ? a->fifo[1].out : l;
    }
    emit(a, l, r);
}

static void sync_to(mc_asc *a, uint64_t t) {
    uint64_t target = ticks_at(a, t);
    while (a->ticks < target) {
        tick(a);
        a->ticks++;
        if (easc(a) && easc_events(a) != a->irq_events) update_irq(a);   /* an event inside the span */
    }
    update_irq(a);
}
static void sync(mc_asc *a) { sync_to(a, now(a)); }

/* Arms the deadline at the first sample tick that can raise a status bit:
 * a FIFO falling below half or running empty, or (record) reaching half or
 * full. Writes only postpone those ticks, so a deadline that arrives early
 * just re-arms. */
static void reschedule(mc_asc *a) {
    uint64_t best = UINT64_MAX;
    if (fifo_running(a) || (!easc(a) && recording(a))) {
        for (int c = 0; c < (stereo(a) ? 2 : 1); c++) {
            mc_asc_fifo *f = &a->fifo[c];
            uint64_t t;
            if (c == 0 && singer_recording(a)) {
                const unsigned count = a->capture_count[0], half = MC_ASC_WFIFO / 2;
                const unsigned need = count < half ? half - count : MC_ASC_WFIFO - count;
                const uint32_t inc = singer_record_incr(a);
                if (!need || !inc) continue;
                t = ((uint64_t)need * ONE - a->capture_phase + inc - 1) / inc;
            } else if (c == 0 && recording(a)) {
                unsigned need = f->count < HALF ? HALF - f->count : f->count < MC_ASC_FIFO ? MC_ASC_FIFO - f->count : 0;
                uint32_t inc = rate_incr(a, 0);
                if (!need || !inc) continue;
                t = ((uint64_t)need * ONE - f->acc + inc - 1) / inc;
            } else {
                const unsigned count = fifo_count(a, c), half = fifo_size(a, c) / 2;
                unsigned n = count >= half ? count - (half - 1) : count;
                uint32_t inc = rate_incr(a, c);
                if (!n || !inc) continue;
                t = ((uint64_t)n * ONE - f->acc + inc - 1) / inc;
            }
            if (a->ticks + t < best) best = a->ticks + t;
        }
    }
    mc_dev_schedule(&a->dev, best == UINT64_MAX ? MC_NEVER : time_of_tick(a, best));
}

static void empty_fifos(mc_asc *a) {
    for (int c = 0; c < 2; c++) {
        mc_asc_fifo *f = &a->fifo[c];
        f->rd = f->wr = f->count = 0;
        f->wrd = f->wwr = f->wcount = 0;
        f->acc = 0;
        f->xa_pos = 0;
        f->s1 = f->s2 = 0;
    }
}
static void clear_fifos(mc_asc *a) {
    if (singer_recording(a)) {
        clear_capture(a);
        a->irq_status = (a->irq_status & 0xFCu) | 12u;
    } else if (easc(a) && recording(a)) {
        /* Recording, FIFO A's events are half full and full: a clear takes
         * the ones latched (its interrupt then comes at half full) and
         * empties FIFO B. */
        a->irq_status = (a->irq_status & 0xFCu) | 0x0Cu;
    } else if (easc(a)) a->irq_status |= 0x0F;       /* both FIFOs emptied */
    empty_fifos(a);
}

static void push(mc_asc *a, int c, uint8_t v) {
    mc_asc_fifo *f = &a->fifo[c];
    if (f->count == MC_ASC_FIFO) return;      /* full: the byte is lost */
    a->ram[c * MC_ASC_FIFO + f->wr] = v;
    f->wr = (uint16_t)((f->wr + 1) % MC_ASC_FIFO);
    f->count++;
    /* Filled up: on the original ASC too (System 7.5.3's Sound Manager
     * fills both FIFOs until $804 bit 3, FIFO B full, reads set). */
    if (f->count == MC_ASC_FIFO) a->irq_status |= ST_EDGE(c);
}
static void push16(mc_asc *a, int c, uint16_t v) {
    mc_asc_fifo *f = &a->fifo[c];
    a->wide |= (uint8_t)(1u << c);
    if (f->wcount == MC_ASC_WFIFO) return;    /* full: the sample is lost */
    a->wram[c][f->wwr] = (int16_t)v;
    f->wwr = (uint16_t)((f->wwr + 1) % MC_ASC_WFIFO);
    f->wcount++;
    if (f->wcount == MC_ASC_WFIFO) a->irq_status |= ST_EDGE(c);
}
static uint8_t pop(mc_asc *a, int c) {
    mc_asc_fifo *f = &a->fifo[c];
    uint8_t v = a->ram[c * MC_ASC_FIFO + f->rd];
    if (!f->count) return v;
    f->rd = (uint16_t)((f->rd + 1) % MC_ASC_FIFO);
    f->count--;
    return v;
}

static uint8_t read8(mc_asc *a, uint32_t off) {
    if (off >= R_PORT16) {
        if (!singer_recording(a)) return 0;
        const unsigned c = (off & 0x800u) != 0;
        if (off & 1u) return (uint8_t)a->capture_word[c];
        a->capture_word[c] = singer_input(a, c);
        return (uint8_t)(a->capture_word[c] >> 8);
    }
    if (off < 0x800) {
        int c = off >= MC_ASC_FIFO;
        if (wavetable(a)) return a->ram[off];
        if (singer_recording(a)) return (uint8_t)((singer_input(a, (unsigned)c) >> 8) ^ 0x80u);
        if (c == 0 && recording(a)) return pop(a, 0);
        return a->ram[c * MC_ASC_FIFO + a->fifo[c].rd];   /* next byte to play † */
    }
    switch (off) {
    case R_VERSION: return easc(a) ? VERSION_EASC : VERSION_ASC;
    case R_MODE: return a->mode;
    case R_CONTROL: return a->control;
    /* The original ASC reads bit 7 back (IIci ROM 368CADFE's register
     * test, $40845160: $80 written to $803 reads $80). */
    case R_FIFO_MODE: return easc(a) ? a->fifo_mode & 0x7F : a->fifo_mode;
    case R_IRQ: {
        if (easc(a)) {
            a->irq_status = 0;                /* the read takes the latched events */
            update_irq(a);
            return easc_status(a);
        }
        uint8_t v = a->irq_status;
        a->irq_status = 0;                    /* read clears (Guide ch. 13) */
        update_irq(a);
        return v;
    }
    case R_WTCTL: return a->wt_control;
    case R_VOLUME: return a->volume;
    case R_CLOCK: return a->clock;
    case R_PLAYREC: return a->play_rec;
    case R_TEST: return a->test;
    default: break;
    }
    if (off >= R_WAVE && off < R_WAVE_END) {
        unsigned n = (off - R_WAVE) / 8, b = (off - R_WAVE) % 8;
        uint32_t v = b < 4 ? a->phase[n] : a->incr[n];
        return (uint8_t)(v >> (8 * (3 - b % 4)));
    }
    if (easc(a) && off >= R_EXT && off < R_EXT_END) return a->xreg[off - R_EXT];
    if (a->block && off >= R_SBLOCK && off < R_SBLOCK_END) return a->breg[off - R_SBLOCK];
    return 0;
}

static void write8(mc_asc *a, uint32_t off, uint8_t v) {
    if (off >= R_PORT16) {
        /* A byte write: the high half at an even address, the sample with
         * the low half at the odd one. */
        if (!(off & 1)) { a->whi = v; return; }
        push16(a, (off & 0x800) != 0, (uint16_t)(a->whi << 8 | v));
        return;
    }
    if (off < 0x800) {
        if (wavetable(a)) { a->ram[off] = v; return; }
        int c = off >= MC_ASC_FIFO;
        a->wide &= (uint8_t)~(1u << c);       /* the byte port */
        push(a, c, v);
        return;
    }
    switch (off) {
    case R_MODE: {
        const unsigned input_rate = singer_input_rate(a);
        /* MC_ASC_V8: leaving FIFO mode stops the FIFOs, an event on each
         * (asc.h). */
        if (a->type == MC_ASC_V8 && a->mode == 1 && (v & 3) != 1) {
            a->irq_status |= 0x0F;
            update_irq(a);
        }
        /* On the EASC, entering FIFO mode starts both FIFOs empty: the
         * PowerBook 520's and 190's ROMs write a word of 0 to each 16-bit
         * port in mode 0, after the clear strobe, before they start their
         * chimes (traced), which would otherwise play ahead of the
         * stream†. */
        if (easc(a) && (v & 3) == 1 && a->mode != 1) empty_fifos(a);
        a->mode = v & 3;
        input_changed(a, input_rate);
        return;
    }
    case R_CONTROL: a->control = v; return;
    case R_FIFO_MODE:
        a->fifo_mode = easc(a) ? v & 0x7F : v;
        if (v & 0x80) clear_fifos(a);         /* strobe */
        return;
    case R_IRQ:
        /* The original ASC's flags take a write (IIci ROM 368CADFE's register
         * test, $40845160: $01, $02, $04, $08 written to $804 read back,
         * each read once)†. */
        if (!easc(a)) {
            a->irq_status = v & 0x0F;
            update_irq(a);
        }
        return;
    case R_WTCTL: a->wt_control = v; return;
    case R_VOLUME: a->volume = v & 0xE0; return;
    case R_CLOCK:
        a->clock = v & 3;
        set_clock(a);
        rebase(a, now(a));
        return;
    case R_PLAYREC: {
        const unsigned input_rate = singer_input_rate(a);
        if (records(a)) {
            if (a->singer && ((a->play_rec ^ v) & 1u)) {
                clear_capture(a);
                a->irq_status &= 0xFCu;
            }
            a->play_rec = v & 3;
        }
        input_changed(a, input_rate);
        return;
    }
    case R_TEST: a->test = v; return;
    default: break;
    }
    if (off >= R_WAVE && off < R_WAVE_END) {
        unsigned n = (off - R_WAVE) / 8, b = (off - R_WAVE) % 8;
        uint32_t *reg = b < 4 ? &a->phase[n] : &a->incr[n];
        unsigned shift = 8 * (3 - b % 4);
        *reg = ((*reg & ~(0xFFu << shift)) | ((uint32_t)v << shift)) & 0xFFFFFFu;
        return;
    }
    if (easc(a) && off >= R_EXT && off < R_EXT_END) {
        const unsigned g = (off - R_EXT) / 0x20, r = (off - R_EXT) % 0x20;
        if (g < 2 && r == X_FCTL && ((a->xreg[off - R_EXT] ^ v) & 3)) {   /* a new stream format */
            a->fifo[g].xa_pos = 0;
            a->fifo[g].s1 = a->fifo[g].s2 = 0;
            a->fifo[g].acc = 0;                /* and its sample phase: both FIFOs start in step */
        }
        const unsigned input_rate = singer_input_rate(a);
        a->xreg[off - R_EXT] = v;
        input_changed(a, input_rate);
    }
    if (a->block && off >= R_SBLOCK && off < R_SBLOCK_END) {
        const unsigned input_rate = singer_input_rate(a);
        a->breg[off - R_SBLOCK] = v;
        input_changed(a, input_rate);
    }
}

/* The ASC decodes 4 KiB; the sound block adds its 16-bit ports above. */
static uint32_t mask(const mc_asc *a) { return a->block ? 2 * MC_ASC_SIZE - 1 : MC_ASC_SIZE - 1; }

static uint32_t asc_read(void *dev, uint32_t offset, unsigned size) {
    mc_asc *a = dev;
    sync(a);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | read8(a, (offset + i) & mask(a));
    reschedule(a);
    return v;
}
static void asc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_asc *a = dev;
    sync(a);
    for (unsigned i = 0; i < size; i++)
        write8(a, (offset + i) & mask(a), (uint8_t)(value >> (8 * (size - 1 - i))));
    update_irq(a);
    reschedule(a);
}
static void asc_event(void *dev, uint64_t t) {
    mc_asc *a = dev;
    sync_to(a, t);
    reschedule(a);
}

static void reset_state(mc_asc *a) {
    mc_asc_type type = (mc_asc_type)a->type;
    mc_dev keep = a->dev;
    int irq = a->irq;
    const uint8_t block = a->block, singer = a->singer, input = a->input;
    const mc_audio_source source = a->source;
    const unsigned input_rate = singer_input_rate(a);
    memset(a, 0, sizeof *a);
    a->dev = keep;
    a->type = (uint8_t)type;
    a->irq = irq;
    a->block = block;
    a->singer = singer;
    a->input = input;
    a->source = source;
    memset(a->ram, 0x80, sizeof a->ram);

    /* The EASC's $F00 registers start at 0 (Livadix's choice: the ROM programs
     * every one it uses before playing), but for the decoder's filter pairs
     * ($F10-$F17, $F30-$F37), which start at the values the Quadra 700's
     * ROM writes there: the PowerBook 520's ROM plays the same compressed
     * chime without writing them†. */
    static const uint8_t filters[8] = {0x00, 0x00, 0x00, 0x3C, 0xCC, 0x73, 0xC9, 0x62};
    if (easc(a))
        for (int c = 0; c < 2; c++) memcpy(xgroup(a, c) + X_XA, filters, sizeof filters);
    set_clock(a);
    a->base_time = now(a);
    input_changed(a, input_rate);
}
static void asc_reset(void *dev) {
    mc_asc *a = dev;
    reset_state(a);
    update_irq(a);
    mc_dev_schedule(&a->dev, MC_NEVER);
}

/* The EASC's status register ($804), once its latched events are taken:
 * its bits follow the FIFOs' levels across half and empty, crossings the
 * deadline (reschedule) is armed at; until then a read returns the same
 * and takes nothing. */
static int asc_stable(void *dev, uint32_t offset, unsigned size) {
    const mc_asc *a = dev;
    return easc(a) && size == 1 && (offset & mask(a)) == R_IRQ && a->irq_status == 0;
}

const mc_dev_ops mc_asc_ops = {asc_read, asc_write, asc_event, asc_reset, asc_stable};

void mc_asc_init(mc_asc *a, const mc_hw_host *host, mc_asc_type type) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_asc_ops;
    a->dev.host = host;
    a->dev.name = type == MC_ASC_ENHANCED ? "EASC" : "ASC";
    a->type = (uint8_t)type;
    reset_state(a);
}

void mc_asc_set_audio_source(mc_asc *a, const mc_audio_source *source) {
    sync(a);
    reschedule(a);
    const mc_audio_source next = source ? *source : (mc_audio_source){0};
    if (a->source.ctx == next.ctx && a->source.sample == next.sample && a->source.enable == next.enable) return;
    const unsigned rate = singer_input_rate(a);
    if (rate && a->source.enable) a->source.enable(a->source.ctx, 0);
    a->source = next;
    if (rate && a->source.enable) a->source.enable(a->source.ctx, rate);
}

size_t mc_asc_pull(mc_asc *a, int16_t *stereo_out, size_t frames) {
    sync(a);
    reschedule(a);
    size_t n = frames < a->ring_count ? frames : a->ring_count;
    for (size_t i = 0; i < n; i++) {
        stereo_out[2 * i] = a->ring[a->ring_rd][0];
        stereo_out[2 * i + 1] = a->ring[a->ring_rd][1];
        a->ring_rd = (a->ring_rd + 1) % MC_ASC_RING;
    }
    a->ring_count -= n;
    return n;
}

void mc_asc_rate_exact(const mc_asc *a, uint64_t *num, uint64_t *den) { *num = a->num; *den = a->den; }
uint32_t mc_asc_rate(const mc_asc *a) {
    return (uint32_t)(((uint64_t)MC_C15M_HZ * a->num * 2 + a->den) / (2 * a->den));
}
unsigned mc_asc_fifo_level(mc_asc *a, int c) {
    sync(a);
    reschedule(a);
    return a->block ? a->fifo[c & 1].count + 2u * a->fifo[c & 1].wcount : a->fifo[c & 1].count;
}
