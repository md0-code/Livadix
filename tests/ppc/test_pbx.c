/* Livadix — PBX reset decoding, Whitney priorities and board wiring. */
#include "ppc/chips/pbx.h"
#include "ppc/ppc_machine.h"
#include "machine.h"
#include "via.h"
#include "pmu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned assertions, failures;
#define CHECK(x) do { ++assertions; if (!(x)) { ++failures; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)
static uint64_t now;
static int cpu;
static uint64_t clock_now(void *ctx) { (void)ctx; return now; }
static void line(void *ctx, void *dev, int signal, int state) {
    (void)ctx; (void)dev; CHECK(signal == MC_WHITNEY_LINE_CPU); cpu = state;
}
static const mc_hw_host host = {.now=clock_now, .line=line};

static void interrupts(void) {
    mc_whitney *w = calloc(1, sizeof *w); CHECK(w != NULL); if (!w) return;
    mc_whitney_init(w, &host);
    const mc_dev_ops *o = &mc_whitney_irq_ops;
    CHECK(o->read(w, 0, 4) == 0);
    mc_whitney_set_ipl(w, 1); CHECK(cpu && o->read(w, 0, 4) == 0x81);
    o->write(w, 0, 4, 0x81); CHECK(!cpu);
    o->write(w, 0, 4, 9); CHECK(!cpu && o->read(w, 3, 1) == 9);
    mc_whitney_set_ipl(w, 2); CHECK(cpu && o->read(w, 0, 4) == 0x8A);
    o->write(w, 3, 1, 0x92); CHECK(!cpu && w->mask == 2);
    mc_whitney_set_ipl(w, 1); CHECK(!cpu);
    /* Returning from a level-2 handler unmasks an already pending level 1. */
    o->write(w, 3, 1, 0); CHECK(cpu);
    o->write(w, 0, 1, 0x80); CHECK(cpu); /* upper byte is not ACK */
    o->write(w, 0, 4, 0x38); CHECK(!cpu);
    mc_whitney_set_ipl(w, 4); CHECK(!cpu);
    o->write(w, 0, 4, 0); CHECK(cpu);
    CHECK(o->read(w, 1, 4) == 0);
    o->reset(w); CHECK(!cpu && !o->read(w, 0, 4)); free(w);
}
static void memory(void) {
    mc_bus *bus = calloc(1, sizeof *bus); uint8_t *ram = calloc(1, 64u << 20);
    CHECK(bus && ram); if (!bus || !ram) { free(bus); free(ram); return; }
    mc_bus_init(bus); mc_pbx p;
    CHECK(!mc_pbx_init(&p, &host, bus, ram, 60, 1));
    CHECK(!mc_pbx_init(&p, &host, bus, ram, 10, 0));
    CHECK(mc_pbx_init(&p, &host, bus, ram, 20, 1));
    uint32_t value = 0;
    CHECK(!mc_bus_write(bus, 0, 4, 0x12345678));
    /* Four consecutive reset pages mirror soldered bank 0's 8 MiB. */
    CHECK(!mc_bus_read(bus, 0, 4, &value) && value == 0x12345678);
    mc_pbx_ops.write(&p, 0x10, 4, 0x88888800);
    CHECK(!mc_bus_read(bus, MC_PBX_PAGE, 4, &value));
    mc_pbx_ops.write(&p, 0x10, 4, 0x88888000);
    CHECK(!mc_bus_read(bus, 0, 4, &value) && value == 0x12345678);
    mc_pbx_ops.write(&p, 0x10, 4, 0x88888810);
    CHECK(!mc_bus_write(bus, MC_PBX_PAGE, 4, 0xABCDEF01));
    CHECK(ram[8u << 20] == 0xAB);
    CHECK(mc_pbx_ops.read(&p, 0, 4) == 1);
    mc_pbx_ops.write(&p, 0, 4, 0); CHECK(mc_pbx_ops.read(&p, 0, 4) == 1);
    mc_pbx_ops.write(&p, 0x11, 1, 0x12); CHECK(mc_pbx_ops.read(&p, 0x11, 1) == 0x12);
    CHECK(mc_pbx_ops.read(&p, 0x1F, 4) == 0);
    mc_pbx_ops.reset(&p); CHECK(mc_pbx_ops.read(&p, 0x10, 4) == 0x88880000);
    /* Apple PowerBook 1400 Developer Note, Tables 4-11/12: fixed
     * factory bank and two independent cards with uniform banks. */
    CHECK(!mc_pbx_init(&p, &host, bus, ram, 8, 0));
    for (unsigned mb = 12; mb <= 64; mb += 4) {
        CHECK(mc_pbx_init(&p, &host, bus, ram, mb, 0));
        uint32_t total = 0;
        for (unsigned i = 0; i < MC_PBX_BANKS; i++) total += p.banks[i];
        CHECK(total == mb << 20 && p.banks[0] == 8u << 20 && p.banks[1] == (mb == 12 ? 4u : 8u) << 20);
        CHECK(p.reg[4] == 0x88110000);
        for (unsigned i = 2; i < MC_PBX_BANKS; i++)
            CHECK(!p.banks[i] || p.banks[i] == p.banks[i < 5 ? 2 : 5]);
        if (mb == 32) CHECK(p.banks[2] == 16u << 20 && !p.banks[3] && !p.banks[5]);
        if (mb == 36) CHECK(p.banks[2] == 16u << 20 && !p.banks[3] && p.banks[5] == 4u << 20);
    }
    CHECK(mc_pbx_init(&p, &host, bus, ram, 16, 0));
    CHECK(!mc_bus_write(bus, 4 * MC_PBX_PAGE, 4, 0xAABBCCDD));
    CHECK(!mc_bus_write(bus, 5 * MC_PBX_PAGE, 4, 0x11223344));
    mc_pbx_ops.write(&p, 0x10, 4, 0x88180000); /* unmap page 4, keep 5 */
    CHECK(!mc_bus_read(bus, 5 * MC_PBX_PAGE, 4, &value) && value == 0x11223344);
    mc_pbx_ops.write(&p, 0x10, 4, 0x88110000);
    CHECK(!mc_bus_read(bus, 4 * MC_PBX_PAGE, 4, &value) && value == 0xAABBCCDD);
    mc_bus_free(bus); free(bus); free(ram);
}
static void boards(void) {
    static const char *const keys[] = {"powerbook-duo-2300c", "powerbook-1400", "powerbook-1400cs"};
    uint8_t *rom = calloc(1, 4u << 20); CHECK(rom != NULL); if (!rom) return;
    for (unsigned at = 4; at < 4u << 20; at += 4) rom[at] = 0x48; /* b . */
    for (unsigned i = 0; i < 3; i++) {
        const uint32_t checksum = i ? 0x83A21950u : 0x83C54F75u;
        for (unsigned b = 0; b < 4; b++) rom[b] = (uint8_t)(checksum >> (24u - 8u * b));
        const mc_ppc_desc *d = mc_ppc_find(keys[i]); CHECK(d != NULL); if (!d) continue;
        CHECK(mc_ppc_find_board(keys[i]) == d);
        mc_config c; mc_config_defaults(&c);
        const mc_model *model = mc_model_find(keys[i]); CHECK(model != NULL); if (!model) continue;
        c.model = (uint32_t)mc_model_index(model);
        mc_config_set_ram_kb(&c, 32u * 1024u); c.jit = 0; c.cpu_hz = d->cpu_hz;
        if (i == 2) c.cdrom_bus = MC_BUS_ATA;
        CHECK(mc_machine_emulated(model));
        uint32_t sizes[32];
        CHECK(mc_machine_ram_sizes(model, sizes, sizeof sizes / sizeof *sizes) > 0);
        CHECK(mc_machine_features(model) & MC_FEATURE_ATA_DISK);
        unsigned builtin, card;
        mc_machine_video(model, model->width, model->height, &builtin, &card);
        const unsigned depth = i ? 16 : 8;
        CHECK(builtin == MC_DEPTHS_UPTO(depth) && !card);
        CHECK(mc_machine_features(model) & MC_FEATURE_FIXED_SCREEN);
        CHECK(!!(mc_machine_features(model) & MC_FEATURE_NO_FLOPPY) == !i);
        CHECK(!!(mc_machine_features(model) & MC_FEATURE_MEDIA_BAY) == !!i);
        CHECK(!!(mc_machine_features(model) & MC_FEATURE_ATA_CD) == !!i);
        if (!i) {
            mc_machine_video(model, 640, 400, &builtin, &card);
            CHECK(builtin == MC_DEPTH_BIT(16) && !card);
        }
        mc_machine_video(model, 1024, 768, &builtin, &card);
        CHECK(!builtin && !card);
        char error[256];
        mc_machine *normal = mc_machine_create(&c, rom, 4u << 20, error, sizeof error);
        CHECK(normal != NULL);
        if (normal) {
            const mc_audio_source source = {0};
            CHECK(mc_machine_set_audio_source(normal, &source));
            CHECK(mc_machine_set_audio_source(normal, NULL));
            CHECK(!mc_machine_instructions(normal));
            CHECK(mc_machine_run(normal, d->cpu_hz / 4u, 0) == MC_RUN_OK);
            CHECK(mc_machine_instructions(normal) > 0);
            CHECK(!strcmp(mc_machine_description(normal), model->name));
            mc_machine_destroy(normal);
        }
        mc_ppc *m = mc_ppc_create(d, &c, rom, 4u << 20, error, sizeof error);
        CHECK(m != NULL); if (!m) { fprintf(stderr, "%s\n", error); continue; }
        CHECK(m->reset_held);
        mc_ppc_run(m, d->cpu_hz / 1000u, 0); CHECK(m->reset_held && !m->cpu.instructions);
        uint32_t v = 0;
        CHECK(!mc_bus_read(&m->bus, 0x50F03E00, 1, &v) && v == 0xFF);
        if (i == 2) {
            /* The task file is still busy for 2 ms after board reset. */
            mc_ppc_run(m, d->cpu_hz / 300u, 0);
            /* ATA packet signature, then IDENTIFY PACKET on the second
             * task file. Only the bay interrupt bit and slot C assert. */
            CHECK(!mc_bus_read(&m->bus, 0x50F1A090, 1, &v) && v == 0x14);
            CHECK(!mc_bus_read(&m->bus, 0x50F1A094, 1, &v) && v == 0xEB);
            CHECK(!mc_bus_write(&m->bus, 0x50F1A09C, 1, 0xA1));
            mc_ppc_run(m, d->cpu_hz / 1000u, 0);
            CHECK(!mc_bus_read(&m->bus, 0x50F1A0D4, 2, &v) && v == 1);
            CHECK(!mc_bus_read(&m->bus, 0x50F03E00, 1, &v) && v == 0xF7);
            CHECK(!mc_bus_read(&m->bus, 0x50F1A09C, 1, &v) && (v & 8));
            CHECK(!mc_bus_read(&m->bus, 0x50F1A0D4, 2, &v) && v == 0);
        }
        CHECK(!mc_bus_read(&m->bus, 0x50F01E00, 1, &v) && v == 0xFF);
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFF8, 4, &v) && v == (i ? 0x5AA50007u : 0x5AA50004u));
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFFC, 4, &v) && v == (i ? 0xA55A180Au : 0xA55A1809u));
        CHECK(!mc_bus_read(&m->bus, 0x40300000, 4, &v));
        CHECK(!mc_bus_write(&m->bus, 0x50F02400, 1, 0xFF));
        CHECK(!mc_bus_write(&m->bus, 0x50F02200, 1, 0x10));
        CHECK(!mc_bus_read(&m->bus, 0x50F03E00, 1, &v) && v == 0xFF); /* slot pins independent of PMU data */
        CHECK(!mc_bus_write(&m->bus, 0x50F02400, 1, 0));
        CHECK(!mc_bus_write(&m->bus, 0x50F02000, 1, 4));
        CHECK(!mc_bus_read(&m->bus, 0x50F02000, 1, &v) && (v & 4)); /* REQ readback even with DDRB=0 */
        CHECK(!mc_bus_read(&m->bus, 0x50F16C00, 1, &v)); /* original boot's SWIM read is answered */
        CHECK(!mc_bus_write(&m->bus, 0x50F96000, 1, 0x12));
        CHECK(!mc_bus_read(&m->bus, 0x50F96000, 1, &v) && v == 0x12);
        CHECK(!mc_bus_write(&m->bus, 0x5FFFFFFC, 4, 0));
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFFC, 4, &v) && v == 0);
        CHECK(!mc_bus_read(&m->bus, 0x5FFFFFFE, 2, &v) && v == (i ? 0x180Au : 0x1809u));
        CHECK(mc_ppc_key(m, 0, 1)); CHECK(mc_ppc_mouse(m, 1, -1, 0));
        mc_ppc_run(m, d->cpu_hz / 4u, 0);
        CHECK(!m->reset_held && m->cpu.instructions > 0);
        mc_framebuffer fb;
        CHECK(!mc_ppc_framebuffer(m, &fb));
        /* The guest unmasks FIFO B with $F29 after the FIFO-clear
         * strobe. Whitney must deliver its active-low interrupt on CB1. */
        CHECK(!mc_bus_write(&m->bus, 0x50F14F09, 1, 1));
        CHECK(!mc_bus_write(&m->bus, 0x50F14F29, 1, 1));
        CHECK(!mc_bus_read(&m->bus, 0x50F14804, 1, &v));
        CHECK(!mc_bus_write(&m->bus, 0x50F03A00, 1, MC_VIA_IRQ_CB1));
        CHECK(!mc_bus_write(&m->bus, 0x50F03C00, 1, 0x80 | MC_VIA_IRQ_CB1));
        CHECK(!mc_bus_write(&m->bus, 0x50F14803, 1, 0x80));
        CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v) && !(v & MC_VIA_IRQ_CB1));
        CHECK(!mc_bus_write(&m->bus, 0x50F14F29, 1, 0));
        CHECK(!mc_bus_read(&m->bus, 0x50F03A00, 1, &v) && (v & MC_VIA_IRQ_CB1));
        CHECK(!mc_bus_read(&m->bus, 0x50F2A000, 4, &v) && v == 0x82);
        CHECK(!mc_bus_read(&m->bus, 0x50F14804, 1, &v) && (v & 8));
        CHECK(!mc_bus_write(&m->bus, 0x50F03A00, 1, MC_VIA_IRQ_CB1));
        CHECK(!mc_bus_read(&m->bus, 0x50F2A000, 4, &v) && !v);
        CHECK(!mc_bus_read(&m->bus, 0x50F20000, 1, &v) && v == (i ? 0x11u : 1u));
        /* Clearing the LCD's frame latch must not let the guest's
         * immediately following poll read a retrace that has not happened. */
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && (v & 2));
        CHECK(!mc_bus_write(&m->bus, 0x50F20014, 1, 0x0E));
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0x0C);
        mc_ppc_run(m, d->cpu_hz / 40u, 0);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0x0E);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0x0E);
        CHECK(!mc_bus_write(&m->bus, 0x50F20014, 1, 0x0C));
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0x0E);
        /* Only CONTROL's big-endian byte lane acknowledges the latch;
         * bit 1 in either adjacent register must not acknowledge it. */
        CHECK(!mc_bus_write(&m->bus, 0x50F20012, 4, 0x03020C02));
        CHECK(!mc_bus_read(&m->bus, 0x50F20012, 4, &v) && v == 0x03020E02);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 2, &v) && v == 0x0E02);
        CHECK(!mc_bus_write(&m->bus, 0x50F20014, 2, 0x0E5A));
        CHECK(!mc_bus_read(&m->bus, 0x50F20012, 4, &v) && v == 0x03020C5A);
        mc_ppc_run(m, d->cpu_hz / 40u, 0);
        CHECK(!mc_bus_read(&m->bus, 0x50F20012, 4, &v) && v == 0x03020E5A);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 4, &v) && v == 0x0E5A0000);
        CHECK(!mc_bus_write(&m->bus, 0x50F20014, 4, 0x0C025A02));
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 4, &v) && v == 0x0E025A02);
        CHECK(!mc_bus_write(&m->bus, 0x50F20014, 4, 0x0E025A02));
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 4, &v) && v == 0x0C025A02);
        mc_ppc_run(m, d->cpu_hz / 40u, 0);
        CHECK(!mc_bus_read(&m->bus, 0x50F20012, 4, &v) && v == 0x03020E02);
        CHECK(!mc_bus_write(&m->bus, 0x50F20012, 4, 0x01020E02));
        CHECK(!mc_bus_read(&m->bus, 0x50F20012, 4, &v) && v == 0x01020C02);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0x0C);
        for (unsigned mode = 0; mode <= 4; mode++) {
            CHECK(!mc_bus_write(&m->bus, 0x50F20012, 1, mode));
            CHECK(mc_ppc_framebuffer(m, &fb));
            CHECK(fb.width == (i ? 800u : 640u));
            CHECK(fb.height == (i ? 600u : mode == 4 ? 400u : 480u));
            CHECK(fb.depth == 1u << mode && fb.row_bytes == fb.width * fb.depth / 8u);
            CHECK(fb.pixels && (mode == 4 ? !fb.palette : fb.palette != NULL));
        }
        CHECK(!mc_bus_write(&m->bus, 0x50F20012, 1, 5)); CHECK(!mc_ppc_framebuffer(m, &fb));
        mc_ppc_run(m, d->cpu_hz / 40u, 0);
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && (v & 2));
        d->ops->reset(m->board);
        CHECK(!mc_ppc_framebuffer(m, &fb));
        CHECK(!mc_bus_read(&m->bus, 0x50F20014, 1, &v) && v == 0);
        CHECK(!mc_bus_read(&m->bus, 0x50F80010, 4, &v) && v == (i ? 0x88110000u : 0x88880000u));
        mc_pmu *pmu = NULL;
        for (unsigned n = 0; n < m->sched.count; n++) {
            mc_dev *dev = m->sched.entry[n].dev;
            if (dev->ops == &mc_pmu_ops) pmu = (mc_pmu *)dev;
        }
        CHECK(pmu != NULL);
        if (pmu) {
            CHECK(!mc_bus_write(&m->bus, 0x50F20014, 1, 0x0C));
            CHECK(mc_ppc_framebuffer(m, &fb));
            const uint64_t visible_serial = fb.serial;
            pmu->power = MC_PMU_POWER_SLEEP; pmu->sleep_mode = 1;
            d->ops->line(m->board, pmu, MC_PMU_LINE_POWER, MC_PMU_POWER_SLEEP);
            CHECK(m->reset_held);
            CHECK(!mc_ppc_framebuffer(m, &fb));
            const uint64_t instructions = m->cpu.instructions;
            mc_ppc_run(m, d->cpu_hz / 100u, 0); CHECK(m->cpu.instructions == instructions);
            d->ops->line(m->board, pmu, MC_PMU_LINE_RESET, 0); CHECK(m->reset_held);
            CHECK(mc_ppc_key(m, 0, 0));    /* release */
            CHECK(mc_ppc_key(m, 0x39, 1)); /* Caps Lock */
            CHECK(mc_ppc_mouse(m, 0, 0, 1));
            mc_ppc_run(m, d->cpu_hz / 100u, 0);
            CHECK(m->reset_held && mc_pmu_asleep(pmu) && m->cpu.instructions == instructions);
            m->cpu.gpr[7] = 0x1234ABCD;
            m->cpu.hid[0] |= 0x00008000u;
            const uint32_t resume_pc = m->cpu.pc;
            CHECK(mc_ppc_key(m, 0, 1));
            mc_ppc_run(m, d->cpu_hz, 0);
            CHECK(!m->reset_held && !mc_pmu_asleep(pmu) && m->cpu.instructions > instructions);
            CHECK(m->cpu.gpr[7] == 0x1234ABCD);
            CHECK(m->cpu.srr0 == resume_pc && !(m->cpu.hid[0] & 0x00008000u));
            CHECK(mc_ppc_framebuffer(m, &fb) && fb.serial > visible_serial);
        }
        mc_ppc_destroy(m);
    }
    CHECK(!mc_ppc_find_board(NULL)); free(rom);
}
int main(void) {
    interrupts(); memory(); boards();
    printf("PBX: %u assertions, %u failures\n", assertions, failures);
    return failures ? 1 : 0;
}
