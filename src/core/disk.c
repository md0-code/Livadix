/* Disk images: raw images written in place, read-only images, DiskCopy 4.2
 * floppy images with their data checksum kept current, and CD images: of
 * 2048-byte sectors, of raw 2352-byte sectors (the data taken out of each),
 * or a cue sheet naming either. */
#include "disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define seek_file _fseeki64
#define tell_file _ftelli64
#else
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#define seek_file fseeko
#define tell_file ftello
#endif
/* A raw image holds sectors from byte zero. A DiskCopy 4.2 image holds them
 * after its 84-byte header (data_offset), and `data` keeps a copy of them
 * for the header's data checksum. */
struct mc_disk {
    FILE *file; uint64_t sectors, data_offset; uint32_t sector_size; int writable, failed;
    uint64_t dirty_since;               /* host milliseconds of the oldest write not yet durable; 0 none */
    struct mc_disk *next_dirty;
    uint8_t *data; char label[24];
    uint8_t *tags; uint32_t tag_size;   /* DiskCopy 4.2 tags: 12 bytes per sector, after the data */
    /* A CD image of raw sectors: each takes `raw` bytes of the file and its
     * 2048 data bytes begin `raw_skip` bytes in; 0 for an image of data only. */
    uint32_t raw, raw_skip;
    struct mc_cd *cd;                   /* a cue sheet's tracks and the audio player; NULL otherwise */
    int locked;                         /* mc_disk_set_locked */
    int blank;                          /* an audio CD: its data reads give zeros */
};
int mc_disk_readonly(const mc_disk *disk) { return !disk || !disk->writable; }
const char *mc_disk_label(const mc_disk *disk) { return disk?disk->label:""; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
static int durable(FILE *file) {
    if (fflush(file)) return 0;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}
/* DiskCopy 4.2 data checksum: add each big-endian word, then rotate right. */
static uint32_t diskcopy_checksum(const uint8_t *data, uint64_t size) {
    uint32_t sum = 0;
    for (uint64_t i = 0; i + 1 < size; i += 2) {
        sum += ((uint32_t)data[i] << 8) | data[i + 1];
        sum = (sum >> 1) | (sum << 31);
    }
    return sum;
}
/* Writes not yet durable: the disks that have them, oldest first. */
static mc_disk *dirty_list;
static uint64_t host_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}
static void unlist(mc_disk *d) {
    for (mc_disk **p = &dirty_list; *p; p = &(*p)->next_dirty)
        if (*p == d) { *p = d->next_dirty; break; }
    d->next_dirty = NULL;
    d->dirty_since = 0;
}
static void make_durable(mc_disk *d) {
    if (!d->dirty_since) return;
    if (!durable(d->file)) d->failed = 1;
    unlist(d);
}
static void sync_at_exit(void) { mc_disk_sync(1); }
/* Called after each write: the data is with the host system (fflush); the
 * disk waits on the list for its flush to the device. */
static int written(mc_disk *d) {
    if (fflush(d->file)) return 0;
    if (!d->dirty_since) {
        static int registered;
        if (!registered) { registered = 1; atexit(sync_at_exit); }
        const uint64_t now = host_ms();
        d->dirty_since = now ? now : 1;
        mc_disk **p = &dirty_list;
        while (*p) p = &(*p)->next_dirty;
        *p = d;
    }
    return 1;
}
void mc_disk_sync(int all) {
    if (!dirty_list) return;
    const uint64_t now = host_ms();
    while (dirty_list && (all || now - dirty_list->dirty_since >= 1000u)) make_durable(dirty_list);
}
struct mc_cd;
static void cd_free(struct mc_cd *cd);
void mc_disk_close(mc_disk *d) {
    if (!d) return;
    make_durable(d);
    if (d->cd) cd_free(d->cd);          /* its files include d->file */
    else if (d->file) fclose(d->file);
    free(d->data); free(d->tags); free(d);
}
/* Header: name length (at most 63) at 0, data size at 40, tag size at 44,
 * data checksum at 48, tag checksum at 4c, format at 50, 0100 at 52. */
static int diskcopy_header(const uint8_t *h, int64_t length) {
    return h[0] <= 63 && h[0x52] == 1 && h[0x53] == 0 &&
           (uint64_t)0x54 + be32(h + 0x40) + be32(h + 0x44) == (uint64_t)length;
}
/* A raw CD sector (ECMA-130 section 14): 12 bytes of sync (00, ten FF, 00),
 * the address in minutes, seconds and frames and the mode. Mode 1 holds 2048
 * data bytes after these 16 (14.3); a Mode 2 sector of the CD-ROM XA form 1
 * holds them after an 8-byte subheader. */
#define RAW_SECTOR 2352u
static int raw_sector_skip(const uint8_t *h) {
    static const uint8_t sync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (memcmp(h, sync, sizeof sync)) return 0;
    return h[15] == 1 ? 16 : h[15] == 2 ? 24 : 0;
}
/* A cue sheet (as CDRWIN wrote them): FILE "name" lines, each followed by
 * its TRACK nn MODE lines with their INDEX 00 (the pregap) and INDEX 01 (the
 * track) as mm:ss:ff in frames of the file, and PREGAP lines (silence not in
 * the file). Track modes: MODE1/2048, MODE1/2352, MODE2/2352 (form 1),
 * MODE2/2336 and AUDIO (2352 bytes: 588 stereo 16-bit little-endian
 * samples, ECMA-130 16.1). Disc addresses: track 1's INDEX 01 is logical
 * block 0; every track after it starts after the one before it, its pregap
 * between them. A file named by the sheet but absent is looked for as the
 * sheet's own name with .bin when the sheet names one file. */
#define CD_MAX_TRACKS 99
#define CD_FRAME_SAMPLES 588u
typedef struct cd_track {
    unsigned number, file, bytes, skip;     /* bytes a frame takes in its file, data's offset in it */
    int audio;
    uint64_t offset;                        /* INDEX 01's byte in its file */
    uint32_t start, length;                 /* disc address of INDEX 01, frames */
    uint32_t index0, index1, pregap;        /* as the sheet gives them, frames */
    int has_index0;
} cd_track;
struct mc_cd {
    FILE *files[CD_MAX_TRACKS];
    unsigned nfiles, ntracks;
    cd_track t[CD_MAX_TRACKS];
    uint32_t leadout;
    /* The player (mc_disk_play...). Times in samples (1/44100 s) of the
     * clock the machine gives. */
    uint64_t (*clock)(void *ctx);
    void *clock_ctx;
    int state;
    uint32_t play_start, play_end;          /* contiguous audio region, frames */
    uint32_t scan_start, stopped;           /* scan origin and last stopped address */
    int scan_step;                         /* 0 normal, +8 forward, -8 reverse */
    uint64_t t0;                            /* the clock when play_start sounded */
    uint64_t paused;                        /* samples played when paused */
    double cursor;                          /* the mixer's next sample, from play_start */
    uint8_t volume[2];
    uint32_t cached;                        /* the frame in `cache`, UINT32_MAX none */
    int16_t cache[CD_FRAME_SAMPLES * 2];
};
static int has_suffix(const char *path, const char *suffix) {
    const size_t n = strlen(path), k = strlen(suffix);
    if (n < k) return 0;
    for (size_t i = 0; i < k; ++i) {
        char c = path[n - k + i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != suffix[i]) return 0;
    }
    return 1;
}
static void cd_free(struct mc_cd *cd) {
    if (!cd) return;
    for (unsigned i = 0; i < cd->nfiles; ++i) if (cd->files[i]) fclose(cd->files[i]);
    free(cd);
}
static int msf_frames(const char *p, uint32_t *frames) {
    unsigned m, sec, f;
    if (sscanf(p, "%u:%u:%u", &m, &sec, &f) != 3 || sec >= 60 || f >= 75) return 0;
    *frames = (m * 60u + sec) * 75u + f;
    return 1;
}
static struct mc_cd *parse_cue(const char *path, const char **problem) {
    char line[1024], names[CD_MAX_TRACKS][512];
    struct mc_cd *cd = calloc(1, sizeof *cd);
    if (!cd) { *problem = "Out of memory"; return NULL; }
    cd->cached = UINT32_MAX;
    cd->volume[0] = cd->volume[1] = 0xFF;
    FILE *cue = fopen(path, "rb");
    if (!cue) { free(cd); return NULL; }
    unsigned files = 0;
    *problem = NULL;
    while (fgets(line, sizeof line, cue) && !*problem) {
        const char *p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (!strncmp(p, "FILE", 4)) {
            const char *a = strchr(p, '"'), *b = a ? strchr(a + 1, '"') : NULL;
            if (!a || !b || (size_t)(b - a - 1) >= sizeof names[0] || files == CD_MAX_TRACKS) { *problem = "The cue sheet's FILE line cannot be read"; break; }
            memcpy(names[files], a + 1, (size_t)(b - a - 1));
            names[files][b - a - 1] = 0;
            files++;
        } else if (!strncmp(p, "TRACK", 5)) {
            if (!files || cd->ntracks == CD_MAX_TRACKS) { *problem = "The cue sheet's TRACK line comes before a FILE"; break; }
            cd_track *t = &cd->t[cd->ntracks++];
            t->number = (unsigned)atoi(p + 5);
            t->file = files - 1;
            if (strstr(p, "AUDIO")) { t->audio = 1; t->bytes = RAW_SECTOR; }
            else if (strstr(p, "MODE1/2048")) t->bytes = 2048;
            else if (strstr(p, "MODE1/2352")) { t->bytes = RAW_SECTOR; t->skip = 16; }
            else if (strstr(p, "MODE2/2352")) { t->bytes = RAW_SECTOR; t->skip = 24; }
            else if (strstr(p, "MODE2/2336")) { t->bytes = 2336; t->skip = 8; }
            else *problem = "The cue sheet has a track mode Livadix does not read";
        } else if (!strncmp(p, "INDEX", 5) && cd->ntracks) {
            cd_track *t = &cd->t[cd->ntracks - 1];
            unsigned index = (unsigned)atoi(p + 5);
            const char *at = p + 5;
            while (*at == ' ') ++at;
            while (*at >= '0' && *at <= '9') ++at;
            uint32_t f;
            if (!msf_frames(at, &f)) { *problem = "The cue sheet's INDEX line cannot be read"; break; }
            if (index == 0) { t->index0 = f; t->has_index0 = 1; }
            else if (index == 1) t->index1 = f;
        } else if (!strncmp(p, "PREGAP", 6) && cd->ntracks) {
            uint32_t f;
            if (msf_frames(p + 6, &f)) cd->t[cd->ntracks - 1].pregap = f;
        }
    }
    fclose(cue);
    if (!*problem && (!files || !cd->ntracks)) *problem = "The cue sheet names no image file with a track";
    size_t folder = 0;
    for (size_t i = 0; path[i]; ++i) if (path[i] == '/' || path[i] == '\\') folder = i + 1;
    for (unsigned k = 0; !*problem && k < files; ++k) {
        char image[2048];
        if (folder + strlen(names[k]) >= sizeof image || strlen(path) + 1 >= sizeof image) { *problem = "The cue sheet's path is too long"; break; }
        memcpy(image, path, folder);
        strcpy(image + folder, names[k]);
        cd->files[k] = fopen(image, "rb");
        if (!cd->files[k] && files == 1) {
            strcpy(image, path);
            strcpy(image + strlen(image) - 4, ".bin");
            cd->files[k] = fopen(image, "rb");
        }
        cd->nfiles = k + 1;
        if (!cd->files[k]) *problem = "Cannot open the image file the cue sheet names";
    }
    /* Byte offsets and lengths within each file, then disc addresses. */
    uint32_t lba = 0;
    for (unsigned k = 0; !*problem && k < cd->ntracks; ++k) {
        cd_track *t = &cd->t[k];
        const uint32_t from = t->has_index0 ? t->index0 : t->index1;
        uint64_t base = 0;
        if (k && cd->t[k - 1].file == t->file) {
            const cd_track *u = &cd->t[k - 1];
            base = u->offset + (uint64_t)(from - u->index1) * u->bytes;
        } else {
            base = (uint64_t)from * t->bytes;
        }
        if (from > t->index1) { *problem = "The cue sheet's indices go backwards"; break; }
        t->offset = base + (uint64_t)(t->index1 - from) * t->bytes;
        if (k + 1 < cd->ntracks && cd->t[k + 1].file == t->file) {
            const cd_track *n = &cd->t[k + 1];
            const uint32_t next = n->has_index0 ? n->index0 : n->index1;
            if (next < t->index1) { *problem = "The cue sheet's indices go backwards"; break; }
            t->length = next - t->index1;
        } else {
            FILE *f = cd->files[t->file];
            if (seek_file(f, 0, SEEK_END)) { *problem = "Cannot read the image file"; break; }
            const int64_t size = tell_file(f);
            t->length = size > (int64_t)t->offset ? (uint32_t)(((uint64_t)size - t->offset) / t->bytes) : 0;
        }
        if (k) lba += t->pregap + (t->has_index0 ? t->index1 - t->index0 : 0);
        t->start = lba;
        lba += t->length;
    }
    cd->leadout = lba;
    if (*problem) { cd_free(cd); return NULL; }
    return cd;
}
mc_disk *mc_disk_open(const char *path, int writable, uint32_t sector_size, char *error, size_t capacity) {
    const char *problem = "Cannot open disk image";
    mc_disk *d = calloc(1, sizeof *d);
    if (!d) { if (capacity) snprintf(error, capacity, "Out of memory"); return NULL; }
    d->sector_size = sector_size;
    const int cue = has_suffix(path, ".cue");
    if (sector_size != 512 && sector_size != 2048) { problem = "Sector size must be 512 or 2048"; goto fail; }
    if (writable && sector_size != 512) { problem = "ISO media is read-only"; goto fail; }
    if (writable && cue) { problem = "A CD image is read-only"; goto fail; }
    if (cue) {
        /* The data view is track 1 (mc_disk_read in 2048-byte sectors). */
        d->cd = parse_cue(path, &problem);
        if (!d->cd) { if (!problem) problem = "Cannot open the cue sheet"; goto fail; }
        const cd_track *t = &d->cd->t[0];
        d->file = d->cd->files[t->file];
        if (t->audio) {
            /* An audio CD: the data view spans the disc and reads zeros
             * (the System finds no volume there and asks for the TOC). */
            d->blank = 1;
            d->sectors = (uint64_t)d->cd->leadout * 2048 / sector_size;
            if (!d->sectors) { problem = "The cue sheet's tracks are empty"; goto fail; }
            goto named;
        }
        d->data_offset = t->offset;
        if (t->bytes != 2048) { d->raw = t->bytes; d->raw_skip = t->skip; }
        d->sectors = (uint64_t)t->length * 2048 / sector_size;
        if (!d->sectors || 2048 % sector_size) { problem = "The cue sheet's data track is empty"; goto fail; }
        goto named;
    }
    d->file = fopen(path, writable ? "r+b" : "rb");
    if (!d->file && writable) {
        FILE *probe = fopen(path, "rb");
        if (probe) {
            fclose(probe);
            problem = "Cannot open disk image for writing (read-only file or in use); set readonly: true to use it unwritten";
        }
        goto fail;
    }
    if (!d->file || seek_file(d->file, 0, SEEK_END)) goto fail;
    int64_t length = tell_file(d->file);
    uint8_t header[0x54];
    if (sector_size == 512 && length >= (int64_t)sizeof header && !seek_file(d->file, 0, SEEK_SET) &&
        fread(header, 1, sizeof header, d->file) == sizeof header && diskcopy_header(header, length)) {
        uint32_t size = be32(header + 0x40);
        if (size != 400u * 1024 && size != 720u * 1024 && size != 800u * 1024 && size != 1440u * 1024) { problem = "DiskCopy image must hold 400, 720, 800 or 1440 KiB"; goto fail; }
        d->data = malloc(size);
        if (!d->data) { problem = "Out of memory"; goto fail; }
        if (fread(d->data, 1, size, d->file) != size) { problem = "Cannot read DiskCopy image"; goto fail; }
        if (diskcopy_checksum(d->data, size) != be32(header + 0x48)) { problem = "DiskCopy image checksum mismatch"; goto fail; }
        uint32_t tag_size = be32(header + 0x44);
        if (tag_size == size / 512 * 12) {
            d->tags = malloc(tag_size);
            if (!d->tags) { problem = "Out of memory"; goto fail; }
            if (fread(d->tags, 1, tag_size, d->file) != tag_size) { problem = "Cannot read DiskCopy image"; goto fail; }
            d->tag_size = tag_size;
        }
        d->data_offset = 0x54; length = size;
    }
    if (!d->data && length >= (int64_t)RAW_SECTOR && (uint64_t)length % RAW_SECTOR == 0) {
        uint8_t head[16];
        if (!seek_file(d->file, 0, SEEK_SET) && fread(head, 1, sizeof head, d->file) == sizeof head && raw_sector_skip(head)) {
            if (writable) { problem = "A CD image is read-only"; goto fail; }
            d->raw = RAW_SECTOR;
            d->raw_skip = (uint32_t)raw_sector_skip(head);
            length = length / RAW_SECTOR * 2048;
        }
    }
    if (length <= 0 || (uint64_t)length % sector_size) { problem = "Image length must be a positive whole number of sectors"; goto fail; }
    d->sectors = (uint64_t)length / sector_size;
    d->writable = writable != 0;
named:;
    const char *label=path;
    for (const char *p=path;*p;++p) if (*p=='/' || *p=='\\')label=p+1;
    size_t label_size=strlen(label);if (label_size>=sizeof d->label)label_size=sizeof d->label-1;
    memcpy(d->label,label,label_size);
    return d;
fail:
    if (capacity) snprintf(error, capacity, "%s", problem);
    mc_disk_close(d); return NULL;
}
const char *mc_disk_cue_sheet(const char *path, char *buffer, size_t capacity) {
    const size_t n = path ? strlen(path) : 0;
    if (n < 5 || n + 1 > capacity || !has_suffix(path, ".bin")) return path;
    static const char *const suffixes[] = {".cue", ".CUE", ".Cue"};
    for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; ++i) {
        memcpy(buffer, path, n - 4);
        memcpy(buffer + n - 4, suffixes[i], 5);
        FILE *f = fopen(buffer, "rb");
        if (f) { fclose(f); return buffer; }
    }
    return path;
}
int mc_disk_same_file(const mc_disk *d, const char *path) {
    if (!d || !path) return 0;
#ifdef _WIN32
    HANDLE other = CreateFileA(path, 0, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (other == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION x, y;
    int same = GetFileInformationByHandle((HANDLE)_get_osfhandle(_fileno(d->file)), &x) && GetFileInformationByHandle(other, &y) &&
        x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
    CloseHandle(other); return same;
#else
    struct stat x, y;
    return !fstat(fileno(d->file), &x) && !stat(path, &y) && x.st_dev == y.st_dev && x.st_ino == y.st_ino;
#endif
}
uint64_t mc_disk_sectors(const mc_disk *d) { return d ? d->sectors : 0; }
uint32_t mc_disk_sector_size(const mc_disk *d) { return d ? d->sector_size : 0; }
static int byte_bounds(const mc_disk *d, uint64_t offset, uint64_t count, const void *buffer) {
    if (!d || !buffer) return 0;
    uint64_t length=d->sectors*d->sector_size;
    return offset<=length && count<=length-offset && count<=SIZE_MAX && offset<=INT64_MAX-d->data_offset;
}
static int read_at(mc_disk *d, uint64_t offset, uint64_t count, void *buffer) {
    if (!byte_bounds(d,offset,count,buffer)) return 0;
    if (d->blank) { memset(buffer, 0, (size_t)count); return 1; }
    if (d->raw) {
        uint8_t *out = buffer;
        while (count) {
            const uint64_t sector = offset / 2048, within = offset % 2048;
            const size_t n = (size_t)(count < 2048 - within ? count : 2048 - within);
            if (seek_file(d->file, (int64_t)(d->data_offset + sector * d->raw + d->raw_skip + within), SEEK_SET) || fread(out, 1, n, d->file) != n) return 0;
            out += n; offset += n; count -= n;
        }
        return 1;
    }
    return !seek_file(d->file,(int64_t)(d->data_offset+offset),SEEK_SET) && fread(buffer,1,(size_t)count,d->file)==count;
}
/* In-place write, made durable later (mc_disk_sync); a DiskCopy image's
 * data checksum is rewritten with it, its tags are left as they are. */
static int write_at(mc_disk *d, uint64_t offset, uint64_t count, const void *buffer) {
    if (!byte_bounds(d,offset,count,buffer) || !d->writable || d->failed) return 0;
    if (seek_file(d->file,(int64_t)(d->data_offset+offset),SEEK_SET) || fwrite(buffer,1,(size_t)count,d->file)!=count) { d->failed = 1; return 0; }
    if (d->data) {
        memcpy(d->data+offset,buffer,(size_t)count);
        uint32_t sum = diskcopy_checksum(d->data, d->sectors*d->sector_size);
        uint8_t field[4] = {(uint8_t)(sum>>24),(uint8_t)(sum>>16),(uint8_t)(sum>>8),(uint8_t)sum};
        if (seek_file(d->file,0x48,SEEK_SET) || fwrite(field,1,4,d->file)!=4) { d->failed = 1; return 0; }
    }
    if (!written(d)) { d->failed = 1; return 0; }
    return 1;
}
int mc_disk_read(mc_disk *d, uint64_t sector, uint32_t count, void *buffer) {
    if (!d || sector > d->sectors) return 0;
    return read_at(d, sector * d->sector_size, (uint64_t)count * d->sector_size, buffer);
}
int mc_disk_write(mc_disk *d, uint64_t sector, uint32_t count, const void *buffer) {
    if (!d || sector > d->sectors) return 0;
    return write_at(d, sector * d->sector_size, (uint64_t)count * d->sector_size, buffer);
}
int mc_disk_read_bytes(mc_disk *d, uint64_t offset, uint32_t count, void *buffer) { return read_at(d, offset, count, buffer); }
int mc_disk_write_bytes(mc_disk *d, uint64_t offset, uint32_t count, const void *buffer) { return write_at(d, offset, count, buffer); }
int mc_disk_floppy(const mc_disk *d) {
    uint64_t n = mc_disk_sectors(d);
    return d && d->sector_size == 512 && (n == 800 || n == 1440 || n == 1600 || n == 2880);
}
int mc_disk_has_tags(const mc_disk *d) { return d && d->tags != NULL; }
int mc_disk_read_tags(mc_disk *d, uint64_t sector, uint32_t count, void *buffer) {
    if (!d || !buffer || sector > d->sectors || count > d->sectors - sector) return 0;
    if (d->tags) memcpy(buffer, d->tags + sector * 12, (size_t)count * 12);
    else memset(buffer, 0, (size_t)count * 12);
    return 1;
}
/* The tag checksum leaves out the first sector's 12 tag bytes (the Disk Copy
 * 4.2 format as publicly documented). */
int mc_disk_write_tags(mc_disk *d, uint64_t sector, uint32_t count, const void *buffer) {
    if (!d || !buffer || sector > d->sectors || count > d->sectors - sector) return 0;
    if (!d->tags) return 1;
    if (!d->writable || d->failed) return 0;
    uint64_t at = 0x54 + d->sectors * d->sector_size + sector * 12;
    if (seek_file(d->file, (int64_t)at, SEEK_SET) || fwrite(buffer, 1, (size_t)count * 12, d->file) != (size_t)count * 12) { d->failed = 1; return 0; }
    memcpy(d->tags + sector * 12, buffer, (size_t)count * 12);
    uint32_t sum = d->tag_size > 12 ? diskcopy_checksum(d->tags + 12, d->tag_size - 12) : 0;
    uint8_t field[4] = {(uint8_t)(sum>>24),(uint8_t)(sum>>16),(uint8_t)(sum>>8),(uint8_t)sum};
    if (seek_file(d->file,0x4C,SEEK_SET) || fwrite(field,1,4,d->file)!=4 || !written(d)) { d->failed = 1; return 0; }
    return 1;
}

/* --- CD tracks and audio (disk.h) --------------------------------------------------------- */

void mc_disk_set_locked(mc_disk *d, int locked) { if (d) d->locked = locked != 0; }
int mc_disk_locked(const mc_disk *d) { return d && d->locked; }

unsigned mc_disk_tracks(const mc_disk *d) {
    if (!d) return 0;
    return d->cd ? d->cd->ntracks : d->sector_size == 2048 ? 1u : 0u;
}
int mc_disk_track_get(const mc_disk *d, unsigned i, mc_disk_track *out) {
    if (!d || i >= mc_disk_tracks(d)) return 0;
    if (!d->cd) {
        *out = (mc_disk_track){1, 0, 0, (uint32_t)(d->sectors * d->sector_size / 2048)};
        return 1;
    }
    const cd_track *t = &d->cd->t[i];
    *out = (mc_disk_track){t->number, t->audio, t->start, t->length};
    return 1;
}
uint32_t mc_disk_leadout(const mc_disk *d) {
    if (!d) return 0;
    return d->cd ? d->cd->leadout : (uint32_t)(d->sectors * d->sector_size / 2048);
}
int mc_disk_track_at(const mc_disk *d, uint32_t lba, mc_disk_track *out) {
    const unsigned n = mc_disk_tracks(d);
    if (!n || lba >= mc_disk_leadout(d)) return 0;
    for (unsigned i = n; i-- > 0;) {
        mc_disk_track t, before;
        mc_disk_track_get(d, i, &t);
        const uint32_t from = i && mc_disk_track_get(d, i - 1, &before) ? before.start + before.length : 0;
        if (lba >= from) { *out = t; return 1; }
    }
    return 0;
}
/* One frame of audio (588 stereo samples): zeros outside the audio tracks. */
static const int16_t *audio_frame(struct mc_cd *cd, uint32_t lba) {
    if (cd->cached == lba) return cd->cache;
    memset(cd->cache, 0, sizeof cd->cache);
    cd->cached = lba;
    for (unsigned i = 0; i < cd->ntracks; ++i) {
        const cd_track *t = &cd->t[i];
        if (!t->audio || lba < t->start || lba >= t->start + t->length) continue;
        uint8_t b[RAW_SECTOR];
        FILE *f = cd->files[t->file];
        if (!seek_file(f, (int64_t)(t->offset + (uint64_t)(lba - t->start) * RAW_SECTOR), SEEK_SET) && fread(b, 1, RAW_SECTOR, f) == RAW_SECTOR)
            for (unsigned k = 0; k < CD_FRAME_SAMPLES * 2; ++k) cd->cache[k] = (int16_t)(b[2 * k] | b[2 * k + 1] << 8);
        break;
    }
    return cd->cache;
}
int mc_disk_read_audio(mc_disk *d, uint32_t lba, int16_t *stereo) {
    if (!d || !d->cd || lba >= d->cd->leadout) return 0;
    memcpy(stereo, audio_frame(d->cd, lba), sizeof d->cd->cache);
    return 1;
}
void mc_disk_set_clock(mc_disk *d, uint64_t (*clock)(void *ctx), void *ctx) {
    if (!d || !d->cd) return;
    d->cd->clock = clock;
    d->cd->clock_ctx = ctx;
}
static uint64_t cd_now(const struct mc_cd *cd) { return cd->clock ? cd->clock(cd->clock_ctx) : 0; }
/* MMC-1 revision 10a 5.1.14: SCAN remains audio play, stopping at data
 * or the end of the audio area. Livadix's virtual drive scans eight sectors
 * per audible sector, keeping each snippet at its original pitch. */
static uint64_t cd_duration(const struct mc_cd *cd) {
    if (!cd->scan_step) return (uint64_t)(cd->play_end - cd->play_start) * CD_FRAME_SAMPLES;
    const uint32_t distance = cd->scan_step > 0 ? cd->play_end - cd->scan_start : cd->scan_start - cd->play_start + 1;
    return ((uint64_t)distance + 7) / 8 * CD_FRAME_SAMPLES;
}
/* Absolute sample address at an elapsed output sample. */
static uint64_t cd_position(const struct mc_cd *cd, uint64_t elapsed) {
    if (!cd->scan_step) return (uint64_t)cd->play_start * CD_FRAME_SAMPLES + elapsed;
    if (elapsed >= cd_duration(cd))
        return (uint64_t)(cd->scan_step > 0 ? cd->play_end : cd->play_start) * CD_FRAME_SAMPLES;
    const int64_t frame = cd->scan_start + (int64_t)cd->scan_step * (int64_t)(elapsed / CD_FRAME_SAMPLES);
    return (uint64_t)frame * CD_FRAME_SAMPLES + elapsed % CD_FRAME_SAMPLES;
}
/* Elapsed output samples, and the end reached. */
static uint64_t cd_played(struct mc_cd *cd) {
    const uint64_t total = cd_duration(cd);
    if (cd->state == MC_CD_PAUSED) return cd->paused;
    if (cd->state != MC_CD_PLAYING) return cd->state == MC_CD_COMPLETED ? total : 0;
    const uint64_t now = cd_now(cd), played = now > cd->t0 ? now - cd->t0 : 0;
    if (played >= total) {
        cd->state = MC_CD_COMPLETED;
        return total;
    }
    return played;
}
int mc_disk_play(mc_disk *d, uint32_t start, uint32_t end) {
    if (!d || !d->cd || start >= end || end > d->cd->leadout) return 0;
    mc_disk_track t;
    if (!mc_disk_track_at(d, start, &t) || !t.audio) return 0;
    struct mc_cd *cd = d->cd;
    cd->play_start = start;
    cd->play_end = end;
    cd->scan_step = 0;
    cd->t0 = cd_now(cd);
    cd->cursor = -1;
    cd->state = MC_CD_PLAYING;
    return 1;
}
int mc_disk_scan(mc_disk *d, uint32_t start, int reverse) {
    if (!d || !d->cd || start >= d->cd->leadout) return 0;
    struct mc_cd *cd = d->cd;
    unsigned i = cd->ntracks;
    while (i && start < (i > 1 ? cd->t[i - 2].start + cd->t[i - 2].length : 0)) --i;
    if (!i || !cd->t[i - 1].audio) return 0;
    unsigned first = i - 1, last = i - 1;
    while (first && cd->t[first - 1].audio) --first;
    while (last + 1 < cd->ntracks && cd->t[last + 1].audio) ++last;
    cd->play_start = first ? cd->t[first - 1].start + cd->t[first - 1].length : 0;
    cd->play_end = cd->t[last].start + cd->t[last].length;
    cd->scan_start = start;
    cd->scan_step = reverse ? -8 : 8;
    cd->t0 = cd_now(cd);
    cd->cursor = -1;
    cd->state = MC_CD_PLAYING;
    return 1;
}
/* PAUSE/RESUME and STOP PLAY/SCAN leave scan mode at the current address
 * (MMC-1 5.1.14 and 5.1.16). A resume plays normally from this position. */
static void cd_normal_at(struct mc_cd *cd, uint64_t elapsed) {
    const uint64_t position = cd_position(cd, elapsed);
    cd->play_start = (uint32_t)(position / CD_FRAME_SAMPLES);
    cd->paused = position % CD_FRAME_SAMPLES;
    cd->scan_step = 0;
    cd->t0 = cd_now(cd) - cd->paused;
    cd->cursor = -1;
}
void mc_disk_pause(mc_disk *d, int pause) {
    if (!d || !d->cd) return;
    struct mc_cd *cd = d->cd;
    if (pause && cd->state == MC_CD_PLAYING) {
        const uint64_t elapsed = cd_played(cd);
        if (cd->state == MC_CD_PLAYING) {
            if (cd->scan_step) cd_normal_at(cd, elapsed);
            else cd->paused = elapsed;
            cd->state = MC_CD_PAUSED;
        }
    } else if (!pause && cd->state == MC_CD_PAUSED) {
        cd->t0 = cd_now(cd) - cd->paused;
        cd->cursor = -1;
        cd->state = MC_CD_PLAYING;
    }
}
int mc_disk_stop_scan(mc_disk *d) {
    if (!d || !d->cd || !d->cd->scan_step || d->cd->state != MC_CD_PLAYING) return 0;
    struct mc_cd *cd = d->cd;
    const uint64_t elapsed = cd_played(cd);
    if (cd->state != MC_CD_PLAYING) return 0;
    cd_normal_at(cd, elapsed);
    return 1;
}
void mc_disk_stop(mc_disk *d) {
    if (!d || !d->cd) return;
    struct mc_cd *cd = d->cd;
    if (cd->state != MC_CD_IDLE) cd->stopped = (uint32_t)(cd_position(cd, cd_played(cd)) / CD_FRAME_SAMPLES);
    cd->scan_step = 0;
    cd->state = MC_CD_IDLE;
}
void mc_disk_seek(mc_disk *d, uint32_t lba, int hold) {
    if (!d || !d->cd) return;
    struct mc_cd *cd = d->cd;
    mc_disk_track t;
    const int state = mc_disk_play_state(d, NULL);
    cd->scan_step = 0;
    if (hold && (state == MC_CD_PLAYING || state == MC_CD_PAUSED) && mc_disk_track_at(d, lba, &t) && t.audio) {
        if (cd->play_end <= lba) cd->play_end = cd->leadout;
        cd->play_start = lba;
        cd->paused = 0;
        cd->cursor = -1;
        cd->state = MC_CD_PAUSED;
        return;
    }
    cd->state = MC_CD_IDLE;
    cd->stopped = lba < cd->leadout ? lba : cd->leadout;
}
int mc_disk_play_state(mc_disk *d, uint32_t *lba) {
    if (!d || !d->cd) { if (lba) *lba = 0; return MC_CD_IDLE; }
    struct mc_cd *cd = d->cd;
    const uint64_t played = cd_played(cd);
    if (lba) *lba = cd->state == MC_CD_IDLE ? cd->stopped : (uint32_t)(cd_position(cd, played) / CD_FRAME_SAMPLES);
    return cd->state;
}
void mc_disk_set_volume(mc_disk *d, uint8_t left, uint8_t right) {
    if (!d || !d->cd) return;
    d->cd->volume[0] = left;
    d->cd->volume[1] = right;
}
static int16_t clip(int32_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
size_t mc_disk_mix(mc_disk *d, int16_t *stereo, size_t have, size_t frames, uint32_t rate) {
    if (!d || !d->cd || !rate || d->cd->state != MC_CD_PLAYING) return have;
    struct mc_cd *cd = d->cd;
    const uint64_t played = cd_played(cd);
    const uint64_t total = cd_duration(cd);
    /* The mixer follows the guest's clock: it starts there, and goes back
     * to it when the host's pulls fall half a second away from it. */
    if (cd->cursor < 0 || cd->cursor > (double)played + 22050 || cd->cursor + 22050 < (double)played) cd->cursor = (double)played;
    const double step = 44100.0 / rate;
    size_t want = played > cd->cursor ? (size_t)(((double)played - cd->cursor) / step) : 0;
    size_t out = have > want ? have : want;
    if (out > frames) out = frames;
    for (size_t i = have; i < out; ++i) stereo[2 * i] = stereo[2 * i + 1] = 0;
    for (size_t i = 0; i < out && cd->cursor < (double)total; ++i) {
        const uint64_t pos = (uint64_t)cd->cursor;
        const double frac = cd->cursor - (double)pos;
        for (unsigned c = 0; c < 2; ++c) {
            const uint64_t a = cd_position(cd, pos);
            /* Do not interpolate across a skipped stretch at a scan jump. */
            const uint64_t b = pos + 1 < total && (!cd->scan_step || pos % CD_FRAME_SAMPLES != CD_FRAME_SAMPLES - 1) ? cd_position(cd, pos + 1) : a;
            const int16_t sa = audio_frame(cd, (uint32_t)(a / CD_FRAME_SAMPLES))[2 * (a % CD_FRAME_SAMPLES) + c];
            const int16_t sb = audio_frame(cd, (uint32_t)(b / CD_FRAME_SAMPLES))[2 * (b % CD_FRAME_SAMPLES) + c];
            const double v = (sa + (sb - sa) * frac) * cd->volume[c] / 255.0;
            stereo[2 * i + c] = clip(stereo[2 * i + c] + (int32_t)v);
        }
        cd->cursor += step;
    }
    return out;
}
