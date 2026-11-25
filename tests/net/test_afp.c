/* test_afp: the AFP server (src/net/afp) answering the calls a Macintosh's
 * AppleShare client makes, against folders under work/afp (one with NTFS
 * streams, one with AppleDouble files, one read-only), checked on both
 * sides: what the replies say (block formats of Inside AppleTalk, 2nd ed.,
 * chapter 13, and AFP 2.1) and what the folders then hold (the named
 * streams AFP_AfpInfo and AFP_Resource, the "._" AppleDouble files of
 * RFC 1740, Windows names with the private-use characters).
 *
 * Livadix's own code. */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE      /* nftw */
#endif
#include "../test.h"
#include "../../src/net/afp/afp.h"
#include "../../src/net/afp/des.h"

#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ftw.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

int mc_test_failures;
static int tests;
#define T() (tests++)

static unsigned get16(const uint8_t *p) { return (unsigned)p[0] << 8 | p[1]; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

#ifdef _WIN32
static wchar_t base[MAX_PATH];

static void wpath(wchar_t *out, const wchar_t *rel) { swprintf(out, MAX_PATH * 2, L"%ls\\%ls", base, rel); }

static void remove_tree(const wchar_t *dir) {
    wchar_t pattern[MAX_PATH * 2];
    swprintf(pattern, MAX_PATH * 2, L"%ls\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wchar_t p[MAX_PATH * 2];
            swprintf(p, MAX_PATH * 2, L"%ls\\%ls", dir, fd.cFileName);
            SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(p);
            else DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void put_file(const wchar_t *rel, const char *data) {
    wchar_t p[MAX_PATH * 2];
    wpath(p, rel);
    HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    DWORD n;
    WriteFile(h, data, (DWORD)strlen(data), &n, NULL);
    CloseHandle(h);
}

/* A file's (or stream's) bytes; -1 when missing. */
static long get_file(const wchar_t *rel, uint8_t *buf, size_t cap) {
    wchar_t p[MAX_PATH * 2];
    wpath(p, rel);
    HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    ReadFile(h, buf, (DWORD)cap, &n, NULL);
    CloseHandle(h);
    return (long)n;
}
#else
static char base[1024];

static int remove_entry(const char *path, const struct stat *s, int kind, struct FTW *f) {
    (void)s; (void)f;
    return kind == FTW_DP ? rmdir(path) : unlink(path);
}
static void remove_tree(const char *dir) { nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS); }

static void put_file(const char *rel, const char *data) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
}

/* A file's bytes; -1 when missing. */
static long get_file(const char *rel, uint8_t *buf, size_t cap) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    const size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return (long)n;
}
#endif

/* ---- the server and calls --------------------------------------------------------- */

static mc_afp *afp;
static void *ses;
static uint8_t reply[8192];
static size_t rlen;
static uint8_t cmd[1024];
static size_t clen;

static void start(int metadata_streams, const char *password) {
    mc_afp_config c;
    memset(&c, 0, sizeof c);
    strcpy(c.server_name, "Livadix");
    c.metadata = metadata_streams ? MC_META_STREAMS : MC_META_APPLEDOUBLE;
    strcpy(c.password, password);
#ifdef _WIN32
    char b[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, base, -1, b, sizeof b, NULL, NULL);
#else
    const char *b = base;
#endif
    snprintf(c.volumes[0].name, sizeof c.volumes[0].name, "Streams");
    snprintf(c.volumes[0].path, sizeof c.volumes[0].path, "%s/streams", b);
    snprintf(c.volumes[1].name, sizeof c.volumes[1].name, "Double");
    snprintf(c.volumes[1].path, sizeof c.volumes[1].path, "%s/double", b);
    snprintf(c.volumes[2].name, sizeof c.volumes[2].name, "Read Only");
    snprintf(c.volumes[2].path, sizeof c.volumes[2].path, "%s/ro", b);
    c.volumes[2].readonly = 1;
    snprintf(c.volumes[3].name, sizeof c.volumes[3].name, "Caf\xC3\xA9");   /* Café */
    snprintf(c.volumes[3].path, sizeof c.volumes[3].path, "%s/streams", b);
    c.volume_count = 4;
    char state[3 * 1024];
    snprintf(state, sizeof state, "%s/state", b);
    c.state_folder = state;
    char error[512];
    afp = mc_afp_create(&c, error, sizeof error);
    if (!afp) fprintf(stderr, "mc_afp_create: %s\n", error);
    CHECK(afp != NULL);
    ses = afp ? mc_afp_session_open(afp) : NULL;
}

static void stop(void) {
    if (ses) mc_afp_session_close(afp, ses);
    mc_afp_destroy(afp);
    afp = NULL;
    ses = NULL;
}

static void begin(unsigned op) {
    clen = 0;
    cmd[clen++] = (uint8_t)op;
}
static void b8(unsigned v) { cmd[clen++] = (uint8_t)v; }
static void b16(unsigned v) { put16(cmd + clen, v); clen += 2; }
static void b32(uint32_t v) { put32(cmd + clen, v); clen += 4; }
static void bstr(const char *s) {
    const size_t n = strlen(s);
    cmd[clen++] = (uint8_t)n;
    memcpy(cmd + clen, s, n);
    clen += n;
}
static void bpad(void) { if (clen & 1) cmd[clen++] = 0; }
static int32_t call(void) { return mc_afp_command(afp, ses, cmd, clen, reply, &rlen); }
static int32_t write_call(const uint8_t *data, size_t n) {
    int32_t result = 0;
    const long want = mc_afp_write_begin(afp, ses, cmd, clen, &result, reply, &rlen);
    if (want < 0) return result;
    return mc_afp_write_end(afp, ses, cmd, clen, data, n < (size_t)want ? n : (size_t)want, reply, &rlen);
}

static int32_t login_guest(void) {
    begin(18);
    bstr("AFPVersion 2.1");
    bstr("No User Authent");
    return call();
}

static unsigned open_vol(const char *name, unsigned bitmap) {
    begin(24);
    b8(0);
    b16(bitmap | 0x20);
    bstr(name);
    if (call()) return 0;
    /* the volume ID follows the fields before it in bitmap order */
    size_t at = 2;
    static const size_t width[5] = {2, 2, 4, 4, 4};
    for (unsigned bit = 0; bit < 5; bit++)
        if ((bitmap | 0x20) & 1u << bit) at += width[bit];
    return get16(reply + at);
}

/* GetFileDirParms: returns the error; the parameters stay in reply. */
static int32_t get_parms(unsigned vol, uint32_t dir, const char *path, unsigned fbits, unsigned dbits) {
    begin(34);
    b8(0);
    b16(vol);
    b32(dir);
    b16(fbits);
    b16(dbits);
    b8(2);
    bstr(path);
    return call();
}

static uint16_t open_fork(unsigned vol, uint32_t dir, const char *path, int rsrc, unsigned mode, int32_t *err) {
    begin(26);
    b8(rsrc ? 0x80 : 0);
    b16(vol);
    b32(dir);
    b16(0);
    b16(mode);
    b8(2);
    bstr(path);
    *err = call();
    return *err ? 0 : (uint16_t)get16(reply + 2);
}

static int32_t fork_write(uint16_t ref, uint32_t offset, const void *data, size_t n) {
    begin(33);
    b8(0);
    b16(ref);
    b32(offset);
    b32((uint32_t)n);
    return write_call(data, n);
}

static int32_t fork_read(uint16_t ref, uint32_t offset, uint32_t count) {
    begin(27);
    b8(0);
    b16(ref);
    b32(offset);
    b32(count);
    b8(0);
    b8(0);
    return call();
}

static int32_t close_fork(uint16_t ref) {
    begin(4);
    b8(0);
    b16(ref);
    return call();
}

static int32_t create(unsigned op, unsigned vol, uint32_t dir, const char *path) {
    begin(op);
    b8(0);
    b16(vol);
    b32(dir);
    b8(2);
    bstr(path);
    return call();
}

/* The long name of the first entry an Enumerate returns matching `name`; 1 if found. */
static int enumerate_has(unsigned vol, uint32_t dir, const char *name, uint32_t *id) {
    for (unsigned start = 1;; ) {
        begin(9);
        b8(0);
        b16(vol);
        b32(dir);
        b16(0x0140);          /* long name, file number */
        b16(0x0140);          /* long name, directory ID */
        b16(20);
        b16((unsigned)start);
        b16(4624);
        b8(2);
        bstr("");
        if (call()) return 0;
        const unsigned count = get16(reply + 4);
        size_t at = 6;
        for (unsigned k = 0; k < count; k++) {
            const uint8_t *e = reply + at;
            const uint8_t *params = e + 2;
            const uint8_t *ln = params + get16(params);
            if (ln[0] == strlen(name) && !memcmp(ln + 1, name, ln[0])) {
                if (id) *id = get32(params + 2);
                return 1;
            }
            at += e[0];
        }
        start += count;
    }
}

/* ---- the tests ------------------------------------------------------------------------- */

static void test_status(void) {
    T();
    uint8_t b[2048];
    const size_t n = mc_afp_status(afp, b, sizeof b);
    CHECK(n > 300);
    CHECK(b[10] == 7 && !memcmp(b + 11, "Livadix", 7));            /* the name right after the flags */
    CHECK(get16(b + 8) & 1);                                    /* SupportsCopyFile */
    const uint8_t *machine = b + get16(b);
    CHECK(machine[0] == 7 && !memcmp(machine + 1, "Livadix", 7));
    const uint8_t *v = b + get16(b + 2);
    CHECK(v[0] == 3 && v[1] == 14 && !memcmp(v + 2, "AFPVersion 1.1", 14));
    const uint8_t *u = b + get16(b + 4);
    CHECK(u[0] == 1 && u[1] == 15 && !memcmp(u + 2, "No User Authent", 15));
    const size_t icon = get16(b + 6);
    CHECK(icon && icon + 256 == n);
    /* the mask covers the icon */
    for (int k = 0; k < 128; k++) CHECK((b[icon + k] & ~b[icon + 128 + k]) == 0);
}

static void test_login(void) {
    T();
    begin(16);
    CHECK_EQ(call(), MC_AFP_USER_NOT_AUTH);                     /* nothing before login */
    begin(18);
    bstr("AFPVersion 3.0");
    bstr("No User Authent");
    CHECK_EQ(call(), MC_AFP_BAD_VERSION);
    begin(18);
    bstr("AFPVersion 2.0");
    bstr("Cleartxt Passwrd");
    bstr("someone");
    CHECK_EQ(call(), MC_AFP_BAD_UAM);
    CHECK_EQ(login_guest(), 0);
    begin(16);
    CHECK_EQ(call(), 0);
    CHECK(reply[4] == 4);
    CHECK(reply[5] == 0 && reply[6] == 7 && !memcmp(reply + 7, "Streams", 7));
    begin(38);                                                  /* FPGetSrvrMsg: none */
    b8(0);
    b16(0);
    b16(1);
    CHECK_EQ(call(), 0);
    CHECK(rlen == 5 && reply[4] == 0);
}

static void test_volume(unsigned *vs, unsigned *vd) {
    T();
    *vs = open_vol("streams", 0x1FF);                           /* names match without case */
    CHECK_EQ(*vs, 1);
    CHECK(get16(reply) == 0x1FF + 0x20 - 0x20);
    CHECK_EQ(get16(reply + 4), 2);                              /* signature: fixed directory IDs */
    *vd = open_vol("Double", 0);
    CHECK_EQ(*vd, 2);
    CHECK_EQ(open_vol("Caf\x8E", 0), 4);                        /* Café in Mac Roman */
    begin(24);
    b8(0);
    b16(0x20);
    bstr("Nothing");
    CHECK_EQ(call(), MC_AFP_OBJECT_NOT_FOUND);
    begin(17);
    b8(0);
    b16(*vs);
    b16(0);
    CHECK_EQ(call(), MC_AFP_BITMAP_ERR);
    begin(17);
    b8(0);
    b16(*vs);
    b16(0x0400);
    CHECK_EQ(call(), MC_AFP_BITMAP_ERR);
}

#ifdef _WIN32
static void test_read_host_file(unsigned vs) {
    T();
    put_file(L"streams\\hello.txt", "hello from the host\r\n");
    CHECK_EQ(get_parms(vs, 2, "hello.txt", 0x0240 | 0x0001, 0), 0);
    CHECK(reply[4] == 0);
    /* attributes, long name offset, data fork length */
    CHECK_EQ(get32(reply + 6 + 4), 21);
    const uint8_t *ln = reply + 6 + get16(reply + 6 + 2);
    CHECK(ln[0] == 9 && !memcmp(ln + 1, "hello.txt", 9));
    CHECK(enumerate_has(vs, 2, "hello.txt", NULL));
    CHECK(!enumerate_has(vs, 2, ".livadix-ids", NULL));
    int32_t e;
    const uint16_t ref = open_fork(vs, 2, "HELLO.TXT", 0, 1, &e);
    CHECK_EQ(e, 0);
    CHECK_EQ(fork_read(ref, 0, 100), MC_AFP_EOF);
    CHECK(rlen == 21 && !memcmp(reply, "hello from the host\r\n", 21));
    CHECK_EQ(fork_read(ref, 6, 4), 0);
    CHECK(rlen == 4 && !memcmp(reply, "from", 4));
    /* newline mask: up to the first CR */
    begin(27);
    b8(0);
    b16(ref);
    b32(0);
    b32(100);
    b8(0xFF);
    b8('\r');
    CHECK_EQ(call(), 0);
    CHECK_EQ(rlen, 20);
    /* the empty resource fork */
    const uint16_t rref = open_fork(vs, 2, "hello.txt", 1, 1, &e);
    CHECK_EQ(e, 0);
    CHECK_EQ(fork_read(rref, 0, 100), MC_AFP_EOF);
    CHECK_EQ(rlen, 0);
    CHECK_EQ(close_fork(rref), 0);
    /* DAlreadyOpen while open */
    CHECK_EQ(get_parms(vs, 2, "hello.txt", 0x0001, 0), 0);
    CHECK(get16(reply + 6) & 0x0008);
    CHECK_EQ(close_fork(ref), 0);
    CHECK_EQ(close_fork(ref), MC_AFP_PARAM_ERR);
    /* a file held by a Windows program alone: deny conflict */
    wchar_t p[MAX_PATH * 2];
    wpath(p, L"streams\\hello.txt");
    HANDLE h = CreateFileW(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    open_fork(vs, 2, "hello.txt", 0, 1, &e);
    CHECK_EQ(e, MC_AFP_DENY_CONFLICT);
    CloseHandle(h);
}

#endif

/* A file written by the Macintosh: type and creator, both forks, a name
 * with characters Windows forbids. */
static void write_mac_file(unsigned vol, const char *name) {
    CHECK_EQ(create(7, vol, 2, name), 0);
    CHECK_EQ(create(7, vol, 2, name), MC_AFP_OBJECT_EXISTS);
    /* Finder info: TEXT/ttxt */
    begin(30);
    b8(0);
    b16(vol);
    b32(2);
    b16(0x0020);
    b8(2);
    bstr(name);
    bpad();
    uint8_t finder[32] = {'T', 'E', 'X', 'T', 't', 't', 'x', 't'};
    memcpy(cmd + clen, finder, 32);
    clen += 32;
    CHECK_EQ(call(), 0);
    int32_t e;
    const uint16_t d = open_fork(vol, 2, name, 0, 3, &e);
    CHECK_EQ(e, 0);
    CHECK_EQ(fork_write(d, 0, "written by the Macintosh", 24), 0);
    CHECK_EQ(get32(reply), 24);
    const uint16_t r = open_fork(vol, 2, name, 1, 3, &e);
    CHECK_EQ(e, 0);
    uint8_t rsrc[3000];
    for (int k = 0; k < 3000; k++) rsrc[k] = (uint8_t)(k * 13);
    CHECK_EQ(fork_write(r, 0, rsrc, 3000), 0);
    /* deny modes: another open for writing with deny-write conflicts */
    open_fork(vol, 2, name, 1, 0x22, &e);
    CHECK_EQ(e, MC_AFP_DENY_CONFLICT);
    CHECK_EQ(close_fork(r), 0);
    CHECK_EQ(close_fork(d), 0);
    CHECK_EQ(get_parms(vol, 2, name, 0x0620, 0), 0);
    CHECK(!memcmp(reply + 6, "TEXTttxt", 8));
    CHECK_EQ(get32(reply + 6 + 32), 24);
    CHECK_EQ(get32(reply + 6 + 36), 3000);
}

#ifdef _WIN32

static void test_write_streams(unsigned vs) {
    T();
    write_mac_file(vs, "Mac/File?");
    uint8_t b[4096];
    CHECK_EQ(get_file(L"streams\\Mac\xF022" L"File\xF025", b, sizeof b), 24);
    CHECK(!memcmp(b, "written by the Macintosh", 24));
    const long n = get_file(L"streams\\Mac\xF022" L"File\xF025:AFP_AfpInfo", b, sizeof b);
    CHECK_EQ(n, 60);
    CHECK(!memcmp(b, "AFP", 4) && b[4] == 0 && b[5] == 0 && b[6] == 1 && b[7] == 0 && !memcmp(b + 16, "TEXTttxt", 8));
    CHECK_EQ(get_file(L"streams\\Mac\xF022" L"File\xF025:AFP_Resource", b, sizeof b), 3000);
    CHECK(b[1] == 13 && b[2999] == ((2999 * 13) & 0xFF));
    /* a Mac Roman name */
    CHECK_EQ(create(7, vs, 2, "Caf\x8E"), 0);
    CHECK(get_file(L"streams\\Caf\x00E9", b, sizeof b) == 0);
}

static void test_write_appledouble(void) {
    T();
    /* the whole server in AppleDouble mode */
    stop();
    start(0, "");
    login_guest();
    const unsigned vd = open_vol("Double", 0);
    CHECK_EQ(mc_afp_volume_metadata(afp, 1, NULL), MC_META_APPLEDOUBLE);
    write_mac_file(vd, "Doc 1");
    uint8_t b[8192];
    CHECK_EQ(get_file(L"double\\Doc 1", b, sizeof b), 24);
    const long n = get_file(L"double\\._Doc 1", b, sizeof b);
    CHECK_EQ(n, 146 + 3000);
    CHECK(get32(b) == 0x00051607 && get32(b + 4) == 0x00020000 && get16(b + 24) == 5);
    /* Finder info entry (9) and the resource fork (2) last */
    int finder = 0, rsrc = 0;
    for (unsigned k = 0; k < 5; k++) {
        const uint8_t *e = b + 26 + 12 * k;
        if (get32(e) == 9 && !memcmp(b + get32(e + 4), "TEXTttxt", 8)) finder = 1;
        if (get32(e) == 2 && get32(e + 4) == 146 && get32(e + 8) == 3000 && b[146 + 1] == 13) rsrc = 1;
    }
    CHECK(finder && rsrc);
    /* the sidecar never shows */
    CHECK(enumerate_has(vd, 2, "Doc 1", NULL));
    CHECK(!enumerate_has(vd, 2, "._Doc 1", NULL));
    /* rename and delete take the sidecar along */
    begin(28);
    b8(0);
    b16(vd);
    b32(2);
    b8(2);
    bstr("Doc 1");
    b8(2);
    bstr("Doc 2");
    CHECK_EQ(call(), 0);
    CHECK_EQ(get_file(L"double\\._Doc 1", b, sizeof b), -1);
    CHECK_EQ(get_file(L"double\\._Doc 2", b, sizeof b), 146 + 3000);
    CHECK_EQ(create(8, vd, 2, "Doc 2"), 0);
    CHECK_EQ(get_file(L"double\\._Doc 2", b, sizeof b), -1);
    CHECK_EQ(get_file(L"double\\Doc 2", b, sizeof b), -1);
    stop();
    start(1, "");
    login_guest();
    open_vol("Streams", 0);
}

static void test_names_and_ids(unsigned vs) {
    T();
    /* a Windows name the Macintosh cannot spell: a short form with its ID */
    put_file(L"streams\\\x4E2D\x6587 notes.txt", "x");
    uint32_t id = 0;
    char shown[40] = {0};
    begin(9);
    b8(0);
    b16(vs);
    b32(2);
    b16(0x0140);
    b16(0);
    b16(50);
    b16(1);
    b16(4624);
    b8(2);
    bstr("");
    CHECK_EQ(call(), 0);
    for (unsigned k = 0, at = 6; k < get16(reply + 4); k++) {
        const uint8_t *params = reply + at + 2;
        const uint8_t *ln = params + get16(params);
        if (ln[0] && ln[1] == '_' && ln[2] == '_') {
            memcpy(shown, ln + 1, ln[0]);
            id = get32(params + 2);
        }
        at += reply[at];
    }
    char expect[40];
    snprintf(expect, sizeof expect, "__ notes#%lX.txt", (unsigned long)id);
    CHECK_STR(shown, expect);
    int32_t e;
    const uint16_t ref = open_fork(vs, 2, shown, 0, 1, &e);
    CHECK_EQ(e, 0);
    CHECK_EQ(fork_read(ref, 0, 10), MC_AFP_EOF);
    CHECK(rlen == 1 && reply[0] == 'x');
    close_fork(ref);
    /* a folder's ID */
    CHECK_EQ(create(6, vs, 2, "Folder"), 0);
    const uint32_t dir = get32(reply);
    CHECK(dir >= 17);
    CHECK_EQ(create(7, vs, dir, "inside"), 0);
    CHECK_EQ(get_parms(vs, dir, "", 0, 0x0100), 0);
    CHECK(reply[4] == 0x80 && get32(reply + 6) == dir);
    /* ascending paths: Folder, up, Folder again, inside */
    begin(34);
    b8(0);
    b16(vs);
    b32(2);
    b16(0x0100);
    b16(0x0100);
    b8(2);
    const uint8_t path[] = {'F', 'o', 'l', 'd', 'e', 'r', 0, 0, 'F', 'o', 'l', 'd', 'e', 'r', 0, 'i', 'n', 's', 'i', 'd', 'e'};
    cmd[clen++] = sizeof path;
    memcpy(cmd + clen, path, sizeof path);
    clen += sizeof path;
    CHECK_EQ(call(), 0);
    CHECK(reply[4] == 0);
    /* from the root's parent, through the volume's name */
    CHECK_EQ(get_parms(vs, 1, "Streams", 0, 0x0100), 0);
    CHECK_EQ(get32(reply + 6), 2);
}

static void test_ids_persist(unsigned *vs) {
    T();
    CHECK_EQ(get_parms(*vs, 2, "Folder", 0, 0x0100), 0);
    const uint32_t dir = get32(reply + 6);
    CHECK_EQ(get_parms(*vs, 2, "hello.txt", 0x0100, 0), 0);
    const uint32_t file = get32(reply + 6);
    stop();
    /* moved and renamed on the host while the server is down */
    wchar_t a[MAX_PATH * 2], b[MAX_PATH * 2];
    wpath(a, L"streams\\hello.txt");
    wpath(b, L"streams\\Folder\\renamed.txt");
    CHECK(MoveFileW(a, b));
    start(1, "");
    login_guest();
    *vs = open_vol("Streams", 0);
    CHECK_EQ(get_parms(*vs, 2, "Folder", 0, 0x0100), 0);
    CHECK_EQ(get32(reply + 6), dir);
    /* the file's ID finds it where it went */
    CHECK_EQ(get_parms(*vs, file, "", 0x0142, 0), MC_AFP_OBJECT_NOT_FOUND);  /* a file's ID is not a directory */
    CHECK_EQ(get_parms(*vs, dir, "renamed.txt", 0x0100, 0), 0);
    CHECK_EQ(get32(reply + 6), file);
    /* and back while it runs */
    CHECK(MoveFileW(b, a));
    CHECK_EQ(get_parms(*vs, 2, "hello.txt", 0x0100, 0), 0);
    CHECK_EQ(get32(reply + 6), file);
}

static void test_host_change(unsigned vs) {
    T();
    begin(17);
    b8(0);
    b16(vs);
    b16(0x0008);
    CHECK_EQ(call(), 0);
    const uint32_t before = get32(reply + 2);
    CHECK_EQ(get_parms(vs, 2, "Folder", 0, 0x0108), 0);
    const uint32_t dir_before = get32(reply + 6);
    Sleep(1100);
    put_file(L"streams\\Folder\\from windows.txt", "new");
    for (int i = 0; i < 50; i++) {
        Sleep(20);
        mc_afp_poll(afp);
    }
    begin(17);
    b8(0);
    b16(vs);
    b16(0x0008);
    CHECK_EQ(call(), 0);
    CHECK(get32(reply + 2) > before);
    CHECK_EQ(get_parms(vs, 2, "Folder", 0, 0x0108), 0);
    CHECK(get32(reply + 6) > dir_before);
    CHECK(enumerate_has(vs, get32(reply + 6 + 4), "from windows.txt", NULL));
    /* the change of a folder the Macintosh has not seen moves its nearest seen parent */
    CHECK(mc_afp_volume_metadata(afp, 0, NULL) == MC_META_STREAMS);
}

static void test_operations(unsigned vs) {
    T();
    /* move and rename into a folder */
    begin(23);
    b8(0);
    b16(vs);
    b32(2);
    b32(2);
    b8(2);
    bstr("Caf\x8E");
    b8(2);
    bstr("Folder");
    b8(2);
    bstr("Moved");
    CHECK_EQ(call(), 0);
    CHECK_EQ(get_parms(vs, 2, "Folder\0Moved" + 0, 0, 0), 0);
    uint8_t b[256];
    CHECK_EQ(get_file(L"streams\\Folder\\Moved", b, sizeof b), 0);
    /* a folder cannot go into itself */
    begin(23);
    b8(0);
    b16(vs);
    b32(2);
    b32(2);
    b8(2);
    bstr("Folder");
    b8(2);
    bstr("Folder");
    b8(2);
    bstr("");
    CHECK_EQ(call(), MC_AFP_CANT_MOVE);
    /* a folder with something in it stays */
    CHECK_EQ(create(8, vs, 2, "Folder"), MC_AFP_DIR_NOT_EMPTY);
    /* copy a file with its forks and Finder info */
    begin(5);
    b8(0);
    b16(vs);
    b32(2);
    b16(vs);
    b32(2);
    b8(2);
    bstr("Mac/File?");
    b8(2);
    bstr("Folder");
    b8(2);
    bstr("Copy");
    CHECK_EQ(call(), 0);
    begin(34);
    b8(0);
    b16(vs);
    b32(2);
    b16(0x0620);
    b16(0);
    b8(2);
    const uint8_t cp[] = {'F', 'o', 'l', 'd', 'e', 'r', 0, 'C', 'o', 'p', 'y'};
    cmd[clen++] = sizeof cp;
    memcpy(cmd + clen, cp, sizeof cp);
    clen += sizeof cp;
    CHECK_EQ(call(), 0);
    CHECK(!memcmp(reply + 6, "TEXTttxt", 8) && get32(reply + 6 + 32) == 24 && get32(reply + 6 + 36) == 3000);
    /* exchange: the forks swap, the names and IDs stay */
    CHECK_EQ(get_parms(vs, 2, "Mac/File?", 0x0100, 0), 0);
    const uint32_t id_a = get32(reply + 6);
    put_file(L"streams\\other", "other data");
    CHECK_EQ(get_parms(vs, 2, "other", 0x0100, 0), 0);
    const uint32_t id_b = get32(reply + 6);
    begin(42);
    b8(0);
    b16(vs);
    b32(2);
    b32(2);
    b8(2);
    bstr("Mac/File?");
    b8(2);
    bstr("other");
    CHECK_EQ(call(), 0);
    CHECK_EQ(get_parms(vs, 2, "Mac/File?", 0x0320, 0), 0);
    CHECK(get32(reply + 6 + 32) == id_a && get32(reply + 6 + 36) == 10 && !memcmp(reply + 6, "TEXTttxt", 8));
    CHECK_EQ(get_parms(vs, 2, "other", 0x0100, 0), 0);
    CHECK_EQ(get32(reply + 6), id_b);
    CHECK_EQ(get_file(L"streams\\other", b, sizeof b), 24);
    /* byte range locks between two forks */
    int32_t e;
    const uint16_t f1 = open_fork(vs, 2, "other", 0, 3, &e), f2 = open_fork(vs, 2, "other", 0, 3, &e);
    CHECK(f1 && f2 && f1 != f2);
    begin(1);
    b8(0);
    b16(f1);
    b32(0);
    b32(10);
    CHECK_EQ(call(), 0);
    CHECK_EQ(get32(reply), 0);
    CHECK_EQ(fork_read(f2, 0, 4), MC_AFP_LOCK_ERR);
    CHECK_EQ(fork_write(f2, 2, "zz", 2), MC_AFP_LOCK_ERR);
    CHECK_EQ(fork_read(f1, 0, 4), 0);
    begin(1);
    b8(0);
    b16(f2);
    b32(5);
    b32(1);
    CHECK_EQ(call(), MC_AFP_LOCK_ERR);
    begin(1);
    b8(1);                   /* unlock */
    b16(f1);
    b32(0);
    b32(10);
    CHECK_EQ(call(), 0);
    CHECK_EQ(fork_read(f2, 0, 4), 0);
    /* an open file cannot be deleted */
    CHECK_EQ(create(8, vs, 2, "other"), MC_AFP_FILE_BUSY);
    /* set the fork's length */
    begin(31);
    b8(0);
    b16(f1);
    b16(0x0200);
    b32(3);
    CHECK_EQ(call(), 0);
    begin(14);
    b8(0);
    b16(f1);
    b16(0x0200);
    CHECK_EQ(call(), 0);
    CHECK_EQ(get32(reply + 2), 3);
    close_fork(f1);
    close_fork(f2);
    CHECK_EQ(create(8, vs, 2, "other"), 0);
    /* lock a file: Windows read-only, writes refused */
    begin(30);
    b8(0);
    b16(vs);
    b32(2);
    b16(0x0001);
    b8(2);
    bstr("Mac/File?");
    bpad();
    b16(0x8000 | 0x0020 | 0x0080 | 0x0100);
    CHECK_EQ(call(), 0);
    wchar_t p[MAX_PATH * 2];
    wpath(p, L"streams\\Mac\xF022" L"File\xF025");
    CHECK(GetFileAttributesW(p) & FILE_ATTRIBUTE_READONLY);
    open_fork(vs, 2, "Mac/File?", 0, 3, &e);
    CHECK_EQ(e, MC_AFP_OBJECT_LOCKED);
    CHECK_EQ(create(8, vs, 2, "Mac/File?"), MC_AFP_OBJECT_LOCKED);
    begin(30);
    b8(0);
    b16(vs);
    b32(2);
    b16(0x0001);
    b8(2);
    bstr("Mac/File?");
    bpad();
    b16(0x0020 | 0x0080 | 0x0100);
    CHECK_EQ(call(), 0);
    CHECK(!(GetFileAttributesW(p) & FILE_ATTRIBUTE_READONLY));
}

static void test_desktop(unsigned vs) {
    T();
    begin(48);
    b8(0);
    b16(vs);
    CHECK_EQ(call(), 0);
    const unsigned dt = get16(reply);
    /* comments */
    begin(56);
    b8(0);
    b16(dt);
    b32(2);
    b8(2);
    bstr("Mac/File?");
    bpad();
    bstr("a comment");
    CHECK_EQ(call(), 0);
    begin(58);
    b8(0);
    b16(dt);
    b32(2);
    b8(2);
    bstr("Mac/File?");
    CHECK_EQ(call(), 0);
    CHECK(reply[0] == 9 && !memcmp(reply + 1, "a comment", 9));
    /* icons, through a write */
    uint8_t icon[256];
    for (int k = 0; k < 256; k++) icon[k] = (uint8_t)k;
    begin(192);
    b8(0);
    b16(dt);
    memcpy(cmd + clen, "ttxtTEXT", 8);
    clen += 8;
    b8(1);
    b8(0);
    b32(0x1234);
    b16(256);
    CHECK_EQ(write_call(icon, 256), 0);
    begin(51);
    b8(0);
    b16(dt);
    memcpy(cmd + clen, "ttxtTEXT", 8);
    clen += 8;
    b8(1);
    b8(0);
    b16(256);
    CHECK_EQ(call(), 0);
    CHECK(rlen == 256 && reply[255] == 255);
    begin(52);
    b8(0);
    b16(dt);
    memcpy(cmd + clen, "ttxt", 4);
    clen += 4;
    b16(1);
    CHECK_EQ(call(), 0);
    CHECK(get32(reply) == 0x1234 && !memcmp(reply + 4, "TEXT", 4) && reply[8] == 1 && get16(reply + 10) == 256);
    /* applications */
    begin(53);
    b8(0);
    b16(dt);
    b32(2);
    memcpy(cmd + clen, "ttxt", 4);
    clen += 4;
    b32(77);
    b8(2);
    bstr("Mac/File?");
    CHECK_EQ(call(), 0);
    begin(55);
    b8(0);
    b16(dt);
    memcpy(cmd + clen, "ttxt", 4);
    clen += 4;
    b16(1);
    b16(0x0040);
    CHECK_EQ(call(), 0);
    CHECK(get32(reply + 2) == 77);
    const uint8_t *ln = reply + 6 + get16(reply + 6);
    CHECK(ln[0] == 9 && !memcmp(ln + 1, "Mac/File?", 9));
    /* kept across a restart */
    stop();
    start(1, "");
    login_guest();
    open_vol("Streams", 0);
    begin(58);
    b8(0);
    b16(dt);
    b32(2);
    b8(2);
    bstr("Mac/File?");
    CHECK_EQ(call(), 0);
    CHECK(reply[0] == 9);
    /* MapID 0: no name */
    begin(21);
    b8(1);
    b32(0);
    CHECK_EQ(call(), 0);
    CHECK(rlen == 1 && reply[0] == 0);
}

static void test_readonly(void) {
    T();
    put_file(L"ro\\fixed.txt", "fixed");
    const unsigned vr = open_vol("Read Only", 0x0001);
    CHECK(vr == 3 && get16(reply + 2) == 1);
    CHECK_EQ(create(7, vr, 2, "new"), MC_AFP_VOL_LOCKED);
    int32_t e;
    open_fork(vr, 2, "fixed.txt", 0, 3, &e);
    CHECK_EQ(e, MC_AFP_VOL_LOCKED);
    const uint16_t ref = open_fork(vr, 2, "fixed.txt", 0, 1, &e);
    CHECK_EQ(e, 0);
    close_fork(ref);
    /* its databases are not in the folder */
    uint8_t b[64];
    CHECK_EQ(get_file(L"ro\\.livadix-ids", b, sizeof b), -1);
    CHECK_EQ(get_parms(vr, 2, "", 0, 0x1000), 0);
    CHECK(reply[6 + 3] == 0x83);
}

static void test_password(void) {
    T();
    stop();
    start(1, "secret");
    uint8_t b[1024];
    const size_t n = mc_afp_status(afp, b, sizeof b);
    CHECK(n > 0);
    const uint8_t *u = b + get16(b + 4);
    CHECK(u[0] == 2 && u[1] == 16 && !memcmp(u + 2, "Cleartxt Passwrd", 16));
    CHECK_EQ(login_guest(), MC_AFP_BAD_UAM);
    begin(18);
    bstr("AFPVersion 2.0");
    bstr("Cleartxt Passwrd");
    bstr("anyone");
    bpad();
    memcpy(cmd + clen, "wrong\0\0\0", 8);
    clen += 8;
    CHECK_EQ(call(), MC_AFP_USER_NOT_AUTH);
    clen -= 8;
    memcpy(cmd + clen, "secret\0\0", 8);
    clen += 8;
    CHECK_EQ(call(), 0);
    mc_afp_session_close(afp, ses);
    ses = mc_afp_session_open(afp);
    /* Randnum Exchange: the random number encrypted with the password */
    begin(18);
    bstr("AFPVersion 2.1");
    bstr("Randnum Exchange");
    bstr("anyone");
    CHECK_EQ(call(), MC_AFP_AUTH_CONTINUE);
    CHECK_EQ(rlen, 10);
    const unsigned id = get16(reply);
    uint8_t random[8], key[8] = {'s', 'e', 'c', 'r', 'e', 't', 0, 0}, enc[8];
    memcpy(random, reply + 2, 8);
    mc_des_encrypt(key, random, enc);
    begin(19);
    b8(0);
    b16(id);
    memcpy(cmd + clen, enc, 8);
    clen += 8;
    CHECK_EQ(call(), 0);
    begin(16);
    CHECK_EQ(call(), 0);
    stop();
    start(1, "");
}
#else
/* Linux: the server on a Linux folder, where the Macintosh's metadata goes in
 * AppleDouble files: the Macintosh's names match the host's without case,
 * dot files stay hidden, and a file the Macintosh writes, renames and
 * deletes takes its sidecar along. */
static void test_linux_share(unsigned vd) {
    T();
    CHECK_EQ(mc_afp_volume_metadata(afp, 1, NULL), MC_META_APPLEDOUBLE);
    put_file("double/ReadMe.txt", "hello");
    put_file("double/.profile", "hidden");
    CHECK(enumerate_has(vd, 2, "ReadMe.txt", NULL));
    CHECK(!enumerate_has(vd, 2, ".profile", NULL));
    CHECK_EQ(get_parms(vd, 2, "readme.TXT", 0x0001, 0), 0);
    int32_t e;
    const uint16_t ref = open_fork(vd, 2, "README.txt", 0, 1, &e);
    CHECK_EQ(e, 0);
    CHECK_EQ(fork_read(ref, 0, 5), 0);
    CHECK(rlen == 5 && !memcmp(reply, "hello", 5));
    CHECK_EQ(close_fork(ref), 0);
    write_mac_file(vd, "Doc 1");
    uint8_t b[8192];
    CHECK_EQ(get_file("double/Doc 1", b, sizeof b), 24);
    CHECK_EQ(get_file("double/._Doc 1", b, sizeof b), 146 + 3000);
    begin(28);                                                  /* FPRename */
    b8(0);
    b16(vd);
    b32(2);
    b8(2);
    bstr("Doc 1");
    b8(2);
    bstr("Doc 2");
    CHECK_EQ(call(), 0);
    CHECK_EQ(get_file("double/._Doc 1", b, sizeof b), -1);
    CHECK_EQ(get_file("double/._Doc 2", b, sizeof b), 146 + 3000);
    CHECK_EQ(create(8, vd, 2, "Doc 2"), 0);                     /* FPDelete */
    CHECK_EQ(get_file("double/Doc 2", b, sizeof b), -1);
    CHECK_EQ(get_file("double/._Doc 2", b, sizeof b), -1);
}
#endif

int main(void) {
#ifdef _WIN32
    swprintf(base, MAX_PATH, L"%hs/afp", MC_WORK);
    for (wchar_t *p = base; *p; p++)
        if (*p == '/') *p = '\\';
    remove_tree(base);
    CreateDirectoryW(base, NULL);
    const wchar_t *dirs[] = {L"streams", L"double", L"ro", L"state"};
    for (int k = 0; k < 4; k++) {
        wchar_t p[MAX_PATH * 2];
        wpath(p, dirs[k]);
        CreateDirectoryW(p, NULL);
    }
    start(1, "");
    if (afp) {
        unsigned vs = 0, vd = 0;
        (void)vd;
        test_status();
        test_login();
        test_volume(&vs, &vd);
        test_read_host_file(vs);
        test_write_streams(vs);
        test_write_appledouble();
        test_names_and_ids(vs);
        test_ids_persist(&vs);
        test_host_change(vs);
        test_operations(vs);
        test_desktop(vs);
        test_readonly();
        test_password();
        stop();
    }
#else
    snprintf(base, sizeof base, "%s/afp", MC_WORK);
    remove_tree(base);
    mkdir(base, 0777);
    const char *dirs[] = {"streams", "double", "ro", "state"};
    for (int k = 0; k < 4; k++) {
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", base, dirs[k]);
        mkdir(p, 0777);
    }
    start(0, "");
    if (afp) {
        unsigned vs = 0, vd = 0;
        test_status();
        test_login();
        test_volume(&vs, &vd);
        test_linux_share(vd);
        stop();
    }
#endif
    printf("test_afp: %d tests, %d failure(s)\n", tests, mc_test_failures);
    return mc_test_failures ? 1 : 0;
}
