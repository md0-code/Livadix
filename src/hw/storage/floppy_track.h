/* Floppy track synthesis: the bytes a Macintosh
 * floppy drive delivers for one side of one track, built from an image's
 * sectors, and decoded back into sectors after the guest wrote or formatted
 * the track.
 *
 *   - Apple GCR 6-and-2 for 400 KiB (one side) and 800 KiB (two sides): five
 *     speed zones of 12 to 8 sectors, address and data fields with their
 *     marks, checksums and the 12 tag bytes of each sector.
 *   - MFM (IBM System/34 layout) for 720 KiB (9 sectors) and 1440 KiB (18
 *     sectors): index mark, ID and data address marks, CRC-16-CCITT.
 *
 * A track is a ring of whole bytes as the disk controller assembles them;
 * byte 0 sits at the index. For MFM, `mark` flags the bytes written with a
 * missing clock (the $A1 and $C2 mark bytes). 1680 KiB is not a Macintosh
 * format and is refused (PLAN.md decision 1). */
#ifndef MC_FLOPPY_TRACK_H
#define MC_FLOPPY_TRACK_H

#include <stddef.h>
#include <stdint.h>

typedef enum mc_floppy_format {
    MC_FLOPPY_NONE,
    MC_FLOPPY_GCR400,       /* 800 sectors, one side */
    MC_FLOPPY_GCR800,       /* 1600 sectors, two sides */
    MC_FLOPPY_MFM720,       /* 1440 sectors, 9 per track side, 250 kbit/s */
    MC_FLOPPY_MFM1440       /* 2880 sectors, 18 per track side, 500 kbit/s */
} mc_floppy_format;

#define MC_FLOPPY_TRACKS 80
/* The longest track: 1440 KiB MFM, 62,500 bytes/s at 300 rpm. */
#define MC_FLOPPY_MAX_TRACK 12500u
/* GCR data field: 12 tag + 512 data bytes in 699 nibbles, 3 checksum bytes
 * in 4 more. */
#define MC_GCR_DATA_NIBBLES 703

typedef struct mc_floppy_track {
    uint32_t length;                        /* bytes in one revolution */
    uint8_t data[MC_FLOPPY_MAX_TRACK];
    uint8_t mark[MC_FLOPPY_MAX_TRACK];      /* MFM: byte has a missing clock */
} mc_floppy_track;

/* The format of an image of `sectors` 512-byte sectors (NONE when unknown). */
mc_floppy_format mc_floppy_format_of(uint64_t sectors);
int mc_floppy_is_gcr(mc_floppy_format f);
unsigned mc_floppy_sides(mc_floppy_format f);
/* Sectors on one side of `track`. */
unsigned mc_floppy_sectors(mc_floppy_format f, unsigned track);
/* Rotational speed in rpm and data rate in bytes per second. In GCR the
 * speed depends on the track's zone; MFM spins at 300 rpm. */
unsigned mc_floppy_gcr_rpm(unsigned track);
unsigned mc_floppy_rpm(int gcr, unsigned track);
uint32_t mc_floppy_bytes_per_second(int gcr, int high_density);
/* Bytes in one revolution at that speed and rate. */
uint32_t mc_floppy_track_length(int gcr, int high_density, unsigned track);
/* First image sector of a track side (-1 if out of range). */
int64_t mc_floppy_first_sector(mc_floppy_format f, unsigned track, unsigned side);

/* Builds the track from its sectors' data (sectors x 512 bytes, in order)
 * and tags (sectors x 12 bytes, or NULL for zeros; GCR only). Returns 0 for
 * a track or side the format does not have. */
int mc_floppy_build_track(mc_floppy_format f, unsigned track, unsigned side,
                          const uint8_t *data, const uint8_t *tags, mc_floppy_track *out);

/* Decodes a track: calls `sector` for every sector whose address field
 * names this track and side and whose data field checks out (GCR checksum or
 * MFM CRC). `index` is the 0-based sector within the track side; `tags` is
 * NULL for MFM. Returns the number of sectors delivered. */
typedef void (*mc_floppy_sector_fn)(void *ctx, unsigned index, const uint8_t *data, const uint8_t *tags);
unsigned mc_floppy_decode_track(mc_floppy_format f, unsigned track, unsigned side,
                                const mc_floppy_track *t, mc_floppy_sector_fn sector, void *ctx);

/* GCR primitives. */
extern const uint8_t mc_gcr_encode6[64];        /* 6-bit value -> disk byte */
int mc_gcr_decode6(uint8_t disk_byte);          /* disk byte -> 6-bit value, -1 if not a data nibble */
/* Encodes 524 bytes (12 tag + 512 data) into 703 nibbles; decodes them back,
 * returning 1 when the checksum matches. */
void mc_gcr_encode_data(const uint8_t in[524], uint8_t out[MC_GCR_DATA_NIBBLES]);
int mc_gcr_decode_data(const uint8_t in[MC_GCR_DATA_NIBBLES], uint8_t out[524]);
/* The five address-field values (6-bit each, before nibblizing): track,
 * sector, side, format and their checksum. */
void mc_gcr_address(unsigned track, unsigned side, unsigned sector, unsigned format, uint8_t out[5]);

/* CRC-16-CCITT as the ISM computes it: x^16 + x^12 + x^5 + 1, MSB first. */
uint16_t mc_mfm_crc(uint16_t crc, const uint8_t *data, size_t size);

#endif
