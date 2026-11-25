/* test_swim2: the SWIM2 (mc_swim2_init) through its IWM and ISM register
 * sets. It runs every data-path test of test_swim.c (GCR 800K read, write
 * and format through the IWM set; MFM 1440K read, write and format through
 * the ISM; index and TACH timing, write protect, eject, refusals, 400K) on
 * the SWIM2, then the 720K MFM media, a DiskCopy 400K image and the chip's
 * identity. The register model is the SWIM's (see swim.h): Apple's SWIM2
 * documents are not among the sources. Images are made under work/tests/swim2. */
#define SWIM_WORK "swim2"
#define SWIM_INIT(s, host, drives) mc_swim2_init(s, host, drives)
#define SWIM_INCLUDE
#include "test_swim.c"

/* A 400K DiskCopy 4.2 image with tags (format byte 0, 800 sectors). */
static const char *make_dc42_400(const char *name, uint8_t **data_out, uint8_t **tags_out) {
    const char *path = hw_work_path(SWIM_WORK, name);
    const uint32_t size = 800 * 512, tsize = 800 * 12;
    uint8_t *img = calloc(1, 0x54 + size + tsize);
    img[0] = 4; memcpy(img + 1, "k400", 4);
    put32(img + 0x40, size); put32(img + 0x44, tsize);
    for (uint32_t i = 0; i < size + tsize; i++) img[0x54 + i] = rnd();
    put32(img + 0x48, dc_sum(img + 0x54, size));
    put32(img + 0x4C, dc_sum(img + 0x54 + size + 12, tsize - 12));
    img[0x50] = 0; img[0x51] = 0x02; img[0x52] = 1; img[0x53] = 0;
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(img, 1, 0x54 + size + tsize, f); fclose(f); }
    *data_out = malloc(size); memcpy(*data_out, img + 0x54, size);
    *tags_out = malloc(tsize); memcpy(*tags_out, img + 0x54 + size, tsize);
    free(img);
    return path;
}

static void test_swim2_identity(void) {
    tests++;
    fresh(2);
    CHECK_EQ(swim.chip, MC_CHIP_SWIM2);
    CHECK(!strcmp(swim.dev.name, "SWIM2"));
    CHECK_EQ(swim.drive[0].type, MC_DRIVE_SUPERDRIVE);
    iwm_mode(0x1F);
    enable_drive(1);
    CHECK_EQ(status(ST_MFMDRIVE), 1);   /* SuperDrives: MFM-capable */
    CHECK_EQ(status(ST_DRVIN), 0);
    to_ism();
    CHECK(swim.ism);                    /* the IWM -> ISM switch */
    wr(6, 0x40);
    CHECK(!swim.ism);
}

/* Reads a 400K DiskCopy image with tags through the IWM set, both sides of
 * a track (side 1 exists on a SuperDrive: it decodes to nothing for 400K). */
static void test_swim2_dc400(void) {
    tests++;
    fresh(1);
    uint8_t *data, *tags;
    mc_disk *disk = open_disk(make_dc42_400("k400.dc42", &data, &tags), 0);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK_EQ(swim.drive[0].format, MC_FLOPPY_GCR400);
    iwm_mode(0x1F);
    enable_drive(0);
    command(CMD_MOTOR, 0);
    command(CMD_DIRTN, 0);
    CHECK_EQ(status(ST_LOWDENS), 1);
    for (unsigned track = 0; track < 80; track += 31) {
        while (swim.drive[0].track < track) command(CMD_STEP, 0);
        read_state(0);
        static mc_floppy_track t;
        t.length = mc_floppy_track_length(1, 0, track);
        for (uint32_t k = 0; k < t.length; k++) { hw_fake_step(&fake, 256); t.data[k] = acc(12); t.mark[k] = 0; }
        capture c = {{0}, {0}, 0};
        unsigned n = mc_floppy_sectors(MC_FLOPPY_GCR400, track);
        CHECK_EQ(mc_floppy_decode_track(MC_FLOPPY_GCR400, track, 0, &t, capture_sector, &c), n);
        int64_t first = mc_floppy_first_sector(MC_FLOPPY_GCR400, track, 0);
        CHECK(!memcmp(c.data, data + first * 512, n * 512));
        CHECK(!memcmp(c.tags, tags + first * 12, n * 12));
    }
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data); free(tags);
}

/* 720K MFM media through the ISM: a track read, then the whole track
 * rewritten (a format) through the FIFO and decoded back into sectors. */
static void test_swim2_mfm720(void) {
    tests++;
    fresh(1);
    uint8_t *data;
    mc_disk *disk = open_disk(make_raw("mfm720.img", 1440, &data), 1);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK_EQ(swim.drive[0].format, MC_FLOPPY_MFM720);
    iwm_mode(0x1F);
    to_ism();
    wr(5, 0x00);
    wr(7, 0x82);
    ism_command(CMD_MODE, 0);
    ism_command(CMD_MOTOR, 0);
    const unsigned track = 0, side = 1;
    mc_swim_set_headsel(&swim, (int)side);
    wr(7, 0x01); wr(6, 0x01);
    wr(7, 0x08);
    static mc_floppy_track t;
    t.length = mc_floppy_track_length(0, 0, track);
    CHECK_EQ(t.length, 6250);
    for (uint32_t k = 0; k < t.length; k++) {
        uint8_t h;
        int spins = 0;
        do { hw_fake_step(&fake, 128); h = acc(15); } while (!(h & 0x80) && ++spins < 100000);
        t.mark[k] = h & 1;
        t.data[k] = acc(t.mark[k] ? 9 : 8);
    }
    CHECK(t.mark[0]);
    CHECK_EQ(acc(10), 0);
    capture c = {{0}, {0}, 0};
    CHECK_EQ(mc_floppy_decode_track(MC_FLOPPY_MFM720, track, side, &t, capture_sector, &c), 9);
    int64_t first = mc_floppy_first_sector(MC_FLOPPY_MFM720, track, side);
    CHECK(!memcmp(c.data, data + first * 512, 9 * 512));
    wr(6, 0x08);
    /* Format: a new track through the FIFO, marks through the MARK register. */
    uint8_t ndata[9 * 512];
    for (unsigned i = 0; i < sizeof ndata; i++) ndata[i] = rnd();
    static mc_floppy_track nt;
    mc_floppy_build_track(MC_FLOPPY_MFM720, track, side, ndata, NULL, &nt);
    wr(7, 0x10);
    wr(7, 0x01); wr(6, 0x01);
    wr(nt.mark[0] ? 1 : 0, nt.data[0]);
    wr(7, 0x08);
    for (uint32_t i = 1; i < nt.length; i++) {
        int spins = 0;
        while (!(acc(15) & 0x80) && ++spins < 100000) hw_fake_step(&fake, 128);
        wr(nt.mark[i] ? 1 : 0, nt.data[i]);
    }
    while (!(acc(15) & 0x40)) hw_fake_step(&fake, 128);
    wr(6, 0x08);
    CHECK_EQ(acc(10), 0);
    uint8_t back[9 * 512];
    CHECK(mc_disk_read(disk, (uint64_t)first, 9, back));
    CHECK(!memcmp(back, ndata, sizeof back));
    CHECK(mc_disk_read(disk, 0, 1, back));          /* side 0 untouched */
    CHECK(!memcmp(back, data, 512));
    wr(6, 0x90);
    wr(6, 0x40);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
}

int main(void) {
    run_swim_tests();
    test_swim2_identity();
    test_swim2_dc400();
    test_swim2_mfm720();
    printf("test_swim2: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
