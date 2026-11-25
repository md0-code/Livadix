/* Livadix — AMIC, the PDM boards' I/O controller (amic.h).
 *
 * Livadix's own code. */
#include "amic.h"
#include "awac.h"

#include <stdlib.h>
#include <string.h>

/* The classic 60.15 Hz period: 370 lines of 704 C15M periods (rbv.c). */
#define TICK_PERIOD (704u * 370u)
/* A control word to the codec: 16 bits of the AWAC's bit clock (256 x 44.1
 * kHz, Developer Note, Figure 3-13) take well under a microsecond; Livadix
 * keeps bit 7 set for 4 C15M periods so a polling loop sees it†. */
#define CODEC_BUSY 4u
#define RING_FRAMES 16384u

/* DMA window offsets. */
#define D_BASE 0x0000u
#define D_ETX_CTL 0x0C20u
#define D_ERX_CTL 0x1028u
#define D_ERX_HEAD 0x1030u
#define D_ERX_TAIL 0x1034u
#define D_ETX_COUNT 0x1044u
#define D_SCSI_A_ADDR 0x1000u
#define D_SCSI_B_ADDR 0x1004u
#define D_SCSI_A_CTL 0x1008u
#define D_SCSI_B_CTL 0x1009u
#define D_FLOP_ADDR 0x1060u
#define D_FLOP_COUNT 0x1064u
#define D_FLOP_CTL 0x1068u
#define D_SER_CTL 0x1088u          /* the first serial channel's control; then every $10 */

/* Channel control bits (Livadix's reading of the floppy's, amic.h). */
#define C_RESET 0x01u
#define C_RUN 0x02u
#define C_TO_DEV 0x40u
#define C_INT 0x80u

/* --- interrupts ------------------------------------------------------------------- */

static void update_cpu(mc_amic *a) {
    if (a->dma_flags) a->sources |= 1u << MC_AMIC_SRC_DMA;
    else a->sources &= (uint8_t)(0xFFu & ~(1u << MC_AMIC_SRC_DMA));
    if (a->via2_irq) a->sources |= 1u << MC_AMIC_SRC_VIA2;
    else a->sources &= (uint8_t)(0xFFu & ~(1u << MC_AMIC_SRC_VIA2));
    /* The 601's INT: raised by any change of the sources, released by a
     * write of bit 7 to +$00 (amic.h)†. */
    const uint8_t cur = a->sources & 0x3Fu;
    if (cur != a->seen) a->latch = 1;
    a->seen = cur;
    const uint8_t cpu = a->latch;
    if (cpu != a->cpu) {
        a->cpu = cpu;
        mc_dev_line(&a->dev, MC_AMIC_LINE_CPU, cpu);
    }
}

void mc_amic_set_source(mc_amic *a, unsigned source, int asserted) {
    if (source > 5 || source == MC_AMIC_SRC_VIA2 || source == MC_AMIC_SRC_DMA) return;
    if (asserted) a->sources |= (uint8_t)(1u << source);
    else a->sources &= (uint8_t)(0xFFu & ~(1u << source));
    update_cpu(a);
}

static uint8_t slot_mask(const mc_amic *a) { return a->variant ? 0x7Fu : 0x7Cu; }

static void update_via2(mc_amic *a) {
    if (a->via2_slots & a->via2_sier & slot_mask(a)) a->via2_ifr |= MC_AMIC_VIA2_SLOT;
    else a->via2_ifr &= (uint8_t)(0xFFu & ~MC_AMIC_VIA2_SLOT);
    a->via2_irq = (a->via2_ifr & a->via2_ier & 0x7Fu) != 0;
    update_cpu(a);
}

void mc_amic_set_via2(mc_amic *a, uint8_t bit, int asserted) {
    bit &= (uint8_t)(0xFFu & ~MC_AMIC_VIA2_SLOT);
    if (asserted) a->via2_ifr |= bit;
    else a->via2_ifr &= (uint8_t)(0xFFu & ~bit);
    update_via2(a);
}

void mc_amic_set_slots(mc_amic *a, uint8_t asserted, uint8_t mask) {
    a->via2_slots = (uint8_t)((a->via2_slots & ~mask) | (asserted & mask & slot_mask(a)));
    update_via2(a);
}

/* The DMA flags follow the channels' interrupt bits. */
static void update_dma(mc_amic *a) {
    uint16_t f = 0;
    if ((a->dma_reg[D_ETX_CTL] | a->dma_reg[D_ERX_CTL]) & C_INT) f |= 1u << MC_AMIC_DMA_ENET;
    if (a->dma_reg[D_FLOP_CTL] & C_INT) f |= 1u << MC_AMIC_DMA_FLOPPY;
    if (a->dma_reg[D_SCSI_A_CTL] & C_INT) f |= 1u << MC_AMIC_DMA_SCSI_A;
    if (a->dma_reg[D_SCSI_B_CTL] & C_INT) f |= 1u << MC_AMIC_DMA_SCSI_B;
    const uint8_t en = a->snd_reg[0x14] & 0x0Cu;
    if ((a->snd_flags14 & 0xC0u) && (en & 0x04u)) f |= 1u << MC_AMIC_DMA_SND_OUT;
    if ((a->snd_flags14 & 0x30u) && (en & 0x08u)) f |= 1u << MC_AMIC_DMA_SND_IN;
    a->dma_flags = f;
    update_cpu(a);
}

uint32_t mc_amic_dma_base(const mc_amic *a) {
    return (uint32_t)a->dma_reg[D_BASE] << 24 | (uint32_t)a->dma_reg[D_BASE + 1] << 16;
}

static uint32_t reg32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

/* --- byte channels ----------------------------------------------------------------- */

static uint32_t ctl_offset(unsigned ch) {
    return ch == MC_AMIC_CH_SCSI_A ? D_SCSI_A_CTL : ch == MC_AMIC_CH_SCSI_B ? D_SCSI_B_CTL : D_FLOP_CTL;
}
static uint32_t addr_offset(unsigned ch) {
    return ch == MC_AMIC_CH_SCSI_A ? D_SCSI_A_ADDR : ch == MC_AMIC_CH_SCSI_B ? D_SCSI_B_ADDR : D_FLOP_ADDR;
}

/* The floppy's address: its high bits follow the DMA base (its driver
 * writes bytes 2-3 only, amic.h)†. */
static uint32_t chan_addr(const mc_amic *a, unsigned ch) {
    const uint32_t v = reg32(a->dma_reg + addr_offset(ch));
    if (ch == MC_AMIC_CH_FLOPPY) return (mc_amic_dma_base(a) & ~0x3FFFFu) | (v & 0x3FFFFu);
    return v;
}

static void run_channel(mc_amic *a, unsigned ch) {
    mc_amic_channel *c = &a->ch[ch];
    if (c->running) return;
    c->running = 1;
    const uint32_t co = ctl_offset(ch);
    while (c->request && (a->dma_reg[co] & C_RUN)) {
        const int floppy = ch == MC_AMIC_CH_FLOPPY;
        uint32_t count = 0;
        if (floppy) {
            count = (uint32_t)a->dma_reg[D_FLOP_COUNT] << 8 | a->dma_reg[D_FLOP_COUNT + 1];
            if (!count) break;
        }
        const uint32_t addr = chan_addr(a, ch);
        uint8_t b = 0;
        /* Bit 6: memory to device (the floppy driver sets it to write,
         * $FFD8FF2E; the SCSI HAL's StartAMIC sets it unless its
         * direction argument is nonzero, $FFC2A5C0-$FFC2A5D4). */
        const int to_dev = (a->dma_reg[co] & C_TO_DEV) != 0;
        if (to_dev) {
            if (!mc_dev_dma(&a->dev, addr, &b, 1, 0) || !c->port.write || !c->port.write(c->port.ctx, b)) break;
        } else {
            if (!c->port.read || !c->port.read(c->port.ctx, &b)) break;
            mc_dev_dma(&a->dev, addr, &b, 1, 1);
        }
        const uint32_t next = addr + 1u;
        put32(a->dma_reg + addr_offset(ch), floppy ? (reg32(a->dma_reg + addr_offset(ch)) & ~0x3FFFFu) | (next & 0x3FFFFu)
                                                   : next);
        /* The floppy counts 4-byte units†: one unit per four bytes. */
        if (floppy && (next & 3u) == 0) {
            count--;
            a->dma_reg[D_FLOP_COUNT] = (uint8_t)(count >> 8);
            a->dma_reg[D_FLOP_COUNT + 1] = (uint8_t)count;
            if (!count) {
                a->dma_reg[co] = (uint8_t)((a->dma_reg[co] & ~C_RUN) | C_INT);
                update_dma(a);
            }
        }
    }
    c->running = 0;
}

void mc_amic_set_port(mc_amic *a, unsigned channel, const mc_amic_port *port) {
    if (channel < MC_AMIC_BYTE_CHANNELS) a->ch[channel].port = *port;
}

/* The SWIM III's interrupt: with the floppy channel's control bit 3 set
 * it raises the channel's interrupt (the floppy driver sets bit 3 with
 * the SWIM's go and takes its completion from DMA flag 6, ROM $FFD90C30,
 * the handler $FFD90C40 in the DMA table's entry 6)†. */
void mc_amic_floppy_irq(mc_amic *a, int asserted) {
    if (asserted && (a->dma_reg[D_FLOP_CTL] & 0x08u)) {
        a->dma_reg[D_FLOP_CTL] |= C_INT;
        update_dma(a);
    }
}

void mc_amic_request(mc_amic *a, unsigned channel, int asserted) {
    if (channel >= MC_AMIC_BYTE_CHANNELS) return;
    a->ch[channel].request = asserted != 0;
    if (asserted) run_channel(a, channel);
}

/* --- Ethernet ---------------------------------------------------------------------- */

void mc_amic_set_enet_transmit(mc_amic *a, void (*transmit)(void *ctx, const uint8_t *frame, size_t length), void *ctx) {
    a->enet_transmit = transmit;
    a->enet_ctx = ctx;
}

static void enet_start_tx(mc_amic *a) {
    /* Buffer 0, the one the ROM's driver fills (amic.h). */
    const unsigned n = 0;
    const uint32_t count = ((uint32_t)a->dma_reg[D_ETX_COUNT + 0x10u * n] << 8 | a->dma_reg[D_ETX_COUNT + 1 + 0x10u * n]) & 0x7FFu;
    if (count) {
        uint8_t frame[MC_AMIC_ENET_TX_BUF];
        if (mc_dev_dma(&a->dev, mc_amic_dma_base(a) + MC_AMIC_AREA_ENET_TX + n * MC_AMIC_ENET_TX_BUF, frame, count, 0) &&
            a->enet_transmit)
            a->enet_transmit(a->enet_ctx, frame, count);
    }
    a->dma_reg[D_ETX_CTL] |= C_INT;
    update_dma(a);
}

int mc_amic_enet_receive(mc_amic *a, const uint8_t *frame, size_t length, const uint8_t status[4]) {
    if (!(a->dma_reg[D_ERX_CTL] & C_RUN) || length > 0xFFFu) return 0;
    const unsigned head = a->dma_reg[D_ERX_HEAD] % MC_AMIC_ENET_SLOTS, tail = a->dma_reg[D_ERX_TAIL] % MC_AMIC_ENET_SLOTS;
    const unsigned need = (unsigned)((length + 8u + MC_AMIC_ENET_SLOT - 1u) / MC_AMIC_ENET_SLOT);
    const unsigned used = (head + MC_AMIC_ENET_SLOTS - tail) % MC_AMIC_ENET_SLOTS;
    if ((a->dma_reg[D_ERX_CTL] & 0x40u) || used + need > MC_AMIC_ENET_SLOTS) return 0;
    /* The header: the MACE's RCVFS1:RCVFS0 (status nibble and 12-bit
     * count), RCVFS2, RCVFS3, four bytes 0†; the frame after it. */
    uint8_t buf[8 + 0xFFFu];
    buf[0] = (uint8_t)((status[1] & 0xF0u) | ((length >> 8) & 0x0Fu));
    buf[1] = (uint8_t)length;
    buf[2] = status[2];
    buf[3] = status[3];
    memset(buf + 4, 0, 4);
    memcpy(buf + 8, frame, length);
    const uint32_t ring = mc_amic_dma_base(a) + MC_AMIC_AREA_ENET_RX;
    unsigned slot = head;
    for (unsigned i = 0; i < need; i++) {
        const size_t off = (size_t)i * MC_AMIC_ENET_SLOT;
        const size_t n = length + 8u - off < MC_AMIC_ENET_SLOT ? length + 8u - off : MC_AMIC_ENET_SLOT;
        mc_dev_dma(&a->dev, ring + slot * MC_AMIC_ENET_SLOT, buf + off, (uint32_t)n, 1);
        slot = (slot + 1u) % MC_AMIC_ENET_SLOTS;
    }
    a->dma_reg[D_ERX_HEAD] = (uint8_t)slot;
    if (slot == tail) a->dma_reg[D_ERX_CTL] |= 0x40u;
    a->dma_reg[D_ERX_CTL] |= C_INT;
    update_dma(a);
    return 1;
}

/* --- the tick and sound ----------------------------------------------------------- */

static uint32_t snd_rate(const mc_amic *a) { return 11025u << ((a->snd_reg[0x10] >> 2) & 3u); }
static uint32_t snd_buf_size(const mc_amic *a) {
    const uint32_t kb = a->snd_reg[0x08];
    return kb && kb <= 8u ? kb * 1024u : 0x1000u;
}

static void ring_put(mc_amic *a, int16_t l, int16_t r) {
    if (!a->ring) return;
    if (a->ring_count == a->ring_size) {
        a->ring_head = (a->ring_head + 1u) % a->ring_size;
        a->ring_count--;
    }
    const size_t at = (a->ring_head + a->ring_count) % a->ring_size;
    a->ring[2 * at] = l;
    a->ring[2 * at + 1] = r;
    a->ring_count++;
}

/* The output, made up to `now`: one source frame each 1/rate, held at
 * the host's 44.1 kHz. */
static void snd_advance(mc_amic *a, uint64_t now) {
    if (now <= a->snd_time) return;
    const uint64_t out = (now * MC_AMIC_SOUND_RATE) / MC_C15M_HZ - (a->snd_time * MC_AMIC_SOUND_RATE) / MC_C15M_HZ;
    a->snd_time = now;
    for (uint64_t i = 0; i < out; i++) {
        if (!(a->snd_reg[0x10] & 1u)) {
            ring_put(a, 0, 0);
            continue;
        }
        a->snd_acc += snd_rate(a);
        while (a->snd_acc >= MC_AMIC_SOUND_RATE) {
            a->snd_acc -= MC_AMIC_SOUND_RATE;
            uint8_t f[4] = {0};
            const uint32_t addr = mc_amic_dma_base(a) + MC_AMIC_AREA_SND_OUT + a->snd_buf * 0x2000u + a->snd_pos;
            mc_dev_dma(&a->dev, addr, f, 4, 0);
            a->snd_played = 1;
            a->snd_frame[0] = (int16_t)(f[0] << 8 | f[1]);
            a->snd_frame[1] = (int16_t)(f[2] << 8 | f[3]);
            a->snd_pos += 4;
            if (a->snd_pos >= snd_buf_size(a)) {
                const uint8_t bit = a->snd_buf ? 0x80u : 0x40u;
                a->snd_flags14 |= bit;
                a->snd_flags18 |= bit;
                a->snd_pos = 0;
                a->snd_buf ^= 1u;
                update_dma(a);
            }
        }
        ring_put(a, a->snd_frame[0], a->snd_frame[1]);
    }
}

size_t mc_amic_pull(mc_amic *a, int16_t *stereo, size_t frames) {
    snd_advance(a, mc_dev_now(&a->dev));
    size_t n = frames < a->ring_count ? frames : a->ring_count;
    for (size_t i = 0; i < n; i++) {
        const size_t at = (a->ring_head + i) % a->ring_size;
        stereo[2 * i] = a->ring[2 * at];
        stereo[2 * i + 1] = a->ring[2 * at + 1];
    }
    a->ring_head = (a->ring_head + n) % a->ring_size;
    a->ring_count -= n;
    if (a->codec) mc_awac_output(a->codec, stereo, n);
    return n;
}

static void schedule(mc_amic *a) {
    uint64_t next = a->tick_start + TICK_PERIOD / 2u;
    if (a->snd_reg[0x10] & 1u) {
        /* The end of the buffer playing. */
        const uint64_t left = (snd_buf_size(a) - a->snd_pos) / 4u + 1u;
        const uint64_t t = a->snd_time + left * MC_C15M_HZ / snd_rate(a) + 1u;
        if (t < next) next = t;
    }
    mc_dev_schedule(&a->dev, next);
}

static void advance(mc_amic *a, uint64_t now) {
    if (now >= a->tick_start + TICK_PERIOD / 2u) {
        const uint64_t n = (now - a->tick_start) / (TICK_PERIOD / 2u);
        a->tick_start += n * (TICK_PERIOD / 2u);
        if (n & 1u) {
            a->tick ^= 1u;
            mc_dev_line(&a->dev, MC_AMIC_LINE_TICK, a->tick);
        }
    }
    snd_advance(a, now);
}

static void amic_event(void *dev, uint64_t now) {
    mc_amic *a = dev;
    advance(a, now);
    schedule(a);
}

/* --- the interrupt controller ---------------------------------------------------- */

static uint32_t int_read(void *dev, uint32_t offset, unsigned size) {
    mc_amic *a = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) {
        const uint32_t o = (offset + i) & 0xFFFu;
        uint8_t b = 0;
        if (o == 0) b = (uint8_t)((a->int_ctl & 0xC0u) | (a->sources & 0x3Fu));
        else if (o == 0x08) b = (uint8_t)a->dma_flags;
        else if (o == 0x0A) b = (uint8_t)(a->dma_flags >> 8);
        v = v << 8 | b;
    }
    return v;
}

static void int_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_amic *a = dev;
    for (unsigned i = 0; i < size; i++)
        if (((offset + i) & 0xFFFu) == 0) {
            a->int_ctl = (uint8_t)(value >> (8 * (size - 1 - i))) & 0xC0u;
            if (a->int_ctl & 0x80u) a->latch = 0;
        }
    update_cpu(a);
}

static void amic_reset(void *dev);
/* The interrupt window's ops carry the whole chip's event and reset (its
 * mc_dev header names them). */
const mc_dev_ops mc_amic_int_ops = {int_read, int_write, amic_event, amic_reset, NULL};

/* --- the pseudo-VIA2 --------------------------------------------------------------- */

static uint8_t via2_read_reg(mc_amic *a, unsigned reg) {
    switch (reg) {
    case 0x02: return (uint8_t)((~a->via2_slots & 0x7Fu) | (a->variant ? 0 : 0x80u));
    case 0x03: {
        const uint8_t f = a->via2_ifr & 0x7Fu;
        return (uint8_t)(f | ((f & a->via2_ier) ? 0x80u : 0));
    }
    case 0x12: return (uint8_t)(a->via2_sier | 0x80u);
    case 0x13: return (uint8_t)(a->via2_ier | 0x80u);
    default: return a->via2_reg[reg];
    }
}

static void via2_write_reg(mc_amic *a, unsigned reg, uint8_t v) {
    switch (reg) {
    case 0x02: break;
    /* Writing 1s clears flags, as the 6522's IFR (the edge-latched ones;
     * levels reassert at once). */
    case 0x03: a->via2_ifr &= (uint8_t)(0xFFu & ~(v & 0x7Fu)); break;
    case 0x12:
        if (v & 0x80u) a->via2_sier |= v & slot_mask(a);
        else a->via2_sier &= (uint8_t)(0xFFu & ~(v & slot_mask(a)));
        break;
    case 0x13:
        if (v & 0x80u) a->via2_ier |= v & 0x7Fu;
        else a->via2_ier &= (uint8_t)(0xFFu & ~(v & 0x7Fu));
        break;
    default: a->via2_reg[reg] = v; break;
    }
    update_via2(a);
}

static uint32_t via2_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | via2_read_reg(dev, (offset + i) & 0x1Fu);
    return v;
}

static void via2_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++) via2_write_reg(dev, (offset + i) & 0x1Fu, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_amic_via2_ops = {via2_read, via2_write, NULL, NULL, NULL};

/* --- DMA registers ---------------------------------------------------------------- */

static uint8_t dma_read_reg(mc_amic *a, uint32_t o) {
    if (o >= D_FLOP_ADDR && o < D_FLOP_ADDR + 4) {
        const uint32_t v = chan_addr(a, MC_AMIC_CH_FLOPPY);
        return (uint8_t)(v >> (8 * (3 - (o - D_FLOP_ADDR))));
    }
    if (o == D_BASE + 2 || o == D_BASE + 3) return 0;
    if (a->variant && (o == D_SCSI_A_CTL || o == D_SCSI_B_CTL))
        return (uint8_t)((a->dma_reg[o] & ~0x20u) | (a->variant == MC_AMIC_FAT_2 ? 0x20u : 0));
    if (o == D_ETX_CTL) return a->dma_reg[o] | 0x60u;     /* ready for a frame (amic.h) */
    return a->dma_reg[o];
}

static void control_write(mc_amic *a, uint32_t o, uint8_t v) {
    uint8_t c = a->dma_reg[o];
    const uint8_t was = c;
    if (v & C_INT) c &= (uint8_t)(0xFFu & ~C_INT);            /* a write with bit 7 clears the interrupt */
    if (o == D_ERX_CTL && (v & 0x40u)) c &= 0xBFu;
    c = (uint8_t)((c & (C_INT | 0x40u)) | (v & ~(C_INT | 0x40u)));
    if (o != D_ERX_CTL) c = (uint8_t)((c & C_INT) | (v & ~C_INT));
    /* Bit 4 flushes what the channel holds; Livadix's channels hold nothing,
     * so it reads clear at once (the SCSI HAL's Wt4AMICComplete sets it
     * and waits for it to clear, $FFC2A612)†. */
    if (o == D_SCSI_A_CTL || o == D_SCSI_B_CTL) c &= 0xEFu;
    a->dma_reg[o] = c;
    /* Bit 0 set resets the channel; the drivers then keep it set (they
     * set and clear the other bits around it, $FFD8FB1E, $FFC2A5C6), so
     * only its rising edge counts†. */
    if ((v & C_RESET) && !(was & C_RESET)) {
        a->dma_reg[o] &= (uint8_t)(0xFFu & ~(C_INT | C_RUN));
        if (o == D_ERX_CTL) {
            a->dma_reg[o] &= 0xBFu;
            a->dma_reg[D_ERX_HEAD] = 0;
        }
    }
    update_dma(a);
    if (o == D_ETX_CTL && (v & C_RUN)) enet_start_tx(a);
    for (unsigned ch = 0; ch < MC_AMIC_BYTE_CHANNELS; ch++)
        if (ctl_offset(ch) == o && a->ch[ch].request) run_channel(a, ch);
}

static void dma_write_reg(mc_amic *a, uint32_t o, uint8_t v) {
    switch (o) {
    case D_ETX_CTL: case D_ERX_CTL: case D_SCSI_A_CTL: case D_SCSI_B_CTL: case D_FLOP_CTL:
        control_write(a, o, v);
        return;
    /* The serial channels' control bytes: bit 4 asks for a flush and bit
     * 5 reads set when it is done (the serial DMA driver sets bit 4 and
     * waits for bit 5, then clears bits 7 and 4; observed in its native
     * code); a write with bit 7 clears the interrupt. No serial DMA moves
     * data in Livadix†. */
    case D_SER_CTL: case D_SER_CTL + 0x10: case D_SER_CTL + 0x20: case D_SER_CTL + 0x30: {
        uint8_t c = (uint8_t)((a->dma_reg[o] & C_INT) | (v & 0x5Fu));
        if (v & C_INT) c &= 0x7Fu;
        if (v & 0x10u) c |= 0x20u;
        a->dma_reg[o] = c;
        return;
    }
    default:
        a->dma_reg[o] = v;
        return;
    }
}

static uint32_t dma_read(void *dev, uint32_t offset, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | dma_read_reg(dev, (offset + i) % MC_AMIC_DMA_SIZE);
    return v;
}

static void dma_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    for (unsigned i = 0; i < size; i++)
        dma_write_reg(dev, (offset + i) % MC_AMIC_DMA_SIZE, (uint8_t)(value >> (8 * (size - 1 - i))));
}

const mc_dev_ops mc_amic_dma_ops = {dma_read, dma_write, NULL, NULL, NULL};

/* --- sound registers ------------------------------------------------------------- */

static uint8_t snd_read_reg(mc_amic *a, uint32_t o, uint64_t now) {
    switch (o) {
    case 0x00: return (uint8_t)((a->snd_reg[0] & 0x40u) | (now < a->codec_busy_until ? 0x80u : 0));
    case 0x04: case 0x05: case 0x06: case 0x07: return a->codec ? mc_awac_status(a->codec, o - 4u) : 0;
    case 0x0C: case 0x0D: case 0x0E: {
        /* The output's position: bytes played into the buffer pair, nonzero
         * only while the output runs†. */
        const uint32_t p = (a->snd_reg[0x10] & 1u) ? (a->snd_buf * 0x2000u + a->snd_pos) << 6 | 0x40u : 0;
        return (uint8_t)(p >> (8 * (0x0E - o)));
    }
    case 0x14: return (uint8_t)(a->snd_flags14 | (a->snd_reg[0x14] & 0x0Fu));
    case 0x18: return a->snd_flags18;
    default: return a->snd_reg[o & 0x1Fu];
    }
}

static void snd_write_reg(mc_amic *a, uint32_t o, uint8_t v, uint64_t now) {
    switch (o) {
    case 0x00:
        a->snd_reg[0] = v;
        if (v & 0x80u) {
            if (a->codec) mc_awac_control(a->codec, (uint16_t)(a->snd_reg[1] << 8 | a->snd_reg[2]));
            a->codec_busy_until = now + CODEC_BUSY;
        }
        break;
    case 0x10: {
        const uint8_t was = a->snd_reg[0x10];
        a->snd_reg[0x10] = v;
        if ((v & 1u) && !(was & 1u)) {
            a->snd_buf = 0;
            a->snd_pos = 0;
            a->snd_acc = 0;
        }
        break;
    }
    case 0x14:
        a->snd_flags14 &= (uint8_t)(0xFFu & ~(v & 0xF0u));
        a->snd_reg[0x14] = v & 0x0Fu;
        break;
    case 0x18: a->snd_flags18 &= (uint8_t)(0xFFu & ~(v & 0xF0u)); break;
    default:
        if (o < sizeof a->snd_reg) a->snd_reg[o] = v;
        break;
    }
    update_dma(a);
}

static uint32_t snd_read(void *dev, uint32_t offset, unsigned size) {
    mc_amic *a = dev;
    const uint64_t now = mc_dev_now(&a->dev);
    advance(a, now);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | snd_read_reg(a, (offset + i) & 0x1Fu, now);
    return v;
}

static void snd_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    mc_amic *a = dev;
    const uint64_t now = mc_dev_now(&a->dev);
    advance(a, now);
    for (unsigned i = 0; i < size; i++) snd_write_reg(a, (offset + i) & 0x1Fu, (uint8_t)(value >> (8 * (size - 1 - i))), now);
    schedule(a);
}

const mc_dev_ops mc_amic_sound_ops = {snd_read, snd_write, NULL, NULL, NULL};

/* --- the diagnostic input --------------------------------------------------------- */

static uint32_t diag_read(void *dev, uint32_t offset, unsigned size) {
    const mc_amic *a = dev;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | (((offset + i) & 0xFFFu) == 0 ? a->diag : 0);
    return v;
}

static void diag_write(void *dev, uint32_t offset, unsigned size, uint32_t value) {
    (void)dev, (void)offset, (void)size, (void)value;
}

const mc_dev_ops mc_amic_diag_ops = {diag_read, diag_write, NULL, NULL, NULL};

/* --- setup ------------------------------------------------------------------------- */

static void amic_reset(void *dev) {
    mc_amic *a = dev;
    a->int_ctl = 0;
    a->latch = a->seen = 0;
    memset(a->via2_reg, 0, sizeof a->via2_reg);
    a->via2_ier = a->via2_sier = 0;
    a->via2_ifr &= MC_AMIC_VIA2_SCSI_A_DRQ | MC_AMIC_VIA2_SCSI_B_DRQ | MC_AMIC_VIA2_SCSI_A_IRQ | MC_AMIC_VIA2_SCSI_B_IRQ |
                   MC_AMIC_VIA2_FDC | MC_AMIC_VIA2_BIT4;
    memset(a->dma_reg, 0, sizeof a->dma_reg);
    put32(a->dma_reg + D_FLOP_ADDR, MC_AMIC_AREA_FLOPPY);
    memset(a->snd_reg, 0, sizeof a->snd_reg);
    a->snd_flags14 = a->snd_flags18 = 0;
    a->snd_buf = 0;
    a->snd_pos = 0;
    a->snd_played = 0;
    a->ring_head = a->ring_count = 0;
    a->snd_acc = 0;
    a->snd_time = mc_dev_now(&a->dev);
    a->snd_frame[0] = a->snd_frame[1] = 0;
    update_dma(a);
    update_via2(a);
    schedule(a);
}

void mc_amic_init(mc_amic *a, const mc_hw_host *host, const char *name) {
    memset(a, 0, sizeof *a);
    a->dev.ops = &mc_amic_int_ops;
    a->dev.host = host;
    a->dev.name = name;
    a->diag = 0x01;
    a->ring_size = RING_FRAMES;
    a->ring = calloc(2u * RING_FRAMES, sizeof *a->ring);
    a->snd_time = a->tick_start = mc_dev_now(&a->dev);
    amic_reset(a);
}

void mc_amic_free(mc_amic *a) {
    free(a->ring);
    a->ring = NULL;
}

void mc_amic_set_codec(mc_amic *a, struct mc_awac *codec) { a->codec = codec; }
