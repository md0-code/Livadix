/* Livadix — the AT&T DSP3210 (dsp3210.h). Livadix's own code, written from the
 * DSP3210 Information Manual; section numbers below are the manual's. */
#include "dsp3210.h"

#include <math.h>
#include <string.h>

/* pcw bits (Table 10-8) */
#define PCW_BLN (1u << 8)
#define PCW_CPN (1u << 10)
#define PCW_BRC (1u << 13)
/* emr bits (Table 10-7) */
#define EMR_AERR (1u << 4)
#define EMR_UV (1u << 5)
#define EMR_NAN (1u << 6)
#define EMR_EXT0 (1u << 8)
#define EMR_TIMER (1u << 9)
#define EMR_EXT1 (1u << 15)
/* DAU flags as kept: the ps register's order (bits 4-7 there) */
#define DF_N 1u
#define DF_Z 2u
#define DF_U 4u
#define DF_V 8u
/* exception vectors (Figure 7-5) */
enum { VEC_RESET = 0, VEC_BUS = 1, VEC_ILLEGAL = 2, VEC_ADDRESS = 4, VEC_DAU = 5, VEC_NAN = 6,
       VEC_EXT0 = 8, VEC_TIMER = 9, VEC_EXT1 = 15 };

static uint32_t sext16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)(uint16_t)v; }
static uint32_t sext8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)(uint8_t)v; }

/* --- floating point (3.3.3, 8.2.3) ------------------------------------------------ */

/* A 40-bit accumulator: bits 39..8 the mantissa (s, 23 fraction bits, 8
 * guard bits), 7..0 the exponent; value M * 2^(e-128), M = (-2)^s + 0.f. */
static double acc_value(uint64_t a) {
    const unsigned e = (unsigned)(a & 0xFFu);
    if (!e) return 0.0;
    const uint32_t m = (uint32_t)(a >> 8);
    const int64_t f = (int64_t)(m & 0x7FFFFFFFu);
    const int64_t q = (m & 0x80000000u) ? f - ((int64_t)1 << 32) : ((int64_t)1 << 31) + f;
    return ldexp((double)q, (int)e - 128 - 31);
}
static uint64_t f32_acc(uint32_t f) { return ((uint64_t)(f & 0xFFFFFF00u) << 8) | (f & 0xFFu); }
static uint32_t acc_f32(uint64_t a) { return (uint32_t)((a >> 8) & 0xFFFFFF00u) | (uint32_t)(a & 0xFFu); }

/* A value normalized into the 40-bit format, truncated (two's complement
 * floor) to `frac` fraction bits (31 for the accumulators, 23 for the 32-bit
 * format); *flags gets N Z U V. Overflow saturates, underflow gives zero. */
static uint64_t acc_from(double x, unsigned frac, uint8_t *flags) {
    uint8_t fl = 0;
    uint64_t out;
    if (x == 0.0 || x != x) {
        fl = DF_Z;
        out = 0;
    } else {
        int ex;
        const double fr = frexp(x, &ex);
        double m = 2.0 * fr;
        int e = ex - 1;
        if (m == -1.0) { m = -2.0; e--; }
        double q = floor(ldexp(m, (int)frac));
        int64_t qi = (int64_t)q << (31 - frac);
        if (qi >= ((int64_t)1 << 32)) { qi >>= 1; e++; }       /* only from rounding up (round) */
        e += 128;
        if (e > 255) {
            fl = DF_V | (x < 0 ? DF_N : 0);
            out = x < 0 ? ((uint64_t)0x80000000u << 8) | 0xFFu : ((uint64_t)0x7FFFFFFFu << 8) | 0xFFu;
            if (frac < 31 && x >= 0) out &= ~(uint64_t)0xFF00u;
        } else if (e < 1) {
            fl = DF_U | DF_Z;
            out = 0;
        } else {
            const uint32_t m32 = qi < 0 ? (uint32_t)(qi + ((int64_t)1 << 32)) | 0x80000000u : (uint32_t)(qi - ((int64_t)1 << 31));
            out = ((uint64_t)m32 << 8) | (uint64_t)e;
            if (x < 0) fl |= DF_N;
        }
    }
    if (flags) *flags = fl;
    return out;
}

double mc_dsp3210_f32_value(uint32_t f) { return acc_value(f32_acc(f)); }
uint32_t mc_dsp3210_f32_from(double x) { return acc_f32(acc_from(x, 23, NULL)); }

static uint32_t f32_exp(uint32_t f) { return f & 0xFFu; }

/* The product register P (8.2.3): the exact product of two 24-bit
 * mantissas truncated to 31 fraction bits of its unnormalized form. */
static double product(uint32_t x, uint32_t y) {
    if (!f32_exp(x) || !f32_exp(y)) return 0.0;
    const double p = mc_dsp3210_f32_value(x) * mc_dsp3210_f32_value(y);
    const int scale = (int)f32_exp(x) - 128 + (int)f32_exp(y) - 128;
    return ldexp(floor(ldexp(p, 31 - scale)), scale - 31);
}

/* IEEE single <-> DSP 32-bit (8.2.4.1). */
static uint32_t to_ieee(uint32_t f) {
    if (!f32_exp(f)) return 0;
    const float v = (float)mc_dsp3210_f32_value(f);
    uint32_t u;
    memcpy(&u, &v, 4);
    if (((u >> 23) & 0xFFu) == 0) u &= 0x80000000u;     /* no denormals */
    if ((u & 0x7FFFFFFFu) == 0) u = 0;                  /* no negative zero */
    return u;
}
static uint64_t from_ieee(uint32_t u, uint8_t *flags, int *nan) {
    const uint32_t e = (u >> 23) & 0xFFu, m = u & 0x7FFFFFu;
    *nan = e == 255 && m != 0;
    if (e == 255) {
        *flags = (u & 0x80000000u) ? DF_N | DF_V : DF_V;
        return f32_acc((u & 0x80000000u) ? 0x800000FFu : 0x7FFFFFFFu);
    }
    if (e == 0) { *flags = DF_Z; return 0; }
    float v;
    memcpy(&v, &u, 4);
    return acc_from((double)v, 31, flags);
}

/* mu-law and A-law (8.2.4.2): bit 7 s, bits 6-4 N, bits 3-0 M; mu-law
 * stored complemented, A-law with s, N2, N0, M2, M0 complemented. */
static double mulaw_value(uint8_t b, int alaw) {
    const unsigned c = alaw ? (unsigned)(b ^ 0xD5u) : (unsigned)(b ^ 0xFFu);
    const int s = (c >> 7) & 1, n = (c >> 4) & 7, m = c & 15;
    double y;
    if (!alaw) y = (16.5 + m) * (double)(1u << n) - 16.5;
    else y = n ? (16.5 + m) * (double)(1u << n) : (0.5 + m) * 2.0;
    return s ? -y : y;
}
static uint8_t mulaw_byte(double y, int alaw) {
    const int s = y < 0;
    const double mag = s ? -y : y;
    unsigned best = 0;
    double bestd = -1.0;
    for (unsigned code = 0; code < 128; code++) {
        const unsigned raw = (unsigned)(s << 7) | code;
        const uint8_t b = (uint8_t)(alaw ? raw ^ 0xD5u : raw ^ 0xFFu);
        const double v = fabs(mulaw_value(b, alaw));
        const double dist = fabs(v - mag);
        if (bestd < 0 || dist < bestd) { bestd = dist; best = b; }
    }
    return (uint8_t)best;
}

/* float -> integer with the dauc's rounding (8.2.5), saturated to [lo, hi]. */
static int64_t to_int(const mc_dsp3210 *d, double x, int64_t lo, int64_t hi) {
    const unsigned mode = (d->dauc >> 4) & 3u;
    double r;
    if (!(mode & 1u)) r = floor(x + 0.5);
    else if (mode == 1u) r = floor(x);
    else r = x < 0 ? ceil(x) : floor(x);
    if (r < (double)lo) return lo;
    if (r > (double)hi) return hi;
    return (int64_t)r;
}

/* --- memory ------------------------------------------------------------------------ */

static void raise_error(mc_dsp3210 *d, int vec, int maskable);

static uint32_t chip_base(const mc_dsp3210 *d) { return (d->pcw & PCW_CPN) ? 0x00000000u : 0x50030000u; }

static uint32_t ld(const uint8_t *p, unsigned size) {
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v = v << 8 | p[i];
    return v;
}
static void st(uint8_t *p, unsigned size, uint32_t v) {
    for (unsigned i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8u * (size - 1u - i)));
}

static void bio_update(mc_dsp3210 *d) {
    if (d->bus.bio) d->bus.bio(d->bus.ctx, (uint8_t)(d->bio_out & d->bioc), d->bioc);
}

static uint32_t bio_read(const mc_dsp3210 *d) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        const unsigned out = (d->bio_out >> i) & 1u;
        const unsigned pin = ((d->bioc >> i) & 1u) ? out : ((d->bio_in >> i) & 1u);
        v |= (uint32_t)(pin | out << 1) << (2u * i);
    }
    return v;
}

/* The MMIO registers (Figure 3-5, big-endian addresses). */
static uint32_t mmio_read(mc_dsp3210 *d, uint32_t off, unsigned size) {
    (void)size;
    switch (off) {
    case 0x404: return d->ibuf;
    case 0x40C: return d->ioc;
    case 0x413: return d->tcon;
    case 0x414: return d->timer;
    case 0x41B: return d->bioc;
    case 0x41E: return bio_read(d);
    case 0x420: return d->idp;
    case 0x424: return d->odp;
    case 0x42A: return d->icnt;
    case 0x42E: return d->ocnt;
    case 0x433: return d->dmac;
    default: return 0;
    }
}
static void mmio_write(mc_dsp3210 *d, uint32_t off, unsigned size, uint32_t v) {
    (void)size;
    switch (off) {
    case 0x408: d->obuf = v; break;
    case 0x40C: d->ioc = v & 0x00FFFFFFu; break;
    case 0x413: d->tcon = (uint8_t)v; break;
    case 0x414: d->timer = d->timer_init = v; break;
    case 0x41B: d->bioc = (uint8_t)v; bio_update(d); break;
    case 0x41E:
        for (unsigned i = 0; i < 8; i++) {
            const unsigned op = (v >> (2u * i)) & 3u, bit = 1u << i;
            if (op == 1) d->bio_out = (uint8_t)(d->bio_out & ~bit);
            else if (op == 2) d->bio_out |= (uint8_t)bit;
            else if (op == 3) d->bio_out ^= (uint8_t)bit;
        }
        bio_update(d);
        break;
    case 0x420: d->idp = v; break;
    case 0x424: d->odp = v; break;
    case 0x42A: d->icnt = (uint16_t)v; break;
    case 0x42E: d->ocnt = (uint16_t)v; break;
    case 0x433: d->dmac = (uint8_t)v; break;
    default: break;
    }
}

/* 0 = done, else an error raised (the caller abandons the instruction). */
static int mem_read(mc_dsp3210 *d, uint32_t a, unsigned size, uint32_t *v) {
    if ((size == 4 && (a & 3u)) || (size == 2 && (a & 1u))) {
        if (d->emr & EMR_AERR) { raise_error(d, VEC_ADDRESS, 1); return 1; }
        a &= ~(size - 1u);
    }
    if ((a & 0xFFFF0000u) == chip_base(d)) {
        const uint32_t off = a & 0xFFFFu;
        if (off >= 0xF000u) *v = ld(d->ram0 + (off - 0xF000u), size);
        else if (off >= 0xE000u) *v = ld(d->ram1 + (off - 0xE000u), size);
        else if (off >= 0x400u && off < 0x800u) *v = mmio_read(d, off, size);
        else *v = 0;                                    /* boot ROM, reserved† */
        return 0;
    }
    if (d->read_map && (a & 0xFFFu) <= 0x1000u - size) {
        const uint8_t *page = d->read_map[a >> MC_DSP3210_PAGE_SHIFT];
        if (page) { *v = ld(page + (a & 0xFFFu), size); return 0; }
    }
    if (d->bus.read && !d->bus.read(d->bus.ctx, a, size, v)) return 0;
    *v = 0;
    raise_error(d, VEC_BUS, 0);
    return 1;
}
static int mem_write(mc_dsp3210 *d, uint32_t a, unsigned size, uint32_t v) {
    if (d->bus.watch) d->bus.watch(d->bus.ctx, a, size, v);
    if ((size == 4 && (a & 3u)) || (size == 2 && (a & 1u))) {
        if (d->emr & EMR_AERR) { raise_error(d, VEC_ADDRESS, 1); return 1; }
        a &= ~(size - 1u);
    }
    if ((a & 0xFFFF0000u) == chip_base(d)) {
        const uint32_t off = a & 0xFFFFu;
        if (off >= 0xF000u) st(d->ram0 + (off - 0xF000u), size, v);
        else if (off >= 0xE000u) st(d->ram1 + (off - 0xE000u), size, v);
        else if (off >= 0x400u && off < 0x800u) mmio_write(d, off, size, v);
        return 0;
    }
    if (d->write_map && (a & 0xFFFu) <= 0x1000u - size) {
        uint8_t *page = d->write_map[a >> MC_DSP3210_PAGE_SHIFT];
        if (page) { st(page + (a & 0xFFFu), size, v); return 0; }
    }
    if (d->bus.write && !d->bus.write(d->bus.ctx, a, size, v)) return 0;
    raise_error(d, VEC_BUS, 0);
    return 1;
}

/* The DA writes in flight (4.4.2.1). */
static void commit(mc_dsp3210 *d) {
    unsigned k = 0;
    for (unsigned i = 0; i < d->npend; i++) {
        if (d->pend[i].due <= d->steps) (void)mem_write(d, d->pend[i].address, d->pend[i].size, d->pend[i].value);
        else d->pend[k++] = d->pend[i];
    }
    d->npend = k;
}
static void da_write(mc_dsp3210 *d, uint32_t a, unsigned size, uint32_t v) {
    if (d->npend == MC_DSP3210_PENDING) {           /* never more than four in flight; keep order */
        d->steps += 4;
        commit(d);
        d->steps -= 4;
    }
    d->pend[d->npend].address = a;
    d->pend[d->npend].value = v;
    d->pend[d->npend].size = size;
    d->pend[d->npend].due = d->steps + 4;
    d->npend++;
}

/* --- registers ---------------------------------------------------------------------- */

/* A CAU register field (Table 10-2): 0 r0, 1-14 r1-r14, 15 pc, 17-21
 * r15-r19, 24-26 r20-r22, 30 pcsh; in an ALU source 22 and 23 are -1 and +1. */
static int reg_index(unsigned code) {
    if (code >= 1 && code <= 14) return (int)code;
    if (code >= 17 && code <= 21) return (int)code - 2;
    if (code >= 24 && code <= 26) return (int)code - 4;
    return -1;
}
static uint32_t reg_get(const mc_dsp3210 *d, unsigned code, uint32_t cur) {
    const int i = reg_index(code);
    if (i >= 0) return d->r[i];
    switch (code) {
    case 15: return cur + 8u;               /* the pc reads two instructions ahead */
    case 22: return 0xFFFFFFFFu;
    case 23: return 1u;
    case 30: return d->pcsh;
    default: return 0;
    }
}
static void reg_set(mc_dsp3210 *d, unsigned code, uint32_t v) {
    const int i = reg_index(code);
    if (i >= 0) d->r[i] = v;
}

static uint32_t ps_read(const mc_dsp3210 *d) {
    return (uint32_t)d->n | (uint32_t)d->z << 1 | (uint32_t)d->v << 2 | (uint32_t)d->c << 3 |
           (uint32_t)(d->dflags & 15u) << 4 | 1u << 9 /* obe */ |
           (uint32_t)((d->irn & 1u) ? 0 : 1) << 12 | (uint32_t)((d->irn & 2u) ? 0 : 1) << 13;
}

static void take_wait(mc_dsp3210 *d);

static uint32_t ior_get(mc_dsp3210 *d, unsigned r) {
    switch (r) {
    case 0: return ps_read(d);
    case 8: return d->emr;
    case 12: return d->pcw;
    case 14: return d->dauc;
    case 15: return d->ctr;
    default: return 0;
    }
}
static void ior_set(mc_dsp3210 *d, unsigned r, unsigned size, uint32_t v) {
    switch (r) {
    case 0:
        d->n = v & 1u; d->z = (v >> 1) & 1u; d->v = (v >> 2) & 1u; d->c = (v >> 3) & 1u;
        break;
    case 8:
        /* emr[0] (SL) shows the level on IACK1-0 while set: clearing it
         * while an external interrupt is served raises its IACK again,
         * which acknowledges it (Table 10-7; Table 5-2, IACK0-1). */
        if ((d->emr & 1u) && !(v & 1u) && d->level == 1 && d->servicing >= 0 && d->bus.iack)
            d->bus.iack(d->bus.ctx, d->servicing);
        d->emr = (uint16_t)v;
        break;
    case 10:                                /* spc: sftrst (byte) and waiti (long) */
        if (size == 1) d->level = 0;
        else take_wait(d);
        break;
    case 12:
        if (!(d->pcw & PCW_BRC)) d->pcw = (uint16_t)v;
        break;
    case 14: d->dauc = (uint8_t)(v & 0x7Fu); break;
    case 15: d->ctr = (uint8_t)(v & 0x3Fu); break;
    default: break;
    }
}

/* --- DAU history (4.4.2.2, 4.4.2.4) ------------------------------------------------ */

static void acc_set(mc_dsp3210 *d, unsigned n, uint64_t v) {
    d->a[n] = v;
    d->ahead[n] = (d->ahead[n] + 1u) % MC_DSP3210_HIST;
    d->ahist[n][d->ahead[n]].value = v;
    d->ahist[n][d->ahead[n]].step = d->steps;
}
/* An accumulator as a multiplier input: as left by the instruction three before. */
static uint64_t acc_mult(const mc_dsp3210 *d, unsigned n) {
    const uint64_t limit = d->steps >= 3 ? d->steps - 3 : 0;
    for (unsigned k = 0; k < MC_DSP3210_HIST; k++) {
        const unsigned i = (d->ahead[n] + MC_DSP3210_HIST - k) % MC_DSP3210_HIST;
        if (d->ahist[n][i].step <= limit || (d->steps < 3 && d->ahist[n][i].step == 0)) return d->ahist[n][i].value;
    }
    return d->ahist[n][(d->ahead[n] + 1u) % MC_DSP3210_HIST].value;
}
static void dflags_set(mc_dsp3210 *d, uint8_t f) {
    d->dflags = f;
    d->ctr = (uint8_t)(((d->ctr << 1) | (f & DF_N ? 1u : 0u)) & 0x3Fu);
    d->fhead = (d->fhead + 1u) % MC_DSP3210_HIST;
    d->fhist[d->fhead].flags = f;
    d->fhist[d->fhead].step = d->steps;
}
/* The DAU flags a CA condition sees: the last DA instruction four or more before. */
static uint8_t dflags_late(const mc_dsp3210 *d) {
    for (unsigned k = 0; k < MC_DSP3210_HIST; k++) {
        const unsigned i = (d->fhead + MC_DSP3210_HIST - k) % MC_DSP3210_HIST;
        if (d->fhist[i].step + 4u <= d->steps) return d->fhist[i].flags;
    }
    return 0;
}

static int cond(const mc_dsp3210 *d, unsigned c) {
    uint8_t f;
    switch (c) {
    case 0: return 0;
    case 1: return 1;
    case 2: return !d->n;
    case 3: return d->n;
    case 4: return !d->z;
    case 5: return d->z;
    case 6: return !d->v;
    case 7: return d->v;
    case 8: return !d->c;
    case 9: return d->c;
    case 10: return !(d->n ^ d->v);
    case 11: return d->n ^ d->v;
    case 12: return !(d->z | (d->n ^ d->v));
    case 13: return d->z | (d->n ^ d->v);
    case 14: return !(d->c | d->z);
    case 15: return d->c | d->z;
    case 32: return 1;              /* ibe: the serial input is idle */
    case 33: return 0;
    case 34: return 0;              /* obf */
    case 35: return 1;              /* obe */
    case 40: return 1;              /* syc */
    case 41: return 0;
    case 42: return 1;              /* fbc */
    case 43: return 0;
    case 44: return (d->irn & 1u) != 0;     /* ir0c: the pin low */
    case 45: return (d->irn & 1u) == 0;
    case 46: return (d->irn & 2u) != 0;
    case 47: return (d->irn & 2u) == 0;
    default: break;
    }
    if (c >= 16 && c <= 25) {
        f = dflags_late(d);
        const int N = (f & DF_N) != 0, Z = (f & DF_Z) != 0, U = (f & DF_U) != 0, V = (f & DF_V) != 0;
        switch (c) {
        case 16: return !U;
        case 17: return U;
        case 18: return !N;
        case 19: return N;
        case 20: return !Z;
        case 21: return Z;
        case 22: return !V;
        case 23: return V;
        case 24: return !(N | Z);
        case 25: return N | Z;
        default: break;
        }
    }
    return 0;
}

/* --- exceptions (7.5) ------------------------------------------------------------------ */

static void raise_error(mc_dsp3210 *d, int vec, int maskable) {
    if (d->fault) return;                           /* one per instruction */
    if (d->level == 3) {
        if (maskable) return;
        d->stopped = 1;                             /* double error: until RESTN */
        if (d->bus.iack) { d->bus.iack(d->bus.ctx, 0); d->bus.iack(d->bus.ctx, 1); }
        d->fault = vec + 1;
        return;
    }
    if (d->level == 2 && maskable) return;
    d->fault = vec + 1;
}

/* The abort (7.5.2): call evtp + vec*8 (r20), nop. */
static void error_entry(mc_dsp3210 *d, int vec) {
    const int maskable = vec == VEC_ADDRESS || vec == VEC_DAU || vec == VEC_NAN;
    d->level = maskable ? 2 : 3;
    d->r[20] = d->pc;
    d->pc = d->r[22] + 8u * (uint32_t)vec;
    d->delay = 0;
    d->do_active = 0;
    d->npend = 0;
    d->wait = 0;
    d->tcon = (uint8_t)(d->tcon & 0xFEu);
    d->dmac = (uint8_t)(d->dmac & 0xEEu);
    d->pcw = (uint16_t)(d->pcw & ~PCW_BRC);
}

static uint16_t requests(const mc_dsp3210 *d) {
    uint16_t r = d->irq_latched;
    if (d->irn & 1u) r |= EMR_EXT0;
    if (d->irn & 2u) r |= EMR_EXT1;
    return (uint16_t)(r & d->emr & (EMR_EXT0 | EMR_TIMER | EMR_EXT1));
}

static void take_wait(mc_dsp3210 *d) {
    if (!requests(d)) d->wait = 1;                  /* a pending interrupt makes waiti a nop */
}

static void interrupt_entry(mc_dsp3210 *d) {
    const uint16_t r = requests(d);
    int vec;
    if (r & EMR_EXT0) vec = VEC_EXT0;
    else if (r & EMR_TIMER) { vec = VEC_TIMER; d->irq_latched = (uint16_t)(d->irq_latched & ~EMR_TIMER); }
    else vec = VEC_EXT1;
    memcpy(d->sh.a, d->a, sizeof d->a);
    d->sh.dauc = d->dauc;
    d->sh.n = d->n; d->sh.z = d->z; d->sh.v = d->v; d->sh.c = d->c;     /* IRETURN: flags restored (4-58) */
    d->sh.dflags = d->dflags;
    d->sh.do_active = d->do_active;
    d->sh.do_left = d->do_left;
    d->sh.do_len = d->do_len;
    d->sh.do_count = d->do_count;
    d->sh.do_start = d->do_start;
    d->do_active = 0;
    d->pcsh = d->pc;
    d->level = 1;
    d->pc = d->r[22] + 8u * (uint32_t)vec;
    d->delay = 0;
    d->servicing = vec == VEC_EXT0 ? 0 : vec == VEC_EXT1 ? 1 : -1;
    if (d->servicing >= 0 && d->bus.iack) d->bus.iack(d->bus.ctx, d->servicing);
}

static void ireturn(mc_dsp3210 *d) {
    for (unsigned i = 0; i < 4; i++) acc_set(d, i, d->sh.a[i]);
    d->dauc = d->sh.dauc;
    d->n = d->sh.n; d->z = d->sh.z; d->v = d->sh.v; d->c = d->sh.c;
    d->dflags = d->sh.dflags;
    d->do_active = d->sh.do_active;
    d->do_left = d->sh.do_left;
    d->do_len = d->sh.do_len;
    d->do_count = d->sh.do_count;
    d->do_start = d->sh.do_start;
    d->level = 0;
    d->servicing = -1;
    d->pc = d->pcsh;
    d->delay = 0;
    d->lock = 1;
}

/* --- CA instructions ----------------------------------------------------------------- */

typedef struct alu_out { uint32_t r; uint8_t n, z, v, c; } alu_out;

static void flags_nz(alu_out *o, uint32_t r, int shrt) {
    o->n = shrt ? (r >> 15) & 1u : r >> 31;
    o->z = shrt ? (r & 0xFFFFu) == 0 : r == 0;
}
static alu_out add(uint32_t a, uint32_t b, unsigned cin, int shrt) {
    alu_out o = {0};
    if (shrt) {
        const uint32_t s = (a & 0xFFFFu) + (b & 0xFFFFu) + cin;
        o.r = sext16(s);
        o.c = (s >> 16) & 1u;
        o.v = ((~(a ^ b) & (a ^ s)) >> 15) & 1u;
    } else {
        const uint64_t s = (uint64_t)a + b + cin;
        o.r = (uint32_t)s;
        o.c = (uint8_t)(s >> 32);
        o.v = ((~(a ^ b) & (a ^ o.r)) >> 31) & 1u;
    }
    flags_nz(&o, o.r, shrt);
    return o;
}
/* a - b; c is the borrow (SUBTRACT: negating 0 leaves c 0). */
static alu_out sub(uint32_t a, uint32_t b, int shrt) {
    alu_out o = {0};
    if (shrt) {
        const uint32_t x = a & 0xFFFFu, y = b & 0xFFFFu, s = (x - y) & 0x1FFFFu;
        o.r = sext16(s);
        o.c = x < y;
        o.v = (((x ^ y) & (x ^ s)) >> 15) & 1u;
    } else {
        o.r = a - b;
        o.c = a < b;
        o.v = (((a ^ b) & (a ^ o.r)) >> 31) & 1u;
    }
    flags_nz(&o, o.r, shrt);
    return o;
}
static uint32_t bitrev(uint32_t v, unsigned bits) {
    uint32_t r = 0;
    for (unsigned i = 0; i < bits; i++) r |= ((v >> i) & 1u) << (bits - 1u - i);
    return r;
}
static alu_out logic_result(uint32_t r, int shrt) {
    alu_out o = {0};
    o.r = shrt ? sext16(r) : r;
    flags_nz(&o, o.r, shrt);
    return o;
}

/* The ALU and barrel shifter (Table 10-2 F); returns 0 when the result is not stored. */
static int alu(mc_dsp3210 *d, unsigned f, uint32_t a, uint32_t b, int shrt, alu_out *o) {
    const unsigned sh = b & 31u;
    switch (f) {
    case 0: *o = add(a, b, 0, shrt); return 1;
    case 1:
        *o = logic_result(shrt ? (a << sh) & 0xFFFFu : a << sh, 0);
        if (shrt) { o->r &= 0xFFFFu; flags_nz(o, o->r, 1); }
        return 1;
    case 2: *o = sub(b, a, shrt); return 1;
    case 3: {
        const unsigned bits = shrt ? 16u : 32u;
        const uint64_t s = (uint64_t)bitrev(a, bits) + bitrev(b, bits);
        const uint32_t r = bitrev((uint32_t)s, bits);
        *o = logic_result(r, shrt);
        o->c = (uint8_t)((s >> bits) & 1u);
        return 1;
    }
    case 4: *o = sub(a, b, shrt); return 1;
    case 6: *o = logic_result(a & ~b, shrt); return 1;
    case 7: *o = sub(a, b, shrt); return 0;
    case 8: *o = logic_result(a ^ b, shrt); return 1;
    case 9:
        if (shrt) {
            *o = logic_result(((uint32_t)d->c << 15) | ((a & 0xFFFFu) >> 1), 1);
        } else {
            *o = logic_result(((uint32_t)d->c << 31) | (a >> 1), 0);
        }
        o->c = a & 1u;
        return 1;
    case 10: *o = logic_result(a | b, shrt); return 1;
    case 11: *o = add(a, a, d->c, shrt); return 1;
    case 12: *o = logic_result(shrt ? (a & 0xFFFFu) >> sh : a >> sh, shrt); return 1;
    case 13: *o = logic_result(shrt ? (uint32_t)((int32_t)(int16_t)a >> sh) : (uint32_t)((int32_t)a >> sh), shrt); return 1;
    case 14: *o = logic_result(a & b, shrt); return 1;
    case 15: *o = logic_result(a & b, shrt); return 0;
    default:
        raise_error(d, VEC_ILLEGAL, 0);
        return 0;
    }
}

static void set_flags(mc_dsp3210 *d, const alu_out *o) { d->n = o->n; d->z = o->z; d->v = o->v; d->c = o->c; }

static void branch(mc_dsp3210 *d, uint32_t target) {
    d->delay = 1;
    d->delay_target = target;
    d->lock = 1;
}

/* The memory operand of a data move: *rP, post-modified by rI (0 none, 22
 * -size, 23 +size, else the register). */
static uint32_t move_address(mc_dsp3210 *d, unsigned rp, unsigned ri, unsigned size) {
    const uint32_t a = reg_get(d, rp, 0);
    uint32_t inc;
    if (ri == 0) inc = 0;
    else if (ri == 22) inc = (uint32_t)-(int32_t)size;
    else if (ri == 23) inc = size;
    else inc = reg_get(d, ri, 0);
    if (ri) reg_set(d, rp, a + inc);
    return a;
}

static unsigned w_size(unsigned w) { return w == 7 ? 4u : (w == 2 || w == 3) ? 2u : 1u; }
static uint32_t w_load(unsigned w, uint32_t v) {
    switch (w) {
    case 0: return v & 0xFFu;
    case 1: return sext8(v);
    case 2: return v & 0xFFFFu;
    case 3: return sext16(v);
    case 4: return (v & 0xFFu) << 8;
    default: return v;
    }
}
static uint32_t w_store(unsigned w, uint32_t v) { return w == 4 ? (v >> 8) & 0xFFu : v; }

static void load_flags(mc_dsp3210 *d, uint32_t v) {
    alu_out o = {0};
    flags_nz(&o, v, 0);
    set_flags(d, &o);
}

static void exec_move(mc_dsp3210 *d, uint32_t w, uint32_t cur) {
    const unsigned t = (w >> 24) & 1u, wf = (w >> 21) & 7u, h = (w >> 16) & 31u;
    const unsigned size = w_size(wf);
    (void)cur;
    if (!(w >> 31)) {                               /* 7a: *L */
        const uint32_t a = chip_base(d) | (w & 0xFFFFu);
        if (t) (void)mem_write(d, a, size, w_store(wf, reg_get(d, h, cur)));
        else {
            uint32_t v;
            if (!mem_read(d, a, size, &v)) { v = w_load(wf, v); reg_set(d, h, v); load_flags(d, v); }
            d->lock = 1;
        }
        return;
    }
    if ((w >> 25) & 1u) {                           /* 7d: an I/O register <-> memory */
        const uint32_t a = move_address(d, (w >> 11) & 31u, w & 31u, size);
        if (t) (void)mem_write(d, a, size, ior_get(d, h));
        else {
            uint32_t v;
            if (!mem_read(d, a, size, &v)) ior_set(d, h, size, v);
            d->lock = 1;
        }
        return;
    }
    if ((w >> 10) & 1u) {                           /* 7b: a CAU register <-> an I/O register */
        const unsigned io = w & 31u;
        if (t) ior_set(d, io, size, w_store(wf, reg_get(d, h, cur)));
        else {
            const uint32_t v = w_load(wf, ior_get(d, io));
            reg_set(d, h, v);
            load_flags(d, v);
            d->lock = 1;
        }
        return;
    }
    {                                               /* 7c: a CAU register <-> memory */
        const uint32_t a = move_address(d, (w >> 11) & 31u, w & 31u, size);
        if (t) (void)mem_write(d, a, size, w_store(wf, reg_get(d, h, cur)));
        else {
            uint32_t v;
            if (!mem_read(d, a, size, &v)) { v = w_load(wf, v); reg_set(d, h, v); load_flags(d, v); }
            d->lock = 1;
        }
    }
}

static void exec_alu_reg(mc_dsp3210 *d, uint32_t w, uint32_t cur) {
    const int shrt = !(w >> 31);
    const unsigned f = (w >> 21) & 15u, rd = (w >> 16) & 31u, rs1 = (w >> 11) & 31u, c = (w >> 5) & 63u, rs2 = w & 31u;
    if (!cond(d, c)) return;
    uint32_t a = reg_get(d, rs1, cur), b = reg_get(d, rs2, cur);
    if (rd == 25 && rs1 == 25 && (rs2 == 22 || rs2 == 23)) {    /* sp = sp ++/-- (by 4) */
        b = rs2 == 23 ? 4u : (uint32_t)-4;
        d->lock = 1;
    }
    alu_out o;
    const int store = alu(d, f, a, b, shrt, &o);
    if (d->fault) return;
    set_flags(d, &o);
    if (store) reg_set(d, rd, o.r);
}

static void exec_alu_imm(mc_dsp3210 *d, uint32_t w, uint32_t cur) {
    const int shrt = !(w >> 31);
    const unsigned f = (w >> 21) & 15u, rd = (w >> 16) & 31u;
    const uint32_t nimm = sext16(w);
    alu_out o;
    int store;
    if (f == 2) store = alu(d, 2, reg_get(d, rd, cur), nimm, shrt, &o);
    else store = alu(d, f, reg_get(d, rd, cur), nimm, shrt, &o);
    if (d->fault) return;
    set_flags(d, &o);
    if (store) reg_set(d, rd, o.r);
}

/* --- DA instructions ---------------------------------------------------------------- */

/* An X/Y/Z field's memory address: *rP post-modified (Table 10-3). */
static uint32_t da_address(mc_dsp3210 *d, unsigned field, unsigned size) {
    const unsigned p = field >> 3, i = field & 7u;
    const uint32_t a = d->r[p];
    uint32_t inc = 0;
    if (i >= 1 && i <= 5) inc = d->r[14 + i];
    else if (i == 6) inc = (uint32_t)-(int32_t)size;
    else if (i == 7) inc = size;
    d->r[p] = a + inc;
    return a;
}

/* An operand read as raw bits of `size` bytes (memory) or, for an
 * accumulator, its 40 bits in *acc (is_acc set). */
static int da_read(mc_dsp3210 *d, unsigned field, unsigned size, uint32_t *v, int *is_acc, unsigned *accn) {
    *is_acc = 0;
    d->ea_p = field >> 3;
    if (d->ea_p == 15u) d->ea_p = 0;
    if (d->ea_p) d->ea = d->r[d->ea_p];
    if ((field >> 3) == 0) {
        const unsigned i = field & 7u;
        if (i < 4) { *is_acc = 1; *accn = i; *v = 0; return 0; }
        if (i == 4) { *v = d->ibuf; return 0; }
        *v = 0;
        return 0;
    }
    return mem_read(d, da_address(d, field, size), size, v);
}

static void da_z(mc_dsp3210 *d, unsigned field, unsigned size, uint32_t v) {
    if ((field >> 3) == 0) {
        if ((field & 7u) == 5) d->obuf = v;
        return;
    }
    if ((field >> 3) == 15u) {
        /* p = 1111 (Table 10-3: "not allowed"): the Apple AV kernel's
         * in-place loops ("*rP = aN = *rP ..." with the no-write code 0x07
         * used everywhere else) write the result back to the Y operand's
         * address and post-modify the Y pointer by the Z field's i†. */
        if (!d->ea_p) return;
        const unsigned i = field & 7u;
        uint32_t inc = 0;
        if (i >= 1 && i <= 5) inc = d->r[14 + i];
        else if (i == 6) inc = (uint32_t)-(int32_t)size;
        else if (i == 7) inc = size;
        da_write(d, d->ea, size, v);
        d->r[d->ea_p] += inc;
        return;
    }
    da_write(d, da_address(d, field, size), size, v);
}

static void da_result(mc_dsp3210 *d, unsigned n, double x) {
    uint8_t f;
    const uint64_t r = acc_from(x, 31, &f);
    acc_set(d, n, r);
    dflags_set(d, f);
    if ((f & (DF_U | DF_V)) && (d->emr & EMR_UV)) raise_error(d, VEC_DAU, 1);
}

/* Formats 1-4 (Table 10-1). */
static void exec_mac(mc_dsp3210 *d, uint32_t w) {
    const unsigned fmt = (w >> 26) == 0x0Eu ? 4u : (w >> 29);
    const unsigned m = (w >> 26) & 7u, fsign = (w >> 24) & 1u, ssign = (w >> 23) & 1u, n = (w >> 21) & 3u;
    const unsigned xf = (w >> 14) & 127u, yf = (w >> 7) & 127u, zf = w & 127u;
    uint32_t xv, yv;
    int xa, ya;
    unsigned xn, yn;
    if (da_read(d, xf, 4, &xv, &xa, &xn)) return;
    if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
    double result;
    uint32_t tap = 0;
    if (fmt == 1 || fmt == 4) {
        /* aN = [-]Y {+,-} (aM or 1.0) * X; Y the adder input. */
        const double yval = ya ? acc_value(d->a[yn]) : mc_dsp3210_f32_value(yv);
        const uint32_t x32 = xa ? acc_f32(acc_mult(d, xn)) : xv;
        uint32_t mult;
        if (fmt == 4) mult = 0x00000080u;                       /* 1.0 */
        else if (m == 4) mult = 0;
        else if (m == 5) mult = 0x00000080u;
        else mult = acc_f32(acc_mult(d, m));
        const double p = product(mult, x32);
        result = (fsign ? -yval : yval) + (ssign ? -p : p);
        tap = ya ? acc_f32(d->a[yn]) : yv;
    } else {
        /* aN = [-](aM or constant) {+,-} Y * X. */
        double addend;
        if (m == 4) addend = 0.0;
        else if (m == 5) addend = 1.0;
        else addend = acc_value(d->a[m]);
        const uint32_t x32 = xa ? acc_f32(acc_mult(d, xn)) : xv;
        const uint32_t y32 = ya ? acc_f32(acc_mult(d, yn)) : yv;
        const double p = product(y32, x32);
        result = (fsign ? -addend : addend) + (ssign ? -p : p);
        tap = y32;
    }
    da_result(d, n, result);
    if ((zf >> 3) || (zf & 7u) == 5) {
        const int is_tap = fmt == 2 || fmt == 4;
        da_z(d, zf, 4, is_tap ? tap : acc_f32(d->a[n]));
    }
}

static double y_float(mc_dsp3210 *d, uint32_t yv, int ya, unsigned yn) {
    return ya ? acc_value(d->a[yn]) : mc_dsp3210_f32_value(yv);
}

/* Format 5, the special functions (4.2.2). */
static void exec_special(mc_dsp3210 *d, uint32_t w) {
    const unsigned g = (w >> 23) & 15u, n = (w >> 21) & 3u, yf = (w >> 7) & 127u, zf = w & 127u;
    const int zw = (zf >> 3) != 0 || (zf & 7u) == 5;
    uint32_t yv;
    int ya;
    unsigned yn;
    uint8_t f;
    switch (g) {
    case 0: {                                       /* ic */
        if (da_read(d, yf, 1, &yv, &ya, &yn)) return;
        const uint8_t b = ya ? (uint8_t)(d->a[yn] >> 32) : (uint8_t)yv;
        double x;
        if (d->dauc & 4u) x = b;
        else x = mulaw_value(b, d->dauc & 1u);
        const uint64_t r = acc_from(x, 31, &f);
        acc_set(d, n, r);
        dflags_set(d, (uint8_t)(f & (DF_N | DF_Z)));
        if (zw) da_z(d, zf, 4, acc_f32(r));
        return;
    }
    case 1: {                                       /* oc */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        const double x = y_float(d, yv, ya, yn);
        uint8_t b;
        if (d->dauc & 8u) b = (uint8_t)to_int(d, x, 0, 255);
        else b = mulaw_byte(x, (d->dauc & 2u) != 0);
        acc_set(d, n, (uint64_t)b << 32);
        if (zw) da_z(d, zf, 1, b);
        return;
    }
    case 2: case 8: {                               /* float16, float32 */
        const unsigned size = g == 2 ? 2u : 4u;
        if (da_read(d, yf, size, &yv, &ya, &yn)) return;
        int32_t i;
        if (ya) i = g == 2 ? (int32_t)(int16_t)(d->a[yn] >> 24) : (int32_t)(uint32_t)(d->a[yn] >> 8);
        else i = g == 2 ? (int32_t)(int16_t)yv : (int32_t)yv;
        const uint64_t r = acc_from((double)i, 31, &f);
        acc_set(d, n, r);
        dflags_set(d, (uint8_t)(f & (DF_N | DF_Z)));
        if (zw) da_z(d, zf, 4, acc_f32(r));
        return;
    }
    case 3: case 9: {                               /* int16, int32 */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        const double x = y_float(d, yv, ya, yn);
        if (g == 3) {
            const uint16_t v = (uint16_t)to_int(d, x, -32768, 32767);
            acc_set(d, n, (uint64_t)v << 24);
            if (zw) da_z(d, zf, 2, v);
        } else {
            const uint32_t v = (uint32_t)to_int(d, x, INT32_MIN, INT32_MAX);
            acc_set(d, n, (uint64_t)v << 8);
            if (zw) da_z(d, zf, 4, v);
        }
        return;
    }
    case 4: {                                       /* round */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        uint64_t r;
        if (ya) {
            const uint64_t s = d->a[yn];
            if (!(s & 0xFFu)) { r = 0; f = DF_Z; }
            else r = acc_from(acc_value(s) + ldexp(1.0, (int)(s & 0xFFu) - 128 - 24), 23, &f);
        } else {
            r = f32_acc(yv);
            f = (uint8_t)((yv & 0xFFu) ? ((yv >> 31) ? DF_N : 0) : DF_Z);
        }
        r &= ~(uint64_t)0xFF00u;
        acc_set(d, n, r);
        dflags_set(d, f);
        if ((f & (DF_U | DF_V)) && (d->emr & EMR_UV)) raise_error(d, VEC_DAU, 1);
        if (zw) da_z(d, zf, 4, acc_f32(r));
        return;
    }
    case 5: case 6: case 7: {                       /* ifalt, ifaeq, ifagt */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        const uint8_t fl = d->dflags;
        int take;
        if (g == 5) take = (fl & DF_N) != 0;
        else if (g == 6) take = (fl & DF_Z) != 0;
        else take = !(fl & (DF_N | DF_Z));
        if (take) acc_set(d, n, ya ? d->a[yn] : f32_acc(yv));
        if ((zf >> 3) != 0) {
            if (take || !(d->dauc & 0x40u)) da_z(d, zf, 4, acc_f32(d->a[n]));
            else if ((zf >> 3) != 15u) (void)da_address(d, zf, 4);
        }
        return;
    }
    case 12: {                                      /* ieee */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        const uint32_t u = to_ieee(ya ? acc_f32(d->a[yn]) : yv);
        acc_set(d, n, (uint64_t)u << 8);
        if (zw) da_z(d, zf, 4, u);
        return;
    }
    case 13: {                                      /* dsp */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        int nan;
        const uint64_t r = from_ieee(ya ? acc_f32(d->a[yn]) : yv, &f, &nan);
        acc_set(d, n, r);
        dflags_set(d, f);
        if (nan && (d->emr & EMR_NAN)) raise_error(d, VEC_NAN, 1);
        if (zw) da_z(d, zf, 4, acc_f32(r));
        return;
    }
    case 14: {                                      /* seed */
        if (da_read(d, yf, 4, &yv, &ya, &yn)) return;
        const uint32_t s = (ya ? acc_f32(d->a[yn]) : yv) ^ 0x7FFFFFFFu;
        const uint64_t r = f32_acc(s);
        f = (uint8_t)((s & 0xFFu) ? ((s >> 31) ? DF_N : 0) : DF_Z);
        acc_set(d, n, r);
        dflags_set(d, f);
        if (zw) da_z(d, zf, 4, s);
        return;
    }
    default:
        raise_error(d, VEC_ILLEGAL, 0);
        return;
    }
}

/* --- the instruction ----------------------------------------------------------------- */

static void exec(mc_dsp3210 *d, uint32_t w, uint32_t cur) {
    const unsigned top3 = w >> 29, top6 = w >> 26;
    switch (top3) {
    case 5: {                                       /* 8a goto rB + M */
        const uint32_t m = (((w >> 21) & 0xFFu) << 16) | (w & 0xFFFFu);
        branch(d, reg_get(d, (w >> 16) & 31u, cur) + m);
        return;
    }
    case 6:                                         /* 8b rD = M */
        reg_set(d, (w >> 16) & 31u, (((w >> 21) & 0xFFu) << 16) | (w & 0xFFFFu));
        return;
    case 7: {                                       /* 8c call M (rM) */
        const uint32_t m = (((w >> 21) & 0xFFu) << 16) | (w & 0xFFFFu);
        reg_set(d, (w >> 16) & 31u, cur + 8u);
        branch(d, m);
        return;
    }
    default:
        break;
    }
    if (top3 >= 1 && top3 <= 3) {
        if (top6 == 0x0Fu || top6 == 0x16u || top6 == 0x17u) { raise_error(d, VEC_ILLEGAL, 0); return; }
        if (top6 == 0x1Eu || top6 == 0x1Fu) exec_special(d, w);
        else exec_mac(d, w);
        return;
    }
    switch (top6) {
    case 0x20: case 0x21: {                         /* 0b/1b if (C) goto rB + N; ireturn = goto pcsh */
        const unsigned c = (w >> 21) & 63u, rb = (w >> 16) & 31u;
        if (rb == 30 && c == 1 && (w & 0xFFFFu) == 0) {
            ireturn(d);
            return;
        }
        d->lock = 1;
        if (cond(d, c)) branch(d, reg_get(d, rb, cur) + sext16(w));
        return;
    }
    case 0x03: {                                    /* 3a if (rM-- >= 0) goto rB + N */
        const unsigned rm = (w >> 21) & 31u;
        const uint32_t v = reg_get(d, rm, cur);
        reg_set(d, rm, v - 1u);
        d->lock = 1;
        if ((int32_t)v >= 0) branch(d, reg_get(d, (w >> 16) & 31u, cur) + sext16(w));
        return;
    }
    case 0x23: {                                    /* 3b/3c do K, {L, rM} */
        const unsigned k = (w >> 11) & 127u;
        const unsigned l = ((w >> 25) & 1u) ? (reg_get(d, w & 31u, cur) & 0x7FFu) : (w & 0x7FFu);
        d->do_active = 1;
        d->do_start = cur + 4u;
        d->do_len = d->do_left = (int)k + 1;
        d->do_count = (int)l;
        return;
    }
    case 0x04: {                                    /* 4a call rB + N (rM) */
        const uint32_t target = reg_get(d, (w >> 16) & 31u, cur) + sext16(w);
        reg_set(d, (w >> 21) & 31u, cur + 8u);
        branch(d, target);
        return;
    }
    case 0x24: {                                    /* 4b rD = rS | N << 16 */
        alu_out o = logic_result(reg_get(d, (w >> 16) & 31u, cur) | (w & 0xFFFFu) << 16, 0);
        set_flags(d, &o);
        reg_set(d, (w >> 21) & 31u, o.r);
        return;
    }
    case 0x05: case 0x25: {                         /* 5a/5b rD = rS3 + N */
        const alu_out o = add(reg_get(d, (w >> 16) & 31u, cur), sext16(w), 0, top6 == 0x05);
        set_flags(d, &o);
        reg_set(d, (w >> 21) & 31u, o.r);
        return;
    }
    case 0x06: case 0x26:                           /* 6a-6d */
        if ((w >> 25) & 1u) exec_alu_imm(d, w, cur);
        else exec_alu_reg(d, w, cur);
        return;
    case 0x07:                                      /* 7a */
        if ((w >> 25) & 1u) { raise_error(d, VEC_ILLEGAL, 0); return; }
        exec_move(d, w, cur);
        return;
    case 0x27:                                      /* 7b-7d */
        exec_move(d, w, cur);
        return;
    default:
        raise_error(d, VEC_ILLEGAL, 0);
        return;
    }
}

/* --- run ------------------------------------------------------------------------------ */

static void timer_tick(mc_dsp3210 *d, unsigned cycles) {
    if (!(d->tcon & 1u)) return;
    const unsigned src = (d->tcon >> 5) & 7u;
    unsigned counts = src == 0 ? 2u * cycles : src == 1 ? cycles : 0u;
    while (counts--) {
        if (d->timer == 0) {
            if (d->tcon & 2u) d->timer = d->timer_init;
            else return;
        } else if (--d->timer == 0) {
            d->irq_latched |= EMR_TIMER;
            if (d->tcon & 2u) {
                /* reload on the next count: the period is N + 1 */
            }
        }
    }
}

unsigned mc_dsp3210_run(mc_dsp3210 *d, unsigned n) {
    unsigned cycles = 0;
    while (cycles < n) {
        if (d->held || d->stopped) break;
        if (d->npend) commit(d);
        if (d->wait) {
            if (!requests(d)) break;
            d->wait = 0;
            d->lock = 1;                            /* the instruction after waiti runs first */
        } else if (!d->lock && d->level == 0 && requests(d)) {
            interrupt_entry(d);
            d->steps += 2;
            cycles += 2;
            timer_tick(d, 2);
            continue;
        }
        d->lock = 0;
        const uint32_t cur = d->pc;
        d->trace[d->trace_pos++ & 31u] = cur;
        uint32_t w;
        if (d->delay) { d->pc = d->delay_target; d->delay = 0; }
        else d->pc = cur + 4u;
        d->fault = 0;
        if (mem_read(d, cur, 4, &w)) {
            /* fetch bus error */
        } else {
            exec(d, w, cur);
        }
        if (!d->fault && d->do_active) {
            if (cur == d->do_start - 4u && d->do_left == d->do_len) {
                /* the do instruction itself: the loop starts next */
            } else if (--d->do_left == 0) {
                if (d->do_count > 0) {
                    d->do_count--;
                    d->do_left = d->do_len;
                    if (!d->delay) d->pc = d->do_start;
                } else {
                    d->do_active = 0;
                }
            }
        }
        d->steps++;
        cycles++;
        timer_tick(d, 1);
        if (d->fault) {
            const int vec = d->fault - 1;
            d->fault = 0;
            if (!d->stopped) {
                error_entry(d, vec);
                d->steps += 3;
                cycles += 3;
            }
        }
    }
    return cycles;
}

int mc_dsp3210_idle(const mc_dsp3210 *d) { return d->held || d->stopped || (d->wait && !requests(d)); }

void mc_dsp3210_set_irq(mc_dsp3210 *d, int line, int asserted) {
    const uint8_t bit = (uint8_t)(1u << (line & 1));
    if (asserted) d->irn |= bit;
    else d->irn = (uint8_t)(d->irn & ~bit);
}

void mc_dsp3210_set_bio(mc_dsp3210 *d, uint8_t levels) { d->bio_in = levels; }

static void reset_state(mc_dsp3210 *d) {
    const uint32_t prev = d->pc;
    d->r[22] = 0;
    d->r[20] = prev;
    d->pc = 0;
    d->delay = 0;
    d->lock = 0;
    d->level = 0;
    d->servicing = -1;
    d->dauc = 0;
    d->ctr = 0;
    d->pcw = (uint16_t)(0x38Fu | ((d->bio_in >> 7) & 1u) << 10 | ((d->bio_in >> 6) & 1u) << 11 |
                        ((d->bio_in >> 5) & 1u) << 12 | ((d->bio_in >> 4) & 1u) << 13);
    d->emr = 0;
    d->tcon = 0;
    d->timer = d->timer_init = 0xFFFFFFFFu;
    d->ioc = 0;
    d->dmac = 0;
    d->bioc = 0;
    d->bio_out = 0;
    d->irq_latched = 0;
    d->do_active = 0;
    d->npend = 0;
    d->wait = 0;
    d->stopped = 0;
    d->fault = 0;
    bio_update(d);
}

void mc_dsp3210_reset(mc_dsp3210 *d, int held) {
    if (held) {
        d->held = 1;
        d->wait = 0;
        d->npend = 0;
        return;
    }
    if (d->held || d->stopped) {
        d->held = 0;
        reset_state(d);
        d->steps += 2;                              /* call evtp + 0 (r20); nop */
    }
}

void mc_dsp3210_init(mc_dsp3210 *d, const mc_dsp3210_bus *bus, uint8_t *const *read_map, uint8_t *const *write_map) {
    memset(d, 0, sizeof *d);
    if (bus) d->bus = *bus;
    d->read_map = read_map;
    d->write_map = write_map;
    d->held = 1;
    reset_state(d);
}
