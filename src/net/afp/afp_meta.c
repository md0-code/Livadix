/* Livadix — Macintosh metadata beside Windows files (afp_meta.h).
 *
 * Sources: Microsoft [MS-FSCC] 5.6.6 "Macintosh Supported Stream Names"
 * (AFP_AfpInfo, AFP_Resource); the AFP_AfpInfo layout as Services for
 * Macintosh wrote it (signature, version, ID, backup time, Finder info,
 * ProDOS info, attributes, access bytes); RFC 1740 (AppleSingle and
 * AppleDouble: the header, the entry table, entry IDs 2, 8, 9, 10, 11, the
 * date base of 2000 and $80000000 for unknown dates, ASMacInfo's locked
 * bit); Inside AppleTalk, 2nd ed., chapter 13 (the backup date of a new
 * CNode is $80000000).
 *
 * Livadix's own code. */
#include "afp_meta.h"

#include <stdlib.h>
#include <string.h>

#define AD_HEADER 26
#define AD_ENTRIES 5
#define AD_FINDER (AD_HEADER + AD_ENTRIES * 12)    /* 86 */
#define AD_DATES (AD_FINDER + 32)                   /* 118 */
#define AD_MACINFO (AD_DATES + 16)                  /* 134 */
#define AD_PRODOS (AD_MACINFO + 4)                  /* 138 */
#define AD_RSRC (AD_PRODOS + 8)                     /* 146 */
#define AFPINFO 60

static const uint16_t stream_info[] = {':', 'A', 'F', 'P', '_', 'A', 'f', 'p', 'I', 'n', 'f', 'o', 0};
static const uint16_t stream_rsrc[] = {':', 'A', 'F', 'P', '_', 'R', 'e', 's', 'o', 'u', 'r', 'c', 'e', 0};

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static void le32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t gle32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static int with_suffix(const uint16_t *path, const uint16_t *suffix, uint16_t *out, size_t capacity) {
    const size_t a = mc_host_len(path), b = mc_host_len(suffix);
    if (a + b + 1 > capacity) return 0;
    memcpy(out, path, a * 2);
    memcpy(out + a, suffix, (b + 1) * 2);
    return 1;
}

int mc_meta_sidecar(const uint16_t *path, uint16_t *out, size_t capacity) {
    const size_t n = mc_host_len(path);
    size_t slash = n;
    while (slash && path[slash - 1] != '\\') slash--;
    if (n + 3 > capacity) return 0;
    memcpy(out, path, slash * 2);
    out[slash] = '.';
    out[slash + 1] = '_';
    memcpy(out + slash + 2, path + slash, (n - slash + 1) * 2);
    return 1;
}

int mc_meta_is_sidecar(const uint16_t *name) { return name[0] == '.' && name[1] == '_'; }

static void defaults(mc_afp_meta *m) {
    memset(m, 0, sizeof *m);
    m->backup = (int32_t)0x80000000u;
    m->created = m->modified = (int32_t)0x80000000u;
}

/* ---- AppleDouble ----------------------------------------------------------- */

typedef struct ad_entry {
    uint32_t offset, length;
    int present;
} ad_entry;

typedef struct ad_file {
    ad_entry e[16];            /* by entry ID (1..15) */
    unsigned count;
    int ours;                  /* our layout: rewritable in place */
} ad_file;

static int ad_parse(mc_host_file *f, ad_file *ad) {
    memset(ad, 0, sizeof *ad);
    uint8_t h[AD_HEADER + 12 * 32];
    size_t got = 0;
    if (mc_host_read(f, 0, h, sizeof h, &got) || got < AD_HEADER) return 0;
    if (get32(h) != 0x00051607 || (get32(h + 4) != 0x00020000 && get32(h + 4) != 0x00010000)) return 0;
    const unsigned n = get16(h + 24);
    if (AD_HEADER + 12 * n > got) return 0;
    ad->count = n;
    uint32_t last_end = 0, rsrc_off = 0;
    for (unsigned k = 0; k < n; k++) {
        const uint8_t *e = h + AD_HEADER + 12 * k;
        const uint32_t id = get32(e), off = get32(e + 4), len = get32(e + 8);
        if (id < 16) {
            ad->e[id].offset = off;
            ad->e[id].length = len;
            ad->e[id].present = 1;
        }
        if (id != 2 && off + len > last_end) last_end = off + len;
        if (id == 2) rsrc_off = off;
    }
    ad->ours = n == AD_ENTRIES && ad->e[9].present && ad->e[9].offset == AD_FINDER && ad->e[9].length == 32 && ad->e[8].present &&
               ad->e[8].offset == AD_DATES && ad->e[10].present && ad->e[10].offset == AD_MACINFO && ad->e[11].present &&
               ad->e[11].offset == AD_PRODOS && ad->e[2].present && rsrc_off == AD_RSRC && last_end <= AD_RSRC;
    return 1;
}

static void ad_header(uint8_t *h, uint32_t rsrc_length) {
    memset(h, 0, AD_RSRC);
    put32(h, 0x00051607);
    put32(h + 4, 0x00020000);
    put16(h + 24, AD_ENTRIES);
    static const uint32_t ids[AD_ENTRIES] = {9, 8, 10, 11, 2};
    static const uint32_t offs[AD_ENTRIES] = {AD_FINDER, AD_DATES, AD_MACINFO, AD_PRODOS, AD_RSRC};
    static const uint32_t lens[AD_ENTRIES] = {32, 16, 4, 8, 0};
    for (unsigned k = 0; k < AD_ENTRIES; k++) {
        uint8_t *e = h + AD_HEADER + 12 * k;
        put32(e, ids[k]);
        put32(e + 4, offs[k]);
        put32(e + 8, k == 4 ? rsrc_length : lens[k]);
    }
}

static void ad_fill(uint8_t *h, const mc_afp_meta *m) {
    memcpy(h + AD_FINDER, m->finder, 32);
    put32(h + AD_DATES, (uint32_t)m->created);
    put32(h + AD_DATES + 4, (uint32_t)m->modified);
    put32(h + AD_DATES + 8, (uint32_t)m->backup);
    put32(h + AD_DATES + 12, 0x80000000u);
    /* ASMacInfo: the locked bit stands for the AFP write inhibit */
    h[AD_MACINFO + 3] = (m->attributes & 0x0020) ? 1 : 0;
    /* ASProdosInfo: access, file type, aux type (ProDOS info: type, 0, aux low, aux high, 0, 0) */
    put16(h + AD_PRODOS, 0);
    put16(h + AD_PRODOS + 2, m->prodos[0]);
    put32(h + AD_PRODOS + 4, (uint32_t)m->prodos[3] << 8 | m->prodos[2]);
}

/* Rewrites a sidecar in our layout, keeping its resource fork. */
static int ad_normalise(mc_host_file *f, const ad_file *ad, const mc_afp_meta *m) {
    uint64_t rlen = ad->e[2].present ? ad->e[2].length : 0;
    uint8_t *rsrc = NULL;
    if (rlen) {
        rsrc = malloc((size_t)rlen);
        size_t got = 0;
        if (!rsrc || mc_host_read(f, ad->e[2].offset, rsrc, (size_t)rlen, &got) || got != rlen) {
            free(rsrc);
            return MC_AFP_MISC_ERR;
        }
    }
    uint8_t h[AD_RSRC];
    ad_header(h, (uint32_t)rlen);
    ad_fill(h, m);
    int e = mc_host_write(f, 0, h, AD_RSRC);
    if (!e && rlen) e = mc_host_write(f, AD_RSRC, rsrc, (size_t)rlen);
    if (!e) e = mc_host_truncate(f, AD_RSRC + rlen);
    free(rsrc);
    return e;
}

static int ad_read(const uint16_t *path, mc_afp_meta *m) {
    uint16_t side[MC_HOST_PATH];
    mc_host_file *f;
    if (!mc_meta_sidecar(path, side, MC_HOST_PATH) || mc_host_open(side, 0, MC_HOST_OPEN, &f)) return 0;
    ad_file ad;
    int found = 0;
    if (ad_parse(f, &ad)) {
        uint8_t b[32];
        size_t got;
        if (ad.e[9].present && ad.e[9].length >= 32 && !mc_host_read(f, ad.e[9].offset, b, 32, &got) && got == 32) {
            memcpy(m->finder, b, 32);
            found = 1;
        }
        if (ad.e[8].present && ad.e[8].length >= 16 && !mc_host_read(f, ad.e[8].offset, b, 16, &got) && got == 16) {
            m->created = (int32_t)get32(b);
            m->modified = (int32_t)get32(b + 4);
            m->backup = (int32_t)get32(b + 8);
        }
        if (ad.e[10].present && ad.e[10].length >= 4 && !mc_host_read(f, ad.e[10].offset, b, 4, &got) && got == 4 && (b[3] & 1))
            m->attributes |= 0x0020 | 0x0080 | 0x0100;
        if (ad.e[11].present && ad.e[11].length >= 8 && !mc_host_read(f, ad.e[11].offset, b, 8, &got) && got == 8) {
            m->prodos[0] = b[3];
            m->prodos[2] = b[7];
            m->prodos[3] = b[6];
        }
    }
    mc_host_close(f);
    return found;
}

static int open_sidecar(const uint16_t *path, mc_host_file **f) {
    uint16_t side[MC_HOST_PATH];
    if (!mc_meta_sidecar(path, side, MC_HOST_PATH)) return MC_AFP_PARAM_ERR;
    int e = mc_host_open(side, 1, MC_HOST_OPEN_ALWAYS, f);
    if (e == MC_AFP_OK) mc_host_set_attribute(side, MC_HOST_HIDDEN, 1);
    return e;
}

static int ad_write(const uint16_t *path, const mc_afp_meta *m) {
    mc_host_file *f;
    int e = open_sidecar(path, &f);
    if (e) return e;
    ad_file ad;
    if (!ad_parse(f, &ad) || !ad.ours) e = ad_normalise(f, &ad, m);
    else {
        uint8_t h[AD_RSRC];
        size_t got;
        e = mc_host_read(f, 0, h, AD_RSRC, &got);
        if (!e) {
            ad_fill(h, m);
            e = mc_host_write(f, AD_FINDER, h + AD_FINDER, AD_RSRC - AD_FINDER);
        }
    }
    mc_host_close(f);
    return e;
}

/* ---- streams ---------------------------------------------------------------- */

static int info_read(const uint16_t *path, mc_afp_meta *m) {
    uint16_t s[MC_HOST_PATH];
    mc_host_file *f;
    if (!with_suffix(path, stream_info, s, MC_HOST_PATH) || mc_host_open(s, 0, MC_HOST_OPEN, &f)) return 0;
    uint8_t b[AFPINFO];
    size_t got = 0;
    const int e = mc_host_read(f, 0, b, AFPINFO, &got);
    mc_host_close(f);
    if (e || got < 54 || memcmp(b, "AFP", 4) || gle32(b + 4) != 0x00010000) return 0;
    m->id = gle32(b + 8);
    m->backup = (int32_t)gle32(b + 12);
    memcpy(m->finder, b + 16, 32);
    memcpy(m->prodos, b + 48, 6);
    if (got >= 56) m->attributes = (uint16_t)(b[54] | b[55] << 8);
    return 1;
}

static int info_write(const uint16_t *path, const mc_afp_meta *m) {
    uint16_t s[MC_HOST_PATH];
    if (!with_suffix(path, stream_info, s, MC_HOST_PATH)) return MC_AFP_PARAM_ERR;
    uint8_t b[AFPINFO];
    memset(b, 0, sizeof b);
    memcpy(b, "AFP", 4);
    le32(b + 4, 0x00010000);
    le32(b + 8, m->id);
    le32(b + 12, (uint32_t)m->backup);
    memcpy(b + 16, m->finder, 32);
    memcpy(b + 48, m->prodos, 6);
    b[54] = (uint8_t)m->attributes;
    b[55] = (uint8_t)(m->attributes >> 8);
    b[56] = b[57] = b[58] = 7;                  /* owner, group, world: search, read, write */
    mc_host_file *f;
    int e = mc_host_open(s, 1, MC_HOST_OPEN_ALWAYS, &f);
    if (e) return e;
    e = mc_host_write(f, 0, b, AFPINFO);
    if (!e) e = mc_host_truncate(f, AFPINFO);
    mc_host_close(f);
    return e;
}

/* A write to a read-only file's metadata: the Finder moves a locked file's
 * icon too, so the read-only attribute steps aside for it. */
static int unlocked(int (*write)(const uint16_t *, const mc_afp_meta *), const uint16_t *path, const mc_afp_meta *m) {
    int e = write(path, m);
    if (e != MC_AFP_ACCESS_DENIED) return e;
    mc_host_stat st;
    if (mc_host_stat_path(path, &st) || !(st.attributes & MC_HOST_READONLY)) return e;
    mc_host_set_attribute(path, MC_HOST_READONLY, 0);
    e = write(path, m);
    mc_host_set_attribute(path, MC_HOST_READONLY, 1);
    return e;
}

int mc_meta_read(int mode, const uint16_t *path, mc_afp_meta *m) {
    defaults(m);
    return mode == MC_META_APPLEDOUBLE ? ad_read(path, m) : info_read(path, m);
}

int mc_meta_write(int mode, const uint16_t *path, const mc_afp_meta *m) {
    return unlocked(mode == MC_META_APPLEDOUBLE ? ad_write : info_write, path, m);
}

/* ---- the resource fork ------------------------------------------------------- */

/* The kinds of open fork: a stream, our AppleDouble layout (writable), an
 * empty fork (no stream or sidecar), another program's AppleDouble layout
 * (read where it stands). */
enum { R_STREAM, R_OURS, R_EMPTY, R_FOREIGN };

struct mc_rsrc {
    mc_host_file *f;
    int kind;
    uint64_t base, length;     /* AppleDouble: where the entry is, its length */
};

static int open_empty(const uint16_t *path, mc_rsrc *r) {
    r->kind = R_EMPTY;
    return mc_host_open(path, 0, MC_HOST_OPEN, &r->f);
}

int mc_rsrc_open(int mode, const uint16_t *path, int write, mc_rsrc **out) {
    mc_rsrc *r = calloc(1, sizeof *r);
    if (!r) return MC_AFP_MISC_ERR;
    int e;
    if (mode == MC_META_STREAMS) {
        uint16_t s[MC_HOST_PATH];
        r->kind = R_STREAM;
        if (!with_suffix(path, stream_rsrc, s, MC_HOST_PATH)) e = MC_AFP_PARAM_ERR;
        else {
            e = mc_host_open(s, write, write ? MC_HOST_OPEN_ALWAYS : MC_HOST_OPEN, &r->f);
            if (e == MC_AFP_OBJECT_NOT_FOUND && !write) e = open_empty(path, r);
        }
    } else if (write) {
        r->kind = R_OURS;
        r->base = AD_RSRC;
        e = open_sidecar(path, &r->f);
        if (!e) {
            ad_file ad;
            const int parsed = ad_parse(r->f, &ad);
            if (parsed && ad.ours) r->length = ad.e[2].length;
            else {
                mc_afp_meta m;
                defaults(&m);
                if (parsed) ad_read(path, &m);
                else memset(&ad, 0, sizeof ad);
                e = ad_normalise(r->f, &ad, &m);
                r->length = parsed && ad.e[2].present ? ad.e[2].length : 0;
            }
        }
    } else {
        uint16_t side[MC_HOST_PATH];
        e = mc_meta_sidecar(path, side, MC_HOST_PATH) ? mc_host_open(side, 0, MC_HOST_OPEN, &r->f) : MC_AFP_PARAM_ERR;
        if (e == MC_AFP_OBJECT_NOT_FOUND) e = open_empty(path, r);
        else if (!e) {
            ad_file ad;
            if (ad_parse(r->f, &ad) && ad.e[2].present) {
                r->kind = ad.ours ? R_OURS : R_FOREIGN;
                r->base = ad.e[2].offset;
                r->length = ad.e[2].length;
            } else {
                mc_host_close(r->f);
                r->f = NULL;
                e = open_empty(path, r);
            }
        }
    }
    if (e) {
        mc_host_close(r->f);
        free(r);
        return e;
    }
    *out = r;
    return MC_AFP_OK;
}

int mc_rsrc_read(mc_rsrc *r, uint64_t offset, void *buffer, size_t length, size_t *got) {
    *got = 0;
    if (r->kind == R_STREAM) return mc_host_read(r->f, offset, buffer, length, got);
    if (r->kind == R_EMPTY || offset >= r->length) return MC_AFP_OK;
    if (length > r->length - offset) length = (size_t)(r->length - offset);
    return mc_host_read(r->f, r->base + offset, buffer, length, got);
}

static int set_length(mc_rsrc *r, uint64_t length) {
    uint8_t b[4];
    put32(b, (uint32_t)length);
    const int e = mc_host_write(r->f, AD_HEADER + 4 * 12 + 8, b, 4);
    if (!e) r->length = length;
    return e;
}

int mc_rsrc_write(mc_rsrc *r, uint64_t offset, const void *buffer, size_t length) {
    if (r->kind == R_STREAM) return mc_host_write(r->f, offset, buffer, length);
    if (r->kind != R_OURS) return MC_AFP_ACCESS_DENIED;
    int e = mc_host_write(r->f, AD_RSRC + offset, buffer, length);
    if (!e && offset + length > r->length) e = set_length(r, offset + length);
    return e;
}

int mc_rsrc_size(mc_rsrc *r, uint64_t *size) {
    if (r->kind == R_STREAM) return mc_host_size(r->f, size);
    *size = r->kind == R_EMPTY ? 0 : r->length;
    return MC_AFP_OK;
}

int mc_rsrc_truncate(mc_rsrc *r, uint64_t size) {
    if (r->kind == R_STREAM) return mc_host_truncate(r->f, size);
    if (r->kind != R_OURS) return MC_AFP_ACCESS_DENIED;
    int e = mc_host_truncate(r->f, AD_RSRC + size);
    if (!e) e = set_length(r, size);
    return e;
}

int mc_rsrc_flush(mc_rsrc *r) { return r->kind == R_STREAM || r->kind == R_OURS ? mc_host_flush(r->f) : MC_AFP_OK; }

void mc_rsrc_close(mc_rsrc *r) {
    if (!r) return;
    mc_host_close(r->f);
    free(r);
}

int mc_rsrc_length(int mode, const uint16_t *path, uint64_t *size) {
    *size = 0;
    if (mode == MC_META_STREAMS) {
        uint16_t s[MC_HOST_PATH];
        return with_suffix(path, stream_rsrc, s, MC_HOST_PATH) ? mc_host_stream_size(s, size) : MC_AFP_PARAM_ERR;
    }
    uint16_t side[MC_HOST_PATH];
    mc_host_file *f;
    if (!mc_meta_sidecar(path, side, MC_HOST_PATH) || mc_host_open(side, 0, MC_HOST_OPEN, &f)) return MC_AFP_OK;
    ad_file ad;
    if (ad_parse(f, &ad) && ad.e[2].present) *size = ad.e[2].length;
    mc_host_close(f);
    return MC_AFP_OK;
}
