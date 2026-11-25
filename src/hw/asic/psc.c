/* Livadix — the PSC (see psc.h for sources). Livadix's own code. */
#include "psc.h"

#include <stdlib.h>
#include <string.h>

#define R_LEVEL 0x100u          /* + $10L: flags, + $10L + 4: enables */
#define R_SND_CONTROL 0x200u
#define R_SND_SINGER 0x204u
#define R_SND_STATUS 0x208u
#define R_SND_PROGRESS 0x20Cu
#define R_SND_IN 0x210u
#define R_SND_OUT 0x214u
#define R_SND_LENGTH 0x218u
#define R_DSP 0x21Cu
#define R_DMA_FLAGS 0x804u
#define R_CONTROL 0xC00u        /* + $10n */
#define R_SETS 0x1000u          /* + $20n + $10s */

#define SND_PLAY 0x0100u        /* sound control: the output buffer plays */
#define SND_DSP 0x0040u         /* sound control: frames for the DSP */
#define SND_RECORD 0x0080u      /* sound control: the DSP's frames record ($00C4 while the Sound Input Manager records) */
#define CTL_SET 0x0001u
#define CTL_FLUSH 0x0200u
#define CTL_PAUSE 0x0400u
#define CTL_RESET 0x0800u
#define CTL_STOPPED 0x4000u
#define CMD_ACTIVE 0x0100u        /* the set has begun (the SCSI HAL waits for it, ROM offset $1F170) */
#define CMD_FROM_DEVICE 0x0200u
#define CMD_TERMINAL 0x0400u
#define CMD_ARMED 0x0800u
#define CMD_INTERRUPT 0x1000u     /* the set's end raises the channel's flag (+$804)† */

#define TICK_HZ100 6015u        /* 60.15 Hz (Guide to the Macintosh Family Hardware, ch. 4) */
#define RING_FRAMES 16384u

/* --- interrupts ------------------------------------------------------------------ */

static void update_levels(mc_psc *p) {
    /* Level 4 bit 3: any DMA channel's flag. */
    if (p->dma_flags) p->flags[1] |= 0x08u;
    else p->flags[1] = (uint8_t)(p->flags[1] & 0xF7u);
    for (unsigned l = 0; l < 4; l++) {
        const uint8_t irq = (p->flags[l] & p->enables[l] & 0x7Fu) != 0;
        if (irq != p->irq[l]) {
            p->irq[l] = irq;
            mc_dev_line(&p->dev, MC_PSC_LINE_IRQ3 + (int)l, irq);
        }
    }
}

static void update_via2(mc_psc *p) {
    const uint8_t irq = (p->via2_inputs & p->via2_ier & 0x7Fu) != 0;
    if (irq != p->via2_irq) {
        p->via2_irq = irq;
        mc_dev_line(&p->dev, MC_PSC_LINE_VIA2, irq);
    }
}

void mc_psc_set_source(mc_psc *p, mc_psc_source source, int asserted) {
    const unsigned group = (unsigned)source >> 4, bit = (unsigned)source & 7u;
    if (group == 2) {
        if (asserted) p->via2_inputs |= (uint8_t)(1u << bit);
        else p->via2_inputs &= (uint8_t)~(1u << bit);
        update_via2(p);
        return;
    }
    if (group < 3 || group > 6) return;
    uint8_t *f = &p->flags[group - 3];
    if (asserted) *f |= (uint8_t)(1u << bit);
    else *f &= (uint8_t)~(1u << bit);
    update_levels(p);
}

void mc_psc_set_slots(mc_psc *p, uint8_t asserted_bits) {
    p->via2_slots = asserted_bits;
    mc_psc_set_source(p, MC_PSC_SRC_VIA2_SLOT, asserted_bits != 0);
}

/* --- DMA --------------------------------------------------------------------------- */

static void run_channel(mc_psc *p, unsigned n);
static void run_loop(mc_psc *p, unsigned n);

void mc_psc_set_port(mc_psc *p, unsigned channel, const mc_psc_port *port) {
    if (channel >= MC_PSC_CHANNELS) return;
    if (port) p->ch[channel].port = *port;
    else memset(&p->ch[channel].port, 0, sizeof p->ch[channel].port);
}

void mc_psc_request(mc_psc *p, unsigned channel, int asserted) {
    if (channel >= MC_PSC_CHANNELS) return;
    p->ch[channel].request = (uint8_t)(asserted != 0);
    if (asserted) run_channel(p, channel);
}

static int enet(unsigned n) { return n == MC_PSC_CH_ENET_RD || n == MC_PSC_CH_ENET_WR; }

/* An Ethernet channel's flag is a level: a set with a frame done (bit 8)
 * and its interrupt bit (12); the driver's handler clears bit 8 ($1E2160,
 * $1E21B0) and the ROM's dispatcher ($10988) never writes +$804. */
static void enet_flag(mc_psc *p, unsigned n) {
    const mc_psc_channel *c = &p->ch[n];
    int on = 0;
    for (unsigned s = 0; s < 2; s++)
        on |= (c->set[s].command & (CMD_ACTIVE | CMD_INTERRUPT)) == (CMD_ACTIVE | CMD_INTERRUPT);
    if (on) p->dma_flags |= 0x80000000u >> n;
    else p->dma_flags &= ~(0x80000000u >> n);
}

static void set_done(mc_psc *p, unsigned n) {
    mc_psc_channel *c = &p->ch[n];
    const unsigned s = c->control & CTL_SET;
    c->set[s].command = (uint16_t)(c->set[s].command & ~CMD_ARMED);
    c->set[s].command |= CMD_TERMINAL;
    if (enet(n)) enet_flag(p, n);
    else if (n < 8 && ((c->set[s].command | c->control) & CMD_INTERRUPT)) p->dma_flags |= 0x80000000u >> n;
    /* The other set goes on when it is armed; the Ethernet channels take
     * their sets in turn, as their driver fills them. */
    if (enet(n) || (c->set[s ^ 1u].command & CMD_ARMED)) c->control ^= CTL_SET;
    update_levels(p);
}

/* ENetWr: each armed set is one frame, sent whole to the port's `frame`. */
static void run_frames(mc_psc *p, unsigned n) {
    mc_psc_channel *c = &p->ch[n];
    for (unsigned guard = 0; guard < 4; guard++) {
        if (c->control & CTL_PAUSE) return;
        const unsigned s = c->control & CTL_SET;
        if (!(c->set[s].command & CMD_ARMED)) return;
        uint8_t buf[MC_PSC_FRAME_MAX];
        const uint32_t len = c->set[s].count < sizeof buf ? c->set[s].count : (uint32_t)sizeof buf;
        if (len) mc_dev_dma(&p->dev, c->set[s].address, buf, len, 0);
        c->set[s].address += c->set[s].count;
        c->set[s].count = 0;
        c->set[s].command |= CMD_ACTIVE;
        if (len) c->port.frame(c->port.ctx, buf, len);
        set_done(p, n);
    }
}

int mc_psc_enet_receive(mc_psc *p, const uint8_t *frame, size_t length, const uint8_t status[4]) {
    mc_psc_channel *c = &p->ch[MC_PSC_CH_ENET_RD];
    if (c->control & CTL_PAUSE) return 0;
    const unsigned s = c->control & CTL_SET;
    if (!(c->set[s].command & CMD_ARMED) || !c->set[s].count) return 0;
    uint8_t head[16] = {0};
    for (unsigned i = 0; i < 4; i++) head[2 * i] = head[2 * i + 1] = status[i];
    if (length > MC_PSC_ENET_SLOT - sizeof head) length = MC_PSC_ENET_SLOT - sizeof head;
    mc_dev_dma(&p->dev, c->set[s].address, head, sizeof head, 1);
    mc_dev_dma(&p->dev, c->set[s].address + (uint32_t)sizeof head, (void *)frame, (uint32_t)length, 1);
    c->set[s].address += MC_PSC_ENET_SLOT;
    c->set[s].command |= CMD_ACTIVE;
    if (--c->set[s].count == 0) set_done(p, MC_PSC_CH_ENET_RD);
    else {
        enet_flag(p, MC_PSC_CH_ENET_RD);
        update_levels(p);
    }
    return 1;
}

/* Moves bytes while the channel runs, its set is armed with a count and the
 * device asks. */
static void run_channel(mc_psc *p, unsigned n) {
    mc_psc_channel *c = &p->ch[n];
    /* A device's request changing while it moves a byte comes back here:
     * the loop below goes on with it. */
    if (c->running) return;
    c->running = 1;
    if (!enet(n)) run_loop(p, n);
    else if (n == MC_PSC_CH_ENET_WR && c->port.frame) run_frames(p, n);
    c->running = 0;
}

static void run_loop(mc_psc *p, unsigned n) {
    mc_psc_channel *c = &p->ch[n];
    for (unsigned guard = 0; guard < 0x100000u; guard++) {
        if (c->control & CTL_PAUSE) return;
        const unsigned s = c->control & CTL_SET;
        if (!(c->set[s].command & CMD_ARMED)) return;
        c->set[s].command |= CMD_ACTIVE;
        if (!c->set[s].count) { set_done(p, n); continue; }
        if (!c->request || !c->port.ctx) return;
        uint8_t b;
        if (c->set[s].command & CMD_FROM_DEVICE) {
            if (!c->port.read || !c->port.read(c->port.ctx, &b)) return;
            mc_dev_dma(&p->dev, c->set[s].address, &b, 1, 1);
        } else {
            mc_dev_dma(&p->dev, c->set[s].address, &b, 1, 0);
            if (!c->port.write || !c->port.write(c->port.ctx, b)) return;
        }
        c->set[s].address++;
        if (--c->set[s].count == 0) set_done(p, n);
    }
}

/* A control or command word written as the VIA's IER: bit 15 sets the other
 * bits written 1, else clears them. */
static uint16_t set_clear(uint16_t r, uint16_t v) { return (v & 0x8000u) ? (uint16_t)(r | (v & 0x7FFFu)) : (uint16_t)(r & ~v); }

static void control_write(mc_psc *p, unsigned n, uint16_t v) {
    mc_psc_channel *c = &p->ch[n];
    c->control = set_clear(c->control, v);
    if (c->control & CTL_RESET) {       /* the channel reset: paused and idle */
        c->control = (uint16_t)((c->control & ~(CTL_RESET | CTL_SET)) | CTL_PAUSE);
        c->set[0].command = c->set[1].command = enet(n) ? CMD_TERMINAL : 0;
        if (n < 8) p->dma_flags &= ~(0x80000000u >> n);
    }
    c->control = (uint16_t)(c->control & ~CTL_FLUSH);
    update_levels(p);
    run_channel(p, n);
}

static uint16_t control_read(const mc_psc *p, unsigned n) {
    const mc_psc_channel *c = &p->ch[n];
    /* Stopped: bits 15 and 14 (the reset channel reads $C400, ROM offset $1850FE).
     * The Ethernet channels: bit 8 while a set has a frame done (bit 8). */
    uint16_t v = (uint16_t)((c->control & 0x3FFFu) | ((c->control & CTL_PAUSE) ? 0x8000u | CTL_STOPPED : 0));
    if (enet(n)) v = (uint16_t)((v & ~CMD_ACTIVE) | ((c->set[0].command | c->set[1].command) & CMD_ACTIVE));
    return v;
}

static void command_write(mc_psc *p, unsigned n, unsigned s, uint16_t v) {
    mc_psc_channel *c = &p->ch[n];
    c->set[s].command = set_clear(c->set[s].command, v);
    /* An Ethernet set armed is busy until done (bit 10: free, the
     * transmitter waits for it, ROM offset $1E1E56). */
    if (enet(n) && (v & 0x8000u) && (v & CMD_ARMED)) c->set[s].command = (uint16_t)(c->set[s].command & ~CMD_TERMINAL);
    if (enet(n)) enet_flag(p, n);
    /* Clearing the terminal count clears the channel's flag. */
    if (!enet(n) && !(c->set[0].command & CMD_TERMINAL) && !(c->set[1].command & CMD_TERMINAL) && n < 8)
        p->dma_flags &= ~(0x80000000u >> n);
    update_levels(p);
    run_channel(p, n);
}

/* --- sound ---------------------------------------------------------------------- */

static void ring_put(mc_psc *p, int16_t l, int16_t r) {
    if (p->ring_count == p->ring_size) return;      /* nobody pulls: the oldest stay */
    const size_t i = (p->ring_head + p->ring_count) % p->ring_size;
    p->ring[2 * i] = l;
    p->ring[2 * i + 1] = r;
    p->ring_count++;
}

static void snd_start(mc_psc *p, uint32_t address) {
    p->snd_out = address;
    p->snd_left = (uint32_t)p->snd_length * 8u;
}

static uint32_t frame_rate(const mc_psc *p);
static uint32_t frame_samples(const mc_psc *p) { return p->snd_length ? p->snd_length : 240u; }

/* Plays the output up to now: a frame every 1/44100 s while a buffer plays. */
static void snd_advance(mc_psc *p) {
    const uint64_t now = mc_dev_now(&p->dev);
    if (now <= p->snd_time) return;
    const uint64_t elapsed = now - p->snd_time;
    p->snd_time = now;
    uint64_t total = elapsed * MC_PSC_SOUND_RATE + p->snd_rem;
    uint64_t frames = total / MC_C15M_HZ;
    p->snd_rem = (uint32_t)(total % MC_C15M_HZ);
    if ((p->snd_control & (SND_DSP | SND_PLAY)) == (SND_DSP | SND_PLAY)) {
        /* The DSP's frames: the two halves of +$214 in turn at the frame
         * rate, each played the frame after the DSP wrote it (during frame
         * k the AV kernel writes half k mod 2: the second half plays first). */
        const uint64_t ring = 2u * (uint64_t)frame_samples(p);
        const uint32_t rate = frame_rate(p);
        for (; frames; frames--, p->frame_played++) {
            const uint64_t at = (p->frame_played * rate / MC_PSC_SOUND_RATE + ring / 2u) % ring;
            uint8_t b[4];
            mc_dev_dma(&p->dev, p->frame_out + 4u * (uint32_t)at, b, 4, 0);
            ring_put(p, (int16_t)(b[0] << 8 | b[1]), (int16_t)(b[2] << 8 | b[3]));
        }
        return;
    }
    while (frames) {
        if (!(p->snd_control & SND_PLAY) || !p->snd_left) {
            if ((p->snd_control & SND_PLAY) && p->snd_pending) {
                p->snd_pending = 0;
                snd_start(p, p->snd_next);
                continue;
            }
            /* Silence while idle. */
            for (uint64_t i = 0; i < frames && i < RING_FRAMES; i++) ring_put(p, 0, 0);
            return;
        }
        uint8_t b[4] = {0};
        mc_dev_dma(&p->dev, p->snd_out, b, 4, 0);
        ring_put(p, (int16_t)(b[0] << 8 | b[1]), (int16_t)(b[2] << 8 | b[3]));
        p->snd_out += 4;
        p->snd_left = p->snd_left > 4 ? p->snd_left - 4 : 0;
        if (!p->snd_left) p->played = 1;
        frames--;
    }
}

size_t mc_psc_pull(mc_psc *p, int16_t *stereo, size_t frames) {
    snd_advance(p);
    size_t n = 0;
    for (; n < frames && p->ring_count; n++) {
        stereo[2 * n] = p->ring[2 * p->ring_head];
        stereo[2 * n + 1] = p->ring[2 * p->ring_head + 1];
        p->ring_head = (p->ring_head + 1) % p->ring_size;
        p->ring_count--;
    }
    return n;
}

/* --- the DSP's frames ------------------------------------------------------------- */

/* The sample rate the ROM selects in +$200 bits 9-10 for the DSP's frames
 * ($4099BD84: 22.05 and 24 kHz set neither, 32 kHz bit 9, 44.1 and 48 kHz
 * bit 10): Singer's 24, 32 or 48 kHz†. */
static uint32_t frame_rate(const mc_psc *p) {
    const unsigned sel = (p->snd_control >> 9) & 3u;
    return sel == 1 ? 32000u : sel == 2 ? 48000u : 24000u;
}
static uint64_t frame_period(const mc_psc *p) {
    return (uint64_t)frame_samples(p) * MC_C15M_HZ / frame_rate(p);
}
static void frame_line(mc_psc *p) { mc_dev_line(&p->dev, MC_PSC_LINE_DSP_FRAME, p->frame_req); }

static int frames_record(const mc_psc *p) { return (p->snd_control & (SND_DSP | SND_RECORD)) == (SND_DSP | SND_RECORD); }

/* The host's input runs while the DSP's frames record. */
static void source_update(mc_psc *p) {
    const unsigned rate = frames_record(p) ? frame_rate(p) : 0;
    if (rate == p->source_rate) return;
    if (p->source.enable) p->source.enable(p->source.ctx, rate);
    p->source_rate = rate;
}

/* A frame recorded: the input (or silence) into the half of +$210 the DSP
 * reads during the next frame (as it writes +$214). */
static void frame_record(mc_psc *p) {
    const uint32_t n = frame_samples(p), rate = frame_rate(p);
    p->frame_count++;
    if (!frames_record(p)) return;
    uint32_t at = p->frame_in + (uint32_t)(p->frame_count & 1u) * n * 4u;
    for (uint32_t i = 0; i < n; i++, at += 4) {
        int16_t s[2] = {0, 0};
        if (p->source.sample) p->source.sample(p->source.ctx, rate, s);
        uint8_t b[4] = {(uint8_t)((uint16_t)s[0] >> 8), (uint8_t)s[0], (uint8_t)((uint16_t)s[1] >> 8), (uint8_t)s[1]};
        mc_dev_dma(&p->dev, at, b, 4, 1);
    }
}

void mc_psc_set_audio_source(mc_psc *p, const mc_audio_source *source) {
    if (p->source_rate && p->source.enable) p->source.enable(p->source.ctx, 0);
    p->source = source ? *source : (mc_audio_source){0};
    p->source_rate = 0;
    source_update(p);
}

static void frame_tick(mc_psc *p, uint64_t now) {
    if (!(p->snd_control & SND_DSP)) return;
    while (p->next_frame <= now) {
        frame_record(p);
        if (p->frame_req && ++p->frame_missed >= 2) {
            /* not acknowledged for two frames: the overrun latch and the
             * host's level 5 bit 1 (Developer Note, chapter 3, "Category
             * Two Frame Overrun")† */
            p->dsp_control |= 4u;
            p->flags[2] |= 2u;
            update_levels(p);
        }
        p->frame_req = 1;
        p->next_frame += frame_period(p);
    }
    frame_line(p);
}

static void psc_reschedule(mc_psc *p) {
    uint64_t next = p->next_tick;
    if ((p->snd_control & SND_DSP) && p->next_frame < next) next = p->next_frame;
    mc_dev_schedule(&p->dev, next);
}

void mc_psc_dsp_ack(mc_psc *p) {
    p->frame_req = 0;
    p->frame_missed = 0;
    frame_line(p);
}

void mc_psc_dsp_doorbell(mc_psc *p) {
    p->flags[2] |= 1u;
    update_levels(p);
}

/* --- registers ------------------------------------------------------------------ */

static uint32_t get_long(const uint8_t *b) { return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3]; }

/* The register longword holding `off` (4-aligned) and whether it is modelled. */
static int long_value(mc_psc *p, uint32_t off, uint32_t *v) {
    if (off >= R_LEVEL + 0x30u && off < R_LEVEL + 0x70u) {
        const unsigned l = (off - R_LEVEL - 0x30u) >> 4;
        if ((off & 0xCu) == 0) { *v = (uint32_t)(p->flags[l] | ((p->flags[l] & p->enables[l] & 0x7Fu) ? 0x80u : 0)) << 24; return 1; }
        if ((off & 0xCu) == 4) { *v = (uint32_t)p->enables[l] << 24; return 1; }
        return 0;
    }
    switch (off) {
    case R_SND_CONTROL: *v = (uint32_t)p->snd_control << 16; return 1;
    case R_SND_SINGER: *v = p->snd_singer; return 1;
    case R_SND_STATUS: *v = 0; return 1;
    case R_SND_PROGRESS:
        snd_advance(p);
        *v = (p->snd_left & 0xFFFFC0u) | (p->snd_pending ? 0x40u : 0);
        return 1;
    case R_SND_IN: *v = p->snd_in; return 1;
    case R_SND_OUT: *v = p->snd_out; return 1;
    case R_SND_LENGTH: *v = (uint32_t)p->snd_length << 16; return 1;
    case R_DSP: *v = (uint32_t)p->dsp_control << 24; return 1;
    case R_DMA_FLAGS: *v = p->dma_flags; return 1;
    default: break;
    }
    if (off >= R_CONTROL && off < R_CONTROL + 0x10u * MC_PSC_CHANNELS && (off & 0xCu) == 0) {
        *v = (uint32_t)control_read(p, (off - R_CONTROL) >> 4) << 16;
        return 1;
    }
    if (off >= R_SETS && off < R_SETS + 0x20u * MC_PSC_CHANNELS) {
        const unsigned n = (off - R_SETS) >> 5, s = (off >> 4) & 1u;
        switch (off & 0xCu) {
        case 0: *v = p->ch[n].set[s].address; return 1;
        case 4: *v = p->ch[n].set[s].count; return 1;
        case 8: *v = (uint32_t)p->ch[n].set[s].command << 16; return 1;
        default: return 0;
        }
    }
    return 0;
}

static uint32_t psc_read(void *dev, uint32_t offset, unsigned size) {
    mc_psc *p = dev;
    offset %= MC_PSC_SIZE;
    uint32_t v;
    if (long_value(p, offset & ~3u, &v)) {
        const unsigned shift = 8u * (4u - size - (offset & 3u));
        return size == 4 ? v : (v >> shift) & (size == 2 ? 0xFFFFu : 0xFFu);
    }
    uint32_t r = 0;
    for (unsigned i = 0; i < size; i++) r = r << 8 | p->other[(offset + i) % MC_PSC_SIZE];
    return r;
}

static void psc_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_psc *p = dev;
    offset %= MC_PSC_SIZE;
    const uint32_t off = offset & ~3u;
    /* The value as it sits in its longword. */
    const unsigned shift = 8u * (4u - size - (offset & 3u));
    const uint32_t mask = size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    uint32_t old = 0;
    long_value(p, off, &old);
    const uint32_t v = (old & ~mask) | ((value << shift) & mask);
    if (off >= R_LEVEL + 0x30u && off < R_LEVEL + 0x70u) {
        const unsigned l = (off - R_LEVEL - 0x30u) >> 4;
        const uint8_t b = (uint8_t)(v >> 24);
        if ((off & 0xCu) == 4) {
            if (b & 0x80u) p->enables[l] |= (uint8_t)(b & 0x7Fu);
            else p->enables[l] &= (uint8_t)~b;
            update_levels(p);
        } else if ((off & 0xCu) == 0 && l == 2) {
            /* level 5's latched DSP sources: a 1 clears ($4099C91A) */
            p->flags[2] = (uint8_t)(p->flags[2] & ~(b & 3u));
            update_levels(p);
        }
        return;
    }
    switch (off) {
    case R_SND_CONTROL: {
        snd_advance(p);
        const uint16_t was = p->snd_control;
        p->snd_control = (uint16_t)(v >> 16);
        if ((p->snd_control & SND_DSP) && !(was & SND_DSP)) {
            p->next_frame = mc_dev_now(&p->dev) + frame_period(p);
            p->frame_missed = 0;
            p->frame_count = p->frame_played = 0;
            psc_reschedule(p);
        } else if (!(p->snd_control & SND_DSP) && (was & SND_DSP)) {
            p->frame_req = 0;
            frame_line(p);
        }
        source_update(p);
        if ((p->snd_control & SND_PLAY) && !p->snd_left && p->snd_pending) {
            p->snd_pending = 0;
            snd_start(p, p->snd_next);
        }
        if (!(p->snd_control & SND_PLAY)) p->snd_left = 0;
        return;
    }
    case R_SND_SINGER: p->snd_singer = v; return;
    case R_SND_IN: p->snd_in = p->frame_in = v; return;
    case R_SND_OUT:
        snd_advance(p);
        p->frame_out = v;
        if ((p->snd_control & SND_PLAY) && !p->snd_left) snd_start(p, v);
        else {
            p->snd_next = v;
            p->snd_pending = 1;
            if (!(p->snd_control & SND_PLAY)) p->snd_out = v;
        }
        return;
    case R_SND_LENGTH: p->snd_length = (uint16_t)(v >> 16); return;
    case R_DSP: {
        const uint8_t b = (uint8_t)(v >> 24);
        if (b & 0x80u) p->dsp_control |= (uint8_t)(b & 7u);
        else p->dsp_control = (uint8_t)(p->dsp_control & ~(b & 7u));
        mc_dev_line(&p->dev, MC_PSC_LINE_DSP_RESET, p->dsp_control & 1u);
        return;
    }
    case R_DMA_FLAGS:
        p->dma_flags &= ~v;
        update_levels(p);
        return;
    default: break;
    }
    if (off >= R_CONTROL && off < R_CONTROL + 0x10u * MC_PSC_CHANNELS && (off & 0xCu) == 0) {
        if (size != 1 || (offset & 3u) < 2) control_write(p, (off - R_CONTROL) >> 4, (uint16_t)(value << (size == 1 && !(offset & 1u) ? 8 : 0)));
        return;
    }
    if (off >= R_SETS && off < R_SETS + 0x20u * MC_PSC_CHANNELS) {
        const unsigned n = (off - R_SETS) >> 5, s = (off >> 4) & 1u;
        switch (off & 0xCu) {
        case 0: p->ch[n].set[s].address = v; return;
        case 4: p->ch[n].set[s].count = v; run_channel(p, n); return;
        case 8: command_write(p, n, s, (uint16_t)(value << (size == 1 && !(offset & 1u) ? 8 : 0))); return;
        default: return;
        }
    }
    for (unsigned i = 0; i < size; i++) p->other[(offset + i) % MC_PSC_SIZE] = (uint8_t)(value >> (8 * (size - 1 - i)));
    (void)get_long;
}

/* --- VIA2 ----------------------------------------------------------------------- */

enum { V_ORB = 0, V_ORA = 1, V_IFR = 13, V_IER = 14, V_ORA_NH = 15 };

static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    mc_psc *p = dev;
    (void)size;
    const unsigned r = (offset >> 9) & 15u;
    switch (r) {
    case V_IFR: return (uint32_t)((p->via2_inputs & 0x7Fu) | ((p->via2_inputs & p->via2_ier & 0x7Fu) ? 0x80u : 0));
    case V_IER: return (uint32_t)(p->via2_ier | 0x80u);
    case V_ORA: case V_ORA_NH: return (uint8_t)~p->via2_slots;
    default: return p->via2_reg[r];
    }
}

static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_psc *p = dev;
    (void)size;
    const unsigned r = (offset >> 9) & 15u;
    const uint8_t b = (uint8_t)value;
    switch (r) {
    case V_IER:
        if (b & 0x80u) p->via2_ier |= (uint8_t)(b & 0x7Fu);
        else p->via2_ier &= (uint8_t)~b;
        update_via2(p);
        break;
    case V_IFR: break;          /* the inputs are levels: nothing to clear */
    default: p->via2_reg[r] = b; break;
    }
}

const mc_dev_ops mc_psc_via2_ops = {via2_read, via2_write, NULL, NULL, NULL};

/* --- events ----------------------------------------------------------------------- */

static void psc_event(void *dev, uint64_t now) {
    mc_psc *p = dev;
    while (p->next_tick <= now) {
        p->tick ^= 1u;
        p->next_tick += (uint64_t)MC_C15M_HZ * 100u / TICK_HZ100 / 2u;
    }
    mc_dev_line(&p->dev, MC_PSC_LINE_TICK, p->tick);
    snd_advance(p);
    frame_tick(p, now);
    psc_reschedule(p);
}

static void psc_reset(void *dev) {
    mc_psc *p = dev;
    memset(p->flags, 0, sizeof p->flags);
    memset(p->enables, 0, sizeof p->enables);
    p->dma_flags = 0;
    for (unsigned n = 0; n < MC_PSC_CHANNELS; n++) {
        p->ch[n].control = CTL_PAUSE;
        memset(p->ch[n].set, 0, sizeof p->ch[n].set);
    }
    p->via2_ier = 0;
    p->snd_control = 0;
    p->snd_left = 0;
    p->snd_pending = 0;
    p->snd_time = mc_dev_now(&p->dev);
    p->dsp_control = 1;
    mc_dev_line(&p->dev, MC_PSC_LINE_DSP_RESET, 1);
    p->frame_req = 0;
    p->frame_missed = 0;
    frame_line(p);
    update_levels(p);
    update_via2(p);
    p->next_tick = mc_dev_now(&p->dev) + (uint64_t)MC_C15M_HZ * 100u / TICK_HZ100 / 2u;
    mc_dev_schedule(&p->dev, p->next_tick);
}

const mc_dev_ops mc_psc_ops = {psc_read, psc_write, psc_event, psc_reset, NULL};

void mc_psc_init(mc_psc *p, const mc_hw_host *host, const char *name) {
    memset(p, 0, sizeof *p);
    p->dev.ops = &mc_psc_ops;
    p->dev.host = host;
    p->dev.name = name;
    p->ring_size = RING_FRAMES;
    p->ring = calloc(RING_FRAMES * 2u, sizeof *p->ring);
    for (unsigned n = 0; n < MC_PSC_CHANNELS; n++) p->ch[n].control = CTL_PAUSE;
}

void mc_psc_free(mc_psc *p) {
    free(p->ring);
    p->ring = NULL;
}
