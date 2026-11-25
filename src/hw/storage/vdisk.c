/* Livadix — the virtual disk (vdisk.h). Layouts from Inside Macintosh: Devices,
 * chapter 3, "Data Structures": Block0 (sbSig $4552 at 0, sbBlkSize 2,
 * sbBlkCount 4, sbDevType 8, sbDevId 10, sbData 12, sbDrvrCount 16, then
 * ddBlock/ddSize/ddType from 18 in 8-byte steps) and Partition (pmSig $504D
 * at 0, pmSigPad 2, pmMapBlkCnt 4, pmPyPartStart 8, pmPartBlkCnt 12,
 * pmPartName 16, pmParType 48, pmLgDataStart 80, pmDataCnt 84, pmPartStatus
 * 88, pmLgBootStart 92, pmBootSize 96, pmBootAddr 100, pmBootAddr2 104,
 * pmBootEntry 108, pmBootEntry2 112, pmBootCksum 116, pmProcessor 120). */
#include "vdisk.h"
#include "hfs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK 512u

typedef struct part {
    uint64_t start, blocks;     /* device blocks */
    int image;                  /* index, or -1 for a gap */
} part;

struct mc_vdisk {
    int passthrough;
    unsigned count;
    mc_disk *img[MC_VDISK_MAX_IMAGES];
    part parts[MC_VDISK_MAX_IMAGES * 2];
    unsigned nparts;
    uint64_t blocks;
    uint8_t *head;                          /* blocks 0-127 as synthesized */
    uint8_t *overlay;                       /* blocks 0-127 as the guest wrote them */
    uint8_t written[MC_VDISK_HEAD_BLOCKS];
    uint64_t overlay_writes;
    /* A volume left mounted that the ROM's check would reject for its tree
     * files' sizes alone (hfs.h, mc_hfs_left_mounted): its master directory
     * block as that check would have left it, read in place of the image's
     * until the guest writes that block. */
    uint8_t *checked[MC_VDISK_MAX_IMAGES];
    uint64_t checked_at[MC_VDISK_MAX_IMAGES];         /* its byte offset in the image */
};

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_str(uint8_t *p, size_t field, const char *s) {
    size_t n = strlen(s);
    if (n >= field) n = field - 1;
    memcpy(p, s, n);
}

uint32_t mc_vdisk_boot_checksum(const uint8_t *code, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum = (sum + code[i]) & 0xFFFF;
        sum = ((sum << 1) | (sum >> 15)) & 0xFFFF;
    }
    return sum ? sum : 0xFFFF;
}

static uint64_t image_blocks(const mc_disk *d) { return mc_disk_sectors(d) * mc_disk_sector_size(d) / BLOCK; }

/* A partition map entry (IM: Devices 3-25, 3-26). Status bits: 0 valid,
 * 1 allocated, 2 in use, 3 boot information, 4 readable, 5 writable, 6 boot
 * code position-independent. */
static void map_entry(uint8_t *e, uint32_t entries, uint64_t start, uint64_t blocks, const char *name, const char *type,
                      uint32_t status) {
    put16(e + 0, 0x504D);
    put32(e + 4, entries);
    put32(e + 8, (uint32_t)start);
    put32(e + 12, (uint32_t)blocks);
    put_str(e + 16, 32, name);
    put_str(e + 48, 32, type);
    put32(e + 84, (uint32_t)blocks);
    put32(e + 88, status);
}

static void driver_entry(uint8_t *e, uint32_t entries, uint64_t start, uint64_t blocks, const char *name, const char *type,
                         const uint8_t *code, size_t size) {
    map_entry(e, entries, start, blocks, name, type, 0x7F);
    put32(e + 96, (uint32_t)size);              /* pmBootSize */
    put32(e + 116, mc_vdisk_boot_checksum(code, size));
    put_str(e + 120, 16, "68000");
}

/* The ATAPI layout (vdisk.h): the G3 ROM's ATAPI boot code reads the map
 * one entry per 2048-byte block (the first 512 bytes of each, ROM offset
 * $288460), takes the descriptor's ddBlock and ddSize in 2048-byte blocks
 * and loads the driver whose Apple_Driver_ATAPI entry, named "Maci...",
 * starts at ddBlock ($287F10), checked against pmBootSize and pmBootCksum
 * ($2889F0). */
static void synthesize_atapi(mc_vdisk *vd, const mc_vdisk_drivers *drv) {
    const uint32_t unit = 2048u / BLOCK, head = MC_VDISK_HEAD_BLOCKS / unit, driver = MC_VDISK_DRIVER_BLOCK / unit;
    const uint32_t ata_blocks = (uint32_t)((drv->ata_size + 2047u) / 2048u);
    vd->nparts = 1;
    vd->parts[0] = (part){MC_VDISK_HEAD_BLOCKS, image_blocks(vd->img[0]), 0};
    vd->blocks = MC_VDISK_HEAD_BLOCKS + image_blocks(vd->img[0]);
    uint8_t *h = vd->head;
    put16(h + 0, 0x4552);
    put16(h + 2, 2048);
    put32(h + 4, (uint32_t)(vd->blocks / unit));
    put16(h + 16, 1);
    put32(h + 18, driver);
    put16(h + 22, (uint16_t)ata_blocks);
    put16(h + 24, MC_VDISK_DDM_ATA);
    map_entry(h + 2048u, 3, 1, driver - 1u, "Apple", "Apple_partition_map", 0x37);
    driver_entry(h + 2u * 2048u, 3, driver, head - driver, "Macintosh ATAPI driver", "Apple_Driver_ATAPI", drv->ata, drv->ata_size);
    map_entry(h + 3u * 2048u, 3, head, image_blocks(vd->img[0]) / unit, mc_disk_label(vd->img[0]), "Apple_HFS", 0x17);
    memcpy(h + MC_VDISK_DRIVER_BLOCK * BLOCK, drv->ata, drv->ata_size);
    memcpy(vd->overlay, vd->head, MC_VDISK_HEAD_BLOCKS * BLOCK);
}

static int synthesize(mc_vdisk *vd, const mc_vdisk_drivers *drv, char *error, size_t capacity) {
    if (drv->atapi && drv->ata && vd->count == 1 && image_blocks(vd->img[0]) % (2048u / BLOCK) == 0 &&
        drv->ata_size <= (MC_VDISK_HEAD_BLOCKS - MC_VDISK_DRIVER_BLOCK) * BLOCK) {
        synthesize_atapi(vd, drv);
        return 1;
    }
    unsigned scsi_blocks = (unsigned)((drv->scsi_size + BLOCK - 1) / BLOCK);
    unsigned ata_blocks = drv->ata ? (unsigned)((drv->ata_size + BLOCK - 1) / BLOCK) : 0;
    unsigned area = MC_VDISK_HEAD_BLOCKS - MC_VDISK_DRIVER_BLOCK;
    if (!drv->scsi || !drv->scsi_size || scsi_blocks + ata_blocks > area) {
        snprintf(error, capacity, "The Livadix driver images do not fit the driver area");
        return 0;
    }
    /* Partitions: each image after the previous; a 2048-byte-sector image
     * starts on a 2048-byte boundary, a gap before it is an Apple_Free
     * partition (every block but block 0 belongs to a partition). */
    uint64_t at = MC_VDISK_HEAD_BLOCKS;
    vd->nparts = 0;
    for (unsigned i = 0; i < vd->count; i++) {
        uint64_t align = mc_disk_sector_size(vd->img[i]) / BLOCK;
        uint64_t start = (at + align - 1) / align * align;
        if (start > at) vd->parts[vd->nparts++] = (part){at, start - at, -1};
        vd->parts[vd->nparts++] = (part){start, image_blocks(vd->img[i]), (int)i};
        at = start + image_blocks(vd->img[i]);
    }
    vd->blocks = at;
    if (vd->blocks > 0xFFFFFFFFu) {
        snprintf(error, capacity, "The images are too large for one virtual disk (2 TiB)");
        return 0;
    }
    uint32_t entries = 2 + (ata_blocks ? 1 : 0) + vd->nparts;
    uint8_t *h = vd->head;
    /* Block 0: the Driver Descriptor Map. */
    put16(h + 0, 0x4552);
    put16(h + 2, BLOCK);
    put32(h + 4, (uint32_t)vd->blocks);
    put16(h + 16, ata_blocks ? 2 : 1);
    put32(h + 18, MC_VDISK_DRIVER_BLOCK);
    put16(h + 22, scsi_blocks);
    put16(h + 24, MC_VDISK_DDM_SCSI);
    if (ata_blocks) {
        put32(h + 26, MC_VDISK_DRIVER_BLOCK + scsi_blocks);
        put16(h + 30, ata_blocks);
        put16(h + 32, MC_VDISK_DDM_ATA);
    }
    /* Blocks 1-63: the map. The SCSI driver's partition takes the driver
     * area up to the ATA driver (or all of it). */
    uint8_t *e = h + BLOCK;
    map_entry(e, entries, 1, MC_VDISK_DRIVER_BLOCK - 1, "Apple", "Apple_partition_map", 0x37);
    e += BLOCK;
    unsigned scsi_part = ata_blocks ? scsi_blocks : area;
    driver_entry(e, entries, MC_VDISK_DRIVER_BLOCK, scsi_part, "Livadix SCSI driver", "Apple_Driver43", drv->scsi, drv->scsi_size);
    e += BLOCK;
    if (ata_blocks) {
        /* The ATA boot code loads a driver only from a partition whose
         * name starts "Maci" (ROM 06684214, ROM offset $D2BF4). */
        driver_entry(e, entries, MC_VDISK_DRIVER_BLOCK + scsi_part, area - scsi_part, "Macintosh ATA driver", "Apple_Driver_ATA",
                     drv->ata, drv->ata_size);
        e += BLOCK;
    }
    for (unsigned p = 0; p < vd->nparts; p++, e += BLOCK) {
        const part *q = &vd->parts[p];
        if (q->image < 0) {
            map_entry(e, entries, q->start, q->blocks, "Extra", "Apple_Free", 0);
        } else {
            uint32_t status = mc_disk_readonly(vd->img[q->image]) ? 0x17 : 0x37;
            map_entry(e, entries, q->start, q->blocks, mc_disk_label(vd->img[q->image]), "Apple_HFS", status);
        }
    }
    /* Blocks 64-127: the drivers. */
    memcpy(h + MC_VDISK_DRIVER_BLOCK * BLOCK, drv->scsi, drv->scsi_size);
    if (ata_blocks) memcpy(h + (MC_VDISK_DRIVER_BLOCK + scsi_part) * BLOCK, drv->ata, drv->ata_size);
    memcpy(vd->overlay, vd->head, MC_VDISK_HEAD_BLOCKS * BLOCK);
    return 1;
}

static int has_ddm(mc_disk *d) {
    uint8_t b[2];
    return mc_disk_read_bytes(d, 0, 2, b) && b[0] == 0x45 && b[1] == 0x52;
}

mc_vdisk *mc_vdisk_create(mc_disk *const *images, unsigned count, const mc_vdisk_drivers *drivers, char *error,
                          size_t capacity) {
    char dummy[1];
    if (!error || !capacity) { error = dummy; capacity = sizeof dummy; }
    if (!images || count == 0 || count > MC_VDISK_MAX_IMAGES) {
        snprintf(error, capacity, "A virtual disk holds 1 to %d images", MC_VDISK_MAX_IMAGES);
        return NULL;
    }
    /* An audio CD holds no volume and gets no driver: the System's own CD
     * driver reads its table of contents. */
    mc_disk_track first;
    if (count == 1 && images[0] && mc_disk_tracks(images[0]) && mc_disk_track_get(images[0], 0, &first) && first.audio)
        return mc_vdisk_create_device(images[0], error, capacity);
    mc_vdisk *vd = calloc(1, sizeof *vd);
    if (!vd) { snprintf(error, capacity, "Out of memory"); return NULL; }
    vd->count = count;
    for (unsigned i = 0; i < count; i++) {
        vd->img[i] = images[i];
        if (!images[i] || image_blocks(images[i]) == 0) {
            snprintf(error, capacity, "Empty disk image");
            goto fail;
        }
        if (has_ddm(images[i])) {
            if (count > 1) {
                snprintf(error, capacity, "%s is already partitioned and needs its own SCSI ID", mc_disk_label(images[i]));
                goto fail;
            }
            vd->passthrough = 1;
        }
    }
    for (unsigned i = 0; i < count; i++) {
        uint8_t mdb[BLOCK];
        uint64_t at = 0;
        if (!mc_hfs_left_mounted(images[i], &at, mdb) || at % BLOCK || !(vd->checked[i] = malloc(BLOCK))) continue;
        memcpy(vd->checked[i], mdb, BLOCK);
        vd->checked_at[i] = at;
        fprintf(stderr, "%s: the volume was left mounted, and the ROM's volume check cannot walk its trees (their files "
                "are too large for its DIVU.W); Livadix gives the ROM the master directory block as that check leaves it\n",
                mc_disk_label(images[i]));
    }
    if (vd->passthrough) {
        vd->blocks = image_blocks(images[0]);
        vd->parts[0] = (part){0, vd->blocks, 0};
        vd->nparts = 1;
        return vd;
    }
    vd->head = calloc(MC_VDISK_HEAD_BLOCKS, BLOCK);
    vd->overlay = calloc(MC_VDISK_HEAD_BLOCKS, BLOCK);
    if (!vd->head || !vd->overlay) { snprintf(error, capacity, "Out of memory"); goto fail; }
    mc_vdisk_drivers none = {0};
    if (!synthesize(vd, drivers ? drivers : &none, error, capacity)) goto fail;
    return vd;
fail:
    mc_vdisk_destroy(vd);
    return NULL;
}

mc_vdisk *mc_vdisk_create_device(mc_disk *disk,char *error,size_t capacity){
 if(!disk||!image_blocks(disk)){if(error&&capacity)snprintf(error,capacity,"Empty device image");return NULL;}
 mc_vdisk *vd=calloc(1,sizeof *vd);if(!vd){if(error&&capacity)snprintf(error,capacity,"Out of memory");return NULL;}
 vd->passthrough=1;vd->count=1;vd->img[0]=disk;vd->blocks=image_blocks(disk);vd->parts[0]=(part){0,vd->blocks,0};vd->nparts=1;return vd;
}

void mc_vdisk_destroy(mc_vdisk *vd) {
    if (!vd) return;
    free(vd->head);
    free(vd->overlay);
    for (unsigned i = 0; i < MC_VDISK_MAX_IMAGES; i++) free(vd->checked[i]);
    free(vd);
}

uint64_t mc_vdisk_blocks(const mc_vdisk *vd) { return vd ? vd->blocks : 0; }
mc_disk *mc_vdisk_single(const mc_vdisk *vd) { return vd && vd->count == 1 ? vd->img[0] : NULL; }
int mc_vdisk_passthrough(const mc_vdisk *vd) { return vd && vd->passthrough; }
uint64_t mc_vdisk_overlay_writes(const mc_vdisk *vd) { return vd ? vd->overlay_writes : 0; }
uint64_t mc_vdisk_image_start(const mc_vdisk *vd, unsigned i) {
    for (unsigned p = 0; vd && p < vd->nparts; p++)
        if (vd->parts[p].image == (int)i) return vd->parts[p].start;
    return 0;
}

int mc_vdisk_all_readonly(const mc_vdisk *vd) {
    for (unsigned i = 0; i < vd->count; i++)
        if (!mc_disk_readonly(vd->img[i])) return 0;
    return 1;
}

/* Calls fn for each piece of [block, block + count): the head area, an
 * image's partition or a gap. */
typedef int (*piece_fn)(mc_vdisk *vd, int image, uint64_t block, uint32_t count, uint8_t *buffer, uint64_t image_block);

static int walk(mc_vdisk *vd, uint64_t block, uint32_t count, uint8_t *buffer, piece_fn fn) {
    if (!vd || block > vd->blocks || count > vd->blocks - block) return 0;
    while (count) {
        uint32_t n = count;
        int image = -2;             /* -2 head, -1 gap */
        uint64_t image_block = 0;
        if (!vd->passthrough && block < MC_VDISK_HEAD_BLOCKS) {
            if (n > MC_VDISK_HEAD_BLOCKS - block) n = (uint32_t)(MC_VDISK_HEAD_BLOCKS - block);
        } else {
            for (unsigned p = 0; p < vd->nparts; p++) {
                const part *q = &vd->parts[p];
                if (block >= q->start && block < q->start + q->blocks) {
                    if (n > q->start + q->blocks - block) n = (uint32_t)(q->start + q->blocks - block);
                    image = q->image;
                    image_block = block - q->start;
                    break;
                }
            }
        }
        if (!fn(vd, image, block, n, buffer, image_block)) return 0;
        block += n;
        count -= n;
        if (buffer) buffer += (size_t)n * BLOCK;
    }
    return 1;
}

static int read_piece(mc_vdisk *vd, int image, uint64_t block, uint32_t count, uint8_t *buffer, uint64_t image_block) {
    if (image == -2) {
        memcpy(buffer, vd->overlay + block * BLOCK, (size_t)count * BLOCK);
        return 1;
    }
    if (image == -1) {
        memset(buffer, 0, (size_t)count * BLOCK);
        return 1;
    }
    if (!mc_disk_read_bytes(vd->img[image], image_block * BLOCK, count * BLOCK, buffer)) return 0;
    const uint64_t at = vd->checked_at[image] / BLOCK;
    if (vd->checked[image] && at >= image_block && at < image_block + count)
        memcpy(buffer + (at - image_block) * BLOCK, vd->checked[image], BLOCK);
    return 1;
}

static int write_piece(mc_vdisk *vd, int image, uint64_t block, uint32_t count, uint8_t *buffer, uint64_t image_block) {
    if (image == -2) {
        memcpy(vd->overlay + block * BLOCK, buffer, (size_t)count * BLOCK);
        for (uint32_t i = 0; i < count; i++) vd->written[block + i] = 1;
        vd->overlay_writes += count;
        return 1;
    }
    if (image == -1) return 1;      /* free space outside every image: kept nowhere */
    const uint64_t at = vd->checked_at[image] / BLOCK;
    if (vd->checked[image] && at >= image_block && at < image_block + count) {
        free(vd->checked[image]);
        vd->checked[image] = NULL;
    }
    return mc_disk_write_bytes(vd->img[image], image_block * BLOCK, count * BLOCK, buffer);
}

static int readonly_piece(mc_vdisk *vd, int image, uint64_t block, uint32_t count, uint8_t *buffer, uint64_t image_block) {
    (void)block; (void)count; (void)buffer; (void)image_block;
    return image < 0 || !mc_disk_readonly(vd->img[image]);
}

int mc_vdisk_read(mc_vdisk *vd, uint64_t block, uint32_t count, void *buffer) { return walk(vd, block, count, buffer, read_piece); }
int mc_vdisk_write(mc_vdisk *vd, uint64_t block, uint32_t count, const void *buffer) {
    if (mc_vdisk_readonly(vd, block, count)) return 0;
    return walk(vd, block, count, (uint8_t *)buffer, write_piece);
}
int mc_vdisk_readonly(const mc_vdisk *vd, uint64_t block, uint32_t count) {
    return !walk((mc_vdisk *)vd, block, count, NULL, readonly_piece);
}
