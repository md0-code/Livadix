/* test_rbv: the Macintosh IIci's RBV and Bt478 (asic/rbv.c), and the NCR
 * 5380's wiring on the IIci board.
 *
 * Livadix's own tests, from the Guide to the Macintosh Family Hardware, 2nd
 * ed. (Tables 4-9, 4-15, 4-27, 12-3, 12-5, 12-6, 12-8) and the IIci ROM's
 * use of the registers (rbv.h): the interrupt enables and flags with the
 * 6522's set/clear and write-1-to-clear rules, the level-2 line, the slot
 * lines active low with their enables, the video's line through each
 * vertical blanking interval, the 60.15 Hz tick, the sense code and video
 * off bit, the depth and the picture from bank A with the pixels' high bits
 * set at the CLUT, the Bt478's auto-incrementing address; then, on the IIci
 * board, the 5380's IRQ and DRQ in the RBV's flags and an INQUIRY of the
 * CD-ROM drive through the pseudo-DMA window with DRQ. */
#include "asic/rbv.h"
#include "hw_machine.h"
#include "hw_test.h"
#include "models.h"
#include "ncr5380.h"
#include "scsi_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static hw_fake f;
static mc_rbv rbv;

static uint8_t rd(uint32_t reg) { return (uint8_t)mc_rbv_ops.read(&rbv, reg, 1); }
static void wr(uint32_t reg, uint8_t v) { mc_rbv_ops.write(&rbv, reg, 1, v); }
static int irq(void) { return hw_fake_line(&f, &rbv, MC_RBV_LINE_IRQ); }

static void setup(uint8_t sense) {
    hw_fake_init(&f);
    mc_rbv_init(&rbv, &f.host, "RBV", sense);
}

static void test_interrupts(void) {
    setup(MC_RBV_SENSE_HIRES);
    CHECK_EQ(rd(MC_RBV_IER), 0x80);
    CHECK(!irq());
    /* A flag without its enable: in IFR, bit 7 clear, no interrupt. */
    mc_rbv_set_irq(&rbv, MC_RBV_IRQ_ASC, 1);
    CHECK_EQ(rd(MC_RBV_IFR), MC_RBV_IRQ_ASC);
    CHECK(!irq());
    /* IER: bit 7 set sets the bits written ($90: the ASC, ROM offset $A026). */
    wr(MC_RBV_IER, 0x90);
    CHECK_EQ(rd(MC_RBV_IER), 0x90);
    CHECK_EQ(rd(MC_RBV_IFR), 0x80 | MC_RBV_IRQ_ASC);
    CHECK(irq());
    /* The flag follows its source's line; writing 1s to IFR clears flags
     * ($90, ROM offset $A05A). */
    mc_rbv_set_irq(&rbv, MC_RBV_IRQ_ASC, 0);
    wr(MC_RBV_IFR, 0x90);
    CHECK_EQ(rd(MC_RBV_IFR), 0);
    CHECK(!irq());
    /* IER: bit 7 clear clears the bits written ($10, ROM offset $A044). */
    wr(MC_RBV_IER, 0x88);
    wr(MC_RBV_IER, 0x10);
    CHECK_EQ(rd(MC_RBV_IER), 0x88);
    mc_rbv_set_irq(&rbv, MC_RBV_IRQ_SCSI, 1);
    CHECK(irq());
    CHECK_EQ(rd(MC_RBV_IFR), 0x80 | MC_RBV_IRQ_SCSI);
    mc_rbv_set_irq(&rbv, MC_RBV_IRQ_SCSI, 0);
    wr(MC_RBV_IFR, MC_RBV_IRQ_SCSI);
    CHECK(!irq());
    /* The register block repeats: the shared VIA2 code reads IFR at
     * +$1A03 and IER at +$1C13 (ROM offset $9BE0). */
    CHECK_EQ((uint8_t)mc_rbv_ops.read(&rbv, 0x1C13, 1), 0x88);
}

static void test_slots(void) {
    setup(MC_RBV_SENSE_NONE);
    /* The slot lines, active low; bit 7 reads high. */
    CHECK_EQ(rd(MC_RBV_SIFR), 0xFF);
    wr(MC_RBV_SIER, 0xFF);                 /* the ROM's early setting ($2F10) */
    CHECK_EQ(rd(MC_RBV_SIER), 0xFF);
    wr(MC_RBV_IER, 0x82);
    mc_rbv_set_slots(&rbv, 0x10);          /* slot $D */
    CHECK_EQ(rd(MC_RBV_SIFR), 0xEF);
    CHECK_EQ(rd(MC_RBV_IFR), 0x80 | MC_RBV_IRQ_SLOT);
    CHECK(irq());
    /* The slot handler (ROM offset $6EAA) clears flag bit 1, then reads the
     * lines and the enables: the flag follows the enabled lines. */
    wr(MC_RBV_IFR, 0x82);
    CHECK(irq());
    wr(MC_RBV_SIER, 0x10);                 /* bit 7 clear: slot $D masked */
    CHECK_EQ(rd(MC_RBV_SIER), 0xEF);
    CHECK_EQ(rd(MC_RBV_IFR), 0);
    CHECK(!irq());
    wr(MC_RBV_SIER, 0x90);
    CHECK(irq());
    mc_rbv_set_slots(&rbv, 0);
    CHECK(!irq());
    CHECK_EQ(rd(MC_RBV_SIFR), 0xFF);
}

static void test_port_b(void) {
    setup(MC_RBV_SENSE_NONE);
    /* The ROM's early value ($2F06): outputs read back, inputs as driven. */
    wr(MC_RBV_DATAB, 0x8F);
    mc_rbv_set_port_b(&rbv, 0x40, 0x70);   /* no plug in the sound jack */
    CHECK_EQ(rd(MC_RBV_DATAB), 0xCF);
    CHECK_EQ(hw_fake_value(&f, &rbv, MC_RBV_LINE_PB), 0x8F);
    /* The ROM's ShutDown sets bit 2, v2PowerOff, then clears it ($40809E2C):
     * the board sees each write. */
    wr(MC_RBV_DATAB, 0x00);
    CHECK_EQ(rd(MC_RBV_DATAB), 0x40);
    CHECK_EQ(hw_fake_value(&f, &rbv, MC_RBV_LINE_PB), 0x00);
}

/* The video's line through each vertical blanking interval (Tables 12-5 and
 * 12-6, 640 x 480: 525 lines of 864 dots at 30.24 MHz, 45 of them blank),
 * and the 60.15 Hz tick. */
static void test_vbl(void) {
    setup(MC_RBV_SENSE_HIRES);
    const uint64_t frame = (uint64_t)MC_C15M_HZ * 864u * 525u / 30240000u;
    const uint64_t blank = (uint64_t)MC_C15M_HZ * 864u * 45u / 30240000u;
    CHECK_EQ(frame, 235008u);              /* 15.00 ms */
    wr(MC_RBV_SIER, 0xC0);
    wr(MC_RBV_IER, 0x82);
    /* A frame starts at power-up: the line is asserted, then released. */
    hw_fake_step(&f, 1);
    CHECK_EQ(rd(MC_RBV_SIFR) & 0x40, 0);
    CHECK(irq());
    hw_fake_advance(&f, blank + 1);
    CHECK_EQ(rd(MC_RBV_SIFR) & 0x40, 0x40);
    CHECK(!irq());
    hw_fake_advance(&f, frame - 1);
    CHECK(!irq());
    hw_fake_advance(&f, frame + 1);
    CHECK(irq());
    CHECK_EQ(hw_fake_changes(&f, &rbv, MC_RBV_LINE_IRQ), 3u);
    /* The driver masks the video's interrupt in RvSIER ($7F9C8). */
    wr(MC_RBV_SIER, 0x40);
    CHECK(!irq());
    /* The tick: 704 x 370 C15M periods per cycle. */
    const unsigned before = hw_fake_changes(&f, &rbv, MC_RBV_LINE_TICK);
    hw_fake_advance(&f, f.now + 10u * 704u * 370u);
    CHECK_EQ(hw_fake_changes(&f, &rbv, MC_RBV_LINE_TICK) - before, 20u);
    /* No monitor: no blanking, the line stays released. */
    setup(MC_RBV_SENSE_NONE);
    wr(MC_RBV_SIER, 0xC0);
    wr(MC_RBV_IER, 0x82);
    hw_fake_advance(&f, 10u * frame);
    CHECK_EQ(rd(MC_RBV_SIFR), 0xFF);
    CHECK(!irq());
}

static void test_video(void) {
    static uint8_t bank_a[640 * 480];
    setup(MC_RBV_SENSE_HIRES);
    mc_rbv_view v;
    /* The sense code in bits 5-3, the video off at reset (bit 6). */
    CHECK_EQ(rd(MC_RBV_MONP), 0x70);
    CHECK(!mc_rbv_view_get(&rbv, bank_a, sizeof bank_a, &v));
    /* The ROM's PrimaryInit turns it on in 1 bit and sets white and black
     * at CLUT entries $FE and $FF through the Bt478 (stride 4: address +0,
     * data +4, pixel mask +8). */
    wr(MC_RBV_MONP, 0x00);
    CHECK_EQ(rd(MC_RBV_MONP), 0x30);
    mc_rbv_vdac_ops.write(&rbv, 8, 1, 0xFF);
    mc_rbv_vdac_ops.write(&rbv, 0, 1, 0xFE);
    for (int i = 0; i < 3; i++) mc_rbv_vdac_ops.write(&rbv, 4, 1, 0xFF);
    for (int i = 0; i < 3; i++) mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x00);
    CHECK_EQ(mc_rbv_vdac_ops.read(&rbv, 0, 1), 0x00);    /* the address went on to $00 */
    CHECK(mc_rbv_view_get(&rbv, bank_a, sizeof bank_a, &v));
    CHECK(v.width == 640 && v.height == 480 && v.depth == 1 && v.row_bytes == 80 && v.pixels == bank_a);
    CHECK_EQ(v.palette[0], 0xFFFFFFFFu);
    CHECK_EQ(v.palette[1], 0xFF000000u);
    /* The driver's depth: mode - $80 in bits 1-0 ($7F908). 8 bits: the
     * CLUT as it is. */
    wr(MC_RBV_MONP, 0x03);
    mc_rbv_vdac_ops.write(&rbv, 0, 1, 0x05);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x12);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x34);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x56);
    CHECK(mc_rbv_view_get(&rbv, bank_a, sizeof bank_a, &v));
    CHECK(v.depth == 8 && v.row_bytes == 640);
    CHECK_EQ(v.palette[5], 0xFF123456u);
    /* The read address: three reads per entry. */
    mc_rbv_vdac_ops.write(&rbv, 12, 1, 0x05);
    CHECK_EQ(mc_rbv_vdac_ops.read(&rbv, 4, 1), 0x12);
    CHECK_EQ(mc_rbv_vdac_ops.read(&rbv, 4, 1), 0x34);
    CHECK_EQ(mc_rbv_vdac_ops.read(&rbv, 4, 1), 0x56);
    CHECK_EQ(mc_rbv_vdac_ops.read(&rbv, 12, 1), 0x06);
    /* 4 bits: entries $F0-$FF. */
    wr(MC_RBV_MONP, 0x02);
    mc_rbv_vdac_ops.write(&rbv, 0, 1, 0xF3);
    for (int i = 0; i < 3; i++) mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x80);
    CHECK(mc_rbv_view_get(&rbv, bank_a, sizeof bank_a, &v));
    CHECK(v.depth == 4 && v.row_bytes == 320);
    CHECK_EQ(v.palette[3], 0xFF808080u);
    /* Video off. */
    wr(MC_RBV_MONP, 0x40);
    CHECK(!mc_rbv_view_get(&rbv, bank_a, sizeof bank_a, &v));
    /* The other monitors: 512 x 384 to 8 bits, the portrait display to 4
     * bits, grays from the CLUT's blue output. */
    CHECK_EQ(mc_rbv_sense_for(512, 384), MC_RBV_SENSE_RGB12);
    CHECK_EQ(mc_rbv_max_depth(MC_RBV_SENSE_RGB12), 8u);
    CHECK_EQ(mc_rbv_sense_for(640, 870), MC_RBV_SENSE_PORTRAIT);
    CHECK_EQ(mc_rbv_max_depth(MC_RBV_SENSE_PORTRAIT), 4u);
    CHECK_EQ(mc_rbv_sense_for(1152, 870), MC_RBV_SENSE_NONE);
    setup(MC_RBV_SENSE_PORTRAIT);
    CHECK_EQ(rd(MC_RBV_MONP), 0x48);
    wr(MC_RBV_MONP, 0x00);
    mc_rbv_vdac_ops.write(&rbv, 0, 1, 0xFE);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x00);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0x00);
    mc_rbv_vdac_ops.write(&rbv, 4, 1, 0xFF);
    static uint8_t tall[80 * 870];
    CHECK(mc_rbv_view_get(&rbv, tall, sizeof tall, &v));
    CHECK(v.width == 640 && v.height == 870 && v.row_bytes == 80);
    CHECK_EQ(v.palette[0], 0xFFFFFFFFu);
    CHECK(!mc_rbv_view_get(&rbv, tall, 1000, &v));       /* the buffer must fit in bank A */
}

/* --- the 5380 on the IIci board --------------------------------------------- */

#define SCSI 0x50F10000u
#define PDMA 0x50F06000u
#define RBV 0x50F26000u

static mc_bus *bus;
static uint8_t n80(unsigned r) {
    uint32_t v = 0;
    mc_bus_read(bus, SCSI + (r << 4), 1, &v);
    return (uint8_t)v;
}
static void n80w(unsigned r, uint8_t v) { mc_bus_write(bus, SCSI + (r << 4), 1, v); }
static uint8_t rbv_flags(void) {
    uint32_t v = 0;
    mc_bus_read(bus, RBV + MC_RBV_IFR, 1, &v);
    return (uint8_t)v;
}
static void pio_out(uint8_t b) {
    n80w(MC_N80_CSD, b);
    n80w(MC_N80_ICR, MC_N80_ICR_DATA);
    n80w(MC_N80_ICR, MC_N80_ICR_DATA | MC_N80_ICR_ACK);
    n80w(MC_N80_ICR, MC_N80_ICR_DATA);
    n80w(MC_N80_ICR, 0);
}
static uint8_t pio_in(void) {
    const uint8_t b = n80(MC_N80_CSD);
    n80w(MC_N80_ICR, MC_N80_ICR_ACK);
    n80w(MC_N80_ICR, 0);
    return b;
}

static void test_ncr5380_wiring(void) {
    uint8_t *rom = calloc(1, 512u << 10);
    if (!rom) return;
    rom[0] = 0x36; rom[1] = 0x8C; rom[2] = 0xAD; rom[3] = 0xFE;
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find("iici"));
    c.ram_mb = 8;
    c.width = 640; c.height = 480; c.depth = 8;
    c.disk_count = 0;
    c.nvram[0] = 0;
    char error[256] = "";
    mc_machine *m = mc_machine_create(&c, rom, 512u << 10, error, sizeof error);
    CHECK(m != NULL);
    if (!m) {
        free(rom);
        return;
    }
    bus = &mc_machine_hw(m)->bus;
    /* Arbitration from ID 7, selection of the CD-ROM drive at ID 3. */
    n80w(MC_N80_CSD, 0x80);
    n80w(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    CHECK(n80(MC_N80_ICR) & MC_N80_ICR_AIP);
    n80w(MC_N80_ICR, MC_N80_ICR_SEL | MC_N80_ICR_BSY);
    n80w(MC_N80_CSD, 0x88);
    n80w(MC_N80_ICR, MC_N80_ICR_SEL | MC_N80_ICR_DATA);
    n80w(MC_N80_MODE, 0);
    CHECK(n80(MC_N80_CSBS) & MC_N80_CSBS_BSY);
    n80w(MC_N80_ICR, 0);
    /* INQUIRY, 36 bytes. */
    n80w(MC_N80_TCR, MC_SCSI_COMMAND);
    static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
    for (int i = 0; i < 6; i++) pio_out(inquiry[i]);
    /* DATA IN by pseudo-DMA: DRQ in the RBV's flag bit 0. */
    CHECK_EQ(rbv_flags() & MC_RBV_IRQ_SCSI_DRQ, 0);
    n80w(MC_N80_TCR, MC_SCSI_DATA_IN);
    n80w(MC_N80_MODE, MC_N80_MODE_DMA);
    n80w(MC_N80_RESET, 0);                  /* Start DMA Initiator Receive */
    CHECK(n80(MC_N80_BSR) & MC_N80_BSR_DRQ);
    CHECK_EQ(rbv_flags() & MC_RBV_IRQ_SCSI_DRQ, MC_RBV_IRQ_SCSI_DRQ);
    uint8_t data[36];
    for (int i = 0; i < 36; i += 4) {
        uint32_t v = 0;
        CHECK(mc_bus_read(bus, PDMA, 4, &v) == 0);
        data[i] = (uint8_t)(v >> 24); data[i + 1] = (uint8_t)(v >> 16); data[i + 2] = (uint8_t)(v >> 8); data[i + 3] = (uint8_t)v;
    }
    CHECK_EQ(data[0], 0x05);                /* a CD-ROM device */
    CHECK(!memcmp(data + 8, "LIVADIX ", 8));
    /* The phase changed to STATUS: DRQ gone, the phase-mismatch interrupt
     * in the RBV's flag bit 3. */
    CHECK_EQ(rbv_flags() & MC_RBV_IRQ_SCSI_DRQ, 0);
    CHECK_EQ(rbv_flags() & MC_RBV_IRQ_SCSI, MC_RBV_IRQ_SCSI);
    n80w(MC_N80_MODE, 0);
    (void)n80(MC_N80_RESET);                /* Reset Parity/Interrupt */
    CHECK_EQ(rbv_flags() & MC_RBV_IRQ_SCSI, 0);
    n80w(MC_N80_TCR, MC_SCSI_STATUS);
    CHECK_EQ(pio_in(), 0);
    n80w(MC_N80_TCR, MC_SCSI_MSG_IN);
    CHECK_EQ(pio_in(), 0);
    CHECK(!(n80(MC_N80_CSBS) & MC_N80_CSBS_BSY));
    mc_machine_destroy(m);
    free(rom);
}

int main(void) {
    test_interrupts();
    test_slots();
    test_port_b();
    test_vbl();
    test_video();
    test_ncr5380_wiring();
    printf("test_rbv: %d failure(s)\n", mc_test_failures);
    return mc_test_failures != 0;
}
