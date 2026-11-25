/* Livadix — the AFP 2.1 server (afp.h).
 *
 * Sources: Inside AppleTalk, 2nd ed. (Apple, 1990), chapter 13: "File
 * system structure" (IDs, names, dates from 2000, attributes, ProDOS
 * info), "Designating a path to a CNode" (null separators, ascending
 * runs), "AFP login" and "File server security" (UAMs: No User Authent,
 * Cleartxt Passwrd, Randnum Exchange with DES; directory access rights and
 * UARights), "File sharing modes" (access and deny modes), "Desktop
 * database", "AFP's use of ASP" (FPWrite and FPAddIcon through SPWrite,
 * FPGetSrvrInfo through SPGetStatus), each call's inputs, outputs, result
 * codes, algorithm and block format (FPAddAPPL to FPWrite); appendix C
 * (command and result codes). AppleTalk Filing Protocol Version 2.1 and
 * 2.2 (AppleShare IP 6.0 Developer's Kit, Apple, 1998): the AFP 2.1
 * version string, the Flags bits of FPGetSrvrInfo, the volume attribute
 * bits of FPGetVolParms, FPGetSrvrMsg, FPExchangeFiles, the new result
 * codes. Inside Macintosh: Files (Apple, 1992), FSpExchangeFiles (what is
 * exchanged: both forks and the modification dates).
 *
 * Livadix's own code. */
#include "afp.h"
#include "afp_store.h"
#include "des.h"
#include "macroman.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

#define FORKS 512
#define MODS 4096
#define QUANTUM 4624
#define NEVER ((int32_t)0x80000000u)

enum {
    FP_BYTE_RANGE_LOCK = 1, FP_CLOSE_VOL = 2, FP_CLOSE_DIR = 3, FP_CLOSE_FORK = 4, FP_COPY_FILE = 5, FP_CREATE_DIR = 6,
    FP_CREATE_FILE = 7, FP_DELETE = 8, FP_ENUMERATE = 9, FP_FLUSH = 10, FP_FLUSH_FORK = 11, FP_GET_FORK_PARMS = 14,
    FP_GET_SRVR_INFO = 15, FP_GET_SRVR_PARMS = 16, FP_GET_VOL_PARMS = 17, FP_LOGIN = 18, FP_LOGIN_CONT = 19, FP_LOGOUT = 20,
    FP_MAP_ID = 21, FP_MAP_NAME = 22, FP_MOVE_AND_RENAME = 23, FP_OPEN_VOL = 24, FP_OPEN_DIR = 25, FP_OPEN_FORK = 26, FP_READ = 27,
    FP_RENAME = 28, FP_SET_DIR_PARMS = 29, FP_SET_FILE_PARMS = 30, FP_SET_FORK_PARMS = 31, FP_SET_VOL_PARMS = 32, FP_WRITE = 33,
    FP_GET_FILE_DIR_PARMS = 34, FP_SET_FILE_DIR_PARMS = 35, FP_CHANGE_PASSWORD = 36, FP_GET_USER_INFO = 37, FP_GET_SRVR_MSG = 38,
    FP_EXCHANGE_FILES = 42, FP_OPEN_DT = 48, FP_CLOSE_DT = 49, FP_GET_ICON = 51, FP_GET_ICON_INFO = 52, FP_ADD_APPL = 53,
    FP_REMOVE_APPL = 54, FP_GET_APPL = 55, FP_ADD_COMMENT = 56, FP_REMOVE_COMMENT = 57, FP_GET_COMMENT = 58, FP_ADD_ICON = 192
};

/* File and directory parameter bits (FPGetFileDirParms). */
enum {
    B_ATTR = 1 << 0, B_PARENT = 1 << 1, B_CREATED = 1 << 2, B_MODIFIED = 1 << 3, B_BACKUP = 1 << 4, B_FINDER = 1 << 5,
    B_LONG = 1 << 6, B_SHORT = 1 << 7, B_ID = 1 << 8, B_DATA_LEN = 1 << 9, B_RSRC_LEN = 1 << 10, B_OFFSPRING = 1 << 9,
    B_OWNER = 1 << 10, B_GROUP = 1 << 11, B_ACCESS = 1 << 12, B_PRODOS = 1 << 13
};
#define FILE_BITS (B_ATTR | B_PARENT | B_CREATED | B_MODIFIED | B_BACKUP | B_FINDER | B_LONG | B_SHORT | B_ID | B_DATA_LEN | B_RSRC_LEN | B_PRODOS)
#define DIR_BITS (B_ATTR | B_PARENT | B_CREATED | B_MODIFIED | B_BACKUP | B_FINDER | B_LONG | B_SHORT | B_ID | B_OFFSPRING | B_OWNER | B_GROUP | B_ACCESS | B_PRODOS)

/* Attributes. */
enum {
    A_INVISIBLE = 1 << 0, A_MULTIUSER = 1 << 1, A_SYSTEM = 1 << 2, A_DOPEN = 1 << 3, A_ROPEN = 1 << 4, A_WRITE_INHIBIT = 1 << 5,
    A_BACKUP_NEEDED = 1 << 6, A_RENAME_INHIBIT = 1 << 7, A_DELETE_INHIBIT = 1 << 8, A_COPY_PROTECT = 1 << 10, A_SET = 1 << 15
};
#define LOCKED (A_WRITE_INHIBIT | A_RENAME_INHIBIT | A_DELETE_INHIBIT)
/* Finder flags (Inside Macintosh: Files, the Finder information): invisible. */
#define FINDER_INVISIBLE 0x4000

/* Access modes. */
enum { M_READ = 1, M_WRITE = 2, M_DENY_READ = 16, M_DENY_WRITE = 32 };

typedef struct mod_override {
    uint32_t id;
    int32_t when;
} mod_override;

typedef struct volume {
    uint8_t name[28];
    size_t name_len;
    uint16_t root[MC_HOST_PATH];
    int readonly, meta;
    char note[200];
    mc_afp_ids *ids;
    mc_afp_desktop *dt;
    int32_t created, modified;
    mod_override mods[MODS];
    size_t nmods, next_mod;
    mc_host_watch *watch;
} volume;

typedef struct range {
    int64_t start, end;        /* end exclusive */
} range;

struct session;

typedef struct fork {
    int used;
    struct session *owner;
    uint16_t refnum;
    volume *vol;
    uint32_t id;
    int rsrc;
    uint16_t access;
    mc_host_file *data;
    mc_rsrc *res;
    int written;
    range *locks;
    size_t nlocks;
} fork;

typedef struct entry {
    uint16_t name[MC_HOST_NAME];
    mc_host_stat st;
} entry;

typedef struct session {
    struct mc_afp *afp;
    int logged_in, version;    /* 11, 20, 21 */
    int pending;               /* Randnum Exchange waiting for FPLoginCont */
    uint16_t login_id;
    uint8_t random[8];
    uint32_t open_vols;
    volume *cache_vol;
    uint32_t cache_dir;
    entry *cache;
    size_t ncache, capcache;
} session;

struct mc_afp {
    uint8_t name[32];
    size_t name_len;
    uint8_t password[8];
    int has_password;
    volume *vols[MC_AFP_VOLUMES];
    size_t nvols;
    fork forks[FORKS];
    uint16_t next_ref, next_login;
    uint32_t seed;
    void (*log)(void *ctx, const char *line);
    void *log_ctx;
};

static void logf_(mc_afp *afp, const char *format, ...) {
    if (!afp->log) return;
    char line[512];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof line, format, args);
    va_end(args);
    afp->log(afp->log_ctx, line);
}

int32_t mc_afp_time(int64_t unix_seconds) {
    const int64_t t = unix_seconds - 946684800;
    if (t < INT32_MIN + 1) return INT32_MIN + 1;
    if (t > INT32_MAX) return INT32_MAX;
    return (int32_t)t;
}
static int64_t unix_time(int32_t afp) { return (int64_t)afp + 946684800; }
static int32_t now_afp(void) { return mc_afp_time(mc_host_now()); }

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---- the server icon ------------------------------------------------------- */

/* Livadix's own drawing: a file server cabinet with a disk drive and a livadix
 * on its screen. The mask is the silhouette (every point not reached from
 * the border through white). */
static const char *const icon_art[32] = {
    "................................",
    "...########################.....",
    "...#......................##....",
    "...#.####################.#.#...",
    "...#.#..................#.#..#..",
    "...#.#.......###........#.#..#..",
    "...#.#......#####.......#.#..#..",
    "...#.#......##.##.......#.#..#..",
    "...#.#......#####.......#.#..#..",
    "...#.#.......###........#.#..#..",
    "...#.#........#.........#.#..#..",
    "...#.#........#.........#.#..#..",
    "...#.#........#.........#.#..#..",
    "...#.#.......###........#.#..#..",
    "...#.####################.#..#..",
    "...#......................#..#..",
    "...#.##################...#..#..",
    "...#.#................#...#..#..",
    "...#.##################...#..#..",
    "...#......................#..#..",
    "...#.##################...#..#..",
    "...#.#................#.#.#..#..",
    "...#.##################...#..#..",
    "...#......................#..#..",
    "...#..................###.#..#..",
    "...#......................#.#...",
    "...########################.....",
    "....#....................#......",
    "....######################......",
    "................................",
    "................................",
    "................................"};

static void make_icon(uint8_t out[256]) {
    uint8_t outside[32][32];
    memset(outside, 0, sizeof outside);
    memset(out, 0, 256);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if (icon_art[y][x] == '#') out[y * 4 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
    /* flood the white reachable from the border */
    int changed = 1;
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if ((x == 0 || y == 0 || x == 31 || y == 31) && icon_art[y][x] != '#') outside[y][x] = 1;
    while (changed) {
        changed = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                if (outside[y][x] || icon_art[y][x] == '#') continue;
                if ((x > 0 && outside[y][x - 1]) || (x < 31 && outside[y][x + 1]) || (y > 0 && outside[y - 1][x]) ||
                    (y < 31 && outside[y + 1][x])) {
                    outside[y][x] = 1;
                    changed = 1;
                }
            }
    }
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++)
            if (!outside[y][x]) out[128 + y * 4 + x / 8] |= (uint8_t)(0x80 >> (x % 8));
}

/* ---- names and paths --------------------------------------------------------- */

static int utf8_to_mac(const char *utf8, uint8_t *out, size_t most) {
    uint16_t w[256];
    mc_host_from_utf8(utf8, w, 256);
    size_t n = 0;
    for (size_t k = 0; w[k]; k++) {
        uint8_t c;
        if (n == most || !mc_unicode_to_macroman(w[k], &c) || c == ':' || c == 0) return -1;
        out[n++] = c;
    }
    return (int)n;
}

static const uint16_t name_ids[] = {'.', 'm', 'a', 'c', 'e', '-', 'i', 'd', 's', 0};
static const uint16_t name_desktop[] = {'.', 'm', 'a', 'c', 'e', '-', 'd', 'e', 's', 'k', 't', 'o', 'p', 0};

static int eq_ci(const uint16_t *a, const uint16_t *b) {
    for (; *a && *b; a++, b++) {
        uint16_t x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (uint16_t)(x + 32);
        if (y >= 'A' && y <= 'Z') y = (uint16_t)(y + 32);
        if (x != y) return 0;
    }
    return *a == *b;
}

/* Names the Macintosh never sees: AppleDouble files, the databases, and
 * what Windows keeps hidden as system files. */
static int hidden_name(const uint16_t *name, const mc_host_stat *st) {
    if (mc_meta_is_sidecar(name) || eq_ci(name, name_ids) || eq_ci(name, name_desktop)) return 1;
    return st && (st->attributes & (MC_HOST_HIDDEN | MC_HOST_SYSTEM)) == (MC_HOST_HIDDEN | MC_HOST_SYSTEM);
}

/* The Macintosh name of a host name: as it is, or (when Mac Roman cannot
 * hold it or it is too long) a short form with the ID: "Begin#1A2F.ext". */
static size_t mac_name(uint32_t id, const uint16_t *host, uint8_t out[32]) {
    uint8_t full[256];
    int exact;
    const size_t n = mc_host_to_mac_name(host, full, &exact);
    if (exact) {
        memcpy(out, full, n);
        return n;
    }
    char tag[16];
    const int tlen = snprintf(tag, sizeof tag, "#%lX", (unsigned long)id);
    size_t ext = 0;
    for (size_t k = n; k > 0 && n - k < 6; k--)
        if (full[k - 1] == '.') {
            ext = n - (k - 1);
            break;
        }
    if (ext == n) ext = 0;
    size_t keep = MC_MAC_NAME_MAX - (size_t)tlen - ext;
    if (keep > n - ext) keep = n - ext;
    memcpy(out, full, keep);
    memcpy(out + keep, tag, (size_t)tlen);
    memcpy(out + keep + (size_t)tlen, full + n - ext, ext);
    return keep + (size_t)tlen + ext;
}

/* The ID a short form names ("…#1A2F" or "…#1A2F.ext"), 0 when it is not one. */
static uint32_t mangled_id(const uint8_t *s, size_t n) {
    size_t end = n;
    for (size_t k = n; k > 0 && n - k < 6; k--)
        if (s[k - 1] == '.') {
            end = k - 1;
            break;
        }
    size_t start = end;
    while (start > 0 && ((s[start - 1] >= '0' && s[start - 1] <= '9') || (s[start - 1] >= 'A' && s[start - 1] <= 'F'))) start--;
    if (start == end || start == 0 || s[start - 1] != '#' || end - start > 8) return 0;
    uint32_t id = 0;
    for (size_t k = start; k < end; k++) id = id << 4 | (uint32_t)(s[k] <= '9' ? s[k] - '0' : s[k] - 'A' + 10);
    return id;
}

typedef struct node {
    volume *vol;
    uint32_t id, parent;
    uint16_t path[MC_HOST_PATH];
    uint16_t name[MC_HOST_NAME];
    int exists;
    mc_host_stat st;
    uint8_t mac[32];           /* the last element as the Macintosh named it (to create) */
    size_t mac_len;
} node;

#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function"   /* kept with the path helpers */
#endif
static void strip_last(uint16_t *path) {
    size_t n = mc_host_len(path);
    while (n && path[n - 1] != '\\') n--;
    if (n) n--;
    path[n] = 0;
}

static int under_root(const volume *v, const uint16_t *path, const uint16_t **rest) {
    const size_t n = mc_host_len(v->root);
    for (size_t k = 0; k < n; k++) {
        uint16_t a = path[k], b = v->root[k];
        if (!a) return 0;
        if (a >= 'a' && a <= 'z') a = (uint16_t)(a - 32);
        if (b >= 'a' && b <= 'z') b = (uint16_t)(b - 32);
        if (a != b) return 0;
    }
    if (path[n] != 0 && path[n] != '\\') return 0;
    *rest = path[n] ? path + n + 1 : path + n;
    return 1;
}

/* The IDs along a path relative to the root, registering them. */
static uint32_t id_of_relative(volume *v, const uint16_t *rel, uint32_t *parent_out) {
    uint32_t id = MC_AFP_ROOT, parent = MC_AFP_ROOT_PARENT;
    uint16_t path[MC_HOST_PATH], part[MC_HOST_NAME];
    mc_host_copy_str(path, v->root, MC_HOST_PATH);
    while (*rel) {
        size_t n = 0;
        while (rel[n] && rel[n] != '\\' && n + 1 < MC_HOST_NAME) {
            part[n] = rel[n];
            n++;
        }
        part[n] = 0;
        rel += n;
        if (*rel == '\\') rel++;
        if (!n) continue;
        mc_host_stat st;
        uint16_t real[MC_HOST_NAME];
        if (!mc_host_join(path, MC_HOST_PATH, path, part) || mc_host_stat_name(path, &st, real)) return 0;
        parent = id;
        id = mc_ids_get(v->ids, parent, real, st.index);
    }
    if (parent_out) *parent_out = parent;
    return id;
}

/* The host path of a directory or file ID; *st its status. */
static int path_of_id(volume *v, uint32_t id, uint16_t *path, mc_host_stat *st, int depth) {
    if (id == MC_AFP_ROOT) {
        mc_host_copy_str(path, v->root, MC_HOST_PATH);
        return mc_host_stat_path(path, st) ? MC_AFP_OBJECT_NOT_FOUND : MC_AFP_OK;
    }
    uint32_t parent;
    uint16_t name[MC_HOST_NAME];
    uint64_t index;
    if (depth > 128 || !mc_ids_lookup(v->ids, id, &parent, name, &index)) return MC_AFP_OBJECT_NOT_FOUND;
    if (path_of_id(v, parent, path, st, depth + 1) == MC_AFP_OK && mc_host_join(path, MC_HOST_PATH, path, name) &&
        mc_host_stat_path(path, st) == MC_AFP_OK && (!index || st->index == index))
        return MC_AFP_OK;
    /* Moved or renamed on the host: the file ID finds it again. */
    uint16_t found[MC_HOST_PATH];
    const uint16_t *rest;
    if (index && mc_host_path_of_index(v->root, index, found, MC_HOST_PATH) && under_root(v, found, &rest) &&
        id_of_relative(v, rest, NULL) == id) {
        mc_host_copy_str(path, found, MC_HOST_PATH);
        return mc_host_stat_path(path, st) ? MC_AFP_OBJECT_NOT_FOUND : MC_AFP_OK;
    }
    return MC_AFP_OBJECT_NOT_FOUND;
}

static int node_of_id(volume *v, uint32_t id, node *n) {
    memset(n, 0, sizeof *n);
    n->vol = v;
    const int e = path_of_id(v, id, n->path, &n->st, 0);
    if (e) return e;
    n->id = id;
    n->exists = 1;
    if (id == MC_AFP_ROOT) n->parent = MC_AFP_ROOT_PARENT;
    else mc_ids_lookup(v->ids, id, &n->parent, n->name, NULL);
    return MC_AFP_OK;
}

/* The child named mac of directory dir. */
static int child(volume *v, const node *dir, const uint8_t *mac, size_t len, node *out) {
    memset(out, 0, sizeof *out);
    out->vol = v;
    out->parent = dir->id;
    if (len > MC_MAC_NAME_MAX) return MC_AFP_OBJECT_NOT_FOUND;
    memcpy(out->mac, mac, len);
    out->mac_len = len;
    uint16_t host[MC_HOST_NAME];
    if (!mc_mac_to_host_name(mac, len, host, MC_HOST_NAME)) return MC_AFP_PARAM_ERR;
    if (!mc_host_join(out->path, MC_HOST_PATH, dir->path, host)) return MC_AFP_PARAM_ERR;
    mc_host_copy_str(out->name, host, MC_HOST_NAME);
    if (!hidden_name(host, NULL) && mc_host_stat_name(out->path, &out->st, out->name) == MC_AFP_OK &&
        !hidden_name(out->name, &out->st)) {
        out->exists = 1;
        out->id = mc_ids_get(v->ids, dir->id, out->name, out->st.index);
        return MC_AFP_OK;
    }
    const uint32_t id = mangled_id(mac, len);
    uint32_t parent;
    if (id && mc_ids_lookup(v->ids, id, &parent, NULL, NULL) && parent == dir->id) {
        node m;
        if (node_of_id(v, id, &m) == MC_AFP_OK && m.parent == dir->id) {
            *out = m;
            out->mac_len = len;
            memcpy(out->mac, mac, len);
            return MC_AFP_OK;
        }
    }
    mc_host_copy_str(out->name, host, MC_HOST_NAME);
    mc_host_join(out->path, MC_HOST_PATH, dir->path, host);
    return MC_AFP_OK;
}

/* A path from a directory (Inside AppleTalk 13, "Designating a path to a
 * CNode"). The node may not exist (its parent does): *n then names where
 * it would be. */
static int resolve(volume *v, uint32_t dirid, uint8_t ptype, const uint8_t *p, size_t plen, node *n) {
    if (ptype != 1 && ptype != 2) return MC_AFP_PARAM_ERR;
    if (!dirid) return MC_AFP_PARAM_ERR;
    node cur;
    size_t at = 0;
    if (dirid == MC_AFP_ROOT_PARENT) {
        /* the first element is the volume's name */
        while (at < plen && p[at] == 0) at++;
        size_t e = at;
        while (e < plen && p[e]) e++;
        if (!mc_mac_name_equal(p + at, e - at, v->name, v->name_len)) return MC_AFP_OBJECT_NOT_FOUND;
        at = e;
        dirid = MC_AFP_ROOT;
    }
    int err = node_of_id(v, dirid, &cur);
    if (err) return err == MC_AFP_OBJECT_NOT_FOUND ? MC_AFP_OBJECT_NOT_FOUND : err;
    if (!cur.st.dir) return MC_AFP_OBJECT_NOT_FOUND;
    while (at < plen) {
        if (p[at] == 0) {
            size_t run = 0;
            while (at < plen && p[at] == 0) at++, run++;
            /* a run of k nulls climbs k - 1 levels (a lone leading null is ignored) */
            for (size_t k = 1; k < run; k++) {
                if (cur.id == MC_AFP_ROOT) return MC_AFP_OBJECT_NOT_FOUND;
                const uint32_t up = cur.parent;
                err = node_of_id(v, up, &cur);
                if (err) return err;
            }
            continue;
        }
        size_t e = at;
        while (e < plen && p[e]) e++;
        node next;
        err = child(v, &cur, p + at, e - at, &next);
        if (err) return err;
        at = e;
        int last = 1;
        for (size_t k = at; k < plen; k++)
            if (p[k]) last = 0;
        /* trailing nulls that climb are not "last" */
        size_t run = 0;
        for (size_t k = at; k < plen && p[k] == 0; k++) run++;
        if (run > 1) last = 0;
        if (!next.exists) {
            if (!last) return MC_AFP_OBJECT_NOT_FOUND;
            *n = next;
            return MC_AFP_OK;
        }
        if (!last && !next.st.dir) return MC_AFP_OBJECT_NOT_FOUND;
        cur = next;
    }
    *n = cur;
    return MC_AFP_OK;
}

/* ---- dates, metadata and parameters --------------------------------------- */

static int32_t dir_modified(volume *v, uint32_t id, int32_t host) {
    for (size_t k = 0; k < v->nmods; k++)
        if (v->mods[k].id == id && v->mods[k].when > host) return v->mods[k].when;
    return host;
}

static void touch_dir(volume *v, uint32_t id) {
    const int32_t t = now_afp();
    v->modified = t > v->modified ? t : v->modified + 1;
    for (size_t k = 0; k < v->nmods; k++)
        if (v->mods[k].id == id) {
            v->mods[k].when = v->modified;
            return;
        }
    mod_override *m = v->nmods < MODS ? &v->mods[v->nmods++] : &v->mods[v->next_mod++ % MODS];
    m->id = id;
    m->when = v->modified;
}

static int fork_open(mc_afp *afp, volume *v, uint32_t id, int rsrc) {
    for (unsigned k = 0; k < FORKS; k++)
        if (afp->forks[k].used && afp->forks[k].vol == v && afp->forks[k].id == id && (rsrc < 0 || afp->forks[k].rsrc == rsrc)) return 1;
    return 0;
}

static unsigned offspring(volume *v, const uint16_t *path) {
    int e;
    mc_host_dir *d = mc_host_opendir(path, &e);
    if (!d) return 0;
    unsigned n = 0;
    uint16_t name[MC_HOST_NAME];
    mc_host_stat st;
    while (mc_host_readdir(d, name, &st))
        if (!hidden_name(name, &st)) n++;
    mc_host_closedir(d);
    (void)v;
    return n > 0xFFFF ? 0xFFFF : n;
}

static size_t short_name(const uint8_t *mac, size_t n, uint8_t out[13]) {
    size_t dot = n, k = 0;
    for (size_t i = n; i > 0; i--)
        if (mac[i - 1] == '.') { dot = i - 1; break; }
    for (size_t i = 0; i < dot && k < 8; i++) {
        const uint8_t c = mc_macroman_upper(mac[i]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[k++] = c;
    }
    if (!k) out[k++] = '_';
    if (dot < n) {
        out[k++] = '.';
        for (size_t i = dot + 1, m = 0; i < n && m < 3; i++) {
            const uint8_t c = mc_macroman_upper(mac[i]);
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[k++] = c, m++;
        }
    }
    return k;
}

static uint16_t attributes_of(mc_afp *afp, const node *n, const mc_afp_meta *m) {
    uint16_t a = (uint16_t)(m->attributes & (A_MULTIUSER | A_SYSTEM | A_BACKUP_NEEDED | A_RENAME_INHIBIT | A_DELETE_INHIBIT | A_WRITE_INHIBIT));
    if (n->st.dir) a &= (uint16_t)(A_SYSTEM | A_BACKUP_NEEDED | A_RENAME_INHIBIT | A_DELETE_INHIBIT);
    if ((get16(m->finder + 8) & FINDER_INVISIBLE) || (n->st.attributes & MC_HOST_HIDDEN)) a |= A_INVISIBLE;
    if (!n->st.dir) {
        if (n->st.attributes & MC_HOST_READONLY) a |= LOCKED;
        if (fork_open(afp, n->vol, n->id, 0)) a |= A_DOPEN;
        if (fork_open(afp, n->vol, n->id, 1)) a |= A_ROPEN;
    }
    return a;
}

static uint32_t clamp32(uint64_t v) { return v > 0x7FFFFFFFu ? 0x7FFFFFFFu : (uint32_t)v; }

/* The parameters of a file or directory in bitmap order, variable fields
 * after the fixed ones. Returns the length, 0 with *err on an error. */
static size_t pack_params(mc_afp *afp, node *n, uint16_t bitmap, uint8_t *out, size_t capacity, int *err) {
    volume *v = n->vol;
    const int dir = n->st.dir;
    if (bitmap & ~(dir ? DIR_BITS : FILE_BITS)) {
        *err = MC_AFP_BITMAP_ERR;
        return 0;
    }
    mc_afp_meta m;
    if (bitmap & (B_ATTR | B_FINDER | B_BACKUP | B_PRODOS)) mc_meta_read(v->meta, n->path, &m);
    else memset(&m, 0, sizeof m);
    uint8_t lname[32], sname[13];
    size_t llen = 0, slen = 0;
    if (bitmap & (B_LONG | B_SHORT)) {
        if (n->id == MC_AFP_ROOT) {
            memcpy(lname, v->name, v->name_len);
            llen = v->name_len;
        } else llen = mac_name(n->id, n->name, lname);
        slen = short_name(lname, llen, sname);
    }
    uint8_t fixed[128];
    size_t f = 0;
    size_t long_at = 0, short_at = 0;
    const int32_t created = mc_afp_time(n->st.created);
    int32_t modified = mc_afp_time(n->st.modified);
    if (dir) modified = dir_modified(v, n->id, modified);
    if (n->id == MC_AFP_ROOT && v->modified > modified) modified = v->modified;
    for (unsigned bit = 0; bit < 16; bit++) {
        if (!(bitmap & 1u << bit)) continue;
        switch (1u << bit) {
        case B_ATTR: put16(fixed + f, attributes_of(afp, n, &m)); f += 2; break;
        case B_PARENT: put32(fixed + f, n->id == MC_AFP_ROOT ? MC_AFP_ROOT_PARENT : n->parent); f += 4; break;
        case B_CREATED: put32(fixed + f, (uint32_t)created); f += 4; break;
        case B_MODIFIED: put32(fixed + f, (uint32_t)modified); f += 4; break;
        case B_BACKUP: put32(fixed + f, (uint32_t)m.backup); f += 4; break;
        case B_FINDER: memcpy(fixed + f, m.finder, 32); f += 32; break;
        case B_LONG: long_at = f; f += 2; break;
        case B_SHORT: short_at = f; f += 2; break;
        case B_ID: put32(fixed + f, n->id); f += 4; break;
        case B_PRODOS:
            if (dir && !m.prodos[0]) { m.prodos[0] = 0x0F; m.prodos[3] = 0x02; }
            memcpy(fixed + f, m.prodos, 6);
            f += 6;
            break;
        default:
            if (!dir && (1u << bit) == B_DATA_LEN) { put32(fixed + f, clamp32(n->st.size)); f += 4; }
            else if (!dir && (1u << bit) == B_RSRC_LEN) {
                uint64_t r = 0;
                mc_rsrc_length(v->meta, n->path, &r);
                put32(fixed + f, clamp32(r));
                f += 4;
            } else if (dir && (1u << bit) == B_OFFSPRING) { put16(fixed + f, offspring(v, n->path)); f += 2; }
            else if (dir && ((1u << bit) == B_OWNER || (1u << bit) == B_GROUP)) { put32(fixed + f, 0); f += 4; }
            else if (dir && (1u << bit) == B_ACCESS) {
                /* everyone may search, read and write; the user owns every folder */
                const uint8_t r = v->readonly ? 3 : 7;
                fixed[f] = r;
                fixed[f + 1] = r;
                fixed[f + 2] = r;
                fixed[f + 3] = (uint8_t)(0x80 | r);
                f += 4;
            }
            break;
        }
    }
    size_t total = f;
    if (bitmap & B_LONG) total += 1 + llen;
    if (bitmap & B_SHORT) total += 1 + slen;
    if (total > capacity) {
        *err = MC_AFP_PARAM_ERR;
        return 0;
    }
    memcpy(out, fixed, f);
    size_t at = f;
    if (bitmap & B_LONG) {
        put16(out + long_at, (unsigned)at);
        out[at++] = (uint8_t)llen;
        memcpy(out + at, lname, llen);
        at += llen;
    }
    if (bitmap & B_SHORT) {
        put16(out + short_at, (unsigned)at);
        out[at++] = (uint8_t)slen;
        memcpy(out + at, sname, slen);
        at += slen;
    }
    *err = 0;
    return at;
}

/* Sets parameters from a packed block (FPSetFileParms and friends). */
static int set_params(mc_afp *afp, node *n, uint16_t bitmap, const uint8_t *p, size_t len) {
    volume *v = n->vol;
    const int dir = n->st.dir;
    const uint16_t settable = dir ? (B_ATTR | B_CREATED | B_MODIFIED | B_BACKUP | B_FINDER | B_OWNER | B_GROUP | B_ACCESS | B_PRODOS)
                                  : (B_ATTR | B_CREATED | B_MODIFIED | B_BACKUP | B_FINDER | B_PRODOS);
    if (bitmap & ~settable) return MC_AFP_BITMAP_ERR;
    if (v->readonly) return MC_AFP_VOL_LOCKED;
    mc_afp_meta m;
    mc_meta_read(v->meta, n->path, &m);
    const mc_afp_meta before = m;
    int64_t created = 0, modified = 0;
    int set_created = 0, set_modified = 0, lock = -1;
    size_t at = 0;
    for (unsigned bit = 0; bit < 16; bit++) {
        if (!(bitmap & 1u << bit)) continue;
        const unsigned b = 1u << bit;
        const size_t need = b == B_ATTR ? 2 : b == B_FINDER ? 32 : b == B_PRODOS ? 6 : 4;
        if (at + need > len) return MC_AFP_PARAM_ERR;
        const uint8_t *q = p + at;
        at += need;
        if (b == B_ATTR) {
            const uint16_t a = (uint16_t)get16(q);
            const uint16_t bits = (uint16_t)(a & ~A_SET & (A_INVISIBLE | A_MULTIUSER | A_SYSTEM | A_WRITE_INHIBIT | A_BACKUP_NEEDED |
                                                           A_RENAME_INHIBIT | A_DELETE_INHIBIT));
            if (a & A_SET) m.attributes |= bits;
            else m.attributes &= (uint16_t)~bits;
            if (bits & A_INVISIBLE) {
                uint16_t flags = (uint16_t)get16(m.finder + 8);
                flags = (a & A_SET) ? (uint16_t)(flags | FINDER_INVISIBLE) : (uint16_t)(flags & ~FINDER_INVISIBLE);
                put16(m.finder + 8, flags);
            }
            if (!dir && (bits & LOCKED)) lock = (a & A_SET) ? 1 : 0;
        } else if (b == B_CREATED) { created = unix_time((int32_t)get32(q)); set_created = 1; }
        else if (b == B_MODIFIED) { modified = unix_time((int32_t)get32(q)); set_modified = 1; }
        else if (b == B_BACKUP) m.backup = (int32_t)get32(q);
        else if (b == B_FINDER) {
            memcpy(m.finder, q, 32);
        } else if (b == B_PRODOS) {
            if (dir && q[0] != 0x0F) return MC_AFP_ACCESS_DENIED;
            memcpy(m.prodos, q, 6);
        }
        /* owner, group and access rights: every user has all of them */
    }
    m.attributes &= (uint16_t)~(A_WRITE_INHIBIT | A_RENAME_INHIBIT | A_DELETE_INHIBIT);  /* the read-only attribute holds the lock */
    int e = MC_AFP_OK;
    if (lock == 0) e = mc_host_set_attribute(n->path, MC_HOST_READONLY, 0);
    mc_afp_meta cmp = m;
    cmp.attributes = before.attributes & (uint16_t)~LOCKED;
    if (!e && (memcmp(&cmp, &before, sizeof m) || m.attributes != (before.attributes & (uint16_t)~LOCKED))) {
        m.id = n->id;
        e = mc_meta_write(v->meta, n->path, &m);
    }
    if (!e && (set_created || set_modified))
        e = mc_host_set_times(n->path, set_created ? &created : NULL, set_modified ? &modified : NULL);
    if (!e && lock == 1) e = mc_host_set_attribute(n->path, MC_HOST_READONLY, 1);
    if (e == MC_HOST_BUSY) e = MC_AFP_ACCESS_DENIED;
    if (!e) touch_dir(v, dir ? n->id : n->parent);
    (void)afp;
    return e;
}

/* ---- sessions, volumes, forks ------------------------------------------------ */

static volume *vol_of(session *s, unsigned id) {
    if (!id || id > s->afp->nvols || !(s->open_vols & 1u << (id - 1))) return NULL;
    return s->afp->vols[id - 1];
}

static fork *fork_of(session *s, unsigned ref) {
    for (unsigned k = 0; k < FORKS; k++)
        if (s->afp->forks[k].used && s->afp->forks[k].owner == s && s->afp->forks[k].refnum == ref) return &s->afp->forks[k];
    return NULL;
}

static void close_fork(fork *f) {
    if (f->written) {
        /* the modification date moves when a written fork closes */
        if (f->data) mc_host_flush(f->data);
        node n;
        if (node_of_id(f->vol, f->id, &n) == MC_AFP_OK) {
            const int64_t now = mc_host_now();
            mc_host_set_times(n.path, NULL, &now);
            touch_dir(f->vol, n.parent);
        }
    }
    mc_host_close(f->data);
    mc_rsrc_close(f->res);
    free(f->locks);
    memset(f, 0, sizeof *f);
}

static int fork_read(fork *f, uint64_t off, void *b, size_t n, size_t *got) {
    return f->rsrc ? mc_rsrc_read(f->res, off, b, n, got) : mc_host_read(f->data, off, b, n, got);
}
static int fork_write(fork *f, uint64_t off, const void *b, size_t n) {
    return f->rsrc ? mc_rsrc_write(f->res, off, b, n) : mc_host_write(f->data, off, b, n);
}
static int fork_size(fork *f, uint64_t *size) { return f->rsrc ? mc_rsrc_size(f->res, size) : mc_host_size(f->data, size); }
static int fork_truncate(fork *f, uint64_t size) { return f->rsrc ? mc_rsrc_truncate(f->res, size) : mc_host_truncate(f->data, size); }
static int fork_flush(fork *f) { return f->rsrc ? mc_rsrc_flush(f->res) : mc_host_flush(f->data); }

/* A range another fork of the same file holds. */
static int locked_by_other(mc_afp *afp, const fork *f, int64_t start, int64_t end) {
    for (unsigned k = 0; k < FORKS; k++) {
        const fork *o = &afp->forks[k];
        if (!o->used || o == f || o->vol != f->vol || o->id != f->id || o->rsrc != f->rsrc) continue;
        for (size_t i = 0; i < o->nlocks; i++)
            if (o->locks[i].start < end && start < o->locks[i].end) return 1;
    }
    return 0;
}

static void end_session(session *s) {
    mc_afp *afp = s->afp;
    for (unsigned k = 0; k < FORKS; k++)
        if (afp->forks[k].used && afp->forks[k].owner == s) close_fork(&afp->forks[k]);
    s->open_vols = 0;
    s->logged_in = 0;
    free(s->cache);
    s->cache = NULL;
    s->ncache = s->capcache = 0;
    s->cache_vol = NULL;
}

void *mc_afp_session_open(mc_afp *afp) {
    session *s = calloc(1, sizeof *s);
    if (s) s->afp = afp;
    return s;
}

void mc_afp_session_close(mc_afp *afp, void *p) {
    (void)afp;
    session *s = p;
    if (!s) return;
    end_session(s);
    free(s);
}

/* ---- the server information block ---------------------------------------------- */

static const char *const versions[] = {"AFPVersion 1.1", "AFPVersion 2.0", "AFPVersion 2.1"};

size_t mc_afp_status(mc_afp *afp, uint8_t *out, size_t capacity) {
    uint8_t b[1024];
    size_t at = 10;
    /* flags: SupportsCopyFile, SupportsServerMessages */
    put16(b + 8, 0x0001 | 0x0008);
    b[at++] = (uint8_t)afp->name_len;
    memcpy(b + at, afp->name, afp->name_len);
    at += afp->name_len;
    if (at & 1) b[at++] = 0;
    put16(b, (unsigned)at);
    static const char machine[] = "Livadix";
    b[at++] = (uint8_t)(sizeof machine - 1);
    memcpy(b + at, machine, sizeof machine - 1);
    at += sizeof machine - 1;
    put16(b + 2, (unsigned)at);
    b[at++] = 3;
    for (unsigned k = 0; k < 3; k++) {
        const size_t n = strlen(versions[k]);
        b[at++] = (uint8_t)n;
        memcpy(b + at, versions[k], n);
        at += n;
    }
    put16(b + 4, (unsigned)at);
    const char *uams[2] = {"Cleartxt Passwrd", "Randnum Exchange"};
    if (afp->has_password) {
        b[at++] = 2;
        for (unsigned k = 0; k < 2; k++) {
            const size_t n = strlen(uams[k]);
            b[at++] = (uint8_t)n;
            memcpy(b + at, uams[k], n);
            at += n;
        }
    } else {
        static const char guest[] = "No User Authent";
        b[at++] = 1;
        b[at++] = (uint8_t)(sizeof guest - 1);
        memcpy(b + at, guest, sizeof guest - 1);
        at += sizeof guest - 1;
    }
    if (at & 1) b[at++] = 0;
    put16(b + 6, (unsigned)at);
    make_icon(b + at);
    at += 256;
    if (at > capacity) return 0;
    memcpy(out, b, at);
    return at;
}

/* ---- request parsing helpers ---------------------------------------------------- */

typedef struct reader {
    const uint8_t *p;
    size_t n, at;
    int bad;
} reader;

static unsigned r8(reader *r) {
    if (r->at + 1 > r->n) { r->bad = 1; return 0; }
    return r->p[r->at++];
}
static unsigned r16(reader *r) {
    if (r->at + 2 > r->n) { r->bad = 1; return 0; }
    const unsigned v = get16(r->p + r->at);
    r->at += 2;
    return v;
}
static uint32_t r32(reader *r) {
    if (r->at + 4 > r->n) { r->bad = 1; return 0; }
    const uint32_t v = get32(r->p + r->at);
    r->at += 4;
    return v;
}
static const uint8_t *rstr(reader *r, size_t *len) {
    const size_t n = r8(r);
    if (r->bad || r->at + n > r->n) { r->bad = 1; *len = 0; return NULL; }
    const uint8_t *s = r->p + r->at;
    r->at += n;
    *len = n;
    return s;
}
static void ralign(reader *r) {
    if (r->at & 1) r->at++;
}

/* Volume, directory, path type and path, then the node. */
static int read_node(session *s, reader *r, unsigned vol_id, node *n, volume **vout) {
    const uint32_t dir = r32(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    if (vout) *vout = v;
    return resolve(v, dir, (uint8_t)ptype, path, plen, n);
}

/* ---- the commands ------------------------------------------------------------------ */

static int32_t do_login(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    mc_afp *afp = s->afp;
    size_t vlen, ulen;
    const uint8_t *version = rstr(r, &vlen);
    const uint8_t *uam = rstr(r, &ulen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    int v = 0;
    for (unsigned k = 0; k < 3; k++)
        if (vlen == strlen(versions[k]) && !memcmp(version, versions[k], vlen)) v = k == 0 ? 11 : k == 1 ? 20 : 21;
    if (!v) return MC_AFP_BAD_VERSION;
    s->version = v;
    char method[20] = {0};
    for (size_t k = 0; k < ulen && k < sizeof method - 1; k++) method[k] = (char)mc_macroman_upper(uam[k]);
    if (!strcmp(method, "NO USER AUTHENT")) {
        if (afp->has_password) return MC_AFP_BAD_UAM;
        s->logged_in = 1;
        logf_(afp, "AFP: a guest logged in (%s)", versions[v == 11 ? 0 : v == 20 ? 1 : 2]);
        return MC_AFP_OK;
    }
    if (!afp->has_password) return MC_AFP_BAD_UAM;
    size_t nlen;
    rstr(r, &nlen);                      /* the user's name: any name with the password */
    if (r->bad) return MC_AFP_PARAM_ERR;
    if (!strcmp(method, "CLEARTXT PASSWRD")) {
        ralign(r);
        if (r->at + 8 > r->n) return MC_AFP_PARAM_ERR;
        if (memcmp(r->p + r->at, afp->password, 8)) return MC_AFP_USER_NOT_AUTH;
        s->logged_in = 1;
        return MC_AFP_OK;
    }
    if (!strcmp(method, "RANDNUM EXCHANGE")) {
        for (unsigned k = 0; k < 8; k++) {
            afp->seed = (afp->seed * 1103515245u + 12345u) ^ (uint32_t)mc_host_now();
            s->random[k] = (uint8_t)(afp->seed >> 16);
        }
        s->login_id = ++afp->next_login;
        s->pending = 1;
        put16(reply, s->login_id);
        memcpy(reply + 2, s->random, 8);
        *rlen = 10;
        return MC_AFP_AUTH_CONTINUE;
    }
    return MC_AFP_BAD_UAM;
}

static int32_t do_login_cont(session *s, reader *r) {
    r8(r);
    const unsigned id = r16(r);
    if (r->bad || r->at + 8 > r->n || !s->pending || id != s->login_id) return MC_AFP_PARAM_ERR;
    s->pending = 0;
    uint8_t expect[8];
    mc_des_encrypt(s->afp->password, s->random, expect);
    if (memcmp(expect, r->p + r->at, 8)) return MC_AFP_USER_NOT_AUTH;
    s->logged_in = 1;
    return MC_AFP_OK;
}

static int32_t vol_params(mc_afp *afp, volume *v, unsigned bitmap, uint8_t *out, size_t *len) {
    if (!bitmap || (bitmap & ~0x01FFu)) return MC_AFP_BITMAP_ERR;
    uint64_t total = 0, free_bytes = 0;
    if (bitmap & 0xC0) mc_host_volume(v->root, &total, &free_bytes, NULL, 0);
    size_t at = 0, name_at = 0;
    put16(out, bitmap);
    uint8_t *p = out + 2;
    for (unsigned bit = 0; bit < 9; bit++) {
        if (!(bitmap & 1u << bit)) continue;
        switch (bit) {
        case 0: put16(p + at, v->readonly ? 1 : 0); at += 2; break;
        case 1: put16(p + at, 2); at += 2; break;           /* fixed directory IDs */
        case 2: put32(p + at, (uint32_t)v->created); at += 4; break;
        case 3: put32(p + at, (uint32_t)v->modified); at += 4; break;
        case 4: {
            mc_afp_meta m;
            mc_meta_read(v->meta, v->root, &m);
            put32(p + at, (uint32_t)m.backup);
            at += 4;
            break;
        }
        case 5: put16(p + at, 0); at += 2; break;           /* filled by the caller */
        case 6: put32(p + at, clamp32(free_bytes)); at += 4; break;
        case 7: put32(p + at, clamp32(total)); at += 4; break;
        case 8: name_at = at; at += 2; break;
        }
    }
    if (bitmap & 0x100) {
        put16(p + name_at, (unsigned)at);
        p[at++] = (uint8_t)v->name_len;
        memcpy(p + at, v->name, v->name_len);
        at += v->name_len;
    }
    (void)afp;
    *len = 2 + at;
    return MC_AFP_OK;
}

static void set_vol_id(uint8_t *out, unsigned bitmap, unsigned id) {
    size_t at = 2;
    static const size_t width[9] = {2, 2, 4, 4, 4, 2, 4, 4, 2};
    for (unsigned bit = 0; bit < 5; bit++)
        if (bitmap & 1u << bit) at += width[bit];
    if (bitmap & 0x20) put16(out + at, id);
}

static int32_t do_open_vol(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    mc_afp *afp = s->afp;
    const unsigned bitmap = r16(r);
    size_t nlen;
    const uint8_t *name = rstr(r, &nlen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    for (unsigned k = 0; k < afp->nvols; k++) {
        volume *v = afp->vols[k];
        if (!mc_mac_name_equal(name, nlen, v->name, v->name_len)) continue;
        const int32_t e = vol_params(afp, v, bitmap, reply, rlen);
        if (e) return e;
        set_vol_id(reply, bitmap, k + 1);
        s->open_vols |= 1u << k;
        return MC_AFP_OK;
    }
    return MC_AFP_OBJECT_NOT_FOUND;
}

/* Deny-mode conflicts with the forks already open (Inside AppleTalk 13,
 * "File sharing modes"). */
static int deny_conflict(mc_afp *afp, volume *v, uint32_t id, int rsrc, unsigned mode) {
    for (unsigned k = 0; k < FORKS; k++) {
        const fork *o = &afp->forks[k];
        if (!o->used || o->vol != v || o->id != id || o->rsrc != rsrc) continue;
        if ((mode & M_READ) && (o->access & M_DENY_READ)) return 1;
        if ((mode & M_WRITE) && (o->access & M_DENY_WRITE)) return 1;
        if ((mode & M_DENY_READ) && (o->access & M_READ)) return 1;
        if ((mode & M_DENY_WRITE) && (o->access & M_WRITE)) return 1;
    }
    return 0;
}

static int32_t do_open_fork(session *s, reader *r, unsigned flag, uint8_t *reply, size_t *rlen) {
    mc_afp *afp = s->afp;
    const unsigned vol_id = r16(r);
    const uint32_t dir = r32(r);
    const unsigned bitmap = r16(r);
    const unsigned mode = r16(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dir, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (n.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    const int rsrc = (flag & 0x80) != 0;
    if (bitmap & (rsrc ? B_DATA_LEN : B_RSRC_LEN)) return MC_AFP_BITMAP_ERR;
    if (mode & M_WRITE) {
        if (v->readonly) return s->version == 11 ? MC_AFP_ACCESS_DENIED : MC_AFP_VOL_LOCKED;
        if (n.st.attributes & MC_HOST_READONLY) return s->version == 11 ? MC_AFP_ACCESS_DENIED : MC_AFP_OBJECT_LOCKED;
    }
    put16(reply, bitmap);
    put16(reply + 2, 0);
    if (deny_conflict(afp, v, n.id, rsrc, mode)) {
        size_t pl = pack_params(afp, &n, (uint16_t)bitmap, reply + 4, QUANTUM - 4, &e);
        *rlen = e ? 0 : 4 + pl;
        return MC_AFP_DENY_CONFLICT;
    }
    fork *f = NULL;
    for (unsigned k = 0; k < FORKS && !f; k++)
        if (!afp->forks[k].used) f = &afp->forks[k];
    if (!f) return MC_AFP_TOO_MANY_FILES_OPEN;
    const int write = (mode & M_WRITE) != 0;
    if (rsrc) e = mc_rsrc_open(v->meta, n.path, write, &f->res);
    else e = mc_host_open(n.path, write, MC_HOST_OPEN, &f->data);
    if (e == MC_HOST_BUSY) e = MC_AFP_DENY_CONFLICT;
    if (e == MC_AFP_ACCESS_DENIED && write) e = MC_AFP_OBJECT_LOCKED;
    if (e) {
        if (e == MC_AFP_DENY_CONFLICT) {
            size_t pl = pack_params(afp, &n, (uint16_t)bitmap, reply + 4, QUANTUM - 4, &e);
            *rlen = 4 + pl;
            return MC_AFP_DENY_CONFLICT;
        }
        return e;
    }
    f->used = 1;
    f->owner = s;
    f->vol = v;
    f->id = n.id;
    f->rsrc = rsrc;
    f->access = (uint16_t)mode;
    do {
        f->refnum = ++afp->next_ref;
    } while (!f->refnum || fork_of(s, f->refnum) != f);
    put16(reply + 2, f->refnum);
    size_t pl = pack_params(afp, &n, (uint16_t)bitmap, reply + 4, QUANTUM - 4, &e);
    if (e) {
        close_fork(f);
        return e;
    }
    *rlen = 4 + pl;
    return MC_AFP_OK;
}

static int32_t do_read(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    fork *f = fork_of(s, r16(r));
    const int32_t offset = (int32_t)r32(r);
    int32_t count = (int32_t)r32(r);
    const unsigned mask = r8(r), ch = r8(r);
    if (r->bad || !f) return MC_AFP_PARAM_ERR;
    if (!(f->access & M_READ)) return MC_AFP_ACCESS_DENIED;
    if (offset < 0 || count < 0) return MC_AFP_PARAM_ERR;
    if (count > QUANTUM) count = QUANTUM;
    if (locked_by_other(s->afp, f, offset, (int64_t)offset + (count ? count : 1))) return MC_AFP_LOCK_ERR;
    size_t got = 0;
    int e = fork_read(f, (uint64_t)offset, reply, (size_t)count, &got);
    if (e == MC_HOST_BUSY) e = MC_AFP_LOCK_ERR;
    if (e) return e;
    if (mask) {
        for (size_t k = 0; k < got; k++)
            if ((reply[k] & mask) == ch) {
                got = k + 1;
                *rlen = got;
                return MC_AFP_OK;
            }
    }
    *rlen = got;
    return got < (size_t)count ? MC_AFP_EOF : MC_AFP_OK;
}

/* FPWrite's command: the fork, where, how much; its data later. */
static long write_begin(session *s, const uint8_t *cmd, size_t len, int32_t *result) {
    reader r = {cmd, len, 1, 0};
    const unsigned flag = r8(&r);
    fork *f = fork_of(s, r16(&r));
    const int32_t offset = (int32_t)r32(&r);
    const int32_t count = (int32_t)r32(&r);
    (void)flag;
    if (r.bad || !f) { *result = MC_AFP_PARAM_ERR; return -1; }
    if (!(f->access & M_WRITE)) { *result = MC_AFP_ACCESS_DENIED; return -1; }
    if (count < 0 || (!(flag & 0x80) && offset < 0)) { *result = MC_AFP_PARAM_ERR; return -1; }
    return count > QUANTUM ? QUANTUM : count;
}

static int32_t write_end(session *s, const uint8_t *cmd, size_t len, const uint8_t *data, size_t dlen, uint8_t *reply, size_t *rlen) {
    reader r = {cmd, len, 1, 0};
    const unsigned flag = r8(&r);
    fork *f = fork_of(s, r16(&r));
    int64_t offset = (int32_t)r32(&r);
    if (r.bad || !f) return MC_AFP_PARAM_ERR;
    if (flag & 0x80) {
        uint64_t size;
        if (fork_size(f, &size)) return MC_AFP_MISC_ERR;
        offset += (int64_t)size;
    }
    if (offset < 0 || offset + (int64_t)dlen > 0x7FFFFFFF) return MC_AFP_DISK_FULL;
    if (locked_by_other(s->afp, f, offset, offset + (int64_t)(dlen ? dlen : 1))) return MC_AFP_LOCK_ERR;
    int e = fork_write(f, (uint64_t)offset, data, dlen);
    if (e == MC_HOST_BUSY) e = MC_AFP_LOCK_ERR;
    if (e) return e;
    f->written = 1;
    put32(reply, (uint32_t)(offset + (int64_t)dlen));
    *rlen = 4;
    return MC_AFP_OK;
}

static int32_t do_byte_range_lock(session *s, reader *r, unsigned flag, uint8_t *reply, size_t *rlen) {
    fork *f = fork_of(s, r16(r));
    int64_t offset = (int32_t)r32(r);
    int64_t length = (int32_t)r32(r);
    if (r->bad || !f) return MC_AFP_PARAM_ERR;
    if (flag & 0x80) {
        uint64_t size;
        if (fork_size(f, &size)) return MC_AFP_MISC_ERR;
        offset += (int64_t)size;
    }
    if (offset < 0 || length == 0 || (length < 0 && length != -1)) return MC_AFP_PARAM_ERR;
    const int64_t end = length == -1 ? 0x7FFFFFFFLL + 1 : offset + length;
    if (flag & 0x01) {
        for (size_t k = 0; k < f->nlocks; k++)
            if (f->locks[k].start == offset) {
                f->locks[k] = f->locks[--f->nlocks];
                put32(reply, (uint32_t)offset);
                *rlen = 4;
                return MC_AFP_OK;
            }
        return MC_AFP_RANGE_NOT_LOCKED;
    }
    for (size_t k = 0; k < f->nlocks; k++)
        if (f->locks[k].start < end && offset < f->locks[k].end) return MC_AFP_RANGE_OVERLAP;
    if (locked_by_other(s->afp, f, offset, end)) return MC_AFP_LOCK_ERR;
    range *grown = realloc(f->locks, (f->nlocks + 1) * sizeof *grown);
    if (!grown) return MC_AFP_NO_MORE_LOCKS;
    f->locks = grown;
    f->locks[f->nlocks].start = offset;
    f->locks[f->nlocks].end = end;
    f->nlocks++;
    put32(reply, (uint32_t)offset);
    *rlen = 4;
    return MC_AFP_OK;
}

/* A directory listing for FPEnumerate, kept between calls of a session. */
static int listing(session *s, volume *v, const node *dir, int fresh) {
    if (!fresh && s->cache_vol == v && s->cache_dir == dir->id) return MC_AFP_OK;
    int e;
    mc_host_dir *d = mc_host_opendir(dir->path, &e);
    if (!d) return e == MC_AFP_OBJECT_NOT_FOUND ? MC_AFP_DIR_NOT_FOUND : e;
    s->ncache = 0;
    entry en;
    while (mc_host_readdir(d, en.name, &en.st)) {
        if (hidden_name(en.name, &en.st)) continue;
        if (s->ncache == s->capcache) {
            const size_t cap = s->capcache ? s->capcache * 2 : 64;
            entry *grown = realloc(s->cache, cap * sizeof *grown);
            if (!grown) break;
            s->cache = grown;
            s->capcache = cap;
        }
        s->cache[s->ncache++] = en;
    }
    mc_host_closedir(d);
    s->cache_vol = v;
    s->cache_dir = dir->id;
    return MC_AFP_OK;
}

static int32_t do_enumerate(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    mc_afp *afp = s->afp;
    const unsigned vol_id = r16(r);
    const uint32_t dirid = r32(r);
    const unsigned fbits = r16(r), dbits = r16(r);
    const unsigned want = r16(r), start = r16(r), max = r16(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    if (!fbits && !dbits) return MC_AFP_BITMAP_ERR;
    if ((fbits & ~FILE_BITS) || (dbits & ~DIR_BITS)) return MC_AFP_BITMAP_ERR;
    node dir;
    int e = resolve(v, dirid, (uint8_t)ptype, path, plen, &dir);
    if (e) return e == MC_AFP_OBJECT_NOT_FOUND ? MC_AFP_DIR_NOT_FOUND : e;
    if (!dir.exists) return MC_AFP_DIR_NOT_FOUND;
    if (!dir.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    if (!start) return MC_AFP_PARAM_ERR;
    e = listing(s, v, &dir, start == 1);
    if (e) return e;
    size_t limit = max < QUANTUM ? max : QUANTUM;
    put16(reply, fbits);
    put16(reply + 2, dbits);
    size_t at = 6;
    unsigned count = 0, index = 0;
    for (size_t k = 0; k < s->ncache && count < want; k++) {
        const entry *en = &s->cache[k];
        if (en->st.dir ? !dbits : !fbits) continue;
        if (++index < start) continue;
        node n;
        memset(&n, 0, sizeof n);
        n.vol = v;
        n.parent = dir.id;
        n.exists = 1;
        n.st = en->st;
        mc_host_copy_str(n.name, en->name, MC_HOST_NAME);
        if (!mc_host_join(n.path, MC_HOST_PATH, dir.path, en->name)) continue;
        n.id = mc_ids_get(v->ids, dir.id, en->name, en->st.index);
        uint8_t params[512];
        int pe;
        const size_t pl = pack_params(afp, &n, (uint16_t)(en->st.dir ? dbits : fbits), params, sizeof params, &pe);
        if (pe) continue;
        size_t len = 2 + pl;
        if (len & 1) len++;
        if (len > 255 || at + len > limit) break;
        reply[at] = (uint8_t)len;
        reply[at + 1] = en->st.dir ? 0x80 : 0;
        memcpy(reply + at + 2, params, pl);
        if ((2 + pl) & 1) reply[at + 2 + pl] = 0;
        at += len;
        count++;
    }
    if (!count) return index < start && at == 6 ? MC_AFP_OBJECT_NOT_FOUND : MC_AFP_PARAM_ERR;
    put16(reply + 4, count);
    *rlen = at;
    return MC_AFP_OK;
}

static int32_t do_get_file_dir_parms(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    const unsigned vol_id = r16(r);
    const uint32_t dirid = r32(r);
    const unsigned fbits = r16(r), dbits = r16(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dirid, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    put16(reply, fbits);
    put16(reply + 2, dbits);
    reply[4] = n.st.dir ? 0x80 : 0;
    reply[5] = 0;
    const size_t pl = pack_params(s->afp, &n, (uint16_t)(n.st.dir ? dbits : fbits), reply + 6, QUANTUM - 6, &e);
    if (e) return e;
    *rlen = 6 + pl;
    return MC_AFP_OK;
}

static int32_t do_set_parms(session *s, reader *r, int want) {
    const unsigned vol_id = r16(r);
    const uint32_t dirid = r32(r);
    const unsigned bitmap = r16(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    ralign(r);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dirid, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (want == 1 && n.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    if (want == 2 && !n.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    return set_params(s->afp, &n, (uint16_t)bitmap, r->p + r->at, r->n > r->at ? r->n - r->at : 0);
}

static int32_t do_create(session *s, reader *r, int directory, unsigned flag, uint8_t *reply, size_t *rlen) {
    volume *v;
    node n;
    int e = read_node(s, r, r16(r), &n, &v);
    if (e) return e;
    if (v->readonly) return MC_AFP_VOL_LOCKED;
    if (n.id == MC_AFP_ROOT && n.exists) return MC_AFP_OBJECT_EXISTS;
    if (!n.mac_len) return MC_AFP_PARAM_ERR;
    if (n.exists) {
        if (directory || n.st.dir) return MC_AFP_OBJECT_EXISTS;
        if (!(flag & 0x80)) return MC_AFP_OBJECT_EXISTS;
        /* a hard create replaces the file, if it is not open */
        if (fork_open(s->afp, v, n.id, -1)) return MC_AFP_FILE_BUSY;
    }
    if (directory) {
        e = mc_host_mkdir(n.path);
        if (e) return e == MC_HOST_BUSY ? MC_AFP_MISC_ERR : e;
    } else {
        mc_host_file *f;
        e = mc_host_open(n.path, 1, n.exists ? MC_HOST_OPEN_ALWAYS : MC_HOST_CREATE_NEW, &f);
        if (e == MC_HOST_BUSY) e = MC_AFP_FILE_BUSY;
        if (e) return e;
        mc_host_truncate(f, 0);
        mc_host_close(f);
        if (n.exists) {
            /* the old forks and metadata go */
            mc_rsrc *rs;
            if (mc_rsrc_open(v->meta, n.path, 1, &rs) == MC_AFP_OK) {
                mc_rsrc_truncate(rs, 0);
                mc_rsrc_close(rs);
            }
            mc_afp_meta m;
            memset(&m, 0, sizeof m);
            m.backup = NEVER;
            mc_meta_write(v->meta, n.path, &m);
        }
    }
    mc_host_stat st;
    uint16_t real[MC_HOST_NAME];
    if (mc_host_stat_name(n.path, &st, real) == MC_AFP_OK) {
        const uint32_t id = mc_ids_get(v->ids, n.parent, real, st.index);
        if (directory) {
            put32(reply, id);
            *rlen = 4;
        }
    }
    touch_dir(v, n.parent);
    return MC_AFP_OK;
}

static int32_t delete_node(mc_afp *afp, volume *v, node *n) {
    if (v->readonly) return MC_AFP_VOL_LOCKED;
    if (!n->exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (n->id == MC_AFP_ROOT) return MC_AFP_ACCESS_DENIED;
    int e;
    if (n->st.dir) {
        if (offspring(v, n->path)) return MC_AFP_DIR_NOT_EMPTY;
        /* its own metadata files go with it */
        e = mc_host_rmdir(n->path);
        if (e == MC_AFP_DIR_NOT_EMPTY) {
            /* only hidden files of ours left (sidecars): they go */
            mc_host_dir *d = mc_host_opendir(n->path, &e);
            uint16_t name[MC_HOST_NAME], p[MC_HOST_PATH];
            mc_host_stat st;
            while (d && mc_host_readdir(d, name, &st))
                if (mc_meta_is_sidecar(name) && mc_host_join(p, MC_HOST_PATH, n->path, name)) mc_host_unlink(p);
            mc_host_closedir(d);
            e = mc_host_rmdir(n->path);
        }
    } else {
        if (fork_open(afp, v, n->id, -1)) return MC_AFP_FILE_BUSY;
        if (n->st.attributes & MC_HOST_READONLY) return MC_AFP_OBJECT_LOCKED;
        e = mc_host_unlink(n->path);
    }
    if (e == MC_HOST_BUSY) e = MC_AFP_FILE_BUSY;
    if (e) return e;
    if (v->meta == MC_META_APPLEDOUBLE) {
        uint16_t side[MC_HOST_PATH];
        if (mc_meta_sidecar(n->path, side, MC_HOST_PATH)) mc_host_unlink(side);
    }
    mc_desktop_remove_comment(v->dt, n->id);
    mc_ids_forget(v->ids, n->id);
    touch_dir(v, n->parent);
    return MC_AFP_OK;
}

/* Moves (and renames) a node into a directory under a new Macintosh name. */
static int32_t move_node(mc_afp *afp, volume *v, node *src, const node *dest_dir, const uint8_t *name, size_t nlen) {
    (void)afp;
    if (v->readonly) return MC_AFP_VOL_LOCKED;
    if (src->id == MC_AFP_ROOT) return MC_AFP_CANT_MOVE;
    if (!nlen) return MC_AFP_PARAM_ERR;
    node target;
    int e = child(v, dest_dir, name, nlen, &target);
    if (e) return e;
    const int same = target.exists && target.id == src->id;
    if (target.exists && !same) return MC_AFP_OBJECT_EXISTS;
    /* a directory cannot go into itself or below */
    if (src->st.dir) {
        for (uint32_t up = dest_dir->id; up && up != MC_AFP_ROOT_PARENT;) {
            if (up == src->id) return MC_AFP_CANT_MOVE;
            if (up == MC_AFP_ROOT) break;
            uint32_t parent;
            if (!mc_ids_lookup(v->ids, up, &parent, NULL, NULL)) break;
            up = parent;
        }
    }
    if (same && !memcmp(target.path, src->path, (mc_host_len(src->path) + 1) * 2)) return MC_AFP_OK;
    e = mc_host_rename(src->path, target.path);
    if (e == MC_HOST_BUSY) e = MC_AFP_FILE_BUSY;
    if (e == MC_AFP_ACCESS_DENIED && (src->st.attributes & MC_HOST_READONLY)) e = MC_AFP_OBJECT_LOCKED;
    if (e) return e;
    if (v->meta == MC_META_APPLEDOUBLE) {
        uint16_t a[MC_HOST_PATH], b[MC_HOST_PATH];
        if (mc_meta_sidecar(src->path, a, MC_HOST_PATH) && mc_meta_sidecar(target.path, b, MC_HOST_PATH)) mc_host_rename(a, b);
    }
    mc_ids_move(v->ids, src->id, dest_dir->id, target.name, src->st.index);
    touch_dir(v, src->parent);
    touch_dir(v, dest_dir->id);
    if (src->st.dir) touch_dir(v, src->id);
    return MC_AFP_OK;
}

static int32_t do_rename(session *s, reader *r) {
    volume *v;
    node n;
    int e = read_node(s, r, r16(r), &n, &v);
    if (e) return e;
    const unsigned ntype = r8(r);
    size_t nlen;
    const uint8_t *name = rstr(r, &nlen);
    if (r->bad || (ntype != 1 && ntype != 2) || !nlen) return MC_AFP_PARAM_ERR;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (n.id == MC_AFP_ROOT) return MC_AFP_CANT_RENAME;
    node parent;
    e = node_of_id(v, n.parent, &parent);
    if (e) return e;
    return move_node(s->afp, v, &n, &parent, name, nlen);
}

static int32_t do_move_and_rename(session *s, reader *r) {
    const unsigned vol_id = r16(r);
    const uint32_t sdir = r32(r), ddir = r32(r);
    const unsigned stype = r8(r);
    size_t slen, dlen, nlen;
    const uint8_t *spath = rstr(r, &slen);
    const unsigned dtype = r8(r);
    const uint8_t *dpath = rstr(r, &dlen);
    const unsigned ntype = r8(r);
    const uint8_t *name = rstr(r, &nlen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    (void)ntype;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    node src, dest;
    int e = resolve(v, sdir, (uint8_t)stype, spath, slen, &src);
    if (e) return e;
    if (!src.exists) return MC_AFP_OBJECT_NOT_FOUND;
    e = resolve(v, ddir, (uint8_t)dtype, dpath, dlen, &dest);
    if (e) return e;
    if (!dest.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (!dest.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    uint8_t keep[32];
    if (!nlen) {
        nlen = src.id == MC_AFP_ROOT ? 0 : mac_name(src.id, src.name, keep);
        name = keep;
    }
    return move_node(s->afp, v, &src, &dest, name, nlen);
}

static int32_t do_copy_file(session *s, reader *r) {
    const unsigned svol = r16(r);
    const uint32_t sdir = r32(r);
    const unsigned dvol = r16(r);
    const uint32_t ddir = r32(r);
    const unsigned stype = r8(r);
    size_t slen, dlen, nlen;
    const uint8_t *spath = rstr(r, &slen);
    const unsigned dtype = r8(r);
    const uint8_t *dpath = rstr(r, &dlen);
    r8(r);
    const uint8_t *name = rstr(r, &nlen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    volume *sv = vol_of(s, svol), *dv = vol_of(s, dvol);
    if (!sv || !dv) return MC_AFP_PARAM_ERR;
    node src, dest;
    int e = resolve(sv, sdir, (uint8_t)stype, spath, slen, &src);
    if (e) return e;
    if (!src.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (src.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    e = resolve(dv, ddir, (uint8_t)dtype, dpath, dlen, &dest);
    if (e) return e;
    if (!dest.exists || !dest.st.dir) return MC_AFP_OBJECT_NOT_FOUND;
    if (dv->readonly) return MC_AFP_VOL_LOCKED;
    if (deny_conflict(s->afp, sv, src.id, 0, M_READ) || deny_conflict(s->afp, sv, src.id, 1, M_READ)) return MC_AFP_DENY_CONFLICT;
    uint8_t keep[32];
    if (!nlen) {
        nlen = mac_name(src.id, src.name, keep);
        name = keep;
    }
    node target;
    e = child(dv, &dest, name, nlen, &target);
    if (e) return e;
    if (target.exists) return MC_AFP_OBJECT_EXISTS;
    e = mc_host_copy(src.path, target.path);
    if (e == MC_HOST_BUSY) e = MC_AFP_DENY_CONFLICT;
    if (e) return e;
    if (sv->meta != dv->meta || sv->meta == MC_META_APPLEDOUBLE) {
        /* metadata and resource fork through the destination's own layout */
        mc_afp_meta m;
        mc_meta_read(sv->meta, src.path, &m);
        mc_meta_write(dv->meta, target.path, &m);
        mc_rsrc *a, *b;
        if (mc_rsrc_open(sv->meta, src.path, 0, &a) == MC_AFP_OK) {
            uint64_t size = 0;
            mc_rsrc_size(a, &size);
            if (size && mc_rsrc_open(dv->meta, target.path, 1, &b) == MC_AFP_OK) {
                uint8_t buf[8192];
                for (uint64_t off = 0; off < size;) {
                    size_t got = 0;
                    if (mc_rsrc_read(a, off, buf, sizeof buf, &got) || !got) break;
                    mc_rsrc_write(b, off, buf, got);
                    off += got;
                }
                mc_rsrc_close(b);
            }
            mc_rsrc_close(a);
        }
    }
    mc_host_stat st;
    uint16_t real[MC_HOST_NAME];
    if (mc_host_stat_name(target.path, &st, real) == MC_AFP_OK) mc_ids_get(dv->ids, dest.id, real, st.index);
    touch_dir(dv, dest.id);
    return MC_AFP_OK;
}

/* Exchanges the forks and modification dates of two files; names, IDs,
 * creation dates and Finder information stay (FSpExchangeFiles). */
static int32_t do_exchange(session *s, reader *r) {
    const unsigned vol_id = r16(r);
    const uint32_t sdir = r32(r), ddir = r32(r);
    const unsigned stype = r8(r);
    size_t slen, dlen;
    const uint8_t *spath = rstr(r, &slen);
    const unsigned dtype = r8(r);
    const uint8_t *dpath = rstr(r, &dlen);
    if (r->bad) return MC_AFP_PARAM_ERR;
    if (s->version < 21) return MC_AFP_CALL_NOT_SUPPORTED;
    volume *v = vol_of(s, vol_id);
    if (!v) return MC_AFP_PARAM_ERR;
    if (v->readonly) return MC_AFP_VOL_LOCKED;
    node a, b;
    memset(&b, 0, sizeof b);
    int e = resolve(v, sdir, (uint8_t)stype, spath, slen, &a);
    if (!e) e = resolve(v, ddir, (uint8_t)dtype, dpath, dlen, &b);
    if (e) return e;
    if (!a.exists || !b.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (a.st.dir || b.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
    if (a.id == b.id) return MC_AFP_SAME_OBJECT;
    mc_afp_meta ma, mb;
    mc_meta_read(v->meta, a.path, &ma);
    mc_meta_read(v->meta, b.path, &mb);
    uint16_t tmp[MC_HOST_PATH];
    static const uint16_t suffix[] = {'.', 'm', 'a', 'c', 'e', '-', 'x', 0};
    const size_t al = mc_host_len(a.path);
    if (al + 8 > MC_HOST_PATH) return MC_AFP_PARAM_ERR;
    memcpy(tmp, a.path, al * 2);
    memcpy(tmp + al, suffix, sizeof suffix);
    uint16_t sa[MC_HOST_PATH], sb[MC_HOST_PATH], st_[MC_HOST_PATH];
    const int ad = v->meta == MC_META_APPLEDOUBLE && mc_meta_sidecar(a.path, sa, MC_HOST_PATH) && mc_meta_sidecar(b.path, sb, MC_HOST_PATH) &&
                   mc_meta_sidecar(tmp, st_, MC_HOST_PATH);
    e = mc_host_rename(a.path, tmp);
    if (!e) {
        e = mc_host_rename(b.path, a.path);
        if (e) mc_host_rename(tmp, a.path);
        else if ((e = mc_host_rename(tmp, b.path)) != 0) {
            mc_host_rename(a.path, b.path);
            mc_host_rename(tmp, a.path);
        }
    }
    if (e == MC_HOST_BUSY) e = MC_AFP_FILE_BUSY;
    if (e) return e;
    if (ad) {
        mc_host_rename(sa, st_);
        mc_host_rename(sb, sa);
        mc_host_rename(st_, sb);
    }
    /* Finder information, attributes and creation dates stay with the names. */
    ma.id = a.id;
    mb.id = b.id;
    mc_meta_write(v->meta, a.path, &ma);
    mc_meta_write(v->meta, b.path, &mb);
    mc_host_set_times(a.path, &a.st.created, NULL);
    mc_host_set_times(b.path, &b.st.created, NULL);
    mc_ids_move(v->ids, a.id, a.parent, a.name, b.st.index);
    mc_ids_move(v->ids, b.id, b.parent, b.name, a.st.index);
    /* open forks follow their data: swap the IDs they refer to */
    for (unsigned k = 0; k < FORKS; k++) {
        fork *f = &s->afp->forks[k];
        if (!f->used || f->vol != v) continue;
        if (f->id == a.id) f->id = b.id;
        else if (f->id == b.id) f->id = a.id;
    }
    touch_dir(v, a.parent);
    touch_dir(v, b.parent);
    return MC_AFP_OK;
}

static int32_t do_get_fork_parms(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    fork *f = fork_of(s, r16(r));
    const unsigned bitmap = r16(r);
    if (r->bad || !f) return MC_AFP_PARAM_ERR;
    if (bitmap & (f->rsrc ? B_DATA_LEN : B_RSRC_LEN)) return MC_AFP_BITMAP_ERR;
    node n;
    int e = node_of_id(f->vol, f->id, &n);
    if (e) return e;
    uint64_t size;
    if (!fork_size(f, &size) && !f->rsrc) n.st.size = size;
    put16(reply, bitmap);
    const size_t pl = pack_params(s->afp, &n, (uint16_t)bitmap, reply + 2, QUANTUM - 2, &e);
    if (e) return e;
    if (f->rsrc && (bitmap & B_RSRC_LEN)) {
        /* the open fork's own length */
        size_t at = 2;
        static const size_t width[16] = {2, 4, 4, 4, 4, 32, 2, 2, 4, 4, 4, 0, 0, 6, 0, 0};
        for (unsigned bit = 0; bit < 10; bit++)
            if (bitmap & 1u << bit) at += width[bit];
        if (!fork_size(f, &size)) put32(reply + at, clamp32(size));
    }
    *rlen = 2 + pl;
    return MC_AFP_OK;
}

static int32_t do_set_fork_parms(session *s, reader *r) {
    fork *f = fork_of(s, r16(r));
    const unsigned bitmap = r16(r);
    const uint32_t length = r32(r);
    if (r->bad || !f) return MC_AFP_PARAM_ERR;
    if (bitmap != (unsigned)(f->rsrc ? B_RSRC_LEN : B_DATA_LEN)) return MC_AFP_BITMAP_ERR;
    if (!(f->access & M_WRITE)) return MC_AFP_ACCESS_DENIED;
    if (length > 0x7FFFFFFF) return MC_AFP_PARAM_ERR;
    int e = fork_truncate(f, length);
    if (e == MC_HOST_BUSY) e = MC_AFP_LOCK_ERR;
    if (!e) f->written = 1;
    return e;
}

static int32_t do_get_srvr_parms(session *s, uint8_t *reply, size_t *rlen) {
    mc_afp *afp = s->afp;
    put32(reply, (uint32_t)now_afp());
    reply[4] = (uint8_t)afp->nvols;
    size_t at = 5;
    for (unsigned k = 0; k < afp->nvols; k++) {
        reply[at++] = 0;                 /* no volume password, no Apple II configuration */
        reply[at++] = (uint8_t)afp->vols[k]->name_len;
        memcpy(reply + at, afp->vols[k]->name, afp->vols[k]->name_len);
        at += afp->vols[k]->name_len;
    }
    *rlen = at;
    return MC_AFP_OK;
}

/* The desktop database's calls address a volume by its DTRefNum: the
 * volume's ID here. */
static volume *dt_of(session *s, unsigned ref) { return vol_of(s, ref); }

static int32_t do_add_appl(session *s, reader *r) {
    volume *v = dt_of(s, r16(r));
    const uint32_t dir = r32(r);
    uint8_t creator[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(r);
    const uint32_t tag = r32(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dir, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists || n.st.dir) return MC_AFP_OBJECT_NOT_FOUND;
    uint8_t name[32];
    const size_t nlen = mac_name(n.id, n.name, name);
    return mc_desktop_add_appl(v->dt, creator, n.parent, name, nlen, tag);
}

static int32_t do_remove_appl(session *s, reader *r) {
    volume *v = dt_of(s, r16(r));
    const uint32_t dir = r32(r);
    uint8_t creator[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dir, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    uint8_t name[32];
    const size_t nlen = mac_name(n.id, n.name, name);
    return mc_desktop_remove_appl(v->dt, creator, n.parent, name, nlen);
}

static int32_t do_get_appl(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    volume *v = dt_of(s, r16(r));
    uint8_t creator[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(r);
    unsigned index = r16(r);
    const unsigned bitmap = r16(r);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    if (!index) index = 1;
    /* the index-th application that still exists */
    for (unsigned k = 1;; k++) {
        uint32_t dir, tag;
        uint8_t name[32];
        size_t nlen;
        if (!mc_desktop_appl(v->dt, creator, k, &dir, name, &nlen, &tag)) return MC_AFP_ITEM_NOT_FOUND;
        node d, n;
        if (node_of_id(v, dir, &d) || child(v, &d, name, nlen, &n) || !n.exists) continue;
        if (--index) continue;
        put16(reply, bitmap);
        put32(reply + 2, tag);
        int e;
        const size_t pl = pack_params(s->afp, &n, (uint16_t)bitmap, reply + 6, QUANTUM - 6, &e);
        if (e) return e;
        *rlen = 6 + pl;
        return MC_AFP_OK;
    }
}

static int32_t do_comment(session *s, reader *r, int op, uint8_t *reply, size_t *rlen) {
    volume *v = dt_of(s, r16(r));
    const uint32_t dir = r32(r);
    const unsigned ptype = r8(r);
    size_t plen;
    const uint8_t *path = rstr(r, &plen);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    node n;
    int e = resolve(v, dir, (uint8_t)ptype, path, plen, &n);
    if (e) return e;
    if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
    if (op == FP_ADD_COMMENT) {
        ralign(r);
        size_t clen;
        const uint8_t *text = rstr(r, &clen);
        if (r->bad) return MC_AFP_PARAM_ERR;
        e = mc_desktop_set_comment(v->dt, n.id, text, clen);
        return e;
    }
    if (op == FP_REMOVE_COMMENT) return mc_desktop_remove_comment(v->dt, n.id);
    size_t clen;
    const uint8_t *text = mc_desktop_comment(v->dt, n.id, &clen);
    if (!text) return MC_AFP_ITEM_NOT_FOUND;
    reply[0] = (uint8_t)clen;
    memcpy(reply + 1, text, clen);
    *rlen = 1 + clen;
    return MC_AFP_OK;
}

static int32_t do_get_icon(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    volume *v = dt_of(s, r16(r));
    uint8_t creator[4], type[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(r);
    for (unsigned k = 0; k < 4; k++) type[k] = (uint8_t)r8(r);
    const unsigned itype = r8(r);
    r8(r);
    const unsigned length = r16(r);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    size_t size;
    const uint8_t *bitmap = mc_desktop_icon(v->dt, creator, type, (uint8_t)itype, &size, NULL);
    if (!bitmap) return MC_AFP_ITEM_NOT_FOUND;
    const size_t n = size < length ? size : length;
    memcpy(reply, bitmap, n);
    *rlen = n;
    return MC_AFP_OK;
}

static int32_t do_get_icon_info(session *s, reader *r, uint8_t *reply, size_t *rlen) {
    volume *v = dt_of(s, r16(r));
    uint8_t creator[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(r);
    const unsigned index = r16(r);
    if (r->bad || !v) return MC_AFP_PARAM_ERR;
    uint8_t type[4], itype;
    uint32_t tag;
    size_t size;
    if (!mc_desktop_icon_info(v->dt, creator, index, type, &itype, &tag, &size)) return MC_AFP_ITEM_NOT_FOUND;
    put32(reply, tag);
    memcpy(reply + 4, type, 4);
    reply[8] = itype;
    reply[9] = 0;
    put16(reply + 10, (unsigned)size);
    *rlen = 12;
    return MC_AFP_OK;
}

static int32_t do_add_icon(session *s, const uint8_t *cmd, size_t len, const uint8_t *data, size_t dlen) {
    reader r = {cmd, len, 2, 0};
    volume *v = dt_of(s, r16(&r));
    uint8_t creator[4], type[4];
    for (unsigned k = 0; k < 4; k++) creator[k] = (uint8_t)r8(&r);
    for (unsigned k = 0; k < 4; k++) type[k] = (uint8_t)r8(&r);
    const unsigned itype = r8(&r);
    r8(&r);
    const uint32_t tag = r32(&r);
    const unsigned size = r16(&r);
    if (r.bad || !v) return MC_AFP_PARAM_ERR;
    if (dlen < size) return MC_AFP_PARAM_ERR;
    return mc_desktop_add_icon(v->dt, creator, type, (uint8_t)itype, tag, data, size);
}

static int32_t do_map_id(session *s, reader *r, unsigned sub, uint8_t *reply, size_t *rlen) {
    const uint32_t id = r32(r);
    (void)s;
    if (r->bad || (sub != 1 && sub != 2)) return MC_AFP_PARAM_ERR;
    if (id) return MC_AFP_ITEM_NOT_FOUND;
    reply[0] = 0;
    *rlen = 1;
    return MC_AFP_OK;
}

static int32_t do_map_name(session *s, reader *r, unsigned sub, uint8_t *reply, size_t *rlen) {
    size_t n;
    rstr(r, &n);
    (void)s;
    if (r->bad || (sub != 3 && sub != 4)) return MC_AFP_PARAM_ERR;
    if (n) return MC_AFP_ITEM_NOT_FOUND;
    put32(reply, 0);
    *rlen = 4;
    return MC_AFP_OK;
}

int32_t mc_afp_command(mc_afp *afp, void *p, const uint8_t *cmd, size_t length, uint8_t *reply, size_t *rlen) {
    session *s = p;
    *rlen = 0;
    if (!length) return MC_AFP_PARAM_ERR;
    const unsigned op = cmd[0];
    reader r = {cmd, length, 1, 0};
    if (op == FP_GET_SRVR_INFO) {
        *rlen = mc_afp_status(afp, reply, QUANTUM);
        return MC_AFP_OK;
    }
    if (op == FP_LOGIN) return do_login(s, &r, reply, rlen);
    if (op == FP_LOGIN_CONT) return do_login_cont(s, &r);
    if (!s->logged_in) return MC_AFP_USER_NOT_AUTH;
    const unsigned flag = r8(&r);
    switch (op) {
    case FP_LOGOUT: end_session(s); return MC_AFP_OK;
    case FP_GET_SRVR_PARMS: return do_get_srvr_parms(s, reply, rlen);
    case FP_OPEN_VOL: return do_open_vol(s, &r, reply, rlen);
    case FP_CLOSE_VOL: {
        const unsigned id = r16(&r);
        volume *v = vol_of(s, id);
        if (!v) return MC_AFP_PARAM_ERR;
        for (unsigned k = 0; k < FORKS; k++)
            if (afp->forks[k].used && afp->forks[k].owner == s && afp->forks[k].vol == v) close_fork(&afp->forks[k]);
        s->open_vols &= ~(1u << (id - 1));
        return MC_AFP_OK;
    }
    case FP_GET_VOL_PARMS: {
        const unsigned id = r16(&r), bitmap = r16(&r);
        volume *v = vol_of(s, id);
        if (r.bad || !v) return MC_AFP_PARAM_ERR;
        const int32_t e = vol_params(afp, v, bitmap, reply, rlen);
        if (!e) set_vol_id(reply, bitmap, id);
        return e;
    }
    case FP_SET_VOL_PARMS: {
        volume *v = vol_of(s, r16(&r));
        const unsigned bitmap = r16(&r);
        const uint32_t backup = r32(&r);
        if (r.bad || !v) return MC_AFP_PARAM_ERR;
        if (bitmap != 0x10) return MC_AFP_BITMAP_ERR;
        if (v->readonly) return MC_AFP_VOL_LOCKED;
        mc_afp_meta m;
        mc_meta_read(v->meta, v->root, &m);
        m.backup = (int32_t)backup;
        m.id = MC_AFP_ROOT;
        return mc_meta_write(v->meta, v->root, &m);
    }
    case FP_FLUSH: {
        volume *v = vol_of(s, r16(&r));
        if (r.bad || !v) return MC_AFP_PARAM_ERR;
        for (unsigned k = 0; k < FORKS; k++)
            if (afp->forks[k].used && afp->forks[k].vol == v && afp->forks[k].owner == s) fork_flush(&afp->forks[k]);
        return MC_AFP_OK;
    }
    case FP_GET_FILE_DIR_PARMS: return do_get_file_dir_parms(s, &r, reply, rlen);
    case FP_SET_FILE_DIR_PARMS: return do_set_parms(s, &r, 0);
    case FP_SET_FILE_PARMS: return do_set_parms(s, &r, 1);
    case FP_SET_DIR_PARMS: return do_set_parms(s, &r, 2);
    case FP_ENUMERATE: return do_enumerate(s, &r, reply, rlen);
    case FP_OPEN_DIR: {
        node n;
        const int e = read_node(s, &r, r16(&r), &n, NULL);
        if (e) return e;
        if (!n.exists) return MC_AFP_OBJECT_NOT_FOUND;
        if (!n.st.dir) return MC_AFP_OBJECT_TYPE_ERR;
        put32(reply, n.id);
        *rlen = 4;
        return MC_AFP_OK;
    }
    case FP_CLOSE_DIR: return MC_AFP_OK;
    case FP_CREATE_DIR: return do_create(s, &r, 1, flag, reply, rlen);
    case FP_CREATE_FILE: return do_create(s, &r, 0, flag, reply, rlen);
    case FP_DELETE: {
        volume *v;
        node n;
        const int e = read_node(s, &r, r16(&r), &n, &v);
        return e ? e : delete_node(afp, v, &n);
    }
    case FP_RENAME: return do_rename(s, &r);
    case FP_MOVE_AND_RENAME: return do_move_and_rename(s, &r);
    case FP_COPY_FILE: return do_copy_file(s, &r);
    case FP_EXCHANGE_FILES: return do_exchange(s, &r);
    case FP_OPEN_FORK: return do_open_fork(s, &r, flag, reply, rlen);
    case FP_CLOSE_FORK: {
        fork *f = fork_of(s, r16(&r));
        if (r.bad || !f) return MC_AFP_PARAM_ERR;
        close_fork(f);
        return MC_AFP_OK;
    }
    case FP_READ: return do_read(s, &r, reply, rlen);
    case FP_FLUSH_FORK: {
        fork *f = fork_of(s, r16(&r));
        if (r.bad || !f) return MC_AFP_PARAM_ERR;
        if (f->written) {
            node n;
            if (node_of_id(f->vol, f->id, &n) == MC_AFP_OK) {
                const int64_t now = mc_host_now();
                mc_host_set_times(n.path, NULL, &now);
            }
        }
        return fork_flush(f) == MC_HOST_BUSY ? MC_AFP_LOCK_ERR : MC_AFP_OK;
    }
    case FP_BYTE_RANGE_LOCK: return do_byte_range_lock(s, &r, flag, reply, rlen);
    case FP_GET_FORK_PARMS: return do_get_fork_parms(s, &r, reply, rlen);
    case FP_SET_FORK_PARMS: return do_set_fork_parms(s, &r);
    case FP_MAP_ID: return do_map_id(s, &r, flag, reply, rlen);
    case FP_MAP_NAME: return do_map_name(s, &r, flag, reply, rlen);
    case FP_OPEN_DT: {
        const unsigned id = r16(&r);
        if (r.bad || !vol_of(s, id)) return MC_AFP_PARAM_ERR;
        put16(reply, id);
        *rlen = 2;
        return MC_AFP_OK;
    }
    case FP_CLOSE_DT: return dt_of(s, r16(&r)) ? MC_AFP_OK : MC_AFP_PARAM_ERR;
    case FP_GET_ICON: return do_get_icon(s, &r, reply, rlen);
    case FP_GET_ICON_INFO: return do_get_icon_info(s, &r, reply, rlen);
    case FP_ADD_APPL: return do_add_appl(s, &r);
    case FP_REMOVE_APPL: return do_remove_appl(s, &r);
    case FP_GET_APPL: return do_get_appl(s, &r, reply, rlen);
    case FP_ADD_COMMENT:
    case FP_REMOVE_COMMENT:
    case FP_GET_COMMENT: return do_comment(s, &r, (int)op, reply, rlen);
    case FP_GET_SRVR_MSG: {
        const unsigned type = r16(&r), bitmap = r16(&r);
        if (r.bad) return MC_AFP_PARAM_ERR;
        if (s->version < 21) return MC_AFP_CALL_NOT_SUPPORTED;
        if (bitmap & ~1u) return MC_AFP_BITMAP_ERR;
        put16(reply, type);
        put16(reply + 2, bitmap);
        reply[4] = 0;
        *rlen = 5;
        return MC_AFP_OK;
    }
    default: return MC_AFP_CALL_NOT_SUPPORTED;
    }
}

long mc_afp_write_begin(mc_afp *afp, void *p, const uint8_t *cmd, size_t length, int32_t *result, uint8_t *reply, size_t *rlen) {
    (void)afp;
    (void)reply;
    session *s = p;
    *rlen = 0;
    if (!length) { *result = MC_AFP_PARAM_ERR; return -1; }
    if (!s->logged_in) { *result = MC_AFP_USER_NOT_AUTH; return -1; }
    if (cmd[0] == FP_WRITE) return write_begin(s, cmd, length, result);
    if (cmd[0] == FP_ADD_ICON) {
        if (length < 20) { *result = MC_AFP_PARAM_ERR; return -1; }
        if (!dt_of(s, get16(cmd + 2))) { *result = MC_AFP_PARAM_ERR; return -1; }
        return get16(cmd + 18);
    }
    *result = MC_AFP_CALL_NOT_SUPPORTED;
    return -1;
}

int32_t mc_afp_write_end(mc_afp *afp, void *p, const uint8_t *cmd, size_t length, const uint8_t *data, size_t dlen, uint8_t *reply,
                         size_t *rlen) {
    (void)afp;
    session *s = p;
    *rlen = 0;
    if (cmd[0] == FP_WRITE) return write_end(s, cmd, length, data, dlen, reply, rlen);
    if (cmd[0] == FP_ADD_ICON) return do_add_icon(s, cmd, length, data, dlen);
    return MC_AFP_CALL_NOT_SUPPORTED;
}

/* ---- host changes ------------------------------------------------------------------- */

void mc_afp_host_changed(mc_afp *afp, unsigned volume_index, const uint16_t *relative) {
    if (volume_index >= afp->nvols) return;
    volume *v = afp->vols[volume_index];
    /* our own databases change with every ID handed out: no news */
    const size_t n = mc_host_len(relative);
    size_t slash = n;
    while (slash && relative[slash - 1] != '\\') slash--;
    if (hidden_name(relative + slash, NULL) && !mc_meta_is_sidecar(relative + slash)) return;
    /* the parent folder of what changed */
    uint32_t id = MC_AFP_ROOT;
    size_t at = 0;
    while (at < (slash ? slash - 1 : 0)) {
        uint16_t part[MC_HOST_NAME];
        size_t k = 0;
        while (at < slash - 1 && relative[at] != '\\' && k + 1 < MC_HOST_NAME) part[k++] = relative[at++];
        part[k] = 0;
        if (at < slash && relative[at] == '\\') at++;
        const uint32_t next = mc_ids_find(v->ids, id, part);
        if (!next) break;            /* a folder the Macintosh has not seen: the nearest one it has */
        id = next;
    }
    touch_dir(v, id);
}

typedef struct watch_ctx {
    mc_afp *afp;
    unsigned volume;
} watch_ctx;

static void watched(void *ctx, const uint16_t *relative) {
    const watch_ctx *w = ctx;
    mc_afp_host_changed(w->afp, w->volume, relative);
}

void mc_afp_poll(mc_afp *afp) {
    for (unsigned k = 0; k < afp->nvols; k++) {
        watch_ctx w = {afp, k};
        mc_host_watch_poll(afp->vols[k]->watch, watched, &w);
    }
}

/* ---- the server ------------------------------------------------------------------------ */

const char *mc_afp_server_name(const mc_afp *afp, size_t *length) {
    *length = afp->name_len;
    return (const char *)afp->name;
}

int mc_afp_volume_metadata(const mc_afp *afp, unsigned k, const char **note) {
    if (k >= afp->nvols) return -1;
    if (note) *note = afp->vols[k]->note;
    return afp->vols[k]->meta;
}

static void state_path(const mc_afp_config *c, const volume *v, const uint16_t *file, uint16_t *out, size_t capacity) {
    uint16_t base[MC_HOST_PATH];
    if (c->state_folder && c->state_folder[0]) mc_host_root(c->state_folder, base, MC_HOST_PATH);
    else {
#ifdef _WIN32
        const char *local = getenv("LOCALAPPDATA");
        char folder[MC_AFP_PATH];
        snprintf(folder, sizeof folder, "%s/Livadix/afp", local ? local : ".");
#else
        /* the user's state folder (XDG Base Directory Specification) */
        const char *state = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
        char folder[MC_AFP_PATH], made[MC_AFP_PATH];
        if (state && state[0] == '/') snprintf(folder, sizeof folder, "%s/Livadix/afp", state);
        else snprintf(folder, sizeof folder, "%s/.local/state/Livadix/afp", home ? home : ".");
        /* mc_host_root takes an existing folder: make each level first */
        for (char *s = folder + 1; *s; s++)
            if (*s == '/') { snprintf(made, sizeof made, "%.*s", (int)(s - folder), folder); mkdir(made, 0700); }
        mkdir(folder, 0700);
#endif
        mc_host_root(folder, base, MC_HOST_PATH);
    }
    /* one folder per shared folder, named by a hash of its path */
    uint64_t h = 0xCBF29CE484222325ULL;
    for (const uint16_t *p = v->root; *p; p++) h = (h ^ (uint16_t)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p)) * 0x100000001B3ULL;
    char hex[17];
    snprintf(hex, sizeof hex, "%016llx", (unsigned long long)h);
    uint16_t sub[32];
    mc_host_from_utf8(hex, sub, 32);
    uint16_t dir[MC_HOST_PATH];
    /* create base\Livadix\afp\<hash> one level at a time */
    mc_host_copy_str(dir, base, MC_HOST_PATH);
    uint16_t walk[MC_HOST_PATH];
    size_t n = mc_host_len(dir);
    for (size_t k = 7; k <= n; k++)
        if (k == n || dir[k] == '\\') {
            memcpy(walk, dir, k * 2);
            walk[k] = 0;
            mc_host_mkdir(walk);
        }
    mc_host_join(dir, MC_HOST_PATH, dir, sub);
    mc_host_mkdir(dir);
    mc_host_join(out, capacity, dir, file);
}

mc_afp *mc_afp_create(const mc_afp_config *c, char *error, size_t capacity) {
    if (capacity) error[0] = 0;
    mc_afp *afp = calloc(1, sizeof *afp);
    if (!afp) {
        snprintf(error, capacity, "Out of memory");
        return NULL;
    }
    afp->log = c->log;
    afp->log_ctx = c->log_ctx;
    afp->seed = (uint32_t)mc_host_now() ^ 0x4C564458u;
    const int nl = utf8_to_mac(c->server_name[0] ? c->server_name : "Livadix", afp->name, 31);
    if (nl <= 0) {
        snprintf(error, capacity, "sharing.name must be 1 to 31 characters of the Macintosh character set, without ':'");
        free(afp);
        return NULL;
    }
    afp->name_len = (size_t)nl;
    const size_t pl = strlen(c->password);
    if (pl > 8) {
        snprintf(error, capacity, "sharing.password has at most 8 characters");
        free(afp);
        return NULL;
    }
    memcpy(afp->password, c->password, pl);
    afp->has_password = pl > 0;
    for (size_t k = 0; k < c->volume_count && k < MC_AFP_VOLUMES; k++) {
        const mc_afp_volume_config *vc = &c->volumes[k];
        volume *v = calloc(1, sizeof *v);
        if (!v) {
            snprintf(error, capacity, "Out of memory");
            mc_afp_destroy(afp);
            return NULL;
        }
        afp->vols[afp->nvols++] = v;
        const int n = utf8_to_mac(vc->name, v->name, 27);
        if (n <= 0) {
            snprintf(error, capacity, "sharing.folders: the name \"%s\" must be 1 to 27 characters of the Macintosh character set, without ':'",
                     vc->name);
            mc_afp_destroy(afp);
            return NULL;
        }
        v->name_len = (size_t)n;
        for (size_t j = 0; j + 1 < afp->nvols; j++)
            if (mc_mac_name_equal(afp->vols[j]->name, afp->vols[j]->name_len, v->name, v->name_len)) {
                snprintf(error, capacity, "sharing.folders: two folders are named \"%s\"", vc->name);
                mc_afp_destroy(afp);
                return NULL;
            }
        mc_host_stat st;
        if (!mc_host_root(vc->path, v->root, MC_HOST_PATH) || mc_host_stat_path(v->root, &st) || !st.dir) {
            snprintf(error, capacity, "sharing.folders: \"%s\" is not a folder", vc->path);
            mc_afp_destroy(afp);
            return NULL;
        }
        v->readonly = vc->readonly;
        v->created = mc_afp_time(st.created);
        v->modified = now_afp();
        char fs[32];
        uint64_t total, free_bytes;
        mc_host_volume(v->root, &total, &free_bytes, fs, sizeof fs);
        v->meta = c->metadata;
        if (v->meta == MC_META_STREAMS && strcmp(fs, "NTFS") && strcmp(fs, "ReFS")) {
            v->meta = MC_META_APPLEDOUBLE;
            snprintf(v->note, sizeof v->note, "the folder of \"%s\" is on %s, which has no named streams: Macintosh metadata goes in AppleDouble ._ files",
                     vc->name, fs[0] ? fs : "a file system");
            logf_(afp, "sharing: %s", v->note);
        }
        uint16_t ids_path[MC_HOST_PATH], dt_path[MC_HOST_PATH];
        if (v->readonly) {
            state_path(c, v, name_ids, ids_path, MC_HOST_PATH);
            state_path(c, v, name_desktop, dt_path, MC_HOST_PATH);
        } else {
            mc_host_join(ids_path, MC_HOST_PATH, v->root, name_ids);
            mc_host_join(dt_path, MC_HOST_PATH, v->root, name_desktop);
        }
        v->ids = mc_ids_open(ids_path, st.index);
        v->dt = mc_desktop_open(dt_path);
        v->watch = mc_host_watch_start(v->root);
        if (!v->ids || !v->dt) {
            snprintf(error, capacity, "Out of memory");
            mc_afp_destroy(afp);
            return NULL;
        }
    }
    return afp;
}

void mc_afp_destroy(mc_afp *afp) {
    if (!afp) return;
    for (unsigned k = 0; k < FORKS; k++)
        if (afp->forks[k].used) close_fork(&afp->forks[k]);
    for (size_t k = 0; k < afp->nvols; k++) {
        volume *v = afp->vols[k];
        mc_host_watch_stop(v->watch);
        mc_ids_close(v->ids);
        mc_desktop_close(v->dt);
        free(v);
    }
    free(afp);
}
