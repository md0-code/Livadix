/* What a disk image holds (docs/NOTES.md, "Compatibility"): its
 * format, its HFS volume, and on a startup disk the blessed System Folder's
 * System and Finder versions and System Enablers. Read-only; the image is
 * opened with mc_disk_open and never written. Livadix's own reader of the HFS
 * structures Inside Macintosh: Files describes. */

#ifndef MC_HFS_H
#define MC_HFS_H

#include <stddef.h>
#include <stdint.h>

enum {
    MC_IMAGE_UNKNOWN,          /* no volume Livadix recognises */
    MC_IMAGE_HFS,              /* a bare HFS volume */
    MC_IMAGE_PARTITIONED,      /* an Apple partition map holding an HFS volume */
    MC_IMAGE_HFS_PLUS,         /* Mac OS Extended (not read further) */
    MC_IMAGE_MFS,              /* the 400K floppies' flat file system (not read further) */
    MC_IMAGE_ISO               /* ISO 9660 without an HFS volume */
};

#define MC_VOLUME_ENABLERS 16
#define MC_VOLUME_BOXES 256

/* A System Enabler: a file of type 'gbly' in the System Folder, and the box
 * flags (Gestalt machine type - 6), or later Gestalt IDs, its 'gbly' resource lists. */
typedef struct mc_enabler {
    char name[64];             /* UTF-8 */
    uint16_t boxes[64];
    size_t box_count;
} mc_enabler;

typedef struct mc_volume_info {
    int kind;                  /* MC_IMAGE_* */
    int diskcopy;              /* a DiskCopy 4.2 image */
    uint64_t bytes;            /* the image's data */
    char volume[64];           /* volume name, UTF-8 */
    int bootable;              /* a blessed System Folder holding a System file */
    int mac_os_x;              /* HFS Plus finderInfo[5] blesses a Mac OS X system */
    char system_name[64], finder_name[64];
    uint16_t system_version;   /* 'vers' 1 as BCD: 0x0711 for 7.1.1; 0 when unknown */
    char system_text[32];      /* its short version string ("7.1.1", "6.0.8L") */
    int system_unversioned;    /* the System file was read and has no 'vers' resource: a System before 6.0 */
    uint16_t finder_version;
    char finder_text[32];
    int system_gbly;           /* the System file has a 'gbly' resource */
    uint16_t system_boxes[MC_VOLUME_BOXES];
    size_t system_box_count;
    int16_t system_ptch[32];   /* the System file's 'PTCH' resource IDs: the ROM versions it patches */
    size_t system_ptch_count;
    mc_enabler enablers[MC_VOLUME_ENABLERS];
    size_t enabler_count;
    int mode32;                /* MODE32 or the 32-Bit System Enabler in the System Folder */
} mc_volume_info;

/* Fills info from the image at path. 0 with error when the file cannot be
 * opened as an image; an image without a volume Livadix recognises is not an
 * error (kind MC_IMAGE_UNKNOWN). */
int mc_volume_inspect(const char *path, mc_volume_info *info, char *error, size_t capacity);
/* "7.1.1" for 0x0711, "8.0" for 0x0800. */
const char *mc_version_text(uint16_t bcd, char *buffer, size_t capacity);
const char *mc_image_kind_name(int kind);
/* In words: the format, the volume's name, the System and Finder versions
 * and the enablers ("HFS volume "HD", System 7.1.1, Finder 7.1.3, enablers
 * 040, 088"). */
const char *mc_volume_describe(const mc_volume_info *info, char *buffer, size_t capacity);

/* A volume left mounted (not shut down: drAtrb bit 8 clear) is checked by
 * the ROM's MountVol before it mounts: a walk of the
 * catalog that recounts the files and folders and the next catalog node ID,
 * then a walk of the extents tree. Each walk stops with badMDBErr (-60)
 * after a number of records the ROM computes as the file's size / record
 * size in a DIVU.W, which overflows for a file of 786,432 bytes or more and
 * leaves the size itself in the register: a 1 MiB extents file allows one
 * record. When that alone would make the ROM reject the volume (and both
 * trees read well), mc_hfs_left_mounted gives the byte offset of the
 * volume's master directory block in the image (bare or in an Apple
 * partition map) and the block as the ROM's check would have left it: its
 * counts recomputed as the check computes them, marked unmounted cleanly
 * so the ROM mounts it without the check. 0 otherwise. Reads only. */
struct mc_disk;
int mc_hfs_left_mounted(struct mc_disk *disk, uint64_t *offset, uint8_t mdb[512]);

#endif /* MC_HFS_H */
