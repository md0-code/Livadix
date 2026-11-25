/* test_amic: the PDM boards' AMIC (amic.h) and its codec AWAC (awac.h)
 * against a fake host: the interrupt register as the nanokernel reads it,
 * the pseudo-VIA2 at the offsets the 68k code uses, the floppy channel's
 * transfer and interrupt, the Ethernet receive ring and transmit buffers
 * as the PDM MACE driver reads and fills them, the sound output's buffers
 * and flags as the start-up chime polls them, and the codec port. */
#include "hw_test.h"
#include "ppc/chips/amic.h"
#include "ppc/chips/awac.h"

#include <string.h>

static hw_fake fake;
static mc_amic amic;
static mc_awac awac;
static uint8_t mem[0x80000];

static int dma(void *ctx, uint32_t address, void *data, uint32_t length, int write) {
    (void)ctx;
    if (address + length > sizeof mem) return 0;
    if (write) memcpy(mem + address, data, length);
    else memcpy(data, mem + address, length);
    return 1;
}

static uint32_t rd(const mc_dev_ops *ops, uint32_t off, unsigned size) { return ops->read(&amic, off, size); }
static void wr(const mc_dev_ops *ops, uint32_t off, unsigned size, uint32_t v) { ops->write(&amic, off, size, v); }

static void test_interrupts(void) {
    /* The nanokernel's sequence ($FFF107BC): write $C0, read. */
    wr(&mc_amic_int_ops, 0, 1, 0xC0);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1), 0xC0);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 0);
    mc_amic_set_source(&amic, MC_AMIC_SRC_VIA1, 1);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1), 0xC1);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);
    mc_amic_set_source(&amic, MC_AMIC_SRC_VIA1, 0);
    mc_amic_set_source(&amic, MC_AMIC_SRC_SCC, 1);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 0x3F, 0x04);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);
    wr(&mc_amic_int_ops, 0, 1, 0xC0);                          /* acknowledged: the SCC still pending, INT released† */
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 0);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1), 0xC4);
    mc_amic_set_source(&amic, MC_AMIC_SRC_VIA1, 1);            /* a change interrupts again */
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);
    wr(&mc_amic_int_ops, 0, 1, 0xC0);
    mc_amic_set_source(&amic, MC_AMIC_SRC_VIA1, 0);
    mc_amic_set_source(&amic, MC_AMIC_SRC_SCC, 0);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);  /* so does a source going away: the level drops */
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1), 0xC0);
    wr(&mc_amic_int_ops, 0, 1, 0xC0);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 0);
}

static void test_via2(void) {
    /* The 68k code's RBV-style offsets: IER at +$1C13, IFR at +$1A03. */
    wr(&mc_amic_via2_ops, 0x1C13, 1, 0x7F);
    wr(&mc_amic_via2_ops, 0x1C13, 1, 0x88);                    /* the SCSI Manager's SCSI-A IRQ */
    CHECK_EQ(rd(&mc_amic_via2_ops, 0x13, 1), 0x88);
    mc_amic_set_via2(&amic, MC_AMIC_VIA2_SCSI_A_IRQ, 1);
    CHECK_EQ(rd(&mc_amic_via2_ops, 0x1A03, 1), 0x88);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 0x02, 0x02);         /* level 2 */
    mc_amic_set_via2(&amic, MC_AMIC_VIA2_SCSI_A_IRQ, 0);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 0x02, 0);
    /* The slots: VBL on bit 6, active low. */
    wr(&mc_amic_via2_ops, 0x12, 1, 0xC0);
    mc_amic_set_slots(&amic, 0x40, 0x40);
    CHECK_EQ(rd(&mc_amic_via2_ops, 0x02, 1), 0xBF);
    CHECK_EQ(rd(&mc_amic_via2_ops, 0x03, 1) & 0x02, 0x02);
    wr(&mc_amic_via2_ops, 0x13, 1, 0x82);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);
    mc_amic_set_slots(&amic, 0, 0x40);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 0x02, 0);
    wr(&mc_amic_int_ops, 0, 1, 0xC0);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 0);
    wr(&mc_amic_via2_ops, 0x13, 1, 0x7F);
}

static unsigned dev_left, dev_next;
static int dev_read(void *ctx, uint8_t *b) {
    (void)ctx;
    if (!dev_left) return 0;
    dev_left--;
    *b = (uint8_t)dev_next++;
    if (!dev_left) mc_amic_request(&amic, MC_AMIC_CH_FLOPPY, 0);
    return 1;
}

static void test_floppy(void) {
    const mc_amic_port port = {&amic, dev_read, NULL};
    mc_amic_set_port(&amic, MC_AMIC_CH_FLOPPY, &port);
    /* The base, then the driver's view of its buffer ($FFD8EB06). */
    wr(&mc_amic_dma_ops, 0x0001, 1, 0x04);
    wr(&mc_amic_dma_ops, 0x0000, 1, 0x00);
    CHECK_EQ(mc_amic_dma_base(&amic), 0x00040000u);
    const uint32_t buf = rd(&mc_amic_dma_ops, 0x1060, 4);
    CHECK_EQ(buf, 0x00040000u + MC_AMIC_AREA_FLOPPY);
    /* A sector ($FFD8FD82): low address bytes, count $80, run, device to memory. */
    wr(&mc_amic_dma_ops, 0x1062, 1, (buf >> 8) & 0xFF);
    wr(&mc_amic_dma_ops, 0x1063, 1, buf & 0xFF);
    wr(&mc_amic_dma_ops, 0x1064, 1, 0x00);
    wr(&mc_amic_dma_ops, 0x1065, 1, 0x80);
    wr(&mc_amic_dma_ops, 0x1068, 1, (rd(&mc_amic_dma_ops, 0x1068, 1) & 0xBF) | 0x02);
    dev_left = 512;
    dev_next = 0;
    mc_amic_request(&amic, MC_AMIC_CH_FLOPPY, 1);
    CHECK_EQ(mem[buf], 0);
    CHECK_EQ(mem[buf + 511], 0xFF);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1064, 2), 0);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1068, 1) & 0x80, 0x80);    /* done */
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 1u << MC_AMIC_DMA_FLOPPY);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 0x10, 0x10);         /* level 4 */
    wr(&mc_amic_dma_ops, 0x1068, 1, 0x80);                     /* cleared */
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 0);
    /* SWIM III's interrupt: only with bit 3 set ($FFD90C30). */
    mc_amic_floppy_irq(&amic, 1);
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 0);
    wr(&mc_amic_dma_ops, 0x1068, 1, 0x08);
    mc_amic_floppy_irq(&amic, 1);
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 1u << MC_AMIC_DMA_FLOPPY);
    wr(&mc_amic_dma_ops, 0x1068, 1, 0x80);
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 0);
}

static uint8_t sent[2048];
static size_t sent_n;
static void transmit(void *ctx, const uint8_t *frame, size_t length) {
    (void)ctx;
    memcpy(sent, frame, length);
    sent_n = length;
}

static void test_ethernet(void) {
    const uint32_t ring = mc_amic_dma_base(&amic) + MC_AMIC_AREA_ENET_RX;
    uint8_t frame[300], status[4] = {0x2C, 0x01, 0x00, 0x00};
    for (unsigned i = 0; i < sizeof frame; i++) frame[i] = (uint8_t)i;
    CHECK(!mc_amic_enet_receive(&amic, frame, sizeof frame, status));     /* not running */
    /* The driver's start ($FFEF0C24-$FFEF0C52). */
    wr(&mc_amic_dma_ops, 0x1028, 1, 0x80);
    wr(&mc_amic_dma_ops, 0x0C20, 1, 0x80);
    wr(&mc_amic_dma_ops, 0x1028, 1, 0x8A);
    wr(&mc_amic_dma_ops, 0x1034, 1, 0);
    CHECK(mc_amic_enet_receive(&amic, frame, sizeof frame, status));
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1030, 1), 2);              /* 8 + 300 bytes: two slots */
    CHECK_EQ(mem[ring] << 8 | mem[ring + 1], 0x0000 | 300);     /* count, no error nibble */
    CHECK_EQ(mem[ring + 8], 0);
    CHECK_EQ(mem[ring + 0x100], 248);                          /* the frame runs on into slot 1 */
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1028, 1) & 0x80, 0x80);
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 1u << MC_AMIC_DMA_ENET);
    wr(&mc_amic_dma_ops, 0x1028, 1, 0x8A);                     /* acknowledged, still running */
    CHECK_EQ(rd(&mc_amic_int_ops, 0x08, 1), 0);
    /* Fill the ring: $C0 slots; the driver has taken none beyond 0. */
    uint8_t big[1500] = {0};
    unsigned frames = 0;
    while (mc_amic_enet_receive(&amic, big, sizeof big, status) && frames < 100) frames++;
    CHECK(frames > 0 && frames < 100);
    /* Transmit buffer 0: count at +$1044 ($FFEF06BC), start with $8A. */
    mc_amic_set_enet_transmit(&amic, transmit, NULL);
    const uint32_t tx = mc_amic_dma_base(&amic) + MC_AMIC_AREA_ENET_TX;
    for (unsigned i = 0; i < 60; i++) mem[tx + i] = (uint8_t)(0xA0 + i);
    wr(&mc_amic_dma_ops, 0x1044, 1, 0);
    wr(&mc_amic_dma_ops, 0x1045, 1, 60);
    wr(&mc_amic_dma_ops, 0x0C20, 1, 0x8A);
    CHECK_EQ(sent_n, 60);
    CHECK_EQ(sent[59], 0xA0 + 59);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x0C20, 1) & 0x80, 0x80);
    wr(&mc_amic_dma_ops, 0x0C20, 1, 0x80);
    /* Ready for the next frame (bits 6-5, $FFEF071A), again from buffer 0
     * ($FFEF0748). */
    CHECK(rd(&mc_amic_dma_ops, 0x0C20, 1) & 0x60);
    mem[tx] = 0x55;
    sent_n = 0;
    wr(&mc_amic_dma_ops, 0x0C20, 1, 0x8A);
    CHECK_EQ(sent_n, 60);
    CHECK_EQ(sent[0], 0x55);
    wr(&mc_amic_dma_ops, 0x0C20, 1, 0x80);
    wr(&mc_amic_dma_ops, 0x1028, 1, 0x01);                     /* reset */
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1030, 1), 0);
}

static void codec_word(uint16_t w) {
    /* The chime's sequence ($FFCE5A46-$FFCE5A84). */
    wr(&mc_amic_sound_ops, 0x00, 1, 0x40);
    wr(&mc_amic_sound_ops, 0x02, 1, w & 0xFF);
    wr(&mc_amic_sound_ops, 0x01, 1, w >> 8);
    wr(&mc_amic_sound_ops, 0x00, 1, 0xC0);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x00, 1) & 0x80, 0x80);
    hw_fake_step(&fake, 10);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x00, 1) & 0x80, 0);
}

static void test_sound(void) {
    codec_word(0x4000);                                        /* speaker: no attenuation */
    codec_word(0x2000 | 0x0E | 0x0E << 6);
    CHECK_EQ(awac.reg[4], 0);
    CHECK_EQ(awac.reg[2], 0x38E);
    mc_awac_set_status(&awac, 2, 0x08);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x06, 1), 0x08);
    /* Two buffers of $1000 at the base + $10000 and + $12000. */
    const uint32_t out = mc_amic_dma_base(&amic) + MC_AMIC_AREA_SND_OUT;
    for (unsigned i = 0; i < 0x1000; i += 4) {
        mem[out + i] = 0x12; mem[out + i + 1] = 0x34; mem[out + i + 2] = 0x56; mem[out + i + 3] = 0x78;
        mem[out + 0x2000 + i] = 0xFF; mem[out + 0x2000 + i + 1] = 0xFE;
        mem[out + 0x2000 + i + 2] = 0x00; mem[out + 0x2000 + i + 3] = 0x02;
    }
    int16_t frames[2 * 4096];
    mc_amic_pull(&amic, frames, 4096);                         /* drain the silence */
    wr(&mc_amic_sound_ops, 0x08, 1, 4);
    wr(&mc_amic_sound_ops, 0x09, 1, 0);
    wr(&mc_amic_sound_ops, 0x18, 1, 0xE0);
    wr(&mc_amic_sound_ops, 0x10, 1, 0x04 | 1);                 /* 22050 Hz†, run */
    CHECK(rd(&mc_amic_sound_ops, 0x0E, 1) | rd(&mc_amic_sound_ops, 0x0D, 1));
    /* $400 frames at 22050 Hz: 46 ms; buffer 0 first (bit 6). */
    hw_fake_step(&fake, MC_C15M_HZ / 20u);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x18, 1) & 0xC0, 0x40);
    wr(&mc_amic_sound_ops, 0x18, 1, 0x40);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x18, 1) & 0xC0, 0);
    hw_fake_step(&fake, MC_C15M_HZ / 20u);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x18, 1) & 0xC0, 0x80);
    const size_t n = mc_amic_pull(&amic, frames, 4096);
    CHECK(n > 3000);
    CHECK_EQ(frames[2 * 10], 0x1234);
    CHECK_EQ(frames[2 * 10 + 1], 0x5678);
    CHECK_EQ(frames[2 * (n - 10) + 1], 2);                     /* the speaker (register 4) unattenuated */
    /* The driver's interrupt: enables in +$14, DMA flag bit 8. */
    wr(&mc_amic_sound_ops, 0x14, 1, 0xF0);
    wr(&mc_amic_sound_ops, 0x14, 1, 0x0C);
    hw_fake_step(&fake, MC_C15M_HZ / 20u);
    CHECK_EQ(rd(&mc_amic_int_ops, 0x0A, 1) & 0x01, 0x01);
    wr(&mc_amic_sound_ops, 0x14, 1, rd(&mc_amic_sound_ops, 0x14, 1));
    CHECK_EQ(rd(&mc_amic_int_ops, 0x0A, 1), 0);
    /* Stopped: the progress reads 0 ($FFCE5C8C). */
    wr(&mc_amic_sound_ops, 0x10, 1, 0);
    CHECK_EQ(rd(&mc_amic_sound_ops, 0x0C, 4) & 0xFFFFFF00u, 0);
}

static void test_tick(void) {
    const unsigned before = hw_fake_changes(&fake, &amic, MC_AMIC_LINE_TICK);
    hw_fake_step(&fake, 704u * 370u * 5u + 1u);
    const unsigned edges = hw_fake_changes(&fake, &amic, MC_AMIC_LINE_TICK) - before;
    CHECK(edges >= 9 && edges <= 11);
    CHECK_EQ(mc_amic_diag_ops.read(&amic, 0, 1), 1);
}

/* 9150 Developer Note, Figure 2 and FAT AMIC IC: the fourth NuBus
 * connector reaches bit 1; SCSI DMA bit 5 identifies the chip revision. */
static void test_fat_amic(void) {
    amic.variant = MC_AMIC_FAT_1;
    mc_amic_int_ops.reset(&amic);
    wr(&mc_amic_int_ops, 0, 1, 0xC0);
    wr(&mc_amic_via2_ops, 0x12, 1, 0x82);
    wr(&mc_amic_via2_ops, 0x13, 1, 0x82);
    mc_amic_set_slots(&amic, 2, 2);
    CHECK_EQ(rd(&mc_amic_via2_ops, 2, 1), 0x7D);
    CHECK_EQ(hw_fake_line(&fake, &amic, MC_AMIC_LINE_CPU), 1);
    mc_amic_set_slots(&amic, 0, 2);
    CHECK_EQ(rd(&mc_amic_int_ops, 0, 1) & 2, 0);
    wr(&mc_amic_dma_ops, 0x1008, 1, 0x20);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1008, 1) & 0x20, 0);
    amic.variant = MC_AMIC_FAT_2;
    mc_amic_int_ops.reset(&amic);
    wr(&mc_amic_dma_ops, 0x1008, 1, 0);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1008, 1) & 0x20, 0x20);
    CHECK_EQ(rd(&mc_amic_dma_ops, 0x1009, 1) & 0x20, 0x20);
}

int main(void) {
    hw_fake_init(&fake);
    fake.host.dma = dma;
    mc_amic_init(&amic, &fake.host, "AMIC");
    mc_awac_init(&awac, &fake.host, "AWAC");
    mc_amic_set_codec(&amic, &awac);
    test_interrupts();
    test_via2();
    test_floppy();
    test_ethernet();
    test_sound();
    test_tick();
    test_fat_amic();
    mc_amic_free(&amic);
    printf("test_amic: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
