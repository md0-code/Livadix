/* test_swim: the SWIM (IWM and ISM register sets), the Apple 3.5-inch drive
 * registers and the GCR/MFM track synthesis, from the SWIM Chip User's
 * Reference (1988), the Apple 800K Double-Sided Drive ERS (699-0452-A) and
 * Inside Macintosh III, "The Disk Interface". Images are made under
 * work/tests/swim. */
#include "hw_test.h"
#include "swim.h"
#include "floppy_track.h"
#include "disk.h"

#include <stdlib.h>

/* test_swim2.c and test_iwm.c include this file and run the same data-path
 * tests on their chip: SWIM_WORK is the work folder, SWIM_INIT the chip's
 * constructor, SWIM_SUPERDRIVE 0 for GCR-only drives, SWIM_INCLUDE drops
 * main. */
#ifndef SWIM_WORK
#define SWIM_WORK "swim"
#endif
#ifndef SWIM_INIT
#define SWIM_INIT(s, host, drives) mc_swim_init(s, host, drives)
#endif
#ifndef SWIM_SUPERDRIVE
#define SWIM_SUPERDRIVE 1
#endif
#ifndef SWIM_NAME
#define SWIM_NAME "test_swim"
#endif

static hw_fake fake;
static mc_swim swim;
static int tests;

/* ---- Images ---- */

static uint32_t rnd_state = 12345;
static uint8_t rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return (uint8_t)(rnd_state >> 16); }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t dc_sum(const uint8_t *p, size_t n) {
    uint32_t sum = 0;
    for (size_t i = 0; i + 1 < n; i += 2) { sum += (uint32_t)p[i] << 8 | p[i + 1]; sum = (sum >> 1) | (sum << 31); }
    return sum;
}

/* A raw image of `sectors` sectors of pseudo-random data; the data stays in *out. */
static const char *make_raw(const char *name, uint32_t sectors, uint8_t **out) {
    const char *path = hw_work_path(SWIM_WORK, name);
    uint8_t *data = malloc((size_t)sectors * 512);
    for (size_t i = 0; i < (size_t)sectors * 512; i++) data[i] = rnd();
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(data, 1, (size_t)sectors * 512, f); fclose(f); }
    if (out) *out = data; else free(data);
    return path;
}

/* A DiskCopy 4.2 800K image with tags. */
static const char *make_dc42(const char *name, uint8_t **data_out, uint8_t **tags_out) {
    const char *path = hw_work_path(SWIM_WORK, name);
    const uint32_t size = 1600 * 512, tsize = 1600 * 12;
    uint8_t *img = calloc(1, 0x54 + size + tsize);
    img[0] = 4; memcpy(img + 1, "test", 4);
    put32(img + 0x40, size); put32(img + 0x44, tsize);
    for (uint32_t i = 0; i < size + tsize; i++) img[0x54 + i] = rnd();
    put32(img + 0x48, dc_sum(img + 0x54, size));
    put32(img + 0x4C, dc_sum(img + 0x54 + size + 12, tsize - 12));
    img[0x50] = 1; img[0x51] = 0x22; img[0x52] = 1; img[0x53] = 0;
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) { fwrite(img, 1, 0x54 + size + tsize, f); fclose(f); }
    *data_out = malloc(size); memcpy(*data_out, img + 0x54, size);
    *tags_out = malloc(tsize); memcpy(*tags_out, img + 0x54 + size, tsize);
    free(img);
    return path;
}

static mc_disk *open_disk(const char *path, int writable) {
    char error[200];
    mc_disk *d = mc_disk_open(path, writable, 512, error, sizeof error);
    if (!d) fprintf(stderr, "open %s: %s\n", path, error);
    CHECK(d != NULL);
    return d;
}

/* ---- Register access helpers ---- */

static uint8_t acc(unsigned reg) { return mc_swim_access(&swim, reg, 0, 0); }
static void wr(unsigned reg, uint8_t v) { mc_swim_access(&swim, reg, 1, v); }
/* IWM latch `n` (0-3 PHASE0-3, 4 MotorOn, 5 drive select, 6 L6, 7 L7):
 * address 2n clears it, 2n+1 sets it [SWIM, "Using the IWM Hardware"]. */
static void latch(unsigned n, int on) { acc(2 * n + (on ? 1u : 0u)); }

/* Status bit at SEL, CA2, CA1, CA0 (addr 0-15, [ERS] Table 1): Q6 on, Q7 off,
 * then bit 7 of the status register. */
static int status(unsigned addr) {
    mc_swim_set_headsel(&swim, addr >> 3 & 1);
    latch(0, addr & 1); latch(1, addr >> 1 & 1); latch(2, addr >> 2 & 1);
    latch(6, 1);
    return acc(14) >> 7 & 1;
}
/* Drive command at SEL, CA1, CA0 (addr 0-7) with CA2 = value, LSTRB pulsed
 * ([ERS] 3.1.2; IM III: CA0 and CA1 high before SEL changes). */
static void command(unsigned addr, int value) {
    latch(3, 0); latch(0, 1); latch(1, 1);
    mc_swim_set_headsel(&swim, addr >> 2 & 1);
    latch(0, addr & 1); latch(1, addr >> 1 & 1); latch(2, value);
    latch(3, 1); latch(3, 0);
}
enum { CMD_DIRTN = 0, CMD_STEP = 1, CMD_MOTOR = 2, CMD_EJECT = 3, CMD_MODE = 5 };
enum { ST_STEP = 0x1, ST_MOTOR = 0x2, ST_CSTIN = 0x8, ST_WRTPRT = 0x9, ST_TK0 = 0xA, ST_TACH = 0xB,
       ST_SIDES = 0x6, ST_DRVIN = 0x7, ST_MFMDRIVE = 0x5, ST_MFMMODE = 0xD, ST_READY = 0xE, ST_LOWDENS = 0xF };

/* IWM mode register: Q6 on, motor off, write at Q7H [SWIM, "Set Mode"]. */
static void iwm_mode(uint8_t v) { latch(6, 1); latch(4, 0); wr(15, v); latch(7, 0); latch(6, 0); }
static void enable_drive(unsigned drive) { latch(5, drive != 0); latch(4, 1); }
/* Selects RDDATA (CA2=1, CA1=0, CA0=0, SEL=head) and the Read Data state. */
static void read_state(unsigned head) {
    latch(0, 0); latch(1, 0); latch(2, 1);
    mc_swim_set_headsel(&swim, (int)head);
    latch(6, 0); latch(7, 0);
}
static void fresh(unsigned drives) {
    hw_fake_init(&fake);
    SWIM_INIT(&swim, &fake.host, drives);
}
static void to_ism(void) { iwm_mode(0x5F); iwm_mode(0x1F); iwm_mode(0x5F); iwm_mode(0x5F); }
/* ISM: a drive command through the phase register (outputs, LSTRB rising). */
static void ism_command(unsigned addr, int value) {
    uint8_t ph = (uint8_t)(0xF0 | (addr & 1) | (addr >> 1 & 1) << 1 | (value ? 4 : 0));
    mc_swim_set_headsel(&swim, addr >> 2 & 1);
    wr(4, ph); wr(4, (uint8_t)(ph | 8)); wr(4, ph);
}

/* ---- Track synthesis ---- */

static void test_gcr_table(void) {
    tests++;
    /* SWIM, "GCR Encoding": 81 bytes have the top bit set and never three
     * zeros in a row; the 64 data bytes have at most one pair of zeros; $D5
     * and $AA are marks. */
    int count = 0;
    for (int b = 0x80; b < 0x100; b++) {
        int run = 0, max_run = 0;
        for (int i = 7; i >= 0; i--) { run = (b >> i & 1) ? 0 : run + 1; if (run > max_run) max_run = run; }
        if (max_run < 3) count++;
    }
    CHECK_EQ(count, 81);
    for (int i = 0; i < 64; i++) {
        uint8_t b = mc_gcr_encode6[i];
        int pairs = 0, ones = 0;
        for (int k = 6; k >= 0; k--) {
            if (!(b >> k & 1) && !(b >> (k + 1) & 1)) pairs++;
            if (k < 6 && (b >> k & 1) && (b >> (k + 1) & 1)) ones++;
        }
        CHECK(b & 0x80);
        CHECK(pairs <= 1);
        CHECK(b != 0xD5 && b != 0xAA);
        if (i) CHECK(b > mc_gcr_encode6[i - 1]);
        CHECK_EQ(mc_gcr_decode6(b), i);
    }
    CHECK_EQ(mc_gcr_decode6(0xD5), -1);
    CHECK_EQ(mc_gcr_decode6(0xAA), -1);
    CHECK_EQ(mc_gcr_decode6(0x95), -1);
}

static void test_gcr_data(void) {
    tests++;
    uint8_t in[524], nib[MC_GCR_DATA_NIBBLES], out[524];
    for (int round = 0; round < 50; round++) {
        for (int i = 0; i < 524; i++) in[i] = round == 0 ? 0 : round == 1 ? 0xFF : rnd();
        mc_gcr_encode_data(in, nib);
        for (int i = 0; i < MC_GCR_DATA_NIBBLES; i++) CHECK(mc_gcr_decode6(nib[i]) >= 0);
        CHECK(mc_gcr_decode_data(nib, out));
        CHECK(!memcmp(in, out, 524));
        /* Any changed nibble breaks the checksum. */
        unsigned at = rnd() * 3u % MC_GCR_DATA_NIBBLES;
        int v = mc_gcr_decode6(nib[at]);
        nib[at] = mc_gcr_encode6[(v + 1) & 63];
        CHECK(!mc_gcr_decode_data(nib, out));
    }
}

static void test_gcr_address(void) {
    tests++;
    uint8_t a[5];
    /* Side in bit 11 of the track word, track bits 6-10 below it; checksum
     * the exclusive-or of the four fields. */
    mc_gcr_address(70, 1, 5, 0x22, a);
    CHECK_EQ(a[0], 70 & 0x3F);
    CHECK_EQ(a[1], 5);
    CHECK_EQ(a[2], 0x20 | 1);
    CHECK_EQ(a[3], 0x22);
    CHECK_EQ(a[4], (70 & 0x3F) ^ 5 ^ 0x21 ^ 0x22);
    mc_gcr_address(3, 0, 11, 0x02, a);
    CHECK_EQ(a[2], 0);
    CHECK_EQ(a[4], 3 ^ 11 ^ 0 ^ 2);
}

static void test_zones(void) {
    tests++;
    /* SWIM, "GCR Track Format": 12, 11, 10, 9, 8 sectors per zone of 16 tracks. */
    static const unsigned want[5] = {12, 11, 10, 9, 8};
    for (unsigned z = 0; z < 5; z++) {
        CHECK_EQ(mc_floppy_sectors(MC_FLOPPY_GCR800, z * 16), want[z]);
        CHECK_EQ(mc_floppy_sectors(MC_FLOPPY_GCR800, z * 16 + 15), want[z]);
    }
    CHECK_EQ(mc_floppy_sectors(MC_FLOPPY_MFM720, 40), 9);
    CHECK_EQ(mc_floppy_sectors(MC_FLOPPY_MFM1440, 79), 18);
    /* Totals: 400K, 800K. */
    int64_t total = 0;
    for (unsigned t = 0; t < 80; t++) total += mc_floppy_sectors(MC_FLOPPY_GCR800, t);
    CHECK_EQ(total, 800);
    CHECK_EQ(mc_floppy_first_sector(MC_FLOPPY_GCR800, 1, 0), 24);
    CHECK_EQ(mc_floppy_first_sector(MC_FLOPPY_GCR800, 0, 1), 12);
    CHECK_EQ(mc_floppy_first_sector(MC_FLOPPY_GCR400, 1, 0), 12);
    CHECK_EQ(mc_floppy_first_sector(MC_FLOPPY_GCR400, 0, 1), -1);
    CHECK_EQ(mc_floppy_first_sector(MC_FLOPPY_MFM1440, 2, 1), 90);
    CHECK_EQ(mc_floppy_format_of(800), MC_FLOPPY_GCR400);
    CHECK_EQ(mc_floppy_format_of(1600), MC_FLOPPY_GCR800);
    CHECK_EQ(mc_floppy_format_of(1440), MC_FLOPPY_MFM720);
    CHECK_EQ(mc_floppy_format_of(2880), MC_FLOPPY_MFM1440);
    CHECK_EQ(mc_floppy_format_of(3360), MC_FLOPPY_NONE);
    /* 800K ERS 3.4: 394, 429, 472, 525, 590 rpm; the track holds its sectors. */
    static const unsigned rpm[5] = {394, 429, 472, 525, 590};
    for (unsigned z = 0; z < 5; z++) {
        CHECK_EQ(mc_floppy_gcr_rpm(z * 16), rpm[z]);
        CHECK(mc_floppy_track_length(1, 0, z * 16) >= want[z] * 725u);
    }
    CHECK_EQ(mc_floppy_track_length(0, 1, 0), 12500);
    CHECK_EQ(mc_floppy_track_length(0, 0, 0), 6250);
}

static void test_mfm_crc(void) {
    tests++;
    /* SWIM, "CRC Generation/Detection": CCITT polynomial, preset to ones. */
    CHECK_EQ(mc_mfm_crc(0xFFFF, (const uint8_t *)"123456789", 9), 0x29B1);
    uint8_t id[10] = {0xA1, 0xA1, 0xA1, 0xFE, 0, 0, 1, 2};
    uint16_t crc = mc_mfm_crc(0xFFFF, id, 8);
    CHECK_EQ(crc, 0xCA6F);
    id[8] = (uint8_t)(crc >> 8); id[9] = (uint8_t)crc;
    CHECK_EQ(mc_mfm_crc(0xFFFF, id, 10), 0);   /* data and CRC through the generator give zero */
}

typedef struct capture { uint8_t data[18 * 512], tags[12 * 12]; uint32_t got; } capture;
static void capture_sector(void *ctx, unsigned index, const uint8_t *data, const uint8_t *tags) {
    capture *c = ctx;
    memcpy(c->data + index * 512, data, 512);
    if (tags) memcpy(c->tags + index * 12, tags, 12);
    c->got |= 1u << index;
}

static void test_build_decode(void) {
    tests++;
    static mc_floppy_track t;
    static const mc_floppy_format formats[4] = {MC_FLOPPY_GCR400, MC_FLOPPY_GCR800, MC_FLOPPY_MFM720, MC_FLOPPY_MFM1440};
    for (int fi = 0; fi < 4; fi++) {
        mc_floppy_format f = formats[fi];
        for (unsigned track = 0; track < 80; track += 13) {
            for (unsigned side = 0; side < mc_floppy_sides(f); side++) {
                uint8_t data[18 * 512], tags[12 * 12];
                unsigned n = mc_floppy_sectors(f, track);
                for (unsigned i = 0; i < n * 512; i++) data[i] = rnd();
                for (unsigned i = 0; i < n * 12; i++) tags[i] = rnd();
                CHECK(mc_floppy_build_track(f, track, side, data, tags, &t));
                capture c = {{0}, {0}, 0};
                CHECK_EQ(mc_floppy_decode_track(f, track, side, &t, capture_sector, &c), n);
                CHECK_EQ(c.got, (1u << n) - 1);
                CHECK(!memcmp(c.data, data, n * 512));
                if (mc_floppy_is_gcr(f)) CHECK(!memcmp(c.tags, tags, n * 12));
                /* The wrong track or side finds nothing. */
                CHECK_EQ(mc_floppy_decode_track(f, track + 1, side, &t, capture_sector, &c), 0);
            }
        }
    }
    CHECK(!mc_floppy_build_track(MC_FLOPPY_GCR400, 0, 1, NULL, NULL, &t));
}

/* ---- Registers ---- */

static void test_iwm_registers(void) {
    tests++;
    fresh(2);
    /* Read All Ones [000]. */
    latch(7, 0); latch(6, 0); latch(4, 0);
    CHECK_EQ(acc(12), 0xFF);
    /* Mode register echoed in status bits 0-4; bit 5 = an enable active. */
    iwm_mode(0x17);
    latch(6, 1);
    CHECK_EQ(acc(14) & 0x3F, 0x17);
    enable_drive(0);
    CHECK(acc(14) & 0x20);
    latch(4, 0);
    CHECK_EQ(acc(14) & 0x20, 0);   /* timer disabled (mode bit 2): enable drops at once */
    /* Timer enabled: the enable stays about one second (2^23 FCLK). */
    iwm_mode(0x13);
    enable_drive(0);
    latch(4, 0);
    latch(6, 1);
    CHECK(acc(14) & 0x20);
    hw_fake_step(&fake, (1u << 23) + 1);
    CHECK_EQ(acc(14) & 0x20, 0);
    /* Write handshake [10x]: bits 0-5 ones, buffer empty, no underrun. */
    latch(6, 0); latch(7, 1);
    CHECK_EQ(acc(12), 0xFF);
    latch(7, 0);
    /* Drive 2 is fitted: DRVIN low; a single-drive SWIM leaves drive 2 floating. */
    iwm_mode(0x1F);
    enable_drive(1);
    CHECK_EQ(status(ST_DRVIN), 0);
    fresh(1);
    iwm_mode(0x1F);
    enable_drive(1);
    CHECK_EQ(status(ST_DRVIN), 1);
    enable_drive(0);
    CHECK_EQ(status(ST_DRVIN), 0);
    CHECK_EQ(status(ST_SIDES), 1);
    CHECK_EQ(status(ST_MFMDRIVE), 1);
}

static void test_ism_switch(void) {
    tests++;
    fresh(1);
    /* Three writes are not enough; 1, 0, 1, 1 in bit 6 switches. */
    iwm_mode(0x5F); iwm_mode(0x1F); iwm_mode(0x5F);
    CHECK(!swim.ism);
    iwm_mode(0x5F);
    CHECK(swim.ism);
    CHECK(acc(14) & 0x40);              /* STATUS: ISM bit */
    CHECK_EQ(acc(12), 0xF0);            /* PHASE after reset: outputs, low */
    CHECK_EQ(acc(13), 0);               /* SETUP */
    /* Parameter RAM: sixteen bytes, auto-increment, index reset by MODE 0;
     * byte $E reads its EARLY nibble with byte $C's NORMAL nibble (the
     * Quadra 700 ROM's ISM test: $EE written there reads $EC beside $CC). */
    wr(6, 0);
    for (int i = 0; i < 16; i++) wr(3, (uint8_t)(0x10 + i));
    wr(6, 0);
    for (int i = 0; i < 16; i++) CHECK_EQ(acc(11), i == 14 ? 0x1C : 0x10 + i);
    wr(6, 0);
    for (int i = 0; i < 16; i++) wr(3, (uint8_t)(0x11 * i));   /* $00 ... $FF, as the ROM writes them */
    wr(6, 0);
    for (int i = 0; i < 16; i++) CHECK_EQ(acc(11), i == 14 ? 0xEC : 0x11 * i);
    /* SETUP read back; MODE ones/zeros set and clear single bits. */
    wr(5, 0x64);
    CHECK_EQ(acc(13), 0x64);
    wr(7, 0x80);
    CHECK_EQ(acc(14), 0xC0);
    CHECK(acc(15) & 0x10);              /* HANDSHAKE: MotorOn */
    wr(6, 0x80);
    CHECK_EQ(acc(14), 0x40);
    /* Reading the data register with ACTION set and nothing there is an
     * error; ERROR clears when read; HANDSHAKE bit 5 follows it. */
    wr(7, 0x08);
    acc(8);
    CHECK(acc(15) & 0x20);
    CHECK_EQ(acc(10), 0x04);
    CHECK_EQ(acc(10), 0);
    CHECK_EQ(acc(15) & 0x20, 0);
    wr(6, 0x08);
    /* Clearing mode bit 6 returns to the IWM set, phases carried over. */
    wr(4, 0xF5);
    wr(6, 0x40);
    CHECK(!swim.ism);
    CHECK_EQ(swim.phases, 0x05);
    /* Reset returns to the IWM. */
    to_ism();
    CHECK(swim.ism);
    mc_swim_ops.reset(&swim);
    CHECK(!swim.ism);
}

static void test_drive_status(void) {
    tests++;
    fresh(1);
    iwm_mode(0x1F);
    enable_drive(0);
    CHECK_EQ(status(ST_CSTIN), 1);      /* no disk */
    CHECK_EQ(status(ST_WRTPRT), 0);     /* low when empty */
    uint8_t *data;
    mc_disk *disk = open_disk(make_raw("status.img", 1600, &data), 1);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK_EQ(status(ST_CSTIN), 0);
    CHECK_EQ(status(ST_WRTPRT), 1);
    CHECK_EQ(status(ST_TK0), 0);
    CHECK_EQ(status(ST_LOWDENS), 1);    /* 800K: low-density media */
    CHECK_EQ(status(ST_MOTOR), 1);
    CHECK_EQ(status(ST_READY), 1);
    command(CMD_MOTOR, 0);
    CHECK_EQ(status(ST_MOTOR), 0);
    CHECK_EQ(status(ST_READY), 0);
    /* Step inward: /STEP low about 12 ms, then high; /TK0 high. */
    command(CMD_DIRTN, 0);
    command(CMD_STEP, 0);
    CHECK_EQ(status(ST_STEP), 0);
    CHECK_EQ(status(ST_READY), 1);
    hw_fake_step(&fake, (uint64_t)MC_C15M_HZ * 12 / 1000 + 1);
    CHECK_EQ(status(ST_STEP), 1);
    CHECK_EQ(status(ST_TK0), 1);
    CHECK_EQ(swim.drive[0].track, 1);
    command(CMD_DIRTN, 1);
    command(CMD_STEP, 0);
    command(CMD_STEP, 0);               /* never below track 0 */
    CHECK_EQ(swim.drive[0].track, 0);
    CHECK_EQ(status(ST_TK0), 0);
    /* Disabling the drive presets /MOTORON high ([ERS] 3.2.2). */
    latch(4, 0);
    enable_drive(0);
    CHECK_EQ(status(ST_MOTOR), 1);
    /* MFM mode command. */
    command(CMD_MODE, 0);
    CHECK_EQ(status(ST_MFMMODE), 1);
    command(CMD_MODE, 1);
    CHECK_EQ(status(ST_MFMMODE), 0);
    /* Eject. */
    command(CMD_EJECT, 1);
    CHECK_EQ(status(ST_CSTIN), 1);
    int ejected = 0;
    CHECK(mc_swim_media(&swim, 0, &ejected) == disk && ejected);
    CHECK(mc_swim_remove(&swim, 0) == disk);
    CHECK(mc_swim_media(&swim, 0, &ejected) == NULL);
    mc_disk_close(disk);
    /* 1440K media reads as high density. */
    disk = open_disk(make_raw("hd.img", 2880, NULL), 0);
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK_EQ(status(ST_LOWDENS), 0);
    CHECK_EQ(status(ST_WRTPRT), 0);     /* read-only image: locked */
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
}

static unsigned count_tach(unsigned seconds_ms) {
    unsigned edges = 0;
    int last = status(ST_TACH);
    for (uint64_t t = 0; t < (uint64_t)MC_C15M_HZ / 1000 * seconds_ms; t += 2000) {
        hw_fake_step(&fake, 2000);
        int v = status(ST_TACH);
        if (v && !last) edges++;
        last = v;
    }
    return edges;
}

static void test_tach(void) {
    tests++;
    fresh(1);
    iwm_mode(0x1F);
    enable_drive(0);
    mc_disk *disk = open_disk(make_raw("tach.img", 1600, NULL), 0);
    char error[120];
    mc_swim_insert(&swim, 0, disk, 0, error, sizeof error);
    command(CMD_MOTOR, 0);
    command(CMD_DIRTN, 0);
    /* 60 pulses a revolution: rising edges per second = rpm (IM III; ERS 3.4). */
    static const unsigned rpm[5] = {394, 429, 472, 525, 590};
    for (unsigned z = 0; z < 5; z++) {
        while (swim.drive[0].track < z * 16) command(CMD_STEP, 0);
        unsigned edges = count_tach(1000);
        CHECK(edges + 5 >= rpm[z] && edges <= rpm[z] + 5);
    }
    /* MFM mode: the same address reads the index, five a second at 300 rpm. */
    command(CMD_MODE, 0);
    unsigned index = count_tach(1000);
    CHECK(index >= 4 && index <= 6);
    command(CMD_MOTOR, 1);
    CHECK_EQ(count_tach(100), 0);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

/* ---- Data paths ---- */

static void test_iwm_read_gcr(void) {
    tests++;
    fresh(1);
    uint8_t *data, *tags;
    mc_disk *disk = open_disk(make_dc42("read.dc42", &data, &tags), 0);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    iwm_mode(0x1F);
    enable_drive(0);
    command(CMD_MOTOR, 0);
    for (unsigned track = 0; track < 80; track += 39) {
        while (swim.drive[0].track < track) command(CMD_STEP, 0);
        for (unsigned side = 0; side < 2; side++) {
            read_state(side);
            static mc_floppy_track t;
            t.length = mc_floppy_track_length(1, 0, track);
            /* One byte every 256 C15M periods; a second read of the same
             * byte returns nothing (asynchronous mode). */
            for (uint32_t k = 0; k < t.length; k++) {
                hw_fake_step(&fake, 256);
                t.data[k] = acc(12);
                t.mark[k] = 0;
                if (k == 10) CHECK_EQ(acc(12), 0);
            }
            capture c = {{0}, {0}, 0};
            unsigned n = mc_floppy_sectors(MC_FLOPPY_GCR800, track);
            CHECK_EQ(mc_floppy_decode_track(MC_FLOPPY_GCR800, track, side, &t, capture_sector, &c), n);
            int64_t first = mc_floppy_first_sector(MC_FLOPPY_GCR800, track, side);
            CHECK(!memcmp(c.data, data + first * 512, n * 512));
            CHECK(!memcmp(c.tags, tags + first * 12, n * 12));
        }
    }
    /* With the drive in MFM mode the GCR disk delivers no byte. */
    if (SWIM_SUPERDRIVE) {
        command(CMD_MODE, 0);
        read_state(0);
        int any = 0;
        for (int k = 0; k < 2000; k++) { hw_fake_step(&fake, 256); if (acc(12) & 0x80) any = 1; }
        CHECK(!any);
    }
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data); free(tags);
}

/* Writes `n` bytes through the IWM: Q6 on and Q7H write starts, then the
 * handshake (Q6 off) is polled before each byte (written at Q6H). */
static void iwm_write_bytes(const uint8_t *bytes, uint32_t n) {
    latch(6, 1);
    wr(15, bytes[0]);
    for (uint32_t i = 1; i < n; i++) {
        uint8_t h;
        do { hw_fake_step(&fake, 64); latch(6, 0); h = acc(12); } while (!(h & 0x80));
        CHECK(h & 0x40);
        wr(13, bytes[i]);
    }
}

static void test_iwm_write_gcr(void) {
    tests++;
    fresh(1);
    uint8_t *data, *tags;
    const char *path = make_dc42("write.dc42", &data, &tags);
    mc_disk *disk = open_disk(path, 1);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    iwm_mode(0x1F);
    enable_drive(0);
    command(CMD_MOTOR, 0);
    const unsigned track = 20, side = 1;
    while (swim.drive[0].track < track) command(CMD_STEP, 0);
    read_state(side);
    /* A format of the whole track with new sectors and tags. */
    unsigned n = mc_floppy_sectors(MC_FLOPPY_GCR800, track);
    uint8_t ndata[12 * 512], ntags[12 * 12];
    for (unsigned i = 0; i < n * 512; i++) ndata[i] = rnd();
    for (unsigned i = 0; i < n * 12; i++) ntags[i] = rnd();
    static mc_floppy_track t;
    mc_floppy_build_track(MC_FLOPPY_GCR800, track, side, ndata, ntags, &t);
    iwm_write_bytes(t.data, t.length);
    latch(7, 0);                        /* write ends: the track is decoded back */
    int64_t first = mc_floppy_first_sector(MC_FLOPPY_GCR800, track, side);
    uint8_t back[12 * 512], back_tags[12 * 12];
    CHECK(mc_disk_read(disk, (uint64_t)first, n, back));
    CHECK(!memcmp(back, ndata, n * 512));
    CHECK(mc_disk_read_tags(disk, (uint64_t)first, n, back_tags));
    CHECK(!memcmp(back_tags, ntags, n * 12));
    /* The other side is untouched. */
    CHECK(mc_disk_read(disk, (uint64_t)first - n, 1, back));
    CHECK(!memcmp(back, data + (first - n) * 512, 512));
    /* Underrun: no byte loaded in time clears handshake bit 6 until the
     * write ends. */
    latch(6, 1);
    wr(15, 0xFF);
    hw_fake_step(&fake, 1000);
    latch(6, 0);
    CHECK_EQ(acc(12) & 0x40, 0);
    latch(7, 0);
    latch(7, 1);
    CHECK(acc(12) & 0x40);
    latch(7, 0);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    /* The image reopens (data checksum valid); the tag checksum is current. */
    disk = open_disk(path, 0);
    mc_disk_close(disk);
    FILE *f = fopen(path, "rb");
    uint8_t *img = malloc(0x54 + 1600 * 512 + 1600 * 12);
    CHECK(f && fread(img, 1, 0x54 + 1600 * 512 + 1600 * 12, f) == 0x54 + 1600 * 512 + 1600 * 12);
    if (f) fclose(f);
    CHECK_EQ(get32(img + 0x4C), dc_sum(img + 0x54 + 1600 * 512 + 12, 1600 * 12 - 12));
    CHECK(!memcmp(img + 0x54 + 1600 * 512 + first * 12, ntags, n * 12));
    free(img); free(data); free(tags);
}

static void test_ism_mfm(void) {
    tests++;
    fresh(1);
    uint8_t *data;
    const char *path = make_raw("mfm.img", 2880, &data);
    mc_disk *disk = open_disk(path, 1);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    iwm_mode(0x1F);
    to_ism();
    wr(5, 0x00);                        /* SETUP: MFM, trans-space machine on */
    wr(7, 0x82);                        /* MODE: MotorOn, drive 1 */
    ism_command(CMD_MODE, 0);       /* the drive in MFM mode */
    ism_command(CMD_MOTOR, 0);
    const unsigned track = 0, side = 0;
    mc_swim_set_headsel(&swim, (int)side);
    /* Read: clear the FIFO, then ACTION; the first byte is a mark. */
    wr(7, 0x01); wr(6, 0x01);
    CHECK_EQ(acc(10), 0);
    wr(7, 0x08);
    static mc_floppy_track t;
    static uint8_t crc_bit[MC_FLOPPY_MAX_TRACK];
    t.length = 12500;
    for (uint32_t k = 0; k < t.length; k++) {
        uint8_t h;
        int spins = 0;
        do { hw_fake_step(&fake, 64); h = acc(15); } while (!(h & 0x80) && ++spins < 100000);
        t.mark[k] = h & 1;
        crc_bit[k] = h >> 1 & 1;
        t.data[k] = acc(t.mark[k] ? 9 : 8);
    }
    CHECK(t.mark[0]);
    CHECK_EQ(acc(10), 0);               /* no error while keeping up */
    capture c = {{0}, {0}, 0};
    CHECK_EQ(mc_floppy_decode_track(MC_FLOPPY_MFM1440, track, side, &t, capture_sector, &c), 18);
    CHECK(!memcmp(c.data, data, 18 * 512));
    /* HANDSHAKE bit 1: the CRC up to and including the byte about to be
     * read is zero exactly at an ID field's second CRC byte. */
    int ids = 0;   /* the capture may begin inside the first ID's mark run */
    for (uint32_t k = 3; k + 6 < t.length; k++) {
        if (t.data[k] == 0xFE && t.mark[k - 1] && t.data[k - 1] == 0xA1) {
            CHECK_EQ(crc_bit[k + 6], 0);
            CHECK_EQ(crc_bit[k + 5], 1);
            ids++;
        }
    }
    CHECK(ids >= 17);
    /* A mark read from the data register is an error. */
    wr(6, 0x08); wr(7, 0x01); wr(6, 0x01);
    wr(7, 0x08);
    while (!(acc(15) & 0x80)) hw_fake_step(&fake, 64);
    CHECK(acc(15) & 0x01);
    acc(8);
    CHECK_EQ(acc(10), 0x02);
    wr(6, 0x08);
    /* Write a whole track through the FIFO: sync, marks through MARK, CRCs
     * through the CRC register, gaps to the end of the track. */
    uint8_t ndata[18 * 512];
    for (unsigned i = 0; i < sizeof ndata; i++) ndata[i] = rnd();
    static uint8_t out[13000], kind[13000];     /* kind: 0 data, 1 mark, 2 CRC */
    uint32_t n = 0;
#define PUT(v, k) (out[n] = (uint8_t)(v), kind[n] = (k), n++)
    for (int i = 0; i < 60; i++) PUT(0x4E, 0);
    for (unsigned s = 0; s < 18; s++) {
        for (int i = 0; i < 12; i++) PUT(0x00, 0);
        for (int i = 0; i < 3; i++) PUT(0xA1, 1);
        PUT(0xFE, 0); PUT(track, 0); PUT(side, 0); PUT(s + 1, 0); PUT(2, 0); PUT(0, 2);
        for (int i = 0; i < 22; i++) PUT(0x4E, 0);
        for (int i = 0; i < 12; i++) PUT(0x00, 0);
        for (int i = 0; i < 3; i++) PUT(0xA1, 1);
        PUT(0xFB, 0);
        for (int i = 0; i < 512; i++) PUT(ndata[s * 512 + i], 0);
        PUT(0, 2);
        for (int i = 0; i < 80; i++) PUT(0x4E, 0);
    }
    /* Each CRC entry sends two bytes: pad to exactly one revolution. */
    while (n + 18 * 2 < 12500) PUT(0x4E, 0);
#undef PUT
    wr(7, 0x10);                        /* write mode */
    wr(7, 0x01); wr(6, 0x01);
    wr(0, out[0]);
    wr(7, 0x08);                        /* ACTION */
    for (uint32_t i = 1; i < n; i++) {
        while (!(acc(15) & 0x80)) hw_fake_step(&fake, 64);
        wr(kind[i] == 1 ? 1 : kind[i] == 2 ? 2 : 0, out[i]);
    }
    while (!(acc(15) & 0x40)) hw_fake_step(&fake, 64);
    wr(6, 0x08);
    CHECK_EQ(acc(10), 0);
    uint8_t back[18 * 512];
    CHECK(mc_disk_read(disk, 0, 18, back));
    CHECK(!memcmp(back, ndata, sizeof back));
    CHECK(mc_disk_read(disk, 18, 1, back));
    CHECK(!memcmp(back, data + 18 * 512, 512));
    /* Holding ACTION past the last byte is an underrun, which clears ACTION. */
    wr(7, 0x01); wr(6, 0x01);
    wr(0, 0x4E);
    wr(7, 0x08);
    hw_fake_step(&fake, 2000);
    CHECK_EQ(acc(14) & 0x08, 0);
    CHECK_EQ(acc(10), 0x01);
    wr(6, 0x90);                        /* read mode, motor off */
    wr(6, 0x40);                        /* back to the IWM */
    CHECK(!swim.ism);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
}

static void test_protect_and_refusals(void) {
    tests++;
    fresh(2);
    char error[120];
    uint8_t *data;
    /* A read-only image is locked and never written. */
    const char *path = make_raw("locked.img", 1600, &data);
    mc_disk *disk = open_disk(path, 0);
    CHECK(mc_swim_insert(&swim, 1, disk, 0, error, sizeof error));
    iwm_mode(0x1F);
    enable_drive(1);
    CHECK_EQ(status(ST_WRTPRT), 0);
    command(CMD_MOTOR, 0);
    read_state(0);
    static mc_floppy_track t;
    uint8_t ndata[12 * 512];
    for (unsigned i = 0; i < sizeof ndata; i++) ndata[i] = rnd();
    mc_floppy_build_track(MC_FLOPPY_GCR800, 0, 0, ndata, NULL, &t);
    iwm_write_bytes(t.data, t.length);
    latch(7, 0);
    uint8_t back[12 * 512];
    CHECK(mc_disk_read(disk, 0, 12, back));
    CHECK(!memcmp(back, data, sizeof back));
    mc_swim_remove(&swim, 1);
    mc_disk_close(disk);
    /* A writable image locked by the user is not written either. */
    disk = open_disk(path, 1);
    CHECK(mc_swim_insert(&swim, 0, disk, 1, error, sizeof error));
    enable_drive(0);
    CHECK_EQ(status(ST_WRTPRT), 0);
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
    /* 1680 KiB, other sizes and 2048-byte media are refused; 400K is taken. */
    disk = open_disk(make_raw("dmf.img", 3360, NULL), 0);
    CHECK(!mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK(strstr(error, "1680") != NULL);
    mc_disk_close(disk);
    disk = open_disk(make_raw("odd.img", 1000, NULL), 0);
    CHECK(!mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    mc_disk_close(disk);
    const char *iso = make_raw("iso.img", 3200, NULL);
    char problem[120];
    disk = mc_disk_open(iso, 0, 2048, problem, sizeof problem);
    CHECK(disk && mc_disk_sectors(disk) == 800);
    if (disk) { CHECK(!mc_swim_insert(&swim, 0, disk, 0, error, sizeof error)); mc_disk_close(disk); }
    disk = open_disk(make_raw("sides.img", 800, NULL), 0);
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    CHECK(!mc_swim_insert(&swim, 2, disk, 0, error, sizeof error));
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
}

static void test_gcr400(void) {
    tests++;
    fresh(1);
    uint8_t *data;
    mc_disk *disk = open_disk(make_raw("400k.img", 800, &data), 0);
    char error[120];
    CHECK(mc_swim_insert(&swim, 0, disk, 0, error, sizeof error));
    iwm_mode(0x1F);
    enable_drive(0);
    command(CMD_MOTOR, 0);
    command(CMD_DIRTN, 0);
    for (int i = 0; i < 70; i++) command(CMD_STEP, 0);
    for (unsigned side = 0; side < 2; side++) {
        read_state(side);
        static mc_floppy_track t;
        t.length = mc_floppy_track_length(1, 0, 70);
        for (uint32_t k = 0; k < t.length; k++) { hw_fake_step(&fake, 256); t.data[k] = acc(12); t.mark[k] = 0; }
        capture c = {{0}, {0}, 0};
        unsigned got = mc_floppy_decode_track(MC_FLOPPY_GCR400, 70, side, &t, capture_sector, &c);
        if (side == 0) {
            CHECK_EQ(got, 8);
            CHECK(!memcmp(c.data, data + mc_floppy_first_sector(MC_FLOPPY_GCR400, 70, 0) * 512, 8 * 512));
        } else {
            CHECK_EQ(got, 0);           /* a one-sided disk: nothing under head 1 */
        }
    }
    mc_swim_remove(&swim, 0);
    mc_disk_close(disk);
    free(data);
}

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* test_iwm.c includes this file without running all of it */
#endif
static void run_swim_tests(void) {
    test_gcr_table();
    test_gcr_data();
    test_gcr_address();
    test_zones();
    test_mfm_crc();
    test_build_decode();
    test_iwm_registers();
    test_ism_switch();
    test_drive_status();
    test_tach();
    test_iwm_read_gcr();
    test_iwm_write_gcr();
    test_ism_mfm();
    test_protect_and_refusals();
    test_gcr400();
}

#ifndef SWIM_INCLUDE
int main(void) {
    run_swim_tests();
    printf("test_swim: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
#endif
