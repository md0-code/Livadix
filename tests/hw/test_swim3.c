/* test_swim3: SWIM III (swim3.h) against a fake host with a 1440K image:
 * the phase register test the driver runs, the drive's status lines through
 * SEL/CA2-CA0, a seek with its interrupt, an address field, a sector read
 * and a write of the driver's MFM stream through the DMA port with the DRQ
 * line; an 800K disk read and written in GCR. */
#include "hw_test.h"
#include "ppc/chips/swim3.h"

#include <stdio.h>
#include <string.h>

static hw_fake fake;
static mc_swim3 s;

static void w(unsigned reg, uint8_t v) { mc_swim3_ops.write(&s, reg * 0x200u, 1, v); }
static uint8_t r(unsigned reg) { return (uint8_t)mc_swim3_ops.read(&s, reg * 0x200u, 1); }

/* The driver's status read ($FFD908EA): phase 3, SEL, then the address. */
static int status(unsigned addr) {
    w(4, 3);
    w((addr & 8u) ? 7 : 6, 0x20);
    w(4, (uint8_t)(addr & 7u));
    return (r(7) & 0x04u) != 0;
}

/* A drive command ($FFD90928): the address, then LSTRB high and low. */
static void command(unsigned addr, unsigned value) {
    w(4, 3);
    w((addr & 4u) ? 7 : 6, 0x20);
    const uint8_t ph = (uint8_t)((addr & 3u) | (value ? 4u : 0u));
    w(4, ph);
    w(4, ph | 8u);
    w(4, ph);
}

int main(void) {
    hw_fake_init(&fake);
    const char *path = hw_work_path("swim3", "hd.img");
    FILE *f = fopen(path, "wb");
    unsigned n;
    uint8_t sec[512];
    for (unsigned i = 0; i < 2880; i++) {
        memset(sec, (int)(i & 0xFFu), sizeof sec);
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    char err[128];
    mc_disk *disk = mc_disk_open(path, 1, 512, err, sizeof err);
    CHECK(disk != NULL);
    mc_swim3_init(&s, &fake.host, "SWIM3", 1);
    mc_swim3_ops.reset(&s);
    CHECK(mc_swim3_insert(&s, 0, disk, 0, err, sizeof err));
    /* The phase register test ($FFD90880). */
    for (uint8_t v = 5; v <= 7; v++) {
        w(4, v);
        CHECK_EQ(r(4), v);
    }
    w(7, 0x02);                                                /* drive 1 ($FFD9097C) */
    CHECK_EQ(status(0x8), 0);                                  /* /CSTIN: a disk */
    CHECK_EQ(status(0x7), 0);                                  /* /DRVIN */
    CHECK_EQ(status(0xF), 0);                                  /* high density */
    command(0x2, 0);                                           /* motor on */
    CHECK_EQ(status(0x2), 0);
    command(0x5, 0);                                           /* MFM mode */
    CHECK_EQ(status(0xD), 1);
    CHECK_EQ(status(0xA), 0);                                  /* track 0 */
    /* Seek 3 tracks inward ($FFD907DE-$FFD907FA). */
    command(0x0, 0);                                           /* DIRTN 0: inward */
    w(9, 3);
    r(8);
    w(7, 0x01);                                                /* interrupts on (mode bit 0) */
    w(15, 0x02);
    w(7, 0x80);
    hw_fake_step(&fake, MC_C15M_HZ / 50u);
    CHECK_EQ(hw_fake_line(&fake, &s, MC_SWIM3_LINE_IRQ), 1);
    CHECK_EQ(r(8) & 0x02, 0x02);
    CHECK_EQ(hw_fake_line(&fake, &s, MC_SWIM3_LINE_IRQ), 0);
    CHECK_EQ(status(0xA), 1);                                  /* off track 0 */
    w(15, 0);
    /* An address field: GO without a count. */
    w(5, 0x28);                                                /* MFM ($FFD908DC) */
    w(14, 0);
    w(15, 0x04);
    w(7, 0x08);
    hw_fake_step(&fake, MC_C15M_HZ / 100u);
    CHECK_EQ(r(8) & 0x04, 0x04);
    CHECK_EQ(r(10) & 0x7F, 3);                                 /* bit 7: the side */
    CHECK_EQ(r(11), 1);                                        /* MFM sectors from 1 */
    CHECK_EQ(r(12), 2);                                        /* the size code: 512 bytes */
    w(15, 0);
    /* Read sector 2 of track 3, side 1: image sector (3 x 2 + 1) x 18 + 1. */
    w(6, 0x18);
    w(7, 0x20);                                                /* SEL: head 1 */
    w(13, 2);
    w(14, 1);
    w(15, 0x08);
    w(7, 0x08);
    CHECK_EQ(hw_fake_line(&fake, &s, MC_SWIM3_LINE_DRQ), 1);
    uint8_t b, first = 0;
    n = 0;
    while (mc_swim3_dma_read(&s, &b)) {
        if (!n) first = b;
        n++;
    }
    CHECK_EQ(n, 512);
    CHECK_EQ(first, (7 * 18 + 1) & 0xFF);
    CHECK_EQ(hw_fake_line(&fake, &s, MC_SWIM3_LINE_DRQ), 0);
    hw_fake_step(&fake, MC_C15M_HZ / 100u);
    CHECK_EQ(r(8) & 0x08, 0x08);
    /* Write it back with a new pattern, as the driver's stream ($FFD90104-
     * $FFD90142): gap, sync, the marks after $99, $99 $0F, the data (its
     * $99 $08 bytes are data), $99 $04, gap, $99 $08 the end. */
    w(15, 0x08);
    w(13, 2);
    w(14, 1);
    w(7, 0x10);
    w(7, 0x08);
    static const uint8_t head[] = {0x4E, 0x4E, 0, 0, 0x99, 0xA1, 0x99, 0xA1, 0x99, 0xA1, 0x99, 0xFB, 0x99, 0x0F};
    static const uint8_t tail[] = {0x99, 0x04, 0x4E, 0x4E, 0x99, 0x08};
    for (unsigned i = 0; i < sizeof head; i++) CHECK(mc_swim3_dma_write(&s, head[i]));
    for (unsigned i = 0; i < 512; i++) CHECK(mc_swim3_dma_write(&s, i == 0 ? 0x99 : i == 1 ? 0x08 : 0xA5));
    for (unsigned i = 0; i < sizeof tail; i++) CHECK(mc_swim3_dma_write(&s, tail[i]));
    CHECK(!mc_swim3_dma_write(&s, 0x4E));                      /* the end taken */
    hw_fake_step(&fake, MC_C15M_HZ / 100u);
    CHECK_EQ(r(8) & 0x08, 0x08);
    w(6, 0x18);
    CHECK(mc_disk_read(disk, 7 * 18 + 1, 1, sec));
    CHECK_EQ(sec[0], 0x99);
    CHECK_EQ(sec[1], 0x08);
    CHECK_EQ(sec[100], 0xA5);
    /* Eject. */
    command(0x3, 1);
    CHECK_EQ(status(0x8), 1);
    int ejected = 0;
    CHECK(mc_swim3_media(&s, 0, &ejected) == disk && ejected);
    mc_disk_close(mc_swim3_remove(&s, 0));

    /* An 800K disk: no address field in MFM; in GCR ($4C) sectors read as
     * the sector number and the data field's 703 nibbles as 6-bit values
     * ($FFD90004-$FFD900B0), written from 6-bit values after $D5 $AA
     * ($FFD90146-$FFD901F0). */
    path = hw_work_path("swim3", "dd.img");
    f = fopen(path, "wb");
    for (unsigned i = 0; i < 1600; i++) {
        memset(sec, (int)(i & 0xFFu), sizeof sec);
        fwrite(sec, 1, sizeof sec, f);
    }
    fclose(f);
    disk = mc_disk_open(path, 1, 512, err, sizeof err);
    CHECK(disk != NULL);
    mc_swim3_ops.reset(&s);
    CHECK(mc_swim3_insert(&s, 0, disk, 0, err, sizeof err));
    w(7, 0x03);
    command(0x2, 0);                                           /* motor on */
    w(5, 0x28);                                                /* MFM: nothing comes round */
    w(14, 0);
    w(15, 0x04);
    w(7, 0x08);
    hw_fake_step(&fake, MC_C15M_HZ / 50u);
    CHECK_EQ(r(8) & 0x04, 0);
    w(6, 0x08);
    w(5, 0x4C);                                                /* GCR */
    w(7, 0x08);
    hw_fake_step(&fake, MC_C15M_HZ / 50u);
    CHECK_EQ(r(8) & 0x04, 0x04);
    w(6, 0x08);
    w(13, 5);                                                  /* track 3 (the head stays), side 0, sector 5: 3 x 24 + 5 */
    w(14, 1);
    w(15, 0x08);
    w(7, 0x08);
    uint8_t field[1 + MC_GCR_DATA_NIBBLES], nib[MC_GCR_DATA_NIBBLES], raw[524];
    n = 0;
    while (n < sizeof field && mc_swim3_dma_read(&s, &field[n])) n++;
    CHECK_EQ(n, sizeof field);
    CHECK_EQ(field[0], 5);
    for (unsigned i = 0; i < MC_GCR_DATA_NIBBLES; i++) nib[i] = mc_gcr_encode6[field[1 + i] & 0x3F];
    CHECK(mc_gcr_decode_data(nib, raw));
    CHECK_EQ(raw[12], 77);
    CHECK_EQ(raw[523], 77);
    hw_fake_step(&fake, MC_C15M_HZ / 100u);
    CHECK_EQ(r(8) & 0x08, 0x08);
    w(6, 0x08);
    /* Write sector 5 back filled with $3C. */
    memset(raw, 0, 12);
    memset(raw + 12, 0x3C, 512);
    mc_gcr_encode_data(raw, nib);
    w(13, 5);
    w(14, 1);
    w(7, 0x10);
    w(7, 0x08);
    static const uint8_t gcr_head[] = {0x3F, 0xBF, 0x1E, 0x34, 0x3C, 0x3F, 0xD5, 0xAA, 0x0B, 0x05};
    static const uint8_t gcr_tail[] = {0x27, 0xAA, 0x3F, 0x3F, 0x99, 0x08};
    for (unsigned i = 0; i < sizeof gcr_head; i++) CHECK(mc_swim3_dma_write(&s, gcr_head[i]));
    for (unsigned i = 0; i < MC_GCR_DATA_NIBBLES; i++) CHECK(mc_swim3_dma_write(&s, (uint8_t)mc_gcr_decode6(nib[i])));
    for (unsigned i = 0; i < sizeof gcr_tail; i++) CHECK(mc_swim3_dma_write(&s, gcr_tail[i]));
    hw_fake_step(&fake, MC_C15M_HZ / 100u);
    CHECK_EQ(r(8) & 0x08, 0x08);
    CHECK_EQ(r(2), 0);                                         /* no error */
    CHECK(mc_disk_read(disk, 77, 1, sec));
    CHECK_EQ(sec[0], 0x3C);
    CHECK_EQ(sec[511], 0x3C);
    mc_disk_close(mc_swim3_remove(&s, 0));
    printf("test_swim3: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
