/* ROM identification (docs/NOTES.md): the 40 ROM images of the 68k machines
 * and the 19 Old World PowerPC ROMs Livadix targets, by header checksum and
 * size, oldest first; the controller firmware, declaration ROMs and New
 * World "Mac OS ROM" files by size and CRC32; and the index of the roms
 * folders. */
#include "romdb.h"
#include "mc_builtin_rom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define name_compare _stricmp
#define tell_file _ftelli64
#else
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#define name_compare strcasecmp
#define tell_file ftello
#endif

const uint8_t *mc_rom_builtin(size_t *size) {
    *size = sizeof mc_builtin_rom_image;
    return mc_builtin_rom_image;
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* ROMs whose header checksum covers only their first part: the Classic's
 * 512 KiB holds the SE's 256 KiB of code, which the sum covers, and the ROM
 * disk after it (the sum of the whole image does not match; the first 256
 * KiB's does). */
static const struct { uint32_t checksum, size, summed; } partial[] = {
    {0xa49f9914, 512 * 1024, 256 * 1024},
};
/* The Old World PowerPC ROMs (4 MiB): the sum covers the first 3 MiB, the
 * 68k ROM; the last MiB, the nanokernel, the 68LC040 emulator and the
 * other PowerPC code, is outside it (computed over all 19 images in
 * roms/ppc: each matches over 3 MiB, none over 1, 2 or 4). */
#define PPC_ROM_SIZE (4u * 1024 * 1024)
#define PPC_ROM_SUMMED (3u * 1024 * 1024)

int mc_rom_validate(const uint8_t *rom, size_t size, mc_rom_info *info) {
    if (!rom || !info || size < 4 || (size & 1u)) return 0;
    info->size = size;
    info->stored_checksum = be32(rom);
    size_t end = size == PPC_ROM_SIZE ? PPC_ROM_SUMMED : size;
    for (size_t k = 0; k < sizeof partial / sizeof *partial; ++k)
        if (partial[k].checksum == info->stored_checksum && partial[k].size == size) end = partial[k].summed;
    info->computed_checksum = 0;
    for (size_t i = 4; i < end; i += 2) info->computed_checksum += ((uint32_t)rom[i] << 8) | rom[i + 1];
    return info->computed_checksum == info->stored_checksum;
}

#define ROM(sum, kib, when, names, processor, name) {sum, (kib) * 1024u, when, names, processor, name ".rom"}
/* The 68000 Macs' dates: Apple's announcements of the machines; the
 * Plus's three ROMs are its 1986 revisions (uncertain: the months of the
 * second and third). */
static const mc_known_rom roms[] = {
    /* Livadix's own (models.h, MC_BUILTIN_ROM; tools/bootrom.py). */
    ROM(0x4c564458, 128, "2026-11", "Built-in (Livadix's own ROM)", "68000", "livadix"),
    ROM(0x28ba61ce, 64, "1984-01", "Macintosh 128K", "68000", "mac-128k"),
    ROM(0x28ba4e50, 64, "1984-09", "Macintosh 512K", "68000", "mac-512k"),
    ROM(0x4d1eeee1, 128, "1986-01", "Macintosh Plus (v1)", "68000", "mac-plus-v1"),
    ROM(0x4d1eeae1, 128, "1986-03", "Macintosh Plus (v2)", "68000", "mac-plus-v2"),
    ROM(0x4d1f8172, 128, "1986-10", "Macintosh Plus, 512Ke", "68000", "mac-plus"),
    ROM(0x97851db6, 256, "1987-03", "Macintosh II (800K drive, v1)", "68020", "mac-ii-v1"),
    ROM(0xb2e362a8, 256, "1987-03", "Macintosh SE", "68000", "mac-se"),
    ROM(0x9779d2c4, 256, "1987-12", "Macintosh II (800K drive, v2)", "68020", "mac-ii-v2"),
    ROM(0x97221136, 256, "1988-09", "Macintosh II FDHD, IIx, IIcx, SE/30", "68020/68030", "mac-iix"),
    ROM(0xb306e171, 256, "1989-08", "Macintosh SE FDHD", "68000", "mac-se-fdhd"),
    ROM(0x368cadfe, 512, "1989-09", "Macintosh IIci", "68030", "iici"),
    ROM(0x96ca3846, 256, "1989-09", "Macintosh Portable", "68HC000", "mac-portable"),
    ROM(0x4147dd77, 512, "1990-03", "Macintosh IIfx", "68030", "iifx"),
    ROM(0x350eacf0, 512, "1990-10", "Macintosh LC", "68020", "lc"),
    ROM(0x36b7fb6c, 512, "1990-10", "Macintosh IIsi", "68030", "iisi"),
    ROM(0xa49f9914, 512, "1990-10", "Macintosh Classic", "68000", "mac-classic"),
    ROM(0x3193670e, 512, "1991-10", "Macintosh Classic II", "68030", "classic-ii"),
    ROM(0x420dbff3, 1024, "1991-10", "Macintosh Quadra 700, Quadra 900, PowerBook 140, 145, 145B, 170", "68040/68030", "quadra-700"),
    ROM(0x96645f9c, 256, "1991-10", "PowerBook 100", "68HC000", "powerbook-100"),
    ROM(0x35c28f5f, 512, "1992-03", "Macintosh LC II", "68030", "lc-ii"),
    ROM(0x3dc27823, 1024, "1992-03", "Macintosh Quadra 950", "68040", "quadra-950"),
    ROM(0x4957eb49, 1024, "1992-10", "Macintosh IIvx, IIvi, Performa 600", "68030", "iivx"),
    ROM(0xe33b2724, 1024, "1992-10", "PowerBook 160, 165, 165c, 180, 180c", "68030", "powerbook-160"),
    ROM(0xecfa989b, 1024, "1992-10", "PowerBook Duo 210, 230, 250", "68030", "powerbook-duo-210"),
    ROM(0xec904829, 1024, "1993-02", "Macintosh LC III (older)", "68030", "lc-iii-v1"),
    ROM(0xecbbc41c, 1024, "1993-02", "Macintosh LC III, LC III+", "68030", "lc-iii"),
    ROM(0xecd99dc0, 1024, "1993-02", "Macintosh Color Classic", "68030", "color-classic"),
    ROM(0xf1a6f343, 1024, "1993-02", "Quadra 610, 650, Centris 610, 650", "68040/68LC040", "centris-610"),
    ROM(0xf1acad13, 1024, "1993-02", "Quadra 610, 650, 800, Centris 610, 650", "68040/68LC040", "quadra-800"),
    ROM(0x0024d346, 1024, "1993-10", "PowerBook Duo 270c", "68030", "powerbook-duo-270c"),
    ROM(0xede66cbd, 1024, "1993-10", "Color Classic II, LC 520, LC 550, Performa 275, 550, 560", "68030", "color-classic-ii"),
    ROM(0xeaf1678d, 1024, "1993-10", "Macintosh TV", "68030", "macintosh-tv"),
    ROM(0xff7439ee, 1024, "1993-10", "LC 475, LC 575, Quadra 605, Performa 475, 476, 575, 577, 578", "68LC040/68040", "quadra-605"),
    ROM(0x015621d7, 1024, "1994-04", "PowerBook Duo 280, 280c", "68LC040", "powerbook-duo-280"),
    ROM(0xb6909089, 2048, "1994-05", "PowerBook 520, 520c, 540, 540c", "68LC040", "powerbook-520"),
    ROM(0x06684214, 1024, "1994-07", "Macintosh LC 630, Quadra 630, Performa 630", "68LC040/68040", "quadra-630"),
    ROM(0xfda22562, 1024, "1994-07", "PowerBook 150", "68030", "powerbook-150"),
    ROM(0x5bf10fd1, 2048, "1994-09", "Quadra 660AV, 840AV", "68040", "quadra-840av"),
    ROM(0x064dc91d, 1024, "1995-04", "LC 580, Performa 580, 588", "68LC040", "performa-580"),
    ROM(0x4d27039c, 2048, "1995-08", "PowerBook 190, 190cs", "68LC040", "powerbook-190"),
    /* The Old World PowerPC ROMs: the dates are the
     * first machine's announcement (uncertain: which shipped first with the
     * PDM and 7200 ROMs' second versions, and the Pippins' months). */
    ROM(0x9feb69b3, 4096, "1994-03", "Power Macintosh 6100, 7100, 8100, Workgroup Server 6150, 8150", "601", "power-macintosh-6100"),
    ROM(0x9c7c98f7, 4096, "1994-04", "Workgroup Server 9150/80", "601", "workgroup-server-9150-80"),
    ROM(0x9b7a3aad, 4096, "1995-01", "Power Macintosh 7100/80, 8100/100, 8100/110", "601", "power-macintosh-7100-80"),
    ROM(0x63abfd3f, 4096, "1995-04", "Power Macintosh and Performa 5200, 5300, 6200, 6300", "603/603e", "power-macintosh-5200"),
    ROM(0x9b037f6f, 4096, "1995-04", "Workgroup Server 9150/120", "601", "workgroup-server-9150-120"),
    ROM(0x96cd923d, 4096, "1995-06", "Power Macintosh 7200, 7500, 8500, 9500 (first ROM)", "601/604", "power-macintosh-7200-v1"),
    ROM(0x9630c68b, 4096, "1995-08", "Power Macintosh 7200, 7500, 8500, 9500", "601/604", "power-macintosh-7200"),
    ROM(0x83c54f75, 4096, "1995-08", "PowerBook Duo 2300c", "603e", "powerbook-duo-2300c"),
    ROM(0x2bf65931, 4096, "1995-12", "Bandai Pippin (development ROM)", "603", "pippin-dev"),
    ROM(0x2bef21b7, 4096, "1996-03", "Bandai Pippin", "603", "pippin"),
    ROM(0x6f5724c0, 4096, "1996-04", "Power Macintosh and Performa 5400, 6400", "603e", "power-macintosh-6400"),
    ROM(0x83a21950, 4096, "1996-11", "PowerBook 1400c, 1400cs", "603e", "powerbook-1400"),
    ROM(0x960e4be9, 4096, "1997-02", "Power Macintosh 7300, 7600, 8600, 9600", "604/604e", "power-macintosh-7300"),
    ROM(0x6e92fe08, 4096, "1997-02", "Power Macintosh 5500, 6500, Twentieth Anniversary Macintosh", "603e", "power-macintosh-6500"),
    ROM(0x960fc647, 4096, "1997-08", "Power Macintosh 8600, 9600 (later ROM)", "604e", "power-macintosh-8600-300"),
    ROM(0x79d68d63, 4096, "1997-11", "Power Macintosh G3 (first ROM)", "750", "power-macintosh-g3-v1"),
    ROM(0x78f57389, 4096, "1998-04", "Power Macintosh G3, G3 Minitower, G3 All-in-One", "750", "power-macintosh-g3"),
    ROM(0xcbb01212, 4096, "1998-05", "PowerBook G3 Series", "750", "powerbook-g3-wallstreet"),
    ROM(0xb46ffb63, 4096, "1998-09", "PowerBook G3 Series (PDQ)", "750", "powerbook-g3-pdq"),
};
#define COUNT (sizeof roms / sizeof *roms)

const mc_known_rom *mc_romdb_find(uint32_t checksum, size_t size) {
    for (size_t i = 0; i < COUNT; ++i)
        if (roms[i].checksum == checksum && roms[i].size == size) return &roms[i];
    return NULL;
}
const mc_known_rom *mc_romdb_identify(const uint8_t *rom, size_t size) {
    return rom && size >= 4 ? mc_romdb_find(be32(rom), size) : NULL;
}
size_t mc_romdb_count(void) { return COUNT; }
const mc_known_rom *mc_romdb_entry(size_t index) { return index < COUNT ? &roms[index] : NULL; }

/* Controller firmware and declaration ROMs: sizes and CRC32s read from the
 * dumps (docs/NOTES.md, "Controller firmware"); the New World ROM files
 * after them. */
static const mc_firmware firmware[] = {
    {"egret-341s0850.bin", "Egret 341S0850", 4352, 0x4906ecd0, "LC, LC II"},
    {"egret-341s0851.bin", "Egret 341S0851", 4352, 0xea9ea6e4, "Classic II, IIvx, IIvi, Performa 600, LC III family, Quadra 900/950"},
    {"egret-344s0100.bin", "Egret 344S0100", 4352, 0x59e2b6b6, "IIsi"},
    {"cuda-341s0060.bin", "Cuda 341S0060", 4352, 0x0f5e7b4a, "LC 520 family, Quadra 630, LC 580"},
    {"cuda-341s0417.bin", "Cuda 341S0417", 4352, 0x571f24c9, "Color Classic"},
    {"cuda-341s0788.bin", "Cuda 341S0788", 4352, 0xdf6e1b43, "LC 475/575, Quadra 605, 660AV, 840AV"},
    {"cuda-341s0789.bin", "Cuda 341S0789", 4352, 0x682d2ace, "Macintosh TV"},
    {"pmu-pb140.bin", "PMU (M50753)", 6144, 0x1a32b5e5, "PowerBook 140, 145, 170"},
    {"pmu-pb160.bin", "PMU (M50753)", 6144, 0xf2df696c, "PowerBook 160, 165, 165c, 180, 180c"},
    {"pmu-portable.bin", "PMU (M50753)", 6144, 0x01dae148, "Macintosh Portable, PowerBook 100"},
    {"pmu-pge-boot.bin", "PG&E (68HC05) boot ROM", 512, 0x62d4dfed, "PowerBook Duo 210, 230, 250"},
    {"video-pb180c.bin", "Video declaration ROM", 32768, 0x810c75ad, "PowerBook 165c, 180c"},
    {"video-se30.bin", "Built-in video declaration ROM", 8192, 0xb74c3463, "SE/30"},
    {"card-mac-ii-video.bin", "Macintosh II Video Card 342-0008-A", 4096, 0xbf50850d, "Mac II family (optional card)"},
    {"card-display-8-24.bin", "Display Card 8-24 341-0868", 32768, 0x57f925fa, "Mac II family (optional card)"},
    {"card-ethernet-nb.bin", "EtherTalk NB Card 670-0210", 16384, 0xe3ae8c26, "NuBus models without built-in Ethernet (optional card)"},
    {"card-ethernet-lc.bin", "Ethernet LC Card 341-0842", 32768, 0x92310cb3, "LC-PDS models (optional card)"},
    /* The New World "Mac OS ROM" files (docs/NOTES.md, "New World ROM
     * files"): no machine's ROM, the file Open Firmware loads from the disk
     * (from the iMac on); sizes and CRC32s read from
     * the files, the machines their <COMPATIBLE> lists (1.1 and 1.1.2 have
     * none: "MacROM for CHRP"). */
#define NW(version, bytes, crc, machines) {"mac-os-rom-" version ".rom", "Mac OS ROM file " version " (New World)", bytes, crc, machines}
    NW("1.1", 1900274, 0xb5df73dd, "CHRP (the iMac)"),
    NW("1.1.2", 1901826, 0x2172203c, "CHRP (the iMac)"),
    NW("1.2", 2064564, 0xceace951, "iMac,1 PowerMac1,1 PowerBook1,1"),
    NW("1.2.1", 2064548, 0x497f0241, "iMac,1 PowerMac1,1 PowerBook1,1"),
    NW("1.4", 1945332, 0xfcce2265, "iMac,1 PowerMac1,1 PowerBook1,1"),
    NW("1.6", 1945746, 0x2f6572b4, "iMac,1 PowerMac1,1 PowerBook1,1"),
    NW("1.8.1", 2519706, 0x5148b324, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac1,2"),
    NW("2.5.1", 3612648, 0xfb6909a4, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1"),
    NW("3.0", 2220334, 0x045cf212, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1"),
    NW("3.7", 2429782, 0x10251dd3, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1 PowerBook3,1 PowerMac3,2"),
    NW("3.8", 2430398, 0x65a0a6e9, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1 PowerBook3,1 PowerMac3,2"),
    NW("6.1", 2448470, 0x8da310ec, "iMac,1 PowerMac1,1 PowerBook1,1 PowerMac2,1 PowerMac3,1 PowerBook2,1 PowerBook3,1 PowerMac2,2 PowerBook2,2 PowerMac3,3 PowerMac5,1"),
    NW("7.5.1", 3592686, 0xb84bb1ee, "MacRISC"),
    NW("8.4", 2552522, 0xd75e79cf, "MacRISC"),
    NW("9.0.1", 2763530, 0x4f3a69c5, "MacRISC"),
    NW("9.1.1", 2782414, 0x16c11580, "MacRISC"),
    NW("9.6.1", 2796422, 0xcc3c0aec, "MacRISC"),
    NW("9.8.1", 2796422, 0x0c50d766, "MacRISC PowerMac4,2 PowerBook4,3"),
    NW("10.2.1", 2796822, 0x5b788642, "MacRISC"),
#undef NW
};
#define FIRMWARE (sizeof firmware / sizeof *firmware)

size_t mc_firmware_count(void) { return FIRMWARE; }
const mc_firmware *mc_firmware_entry(size_t index) { return index < FIRMWARE ? &firmware[index] : NULL; }
static int firmware_size(uint64_t size) {
    for (size_t i = 0; i < FIRMWARE; ++i) if (firmware[i].size == size) return 1;
    return 0;
}
uint32_t mc_crc32(const uint8_t *data, size_t size) {
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc ^ 0xffffffffu;
}
const mc_firmware *mc_firmware_identify(const uint8_t *data, size_t size) {
    if (!data || !firmware_size(size)) return NULL;
    const uint32_t crc = mc_crc32(data, size);
    for (size_t i = 0; i < FIRMWARE; ++i)
        if (firmware[i].size == size && firmware[i].crc32 == crc) return &firmware[i];
    return NULL;
}
const mc_firmware *mc_firmware_find(const char *key) {
    for (size_t i = 0; key && i < FIRMWARE; ++i) {
        const size_t n = strlen(firmware[i].file) - 4;
        if (strlen(key) == n && !strncmp(firmware[i].file, key, n)) return &firmware[i];
    }
    return NULL;
}

/* --- the index of the roms folders --------------------------------------------- */

static int compare_names(const void *a, const void *b) {
    return name_compare(*(const char *const *)a, *(const char *const *)b);
}
/* The file names of a folder, sorted and NULL-terminated (none when the
 * folder cannot be listed); NULL when out of memory. */
static char **list_folder(const char *folder) {
    size_t count = 0, capacity = 16;
    char **names = malloc(capacity * sizeof *names);
    if (!names) return NULL;
#ifdef _WIN32
    char pattern[1100];
    snprintf(pattern, sizeof pattern, "%s*", folder);
    WIN32_FIND_DATAA entry;
    HANDLE h = FindFirstFileA(pattern, &entry);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const char *name = entry.cFileName;
#else
    DIR *dir = opendir(folder[0] ? folder : ".");
    if (dir) {
        for (struct dirent *e; (e = readdir(dir));) {
            const char *name = e->d_name;
            if (name[0] == '.') continue;
            if (e->d_type == DT_DIR) continue;
            if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {   /* a link to a folder, or a file system that does not say */
                char full[1100];
                struct stat st;
                snprintf(full, sizeof full, "%s%s", folder, name);
                if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) continue;
            }
#endif
            if (count + 1 >= capacity) {
                char **more = realloc(names, (capacity *= 2) * sizeof *names);
                if (!more) break;
                names = more;
            }
            const size_t n = strlen(name) + 1;
            if ((names[count] = malloc(n)) != NULL) memcpy(names[count++], name, n);
#ifdef _WIN32
        } while (FindNextFileA(h, &entry));
        FindClose(h);
    }
#else
        }
        closedir(dir);
    }
#endif
    names[count] = NULL;
    qsort(names, count, sizeof *names, compare_names);
    return names;
}
static void identify_file(mc_rom_file *f) {
    FILE *file = fopen(f->path, "rb");
    if (!file) return;
    uint8_t head[4] = {0};
    long long end = -1;
    if (fread(head, 1, 4, file) == 4 && !fseek(file, 0, SEEK_END)) end = (long long)tell_file(file);
    if (end > 0) {
        f->size = (uint64_t)end;
        f->checksum = ((uint32_t)head[0] << 24) | ((uint32_t)head[1] << 16) | ((uint32_t)head[2] << 8) | head[3];
        f->rom = mc_romdb_find(f->checksum, (size_t)f->size);
        if (!f->rom && firmware_size(f->size)) {
            uint8_t *data = malloc((size_t)f->size);
            if (data && !fseek(file, 0, SEEK_SET) && fread(data, 1, (size_t)f->size, file) == f->size) {
                f->crc32 = mc_crc32(data, (size_t)f->size);
                f->firmware = mc_firmware_identify(data, (size_t)f->size);
            }
            free(data);
        }
    }
    fclose(file);
}
static const mc_rom_file *preferred(const mc_rom_index *ix, const mc_known_rom *rom, const mc_firmware *fw) {
    const mc_rom_file *first = NULL;
    for (size_t i = 0; ix && i < ix->count; ++i) {
        const mc_rom_file *f = &ix->files[i];
        if ((rom && f->rom != rom) || (fw && f->firmware != fw) || (!rom && !fw)) continue;
        if (f->standard) return f;
        if (!first) first = f;
    }
    return first;
}
int mc_rom_index_scan(mc_rom_index *ix, const char *const *folders, size_t count) {
    memset(ix, 0, sizeof *ix);
    size_t capacity = 0;
    for (size_t k = 0; k < count && ix->folder_count < 2; ++k) {
        if (!folders[k]) continue;
        int seen = 0;
        for (size_t j = 0; j < ix->folder_count; ++j) seen |= !name_compare(ix->folders[j], folders[k]);
        if (seen) continue;
        snprintf(ix->folders[ix->folder_count++], sizeof ix->folders[0], "%s", folders[k]);
        char **names = list_folder(folders[k]);
        if (!names) { mc_rom_index_free(ix); return 0; }
        int ok = 1;
        for (char **n = names; ok && *n; ++n) {
            if (ix->count == capacity) {
                capacity = capacity ? capacity * 2 : 32;
                mc_rom_file *more = realloc(ix->files, capacity * sizeof *more);
                if (!more) { ok = 0; break; }
                ix->files = more;
            }
            mc_rom_file *f = &ix->files[ix->count];
            memset(f, 0, sizeof *f);
            const int w = snprintf(f->path, sizeof f->path, "%s%s", folders[k], *n);
            if (w <= 0 || w >= (int)sizeof f->path) continue;
            f->duplicate = -1;
            identify_file(f);
            const char *standard = f->rom ? f->rom->file : f->firmware ? f->firmware->file : NULL;
            f->standard = standard && !name_compare(*n, standard);
            ix->count++;
        }
        for (char **n = names; *n; ++n) free(*n);
        free(names);
        if (!ok) { mc_rom_index_free(ix); return 0; }
    }
    for (size_t i = 0; i < ix->count; ++i) {
        mc_rom_file *f = &ix->files[i];
        const char *slash = strrchr(f->path, '/'), *back = strrchr(f->path, '\\');
        if (back > slash) slash = back;
        f->name = slash ? slash + 1 : f->path;
    }
    /* Duplicates: the standard name wins, else the first found. */
    for (size_t i = 0; i < ix->count; ++i) {
        mc_rom_file *f = &ix->files[i];
        if (!f->rom && !f->firmware) continue;
        const mc_rom_file *used = preferred(ix, f->rom, f->rom ? NULL : f->firmware);
        f->duplicate = used && used != f ? (int)(used - ix->files) : -1;
    }
    return 1;
}
void mc_rom_index_free(mc_rom_index *ix) {
    if (!ix) return;
    free(ix->files);
    memset(ix, 0, sizeof *ix);
}
const mc_rom_file *mc_rom_index_rom(const mc_rom_index *ix, const mc_known_rom *rom) { return rom ? preferred(ix, rom, NULL) : NULL; }
const mc_rom_file *mc_rom_index_firmware(const mc_rom_index *ix, const mc_firmware *fw) { return fw ? preferred(ix, NULL, fw) : NULL; }
const char *mc_rom_file_describe(const mc_rom_index *ix, const mc_rom_file *f, char *buffer, size_t capacity) {
    int n;
    if (f->rom)
        n = snprintf(buffer, capacity, "Macintosh ROM %08X, %u KiB (%s, %s)", (unsigned)f->rom->checksum,
                     (unsigned)(f->rom->size / 1024), f->rom->models, f->rom->date);
    else if (f->firmware)
        n = snprintf(buffer, capacity, "%s, CRC32 %08x (%s)", f->firmware->part, (unsigned)f->firmware->crc32, f->firmware->models);
    else
        n = snprintf(buffer, capacity, "unknown");
    if (n < 0 || (size_t)n >= capacity) return buffer;
    if (f->duplicate >= 0 && ix && (size_t)f->duplicate < ix->count)
        snprintf(buffer + n, capacity - (size_t)n, "; duplicate of %s, not used", ix->files[f->duplicate].name);
    else if ((f->rom || f->firmware) && !f->standard)
        snprintf(buffer + n, capacity - (size_t)n, "; standard name %s", f->rom ? f->rom->file : f->firmware->file);
    return buffer;
}
