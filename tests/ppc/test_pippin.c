/* Livadix - Pippin input pins, ADB wiring and persistent board state. */
#include "ppc/ppc_machine.h"
#include "adbctl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static mc_ppc *board(const char *key, uint32_t checksum, const char *nvram) {
    uint8_t *rom = calloc(1, 4u << 20);
    CHECK(rom != NULL);
    if (!rom) return NULL;
    for (unsigned at = 4; at < 4u << 20; at += 4) rom[at] = 0x48;
    for (unsigned i = 0; i < 4; i++) rom[i] = (uint8_t)(checksum >> (24u - 8u * i));
    mc_config c;
    mc_config_defaults(&c);
    c.ram_mb = 6; c.ram_kb = 6u * 1024u; c.jit = 0;
    c.cpu_hz = 66000000u; c.controller = MC_CONTROLLER_PROTOCOL;
    c.nvram[0] = 0;
    if (nvram) {   /* the file is the board's own session file of a Pippin profile (machine.c) */
        snprintf(c.nvram, sizeof c.nvram, "%s", nvram);
        c.model = (uint32_t)mc_model_index(mc_model_find(key));
        c.nvram_private = 1;
    }
    const mc_ppc_desc *d = mc_ppc_find_board(key);
    CHECK(d != NULL);
    char error[256];
    mc_ppc *m = d ? mc_ppc_create(d, &c, rom, 4u << 20, error, sizeof error) : NULL;
    CHECK(m != NULL);
    if (d && !m) fprintf(stderr, "%s\n", error);
    free(rom);
    return m;
}

static mc_adbctl *cuda(mc_ppc *m) {
    for (unsigned i = 0; i < m->sched.count; i++) {
        mc_dev *d = m->sched.entry[i].dev;
        if (d && d->name && !strcmp(d->name, "Cuda")) return (mc_adbctl *)d;
    }
    return NULL;
}

static void wiring(const char *key, uint32_t checksum) {
    mc_ppc *m = board(key, checksum, NULL);
    if (!m) return;
    uint32_t value;
    CHECK(!mc_bus_read(&m->bus, 0xF8000010u, 4, &value) && value == 0x40010000u);
    CHECK(!mc_bus_read(&m->bus, 0xF0800068u, 4, &value) && value == 0x08000000u);
    CHECK(!mc_bus_write(&m->bus, 0xF0800068u, 4, 0));
    CHECK(!mc_bus_read(&m->bus, 0xF0800068u, 1, &value) && value == 8);
    CHECK(!mc_bus_read(&m->bus, 0xF0800068u, 2, &value) && value == 0x0800u);
    CHECK(!mc_bus_read(&m->bus, 0xF0800069u, 1, &value) && value == 0);
    CHECK(!mc_bus_write(&m->bus, 0xF0800068u, 4, 0x10203040u));
    CHECK(!mc_bus_read(&m->bus, 0xF0800068u, 4, &value) && value == 0x18203040u);
    CHECK(!mc_bus_write(&m->bus, 0xF00D0040u, 4, 0x12345678u));
    m->desc->ops->reset(m->board);
    CHECK(!mc_bus_read(&m->bus, 0xF0800068u, 4, &value) && value == 0x08000000u);
    CHECK(!mc_bus_read(&m->bus, 0xF00D0040u, 4, &value) && value == 0x12345678u);
    const mc_bus_region *r = &m->bus.region[mc_ppc_region(m, "early")];
    CHECK(r->nwin == 1);
    if (r->nwin) {
        r->win[0].ops->write(r->win[0].dev, 0xFFFu, 4, 0xFFFFFFFFu);
        CHECK(r->win[0].ops->read(r->win[0].dev, 0xFFFu, 1) == 0);
    }
    CHECK(!mc_bus_write(&m->bus, 0xF8000018u, 4, 0));
    CHECK(!mc_bus_write(&m->bus, 0x01000000u, 4, 0x11223344u));
    CHECK(!mc_bus_read(&m->bus, 0x01100000u, 4, &value) && value == 0);
    CHECK(!mc_bus_read(&m->bus, 0x02000000u, 4, &value) && value == 0);
    CHECK(!mc_bus_write(&m->bus, 0x00200000u, 4, 0xFF8099F8u));
    CHECK(!mc_bus_write(&m->bus, 0x00600000u, 4, 0x00600000u));
    CHECK(!mc_bus_read(&m->bus, 0x00600000u, 4, &value) && value == 0);
    CHECK(!mc_bus_read(&m->bus, 0x00200000u, 4, &value) && value == 0xFF8099F8u);
    CHECK(!mc_bus_write(&m->bus, 0xF8000018u, 4, 0xAA000000u));
    CHECK(!mc_bus_read(&m->bus, 0x01100000u, 4, &value) && value == 0x11223344u);
    CHECK(!mc_bus_write(&m->bus, 0xF8000018u, 4, 0));
    CHECK(!mc_bus_read(&m->bus, 0x01100000u, 4, &value) && value == 0);
    CHECK(m->desc->ops->key && m->desc->ops->mouse && m->desc->ops->set_clock);
    CHECK(m->desc->ops->key(m->board, 0, 1));
    CHECK(m->desc->ops->key(m->board, 0, 0));
    mc_adbctl *c = cuda(m);
    CHECK(c != NULL);
    if (c) {
        uint8_t reply[8];
        CHECK(c->adb->count == 2);
        CHECK(mc_adb_command(c->adb, 0x2C, NULL, 0, reply) == 2 && reply[0] == 0 && reply[1] == 0x80);
        CHECK(m->desc->ops->mouse(m->board, 7, -3, 1));
        CHECK(mc_adb_command(c->adb, 0x3C, NULL, 0, reply) == 2 && reply[0] == 0x7D && reply[1] == 0x87);
        CHECK(m->desc->ops->mouse(m->board, 0, 0, 0));
        CHECK(mc_adb_command(c->adb, 0x3C, NULL, 0, reply) == 2 && reply[0] == 0x80 && reply[1] == 0x80);
        m->desc->ops->set_clock(m->board, 123456789);
        CHECK(mc_adbctl_seconds(c) == 123456789u);
    }
    /* No uninitialized host buffer may count as a usable guest screen. */
    mc_framebuffer fb;
    CHECK(m->desc->ops->framebuffer != NULL);
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    CHECK(!mc_bus_write(&m->bus, 0xF0800020u, 4, 0x37400000u));
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    CHECK(!mc_bus_write(&m->bus, 0xF0800004u, 4, 0x28000000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0800054u, 4, 0x02200000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0800058u, 4, 0x20200000u));
    CHECK(!mc_bus_write(&m->bus, 0xF080001Cu, 4, 0x20200000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0000000u, 1, 3));
    CHECK(!mc_bus_write(&m->bus, 0xF100000Cu, 4, 0x12345600u));
    CHECK(!mc_bus_read(&m->bus, 0xF100000Cu, 4, &value) && value == 0x12345600u);
    CHECK(m->desc->ops->framebuffer(m->board, &fb));
    CHECK(fb.width == 640 && fb.height == 480 && fb.depth == 8 && fb.row_bytes == 640);
    CHECK(fb.pixels[0] == 3 && fb.palette[3] == 0x123456u);
    uint64_t serial = fb.serial;
    CHECK(!mc_bus_write(&m->bus, 0xF100000Du, 2, 0xABCD));
    CHECK(!mc_bus_read(&m->bus, 0xF100000Cu, 4, &value) && value == 0x12ABCD00u);
    CHECK(!mc_bus_read(&m->bus, 0xF1000400u, 4, &value) && value == 0);
    CHECK(m->desc->ops->framebuffer(m->board, &fb) && fb.serial != serial);
    CHECK(fb.palette[3] == 0x12ABCDu);
    CHECK(!mc_bus_write(&m->bus, 0xF0800000u, 4, 0x96000000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0800004u, 4, 0x50000000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0800008u, 4, 0x80000000u));
    CHECK(!mc_bus_write(&m->bus, 0xF004B000u, 2, 0x7C00));
    CHECK(m->desc->ops->framebuffer(m->board, &fb));
    CHECK(fb.width == 640 && fb.height == 480 && fb.depth == 16 && fb.row_bytes == 1280);
    CHECK(fb.pixels[0] == 0x7C && fb.pixels[1] == 0);
    CHECK(!mc_bus_write(&m->bus, 0xF00A0000u, 2, 0x03E0));
    CHECK(fb.pixels[0x55000] == 3 && fb.pixels[0x55001] == 0xE0);
    CHECK(fb.pixels[0x85040] == 0x12 && fb.pixels[0x85041] == 0x34);
    CHECK(!mc_bus_write(&m->bus, 0xF00D0040u, 2, 0x001F));
    CHECK(fb.pixels[0x85040] == 0 && fb.pixels[0x85041] == 0x1F);
    CHECK(!mc_bus_write(&m->bus, 0xF0800058u, 4, 0x60000000u));
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    CHECK(!mc_bus_write(&m->bus, 0xF0800058u, 4, 0xFFF00000u));
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    CHECK(!mc_bus_write(&m->bus, 0xF0800058u, 4, 0x01000000u));
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    CHECK(!mc_bus_write(&m->bus, 0xF0800058u, 4, 0x20200000u));
    CHECK(!mc_bus_write(&m->bus, 0xF0800020u, 4, 0));
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    m->desc->ops->reset(m->board);
    CHECK(!m->desc->ops->framebuffer(m->board, &fb));
    mc_ppc_destroy(m);
}

static int seed(const char *path, size_t service_bytes) {
    uint8_t prefix[273] = {0};
    memcpy(prefix, "LVDXPRAM", 8); prefix[8] = 1;
    prefix[9] = 0x80; prefix[10] = 0x0D; /* 3456 seconds, little endian */
    prefix[17 + 0x44] = 0xA5;
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(prefix, 1, sizeof prefix, f) == sizeof prefix;
    for (size_t i = 0; i < service_bytes; i++) if (fputc(i == 0x40 ? 0xAB : 0, f) == EOF) ok = 0;
    if (fclose(f)) ok = 0;
    return ok;
}

/* A flash command through the board: the chip's address every four bytes. */
static void flash_command(mc_ppc *m, uint8_t value) {
    CHECK(!mc_bus_write(&m->bus, 0x40015554u, 1, 0xAA));
    CHECK(!mc_bus_write(&m->bus, 0x4000AAA8u, 1, 0x55));
    CHECK(!mc_bus_write(&m->bus, 0x40015554u, 1, value));
}

static void persistence(void) {
    const char *path = "pippin-board-regression.nvram";
    FILE *existing = fopen(path, "rb");
    CHECK(existing == NULL);
    if (existing) { fclose(existing); return; }
    CHECK(seed(path, 0x20000u));
    mc_ppc *m = board("pippin", 0x2BEF21B7u, path);
    if (m) {
        uint32_t value;
        /* The file's flash memory is the chip's array, not the ROM's work
         * area in the display memory. */
        CHECK(!mc_bus_read(&m->bus, 0xF00D0040u, 1, &value) && value == 0);
        CHECK(!mc_bus_read(&m->bus, 0x40000100u, 1, &value) && value == 0xAB);
        CHECK(!mc_bus_read(&m->bus, 0x40000101u, 1, &value) && value == 0xFF);
        CHECK(!mc_bus_read(&m->bus, 0x40000104u, 1, &value) && value == 0);
        /* The identification the ROM looks for ($FFCCCF22-$FFCCCF74). */
        flash_command(m, 0x90);
        CHECK(!mc_bus_read(&m->bus, 0x40000000u, 1, &value) && value == 0x01);
        CHECK(!mc_bus_read(&m->bus, 0x40000004u, 1, &value) && value == 0x20);
        flash_command(m, 0xF0);
        /* A sector erased and a byte programmed, as the ROM writes back. */
        flash_command(m, 0x80);
        CHECK(!mc_bus_write(&m->bus, 0x40015554u, 1, 0xAA));
        CHECK(!mc_bus_write(&m->bus, 0x4000AAA8u, 1, 0x55));
        CHECK(!mc_bus_write(&m->bus, 0x40000000u, 1, 0x30));
        CHECK(!mc_bus_read(&m->bus, 0x40000100u, 1, &value) && value == 0xFF);
        CHECK(!mc_bus_read(&m->bus, 0x40010000u, 1, &value) && value == 0);
        flash_command(m, 0xA0);
        CHECK(!mc_bus_write(&m->bus, 0x40000104u, 1, 0x5A));
        CHECK(!mc_bus_read(&m->bus, 0x40000104u, 1, &value) && value == 0x5A);
        CHECK(!mc_bus_write(&m->bus, 0xF00D0100u, 4, 0xDEADBEEFu));
        m->desc->ops->set_clock(m->board, 10000);
        mc_adbctl *c = cuda(m); CHECK(c != NULL);
        if (c) {
            CHECK(mc_adbctl_pram(c)[0x44] == 0xA5);
            CHECK(mc_adbctl_seconds(c) == 13456u);
            mc_adbctl_pram(c)[0x45] = 0x5A;
        }
        m->desc->ops->reset(m->board);
        CHECK(!mc_bus_read(&m->bus, 0xF00D0100u, 4, &value) && value == 0xDEADBEEFu);
        CHECK(!mc_bus_read(&m->bus, 0x40000104u, 1, &value) && value == 0x5A);
        mc_ppc_destroy(m);
        FILE *f = fopen(path, "rb"); CHECK(f != NULL);
        if (f) { CHECK(!fseek(f, 0, SEEK_END) && ftell(f) == 273 + 0x20000); fclose(f); }
        m = board("pippin", 0x2BEF21B7u, path);
        if (m) {
            CHECK(!mc_bus_read(&m->bus, 0xF00D0100u, 4, &value) && value == 0);
            CHECK(!mc_bus_read(&m->bus, 0x40000104u, 1, &value) && value == 0x5A);
            CHECK(!mc_bus_read(&m->bus, 0x40000100u, 1, &value) && value == 0xFF);
            CHECK(!mc_bus_read(&m->bus, 0x40010000u, 1, &value) && value == 0);
            m->desc->ops->set_clock(m->board, 20000);
            c = cuda(m); CHECK(c != NULL);
            if (c) CHECK(mc_adbctl_pram(c)[0x45] == 0x5A && mc_adbctl_seconds(c) == 23456u);
            mc_ppc_destroy(m);
        }
    }
    CHECK(seed(path, 4));
    m = board("pippin-dev", 0x2BF65931u, path);
    if (m) {
        uint32_t value;
        /* A truncated flash memory is not taken (the chip is erased); the
         * board writes its session file whole when it goes (whether the
         * session is kept is nvram.c's: it is not for a board never started). */
        CHECK(!mc_bus_read(&m->bus, 0x40000100u, 1, &value) && value == 0xFF);
        CHECK(!mc_bus_read(&m->bus, 0x40000000u, 1, &value) && value == 0xFF);
        mc_ppc_destroy(m);
        FILE *f = fopen(path, "rb"); CHECK(f != NULL);
        if (f) { CHECK(!fseek(f, 0, SEEK_END) && ftell(f) == 273 + 0x20000); fclose(f); }
    }
    CHECK(!remove(path));
}

int main(void) {
    wiring("pippin", 0x2BEF21B7u);
    wiring("pippin-dev", 0x2BF65931u);
    persistence();
    printf("test_pippin: %u assertions, %u failures\n", assertions, failures);
    return failures != 0;
}
