/* The core without a window: ROM identification, models, the
 * configuration, disk images and the machine interface (stub machine).
 * Every file is made here, under work/tests/core; nothing outside work/ is
 * read or written. */

#include "test.h"

#include "config.h"
#include "nvram.h"
#include "host.h"
#include "disk.h"
#include "machine.h"
#include "models.h"
#include "romdb.h"
#include "yamledit.h"
#include "hfs.h"
#include "vdisk.h"
#include "compat.h"
#include "profile.h"
#include "livadix_default_yaml.h"

#include <stdlib.h>

static char dir[1024];

static const char *path_of(const char *name) {
    static char path[4][1100];
    static unsigned next;
    char *p = path[next++ % 4];
    snprintf(p, 1100, "%s/%s", dir, name);
    return p;
}
static void write_bytes(const char *name, const void *data, size_t size) {
    FILE *f = fopen(path_of(name), "wb");
    CHECK(f != NULL);
    if (!f) return;
    CHECK(fwrite(data, 1, size, f) == size);
    fclose(f);
}
static void write_text(const char *name, const char *text) { write_bytes(name, text, strlen(text)); }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
/* A ROM image of the given size whose header holds `checksum`. */
static uint8_t *fake_rom(uint32_t checksum, size_t size) {
    uint8_t *rom = calloc(1, size);
    if (rom) be32(rom, checksum);
    return rom;
}

/* --- ROM identification ------------------------------------------------------ */

static void test_romdb(void) {
    CHECK_EQ(mc_romdb_count(), 60);
    for (size_t i = 0; i < mc_romdb_count(); ++i) {
        const mc_known_rom *a = mc_romdb_entry(i);
        CHECK(a->size == 64u * 1024 || a->size == 128u * 1024 || a->size == 256u * 1024 || a->size == 512u * 1024 || a->size == 1024u * 1024 ||
              a->size == 2048u * 1024 || a->size == 4096u * 1024);
        CHECK(strlen(a->file) > 4 && !strcmp(a->file + strlen(a->file) - 4, ".rom"));
        for (size_t j = 0; j < i; ++j) {
            CHECK(mc_romdb_entry(j)->checksum != a->checksum);
            CHECK(strcmp(mc_romdb_entry(j)->file, a->file) != 0);
        }
        /* The standard name is the key of a model of this ROM. */
        char key[64];
        snprintf(key, sizeof key, "%.*s", (int)(strlen(a->file) - 4), a->file);
        const mc_model *p = mc_model_find(a->checksum == MC_BUILTIN_ROM ? MC_MODEL_BUILTIN : key);   /* livadix.rom: the Built-in machine's */
        CHECK(p && p->rom == a->checksum);
    }
    /* The checksum: big-endian words after the header, modulo 2^32. */
    uint8_t image[64] = {0};
    uint32_t sum = 0;
    for (unsigned i = 4; i < sizeof image; i += 2) { image[i] = (uint8_t)(0xf0 + i); image[i + 1] = (uint8_t)(i * 7); sum += (uint32_t)image[i] << 8 | image[i + 1]; }
    be32(image, sum);
    mc_rom_info info;
    CHECK(mc_rom_validate(image, sizeof image, &info));
    CHECK_EQ(info.stored_checksum, sum);
    image[33] ^= 1;
    CHECK(!mc_rom_validate(image, sizeof image, &info));
    CHECK(!mc_rom_validate(image, 63, &info));
    uint8_t *rom = fake_rom(0x420dbff3, 1024 * 1024);
    const mc_known_rom *q700 = mc_romdb_identify(rom, 1024 * 1024);
    CHECK(q700 && !strcmp(q700->file, "quadra-700.rom"));
    CHECK(mc_romdb_identify(rom, 512 * 1024) == NULL);
    free(rom);
    /* An Old World PowerPC ROM (4 MiB): the sum covers its first 3 MiB. */
    rom = fake_rom(0, 4u << 20);
    if (rom) {
        uint32_t s = 0;
        for (size_t i = 4; i < (3u << 20); i += 2) { rom[i] = (uint8_t)(i >> 9); rom[i + 1] = (uint8_t)i; s += (uint32_t)rom[i] << 8 | rom[i + 1]; }
        be32(rom, s);
        memset(rom + (3u << 20), 0xA5, 1u << 20);      /* the PowerPC code is outside the sum */
        CHECK(mc_rom_validate(rom, 4u << 20, &info) && info.computed_checksum == s);
        rom[100] ^= 1;
        CHECK(!mc_rom_validate(rom, 4u << 20, &info));
        be32(rom, 0x9feb69b3);
        const mc_known_rom *pdm = mc_romdb_identify(rom, 4u << 20);
        CHECK(pdm && !strcmp(pdm->file, "power-macintosh-6100.rom") && !strcmp(pdm->cpu, "601"));
        free(rom);
    }
    /* Firmware: size and CRC32 (the check value of CRC-32 is CBF43926). */
    CHECK_EQ(mc_crc32((const uint8_t *)"123456789", 9), 0xcbf43926u);
    /* 17 controller firmware and declaration ROMs (.bin), 19 New World
     * "Mac OS ROM" files (.rom). */
    CHECK(mc_firmware_count() == 36);
    for (size_t i = 0; i < mc_firmware_count(); ++i) {
        const mc_firmware *f = mc_firmware_entry(i);
        char key[64];
        snprintf(key, sizeof key, "%.*s", (int)(strlen(f->file) - 4), f->file);
        CHECK(mc_firmware_find(key) == f);
        CHECK(i < 17 ? !strcmp(f->file + strlen(f->file) - 4, ".bin")
                     : !strncmp(f->file, "mac-os-rom-", 11) && !strcmp(f->file + strlen(f->file) - 4, ".rom") && strstr(f->part, "New World"));
    }
    CHECK(mc_firmware_find("egret") == NULL);
}

/* Sets the last four bytes of data so that its CRC-32 is target: the
 * register after them depends only on the four table indices they select,
 * which the target's bytes give from the top down. */
static void force_crc(uint8_t *data, size_t size, uint32_t target) {
    uint32_t table[256], index[4];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        table[i] = c;
    }
    uint32_t t = ~target;
    for (int k = 3; k >= 0; --k) {
        for (uint32_t j = 0; j < 256; ++j)
            if (table[j] >> 24 == t >> 24) { index[k] = j; t = (t ^ table[j]) << 8; break; }
    }
    uint32_t r = ~mc_crc32(data, size - 4);
    for (int k = 0; k < 4; ++k) {
        data[size - 4 + k] = (uint8_t)((r ^ index[k]) & 0xff);
        r = table[index[k]] ^ (r >> 8);
    }
}

/* The index of the roms folders: Macintosh ROMs by checksum and size,
 * firmware by size and CRC32, whatever the names; duplicates and unknowns. */
static void test_rom_index(void) {
    char a[1100], b[1100];
    snprintf(a, sizeof a, "%s/index-a/", dir);
    snprintf(b, sizeof b, "%s/index-b/", dir);
    _mkdir(a); _mkdir(b);
    const char *names[] = {"index-a/Q700 copy.bin", "index-a/quadra-700.rom", "index-a/zz.rom", "index-a/notes.txt",
                           "index-a/firmware.dat", "index-a/near.bin", "index-b/aaa.rom", "index-b/egret-341s0851.bin",
                           "index-b/iici.rom", "index-b/first.bin", "index-b/second.bin", "index-b/my egret.bin",
                           "index-b/Mac OS ROM"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) remove(path_of(names[i]));
    uint8_t *q700 = fake_rom(0x420dbff3, 1024 * 1024), *iici = fake_rom(0x368cadfe, 512 * 1024);
    write_bytes("index-a/Q700 copy.bin", q700, 1024 * 1024);
    write_bytes("index-a/quadra-700.rom", q700, 1024 * 1024);   /* the standard name wins over the earlier file */
    write_bytes("index-b/aaa.rom", q700, 1024 * 1024);          /* the second folder's copy is a duplicate too */
    write_bytes("index-a/zz.rom", iici, 512 * 1024 - 2);         /* a wrong size is no ROM */
    write_text("index-a/notes.txt", "not a ROM");
    write_bytes("index-b/first.bin", iici, 512 * 1024);           /* no standard name: the first found wins */
    write_bytes("index-b/second.bin", iici, 512 * 1024);
    /* Firmware by size and CRC32: a made-up 4352-byte image with the Egret
     * 341S0851's CRC32 is that firmware under any name; one of another CRC
     * is unknown even under the standard name. */
    uint8_t firmware[4352], egret[4352];
    for (size_t i = 0; i < sizeof firmware; ++i) firmware[i] = egret[i] = (uint8_t)(i * 7);
    force_crc(egret, sizeof egret, 0xea9ea6e4u);
    CHECK_EQ(mc_crc32(egret, sizeof egret), 0xea9ea6e4u);
    CHECK(mc_firmware_identify(egret, sizeof egret) == mc_firmware_find("egret-341s0851"));
    write_bytes("index-b/my egret.bin", egret, sizeof egret);
    write_bytes("index-b/egret-341s0851.bin", firmware, sizeof firmware);
    write_bytes("index-a/near.bin", firmware, 4096);
    const char *folders[] = {a, b};
    mc_rom_index ix;
    /* A New World "Mac OS ROM" file (1.1's size and CRC32) under another
     * name: recognised as that file, no machine's ROM. */
    static uint8_t newworld[1900274];
    memcpy(newworld, "<CHRP-BOOT>", 11);
    force_crc(newworld, sizeof newworld, 0xb5df73ddu);
    write_bytes("index-b/Mac OS ROM", newworld, sizeof newworld);
    CHECK(mc_rom_index_scan(&ix, folders, 2));
    CHECK_EQ(ix.count, 11);
    const mc_rom_file *fw = mc_rom_index_firmware(&ix, mc_firmware_find("egret-341s0851"));
    CHECK(fw && !strcmp(fw->name, "my egret.bin") && !fw->standard && fw->crc32 == 0xea9ea6e4u);
    CHECK_EQ(ix.folder_count, 2);
    const mc_rom_file *used = mc_rom_index_rom(&ix, mc_romdb_find(0x420dbff3, 1024 * 1024));
    CHECK(used && !strcmp(used->name, "quadra-700.rom") && used->standard && used->duplicate < 0);
    const mc_rom_file *ci = mc_rom_index_rom(&ix, mc_romdb_find(0x368cadfe, 512 * 1024));
    CHECK(ci && !strcmp(ci->name, "first.bin") && !ci->standard);
    char what[512];
    for (size_t i = 0; i < ix.count; ++i) {
        const mc_rom_file *f = &ix.files[i];
        mc_rom_file_describe(&ix, f, what, sizeof what);
        if (!strcmp(f->name, "Q700 copy.bin") || !strcmp(f->name, "aaa.rom"))
            CHECK(f->rom && f->duplicate >= 0 && &ix.files[f->duplicate] == used && strstr(what, "duplicate of quadra-700.rom"));
        else if (!strcmp(f->name, "second.bin"))
            CHECK(f->duplicate >= 0 && &ix.files[f->duplicate] == ci);
        else if (!strcmp(f->name, "my egret.bin"))
            CHECK(strstr(what, "Egret 341S0851, CRC32 ea9ea6e4") && strstr(what, "standard name egret-341s0851.bin"));
        else if (!strcmp(f->name, "first.bin"))
            CHECK(strstr(what, "Macintosh ROM 368CADFE") && strstr(what, "standard name iici.rom"));
        else if (!strcmp(f->name, "Mac OS ROM"))
            CHECK(!f->rom && f->firmware == mc_firmware_find("mac-os-rom-1.1") && strstr(what, "Mac OS ROM file 1.1 (New World)") &&
                  strstr(what, "standard name mac-os-rom-1.1.rom"));
        else if (!strcmp(f->name, "zz.rom") || !strcmp(f->name, "notes.txt") || !strcmp(f->name, "near.bin") ||
                 !strcmp(f->name, "egret-341s0851.bin"))
            CHECK(!f->rom && !f->firmware && !strcmp(what, "unknown"));
        else
            CHECK(f == used);
    }
    /* The CRC is computed for firmware sizes only. */
    for (size_t i = 0; i < ix.count; ++i)
        if (!strcmp(ix.files[i].name, "egret-341s0851.bin")) CHECK_EQ(ix.files[i].crc32, mc_crc32(firmware, sizeof firmware));
    mc_rom_index_free(&ix);
    CHECK(mc_firmware_identify(firmware, sizeof firmware) == NULL);
    /* A missing folder is skipped. */
    char missing[1100];
    snprintf(missing, sizeof missing, "%s/no-such-folder/", dir);
    const char *none[] = {missing};
    CHECK(mc_rom_index_scan(&ix, none, 1) && ix.count == 0);
    mc_rom_index_free(&ix);
    free(q700);
    free(iici);
}

/* --- models ---------------------------------------------------------- */

static void test_models(void) {
    CHECK_EQ(mc_model_count(), 125);
    for (size_t i = 0; i < mc_model_count(); ++i) {
        const mc_model *p = mc_model_entry(i);
        CHECK(mc_model_rom(p) != NULL);
        CHECK(p->ram_mb >= 1 && p->ram_mb <= p->ram_max_mb && (p->ram_kb ? p->ram_kb < 1024 : p->ram_mb >= 4));
        CHECK(p->cpu_hz >= MC_CONFIG_MIN_HZ && p->cpu_hz <= MC_CONFIG_MAX_HZ);
        CHECK(p->look <= MC_LOOK_LCD && p->monitor <= MC_MONITOR_LCD_GRAY);
        CHECK(p->monitors & (1u << p->monitor));
        {
            char why[400];
            CHECK(mc_config_monitor_check(p, p->width, p->height, p->depth, p->monitor, why, sizeof why));
        }
        CHECK(p->monitors & (1u << p->monitor));
        {
            char why[400];
            CHECK(mc_config_monitor_check(p, p->width, p->height, p->depth, p->monitor, why, sizeof why));
        }
        int resolution = 0, depth = 0;
        for (unsigned r = 0; r < MC_CONFIG_RESOLUTIONS; ++r)
            resolution |= mc_config_resolutions[r][0] == p->width && mc_config_resolutions[r][1] == p->height;
        for (unsigned d = 0; d < MC_CONFIG_DEPTHS; ++d) depth |= mc_config_depths[d] == p->depth;
        CHECK(resolution && depth);
        CHECK((uint64_t)p->width * p->height * p->depth / 8 <= MC_CONFIG_VRAM);
        CHECK(mc_model_find(p->key) == p);
        CHECK_EQ(mc_model_index(p), i);
    }
    CHECK(mc_model_find("fast") == NULL && mc_model_find("auto") == NULL);
    /* The Built-in machine: the first model, the 128K's board with Livadix's ROM. */
    const mc_model *q700 = mc_model_find("quadra-700"), *builtin = mc_model_entry(0);
    CHECK(mc_model_builtin(builtin) && builtin->rom == MC_BUILTIN_ROM && builtin->ram_kb == 128 && mc_machine_emulated(builtin));
    const mc_model *pm6100 = mc_model_find("power-macintosh-6100"), *pm8100 = mc_model_find("power-macintosh-8100");
    CHECK(mc_model_ppc(pm8100) && pm8100->fpu && !mc_model_ppc(q700) && pm8100->gestalt == 65 && pm6100->gestalt == 75);
    CHECK_EQ(mc_model_find("power-macintosh-7100")->gestalt, 112);
    CHECK_EQ(mc_model_find("power-macintosh-g3")->gestalt, 510);
    CHECK(mc_model_find("pippin")->console && mc_model_find("pippin-dev")->console && !pm6100->console);
    CHECK_EQ(mc_model_find("quadra-700")->gestalt, 22);
    CHECK_EQ(mc_model_find("quadra-630")->gestalt, 98);
    CHECK_EQ(mc_model_find("iici")->gestalt, 11);
    CHECK_EQ(mc_model_find("powerbook-190")->gestalt, 85);
    CHECK(mc_model_find("macintosh-128k") == NULL);
}

/* --- configuration ---------------------------------------------------------------- */

static int load(const char *text, mc_config *c, char *error, size_t capacity) {
    write_text("test.yaml", text);
    return mc_config_load(path_of("test.yaml"), c, error, capacity);
}
/* The ROM of a loaded configuration, from a fresh index of its roms folder. */
static int find_rom(mc_config *c, char *error, size_t capacity) {
    char folders[2][MC_CONFIG_PATH];
    const size_t n = mc_config_rom_folders(c, NULL, folders);
    const char *list[2] = {folders[0], n > 1 ? folders[1] : NULL};
    static mc_rom_index ix;
    mc_rom_index_free(&ix);
    CHECK(mc_rom_index_scan(&ix, list, n));
    return mc_config_find_rom(c, &ix, error, capacity);
}
static void refused(const char *text, const char *message) {
    mc_config c;
    char error[512] = "";
    CHECK(!load(text, &c, error, sizeof error));
    if (!strstr(error, message)) { fprintf(stderr, "expected \"%s\" in \"%s\"\n", message, error); mc_test_failures++; }
}

static void test_config(void) {
    mc_config c;
    char error[512] = "";
    /* The embedded default configuration template. */
    write_bytes("default.yaml", mc_default_yaml, sizeof mc_default_yaml);
    CHECK(mc_config_load(path_of("default.yaml"), &c, error, sizeof error));
    CHECK_STR(c.profile, "Built-in");                               /* the first run's machine */
    CHECK_STR(mc_model_entry(c.model)->key, MC_MODEL_BUILTIN);
    CHECK_EQ(c.ram_mb, 1); CHECK_EQ(c.cpu_hz, 7833600);
    CHECK(c.width == mc_model_entry(c.model)->width && c.height == mc_model_entry(c.model)->height && c.refresh == 60);
    CHECK(c.filter == MC_CONFIG_FILTER_SHARP && c.scale == MC_CONFIG_SCALE_FIT && c.vsync && !c.fullscreen);
    CHECK(c.jit && c.startup_sound && c.sound && !c.fpu_full && c.controller == MC_CONTROLLER_AUTO);
    CHECK_EQ(c.video_card, MC_VIDEO_CARD_AUTO);
    CHECK_EQ(c.disk_count, 0);
    CHECK(c.floppy_count == 1 && !c.floppy_off[0] && c.floppy_off[1] && !c.floppies[0].image[0]);
    CHECK(c.keyboard_layout == MC_KEYBOARD_ANSI && c.command_key == MC_COMMAND_LEFT_ALT);
    CHECK(c.mouse[0] == MC_MOUSE_NORMAL && c.mouse[1] == MC_MOUSE_NORMAL);
    CHECK(c.ports[0].type == MC_PORT_DEFAULT && c.boot_priority == MC_BOOT_DISK);
    CHECK(c.close_action == MC_CLOSE_SHUTDOWN && c.close_timeout == 30);

    /* A model supplies what the file leaves out; the file wins. */
    CHECK(load("video:\n  depth: 16\nmodel: se-30\nemulation:\n  ram_mb: 8\n", &c, error, sizeof error));
    CHECK(c.width == 512 && c.height == 342 && c.depth == 16 && c.ram_mb == 8 && c.cpu_hz == 15667200);
    CHECK(c.look == MC_LOOK_CRT && c.monitor == MC_MONITOR_MONO);
    CHECK(load("model: quadra-700\nemulation:\n  ram_mb: 68\n", &c, error, sizeof error));
    /* No model, or auto (before 0.35): the Built-in machine, with its 128 KB and screen whatever the file says. */
    CHECK(load("model: auto\n", &c, error, sizeof error) && mc_model_builtin(mc_model_entry(c.model)));
    CHECK(load("emulation: {ram_mb: 20}\nvideo: {resolution: 640x480}\n", &c, error, sizeof error) &&
          mc_model_builtin(mc_model_entry(c.model)) && c.ram_mb == 1 && c.width == 512);
    CHECK(load("input:\n  keyboard: {layout: iso, command: right-alt}\n  mouse: {right: control-click}\n"
               "storage:\n  floppies: [off, {}]\n  boot_priority: cdrom\n"
               "ports:\n  a: {serial: " TEST_SERIAL "}\n  b: {output: printer.prn}\n"
               "model: powerbook-170\n"
               "video: {look: lcd, monitor: lcd-gray, filter: nearest, scale: integer, refresh: 30}\n"
               "emulation: {startup_sound: false, jit: false, fpu_full: true, controller: firmware}\nsound: {enabled: false}\n", &c, error, sizeof error));
    CHECK(c.keyboard_layout == MC_KEYBOARD_ISO && c.command_key == MC_COMMAND_RIGHT_ALT && c.mouse[1] == MC_MOUSE_CONTROL_CLICK);
    CHECK(c.floppy_count == 2 && c.floppy_off[0] && !c.floppy_off[1] && c.boot_priority == MC_BOOT_CDROM);
    CHECK(c.ports[0].type == MC_PORT_SERIAL && !strcmp(c.ports[0].path, TEST_SERIAL) && c.ports[1].type == MC_PORT_OUTPUT);
    CHECK(c.look == MC_LOOK_LCD && c.monitor == MC_MONITOR_LCD_GRAY && c.filter == MC_CONFIG_FILTER_NEAREST);
    CHECK(c.scale == MC_CONFIG_SCALE_INTEGER && c.refresh == 30 && !c.startup_sound && !c.jit && !c.sound && c.fpu_full);
    CHECK_EQ(c.controller, MC_CONTROLLER_FIRMWARE);
    CHECK_STR(mc_config_controller_name(MC_CONTROLLER_PROTOCOL), "protocol");
    /* What closing the window does. */
    CHECK(load("emulation: {close_action: quit, close_timeout: 120}\n", &c, error, sizeof error));
    CHECK(c.close_action == MC_CLOSE_QUIT && c.close_timeout == 120);
    CHECK_STR(mc_config_close_action_name(MC_CLOSE_SHUTDOWN), "shutdown");
    refused("emulation:\n  close_action: sleep\n", "Unknown setting value");
    refused("emulation:\n  close_timeout: 0\n", "1 to 3600");

    refused("video: {monitor: green}\n", "Unknown setting value");
    refused("video: {monitor: amber}\n", "Unknown setting value");
    /* video.monitor is what the model's real display offered. */
    refused("model: se-30\nvideo: {monitor: color}\n", "built-in monochrome screen, so video.monitor is mono");
    refused("model: classic-ii\nvideo: {monitor: lcd-gray}\n", "built-in monochrome screen");
    refused("model: powerbook-170\nvideo: {monitor: mono}\n", "built-in grayscale LCD screen, so video.monitor is lcd-gray");
    refused("model: powerbook-170\nvideo: {monitor: color}\n", "video.monitor is lcd-gray");
    refused("model: powerbook-180c\nvideo: {monitor: lcd-gray}\n", "built-in color screen, so video.monitor is color");
    refused("model: color-classic\nvideo: {monitor: mono}\n", "built-in color screen");
    refused("model: quadra-700\nvideo: {monitor: lcd-gray}\n", "no built-in screen");
    refused("model: quadra-700\nvideo: {monitor: mono, resolution: 832x624}\n", "640x480, 640x870 or 1152x870");
    refused("model: iici\nvideo: {monitor: mono, depth: 16}\n", "at 16 bits");
    CHECK(load("model: quadra-700\nvideo: {monitor: mono}\n", &c, error, sizeof error) && c.monitor == MC_MONITOR_MONO);
    CHECK(load("model: iici\nvideo: {monitor: mono, resolution: 1152x870, depth: 8}\n", &c, error, sizeof error));
    CHECK(load("model: se-30\nvideo: {monitor: mono}\n", &c, error, sizeof error));
    CHECK(load("model: powerbook-140\nvideo: {monitor: lcd-gray}\n", &c, error, sizeof error));
    refused("model: fast\n", "Unknown model");
    refused("model: quadra-700\nemulation:\n  ram_mb: 128\n", "exceeds the Macintosh Quadra 700's maximum");
    refused("emulation:\n  ram_mb: 2000\n", "1..1024");
    /* 2.5 MB: a size that is not a whole number of MiB. */
    CHECK(load("model: mac-plus\nemulation:\n  ram_mb: 2.5\n", &c, error, sizeof error));
    CHECK(c.ram_mb == 2 && mc_config_ram_kb(&c) == 2560);
    refused("model: mac-plus\nemulation:\n  ram_mb: 2.3\n", "three decimals");
    refused("model: mac-plus\nemulation:\n  ram_mb: 4.5\n", "exceeds the Macintosh Plus's maximum of 4 MiB");
    /* The PowerBook 100 started by its charger. */
    CHECK(load("model: powerbook-100\nemulation: {charger_start: true}\n", &c, error, sizeof error) && c.charger_start);
    refused("emulation:\n  charger_start: 1\n", "true or false");
    /* The Macintosh II's PMMU socket. */
    CHECK(load("model: mac-ii-v2\nemulation: {pmmu: true}\n", &c, error, sizeof error) && c.pmmu);
    refused("emulation:\n  pmmu: 2\n", "true or false");
    refused("model: lisa\n", "Unknown model");
    refused("emulation:\n  turbo: true\n", "Unknown emulation key");
    refused("colour: red\n", "Unknown configuration key");
    refused("sound:\n  enabled: true\n  enabled: false\n", "Duplicate");
    refused("emulation:\n  startup_sound: 1\n", "true or false");
    refused("emulation:\n  cpu_mhz: 0\n", "1 to 10000");
    refused("emulation:\n  cpu_mhz: 10000.5\n", "1 to 10000");
    refused("emulation:\n  cpu_mhz: 25.1234567\n", "six decimals");
    refused("emulation:\n  cpu_mhz: 25MHz\n", "number of MHz");
    refused("emulation:\n  cpu_mips: 19\n", "Unknown emulation key");
    CHECK(load("emulation:\n  cpu_mhz: 33.33\n", &c, error, sizeof error) && c.cpu_hz == 33330000);
    CHECK(load("emulation:\n  cpu_mhz: 15.6672\n", &c, error, sizeof error) && c.cpu_hz == 15667200);
    CHECK(load("emulation:\n  cpu_mhz: 10000\n", &c, error, sizeof error) && c.cpu_hz == 10000000000ull);
    {
        char t[24];
        CHECK_STR(mc_config_mhz_text(33330000, t, sizeof t), "33.33");
        CHECK_STR(mc_config_mhz_text(25000000, t, sizeof t), "25");
        CHECK_STR(mc_config_mhz_text(15667200, t, sizeof t), "15.6672");
        CHECK_STR(mc_config_mhz_text(1000001, t, sizeof t), "1.000001");
    }
    refused("video:\n  resolution: 1000x700\n", "video.resolution must be one of");
    refused("video:\n  depth: 24\n", "1, 2, 4, 8, 16 or 32");
    refused("video:\n  resolution: 1920x1080\n  depth: 16\n", "goes up to 8 bits");
    refused("video:\n  refresh: 50\n", "60, 30, 20, 15, 12 or 10");
    refused("video:\n  card: nubus\n", "Unknown setting value");
    CHECK(load("video:\n  card: livadix\n", &c, error, sizeof error) && c.video_card == MC_VIDEO_CARD_LIVADIX);
    CHECK(load("video:\n  card: builtin\n", &c, error, sizeof error) && c.video_card == MC_VIDEO_CARD_BUILTIN);
    CHECK_STR(mc_config_video_card_name(MC_VIDEO_CARD_AUTO), "auto");
    refused("input:\n  mouse: {left: disabled, right: control-click}\n", "At least one mouse button");
    refused("storage:\n  floppies: [{}, {}, {}]\n", "At most two floppy drives");
    refused("storage:\n  disks:\n    - image: a.img\n    - image: a.img\n      readonly: true\n", "configured twice");
    refused("storage:\n  disks:\n    - readonly: true\n", "Disk image is required");
    refused("storage:\n  disks:\n    - {image: a.img, scsi_id: 3}\n", "scsi_id must be");
    refused("storage:\n  disks:\n    - {image: a.img, scsi_id: 7}\n", "scsi_id must be");
    refused("storage:\n  disks:\n    - {image: a.img, scsi_id: 2}\n    - {image: b.img, scsi_id: 2}\n", "same scsi_id");
    refused("storage:\n  floppies:\n    - {image: a.img, scsi_id: 2}\n", "Unknown floppy key");
    CHECK(load("storage:\n  disks:\n    - {image: a.img, scsi_id: 5}\n    - {image: b.img}\n", &c, error, sizeof error));
    CHECK(c.disks[0].scsi_id_set && c.disks[0].scsi_id == 5 && !c.disks[1].scsi_id_set);
    /* bus: ata where the machine has it (NOTES.md, "SCSI IDs"). */
    CHECK(load("storage:\n  disks:\n    - {image: a.img, bus: ata}\n    - {image: b.img, bus: scsi}\n", &c, error, sizeof error));
    CHECK(c.disks[0].bus == MC_BUS_ATA && c.disks[1].bus == MC_BUS_SCSI);
    refused("storage:\n  disks:\n    - {image: a.img, bus: ata, scsi_id: 2}\n", "takes no scsi_id");
    refused("storage:\n  disks:\n    - {image: a.img, bus: ide}\n", "Unknown setting value");
    /* storage.cdrom: a path, or {image, bus} (ata: the PowerBook 190's expansion bay), empty without an image. */
    CHECK(load("storage:\n  cdrom: {image: cd.iso, bus: ata}\n", &c, error, sizeof error));
    CHECK(c.cdrom_bus == MC_BUS_ATA && strstr(c.cdrom, "cd.iso"));
    CHECK(load("storage:\n  cdrom: {bus: ata}\n", &c, error, sizeof error));
    CHECK(c.cdrom_bus == MC_BUS_ATA && !c.cdrom[0]);
    CHECK(load("storage:\n  cdrom: cd.iso\n", &c, error, sizeof error));
    CHECK(c.cdrom_bus == MC_BUS_SCSI && strstr(c.cdrom, "cd.iso"));
    refused("storage:\n  cdrom: {image: cd.iso, scsi_id: 3}\n", "Unknown cdrom key");
    refused("ports:\n  a: {serial: LPT1}\n", TEST_SERIAL_REFUSED);
    refused("ports:\n  c: {disabled: true}\n", "named a or b");
    /* network: NAT by default; none; forwards with and without an address. */
    CHECK(load("model: auto\n", &c, error, sizeof error) && c.network == MC_NETWORK_NAT && c.forward_count == 0);
    CHECK(load("network:\n  mode: none\n", &c, error, sizeof error) && c.network == MC_NETWORK_NONE);
    CHECK(load("network:\n  forward: [\"tcp:8080:80\", \"udp:0.0.0.0:5353:53\"]\n", &c, error, sizeof error));
    CHECK(c.forward_count == 2 && !c.forwards[0].udp && c.forwards[0].address == 0x7F000001u && c.forwards[0].host_port == 8080 &&
          c.forwards[0].guest_port == 80 && c.forwards[1].udp && c.forwards[1].address == 0 && c.forwards[1].guest_port == 53);
    char fw[48];
    mc_config_forward_text(&c.forwards[1], fw, sizeof fw);
    CHECK_STR(fw, "udp:0.0.0.0:5353:53");
    mc_config_forward_text(&c.forwards[0], fw, sizeof fw);
    CHECK_STR(fw, "tcp:8080:80");
    CHECK_STR(mc_config_network_name(MC_NETWORK_NONE), "none");
    refused("network:\n  mode: tap\n", "Unknown setting value");
    refused("network:\n  forward: [\"sctp:1:2\"]\n", "tcp or udp");
    refused("network:\n  forward: [\"tcp:0:80\"]\n", "1 to 65535");
    refused("network:\n  forward: [\"tcp:70000:80\"]\n", "1 to 65535");
    refused("network:\n  forward: [\"tcp:1.2.3:80:80\"]\n", "IPv4 address");
    refused("network:\n  forward: [\"tcp:80\"]\n", "HOSTPORT:GUESTPORT");
    refused("network:\n  forward: [\"tcp:8080:80\", \"tcp:8080:81\"]\n", "share a host port");
    refused("network:\n  forward: tcp:8080:80\n", "must be a sequence");
    refused("network:\n  bridge: true\n", "Unknown network key");
    refused("model: auto\n---\nmodel: auto\n", "Only one YAML document");
    refused("model: [auto\n", "YAML line");

    /* The ROM: standard name, then any file with the checksum, else an error. */
    _mkdir(path_of("roms"));
    uint8_t *rom = fake_rom(0x420dbff3, 1024 * 1024), *other = fake_rom(0x12345678, 1024 * 1024);   /* no machine's ROM */
    remove(path_of("roms/quadra-700.rom"));
    remove(path_of("roms/renamed.bin"));
    remove(path_of("roms/quadra-630.rom"));        /* left by earlier versions of this test */
    CHECK(load("model: quadra-700\n", &c, error, sizeof error));
    CHECK(!find_rom(&c, error, sizeof error) && strstr(error, "quadra-700.rom") && strstr(error, "any name"));
    write_bytes("roms/quadra-700.rom", other, 1024 * 1024);   /* the name is only a suggestion */
    CHECK(!find_rom(&c, error, sizeof error));
    remove(path_of("roms/quadra-700.rom"));
    write_bytes("roms/renamed.bin", rom, 1024 * 1024);
    CHECK(find_rom(&c, error, sizeof error) && c.rom_scanned && strstr(c.rom, "renamed.bin"));
    write_bytes("roms/quadra-700.rom", rom, 1024 * 1024);
    CHECK(find_rom(&c, error, sizeof error) && !c.rom_scanned && strstr(c.rom, "roms/quadra-700.rom"));
    write_bytes("roms/powerbook-150.rom", other, 1024 * 1024);
    CHECK(load("model: quadra-700\nemulation:\n  rom: roms/powerbook-150.rom\n", &c, error, sizeof error));
    CHECK(!find_rom(&c, error, sizeof error) && strstr(error, "is not a known Macintosh ROM; model: quadra-700 needs ROM 420DBFF3"));
    /* The Built-in machine uses the embedded ROM even beside an old export. */
    CHECK(load("model: builtin\n", &c, error, sizeof error) && find_rom(&c, error, sizeof error) && !c.rom[0]);
    uint8_t *own = fake_rom(MC_BUILTIN_ROM, 128 * 1024);
    write_bytes("roms/livadix.rom", own, 128 * 1024);
    free(own);
    CHECK(find_rom(&c, error, sizeof error) && !c.rom[0]);
    remove(path_of("roms/livadix.rom"));
    remove(path_of("roms/quadra-700.rom"));
    remove(path_of("roms/renamed.bin"));
    /* emulation.controller: firmware needs the model's controller dump in
     * the roms folders, under any name; auto and protocol start without. */
    uint8_t *lc475 = fake_rom(0xff7439ee, 1024 * 1024);
    write_bytes("roms/a.rom", lc475, 1024 * 1024);
    free(lc475);
    remove(path_of("roms/fw.bin"));
    CHECK_STR(mc_model_controller_firmware(mc_model_find("lc-475")), "cuda-341s0788");
    CHECK(mc_model_controller_firmware(mc_model_find("quadra-700")) == NULL);
    CHECK(load("model: lc-475\nemulation: {controller: firmware}\n", &c, error, sizeof error));
    CHECK(!find_rom(&c, error, sizeof error) && strstr(error, "emulation.controller: firmware needs the Cuda 341S0788 dump"));
    CHECK(load("model: lc-475\nemulation: {controller: auto}\n", &c, error, sizeof error) && find_rom(&c, error, sizeof error));
    CHECK(load("model: lc-475\nemulation: {controller: protocol}\n", &c, error, sizeof error) && find_rom(&c, error, sizeof error));
    static uint8_t cuda[4352];
    for (size_t i = 0; i < sizeof cuda; ++i) cuda[i] = (uint8_t)(i * 11);
    force_crc(cuda, sizeof cuda, 0xdf6e1b43u);
    write_bytes("roms/fw.bin", cuda, sizeof cuda);
    CHECK(load("model: lc-475\nemulation: {controller: firmware}\n", &c, error, sizeof error) && find_rom(&c, error, sizeof error));
    CHECK(load("model: quadra-700\nemulation: {controller: firmware}\n", &c, error, sizeof error));
    remove(path_of("roms/a.rom"));
    remove(path_of("roms/fw.bin"));
    free(rom);
    free(other);
}

/* --- profiles ------------------------------------------------------------------------- */

static void refused_profile(const char *text, const char *profile, const char *message) {
    mc_config c;
    char error[512] = "";
    write_text("profile.yaml", text);
    CHECK(!mc_config_load_profile(path_of("profile.yaml"), profile, &c, error, sizeof error));
    if (!strstr(error, message)) { fprintf(stderr, "expected \"%s\" in \"%s\"\n", message, error); mc_test_failures++; }
}

static void test_profiles(void) {
    mc_config c;
    char error[512] = "";
    static const char text[] =
        "# shared\n"                                         /* 1 */
        "default: Second\n"                                  /* 2 */
        "emulation:\n"                                       /* 3 */
        "  ram_mb: 8\n"                                      /* 4 */
        "  jit: false\n"                                     /* 5 */
        "storage:\n"                                         /* 6 */
        "  disks:\n"                                         /* 7 */
        "    - image: shared.img\n"                          /* 8 */
        "profiles:\n"                                        /* 9 */
        "  \"Quadra 700 \xe2\x80\x93 System 7.1\":\n"        /* 10 */
        "    model: quadra-700\n"                            /* 11 */
        "    emulation: { ram_mb: 20 }\n"                    /* 12 */
        "    storage:\n"                                     /* 13 */
        "      disks:\n"                                     /* 14 */
        "        - {image: own.img, scsi_id: 2}\n"           /* 15 */
        "  Second:\n"                                        /* 16 */
        "    video:\n"                                       /* 17 */
        "      resolution: 640x480\n";                       /* 18 */
    write_text("profile.yaml", text);
    /* The default: default: names it; shared keys, then its own. */
    CHECK(mc_config_load(path_of("profile.yaml"), &c, error, sizeof error));
    CHECK_STR(c.profile, "Second");
    CHECK(mc_model_builtin(mc_model_entry(c.model)) && !c.jit && c.width == 512 && c.height == 342);   /* no model: the Built-in machine */
    CHECK(c.disk_count == 1 && strstr(c.disks[0].image, "shared.img"));
    CHECK(!c.nvram[0]);       /* no automatic PRAM for the Built-in desktop */
    /* Another by its name: a section's keys override one by one, a list as a whole. */
    CHECK(mc_config_load_profile(path_of("profile.yaml"), "Quadra 700 \xe2\x80\x93 System 7.1", &c, error, sizeof error));
    CHECK(!strcmp(mc_model_entry(c.model)->key, "quadra-700"));
    CHECK(c.ram_mb == 20 && !c.jit && c.width == 1152 && c.height == 870);
    CHECK(c.disk_count == 1 && strstr(c.disks[0].image, "own.img") && c.disks[0].scsi_id == 2);
    CHECK(strstr(c.nvram, "nvram/profile-quadra-700-system-7-1.nvram") != NULL);
    CHECK(mc_config_load_profile(path_of("profile.yaml"), "second", &c, error, sizeof error) && !strcmp(c.profile, "Second"));
    /* The list. */
    mc_profile_list list;
    CHECK(mc_config_profiles(path_of("profile.yaml"), &list, error, sizeof error));
    CHECK(list.count == 2 && list.default_index == 1);
    CHECK_STR(list.names[0], "Quadra 700 \xe2\x80\x93 System 7.1");
    CHECK_STR(list.models[0], "quadra-700");
    CHECK_STR(list.models[1], MC_MODEL_BUILTIN);
    /* Without default: the first; without profiles: one configuration. */
    write_text("profile.yaml", "profiles:\n  A: {model: iici}\n  B:\n");
    CHECK(mc_config_load(path_of("profile.yaml"), &c, error, sizeof error) && !strcmp(c.profile, "A"));
    CHECK(!strcmp(mc_model_entry(c.model)->key, "iici"));
    CHECK(mc_config_load_profile(path_of("profile.yaml"), "B", &c, error, sizeof error) && mc_model_builtin(mc_model_entry(c.model)));
    write_text("profile.yaml", "model: iici\n");
    CHECK(mc_config_load(path_of("profile.yaml"), &c, error, sizeof error) && !c.profile[0] && strstr(c.nvram,"nvram/profile.nvram"));
    CHECK(mc_config_profiles(path_of("profile.yaml"), &list, error, sizeof error) && list.count == 0);
    /* Errors name the line and the profile; every profile is checked. */
    refused_profile("profiles:\n  A:\n    model: iici\n  B:\n    emulation:\n      turbo: true\n", NULL,
                    "line 6 (profile \"B\"): Unknown emulation key");
    refused_profile("profiles:\n  A:\n    model: quadra-700\n    emulation:\n      ram_mb: 128\n", NULL,
                    "line 5 (profile \"A\"): emulation.ram_mb: 128 exceeds");
    refused_profile("profiles:\n  A:\n    colour: red\n", NULL, "line 3 (profile \"A\"): Unknown profile key");
    refused_profile("default: C\nprofiles:\n  A: {}\n", NULL, "line 1: default: there is no profile \"C\"");
    refused_profile("default: A\nmodel: iici\n", NULL, "default: names a profile, but the file has no profiles:");
    refused_profile("profiles:\n  A: {}\n  A: {}\n", NULL, "Duplicate");
    refused_profile("profiles:\n  A: 3\n", NULL, "line 2: Expected a mapping");
    refused_profile("profiles: {}\n", NULL, "holds no profile");
    refused_profile("profiles:\n  A: {}\n", "Z", "There is no profile \"Z\"");
    refused_profile("model: iici\n", "A", "the file has no profiles:");
    refused_profile("emulation:\n  ram_mb: 8\n\nvideo:\n  depth: 32\n  resolution: 1920x1080\n", NULL, "line 5: video.resolution 1920x1080");
    refused_profile("profiles:\n  A:\n    video:\n      refresh: 50\n", NULL, "line 4 (profile \"A\"): video.refresh");
    refused_profile("model: lisa\n", NULL, "line 1: Unknown model");
    refused_profile("profiles:\n  A:\n    storage:\n      disks:\n        - image: x.img\n        - image: x.img\n", NULL,
                    "(profile \"A\"): The same image is configured twice");
}

/* --- the YAML editor --------------------------------------------------------------- */

/* Every comment line of a, in order, appears in b. */
static int comments_kept(const char *a, const char *b, const char *except) {
    const char *at = b;
    for (const char *line = a; *line;) {
        const char *end = strchr(line, '\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        size_t i = 0;
        while (i < n && line[i] == ' ') i++;
        if (i < n && line[i] == '#' && !(except && !strncmp(line, except, strlen(except)))) {
            char want[512];
            snprintf(want, sizeof want, "%.*s", (int)n, line);
            const char *found = strstr(at, want);
            if (!found) { fprintf(stderr, "comment lost: %s\n", want); return 0; }
            at = found + n;
        }
        line += n + (end != NULL);
    }
    return 1;
}
static int parse_doc(const mc_ydoc *doc, const char *profile, mc_config *c) {
    char error[512] = "";
    size_t length = 0;
    char *text = mc_ydoc_text(doc, &length);
    const int ok = text && mc_config_parse(text, length, path_of("edited.yaml"), profile, c, error, sizeof error);
    if (!ok) fprintf(stderr, "edited text refused: %s\n%s\n", error, text ? text : "");
    free(text);
    return ok;
}

static void test_yaml_edit(void) {
    /* The embedded file has no terminator: comments_kept reads it as a string. */
    static char template_buffer[sizeof mc_default_yaml + 1];
    memcpy(template_buffer, mc_default_yaml, sizeof mc_default_yaml);
    const char *template_text = template_buffer;
    const size_t template_size = sizeof mc_default_yaml;
    mc_ydoc doc;
    CHECK(mc_ydoc_parse(&doc, template_text, template_size));
    size_t length = 0;
    char *text = mc_ydoc_text(&doc, &length);
    /* Unchanged, the text is the same, byte for byte. */
    CHECK(text && length == template_size && !memcmp(text, template_text, length));
    free(text);
    const char *profile[] = {"profiles", "Mac OS 8.0", "emulation", "ram_mb"};
    const char *resolution[] = {"video", "resolution"};
    const char *added[] = {"profiles", "Quadra 700 \xe2\x80\x93 System 7.1", "model"};
    const char *added_disks[] = {"profiles", "Quadra 700 \xe2\x80\x93 System 7.1", "storage", "disks"};
    const char *def[] = {"default"};
    mc_config c;
    /* A key of a profile: its section is made. */
    CHECK(mc_ydoc_set(&doc, profile, 4, "20"));
    /* A shared key the template has commented out: uncommented in place. */
    CHECK(mc_ydoc_set(&doc, resolution, 2, "640x480"));
    /* A new profile, its disks, and the default. */
    CHECK(mc_ydoc_set(&doc, added, 3, "quadra-700"));
    const char *disks[] = {"- image: 'data/disk/HD240-71.HF7'", "  readonly: true"};
    CHECK(mc_ydoc_set_block(&doc, added_disks, 4, disks, 2));
    char quoted[256];
    CHECK(mc_ydoc_set(&doc, def, 1, mc_ydoc_quote("Quadra 700 \xe2\x80\x93 System 7.1", quoted, sizeof quoted)));
    text = mc_ydoc_text(&doc, &length);
    CHECK(text && comments_kept(template_text, text, "  # resolution:"));
    CHECK(text && strstr(text, "\n  resolution: 640x480\n") && !strstr(text, "# resolution:"));
    CHECK(text && strstr(text, "default: Quadra 700 \xe2\x80\x93 System 7.1\n"));
    free(text);
    CHECK(parse_doc(&doc, NULL, &c));
    CHECK_STR(c.profile, "Quadra 700 \xe2\x80\x93 System 7.1");
    CHECK(c.width == 640 && c.disk_count == 1 && c.disks[0].readonly && strstr(c.disks[0].image, "HD240-71.HF7"));
    CHECK(parse_doc(&doc, "Mac OS 8.0", &c) && c.ram_mb == 20 && c.disk_count == 1 && strstr(c.disks[0].image, "HD240.HF8"));
    char keys[8][128];
    const char *top[] = {"profiles"};
    CHECK_EQ(mc_ydoc_keys(&doc, top, 1, keys, 8), 3);
    CHECK_STR(keys[2], "Quadra 700 \xe2\x80\x93 System 7.1");
    /* Duplicate, rename, remove: the other profiles stay as they were. */
    const char *first[] = {"profiles", "Mac OS 8.0"};
    CHECK(mc_ydoc_duplicate(&doc, first, 2, "Copy"));
    const char *copy[] = {"profiles", "Copy"};
    CHECK(mc_ydoc_rename(&doc, copy, 2, "Mac OS 8.0 \"big\""));
    const char *renamed[] = {"profiles", "Mac OS 8.0 \"big\"", "emulation", "ram_mb"};
    CHECK(mc_ydoc_set(&doc, renamed, 4, "68"));
    CHECK(parse_doc(&doc, "Mac OS 8.0 \"big\"", &c) && c.ram_mb == 68);
    CHECK(parse_doc(&doc, "Mac OS 8.0", &c) && c.ram_mb == 20);
    CHECK(mc_ydoc_remove(&doc, first, 2));
    CHECK_EQ(mc_ydoc_keys(&doc, top, 1, keys, 8), 3);
    CHECK_STR(keys[1], "Mac OS 8.0 \"big\"");
    CHECK(parse_doc(&doc, NULL, &c) && !strcmp(mc_model_entry(c.model)->key, "quadra-700"));
    text = mc_ydoc_text(&doc, &length);
    CHECK(text && comments_kept(template_text, text, "  # resolution:"));
    free(text);
    /* Removing a key whose line the template commented out. */
    CHECK(mc_ydoc_remove(&doc, resolution, 2));
    CHECK(parse_doc(&doc, NULL, &c) && c.width == 1152);
    mc_ydoc_free(&doc);

    /* A user's file: flow mappings an edit reaches into become block style;
     * comments, CR LF line ends and the other profile stay. */
    static const char user[] =
        "# my file\r\n"
        "profiles:\r\n"
        "  # the old one\r\n"
        "  \"Q700\": { model: quadra-700, emulation: { ram_mb: 20 } }   # fast\r\n"
        "  Other:\r\n"
        "    model: iici   # keep me\r\n"
        "    storage: {disks: [{image: a.img}, {image: b.img, readonly: true}]}\r\n";
    CHECK(mc_ydoc_parse(&doc, user, sizeof user - 1));
    const char *jit[] = {"profiles", "Q700", "emulation", "jit"};
    CHECK(mc_ydoc_set(&doc, jit, 4, "false"));
    CHECK(parse_doc(&doc, "Q700", &c) && c.ram_mb == 20 && !c.jit && !strcmp(mc_model_entry(c.model)->key, "quadra-700"));
    CHECK(parse_doc(&doc, "Other", &c) && c.disk_count == 2 && c.disks[1].readonly);
    text = mc_ydoc_text(&doc, &length);
    CHECK(text && strstr(text, "# my file\r\n") && strstr(text, "  # the old one\r\n") && strstr(text, "# fast"));
    CHECK(text && strstr(text, "    model: iici   # keep me\r\n"));
    CHECK(text && strstr(text, "    storage: {disks: [{image: a.img}, {image: b.img, readonly: true}]}\r\n"));
    CHECK(text && !strstr(text, "\n\n") && !strstr(text, "\r\r"));
    free(text);
    /* Saved and loaded again: equal. */
    CHECK(mc_ydoc_save(&doc, path_of("edited.yaml")));
    mc_ydoc again;
    CHECK(mc_ydoc_load(&again, path_of("edited.yaml")));
    char *a = mc_ydoc_text(&doc, NULL), *b = mc_ydoc_text(&again, NULL);
    CHECK(a && b && !strcmp(a, b));
    free(a);
    free(b);
    mc_config d;
    char error[512];
    CHECK(mc_config_load_profile(path_of("edited.yaml"), "Q700", &d, error, sizeof error));
    CHECK(parse_doc(&doc, "Q700", &c) && !memcmp(&c, &d, sizeof c));
    mc_ydoc_free(&again);
    mc_ydoc_free(&doc);
    /* Quoting. */
    char q[128];
    CHECK_STR(mc_ydoc_quote("plain-name", q, sizeof q), "plain-name");
    CHECK_STR(mc_ydoc_quote("off", q, sizeof q), "\"off\"");
    CHECK_STR(mc_ydoc_quote("a: b", q, sizeof q), "\"a: b\"");
    CHECK_STR(mc_ydoc_quote_path("C:\\disks\\a.img", q, sizeof q), "'C:\\disks\\a.img'");
    CHECK_STR(mc_ydoc_quote_path("C:/it's.img", q, sizeof q), "'C:/it''s.img'");
}

/* --- the configuration window's rules (profile.h) ------------------------------- */

static mc_config model_config(const char *key) {
    mc_config c;
    mc_config_defaults(&c);
    snprintf(c.source, sizeof c.source, "%s", path_of("profile.yaml"));
    const mc_model *m = mc_model_find(key);
    c.model = (uint32_t)mc_model_index(m);
    mc_profile_env env = {m, NULL, NULL};
    mc_profile_preset(&c, &env);
    return c;
}
static void normalise(mc_config *c, int changed) {
    mc_profile_env env = {mc_model_entry(c->model), NULL, NULL};
    mc_profile_normalise(c, &env, changed);
}

static void test_profile_rules(void) {
    /* Presets: a model's own settings, nothing of the one before. */
    mc_config c = model_config("centris-650");
    c.video_card = MC_VIDEO_CARD_LIVADIX; c.depth = 32; c.look = MC_LOOK_CRT; c.ram_mb = 136; c.fpu_full = 1;
    snprintf(c.disks[0].image, sizeof c.disks[0].image, "a.img");
    c.disk_count = 1;
    const mc_model *q700 = mc_model_find("quadra-700");
    c.model = (uint32_t)mc_model_index(q700);
    mc_profile_env env = {q700, NULL, NULL};
    mc_profile_preset(&c, &env);
    CHECK(c.width == 1152 && c.height == 870 && c.depth == 8 && c.video_card == MC_VIDEO_CARD_AUTO && c.look == MC_LOOK_PLAIN);
    CHECK(c.ram_mb == 4 && c.cpu_hz == q700->cpu_hz && !c.fpu_full && c.disk_count == 1);
    /* With Mac OS 8 on the startup disk: at least its 12 MB, a size the board takes. */
    mc_volume_info os8;
    memset(&os8, 0, sizeof os8);
    os8.bootable = 1;
    os8.system_version = 0x0800;
    env.boot = &os8;
    mc_profile_preset(&c, &env);
    CHECK_EQ(c.ram_mb, 20);
    CHECK_EQ(mc_profile_ram_least(&env), 12);
    c.ram_mb = 8;
    mc_profile_normalise(&c, &env, MC_CHANGED_RAM);
    CHECK_EQ(c.ram_mb, 20);
    /* Looks: crt on a model with a CRT (or a modular Mac's monitor), lcd on a PowerBook. */
    c = model_config("quadra-700");
    c.look = MC_LOOK_LCD;
    normalise(&c, MC_CHANGED_OTHER);
    CHECK_EQ(c.look, MC_LOOK_CRT);
    c = model_config("powerbook-170");
    c.look = MC_LOOK_CRT;
    normalise(&c, MC_CHANGED_OTHER);
    CHECK_EQ(c.look, MC_LOOK_LCD);
    c.look = MC_LOOK_PLAIN;
    normalise(&c, MC_CHANGED_OTHER);
    CHECK_EQ(c.look, MC_LOOK_PLAIN);
    CHECK(!mc_config_look_ok(mc_model_find("classic-ii"), MC_LOOK_LCD) && mc_config_look_ok(mc_model_find("classic-ii"), MC_LOOK_CRT));
    /* Memory snaps to the sizes the board takes. */
    c = model_config("quadra-700");
    c.ram_mb = 24; normalise(&c, MC_CHANGED_RAM); CHECK_EQ(c.ram_mb, 68);
    c.ram_mb = 900; normalise(&c, MC_CHANGED_RAM); CHECK_EQ(c.ram_mb, 68);
    uint32_t sizes[32];
    CHECK_EQ(mc_profile_ram_sizes(q700, sizes, 32), 4);
    CHECK(sizes[0] == 4 * 1024 && sizes[3] == 68 * 1024);
    /* Sizes that are not whole MiB: the Plus's and SE's 2.5 MB (two 1 MB
     * and two 256 KB SIMMs), between 2 and 4 MB. */
    CHECK_EQ(mc_profile_ram_sizes(mc_model_find("mac-plus"), sizes, 32), 4);
    CHECK(sizes[0] == 1024 && sizes[1] == 2048 && sizes[2] == 2560 && sizes[3] == 4096);
    CHECK_EQ(mc_profile_ram_sizes(mc_model_find("mac-se"), sizes, 32), 4);
    CHECK_EQ(sizes[2], 2560);
    c = model_config("mac-plus");
    mc_config_set_ram_kb(&c, 2560);
    CHECK(c.ram_mb == 2 && c.ram_kb == 2560 && mc_config_ram_kb(&c) == 2560);
    normalise(&c, MC_CHANGED_RAM);
    CHECK_EQ(mc_config_ram_kb(&c), 2560);
    c.ram_mb = 4;                               /* a whole size written over it */
    CHECK_EQ(mc_config_ram_kb(&c), 4096);
    mc_config_set_ram_kb(&c, 3 * 1024);         /* not a Plus size: the next one up */
    normalise(&c, MC_CHANGED_RAM);
    CHECK(mc_config_ram_kb(&c) == 4096 && c.ram_kb == 0);
    c = model_config("quadra-700");
    mc_config_set_ram_kb(&c, 2560);             /* the Quadra 700 takes whole sizes only */
    normalise(&c, MC_CHANGED_RAM);
    CHECK_EQ(mc_config_ram_kb(&c), 4096);
    {
        char text[24];
        CHECK(mc_config_parse_ram("2.5") == 2560 && mc_config_parse_ram("4") == 4096 && mc_config_parse_ram("0.125") == 128);
        CHECK(!mc_config_parse_ram("2.3") && !mc_config_parse_ram("2.") && !mc_config_parse_ram("x") && !mc_config_parse_ram("2000"));
        CHECK(!strcmp(mc_config_ram_text(2560, text, sizeof text), "2.5") && !strcmp(mc_config_ram_text(4096, text, sizeof text), "4"));
    }
    /* The Quadra 700: the DAFB has no 16-bit mode, the card does. */
    c.width = 640; c.height = 480; c.depth = 16; c.video_card = MC_VIDEO_CARD_BUILTIN;
    normalise(&c, MC_CHANGED_NONE);
    CHECK(c.depth == 8 || c.depth == 32);
    CHECK(!(mc_profile_depths(&c, q700, 640, 480) & MC_DEPTH_BIT(16)));
    c.video_card = MC_VIDEO_CARD_AUTO; c.depth = 16;
    normalise(&c, MC_CHANGED_DEPTH);
    CHECK_EQ(c.depth, 16);
    /* 1920 x 1080 fits the card's 2 MiB to 8 bits; built-in video just
     * chosen at a size it does not show takes a size it shows. */
    c.width = 1920; c.height = 1080; c.depth = 32;
    normalise(&c, MC_CHANGED_RESOLUTION);
    CHECK(c.width == 1920 && c.depth == 8);
    c.video_card = MC_VIDEO_CARD_BUILTIN;
    normalise(&c, MC_CHANGED_CARD);
    CHECK(c.video_card == MC_VIDEO_CARD_BUILTIN && c.width == 1152 && c.height == 870);
    c.width = 800; c.height = 600;
    normalise(&c, MC_CHANGED_RESOLUTION);
    CHECK(c.width == 800 && c.video_card == MC_VIDEO_CARD_AUTO);
    /* A PowerBook's panel is its only screen; no card. */
    c = model_config("powerbook-170");
    CHECK(c.width == 640 && c.height == 400);
    c.width = 1024; c.height = 768; c.video_card = MC_VIDEO_CARD_LIVADIX;
    normalise(&c, MC_CHANGED_RESOLUTION);
    CHECK(c.width == 640 && c.height == 400 && c.video_card == MC_VIDEO_CARD_AUTO && c.monitor == MC_MONITOR_LCD_GRAY);
    CHECK(!mc_profile_has_card(mc_model_find("powerbook-170")) && mc_profile_has_card(q700));
    /* The Classic II: 512 x 342 in 1 bit. */
    c = model_config("classic-ii");
    c.depth = 8;
    normalise(&c, MC_CHANGED_DEPTH);
    CHECK(c.width == 512 && c.height == 342 && c.depth == 1);
    /* A modular Mac's monochrome display: one of Apple's monochrome modes
     * when it is chosen, the color monitor when the size is. */
    c = model_config("iici");
    c.width = 800; c.height = 600; c.monitor = MC_MONITOR_MONO;
    normalise(&c, MC_CHANGED_MONITOR);
    CHECK(c.monitor == MC_MONITOR_MONO && c.width == 640 && c.height == 480 && c.depth <= 8);
    c.width = 800; c.height = 600;
    normalise(&c, MC_CHANGED_RESOLUTION);
    CHECK(c.monitor == MC_MONITOR_COLOR && c.width == 800);
    /* One mouse button always clicks. */
    c.mouse[0] = MC_MOUSE_NORMAL; c.mouse[1] = MC_MOUSE_CONTROL_CLICK;
    c.mouse[0] = MC_MOUSE_DISABLED;
    normalise(&c, MC_CHANGED_MOUSE_LEFT);
    CHECK(c.mouse[0] == MC_MOUSE_DISABLED && c.mouse[1] == MC_MOUSE_NORMAL);
    /* The buses, slots and images the model has. */
    c = model_config("quadra-700");
    c.disk_count = 3;
    snprintf(c.disks[0].image, sizeof c.disks[0].image, "a.img"); c.disks[0].bus = MC_BUS_ATA;
    snprintf(c.disks[1].image, sizeof c.disks[1].image, "b.img"); c.disks[1].scsi_id_set = 1; c.disks[1].scsi_id = 3;
    snprintf(c.disks[2].image, sizeof c.disks[2].image, TEST_IMAGE_RESPELLED);
    c.cdrom_bus = MC_BUS_ATA;
    c.network_card = MC_NET_CARD_NUBUS;
    c.controller = MC_CONTROLLER_FIRMWARE;
    c.fpu_full = 1;
    normalise(&c, MC_CHANGED_STORAGE);
    CHECK(c.disk_count == 2 && c.disks[0].bus == MC_BUS_SCSI && !c.disks[1].scsi_id_set && c.cdrom_bus == MC_BUS_SCSI);
    CHECK(c.network_card == MC_NET_CARD_AUTO && c.controller == MC_CONTROLLER_AUTO && c.fpu_full);
    c = model_config("quadra-630");
    c.disk_count = 1;
    snprintf(c.disks[0].image, sizeof c.disks[0].image, "a.img");
    mc_profile_env e630 = {mc_model_find("quadra-630"), NULL, NULL};
    mc_profile_preset(&c, &e630);
    CHECK(c.disks[0].bus == MC_BUS_ATA && !c.disks[0].scsi_id_set);
    c = model_config("powerbook-190");
    c.cdrom_bus = MC_BUS_ATA;
    normalise(&c, MC_CHANGED_STORAGE);
    CHECK(c.cdrom_bus == MC_BUS_ATA && c.floppy_count == 0);
    c = model_config("power-macintosh-g3");
    c.cdrom_bus = MC_BUS_ATA; c.floppy_count = 1;
    normalise(&c, MC_CHANGED_STORAGE);
    CHECK(c.cdrom_bus == MC_BUS_ATA && c.floppy_count == 1);
    unsigned g3_builtin=0,g3_card=0;
    mc_machine_video(mc_model_find("power-macintosh-g3"),1024,768,&g3_builtin,&g3_card);
    CHECK(!g3_builtin && (g3_card & MC_DEPTH_BIT(16)));

    unsigned builtin = 0, expansion = 0;
    const mc_model *psx5400 = mc_model_find("power-macintosh-5400");
    const mc_model *psx6400 = mc_model_find("power-macintosh-6400");
    mc_machine_video(psx5400, 640, 480, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)) && !expansion);
    mc_machine_video(psx5400, 800, 600, &builtin, &expansion);
    CHECK(!builtin && !expansion);
    mc_machine_video(psx6400, 800, 600, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)) && !expansion);
    mc_machine_video(psx6400, 1024, 768, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8)) && !expansion);
    mc_machine_video(psx6400, 1152, 870, &builtin, &expansion);
    CHECK(!builtin && !expansion);

    const mc_model *plus6500 = mc_model_find("power-macintosh-6500");
    const mc_model *plus5500 = mc_model_find("power-macintosh-5500");
    const mc_model *tam = mc_model_find("twentieth-anniversary-macintosh");
    mc_machine_video(plus6500, 800, 600, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16) | MC_DEPTH_BIT(32)) && !expansion);
    mc_machine_video(plus6500, 1280, 1024, &builtin, &expansion);
    CHECK(builtin == MC_DEPTH_BIT(8) && !expansion);
    mc_machine_video(plus5500, 800, 600, &builtin, &expansion);CHECK(!builtin && !expansion);
    mc_machine_video(tam, 800, 600, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)) && !expansion);
    mc_machine_video(tam, 640, 480, &builtin, &expansion);
    CHECK(builtin == (MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)) && !expansion);
    mc_machine_video(tam, 832, 624, &builtin, &expansion);CHECK(!builtin && !expansion);
    c = model_config("twentieth-anniversary-macintosh");
    c.floppy_count = 2;c.floppy_off[0] = c.floppy_off[1] = 0;
    normalise(&c, MC_CHANGED_OTHER);CHECK(!c.floppy_count && c.floppy_off[0] && c.floppy_off[1]);

    c = model_config("lc");
    c.network_card = MC_NET_CARD_NUBUS; normalise(&c, MC_CHANGED_OTHER); CHECK_EQ(c.network_card, MC_NET_CARD_AUTO);
    c.network_card = MC_NET_CARD_LC; normalise(&c, MC_CHANGED_OTHER); CHECK_EQ(c.network_card, MC_NET_CARD_LC);
    c = model_config("centris-610");
    c.fpu_full = 1; normalise(&c, MC_CHANGED_OTHER); CHECK(!c.fpu_full);
    c.charger_start = 1; normalise(&c, MC_CHANGED_OTHER); CHECK(!c.charger_start);
    c.pmmu = 1; normalise(&c, MC_CHANGED_OTHER); CHECK(!c.pmmu);
    c = model_config("powerbook-100");
    c.charger_start = 1; normalise(&c, MC_CHANGED_OTHER); CHECK(c.charger_start);
    c = model_config("mac-ii-v1");
    c.pmmu = 1; normalise(&c, MC_CHANGED_OTHER); CHECK(c.pmmu);
    /* What a running Macintosh takes at once, and what needs a new start. */
    mc_config a = model_config("quadra-700"), b = a;
    CHECK(mc_profile_same(&a, &b) && !mc_profile_needs_restart(&a, &b));
    b.look = MC_LOOK_CRT; b.filter = MC_CONFIG_FILTER_LINEAR; b.scale = MC_CONFIG_SCALE_INTEGER; b.vsync = 0; b.fullscreen = 1;
    b.command_key = MC_COMMAND_RIGHT_ALT; b.mouse[1] = MC_MOUSE_CONTROL_CLICK; b.refresh = 30; b.sound = 0; b.close_action = MC_CLOSE_QUIT;
    CHECK(!mc_profile_same(&a, &b) && !mc_profile_needs_restart(&a, &b));
    CHECK(mc_profile_needs_restart(&b, &a));                       /* sound on again: the machine was built without */
    b = a; snprintf(b.cdrom, sizeof b.cdrom, "x.iso"); CHECK(!mc_profile_needs_restart(&a, &b));  /* a disc goes in as it runs */
    b = a; b.ram_mb = 20; CHECK(mc_profile_needs_restart(&a, &b));
    b = a; b.width = 640; b.height = 480; CHECK(mc_profile_needs_restart(&a, &b));
    b = a; b.disk_count = 1; snprintf(b.disks[0].image, sizeof b.disks[0].image, "x.img"); CHECK(mc_profile_needs_restart(&a, &b));
    b = a; b.model = (uint32_t)mc_model_index(mc_model_find("quadra-900")); CHECK(mc_profile_needs_restart(&a, &b));
    /* Paths compare without letter case or slash. */
    a.disk_count = b.disk_count = 1;
    snprintf(a.disks[0].image, sizeof a.disks[0].image, "C:/Disks/A.img");
    b = a;
    snprintf(b.disks[0].image, sizeof b.disks[0].image, "c:\\disks\\a.IMG");
    CHECK(mc_profile_same(&a, &b));
}

static void test_profile_write(void) {
    /* A user's profiles: a profile's video keys set back to the model's go,
     * and the section with the last of them ("Expected a mapping"). */
    char text[16384];
    snprintf(text, sizeof text,
             "%.*s  \"Centris 650 \xe2\x80\x93 Mac OS 8.0\":\n    model: centris-650\n    emulation:\n      ram_mb: 24\n"
             "    storage:\n      disks:\n        - image: disks/HD240.HF8\n\n"
             "  Flow: {model: quadra-700, video: {look: crt, depth: 16}}\n",
             (int)sizeof mc_default_yaml, (const char *)mc_default_yaml);
    mc_ydoc doc;
    CHECK(mc_ydoc_parse(&doc, text, strlen(text)));
    const char *source = path_of("profile.yaml"), *c650 = "Centris 650 \xe2\x80\x93 Mac OS 8.0";
    mc_config c, back;
    char error[512];
    CHECK(mc_profile_read(&doc, c650, source, NULL, &c, error, sizeof error));
    CHECK(c.ram_mb == 24 && c.disk_count == 1 && c.width == 640 && c.height == 480);
    c.width = 1024; c.height = 768;
    CHECK(mc_profile_write(&doc, c650, &c, source, NULL, error, sizeof error));
    const char *video[] = {"profiles", c650, "video"}, *resolution[] = {"profiles", c650, "video", "resolution"};
    CHECK(mc_ydoc_has(&doc, resolution, 4));
    c.width = 640; c.height = 480;
    CHECK(mc_profile_write(&doc, c650, &c, source, NULL, error, sizeof error));
    CHECK(!mc_ydoc_has(&doc, video, 3));
    CHECK(mc_profile_read(&doc, c650, source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &c) && back.disk_count == 1);
    /* Removing a mapping's last key: a profile's section goes, a top-level
     * one keeps its comments as {}. */
    const char *depth[] = {"profiles", c650, "video", "depth"};
    CHECK(mc_ydoc_set(&doc, depth, 4, "16"));
    CHECK(mc_ydoc_remove(&doc, depth, 4) && !mc_ydoc_has(&doc, video, 3));
    mc_ydoc top;
    static const char sections[] = "video:\n  # resolution: 640x480\n  depth: 8\nsound:\n  enabled: true\n";
    CHECK(mc_ydoc_parse(&top, sections, sizeof sections - 1));
    const char *top_depth[] = {"video", "depth"};
    CHECK(mc_ydoc_remove(&top, top_depth, 2));
    char *t = mc_ydoc_text(&top, NULL);
    CHECK(t && strstr(t, "video: {}\n  # resolution: 640x480\n"));
    CHECK(t && mc_config_parse(t, strlen(t), source, NULL, &back, error, sizeof error));
    free(t);
    mc_ydoc_free(&top);
    /* A flow profile: its keys on lines of their own when written. */
    CHECK(mc_profile_read(&doc, "Flow", source, NULL, &c, error, sizeof error) && c.look == MC_LOOK_CRT && c.depth == 16);
    c.look = MC_LOOK_PLAIN;
    CHECK(mc_profile_write(&doc, "Flow", &c, source, NULL, error, sizeof error));
    CHECK(mc_profile_read(&doc, "Flow", source, NULL, &back, error, sizeof error) && back.look == MC_LOOK_PLAIN && back.depth == 16);
    /* Every kind of key, written and read back as Livadix reads it; the
     * other profile untouched. */
    c.ram_mb = 20; c.cpu_hz = 33330000; c.jit = 0; c.startup_sound = 0; c.close_action = MC_CLOSE_QUIT; c.close_timeout = 60;
    c.width = 832; c.height = 624; c.refresh = 30; c.video_card = MC_VIDEO_CARD_LIVADIX; c.video_acceleration = 0;
    c.monitor = MC_MONITOR_COLOR; c.filter = MC_CONFIG_FILTER_NEAREST; c.scale = MC_CONFIG_SCALE_INTEGER; c.fullscreen = 1; c.vsync = 0;
    c.disk_count = 2;
    snprintf(c.disks[0].image, sizeof c.disks[0].image, "%s", path_of("one disk.img")); c.disks[0].readonly = 1;
    snprintf(c.disks[1].image, sizeof c.disks[1].image, "D:\\Mac\\two.hfs"); c.disks[1].scsi_id_set = 1; c.disks[1].scsi_id = 5;
    c.floppy_count = 2; c.floppy_off[0] = 1; c.floppy_off[1] = 0;
    snprintf(c.floppies[1].image, sizeof c.floppies[1].image, "%s", path_of("f.img")); c.floppies[1].readonly = 1;
    snprintf(c.cdrom, sizeof c.cdrom, "%s", path_of("cd.iso")); c.boot_priority = MC_BOOT_CDROM; c.sound = 0;
    c.keyboard_layout = MC_KEYBOARD_ISO; c.command_key = MC_COMMAND_LEFT_CONTROL; c.mouse[1] = MC_MOUSE_CONTROL_CLICK;
    c.ports[0].type = MC_PORT_SERIAL; snprintf(c.ports[0].path, sizeof c.ports[0].path, TEST_SERIAL);
    c.ports[1].type = MC_PORT_PIPE; snprintf(c.ports[1].path, sizeof c.ports[1].path, TEST_PIPE);
    c.network = MC_NETWORK_NONE; c.forward_count = 1;
    CHECK(mc_config_parse_forward("tcp:0.0.0.0:2121:21", &c.forwards[0], error, sizeof error));
    snprintf(c.sharing_name, sizeof c.sharing_name, "My Mac"); c.sharing_metadata = MC_SHARING_APPLEDOUBLE;
    snprintf(c.sharing_password, sizeof c.sharing_password, "secret");
    c.share_count = 1; snprintf(c.shares[0].name, sizeof c.shares[0].name, "PC");
    snprintf(c.shares[0].path, sizeof c.shares[0].path, "C:/Share"); c.shares[0].readonly = 1;
    CHECK(mc_profile_write(&doc, "Flow", &c, source, NULL, error, sizeof error));
    if (error[0] && !mc_profile_read(&doc, "Flow", source, NULL, &back, error, sizeof error)) fprintf(stderr, "%s\n", error);
    CHECK(mc_profile_read(&doc, "Flow", source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &c));
    /* Every model chosen for this profile, as the window chooses one (its
     * preset, then the rules): written, read back as edited, the whole
     * document still read. */
    {
        const mc_config rich = c;
        size_t failed = 0;
        for (size_t i = 0; i < mc_model_count(); ++i) {
            mc_ydoc trial;
            CHECK(mc_ydoc_copy(&trial, &doc));
            mc_config m = rich;
            m.model = (uint32_t)i;
            mc_profile_env env = {mc_model_entry(i), NULL, NULL};
            mc_profile_preset(&m, &env);
            int ok = mc_profile_write(&trial, "Flow", &m, source, NULL, error, sizeof error);
            if (ok) ok = mc_profile_read(&trial, "Flow", source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &m);
            size_t n = 0;
            char *whole = ok ? mc_ydoc_text(&trial, &n) : NULL;
            mc_profile_list *l = malloc(sizeof *l);
            if (ok) ok = whole && l && mc_config_profiles_text(whole, n, source, l, error, sizeof error);
            free(l);
            free(whole);
            if (!ok && failed++ < 8) fprintf(stderr, "model %s: %s\n", mc_model_entry(i)->key, error);
            mc_ydoc_free(&trial);
        }
        CHECK_EQ(failed, 0);
        /* The same from a profile that writes nothing, under shared
         * settings that name disks, two floppy drives, a CD, a screen and
         * memory no single model takes all of. */
        static const char shared[] = "emulation:\n  ram_mb: 24\nvideo:\n  resolution: 1024x768\n  depth: 16\n"
                                     "storage:\n  disks:\n    - image: shared.img\n  floppies: [{}, {}]\n  cdrom: cd.iso\n"
                                     "profiles:\n  P:\n    model: quadra-700\n";
        mc_ydoc base;
        CHECK(mc_ydoc_parse(&base, shared, sizeof shared - 1));
        for (size_t i = 0; i < mc_model_count(); ++i) {
            mc_ydoc trial;
            CHECK(mc_ydoc_copy(&trial, &base));
            mc_config m;
            int ok = mc_profile_read(&trial, "P", source, NULL, &m, error, sizeof error);
            m.model = (uint32_t)i;
            mc_profile_env env = {mc_model_entry(i), NULL, NULL};
            mc_profile_preset(&m, &env);
            if (ok) ok = mc_profile_write(&trial, "P", &m, source, NULL, error, sizeof error);
            if (ok) ok = mc_profile_read(&trial, "P", source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &m);
            if (!ok && failed++ < 8) fprintf(stderr, "model %s (shared): %s\n", mc_model_entry(i)->key, error);
            mc_ydoc_free(&trial);
        }
        mc_ydoc_free(&base);
        CHECK_EQ(failed, 0);
    }
    /* ... and back to nothing: the empty lists and the unconnected ports
     * the shared settings and the model give. */
    c.disk_count = 0; c.cdrom[0] = 0; c.ports[0].type = c.ports[1].type = MC_PORT_DEFAULT; c.forward_count = 0; c.share_count = 0;
    c.sharing_password[0] = 0;
    CHECK(mc_profile_write(&doc, "Flow", &c, source, NULL, error, sizeof error));
    CHECK(mc_profile_read(&doc, "Flow", source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &c));
    CHECK(mc_profile_read(&doc, c650, source, NULL, &back, error, sizeof error) && back.ram_mb == 24 && back.disk_count == 1);
    /* A model changed: written, and another model's ROM file does not follow. */
    const char *rom[] = {"profiles", c650, "emulation", "rom"};
    CHECK(mc_ydoc_set(&doc, rom, 4, "roms/centris-610.rom"));
    CHECK(mc_profile_read(&doc, c650, source, NULL, &c, error, sizeof error));
    c.model = (uint32_t)mc_model_index(mc_model_find("quadra-800"));
    CHECK(mc_profile_write(&doc, c650, &c, source, NULL, error, sizeof error));
    CHECK(!mc_ydoc_has(&doc, rom, 4));
    CHECK(mc_profile_read(&doc, c650, source, NULL, &back, error, sizeof error));
    CHECK(back.nvram_id[0] != 0);
    /* A profile that does not read is written anew. */
    const char *broken[] = {"profiles", "Broken"};
    CHECK(mc_ydoc_set(&doc, broken, 2, "{model: quadra-700, video: 7}"));
    CHECK(!mc_profile_read(&doc, "Broken", source, NULL, &c, error, sizeof error));
    CHECK(mc_profile_read(&doc, c650, source, NULL, &back, error, sizeof error));
    c = model_config("quadra-700");
    CHECK(mc_profile_write(&doc, "Broken", &c, source, NULL, error, sizeof error));
    CHECK(mc_profile_read(&doc, "Broken", source, NULL, &back, error, sizeof error) && mc_profile_same(&back, &c));
    /* The whole document reads. */
    size_t length = 0;
    char *all = mc_ydoc_text(&doc, &length);
    mc_profile_list *list = malloc(sizeof *list);
    CHECK(all && list && mc_config_profiles_text(all, length, source, list, error, sizeof error));
    free(list);
    free(all);
    mc_ydoc_free(&doc);
}

/* --- disk images ----------------------------------------------------------------- */

static uint32_t diskcopy_sum(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i += 2) { sum += (uint32_t)data[i] << 8 | data[i + 1]; sum = sum >> 1 | sum << 31; }
    return sum;
}
static uint8_t *file_bytes(const char *name, size_t size) {
    uint8_t *data = malloc(size);
    FILE *f = fopen(path_of(name), "rb");
    if (!f || !data || fread(data, 1, size, f) != size) { free(data); data = NULL; }
    if (f) fclose(f);
    return data;
}

static void test_disk(void) {
    char error[256];
    const size_t size = 800 * 1024;
    uint8_t *data = calloc(1, size + 0x54), sector[512];
    if (!data) { CHECK(0); return; }
    for (size_t i = 0; i < size; ++i) data[0x54 + i] = (uint8_t)(i * 13 + i / 512);
    write_bytes("raw.img", data + 0x54, size);
    /* Raw: written in place. */
    mc_disk *d = mc_disk_open(path_of("raw.img"), 1, 512, error, sizeof error);
    CHECK(d && mc_disk_sectors(d) == 1600 && mc_disk_floppy(d) && !mc_disk_readonly(d));
    memset(sector, 0xa5, sizeof sector);
    CHECK(mc_disk_write(d, 7, 1, sector));
    CHECK(mc_disk_write_bytes(d, 512 * 9 + 100, 3, "abc"));
    CHECK(mc_disk_same_file(d, path_of("raw.img")));
    mc_disk_close(d);
    uint8_t *back = file_bytes("raw.img", size);
    CHECK(back && back[512 * 7] == 0xa5 && back[512 * 8 - 1] == 0xa5 && !memcmp(back + 512 * 9 + 100, "abc", 3));
    CHECK(back && back[512 * 9 + 99] == data[0x54 + 512 * 9 + 99]);
    free(back);
    /* Read-only: no write reaches the file. */
    d = mc_disk_open(path_of("raw.img"), 0, 512, error, sizeof error);
    CHECK(d && mc_disk_readonly(d) && !mc_disk_write(d, 0, 1, sector));
    CHECK(d && mc_disk_read(d, 7, 1, sector) && sector[0] == 0xa5);
    CHECK(d && !mc_disk_read(d, 1600, 1, sector));
    mc_disk_close(d);
    /* DiskCopy 4.2: the header's data checksum follows every write. */
    memset(data, 0, 0x54);
    data[0] = 4; memcpy(data + 1, "Test", 4);
    be32(data + 0x40, (uint32_t)size);
    be32(data + 0x48, diskcopy_sum(data + 0x54, size));
    data[0x50] = 1; data[0x51] = 2; data[0x52] = 1; data[0x53] = 0;
    write_bytes("copy.image", data, size + 0x54);
    d = mc_disk_open(path_of("copy.image"), 1, 512, error, sizeof error);
    CHECK(d && mc_disk_sectors(d) == 1600 && mc_disk_floppy(d));
    CHECK(d && mc_disk_read(d, 3, 1, sector) && !memcmp(sector, data + 0x54 + 3 * 512, 512));
    memset(sector, 0x3c, sizeof sector);
    CHECK(d && mc_disk_write(d, 3, 1, sector));
    mc_disk_close(d);
    back = file_bytes("copy.image", size + 0x54);
    CHECK(back && back[0x54 + 3 * 512] == 0x3c);
    if (back) {
        uint32_t stored = (uint32_t)back[0x48] << 24 | (uint32_t)back[0x49] << 16 | (uint32_t)back[0x4a] << 8 | back[0x4b];
        CHECK_EQ(stored, diskcopy_sum(back + 0x54, size));
    }
    free(back);
    data[0x54] ^= 0xff;    /* the stored checksum no longer matches */
    write_bytes("bad.image", data, size + 0x54);
    CHECK(!mc_disk_open(path_of("bad.image"), 0, 512, error, sizeof error) && strstr(error, "checksum mismatch"));
    /* 1680 KiB is not a floppy size. */
    uint8_t *big = calloc(1, 1680 * 1024);
    write_bytes("dmf.img", big, 1680 * 1024);
    free(big);
    d = mc_disk_open(path_of("dmf.img"), 0, 512, error, sizeof error);
    CHECK(d && !mc_disk_floppy(d));
    mc_disk_close(d);
    /* 400 KiB: raw and DiskCopy 4.2 are floppies. */
    write_bytes("k400.img", data + 0x54, 400 * 1024);
    d = mc_disk_open(path_of("k400.img"), 1, 512, error, sizeof error);
    CHECK(d && mc_disk_sectors(d) == 800 && mc_disk_floppy(d));
    mc_disk_close(d);
    memset(data, 0, 0x54);
    data[0] = 4; memcpy(data + 1, "K400", 4);
    be32(data + 0x40, 400 * 1024);
    be32(data + 0x48, diskcopy_sum(data + 0x54, 400 * 1024));
    data[0x50] = 0; data[0x51] = 2; data[0x52] = 1; data[0x53] = 0;
    write_bytes("k400.image", data, 400 * 1024 + 0x54);
    d = mc_disk_open(path_of("k400.image"), 1, 512, error, sizeof error);
    CHECK(d && mc_disk_sectors(d) == 800 && mc_disk_floppy(d));
    CHECK(d && mc_disk_write(d, 5, 1, sector));
    mc_disk_close(d);
    /* CD: 2048-byte sectors, read-only. */
    write_bytes("disc.iso", data, 2048 * 10);
    d = mc_disk_open(path_of("disc.iso"), 0, 2048, error, sizeof error);
    CHECK(d && mc_disk_sectors(d) == 10 && !mc_disk_floppy(d) && mc_disk_readonly(d));
    mc_disk_close(d);
    CHECK(!mc_disk_open(path_of("disc.iso"), 1, 2048, error, sizeof error) && strstr(error, "read-only"));
    write_bytes("odd.iso", data, 2048 * 10 + 512);
    CHECK(!mc_disk_open(path_of("odd.iso"), 0, 2048, error, sizeof error));
    CHECK(!mc_disk_open(path_of("missing.img"), 0, 512, error, sizeof error));
    free(data);
}

/* --- what disk images hold ------------------------------------------------------------ */

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
/* A resource fork of 'vers' 1 and, when boxes, a 'gbly' -16385. */
/* pad: bytes between the header and the data, to make a fork of several
 * allocation blocks. */
static size_t make_fork(uint8_t *out, uint16_t version, const char *text, const uint16_t *boxes, size_t box_count, size_t pad) {
    uint8_t data[512];
    size_t d = 0;
    uint32_t vers_at = 0, gbly_at = 0;
    /* 'vers': BCD version, stage, release, region, short and long strings. */
    const size_t n = strlen(text);
    vers_at = (uint32_t)d;
    be32(data + d, (uint32_t)(6 + 1 + n + 1));
    put16(data + d + 4, version);
    data[d + 6] = 0x80; data[d + 7] = 0; put16(data + d + 8, 0);
    data[d + 10] = (uint8_t)n; memcpy(data + d + 11, text, n); data[d + 11 + n] = 0;
    d += 4 + 6 + 1 + n + 1;
    if (boxes) {
        gbly_at = (uint32_t)d;
        be32(data + d, (uint32_t)(8 + 2 * box_count));
        put16(data + d + 4, 1); be32(data + d + 6, 0xA6C2441Bu); put16(data + d + 10, (uint32_t)box_count);
        for (size_t i = 0; i < box_count; ++i) put16(data + d + 12 + 2 * i, boxes[i]);
        d += 4 + 8 + 2 * box_count;
    }
    /* The map: header copy, next, file, attributes, type list offset, name
     * list offset, then the type list and the reference lists. */
    const int types = boxes ? 2 : 1;
    const size_t map = 16 + pad + d, type_list = 28, refs = type_list + 2 + 8 * (size_t)types, map_size = refs + 12 * (size_t)types;
    memset(out, 0, map + map_size);
    be32(out, (uint32_t)(16 + pad)); be32(out + 4, (uint32_t)map); be32(out + 8, (uint32_t)d); be32(out + 12, (uint32_t)map_size);
    memcpy(out + 16 + pad, data, d);
    uint8_t *m = out + map;
    memcpy(m, out, 16);
    put16(m + 24, (uint32_t)type_list);
    put16(m + 26, (uint32_t)map_size);
    put16(m + type_list, (uint32_t)(types - 1));
    for (int t = 0; t < types; ++t) {
        uint8_t *e = m + type_list + 2 + 8 * t;
        memcpy(e, t ? "gbly" : "vers", 4);
        put16(e + 4, 0);
        put16(e + 6, (uint32_t)(refs - type_list + 12 * (size_t)t));
        uint8_t *r = m + refs + 12 * (size_t)t;
        put16(r, t ? 0xBFFFu : 1);             /* -16385, 1 */
        put16(r + 2, 0xFFFF);
        be32(r + 4, t ? gbly_at : vers_at);
    }
    return map + map_size;
}

/* A synthesized HFS volume (Inside Macintosh: Files): boot blocks, the MDB,
 * an extents tree whose leaf holds the System's fourth resource extent, a
 * catalog of the System Folder (System, Finder, an enabler) and its
 * Extensions folder (MODE32). 512-byte allocation blocks from sector 4. */
#define SYNTH_BLOCKS 128
static void synth_volume(uint8_t *v, int blessed, int with_enabler) {
    memset(v, 0, SYNTH_BLOCKS * 512);
    v[0] = 'L'; v[1] = 'K';
    v[10] = 6; memcpy(v + 11, "System", 6);
    v[26] = 6; memcpy(v + 27, "Finder", 6);
    uint8_t *mdb = v + 1024;
    put16(mdb, 0x4244);
    put16(mdb + 18, SYNTH_BLOCKS - 4);
    be32(mdb + 20, 512);
    put16(mdb + 28, 4);
    mdb[36] = 5; memcpy(mdb + 37, "Synth", 5);
    be32(mdb + 92, blessed ? 16 : 0);
    be32(mdb + 130, 1024); put16(mdb + 134, 0); put16(mdb + 136, 2);   /* extents: blocks 0-1 */
    be32(mdb + 146, 1536); put16(mdb + 150, 2); put16(mdb + 152, 3);   /* catalog: blocks 2-4 */
    uint8_t *ab = v + 4 * 512;                                          /* allocation block 0 */
    /* The B*-tree header nodes: first leaf node 1, node size 512. */
    for (int t = 0; t < 2; ++t) {
        uint8_t *h = ab + (size_t)t * 1024;
        h[8] = 1;                                                       /* header node */
        put16(h + 10, 3);
        be32(h + 14 + 10, 1); be32(h + 14 + 14, 1);
        put16(h + 14 + 18, 512);
    }
    /* Forks: System's resources in four one-block extents (blocks 8, 10, 12, 14
     * and the overflow's 16), the Finder's at 20, the enabler's at 24. */
    uint8_t fork[4096];
    static const uint16_t system_boxes[] = {16, 20}, enabler_boxes[] = {83, 88};
    const size_t system_size = make_fork(fork, 0x0711, "7.1.1", system_boxes, 2, 1600);
    CHECK(system_size > 3 * 512 && system_size <= 4 * 512);
    for (int k = 0; k < 4; ++k) memcpy(ab + (size_t)(8 + 2 * k) * 512, fork + 512 * k, 512);
    const size_t finder_size = make_fork(fork, 0x0713, "7.1.3", NULL, 0, 0);
    memcpy(ab + 20 * 512, fork, finder_size);
    const size_t enabler_size = make_fork(fork, 0x0711, "7.1.1", enabler_boxes, 2, 0);
    memcpy(ab + 24 * 512, fork, enabler_size);
    /* The extents leaf: the System's (file 20) resource fork from fork block 3. */
    uint8_t *xleaf = ab + 512;
    xleaf[8] = 0xFF; xleaf[9] = 1; put16(xleaf + 10, 1);
    uint8_t *xr = xleaf + 14;
    xr[0] = 7; xr[1] = 0xFF; be32(xr + 2, 20); put16(xr + 6, 3);
    put16(xr + 8, 14); put16(xr + 10, 1);
    put16(xleaf + 510, 14); put16(xleaf + 508, 14 + 20);
    /* The catalog's two leaf nodes (1 and 2, linked). */
    struct { uint32_t parent; const char *name; int dir; uint32_t id; const char *type; uint32_t first, size, blocks; } recs[] = {
        {2, "System Folder", 1, 16, NULL, 0, 0, 0},
        {16, "Extensions", 1, 17, NULL, 0, 0, 0},
        {16, "Finder", 0, 21, "FNDRMACS", 20, (uint32_t)finder_size, 1},
        {16, "System", 0, 20, "zsysMACS", 8, (uint32_t)system_size, 0},
        {16, "System Enabler 065", 0, 22, "gblyMACS", 24, (uint32_t)enabler_size, 1},
        {17, "MODE32", 0, 23, "INITMODE", 0, 0, 0},
    };
    uint8_t *leaf = NULL;
    size_t at = 0, count = 0, node = 0;
    uint16_t offsets[8];
    for (size_t i = 0; i <= sizeof recs / sizeof *recs; ++i) {
        const int done = i == sizeof recs / sizeof *recs;
        if (leaf && (done || at + 130 > 512 - 2 * (count + 2))) {
            /* Close the node: its record count and offsets. */
            put16(leaf + 10, (uint32_t)count);
            offsets[count] = (uint16_t)at;
            for (size_t k = 0; k <= count; ++k) put16(leaf + 510 - 2 * k, offsets[k]);
            if (!done) be32(leaf, (uint32_t)node + 1);
            leaf = NULL;
        }
        if (done) break;
        if (!leaf) {
            node++;
            leaf = ab + (size_t)(2 + node) * 512;
            leaf[8] = 0xFF; leaf[9] = 1;
            at = 14; count = 0;
        }
        if (!with_enabler && !strcmp(recs[i].name, "System Enabler 065")) continue;
        uint8_t *r = leaf + at;
        const size_t n = strlen(recs[i].name), key = 6 + n;
        offsets[count++] = (uint16_t)at;
        r[0] = (uint8_t)key; be32(r + 2, recs[i].parent); r[6] = (uint8_t)n; memcpy(r + 7, recs[i].name, n);
        uint8_t *d = r + ((key + 2) & ~(size_t)1);
        if (recs[i].dir) {
            d[0] = 1; be32(d + 6, recs[i].id);
            at = (size_t)(d - leaf) + 70;
        } else {
            d[0] = 2; memcpy(d + 4, recs[i].type, 8); be32(d + 20, recs[i].id);
            be32(d + 36, recs[i].size);
            if (recs[i].id == 20) {
                for (int k = 0; k < 3; ++k) { put16(d + 86 + 4 * k, 8 + 2 * (uint32_t)k); put16(d + 88 + 4 * k, 1); }
            } else if (recs[i].size) {
                put16(d + 86, recs[i].first); put16(d + 88, 2);
            }
            at = (size_t)(d - leaf) + 102;
        }
    }
}

static void test_volumes(void) {
    static uint8_t image[(SYNTH_BLOCKS + 64) * 512];
    char error[256];
    mc_volume_info v;
    /* A bare volume. */
    synth_volume(image, 1, 1);
    write_bytes("synth.hfs", image, SYNTH_BLOCKS * 512);
    CHECK(mc_volume_inspect(path_of("synth.hfs"), &v, error, sizeof error));
    CHECK(v.kind == MC_IMAGE_HFS && !strcmp(v.volume, "Synth") && v.bootable);
    CHECK(v.system_version == 0x0711 && !strcmp(v.system_text, "7.1.1") && !strcmp(v.system_name, "System"));
    CHECK(v.finder_version == 0x0713 && !strcmp(v.finder_text, "7.1.3"));
    CHECK(v.system_gbly && v.system_box_count == 2 && v.system_boxes[0] == 16);
    CHECK(v.enabler_count == 1 && !strcmp(v.enablers[0].name, "System Enabler 065") && v.enablers[0].box_count == 2 &&
          v.enablers[0].boxes[1] == 88);
    CHECK(v.mode32);
    /* Its compatibility: the System lists the Quadra 700; the enabler the 605. */
    mc_compat r;
    CHECK(mc_compat_check(mc_model_find("quadra-700"), &v, 20 * 1024, &r) == MC_COMPAT_OK);
    CHECK(mc_compat_check(mc_model_find("quadra-605"), &v, 8 * 1024, &r) == MC_COMPAT_OK && strstr(r.reason, "with System Enabler 065"));
    CHECK(mc_compat_check(mc_model_find("powerbook-180"), &v, 8 * 1024, &r) == MC_COMPAT_WARNING &&
          strstr(r.reason, "System 7.1.1 on a PowerBook 180 needs System Enabler 131, not found on the disk"));
    CHECK(mc_compat_check(mc_model_find("iici"), &v, 8 * 1024, &r) == MC_COMPAT_OK);
    /* The ROM patches: a System without the Plus ROM's 'PTCH' 117 is not
     * for the Plus; with it, it is; without any 'PTCH', not checked. */
    {
        mc_volume_info p = v;
        p.system_ptch_count = 2, p.system_ptch[0] = 0, p.system_ptch[1] = 630;
        CHECK(mc_compat_check(mc_model_find("mac-plus"), &p, 4 * 1024, &r) == MC_COMPAT_WARNING && strstr(r.reason, "('PTCH' 117)"));
        CHECK(mc_compat_check(mc_model_find("mac-se"), &p, 4 * 1024, &r) == MC_COMPAT_OK);
        p.system_ptch[1] = 117;
        CHECK(mc_compat_check(mc_model_find("mac-plus"), &p, 4 * 1024, &r) == MC_COMPAT_OK);
        p.system_ptch_count = 0;
        CHECK(mc_compat_check(mc_model_find("mac-plus"), &p, 4 * 1024, &r) == MC_COMPAT_OK);
    }
    /* The PowerPC models: their oldest and newest Systems, enablers, Mac OS
     * 7.6 refused where 7.6.1 runs, memory for 8.5 and 9; the 68k ROMs'
     * 'PTCH' check does not apply to theirs. */
    {
        mc_volume_info p = v;
        const mc_model *pm6100 = mc_model_find("power-macintosh-6100");
        p.system_text[0] = 0;
        CHECK(mc_compat_check(pm6100, &p, 16 * 1024, &r) == MC_COMPAT_WARNING && strstr(r.reason, "needs System 7.1.2 or later"));
        p.system_version = 0x0753;
        p.system_ptch_count = 2, p.system_ptch[0] = 0, p.system_ptch[1] = 630;
        CHECK(mc_compat_check(pm6100, &p, 16 * 1024, &r) == MC_COMPAT_OK);
        p.system_version = 0x0752;
        CHECK(mc_compat_check(mc_model_find("power-macintosh-7200"), &p, 16 * 1024, &r) == MC_COMPAT_WARNING &&
              strstr(r.reason, "needs System Enabler 701, not found on the disk"));
        p.system_version = 0x0760;
        CHECK(mc_compat_check(mc_model_find("power-macintosh-7300"), &p, 32 * 1024, &r) == MC_COMPAT_WARNING &&
              strstr(r.reason, "Mac OS 7.6.1 and the Systems before 7.6 do"));
        p.system_version = 0x0761;
        CHECK(mc_compat_check(mc_model_find("power-macintosh-7300"), &p, 32 * 1024, &r) == MC_COMPAT_OK);
        p.system_version = 0x0850;
        CHECK(mc_compat_min_ram(&p) == 16);
        CHECK(mc_compat_check(mc_model_find("quadra-700"), &p, 32 * 1024, &r) == MC_COMPAT_WARNING && strstr(r.reason, "needs a PowerPC"));
        p.system_version = 0x0922;
        CHECK(mc_compat_min_ram(&p) == 32);
        CHECK(mc_compat_check(pm6100, &p, 40 * 1024, &r) == MC_COMPAT_WARNING && strstr(r.reason, "Mac OS 9.2.2 needs a G3") &&
              strstr(r.reason, "up to Mac OS 9.1"));
        CHECK(mc_compat_check(mc_model_find("power-macintosh-g3"), &p, 64 * 1024, &r) == MC_COMPAT_OK);
        CHECK(mc_compat_check(mc_model_find("power-macintosh-g3"), &p, 16 * 1024, &r) == MC_COMPAT_WARNING &&
              strstr(r.reason, "needs at least 32 MB"));
        p.system_version = 0x0904;
        CHECK(mc_compat_check(mc_model_find("power-macintosh-8100"), &p, 40 * 1024, &r) == MC_COMPAT_OK);
    }
    /* Inside an Apple partition map, after a driver partition. */
    memset(image, 0, sizeof image);
    image[0] = 'E'; image[1] = 'R'; put16(image + 2, 512);
    for (int i = 0; i < 3; ++i) {
        uint8_t *pm = image + 512 * (1 + i);
        pm[0] = 'P'; pm[1] = 'M'; be32(pm + 4, 3);
        static const char *const types[] = {"Apple_partition_map", "Apple_Driver43", "Apple_HFS"};
        be32(pm + 8, i == 2 ? 64 : i == 1 ? 32 : 1); be32(pm + 12, i == 2 ? SYNTH_BLOCKS : 32);
        snprintf((char *)pm + 48, 32, "%s", types[i]);
    }
    synth_volume(image + 64 * 512, 1, 0);
    write_bytes("synth.img", image, (SYNTH_BLOCKS + 64) * 512);
    CHECK(mc_volume_inspect(path_of("synth.img"), &v, error, sizeof error));
    CHECK(v.kind == MC_IMAGE_PARTITIONED && v.bootable && v.system_version == 0x0711 && v.enabler_count == 0);
    CHECK(mc_compat_check(mc_model_find("quadra-605"), &v, 8 * 1024, &r) == MC_COMPAT_WARNING &&
          strstr(r.reason, "System 7.1.1 on a Macintosh Quadra 605 needs System Enabler 065, not found on the disk"));
    /* Not a startup disk; not a volume. */
    synth_volume(image, 0, 1);
    write_bytes("data.hfs", image, SYNTH_BLOCKS * 512);
    CHECK(mc_volume_inspect(path_of("data.hfs"), &v, error, sizeof error) && v.kind == MC_IMAGE_HFS && !v.bootable);
    CHECK(mc_compat_check(mc_model_find("quadra-700"), &v, 8 * 1024, &r) == MC_COMPAT_UNKNOWN);
    memset(image, 0x5A, 64 * 512);
    write_bytes("noise.img", image, 64 * 512);
    CHECK(mc_volume_inspect(path_of("noise.img"), &v, error, sizeof error) && v.kind == MC_IMAGE_UNKNOWN && !v.bootable);
    CHECK(!mc_volume_inspect(path_of("absent.img"), &v, error, sizeof error));
    /* The boot image of a configuration: ID 0 first, then 6 down to 1. */
    mc_config c;
    write_text("boot.yaml", "storage:\n  disks:\n    - image: data.hfs\n    - {image: synth.img, scsi_id: 6}\n    - {image: synth.hfs, scsi_id: 5}\n");
    CHECK(mc_config_load(path_of("boot.yaml"), &c, error, sizeof error));
    const char *boot = mc_config_boot_image(&c, &v);
    CHECK(boot && strstr(boot, "synth.img") && v.kind == MC_IMAGE_PARTITIONED);
    /* An MFS floppy (a 400K System disk's flat file system) holding a System file
     * is a startup disk, and a disk in a drive starts the Macintosh before a
     * hard disk does; a Mac OS Extended volume is reported as unexamined. */
    {
        static uint8_t mfs[64 * 512];
        memset(mfs, 0, sizeof mfs);
        uint8_t *mdb = mfs + 1024;
        put16(mdb, 0xD2D7); put16(mdb + 14, 3); put16(mdb + 16, 1); put16(mdb + 18, 20);
        be32(mdb + 20, 1024); put16(mdb + 28, 4);
        mdb[36] = 4; memcpy(mdb + 37, "Disk", 4);
        mdb[64] = 0x00; mdb[65] = 0x30; mdb[66] = 0x01;         /* the allocation block map: block 2 -> 3, block 3 the last */
        uint8_t *entry = mfs + 3 * 512;
        entry[0] = 0x80; memcpy(entry + 2, "ZSYS", 4); memcpy(entry + 6, "MACS", 4);
        put16(entry + 32, 2); be32(entry + 34, 1500);           /* the resource fork: allocation blocks 2 and 3 */
        entry[50] = 6; memcpy(entry + 51, "System", 6);
        write_bytes("mfs.img", mfs, sizeof mfs);
        CHECK(mc_volume_inspect(path_of("mfs.img"), &v, error, sizeof error));
        CHECK(v.kind == MC_IMAGE_MFS && v.bootable && v.system_unversioned && !v.system_version);
        CHECK_STR(v.system_name, "System");
        CHECK(mc_compat_check(mc_model_find("mac-128k"), &v, 128, &r) == MC_COMPAT_UNKNOWN && strstr(r.reason, "before 6.0"));
        CHECK(mc_compat_check(mc_model_find("quadra-700"), &v, 8 * 1024, &r) == MC_COMPAT_WARNING && strstr(r.reason, "needs System"));
        entry[0] = 0;                                           /* no file: not a startup disk */
        write_bytes("mfs-data.img", mfs, sizeof mfs);
        CHECK(mc_volume_inspect(path_of("mfs-data.img"), &v, error, sizeof error) && v.kind == MC_IMAGE_MFS && !v.bootable);
        write_text("floppy.yaml", "storage:\n  disks:\n    - {image: synth.img, scsi_id: 0}\n  floppies:\n    - {image: mfs.img}\n");
        CHECK(mc_config_load(path_of("floppy.yaml"), &c, error, sizeof error));
        boot = mc_config_boot_image(&c, &v);
        CHECK(boot && strstr(boot, "mfs.img") && v.kind == MC_IMAGE_MFS);
        memset(mfs, 0, sizeof mfs);
        put16(mfs + 1024, 0x482B);
        write_bytes("plus.img", mfs, sizeof mfs);
        write_text("plus.yaml", "storage:\n  disks:\n    - {image: data.hfs}\n    - {image: plus.img}\n");
        CHECK(mc_config_load(path_of("plus.yaml"), &c, error, sizeof error));
        CHECK(!mc_config_boot_image(&c, &v));
        boot = mc_config_unexamined_image(&c, &v);
        CHECK(boot && strstr(boot, "plus.img") && v.kind == MC_IMAGE_HFS_PLUS);
    }
    char text[16];
    CHECK_STR(mc_version_text(0x0711, text, sizeof text), "7.1.1");
    CHECK_STR(mc_version_text(0x0800, text, sizeof text), "8.0");
    CHECK_STR(mc_version_text(0x0608, text, sizeof text), "6.0.8");
    /* The table: every model has a row but the Pippins (consoles); the oldest
     * System before the newest. */
    CHECK_EQ(mc_compat_count(), mc_model_count() - 3);   /* and the Built-in machine, which runs no System */
    for (size_t i = 0; i < mc_model_count(); ++i) {
        const mc_model *m = mc_model_entry(i);
        const mc_compat_entry *e = mc_compat_find(m->key);
        CHECK(m->console || mc_model_builtin(m) ? !e : e && e->min_system < e->max_system);
        /* Mac OS 8 needs a 68040 or a PowerPC (Technote 1102); 9 a PowerPC. */
        if (e) CHECK((e->max_system >= 0x0800) == (strstr(m->cpu, "040") != NULL || mc_model_ppc(m)));
        if (e) CHECK((e->max_system >= 0x0900) == mc_model_ppc(m));
    }
}

/* A volume left mounted (drAtrb bit 8 clear) whose extents file is 1 MiB:
 * the ROM's check would walk one record (its DIVU.W overflows), so
 * mc_hfs_left_mounted gives the block as the check leaves it, and the
 * virtual disk reads it until the guest writes that block. */
static void test_left_mounted(void) {
    enum { XT_BLOCKS = 2048, BLOCKS = 4 + SYNTH_BLOCKS + XT_BLOCKS };
    static uint8_t image[BLOCKS * 512];
    char error[256];
    uint8_t mdb[512];
    uint64_t at = 0;
    synth_volume(image, 1, 1);                          /* drAtrb 0: left mounted */
    write_bytes("left.hfs", image, SYNTH_BLOCKS * 512);
    mc_disk *d = mc_disk_open(path_of("left.hfs"), 0, 512, error, sizeof error);
    CHECK(d && !mc_hfs_left_mounted(d, &at, mdb));      /* a 1 KiB extents file: the check walks it */
    mc_disk_close(d);
    /* The extents tree moved to a 1 MiB file at allocation block SYNTH_BLOCKS. */
    memcpy(image + (4 + SYNTH_BLOCKS) * 512, image + 4 * 512, 1024);
    be32(image + 1024 + 130, XT_BLOCKS * 512);
    put16(image + 1024 + 134, SYNTH_BLOCKS);
    put16(image + 1024 + 136, XT_BLOCKS);
    write_bytes("left.hfs", image, sizeof image);
    d = mc_disk_open(path_of("left.hfs"), 1, 512, error, sizeof error);
    CHECK(d && mc_hfs_left_mounted(d, &at, mdb) && at == 1024);
    CHECK(mdb[10] == 0x01 && mdb[11] == 0);             /* unmounted cleanly */
    CHECK(mdb[12] == 0 && mdb[13] == 0);                /* drNmFls: no file in the root */
    CHECK(mdb[82] == 0 && mdb[83] == 1);                /* drNmRtDirs: System Folder */
    CHECK(mdb[87] == 4 && mdb[91] == 2);                /* drFilCnt, drDirCnt */
    CHECK(mdb[33] == 24);                               /* drNxtCNID: above MODE32's 23 */
    /* Read through the virtual disk until the guest writes the block. */
    static const uint8_t driver[512] = {1};
    mc_vdisk *vd = mc_vdisk_create(&d, 1, &(mc_vdisk_drivers){driver, sizeof driver, NULL, 0}, error, sizeof error);
    uint8_t block[512];
    const uint64_t mdb_block = mc_vdisk_image_start(vd, 0) + 2;
    CHECK(vd && mc_vdisk_read(vd, mdb_block, 1, block) && !memcmp(block, mdb, 512));
    block[11] = 0x80;
    block[10] = 0;
    CHECK(mc_vdisk_write(vd, mdb_block, 1, block));
    CHECK(mc_vdisk_read(vd, mdb_block, 1, block) && block[10] == 0 && block[11] == 0x80);
    mc_vdisk_destroy(vd);
    mc_disk_close(d);
    /* Unmounted cleanly: nothing to do. */
    image[1024 + 10] = 0x01;
    write_bytes("left.hfs", image, sizeof image);
    d = mc_disk_open(path_of("left.hfs"), 0, 512, error, sizeof error);
    CHECK(d && !mc_hfs_left_mounted(d, &at, mdb));
    mc_disk_close(d);
}

/* --- the machine interface ---------------------------------------------------------- */

static uint32_t pixel(mc_machine *m, uint32_t x, uint32_t y) {
    mc_framebuffer fb;
    if (!mc_machine_framebuffer(m, &fb)) return 0;
    uint32_t *argb = malloc((size_t)fb.width * fb.height * 4);
    if (!argb) return 0;
    mc_framebuffer_argb(&fb, argb);
    uint32_t v = argb[(size_t)y * fb.width + x];
    free(argb);
    return v;
}
static mc_machine *stub(uint32_t width, uint32_t height, uint32_t depth) {
    mc_config c;
    mc_config_defaults(&c);
    c.model = (uint32_t)mc_model_index(mc_model_find(MC_MODEL_STUB));   /* the stub machine's own model */
    c.cpu_hz = 19000000;
    c.width = width; c.height = height; c.depth = depth;
    char error[256];
    uint8_t rom[16] = {0};
    mc_machine *m = mc_machine_create(&c, rom, sizeof rom, error, sizeof error);
    CHECK(m != NULL);
    return m;
}

static void test_machine(void) {
    CHECK(mc_machine_emulated(mc_model_find("quadra-700")));
    CHECK(mc_machine_emulated(mc_model_find("iici")));
    CHECK(mc_machine_emulated(mc_model_find("iifx")));
    CHECK(!mc_machine_emulated(mc_model_find(MC_MODEL_STUB)));
    /* Every 68k model is emulated, of the PowerPC ones the 6100, 7100 and
     * 8100 (PLAN 6.4), Capella (6.7), and the first PCI generation's
     * including the 9500/9600 on the PCI Livadix Display Card (6.8);
     * the stub's model is hidden from the list and from model: auto. */
    static const char ppc_emulated[] = " power-macintosh-6100 power-macintosh-7100 power-macintosh-8100 power-macintosh-7200 "
                                       "workgroup-server-6150 workgroup-server-8150 workgroup-server-9150-80 workgroup-server-9150-120 "
                                       "power-macintosh-7100-80 power-macintosh-8100-100 power-macintosh-8100-110 "
                                       "power-macintosh-7200-v1 power-macintosh-7500 power-macintosh-7600 power-macintosh-8500 "
                                       "power-macintosh-8600 power-macintosh-8600-300 power-macintosh-9500 power-macintosh-9600 "
                                       "power-macintosh-9600-350 power-macintosh-7300 power-macintosh-5200 "
                                       "power-macintosh-5300 power-macintosh-5400 power-macintosh-6400 power-macintosh-5500 power-macintosh-6500 twentieth-anniversary-macintosh "
                                       "power-macintosh-6200 power-macintosh-6300 power-macintosh-g3 power-macintosh-g3-v1 "
                                       "power-macintosh-g3-minitower power-macintosh-g3-all-in-one "
                                       "powerbook-duo-2300c powerbook-1400 powerbook-1400cs pippin pippin-dev powerbook-g3-wallstreet powerbook-g3-pdq ";
    for (size_t i = 0; i < mc_model_count(); ++i) {
        char key[48];
        snprintf(key, sizeof key, " %s ", mc_model_entry(i)->key);
        CHECK(mc_machine_emulated(mc_model_entry(i)) == (!mc_model_ppc(mc_model_entry(i)) || strstr(ppc_emulated, key) != NULL));
    }
    CHECK(mc_model_entry(mc_model_count()) == mc_model_find(MC_MODEL_STUB));
    CHECK(mc_model_index(mc_model_find(MC_MODEL_STUB)) == mc_model_count());
    /* Every depth: the second colour bar is yellow, as near as the palette allows. */
    static const uint8_t depths[] = {1, 2, 4, 8, 16, 32};
    static const uint32_t yellow[] = {0xffffffff, 0xffaaaaaa, 0xfffcf305, 0xffffff00, 0xffffff00, 0xffffff00};
    for (unsigned i = 0; i < 6; ++i) {
        mc_machine *m = stub(640, 480, depths[i]);
        if (!m) continue;
        mc_framebuffer fb;
        CHECK(mc_machine_framebuffer(m, &fb) && fb.width == 640 && fb.height == 480 && fb.depth == depths[i]);
        CHECK_EQ(fb.row_bytes, 640u * depths[i] / 8);
        CHECK_EQ(pixel(m, 0, 0), 0xff000000);                      /* border */
        CHECK_EQ(pixel(m, 120, 40), yellow[i]);
        CHECK_EQ(pixel(m, 20, 20), 0xffffffff);                    /* white bar */
        mc_machine_destroy(m);
    }
    mc_machine *m = stub(640, 480, 8);
    if (!m) return;
    CHECK(strstr(mc_machine_description(m), "not emulated yet") != NULL);
    /* Guest time: 19 MHz for one second is one second of C15M. */
    CHECK_EQ(mc_machine_rate(m), 19000000);
    CHECK(mc_machine_run(m, 19000000, 0) == MC_RUN_OK);
    CHECK_EQ(mc_machine_time(m), MC_C15M_HZ);
    CHECK(mc_machine_run(m, UINT64_MAX / 4, 2ull * MC_C15M_HZ) == MC_RUN_OK);
    CHECK_EQ(mc_machine_time(m), 2ull * MC_C15M_HZ);
    CHECK_EQ(mc_machine_cycles(m), 38000000);
    /* Input echo: the key's cell lights; the cursor follows the mouse. */
    uint32_t cell_x = 8 + 3, cell_y = 480 / 2 + 8 + 3;
    uint32_t before = pixel(m, cell_x, cell_y);
    CHECK(before != 0xff00ff00);
    CHECK(mc_machine_key(m, 0x00, 1));
    CHECK_EQ(pixel(m, cell_x, cell_y), 0xff00ff00);
    CHECK(mc_machine_key(m, 0x00, 0));
    CHECK_EQ(pixel(m, cell_x, cell_y), before);
    CHECK(mc_machine_mouse(m, 10, 20, 1));
    CHECK_EQ(pixel(m, 320 + 10 + 4, 240 + 20 + 4), 0xffff0000);
    CHECK(mc_machine_mouse(m, 0, 0, 0));
    CHECK(pixel(m, 320 + 10 + 4, 240 + 20 + 4) != 0xffff0000);
    /* Sound: C15M / 704 frames per second, a tone after each key press. */
    int16_t samples[30000 * 2];
    CHECK_EQ(mc_machine_audio_rate(m), 22255);
    size_t got = mc_machine_audio_pull(m, samples, 30000);
    CHECK_EQ(got, 22255);   /* two seconds due, at most one kept */
    CHECK(mc_machine_key(m, 0x01, 1));
    mc_machine_run(m, 1900000, 0);   /* 100 ms */
    got = mc_machine_audio_pull(m, samples, 30000);
    int loud = 0;
    for (size_t i = 0; i < got * 2; ++i) loud |= samples[i] != 0;
    CHECK(got > 2000 && loud);
    /* Media: floppy and CD drives, eject and reinsert; hard disks only before running. */
    char error[256];
    CHECK_EQ(mc_machine_drives(m, MC_MEDIA_FLOPPY), 1);
    mc_disk *floppy = mc_disk_open(path_of("raw.img"), 0, 512, error, sizeof error);
    mc_disk *disc = mc_disk_open(path_of("disc.iso"), 0, 2048, error, sizeof error);
    mc_disk *disk = mc_disk_open(path_of("raw.img"), 0, 512, error, sizeof error);
    CHECK(!mc_machine_insert(m, MC_MEDIA_FLOPPY, 0, disc, error, sizeof error) && strstr(error, "400, 720, 800 or 1440"));
    CHECK(!mc_machine_insert(m, MC_MEDIA_FLOPPY, 1, floppy, error, sizeof error));
    CHECK(mc_machine_insert(m, MC_MEDIA_FLOPPY, 0, floppy, error, sizeof error));
    CHECK(mc_machine_insert(m, MC_MEDIA_CDROM, 0, disc, error, sizeof error));
    CHECK(!mc_machine_insert(m, MC_MEDIA_DISK, 0, disk, error, sizeof error) && strstr(error, "before"));
    mc_disk_close(disk);
    int ejected = 0;
    CHECK(mc_machine_media(m, MC_MEDIA_FLOPPY, 0, &ejected) == floppy && !ejected);
    CHECK(!mc_machine_reinsert(m));
    CHECK(mc_machine_eject(m, MC_MEDIA_FLOPPY, 0));
    CHECK(mc_machine_media(m, MC_MEDIA_FLOPPY, 0, &ejected) == floppy && ejected);
    CHECK(mc_machine_reinsert(m));
    CHECK(mc_machine_media(m, MC_MEDIA_FLOPPY, 0, &ejected) == floppy && !ejected);
    mc_machine_destroy(m);   /* closes the floppy and the disc */
    /* Hard disks go in before the first run. */
    m = stub(512, 342, 1);
    if (!m) return;
    disk = mc_disk_open(path_of("raw.img"), 0, 512, error, sizeof error);
    CHECK(mc_machine_insert(m, MC_MEDIA_DISK, 0, disk, error, sizeof error));
    CHECK(!mc_machine_eject(m, MC_MEDIA_DISK, 0));
    mc_machine_destroy(m);
}

static void nv_test_write(const char *path,const void *data,size_t n) {
    FILE *f=fopen(path,"wb");CHECK(f!=NULL);if(!f)return;
    CHECK(fwrite(data,1,n,f)==n);CHECK_EQ(fclose(f),0);
}
static size_t nv_test_read(const char *path,void *data,size_t capacity) {
    FILE *f=fopen(path,"rb");CHECK(f!=NULL);if(!f)return 0;
    size_t n=fread(data,1,capacity,f);fclose(f);return n;
}
static void test_nvram_store(void) {
    mc_config c;mc_config_defaults(&c);c.model=(uint32_t)mc_model_index(mc_model_find("quadra-700"));
    snprintf(c.source,sizeof c.source,"%s/owned-source.yaml",dir);
    snprintf(c.profile,sizeof c.profile,"A B");
    snprintf(c.nvram,sizeof c.nvram,"%s/owned-state-%u.nvram",dir,(unsigned)_getpid());
    mc_config_nvram_identity(&c);
    mc_config fresh=c;CHECK(mc_config_new_nvram_identity(&fresh));CHECK(strcmp(fresh.nvram_id,c.nvram_id)!=0);
    mc_config elsewhere=c;elsewhere.nvram_id[0]=0;snprintf(elsewhere.source,sizeof elsewhere.source,"another/owned-source.yaml");
    mc_config_nvram_identity(&elsewhere);CHECK(strcmp(elsewhere.nvram_id,c.nvram_id)!=0);
    char error[1400],work[1024],work2[1024],target[1024],other[1024];mc_nvram *a=NULL,*b=NULL;
    unsigned char pram[273]={0},record[4096],record2[4096],check[4096];
    memcpy(pram,"LVDXPRAM",8);pram[8]=1;pram[17]=0x11;
    nv_test_write(c.nvram,pram,sizeof pram); /* unowned input is never loaded */
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    snprintf(target,sizeof target,"%s",mc_nvram_target(a));
    FILE *missing=fopen(work,"rb");CHECK(missing==NULL);if(missing)fclose(missing);
    CHECK(!mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&b,work2,sizeof work2,error,sizeof error));
    nv_test_write(work,pram,sizeof pram);CHECK(mc_nvram_end(a,1,error,sizeof error));
    size_t n=nv_test_read(target,record,sizeof record);CHECK(n>sizeof pram);
    snprintf(c.profile,sizeof c.profile,"Renamed"); /* stored identity survives */
    snprintf(c.source,sizeof c.source,"elsewhere/owned-source.yaml"); /* a portable folder can move */
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    CHECK_STR(mc_nvram_target(a),target);CHECK_EQ(nv_test_read(work,check,sizeof check),273);CHECK_EQ(check[17],0x11);
    pram[17]=0x22;nv_test_write(work,pram,sizeof pram);CHECK(mc_nvram_end(a,1,error,sizeof error));
    n=nv_test_read(target,record,sizeof record);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    nv_test_write(work,pram,12);CHECK(!mc_nvram_end(a,1,error,sizeof error));remove(work);
    CHECK_EQ(nv_test_read(target,check,sizeof check),n);CHECK(!memcmp(record,check,n));
    /* Corrupt primary: restore from the last validated backup. */
    record[n-1]^=1;nv_test_write(target,record,n);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    CHECK_EQ(nv_test_read(work,check,sizeof check),273);CHECK_EQ(check[17],0x11);
    CHECK(mc_nvram_end(a,1,error,sizeof error));n=nv_test_read(target,record,sizeof record);
    mc_config copy=c;copy.nvram_id[0]=0;snprintf(copy.profile,sizeof copy.profile,"A-B");mc_config_nvram_identity(&copy);
    CHECK(mc_nvram_begin(&copy,copy.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    snprintf(other,sizeof other,"%s",mc_nvram_target(a));CHECK(strcmp(target,other)!=0);
    nv_test_write(work,pram,sizeof pram);CHECK(mc_nvram_end(a,1,error,sizeof error));
    size_t n2=nv_test_read(other,record2,sizeof record2);nv_test_write(target,record2,n2);
    /* A valid foreign record is refused even when our backup is valid. */
    CHECK(!mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));
    CHECK(strstr(error,"another profile/model/ROM")!=NULL);nv_test_write(target,record,n);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b62u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    CHECK(strcmp(target,mc_nvram_target(a))!=0);CHECK(mc_nvram_end(a,0,error,sizeof error));
    /* Disk replacement and return restore separate exact payloads; the
     * old record stays unchanged for compatible first-use migration. */
    char media_a[1024],media_b[1024];
    c.disk_count=1;snprintf(c.disks[0].image,sizeof c.disks[0].image,"%s/nvram-disk-a.img",dir);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    snprintf(media_a,sizeof media_a,"%s",mc_nvram_target(a));CHECK(strcmp(media_a,target)!=0);
    CHECK_EQ(nv_test_read(work,check,sizeof check),273);CHECK_EQ(check[17],0x11);
    pram[17]=0xA1;nv_test_write(work,pram,sizeof pram);CHECK(mc_nvram_end(a,1,error,sizeof error));
    snprintf(c.disks[0].image,sizeof c.disks[0].image,"%s/nvram-disk-b.img",dir);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    snprintf(media_b,sizeof media_b,"%s",mc_nvram_target(a));CHECK(strcmp(media_a,media_b)!=0);
    pram[17]=0xB2;nv_test_write(work,pram,sizeof pram);CHECK(mc_nvram_end(a,1,error,sizeof error));
    snprintf(c.disks[0].image,sizeof c.disks[0].image,"%s/nvram-disk-a.img",dir);
    CHECK(mc_nvram_begin(&c,c.nvram,0x05f5b61u,273,&a,work,sizeof work,error,sizeof error));if(!a)return;
    CHECK_EQ(nv_test_read(work,check,sizeof check),273);CHECK_EQ(check[17],0xA1);
    CHECK(mc_nvram_end(a,0,error,sizeof error));
    CHECK_EQ(nv_test_read(target,check,sizeof check),n);CHECK(!memcmp(record,check,n));
    remove(media_a);remove(media_b);c.disk_count=0;
    uint8_t negative[8]={255,255,255,255,255,255,255,255};CHECK_EQ(mc_nvram_clock_offset(negative),-1);
    memset(negative,0,8);negative[7]=128;CHECK(mc_nvram_clock_offset(negative)==INT64_MIN);
    const char *duplicate="profiles:\n  One: {model: quadra-700, emulation: {nvram_id: '01234567890123456789012345678901'}}\n  Two: {model: quadra-700, emulation: {nvram_id: '01234567890123456789012345678901'}}\n";
    CHECK(!mc_config_parse(duplicate,strlen(duplicate),c.source,NULL,&copy,error,sizeof error));
    CHECK(strstr(error,"different emulation.nvram_id")!=NULL);
    remove(c.nvram);remove(target);remove(other);snprintf(work,sizeof work,"%s.bak",target);remove(work);
}

int mc_test_core(void) {
    snprintf(dir, sizeof dir, "%s", MC_WORK);
    _mkdir(dir);
    snprintf(dir, sizeof dir, "%s/tests", MC_WORK);
    _mkdir(dir);
    snprintf(dir, sizeof dir, "%s/tests/core", MC_WORK);
    _mkdir(dir);
    test_romdb();
    test_rom_index();
    test_models();
    test_config();
    test_profiles();
    test_yaml_edit();
    test_profile_rules();
    test_profile_write();
    test_nvram_store();
    test_disk();
    test_volumes();
    test_left_mounted();
    test_machine();
    return 0;
}
