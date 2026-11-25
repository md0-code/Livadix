/* The drives of a machine (media.h). Livadix's own code. */
#include "media.h"

#include <stdio.h>
#include <string.h>

void mc_media_init(mc_media *media, const mc_config *c) {
    memset(media, 0, sizeof *media);
    for (unsigned i = 0; i < c->floppy_count && i < MC_CONFIG_FLOPPIES; ++i)
        if (!c->floppy_off[i]) ++media->floppy_drives;
    media->last_kind = -1;
}

static void close_drive(mc_media_drive *d) { mc_disk_close(d->disk); d->disk = NULL; d->ejected = 0; }

void mc_media_close(mc_media *media) {
    for (unsigned i = 0; i < MC_CONFIG_DISKS; ++i) close_drive(&media->disks[i]);
    for (unsigned i = 0; i < MC_CONFIG_FLOPPIES; ++i) close_drive(&media->floppies[i]);
    close_drive(&media->cdrom);
}

static mc_media_drive *slot(const mc_media *media, mc_media_kind kind, unsigned index) {
    mc_media *w = (mc_media *)media;
    if (kind == MC_MEDIA_DISK) return index < MC_CONFIG_DISKS ? &w->disks[index] : NULL;
    if (kind == MC_MEDIA_FLOPPY) return index < media->floppy_drives ? &w->floppies[index] : NULL;
    return kind == MC_MEDIA_CDROM && index == 0 ? &w->cdrom : NULL;
}

unsigned mc_media_drives(const mc_media *media, mc_media_kind kind) {
    return kind == MC_MEDIA_DISK ? MC_CONFIG_DISKS : kind == MC_MEDIA_FLOPPY ? media->floppy_drives : 1;
}

int mc_media_insert(mc_media *media, mc_media_kind kind, unsigned index, mc_disk *disk, int started, char *error, size_t capacity) {
    mc_media_drive *d = slot(media, kind, index);
    const char *problem = NULL;
    if (!d || !disk) problem = "No such drive";
    else if (d->disk && !d->ejected) problem = "The drive is not empty";
    else if (kind == MC_MEDIA_DISK && started) problem = "Hard disks are attached before the Macintosh starts";
    else if (kind == MC_MEDIA_FLOPPY && !mc_disk_floppy(disk)) problem = "A floppy image is 400, 720, 800 or 1440 KiB, raw or DiskCopy 4.2";
    else if (kind == MC_MEDIA_CDROM && !mc_disk_readonly(disk)) problem = "A CD image is read-only";
    if (problem) { if (capacity) snprintf(error, capacity, "%s", problem); return 0; }
    if (d->disk != disk) mc_disk_close(d->disk);
    d->disk = disk; d->ejected = 0;
    if (media->last_kind == (int)kind && media->last_drive == (int)index) media->last_kind = -1;
    return 1;
}

mc_disk *mc_media_get(const mc_media *media, mc_media_kind kind, unsigned index, int *ejected) {
    mc_media_drive *d = slot(media, kind, index);
    if (ejected) *ejected = d ? d->ejected : 0;
    return d ? d->disk : NULL;
}

int mc_media_eject(mc_media *media, mc_media_kind kind, unsigned index) {
    mc_media_drive *d = slot(media, kind, index);
    if (!d || !d->disk || d->ejected || kind == MC_MEDIA_DISK) return 0;
    d->ejected = 1; media->last_kind = (int)kind; media->last_drive = (int)index;
    return 1;
}

int mc_media_reinsert(mc_media *media) {
    if (media->last_kind < 0) return 0;
    mc_media_drive *d = slot(media, (mc_media_kind)media->last_kind, (unsigned)media->last_drive);
    media->last_kind = -1;
    if (!d || !d->disk || !d->ejected) return 0;
    d->ejected = 0;
    return 1;
}

void mc_media_forget(mc_media *media, mc_media_kind kind, unsigned index) {
    mc_media_drive *d = slot(media, kind, index);
    if (d) { d->disk = NULL; d->ejected = 0; }
}
