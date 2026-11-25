/* Livadix — where the AFP server keeps what Windows files lack: the Finder
 * information, ProDOS information, AFP attributes, backup date and the
 * resource fork.
 *
 * streams: NTFS named streams with the names Services for Macintosh used,
 *   "AFP_AfpInfo" (60 bytes, little-endian numbers: signature "AFP\0",
 *   version $00010000, the AFP ID, the backup date, the 32 bytes of Finder
 *   information, 6 of ProDOS information, the attributes word and the
 *   three access bytes) and "AFP_Resource" (the resource fork as it is).
 * appledouble: a "._name" file beside each file or folder in the
 *   AppleDouble format (RFC 1740, appendix B: magic $00051607, version
 *   $00020000) with Finder Info (9), File Dates Info (8), Macintosh File
 *   Info (10), ProDOS File Info (11) and the resource fork (2) last, so it
 *   can grow. AppleDouble files written by others are read by their entry
 *   table and rewritten in this layout when they change.
 *
 * Livadix's own code. */
#ifndef MC_AFP_META_H
#define MC_AFP_META_H

#include "afp_host.h"

enum { MC_META_STREAMS, MC_META_APPLEDOUBLE };

typedef struct mc_afp_meta {
    uint8_t finder[32];
    uint8_t prodos[6];
    uint16_t attributes;       /* AFP file or directory attributes kept here */
    int32_t backup;            /* AFP date ($80000000: never) */
    int32_t created, modified; /* AppleDouble's dates (written only) */
    uint32_t id;               /* the AFP ID (streams) */
} mc_afp_meta;

/* The metadata of a file or folder; defaults (zeros, backup $80000000)
 * when it has none. Returns 1 when some was stored. */
int mc_meta_read(int mode, const uint16_t *path, mc_afp_meta *meta);
int mc_meta_write(int mode, const uint16_t *path, const mc_afp_meta *meta);
/* The "._name" beside path (AppleDouble). 0 when too long. */
int mc_meta_sidecar(const uint16_t *path, uint16_t *out, size_t capacity);
/* 1 when name is an AppleDouble file's ("._" prefix). */
int mc_meta_is_sidecar(const uint16_t *name);

typedef struct mc_rsrc mc_rsrc;
int mc_rsrc_open(int mode, const uint16_t *path, int write, mc_rsrc **out);
int mc_rsrc_read(mc_rsrc *r, uint64_t offset, void *buffer, size_t length, size_t *got);
int mc_rsrc_write(mc_rsrc *r, uint64_t offset, const void *buffer, size_t length);
int mc_rsrc_size(mc_rsrc *r, uint64_t *size);
int mc_rsrc_truncate(mc_rsrc *r, uint64_t size);
int mc_rsrc_flush(mc_rsrc *r);
void mc_rsrc_close(mc_rsrc *r);
/* The resource fork's length without opening it for long. */
int mc_rsrc_length(int mode, const uint16_t *path, uint64_t *size);

#endif
