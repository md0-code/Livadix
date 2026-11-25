/* What a disk image holds: the HFS volume of a bare image or of an Apple
 * partition map, and on a startup disk its System Folder.
 *
 * Structures from Inside Macintosh: Files (1992), chapter 2 "File Manager",
 * "Data Organization on Volumes": the boot blocks (bbID 'LK', bbSysName at
 * 10 and bbShellName at 26, Str15 each), the master directory block at byte
 * 1024 of the volume (drSigWord $4244, drAlBlkSiz at 20, drAlBlSt at 28,
 * drVN at 36, drFndrInfo at 92 whose first long word is the blessed System
 * Folder's directory ID, drEmbedSigWord at 124, drXTFlSize/drXTExtRec at
 * 130/134, drCTFlSize/drCTExtRec at 146/150), extent descriptors (start, count),
 * B*-tree nodes (descriptor: ndFLink, ndBLink, ndType, ndNHeight, ndNRecs;
 * record offsets from the end of the node; the header record's
 * bthFNode at 10 and bthNodeSize at 18), catalog keys (ckrKeyLen,
 * ckrParID at 2, ckrCName at 6) and records (cdrType 1 directory with
 * dirDirID at 6; 2 file with filUsrWds at 4, filFlNum at 20, filLgLen at
 * 26, filRLgLen at 36, filExtRec at 74, filRExtRec at 86), extents keys
 * (xkrFkType at 1, xkrFNum at 2, xkrFABN at 6). The partition map: Inside
 * Macintosh: Devices (1994), chapter 3 "SCSI Manager", "Disk Partitioning"
 * (driver descriptor 'ER' with sbBlkSize; partition map entries 'PM' with
 * pmMapBlkCnt at 4, pmPyPartStart at 8, pmPartBlkCnt at 12, pmParType at
 * 48). The resource fork: Inside Macintosh: More Macintosh Toolbox (1993),
 * chapter 1 "Resource Manager", "Resource File Format" (header, map, type
 * list, reference list, data lengths), and the 'vers' resource (major
 * revision in BCD, minor and bug-fix revision, stage, short version
 * string at 6). The 'gbly' resource of the System file and of System
 * Enablers: a version word, a long word, a count and that many box flags
 * (Gestalt machine type minus 6), as read from the System files and the
 * enablers of the disk images in data/disk. MFS's master directory block
 * and file directory (Inside Macintosh II, "The File Manager"): read for
 * the System file only. Mac OS Extended ('H+') and ISO 9660 ('CD001' at
 * byte 32769) are recognised. HFS Plus startup metadata follows Apple's
 * TN1150; the blessed CoreServices folder supplies SystemVersion.plist. The
 * check of a volume left mounted (mc_hfs_left_mounted) follows the ROM's,
 * as traced, with DIVU.W's overflow as the M68000
 * Family Programmer's Reference Manual gives it (4-96, DIVU).
 * Livadix's own code. */
#include "hfs.h"
#include "disk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Mac OS Roman $80-$FF in Unicode. */
static const uint16_t roman[128] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1, 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};
/* A Mac OS Roman string of n bytes as UTF-8 (control characters dropped). */
static void utf8(const uint8_t *s, size_t n, char *out, size_t capacity) {
    size_t used = 0;
    for (size_t i = 0; i < n && used + 4 < capacity; ++i) {
        const unsigned c = s[i] < 0x80 ? s[i] : roman[s[i] - 0x80];
        if (c < 0x20) continue;
        if (c < 0x80) out[used++] = (char)c;
        else if (c < 0x800) { out[used++] = (char)(0xC0 | c >> 6); out[used++] = (char)(0x80 | (c & 0x3F)); }
        else { out[used++] = (char)(0xE0 | c >> 12); out[used++] = (char)(0x80 | (c >> 6 & 0x3F)); out[used++] = (char)(0x80 | (c & 0x3F)); }
    }
    if (capacity) out[used] = 0;
}

typedef struct volume {
    mc_disk *disk;
    uint64_t base, end;            /* the volume's bytes in the image */
    uint32_t block_size, first_block;
    uint8_t mdb[512];
} volume;

static int read_at(volume *v, uint64_t offset, uint32_t count, void *buffer) {
    return offset + count <= v->end && mc_disk_read_bytes(v->disk, offset, count, buffer);
}

/* A fork's bytes from its first three extents and the extents overflow
 * file's records (extents, the overflow tree's leaf records for the file). */
typedef struct extent { uint16_t start, count; } extent;
#define EXTENTS 64
static uint8_t *read_extents(volume *v, const extent *e, size_t n, uint32_t size) {
    if (size > 64u * 1024 * 1024) return NULL;
    uint8_t *data = calloc(1, size ? size : 1);
    if (!data) return NULL;
    uint32_t done = 0;
    for (size_t i = 0; i < n && done < size; ++i) {
        const uint64_t at = v->base + (uint64_t)v->first_block * 512 + (uint64_t)e[i].start * v->block_size;
        uint64_t length = (uint64_t)e[i].count * v->block_size;
        if (length > size - done) length = size - done;
        if (!read_at(v, at, (uint32_t)length, data + done)) { free(data); return NULL; }
        done += (uint32_t)length;
    }
    if (done < size) { free(data); return NULL; }
    return data;
}
static size_t extent_record(const uint8_t *p, extent *out) {
    size_t n = 0;
    for (int i = 0; i < 3; ++i)
        if (be16(p + 4 * i + 2)) { out[n].start = be16(p + 4 * i); out[n].count = be16(p + 4 * i + 2); n++; }
    return n;
}

/* The leaf records of a B*-tree file, in order. */
typedef struct records { const uint8_t **at; uint16_t *size; size_t count, capacity; } records;
static int leaf_records(const uint8_t *tree, uint32_t size, records *out) {
    memset(out, 0, sizeof *out);
    if (size < 512) return 0;
    const uint32_t node_size = be16(tree + 14 + 18);
    if (node_size < 512 || node_size > 32768 || size < node_size) return 0;
    const uint32_t nodes = size / node_size;
    uint32_t n = be32(tree + 14 + 10);
    for (uint32_t visited = 0; n && n < nodes && visited < nodes; ++visited) {
        const uint8_t *node = tree + (size_t)n * node_size;
        if (node[8] != 0xFF) return 0;   /* ndType: a leaf node is -1 */
        const uint16_t count = be16(node + 10);
        if ((uint32_t)count * 2 + 16 > node_size) return 0;
        for (uint16_t i = 0; i < count; ++i) {
            const uint16_t start = be16(node + node_size - 2 * (i + 1)), end = be16(node + node_size - 2 * (i + 2));
            if (start < 14 || end <= start || end > node_size) return 0;
            if (out->count == out->capacity) {
                out->capacity = out->capacity ? out->capacity * 2 : 256;
                const uint8_t **a = realloc(out->at, out->capacity * sizeof *a);
                if (!a) return 0;
                out->at = a;
                uint16_t *s = realloc(out->size, out->capacity * sizeof *s);
                if (!s) return 0;
                out->size = s;
            }
            out->at[out->count] = node + start;
            out->size[out->count++] = (uint16_t)(end - start);
        }
        n = be32(node);
    }
    return 1;
}
static void free_records(records *r) { free(r->at); free(r->size); memset(r, 0, sizeof *r); }

typedef struct catalog {
    uint8_t *tree, *overflow;
    uint32_t tree_size, overflow_size;
    records files, extents;
} catalog;

/* A file record's fork: its extents, then those of the overflow file. */
static uint8_t *read_fork(volume *v, catalog *c, const uint8_t *file, int resource, uint32_t *size) {
    extent e[EXTENTS];
    size_t n = extent_record(file + (resource ? 86 : 74), e);
    const uint32_t number = be32(file + 20);
    *size = be32(file + (resource ? 36 : 26));
    /* Overflow records are keyed by the start block in the fork; the tree
     * keeps them in order. */
    for (size_t i = 0; i < c->extents.count && n + 3 <= EXTENTS; ++i) {
        const uint8_t *r = c->extents.at[i];
        if (c->extents.size[i] < 20 || r[0] < 7) continue;
        if (r[1] == (resource ? 0xFF : 0x00) && be32(r + 2) == number) n += extent_record(r + 8, e + n);
    }
    return read_extents(v, e, n, *size);
}

/* A resource of a resource fork, or NULL; *size its length. */
static const uint8_t *resource(const uint8_t *fork, uint32_t fork_size, const char *type, int16_t id, uint32_t *size) {
    if (fork_size < 16) return NULL;
    const uint32_t data = be32(fork), map = be32(fork + 4), map_size = be32(fork + 12);
    if (map >= fork_size || map_size > fork_size - map || map_size < 30) return NULL;
    const uint8_t *m = fork + map;
    const uint32_t types = be16(m + 24);
    if (types + 2 > map_size) return NULL;
    const uint8_t *t = m + types;
    const uint32_t count = (uint32_t)(be16(t) + 1) & 0xFFFF;
    for (uint32_t i = 0; i < count && types + 2 + 8 * (i + 1) <= map_size; ++i) {
        const uint8_t *entry = t + 2 + 8 * i;
        if (memcmp(entry, type, 4)) continue;
        const uint32_t refs = (uint32_t)be16(entry + 4) + 1, list = be16(entry + 6);
        for (uint32_t k = 0; k < refs && types + list + 12 * (k + 1) <= map_size; ++k) {
            const uint8_t *ref = t + list + 12 * k;
            if ((int16_t)be16(ref) != id) continue;
            const uint32_t at = data + (be32(ref + 4) & 0xFFFFFF);
            if (at + 4 > fork_size) return NULL;
            const uint32_t length = be32(fork + at);
            if (length > fork_size - at - 4) return NULL;
            *size = length;
            return fork + at + 4;
        }
    }
    return NULL;
}
/* The IDs of a resource type's resources (at most capacity). */
static size_t resource_ids(const uint8_t *fork, uint32_t fork_size, const char *type, int16_t *ids, size_t capacity) {
    if (fork_size < 16) return 0;
    const uint32_t map = be32(fork + 4), map_size = be32(fork + 12);
    if (map >= fork_size || map_size > fork_size - map || map_size < 30) return 0;
    const uint8_t *m = fork + map;
    const uint32_t types = be16(m + 24);
    if (types + 2 > map_size) return 0;
    const uint8_t *t = m + types;
    const uint32_t count = (uint32_t)(be16(t) + 1) & 0xFFFF;
    size_t n = 0;
    for (uint32_t i = 0; i < count && types + 2 + 8 * (i + 1) <= map_size; ++i) {
        const uint8_t *entry = t + 2 + 8 * i;
        if (memcmp(entry, type, 4)) continue;
        const uint32_t refs = (uint32_t)be16(entry + 4) + 1, list = be16(entry + 6);
        for (uint32_t k = 0; k < refs && n < capacity && types + list + 12 * (k + 1) <= map_size; ++k)
            ids[n++] = (int16_t)be16(t + list + 12 * k);
    }
    return n;
}
/* 'vers' 1 (else 2): BCD version and short version string. */
static void read_vers(const uint8_t *fork, uint32_t size, uint16_t *version, char *text, size_t capacity) {
    uint32_t length = 0;
    const uint8_t *vers = resource(fork, size, "vers", 1, &length);
    if (!vers || length < 7) vers = resource(fork, size, "vers", 2, &length);
    if (!vers || length < 7) return;
    *version = be16(vers);
    const uint32_t n = vers[6] < length - 7 ? vers[6] : length - 7;
    utf8(vers + 7, n, text, capacity);
}
/* A 'gbly' resource's box flags. */
static size_t read_gbly(const uint8_t *fork, uint32_t size, uint16_t *boxes, size_t capacity, int *present) {
    uint32_t length = 0;
    const uint8_t *g = resource(fork, size, "gbly", -16385, &length);
    if (!g || length < 8) return 0;
    if (present) *present = 1;
    size_t n = be16(g + 6);
    if (n > (length - 8) / 2) n = (length - 8) / 2;
    if (n > capacity) n = capacity;
    for (size_t i = 0; i < n; ++i) boxes[i] = be16(g + 8 + 2 * i);
    return n;
}

static int pascal_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn) {
    if (an != bn) return 0;
    for (size_t i = 0; i < an; ++i) {
        uint8_t x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x = (uint8_t)(x - 32);
        if (y >= 'a' && y <= 'z') y = (uint8_t)(y - 32);
        if (x != y) return 0;
    }
    return 1;
}

/* The volume's System Folder. */
static void system_folder(volume *v, mc_volume_info *info) {
    const uint32_t blessed = be32(v->mdb + 92);
    if (!blessed) return;
    catalog c;
    memset(&c, 0, sizeof c);
    extent e[EXTENTS];
    c.tree_size = be32(v->mdb + 146);
    c.overflow_size = be32(v->mdb + 130);
    c.overflow = read_extents(v, e, extent_record(v->mdb + 134, e), c.overflow_size);
    if (c.overflow && !leaf_records(c.overflow, c.overflow_size, &c.extents)) free_records(&c.extents);
    /* The catalog file itself may have overflow extents. */
    size_t n = extent_record(v->mdb + 150, e);
    for (size_t i = 0; i < c.extents.count && n + 3 <= EXTENTS; ++i) {
        const uint8_t *r = c.extents.at[i];
        if (c.extents.size[i] >= 20 && r[1] == 0 && be32(r + 2) == 4) n += extent_record(r + 8, e + n);
    }
    c.tree = read_extents(v, e, n, c.tree_size);
    if (!c.tree || !leaf_records(c.tree, c.tree_size, &c.files)) goto done;
    /* The System and Finder names from the boot blocks. */
    uint8_t boot[1024];
    uint8_t system_name[16] = {6, 'S', 'y', 's', 't', 'e', 'm'}, finder_name[16] = {6, 'F', 'i', 'n', 'd', 'e', 'r'};
    if (read_at(v, v->base, sizeof boot, boot) && boot[0] == 'L' && boot[1] == 'K') {
        if (boot[10] && boot[10] <= 15) memcpy(system_name, boot + 10, 16);
        if (boot[26] && boot[26] <= 15) memcpy(finder_name, boot + 26, 16);
        /* System 6 pads the names with spaces. */
        while (system_name[0] && system_name[system_name[0]] == ' ') system_name[0]--;
        while (finder_name[0] && finder_name[finder_name[0]] == ' ') finder_name[0]--;
    }
    /* Folders directly inside the System Folder (Extensions, Control Panels). */
    uint32_t inside[64];
    size_t folders = 0;
    for (size_t i = 0; i < c.files.count; ++i) {
        const uint8_t *r = c.files.at[i];
        const size_t key = r[0], start = (key + 2) & ~(size_t)1;
        if (key < 6 || c.files.size[i] < start + 10) continue;
        if (be32(r + 2) == blessed && r[start] == 1 && folders < 64) inside[folders++] = be32(r + start + 6);
    }
    for (size_t i = 0; i < c.files.count; ++i) {
        const uint8_t *r = c.files.at[i];
        const size_t key = r[0], start = (key + 2) & ~(size_t)1;
        if (key < 6 || r[6] > key - 6 || c.files.size[i] < start + 102 || r[start] != 2) continue;
        const uint32_t parent = be32(r + 2);
        const uint8_t *file = r + start, *name = r + 7;
        const size_t name_length = r[6];
        int in_folder = parent == blessed;
        for (size_t k = 0; k < folders && !in_folder; ++k) in_folder = parent == inside[k];
        if (!in_folder) continue;
        if (pascal_equal(name, name_length, (const uint8_t *)"MODE32", 6) ||
            pascal_equal(name, name_length, (const uint8_t *)"32-Bit System Enabler", 21))
            info->mode32 = 1;
        if (parent != blessed) continue;
        const int is_system = (!memcmp(file + 4, "zsys", 4) || !memcmp(file + 4, "ZSYS", 4)) &&
                              pascal_equal(name, name_length, system_name + 1, system_name[0]);
        const int is_finder = !memcmp(file + 4, "FNDR", 4) && pascal_equal(name, name_length, finder_name + 1, finder_name[0]);
        const int is_enabler = !memcmp(file + 4, "gbly", 4);
        if (!is_system && !is_finder && !is_enabler) continue;
        uint32_t size = 0;
        uint8_t *fork = read_fork(v, &c, file, 1, &size);
        if (is_system) {
            info->bootable = 1;
            utf8(name, name_length, info->system_name, sizeof info->system_name);
            if (fork) {
                read_vers(fork, size, &info->system_version, info->system_text, sizeof info->system_text);
                info->system_unversioned = !info->system_version;
                info->system_box_count = read_gbly(fork, size, info->system_boxes, MC_VOLUME_BOXES, &info->system_gbly);
                info->system_ptch_count = resource_ids(fork, size, "PTCH", info->system_ptch,
                                                       sizeof info->system_ptch / sizeof *info->system_ptch);
            }
        } else if (is_finder) {
            utf8(name, name_length, info->finder_name, sizeof info->finder_name);
            if (fork) read_vers(fork, size, &info->finder_version, info->finder_text, sizeof info->finder_text);
        } else if (info->enabler_count < MC_VOLUME_ENABLERS) {
            mc_enabler *en = &info->enablers[info->enabler_count++];
            utf8(name, name_length, en->name, sizeof en->name);
            if (fork) en->box_count = read_gbly(fork, size, en->boxes, sizeof en->boxes / sizeof *en->boxes, NULL);
        }
        free(fork);
    }
done:
    free_records(&c.files);
    free_records(&c.extents);
    free(c.tree);
    free(c.overflow);
}

/* An MFS volume's System file (Inside Macintosh II, "The File Manager": the
 * master directory block at logical block 2, drDirSt at 14, drBlLen at 16,
 * drNmAlBlks at 18, drAlBlkSiz at 20, drAlBlSt at 28, the allocation block
 * map of 12-bit entries from byte 64 (0 free, 1 last block of a file,
 * entry n for allocation block n, the first being 2); the directory's file
 * entries in the logical blocks from drDirSt, none across a block's end,
 * the first byte flFlags with bit 7 set for a used entry, flUsrWds at 2
 * with the file type, flStBlk at 22, flLgLen at 24, flRStBlk at 32,
 * flRLgLen at 34, the name at 50): a flat volume has no System Folder, and
 * is a startup disk when it holds the System file, type 'ZSYS', named as
 * the boot blocks say. */
static uint8_t *mfs_fork(volume *v, const uint8_t *map, uint32_t blocks, uint32_t start, uint32_t length) {
    const uint32_t block_size = be32(v->mdb + 20), first = be16(v->mdb + 28);
    if (!length || length > 4u * 1024 * 1024 || !block_size) return NULL;
    uint8_t *data = calloc(1, length);
    if (!data) return NULL;
    uint32_t done = 0, n = start;
    for (uint32_t steps = 0; done < length && n >= 2 && n < blocks + 2 && steps < blocks; ++steps) {
        uint32_t chunk = length - done < block_size ? length - done : block_size;
        const uint32_t i = n - 2, e = i & 1 ? (uint32_t)(map[i / 2 * 3 + 1] & 15) << 8 | map[i / 2 * 3 + 2] : (uint32_t)map[i / 2 * 3] << 4 | map[i / 2 * 3 + 1] >> 4;
        if (!read_at(v, v->base + (uint64_t)first * 512 + (uint64_t)i * block_size, chunk, data + done)) break;
        done += chunk;
        if (e == 1) break;
        n = e;
    }
    if (done < length) { free(data); return NULL; }
    return data;
}

static void mfs_system(volume *v, mc_volume_info *info) {
    const uint32_t blocks = be16(v->mdb + 18), dir = be16(v->mdb + 14), dir_blocks = be16(v->mdb + 16);
    if (!blocks || blocks > 4096 || !dir_blocks || dir_blocks > 64) return;
    const uint32_t map_size = (blocks * 3 + 1) / 2;
    uint8_t *map = calloc(1, map_size + 3);
    uint8_t *directory = calloc(dir_blocks, 512);
    if (!map || !directory || !read_at(v, v->base + 1024 + 64, map_size, map) || !read_at(v, v->base + (uint64_t)dir * 512, dir_blocks * 512, directory)) goto done;
    uint8_t boot[1024];
    uint8_t system_name[16] = {6, 'S', 'y', 's', 't', 'e', 'm'}, finder_name[16] = {6, 'F', 'i', 'n', 'd', 'e', 'r'};
    if (read_at(v, v->base, sizeof boot, boot) && boot[0] == 'L' && boot[1] == 'K') {
        if (boot[10] && boot[10] <= 15) memcpy(system_name, boot + 10, 16);
        if (boot[26] && boot[26] <= 15) memcpy(finder_name, boot + 26, 16);
        while (system_name[0] && system_name[system_name[0]] == ' ') system_name[0]--;
        while (finder_name[0] && finder_name[finder_name[0]] == ' ') finder_name[0]--;
    }
    for (uint32_t b = 0; b < dir_blocks; ++b) {
        const uint8_t *block = directory + (size_t)b * 512;
        for (size_t at = 0; at + 51 <= 512 && block[at] & 0x80;) {
            const uint8_t *file = block + at;
            const size_t name_length = file[50];
            if (at + 51 + name_length > 512) break;
            const int is_system = !memcmp(file + 2, "ZSYS", 4) && pascal_equal(file + 51, name_length, system_name + 1, system_name[0]);
            const int is_finder = !memcmp(file + 2, "FNDR", 4) && pascal_equal(file + 51, name_length, finder_name + 1, finder_name[0]);
            if (is_system || is_finder) {
                uint32_t size = be32(file + 34);
                uint8_t *fork = mfs_fork(v, map, blocks, be16(file + 32), size);
                if (is_system) {
                    info->bootable = 1;
                    utf8(file + 51, name_length, info->system_name, sizeof info->system_name);
                    if (fork) {
                        read_vers(fork, size, &info->system_version, info->system_text, sizeof info->system_text);
                        info->system_unversioned = !info->system_version;
                        info->system_box_count = read_gbly(fork, size, info->system_boxes, MC_VOLUME_BOXES, &info->system_gbly);
                        info->system_ptch_count = resource_ids(fork, size, "PTCH", info->system_ptch,
                                                               sizeof info->system_ptch / sizeof *info->system_ptch);
                    }
                } else {
                    utf8(file + 51, name_length, info->finder_name, sizeof info->finder_name);
                    if (fork) read_vers(fork, size, &info->finder_version, info->finder_text, sizeof info->finder_text);
                }
                free(fork);
            }
            at += (51 + name_length + 1) & ~(size_t)1;
        }
    }
done:
    free(map);
    free(directory);
}

/* HFS Plus: TN1150, Volume Header, Fork Data Structure, B-Trees and
 * Catalog File. Only the blessed CoreServices folder is searched, for
 * SystemVersion.plist; no host filesystem mounts or writes are involved. */
static uint64_t plus_size(const uint8_t *fork) { return (uint64_t)be32(fork) << 32 | be32(fork + 4); }

static int plus_read(volume *v, const uint8_t *fork, uint64_t offset, uint32_t count, void *buffer) {
    if (offset > plus_size(fork) || count > plus_size(fork) - offset) return 0;
    uint8_t *out = buffer;
    if (v->base > v->end) return 0;
    for (unsigned i = 0; i < 8 && count; ++i) {
        const uint32_t start = be32(fork + 16 + 8 * i), blocks = be32(fork + 20 + 8 * i);
        const uint64_t bytes = (uint64_t)blocks * v->block_size;
        if (offset >= bytes) { offset -= bytes; continue; }
        const uint32_t n = bytes - offset < count ? (uint32_t)(bytes - offset) : count;
        const uint64_t physical = (uint64_t)start * v->block_size;
        if (physical > v->end - v->base || offset > v->end - v->base - physical ||
            !read_at(v, v->base + physical + offset, n, out)) return 0;
        out += n; count -= n; offset = 0;
    }
    return count == 0;
}

static int plus_version_fork(volume *v, const uint8_t *catalog_fork, uint32_t folder, uint8_t *fork) {
    uint8_t head[64];
    if (plus_size(catalog_fork) > 64u * 1024 * 1024) return 0;
    if (!plus_read(v, catalog_fork, 0, sizeof head, head)) return 0;
    const unsigned size = be16(head + 32);
    if (size < 512 || size > 32768 || (size & (size - 1))) return 0;
    uint32_t leaf = be32(head + 24);
    uint64_t limit = plus_size(catalog_fork) / size;
    uint8_t *node = malloc(size);
    if (!node) return 0;
    int found = 0;
    while (leaf && limit--) {
        if (!plus_read(v, catalog_fork, (uint64_t)leaf * size, size, node) || node[8] != 0xFF) break;
        const unsigned n = be16(node + 10);
        if (n > (size - 14u) / 2u - 1u) break;
        const unsigned data_end = size - 2u * (n + 1u);
        for (unsigned i = 0; i < n; ++i) {
            const unsigned at = be16(node + size - 2u * (i + 1u));
            if (at < 14 || at + 8 > data_end) continue;
            const unsigned key = be16(node + at), chars = be16(node + at + 6);
            if (key < 6 || chars > (key - 6u) / 2u || at + 2u + key > data_end || be32(node + at + 2) != folder) continue;
            static const char name[] = "SystemVersion.plist";
            if (chars != sizeof name - 1) continue;
            unsigned j = 0;
            while (j < chars && be16(node + at + 8 + 2u * j) == (unsigned char)name[j]) ++j;
            const unsigned rec = at + 2u + key;
            if (j != chars || rec + 168u > data_end || be16(node + rec) != 2) continue;
            memcpy(fork, node + rec + 88, 80); found = 1; break;
        }
        if (found) break;
        leaf = be32(node);
    }
    free(node);
    return found;
}

static void plus_system(volume *v, uint64_t base, uint64_t end, mc_volume_info *info) {
    uint8_t header[512], fork[80];
    v->base = base; v->end = end;
    if (!read_at(v, base + 1024, sizeof header, header)) return;
    if (be16(header) != 0x482B && be16(header) != 0x4858) return;
    const uint32_t folder = be32(header + 100);  /* finderInfo[5]: blessed Mac OS X folder */
    v->block_size = be32(header + 40);
    if (!folder || v->block_size < 512 || (v->block_size & (v->block_size - 1))) return;
    info->mac_os_x = info->bootable = 1;
    snprintf(info->system_name, sizeof info->system_name, "Mac OS X");
    if (!plus_version_fork(v, header + 272, folder, fork)) return;
    const uint64_t size = plus_size(fork);
    if (!size || size > 65536) return;
    char *text = calloc((size_t)size + 1, 1);
    if (!text) return;
    if (plus_read(v, fork, 0, (uint32_t)size, text)) {
        const char *key = strstr(text, "<key>ProductVersion</key>");
        const char *value = key ? strstr(key + 25, "<string>") : NULL;
        unsigned major = 0, minor = 0, patch = 0;
        if (value && sscanf(value + 8, "%u.%u.%u", &major, &minor, &patch) >= 2 && major < 100 && minor < 10 && patch < 10) {
            info->system_version = (uint16_t)((major / 10) << 12 | (major % 10) << 8 | minor << 4 | patch);
            mc_version_text(info->system_version, info->system_text, sizeof info->system_text);
        }
    }
    free(text);
}

/* The volume at base: HFS (read), or recognised only. */
static int volume_at(volume *v, uint64_t base, uint64_t end, mc_volume_info *info) {
    v->base = base;
    v->end = end;
    if (!read_at(v, base + 1024, 512, v->mdb)) return 0;
    const uint16_t signature = be16(v->mdb);
    if (signature == 0x482B || signature == 0x4858) {
        info->kind = MC_IMAGE_HFS_PLUS; plus_system(v, base, end, info); return 1;
    }
    if (signature == 0xD2D7) {
        info->kind = MC_IMAGE_MFS;
        utf8(v->mdb + 37, v->mdb[36] <= 27 ? v->mdb[36] : 27, info->volume, sizeof info->volume);
        mfs_system(v, info);
        return 1;
    }
    if (signature != 0x4244) return 0;
    v->block_size = be32(v->mdb + 20);
    v->first_block = be16(v->mdb + 28);
    if (!v->block_size || v->block_size % 512) return 0;
    utf8(v->mdb + 37, v->mdb[36] <= 27 ? v->mdb[36] : 27, info->volume, sizeof info->volume);
    if (be16(v->mdb + 124) == 0x482B) {
        info->kind = MC_IMAGE_HFS_PLUS;
        const uint64_t embedded = base + (uint64_t)v->first_block * 512 + (uint64_t)be16(v->mdb + 126) * v->block_size;
        const uint64_t length = (uint64_t)be16(v->mdb + 128) * v->block_size;
        if (embedded < end && length <= end - embedded) plus_system(v, embedded, embedded + length, info);
        return 1;
    }
    system_folder(v, info);
    return 1;
}

/* The number of records the ROM's volume check walks for a tree file of
 * `size` bytes and records of at least `record` bytes: size / record rounded
 * up, computed with DIVU.W (M68000 PRM 4-96: on overflow the destination is
 * unaffected, so the size's low word stands for the quotient and its high
 * word for the remainder), in a word. The walk needs one more step than the
 * tree has records to find its end. */
static uint32_t check_limit(uint32_t size, uint32_t record) {
    uint32_t q = size / record, r = size % record;
    if (q > 0xFFFFu) { q = size & 0xFFFFu; r = size >> 16; }
    return (q + (r != 0)) & 0xFFFFu;
}

static int left_mounted_at(volume *v, uint8_t out[512]) {
    if (!read_at(v, v->base + 1024, 512, v->mdb) || be16(v->mdb) != 0x4244 || be16(v->mdb + 124) == 0x482B) return 0;
    if (be16(v->mdb + 10) & 0x0100u) return 0;          /* unmounted cleanly */
    v->block_size = be32(v->mdb + 20);
    v->first_block = be16(v->mdb + 28);
    if (!v->block_size || v->block_size % 512) return 0;
    int result = 0;
    catalog c;
    memset(&c, 0, sizeof c);
    extent e[EXTENTS];
    c.overflow_size = be32(v->mdb + 130);
    c.tree_size = be32(v->mdb + 146);
    if (c.tree_size / 70 <= 0xFFFFu && c.overflow_size / 12 <= 0xFFFFu) return 0;   /* no overflow in the limits */
    c.overflow = read_extents(v, e, extent_record(v->mdb + 134, e), c.overflow_size);
    if (!c.overflow || !leaf_records(c.overflow, c.overflow_size, &c.extents)) goto done;
    size_t n = extent_record(v->mdb + 150, e);
    for (size_t i = 0; i < c.extents.count && n + 3 <= EXTENTS; ++i) {
        const uint8_t *r = c.extents.at[i];
        if (c.extents.size[i] >= 20 && r[1] == 0 && be32(r + 2) == 4) n += extent_record(r + 8, e + n);
    }
    c.tree = read_extents(v, e, n, c.tree_size);
    if (!c.tree || !leaf_records(c.tree, c.tree_size, &c.files)) goto done;
    /* The catalog walk starts at the root's thread record (key: parent 2,
     * no name) and counts what follows it: directory records (cdrType 1)
     * and file records (2), those in the root (parent 2) apart, and the
     * highest dirDirID or filFlNum. */
    uint32_t after = 0, files = 0, dirs = 0, root_files = 0, root_dirs = 0, top = 0;
    for (size_t i = 0; i < c.files.count; ++i) {
        const uint8_t *r = c.files.at[i];
        const size_t key = r[0], start = (key + 2) & ~(size_t)1;
        if (key < 6 || c.files.size[i] < start + 24) goto done;
        const uint32_t parent = be32(r + 2);
        if (parent < 2 || (parent == 2 && r[6] == 0)) continue;
        after++;
        const uint8_t *d = r + start;
        if (d[0] == 1) {
            dirs++;
            root_dirs += parent == 2;
            if (be32(d + 6) > top) top = be32(d + 6);
        } else if (d[0] == 2) {
            files++;
            root_files += parent == 2;
            if (be32(d + 20) > top) top = be32(d + 20);
        }
    }
    if (check_limit(c.tree_size, 70) > after && check_limit(c.overflow_size, 12) > c.extents.count) goto done;
    memcpy(out, v->mdb, 512);
    out[10] |= 0x01;                                    /* drAtrb bit 8 */
    out[12] = (uint8_t)(root_files >> 8); out[13] = (uint8_t)root_files;            /* drNmFls */
    if (top >= be32(v->mdb + 30)) {                                                 /* drNxtCNID */
        const uint32_t next = top + 1;
        out[30] = (uint8_t)(next >> 24); out[31] = (uint8_t)(next >> 16); out[32] = (uint8_t)(next >> 8); out[33] = (uint8_t)next;
    }
    out[82] = (uint8_t)(root_dirs >> 8); out[83] = (uint8_t)root_dirs;              /* drNmRtDirs */
    out[84] = (uint8_t)(files >> 24); out[85] = (uint8_t)(files >> 16); out[86] = (uint8_t)(files >> 8); out[87] = (uint8_t)files;
    out[88] = (uint8_t)(dirs >> 24); out[89] = (uint8_t)(dirs >> 16); out[90] = (uint8_t)(dirs >> 8); out[91] = (uint8_t)dirs;
    result = 1;
done:
    free_records(&c.files);
    free_records(&c.extents);
    free(c.tree);
    free(c.overflow);
    return result;
}

int mc_hfs_left_mounted(mc_disk *disk, uint64_t *offset, uint8_t mdb[512]) {
    volume v;
    memset(&v, 0, sizeof v);
    v.disk = disk;
    v.end = mc_disk_sectors(disk) * mc_disk_sector_size(disk);
    uint8_t block[512];
    if (read_at(&v, 0, 512, block) && block[0] == 'E' && block[1] == 'R') {
        uint32_t entries = 1;
        for (uint32_t i = 1; i <= entries && i < 64; ++i) {
            if (!read_at(&v, (uint64_t)i * 512, 512, block) || block[0] != 'P' || block[1] != 'M') break;
            entries = be32(block + 4);
            if (strncmp((const char *)block + 48, "Apple_HFS", 32)) continue;
            const uint64_t start = (uint64_t)be32(block + 8) * 512, length = (uint64_t)be32(block + 12) * 512;
            const uint64_t end = v.end;
            if (start >= end) continue;
            v.base = start;
            v.end = start + length < end ? start + length : end;
            if (left_mounted_at(&v, mdb)) { *offset = start + 1024; return 1; }
            v.end = end;
        }
        return 0;
    }
    v.base = 0;
    if (!left_mounted_at(&v, mdb)) return 0;
    *offset = 1024;
    return 1;
}

int mc_volume_inspect(const char *path, mc_volume_info *info, char *error, size_t capacity) {
    memset(info, 0, sizeof *info);
    volume v;
    memset(&v, 0, sizeof v);
    v.disk = mc_disk_open(path, 0, 512, error, capacity);
    if (!v.disk) return 0;
    info->bytes = mc_disk_sectors(v.disk) * 512u;
    /* A DiskCopy 4.2 header: mc_disk_open took the data after it. */
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t h[0x54];
        if (fread(h, 1, sizeof h, f) == sizeof h && h[0] <= 63 && h[0x52] == 1 && h[0x53] == 0 &&
            be32(h + 0x40) == info->bytes)
            info->diskcopy = 1;
        fclose(f);
    }
    uint8_t block[512];
    const uint64_t end = info->bytes;
    v.end = end;
    if (read_at(&v, 0, 512, block) && block[0] == 'E' && block[1] == 'R') {
        /* The partition map: entries at 512-byte blocks, or at the driver
         * descriptor's block size (CD-ROMs of 2048-byte blocks). */
        const uint32_t sizes[2] = {512, be16(block + 2)};
        for (int s = 0; s < 2 && info->kind == MC_IMAGE_UNKNOWN; ++s) {
            const uint32_t unit = sizes[s];
            if (unit < 512 || unit > 4096 || (s && unit == 512)) continue;
            uint32_t entries = 1;
            for (uint32_t i = 1; i <= entries && i < 64; ++i) {
                if (!read_at(&v, (uint64_t)i * unit, 512, block) || block[0] != 'P' || block[1] != 'M') break;
                entries = be32(block + 4);
                if (strncmp((const char *)block + 48, "Apple_HFS", 32)) continue;
                const uint64_t start = (uint64_t)be32(block + 8) * unit, length = (uint64_t)be32(block + 12) * unit;
                if (start >= end) continue;
                if (volume_at(&v, start, start + length < end ? start + length : end, info)) {
                    if (info->kind == MC_IMAGE_UNKNOWN) info->kind = MC_IMAGE_PARTITIONED;
                    break;
                }
            }
        }
    } else if (volume_at(&v, 0, end, info)) {
        if (info->kind == MC_IMAGE_UNKNOWN) info->kind = MC_IMAGE_HFS;
    }
    if (info->kind == MC_IMAGE_UNKNOWN && end > 32769 + 5) {
        v.base = 0; v.end = end;
        if (read_at(&v, 32768, 512, block) && !memcmp(block + 1, "CD001", 5)) info->kind = MC_IMAGE_ISO;
    }
    mc_disk_close(v.disk);
    return 1;
}

const char *mc_version_text(uint16_t bcd, char *buffer, size_t capacity) {
    const unsigned major = (bcd >> 12) * 10 + (bcd >> 8 & 15), minor = bcd >> 4 & 15, bug = bcd & 15;
    if (bug) snprintf(buffer, capacity, "%u.%u.%u", major, minor, bug);
    else snprintf(buffer, capacity, "%u.%u", major, minor);
    return buffer;
}

const char *mc_volume_describe(const mc_volume_info *v, char *buffer, size_t capacity) {
    size_t used = 0;
#define ADD(...) do { if (used < capacity) { int n_ = snprintf(buffer + used, capacity - used, __VA_ARGS__); if (n_ > 0) used += (size_t)n_; } } while (0)
    ADD("%s%s", v->diskcopy ? "DiskCopy 4.2 image, " : "", mc_image_kind_name(v->kind));
    if (v->volume[0]) ADD(" \"%s\"", v->volume);
    if (v->mac_os_x) {
        ADD(", Mac OS X %s", v->system_text[0] ? v->system_text : "(version unknown)");
    } else if (v->bootable) {
        char text[16];
        ADD(", %s %s", v->system_version >= 0x0760 ? "Mac OS" : "System",
            v->system_text[0] ? v->system_text : v->system_version ? mc_version_text(v->system_version, text, sizeof text)
            : v->system_unversioned ? "(before 6.0: no 'vers' resource)" : "(version unknown)");
        if (v->finder_version) ADD(", Finder %s", v->finder_text[0] ? v->finder_text : mc_version_text(v->finder_version, text, sizeof text));
        for (size_t i = 0; i < v->enabler_count; ++i) {
            const char *name = v->enablers[i].name;
            if (!strncmp(name, "System Enabler ", 15)) name += 15;
            ADD("%s%s", i ? ", " : ", enablers ", name);
        }
    } else if (v->kind == MC_IMAGE_HFS || v->kind == MC_IMAGE_PARTITIONED || v->kind == MC_IMAGE_MFS) {
        ADD(", not a startup disk");
    } else if (v->kind == MC_IMAGE_HFS_PLUS) {
        ADD(", System Folder not examined");
    }
#undef ADD
    if (!capacity) return buffer;
    if (used >= capacity) buffer[capacity - 1] = 0;
    return buffer;
}

const char *mc_image_kind_name(int kind) {
    switch (kind) {
    case MC_IMAGE_HFS: return "HFS volume";
    case MC_IMAGE_PARTITIONED: return "Apple-partitioned disk, HFS";
    case MC_IMAGE_HFS_PLUS: return "Mac OS Extended (HFS+) volume";
    case MC_IMAGE_MFS: return "MFS volume";
    case MC_IMAGE_ISO: return "ISO 9660 disc";
    default: return "no volume Livadix recognises";
    }
}
