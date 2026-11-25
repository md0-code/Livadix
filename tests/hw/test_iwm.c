/* test_iwm: the IWM alone (mc_iwm_init), as in the Mac II (v1 ROM): the IWM
 * register set only, no ISM, no MFM, with 800K (699-0326) and 400K
 * (699-0285) GCR drives. Sources: SWIM Chip User's Reference ("Using the IWM
 * Hardware"), the Apple 800K Double-Sided Drive ERS 699-0452-A (Table 1,
 * 3.2.4.6, 3.2.4.13), Guide to the Macintosh Family Hardware ch. 2 and 9.
 * GCR reads, writes and formats reuse the data-path tests of test_swim.c on
 * an 800K drive. Images are made under work/tests/iwm. */
#define SWIM_WORK "iwm"
#define SWIM_INIT(s, host, drives) mc_iwm_init(s, host, drives, MC_DRIVE_800K)
#define SWIM_SUPERDRIVE 0
#define SWIM_INCLUDE
#include "test_swim.c"

static void fresh_iwm(unsigned drives, mc_drive_type type) {
    hw_fake_init(&fake);
    mc_iwm_init(&swim, &fake.host, drives, type);
}

static void test_iwm_identity(void) {
    tests++;
    fresh_iwm(2, MC_DRIVE_800K);
    CHECK_EQ(swim.chip, MC_CHIP_IWM);
    CHECK(!strcmp(swim.dev.name, "IWM"));
    /* The mode sequence that selects the ISM on a SWIM does nothing. */
    to_ism();
    CHECK(!swim.ism);
    iwm_mode(0x1F);
    latch(6, 1);
    CHECK_EQ(acc(14) & 0x1F, 0x1F);
    enable_drive(0);
    CHECK(acc(14) & 0x20);
    /* An 800K drive: not MFM-capable, double-sided, REVISED set, present;
     * the MFM command is ignored and MFM mode reads off. */
    CHECK_EQ(status(ST_DRVIN), 0);
    CHECK_EQ(status(ST_MFMDRIVE), 0);
    CHECK_EQ(status(ST_SIDES), 1);
    CHECK_EQ(status(ST_LOWDENS), 1);
    command(CMD_MODE, 0);
    CHECK_EQ(status(ST_MFMMODE), 0);
    CHECK_EQ(swim.drive[0].mfm_mode, 0);
    /* Both drives are enabled by the IWM's own drive-select latch. */
    enable_drive(1);
    CHECK_EQ(status(ST_DRVIN), 0);
    fresh_iwm(1, MC_DRIVE_800K);
    iwm_mode(0x1F);
    enable_drive(1);
    CHECK_EQ(status(ST_DRVIN), 1);      /* no second drive */
}

/* The SE's VIA1 PA4 (vDriveSel, Guide, Table 4-3): low, /ENBL1 enables
 * the upper internal drive, not fitted; high, the lower one, drive 0. The
 * external drive on the IWM's second enable is not affected. */
static void test_iwm_se_drive_select(void) {
    tests++;
    fresh_iwm(2, MC_DRIVE_800K);
    iwm_mode(0x1F);
    enable_drive(0);
    CHECK_EQ(status(ST_DRVIN), 0);
    mc_swim_set_upper(&swim, 1);
    CHECK_EQ(status(ST_DRVIN), 1);      /* nothing answers */
    enable_drive(1);
    CHECK_EQ(status(ST_DRVIN), 0);
    mc_swim_set_upper(&swim, 0);
    enable_drive(0);
    CHECK_EQ(status(ST_DRVIN), 0);
}

static void test_iwm_drive_status(void) {
    tests++;
    fresh_iwm(1, MC_DRIVE_800K);
    iwm_mode(0x1F);
    enable_drive(0);
    char error[120];
    CHECK_EQ(status(ST_CSTIN), 1);
    CHECK_EQ(status(ST_WRTPRT), 0);
    mc_disk *disk = open_disk(make_raw("iwm800.img", 1600, NULL), 1);
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK_EQ(status(ST_CSTIN), 0);
    CHECK_EQ(status(ST_WRTPRT), 1);
    CHECK_EQ(status(ST_TK0), 0);
    CHECK_EQ(status(ST_READY), 1);
    command(CMD_MOTOR, 0);
    CHECK_EQ(status(ST_MOTOR), 0);
    CHECK_EQ(status(ST_READY), 0);
    command(CMD_DIRTN, 0);
    command(CMD_STEP, 0);
    CHECK_EQ(status(ST_STEP), 0);
    hw_fake_step(&fake, (uint64_t)MC_C15M_HZ * 12 / 1000 + 1);
    CHECK_EQ(status(ST_STEP), 1);
    CHECK_EQ(status(ST_TK0), 1);
    /* Write protect: a locked disk reads low. */
    mc_swim_remove(&swim, 0);
    CHECK(mc_swim_insert(&swim, 0, disk, 1, error, sizeof error));
    CHECK_EQ(status(ST_WRTPRT), 0);
    /* Eject. */
    command(CMD_EJECT, 1);
    CHECK_EQ(status(ST_CSTIN), 1);
    int ejected = 0;
    CHECK(mc_swim_media(&swim, 0, &ejected) == disk && ejected);
    CHECK(mc_swim_remove(&swim, 0) == disk);
    mc_disk_close(disk);
    /* MFM media do not go into a GCR-only drive. */
    disk = open_disk(make_raw("iwm1440.img", 2880, NULL), 0);
    CHECK(!mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    mc_disk_close(disk);
    disk = open_disk(make_raw("iwm720.img", 1440, NULL), 0);
    CHECK(!mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    mc_disk_close(disk);
}

static void test_iwm_write_protect_data(void) {
    tests++;
    fresh_iwm(1, MC_DRIVE_800K);
    uint8_t *data;
    mc_disk *disk = open_disk(make_raw("iwmwp.img", 1600, &data), 1);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 1, error, sizeof error));
    iwm_mode(0x1F);
    enable_drive(0);
    command(CMD_MOTOR, 0);
    read_state(0);
    static mc_floppy_track t;
    unsigned n = mc_floppy_sectors(MC_FLOPPY_GCR800, 0);
    uint8_t ndata[12 * 512];
    for (unsigned i = 0; i < n * 512; i++) ndata[i] = rnd();
    mc_floppy_build_track(MC_FLOPPY_GCR800, 0, 0, ndata, NULL, &t);
    iwm_write_bytes(t.data, t.length);
    latch(7, 0);
    uint8_t back[12 * 512];
    CHECK(mc_disk_read(disk, 0, n, back));
    CHECK(!memcmp(back, data, n * 512));    /* the locked disk is untouched */
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
}

/* TACH: 60 pulses a revolution at the zone's speed, on the IWM-only drive. */
static void test_iwm_tach(void) {
    tests++;
    fresh_iwm(1, MC_DRIVE_800K);
    iwm_mode(0x1F);
    enable_drive(0);
    mc_disk *disk = open_disk(make_raw("iwmtach.img", 1600, NULL), 0);
    char error[120];
    mc_swim_insert(&swim, 0, disk, 0, error, sizeof error);
    command(CMD_MOTOR, 0);
    command(CMD_DIRTN, 0);
    static const unsigned rpm[5] = {394, 429, 472, 525, 590};
    for (unsigned z = 0; z < 5; z++) {
        while (swim.drive[0].track < z * 16) command(CMD_STEP, 0);
        unsigned edges = count_tach(1000);
        CHECK(edges + 5 >= rpm[z] && edges <= rpm[z] + 5);
    }
    command(CMD_MOTOR, 1);
    CHECK_EQ(count_tach(100), 0);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

/* The 400K single-sided drive: /SINGLE SIDE low, REVISED low, not
 * MFM-capable, head 1 delivers nothing; a 400K disk reads on side 0. */
static void test_iwm_400k_drive(void) {
    tests++;
    fresh_iwm(1, MC_DRIVE_400K);
    CHECK_EQ(swim.drive[0].type, MC_DRIVE_400K);
    iwm_mode(0x1F);
    enable_drive(0);
    CHECK_EQ(status(ST_SIDES), 0);
    CHECK_EQ(status(ST_LOWDENS), 0);
    CHECK_EQ(status(ST_MFMDRIVE), 0);
    CHECK_EQ(status(ST_DRVIN), 0);
    uint8_t *data;
    char error[120];
    mc_disk *disk = open_disk(make_raw("iwm400.img", 800, &data), 0);
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    command(CMD_MOTOR, 0);
    command(CMD_DIRTN, 0);
    for (int i = 0; i < 40; i++) command(CMD_STEP, 0);
    for (unsigned side = 0; side < 2; side++) {
        read_state(side);
        static mc_floppy_track t;
        t.length = mc_floppy_track_length(1, 0, 40);
        for (uint32_t k = 0; k < t.length; k++) { hw_fake_step(&fake, 256); t.data[k] = acc(12); t.mark[k] = 0; }
        capture c = {{0}, {0}, 0};
        unsigned got = mc_floppy_decode_track(MC_FLOPPY_GCR400, 40, side, &t, capture_sector, &c);
        unsigned n = mc_floppy_sectors(MC_FLOPPY_GCR400, 40);
        if (side == 0) {
            CHECK_EQ(got, n);
            CHECK(!memcmp(c.data, data + mc_floppy_first_sector(MC_FLOPPY_GCR400, 40, 0) * 512, n * 512));
        } else {
            CHECK_EQ(got, 0);
        }
    }
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
    /* An 800K disk in a 400K drive: head 1 is not there. */
    disk = open_disk(make_raw("iwm800b.img", 1600, NULL), 0);
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    read_state(1);
    int any = 0;
    for (int k = 0; k < 2000; k++) { hw_fake_step(&fake, 256); if (acc(12) & 0x80) any = 1; }
    CHECK(!any);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

/* The 400K drive under the disk-speed PWM (Guide, chapter 13): its TACH
 * follows the speed the line sets, from the line's source at each read
 * (the board's sound buffer), whatever the zone; the bytes still pass at
 * the media's rate. The 800K drive keeps its zone speed. */
static uint32_t line_rpm16;
static uint32_t line_speed(void *ctx) { (void)ctx; return line_rpm16; }
static void test_iwm_disk_speed(void) {
    tests++;
    fresh_iwm(1, MC_DRIVE_400K);
    iwm_mode(0x1F);
    enable_drive(0);
    mc_disk *disk = open_disk(make_raw("iwmspeed.img", 800, NULL), 0);
    char error[120];
    mc_swim_insert(&swim, 0, disk, 0, error, sizeof error);
    command(CMD_MOTOR, 0);
    mc_swim_set_speed(&swim, 500u * 16u);
    unsigned edges = count_tach(1000);
    CHECK(edges + 5 >= 500 && edges <= 505);
    mc_swim_speed_source(&swim, line_speed, NULL);
    line_rpm16 = 394u * 16u;
    edges = count_tach(1000);
    CHECK(edges + 5 >= 394 && edges <= 399);
    line_rpm16 = 600u * 16u;
    edges = count_tach(1000);
    CHECK(edges + 5 >= 600 && edges <= 605);
    command(CMD_MOTOR, 1);
    CHECK_EQ(count_tach(100), 0);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    /* The 800K drive ignores the line. */
    fresh_iwm(1, MC_DRIVE_800K);
    iwm_mode(0x1F);
    enable_drive(0);
    disk = open_disk(make_raw("iwmspeed8.img", 1600, NULL), 0);
    mc_swim_insert(&swim, 0, disk, 0, error, sizeof error);
    command(CMD_MOTOR, 0);
    mc_swim_set_speed(&swim, 600u * 16u);
    edges = count_tach(1000);
    CHECK(edges + 5 >= 394 && edges <= 399);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

int main(void) {
    test_gcr_table();
    test_gcr_data();
    test_gcr_address();
    test_zones();
    test_build_decode();
    test_iwm_read_gcr();
    test_iwm_write_gcr();
    test_gcr400();
    test_iwm_identity();
    test_iwm_drive_status();
    test_iwm_se_drive_select();
    test_iwm_write_protect_data();
    test_iwm_tach();
    test_iwm_400k_drive();
    test_iwm_disk_speed();
    printf("test_iwm: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
