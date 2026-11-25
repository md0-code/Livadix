/* ROM identification (docs/NOTES.md).
 *
 * A Macintosh ROM image is known by its header checksum (bytes 0..3) and its
 * size; controller firmware and declaration ROMs, which carry no header
 * checksum, by their size and CRC32. File names are only a suggestion: each
 * known image has a standard file name in roms/ (a Macintosh ROM's is the key
 * of its first model, models.c, plus ".rom"), which wins when two
 * files hold the same image. */

#ifndef MC_ROMDB_H
#define MC_ROMDB_H

#include <stddef.h>
#include <stdint.h>

typedef struct mc_known_rom {
    uint32_t checksum, size;
    const char *date;          /* release, YYYY-MM */
    const char *models;        /* the machines that shipped with it */
    const char *cpu;
    const char *file;          /* standard file name in roms/ */
} mc_known_rom;

/* The checksum stored in a ROM image's header and the one computed over the
 * image, as the ROM's own start-up test does: the sum of the big-endian
 * 16-bit words after the four-byte header, modulo 2^32. */
typedef struct mc_rom_info {
    uint32_t stored_checksum, computed_checksum;
    size_t size;
} mc_rom_info;

/* Livadix's own ROM, generated from the current code/data and documents at
 * build time and embedded in the executable. The returned bytes are read-only. */
const uint8_t *mc_rom_builtin(size_t *size);

/* 1 when the image's computed checksum matches its header. */
int mc_rom_validate(const uint8_t *rom, size_t size, mc_rom_info *info);

/* The known ROM of an image (header checksum and size), or NULL. */
const mc_known_rom *mc_romdb_identify(const uint8_t *rom, size_t size);
const mc_known_rom *mc_romdb_find(uint32_t checksum, size_t size);
size_t mc_romdb_count(void);
const mc_known_rom *mc_romdb_entry(size_t index);

/* Controller firmware and declaration ROMs (docs/NOTES.md, "Controller
 * firmware"), and the New World "Mac OS ROM" files ("New World ROM files":
 * no machine's ROM; standard names mac-os-rom-VERSION.rom). */
typedef struct mc_firmware {
    const char *file;          /* standard file name in roms/; its stem is the key */
    const char *part;
    uint32_t size, crc32;
    const char *models;        /* the machines that carry it */
} mc_firmware;
size_t mc_firmware_count(void);
const mc_firmware *mc_firmware_entry(size_t index);
/* The firmware of an image (size and CRC32), or NULL. */
const mc_firmware *mc_firmware_identify(const uint8_t *data, size_t size);
/* The firmware whose standard file name is key + ".bin", or NULL. */
const mc_firmware *mc_firmware_find(const char *key);
/* CRC-32 as ISO 3309 / ITU-T V.42 define it (reflected polynomial
 * 0xEDB88320, initial and final value 0xFFFFFFFF), the one zip uses. */
uint32_t mc_crc32(const uint8_t *data, size_t size);

/* Every file of the roms folders, scanned once at start and identified by
 * content. */
typedef struct mc_rom_file {
    char path[1024];
    const char *name;          /* the file name, inside path */
    uint64_t size;
    uint32_t checksum;         /* header longword */
    uint32_t crc32;            /* computed for firmware sizes only */
    const mc_known_rom *rom; /* a Macintosh ROM, or NULL */
    const mc_firmware *firmware; /* firmware or declaration ROM, or NULL */
    int standard;              /* under its standard file name */
    int duplicate;             /* index of the file used for the same image, or -1 */
} mc_rom_file;
typedef struct mc_rom_index {
    mc_rom_file *files;
    size_t count;
    char folders[2][1024];     /* the folders scanned, each with its separator */
    size_t folder_count;
} mc_rom_index;
/* Scans each folder (a path ending in a separator; missing folders are
 * skipped), files in name order, the folders in the order given. Of files
 * holding the same image the one under the standard name is used, else the
 * first found; the others are duplicates. 0 when out of memory. */
int mc_rom_index_scan(mc_rom_index *index, const char *const *folders, size_t count);
void mc_rom_index_free(mc_rom_index *index);
/* The file used for a Macintosh ROM or a firmware image, or NULL. */
const mc_rom_file *mc_rom_index_rom(const mc_rom_index *index, const mc_known_rom *rom);
const mc_rom_file *mc_rom_index_firmware(const mc_rom_index *index, const mc_firmware *firmware);
/* What a file was recognised as, in words ("unknown" for neither). */
const char *mc_rom_file_describe(const mc_rom_index *index, const mc_rom_file *file, char *buffer, size_t capacity);

#endif /* MC_ROMDB_H */
