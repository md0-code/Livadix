/* Livadix — the host's file system for the AFP server on Linux (afp_host.h).
 *
 * The server keeps its paths as on Windows: UTF-16 with '\' between the
 * components ("\home\user\Shared\Folder"); each call here turns one into
 * the UTF-8 path with '/' the kernel takes. A name the Macintosh gives
 * never holds '\' or '/' (macroman.c maps both to private code points), and
 * a Linux name that holds '\' is left out of listings, so the separator
 * stays unambiguous. Names compare without regard to case, as on the
 * Macintosh and on Windows: a name not found as given is looked for in its
 * folder without case (mc_host_stat_name gives the name as stored).
 *
 * Sources: POSIX.1-2017 (open, pread, pwrite, ftruncate, fsync, mkdir,
 * rmdir, unlink, opendir/readdir, statvfs, utimensat, realpath, nftw) and
 * the Linux manual pages statx(2) (birth time), renameat2(2)
 * (RENAME_NOREPLACE), statfs(2) (file system types), inotify(7),
 * copy_file_range(2).
 *
 * Livadix's own code. */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "afp_host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

size_t mc_host_to_utf8(const uint16_t *s, char *out, size_t capacity);
size_t mc_host_from_utf8(const char *s, uint16_t *out, size_t capacity);
void mc_host_copy_str(uint16_t *dst, const uint16_t *src, size_t capacity);

/* --- paths ------------------------------------------------------------------------- */

#define NATIVE (MC_HOST_PATH * 3 + 4)

/* The kernel's form of a server path: UTF-8, '/' between the components. */
static int native(const uint16_t *path, char *out) {
    if (!path[0]) return 0;
    const size_t n = mc_host_to_utf8(path, out, NATIVE);
    if (n + 1 >= NATIVE) return 0;
    for (size_t k = 0; k < n; k++)
        if (out[k] == '\\') out[k] = '/';
    return 1;
}

static int error_of(int e) {
    switch (e) {
    case ENOENT:
    case ENOTDIR:
    case ENAMETOOLONG:
    case ELOOP: return MC_AFP_OBJECT_NOT_FOUND;
    case EACCES:
    case EPERM: return MC_AFP_ACCESS_DENIED;
    case EBUSY:
    case ETXTBSY: return MC_HOST_BUSY;
    case EEXIST: return MC_AFP_OBJECT_EXISTS;
    case ENOSPC:
    case EDQUOT:
    case EFBIG: return MC_AFP_DISK_FULL;
    case ENOTEMPTY: return MC_AFP_DIR_NOT_EMPTY;
    case EROFS: return MC_AFP_VOL_LOCKED;
    case EMFILE:
    case ENFILE: return MC_AFP_TOO_MANY_FILES_OPEN;
    default: return MC_AFP_MISC_ERR;
    }
}
static int last_error(void) { return error_of(errno); }

/* Case folding of the name comparisons: ASCII and Latin-1 letters, as the
 * Macintosh's own comparison treats the Roman letters. */
static uint16_t fold(uint16_t c) {
    if (c >= 'A' && c <= 'Z') return (uint16_t)(c + 32);
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return (uint16_t)(c + 32);
    return c;
}

/* --- status ------------------------------------------------------------------------ */

static void fill_stat(mc_host_stat *st, const struct statx *x, const char *name) {
    st->dir = S_ISDIR(x->stx_mode);
    st->size = st->dir ? 0 : x->stx_size;
    st->index = x->stx_ino;
    st->modified = x->stx_mtime.tv_sec;
    /* the birth time where the file system keeps one, else the last change */
    st->created = (x->stx_mask & STATX_BTIME) ? x->stx_btime.tv_sec : x->stx_mtime.tv_sec;
    st->attributes = (x->stx_mode & S_IWUSR) ? 0 : MC_HOST_READONLY;
    /* a dot name is hidden on Linux as a hidden system file is on Windows: the Macintosh does not see it */
    if (name && name[0] == '.') st->attributes |= MC_HOST_HIDDEN | MC_HOST_SYSTEM;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int stat_native(const char *path, mc_host_stat *st) {
    struct statx x;
    if (statx(AT_FDCWD, path, 0, STATX_BASIC_STATS | STATX_BTIME, &x)) return last_error();
    fill_stat(st, &x, base_name(path));
    return MC_AFP_OK;
}

/* The entry of a folder whose name equals name without case; 1 with the
 * stored name in out. */
static int find_folded(const char *folder, const char *name, char *out, size_t capacity) {
    uint16_t want[MC_HOST_NAME], have[MC_HOST_NAME];
    mc_host_from_utf8(name, want, MC_HOST_NAME);
    DIR *d = opendir(folder[0] ? folder : "/");
    if (!d) return 0;
    int found = 0;
    for (struct dirent *e; !found && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        mc_host_from_utf8(e->d_name, have, MC_HOST_NAME);
        size_t k = 0;
        while (want[k] && fold(want[k]) == fold(have[k])) k++;
        if (!want[k] && !have[k] && strlen(e->d_name) < capacity) {
            strcpy(out, e->d_name);
            found = 1;
        }
    }
    closedir(d);
    return found;
}

/* A path as given, else with each component that is not found replaced by
 * the one of its folder that equals it without case. 1 when it exists. */
static int resolve(char *path) {
    struct stat s;
    if (!lstat(path, &s)) return 1;
    if (errno != ENOENT) return 0;
    char built[NATIVE], name[NAME_MAX + 1];
    size_t n = 0;
    built[0] = 0;
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *end = strchr(p, '/');
        const size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len > NAME_MAX || n + 1 + len + 1 > sizeof built) return 0;
        built[n] = '/';
        memcpy(built + n + 1, p, len);
        built[n + 1 + len] = 0;
        if (lstat(built, &s)) {
            char folded[NAME_MAX + 1];
            memcpy(name, p, len);
            name[len] = 0;
            built[n] = 0;
            if (errno != ENOENT || !find_folded(built, name, folded, sizeof folded)) return 0;
            const size_t flen = strlen(folded);
            if (n + 1 + flen + 1 > sizeof built) return 0;
            built[n] = '/';
            memcpy(built + n + 1, folded, flen + 1);
            n += 1 + flen;
        } else n += 1 + len;
        p += len;
    }
    strcpy(path, built);
    return 1;
}

int mc_host_stat_path(const uint16_t *path, mc_host_stat *st) {
    char p[NATIVE];
    if (!native(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    const int e = stat_native(p, st);
    if (e != MC_AFP_OBJECT_NOT_FOUND || !resolve(p)) return e;
    return stat_native(p, st);
}

int mc_host_stat_name(const uint16_t *path, mc_host_stat *st, uint16_t name[MC_HOST_NAME]) {
    char p[NATIVE];
    if (!native(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    if (!resolve(p)) return errno == EACCES ? MC_AFP_ACCESS_DENIED : MC_AFP_OBJECT_NOT_FOUND;
    const int e = stat_native(p, st);
    if (e) return e;
    mc_host_from_utf8(base_name(p), name, MC_HOST_NAME);
    return MC_AFP_OK;
}

/* The path itself, or the one that equals it without case (for the calls
 * that open what exists). */
static int existing(const uint16_t *path, char *p) {
    if (!native(path, p)) return 0;
    resolve(p);
    return 1;
}

/* --- folders ----------------------------------------------------------------------- */

struct mc_host_dir {
    DIR *d;
    int fd;
};

mc_host_dir *mc_host_opendir(const uint16_t *path, int *error) {
    char p[NATIVE];
    if (!existing(path, p)) { *error = MC_AFP_OBJECT_NOT_FOUND; return NULL; }
    mc_host_dir *d = calloc(1, sizeof *d);
    if (!d) { *error = MC_AFP_MISC_ERR; return NULL; }
    d->d = opendir(p);
    if (!d->d) {
        *error = last_error();
        free(d);
        return NULL;
    }
    d->fd = dirfd(d->d);
    return d;
}

int mc_host_readdir(mc_host_dir *d, uint16_t name[MC_HOST_NAME], mc_host_stat *st) {
    for (struct dirent *e; (e = readdir(d->d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (strchr(e->d_name, '\\')) continue;            /* not a name the server can address */
        if (mc_host_from_utf8(e->d_name, name, MC_HOST_NAME) + 1 >= MC_HOST_NAME) continue;
        struct statx x;
        if (statx(d->fd, e->d_name, 0, STATX_BASIC_STATS | STATX_BTIME, &x)) continue;   /* a dangling link */
        if (!S_ISREG(x.stx_mode) && !S_ISDIR(x.stx_mode)) continue;   /* sockets, devices, pipes */
        fill_stat(st, &x, e->d_name);
        return 1;
    }
    return 0;
}

void mc_host_closedir(mc_host_dir *d) {
    if (!d) return;
    closedir(d->d);
    free(d);
}

/* --- files ------------------------------------------------------------------------- */

struct mc_host_file {
    int fd;
};

int mc_host_open(const uint16_t *path, int write, int how, mc_host_file **out) {
    char p[NATIVE];
    if (!native(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    if (how != MC_HOST_CREATE_NEW) resolve(p);
    const int flags = (write ? O_RDWR : O_RDONLY) | O_CLOEXEC |
                      (how == MC_HOST_CREATE_NEW ? O_CREAT | O_EXCL : how == MC_HOST_OPEN_ALWAYS ? O_CREAT : 0);
    const int fd = open(p, flags, 0666);
    if (fd < 0) return last_error();
    struct stat s;
    if (fstat(fd, &s) || S_ISDIR(s.st_mode)) {
        close(fd);
        return MC_AFP_OBJECT_TYPE_ERR;
    }
    mc_host_file *f = malloc(sizeof *f);
    if (!f) { close(fd); return MC_AFP_MISC_ERR; }
    f->fd = fd;
    *out = f;
    return MC_AFP_OK;
}

int mc_host_read(mc_host_file *f, uint64_t offset, void *buffer, size_t length, size_t *got) {
    *got = 0;
    for (;;) {
        const ssize_t n = pread(f->fd, buffer, length, (off_t)offset);
        if (n >= 0) { *got = (size_t)n; return MC_AFP_OK; }
        if (errno != EINTR) return last_error();
    }
}

int mc_host_write(mc_host_file *f, uint64_t offset, const void *buffer, size_t length) {
    const uint8_t *b = buffer;
    while (length) {
        const ssize_t n = pwrite(f->fd, b, length, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            return last_error();
        }
        if (n == 0) return MC_AFP_DISK_FULL;
        b += n;
        offset += (uint64_t)n;
        length -= (size_t)n;
    }
    return MC_AFP_OK;
}

int mc_host_size(mc_host_file *f, uint64_t *size) {
    struct stat s;
    if (fstat(f->fd, &s)) return last_error();
    *size = (uint64_t)s.st_size;
    return MC_AFP_OK;
}

int mc_host_truncate(mc_host_file *f, uint64_t size) { return ftruncate(f->fd, (off_t)size) ? last_error() : MC_AFP_OK; }

int mc_host_flush(mc_host_file *f) { return fsync(f->fd) ? last_error() : MC_AFP_OK; }

void mc_host_close(mc_host_file *f) {
    if (!f) return;
    close(f->fd);
    free(f);
}

int mc_host_stream_size(const uint16_t *path, uint64_t *size) {
    char p[NATIVE];
    struct stat s;
    *size = 0;
    if (!existing(path, p)) return MC_AFP_OK;
    if (stat(p, &s)) return errno == ENOENT || errno == ENOTDIR ? MC_AFP_OK : last_error();
    *size = S_ISDIR(s.st_mode) ? 0 : (uint64_t)s.st_size;
    return MC_AFP_OK;
}

/* --- changes ----------------------------------------------------------------------- */

int mc_host_mkdir(const uint16_t *path) {
    char p[NATIVE];
    if (!native(path, p)) return MC_AFP_PARAM_ERR;
    return mkdir(p, 0777) ? last_error() : MC_AFP_OK;
}

int mc_host_rmdir(const uint16_t *path) {
    char p[NATIVE];
    if (!existing(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    return rmdir(p) ? last_error() : MC_AFP_OK;
}

int mc_host_unlink(const uint16_t *path) {
    char p[NATIVE];
    if (!existing(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    return unlink(p) ? last_error() : MC_AFP_OK;
}

int mc_host_rename(const uint16_t *from, const uint16_t *to) {
    char a[NATIVE], b[NATIVE];
    if (!existing(from, a) || !native(to, b)) return MC_AFP_OBJECT_NOT_FOUND;
    /* never over an existing name, as MoveFileEx without replacing: a name
     * that differs from the source only in case is the source itself */
    if (!renameat2(AT_FDCWD, a, AT_FDCWD, b, RENAME_NOREPLACE)) return MC_AFP_OK;
    if (errno == EEXIST) {
        struct stat sa, sb;
        if (!lstat(a, &sa) && !lstat(b, &sb) && sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino && strcmp(a, b))
            return rename(a, b) ? last_error() : MC_AFP_OK;
        return MC_AFP_OBJECT_EXISTS;
    }
    if (errno == EINVAL || errno == ENOSYS) {   /* a file system without RENAME_NOREPLACE */
        struct stat s;
        if (!lstat(b, &s)) return MC_AFP_OBJECT_EXISTS;
        return rename(a, b) ? last_error() : MC_AFP_OK;
    }
    return last_error();
}

int mc_host_copy(const uint16_t *from, const uint16_t *to) {
    char a[NATIVE], b[NATIVE];
    if (!existing(from, a) || !native(to, b)) return MC_AFP_OBJECT_NOT_FOUND;
    const int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) return last_error();
    struct stat s;
    if (fstat(in, &s)) { const int e = last_error(); close(in); return e; }
    const int out = open(b, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, s.st_mode & 0777);
    if (out < 0) { const int e = last_error(); close(in); return e; }
    int e = MC_AFP_OK;
    for (off_t left = s.st_size; left > 0 && !e;) {
        ssize_t n = copy_file_range(in, NULL, out, NULL, (size_t)left, 0);
        if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
            /* copy_file_range across or on a file system that has none: by hand */
            char buffer[64 * 1024];
            n = read(in, buffer, sizeof buffer);
            if (n > 0 && write(out, buffer, (size_t)n) != n) n = -1;
        }
        if (n < 0) { if (errno != EINTR) e = last_error(); }
        else if (n == 0) break;
        else left -= n;
    }
    const struct timespec times[2] = {s.st_atim, s.st_mtim};
    futimens(out, times);
    if (close(out) && !e) e = last_error();
    close(in);
    if (e) unlink(b);
    return e;
}

int mc_host_set_times(const uint16_t *path, const int64_t *created, const int64_t *modified) {
    (void)created;          /* Linux has no call that sets a file's birth time */
    char p[NATIVE];
    if (!existing(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    if (!modified) return MC_AFP_OK;
    const struct timespec times[2] = {{0, UTIME_OMIT}, {(time_t)*modified, 0}};
    return utimensat(AT_FDCWD, p, times, 0) ? last_error() : MC_AFP_OK;
}

int mc_host_set_attribute(const uint16_t *path, uint32_t attribute, int on) {
    char p[NATIVE];
    struct stat s;
    if (!existing(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    if (stat(p, &s)) return last_error();
    /* read-only: the write permissions; a dot name hides, nothing is "system" */
    if (!(attribute & MC_HOST_READONLY) || S_ISDIR(s.st_mode)) return MC_AFP_OK;
    mode_t mode = s.st_mode & 07777;
    if (on) mode &= (mode_t)~(S_IWUSR | S_IWGRP | S_IWOTH);
    else mode |= S_IWUSR;
    if (mode == (s.st_mode & 07777)) return MC_AFP_OK;
    return chmod(p, mode) ? last_error() : MC_AFP_OK;
}

/* --- the volume -------------------------------------------------------------------- */

int mc_host_volume(const uint16_t *path, uint64_t *total, uint64_t *free_bytes, char *fs_name, size_t capacity) {
    char p[NATIVE];
    struct statvfs v;
    *total = *free_bytes = 0;
    if (fs_name && capacity) fs_name[0] = 0;
    if (!existing(path, p)) return MC_AFP_OBJECT_NOT_FOUND;
    if (statvfs(p, &v)) return last_error();
    *total = (uint64_t)v.f_blocks * v.f_frsize;
    *free_bytes = (uint64_t)v.f_bavail * v.f_frsize;
    if (fs_name && capacity) {
        /* the kind of file system (statfs(2)); none of these has named
           streams, so the server keeps Macintosh metadata in AppleDouble files */
        static const struct { long type; const char *name; } kinds[] = {
            {0xEF53, "ext4"}, {0x9123683E, "Btrfs"}, {0x58465342, "XFS"}, {0x01021994, "tmpfs"},
            {0x2FC12FC1, "ZFS"}, {0x6969, "NFS"}, {0xFF534D42, "CIFS"}, {0xFE534D42, "SMB2"},
            {0x4D44, "FAT"}, {0x2011BAB0, "exFAT"}, {0x5346544E, "NTFS (FUSE)"}, {0x65735546, "FUSE"},
            {0x01021997, "9P"}, {0x794C7630, "overlay"}, {0xF2F52010, "F2FS"}, {0x3153464A, "JFS"},
        };
        struct statfs f;
        const char *name = "a Linux file system";
        if (!statfs(p, &f))
            for (size_t k = 0; k < sizeof kinds / sizeof *kinds; k++)
                if ((long)f.f_type == kinds[k].type) name = kinds[k].name;
        snprintf(fs_name, capacity, "%s", name);
    }
    return MC_AFP_OK;
}

/* The search of mc_host_path_of_index (nftw has no context argument). */
static uint64_t wanted_index;
static char found_path[NATIVE];
static int found_one(const char *path, const struct stat *s, int kind, struct FTW *f) {
    (void)kind;
    (void)f;
    if ((uint64_t)s->st_ino != wanted_index) return 0;
    snprintf(found_path, sizeof found_path, "%s", path);
    return 1;
}

int mc_host_path_of_index(const uint16_t *root, uint64_t index, uint16_t *out, size_t capacity) {
    /* No call opens a file by its number: the shared folder is searched. */
    char r[NATIVE];
    if (!index || !existing(root, r)) return 0;
    wanted_index = index;
    found_path[0] = 0;
    if (nftw(r, found_one, 32, FTW_PHYS | FTW_MOUNT) != 1) return 0;
    for (char *c = found_path; *c; c++)
        if (*c == '/') *c = '\\';
    return mc_host_from_utf8(found_path, out, capacity) + 1 < capacity;
}

int64_t mc_host_now(void) { return (int64_t)time(NULL); }

int mc_host_root(const char *utf8, uint16_t *out, size_t capacity) {
    char full[PATH_MAX];
    if (!realpath(utf8, full)) return 0;
    for (char *c = full; *c; c++)
        if (*c == '\\') return 0;          /* a folder the server could not address */
    size_t len = strlen(full);
    while (len > 1 && full[len - 1] == '/') full[--len] = 0;
    for (char *c = full; *c; c++)
        if (*c == '/') *c = '\\';
    return mc_host_from_utf8(full, out, capacity) + 1 < capacity;
}

/* --- changes made by other programs ------------------------------------------------------- */

/* inotify watches one folder per watch: every folder of the share gets one,
 * and a folder made later gets its own when it shows. */
#define WATCHES 8192

struct mc_host_watch {
    int fd, overflowed;
    unsigned count;
    struct { int wd; uint16_t *rel; } dirs[WATCHES];
    char root[NATIVE];
};

static void watch_add(mc_host_watch *w, const char *path, const uint16_t *rel);

static uint16_t *copy_rel(const uint16_t *rel) {
    const size_t n = mc_host_len(rel) + 1;
    uint16_t *c = malloc(n * sizeof *c);
    if (c) memcpy(c, rel, n * sizeof *c);
    return c;
}

static void watch_tree(mc_host_watch *w, const char *path, const uint16_t *rel) {
    watch_add(w, path, rel);
    DIR *d = opendir(path);
    if (!d) return;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || strchr(e->d_name, '\\')) continue;
        char child[NATIVE];
        if ((size_t)snprintf(child, sizeof child, "%s/%s", path, e->d_name) >= sizeof child) continue;
        struct stat s;
        if (lstat(child, &s) || !S_ISDIR(s.st_mode)) continue;
        uint16_t name[MC_HOST_NAME], sub[MC_HOST_PATH];
        mc_host_from_utf8(e->d_name, name, MC_HOST_NAME);
        if (rel[0]) {
            if (!mc_host_join(sub, MC_HOST_PATH, rel, name)) continue;
        } else mc_host_copy_str(sub, name, MC_HOST_PATH);
        watch_tree(w, child, sub);
    }
    closedir(d);
}

static void watch_add(mc_host_watch *w, const char *path, const uint16_t *rel) {
    if (w->count >= WATCHES) { w->overflowed = 1; return; }
    const int wd = inotify_add_watch(w->fd, path, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY |
                                                      IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_ONLYDIR);
    if (wd < 0) return;
    for (unsigned k = 0; k < w->count; k++)
        if (w->dirs[k].wd == wd) {      /* the same folder again (renamed): its new place */
            uint16_t *c = copy_rel(rel);
            if (c) { free(w->dirs[k].rel); w->dirs[k].rel = c; }
            return;
        }
    if (!(w->dirs[w->count].rel = copy_rel(rel))) { inotify_rm_watch(w->fd, wd); return; }
    w->dirs[w->count].wd = wd;
    w->count++;
}

mc_host_watch *mc_host_watch_start(const uint16_t *root) {
    mc_host_watch *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (w->fd < 0 || !existing(root, w->root)) {
        if (w->fd >= 0) close(w->fd);
        free(w);
        return NULL;
    }
    const uint16_t none[1] = {0};
    watch_tree(w, w->root, none);
    return w;
}

void mc_host_watch_poll(mc_host_watch *w, void (*changed)(void *ctx, const uint16_t *relative), void *ctx) {
    if (!w) return;
    static const uint16_t everything[1] = {0};
    if (w->overflowed) {
        w->overflowed = 0;
        changed(ctx, everything);
    }
    union { struct inotify_event align; char bytes[32 * 1024]; } buffer;
    for (;;) {
        const ssize_t n = read(w->fd, buffer.bytes, sizeof buffer.bytes);
        if (n <= 0) return;
        for (ssize_t at = 0; at < n;) {
            const struct inotify_event *e = (const struct inotify_event *)(buffer.bytes + at);
            at += (ssize_t)(sizeof *e + e->len);
            if (e->mask & IN_Q_OVERFLOW) { changed(ctx, everything); continue; }
            const uint16_t *folder = NULL;
            for (unsigned k = 0; k < w->count; k++)
                if (w->dirs[k].wd == e->wd) folder = w->dirs[k].rel;
            if (!folder) continue;
            uint16_t rel[MC_HOST_PATH], name[MC_HOST_NAME];
            if (e->len && e->name[0]) {
                mc_host_from_utf8(e->name, name, MC_HOST_NAME);
                if (folder[0]) {
                    if (!mc_host_join(rel, MC_HOST_PATH, folder, name)) continue;
                } else mc_host_copy_str(rel, name, MC_HOST_PATH);
            } else mc_host_copy_str(rel, folder, MC_HOST_PATH);
            if ((e->mask & (IN_CREATE | IN_MOVED_TO)) && (e->mask & IN_ISDIR)) {
                /* a new folder: watched too, with what it already holds */
                char p[NATIVE];
                if (native(rel, p)) {
                    char full[NATIVE * 2];
                    snprintf(full, sizeof full, "%s/%s", w->root, p);
                    watch_tree(w, full, rel);
                }
            }
            changed(ctx, rel);
        }
    }
}

void mc_host_watch_stop(mc_host_watch *w) {
    if (!w) return;
    close(w->fd);
    for (unsigned k = 0; k < w->count; k++) free(w->dirs[k].rel);
    free(w);
}
#endif
