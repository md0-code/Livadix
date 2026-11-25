/* The drives of a machine and the images in them (machine.h, "Media"):
 * hard disk slots, the fitted floppy drives and the CD-ROM drive, with the
 * eject/reinsert bookkeeping every machine shares. Livadix's own code. */
#ifndef MC_MEDIA_H
#define MC_MEDIA_H

#include "config.h"
#include "disk.h"
#include "machine.h"

typedef struct mc_media_drive { mc_disk *disk; int ejected; } mc_media_drive;

typedef struct mc_media {
    mc_media_drive disks[MC_CONFIG_DISKS], floppies[MC_CONFIG_FLOPPIES], cdrom;
    unsigned floppy_drives;
    int last_kind, last_drive;
} mc_media;

void mc_media_init(mc_media *media, const mc_config *config);
void mc_media_close(mc_media *media);
unsigned mc_media_drives(const mc_media *media, mc_media_kind kind);
/* `started`: the machine has run (hard disks go in before). */
int mc_media_insert(mc_media *media, mc_media_kind kind, unsigned drive, mc_disk *disk, int started, char *error, size_t capacity);
mc_disk *mc_media_get(const mc_media *media, mc_media_kind kind, unsigned drive, int *ejected);
int mc_media_eject(mc_media *media, mc_media_kind kind, unsigned drive);
int mc_media_reinsert(mc_media *media);
/* Empties a drive without closing its image (an insert the machine refused). */
void mc_media_forget(mc_media *media, mc_media_kind kind, unsigned drive);

#endif
