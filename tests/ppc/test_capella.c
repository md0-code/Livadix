/* Livadix — Capella's ROM-observed bus and interrupt contract (capella.h). */
#include "ppc/chips/capella.h"
#include "ppc/ppc_machine.h"
#include "via.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static int cpu;
static unsigned rises;
static void line(void *ctx, void *dev, int which, int state) {
    (void)ctx; (void)dev;
    CHECK(which == MC_CAPELLA_LINE_CPU);
    if (state && !cpu) rises++;
    cpu = state;
}
static const mc_hw_host host = {.line = line};

static void bridge(void) {
    mc_capella c;
    mc_capella_init(&c, &host, "Capella");
    const mc_dev_ops *o = &mc_capella_ops;
    CHECK(o->read(&c, MC_CAPELLA_IPL, 4) == 7);
    CHECK(!cpu);
    /* The two posted-read drains do not acknowledge the INT latch. */
    for (unsigned level = 1; level <= 7; level++) {
        mc_capella_set_ipl(&c, level);
        CHECK(cpu && o->read(&c, MC_CAPELLA_ACK, 4) == 1);
        CHECK(o->read(&c, MC_CAPELLA_IPL, 4) == (level ^ 7u));
        CHECK(o->read(&c, MC_CAPELLA_ACK, 4) == 1);
        o->write(&c, MC_CAPELLA_ACK, 4, 0);
        CHECK(!cpu && !o->read(&c, MC_CAPELLA_ACK, 4));
        mc_capella_set_ipl(&c, level);
        CHECK(!cpu);
    }
    CHECK(rises == 7);
    mc_capella_set_ipl(&c, 0);
    CHECK(cpu && o->read(&c, MC_CAPELLA_IPL, 4) == 7);
    o->write(&c, MC_CAPELLA_IPL, 4, 0);
    CHECK(o->read(&c, MC_CAPELLA_IPL, 4) == 7);
    mc_capella_set_ipl(&c, 8);
    CHECK(o->read(&c, MC_CAPELLA_IPL, 4) == 7);
    /* Longwords and byte-lane writes address the same cache registers. */
    o->write(&c, 0x0C, 4, 0x12345678);
    CHECK(o->read(&c, 0x0C, 2) == 0x1234);
    CHECK(o->read(&c, 0x0F, 1) == 0x78);
    o->write(&c, 0x0F, 1, 1);
    CHECK(o->read(&c, 0x0C, 4) == 0x12345601);
    o->write(&c, 0x16, 2, 0xABCD);
    CHECK(o->read(&c, 0x14, 4) == 0xABCD);
    CHECK(o->read(&c, 0x100, 4) == 0);
    o->reset(&c);
    CHECK(!cpu && o->read(&c, MC_CAPELLA_IPL, 4) == 7);
    CHECK(!o->read(&c, 0x0C, 4));
}

/* Native 4.3's DREQ polling sequence: PowerPC Enabler 9.0.4,
 * nitt 43, +$5B1C. An INQUIRY to the always-present empty CD target
 * needs no disk image. NCR 53C96 Tables 7/9 and Transfer Information. */
static void scsi_dreq(mc_ppc *m) {
    static const uint8_t inquiry[] = {0x80, 0x12, 0, 0, 0, 36, 0};
    uint32_t v = 0;
    CHECK(!mc_bus_write(&m->bus, 0x50F03800, 1, 0x26)); /* VIA2 PCR */
    CHECK(!mc_bus_write(&m->bus, 0x50F10040, 1, 3));    /* target 3 */
    for (unsigned i = 0; i < sizeof inquiry; i++)
        CHECK(!mc_bus_write(&m->bus, 0x50F10020, 1, inquiry[i]));
    CHECK(!mc_bus_write(&m->bus, 0x50F10030, 1, 0x42));
    m->reset_held = 1;
    CHECK(mc_ppc_run(m, 1000, 0) == MC_RUN_OK);
    CHECK(!mc_bus_read(&m->bus, 0x50F10050, 1, &v) && v == 0x18);
    CHECK(!mc_bus_write(&m->bus, 0x50F10000, 1, 36));
    CHECK(!mc_bus_write(&m->bus, 0x50F10010, 1, 0));
    CHECK(!mc_bus_write(&m->bus, 0x50F10030, 1, 0x90));
    CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v) && (v & MC_VIA_IRQ_CA2));
    /* Clearing the compatibility flag cannot hide a live DMA request. */
    CHECK(!mc_bus_write(&m->bus, 0x50F03A00, 1, MC_VIA_IRQ_CA2));
    CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v) && (v & MC_VIA_IRQ_CA2));
    unsigned words = 0;
    for (; words < 32; words++) {
        CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v));
        if (!(v & MC_VIA_IRQ_CA2)) break;
        CHECK(!mc_bus_read(&m->bus, 0x50F10100, 2, &v));
        if (!words) CHECK(v == 0x0580); /* CD-ROM, removable */
    }
    CHECK(words == 18);
    CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v) && !(v & MC_VIA_IRQ_CA2));
    m->reset_held = 0;
}

static void boards(void) {
    static const char *const keys[] = {"power-macintosh-5200", "power-macintosh-5300", "power-macintosh-6200", "power-macintosh-6300"};
    static const uint32_t ids[] = {0xA55A225C, 0xA55A2259, 0xA55A2254, 0xA55A2251};
    uint8_t *rom = calloc(1, 1u << 22);
    CHECK(rom != NULL);
    if (!rom) return;
    rom[0] = 0x63; rom[1] = 0xAB; rom[2] = 0xFD; rom[3] = 0x3F;
    for (unsigned i = 0; i < 4; i++) {
        const mc_ppc_desc *d = mc_ppc_find_board(keys[i]);
        CHECK(d && mc_ppc_find(keys[i]) == d);
        if (!d) continue;
        CHECK(d->cpu == (i & 1 ? PPC_MODEL_603E : PPC_MODEL_603));
        /* 5200/6200 note ch. 2 Valkyrie IC and Table 3-10: 4/8/16,
         * only 4/8 on the larger monitors; panels stay at 640x480. */
        CHECK(d->caps->builtin(NULL, 640, 480) == (MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)));
        CHECK(d->caps->builtin(NULL, 512, 384) == (i < 2 ? 0 : MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8) | MC_DEPTH_BIT(16)));
        CHECK(d->caps->builtin(NULL, 800, 600) == (i < 2 ? 0 : MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8)));
        CHECK(d->caps->builtin(NULL, 832, 624) == (i < 2 ? 0 : MC_DEPTH_BIT(4) | MC_DEPTH_BIT(8)));

        mc_config config;
        mc_config_defaults(&config);
        const mc_model *model = mc_model_find(keys[i]);
        CHECK(model != NULL);
        if (!model) continue;
        const char *firmware = mc_model_controller_firmware(model);
        CHECK(firmware && !strcmp(firmware, "cuda-341s0060"));
        config.model = (uint32_t)mc_model_index(model);
        config.cpu_hz = model->cpu_hz;
        config.width = model->width; config.height = model->height; config.depth = model->depth;
        config.controller = MC_CONTROLLER_PROTOCOL;
        strcpy(config.nvram, "capella-test-pram.tmp");
        mc_config_set_ram_kb(&config, 64u * 1024);
        char error[256] = "";
        mc_ppc *m = mc_ppc_create(d, &config, rom, 1u << 22, error, sizeof error);
        CHECK(m != NULL);
        if (!m) { fprintf(stderr, "%s: %s\n", keys[i], error); continue; }
        uint32_t v = 0;
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFFC, 4, &v) && v == ids[i]);
        CHECK(!mc_bus_write(&m->bus, 0x5FFFFFFC, 4, 0));
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFFC, 4, &v) && v == ids[i]);
        CHECK(!mc_bus_read(&m->bus, 0xFFC00000, 4, &v) && v == 0x63ABFD3F);
        CHECK(!mc_bus_read(&m->bus, 0x41000000, 4, &v) && v == 0x63ABFD3F);
        CHECK(!mc_bus_read(&m->bus, 0x53000024, 4, &v) && v == 7);
        CHECK(!mc_bus_write(&m->bus, 0x5300000F, 1, 1));
        CHECK(!mc_bus_read(&m->bus, 0x5300000C, 4, &v) && v == 1);
        CHECK(!mc_bus_write(&m->bus, 0x5300000C, 4, 6));
        CHECK(!mc_bus_write(&m->bus, 0x51000000, 4, 0xA5A5A5A5));
        CHECK(!mc_bus_read(&m->bus, 0x51000000, 4, &v) && v == 0xA5A5A5A5);
        CHECK(!mc_bus_write(&m->bus, 0x52000007, 1, 0x0A));
        CHECK(!mc_bus_read(&m->bus, 0x52000007, 1, &v) && v == 2);
        CHECK(!mc_bus_write(&m->bus, 0x5200000F, 1, 0x0A));
        CHECK(!mc_bus_read(&m->bus, 0x5200000F, 1, &v) && v == 10);
        CHECK(!mc_bus_write(&m->bus, 0x5300000C, 4, 0));
        CHECK(!mc_bus_read(&m->bus, 0x51000000, 1, &v) && v != 0xA5);
        m->reset_held = 1;
        const uint32_t pc = m->cpu.pc;
        const uint64_t instructions = m->cpu.instructions;
        CHECK(mc_ppc_run(m, 1000, 0) == MC_RUN_OK);
        CHECK(m->cpu.pc == pc && m->cpu.instructions == instructions);
        CHECK(mc_ppc_now(m) > 0);
        m->reset_held = 0;
        CHECK(!mc_bus_write(&m->bus, 0xF9001000, 4, 0x12345678));
        CHECK(!mc_bus_read(&m->bus, 0xF9101000, 4, &v) && v == 0x12345678);
        CHECK(!mc_bus_write(&m->bus, (64u << 20) - 4, 4, 0x9ABCDEF0));
        CHECK(!mc_bus_read(&m->bus, (64u << 20) - 4, 4, &v) && v == 0x9ABCDEF0);
        scsi_dreq(m);
        mc_ppc_destroy(m);
        remove(config.nvram);
    }
    free(rom);
}

int main(void) {
    bridge();
    boards();
    printf("Capella: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
