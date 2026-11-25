/* The virtual disk against Inside Macintosh:
 * Devices, chapter 3 (Block0 and Partition records). */
#include "scsi_util.h"
#include "vdisk.h"

#include <string.h>

static uint8_t driver[3000];
static uint8_t blk[512 * 8];

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static mc_disk *open_image(const char *name, uint32_t bytes, uint8_t tag, int writable, uint32_t sector) {
    char error[200];
    mc_disk *d = mc_disk_open(scsi_make_image("vdisk", name, bytes, tag), writable, sector, error, sizeof error);
    CHECK(d != NULL);
    return d;
}

/* One bare image: the DDM, the map's three entries, the driver area and
 * the image at 128. */
static void test_layout(void) {
    for (unsigned i = 0; i < sizeof driver; i++) driver[i] = (uint8_t)(i * 3 + 1);
    mc_disk *img = open_image("one.img", 300 * 512, 0x31, 1, 512);
    mc_vdisk_drivers d = {driver, sizeof driver, NULL, 0};
    char error[200];
    mc_vdisk *vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    CHECK(vd != NULL);
    if (!vd) return;
    CHECK(!mc_vdisk_passthrough(vd));
    CHECK_EQ(mc_vdisk_blocks(vd), 128 + 300);
    CHECK(mc_vdisk_read(vd, 0, 1, blk));
    CHECK_EQ(be16(blk), 0x4552);                /* sbSig */
    CHECK_EQ(be16(blk + 2), 512);               /* sbBlkSize */
    CHECK_EQ(be32(blk + 4), 428);               /* sbBlkCount */
    CHECK_EQ(be16(blk + 16), 1);                /* sbDrvrCount */
    CHECK_EQ(be32(blk + 18), 64);               /* ddBlock */
    CHECK_EQ(be16(blk + 22), 6);                /* ddSize: 3000 bytes in 512-byte blocks */
    CHECK_EQ(be16(blk + 24), 1);                /* ddType: Mac OS */
    CHECK(mc_vdisk_read(vd, 1, 4, blk));
    const uint8_t *e = blk;
    CHECK_EQ(be16(e), 0x504D); CHECK_EQ(be32(e + 4), 3);
    CHECK_EQ(be32(e + 8), 1); CHECK_EQ(be32(e + 12), 63);
    CHECK(!strcmp((const char *)e + 48, "Apple_partition_map"));
    e += 512;
    CHECK_EQ(be16(e), 0x504D); CHECK_EQ(be32(e + 4), 3);
    CHECK(!strcmp((const char *)e + 48, "Apple_Driver43"));
    CHECK(strncmp((const char *)e + 16, "Maci", 4) != 0);  /* no checksum verification by the Start Manager */
    CHECK_EQ(be32(e + 8), 64); CHECK_EQ(be32(e + 12), 64);
    CHECK_EQ(be32(e + 96), sizeof driver);      /* pmBootSize */
    CHECK_EQ(be32(e + 116), mc_vdisk_boot_checksum(driver, sizeof driver));
    CHECK(!strcmp((const char *)e + 120, "68000"));
    CHECK_EQ(be32(e + 88), 0x7F);
    e += 512;
    CHECK(!strcmp((const char *)e + 48, "Apple_HFS"));
    CHECK_EQ(be32(e + 8), 128); CHECK_EQ(be32(e + 12), 300);
    CHECK_EQ(be32(e + 84), 300); CHECK_EQ(be32(e + 88), 0x37);
    CHECK(!strcmp((const char *)e + 16, "one.img"));
    e += 512;
    CHECK_EQ(be16(e), 0);                       /* nothing after the last entry */
    CHECK(mc_vdisk_read(vd, 64, 6, blk));
    CHECK(!memcmp(blk, driver, sizeof driver));
    CHECK_EQ(blk[sizeof driver], 0);
    CHECK(mc_vdisk_read(vd, 128 + 7, 1, blk));
    CHECK_EQ(be32(blk), 7); CHECK_EQ(blk[4], 0x31);
    /* A read crossing the head and the image. */
    CHECK(mc_vdisk_read(vd, 127, 2, blk));
    CHECK_EQ(be32(blk + 512), 0);
    CHECK(!mc_vdisk_read(vd, 427, 2, blk));     /* past the end */
    /* Writes to blocks 0-127 go to the session overlay, not the image. */
    memset(blk, 0xAB, 512);
    CHECK(mc_vdisk_write(vd, 0, 1, blk));
    CHECK_EQ(mc_vdisk_overlay_writes(vd), 1);
    CHECK(mc_vdisk_read(vd, 0, 1, blk) && blk[0] == 0xAB);
    uint8_t raw[512];
    CHECK(mc_disk_read(img, 0, 1, raw) && be32(raw) == 0);
    /* Writes to the HFS area go to the image. */
    memset(blk, 0x5C, 1024);
    CHECK(mc_vdisk_write(vd, 127, 3, blk));
    CHECK_EQ(mc_vdisk_overlay_writes(vd), 2);
    CHECK(mc_disk_read(img, 0, 1, raw) && raw[0] == 0x5C && raw[511] == 0x5C);
    mc_vdisk_destroy(vd);
    mc_disk_close(img);
}

/* Several bare images: one Apple_HFS each, back to back; a read-only image
 * refuses writes (status without the write bit). */
static void test_several(void) {
    mc_disk *imgs[3] = {open_image("a.img", 40 * 512, 0xA1, 1, 512), open_image("b.img", 50 * 512, 0xB2, 0, 512),
                        open_image("c.img", 60 * 512, 0xC3, 1, 512)};
    mc_vdisk_drivers d = {driver, 600, NULL, 0};
    char error[200];
    mc_vdisk *vd = mc_vdisk_create(imgs, 3, &d, error, sizeof error);
    CHECK(vd != NULL);
    if (!vd) return;
    CHECK_EQ(mc_vdisk_blocks(vd), 128 + 150);
    CHECK(mc_vdisk_read(vd, 1, 5, blk));
    CHECK_EQ(be32(blk + 4), 5);
    CHECK_EQ(be32(blk + 3 * 512 + 8), 168); CHECK_EQ(be32(blk + 3 * 512 + 12), 50);
    CHECK_EQ(be32(blk + 3 * 512 + 88), 0x17);
    CHECK_EQ(be32(blk + 4 * 512 + 8), 218);
    CHECK(mc_vdisk_read(vd, 218 + 9, 1, blk) && be32(blk) == 9 && blk[4] == 0xC3);
    CHECK(mc_vdisk_readonly(vd, 170, 1));
    CHECK(!mc_vdisk_readonly(vd, 130, 1));
    CHECK(!mc_vdisk_write(vd, 167, 2, blk));    /* touches the read-only image */
    CHECK(!mc_vdisk_all_readonly(vd));
    mc_vdisk_destroy(vd);
    for (int i = 0; i < 3; i++) mc_disk_close(imgs[i]);
}

/* An image that already has a DDM is presented as is; it cannot share a
 * virtual disk. */
static void test_passthrough(void) {
    mc_disk *img = open_image("parted.img", 100 * 512, 0x44, 1, 512);
    uint8_t ddm[512] = {0x45, 0x52, 0x02, 0x00};
    CHECK(mc_disk_write(img, 0, 1, ddm));
    char error[200];
    mc_vdisk_drivers d = {driver, 600, NULL, 0};
    mc_vdisk *vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    CHECK(vd && mc_vdisk_passthrough(vd));
    if (vd) {
        CHECK_EQ(mc_vdisk_blocks(vd), 100);
        CHECK(mc_vdisk_read(vd, 5, 1, blk) && be32(blk) == 5);
        blk[0] = 0x77;
        CHECK(mc_vdisk_write(vd, 0, 1, blk));
        CHECK_EQ(mc_vdisk_overlay_writes(vd), 0);
        uint8_t raw[512];
        CHECK(mc_disk_read(img, 0, 1, raw) && raw[0] == 0x77);
        mc_vdisk_destroy(vd);
    }
    mc_disk *other = open_image("plain.img", 20 * 512, 0x45, 1, 512);
    mc_disk *both[2] = {other, img};
    CHECK(mc_disk_write(img, 0, 1, ddm));
    vd = mc_vdisk_create(both, 2, &d, error, sizeof error);
    CHECK(vd == NULL && strstr(error, "own SCSI ID") != NULL);
    mc_disk_close(img);
    mc_disk_close(other);
}

/* A 2048-byte-sector image after an odd-sized one starts on a 2048-byte
 * boundary; the gap is an Apple_Free partition. */
static void test_alignment(void) {
    mc_disk *imgs[2] = {open_image("odd.img", 5 * 512, 0x51, 1, 512), open_image("cd.iso", 4 * 2048, 0x52, 0, 2048)};
    mc_vdisk_drivers d = {driver, 600, NULL, 0};
    char error[200];
    mc_vdisk *vd = mc_vdisk_create(imgs, 2, &d, error, sizeof error);
    CHECK(vd != NULL);
    if (vd) {
        CHECK_EQ(mc_vdisk_image_start(vd, 1), 136);
        CHECK_EQ(mc_vdisk_blocks(vd), 136 + 16);
        CHECK(mc_vdisk_read(vd, 1, 5, blk));
        CHECK_EQ(be32(blk + 4), 5);
        CHECK(!strcmp((const char *)blk + 3 * 512 + 48, "Apple_Free"));
        CHECK_EQ(be32(blk + 3 * 512 + 8), 133); CHECK_EQ(be32(blk + 3 * 512 + 12), 3);
        CHECK(mc_vdisk_read(vd, 136 + 5, 1, blk) && be32(blk) == 5 && blk[4] == 0x52);
        CHECK(mc_vdisk_read(vd, 134, 1, blk) && blk[0] == 0 && blk[100] == 0);
        mc_vdisk_destroy(vd);
    }
    mc_disk_close(imgs[0]);
    mc_disk_close(imgs[1]);
}

/* The ATA driver slot (5.1e) and the checksum's arithmetic: a 16-bit sum
 * rotated left once per byte; zero becomes $FFFF. Too large a driver is
 * refused. */
static void test_drivers(void) {
    const uint8_t one[1] = {1}, two[2] = {0x80, 0x00};
    CHECK_EQ(mc_vdisk_boot_checksum(one, 1), 2);
    CHECK_EQ(mc_vdisk_boot_checksum(two, 2), 0x0200);
    CHECK_EQ(mc_vdisk_boot_checksum(two + 1, 1), 0xFFFF);
    mc_disk *img = open_image("ata.img", 10 * 512, 0x61, 1, 512);
    static uint8_t ata[700];
    mc_vdisk_drivers d = {driver, 600, ata, sizeof ata};
    char error[200];
    mc_vdisk *vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    CHECK(vd != NULL);
    if (vd) {
        CHECK(mc_vdisk_read(vd, 0, 4, blk));
        CHECK_EQ(be16(blk + 16), 2);
        CHECK_EQ(be32(blk + 26), 66); CHECK_EQ(be16(blk + 30), 2); CHECK_EQ(be16(blk + 32), 0x0701);
        CHECK_EQ(be32(blk + 512 + 4), 4);
        CHECK(!strcmp((const char *)blk + 3 * 512 + 48, "Apple_Driver_ATA"));
        CHECK_EQ(be32(blk + 3 * 512 + 8), 66); CHECK_EQ(be32(blk + 3 * 512 + 12), 62);
        mc_vdisk_destroy(vd);
    }
    static uint8_t huge[64 * 512 + 1];
    mc_vdisk_drivers big = {huge, sizeof huge, NULL, 0};
    CHECK(mc_vdisk_create(&img, 1, &big, error, sizeof error) == NULL);
    mc_disk_close(img);
}

/* A real uninitialized drive must retain guest partition/driver writes,
 * including LBA0, after the emulator closes and reopens the device. */
static void test_raw_device(void){
    char error[200];const char *path=scsi_make_image("vdisk","raw.img",300*512,0x31);
    mc_disk *img=mc_disk_open(path,1,512,error,sizeof error);
    mc_vdisk *v=mc_vdisk_create_device(img,error,sizeof error);CHECK(v!=NULL);if(!v)return;
    CHECK(mc_vdisk_passthrough(v));CHECK_EQ(mc_vdisk_blocks(v),300);
    memset(blk,0,512);blk[0]=0x45;blk[1]=0x52;blk[2]=2;
    CHECK(mc_vdisk_write(v,0,1,blk));memset(blk,0x5A,512);CHECK(mc_vdisk_write(v,299,1,blk));
    CHECK(!mc_vdisk_write(v,300,1,blk));mc_vdisk_destroy(v);mc_disk_close(img);
    img=mc_disk_open(path,1,512,error,sizeof error);v=mc_vdisk_create_device(img,error,sizeof error);
    CHECK(v!=NULL);if(v){CHECK(mc_vdisk_read(v,0,1,blk));CHECK_EQ(be16(blk),0x4552);CHECK_EQ(be16(blk+2),512);
    CHECK(mc_vdisk_read(v,299,1,blk));CHECK_EQ(blk[0],0x5A);CHECK_EQ(blk[511],0x5A);mc_vdisk_destroy(v);}mc_disk_close(img);
}

/* The ATAPI layout (vdisk.h): everything in 2048-byte blocks, one map
 * entry at the start of each, the driver's entry starting at ddBlock. */
static void test_atapi(void) {
    for (unsigned i = 0; i < sizeof driver; i++) driver[i] = (uint8_t)(i * 5 + 3);
    mc_disk *img = open_image("cd.img", 400 * 2048, 0x41, 0, 2048);
    mc_vdisk_drivers d = {driver, sizeof driver, driver, sizeof driver, 1};
    char error[200];
    mc_vdisk *vd = mc_vdisk_create(&img, 1, &d, error, sizeof error);
    CHECK(vd != NULL);
    if (!vd) return;
    CHECK_EQ(mc_vdisk_blocks(vd), 128 + 1600);
    CHECK(mc_vdisk_read(vd, 0, 1, blk));
    CHECK_EQ(be16(blk + 2), 2048);              /* sbBlkSize */
    CHECK_EQ(be32(blk + 4), 432);               /* sbBlkCount */
    CHECK_EQ(be16(blk + 16), 1);
    CHECK_EQ(be32(blk + 18), 16);               /* ddBlock */
    CHECK_EQ(be16(blk + 22), 2);                /* ddSize: 3000 bytes in 2048-byte blocks */
    CHECK_EQ(be16(blk + 24), 0x0701);
    CHECK(mc_vdisk_read(vd, 8, 1, blk));         /* entry 2 at 2048-byte block 2 */
    CHECK_EQ(be16(blk), 0x504D);
    CHECK_EQ(be32(blk + 4), 3);                 /* pmMapBlkCnt */
    CHECK_EQ(be32(blk + 8), 16);                /* pmPyPartStart = ddBlock */
    CHECK(!strcmp((const char *)blk + 48, "Apple_Driver_ATAPI"));
    CHECK(!memcmp(blk + 16, "Maci", 4));
    CHECK_EQ(be32(blk + 96), sizeof driver);    /* pmBootSize */
    CHECK_EQ(be32(blk + 116), mc_vdisk_boot_checksum(driver, sizeof driver));
    CHECK(mc_vdisk_read(vd, 12, 1, blk));
    CHECK(!strcmp((const char *)blk + 48, "Apple_HFS"));
    CHECK_EQ(be32(blk + 8), 32);
    CHECK_EQ(be32(blk + 12), 400);
    CHECK(mc_vdisk_read(vd, 64, 1, blk));
    CHECK(!memcmp(blk, driver, 512));
    CHECK(mc_vdisk_read(vd, 129, 1, blk));      /* the image's block 1 */
    CHECK_EQ(blk[3], 1);
    CHECK_EQ(blk[4], 0x41);
    mc_vdisk_destroy(vd);
    mc_disk_close(img);
}

int main(void) {
    int tests = 0;
#define RUN(t) (t(), tests++)
    RUN(test_raw_device);
    RUN(test_layout);
    RUN(test_several);
    RUN(test_passthrough);
    RUN(test_alignment);
    RUN(test_drivers);
    RUN(test_atapi);
    printf("test_vdisk: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures != 0;
}
