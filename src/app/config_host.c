/* The configuration window's services from the host (config_host.h): the
 * file picker's folders, the serial ports, BMP files and scripts. Livadix's own
 * code. */
#include "config_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <glob.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* --- the file picker's view of the host (config_picker.c) -------------------------------------- */

static int entry_order(const void *x, const void *y) {
    const cfg_dir_entry *a = x, *b = y;
    if (a->dir != b->dir) return a->dir ? -1 : 1;
    for (const unsigned char *p = (const unsigned char *)a->name, *q = (const unsigned char *)b->name;; ++p, ++q) {
        const int c = *p >= 'A' && *p <= 'Z' ? *p + 32 : *p, d = *q >= 'A' && *q <= 'Z' ? *q + 32 : *q;
        if (c != d || !c) return c - d;
    }
}

static int add_entry(cfg_dir_entry **list, int *count, int *capacity, const char *name, int dir, long long size, long long time) {
    if (*count == *capacity) {
        const int more = *capacity ? *capacity * 2 : 256;
        cfg_dir_entry *bigger = realloc(*list, (size_t)more * sizeof *bigger);
        if (!bigger) return 0;
        *list = bigger;
        *capacity = more;
    }
    cfg_dir_entry *e = &(*list)[(*count)++];
    snprintf(e->name, sizeof e->name, "%s", name);
    e->dir = dir;
    e->size = size;
    e->time = time;
    return 1;
}

#ifdef _WIN32
char cfg_host_separator(void) { return '\\'; }

static int wide_of(const char *utf8, wchar_t *out, int capacity) { return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, capacity) > 0; }
static int utf8_of(const wchar_t *wide, char *out, int capacity) { return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, capacity, NULL, NULL) > 0; }

int cfg_host_list(const char *folder, cfg_dir_entry **out) {
    wchar_t pattern[MC_CONFIG_PATH + 4];
    *out = NULL;
    if (!wide_of(folder, pattern, MC_CONFIG_PATH)) return -1;
    const size_t n = wcslen(pattern);
    if (n && pattern[n - 1] != L'\\' && pattern[n - 1] != L'/') wcscat(pattern, L"\\");
    wcscat(pattern, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : -1;
    int count = 0, capacity = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;      /* as Explorer shows a folder */
        char name[256];
        if (!utf8_of(fd.cFileName, name, sizeof name)) continue;
        const ULARGE_INTEGER t = {{fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime}};
        add_entry(out, &count, &capacity, name, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
                  (long long)fd.nFileSizeHigh << 32 | fd.nFileSizeLow, (long long)(t.QuadPart / 10000000ull) - 11644473600ll);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (count) qsort(*out, (size_t)count, sizeof **out, entry_order);
    return count;
}

int cfg_host_places(char names[][64], char paths[][MC_CONFIG_PATH], int max) {
    int n = 0;
    const char *home = getenv("USERPROFILE");
    char folder[MC_CONFIG_PATH];
    wchar_t wide[MC_CONFIG_PATH];
    if (home && n < max) {
        snprintf(folder, sizeof folder, "%s\\Desktop", home);
        if (wide_of(folder, wide, MC_CONFIG_PATH) && (GetFileAttributesW(wide) & FILE_ATTRIBUTE_DIRECTORY) &&
            GetFileAttributesW(wide) != INVALID_FILE_ATTRIBUTES) {
            snprintf(names[n], 64, "Desktop");
            snprintf(paths[n++], MC_CONFIG_PATH, "%s", folder);
        }
        if (n < max) {
            snprintf(names[n], 64, "Home");
            snprintf(paths[n++], MC_CONFIG_PATH, "%s", home);
        }
    }
    const DWORD drives = GetLogicalDrives();
    for (int k = 0; k < 26 && n < max; ++k)
        if (drives & (1u << k)) {
            snprintf(names[n], 64, "%c:", 'A' + k);
            snprintf(paths[n++], MC_CONFIG_PATH, "%c:\\", 'A' + k);
        }
    return n;
}

int cfg_host_absolute(const char *path, char *out, size_t capacity) {
    wchar_t wide[MC_CONFIG_PATH], full[MC_CONFIG_PATH];
    if (!wide_of(path[0] ? path : ".", wide, MC_CONFIG_PATH)) return 0;
    const DWORD n = GetFullPathNameW(wide, MC_CONFIG_PATH, full, NULL);
    return n && n < MC_CONFIG_PATH && utf8_of(full, out, (int)capacity);
}

int cfg_host_is_folder(const char *path) {
    wchar_t wide[MC_CONFIG_PATH];
    if (!wide_of(path, wide, MC_CONFIG_PATH)) return 0;
    const DWORD a = GetFileAttributesW(wide);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* The core opens files through the C library, by their names in the Windows
 * code page (as the file chooser gave them); a shared folder is UTF-8. */
int cfg_host_native_path(const char *utf8, int kind, char *out, size_t capacity) {
    if (kind == CFG_PICK_FOLDER) {
        if (strlen(utf8) >= capacity) return 0;
        memcpy(out, utf8, strlen(utf8) + 1);
        return 1;
    }
    wchar_t wide[MC_CONFIG_PATH];
    BOOL lossy = FALSE;
    return wide_of(utf8, wide, MC_CONFIG_PATH) &&
           WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, out, (int)capacity, NULL, &lossy) > 0 && !lossy;
}
int cfg_host_utf8_path(const char *native, int kind, char *out, size_t capacity) {
    if (kind == CFG_PICK_FOLDER) {
        if (strlen(native) >= capacity) return 0;
        memcpy(out, native, strlen(native) + 1);
        return 1;
    }
    wchar_t wide[MC_CONFIG_PATH];
    return MultiByteToWideChar(CP_ACP, 0, native, -1, wide, MC_CONFIG_PATH) > 0 && utf8_of(wide, out, (int)capacity);
}
#else
char cfg_host_separator(void) { return '/'; }

int cfg_host_list(const char *folder, cfg_dir_entry **out) {
    *out = NULL;
    DIR *d = opendir(folder);
    if (!d) return -1;
    int count = 0, capacity = 0;
    for (struct dirent *e; (e = readdir(d));) {
        if (e->d_name[0] == '.') continue;          /* hidden, as the desktop's file managers show a folder */
        char path[MC_CONFIG_PATH * 2];
        struct stat st;
        snprintf(path, sizeof path, "%s/%s", folder, e->d_name);
        if (stat(path, &st)) continue;                /* a dangling link */
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode) && !S_ISBLK(st.st_mode)) continue;
        add_entry(out, &count, &capacity, e->d_name, S_ISDIR(st.st_mode), (long long)st.st_size, (long long)st.st_mtime);
    }
    closedir(d);
    if (count) qsort(*out, (size_t)count, sizeof **out, entry_order);
    return count;
}

int cfg_host_is_folder(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

static int add_place(char names[][64], char paths[][MC_CONFIG_PATH], int n, int max, const char *name, const char *path) {
    if (n >= max || !cfg_host_is_folder(path)) return n;
    snprintf(names[n], 64, "%s", name);
    snprintf(paths[n], MC_CONFIG_PATH, "%s", path);
    return n + 1;
}

int cfg_host_places(char names[][64], char paths[][MC_CONFIG_PATH], int max) {
    int n = 0;
    const char *home = getenv("HOME"), *user = getenv("USER");
    char folder[MC_CONFIG_PATH];
    if (home) {
        snprintf(folder, sizeof folder, "%s/Desktop", home);
        n = add_place(names, paths, n, max, "Desktop", folder);
        n = add_place(names, paths, n, max, "Home", home);
    }
    n = add_place(names, paths, n, max, "Computer", "/");
    /* removable media and other mounts: /media/USER, /run/media/USER, /mnt (WSL's Windows drives) */
    static const char *const roots[] = {"/media/", "/run/media/", "/mnt"};
    for (int r = 0; r < 3; ++r) {
        char root[MC_CONFIG_PATH];
        snprintf(root, sizeof root, "%s%s", roots[r], r < 2 && user ? user : "");
        cfg_dir_entry *list;
        const int count = r < 2 && !user ? -1 : cfg_host_list(root, &list);
        for (int k = 0; k < count; ++k)
            if (list[k].dir) {
                snprintf(folder, sizeof folder, "%s/%s", root, list[k].name);
                n = add_place(names, paths, n, max, folder, folder);       /* named by its path: /mnt/c, /media/me/disk */
            }
        if (count > 0) free(list);
    }
    return n;
}

int cfg_host_absolute(const char *path, char *out, size_t capacity) {
    char full[PATH_MAX];
    if (!realpath(path[0] ? path : ".", full) || strlen(full) >= capacity) return 0;
    memcpy(out, full, strlen(full) + 1);
    return 1;
}

int cfg_host_native_path(const char *utf8, int kind, char *out, size_t capacity) {
    (void)kind;
    if (strlen(utf8) >= capacity) return 0;
    memcpy(out, utf8, strlen(utf8) + 1);
    return 1;
}
int cfg_host_utf8_path(const char *native, int kind, char *out, size_t capacity) { return cfg_host_native_path(native, kind, out, capacity); }
#endif

#ifdef _WIN32
void cfg_host_serial_ports(cfg_app *a) {
    HKEY key;
    a->com_count = 0;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0; a->com_count < CFG_COM_PORTS; ++i) {
        char name[256], data[32];
        DWORD n = sizeof name, d = sizeof data, type = 0;
        if (RegEnumValueA(key, i, name, &n, NULL, &type, (BYTE *)data, &d) != ERROR_SUCCESS) break;
        if (type == REG_SZ && d < sizeof data) { data[d] = 0; snprintf(a->com_ports[a->com_count++], 16, "%s", data); }
    }
    RegCloseKey(key);
}
#else
static int port_order(const void *x, const void *y) {
    const char *a = x, *b = y;
    const size_t la = strlen(a), lb = strlen(b);
    return la != lb ? (la < lb ? -1 : 1) : strcmp(a, b);
}

/* The USB and ACM serial adapters, then the on-board ports; an on-board
 * ttyS node whose UART the kernel did not find (type 0 in sysfs) is not a
 * port. */
void cfg_host_serial_ports(cfg_app *a) {
    static const char *const patterns[] = {"/dev/ttyUSB*", "/dev/ttyACM*", "/dev/ttyS*"};
    a->com_count = 0;
    for (int k = 0; k < 3; ++k) {
        glob_t g;
        memset(&g, 0, sizeof g);
        const int first = a->com_count;
        if (glob(patterns[k], 0, NULL, &g) == 0)
            for (size_t i = 0; i < g.gl_pathc && a->com_count < CFG_COM_PORTS; ++i) {
                const char *path = g.gl_pathv[i];
                if (strlen(path) >= sizeof a->com_ports[0]) continue;
                if (k == 2) {
                    char sys[64];
                    int type = -1;
                    snprintf(sys, sizeof sys, "/sys/class/tty/%s/type", path + 5);
                    FILE *f = fopen(sys, "r");
                    if (f) { if (fscanf(f, "%d", &type) != 1) type = -1; fclose(f); }
                    if (!type) continue;
                }
                snprintf(a->com_ports[a->com_count++], sizeof a->com_ports[0], "%s", path);
            }
        globfree(&g);
        qsort(a->com_ports[first], (size_t)(a->com_count - first), sizeof a->com_ports[0], port_order);
    }
}
#endif

int cfg_host_save_bmp(const ui_canvas *c, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    const int stride = (c->w * 3 + 3) & ~3;
    const uint32_t data = (uint32_t)stride * (uint32_t)c->h, size = 54 + data;
    uint8_t h[54] = {'B', 'M'};
    const uint32_t fields[] = {size, 0, 54, 40, (uint32_t)c->w, (uint32_t)c->h};
    for (int i = 0; i < 6; ++i) memcpy(h + 2 + 4 * i, &fields[i], 4);
    h[26] = 1; h[28] = 24;
    memcpy(h + 34, &data, 4);
    int ok = fwrite(h, 1, 54, f) == 54;
    uint8_t *row = calloc(1, (size_t)stride);
    for (int y = c->h - 1; ok && row && y >= 0; --y) {
        for (int x = 0; x < c->w; ++x) {
            const uint32_t p = c->px[(size_t)y * c->w + x];
            row[3 * x] = (uint8_t)p; row[3 * x + 1] = (uint8_t)(p >> 8); row[3 * x + 2] = (uint8_t)(p >> 16);
        }
        ok = fwrite(row, 1, (size_t)stride, f) == (size_t)stride;
    }
    if (!row) ok = 0;
    free(row);
    if (fclose(f)) ok = 0;
    return ok;
}

int cfg_script_words(char *line, char **out, int max) {
    int n = 0;
    char *s = line;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (*s == '"') {
            out[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            out[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    return n;
}
int cfg_script_key(const char *name, int *mods) {
    static const struct { const char *name; int key; } keys[] = {
        {"tab", UI_KEY_TAB}, {"enter", UI_KEY_ENTER}, {"escape", UI_KEY_ESCAPE}, {"space", UI_KEY_SPACE}, {"up", UI_KEY_UP},
        {"down", UI_KEY_DOWN}, {"left", UI_KEY_LEFT}, {"right", UI_KEY_RIGHT}, {"home", UI_KEY_HOME}, {"end", UI_KEY_END},
        {"backspace", UI_KEY_BACKSPACE}, {"delete", UI_KEY_DELETE}};
    *mods = 0;
    for (;;) {
        if (!strncmp(name, "shift+", 6)) { *mods |= UI_MOD_SHIFT; name += 6; }
        else if (!strncmp(name, "ctrl+", 5)) { *mods |= UI_MOD_CTRL; name += 5; }
        else if (!strncmp(name, "alt+", 4)) { *mods |= UI_MOD_ALT; name += 4; }
        else break;
    }
    for (size_t i = 0; i < sizeof keys / sizeof *keys; ++i) if (!strcmp(keys[i].name, name)) return keys[i].key;
    if (name[0] && !name[1]) return -name[0];
    return 0;
}
