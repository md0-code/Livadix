/* Livadix — the virtual disk.
 *
 * What a SCSI (later ATA) target presents for one or more disk images, in
 * 512-byte blocks:
 *
 *   block 0        Driver Descriptor Map: 'ER', block size 512, block count,
 *                  one descriptor per Livadix driver (type 1 for the SCSI
 *                  driver; $0701 for the ATA driver, the type the Quadra
 *                  630 ROM's ATA boot code loads)
 *   blocks 1-63    the partition map: Apple_partition_map, Apple_Driver43
 *                  (and Apple_Driver_ATA when an ATA driver is given), one
 *                  Apple_HFS per image
 *   blocks 64-127  the Livadix drivers
 *   block 128...   the images, block for block, one after the other
 *
 * The ATAPI layout (mc_vdisk_drivers.atapi) counts in 2048-byte blocks:
 * block 0 the map with block size 2048 and the ATA flavour's descriptor
 * (type $0701), blocks 1-15 the partition map, one entry at the start of
 * each block (Apple_partition_map, Apple_Driver_ATAPI named "Macintosh...",
 * Apple_HFS), blocks 16-31 the driver, block 32 on the image: the same
 * bytes as 512-byte blocks 0-127 and 128 on.
 *
 * Structures from Inside Macintosh: Devices, chapter 3 "SCSI Manager", "The
 * Structure of Block Devices" and "Data Structures" (Block0 and Partition
 * records, pMapSIG, partition types, pmPartStatus bits).
 *
 * Blocks 0-127 are synthesized in memory; writes to them go to an overlay
 * kept for the session (counted, never written to an image). Writes to an
 * image's blocks go to the image through the disk layer (src/core/disk.c).
 * An image whose block 0 is already a Driver Descriptor Map is presented as
 * it is (pass-through), with nothing synthesized.
 *
 * The vdisk does not own the mc_disk images; close them after destroying
 * the vdisk. */
#ifndef MC_VDISK_H
#define MC_VDISK_H

#include "disk.h"

#include <stddef.h>
#include <stdint.h>

#define MC_VDISK_MAX_IMAGES 10
#define MC_VDISK_HEAD_BLOCKS 128u      /* DDM, map and drivers */
#define MC_VDISK_DRIVER_BLOCK 64u
#define MC_VDISK_DDM_SCSI 1u          /* ddType of the SCSI driver: Mac OS (IM: OS Utilities, GetOSDefault) */
#define MC_VDISK_DDM_ATA 0x0701u      /* $0700 + 1: what ROM 06684214's ATALOAD (ROM offset $D2898) looks for */

typedef struct mc_vdisk mc_vdisk;

typedef struct mc_vdisk_drivers {
    const uint8_t *scsi; size_t scsi_size;  /* the Livadix SCSI driver image (livadixdisk) */
    const uint8_t *ata; size_t ata_size;    /* the ATA flavour (livadixdisk_ata) */
    /* A CD on an ATAPI drive for a ROM whose ATAPI boot code reads the
     * disc in 2048-byte blocks (the Power Macintosh G3's, ROM offset
     * $288800): one image, a whole number of 2048-byte blocks, presented with a 2048-byte
     * map (see above), the ATA flavour in an Apple_Driver_ATAPI partition. */
    int atapi;
} mc_vdisk_drivers;

/* A virtual disk over `count` images (1..MC_VDISK_MAX_IMAGES, 512- or
 * 2048-byte sectors). A single image that already carries a Driver
 * Descriptor Map is passed through; such an image cannot be combined with
 * others. NULL with a message on error. */
mc_vdisk *mc_vdisk_create(mc_disk *const *images, unsigned count, const mc_vdisk_drivers *drivers,
                          char *error, size_t capacity);
/* A complete device, including uninitialized block0: guest map/driver
 * writes persist in its image, without a synthetic partition overlay. */
mc_vdisk *mc_vdisk_create_device(mc_disk *,char *,size_t);
void mc_vdisk_destroy(mc_vdisk *vd);
/* The one image of a virtual disk made of one (a CD's disc), else NULL. */
mc_disk *mc_vdisk_single(const mc_vdisk *vd);

/* Size in 512-byte blocks. */
uint64_t mc_vdisk_blocks(const mc_vdisk *vd);
int mc_vdisk_passthrough(const mc_vdisk *vd);
/* Whether a write of these blocks would be refused (a read-only image). */
int mc_vdisk_readonly(const mc_vdisk *vd, uint64_t block, uint32_t count);
/* Whether every image is read-only. */
int mc_vdisk_all_readonly(const mc_vdisk *vd);
/* 1 on success. */
int mc_vdisk_read(mc_vdisk *vd, uint64_t block, uint32_t count, void *buffer);
int mc_vdisk_write(mc_vdisk *vd, uint64_t block, uint32_t count, const void *buffer);
/* Blocks written into the session overlay (0-127) so far. */
uint64_t mc_vdisk_overlay_writes(const mc_vdisk *vd);
/* First block of image i's Apple_HFS partition (synthesized disks). */
uint64_t mc_vdisk_image_start(const mc_vdisk *vd, unsigned i);

/* The boot-code checksum of a partition entry (pmBootCksum): each byte added
 * to a 16-bit sum which is then rotated left one bit; 0 becomes $FFFF, as
 * ROM 06684214's ATA boot code computes it (ROM offset $D2BD0; Inside
 * Macintosh: Devices names the field only). The SCSI driver's partition is
 * named so the Start Manager does not verify it (names beginning "Maci" are
 * verified, IM: Devices 3-14); the ATA driver's begins "Maci", the only name
 * the 630 ROM's ATA boot code loads a driver from. */
uint32_t mc_vdisk_boot_checksum(const uint8_t *code, size_t size);

#endif
