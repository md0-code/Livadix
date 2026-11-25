/* Whether a model runs the System on a disk (docs/NOTES.md,
 * "Compatibility"): the oldest and newest System each model runs, the
 * System Enabler required by a release on models newer than the System,
 * and the 24-bit limits. A check warns and never blocks. */

#ifndef MC_COMPAT_H
#define MC_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#include "hfs.h"

struct mc_model;
struct mc_config;

/* flags */
enum {
    MC_COMPAT_608L = 1,        /* System 6.0.8L runs it, though 6.0.8 does not */
    MC_COMPAT_24BIT = 2,       /* the ROM addresses 24 bits: 32-bit addressing needs MODE32 or the 32-Bit System Enabler */
    MC_COMPAT_GESTALT_LIST = 8, /* later enablers list Gestalt machine IDs rather than legacy box flags */
    MC_COMPAT_NO_76 = 4        /* Mac OS 7.6 does not run it, though the Systems before and 7.6.1 do */
};

typedef struct mc_compat_entry {
    const char *model;         /* models.c key */
    uint16_t min_system;       /* BCD: the oldest System it runs */
    uint16_t max_system;       /* BCD: the newest */
    const char *enabler;       /* required release-specific enabler, or NULL */
    uint16_t native;           /* BCD: from this System on no enabler is needed */
    uint8_t flags;
} mc_compat_entry;

enum { MC_COMPAT_OK, MC_COMPAT_WARNING, MC_COMPAT_UNKNOWN };

typedef struct mc_compat {
    int status;                /* MC_COMPAT_* */
    char reason[320];          /* why (a warning), or what runs (OK) */
} mc_compat;

const mc_compat_entry *mc_compat_find(const char *model);
/* The Systems the model runs, in words ("Runs System 7.1 to Mac OS 7.6.1;
 * before 7.5 with System Enabler 403"), for the configuration window. */
void mc_compat_describe(const struct mc_model *model, char *out, size_t capacity);
size_t mc_compat_count(void);
const mc_compat_entry *mc_compat_entry_at(size_t index);

/* The model with the System on volume; ram_kb the configured memory in KiB
 * (mc_config_ram_kb; 0 to leave memory out). */
int mc_compat_check(const struct mc_model *model, const mc_volume_info *volume, uint32_t ram_kb, mc_compat *out);
/* The memory the System on volume needs (MiB; 0 when unknown). */
uint32_t mc_compat_min_ram(const mc_volume_info *volume);
/* Only the memory: MC_COMPAT_OK, or a warning with the reason when ram_kb
 * (KiB) is below what the System needs. */
int mc_compat_ram(const mc_volume_info *volume, uint32_t ram_kb, mc_compat *out);
/* A warning when a model that takes at most most_kb cannot have the memory
 * the volume's System needs. */
int mc_compat_ram_most(const mc_volume_info *volume, uint32_t most_kb, const char *model, mc_compat *out);

/* The image the configuration starts from: with boot_priority cdrom the CD,
 * else a floppy in a drive, else the first hard disk holding a blessed
 * System Folder in SCSI ID order 0, then 6 down to 1 (Livadix's approximation
 * of the ROM's search); NULL when none. The disks' SCSI IDs as
 * docs/NOTES.md, "SCSI IDs" assigns them. info receives what the
 * image holds. */
const char *mc_config_boot_image(const struct mc_config *config, mc_volume_info *info);
/* When mc_config_boot_image finds none: the first image, in the same
 * order, that is a Mac OS Extended volume, whose System Folder Livadix does not
 * read (so that "no System Folder" is not reported for it); NULL when
 * there is none. */
const char *mc_config_unexamined_image(const struct mc_config *config, mc_volume_info *info);

#endif /* MC_COMPAT_H */
