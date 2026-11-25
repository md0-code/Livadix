/* Livadix — the SCSI CD-ROM drive (scsi_target.h): the disk's command set with
 * 512-byte blocks (2048 after a MODE SELECT), read-only media, eject, and
 * READ TOC (SCSI-2 14.2.11), and the audio commands of 14.2: the disc's
 * tracks are its image's (a cue sheet's, src/core/disk.h), its audio is
 * played by the disc's player there, which the machine mixes into its sound.
 *
 * 512 at power-on, after a reset and with a new disc, unless the disc's
 * driver descriptor record names 2048 (an Apple CD, whose partition map
 * and drivers lie in 2048-byte blocks): the Quadra 700 ROM boots and loads
 * the drivers of a CD reading blocks of the length the record names, with
 * no MODE SELECT first (a Livadix disc's READ(6) of block 0, the map at block
 * 1 and the driver at block 64; an Apple CD's driver, 5 blocks of 2048 at
 * block 16, traced). */
#include "scsi_target.h"

#include <string.h>

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* A track address, as a logical block (of the current length) or as MSF
 * (14.1.4: 75 frames of 2048 bytes a second, logical block 0 at 00:02:00). */
static void address(const mc_scsi_unit *u, uint8_t *p, uint32_t sector2048, int msf) {
    if (msf) {
        uint32_t f = sector2048 + 150;
        p[0] = 0;
        p[1] = (uint8_t)(f / (60 * 75));
        p[2] = (uint8_t)(f / 75 % 60);
        p[3] = (uint8_t)(f % 75);
    } else {
        put32(p, sector2048 * (2048 / u->block));
    }
}

static int illegal(mc_scsi_unit *u, uint8_t asc);
static mc_disk *disc(const mc_scsi_unit *u) { return u->media ? mc_vdisk_single(u->media) : NULL; }

/* The tracks the TOC lists: the image's, or for a disc without a cue sheet
 * one data track as long as the virtual disk. */
static unsigned toc_tracks(const mc_scsi_unit *u, mc_disk_track *t, unsigned max) {
    mc_disk *d = disc(u);
    unsigned n = d && mc_disk_tracks(d) > 1 ? mc_disk_tracks(d) : 0;
    if (n > max) n = max;
    for (unsigned i = 0; i < n; ++i) mc_disk_track_get(d, i, &t[i]);
    if (n) return n;
    t[0] = (mc_disk_track){1, 0, 0, (uint32_t)(mc_vdisk_blocks(u->media) / 4)};
    return 1;
}
static uint32_t toc_leadout(const mc_disk_track *t, unsigned n) { return t[n - 1].start + t[n - 1].length; }

/* READ TOC (Table 260): MSF in byte 1 bit 1, starting track in byte 6,
 * allocation length in bytes 7-8. Response (Table 261/262): data length,
 * first and last track, then 8-byte descriptors (reserved, ADR/control,
 * track number, reserved, address). A data track's control is 4, an audio
 * track's 0 (two channels, no pre-emphasis, copy prohibited), ADR 1; the
 * lead-out is track AAh. */
static int read_toc(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    if (!u->media) {
        mc_scsi_unit_sense(u, MC_SENSE_NOT_READY, 0x3A, 0);
        return MC_SCSI_CHECK_CONDITION;
    }
    mc_disk_track t[99];
    const unsigned n = toc_tracks(u, t, 40);
    int msf = (cdb[1] & 0x02) != 0;
    unsigned start = cdb[6];
    uint32_t allocation = (uint32_t)cdb[7] << 8 | cdb[8];
    /* The format, in byte 9 bits 7-6 (SFF-8020i 10.8.25, which Apple's CD-ROM
     * driver uses) or byte 2 bits 3-0 (MMC): 1 the session information, 2
     * the full TOC (the Q sub-channel's entries of the lead-in). One
     * session. */
    const unsigned format = cdb[9] >> 6 ? cdb[9] >> 6 : cdb[2] & 0x0F;
    if (format == 1 || format == 2) {
        uint8_t *d = u->data;
        memset(d, 0, sizeof u->data);
        unsigned at = 4;
        d[2] = d[3] = 1;
        if (format == 1) {
            d[at + 1] = t[0].audio ? 0x10 : 0x14;
            d[at + 2] = (uint8_t)t[0].number;
            address(u, d + at + 4, t[0].start, msf);
            at += 8;
        } else {
            if (start > 1) return illegal(u, 0x24);
            /* A0: the first track (PMIN) and the disc type (PSEC, 00h
             * CD-DA or CD-ROM); A1: the last track; A2: the lead-out; then
             * one entry per track (11 bytes each: session, ADR/control,
             * TNO, POINT, MIN, SEC, FRAME, zero, PMIN, PSEC, PFRAME). */
            const uint32_t points[3] = {0xA0, 0xA1, 0xA2};
            for (unsigned k = 0; k < 3 + n; ++k) {
                uint8_t *e = d + at;
                e[0] = 1;
                if (k < 3) {
                    e[1] = (k == 0 ? t[0].audio : t[n - 1].audio) ? 0x10 : 0x14;
                    e[3] = (uint8_t)points[k];
                    if (k == 0) e[8] = (uint8_t)t[0].number;
                    else if (k == 1) e[8] = (uint8_t)t[n - 1].number;
                    else address(u, e + 7, toc_leadout(t, n), 1);
                } else {
                    e[1] = t[k - 3].audio ? 0x10 : 0x14;
                    e[3] = (uint8_t)t[k - 3].number;
                    address(u, e + 7, t[k - 3].start, 1);
                }
                e[7] = 0;
                at += 11;
            }
        }
        put16(d, at - 2);
        u->mode = MC_UNIT_RESPONSE;
        *direction = MC_SCSI_XFER_IN;
        *bytes = at < allocation ? at : allocation;
        return MC_SCSI_GOOD;
    }
    if (start > t[n - 1].number && start != 0xAA) {
        mc_scsi_unit_sense(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0);
        return MC_SCSI_CHECK_CONDITION;
    }
    uint8_t *d = u->data;
    unsigned at = 4;
    memset(d, 0, sizeof u->data);
    d[2] = (uint8_t)t[0].number;
    d[3] = (uint8_t)t[n - 1].number;
    for (unsigned i = 0; i < n && start != 0xAA; ++i) {
        if (t[i].number < start) continue;
        d[at + 1] = t[i].audio ? 0x10 : 0x14;
        d[at + 2] = (uint8_t)t[i].number;
        address(u, d + at + 4, t[i].start, msf);
        at += 8;
    }
    d[at + 1] = t[n - 1].audio ? 0x10 : 0x14;
    d[at + 2] = 0xAA;
    address(u, d + at + 4, toc_leadout(t, n), msf);
    at += 8;
    put16(d, at - 2);
    u->mode = MC_UNIT_RESPONSE;
    *direction = MC_SCSI_XFER_IN;
    *bytes = at < allocation ? at : allocation;
    return MC_SCSI_GOOD;
}

static int illegal(mc_scsi_unit *u, uint8_t asc) {
    mc_scsi_unit_sense(u, MC_SENSE_ILLEGAL_REQUEST, asc, 0);
    return MC_SCSI_CHECK_CONDITION;
}

/* READ SUB-CHANNEL (14.2.10, Tables 249-251): MSF in byte 1 bit 1, SubQ in
 * byte 2 bit 6, the data format in byte 3 (01h CD-ROM current position;
 * 02h the media catalogue number and 03h the ISRC, neither on the disc),
 * allocation length in bytes 7-8. The header's audio status: 11h playing,
 * 12h paused, 13h completed (reported once), 15h no current status. */
static int read_subchannel(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    if (!u->media) {
        mc_scsi_unit_sense(u, MC_SENSE_NOT_READY, 0x3A, 0);
        return MC_SCSI_CHECK_CONDITION;
    }
    mc_disk *dk = disc(u);
    const int msf = (cdb[1] & 0x02) != 0, subq = (cdb[2] & 0x40) != 0;
    const unsigned format = cdb[3];
    const uint32_t allocation = (uint32_t)cdb[7] << 8 | cdb[8];
    if (subq && (format < 1 || format > 3)) return illegal(u, 0x24);
    uint32_t lba = 0;
    const int state = mc_disk_play_state(dk, &lba);
    uint8_t *d = u->data;
    memset(d, 0, 48);
    switch (state) {
    case MC_CD_PLAYING: d[1] = 0x11; u->audio_reported = 0; break;
    case MC_CD_PAUSED: d[1] = 0x12; break;
    case MC_CD_COMPLETED: d[1] = u->audio_reported ? 0x15 : 0x13; u->audio_reported = 1; break;
    default: d[1] = 0x15; break;
    }
    unsigned at = 4;
    if (subq) {
        d[at] = (uint8_t)format;
        if (format == 1) {
            mc_disk_track tr = {1, 0, 0, 0};
            if (dk) mc_disk_track_at(dk, lba, &tr);
            d[at + 1] = tr.audio ? 0x10 : 0x14;
            d[at + 2] = (uint8_t)tr.number;
            d[at + 3] = lba >= tr.start ? 1 : 0;    /* index 1, 0 in the pregap */
            address(u, d + at + 4, lba, msf);
            const uint32_t rel = lba >= tr.start ? lba - tr.start : 0;
            if (msf) {
                d[at + 9] = (uint8_t)(rel / (60 * 75));
                d[at + 10] = (uint8_t)(rel / 75 % 60);
                d[at + 11] = (uint8_t)(rel % 75);
            } else {
                put32(d + at + 8, rel * (2048 / u->block));
            }
            at += 12;
        } else {
            d[at + 4] = 0;      /* MCval / TCval 0: none */
            at += 20;
        }
    }
    put16(d + 2, at - 4);
    u->mode = MC_UNIT_RESPONSE;
    *direction = MC_SCSI_XFER_IN;
    *bytes = at < allocation ? at : allocation;
    return MC_SCSI_GOOD;
}

/* Starts play of frames [start, end). A length of 0 plays nothing (14.2.2,
 * 14.2.4) but moves the head there, a play in progress or paused waiting
 * there paused: Apple's CD-ROM driver goes to a track (AppleCD Audio
 * Player's next and previous track) with PAUSE, then a PLAY AUDIO MSF whose
 * start and end are equal, then, unless the player was paused, a play from
 * the current location (a start of FF:FF:FF). A start in a data track:
 * illegal mode for this track (64h). */
static int play(mc_scsi_unit *u, uint32_t start, uint32_t end) {
    if (!u->media) {
        mc_scsi_unit_sense(u, MC_SENSE_NOT_READY, 0x3A, 0);
        return MC_SCSI_CHECK_CONDITION;
    }
    mc_disk *dk = disc(u);
    if (start == end) {
        if (dk && start < mc_disk_leadout(dk)) mc_disk_seek(dk, start, 1);
        return MC_SCSI_GOOD;
    }
    if (!dk || end > mc_disk_leadout(dk) || start > end) return illegal(u, 0x21);   /* out of range */
    if (!mc_disk_play(dk, start, end)) return illegal(u, 0x64);
    mc_disk_set_volume(dk, u->volume[0], u->volume[1]);
    u->audio_reported = 0;
    return MC_SCSI_GOOD;
}
static uint32_t msf_frame(const uint8_t *p) { return ((uint32_t)p[0] * 60u + p[1]) * 75u + p[2]; }

/* Starts a scan from frame `start` (MMC-1 5.1.14). */
static int scan(mc_scsi_unit *u, uint32_t start, int reverse) {
    mc_disk *dk = disc(u);
    if (!dk || start >= mc_disk_leadout(dk)) return illegal(u, 0x21);
    if (!mc_disk_scan(dk, start, reverse)) return illegal(u, 0x64);
    mc_disk_set_volume(dk, u->volume[0], u->volume[1]);
    u->audio_reported = 0;
    return MC_SCSI_GOOD;
}

static int audio(mc_scsi_unit *u, const uint8_t *cdb) {
    mc_disk *dk = disc(u);
    const uint32_t per = 2048 / u->block;      /* logical blocks a frame */
    uint32_t now = 0;
    mc_disk_play_state(dk, &now);
    switch (cdb[0]) {
    case 0x45: {    /* PLAY AUDIO(10), 14.2.2: LBA bytes 2-5, length bytes 7-8 (blocks) */
        const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        const uint32_t n = (uint32_t)cdb[7] << 8 | cdb[8];
        const uint32_t start = lba == 0xFFFFFFFFu ? now : lba / per;
        return play(u, start, start + (n + per - 1) / per);
    }
    case 0xA5: {    /* PLAY AUDIO(12), 14.2.3: length bytes 6-9 */
        const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        const uint32_t n = (uint32_t)cdb[6] << 24 | (uint32_t)cdb[7] << 16 | (uint32_t)cdb[8] << 8 | cdb[9];
        const uint32_t start = lba == 0xFFFFFFFFu ? now : lba / per;
        return play(u, start, start + (n + per - 1) / per);
    }
    case 0x47: {    /* PLAY AUDIO MSF, 14.2.4: start M S F bytes 3-5, end bytes 6-8 */
        const uint32_t s = msf_frame(cdb + 3), e = msf_frame(cdb + 6);
        const uint32_t start = cdb[3] == 0xFF && cdb[4] == 0xFF && cdb[5] == 0xFF ? now : s < 150 ? 0 : s - 150;
        if (e < 150) return illegal(u, 0x24);
        return play(u, start, e - 150);
    }
    case 0x48: {    /* PLAY AUDIO TRACK INDEX, 14.2.5: tracks in bytes 4 and 7 */
        mc_disk_track t[99];
        const unsigned n = u->media ? toc_tracks(u, t, 99) : 0;
        int first = -1, last = -1;
        for (unsigned i = 0; i < n; ++i) {
            if (t[i].number == cdb[4]) first = (int)i;
            if (t[i].number <= cdb[7]) last = (int)i;
        }
        if (!u->media) return play(u, 0, 0);
        if (first < 0 || last < first) return illegal(u, 0x24);
        return play(u, t[first].start, t[last].start + t[last].length);
    }
    case 0x4B:      /* PAUSE/RESUME, 14.2.1: byte 8 bit 0 resume */
        if (!u->media) return play(u, 0, 0);
        {
            const int state = mc_disk_play_state(dk, NULL);
            if (state != MC_CD_PLAYING && state != MC_CD_PAUSED) return illegal(u, 0x2C);   /* command sequence error */
        }
        mc_disk_pause(dk, !(cdb[8] & 1));
        return MC_SCSI_GOOD;
    case 0x4E:      /* STOP PLAY/SCAN (MMC-1 5.1.16): a scan becomes play */
        if (!mc_disk_stop_scan(dk)) mc_disk_stop(dk);
        return MC_SCSI_GOOD;
    case 0xBA: {    /* MMC-1 revision 10a 5.1.14, Table 37: SCAN */
        if (!u->media) return play(u, 0, 0);
        const unsigned type = cdb[9] >> 6;
        if ((cdb[1] & ~0x10u) || cdb[6] || cdb[7] || cdb[8] || (cdb[9] & 0x3F) || cdb[10] || type == 3)
            return illegal(u, 0x24);
        uint32_t start;
        if (type == 0) {
            const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
            start = lba / per;
        } else if (type == 1) {
            if (cdb[2] || cdb[3] > 99 || cdb[4] > 59 || cdb[5] > 74 || msf_frame(cdb + 3) < 150)
                return illegal(u, 0x24);
            start = msf_frame(cdb + 3) - 150;
        } else {
            if (cdb[2] || cdb[3] || cdb[4]) return illegal(u, 0x24);
            mc_disk_track tracks[99];
            const unsigned n = toc_tracks(u, tracks, 99);
            unsigned i = 0;
            while (i < n && tracks[i].number != cdb[5]) ++i;
            if (i == n) return illegal(u, 0x24);
            start = tracks[i].start;
        }
        return scan(u, start, (cdb[1] & 0x10) != 0);
    }
    /* Apple's vendor audio scan, which Apple's CD-ROM driver sends a drive
     * answering mode page $30 for AppleCD Audio Player's fast forward and
     * rewind (traced on Mac OS 8.1: byte 1 bit 4 reverse, the current
     * location as binary MSF in bytes 3-5†; resumed by a PLAY AUDIO MSF from
     * FF:FF:FF). Without it the player only skips a few seconds. */
    case 0xCD:
        if (!u->media) return play(u, 0, 0);
        if (cdb[2] || cdb[3] > 99 || cdb[4] > 59 || cdb[5] > 74 || msf_frame(cdb + 3) < 150) return illegal(u, 0x24);
        return scan(u, msf_frame(cdb + 3) - 150, (cdb[1] & 0x10) != 0);
    default:
        return -1;
    }
}

/* READ CD (SFF-8020i revision 2.6, 10.8.14; the ATAPI drives only): the
 * expected sector type in byte 1 bits 4-2 (0 any, 2 Mode 1), the starting
 * block in bytes 2-5, the length in blocks in bytes 6-8, the fields wanted
 * in byte 9 (bit 4 user data; sync, header, EDC/ECC and error bits),
 * sub-channel data in byte 10. The disc's one track is Mode 1 data, so
 * user data alone is a READ of 2048-byte blocks and no field is no data;
 * the other fields are not synthesized (invalid field in CDB). The ROM of
 * the PowerBook 190 reads the disc's partition map and driver so (ROM
 * 4D27039C ATAPI_Read, offset $AF934: BEh with byte 9 = 10h). */
static int read_cd(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    const unsigned type = (cdb[1] >> 2) & 7;
    const uint32_t lba = (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
    const uint32_t count = (uint32_t)cdb[6] << 16 | (uint32_t)cdb[7] << 8 | cdb[8];
    if ((cdb[9] != 0x10 && cdb[9] != 0) || cdb[10] || u->block != 2048) {
        mc_scsi_unit_sense(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0);
        return MC_SCSI_CHECK_CONDITION;
    }
    if (type != 0 && type != 2) {
        mc_scsi_unit_sense(u, MC_SENSE_ILLEGAL_REQUEST, 0x64, 0);   /* illegal mode for this track */
        return MC_SCSI_CHECK_CONDITION;
    }
    return mc_scsi_unit_read(u, lba, cdb[9] ? count : 0, direction, bytes);
}

static int cd_extra(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    if (cdb[0] == 0x43) return read_toc(u, cdb, direction, bytes);
    if (cdb[0] == 0x42) return read_subchannel(u, cdb, direction, bytes);
    if (cdb[0] == 0x45 || cdb[0] == 0xA5 || cdb[0] == 0x47 || cdb[0] == 0x48 || cdb[0] == 0x4B || cdb[0] == 0x4E || cdb[0] == 0xBA ||
        cdb[0] == 0xCD) {
        *direction = MC_SCSI_XFER_NONE;
        *bytes = 0;
        return audio(u, cdb);
    }
    if (cdb[0] == 0xBE && u->read_cd) return read_cd(u, cdb, direction, bytes);
    /* A seek ends audio play and moves the head (REZERO UNIT to block 0:
     * AppleCD Audio Player's stop button on a SCSI drive); START STOP UNIT
     * stopping the disc ends it too. Their status is the common one. */
    if (u->media && (cdb[0] == 0x01 || cdb[0] == 0x0B || cdb[0] == 0x2B)) {
        const uint32_t lba = cdb[0] == 0x01 ? 0
                           : cdb[0] == 0x0B ? ((uint32_t)cdb[1] << 16 | (uint32_t)cdb[2] << 8 | cdb[3]) & 0x1FFFFF
                                            : (uint32_t)cdb[2] << 24 | (uint32_t)cdb[3] << 16 | (uint32_t)cdb[4] << 8 | cdb[5];
        mc_disk_seek(disc(u), lba / (2048 / u->block), 0);
    } else if (u->media && cdb[0] == 0x1B && !(cdb[4] & 0x01)) {
        mc_disk_stop(disc(u));
    }
    return -1;
}

void mc_scsi_cd_init(mc_scsi_unit *u) {
    mc_scsi_unit_init_common(u);
    u->cd = 1;
    u->cd_block = u->block = MC_SCSI_CD_BLOCK;
    u->extra = cd_extra;
    u->volume[0] = u->volume[1] = 0xFF;
}

/* The Pippin ROM reads 2048 bytes per READ(10) block without MODE SELECT,
 * even from a bare HFS disc. */
void mc_scsi_cd_init_pippin(mc_scsi_unit *u) {
    mc_scsi_cd_init(u);
    u->apple_cd = 1;
    u->cd_block = u->block = 2048;
}

/* The block length a disc is laid out for: sbBlkSize of its driver
 * descriptor record ('ER' at block 0, Inside Macintosh: Devices, "SCSI
 * Manager", "Disk Partitioning") when that is 2048, else 512. */
static uint32_t disc_block(mc_vdisk *media) {
    uint8_t first[512];
    if (mc_vdisk_read(media, 0, 1, first) && first[0] == 'E' && first[1] == 'R' && (first[2] << 8 | first[3]) == 2048) return 2048;
    return MC_SCSI_CD_BLOCK;
}

int mc_scsi_cd_insert(mc_scsi_unit *u, mc_vdisk *media) {
    if (u->media || !media) return 0;
    u->media = media;
    /* KINKA's .AppleCD reads one whole 2048-byte block from the
     * internal drive without MODE SELECT, including a bare HFS disc. */
    u->cd_block = u->apple_cd ? 2048 : disc_block(media);
    u->block = u->cd_block;
    u->ua_asc = 0x28;   /* not ready to ready change, medium may have changed (SCSI-2 7.9) */
    mc_disk_stop(mc_vdisk_single(media));
    mc_disk_set_volume(mc_vdisk_single(media), u->volume[0], u->volume[1]);
    return 1;
}
