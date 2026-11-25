/* The NCR 5380 / 53C80 against its design manual (NCR SP-1051, March 1986):
 * the register set (6), arbitration (6.2, 6.3, 7), selection with and
 * without ATN, programmed I/O (10.1), pseudo-DMA in normal and block mode
 * (10.2-10.4), EOP (8.2, 10.5.1), the interrupts (8.1-8.6), the reset
 * conditions (9) and the target-role register rules (6.3, 6.4, 6.8), against
 * a SCSI disk target on a synthesized image. */
#include "scsi_util.h"
#include "ncr5380.h"
#include "scsi_target.h"

#include <string.h>

static hw_fake f;
static mc_scsi_bus bus;
static mc_ncr5380 chip;
static mc_scsi_unit disk;
static mc_disk *img;
static mc_vdisk *vd;
static const uint8_t driver[512] = {0x4E, 0x75};

static uint8_t rd(unsigned r) { return (uint8_t)mc_ncr5380_ops.read(&chip, r, 1); }
static void wr(unsigned r, uint8_t v) { mc_ncr5380_ops.write(&chip, r, 1, v); }
static int irq(void) { return hw_fake_line(&f, &chip, MC_NCR5380_LINE_IRQ); }
static int drq(void) { return hw_fake_line(&f, &chip, MC_NCR5380_LINE_DRQ); }
static int ready(void) { return hw_fake_line(&f, &chip, MC_NCR5380_LINE_READY); }

static void setup(void) {
    char error[200];
    hw_fake_init(&f);
    img = mc_disk_open(scsi_make_image("ncr5380", "disk.img", 256 * 512, 0x5A), 1, 512, error, sizeof error);
    CHECK(img != NULL);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    mc_scsi_bus_init(&bus);
    mc_scsi_disk_init(&disk, vd);
    mc_scsi_bus_attach(&bus, 0, mc_scsi_unit_target(&disk));
    mc_ncr5380_init(&chip, &f.host, &bus, 0);
}

/* Back to a quiet chip and bus. */
static void quiet(void) {
    mc_scsi_bus_release(&bus);
    mc_ncr5380_reset(&chip);
    chip.bad_parity = 0;
    chip.eop_pin = 0;
    chip.tgt_send = NULL;
    chip.tgt_recv = NULL;
    mc_ncr5380_ext_release(&chip);
    mc_ncr5380_ext_bsy(&chip, 0);
    mc_ncr5380_ext_signals(&chip, 0, 0);
    chip.c80 = 0;
}

/* ---- programmed I/O helpers (10.1) ---------------------------------------- */

static void phase(int p) { wr(MC_N80_TCR, (uint8_t)p); }
static int req(void) { return (rd(MC_N80_CSBS) & MC_N80_CSBS_REQ) != 0; }

/* Arbitration (6.3 bit 0) from ID 7, then selection of ID 0 (SEL, both ID
 * bits on the data lines, BSY released by clearing ARBITRATE). */
static int arbitrate(void) {
    wr(MC_N80_CSD, 0x80);
    wr(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    return (rd(MC_N80_ICR) & MC_N80_ICR_AIP) && !(rd(MC_N80_ICR) & MC_N80_ICR_LA);
}
static int select_target(int atn) {
    if (!arbitrate()) return 0;
    wr(MC_N80_ICR, MC_N80_ICR_SEL | MC_N80_ICR_BSY);
    wr(MC_N80_CSD, 0x81);
    wr(MC_N80_ICR, (uint8_t)(MC_N80_ICR_SEL | MC_N80_ICR_DATA | (atn ? MC_N80_ICR_ATN : 0)));
    wr(MC_N80_MODE, 0);
    int ok = (rd(MC_N80_CSBS) & MC_N80_CSBS_BSY) != 0;
    wr(MC_N80_ICR, atn ? MC_N80_ICR_ATN : 0);   /* SEL and the ID lines off */
    return ok;
}
static void pio_out(uint8_t b) {
    CHECK(req());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    uint8_t keep = rd(MC_N80_ICR) & MC_N80_ICR_ATN;
    wr(MC_N80_CSD, b);
    wr(MC_N80_ICR, (uint8_t)(MC_N80_ICR_DATA | keep));
    wr(MC_N80_ICR, (uint8_t)(MC_N80_ICR_DATA | MC_N80_ICR_ACK | keep));
    CHECK(!req());
    wr(MC_N80_ICR, (uint8_t)(MC_N80_ICR_DATA | keep));
    wr(MC_N80_ICR, keep);
}
static uint8_t pio_in(void) {
    CHECK(req());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    uint8_t b = rd(MC_N80_CSD);
    uint8_t keep = rd(MC_N80_ICR) & MC_N80_ICR_ATN;
    wr(MC_N80_ICR, (uint8_t)(MC_N80_ICR_ACK | keep));
    wr(MC_N80_ICR, keep);
    return b;
}
static void send_cdb(const uint8_t *cdb, unsigned n) {
    phase(MC_SCSI_COMMAND);
    CHECK_EQ(rd(MC_N80_CSBS) & 0x1C, MC_SCSI_COMMAND << 2);
    for (unsigned i = 0; i < n; i++) pio_out(cdb[i]);
}
/* Status and the message that frees the bus, by programmed I/O. */
static uint8_t finish_pio(void) {
    phase(MC_SCSI_STATUS);
    uint8_t st = pio_in();
    phase(MC_SCSI_MSG_IN);
    CHECK_EQ(pio_in(), 0);
    CHECK(!(rd(MC_N80_CSBS) & MC_N80_CSBS_BSY));
    phase(0);
    return st;
}
/* Selects with ATN, IDENTIFY, then the CDB. */
static void start_cmd(const uint8_t *cdb, unsigned n) {
    CHECK(select_target(1));
    phase(MC_SCSI_MSG_OUT);
    CHECK_EQ(rd(MC_N80_CSBS) & 0x1C, MC_SCSI_MSG_OUT << 2);
    wr(MC_N80_ICR, 0);          /* ATN off before the last message byte */
    pio_out(0x80);
    send_cdb(cdb, n);
}
static void start_rw(uint8_t op, uint32_t lba, uint8_t blocks) {
    lba += 128;     /* the image starts after the driver partition */
    const uint8_t cdb[6] = {op, (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba, blocks, 0};
    start_cmd(cdb, 6);
}

/* ---- register tests -------------------------------------------------------- */

/* Reset values and read-back (6.0-6.9, 9.1). */
static void test_registers(void) {
    quiet();
    CHECK_EQ(rd(MC_N80_ICR), 0);
    CHECK_EQ(rd(MC_N80_MODE), 0);
    CHECK_EQ(rd(MC_N80_TCR), 0);
    CHECK_EQ(rd(MC_N80_BSR), MC_N80_BSR_PHASE);       /* phase lines 000 match TCR 000 */
    CHECK_EQ(rd(MC_N80_CSBS), 0);                     /* all lines released, including parity */
    CHECK_EQ(rd(MC_N80_IDR), 0);
    CHECK(!irq() && !drq());
    /* Initiator Command: bits 4-0 and 7 read back; bit 6 reads AIP, bit 5 LA. */
    wr(MC_N80_ICR, 0x1F);
    CHECK_EQ(rd(MC_N80_ICR), 0x1F);
    wr(MC_N80_ICR, 0);
    /* The mode register reads back (DMA MODE needs BSY, tested below). */
    wr(MC_N80_MODE, 0xFD);
    CHECK_EQ(rd(MC_N80_MODE), 0xFD);
    wr(MC_N80_MODE, 0);
    rd(MC_N80_RESET);           /* MONITOR BUSY without BSY interrupted */
    /* Target Command: bits 3-0 (6.4); bit 7 only on the 53C80. */
    wr(MC_N80_TCR, 0xFF);
    CHECK_EQ(rd(MC_N80_TCR), 0x0F);
    wr(MC_N80_TCR, 0);
    /* The Output Data Register is write-only: Current SCSI Data reads the
     * bus, which carries the byte only when ASSERT DATA BUS is set and the
     * phase lines match (6.2 bit 0). */
    wr(MC_N80_CSD, 0xA5);
    CHECK_EQ(rd(MC_N80_CSD), 0);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    CHECK_EQ(rd(MC_N80_CSD), 0xA5);
    CHECK_EQ(rd(MC_N80_CSBS) & MC_N80_CSBS_DBP, MC_N80_CSBS_DBP);   /* 0xA5: four ones, odd parity needs DBP=1 */
    wr(MC_N80_CSD, 0x01);
    CHECK_EQ(rd(MC_N80_CSBS) & MC_N80_CSBS_DBP, 0);   /* one bit set: DBP 0 */
    wr(MC_N80_CSD, 0);
    CHECK_EQ(rd(MC_N80_CSBS) & MC_N80_CSBS_DBP, MC_N80_CSBS_DBP);   /* driven zero asserts parity */
    wr(MC_N80_TCR, 1);                                /* the chip must stay off the bus on a mismatch */
    CHECK_EQ(rd(MC_N80_CSD), 0);
    CHECK_EQ(rd(MC_N80_CSBS) & MC_N80_CSBS_DBP, 0);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PHASE));
    wr(MC_N80_TCR, 0);
    wr(MC_N80_ICR, MC_N80_ICR_ACK | MC_N80_ICR_ATN);
    CHECK_EQ(rd(MC_N80_BSR) & 3, 3);                  /* ACK and ATN read back in the Bus and Status register */
    wr(MC_N80_ICR, 0);
    /* Hardware reset clears everything, irq latch included (9.1). */
    wr(MC_N80_MODE, 0x3C);
    wr(MC_N80_CSBS, 0x80);
    wr(MC_N80_TCR, 5);
    mc_ncr5380_ext_select(&chip, 0x80, 0);
    CHECK(irq());
    mc_ncr5380_ops.reset(&chip);
    CHECK_EQ(rd(MC_N80_MODE), 0);
    CHECK_EQ(rd(MC_N80_TCR), 0);
    CHECK(!irq());
    CHECK_EQ(rd(MC_N80_BSR) & MC_N80_BSR_IRQ, 0);
}

/* The 53C80's Last Byte Sent flag is bit 7 of the Target Command register
 * (6.4); the 5380 reads it as 0. */
static void test_last_byte_flag(void) {
    quiet();
    CHECK_EQ(rd(MC_N80_TCR) & 0x80, 0);
    chip.c80 = 1;
    CHECK_EQ(rd(MC_N80_TCR) & 0x80, 0);
    chip.last_byte = 1;
    CHECK_EQ(rd(MC_N80_TCR) & 0x80, 0x80);
    chip.last_byte = 0;
    chip.c80 = 0;
    chip.last_byte = 1;
    CHECK_EQ(rd(MC_N80_TCR) & 0x80, 0);
    chip.last_byte = 0;
}

/* Arbitration (6.2 AIP and LA, 6.3 bit 0, 7). */
static void test_arbitration(void) {
    quiet();
    CHECK(arbitrate());
    CHECK_EQ(rd(MC_N80_CSD), 0x80);                   /* the ID is on the bus */
    CHECK(rd(MC_N80_CSBS) & MC_N80_CSBS_BSY);         /* BSY asserted by the chip */
    CHECK_EQ(bus.phase, MC_SCSI_ARBITRATION);
    CHECK(!(rd(MC_N80_ICR) & MC_N80_ICR_LA));
    wr(MC_N80_MODE, 0);                               /* AIP drops with ARBITRATE; the bus is let go */
    CHECK(!(rd(MC_N80_ICR) & MC_N80_ICR_AIP));
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
    CHECK(!(rd(MC_N80_CSBS) & MC_N80_CSBS_BSY));
    /* Arbitration waits for a free bus. */
    mc_ncr5380_ext_bsy(&chip, 1);
    wr(MC_N80_CSD, 0x40);
    wr(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    CHECK(!(rd(MC_N80_ICR) & MC_N80_ICR_AIP));
    mc_ncr5380_ext_bsy(&chip, 0);
    CHECK(rd(MC_N80_ICR) & MC_N80_ICR_AIP);
    CHECK_EQ(rd(MC_N80_CSD), 0x40);
    /* SEL from another device while arbitrating: lost arbitration. */
    mc_ncr5380_ext_select(&chip, 0x20, 0);
    CHECK(rd(MC_N80_ICR) & MC_N80_ICR_LA);
    CHECK(rd(MC_N80_ICR) & MC_N80_ICR_AIP);
    wr(MC_N80_MODE, 0);
    CHECK_EQ(rd(MC_N80_ICR) & (MC_N80_ICR_LA | MC_N80_ICR_AIP), 0);
    mc_ncr5380_ext_release(&chip);
    /* SEL already asserted: arbitration does not begin. */
    mc_ncr5380_ext_select(&chip, 0x20, 0);
    wr(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    CHECK(!(rd(MC_N80_ICR) & MC_N80_ICR_AIP));
    wr(MC_N80_MODE, 0);
    mc_ncr5380_ext_release(&chip);
    /* The other initiator on the bus (a connected target) keeps it busy. */
    CHECK(mc_scsi_bus_arbitrate(&bus, 5));
    wr(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    CHECK(!(rd(MC_N80_ICR) & MC_N80_ICR_AIP));
    mc_scsi_bus_release(&bus);
    CHECK(rd(MC_N80_ICR) & MC_N80_ICR_AIP);
    wr(MC_N80_MODE, 0);
}

/* ---- transactions ---------------------------------------------------------- */

/* Select without ATN, TEST UNIT READY by programmed I/O, status, message
 * in, bus free (6.2, 10.1). */
static void test_select_no_atn(void) {
    quiet();
    CHECK(select_target(0));
    CHECK_EQ(rd(MC_N80_CSBS) & 0x7C, MC_N80_CSBS_BSY | MC_N80_CSBS_REQ | (MC_SCSI_COMMAND << 2));
    const uint8_t tur[6] = {0};
    send_cdb(tur, 6);
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
    CHECK(!irq());
}

/* Select with ATN: MESSAGE OUT first, IDENTIFY, INQUIRY with data in by
 * programmed I/O (REQ polled, phase match checked, ACK toggled). */
static void test_select_atn_inquiry(void) {
    quiet();
    CHECK(select_target(1));
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_ATN);
    phase(MC_SCSI_MSG_OUT);
    CHECK_EQ(rd(MC_N80_CSBS) & 0x1C, MC_SCSI_MSG_OUT << 2);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    wr(MC_N80_ICR, 0);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_ATN));
    pio_out(0x80);
    CHECK_EQ(bus.phase, MC_SCSI_COMMAND);
    phase(MC_SCSI_DATA_IN);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PHASE));      /* the bus is in COMMAND */
    const uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    send_cdb(inq, 6);
    phase(MC_SCSI_DATA_IN);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    uint8_t data[36];
    for (int i = 0; i < 36; i++) data[i] = pio_in();
    CHECK(!memcmp(data + 8, "LIVADIX ", 8));
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
}

/* The ATN condition: a target in COMMAND with ATN asserted goes to MESSAGE
 * OUT after the byte (the chip's ATN reaches the bus, 6.2 bit 1). */
static void test_atn_to_bus(void) {
    quiet();
    CHECK(select_target(0));
    phase(MC_SCSI_COMMAND);
    wr(MC_N80_ICR, MC_N80_ICR_ATN);
    CHECK(bus.atn);
    pio_out(0x00);          /* first CDB byte */
    CHECK_EQ(bus.phase, MC_SCSI_MSG_OUT);
    phase(MC_SCSI_MSG_OUT);
    wr(MC_N80_ICR, 0);
    CHECK(!bus.atn);
    pio_out(MC_SCSI_MSG_ABORT);
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
}

/* A selection nobody answers: BSY never comes, the bus is free again. */
static void test_select_nobody(void) {
    quiet();
    CHECK(arbitrate());
    wr(MC_N80_CSD, 0x84);   /* ID 2 not attached */
    wr(MC_N80_ICR, MC_N80_ICR_SEL | MC_N80_ICR_DATA);
    wr(MC_N80_MODE, 0);
    CHECK(!(rd(MC_N80_CSBS) & MC_N80_CSBS_BSY));
    CHECK(rd(MC_N80_CSBS) & MC_N80_CSBS_SEL);
    wr(MC_N80_ICR, 0);
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
}

/* ---- DMA ------------------------------------------------------------------- */

static void check_block(const uint8_t *p, unsigned blk) {
    CHECK_EQ(p[0], 0); CHECK_EQ(p[1], 0); CHECK_EQ(p[2], blk >> 8); CHECK_EQ(p[3], blk & 0xFF);
    CHECK_EQ(p[4], 0x5A);
    CHECK_EQ(p[5], 5 ^ 0x5A);
    CHECK_EQ(p[511], (511 & 0xFF) ^ 0x5A);
}

/* READ(6) of 4 blocks by pseudo-DMA: DMA MODE, Start DMA Initiator Receive,
 * DRQ, long-word DACK cycles, then the phase mismatch interrupt (8.5,
 * 10.4, 10.5.2). */
static void test_dma_read(void) {
    quiet();
    start_rw(0x08, 10, 4);
    phase(MC_SCSI_DATA_IN);
    /* Not started yet: no DRQ, the window refuses. */
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    CHECK(rd(MC_N80_MODE) & MC_N80_MODE_DMA);
    CHECK(!drq());
    uint32_t v;
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 0);
    wr(MC_N80_RESET, 0);                              /* Start DMA Initiator Receive */
    CHECK(drq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_DRQ);
    CHECK(!irq());
    uint8_t buf[2048];
    for (int i = 0; i < 2048; i += 4) {
        CHECK(drq());
        CHECK_EQ(mc_ncr5380_dma_read(&chip, 4, &v), 4);
        buf[i] = (uint8_t)(v >> 24); buf[i + 1] = (uint8_t)(v >> 16); buf[i + 2] = (uint8_t)(v >> 8); buf[i + 3] = (uint8_t)v;
    }
    for (unsigned b = 0; b < 4; b++) check_block(buf + 512 * b, 10 + b);
    /* The target went to STATUS: REQ with a phase mismatch interrupts and
     * DRQ stays false; the window refuses. */
    CHECK(!drq());
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_IRQ);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PHASE));
    CHECK_EQ(rd(MC_N80_CSBS) & 0x1C, MC_SCSI_STATUS << 2);
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 0);
    wr(MC_N80_MODE, 0);                               /* DMA MODE reset (10.5.3) */
    rd(MC_N80_RESET);
    CHECK(!irq());
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
}

/* Block mode: READY follows the chip's readiness, the bytes are the same. */
static void test_block_mode(void) {
    quiet();
    start_rw(0x08, 3, 1);
    phase(MC_SCSI_DATA_IN);
    wr(MC_N80_MODE, MC_N80_MODE_DMA | MC_N80_MODE_BLOCK);
    CHECK(!ready());
    wr(MC_N80_RESET, 0);
    CHECK(drq() && ready());
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) {
        uint32_t v;
        CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 1);
        buf[i] = (uint8_t)v;
    }
    check_block(buf, 3);
    CHECK(!ready() && !drq());
    wr(MC_N80_MODE, 0);
    rd(MC_N80_RESET);
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
    CHECK(!ready());
}

/* WRITE(6) of 2 blocks by pseudo-DMA: ASSERT DATA BUS, Start DMA Send,
 * long-word DACK cycles, phase mismatch at the status phase; the output
 * register then empty, DRQ asks for a byte the status phase does not take
 * (†, ncr5380.c). */
static void test_dma_write(void) {
    quiet();
    start_rw(0x0A, 40, 2);
    phase(MC_SCSI_DATA_OUT);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    CHECK(!drq());
    CHECK_EQ(mc_ncr5380_dma_write(&chip, 1, 0), 0);
    wr(MC_N80_BSR, 0);                                /* Start DMA Send */
    CHECK(drq());
    for (int i = 0; i < 1024; i += 4)
        CHECK_EQ(mc_ncr5380_dma_write(&chip, 4, 0xC0000000u | (uint32_t)i), 4);
    CHECK(drq());
    CHECK(irq());
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PHASE));
    CHECK_EQ(mc_ncr5380_dma_write(&chip, 2, 0), 1);    /* held, not sent */
    CHECK(!drq());
    wr(MC_N80_MODE, 0);
    wr(MC_N80_ICR, 0);
    rd(MC_N80_RESET);
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
    uint8_t back[512];
    CHECK(mc_disk_read(img, 40, 1, back));
    CHECK_EQ(back[0], 0xC0); CHECK_EQ(back[1], 0x00); CHECK_EQ(back[2], 0x00); CHECK_EQ(back[3], 0x00);
    CHECK_EQ(back[4], 0xC0); CHECK_EQ(back[5], 0x00); CHECK_EQ(back[6], 0x00); CHECK_EQ(back[7], 0x04);
    CHECK(mc_disk_read(img, 41, 1, back));
    CHECK_EQ(back[0], 0xC0); CHECK_EQ(back[1], 0x00); CHECK_EQ(back[2], 0x02); CHECK_EQ(back[3], 0x00);
}

/* A short pseudo-DMA access: REQUEST SENSE is 18 bytes, the fifth long word
 * gets two. */
static void test_dma_short(void) {
    quiet();
    const uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    start_cmd(cdb, 6);
    phase(MC_SCSI_DATA_IN);
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    wr(MC_N80_RESET, 0);
    uint32_t v;
    for (int i = 0; i < 4; i++) CHECK_EQ(mc_ncr5380_dma_read(&chip, 4, &v), 4);
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 4, &v), 2);
    CHECK(irq());
    wr(MC_N80_MODE, 0);
    rd(MC_N80_RESET);
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
}

/* EOP (8.2, 10.5.1): the byte of that cycle moves, END OF DMA sets, the
 * interrupt needs ENABLE EOP INTERRUPT, no further byte is requested, DMA
 * MODE stays set and END OF DMA resets with it. */
static void test_eop(void) {
    quiet();
    start_rw(0x08, 7, 1);
    phase(MC_SCSI_DATA_IN);
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    wr(MC_N80_RESET, 0);
    uint32_t v;
    for (int i = 0; i < 10; i++) CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 1);
    mc_ncr5380_set_eop(&chip, 1);
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 1);
    mc_ncr5380_set_eop(&chip, 0);
    CHECK_EQ(v, 10 ^ 0x5A);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_EOD);
    CHECK(!irq());                                    /* interrupt not enabled */
    CHECK(!drq());
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 0);
    CHECK(rd(MC_N80_MODE) & MC_N80_MODE_DMA);
    wr(MC_N80_RESET, 0);                              /* DMA restarts with a new Start DMA register write */
    CHECK(drq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_EOD);
    wr(MC_N80_MODE, MC_N80_MODE_DMA | MC_N80_MODE_EOPINT);
    mc_ncr5380_set_eop(&chip, 1);
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 1);
    mc_ncr5380_set_eop(&chip, 0);
    CHECK(irq());
    CHECK_EQ(v, 11 ^ 0x5A);
    wr(MC_N80_MODE, 0);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_EOD));
    rd(MC_N80_RESET);
    CHECK(!irq());
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 0);
    /* Finish the command by hand: drain the rest and the phases. */
    phase(MC_SCSI_DATA_IN);
    for (int i = 12; i < 512; i++) pio_in();
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
}

/* The 53C80's Last Byte Sent after an EOP-ended send (6.4). */
static void test_dma_send_eop_c80(void) {
    quiet();
    chip.c80 = 1;
    start_rw(0x0A, 60, 1);
    phase(MC_SCSI_DATA_OUT);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    wr(MC_N80_BSR, 0);
    for (int i = 0; i < 511; i++) CHECK_EQ(mc_ncr5380_dma_write(&chip, 1, (uint32_t)i), 1);
    CHECK(!(rd(MC_N80_TCR) & 0x80));
    mc_ncr5380_set_eop(&chip, 1);
    CHECK_EQ(mc_ncr5380_dma_write(&chip, 1, 0x77), 1);
    mc_ncr5380_set_eop(&chip, 0);
    CHECK(rd(MC_N80_TCR) & 0x80);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_EOD);
    wr(MC_N80_MODE, 0);
    CHECK(!(rd(MC_N80_TCR) & 0x80));
    wr(MC_N80_ICR, 0);
    rd(MC_N80_RESET);
    CHECK_EQ(finish_pio(), MC_SCSI_GOOD);
    uint8_t back[512];
    CHECK(mc_disk_read(img, 60, 1, back));
    CHECK_EQ(back[511], 0x77);
    CHECK_EQ(back[100], 100);
}

/* ---- interrupts ------------------------------------------------------------ */

/* A phase mismatch without DMA MODE interrupts nothing (8.5); DMA MODE
 * cannot be set while BSY is false (6.3 bit 1). */
static void test_dma_mode_rules(void) {
    quiet();
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    CHECK(!(rd(MC_N80_MODE) & MC_N80_MODE_DMA));
    wr(MC_N80_RESET, 0);
    wr(MC_N80_BSR, 0);
    CHECK(!drq());
    CHECK(select_target(0));
    phase(MC_SCSI_DATA_IN);                           /* mismatch while the target is in COMMAND */
    CHECK(!irq());
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    CHECK(rd(MC_N80_MODE) & MC_N80_MODE_DMA);
    wr(MC_N80_RESET, 0);                              /* receive started in the wrong phase: no DRQ, no recognised REQ */
    CHECK(!drq());
    CHECK(!irq());                                    /* no REQ edge while the mismatch stands */
    /* An ACK cycle in programmed I/O with DMA MODE set: REQ comes back and
     * the new phase does not match: the interrupt (8.5). */
    phase(MC_SCSI_COMMAND);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    wr(MC_N80_CSD, 0);
    wr(MC_N80_ICR, MC_N80_ICR_DATA | MC_N80_ICR_ACK);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    CHECK(!irq());                                    /* same phase: match */
    wr(MC_N80_MODE, 0);
    wr(MC_N80_ICR, 0);
    rd(MC_N80_RESET);
}

/* Parity (6.1.1, 6.3 bits 5-4, 6.7 bit 5, 8.1, 8.4). */
static void test_parity(void) {
    quiet();
    const uint8_t inq[6] = {0x12, 0, 0, 0, 8, 0};
    start_cmd(inq, 6);
    phase(MC_SCSI_DATA_IN);
    chip.bad_parity = 1;
    /* Not checked: the bit stays clear. */
    rd(MC_N80_CSD);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PARITY));
    /* Checked without the interrupt: the latch only. */
    wr(MC_N80_MODE, MC_N80_MODE_PARCHK);
    rd(MC_N80_CSD);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PARITY);
    CHECK(!irq());
    rd(MC_N80_RESET);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_PARITY));
    /* Checked with the interrupt. */
    wr(MC_N80_MODE, MC_N80_MODE_PARCHK | MC_N80_MODE_PARINT);
    rd(MC_N80_CSD);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PARITY);
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_IRQ);
    rd(MC_N80_RESET);
    CHECK(!irq() && !(rd(MC_N80_BSR) & MC_N80_BSR_PARITY));
    /* DMA receive: checked when the byte is latched. */
    wr(MC_N80_MODE, MC_N80_MODE_PARCHK | MC_N80_MODE_PARINT | MC_N80_MODE_DMA);
    wr(MC_N80_RESET, 0);
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PARITY);
    chip.bad_parity = 0;
    wr(MC_N80_MODE, 0);
    rd(MC_N80_RESET);
    mc_scsi_bus_release(&bus);
}

/* Selection and reselection by another device (8.1): SEL, an ID bit in the
 * Select Enable Register and BSY false; I/O marks a reselection; no
 * interrupt without the enable bit; a parity error shows without the parity
 * interrupt. */
static void test_selected(void) {
    quiet();
    wr(MC_N80_CSBS, 0x80);
    mc_ncr5380_ext_select(&chip, 0x41, 0);
    CHECK(!irq());                                    /* not our ID */
    mc_ncr5380_ext_release(&chip);
    mc_ncr5380_ext_select(&chip, 0x82, 0);
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_IRQ);
    CHECK_EQ(rd(MC_N80_CSBS) & (MC_N80_CSBS_SEL | MC_N80_CSBS_BSY | MC_N80_CSBS_IO), MC_N80_CSBS_SEL);
    CHECK_EQ(rd(MC_N80_CSD), 0x82);
    rd(MC_N80_RESET);
    CHECK(!irq());                                    /* edge: the standing SEL does not interrupt again */
    CHECK(!irq());
    mc_ncr5380_ext_release(&chip);
    mc_ncr5380_ext_select(&chip, 0x81, 1);            /* reselection */
    CHECK(irq());
    CHECK(rd(MC_N80_CSBS) & MC_N80_CSBS_IO);
    rd(MC_N80_RESET);
    mc_ncr5380_ext_release(&chip);
    /* BSY true: not a selection. */
    mc_ncr5380_ext_bsy(&chip, 1);
    mc_ncr5380_ext_select(&chip, 0x80, 0);
    CHECK(!irq());
    mc_ncr5380_ext_release(&chip);
    mc_ncr5380_ext_bsy(&chip, 0);
    /* Parity error during selection with parity checking, no PARITY INTERRUPT. */
    wr(MC_N80_MODE, MC_N80_MODE_PARCHK);
    chip.bad_parity = 1;
    mc_ncr5380_ext_select(&chip, 0x80, 0);
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PARITY);
    rd(MC_N80_RESET);
    mc_ncr5380_ext_release(&chip);
    chip.bad_parity = 0;
    /* All Select Enable bits clear: disabled. */
    wr(MC_N80_CSBS, 0);
    mc_ncr5380_ext_select(&chip, 0x80, 0);
    CHECK(!irq());
    mc_ncr5380_ext_release(&chip);
}

/* Loss of BSY (6.3 bit 2, 6.7 bit 2, 8.6): the target goes BUS FREE after
 * COMMAND COMPLETE; the interrupt, BUSY ERROR, the Initiator Command bits
 * cleared, DMA MODE reset. */
static void test_busy_loss(void) {
    quiet();
    const uint8_t tur[6] = {0};
    CHECK(select_target(0));
    send_cdb(tur, 6);
    wr(MC_N80_MODE, MC_N80_MODE_MONBSY);
    CHECK(!irq());
    phase(MC_SCSI_STATUS);
    CHECK_EQ(pio_in(), MC_SCSI_GOOD);
    phase(MC_SCSI_MSG_IN);
    wr(MC_N80_MODE, MC_N80_MODE_MONBSY | MC_N80_MODE_DMA);
    CHECK_EQ(rd(MC_N80_MODE), MC_N80_MODE_MONBSY | MC_N80_MODE_DMA);
    wr(MC_N80_ICR, MC_N80_ICR_ACK);                   /* ACK held on the last message byte */
    CHECK(!irq());
    wr(MC_N80_ICR, MC_N80_ICR_DATA);                  /* ACK released: the target lets go of BSY */
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_BUSYERR);
    CHECK_EQ(rd(MC_N80_ICR) & 0x1F, 0);
    CHECK(!(rd(MC_N80_MODE) & MC_N80_MODE_DMA));
    CHECK(rd(MC_N80_MODE) & MC_N80_MODE_MONBSY);
    rd(MC_N80_RESET);
    CHECK(!irq() && !(rd(MC_N80_BSR) & MC_N80_BSR_BUSYERR));
    wr(MC_N80_MODE, 0);
}

/* Bus reset issued (9.3) and received (9.2, 8.3): interrupt that cannot be
 * disabled, registers cleared except the interrupt latch and ASSERT RST,
 * the targets reset, the bus is free; deasserting releases RST. */
static void test_reset_conditions(void) {
    quiet();
    CHECK(select_target(0));
    wr(MC_N80_MODE, MC_N80_MODE_MONBSY | MC_N80_MODE_PARCHK);
    wr(MC_N80_TCR, 3);
    wr(MC_N80_CSBS, 0x80);
    wr(MC_N80_ICR, MC_N80_ICR_RST | MC_N80_ICR_DATA);
    CHECK(irq());
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_IRQ);
    CHECK_EQ(rd(MC_N80_ICR), MC_N80_ICR_RST);
    CHECK_EQ(rd(MC_N80_MODE), 0);
    CHECK_EQ(rd(MC_N80_TCR), 0);
    CHECK(rd(MC_N80_CSBS) & MC_N80_CSBS_RST);
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
    CHECK(!(rd(MC_N80_BSR) & MC_N80_BSR_BUSYERR));
    /* The interrupt latch survives a further reset-class write, clears on read. */
    wr(MC_N80_MODE, 0);
    CHECK(irq());
    rd(MC_N80_RESET);
    CHECK(!irq());
    CHECK(rd(MC_N80_CSBS) & MC_N80_CSBS_RST);         /* RST stays asserted until ASSERT RST drops */
    wr(MC_N80_ICR, 0);
    CHECK(!(rd(MC_N80_CSBS) & MC_N80_CSBS_RST));
    CHECK(!irq());
    /* RST received from another device. */
    CHECK(select_target(0));
    wr(MC_N80_MODE, MC_N80_MODE_ARBITRATE);
    mc_ncr5380_ext_reset(&chip);
    CHECK(irq());
    CHECK_EQ(rd(MC_N80_MODE), 0);
    CHECK_EQ(bus.phase, MC_SCSI_BUS_FREE);
    rd(MC_N80_RESET);
    CHECK(!irq());
}

/* ---- target role ------------------------------------------------------------ */

static uint8_t tgt_log[8];
static unsigned tgt_n;
static const uint8_t tgt_feed[4] = {0x11, 0x22, 0x33, 0x44};
static unsigned tgt_feed_n;
static void on_send(void *ctx, uint8_t b) { (void)ctx; if (tgt_n < 8) tgt_log[tgt_n++] = b; }
static int on_recv(void *ctx, uint8_t *b) { (void)ctx; if (tgt_feed_n >= 4) return 0; *b = tgt_feed[tgt_feed_n++]; return 1; }

/* TARGETMODE (6.3 bit 6, 6.4, 6.8.2): the phase and REQ come from the
 * Target Command Register, ATN and ACK are not driven by the chip (they
 * show what the initiator drives), the data lines follow ASSERT DATA BUS,
 * Start DMA Target Receive needs the mode and Start DMA Initiator Receive
 * refuses it. */
static void test_target_mode(void) {
    quiet();
    wr(MC_N80_MODE, MC_N80_MODE_TARGET);
    wr(MC_N80_TCR, MC_N80_TCR_REQ | 1);
    CHECK_EQ(rd(MC_N80_CSBS) & (MC_N80_CSBS_REQ | MC_N80_CSBS_IO | MC_N80_CSBS_CD | MC_N80_CSBS_MSG),
             MC_N80_CSBS_REQ | MC_N80_CSBS_IO);
    CHECK(rd(MC_N80_BSR) & MC_N80_BSR_PHASE);
    wr(MC_N80_TCR, MC_SCSI_MSG_IN);
    CHECK_EQ(rd(MC_N80_CSBS) & 0x3C, 0x1C);
    wr(MC_N80_ICR, MC_N80_ICR_ATN | MC_N80_ICR_ACK);
    CHECK_EQ(rd(MC_N80_BSR) & 3, 0);                  /* initiator-only outputs */
    mc_ncr5380_ext_signals(&chip, 1, 0);
    CHECK_EQ(rd(MC_N80_BSR) & 3, 2);
    mc_ncr5380_ext_signals(&chip, 1, 1);
    CHECK_EQ(rd(MC_N80_BSR) & 3, 3);
    mc_ncr5380_ext_signals(&chip, 0, 0);
    wr(MC_N80_ICR, MC_N80_ICR_DATA);
    wr(MC_N80_CSD, 0x3C);
    CHECK_EQ(rd(MC_N80_CSD), 0x3C);                   /* driven as a target whatever the phase lines */
    /* DMA: Start Initiator Receive is refused in target mode. */
    wr(MC_N80_MODE, MC_N80_MODE_TARGET | MC_N80_MODE_DMA);
    CHECK(!(rd(MC_N80_MODE) & MC_N80_MODE_DMA));      /* BSY is not asserted */
    wr(MC_N80_ICR, MC_N80_ICR_BSY | MC_N80_ICR_DATA);
    wr(MC_N80_MODE, MC_N80_MODE_TARGET | MC_N80_MODE_DMA);
    CHECK(rd(MC_N80_MODE) & MC_N80_MODE_DMA);
    chip.tgt_send = on_send;
    chip.tgt_recv = on_recv;
    tgt_n = tgt_feed_n = 0;
    wr(MC_N80_RESET, 0);
    CHECK(!drq());
    wr(MC_N80_IDR, 0);                                /* Start DMA Target Receive */
    CHECK(drq());
    uint32_t v;
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 4, &v), 4);
    CHECK_EQ(v, 0x11223344u);
    CHECK(!drq());
    CHECK_EQ(mc_ncr5380_dma_read(&chip, 1, &v), 0);
    wr(MC_N80_BSR, 0);                                /* Start DMA Send */
    CHECK(drq());
    CHECK_EQ(mc_ncr5380_dma_write(&chip, 2, 0xAB12), 2);
    CHECK_EQ(tgt_n, 2);
    CHECK_EQ(tgt_log[0], 0xAB);
    CHECK_EQ(tgt_log[1], 0x12);
    /* Without a receiving initiator the send never gets DRQ. */
    chip.tgt_send = NULL;
    wr(MC_N80_BSR, 0);
    CHECK(!drq());
    wr(MC_N80_MODE, 0);
    wr(MC_N80_ICR, 0);
    /* Start DMA Target Receive is refused as an initiator. */
    chip.tgt_recv = on_recv;
    tgt_feed_n = 0;
    CHECK(select_target(0));
    wr(MC_N80_MODE, MC_N80_MODE_DMA);
    wr(MC_N80_IDR, 0);
    CHECK(!drq());
    wr(MC_N80_MODE, 0);
    mc_scsi_bus_release(&bus);
}

int main(void) {
    int tests = 0;
    setup();
#define RUN(t) (t(), tests++)
    RUN(test_registers);
    RUN(test_last_byte_flag);
    RUN(test_arbitration);
    RUN(test_select_no_atn);
    RUN(test_select_atn_inquiry);
    RUN(test_atn_to_bus);
    RUN(test_select_nobody);
    RUN(test_dma_read);
    RUN(test_block_mode);
    RUN(test_dma_write);
    RUN(test_dma_short);
    RUN(test_eop);
    RUN(test_dma_send_eop_c80);
    RUN(test_dma_mode_rules);
    RUN(test_parity);
    RUN(test_selected);
    RUN(test_busy_loss);
    RUN(test_reset_conditions);
    RUN(test_target_mode);
    mc_scsi_bus_free(&bus);
    mc_vdisk_destroy(vd);
    mc_disk_close(img);
    printf("test_ncr5380: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
