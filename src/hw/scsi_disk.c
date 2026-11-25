/* Livadix — the SCSI direct-access disk and the command set it shares with the
 * CD-ROM (scsi_target.h). SCSI-2 (X3.131-1994) sections cited per command. */
#include "scsi_target.h"

#include <string.h>


static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put24(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t get24(const uint8_t *p) { return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

void mc_scsi_unit_sense(mc_scsi_unit *u, uint8_t key, uint8_t asc, uint8_t ascq) {
    u->key = key;
    u->asc = asc;
    u->ascq = ascq;
    u->info_valid = 0;
}

uint32_t mc_scsi_unit_capacity(const mc_scsi_unit *u) {
    if (!u->media) return 0;
    uint64_t n = mc_vdisk_blocks(u->media) / (u->block / 512u);
    return n > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)n;
}

static uint8_t check(mc_scsi_unit *u, uint8_t key, uint8_t asc, uint8_t ascq) {
    mc_scsi_unit_sense(u, key, asc, ascq);
    return MC_SCSI_CHECK_CONDITION;
}

/* A response of `size` bytes in u->data, cut to the allocation length. */
static uint8_t respond(mc_scsi_unit *u, uint32_t size, uint32_t allocation, int *direction, uint32_t *bytes) {
    u->mode = MC_UNIT_RESPONSE;
    *direction = MC_SCSI_XFER_IN;
    *bytes = size < allocation ? size : allocation;
    return MC_SCSI_GOOD;
}

/* INQUIRY standard data (8.2.5.1, Table 45). */
static uint8_t inquiry(mc_scsi_unit *u, unsigned lun, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    if (cdb[1] & 1) return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0);   /* EVPD: no vital product data pages */
    uint8_t *d = u->data;
    memset(d, 0, 36);
    if (lun) {
        d[0] = 0x7F;            /* peripheral qualifier 3, type 1Fh: no unit at this LUN */
    } else {
        d[0] = u->cd ? 0x05 : 0x00;
        d[1] = u->cd ? 0x80 : 0x00;     /* RMB */
    }
    d[2] = 0x02;                /* ANSI-approved version: SCSI-2 */
    d[3] = 0x02;                /* response data format 2 */
    d[4] = 31;                  /* additional length */
    memcpy(d + 8, "LIVADIX ", 8);
    memcpy(d + 16, u->cd ? "VIRTUAL CD-ROM  " : "VIRTUAL DISK    ", 16);
    memcpy(d + 32, "1.0 ", 4);
    return respond(u, 36, cdb[4], direction, bytes);
}

/* REQUEST SENSE: fixed-format sense data (8.2.14.1, Table 65). A pending
 * unit attention is reported (and cleared) first. An allocation length of
 * zero transfers nothing (8.2.14). */
static uint8_t request_sense(mc_scsi_unit *u, const uint8_t *cdb, int *direction, uint32_t *bytes) {
    if (u->ua_asc) {
        mc_scsi_unit_sense(u, MC_SENSE_UNIT_ATTENTION, u->ua_asc, 0);
        u->ua_asc = 0;
    }
    uint8_t *d = u->data;
    memset(d, 0, 18);
    d[0] = 0x70 | (u->info_valid ? 0x80 : 0);
    d[2] = u->key;
    put32(d + 3, u->info);
    d[7] = 10;
    d[12] = u->asc;
    d[13] = u->ascq;
    mc_scsi_unit_sense(u, MC_SENSE_NONE, 0, 0);
    u->info = 0;
    return respond(u, 18, cdb[4], direction, bytes);
}

/* A notional geometry for pages 03h and 04h: 16 heads, 63 sectors per
 * track (Livadix's choice; the drive has no physical geometry). */
#define HEADS 16u
#define SECTORS 63u

/* One mode page; `changeable` returns the mask of changeable fields (none
 * but the block descriptor's length). Returns its length, 0 when absent. */
static unsigned mode_page(mc_scsi_unit *u, unsigned page, int changeable, uint8_t *p) {
    uint32_t blocks = mc_scsi_unit_capacity(u);
    unsigned n;
    switch (page) {
    case 0x01: n = 12; break;   /* read-write error recovery (9.3.3.6), page length 0Ah */
    case 0x03: n = 24; break;   /* format device (9.3.3.3), 16h */
    case 0x04: n = 24; break;   /* rigid disk drive geometry (9.3.3.7), 16h */
    case 0x08: n = 12; break;   /* caching (9.3.3.1), 0Ah */
    case 0x0D: n = u->cd ? 8 : 0; break;   /* CD-ROM device parameters (14.3.3.2), 06h */
    case 0x0E: n = u->cd ? 16 : 0; break;  /* CD-ROM audio control (14.3.3.1), 0Eh */
    case 0x2A: n = u->read_cd ? 20 : 0; break;     /* ATAPI: CD capabilities and mechanical status (SFF-8020i), 12h */
    case 0x30: n = u->cd ? 24 : 0; break;   /* Apple's identification, below */
    default: n = 0; break;
    }
    if (!n) return 0;
    memset(p, 0, n);
    p[0] = (uint8_t)page;
    p[1] = (uint8_t)(n - 2);
    if (changeable) return n;
    switch (page) {
    case 0x03:
        put16(p + 10, SECTORS);
        put16(p + 12, u->block);
        put16(p + 14, 1);       /* interleave */
        p[20] = u->cd ? 0x20 : 0x40;    /* RMB for removable media; HSEC (hard sectored) otherwise */
        break;
    case 0x04:
        put24(p + 2, (blocks + HEADS * SECTORS - 1) / (HEADS * SECTORS));
        p[5] = HEADS;
        put16(p + 20, 3600);    /* medium rotation rate, rpm */
        break;
    case 0x0D:
        put16(p + 4, 60);       /* S units per M unit */
        put16(p + 6, 75);       /* F units per S unit */
        break;
    case 0x0E:
        p[2] = 0x04;            /* Immed */
        p[8] = 1; p[9] = u->volume[0];     /* output port 0: channel 0 */
        p[10] = 2; p[11] = u->volume[1];   /* output port 1: channel 1 */
        break;
    case 0x30:
        /* The identification Apple's CD drives give: the Pippin
         * development ROM 2BF65931's .AppleCD ($244A3E-$244AF8: MODE
         * SENSE(6), page $30, allocation $24; 22 bytes compared at
         * response offset 14 after the header, descriptor and page
         * header), and the Apple CD-ROM extension of the Systems, which
         * takes a drive only once it reads it (Mac OS 8.1 sends the same
         * MODE SENSE at start-up). */
        memcpy(p + 2, "APPLE COMPUTER, INC   ", 22);
        break;
    case 0x2A:
        /* No CD-R/E reading or writing; audio play (byte 4 bit 0); lock and
         * eject supported, the lock state, a tray (loading mechanism 001b);
         * a 4x drive (706 KB/s maximum and current), 256 volume levels, a
         * 128 KB buffer†. */
        p[4] = 0x01;
        p[6] = (uint8_t)(0x20 | 0x08 | (u->prevent ? 0x02 : 0) | 0x01);
        put16(p + 8, 706);
        put16(p + 10, 256);
        put16(p + 12, 128);
        put16(p + 14, 706);
        break;
    default:
        break;
    }
    return n;
}

/* MODE SENSE(6) and (10) (8.2.10, 8.2.11): header, one block descriptor
 * unless DBD, then the pages asked for. */
static uint8_t mode_sense(mc_scsi_unit *u, const uint8_t *cdb, int ten, int *direction, uint32_t *bytes) {
    unsigned pc = cdb[2] >> 6, page = cdb[2] & 0x3F;
    /* ATAPI drives return no block descriptor (SFF-8020i, mode parameter
     * header: block descriptor length 0). */
    int dbd = (cdb[1] & 0x08) != 0 || u->read_cd;
    uint32_t allocation = ten ? get16(cdb + 7) : cdb[4];
    if (pc == 3) return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x39, 0);   /* saving parameters not supported */
    uint8_t *d = u->data;
    unsigned header = ten ? 8 : 4, at = header;
    memset(d, 0, sizeof u->data);
    if (!dbd) {
        uint32_t blocks = mc_scsi_unit_capacity(u);
        if (pc != 1) {
            put24(d + at + 1, blocks > 0xFFFFFF ? 0 : blocks);
            put24(d + at + 5, u->block);
        } else {
            put24(d + at + 5, u->cd ? 0xFFFFFF : 0);    /* only a CD's block length can change */
        }
        at += 8;
    }
    if (page == 0x3F) {
        static const unsigned all[] = {0x01, 0x03, 0x04, 0x08, 0x0D, 0x0E, 0x2A, 0x30};
        for (unsigned i = 0; i < sizeof all / sizeof all[0]; i++) at += mode_page(u, all[i], pc == 1, d + at);
    } else if (page) {
        unsigned n = mode_page(u, page, pc == 1, d + at);
        if (!n) return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0);
        at += n;
    }
    uint8_t specific = (u->cd || (u->media && mc_vdisk_all_readonly(u->media))) ? 0x80 : 0;   /* WP */
    if (ten) {
        put16(d, at - 2);
        d[3] = specific;
        put16(d + 6, dbd ? 0 : 8);
    } else {
        d[0] = (uint8_t)(at - 1);
        d[2] = specific;
        d[3] = dbd ? 0 : 8;
    }
    return respond(u, at, allocation, direction, bytes);
}

/* MODE SELECT parameters (8.2.8, 8.3.3): the block descriptor's length and
 * a CD's audio control page (14.3.3.1: the volumes of output ports 0 and 1)
 * are acted on; the other pages are accepted and kept nowhere. */
static uint8_t mode_select_done(mc_scsi_unit *u) {
    const uint8_t *d = u->data;
    int ten = u->lba != 0;
    uint32_t n = u->length;
    uint32_t header = ten ? 8 : 4;
    uint32_t bdl = ten ? get16(d + 6) : d[3];
    if (n < header) return n ? check(u, MC_SENSE_ILLEGAL_REQUEST, 0x1A, 0) : MC_SCSI_GOOD;
    if (bdl >= 8 && n >= header + 8) {
        uint32_t length = get24(d + header + 5);
        if (length && length != u->block) {
            if (!u->cd || (length != 512 && length != 2048)) return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x26, 0);
            u->block = length;
        }
    }
    for (uint32_t at = header + bdl; u->cd && at + 2 <= n;) {
        const uint8_t *p = d + at;
        if ((p[0] & 0x3F) == 0x0E && p[1] >= 0x0E && at + 12 <= n) {
            u->volume[0] = p[9];
            u->volume[1] = p[11];
            if (u->media) mc_disk_set_volume(mc_vdisk_single(u->media), p[9], p[11]);
        }
        at += 2u + p[1];
    }
    return MC_SCSI_GOOD;
}

static uint8_t medium_ready(mc_scsi_unit *u) {
    if (!u->media) return check(u, MC_SENSE_NOT_READY, 0x3A, 0);   /* medium not present */
    return MC_SCSI_GOOD;
}

/* READ and WRITE of every size (9.2.5, 9.2.6, 9.2.20, 9.2.21, 9.2.22). */
static uint8_t transfer(mc_scsi_unit *u, uint64_t lba, uint32_t count, int write, int *direction, uint32_t *bytes) {
    if (medium_ready(u)) return MC_SCSI_CHECK_CONDITION;
    uint32_t capacity = mc_scsi_unit_capacity(u);
    if (lba > capacity || count > capacity - lba) {
        mc_scsi_unit_sense(u, MC_SENSE_ILLEGAL_REQUEST, 0x21, 0);   /* LBA out of range */
        u->info_valid = 1;
        u->info = (uint32_t)lba;
        return MC_SCSI_CHECK_CONDITION;
    }
    uint32_t factor = u->block / 512u;
    if (u->cd && !write && count) {
        /* An audio track read as data: illegal mode for this track (64h,
         * SCSI-2 Table 71), as Apple's CD-ROM driver expects of an audio
         * CD before it hands the disc to Audio CD Access. */
        mc_disk *d = mc_vdisk_single(u->media);
        mc_disk_track t;
        const uint32_t per = 2048 / u->block;
        if (d && mc_disk_tracks(d) > 1 && mc_disk_track_at(d, (uint32_t)(lba / per), &t) && t.audio)
            return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x64, 0);
        mc_disk_track first;
        if (d && mc_disk_track_get(d, 0, &first) && first.audio && mc_disk_tracks(d) == 1)
            return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x64, 0);
    }
    if (write && (u->cd || mc_vdisk_readonly(u->media, lba * factor, count * factor)))
        return check(u, MC_SENSE_DATA_PROTECT, 0x27, 0);           /* write protected */
    if (!count) return MC_SCSI_GOOD;
    if ((uint64_t)count * u->block > 0xFFFFFFFFu) return check(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0);
    u->mode = write ? MC_UNIT_WRITE : MC_UNIT_READ;
    u->lba = lba;
    *direction = write ? MC_SCSI_XFER_OUT : MC_SCSI_XFER_IN;
    *bytes = count * u->block;
    return MC_SCSI_GOOD;
}

uint8_t mc_scsi_unit_read(mc_scsi_unit *u, uint64_t lba, uint32_t count, int *direction, uint32_t *bytes) {
    return transfer(u, lba, count, 0, direction, bytes);
}

static uint8_t unit_command(mc_scsi_target *t, unsigned lun, const uint8_t *cdb, unsigned length, int *direction,
                            uint32_t *bytes) {
    mc_scsi_unit *u = (mc_scsi_unit *)t;
    (void)length;
    u->mode = MC_UNIT_NONE;
    u->status = MC_SCSI_GOOD;
    uint8_t op = cdb[0];
    /* Without an IDENTIFY message the LUN is in the CDB (SCSI-1 style). */
    if (!lun) lun = cdb[1] >> 5;
    if (op == 0x12) return u->status = inquiry(u, lun, cdb, direction, bytes);
    if (lun) return u->status = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x25, 0);   /* LUN not supported */
    if (op == 0x03) return u->status = request_sense(u, cdb, direction, bytes);
    if (u->ua_asc) {        /* 7.9: a pending unit attention fails the command */
        mc_scsi_unit_sense(u, MC_SENSE_UNIT_ATTENTION, u->ua_asc, 0);
        u->ua_asc = 0;
        return u->status = MC_SCSI_CHECK_CONDITION;
    }
    if (u->extra) {
        int r = u->extra(u, cdb, direction, bytes);
        if (r >= 0) return u->status = (uint8_t)r;
    }
    uint8_t s = MC_SCSI_GOOD;
    switch (op) {
    case 0x00:              /* TEST UNIT READY */
    case 0x01:              /* REZERO UNIT */
    case 0x0B:              /* SEEK(6) */
    case 0x2B:              /* SEEK(10) */
    case 0x35:              /* SYNCHRONIZE CACHE */
        s = medium_ready(u);
        break;
    case 0x16: case 0x17:   /* RESERVE, RELEASE */
    case 0x1D:              /* SEND DIAGNOSTIC: the self-test passes */
        break;
    case 0x04:              /* FORMAT UNIT: nothing to do; a parameter list header is taken and dropped */
        if (u->cd) { s = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x20, 0); break; }
        if ((s = medium_ready(u)) != 0) break;
        if (cdb[1] & 0x10) {
            u->mode = MC_UNIT_DISCARD;
            *direction = MC_SCSI_XFER_OUT;
            *bytes = 4;
        }
        break;
    case 0x08: s = transfer(u, get24(cdb + 1) & 0x1FFFFF, cdb[4] ? cdb[4] : 256, 0, direction, bytes); break;
    case 0x0A: s = transfer(u, get24(cdb + 1) & 0x1FFFFF, cdb[4] ? cdb[4] : 256, 1, direction, bytes); break;
    case 0x28: s = transfer(u, get32(cdb + 2), get16(cdb + 7), 0, direction, bytes); break;
    case 0x2A: case 0x2E: s = transfer(u, get32(cdb + 2), get16(cdb + 7), 1, direction, bytes); break;
    case 0xA8: s = transfer(u, get32(cdb + 2), get32(cdb + 6), 0, direction, bytes); break;
    case 0xAA: s = transfer(u, get32(cdb + 2), get32(cdb + 6), 1, direction, bytes); break;
    case 0x2F:              /* VERIFY: the medium is always good; BytChk data is taken and dropped */
        if ((s = medium_ready(u)) != 0) break;
        if (cdb[1] & 0x02) {
            uint64_t n = (uint64_t)get16(cdb + 7) * u->block;
            if (n > 0xFFFFFFFFu) { s = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0); break; }
            u->mode = MC_UNIT_DISCARD;
            *direction = MC_SCSI_XFER_OUT;
            *bytes = (uint32_t)n;
        }
        break;
    case 0x25: {            /* READ CAPACITY: last block and block length (9.2.7.1) */
        if ((s = medium_ready(u)) != 0) break;
        uint32_t n = mc_scsi_unit_capacity(u);
        put32(u->data, n ? n - 1 : 0);
        put32(u->data + 4, u->block);
        s = respond(u, 8, 8, direction, bytes);
        break;
    }
    case 0x1A: s = mode_sense(u, cdb, 0, direction, bytes); break;
    case 0x5A: s = mode_sense(u, cdb, 1, direction, bytes); break;
    case 0x15: case 0x55: { /* MODE SELECT(6), (10): the parameter list comes in DATA OUT */
        uint32_t n = op == 0x15 ? cdb[4] : get16(cdb + 7);
        if (n > sizeof u->data) { s = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x24, 0); break; }
        u->lba = op == 0x55;
        u->length = n;
        if (n) {
            u->mode = MC_UNIT_PARAMS;
            *direction = MC_SCSI_XFER_OUT;
            *bytes = n;
        }
        break;
    }
    case 0x1B:              /* START STOP UNIT: LoEj with Start clear ejects (9.2.17) */
        if ((cdb[4] & 0x03) == 0x02) {
            if (!u->cd) break;              /* a fixed disk has nothing to eject */
            if (u->prevent) { s = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x53, 0x02); break; }
            mc_vdisk *media = mc_scsi_unit_remove(u);
            if (media && u->on_eject) u->on_eject(u->eject_ctx, u, media);
        } else if (cdb[4] & 0x01) {
            s = medium_ready(u);
        }
        break;
    case 0x1E:              /* PREVENT ALLOW MEDIUM REMOVAL */
        u->prevent = cdb[4] & 1;
        if (u->media) mc_disk_set_locked(mc_vdisk_single(u->media), u->prevent);
        break;
    default:
        s = check(u, MC_SENSE_ILLEGAL_REQUEST, 0x20, 0);   /* invalid command operation code */
        break;
    }
    return u->status = s;
}

static int unit_data_in(mc_scsi_target *t, uint8_t *buffer, uint32_t offset, uint32_t count) {
    mc_scsi_unit *u = (mc_scsi_unit *)t;
    if (u->mode == MC_UNIT_RESPONSE) {
        memcpy(buffer, u->data + offset, count);
        return 0;
    }
    uint32_t factor = u->block / 512u;
    uint64_t first = u->lba * factor + offset / 512u;
    if (!u->media || !mc_vdisk_read(u->media, first, count / 512u, buffer)) {
        mc_scsi_unit_sense(u, MC_SENSE_MEDIUM_ERROR, 0x11, 0);     /* unrecovered read error */
        u->info_valid = 1;
        u->info = (uint32_t)(u->lba + offset / u->block);
        u->status = MC_SCSI_CHECK_CONDITION;
        return 1;
    }
    return 0;
}

static int unit_data_out(mc_scsi_target *t, const uint8_t *buffer, uint32_t offset, uint32_t count) {
    mc_scsi_unit *u = (mc_scsi_unit *)t;
    if (u->mode == MC_UNIT_PARAMS) {
        memcpy(u->data + offset, buffer, count);
        if (offset + count >= u->length) u->status = mode_select_done(u);
        return 0;
    }
    if (u->mode != MC_UNIT_WRITE) return 0;
    uint32_t factor = u->block / 512u;
    uint64_t first = u->lba * factor + offset / 512u;
    if (!u->media || !mc_vdisk_write(u->media, first, count / 512u, buffer)) {
        mc_scsi_unit_sense(u, MC_SENSE_MEDIUM_ERROR, 0x0C, 0);     /* write error */
        u->status = MC_SCSI_CHECK_CONDITION;
        return 1;
    }
    return 0;
}

static uint8_t unit_finish(mc_scsi_target *t) { return ((mc_scsi_unit *)t)->status; }

static void unit_reset(mc_scsi_target *t) {
    mc_scsi_unit *u = (mc_scsi_unit *)t;
    u->prevent = 0;
    u->mode = MC_UNIT_NONE;
    if (u->cd) u->block = u->cd_block;          /* mode parameters return to their defaults (7.5) */
}

static void unit_init(mc_scsi_unit *u) {
    memset(u, 0, sizeof *u);
    u->target.command = unit_command;
    u->target.data_in = unit_data_in;
    u->target.data_out = unit_data_out;
    u->target.finish = unit_finish;
    u->target.reset = unit_reset;
}

void mc_scsi_disk_init(mc_scsi_unit *u, mc_vdisk *media) {
    unit_init(u);
    u->block = 512;
    u->media = media;
}

void mc_scsi_unit_init_common(mc_scsi_unit *u) { unit_init(u); }

mc_vdisk *mc_scsi_unit_remove(mc_scsi_unit *u) {
    mc_vdisk *media = u->media;
    if (media) mc_disk_set_locked(mc_vdisk_single(media), 0);
    u->media = NULL;
    u->prevent = 0;
    return media;
}

void mc_scsi_unit_on_eject(mc_scsi_unit *u, mc_scsi_eject_fn fn, void *ctx) {
    u->on_eject = fn;
    u->eject_ctx = ctx;
}
