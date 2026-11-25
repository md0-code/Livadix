/* test_psc: the PSC (psc.h) against a fake host: the level 3-6 interrupt
 * flags and enables, VIA2's flags, enables and slot port, a DMA channel's
 * reset, pause, register sets and transfer (the SCSI HAL's sequence), the
 * channel flag only for a set with bit 12, the sound output's buffers as
 * the chime gives them, and the 60.15 Hz tick. */
#include "hw_test.h"
#include "asic/psc.h"

#include <stdlib.h>
#include <string.h>

static hw_fake fake;
static mc_psc psc;
static uint8_t mem[0x10000];

static int dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    (void)ctx;
    if (address + length > sizeof mem) return 0;
    if (write) memcpy(mem + address, data, length);
    else memcpy(data, mem + address, length);
    return 1;
}

static uint32_t rd(uint32_t off, unsigned size) { return mc_psc_ops.read(&psc, off, size); }
static void wr(uint32_t off, unsigned size, uint32_t v) { mc_psc_ops.write(&psc, off, size, v); }
static uint8_t via2(uint32_t reg) { return (uint8_t)mc_psc_via2_ops.read(&psc, reg << 9, 1); }
static void via2_wr(uint32_t reg, uint8_t v) { mc_psc_via2_ops.write(&psc, reg << 9, 1, v); }

static void test_levels(void) {
    mc_psc_set_source(&psc, MC_PSC_SRC_SCC_A, 1);
    CHECK_EQ(rd(0x140, 1) & 0x0Fu, 0x02);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_IRQ4), 0);   /* not enabled */
    wr(0x144, 1, 0x86);                                         /* bits 1 and 2, as the ROM ($10A38) */
    CHECK_EQ(rd(0x144, 1), 0x06);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_IRQ4), 1);
    CHECK_EQ(rd(0x140, 1), 0x82);                               /* bit 7: an enabled flag */
    wr(0x144, 1, 0x02);                                         /* cleared */
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_IRQ4), 0);
    mc_psc_set_source(&psc, MC_PSC_SRC_SCC_A, 0);
    mc_psc_set_source(&psc, MC_PSC_SRC_ENET, 1);
    wr(0x134, 1, 0x81);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_IRQ3), 1);
    mc_psc_set_source(&psc, MC_PSC_SRC_ENET, 0);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_IRQ3), 0);
}

static void test_via2(void) {
    via2_wr(14, 0x88);                                          /* the SCSI Manager's enable */
    CHECK_EQ(via2(14), 0x88);
    mc_psc_set_source(&psc, MC_PSC_SRC_VIA2_SCSI_IRQ, 1);
    CHECK_EQ(via2(13), 0x88);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_VIA2), 1);
    mc_psc_set_source(&psc, MC_PSC_SRC_VIA2_SCSI_IRQ, 0);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_VIA2), 0);
    mc_psc_set_source(&psc, MC_PSC_SRC_VIA2_FDC, 1);
    CHECK_EQ(via2(13), 0x20);                                   /* not enabled: no bit 7 */
    mc_psc_set_source(&psc, MC_PSC_SRC_VIA2_FDC, 0);
    mc_psc_set_slots(&psc, 1u << (0xC - 9));                    /* slot $C */
    CHECK_EQ(via2(15), 0xF7u);
    CHECK_EQ(via2(13) & 0x02, 0x02);
    mc_psc_set_slots(&psc, 0);
}

/* A device that gives `left` bytes 0, 1, 2... while it asks. */
static unsigned dev_left, dev_next;
static int dev_read(void *ctx, uint8_t *b) {
    (void)ctx;
    if (!dev_left) return 0;
    dev_left--;
    *b = (uint8_t)dev_next++;
    if (!dev_left) mc_psc_request(&psc, MC_PSC_CH_SCSI, 0);
    return 1;
}

static void test_dma(void) {
    const mc_psc_port port = {&psc, dev_read, NULL};
    mc_psc_set_port(&psc, MC_PSC_CH_SCSI, &port);
    wr(0xC00, 2, 0x8800);                                       /* reset */
    CHECK_EQ(rd(0xC00, 2), 0xC400);
    wr(0xC00, 2, 0x8400);                                       /* the SCSI HAL ($1F100) */
    CHECK(rd(0xC00, 2) & 0x4000u);
    const unsigned set = rd(0xC00, 2) & 1u;
    wr(0x1004 + 0x10 * set, 4, 64);
    wr(0x1000 + 0x10 * set, 4, 0x2000);
    wr(0x1008 + 0x10 * set, 2, 0x8200);                         /* from the device */
    wr(0x1008 + 0x10 * set, 2, 0x0100);
    wr(0x1008 + 0x10 * set, 2, 0x8800);                         /* armed */
    CHECK_EQ(rd(0x1008 + 0x10 * set, 2) & 0x0100u, 0);          /* paused: not begun */
    wr(0xC00, 2, 0x0400);
    CHECK_EQ(rd(0xC00, 2) & 0x4000u, 0);
    dev_left = 40;
    dev_next = 0;
    mc_psc_request(&psc, MC_PSC_CH_SCSI, 1);
    CHECK_EQ(rd(0x1004 + 0x10 * set, 4), 24);                   /* the residual */
    CHECK_EQ(rd(0x1008 + 0x10 * set, 2) & 0x0900u, 0x0900);     /* begun, still armed */
    CHECK_EQ(mem[0x2000], 0);
    CHECK_EQ(mem[0x2027], 39);
    dev_left = 30;
    mc_psc_request(&psc, MC_PSC_CH_SCSI, 1);
    CHECK_EQ(rd(0x1004 + 0x10 * set, 4), 0);
    CHECK_EQ(rd(0x1008 + 0x10 * set, 2) & 0x0D00u, 0x0500);     /* terminal count, no longer armed */
    CHECK_EQ(mem[0x203F], 63);
    CHECK_EQ(rd(0x804, 4), 0);                                  /* no bit 12: no flag */
    /* A set with bit 12 raises the channel's flag and level 4 bit 3. */
    wr(0xC00, 2, 0x8400);
    const unsigned s2 = rd(0xC00, 2) & 1u;
    wr(0x1004 + 0x10 * s2, 4, 4);
    wr(0x1000 + 0x10 * s2, 4, 0x3000);
    wr(0x1008 + 0x10 * s2, 2, 0x9A00);
    wr(0xC00, 2, 0x0400);
    dev_left = 4;
    mc_psc_request(&psc, MC_PSC_CH_SCSI, 1);
    CHECK_EQ(rd(0x804, 4), 0x80000000u);
    CHECK_EQ(rd(0x140, 1) & 0x08u, 0x08);
    wr(0xC00, 2, 0x8800);                                       /* the handler's reset */
    CHECK_EQ(rd(0x804, 4), 0);
    mc_psc_set_port(&psc, MC_PSC_CH_SCSI, NULL);
}

static void test_sound(void) {
    for (unsigned i = 0; i < 0x200; i++) mem[0x4000 + i] = (uint8_t)i;
    memset(mem + 0x4200, 0xAA, 0x200);
    wr(0x218, 2, 0x40);                                         /* 512 bytes: 128 frames */
    wr(0x214, 4, 0x4000);
    wr(0x200, 2, 0x0104);
    CHECK(rd(0x20C, 4) & 0x3FFFC0u);                            /* playing */
    hw_fake_step(&fake, (uint64_t)MC_C15M_HZ * 60u / MC_PSC_SOUND_RATE);
    wr(0x214, 4, 0x4200);                                       /* the next buffer, waiting */
    hw_fake_step(&fake, (uint64_t)MC_C15M_HZ * 100u / MC_PSC_SOUND_RATE);
    CHECK(rd(0x20C, 4) & 0x3FFFC0u);                            /* the second one plays */
    int16_t out[2 * 256];
    const size_t n = mc_psc_pull(&psc, out, 256);
    CHECK(n >= 150);
    CHECK_EQ((uint16_t)out[0], 0x0001u);                        /* left: bytes 0, 1 */
    CHECK_EQ((uint16_t)out[1], 0x0203u);                        /* right: bytes 2, 3 */
    CHECK_EQ((uint16_t)out[2 * 128], 0xAAAAu);                  /* the second buffer follows at once */
    wr(0x200, 2, 0x0004);
    CHECK_EQ(rd(0x20C, 4) & 0x3FFFC0u, 0);
}
/* The DSP's frames: a frame interrupt every +$218 samples at 24 kHz, IACK
 * clearing it; while recording, the input into the halves of +$210. */
static void src_sample(void *ctx, unsigned rate, int16_t frame[2]) {
    (void)ctx; (void)rate;
    frame[0] = 0x1234;
    frame[1] = -2;
}
static unsigned src_rate;
static void src_enable(void *ctx, unsigned rate) { (void)ctx; src_rate = rate; }
static void test_dsp_frames(void) {
    const mc_audio_source source = {NULL, src_sample, src_enable};
    mc_psc_set_audio_source(&psc, &source);
    memset(mem + 0x6000, 0, 0x100);
    wr(0x218, 2, 8);                                            /* 8 samples a frame */
    wr(0x210, 4, 0x6000);
    wr(0x200, 2, 0x0044);
    const uint64_t period = 8ull * MC_C15M_HZ / 24000u;
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_DSP_FRAME), 0);
    hw_fake_step(&fake, period + 1u);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_DSP_FRAME), 1);
    CHECK_EQ(src_rate, 0u);
    CHECK_EQ(mem[0x6020], 0);                                   /* not recording */
    mc_psc_dsp_ack(&psc);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_DSP_FRAME), 0);
    wr(0x200, 2, 0x00C4);
    CHECK_EQ(src_rate, 24000u);
    hw_fake_step(&fake, period);
    CHECK_EQ(mem[0x6000] | mem[0x6020], 0x12);                  /* one half: $1234, $FFFE */
    CHECK_EQ(mem[0x6002] | mem[0x6022], 0xFF);
    wr(0x200, 2, 0x0004);
    CHECK_EQ(src_rate, 0u);
    CHECK_EQ(hw_fake_line(&fake, &psc, MC_PSC_LINE_DSP_FRAME), 0);
    mc_psc_set_audio_source(&psc, NULL);
}
static void test_tick(void) {
    const uint64_t half = (uint64_t)MC_C15M_HZ * 100u / 6015u / 2u;
    const unsigned before = hw_fake_changes(&fake, &psc, MC_PSC_LINE_TICK);
    hw_fake_step(&fake, half * 10u + 1u);
    const unsigned edges = hw_fake_changes(&fake, &psc, MC_PSC_LINE_TICK) - before;
    CHECK(edges >= 9 && edges <= 11);
}

int main(void) {
    hw_fake_init(&fake);
    fake.host.dma = dma;
    mc_psc_init(&psc, &fake.host, "PSC");
    mc_psc_ops.reset(&psc);
    test_levels();
    test_via2();
    test_dma();
    test_sound();
    test_dsp_frames();
    test_tick();
    mc_psc_free(&psc);
    printf("test_psc: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
