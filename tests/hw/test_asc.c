/* ASC and EASC (src/hw/asc.h): registers, FIFO timing in guest time, the
 * FIFO interrupt, output routing and volume, wave tables, the EASC's
 * decoder and the host pull. Expected values follow asc.h's register
 * description: the Guide to the Macintosh Family Hardware, 2nd ed., ch. 13
 * for the ASC, and for the EASC what the Quadra 700 ROM programs, tests and
 * expects (its register test table, start beep, chime and interrupt test). */
#include "hw_test.h"
#include "asc.h"

#include <stdlib.h>

#define TICK 704u   /* ASC: C15M periods per sample */

static hw_fake host;
static int tests;

/* EASC: the guest time of output sample n at 44.1 kHz. */
static uint64_t etime(uint64_t n) { return (n * MC_C15M_HZ + 44099u) / 44100u; }

static uint8_t rd(mc_asc *a, uint32_t off) { return (uint8_t)mc_asc_ops.read(a, off, 1); }
static void wr(mc_asc *a, uint32_t off, uint8_t v) { mc_asc_ops.write(a, off, 1, v); }
static void wr16(mc_asc *a, uint32_t off, uint16_t v) { mc_asc_ops.write(a, off, 2, v); }
static void fill(mc_asc *a, int fifo, unsigned n, uint8_t v) {
    for (unsigned i = 0; i < n; i++) wr(a, fifo ? 0x400 + (i & 0x3FF) : (i & 0x3FF), v);
}
static mc_asc *fresh(mc_asc_type type) {
    static mc_asc *a;
    free(a);
    a = calloc(1, sizeof *a);
    hw_fake_init(&host);
    mc_asc_init(a, &host.host, type);
    return a;
}
static size_t drain(mc_asc *a) {
    static int16_t buf[2 * MC_ASC_RING];
    return mc_asc_pull(a, buf, MC_ASC_RING);
}
/* An EASC set up as the ROM leaves it ($40804318-$40804382): both FIFO
 * interrupts masked, FIFOs cleared, FIFO mode, 22,254.5 Hz, A left and B
 * right at full volume. */
static mc_asc *easc_rom(void) {
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    wr(a, 0x806, 7 << 5);
    wr16(a, 0xF06, 0x7F00);
    wr16(a, 0xF26, 0x007F);
    wr(a, 0xF09, 1);
    wr(a, 0xF29, 1);
    wr(a, 0x803, 0x80);
    wr(a, 0x803, 0x00);
    wr(a, 0x80A, 2);
    wr(a, 0x801, 1);
    wr16(a, 0xF04, 0x812F);
    wr16(a, 0xF24, 0x812F);
    wr(a, 0xF08, 0x80);
    wr(a, 0xF28, 0x80);
    rd(a, 0x804);                     /* takes the clear's events */
    return a;
}

static void test_reset(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    CHECK_EQ(rd(a, 0x800), 0x00);
    CHECK_EQ(rd(a, 0x801), 0);
    CHECK_EQ(rd(a, 0x804), 0);
    CHECK_EQ(rd(a, 0x807), 0);
    CHECK_EQ(mc_asc_rate(a), 22255);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    CHECK(hw_fake_deadline(&host, a) == MC_NEVER);
    a = fresh(MC_ASC_ENHANCED);
    CHECK_EQ(rd(a, 0x800), 0xB0);
    CHECK_EQ(mc_asc_rate(a), 44100);
    CHECK_EQ(rd(a, 0x804), 0x0F);     /* both FIFOs empty (and below half) */
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
}

/* The ROM's register test (table at ROM offset $48068): each register reads
 * back what was written under its mask. */
static void test_register_readback(void) {
    tests++;
    static const struct { uint16_t off; uint8_t mask; uint8_t n; uint8_t v[9]; } table[] = {
        {0x801, 0x03, 3, {0x00, 0x01, 0x00}},
        {0x806, 0xE0, 8, {0x00, 0x20, 0x40, 0x60, 0x80, 0xA0, 0xC0, 0xE0}},
        {0x80A, 0x01, 3, {0x00, 0x01, 0x00}},
        {0xF08, 0xFF, 9, {0x00, 0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0x83, 0x00}},
        {0xF28, 0xFF, 9, {0x00, 0x01, 0x02, 0x03, 0x80, 0x81, 0x82, 0x83, 0x00}},
    };
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    for (size_t t = 0; t < sizeof table / sizeof *table; t++)
        for (unsigned k = 0; k < table[t].n; k++) {
            wr(a, table[t].off, table[t].v[k]);
            CHECK_EQ(rd(a, table[t].off) & table[t].mask, table[t].v[k]);
        }
    /* Mode 2 makes the 2 KiB plain RAM (the ROM's RAM test, $40847EE4). */
    wr(a, 0x801, 2);
    for (unsigned i = 0; i < 2048; i++) wr(a, i, (uint8_t)(i ^ 0x5A));
    int ok = 1;
    for (unsigned i = 0; i < 2048; i++) ok &= rd(a, i) == (uint8_t)(i ^ 0x5A);
    CHECK(ok);
}

/* ASC: one byte per C15M/704 tick. EASC: the increment in 1/$10000 bytes
 * per 44.1 kHz sample. */
static void test_drain_rate(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    fill(a, 0, 1024, 0x90);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024);
    wr(a, 0x801, 1);
    hw_fake_advance(&host, 100 * TICK - 1);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024 - 99);
    hw_fake_advance(&host, 100 * TICK);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024 - 100);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 0);   /* mono: FIFO B idle */
    /* ASC at 44.1 kHz: floor(t * 44100 / C15M) ticks. */
    a = fresh(MC_ASC_ORIGINAL);
    fill(a, 0, 1024, 0x90);
    wr(a, 0x807, 3);
    CHECK_EQ(mc_asc_rate(a), 44100);
    wr(a, 0x801, 1);
    hw_fake_advance(&host, MC_C15M_HZ / 100);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024 - 441);
    /* EASC: $8000 is half a byte per sample, both FIFOs at once. */
    a = easc_rom();
    wr16(a, 0xF04, 0x8000);
    wr16(a, 0xF24, 0x4000);
    fill(a, 0, 1000, 0x90);
    fill(a, 1, 1000, 0x90);
    hw_fake_advance(&host, etime(200));
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1000 - 100);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 1000 - 50);
    /* $812F, the ROM's value for 22,254.5 Hz: about 22,254 bytes a second. */
    a = easc_rom();
    fill(a, 0, 1024, 0x90);
    hw_fake_advance(&host, MC_C15M_HZ / 50);   /* 20 ms */
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024 - 445);
}

static void test_status_irq(void) {
    tests++;
    /* ASC: latched flags, cleared by the read. */
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    fill(a, 0, 1024, 0x80);
    /* Filled up (System 7.5.3's Sound Manager on the IIci, asc.h). */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804), 0x02);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    wr(a, 0x801, 1);
    CHECK_EQ(hw_fake_deadline(&host, a), 513ull * TICK);
    hw_fake_advance(&host, 513ull * TICK - 1);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    hw_fake_advance(&host, 513ull * TICK);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804), 0x01);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    CHECK_EQ(rd(a, 0x804), 0x00);
    hw_fake_advance(&host, 1024ull * TICK);
    CHECK_EQ(rd(a, 0x804), 0x02);

    /* EASC status: the FIFOs' state, read without clearing; full and empty
     * share a bit (the start beep fills FIFO A until bit 1 sets). */
    a = easc_rom();
    wr16(a, 0xF04, 0);                /* hold FIFO A still */
    wr16(a, 0xF24, 0);
    CHECK_EQ(rd(a, 0x804), 0x0F);
    CHECK_EQ(rd(a, 0x804), 0x0F);
    fill(a, 0, 1, 0x80);
    CHECK_EQ(rd(a, 0x804), 0x0D);     /* A: below half, neither full nor empty */
    fill(a, 0, 600, 0x80);
    CHECK_EQ(rd(a, 0x804), 0x0C);
    fill(a, 0, 423, 0x80);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024);
    CHECK_EQ(rd(a, 0x804), 0x0E);     /* A full */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);   /* masked */

    /* EASC interrupt (the ROM's interrupt test): FIFO B unmasked, A masked;
     * an interrupt when B fills up, another when it falls below half. */
    a = easc_rom();
    wr16(a, 0xF24, 0x3FDB);           /* the test's 11 kHz */
    wr(a, 0xF29, 0);
    fill(a, 1, 1024, 0x80);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804) & 0x0C, 0x08);   /* B full */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    const uint64_t half = hw_fake_deadline(&host, a);
    CHECK(half != MC_NEVER);
    hw_fake_advance(&host, half);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804) & 0x0C, 0x04);   /* B below half */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    /* A masked FIFO's events do not interrupt; an idle unmasked one with
     * its events taken stays quiet. */
    wr(a, 0xF29, 1);
    hw_fake_advance(&host, half + MC_C15M_HZ);   /* B runs empty, masked */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    rd(a, 0x804);
    wr(a, 0xF29, 0);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    /* A clear is an event: unmasking FIFO A after a clear interrupts at
     * once (the record test, $40849526). */
    wr(a, 0xF29, 1);
    wr(a, 0x803, 0x80);
    wr(a, 0x803, 0x00);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    wr(a, 0xF09, 0);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
}

static void test_clear(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    fill(a, 0, 100, 0x80);
    fill(a, 1, 50, 0x80);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 50);
    wr(a, 0x803, 0x80);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 0);
    fill(a, 0, 1025, 0x80);           /* the byte beyond full is lost */
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024);
    /* A 32-bit write appends four bytes. */
    wr(a, 0x803, 0x80);
    mc_asc_ops.write(a, 0x10, 4, 0x11223344);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 4);
}

static void test_routing_volume(void) {
    tests++;
    int16_t buf[64];
    /* ASC stereo and mono, master volume. */
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x806, 7 << 5);
    wr(a, 0x802, 2);
    fill(a, 0, 100, 0xC0);
    fill(a, 1, 100, 0x40);
    wr(a, 0x801, 1);
    hw_fake_advance(&host, 10 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 10);
    CHECK_EQ(buf[0], 16384);
    CHECK_EQ(buf[1], -16384);
    wr(a, 0x802, 0);
    hw_fake_step(&host, 2 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 2);
    CHECK_EQ(buf[2], 16384);
    CHECK_EQ(buf[3], 16384);
    wr(a, 0x806, 3 << 5);
    hw_fake_step(&host, TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 1);
    CHECK_EQ(buf[0], 16384 * 3 / 7);
    /* The ASC switched off is silent. */
    a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x806, 7 << 5);
    fill(a, 0, 10, 0xC0);
    hw_fake_step(&host, 2 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 2);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 10);

    /* EASC: FIFO A left and FIFO B right as the ROM sets them, one byte a
     * sample. */
    a = easc_rom();
    wr16(a, 0xF04, 0x10000 - 1);
    wr16(a, 0xF24, 0x10000 - 1);
    fill(a, 0, 100, 0xC0);
    fill(a, 1, 100, 0x40);
    hw_fake_advance(&host, etime(10));
    CHECK_EQ(mc_asc_pull(a, buf, 32), 10);
    CHECK_EQ(buf[2], 16384);
    CHECK_EQ(buf[3], -16384);
    /* Both FIFOs to the left side: the sum. */
    wr16(a, 0xF26, 0x7F00);
    hw_fake_advance(&host, etime(11));
    CHECK_EQ(mc_asc_pull(a, buf, 32), 1);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[1], 0);
    /* Half volume on A; the master volume. */
    wr16(a, 0xF26, 0x007F);
    wr(a, 0xF06, 0x40);
    wr(a, 0x806, 3 << 5);
    hw_fake_advance(&host, etime(12));
    CHECK_EQ(mc_asc_pull(a, buf, 32), 1);
    CHECK_EQ(buf[0], (16384 * 0x40 / 0x7F) * 3 / 7);
    CHECK_EQ(buf[1], -16384 * 3 / 7);
    /* Mode 0: silence. */
    wr(a, 0x801, 0);
    hw_fake_advance(&host, etime(14));
    CHECK_EQ(mc_asc_pull(a, buf, 32), 2);
    CHECK_EQ(buf[2], 0);
}

static void test_wavetable(void) {
    tests++;
    int16_t buf[64];
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x801, 2);
    wr(a, 0x806, 7 << 5);
    for (unsigned i = 0; i < 512; i++) wr(a, i, (uint8_t)i);     /* voice 0: a ramp */
    CHECK_EQ(rd(a, 0x005), 5);        /* random access in wavetable mode */
    wr(a, 0x814, 0x00); wr(a, 0x815, 0x01); wr(a, 0x816, 0x00); wr(a, 0x817, 0x00);  /* 2.0 */
    CHECK_EQ(mc_asc_ops.read(a, 0x814, 4), 0x10000);
    hw_fake_step(&host, 4 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 4);
    for (int k = 0; k < 4; k++) CHECK_EQ(buf[2 * k], (2 * k - 128) << 6);
    CHECK_EQ(mc_asc_ops.read(a, 0x810, 4), 4 * 0x10000);
    /* The phase wraps within the 512-byte table. */
    mc_asc_ops.write(a, 0x810, 4, 511u << 15);
    wr(a, 0x816, 0x80);               /* increment 0x18000 = 3.0 */
    hw_fake_step(&host, 2 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 2);
    CHECK_EQ(buf[0], (255 - 128) << 6);           /* ram[511] = 0xFF */
    CHECK_EQ(buf[2], (2 - 128) * 64);             /* (511 + 3) mod 512 = 2 */
    /* Stereo: voices 0+1 left, 2+3 right. */
    a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x801, 2);
    wr(a, 0x802, 2);
    wr(a, 0x806, 7 << 5);
    wr(a, 0x400, 0xA0);               /* voice 2, entry 0 */
    hw_fake_step(&host, TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 32), 1);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[1], 0x20 << 7);
}

/* The EASC decoder (FIFO control bits 0-1 = 2): 15-byte groups, a header
 * (filter, shift) and 28 4-bit samples, low nibble first. */
static void test_decode(void) {
    tests++;
    int16_t buf[2 * 64];
    mc_asc *a = easc_rom();
    wr16(a, 0xF04, 0xFFFF);           /* one decoded sample per output sample (almost) */
    wr16(a, 0xF10, 0x0000); wr16(a, 0xF12, 0x003C); wr16(a, 0xF14, 0xCC73); wr16(a, 0xF16, 0xC962);
    wr(a, 0xF08, 0x82);
    /* Group 1: filter 0, shift 12: the nibbles as they are. */
    wr(a, 0, 0x0C);
    wr(a, 0, 0x21);                   /* samples 1, 2 */
    for (int i = 0; i < 13; i++) wr(a, 0, 0x00);
    /* Group 2: filter 1 (K0 60/64), shift 12, all zero nibbles: the last
     * sample decays by 60/64 each step. */
    wr(a, 0, 0x1C);
    for (int i = 0; i < 14; i++) wr(a, 0, 0x00);
    /* The accumulator reaches a whole sample on the second tick: decoded
     * sample k plays at output k + 1. */
    hw_fake_advance(&host, etime(31));
    CHECK_EQ(mc_asc_pull(a, buf, 64), 31);
    CHECK_EQ(buf[0], 0);
    CHECK_EQ(buf[2], 1);
    CHECK_EQ(buf[4], 2);
    CHECK_EQ(buf[6], 0);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 13);    /* 30 samples: 15 bytes, then a header and one byte */
    /* Shift 0 puts a nibble at the top: -8 is -32768. */
    a = easc_rom();
    wr16(a, 0xF04, 0xFFFF);
    wr(a, 0xF08, 0x82);
    wr(a, 0, 0x00);
    wr(a, 0, 0x78);
    hw_fake_advance(&host, etime(3));
    CHECK_EQ(mc_asc_pull(a, buf, 64), 3);
    CHECK_EQ(buf[2], -32768);
    CHECK_EQ(buf[4], 7 * 4096);
}

/* The chime's start: a decoder group begun with only its header in the
 * FIFO waits for its first data byte (the header is not lost), and two
 * FIFOs whose rates were written a sample apart play in step once their
 * stream format is set. The Quadra 700 ROM's chime hit both. */
static void test_decode_start(void) {
    tests++;
    int16_t buf[2 * 64];
    mc_asc *a = easc_rom();
    wr16(a, 0xF04, 0xFFFF);
    wr(a, 0xF08, 0x82);
    wr(a, 0, 0x0C);                   /* the header alone: filter 0, shift 12 */
    hw_fake_advance(&host, etime(4));
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1);
    wr(a, 0, 0x21);                   /* samples 1, 2 */
    for (int i = 0; i < 13; i++) wr(a, 0, 0x00);
    hw_fake_advance(&host, etime(12));
    CHECK_EQ(mc_asc_pull(a, buf, 64), 12);
    int at = -1;
    for (int i = 0; i < 12; i++)
        if (buf[2 * i] == 1) { at = i; break; }
    CHECK(at > 0 && at < 11);
    if (at > 0 && at < 11) CHECK_EQ(buf[2 * at + 2], 2);
    /* FIFO A's rate written a sample before FIFO B's: A left, B right, the
     * same bytes to both, as the ROM sends its chime. */
    a = fresh(MC_ASC_ENHANCED);
    wr(a, 0x806, 7 << 5);
    wr16(a, 0xF06, 0x7F00);
    wr16(a, 0xF26, 0x007F);
    wr(a, 0x801, 1);
    wr16(a, 0xF04, 0x812F);
    hw_fake_advance(&host, etime(1));
    wr16(a, 0xF24, 0x812F);
    wr16(a, 0xF04, 0x7FFF);
    wr16(a, 0xF24, 0x7FFF);
    wr(a, 0xF08, 0x82);
    wr(a, 0xF28, 0x82);
    for (int g = 0; g < 2; g++)
        for (int i = 0; i < 15; i++) {
            const uint8_t v = i == 0 ? 0x1A : (uint8_t)(0x31 + 17 * i);
            wr(a, 0x400, v);
            wr(a, 0, v);
        }
    hw_fake_advance(&host, etime(60));
    const size_t n = mc_asc_pull(a, buf, 64);
    CHECK_EQ(n, 60);
    int same = 1, sound = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[2 * i] != buf[2 * i + 1]) same = 0;
        if (buf[2 * i]) sound = 1;
    }
    CHECK(same);
    CHECK(sound);
}

/* The EASC's filter pairs start at the Quadra 700 ROM's values (the
 * PowerBook 520's ROM never writes them); entering FIFO mode empties the
 * FIFOs on the EASC (the 520's and 190's zero word before their chimes),
 * not on the original ASC. */
static void test_easc_start_state(void) {
    tests++;
    static const uint8_t filters[8] = {0x00, 0x00, 0x00, 0x3C, 0xCC, 0x73, 0xC9, 0x62};
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    for (int i = 0; i < 8; i++) {
        CHECK_EQ(rd(a, 0xF10 + i), filters[i]);
        CHECK_EQ(rd(a, 0xF30 + i), filters[i]);
    }
    wr16(a, 0, 0x0000);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 2);
    wr(a, 0x801, 1);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
    a = fresh(MC_ASC_ORIGINAL);
    wr16(a, 0, 0x0000);
    wr(a, 0x801, 1);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 2);
}

/* The PowerBook 520's and 190's sound block: $F40-$F4F read back, the
 * 16-bit ports at +$1000 (FIFO A) and +$1800 (FIFO B) beside the EASC's
 * registers, the rate from $F40 bits 0-1 (22,050 Hz: one sample every two
 * output samples), 1024 samples per FIFO, the last sample held on an
 * underrun, each FIFO 16-bit after its own port was written. */
static void test_wide(void) {
    tests++;
    int16_t buf[2 * 16];
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    a->block = 1;
    wr(a, 0x806, 7 << 5);
    wr16(a, 0xF06, 0x7F00);
    wr16(a, 0xF26, 0x007F);
    wr(a, 0x801, 1);
    wr(a, 0xF40, 0x21);
    mc_asc_ops.write(a, 0xF44, 4, 0x00FF0000u);
    wr16(a, 0xF4A, 0x0FFC);
    CHECK_EQ(rd(a, 0xF40), 0x21);
    CHECK_EQ(mc_asc_ops.read(a, 0xF44, 4), 0x00FF0000u);
    CHECK_EQ(mc_asc_ops.read(a, 0xF4A, 2), 0x0FFC);
    wr(a, 0xF40, rd(a, 0xF40) | 0x20);     /* the output component's ori.b */
    CHECK_EQ(rd(a, 0xF40), 0x21);
    for (int i = 0; i < 4; i++) {
        wr16(a, 0x1000, (uint16_t)(0x1234 + i));
        wr16(a, 0x1800, (uint16_t)(0x8765 - i));
    }
    CHECK_EQ(rd(a, 0x801), 1);              /* +$1800 is not the mode register */
    CHECK_EQ(rd(a, 0x800), 0xB0);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 8);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 8);
    hw_fake_advance(&host, etime(8));
    CHECK_EQ(mc_asc_pull(a, buf, 16), 8);
    CHECK_EQ(buf[2], 0x1234);
    CHECK_EQ(buf[3], 0x8765 - 0x10000);
    CHECK_EQ(buf[6], 0x1235);
    CHECK_EQ(buf[7], 0x8764 - 0x10000);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
    hw_fake_advance(&host, etime(16));
    CHECK_EQ(mc_asc_pull(a, buf, 16), 8);
    CHECK_EQ(buf[14], 0x1237);              /* underrun: the last sample holds */
    /* $F40 bits 0-1 = 0: 44,100 Hz, a sample each output sample. */
    wr(a, 0xF40, 0x20);
    wr16(a, 0x1000, 0x1111);
    wr16(a, 0x1000, 0x2222);
    hw_fake_advance(&host, etime(18));
    CHECK_EQ(mc_asc_pull(a, buf, 16), 2);
    CHECK_EQ(buf[0], 0x1111);
    CHECK_EQ(buf[2], 0x2222);
    /* 1024 samples a FIFO: below half under 512 (status bit 2), full at
     * 1024 (bit 3), the rest lost. */
    wr(a, 0xF40, 0x23);                     /* rate code 3: none */
    for (int i = 0; i < 511; i++) wr16(a, 0x1800, 0);
    CHECK_EQ(rd(a, 0x804) & 0x0C, 0x04);
    wr16(a, 0x1800, 0);
    CHECK_EQ(rd(a, 0x804) & 0x0C, 0x00);
    for (int i = 0; i < 600; i++) wr16(a, 0x1800, 0);
    CHECK_EQ(mc_asc_fifo_level(a, 1), 2 * MC_ASC_WFIFO);
    CHECK_EQ(rd(a, 0x804) & 0x0C, 0x08);
    /* A byte to the EASC's port: the FIFOs play bytes again. */
    wr(a, 0x803, 0x80);
    wr(a, 0x803, 0);
    wr(a, 0x000, 0x40);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1);
    wr16(a, 0x1800, 0x5555);                /* FIFO B 16-bit, A bytes */
    CHECK_EQ(mc_asc_fifo_level(a, 1), 2);
    CHECK_EQ(rd(a, 0x804) & 0x05, 0x05);
}

static void test_pull(void) {
    tests++;
    int16_t buf[2 * 2000];
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x806, 7 << 5);
    hw_fake_advance(&host, 10 * TICK + 5);
    CHECK_EQ(mc_asc_pull(a, buf, 1000), 10);
    CHECK_EQ(mc_asc_pull(a, buf, 1000), 0);      /* underrun: nothing ahead of guest time */
    hw_fake_step(&host, 3 * TICK);
    CHECK_EQ(mc_asc_pull(a, buf, 2), 2);
    CHECK_EQ(mc_asc_pull(a, buf, 2), 1);
    /* A host that stops pulling loses the oldest frames. */
    hw_fake_step(&host, (MC_ASC_RING + 100ull) * TICK);
    CHECK_EQ(drain(a), MC_ASC_RING);
    CHECK_EQ(a->dropped, 100);
}

/* EASC record: FIFO A fills from the input at its rate; status bit 0 less
 * than half in, bit 1 full. */
static void test_record(void) {
    tests++;
    mc_asc *a = easc_rom();
    wr16(a, 0xF04, 0x8000);           /* a byte every two samples */
    wr(a, 0x80A, 1);
    hw_fake_advance(&host, etime(200));
    CHECK_EQ(mc_asc_fifo_level(a, 0), 100);
    CHECK_EQ(rd(a, 0x804) & 3, 1);
    hw_fake_advance(&host, etime(1024));
    CHECK_EQ(rd(a, 0x804) & 3, 0);    /* half full */
    hw_fake_advance(&host, etime(2048));
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1024);
    CHECK_EQ(rd(a, 0x804) & 3, 2);
    CHECK_EQ(rd(a, 0x123), 0x80);     /* silence, popped */
    CHECK_EQ(mc_asc_fifo_level(a, 0), 1023);
}

/* The input's frame, and the rates the source was asked for and given. */
static unsigned input_on, input_rate;
static void record_enable(void *ctx, unsigned rate) { (void)ctx; input_on = rate; }
static void input_sample(void *ctx, unsigned rate, int16_t frame[2]) {
    (void)ctx;
    input_rate = rate;
    frame[0] = 0x1000;
    frame[1] = 0x3000;
}

/* The LC 475's sound input under System 7.5.5: FIFO A unmasked with older
 * events latched, recording, a clear and VIA2's flag cleared, then the
 * half-full interrupt (a new edge) and the bytes, the input's two channels
 * mixed in offset binary. */
static void test_record_input(void) {
    tests++;
    mc_asc *a = easc_rom();
    const mc_audio_source source = {.sample = input_sample, .enable = record_enable};
    input_on = input_rate = 0;
    mc_asc_set_audio_source(a, &source);
    wr(a, 0xF29, 0);
    wr(a, 0x803, 0x80);                      /* a clear in playback: events latched */
    wr(a, 0x803, 0x00);
    wr(a, 0xF09, 0);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    wr(a, 0x80A, 1);
    CHECK_EQ(input_on, 22254);
    wr(a, 0x803, 0x80);
    wr(a, 0x803, 0x00);
    const unsigned edges = hw_fake_changes(&host, a, MC_ASC_LINE_IRQ);
    hw_fake_advance(&host, etime(2 * 512 + 8));   /* to a time */
    CHECK(hw_fake_changes(&host, a, MC_ASC_LINE_IRQ) >= edges + 2);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804) & 1, 0);           /* half in: bit 0 clear */
    CHECK_EQ(rd(a, 0x000), 0xA0);
    CHECK_EQ(input_rate, 22254);
    wr(a, 0x80A, 2);
    CHECK_EQ(input_on, 0);
    mc_asc_set_audio_source(a, NULL);
}

/* The LC family's ASC records into FIFO A with the mode left at 0 (the
 * Color Classic's driver), a byte per tick of its 22,254.5 Hz clock; the
 * original ASC does not. */
static void test_lc_input(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    wr(a, 0x80A, 1);
    hw_fake_advance(&host, 704u * 100u);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
    a = fresh(MC_ASC_ORIGINAL);
    a->input = 1;
    const mc_audio_source source = {.sample = input_sample, .enable = record_enable};
    mc_asc_set_audio_source(a, &source);
    wr(a, 0x80A, 1);
    CHECK_EQ(input_on, 22255);
    hw_fake_advance(&host, 704u * 512u + 1u);
    CHECK_EQ(rd(a, 0x804) & 1, 1);           /* half full, read-cleared */
    CHECK_EQ(rd(a, 0x000), 0xA0);
    mc_asc_set_audio_source(a, NULL);
}

static void sample(void *ctx, unsigned rate, int16_t frame[2]) {
    unsigned *index = ctx;
    frame[0] = (int16_t)(0x1234u + *index);
    frame[1] = (int16_t)(0xBCDFu + *index);
    (*index)++;
    CHECK_EQ(rate, 22050);
}

static void test_singer_input(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    a->block = a->singer = 1;
    unsigned index = 0;
    const mc_audio_source source = {.ctx = &index, .sample = sample};
    mc_asc_set_audio_source(a, &source);
    wr(a, 0xF40, 1);
    wr(a, 0xF29, 1);
    wr(a, 0x801, 1);
    wr(a, 0x80A, 1);
    wr(a, 0x803, 0x80);
    CHECK_EQ(rd(a, 0x804) & 3, 1); /* active-low half-full */
    hw_fake_advance(&host, etime(1023));
    CHECK_EQ(rd(a, 0x804) & 3, 1);
    CHECK_EQ(index, 511);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    hw_fake_advance(&host, etime(1024));
    CHECK_EQ(index, 512);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804) & 3, 0);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    CHECK_EQ(mc_asc_ops.read(a, 0x1000, 2), 0x1234);
    CHECK_EQ(mc_asc_ops.read(a, 0x1800, 2), 0xBCDF);
    CHECK_EQ(rd(a, 0x1000), 0x12);
    CHECK_EQ(rd(a, 0x1001), 0x35);
    CHECK_EQ(mc_asc_ops.read(a, 0x1800, 2), 0xBCE0);
    CHECK_EQ(rd(a, 0x804) & 3, 1);
    wr(a, 0x80A, 0);
    hw_fake_advance(&host, etime(4096));
    CHECK_EQ(index, 512);
    CHECK_EQ(mc_asc_ops.read(a, 0x1000, 2), 0);
}

typedef struct input_observation {
    unsigned samples, rate, notifications;
    unsigned notified[16];
} input_observation;

static void input_enable(void *ctx, unsigned rate) {
    input_observation *input = ctx;
    if (input->notifications < 16) input->notified[input->notifications] = rate;
    input->notifications++;
    input->rate = rate;
}
static void observed_sample(void *ctx, unsigned rate, int16_t frame[2]) {
    input_observation *input = ctx;
    CHECK(rate != 0);
    CHECK_EQ(rate, input->rate);
    frame[0] = (int16_t)(1000 + input->samples);
    frame[1] = (int16_t)(-1000 - (int)input->samples);
    input->samples++;
}

/* ASC rendering is lazy; advancing fake time alone need not reach an
 * IRQ deadline, so a pull must service the sample ticks before assertions. */
static void advance_input(mc_asc *a, uint64_t ticks) {
    hw_fake_advance(&host, etime(ticks));
    drain(a);
}

/* The original 1400 ROM starts input with $801=1 ($401A8846) and
 * $80A=1 ($401A88D2); the host follows the existing Singer sample gate. */
static void test_singer_input_enable(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    a->block = a->singer = 1;
    input_observation input = {0};
    const mc_audio_source source = {.ctx = &input, .sample = observed_sample, .enable = input_enable};
    mc_asc_set_audio_source(a, &source);
    wr(a, 0x80A, 1);
    advance_input(a, 2);
    CHECK_EQ(input.notifications, 0);
    CHECK_EQ(input.samples, 0);
    wr(a, 0x801, 1);
    advance_input(a, 3);
    CHECK_EQ(input.samples, 1);
    wr(a, 0xF40, 0);
    CHECK_EQ(input.notifications, 1);
    wr(a, 0xF40, 1);
    advance_input(a, 5);
    CHECK_EQ(input.samples, 2);
    wr(a, 0xF40, 2);
    advance_input(a, 9);
    CHECK_EQ(input.samples, 3);
    wr(a, 0xF40, 3);
    advance_input(a, 13);
    CHECK_EQ(input.samples, 3);
    wr(a, 0xF40, 0);
    wr(a, 0x801, 1);
    wr(a, 0x80A, 1);
    CHECK_EQ(input.notifications, 5);
    wr(a, 0x801, 2);
    advance_input(a, 14);
    CHECK_EQ(input.samples, 3);
    wr(a, 0x801, 1);
    wr(a, 0x80A, 0);
    wr(a, 0x80A, 1);
    mc_asc_ops.reset(a);
    CHECK_EQ(input.notifications, 10);
    CHECK_EQ(input.rate, 0);
    CHECK(a->source.ctx == &input);
    static const unsigned expected[] = {44100, 22050, 11025, 0, 44100, 0, 44100, 0, 44100, 0};
    for (unsigned i = 0; i < sizeof expected / sizeof *expected; i++) CHECK_EQ(input.notified[i], expected[i]);
    advance_input(a, 20);
    CHECK_EQ(input.samples, 3);
    mc_asc_set_audio_source(a, NULL);
    CHECK_EQ(input.notifications, 10);

    a = fresh(MC_ASC_ENHANCED);
    input = (input_observation){0};
    mc_asc_set_audio_source(a, &source);
    wr(a, 0x801, 1);
    wr(a, 0x80A, 1);
    advance_input(a, 10);
    CHECK_EQ(input.notifications, 0);
    CHECK_EQ(input.samples, 0);
    mc_asc_set_audio_source(a, NULL);
}

static void test_singer_input_replacement(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    a->block = a->singer = 1;
    input_observation first = {0}, second = {0};
    const mc_audio_source source1 = {.ctx = &first, .sample = observed_sample, .enable = input_enable};
    const mc_audio_source source2 = {.ctx = &second, .sample = observed_sample, .enable = input_enable};
    mc_asc_set_audio_source(a, &source1);
    wr(a, 0xF40, 1);
    wr(a, 0x801, 1);
    wr(a, 0x80A, 1);
    advance_input(a, 2);
    CHECK_EQ(first.samples, 1);
    mc_asc_set_audio_source(a, &source2);
    CHECK_EQ(first.notifications, 2);
    CHECK_EQ(first.notified[0], 22050);
    CHECK_EQ(first.notified[1], 0);
    CHECK_EQ(second.notifications, 1);
    CHECK_EQ(second.notified[0], 22050);
    advance_input(a, 4);
    CHECK_EQ(first.samples, 1);
    CHECK_EQ(second.samples, 1);
    mc_asc_set_audio_source(a, &source2);
    mc_asc_set_audio_source(a, &a->source);
    CHECK_EQ(second.notifications, 1);
    mc_asc_set_audio_source(a, NULL);
    CHECK_EQ(second.notifications, 2);
    CHECK_EQ(second.notified[1], 0);
    advance_input(a, 6);
    CHECK_EQ(first.samples, 1);
    CHECK_EQ(second.samples, 1);
    mc_asc_set_audio_source(a, &source1);
    CHECK_EQ(first.notifications, 3);
    CHECK_EQ(first.notified[2], 22050);
    advance_input(a, 8);
    CHECK_EQ(first.samples, 2);
    wr(a, 0x80A, 0);
    CHECK_EQ(first.notifications, 4);
    CHECK_EQ(first.notified[3], 0);
    mc_asc_set_audio_source(a, &source2);
    CHECK_EQ(second.notifications, 2);
    mc_asc_ops.reset(a);
    CHECK_EQ(second.notifications, 2);
    mc_asc_set_audio_source(a, NULL);
}

/* Both original PBX ROMs program these $F44 values (the
 * powerbook-*-singer-buffer.log traces). No RAM layout is inferred. */
static void test_singer_buffer_base_readback(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ENHANCED);
    a->block = a->singer = 1;
    static const uint32_t bases[] = {0x00010000u, 0x01FC0000u};
    for (unsigned i = 0; i < sizeof bases / sizeof *bases; i++) {
        mc_asc_ops.write(a, 0xF44, 4, bases[i]);
        CHECK_EQ(mc_asc_ops.read(a, 0xF44, 4), bases[i]);
        wr(a, 0x801, 1);
        wr(a, 0x80A, 1);
        wr(a, 0x803, 0x80);
        CHECK_EQ(mc_asc_ops.read(a, 0xF44, 4), bases[i]);
    }
}

static void test_reset_op(void) {
    tests++;
    mc_asc *a = fresh(MC_ASC_ORIGINAL);
    fill(a, 0, 1024, 0x80);
    /* Filled up (System 7.5.3's Sound Manager on the IIci, asc.h). */
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    CHECK_EQ(rd(a, 0x804), 0x02);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    wr(a, 0x801, 1);
    hw_fake_advance(&host, 600ull * TICK);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 1);
    mc_asc_ops.reset(a);
    CHECK_EQ(hw_fake_line(&host, a, MC_ASC_LINE_IRQ), 0);
    CHECK_EQ(rd(a, 0x801), 0);
    CHECK_EQ(mc_asc_fifo_level(a, 0), 0);
    CHECK(hw_fake_deadline(&host, a) == MC_NEVER);
}

int main(void) {
    test_reset();
    test_register_readback();
    test_drain_rate();
    test_status_irq();
    test_clear();
    test_routing_volume();
    test_wavetable();
    test_decode();
    test_decode_start();
    test_easc_start_state();
    test_wide();
    test_pull();
    test_record();
    test_record_input();
    test_lc_input();
    test_singer_input();
    test_singer_input_enable();
    test_singer_input_replacement();
    test_singer_buffer_base_readback();
    test_reset_op();
    printf("test_asc: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
