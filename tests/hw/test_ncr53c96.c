/* The NCR 53C96 against its data sheet (NCR 53C94/53C95/53C96, February
 * 1990): register set (Table 7), reset values, FIFO and flags, the selection
 * sequences and their sequence steps (Tables 8, 9, 11), transfer
 * information in DMA and non-DMA forms, initiator command complete
 * sequence, message accepted, stacked interrupts, reset SCSI bus, the
 * selection time-out formula, and the Quadra 700 pseudo-DMA path. */
#include "scsi_util.h"
#include "ncr53c96.h"
#include "scsi_target.h"

#include <string.h>

static hw_fake f;
static mc_scsi_bus bus;
static mc_ncr53c96 chip;
static mc_scsi_unit disk;
static mc_disk *img;
static mc_vdisk *vd;
static const uint8_t driver[512] = {0x4E, 0x75};

static uint8_t rd(unsigned r) { return (uint8_t)mc_ncr53c96_ops.read(&chip, r, 1); }
static void wr(unsigned r, uint8_t v) { mc_ncr53c96_ops.write(&chip, r, 1, v); }
static int irq(void) { return hw_fake_line(&f, &chip, MC_NCR53C96_LINE_IRQ); }
static int drq(void) { return hw_fake_line(&f, &chip, MC_NCR53C96_LINE_DRQ); }
/* Lets the pending completion arrive. */
static void settle(void) {
    uint64_t when = hw_fake_deadline(&f, &chip);
    if (when != MC_NEVER) hw_fake_advance(&f, when);
}
/* Waits for INT and returns the interrupt register (clearing it); status
 * and sequence step as they were. */
static uint8_t wait_intr(uint8_t *status, uint8_t *seq) {
    settle();
    uint8_t st = rd(MC_NCR_STATUS), sq = rd(MC_NCR_SEQ) & 7;
    if (status) *status = st;
    if (seq) *seq = sq;
    return rd(MC_NCR_INTR);
}
static void fifo_put(const uint8_t *p, unsigned n) { for (unsigned i = 0; i < n; i++) wr(MC_NCR_FIFO, p[i]); }

static void setup(void) {
    char error[200];
    hw_fake_init(&f);
    img = mc_disk_open(scsi_make_image("ncr53c96", "disk.img", 256 * 512, 0x5A), 1, 512, error, sizeof error);
    CHECK(img != NULL);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    mc_scsi_bus_init(&bus);
    mc_scsi_disk_init(&disk, vd);
    mc_scsi_bus_attach(&bus, 0, mc_scsi_unit_target(&disk));
    mc_ncr53c96_init(&chip, &f.host, &bus, 25000000);
    wr(MC_NCR_CONFIG1, 0x07);   /* bus ID 7 */
    wr(MC_NCR_CLOCK, 5);        /* 20.01-25 MHz: factor 5 (Clock Conversion table) */
    wr(MC_NCR_INTR, 153);       /* 250 ms at 25 MHz (Time-Out) */
}

/* Reset values: status, interrupt and sequence step clear; FIFO empty;
 * config 2 and 3 clear; config 1 keeps the bus ID (Reset Chip and the
 * registers' descriptions). Config registers read back what was written. */
static void test_reset(void) {
    wr(MC_NCR_CONFIG2, 0x48);
    wr(MC_NCR_CONFIG3, 0x05);
    wr(MC_NCR_CONFIG1, 0x57);
    CHECK_EQ(rd(MC_NCR_CONFIG2), 0x48);
    CHECK_EQ(rd(MC_NCR_CONFIG3), 0x05);
    fifo_put((const uint8_t *)"abc", 3);
    wr(MC_NCR_COMMAND, 0x02);   /* reset chip */
    CHECK_EQ(rd(MC_NCR_STATUS) & 0xF8, 0);
    CHECK_EQ(rd(MC_NCR_INTR), 0);
    CHECK_EQ(rd(MC_NCR_FLAGS), 0);
    CHECK_EQ(rd(MC_NCR_CONFIG1), 0x07);
    CHECK_EQ(rd(MC_NCR_CONFIG2), 0);
    CHECK_EQ(rd(MC_NCR_CONFIG3), 0);
    CHECK_EQ(rd(MC_NCR_COMMAND), 0x02);
    CHECK_EQ(chip.ccf, 2);
    CHECK_EQ(chip.sync_period, 5);
    CHECK(!irq() && !drq());
    wr(MC_NCR_CLOCK, 5);
}

/* FIFO: 16 deep, flags in bits 4-0 of register 7; flush empties it; a write
 * into a full FIFO sets gross error (Figure 8 bit 6). DMA NOP loads the
 * counter from the count register (Transfer Count; NOP). */
static void test_fifo(void) {
    for (int i = 0; i < 5; i++) wr(MC_NCR_FIFO, (uint8_t)(0x10 + i));
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 5);
    CHECK_EQ(rd(MC_NCR_FIFO), 0x10);
    CHECK_EQ(rd(MC_NCR_FIFO), 0x11);
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 3);
    wr(MC_NCR_COMMAND, 0x01);
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 0);
    for (int i = 0; i < 17; i++) wr(MC_NCR_FIFO, (uint8_t)i);
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 16);
    CHECK(rd(MC_NCR_STATUS) & 0x40);
    wr(MC_NCR_COMMAND, 0x02);
    wr(MC_NCR_CLOCK, 5);
    wr(MC_NCR_INTR, 153);
    wr(MC_NCR_TC_LO, 0x34);
    wr(MC_NCR_TC_HI, 0x12);
    CHECK_EQ(rd(MC_NCR_TC_LO), 0);   /* the counter, not the register */
    wr(MC_NCR_COMMAND, 0x80);
    CHECK_EQ(rd(MC_NCR_TC_LO), 0x34);
    CHECK_EQ(rd(MC_NCR_TC_HI), 0x12);
    CHECK(!(rd(MC_NCR_STATUS) & 0x10));
    CHECK(!irq());
}

/* Selection time-out: disconnect interrupt, sequence step 0, after
 * RV x 8192 x CCF clocks (153 x 8192 x 5 / 25 MHz = 250.7 ms). */
static void test_timeout(void) {
    uint64_t start = f.now;
    wr(MC_NCR_STATUS, 5);       /* destination ID: nobody */
    wr(MC_NCR_FIFO, 0x00);
    wr(MC_NCR_COMMAND, 0x41);
    uint64_t expect = (uint64_t)153 * 8192 * 5 * MC_C15M_HZ / 25000000u;
    CHECK_EQ(hw_fake_deadline(&f, &chip) - start, expect);
    hw_fake_advance(&f, start + expect - 1);
    CHECK(!irq());
    uint8_t st, seq;
    CHECK_EQ(wait_intr(&st, &seq), 0x20);
    CHECK(st & 0x80);
    CHECK_EQ(seq, 0);
    CHECK(!irq());
    wr(MC_NCR_COMMAND, 0x01);
}

/* Select without ATN (Table 8): complete is BS|FC with step 4 and the
 * target in its next phase; ICCS leaves status and message in the FIFO with
 * FC; message accepted ends with the disconnect interrupt (the target went
 * BUS FREE after COMMAND COMPLETE). */
static void test_select_tur(void) {
    const uint8_t tur[6] = {0};
    wr(MC_NCR_STATUS, 0);
    fifo_put(tur, 6);
    wr(MC_NCR_COMMAND, 0x41);
    CHECK(!irq());
    uint8_t st, seq;
    CHECK_EQ(wait_intr(&st, &seq), 0x18);
    CHECK_EQ(seq, 4);
    CHECK_EQ(st & 7, MC_SCSI_STATUS);
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 0);
    wr(MC_NCR_COMMAND, 0x11);
    CHECK_EQ(wait_intr(NULL, NULL), 0x08);
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 2);
    CHECK_EQ(rd(MC_NCR_FIFO), MC_SCSI_GOOD);
    CHECK_EQ(rd(MC_NCR_FIFO), MC_SCSI_MSG_COMMAND_COMPLETE);
    wr(MC_NCR_COMMAND, 0x12);
    CHECK_EQ(wait_intr(NULL, NULL), 0x20);
    CHECK_EQ(chip.state, 0);
    /* An initiator command while disconnected: illegal command interrupt. */
    wr(MC_NCR_COMMAND, 0x10);
    CHECK_EQ(wait_intr(NULL, NULL), 0x40);
}

/* Select with ATN (Table 9) and INQUIRY read byte by byte with non-DMA
 * transfer information: one byte and a bus service interrupt each. */
static void test_inquiry_pio(void) {
    const uint8_t msg_cdb[7] = {0x80, 0x12, 0, 0, 0, 36, 0};
    fifo_put(msg_cdb, 7);
    wr(MC_NCR_COMMAND, 0x42);
    uint8_t st, seq;
    CHECK_EQ(wait_intr(&st, &seq), 0x18);
    CHECK_EQ(seq, 4);
    CHECK_EQ(st & 7, MC_SCSI_DATA_IN);
    char text[37] = {0};
    for (int i = 0; i < 36; i++) {
        wr(MC_NCR_COMMAND, 0x10);
        CHECK_EQ(wait_intr(&st, NULL), 0x10);
        text[i] = (char)rd(MC_NCR_FIFO);
    }
    CHECK(!memcmp(text + 8, "LIVADIX ", 8));
    CHECK_EQ(st & 7, MC_SCSI_STATUS);
    /* Status by transfer information, then the message byte: FC with ACK
     * held (Transfer Information, last byte of a message in). */
    wr(MC_NCR_COMMAND, 0x10);
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK_EQ(rd(MC_NCR_FIFO), 0);
    CHECK_EQ(st & 7, MC_SCSI_MSG_IN);
    wr(MC_NCR_COMMAND, 0x10);
    CHECK_EQ(wait_intr(NULL, NULL), 0x08);
    CHECK_EQ(rd(MC_NCR_FIFO), 0);
    wr(MC_NCR_COMMAND, 0x12);
    CHECK_EQ(wait_intr(NULL, NULL), 0x20);
}

/* Starts READ(10)/WRITE(10) of `blocks` at `lba` with select-with-ATN. */
static void start_rw(uint8_t op, uint32_t lba, uint16_t blocks) {
    const uint8_t cmd[11] = {0x80, op, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                             0, (uint8_t)(blocks >> 8), (uint8_t)blocks, 0};
    fifo_put(cmd, 11);
    wr(MC_NCR_COMMAND, 0x42);
    uint8_t seq;
    CHECK_EQ(wait_intr(NULL, &seq), 0x18);
    CHECK_EQ(seq, 4);
}

static void finish_command(uint8_t want_status) {
    wr(MC_NCR_COMMAND, 0x11);
    CHECK_EQ(wait_intr(NULL, NULL), 0x08);
    CHECK_EQ(rd(MC_NCR_FIFO), want_status);
    rd(MC_NCR_FIFO);
    wr(MC_NCR_COMMAND, 0x12);
    CHECK_EQ(wait_intr(NULL, NULL), 0x20);
}

/* DMA transfer information through the pseudo-DMA window: DREQ while data
 * waits, the counter decrementing per DACK, terminal count in the status
 * register, bus service at the end with the target in status phase. */
static void test_dma_read(void) {
    start_rw(0x28, 128 + 10, 2);
    CHECK(!drq());
    wr(MC_NCR_TC_LO, 0x00);
    wr(MC_NCR_TC_HI, 0x04);
    wr(MC_NCR_COMMAND, 0x90);
    CHECK(drq());
    uint8_t data[1024];
    for (int i = 0; i < 1024; i += 2) {
        uint32_t v;
        CHECK_EQ(mc_ncr53c96_dma_read(&chip, 2, &v), 2);
        data[i] = (uint8_t)(v >> 8);
        data[i + 1] = (uint8_t)v;
    }
    CHECK(!drq());
    uint32_t v;
    CHECK_EQ(mc_ncr53c96_dma_read(&chip, 2, &v), 0);   /* counter exhausted: no DREQ */
    uint8_t st;
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK(st & 0x10);
    CHECK_EQ(st & 7, MC_SCSI_STATUS);
    uint8_t want[1024];
    CHECK(mc_disk_read(img, 10, 2, want));
    CHECK(!memcmp(data, want, sizeof want));
    finish_command(MC_SCSI_GOOD);
    /* Longword accesses take four DACK cycles. */
    start_rw(0x28, 128 + 3, 1);
    wr(MC_NCR_TC_LO, 0x00);
    wr(MC_NCR_TC_HI, 0x02);
    wr(MC_NCR_COMMAND, 0x90);
    CHECK_EQ(mc_ncr53c96_dma_read(&chip, 4, &v), 4);
    CHECK_EQ(v, 3);
    for (int i = 4; i < 512; i += 4) mc_ncr53c96_dma_read(&chip, 4, &v);
    CHECK_EQ(wait_intr(NULL, NULL), 0x10);
    finish_command(MC_SCSI_GOOD);
}

/* A DMA transfer information whose counter outlasts the target's data (a
 * READ TOC of 20 bytes read in 16-byte chunks), the FIFO emptied by the CPU
 * and not by DACK cycles: the target went on to status, so the transfer
 * ends with a bus service interrupt, the target in that phase. */
static void test_dma_short_cpu(void) {
    start_rw(0x28, 128 + 5, 1);
    wr(MC_NCR_TC_LO, 0xF0);
    wr(MC_NCR_TC_HI, 0x01);
    wr(MC_NCR_COMMAND, 0x90);
    uint32_t v;
    for (int i = 0; i < 496; i += 2) CHECK_EQ(mc_ncr53c96_dma_read(&chip, 2, &v), 2);
    uint8_t st;
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK_EQ(st & 7, MC_SCSI_DATA_IN);
    wr(MC_NCR_TC_LO, 0x20);
    wr(MC_NCR_TC_HI, 0x00);
    wr(MC_NCR_COMMAND, 0x90);                       /* 32 bytes asked, 16 left */
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 16);
    CHECK(!irq());
    for (int i = 0; i < 16; i++) rd(MC_NCR_FIFO);
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK_EQ(st & 7, MC_SCSI_STATUS);
    CHECK_EQ(rd(MC_NCR_TC_LO), 0x10);               /* the counter left: 16 */
    finish_command(MC_SCSI_GOOD);
}

static void test_dma_write(void) {
    start_rw(0x2A, 128 + 20, 1);
    wr(MC_NCR_TC_LO, 0x00);
    wr(MC_NCR_TC_HI, 0x02);
    wr(MC_NCR_COMMAND, 0x90);
    CHECK(drq());
    for (int i = 0; i < 512; i += 2) CHECK_EQ(mc_ncr53c96_dma_write(&chip, 2, (uint32_t)(0xC000 + i)), 2);
    CHECK(!drq());
    CHECK_EQ(mc_ncr53c96_dma_write(&chip, 2, 0), 0);
    uint8_t st;
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK(st & 0x10);
    finish_command(MC_SCSI_GOOD);
    uint8_t back[512];
    CHECK(mc_disk_read(img, 20, 1, back));
    CHECK_EQ(back[0], 0xC0); CHECK_EQ(back[1], 0x00); CHECK_EQ(back[510], 0xC1); CHECK_EQ(back[511], 0xFE);
    /* No DMA command running: the window refuses. */
    CHECK_EQ(mc_ncr53c96_dma_write(&chip, 1, 0), 0);
}

/* A CHECK CONDITION and the sense data by non-DMA transfer of a REQUEST
 * SENSE, the CDB sent by a DMA selection. */
static void test_check_condition(void) {
    start_rw(0x28, 0x10000, 1);
    finish_command(MC_SCSI_CHECK_CONDITION);
    const uint8_t sense[7] = {0x80, 0x03, 0, 0, 0, 18, 0};
    wr(MC_NCR_TC_LO, 7);
    wr(MC_NCR_TC_HI, 0);
    wr(MC_NCR_COMMAND, 0xC2);   /* DMA select with ATN */
    CHECK(drq());
    for (int i = 0; i < 7; i++) CHECK_EQ(mc_ncr53c96_dma_write(&chip, 1, sense[i]), 1);
    uint8_t seq;
    CHECK_EQ(wait_intr(NULL, &seq), 0x18);
    CHECK_EQ(seq, 4);
    wr(MC_NCR_TC_LO, 18);
    wr(MC_NCR_COMMAND, 0x90);
    uint8_t s[18];
    for (int i = 0; i < 18; i++) {
        uint32_t v;
        CHECK_EQ(mc_ncr53c96_dma_read(&chip, 1, &v), 1);
        s[i] = (uint8_t)v;
    }
    CHECK_EQ(wait_intr(NULL, NULL), 0x10);
    CHECK_EQ(s[2], MC_SENSE_ILLEGAL_REQUEST);
    CHECK_EQ(s[12], 0x21);
    finish_command(MC_SCSI_GOOD);
}

/* Select with ATN and stop (Table 11): step 1 with ATN still asserted; an
 * SDTR sent by transfer information is rejected by the target (MESSAGE
 * REJECT in message in); after message accepted the target asks for the
 * command. Then a command and its completion as stacked interrupts. */
static void test_atn_stop(void) {
    wr(MC_NCR_FIFO, 0x80);
    wr(MC_NCR_COMMAND, 0x43);
    uint8_t st, seq;
    CHECK_EQ(wait_intr(&st, &seq), 0x18);
    CHECK_EQ(seq, 1);
    CHECK_EQ(st & 7, MC_SCSI_MSG_OUT);
    CHECK(bus.atn);
    const uint8_t sdtr[5] = {0x01, 3, 0x01, 25, 8};
    fifo_put(sdtr, 5);
    wr(MC_NCR_COMMAND, 0x10);
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK(!bus.atn);
    CHECK_EQ(st & 7, MC_SCSI_MSG_IN);
    wr(MC_NCR_COMMAND, 0x10);
    CHECK_EQ(wait_intr(NULL, NULL), 0x08);
    CHECK_EQ(rd(MC_NCR_FIFO), MC_SCSI_MSG_REJECT);
    wr(MC_NCR_COMMAND, 0x12);
    CHECK_EQ(wait_intr(&st, NULL), 0x10);
    CHECK_EQ(st & 7, MC_SCSI_COMMAND);
    const uint8_t tur[6] = {0};
    fifo_put(tur, 6);
    /* Two commands stacked: transfer information, then the command complete
     * sequence; both interrupts wait, the second behind the first. */
    wr(MC_NCR_COMMAND, 0x10);
    wr(MC_NCR_COMMAND, 0x11);
    settle();
    settle();
    CHECK(irq());
    CHECK_EQ(rd(MC_NCR_INTR), 0x10);
    CHECK(irq());
    CHECK_EQ(rd(MC_NCR_INTR), 0x08);
    CHECK(!irq());
    CHECK_EQ(rd(MC_NCR_FLAGS) & 0x1F, 2);
    wr(MC_NCR_COMMAND, 0x01);
    wr(MC_NCR_COMMAND, 0x12);
    CHECK_EQ(wait_intr(NULL, NULL), 0x20);
}

/* Reset SCSI bus: the SCSI reset interrupt after T2 = 130 x CCF clocks,
 * unless config 1 bit 6 disables its report; the target loses its state.
 * Phase latch (config 2 bit 6) keeps the phase of the completion. */
static void test_bus_reset(void) {
    wr(MC_NCR_COMMAND, 0x03);
    CHECK_EQ(hw_fake_deadline(&f, &chip) - f.now, 130 * 5 * (uint64_t)MC_C15M_HZ / 25000000u + 1);
    CHECK_EQ(wait_intr(NULL, NULL), 0x80);
    wr(MC_NCR_CONFIG1, 0x47);
    wr(MC_NCR_COMMAND, 0x03);
    settle();
    CHECK(!irq());
    CHECK_EQ(hw_fake_deadline(&f, &chip), MC_NEVER);
    wr(MC_NCR_CONFIG1, 0x07);
    wr(MC_NCR_CONFIG2, 0x40);
    const uint8_t tur[6] = {0};
    fifo_put(tur, 6);
    wr(MC_NCR_STATUS, 0);
    wr(MC_NCR_COMMAND, 0x41);
    settle();
    CHECK_EQ(rd(MC_NCR_STATUS) & 7, MC_SCSI_STATUS);
    CHECK_EQ(rd(MC_NCR_INTR), 0x18);
    wr(MC_NCR_CONFIG2, 0);
    finish_command(MC_SCSI_GOOD);
}

int main(void) {
    int tests = 0;
    setup();
#define RUN(t) (t(), tests++)
    RUN(test_reset);
    RUN(test_fifo);
    RUN(test_timeout);
    RUN(test_select_tur);
    RUN(test_inquiry_pio);
    RUN(test_dma_read);
    RUN(test_dma_short_cpu);
    RUN(test_dma_write);
    RUN(test_check_condition);
    RUN(test_atn_stop);
    RUN(test_bus_reset);
    mc_scsi_bus_free(&bus);
    mc_vdisk_destroy(vd);
    mc_disk_close(img);
    printf("test_ncr53c96: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
