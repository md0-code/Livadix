/* Livadix — the host's file system for the AFP server: Windows paths (UTF-16,
 * with the \\?\ prefix so lengths have no MAX_PATH limit), directory
 * listings with file IDs, files and their named streams, volume space,
 * and the errors as AFP result codes.
 *
 * Files are opened sharing reading, writing and deletion with every other
 * program, so a Windows application keeps working on a file the Macintosh
 * has open; a file a Windows application holds exclusively gives
 * MC_HOST_BUSY, which each AFP call turns into its own result code.
 *
 * Livadix's own code. */
#ifndef MC_AFP_HOST_H
#define MC_AFP_HOST_H

#include <stddef.h>
#include <stdint.h>

#define MC_HOST_PATH 1024          /* UTF-16 units of a path */
#define MC_HOST_NAME 256           /* UTF-16 units of a name */

/* AFP result codes (Inside AppleTalk, appendix C; AFP 2.1 and 2.2). */
enum {
    MC_AFP_OK = 0,
    MC_AFP_ACCESS_DENIED = -5000,
    MC_AFP_AUTH_CONTINUE = -5001,
    MC_AFP_BAD_UAM = -5002,
    MC_AFP_BAD_VERSION = -5003,
    MC_AFP_BITMAP_ERR = -5004,
    MC_AFP_CANT_MOVE = -5005,
    MC_AFP_DENY_CONFLICT = -5006,
    MC_AFP_DIR_NOT_EMPTY = -5007,
    MC_AFP_DISK_FULL = -5008,
    MC_AFP_EOF = -5009,
    MC_AFP_FILE_BUSY = -5010,
    MC_AFP_FLAT_VOL = -5011,
    MC_AFP_ITEM_NOT_FOUND = -5012,
    MC_AFP_LOCK_ERR = -5013,
    MC_AFP_MISC_ERR = -5014,
    MC_AFP_NO_MORE_LOCKS = -5015,
    MC_AFP_NO_SERVER = -5016,
    MC_AFP_OBJECT_EXISTS = -5017,
    MC_AFP_OBJECT_NOT_FOUND = -5018,
    MC_AFP_PARAM_ERR = -5019,
    MC_AFP_RANGE_NOT_LOCKED = -5020,
    MC_AFP_RANGE_OVERLAP = -5021,
    MC_AFP_SESS_CLOSED = -5022,
    MC_AFP_USER_NOT_AUTH = -5023,
    MC_AFP_CALL_NOT_SUPPORTED = -5024,
    MC_AFP_OBJECT_TYPE_ERR = -5025,
    MC_AFP_TOO_MANY_FILES_OPEN = -5026,
    MC_AFP_SERVER_GOING_DOWN = -5027,
    MC_AFP_CANT_RENAME = -5028,
    MC_AFP_DIR_NOT_FOUND = -5029,
    MC_AFP_ICON_TYPE_ERR = -5030,
    MC_AFP_VOL_LOCKED = -5031,
    MC_AFP_OBJECT_LOCKED = -5032,
    MC_AFP_ID_NOT_FOUND = -5034,
    MC_AFP_SAME_OBJECT = -5038,
    /* not an AFP code: a sharing violation, mapped per call */
    MC_HOST_BUSY = -1
};

enum { MC_HOST_READONLY = 1, MC_HOST_HIDDEN = 2, MC_HOST_SYSTEM = 4 };

typedef struct mc_host_stat {
    int dir;
    uint64_t size;                 /* data fork (files) */
    uint64_t index;                /* the file system's file ID (stable on NTFS) */
    int64_t created, modified;     /* seconds since 1970-01-01 UTC */
    uint32_t attributes;           /* MC_HOST_* */
} mc_host_stat;

int mc_host_stat_path(const uint16_t *path, mc_host_stat *st);
/* The same, with the name as the file system keeps it (its letter case). */
int mc_host_stat_name(const uint16_t *path, mc_host_stat *st, uint16_t name[MC_HOST_NAME]);

/* A directory's entries ("." and ".." left out). */
typedef struct mc_host_dir mc_host_dir;
mc_host_dir *mc_host_opendir(const uint16_t *path, int *error);
/* 1 with the next entry's name (NUL-terminated) and status, 0 at the end. */
int mc_host_readdir(mc_host_dir *d, uint16_t name[MC_HOST_NAME], mc_host_stat *st);
void mc_host_closedir(mc_host_dir *d);

typedef struct mc_host_file mc_host_file;
enum { MC_HOST_OPEN, MC_HOST_CREATE_NEW, MC_HOST_OPEN_ALWAYS };
/* Opens a file (or a named stream: "path:name"); write for writing too. */
int mc_host_open(const uint16_t *path, int write, int how, mc_host_file **out);
int mc_host_read(mc_host_file *f, uint64_t offset, void *buffer, size_t length, size_t *got);
int mc_host_write(mc_host_file *f, uint64_t offset, const void *buffer, size_t length);
int mc_host_size(mc_host_file *f, uint64_t *size);
int mc_host_truncate(mc_host_file *f, uint64_t size);
int mc_host_flush(mc_host_file *f);
void mc_host_close(mc_host_file *f);
/* The size of a file or stream by name (0 and MC_AFP_OK when it does not exist). */
int mc_host_stream_size(const uint16_t *path, uint64_t *size);

int mc_host_mkdir(const uint16_t *path);
int mc_host_rmdir(const uint16_t *path);
int mc_host_unlink(const uint16_t *path);
/* Renames or moves; never replaces an existing name. */
int mc_host_rename(const uint16_t *from, const uint16_t *to);
/* Copies a file with its named streams; never replaces. */
int mc_host_copy(const uint16_t *from, const uint16_t *to);
/* Times in seconds since 1970 UTC; a NULL leaves one as it is. */
int mc_host_set_times(const uint16_t *path, const int64_t *created, const int64_t *modified);
int mc_host_set_attribute(const uint16_t *path, uint32_t attribute, int on);

/* Space of the volume holding path; its file system's name ("NTFS"). */
int mc_host_volume(const uint16_t *path, uint64_t *total, uint64_t *free_bytes, char *fs_name, size_t capacity);
/* The current path of a file ID on the volume holding root (0 when gone). */
int mc_host_path_of_index(const uint16_t *root, uint64_t index, uint16_t *out, size_t capacity);
/* Seconds since 1970 UTC now. */
int64_t mc_host_now(void);

/* Path helpers: a\b, with a separator added; 0 when too long. */
int mc_host_join(uint16_t *out, size_t capacity, const uint16_t *dir, const uint16_t *name);
/* A configured folder (UTF-8, / or \) as an absolute \\?\ path without a
 * trailing separator. 0 when it cannot be. */
int mc_host_root(const char *utf8, uint16_t *out, size_t capacity);
size_t mc_host_len(const uint16_t *s);
void mc_host_copy_str(uint16_t *dst, const uint16_t *src, size_t capacity);
/* UTF-16 to UTF-8 and back (for the databases and logs). */
size_t mc_host_to_utf8(const uint16_t *s, char *out, size_t capacity);
size_t mc_host_from_utf8(const char *s, uint16_t *out, size_t capacity);

/* Watching a folder tree for changes (ReadDirectoryChangesW). */
typedef struct mc_host_watch mc_host_watch;
mc_host_watch *mc_host_watch_start(const uint16_t *root);
/* Calls changed(ctx, relative path of what changed) for every change
 * reported since the last call; never blocks. */
void mc_host_watch_poll(mc_host_watch *w, void (*changed)(void *ctx, const uint16_t *relative), void *ctx);
void mc_host_watch_stop(mc_host_watch *w);

#endif
