/* Livadix — the host's file system for the AFP server (afp_host.h).
 *
 * Sources: Microsoft's Win32 documentation: CreateFileW (share modes,
 * \\?\ paths, named streams as "file:stream"), GetFileInformationByHandleEx
 * with FileIdBothDirectoryInfo (directory entries with their file IDs),
 * OpenFileById and GetFinalPathNameByHandleW, ReadDirectoryChangesW
 * (FILE_NOTIFY_INFORMATION), GetDiskFreeSpaceExW, GetVolumeInformationW,
 * CopyFileExW (named streams copied), MoveFileExW, the system error codes.
 *
 * Livadix's own code. */
#include "afp_host.h"

#include <stdlib.h>
#include <string.h>

size_t mc_host_len(const uint16_t *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void mc_host_copy_str(uint16_t *dst, const uint16_t *src, size_t capacity) {
    size_t n = 0;
    while (src[n] && n + 1 < capacity) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = 0;
}

int mc_host_join(uint16_t *out, size_t capacity, const uint16_t *dir, const uint16_t *name) {
    const size_t a = mc_host_len(dir), b = mc_host_len(name);
    if (a + 1 + b + 1 > capacity) return 0;
    if (out != dir) memmove(out, dir, a * 2);
    size_t n = a;
    if (n && out[n - 1] != '\\') out[n++] = '\\';
    memmove(out + n, name, b * 2);
    out[n + b] = 0;
    return 1;
}

size_t mc_host_to_utf8(const uint16_t *s, char *out, size_t capacity) {
    size_t n = 0;
    for (; *s; s++) {
        uint32_t c = *s;
        if (c >= 0xD800 && c < 0xDC00 && s[1] >= 0xDC00 && s[1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (s[1] - 0xDC00);
            s++;
        }
        uint8_t b[4];
        size_t k;
        if (c < 0x80) { b[0] = (uint8_t)c; k = 1; }
        else if (c < 0x800) { b[0] = (uint8_t)(0xC0 | c >> 6); b[1] = (uint8_t)(0x80 | (c & 63)); k = 2; }
        else if (c < 0x10000) {
            b[0] = (uint8_t)(0xE0 | c >> 12); b[1] = (uint8_t)(0x80 | (c >> 6 & 63)); b[2] = (uint8_t)(0x80 | (c & 63)); k = 3;
        } else {
            b[0] = (uint8_t)(0xF0 | c >> 18); b[1] = (uint8_t)(0x80 | (c >> 12 & 63)); b[2] = (uint8_t)(0x80 | (c >> 6 & 63));
            b[3] = (uint8_t)(0x80 | (c & 63)); k = 4;
        }
        if (n + k + 1 > capacity) break;
        memcpy(out + n, b, k);
        n += k;
    }
    if (capacity) out[n] = 0;
    return n;
}

size_t mc_host_from_utf8(const char *s, uint16_t *out, size_t capacity) {
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)s;
    while (*p && n + 2 < capacity) {
        uint32_t c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = (c & 7) << 18 | (uint32_t)(p[0] & 63) << 12 | (uint32_t)(p[1] & 63) << 6 | (p[2] & 63); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = (c & 15) << 12 | (uint32_t)(p[0] & 63) << 6 | (p[1] & 63); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
        if (c >= 0x10000) {
            c -= 0x10000;
            out[n++] = (uint16_t)(0xD800 + (c >> 10));
            out[n++] = (uint16_t)(0xDC00 + (c & 0x3FF));
        } else out[n++] = (uint16_t)c;
    }
    out[n] = 0;
    return n;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define W(p) ((const wchar_t *)(p))

/* 100 ns units from 1601 to seconds from 1970. */
static int64_t from_filetime(LARGE_INTEGER t) { return t.QuadPart / 10000000 - 11644473600LL; }
static FILETIME to_filetime(int64_t s) {
    const uint64_t v = (uint64_t)(s + 11644473600LL) * 10000000ULL;
    FILETIME f;
    f.dwLowDateTime = (DWORD)v;
    f.dwHighDateTime = (DWORD)(v >> 32);
    return f;
}

static int error_of(DWORD e) {
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_DIRECTORY: return MC_AFP_OBJECT_NOT_FOUND;
    case ERROR_ACCESS_DENIED: return MC_AFP_ACCESS_DENIED;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
    case ERROR_USER_MAPPED_FILE: return MC_HOST_BUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return MC_AFP_OBJECT_EXISTS;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL: return MC_AFP_DISK_FULL;
    case ERROR_DIR_NOT_EMPTY: return MC_AFP_DIR_NOT_EMPTY;
    case ERROR_WRITE_PROTECT: return MC_AFP_VOL_LOCKED;
    case ERROR_TOO_MANY_OPEN_FILES: return MC_AFP_TOO_MANY_FILES_OPEN;
    default: return MC_AFP_MISC_ERR;
    }
}
static int last_error(void) { return error_of(GetLastError()); }

static const DWORD share_all = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

static void fill_stat(mc_host_stat *st, DWORD attributes, LARGE_INTEGER created, LARGE_INTEGER written, LARGE_INTEGER size,
                      LARGE_INTEGER index) {
    st->dir = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    st->size = st->dir ? 0 : (uint64_t)size.QuadPart;
    st->index = (uint64_t)index.QuadPart;
    st->created = from_filetime(created);
    st->modified = from_filetime(written);
    st->attributes = (attributes & FILE_ATTRIBUTE_READONLY ? MC_HOST_READONLY : 0) | (attributes & FILE_ATTRIBUTE_HIDDEN ? MC_HOST_HIDDEN : 0) |
                     (attributes & FILE_ATTRIBUTE_SYSTEM ? MC_HOST_SYSTEM : 0);
}

int mc_host_stat_path(const uint16_t *path, mc_host_stat *st) {
    HANDLE h = CreateFileW(W(path), FILE_READ_ATTRIBUTES, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return last_error();
    BY_HANDLE_FILE_INFORMATION i;
    const BOOL ok = GetFileInformationByHandle(h, &i);
    CloseHandle(h);
    if (!ok) return last_error();
    LARGE_INTEGER c, w, s, x;
    c.LowPart = i.ftCreationTime.dwLowDateTime; c.HighPart = (LONG)i.ftCreationTime.dwHighDateTime;
    w.LowPart = i.ftLastWriteTime.dwLowDateTime; w.HighPart = (LONG)i.ftLastWriteTime.dwHighDateTime;
    s.LowPart = i.nFileSizeLow; s.HighPart = (LONG)i.nFileSizeHigh;
    x.LowPart = i.nFileIndexLow; x.HighPart = (LONG)i.nFileIndexHigh;
    fill_stat(st, i.dwFileAttributes, c, w, s, x);
    return MC_AFP_OK;
}

int mc_host_stat_name(const uint16_t *path, mc_host_stat *st, uint16_t name[MC_HOST_NAME]) {
    const int e = mc_host_stat_path(path, st);
    if (e) return e;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(W(path), FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    if (h == INVALID_HANDLE_VALUE) {
        /* a root has no entry of its own: its last component */
        const size_t n = mc_host_len(path);
        size_t k = n;
        while (k && path[k - 1] != '\\') k--;
        mc_host_copy_str(name, path + k, MC_HOST_NAME);
        return MC_AFP_OK;
    }
    FindClose(h);
    mc_host_copy_str(name, (const uint16_t *)fd.cFileName, MC_HOST_NAME);
    return MC_AFP_OK;
}

struct mc_host_dir {
    HANDLE h;
    int first, done;
    size_t at, used;
    union { FILE_ID_BOTH_DIR_INFO align; uint8_t bytes[64 * 1024]; } buffer;
};

mc_host_dir *mc_host_opendir(const uint16_t *path, int *error) {
    mc_host_dir *d = calloc(1, sizeof *d);
    if (!d) { *error = MC_AFP_MISC_ERR; return NULL; }
    d->h = CreateFileW(W(path), FILE_LIST_DIRECTORY, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (d->h == INVALID_HANDLE_VALUE) {
        *error = last_error();
        free(d);
        return NULL;
    }
    d->first = 1;
    return d;
}

int mc_host_readdir(mc_host_dir *d, uint16_t name[MC_HOST_NAME], mc_host_stat *st) {
    for (;;) {
        if (d->done) return 0;
        if (d->at >= d->used) {
            const FILE_INFO_BY_HANDLE_CLASS cls = d->first ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            d->first = 0;
            if (!GetFileInformationByHandleEx(d->h, cls, d->buffer.bytes, sizeof d->buffer.bytes)) {
                d->done = 1;
                return 0;
            }
            d->at = 0;
            d->used = sizeof d->buffer.bytes;
        }
        const FILE_ID_BOTH_DIR_INFO *e = (const FILE_ID_BOTH_DIR_INFO *)(d->buffer.bytes + d->at);
        d->at = e->NextEntryOffset ? d->at + e->NextEntryOffset : d->used;
        const size_t n = e->FileNameLength / 2;
        if ((n == 1 && e->FileName[0] == '.') || (n == 2 && e->FileName[0] == '.' && e->FileName[1] == '.')) continue;
        if (n >= MC_HOST_NAME) continue;
        memcpy(name, e->FileName, n * 2);
        name[n] = 0;
        fill_stat(st, e->FileAttributes, e->CreationTime, e->LastWriteTime, e->EndOfFile, e->FileId);
        return 1;
    }
}

void mc_host_closedir(mc_host_dir *d) {
    if (!d) return;
    CloseHandle(d->h);
    free(d);
}

struct mc_host_file {
    HANDLE h;
};

int mc_host_open(const uint16_t *path, int write, int how, mc_host_file **out) {
    const DWORD disposition = how == MC_HOST_CREATE_NEW ? CREATE_NEW : how == MC_HOST_OPEN_ALWAYS ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE h = CreateFileW(W(path), GENERIC_READ | (write ? GENERIC_WRITE : 0), share_all, NULL, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return last_error();
    mc_host_file *f = malloc(sizeof *f);
    if (!f) { CloseHandle(h); return MC_AFP_MISC_ERR; }
    f->h = h;
    *out = f;
    return MC_AFP_OK;
}

int mc_host_read(mc_host_file *f, uint64_t offset, void *buffer, size_t length, size_t *got) {
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD n = 0;
    *got = 0;
    if (!ReadFile(f->h, buffer, (DWORD)length, &n, &o)) {
        const DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF) return MC_AFP_OK;
        return error_of(e);
    }
    *got = n;
    return MC_AFP_OK;
}

int mc_host_write(mc_host_file *f, uint64_t offset, const void *buffer, size_t length) {
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)offset;
    o.OffsetHigh = (DWORD)(offset >> 32);
    DWORD n = 0;
    if (!WriteFile(f->h, buffer, (DWORD)length, &n, &o)) return last_error();
    return n == length ? MC_AFP_OK : MC_AFP_DISK_FULL;
}

int mc_host_size(mc_host_file *f, uint64_t *size) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(f->h, &s)) return last_error();
    *size = (uint64_t)s.QuadPart;
    return MC_AFP_OK;
}

int mc_host_truncate(mc_host_file *f, uint64_t size) {
    FILE_END_OF_FILE_INFO e;
    e.EndOfFile.QuadPart = (LONGLONG)size;
    return SetFileInformationByHandle(f->h, FileEndOfFileInfo, &e, sizeof e) ? MC_AFP_OK : last_error();
}

int mc_host_flush(mc_host_file *f) { return FlushFileBuffers(f->h) ? MC_AFP_OK : last_error(); }

void mc_host_close(mc_host_file *f) {
    if (!f) return;
    CloseHandle(f->h);
    free(f);
}

int mc_host_stream_size(const uint16_t *path, uint64_t *size) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    *size = 0;
    if (!GetFileAttributesExW(W(path), GetFileExInfoStandard, &a)) {
        const DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? MC_AFP_OK : error_of(e);
    }
    *size = (uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
    return MC_AFP_OK;
}

int mc_host_mkdir(const uint16_t *path) { return CreateDirectoryW(W(path), NULL) ? MC_AFP_OK : last_error(); }

int mc_host_rmdir(const uint16_t *path) { return RemoveDirectoryW(W(path)) ? MC_AFP_OK : last_error(); }

int mc_host_unlink(const uint16_t *path) { return DeleteFileW(W(path)) ? MC_AFP_OK : last_error(); }

int mc_host_rename(const uint16_t *from, const uint16_t *to) { return MoveFileExW(W(from), W(to), 0) ? MC_AFP_OK : last_error(); }

int mc_host_copy(const uint16_t *from, const uint16_t *to) {
    return CopyFileExW(W(from), W(to), NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS) ? MC_AFP_OK : last_error();
}

int mc_host_set_times(const uint16_t *path, const int64_t *created, const int64_t *modified) {
    HANDLE h = CreateFileW(W(path), FILE_WRITE_ATTRIBUTES, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return last_error();
    FILE_BASIC_INFO b;
    memset(&b, 0, sizeof b);            /* zero: leave as it is */
    if (created) {
        const FILETIME f = to_filetime(*created);
        b.CreationTime.LowPart = f.dwLowDateTime;
        b.CreationTime.HighPart = (LONG)f.dwHighDateTime;
    }
    if (modified) {
        const FILETIME f = to_filetime(*modified);
        b.LastWriteTime.LowPart = f.dwLowDateTime;
        b.LastWriteTime.HighPart = (LONG)f.dwHighDateTime;
        b.ChangeTime = b.LastWriteTime;
    }
    const BOOL ok = SetFileInformationByHandle(h, FileBasicInfo, &b, sizeof b);
    CloseHandle(h);
    return ok ? MC_AFP_OK : last_error();
}

int mc_host_set_attribute(const uint16_t *path, uint32_t attribute, int on) {
    const DWORD a = GetFileAttributesW(W(path));
    if (a == INVALID_FILE_ATTRIBUTES) return last_error();
    DWORD bit = 0;
    if (attribute & MC_HOST_READONLY) bit |= FILE_ATTRIBUTE_READONLY;
    if (attribute & MC_HOST_HIDDEN) bit |= FILE_ATTRIBUTE_HIDDEN;
    if (attribute & MC_HOST_SYSTEM) bit |= FILE_ATTRIBUTE_SYSTEM;
    DWORD b = on ? a | bit : a & ~bit;
    b &= ~(DWORD)FILE_ATTRIBUTE_DIRECTORY;
    if (!b) b = FILE_ATTRIBUTE_NORMAL;
    if (b == (a & ~(DWORD)FILE_ATTRIBUTE_DIRECTORY)) return MC_AFP_OK;
    return SetFileAttributesW(W(path), b) ? MC_AFP_OK : last_error();
}

int mc_host_volume(const uint16_t *path, uint64_t *total, uint64_t *free_bytes, char *fs_name, size_t capacity) {
    ULARGE_INTEGER avail, all, freeb;
    if (!GetDiskFreeSpaceExW(W(path), &avail, &all, &freeb)) return last_error();
    *total = all.QuadPart;
    *free_bytes = avail.QuadPart;
    if (fs_name && capacity) {
        fs_name[0] = 0;
        HANDLE h = CreateFileW(W(path), 0, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            wchar_t fs[64];
            if (GetVolumeInformationByHandleW(h, NULL, 0, NULL, NULL, NULL, fs, 64)) {
                size_t k = 0;
                for (; fs[k] && k + 1 < capacity; k++) fs_name[k] = (char)fs[k];
                fs_name[k] = 0;
            }
            CloseHandle(h);
        }
    }
    return MC_AFP_OK;
}

int mc_host_path_of_index(const uint16_t *root, uint64_t index, uint16_t *out, size_t capacity) {
    HANDLE r = CreateFileW(W(root), 0, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (r == INVALID_HANDLE_VALUE) return 0;
    FILE_ID_DESCRIPTOR id;
    memset(&id, 0, sizeof id);
    id.dwSize = sizeof id;
    id.Type = FileIdType;
    id.FileId.QuadPart = (LONGLONG)index;
    HANDLE h = OpenFileById(r, &id, 0, share_all, NULL, FILE_FLAG_BACKUP_SEMANTICS);
    CloseHandle(r);
    if (h == INVALID_HANDLE_VALUE) return 0;
    const DWORD n = GetFinalPathNameByHandleW(h, (wchar_t *)out, (DWORD)capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    return n > 0 && n < capacity;
}

int64_t mc_host_now(void) {
    FILETIME f;
    GetSystemTimeAsFileTime(&f);
    LARGE_INTEGER t;
    t.LowPart = f.dwLowDateTime;
    t.HighPart = (LONG)f.dwHighDateTime;
    return from_filetime(t);
}

int mc_host_root(const char *utf8, uint16_t *out, size_t capacity) {
    uint16_t given[MC_HOST_PATH];
    if (!mc_host_from_utf8(utf8, given, MC_HOST_PATH)) return 0;
    for (size_t k = 0; given[k]; k++)
        if (given[k] == '/') given[k] = '\\';
    wchar_t full[MC_HOST_PATH];
    const DWORD n = GetFullPathNameW(W(given), MC_HOST_PATH, full, NULL);
    if (!n || n >= MC_HOST_PATH - 8) return 0;
    size_t len = n;
    while (len > 3 && full[len - 1] == '\\') full[--len] = 0;
    /* \\?\C:\..., \\?\UNC\server\share\... */
    if (len >= 4 && full[0] == '\\' && full[1] == '\\' && full[2] == '?' && full[3] == '\\') {
        if (len + 1 > capacity) return 0;
        memcpy(out, full, (len + 1) * 2);
    } else if (full[0] == '\\' && full[1] == '\\') {
        const wchar_t *prefix = L"\\\\?\\UNC";
        if (7 + len - 1 + 1 > capacity) return 0;
        memcpy(out, prefix, 7 * 2);
        memcpy(out + 7, full + 1, len * 2);
    } else {
        if (4 + len + 1 > capacity) return 0;
        memcpy(out, L"\\\\?\\", 8);
        memcpy(out + 4, full, (len + 1) * 2);
    }
    return 1;
}

struct mc_host_watch {
    HANDLE dir, event;
    OVERLAPPED o;
    int pending;
    union { FILE_NOTIFY_INFORMATION align; uint8_t bytes[32 * 1024]; } buffer;
};

static void arm(mc_host_watch *w) {
    memset(&w->o, 0, sizeof w->o);
    w->o.hEvent = w->event;
    ResetEvent(w->event);
    w->pending = ReadDirectoryChangesW(w->dir, w->buffer.bytes, sizeof w->buffer.bytes, TRUE,
                                       FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_CREATION,
                                       NULL, &w->o, NULL) != 0;
}

mc_host_watch *mc_host_watch_start(const uint16_t *root) {
    mc_host_watch *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->dir = CreateFileW(W(root), FILE_LIST_DIRECTORY, share_all, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
    w->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (w->dir == INVALID_HANDLE_VALUE || !w->event) {
        if (w->dir != INVALID_HANDLE_VALUE) CloseHandle(w->dir);
        if (w->event) CloseHandle(w->event);
        free(w);
        return NULL;
    }
    arm(w);
    return w;
}

void mc_host_watch_poll(mc_host_watch *w, void (*changed)(void *ctx, const uint16_t *relative), void *ctx) {
    if (!w) return;
    if (!w->pending) {
        arm(w);
        return;
    }
    DWORD n = 0;
    if (!GetOverlappedResult(w->dir, &w->o, &n, FALSE)) {
        if (GetLastError() == ERROR_IO_INCOMPLETE) return;
        arm(w);
        return;
    }
    if (n == 0) changed(ctx, (const uint16_t *)L"");     /* the buffer overflowed: something changed */
    for (size_t at = 0; n && at < n;) {
        const FILE_NOTIFY_INFORMATION *e = (const FILE_NOTIFY_INFORMATION *)(w->buffer.bytes + at);
        uint16_t rel[MC_HOST_PATH];
        size_t len = e->FileNameLength / 2;
        if (len >= MC_HOST_PATH) len = MC_HOST_PATH - 1;
        memcpy(rel, e->FileName, len * 2);
        rel[len] = 0;
        changed(ctx, rel);
        if (!e->NextEntryOffset) break;
        at += e->NextEntryOffset;
    }
    arm(w);
}

void mc_host_watch_stop(mc_host_watch *w) {
    if (!w) return;
    if (w->pending) {
        CancelIoEx(w->dir, &w->o);
        DWORD n;
        GetOverlappedResult(w->dir, &w->o, &n, TRUE);
    }
    CloseHandle(w->dir);
    CloseHandle(w->event);
    free(w);
}

#endif   /* elsewhere: afp_host_posix.c */
