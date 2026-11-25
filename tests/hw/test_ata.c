/* The ATA controller and its devices (src/hw/ata.c) against ATA-2 (X3.279-1996),
 * the ATA-3 draft and SFF-8020i (ATAPI), through the task file only: a fake
 * host keeps guest time and the INTRQ line. Expected values come from the
 * register, status, error and command descriptions of those documents
 * (cited per test) and from the vdisk's own blocks. */
#include "scsi_util.h"
#include "ata.h"

#include <string.h>

static hw_fake f;
static mc_ata ata;
static mc_disk *img, *img_ro, *iso;
static mc_vdisk *vd, *vd_ro, *vcd;
static const uint8_t driver[1000] = {0x60, 0x06};
static uint8_t io[64 * 1024];
static int ejects;
static mc_vdisk *ejected_media;

static uint8_t rd(unsigned reg) { return (uint8_t)mc_ata_reg_read(&ata, reg, 1); }
static void wr(unsigned reg, uint8_t v) { mc_ata_reg_write(&ata, reg, 1, v); }
static int irq(void) { return hw_fake_line(&f, &ata, MC_ATA_LINE_INTRQ); }
static void step(uint64_t n) { hw_fake_step(&f, n); }
static uint8_t alt(void) { return rd(MC_ATA_ALT_STATUS); }

/* Runs time until BSY is clear (at most ~1 s), returns the status (alternate: no side effect). */
static uint8_t wait_idle(void) {
    for (int i = 0; i < 20000 && (alt() & MC_ATA_ST_BSY); i++) step(1000);
    return alt();
}

static void select_dev(unsigned dev, int lba) { wr(MC_ATA_DRIVE, (uint8_t)(0xA0 | (dev << 4) | (lba ? 0x40 : 0))); }

static void set_lba(uint32_t lba, unsigned count, unsigned dev) {
    wr(MC_ATA_DRIVE, (uint8_t)(0xE0 | (dev << 4) | ((lba >> 24) & 15)));
    wr(MC_ATA_COUNT, (uint8_t)count);
    wr(MC_ATA_SECTOR, (uint8_t)lba);
    wr(MC_ATA_CYL_LO, (uint8_t)(lba >> 8));
    wr(MC_ATA_CYL_HI, (uint8_t)(lba >> 16));
}

static void command(uint8_t cmd) { wr(MC_ATA_COMMAND, cmd); }

/* Reads one DRQ block of `bytes` as ATA words into io+at. */
static void read_block(uint32_t at, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 2) {
        uint16_t w = mc_ata_data_read16(&ata);
        io[at + i] = (uint8_t)w;
        io[at + i + 1] = (uint8_t)(w >> 8);
    }
}
static void write_block(const uint8_t *src, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; i += 2) mc_ata_data_write16(&ata, (uint16_t)(src[i] | src[i + 1] << 8));
}

static uint32_t word(unsigned w) { return io[2 * w] | io[2 * w + 1] << 8; }
static void field(unsigned first, unsigned words, char *out) {
    for (unsigned i = 0; i < words; i++) { out[2 * i] = (char)io[2 * (first + i) + 1]; out[2 * i + 1] = (char)io[2 * (first + i)]; }
    out[2 * words] = 0;
}

static void setup(void) {
    char error[200];
    img = mc_disk_open(scsi_make_image("ata", "disk.img", 4096 * 512, 0x31), 1, 512, error, sizeof error);
    img_ro = mc_disk_open(scsi_make_image("ata", "ro.img", 64 * 512, 0x32), 0, 512, error, sizeof error);
    iso = mc_disk_open(scsi_make_image("ata", "cd.iso", 64 * 2048, 0x33), 0, 2048, error, sizeof error);
    CHECK(img && img_ro && iso);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    vd_ro = mc_vdisk_create(&img_ro, 1, &d, error, sizeof error);
    vcd = mc_vdisk_create(&iso, 1, &d, error, sizeof error);
    CHECK(vd && vd_ro && vcd);
    hw_fake_init(&f);
    mc_ata_init(&ata, &f.host, 2);
    mc_ata_attach_disk(&ata, 0, vd);
    mc_ata_reset(&ata);
    step(MC_ATA_T_RESET + 1);
}

/* 7.2.13 status, 9.9 reset: BSY after the hard reset, the signature (count 1,
 * sector 1, cylinder 0) with DRDY|DSC and error 01h after it; a soft reset
 * (SRST, 7.2.7) holds BSY while set and ends the same way after it is
 * cleared; the drive/head register has its obsolete bits 7 and 5 set. */
static void test_reset_signature(void) {
    mc_ata_reset(&ata);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(MC_ATA_T_RESET - 1);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(1);
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(rd(MC_ATA_ERROR), 0x01);
    CHECK_EQ(rd(MC_ATA_COUNT), 1); CHECK_EQ(rd(MC_ATA_SECTOR), 1);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0); CHECK_EQ(rd(MC_ATA_CYL_HI), 0);
    CHECK_EQ(rd(MC_ATA_DRIVE), 0xA0);
    /* Soft reset after a command changed the registers. */
    set_lba(77, 5, 0);
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_SRST);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(MC_ATA_T_RESET * 3);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);        /* still held */
    wr(MC_ATA_DEV_CONTROL, 0);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(MC_ATA_T_RESET);
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(rd(MC_ATA_COUNT), 1); CHECK_EQ(rd(MC_ATA_SECTOR), 1);
    CHECK_EQ(rd(MC_ATA_DRIVE), 0xA0);
    CHECK_EQ(irq(), 0);
}

/* Task file: registers hold what is written (7.2); an absent device 1
 * reads 00h; the DEV bit selects; the drive address register (7.2.1). */
static void test_task_file(void) {
    wr(MC_ATA_COUNT, 0x12); wr(MC_ATA_SECTOR, 0x34); wr(MC_ATA_CYL_LO, 0x56); wr(MC_ATA_CYL_HI, 0x78);
    CHECK_EQ(rd(MC_ATA_COUNT), 0x12); CHECK_EQ(rd(MC_ATA_SECTOR), 0x34);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0x56); CHECK_EQ(rd(MC_ATA_CYL_HI), 0x78);
    wr(MC_ATA_DRIVE, 0x4B);
    CHECK_EQ(rd(MC_ATA_DRIVE), 0xEB);
    wr(MC_ATA_DRIVE, 0x00);
    CHECK_EQ(rd(MC_ATA_DRIVE), 0xA0);
    CHECK_EQ(rd(MC_ATA_DRIVE_ADDRESS) & 3, 2);       /* nDS0 low: device 0 */
    select_dev(1, 0);
    CHECK_EQ(rd(MC_ATA_STATUS), 0x00);
    CHECK_EQ(rd(MC_ATA_ERROR), 0x00);
    CHECK_EQ(rd(MC_ATA_DRIVE_ADDRESS) & 3, 1);
    command(0xEC);                       /* no device: nothing happens */
    step(100000);
    CHECK_EQ(rd(MC_ATA_STATUS), 0x00);
    CHECK_EQ(irq(), 0);
    select_dev(0, 0);
    CHECK_EQ(rd(MC_ATA_STATUS), 0x50);
    /* Register spacing: stride 4 through the machine's ops. */
    mc_ata_ops.write(&ata, 3u << 2, 1, 0x9C);
    CHECK_EQ(mc_ata_ops.read(&ata, 3u << 2, 1), 0x9C);
    CHECK_EQ(mc_ata_ops.read(&ata, 7u << 2, 1), 0x50);
    CHECK_EQ(mc_ata_ops.read(&ata, 14u << 2, 1), 0x50);
}

/* IDENTIFY DEVICE (8.12, Table 17). */
static void test_identify(void) {
    select_dev(0, 0);
    command(0xEC);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    CHECK_EQ(irq(), 0);
    uint8_t s = wait_idle();
    CHECK_EQ(s, 0x58);
    CHECK_EQ(irq(), 1);
    CHECK_EQ(rd(MC_ATA_STATUS), 0x58);   /* a status read clears INTRQ */
    CHECK_EQ(irq(), 0);
    read_block(0, 512);
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 0);                  /* no interrupt after the last word */
    CHECK_EQ(word(0), 0x0040);
    uint32_t total = (uint32_t)mc_vdisk_blocks(vd);
    CHECK_EQ(total, 128 + 4096);
    CHECK_EQ(word(1) * word(3) * word(6), total / (16 * 63) * 16 * 63);
    CHECK_EQ(word(3), 16); CHECK_EQ(word(6), 63);
    CHECK_EQ(word(60) | word(61) << 16, total);
    CHECK_EQ(word(49) & 0x0200, 0x0200);     /* LBA */
    CHECK_EQ(word(47) & 0xFF, 16);
    CHECK_EQ(word(53) & 3, 3);
    CHECK_EQ(word(54), word(1)); CHECK_EQ(word(55), 16); CHECK_EQ(word(56), 63);
    CHECK_EQ(word(57) | word(58) << 16, word(54) * 16 * 63);
    CHECK_EQ(word(59), 0);
    CHECK_EQ(word(64) & 3, 3);
    CHECK_EQ(word(80) & 0x000E, 0x000E);
    char text[64];
    field(27, 20, text);
    CHECK(!strncmp(text, "LIVADIX VIRTUAL DISK", 17));
    field(10, 10, text);
    CHECK(!strncmp(text, "LVDXHD00000001", 14));
    field(23, 4, text);
    CHECK(!strncmp(text, "1.0", 3));
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += io[i];
    CHECK_EQ(io[510], 0xA5);
    CHECK_EQ(sum & 0xFF, 0);                 /* integrity word: the bytes sum to zero */
    /* The same bytes through the 68k lanes: first byte in bits 15..8. */
    command(0xEC);
    wait_idle();
    uint32_t v = mc_ata_reg_read(&ata, MC_ATA_DATA, 2);
    CHECK_EQ(v, (uint32_t)io[0] << 8 | io[1]);
    uint32_t l = mc_ata_reg_read(&ata, MC_ATA_DATA, 4);
    CHECK_EQ(l, (uint32_t)io[2] << 24 | (uint32_t)io[3] << 16 | (uint32_t)io[4] << 8 | io[5]);
    for (int i = 0; i < 253; i++) mc_ata_reg_read(&ata, MC_ATA_DATA, 2);
    CHECK_EQ(alt(), 0x50);
}

/* READ SECTORS (8.? 20h/21h): BSY at once, DRQ with INTRQ after the first
 * block's delay, one INTRQ per sector, none after the last, the task file
 * counting down and addressing the sector transferred. */
static void test_read_lba(void) {
    set_lba(130, 3, 0);
    command(0x20);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(MC_ATA_T_CMD + MC_ATA_T_FIRST - 1);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    CHECK_EQ(irq(), 0);
    step(1);
    CHECK_EQ(alt(), 0x58);
    CHECK_EQ(irq(), 1);
    CHECK_EQ(rd(MC_ATA_COUNT), 2);
    CHECK_EQ(rd(MC_ATA_SECTOR), 130);
    uint8_t want[512 * 3];
    CHECK(mc_vdisk_read(vd, 130, 3, want));
    unsigned irqs = hw_fake_changes(&f, &ata, MC_ATA_LINE_INTRQ);
    for (unsigned s = 0; s < 3; s++) {
        rd(MC_ATA_STATUS);
        read_block(s * 512, 512);
        if (s < 2) {
            CHECK_EQ(alt(), MC_ATA_ST_BSY);
            CHECK_EQ(irq(), 0);
            wait_idle();
            CHECK_EQ(alt(), 0x58);
            CHECK_EQ(irq(), 1);
            CHECK_EQ(rd(MC_ATA_SECTOR), 131 + s);
            CHECK_EQ(rd(MC_ATA_COUNT), 1 - s);
        }
    }
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 0);
    CHECK(!memcmp(io, want, sizeof want));
    CHECK_EQ(hw_fake_changes(&f, &ata, MC_ATA_LINE_INTRQ) - irqs, 5);   /* three status reads (falls) and two rises between them */
    CHECK_EQ(rd(MC_ATA_COUNT), 0);
    CHECK_EQ(rd(MC_ATA_SECTOR), 132);
    CHECK_EQ(rd(MC_ATA_ERROR), 0);
    /* A count of 0 is 256 sectors; 21h is the same without retry. */
    set_lba(200, 0, 0);
    command(0x21);
    uint32_t got = 0;
    for (int b = 0; b < 256; b++) { wait_idle(); CHECK_EQ(alt(), 0x58); read_block(got % (60 * 512), 512); got += 512; }
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(got, 256 * 512);
    /* Data register reads with no DRQ return 0 and change nothing. */
    CHECK_EQ(mc_ata_data_read16(&ata), 0);
    CHECK_EQ(alt(), 0x50);
}

/* CHS addressing (drive/head bit 6 clear): the same sector as LBA
 * (cyl * heads + head) * sectors + sector - 1; sector 0, a head past the
 * translation and a cylinder past it are IDNF (error 10h). */
static void test_chs(void) {
    uint32_t lba = (3 * 16 + 5) * 63 + 9 - 1;
    wr(MC_ATA_DRIVE, 0xA0 | 5);
    wr(MC_ATA_COUNT, 1); wr(MC_ATA_SECTOR, 9); wr(MC_ATA_CYL_LO, 3); wr(MC_ATA_CYL_HI, 0);
    command(0x20);
    wait_idle();
    read_block(0, 512);
    uint8_t want[512];
    CHECK(mc_vdisk_read(vd, lba, 1, want));
    CHECK(!memcmp(io, want, 512));
    CHECK_EQ(rd(MC_ATA_SECTOR), 9); CHECK_EQ(rd(MC_ATA_CYL_LO), 3); CHECK_EQ(rd(MC_ATA_DRIVE) & 15, 5);
    wr(MC_ATA_COUNT, 1);       /* the count ran down to 0, which means 256 */
    wr(MC_ATA_SECTOR, 0);
    command(0x20);
    wait_idle();
    CHECK_EQ(alt(), 0x51);
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    CHECK_EQ(irq(), 1);
    wr(MC_ATA_SECTOR, 64);
    command(0x20); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    wr(MC_ATA_SECTOR, 1); wr(MC_ATA_COUNT, 1); wr(MC_ATA_DRIVE, 0xA0 | 15);
    command(0x20); wait_idle();
    CHECK_EQ(alt(), 0x58);               /* head 15 exists */
    read_block(0, 512);
    wr(MC_ATA_COUNT, 1); wr(MC_ATA_CYL_LO, 0xFF); wr(MC_ATA_CYL_HI, 0x3F);
    command(0x20); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
}

/* WRITE SECTORS (30h/31h): DRQ without an interrupt, an interrupt when each
 * sector is written, the data on the disk; a read-only image aborts. */
static void test_write(void) {
    uint8_t data[512 * 2];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + 3);
    set_lba(300, 2, 0);
    command(0x30);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    step(MC_ATA_T_CMD);
    CHECK_EQ(alt(), 0x58);
    CHECK_EQ(irq(), 0);
    write_block(data, 512);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    wait_idle();
    CHECK_EQ(alt(), 0x58);
    CHECK_EQ(irq(), 1);
    CHECK_EQ(rd(MC_ATA_COUNT), 1);
    CHECK_EQ(rd(MC_ATA_SECTOR), 300 & 255);
    rd(MC_ATA_STATUS);
    write_block(data + 512, 512);
    wait_idle();
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 1);
    CHECK_EQ(rd(MC_ATA_COUNT), 0);
    uint8_t back[1024];
    CHECK(mc_vdisk_read(vd, 300, 2, back));
    CHECK(!memcmp(back, data, 1024));
    /* Data register writes with no DRQ are ignored. */
    mc_ata_data_write16(&ata, 0x1234);
    CHECK_EQ(alt(), 0x50);
    /* A read-only image. */
    ata.unit[1].kind = MC_ATA_NONE;
    mc_ata_attach_disk(&ata, 1, vd_ro);
    select_dev(1, 1);
    mc_ata_reset(&ata);
    step(MC_ATA_T_RESET);
    set_lba(130, 1, 1);
    command(0x31);
    wait_idle();
    CHECK_EQ(alt(), 0x51);
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    /* It reads. */
    set_lba(130, 1, 1);
    command(0x20); wait_idle(); read_block(0, 512);
    CHECK_EQ(alt(), 0x50);
    select_dev(0, 1);
    ata.unit[1].kind = MC_ATA_NONE;
}

/* SET MULTIPLE MODE (C6h), READ MULTIPLE (C4h), WRITE MULTIPLE (C5h): the
 * multiple commands abort until a count is set; the interrupt comes per
 * block; a count that is not a power of two up to the maximum aborts. */
static void test_multiple(void) {
    set_lba(400, 4, 0);
    command(0xC4); wait_idle();
    CHECK_EQ(alt(), 0x51); CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    command(0xC5); wait_idle();
    CHECK_EQ(alt(), 0x51);
    wr(MC_ATA_COUNT, 3); command(0xC6); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    wr(MC_ATA_COUNT, 32); command(0xC6); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    wr(MC_ATA_COUNT, 4); command(0xC6); wait_idle();
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 1);
    command(0xEC); wait_idle(); read_block(0, 512);
    CHECK_EQ(word(59), 0x0104);
    set_lba(400, 10, 0);
    command(0xC4);
    uint8_t want[512 * 10];
    CHECK(mc_vdisk_read(vd, 400, 10, want));
    uint32_t sizes[3] = {4, 4, 2};
    for (int b = 0; b < 3; b++) {
        wait_idle();
        CHECK_EQ(alt(), 0x58);
        CHECK_EQ(irq(), 1);
        rd(MC_ATA_STATUS);
        read_block(b * 4 * 512, sizes[b] * 512);       /* one DRQ block of 4 sectors (the last 2) */
    }
    CHECK_EQ(alt(), 0x50);
    CHECK(!memcmp(io, want, sizeof want));
    uint8_t data[512 * 6];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i ^ 0x5A);
    set_lba(500, 6, 0);
    command(0xC5);
    step(MC_ATA_T_CMD);
    CHECK_EQ(alt(), 0x58);
    write_block(data, 4 * 512);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    wait_idle();
    CHECK_EQ(alt(), 0x58); CHECK_EQ(irq(), 1);
    write_block(data + 4 * 512, 2 * 512);
    wait_idle();
    CHECK_EQ(alt(), 0x50);
    uint8_t back[512 * 6];
    CHECK(mc_vdisk_read(vd, 500, 6, back));
    CHECK(!memcmp(back, data, sizeof back));
    wr(MC_ATA_COUNT, 0); command(0xC6); wait_idle();      /* 0 disables */
    CHECK_EQ(alt(), 0x50);
    set_lba(400, 1, 0); command(0xC4); wait_idle();
    CHECK_EQ(alt(), 0x51);
}

/* READ VERIFY (40h/41h): the interrupt at the end, no data, the last sector
 * in the task file, IDNF beyond the end. SEEK (7xh) and RECALIBRATE (1xh). */
static void test_verify_seek(void) {
    set_lba(10, 20, 0);
    command(0x40);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    wait_idle();
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 1);
    CHECK_EQ(rd(MC_ATA_COUNT), 0);
    CHECK_EQ(rd(MC_ATA_SECTOR), 29);
    uint32_t total = (uint32_t)mc_vdisk_blocks(vd);
    set_lba(total - 2, 5, 0);
    command(0x41); wait_idle();
    CHECK_EQ(alt(), 0x51); CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    CHECK_EQ(rd(MC_ATA_COUNT), 3);        /* sectors not verified */
    set_lba(total, 1, 0);
    command(0x40); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    set_lba(100, 1, 0);
    command(0x70); wait_idle();
    CHECK_EQ(alt(), 0x50);
    set_lba(total + 5, 1, 0);
    command(0x7F); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    command(0x10); wait_idle();
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0); CHECK_EQ(rd(MC_ATA_CYL_HI), 0);
    set_lba(total - 1, 1, 0);
    command(0x20); wait_idle(); read_block(0, 512);      /* the last sector reads */
    CHECK_EQ(alt(), 0x50);
}

/* INITIALIZE DEVICE PARAMETERS (91h): the heads and sectors per track of
 * the CHS translation change (IDENTIFY words 54-58 follow); an invalid
 * translation aborts. */
static void test_init_params(void) {
    wr(MC_ATA_DRIVE, 0xA0 | 7);
    wr(MC_ATA_COUNT, 32);
    command(0x91); wait_idle();
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(irq(), 1);
    select_dev(0, 0);
    command(0xEC); wait_idle(); read_block(0, 512);
    CHECK_EQ(word(55), 8); CHECK_EQ(word(56), 32);
    CHECK_EQ(word(54), (mc_vdisk_blocks(vd) / (8 * 32)));
    CHECK_EQ(word(3), 16); CHECK_EQ(word(6), 63);        /* the default translation stays */
    wr(MC_ATA_DRIVE, 0xA0 | 3); wr(MC_ATA_COUNT, 1); wr(MC_ATA_SECTOR, 2); wr(MC_ATA_CYL_LO, 1); wr(MC_ATA_CYL_HI, 0);
    command(0x20); wait_idle(); read_block(0, 512);
    uint8_t want[512];
    CHECK(mc_vdisk_read(vd, (1 * 8 + 3) * 32 + 1, 1, want));
    CHECK(!memcmp(io, want, 512));
    wr(MC_ATA_DRIVE, 0xA0 | 15 | 0); wr(MC_ATA_COUNT, 0);
    command(0x91); wait_idle();
    CHECK_EQ(alt(), 0x51); CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    wr(MC_ATA_COUNT, 255); wr(MC_ATA_DRIVE, 0xA0 | 15); command(0x91); wait_idle();
    CHECK_EQ(alt(), 0x50);
    wr(MC_ATA_COUNT, 63); command(0x91); wait_idle();     /* back to 16 x 63 */
    CHECK_EQ(alt(), 0x50);
}

/* SET FEATURES (EFh): write cache (02h/82h, visible in IDENTIFY word 85),
 * read look-ahead, PIO transfer modes (03h with 08h + mode); DMA modes and
 * unknown features abort. */
static void test_set_features(void) {
    select_dev(0, 0);
    wr(MC_ATA_FEATURES, 0x82); command(0xEF); wait_idle();
    CHECK_EQ(alt(), 0x50); CHECK_EQ(irq(), 1);
    command(0xEC); wait_idle(); read_block(0, 512);
    CHECK_EQ(word(85) & 0x20, 0);
    wr(MC_ATA_FEATURES, 0x02); command(0xEF); wait_idle();
    command(0xEC); wait_idle(); read_block(0, 512);
    CHECK_EQ(word(85) & 0x20, 0x20);
    wr(MC_ATA_FEATURES, 0x03); wr(MC_ATA_COUNT, 0x0C); command(0xEF); wait_idle();
    CHECK_EQ(alt(), 0x50); CHECK_EQ(ata.unit[0].pio_mode, 4);
    wr(MC_ATA_FEATURES, 0x03); wr(MC_ATA_COUNT, 0x0D); command(0xEF); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    wr(MC_ATA_FEATURES, 0x03); wr(MC_ATA_COUNT, 0x22); command(0xEF); wait_idle();       /* multiword DMA 2 */
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    wr(MC_ATA_FEATURES, 0x55); command(0xEF); wait_idle();
    CHECK_EQ(alt(), 0x50);
    wr(MC_ATA_FEATURES, 0xAA); command(0xEF); wait_idle();
    wr(MC_ATA_FEATURES, 0x5E); command(0xEF); wait_idle();
    CHECK_EQ(alt(), 0x51);
}

/* Power commands (ATA-2 8.?): CHECK POWER MODE returns 00h standby, 80h
 * idle, FFh active in the count register; a media command after standby
 * waits for the spin-up; SLEEP ignores commands until a reset; FLUSH CACHE
 * completes with an interrupt. */
static void test_power(void) {
    select_dev(0, 0);
    command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0xFF);
    command(0xE3); wait_idle();
    command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0x80);
    command(0xE0); wait_idle();
    CHECK_EQ(alt(), 0x50); CHECK_EQ(irq(), 1);
    command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0x00);
    set_lba(130, 1, 0);
    command(0x20);
    step(MC_ATA_T_CMD + MC_ATA_T_FIRST + 100);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);                 /* spinning up */
    wait_idle(); read_block(0, 512);
    command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0xFF);
    command(0xE2); wait_idle(); command(0xE1); wait_idle(); command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0x80);
    command(0xE7); wait_idle();
    CHECK_EQ(alt(), 0x50); CHECK_EQ(irq(), 1);
    command(0xE6); wait_idle();
    CHECK_EQ(alt(), 0x50);
    rd(MC_ATA_STATUS);
    command(0xE5); step(100000);
    CHECK_EQ(irq(), 0);                              /* ignored while asleep */
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_SRST); wr(MC_ATA_DEV_CONTROL, 0); step(MC_ATA_T_RESET);
    command(0xE5); wait_idle();
    CHECK_EQ(rd(MC_ATA_COUNT), 0xFF);
    /* EXECUTE DEVICE DIAGNOSTIC: the signature, code 01h, an interrupt. */
    set_lba(9, 9, 0);
    command(0x90); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), 0x01); CHECK_EQ(rd(MC_ATA_COUNT), 1); CHECK_EQ(rd(MC_ATA_SECTOR), 1);
    CHECK_EQ(irq(), 1);
}

/* READ BUFFER (E4h) and WRITE BUFFER (E8h) keep one 512-byte sector. */
static void test_buffer(void) {
    uint8_t data[512];
    for (int i = 0; i < 512; i++) data[i] = (uint8_t)(i * 3 + 1);
    command(0xE8);
    step(MC_ATA_T_CMD);
    CHECK_EQ(alt(), 0x58);
    write_block(data, 512);
    wait_idle();
    CHECK_EQ(alt(), 0x50); CHECK_EQ(irq(), 1);
    command(0xE4); wait_idle();
    read_block(0, 512);
    CHECK(!memcmp(io, data, 512));
    CHECK_EQ(alt(), 0x50);
}

/* Errors: an unknown or unsupported command (DMA, long, 48-bit) aborts with
 * ABRT (04h), status 51h and an interrupt; reads past the end are IDNF with
 * the failing sector in the task file; commands during BSY or DRQ are
 * ignored. */
static void test_errors(void) {
    select_dev(0, 1);
    static const uint8_t bad[] = {0xC8, 0xCA, 0x25, 0x22, 0x24, 0xFF, 0xA0, 0xA1, 0x08, 0x00, 0xF8};
    for (unsigned i = 0; i < sizeof bad; i++) {
        rd(MC_ATA_STATUS);
        command(bad[i]); wait_idle();
        CHECK_EQ(alt(), 0x51);
        CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
        CHECK_EQ(irq(), 1);
    }
    uint32_t total = (uint32_t)mc_vdisk_blocks(vd);
    set_lba(total - 1, 3, 0);
    command(0x20); wait_idle();
    CHECK_EQ(alt(), 0x58);
    read_block(0, 512);
    wait_idle();
    CHECK_EQ(alt(), 0x51);
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    CHECK_EQ(rd(MC_ATA_SECTOR), (uint8_t)total);   /* the first sector beyond the end */
    set_lba(0x0FFFFFFF, 1, 0);
    command(0x20); wait_idle();
    CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_IDNF);
    /* A command written while BSY, and registers written while DRQ. */
    set_lba(130, 1, 0);
    command(0x20);
    command(0xEC);                       /* ignored: BSY */
    wr(MC_ATA_SECTOR, 7);
    wait_idle();
    CHECK_EQ(alt(), 0x58);
    command(0xE5);                       /* ignored: DRQ */
    wr(MC_ATA_SECTOR, 9);
    CHECK_EQ(rd(MC_ATA_SECTOR), 130 & 255);
    read_block(0, 512);
    uint8_t want[512];
    CHECK(mc_vdisk_read(vd, 130, 1, want));
    CHECK(!memcmp(io, want, 512));
    CHECK_EQ(alt(), 0x50);
}

/* Interrupts (7.2.7): nIEN negates the pin, which follows when it is
 * cleared; a status read and a command write clear INTRQ, the alternate
 * status does not; SRST during a transfer abandons it. */
static void test_interrupts(void) {
    set_lba(130, 2, 0);
    command(0x20);
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_NIEN);
    wait_idle();
    CHECK_EQ(irq(), 0);                  /* INTRQ is pending but not driven */
    wr(MC_ATA_DEV_CONTROL, 0);
    CHECK_EQ(irq(), 1);
    alt();
    CHECK_EQ(irq(), 1);                  /* the alternate status leaves it */
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_NIEN);
    CHECK_EQ(irq(), 0);
    rd(MC_ATA_STATUS);                   /* cleared while masked */
    wr(MC_ATA_DEV_CONTROL, 0);
    CHECK_EQ(irq(), 0);
    /* Abandon the transfer with SRST. */
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_SRST);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    wr(MC_ATA_DEV_CONTROL, 0);
    step(MC_ATA_T_RESET);
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(mc_ata_data_read16(&ata), 0);
    set_lba(130, 1, 0);
    command(0x20); wait_idle();
    CHECK_EQ(irq(), 1);
    command(0xE5);                       /* ignored while DRQ: INTRQ stays */
    CHECK_EQ(irq(), 1);
    read_block(0, 512);
    command(0xE5);                       /* a command write clears INTRQ */
    CHECK_EQ(irq(), 0);
    wait_idle();
    CHECK_EQ(irq(), 1);
    /* Hard reset clears everything, including nIEN. */
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_NIEN);
    mc_ata_reset(&ata);
    CHECK_EQ(irq(), 0);
    step(MC_ATA_T_RESET);
    command(0xE5); wait_idle();
    CHECK_EQ(irq(), 1);
    rd(MC_ATA_STATUS);
}

/* Reads through the ops without lane swapping give the ATA word. */
static void test_lanes(void) {
    ata.lane_swap = 0;
    command(0xEC); wait_idle();
    uint32_t w = mc_ata_ops.read(&ata, 0, 2);
    CHECK_EQ(w, 0x0040);
    for (int i = 0; i < 255; i++) mc_ata_ops.read(&ata, 0, 2);
    ata.lane_swap = 1;
    CHECK_EQ(alt(), 0x50);
}

/* The stable hook: a status read is stable when INTRQ is not asserted, the
 * data register never. */
static void test_stable(void) {
    rd(MC_ATA_STATUS);
    CHECK(mc_ata_ops.stable(&ata, 7u << 2, 1));
    CHECK(mc_ata_ops.stable(&ata, 14u << 2, 1));
    CHECK(!mc_ata_ops.stable(&ata, 0, 2));
    command(0xE7); wait_idle();
    CHECK(!mc_ata_ops.stable(&ata, 7u << 2, 1));
    CHECK(mc_ata_ops.stable(&ata, 14u << 2, 1));
    rd(MC_ATA_STATUS);
}

/* --- ATAPI --------------------------------------------------------------------------- */

static void on_eject(void *ctx, unsigned dev, mc_vdisk *media) { (void)ctx; CHECK_EQ(dev, 1); ejects++; ejected_media = media; }

static void atapi_setup(void) {
    mc_ata_attach_cd(&ata, 1);
    mc_ata_on_eject(&ata, on_eject, NULL);
    mc_ata_reset(&ata);
    step(MC_ATA_T_RESET);
    select_dev(1, 0);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Runs one packet command on device 1 with the byte count limit `limit`:
 * the command packet is requested with DRQ and CoD (interrupt reason 01h),
 * the data comes in chunks of at most the limit, each with DRQ, IO, the
 * count in the cylinder registers and an interrupt; the end has CoD|IO
 * (03h). Returns the final status; *got the bytes read (into io), *chunks
 * the number of data DRQs. */
static uint8_t packet(const uint8_t *cdb, unsigned limit, const uint8_t *out, uint32_t out_size, uint32_t *got, unsigned *chunks) {
    select_dev(1, 0);
    wr(MC_ATA_FEATURES, 0);
    wr(MC_ATA_CYL_LO, (uint8_t)limit);
    wr(MC_ATA_CYL_HI, (uint8_t)(limit >> 8));
    command(0xA0);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    wait_idle();
    CHECK_EQ(alt() & 0xC8, 0x48);
    CHECK_EQ(rd(MC_ATA_COUNT) & 3, MC_ATAPI_COD);
    CHECK_EQ(irq(), 0);                  /* microprocessor DRQ: no interrupt for the command packet */
    write_block(cdb, 12);
    CHECK_EQ(alt(), MC_ATA_ST_BSY);
    uint32_t n = 0, wrote = 0;
    unsigned c = 0;
    for (;;) {
        wait_idle();
        uint8_t s = alt();
        if (!(s & MC_ATA_ST_DRQ)) {
            CHECK_EQ(rd(MC_ATA_COUNT) & 3, MC_ATAPI_COD | MC_ATAPI_IO);
            CHECK_EQ(irq(), 1);
            rd(MC_ATA_STATUS);
            if (got) *got = n;
            if (chunks) *chunks = c;
            return s;
        }
        CHECK_EQ(irq(), 1);
        rd(MC_ATA_STATUS);
        uint32_t size = rd(MC_ATA_CYL_LO) | rd(MC_ATA_CYL_HI) << 8;
        CHECK(size <= (limit ? limit : 0xFFFEu));
        if (rd(MC_ATA_COUNT) & MC_ATAPI_IO) {
            read_block(n, (size + 1) & ~1u);
            n += size;
        } else {
            CHECK_EQ(rd(MC_ATA_COUNT) & 3, 0);
            CHECK(out != NULL && wrote + size <= out_size);
            write_block(out + wrote, size);
            wrote += size;
        }
        c++;
    }
}

static void test_atapi_basics(void) {
    atapi_setup();
    /* After reset: the packet signature, status 00h, error 01h (SFF-8020i). */
    CHECK_EQ(rd(MC_ATA_STATUS), 0x00);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0x14); CHECK_EQ(rd(MC_ATA_CYL_HI), 0xEB);
    CHECK_EQ(rd(MC_ATA_COUNT), 1); CHECK_EQ(rd(MC_ATA_SECTOR), 1);
    CHECK_EQ(rd(MC_ATA_ERROR), 1);
    /* The master is still the ATA disk with its own signature. */
    select_dev(0, 0);
    CHECK_EQ(rd(MC_ATA_CYL_HI), 0x00);
    CHECK_EQ(rd(MC_ATA_STATUS), 0x50);
    select_dev(1, 0);
    /* IDENTIFY DEVICE aborts and leaves the signature; IDENTIFY PACKET DEVICE answers. */
    command(0xEC); wait_idle();
    CHECK_EQ(alt(), 0x51); CHECK_EQ(rd(MC_ATA_ERROR), MC_ATA_ER_ABRT);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0x14); CHECK_EQ(rd(MC_ATA_CYL_HI), 0xEB);
    rd(MC_ATA_STATUS);
    command(0x20); wait_idle();
    CHECK_EQ(alt(), 0x51);
    command(0xA1); wait_idle();
    CHECK_EQ(alt() & 0x58, 0x58);
    read_block(0, 512);
    CHECK_EQ(word(0), 0x8580);
    char text[64];
    field(27, 20, text);
    CHECK(!strncmp(text, "LIVADIX VIRTUAL CD-ROM", 19));
    CHECK_EQ(word(49) & 0x0200, 0x0200);
    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += io[i];
    CHECK_EQ(sum & 0xFF, 0);
    /* DEVICE RESET (08h): the signature again, no interrupt. */
    rd(MC_ATA_STATUS);
    wr(MC_ATA_CYL_LO, 0x55);
    command(0x08); step(1000);
    CHECK_EQ(alt(), 0x50);
    CHECK_EQ(rd(MC_ATA_CYL_LO), 0x14);
    CHECK_EQ(irq(), 0);
    /* EXECUTE DEVICE DIAGNOSTIC. */
    command(0x90); wait_idle();
    CHECK_EQ(rd(MC_ATA_CYL_HI), 0xEB); CHECK_EQ(rd(MC_ATA_ERROR), 1);
    /* SET FEATURES with a PIO mode; DMA in a packet command aborts. */
    wr(MC_ATA_FEATURES, 0x03); wr(MC_ATA_COUNT, 0x0B); command(0xEF); wait_idle();
    CHECK_EQ(alt(), 0x50);
    wr(MC_ATA_FEATURES, 1); wr(MC_ATA_CYL_LO, 0); wr(MC_ATA_CYL_HI, 2); command(0xA0); wait_idle();
    CHECK_EQ(alt(), 0x51);
    wr(MC_ATA_FEATURES, 0);
}

static void test_atapi_commands(void) {
    uint32_t got;
    unsigned chunks;
    uint8_t s;
    /* TEST UNIT READY with no disc: CHECK CONDITION, sense key NOT READY in the error register. */
    const uint8_t tur[12] = {0x00};
    s = packet(tur, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1);
    CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_NOT_READY);
    /* REQUEST SENSE: the key, ASC 3Ah (medium not present). */
    const uint8_t sense[12] = {0x03, 0, 0, 0, 18};
    s = packet(sense, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50);
    CHECK_EQ(got, 18); CHECK_EQ(chunks, 1);
    CHECK_EQ(io[2] & 15, MC_SENSE_NOT_READY); CHECK_EQ(io[12], 0x3A);
    /* INQUIRY: CD-ROM, Livadix's strings; the allocation length cuts it. */
    const uint8_t inq[12] = {0x12, 0, 0, 0, 36};
    s = packet(inq, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 36); CHECK_EQ(io[0] & 31, 5); CHECK_EQ(io[1], 0x80);
    CHECK(!memcmp(io + 16, "VIRTUAL CD-ROM  ", 16));
    /* A byte count limit below the response: 36 bytes in chunks of 16. */
    s = packet(inq, 16, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 36); CHECK_EQ(chunks, 3);
    CHECK_EQ(io[0] & 31, 5);
    CHECK(!memcmp(io + 8, "LIVADIX VIRTUAL CD-ROM  ", 24));
    /* Insert a disc: the first command reports the unit attention (key 6). */
    CHECK(mc_ata_cd_insert(&ata, 1, vcd));
    CHECK(!mc_ata_cd_insert(&ata, 1, vcd));
    s = packet(tur, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1); CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_UNIT_ATTENTION);
    s = packet(sense, 0x200, NULL, 0, &got, &chunks);       /* the attention was taken by the failed TUR */
    CHECK_EQ(io[2] & 15, MC_SENSE_UNIT_ATTENTION); CHECK_EQ(io[12], 0x28);
    s = packet(tur, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50);
    CHECK_EQ(chunks, 0);
    /* READ CAPACITY: 2048-byte sectors (the disc is 64 of them plus the 128-block head, 2048-aligned). */
    const uint8_t cap[12] = {0x25};
    s = packet(cap, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 8);
    uint32_t blocks = (uint32_t)mc_vdisk_blocks(vcd) / 4;
    CHECK_EQ(be32(io), blocks - 1); CHECK_EQ(be32(io + 4), 2048);
    /* READ(10) of 3 sectors: 6144 bytes in chunks of 2048 (limit 0800h), then of 512. */
    const uint8_t rd10[12] = {0x28, 0, 0, 0, 0, 33, 0, 0, 3};
    uint8_t want[6144];
    CHECK(mc_vdisk_read(vcd, 33 * 4, 12, want));
    s = packet(rd10, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(got, 6144); CHECK_EQ(chunks, 3);
    CHECK(!memcmp(io, want, 6144));
    s = packet(rd10, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 6144); CHECK_EQ(chunks, 12);
    CHECK(!memcmp(io, want, 6144));
    s = packet(rd10, 0xFFFE, NULL, 0, &got, &chunks);          /* one chunk of the whole transfer */
    CHECK_EQ(got, 6144); CHECK_EQ(chunks, 1);
    CHECK(!memcmp(io, want, 6144));
    s = packet(rd10, 0, NULL, 0, &got, &chunks);               /* 0: the largest */
    CHECK_EQ(chunks, 1);
    s = packet(rd10, 0x100, NULL, 0, &got, &chunks);           /* a limit below a sector: pieces of 256 */
    CHECK_EQ(got, 6144); CHECK_EQ(chunks, 24);
    CHECK(!memcmp(io, want, 6144));
    /* READ CD of user data (byte 9 = 10h): the same 2048-byte sectors; no
     * field asked: no data; headers or a CD-DA sector type: refused. */
    const uint8_t rcd[12] = {0xBE, 0, 0, 0, 0, 33, 0, 0, 3, 0x10};
    s = packet(rcd, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(got, 6144);
    CHECK(!memcmp(io, want, 6144));
    const uint8_t rcd0[12] = {0xBE, 0, 0, 0, 0, 33, 0, 0, 3, 0};
    s = packet(rcd0, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(got, 0);
    const uint8_t rcdh[12] = {0xBE, 0, 0, 0, 0, 33, 0, 0, 3, 0x30};
    s = packet(rcdh, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1); CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_ILLEGAL_REQUEST);
    const uint8_t rcda[12] = {0xBE, 0x04, 0, 0, 0, 33, 0, 0, 3, 0x10};
    s = packet(rcda, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1); CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_ILLEGAL_REQUEST);
    /* MODE SENSE(10) of page 2Ah: no block descriptor, the 20-byte page
     * (Apple CD-ROM 5.4 asks for 1Ch bytes). */
    const uint8_t ms2a[12] = {0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 0x1C};
    s = packet(ms2a, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(got, 0x1C);
    CHECK_EQ(io[6] << 8 | io[7], 0); CHECK_EQ(io[8], 0x2A); CHECK_EQ(io[9], 0x12);
    CHECK_EQ(io[8 + 6] & 0x09, 0x09); CHECK_EQ(io[8 + 8] << 8 | io[8 + 9], 706);
    /* READ(12), and a read past the end: ILLEGAL REQUEST, ABRT in the error register. */
    const uint8_t rd12[12] = {0xA8, 0, 0, 0, 0, 2, 0, 0, 0, 1};
    s = packet(rd12, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 2048);
    CHECK(mc_vdisk_read(vcd, 8, 4, want));
    CHECK(!memcmp(io, want, 2048));
    const uint8_t past[12] = {0x28, 0, 0, 0, 0, 200, 0, 0, 1};
    s = packet(past, 0x800, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1); CHECK_EQ(chunks, 0);
    CHECK_EQ(rd(MC_ATA_ERROR), (MC_SENSE_ILLEGAL_REQUEST << 4) | MC_ATA_ER_ABRT);
    /* READ TOC: first and last track, lead-out. */
    const uint8_t toc[12] = {0x43, 0, 0, 0, 0, 0, 0, 0, 20};
    s = packet(toc, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 20); CHECK_EQ(io[2], 1); CHECK_EQ(io[3], 1); CHECK_EQ(io[4 + 2], 1); CHECK_EQ(io[12 + 2], 0xAA);
    /* MODE SENSE(10): no block descriptor on an ATAPI drive (SFF-8020i),
     * the pages from byte 8. */
    const uint8_t ms[12] = {0x5A, 0, 0x3F, 0, 0, 0, 0, 0, 16};
    s = packet(ms, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 16); CHECK_EQ(io[6] << 8 | io[7], 0); CHECK_EQ(io[8], 0x01);
    /* MODE SELECT(10): the block length switches to 512 (data out), then READ CAPACITY and READ(10) follow it. */
    const uint8_t msel[12] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 16};
    uint8_t params[16] = {0, 0, 0, 0, 0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 2, 0};
    s = packet(msel, 0x200, params, 16, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(chunks, 1);
    s = packet(cap, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(be32(io + 4), 512);
    CHECK_EQ(be32(io), (uint32_t)mc_vdisk_blocks(vcd) - 1);
    const uint8_t rd512[12] = {0x28, 0, 0, 0, 0, 5, 0, 0, 2};
    s = packet(rd512, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(got, 1024); CHECK_EQ(chunks, 2);
    CHECK(mc_vdisk_read(vcd, 5, 2, want));
    CHECK(!memcmp(io, want, 1024));
    /* A reset puts the drive back to 2048-byte sectors. */
    wr(MC_ATA_DEV_CONTROL, MC_ATA_DC_SRST); wr(MC_ATA_DEV_CONTROL, 0); step(MC_ATA_T_RESET);
    select_dev(1, 0);
    s = packet(cap, 0x200, NULL, 0, &got, &chunks);
    s = packet(cap, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(be32(io + 4), 2048);
    /* An invalid opcode: ILLEGAL REQUEST. */
    const uint8_t inv[12] = {0xEE};
    s = packet(inv, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1);
    CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_ILLEGAL_REQUEST);
    /* PREVENT ALLOW MEDIUM REMOVAL, then START STOP UNIT (eject) is refused; after ALLOW it ejects. */
    const uint8_t prevent[12] = {0x1E, 0, 0, 0, 1};
    const uint8_t allow[12] = {0x1E, 0, 0, 0, 0};
    const uint8_t eject[12] = {0x1B, 0, 0, 0, 0x02};
    s = packet(prevent, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50);
    s = packet(eject, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s & 1, 1); CHECK_EQ(ejects, 0);
    s = packet(allow, 0x200, NULL, 0, &got, &chunks);
    s = packet(eject, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(s, 0x50); CHECK_EQ(ejects, 1); CHECK(ejected_media == vcd);
    s = packet(tur, 0x200, NULL, 0, &got, &chunks);
    CHECK_EQ(rd(MC_ATA_ERROR) >> 4, MC_SENSE_NOT_READY);
    CHECK(mc_ata_cd_remove(&ata, 1) == NULL);
    CHECK(mc_ata_cd_insert(&ata, 1, vcd));
    CHECK(mc_ata_cd_remove(&ata, 1) == vcd);
    /* The ATA disk keeps working beside it (master and slave). */
    select_dev(0, 1);
    set_lba(130, 1, 0);
    command(0x20); wait_idle(); read_block(0, 512);
    CHECK(mc_vdisk_read(vd, 130, 1, want));
    CHECK(!memcmp(io, want, 512));
}

/* A drive with the CD as device 0 and no slave (the other wiring). */
static void test_cd_only(void) {
    mc_ata cd_only;
    hw_fake f2;
    hw_fake_init(&f2);
    mc_ata_init(&cd_only, &f2.host, 2);
    mc_ata_attach_cd(&cd_only, 0);
    mc_ata_reset(&cd_only);
    hw_fake_step(&f2, MC_ATA_T_RESET);
    CHECK_EQ(mc_ata_reg_read(&cd_only, MC_ATA_CYL_HI, 1), 0xEB);
    mc_ata_reg_write(&cd_only, MC_ATA_DRIVE, 1, 0xB0);
    CHECK_EQ(mc_ata_reg_read(&cd_only, MC_ATA_STATUS, 1), 0);
}

int main(void) {
    int tests = 0;
    setup();
#define RUN(t) (t(), tests++)
    RUN(test_reset_signature);
    RUN(test_task_file);
    RUN(test_identify);
    RUN(test_read_lba);
    RUN(test_chs);
    RUN(test_write);
    RUN(test_multiple);
    RUN(test_verify_seek);
    RUN(test_init_params);
    RUN(test_set_features);
    RUN(test_power);
    RUN(test_buffer);
    RUN(test_errors);
    RUN(test_interrupts);
    RUN(test_lanes);
    RUN(test_stable);
    RUN(test_atapi_basics);
    RUN(test_atapi_commands);
    RUN(test_cd_only);
    mc_vdisk_destroy(vd);
    mc_vdisk_destroy(vd_ro);
    mc_vdisk_destroy(vcd);
    mc_disk_close(img);
    mc_disk_close(img_ro);
    mc_disk_close(iso);
    printf("test_ata: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
