/* test_newage: the New Age floppy controller's command and result phases
 * (newage.h) as the ROM's driver drives them: the reset through the
 * data-rate register, the status register's ready and direction bits,
 * SENSE DRIVE STATUS of a missing drive ($FF), RECALIBRATE's interrupt
 * and SENSE INTERRUPT STATUS's two bytes, an invalid SENSE INTERRUPT,
 * VERSION, and the serial DMA status word; then a SuperDrive with an 800K
 * image: the insertion reported through SENSE INTERRUPT STATUS ($C0),
 * SENSE DRIVE STATUS, SEEK's seek end and cylinder, GCR READ ID's format
 * byte ($22) and READ DATA through the DMA port. */
#include "hw_test.h"
#include "newage.h"

#include <stdio.h>
#include <string.h>

static hw_fake fake;
static mc_newage na;

static uint8_t msr(void) { return (uint8_t)mc_newage_ops.read(&na, 0x101, 1); }
static void send(uint8_t b) {
    CHECK_EQ(msr() & 0xC0u, 0x80u);                 /* ready, to the controller */
    mc_newage_ops.write(&na, 0x141, 1, b);
}
static uint8_t result(void) {
    CHECK_EQ(msr() & 0xD0u, 0xD0u);                 /* ready, to the processor, busy */
    return (uint8_t)mc_newage_ops.read(&na, 0x141, 1);
}

int main(void) {
    hw_fake_init(&fake);
    mc_newage_init(&na, &fake.host, "New Age");
    mc_newage_ops.write(&na, 0x101, 1, 0x9C);      /* reset, as the driver ($191074) */
    CHECK_EQ(msr(), 0x80);
    send(0x13); send(0x00); send(0x0F); send(0x00); /* CONFIGURE: no result */
    CHECK_EQ(msr(), 0x80);
    send(0x04); send(0x00);                         /* SENSE DRIVE STATUS, no drive */
    CHECK_EQ(result(), 0xFF);
    CHECK_EQ(msr(), 0x80);
    send(0x08);                                     /* nothing pending: invalid */
    CHECK_EQ(result(), 0x80);
    send(0x07); send(0x00);                         /* RECALIBRATE */
    hw_fake_advance(&fake, hw_fake_deadline(&fake, &na));
    CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_IRQ), 1);
    send(0x08);
    CHECK_EQ(result() & 0xC8u, 0x48u);              /* abnormal, not ready */
    CHECK_EQ(result(), 0);
    CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_IRQ), 0);
    send(0x10);
    CHECK_EQ(result(), 0x90);
    CHECK_EQ(mc_newage_ops.read(&na, 0x090, 4), 0x40000000u);
    /* A SuperDrive with an 800K disk (sector s filled with s & $FF). */
    {
        const char *path = hw_work_path("newage", "dd.img");
        FILE *f = fopen(path, "wb");
        CHECK(f != NULL);
        static uint8_t sec[512];
        for (unsigned s = 0; f && s < 1600; s++) {
            memset(sec, (int)(s & 0xFFu), sizeof sec);
            fwrite(sec, 1, sizeof sec, f);
        }
        if (f) fclose(f);
        char err[200];
        mc_disk *d = mc_disk_open(path, 1, 512, err, sizeof err);
        CHECK(d != NULL);
        mc_newage_fit(&na, 1);
        CHECK_EQ(mc_newage_insert(&na, 0, d, 0, err, sizeof err), 1);
        CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_IRQ), 1);
        send(0x08);
        CHECK_EQ(result(), 0xC0);                   /* inserted, drive 0 */
        CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_IRQ), 0);
        send(0x04); send(0x00);
        CHECK_EQ(result(), 0xFC);                   /* DD, writable, SuperDrive, track 0 */
        send(0x0F); send(0x00); send(0x10);         /* SEEK 16 */
        hw_fake_advance(&fake, hw_fake_deadline(&fake, &na));
        CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_IRQ), 1);
        send(0x08);
        CHECK_EQ(result(), 0x20);
        CHECK_EQ(result(), 0x10);
        send(0x0A); send(0x04);                     /* GCR READ ID, head 1 */
        CHECK_EQ(msr(), 0x10);                      /* executing */
        hw_fake_advance(&fake, hw_fake_deadline(&fake, &na));
        CHECK_EQ(result(), 0x04);
        CHECK_EQ(result(), 0); CHECK_EQ(result(), 0);
        CHECK_EQ(result(), 16); CHECK_EQ(result(), 1); (void)result();
        CHECK_EQ(result(), 0x22);
        /* READ DATA, cylinder 16 head 1 sectors 2-3: image sectors
         * 2 x (12 x 16) + 11 + 2 = 397, 398. */
        const uint8_t rd[9] = {0x06, 0x04, 16, 1, 2, 0x22, 3, 0x1B, 0};
        for (unsigned i = 0; i < 9; i++) send(rd[i]);
        hw_fake_advance(&fake, hw_fake_deadline(&fake, &na));
        CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_DRQ), 1);
        uint8_t b = 0;
        unsigned got = 0, good = 0;
        while (mc_newage_dma_read(&na, &b)) {
            good += b == (uint8_t)((397u + got / 512u) & 0xFFu);
            got++;
        }
        CHECK_EQ(got, 1024u);
        CHECK_EQ(good, 1024u);
        CHECK_EQ(hw_fake_line(&fake, &na, MC_NEWAGE_LINE_DRQ), 0);
        CHECK_EQ(result(), 0x04);
        mc_newage_remove(&na, 0);
        mc_disk_close(d);
    }
    printf("test_newage: %d failure(s)\n", mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
