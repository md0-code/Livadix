/* The SCSI disk and CD-ROM targets over the bus, against SCSI-2 (X3.131-1994):
 * expected values from the command and data descriptions cited per test. */
#include "scsi_util.h"
#include "scsi_target.h"

#include <string.h>

static mc_scsi_bus bus;
static mc_scsi_unit disk, cd;
static mc_disk *img, *iso;
static mc_vdisk *vdisk, *vcd;
static uint8_t buf[256 * 512 + 512];
static const uint8_t driver[1000] = {0x60, 0x06};     /* a stand-in driver image */

static scsi_result run(int id, const uint8_t *cdb, unsigned n) { return scsi_run(&bus, id, cdb, n, buf, sizeof buf, NULL, 0); }
static scsi_result run_out(int id, const uint8_t *cdb, unsigned n, const uint8_t *out, uint32_t size) {
    return scsi_run(&bus, id, cdb, n, buf, sizeof buf, out, size);
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* REQUEST SENSE (8.2.14): key, ASC, ASCQ of the last CHECK CONDITION. */
static void sense(int id, uint8_t *key, uint8_t *asc, uint8_t *ascq) {
    const uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    scsi_result r = run(id, cdb, 6);
    *key = buf[2] & 15;
    *asc = buf[12];
    *ascq = buf[13];
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 18 && buf[0] == 0x70);
}

static void setup(void) {
    char error[200];
    const char *path = scsi_make_image("scsi_target", "disk.img", 1024 * 512, 0x11);
    img = mc_disk_open(path, 1, 512, error, sizeof error);
    CHECK(img != NULL);
    path = scsi_make_image("scsi_target", "cd.iso", 64 * 2048, 0x22);
    iso = mc_disk_open(path, 0, 2048, error, sizeof error);
    CHECK(iso != NULL);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    vdisk = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    vcd = mc_vdisk_create(&iso, 1, &d, error, sizeof error);
    CHECK(vdisk && vcd);
    mc_scsi_bus_init(&bus);
    mc_scsi_disk_init(&disk, vdisk);
    mc_scsi_cd_init(&cd);
    mc_scsi_bus_attach(&bus, 0, mc_scsi_unit_target(&disk));
    mc_scsi_bus_attach(&bus, 3, mc_scsi_unit_target(&cd));
}

/* INQUIRY standard data (8.2.5.1, Table 45): type, RMB, version, format,
 * additional length, and Livadix's identification strings. */
static void test_inquiry(void) {
    const uint8_t cdb[6] = {0x12, 0, 0, 0, 36, 0};
    scsi_result r = run(0, cdb, 6);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 36);
    CHECK_EQ(buf[0], 0x00); CHECK_EQ(buf[1], 0x00); CHECK_EQ(buf[2], 2); CHECK_EQ(buf[3], 2); CHECK_EQ(buf[4], 31);
    CHECK(!memcmp(buf + 8, "LIVADIX VIRTUAL DISK    ", 24));
    r = run(3, cdb, 6);
    CHECK_EQ(buf[0], 0x05); CHECK_EQ(buf[1], 0x80);
    CHECK(!memcmp(buf + 16, "VIRTUAL CD-ROM  ", 16));
    /* Allocation length cuts the transfer. */
    const uint8_t shortc[6] = {0x12, 0, 0, 0, 5, 0};
    r = run(0, shortc, 6);
    CHECK_EQ(r.moved, 5);
    /* LUN 1 in the CDB (no unit there): qualifier 3, type 1Fh (8.2.5.1). */
    const uint8_t lun1[6] = {0x12, 0x20, 0, 0, 36, 0};
    r = run(0, lun1, 6);
    CHECK_EQ(buf[0], 0x7F);
    /* EVPD: no vital product data pages: ILLEGAL REQUEST, invalid field in CDB. */
    const uint8_t evpd[6] = {0x12, 1, 0, 0, 36, 0};
    r = run(0, evpd, 6);
    CHECK_EQ(r.status, MC_SCSI_CHECK_CONDITION);
    uint8_t k, a, q;
    sense(0, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_ILLEGAL_REQUEST); CHECK_EQ(a, 0x24);
}

/* READ CAPACITY (9.2.7): last block address and block length. The
 * synthesized disk is 128 head blocks plus the image's 1024. */
static void test_capacity(void) {
    const uint8_t cdb[10] = {0x25};
    scsi_result r = run(0, cdb, 10);
    CHECK(r.ok && r.moved == 8);
    CHECK_EQ(be32(buf), 128 + 1024 - 1);
    CHECK_EQ(be32(buf + 4), 512);
    const uint8_t tur[6] = {0};
    CHECK(run(0, tur, 6).status == MC_SCSI_GOOD);
}

/* READ(6)/READ(10)/WRITE(10) (9.2.5, 9.2.6, 9.2.21): blocks of the image
 * appear at 128 + n; writes land in the image. */
static void test_read_write(void) {
    const uint8_t r6[6] = {0x08, 0, 0, 130, 2, 0};
    scsi_result r = run(0, r6, 6);
    CHECK(r.ok && r.moved == 1024);
    CHECK_EQ(be32(buf), 2); CHECK_EQ(buf[4], 0x11); CHECK_EQ(be32(buf + 512), 3);
    /* A transfer length of 0 in READ(6) means 256 blocks. */
    const uint8_t r256[6] = {0x08, 0, 0, 128, 0, 0};
    r = run(0, r256, 6);
    CHECK_EQ(r.moved, 256 * 512);
    CHECK_EQ(be32(buf + 255 * 512), 255);
    /* READ(10) of 130 blocks crosses a 64 KiB chunk of the bus. */
    const uint8_t r10[10] = {0x28, 0, 0, 0, 0, 128, 0, 0, 130, 0};
    r = run(0, r10, 10);
    CHECK(r.ok && r.moved == 130 * 512);
    CHECK_EQ(be32(buf + 129 * 512), 129);
    uint8_t data[1024];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7);
    const uint8_t w10[10] = {0x2A, 0, 0, 0, 0, 200, 0, 0, 2, 0};
    r = run_out(0, w10, 10, data, sizeof data);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 1024);
    uint8_t back[1024];
    CHECK(mc_disk_read(img, 72, 2, back) && !memcmp(back, data, sizeof back));
    /* WRITE(6) and READ back. */
    const uint8_t w6[6] = {0x0A, 0, 0, 201, 1, 0};
    data[0] = 0xEE;
    r = run_out(0, w6, 6, data, 512);
    CHECK(r.ok);
    CHECK(mc_disk_read(img, 73, 1, back) && back[0] == 0xEE);
}

/* Out of range (9.2.6; sense 8.2.14.3 Table 71 ASC 21h) with the
 * information field holding the LBA. */
static void test_range(void) {
    const uint8_t r10[10] = {0x28, 0, 0, 0, 0x04, 0x7F, 0, 0, 2, 0};
    scsi_result r = run(0, r10, 10);
    CHECK_EQ(r.status, MC_SCSI_CHECK_CONDITION);
    CHECK_EQ(r.moved, 0);
    const uint8_t cdb[6] = {0x03, 0, 0, 0, 18, 0};
    run(0, cdb, 6);
    CHECK_EQ(buf[0], 0xF0);     /* valid, current error */
    CHECK_EQ(buf[2], MC_SENSE_ILLEGAL_REQUEST);
    CHECK_EQ(be32(buf + 3), 0x47F);
    CHECK_EQ(buf[7], 10);
    CHECK_EQ(buf[12], 0x21);
    /* Sense is cleared by REQUEST SENSE; an allocation length of zero moves
     * nothing (8.2.14). */
    uint8_t k, a, q;
    sense(0, &k, &a, &q);
    CHECK_EQ(k, 0); CHECK_EQ(a, 0);
    const uint8_t zero[6] = {0x03, 0, 0, 0, 0, 0};
    r = run(0, zero, 6);
    CHECK(r.ok && r.moved == 0);
    /* An unknown operation code: ASC 20h. */
    const uint8_t bad[10] = {0x3C, 0, 0, 0, 0, 0, 0, 0, 0, 0};   /* READ BUFFER: not implemented */
    r = run(0, bad, 10);
    CHECK_EQ(r.status, MC_SCSI_CHECK_CONDITION);
    sense(0, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_ILLEGAL_REQUEST); CHECK_EQ(a, 0x20);
}

/* MODE SENSE(6)/(10) (8.2.10, 8.2.11): header, block descriptor (Table 93),
 * pages 01h, 03h, 04h, 08h with their page lengths (9.3.3). */
static void test_mode_sense(void) {
    const uint8_t all[6] = {0x1A, 0, 0x3F, 0, 255, 0};
    scsi_result r = run(0, all, 6);
    CHECK(r.ok);
    CHECK_EQ(buf[0] + 1u, r.moved);
    CHECK_EQ(buf[3], 8);
    CHECK_EQ(buf[4 + 1] << 16 | buf[4 + 2] << 8 | buf[4 + 3], 1152);
    CHECK_EQ(buf[4 + 5] << 16 | buf[4 + 6] << 8 | buf[4 + 7], 512);
    CHECK_EQ(r.moved, 4 + 8 + 12 + 24 + 24 + 12);
    const uint8_t *p = buf + 12;
    CHECK_EQ(p[0], 0x01); CHECK_EQ(p[1], 0x0A);
    p += 12;
    CHECK_EQ(p[0], 0x03); CHECK_EQ(p[1], 0x16); CHECK_EQ(p[12] << 8 | p[13], 512);
    p += 24;
    CHECK_EQ(p[0], 0x04); CHECK_EQ(p[1], 0x16); CHECK_EQ(p[5], 16);
    p += 24;
    CHECK_EQ(p[0], 0x08); CHECK_EQ(p[1], 0x0A);
    /* DBD and a single page; MODE SENSE(10) header. */
    const uint8_t one[10] = {0x5A, 0x08, 0x04, 0, 0, 0, 0, 0, 255, 0};
    r = run(0, one, 10);
    CHECK_EQ(r.moved, 8 + 24);
    CHECK_EQ(buf[0] << 8 | buf[1], 8 + 24 - 2);
    CHECK_EQ(buf[6] << 8 | buf[7], 0);
    CHECK_EQ(buf[8], 0x04);
    /* Saved values are not supported: ASC 39h. An unknown page: 24h. */
    const uint8_t saved[6] = {0x1A, 0, 0xC1, 0, 255, 0};
    CHECK_EQ(run(0, saved, 6).status, MC_SCSI_CHECK_CONDITION);
    uint8_t k, a, q;
    sense(0, &k, &a, &q);
    CHECK_EQ(a, 0x39);
    const uint8_t unknown[6] = {0x1A, 0, 0x2A, 0, 255, 0};
    CHECK_EQ(run(0, unknown, 6).status, MC_SCSI_CHECK_CONDITION);
}

/* MODE SELECT (8.2.8): a disk keeps 512-byte blocks; another length is an
 * invalid field in the parameter list (26h). */
static void test_mode_select_disk(void) {
    const uint8_t cdb[6] = {0x15, 0x10, 0, 0, 12, 0};
    uint8_t params[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00};
    CHECK_EQ(run_out(0, cdb, 6, params, 12).status, MC_SCSI_GOOD);
    params[10] = 0x04;
    CHECK_EQ(run_out(0, cdb, 6, params, 12).status, MC_SCSI_CHECK_CONDITION);
    uint8_t k, a, q;
    sense(0, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_ILLEGAL_REQUEST); CHECK_EQ(a, 0x26);
}

static int ejected;
static void on_eject(void *ctx, mc_scsi_unit *u, mc_vdisk *media) {
    (void)ctx; (void)u;
    CHECK(media == vcd);
    ejected++;
}

/* The CD: no disc (NOT READY, 3Ah), insertion's unit attention (28h, 7.9),
 * 512-byte blocks (the Quadra 700 ROM's boot), the switch to 2048 by MODE
 * SELECT, read-only (DATA PROTECT, 27h), READ TOC (14.2.11), PREVENT/ALLOW
 * and eject (9.2.4, 9.2.17). */
static void test_cd(void) {
    const uint8_t tur[6] = {0};
    uint8_t k, a, q;
    CHECK_EQ(run(3, tur, 6).status, MC_SCSI_CHECK_CONDITION);
    sense(3, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_NOT_READY); CHECK_EQ(a, 0x3A);
    CHECK(mc_scsi_cd_insert(&cd, vcd));
    CHECK_EQ(run(3, tur, 6).status, MC_SCSI_CHECK_CONDITION);
    sense(3, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_UNIT_ATTENTION); CHECK_EQ(a, 0x28);
    CHECK_EQ(run(3, tur, 6).status, MC_SCSI_GOOD);
    const uint8_t cap[10] = {0x25};
    run(3, cap, 10);
    CHECK_EQ(be32(buf + 4), 512);
    CHECK_EQ(be32(buf), 128 + 256 - 1);
    const uint8_t r6[6] = {0x08, 0, 0, 129, 1, 0};
    scsi_result r = run(3, r6, 6);
    CHECK_EQ(r.moved, 512); CHECK_EQ(be32(buf), 1);
    /* MODE SELECT to 2048-byte blocks. */
    const uint8_t sel[6] = {0x15, 0x10, 0, 0, 12, 0};
    const uint8_t params[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x08, 0x00};
    CHECK_EQ(run_out(3, sel, 6, params, 12).status, MC_SCSI_GOOD);
    run(3, cap, 10);
    CHECK_EQ(be32(buf), (128 * 512 + 64 * 2048) / 2048 - 1);
    CHECK_EQ(be32(buf + 4), 2048);
    /* Block 32 (2048 bytes) holds 512-byte blocks 128-131: the image's first. */
    const uint8_t r10[10] = {0x28, 0, 0, 0, 0, 32, 0, 0, 1, 0};
    r = run(3, r10, 10);
    CHECK_EQ(r.moved, 2048);
    CHECK_EQ(be32(buf), 0); CHECK_EQ(buf[4], 0x22); CHECK_EQ(be32(buf + 1536), 3);
    /* Block 0 is the DDM in either mode. */
    const uint8_t b0[6] = {0x08, 0, 0, 0, 1, 0};
    run(3, b0, 6);
    CHECK_EQ(buf[0], 0x45); CHECK_EQ(buf[1], 0x52);
    /* Back to 512 for the rest. */
    const uint8_t params512[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00};
    CHECK_EQ(run_out(3, sel, 6, params512, 12).status, MC_SCSI_GOOD);
    /* Writes are refused. */
    const uint8_t w[10] = {0x2A, 0, 0, 0, 0, 200, 0, 0, 1, 0};
    r = run_out(3, w, 10, buf, 512);
    CHECK_EQ(r.status, MC_SCSI_CHECK_CONDITION); CHECK_EQ(r.moved, 0);
    sense(3, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_DATA_PROTECT); CHECK_EQ(a, 0x27);
    /* READ TOC in LBA form: header, track 1 at 0, lead-out AAh at the
     * capacity (in current 512-byte blocks); in MSF, 00:02:00. */
    const uint8_t toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0};
    r = run(3, toc, 10);
    CHECK_EQ(r.moved, 20);
    CHECK_EQ(buf[0] << 8 | buf[1], 18); CHECK_EQ(buf[2], 1); CHECK_EQ(buf[3], 1);
    CHECK_EQ(buf[5], 0x14); CHECK_EQ(buf[6], 1); CHECK_EQ(be32(buf + 8), 0);
    CHECK_EQ(buf[14], 0xAA); CHECK_EQ(be32(buf + 16), 384);
    const uint8_t tocm[10] = {0x43, 2, 0, 0, 0, 0, 0, 0, 100, 0};
    run(3, tocm, 10);
    CHECK_EQ(buf[9], 0); CHECK_EQ(buf[10], 2); CHECK_EQ(buf[11], 0);
    CHECK_EQ(buf[17], 0); CHECK_EQ(buf[18], 3); CHECK_EQ(buf[19], 21);    /* 96 sectors + 150 = 246 frames: 00:03:21 */
    /* MODE SENSE reports write protection; page 0Dh exists on the CD. */
    const uint8_t ms[6] = {0x1A, 0, 0x0D, 0, 255, 0};
    r = run(3, ms, 6);
    CHECK_EQ(buf[2], 0x80); CHECK_EQ(buf[12], 0x0D);
    /* A bus reset returns the block length to 512. */
    CHECK_EQ(run_out(3, sel, 6, params, 12).status, MC_SCSI_GOOD);
    mc_scsi_bus_reset(&bus);
    run(3, cap, 10);
    CHECK_EQ(be32(buf + 4), 512);
    /* Eject refused while prevented, then done. */
    mc_scsi_unit_on_eject(&cd, on_eject, NULL);
    const uint8_t prevent[6] = {0x1E, 0, 0, 0, 1, 0}, allow[6] = {0x1E, 0, 0, 0, 0, 0}, eject[6] = {0x1B, 0, 0, 0, 2, 0};
    CHECK_EQ(run(3, prevent, 6).status, MC_SCSI_GOOD);
    CHECK_EQ(run(3, eject, 6).status, MC_SCSI_CHECK_CONDITION);
    sense(3, &k, &a, &q);
    CHECK_EQ(a, 0x53); CHECK_EQ(q, 0x02);
    CHECK_EQ(run(3, allow, 6).status, MC_SCSI_GOOD);
    CHECK_EQ(run(3, eject, 6).status, MC_SCSI_GOOD);
    CHECK_EQ(ejected, 1);
    CHECK(cd.media == NULL);
    CHECK_EQ(run(3, tur, 6).status, MC_SCSI_CHECK_CONDITION);
}

/* An Apple CD (its driver descriptor record names 2048-byte blocks, its map
 * and drivers lie in them) is read in 2048-byte blocks from power-on, after
 * a reset and with a new disc, as the Quadra 700 ROM boots it (no MODE
 * SELECT; READ(6) of 5 blocks at block 16 is 10240 bytes). */
static void test_cd_apple_block(void) {
    char error[200];
    const char *path = scsi_make_image("scsi_target", "apple.iso", 64 * 2048, 0x33);
    FILE *f = fopen(path, "r+b");
    CHECK(f != NULL);
    const uint8_t ddm[4] = {'E', 'R', 0x08, 0x00};
    fwrite(ddm, 1, sizeof ddm, f);
    fclose(f);
    mc_disk *disc = mc_disk_open(path, 0, 2048, error, sizeof error);
    CHECK(disc != NULL);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    mc_vdisk *vapple = mc_vdisk_create(&disc, 1, &d, error, sizeof error);
    CHECK(vapple != NULL);
    mc_scsi_unit apple;
    mc_scsi_cd_init(&apple);
    mc_scsi_bus_attach(&bus, 1, mc_scsi_unit_target(&apple));
    CHECK(mc_scsi_cd_insert(&apple, vapple));
    const uint8_t tur[6] = {0}, cap[10] = {0x25};
    run(1, tur, 6);                                     /* the unit attention */
    run(1, cap, 10);
    CHECK_EQ(be32(buf + 4), 2048);
    const uint8_t r6[6] = {0x08, 0, 0, 16, 5, 0};
    scsi_result r = run(1, r6, 6);
    CHECK(r.ok); CHECK_EQ(r.moved, 5 * 2048);
    const uint8_t sel[6] = {0x15, 0x10, 0, 0, 12, 0};
    const uint8_t params512[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x02, 0x00};
    CHECK_EQ(run_out(1, sel, 6, params512, 12).status, MC_SCSI_GOOD);
    run(1, cap, 10);
    CHECK_EQ(be32(buf + 4), 512);
    mc_scsi_bus_reset(&bus);
    run(1, cap, 10);
    CHECK_EQ(be32(buf + 4), 2048);
    /* A disc without that record (the virtual map of a bare image) is 512. */
    mc_scsi_unit plain;
    mc_scsi_cd_init(&plain);
    mc_scsi_bus_attach(&bus, 2, mc_scsi_unit_target(&plain));
    CHECK(mc_scsi_cd_insert(&plain, vcd));
    run(2, tur, 6);
    run(2, cap, 10);
    CHECK_EQ(be32(buf + 4), 512);
}

/* FORMAT UNIT with a parameter list header, VERIFY with BytChk, SEEK,
 * SYNCHRONIZE CACHE, START STOP on a disk: accepted and good. */
static void test_misc(void) {
    const uint8_t fmt[6] = {0x04, 0x10, 0, 0, 0, 0};
    const uint8_t header[4] = {0};
    scsi_result r = run_out(0, fmt, 6, header, 4);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 4);
    const uint8_t verify[10] = {0x2F, 0x02, 0, 0, 0, 0, 0, 0, 1, 0};
    r = run_out(0, verify, 10, buf, 512);
    CHECK(r.ok && r.moved == 512);
    const uint8_t seek[10] = {0x2B, 0, 0, 0, 1, 0, 0, 0, 0, 0}, sync[10] = {0x35}, stop[6] = {0x1B, 0, 0, 0, 2, 0};
    CHECK_EQ(run(0, seek, 10).status, MC_SCSI_GOOD);
    CHECK_EQ(run(0, sync, 10).status, MC_SCSI_GOOD);
    CHECK_EQ(run(0, stop, 6).status, MC_SCSI_GOOD);
    CHECK(disk.media == vdisk);
}

/* The bus: arbitration on a busy bus fails; selecting an empty ID fails;
 * an SDTR extended message gets MESSAGE REJECT (5.6.13, 5.6.21) and the
 * target returns to MESSAGE OUT/COMMAND; ABORT frees the bus (5.6.1);
 * a target selected without ATN goes straight to COMMAND (5.1.3). */
static void test_bus(void) {
    CHECK(mc_scsi_bus_arbitrate(&bus, 7));
    CHECK(!mc_scsi_bus_arbitrate(&bus, 6));
    CHECK(!mc_scsi_bus_select(&bus, 5, 0));
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_BUS_FREE);
    CHECK(mc_scsi_bus_arbitrate(&bus, 7) && mc_scsi_bus_select(&bus, 0, 0));
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_COMMAND);
    mc_scsi_bus_release(&bus);
    CHECK(mc_scsi_bus_arbitrate(&bus, 7) && mc_scsi_bus_select(&bus, 0, 1));
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_MSG_OUT);
    mc_scsi_bus_out(&bus, 0x80);            /* IDENTIFY with ATN still asserted: more messages */
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_MSG_OUT);
    const uint8_t sdtr[5] = {0x01, 3, 0x01, 25, 8};
    for (int i = 0; i < 5; i++) {
        if (i == 4) mc_scsi_bus_set_atn(&bus, 0);
        mc_scsi_bus_out(&bus, sdtr[i]);
    }
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_MSG_IN);
    CHECK_EQ(mc_scsi_bus_in(&bus, 1), MC_SCSI_MSG_REJECT);
    CHECK(!mc_scsi_bus_req(&bus));          /* ACK held */
    mc_scsi_bus_release_ack(&bus);
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_COMMAND);
    mc_scsi_bus_set_atn(&bus, 1);
    mc_scsi_bus_out(&bus, 0x00);            /* first CDB byte; ATN takes the target to MESSAGE OUT */
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_MSG_OUT);
    mc_scsi_bus_set_atn(&bus, 0);
    mc_scsi_bus_out(&bus, MC_SCSI_MSG_ABORT);
    CHECK_EQ(mc_scsi_bus_phase(&bus), MC_SCSI_BUS_FREE);
}

/* The original Pippin .AppleCD driver compares 22 bytes starting at
 * offset 14 in a 36-byte MODE SENSE(6) response. */
static void test_pippin_cd_page(void) {
    /* Every CD drive identifies itself as Apple's (page $30), as Apple's
     * CD-ROM extension requires; the disk does not. */
    const uint8_t mode[6] = {0x1a, 0, 0x30, 0, 36, 0};
    scsi_result g = run(3, mode, 6);
    CHECK(g.status == MC_SCSI_GOOD && !memcmp(buf + 14, "APPLE COMPUTER, INC   ", 22));
    mc_scsi_cd_init_pippin(&cd);
    scsi_result r = run(3, mode, 6);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 36);
    CHECK_EQ(buf[0], 35);
    CHECK_EQ(buf[3], 8);
    CHECK_EQ(buf[12], 0x30);
    CHECK_EQ(buf[13], 22);
    CHECK(!memcmp(buf + 14, "APPLE COMPUTER, INC   ", 22));
    CHECK_EQ(run(0, mode, 6).status, MC_SCSI_CHECK_CONDITION);
    const uint8_t capacity[10] = {0x25}, read[10] = {0x28, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    CHECK(mc_scsi_cd_insert(&cd, vcd)); /* no 2048-byte Apple descriptor */
    mc_scsi_bus_reset(&bus);
    CHECK_EQ(run(3, capacity, 10).status, MC_SCSI_CHECK_CONDITION);
    uint8_t k, a, q;
    sense(3, &k, &a, &q);
    CHECK_EQ(k, MC_SENSE_UNIT_ATTENTION); CHECK_EQ(a, 0x28);
    r = run(3, capacity, 10);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 8);
    CHECK_EQ(be32(buf + 4), 2048);
    r = run(3, read, 10);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 2048);
}

/* A mixed-mode disc from a cue sheet (src/core/disk.h): a data track of 16
 * frames, then two audio tracks in files of their own, each file opening on
 * a one-second pregap (INDEX 00 at 00:00:00, INDEX 01 at 00:01:00). Track 2
 * has 75 frames whose left samples count up, track 3 150 frames of
 * constant samples. */
static uint64_t audio_now;  /* the player's clock, samples */
static uint64_t audio_clock(void *ctx) { (void)ctx; return audio_now; }
static void write_frames(const char *path, unsigned pregap, unsigned frames, int16_t value, int count_up) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t b[2352];
    memset(b, 0, sizeof b);
    for (unsigned i = 0; i < pregap; ++i) fwrite(b, 1, sizeof b, f);
    for (unsigned i = 0; i < frames; ++i) {
        for (unsigned k = 0; k < 588; ++k) {
            const int16_t l = count_up ? (int16_t)(i * 588 + k) : value, r = (int16_t)-value;
            b[4 * k] = (uint8_t)l; b[4 * k + 1] = (uint8_t)(l >> 8);
            b[4 * k + 2] = (uint8_t)r; b[4 * k + 3] = (uint8_t)(r >> 8);
        }
        fwrite(b, 1, sizeof b, f);
    }
    fclose(f);
}
static void test_cd_audio(void) {
    char error[200];
    scsi_make_image("scsi_target", "mixed-data.bin", 16 * 2048, 0x33);
    write_frames(hw_work_path("scsi_target", "mixed-2.bin"), 75, 75, 0, 1);
    write_frames(hw_work_path("scsi_target", "mixed-3.bin"), 75, 150, 1000, 0);
    const char *cue = hw_work_path("scsi_target", "mixed.cue");
    FILE *f = fopen(cue, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fputs("FILE \"mixed-data.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n"
          "FILE \"mixed-2.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:01:00\n"
          "FILE \"mixed-3.bin\" BINARY\n  TRACK 03 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:01:00\n", f);
    fclose(f);
    mc_disk *mixed = mc_disk_open(cue, 0, 2048, error, sizeof error);
    CHECK(mixed != NULL);
    if (!mixed) return;
    /* The tracks: data 0-15; audio after one-second pregaps at 91 and 241. */
    mc_disk_track t;
    CHECK_EQ(mc_disk_tracks(mixed), 3);
    CHECK(mc_disk_track_get(mixed, 1, &t) && t.number == 2 && t.audio && t.start == 16 + 75 && t.length == 75);
    CHECK(mc_disk_track_get(mixed, 2, &t) && t.number == 3 && t.audio && t.start == 91 + 75 + 75 && t.length == 150);
    CHECK_EQ(mc_disk_leadout(mixed), 241 + 150);
    CHECK_EQ(mc_disk_sectors(mixed), 16);
    CHECK(mc_disk_track_at(mixed, 50, &t) && t.number == 2);       /* track 2's pregap */
    int16_t frame[588 * 2];
    CHECK(mc_disk_read_audio(mixed, 91 + 2, frame) && frame[0] == 2 * 588 && frame[2] == 2 * 588 + 1);
    mc_disk_set_clock(mixed, audio_clock, NULL);
    audio_now = 0;

    mc_scsi_unit drive;
    mc_scsi_bus b2;
    mc_scsi_bus_init(&b2);
    mc_scsi_cd_init(&drive);
    mc_scsi_bus_attach(&b2, 3, mc_scsi_unit_target(&drive));
    mc_vdisk *v = mc_vdisk_create_device(mixed, error, sizeof error);
    CHECK(v && mc_scsi_cd_insert(&drive, v));
    const uint8_t tur[6] = {0};
    scsi_run(&b2, 3, tur, 6, buf, sizeof buf, NULL, 0);      /* the unit attention */
    const uint8_t sel[6] = {0x15, 0x10, 0, 0, 12, 0};
    const uint8_t blocks2048[12] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0x08, 0x00};
    CHECK_EQ(scsi_run(&b2, 3, sel, 6, buf, sizeof buf, blocks2048, 12).status, MC_SCSI_GOOD);

    /* READ TOC (14.2.11): three tracks, controls 4 and 0, the lead-out. */
    const uint8_t toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 100, 0};
    scsi_result r = scsi_run(&b2, 3, toc, 10, buf, sizeof buf, NULL, 0);
    CHECK(r.ok && r.status == MC_SCSI_GOOD && r.moved == 4 + 4 * 8);
    CHECK(buf[2] == 1 && buf[3] == 3);
    CHECK(buf[5] == 0x14 && buf[6] == 1 && be32(buf + 8) == 0);
    CHECK(buf[13] == 0x10 && buf[14] == 2 && be32(buf + 16) == 91);
    CHECK(buf[21] == 0x10 && buf[22] == 3 && be32(buf + 24) == 241);
    CHECK(buf[30] == 0xAA && be32(buf + 32) == 391);
    const uint8_t toc_msf[10] = {0x43, 0x02, 0, 0, 0, 0, 3, 0, 100, 0};
    r = scsi_run(&b2, 3, toc_msf, 10, buf, sizeof buf, NULL, 0);
    CHECK(r.moved == 4 + 2 * 8 && buf[6] == 3 && buf[9] == 0 && buf[10] == 5 && buf[11] == 16);   /* 241 + 150 = 5 s 16 f */

    /* PLAY AUDIO TRACK INDEX of a data track: illegal mode for this track. */
    const uint8_t play_data[10] = {0x48, 0, 0, 0, 1, 1, 0, 1, 1, 0};
    CHECK_EQ(scsi_run(&b2, 3, play_data, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_CHECK_CONDITION);
    const uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
    scsi_run(&b2, 3, rs, 6, buf, sizeof buf, NULL, 0);
    CHECK(buf[2] == MC_SENSE_ILLEGAL_REQUEST && buf[12] == 0x64);

    /* Track 2 to the end of track 3; the sub-channel follows the clock. */
    const uint8_t play_tracks[10] = {0x48, 0, 0, 0, 2, 1, 0, 3, 1, 0};
    CHECK_EQ(scsi_run(&b2, 3, play_tracks, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    const uint8_t sub[10] = {0x42, 0, 0x40, 1, 0, 0, 0, 0, 16, 0};
    audio_now = 10 * 588 + 5;
    r = scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(r.moved == 16 && buf[1] == 0x11 && buf[4] == 1 && buf[5] == 0x10 && buf[6] == 2 && buf[7] == 1);
    CHECK(be32(buf + 8) == 91 + 10 && be32(buf + 12) == 10);
    /* The mixer: it starts at the guest's time and gives what the clock has
     * gone past since (at 44100 Hz the samples themselves). */
    int16_t out[64 * 2];
    memset(out, 0, sizeof out);
    CHECK_EQ(mc_disk_mix(mixed, out, 0, 4, 44100), 0);
    audio_now += 4;
    size_t n = mc_disk_mix(mixed, out, 0, 4, 44100);
    audio_now -= 4;
    CHECK(n == 4 && out[0] == 10 * 588 + 5 && out[2] == 10 * 588 + 6 && out[1] == 0);
    /* Pause, then resume where it stopped. */
    const uint8_t pause[10] = {0x4B, 0, 0, 0, 0, 0, 0, 0, 0, 0}, resume[10] = {0x4B, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    CHECK_EQ(scsi_run(&b2, 3, pause, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 44100;
    r = scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(buf[1] == 0x12 && be32(buf + 8) == 101);
    CHECK_EQ(scsi_run(&b2, 3, resume, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 75 * 588;      /* one second later: track 3's pregap */
    r = scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(buf[1] == 0x11 && buf[6] == 3 && buf[7] == 0 && be32(buf + 8) == 176);
    /* The audio control page halves the volume. */
    const uint8_t page_sel[6] = {0x15, 0x10, 0, 0, 4 + 16, 0};
    uint8_t page[20] = {0, 0, 0, 0, 0x0E, 0x0E, 0x04, 0, 0, 0, 0, 0, 1, 0x80, 2, 0x80, 0, 0, 0, 0};
    CHECK_EQ(scsi_run(&b2, 3, page_sel, 6, buf, sizeof buf, page, 20).status, MC_SCSI_GOOD);
    audio_now += 80 * 588;      /* into track 3 */
    memset(out, 0, sizeof out);
    mc_disk_mix(mixed, out, 0, 64, 44100);
    audio_now += 2;
    n = mc_disk_mix(mixed, out, 0, 2, 44100);
    CHECK(n == 2 && out[0] == 1000 * 0x80 / 255 && out[1] == -1000 * 0x80 / 255);
    /* The end: completed, reported once, then no status. */
    audio_now += 200 * 588;
    scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(buf[1] == 0x13 && be32(buf + 8) == 391);
    scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK_EQ(buf[1], 0x15);
    /* PLAY AUDIO MSF from track 3 to its first second's end, then STOP. */
    const uint8_t msf[10] = {0x47, 0, 0, 0, 3, 16, 0, 4, 16, 0};     /* 241+150 = 3:16, +75 */
    CHECK_EQ(scsi_run(&b2, 3, msf, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK_EQ(mc_disk_play_state(mixed, NULL), MC_CD_PLAYING);
    const uint8_t stop[12] = {0x4E};
    CHECK_EQ(scsi_run(&b2, 3, stop, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK_EQ(mc_disk_play_state(mixed, NULL), MC_CD_IDLE);
    CHECK_EQ(mc_disk_mix(mixed, out, 3, 8, 44100), 3);
    /* AppleCD Audio Player's next track: a PLAY AUDIO MSF with equal ends
     * moves the head of a play to track 3, paused, and a play from FF:FF:FF
     * starts there; REZERO UNIT stops play; Apple's CDh scans. */
    CHECK_EQ(scsi_run(&b2, 3, msf, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    const uint8_t seek[10] = {0x47, 0, 0, 0, 5, 16, 0, 5, 16, 0}, here[10] = {0x47, 0, 0, 0xFF, 0xFF, 0xFF, 0, 6, 16, 0};
    CHECK_EQ(scsi_run(&b2, 3, seek, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PAUSED && t.start == 241);
    CHECK_EQ(scsi_run(&b2, 3, here, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 241);
    const uint8_t rezero[6] = {0x01}, apple_scan[6] = {0xCD, 0, 0, 0, 5, 16};
    CHECK_EQ(scsi_run(&b2, 3, rezero, 6, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_IDLE && t.start == 0);
    CHECK_EQ(scsi_run(&b2, 3, apple_scan, 6, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 249);
    scsi_run(&b2, 3, stop, 12, buf, sizeof buf, NULL, 0);
    scsi_run(&b2, 3, stop, 12, buf, sizeof buf, NULL, 0);

    /* MMC-1 5.1.14: forward/reverse SCAN is audible play; SUB-CHANNEL
     * follows the accelerated address, and PAUSE/STOP restore normal play. */
    const uint8_t scan[12] = {0xBA, 0, 0, 0, 0, 100};
    CHECK_EQ(scsi_run(&b2, 3, scan, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588 + 5;
    scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(buf[1] == 0x11 && be32(buf + 8) == 108);
    memset(out, 0, sizeof out);
    CHECK_EQ(mc_disk_mix(mixed, out, 0, 4, 44100), 0);
    audio_now += 4;
    CHECK_EQ(mc_disk_mix(mixed, out, 0, 4, 44100), 4);
    CHECK(out[0] == (17 * 588 + 5) * 0x80 / 255 && out[2] == (17 * 588 + 6) * 0x80 / 255 && out[1] == 0);
    CHECK_EQ(scsi_run(&b2, 3, stop, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 109);
    scsi_run(&b2, 3, stop, 12, buf, sizeof buf, NULL, 0);
    const uint8_t current[10] = {0x45, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 75, 0};
    CHECK_EQ(scsi_run(&b2, 3, current, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 109);

    const uint8_t reverse[12] = {0xBA, 0x10, 0, 0, 3, 45, 0, 0, 0, 0x40}; /* MSF 00:03:45 = LBA 120 */
    CHECK_EQ(scsi_run(&b2, 3, reverse, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588 + 5;
    scsi_run(&b2, 3, sub, 10, buf, sizeof buf, NULL, 0);
    CHECK(buf[1] == 0x11 && be32(buf + 8) == 112);
    memset(out, 0, sizeof out);
    mc_disk_mix(mixed, out, 0, 4, 44100);
    audio_now += 4;
    CHECK_EQ(mc_disk_mix(mixed, out, 0, 4, 44100), 4);
    CHECK(out[0] == (21 * 588 + 5) * 0x80 / 255 && out[1] == 0);
    CHECK_EQ(scsi_run(&b2, 3, pause, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 44100;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PAUSED && t.start == 112);
    CHECK_EQ(scsi_run(&b2, 3, resume, 10, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 113);

    const uint8_t scan_track[12] = {0xBA, 0, 0, 0, 0, 3, 0, 0, 0, 0x80};
    CHECK_EQ(scsi_run(&b2, 3, scan_track, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 241);
    const uint8_t scan_bad_type[12] = {0xBA, 0, 0, 0, 0, 3, 0, 0, 0, 0xC0};
    CHECK_EQ(scsi_run(&b2, 3, scan_bad_type, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_CHECK_CONDITION);
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_PLAYING && t.start == 241);
    const uint8_t scan_data[12] = {0xBA, 0, 0, 0, 0, 4};
    CHECK_EQ(scsi_run(&b2, 3, scan_data, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_CHECK_CONDITION);
    scsi_run(&b2, 3, rs, 6, buf, sizeof buf, NULL, 0);
    CHECK(buf[2] == MC_SENSE_ILLEGAL_REQUEST && buf[12] == 0x64);
    const uint8_t scan_end[12] = {0xBA, 0, 0, 0, 1, 130}; /* LBA 386, five sectors before lead-out */
    CHECK_EQ(scsi_run(&b2, 3, scan_end, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_COMPLETED && t.start == 391);
    const uint8_t scan_back_end[12] = {0xBA, 0x10, 0, 0, 0, 20};
    CHECK_EQ(scsi_run(&b2, 3, scan_back_end, 12, buf, sizeof buf, NULL, 0).status, MC_SCSI_GOOD);
    audio_now += 588;
    CHECK(mc_disk_play_state(mixed, &t.start) == MC_CD_COMPLETED && t.start == 16); /* before data, without underflow */
    mc_scsi_bus_free(&b2);
    mc_vdisk_destroy(v);
    mc_disk_close(mixed);
}

int main(void) {
    int tests = 0;
    setup();
#define RUN(t) (t(), tests++)
    RUN(test_inquiry);
    RUN(test_capacity);
    RUN(test_read_write);
    RUN(test_range);
    RUN(test_mode_sense);
    RUN(test_mode_select_disk);
    RUN(test_cd);
    RUN(test_cd_apple_block);
    RUN(test_misc);
    RUN(test_bus);
    RUN(test_pippin_cd_page);
    RUN(test_cd_audio);
    mc_scsi_bus_free(&bus);
    mc_vdisk_destroy(vdisk);
    mc_vdisk_destroy(vcd);
    mc_disk_close(img);
    mc_disk_close(iso);
    printf("test_scsi_target: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
